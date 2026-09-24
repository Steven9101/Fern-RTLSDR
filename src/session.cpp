// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "session.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/eventfd.h>
#include <unistd.h>

#include "failure.h"
#include "io.h"
#include "json.h"
#include "log.h"
#include "receiver.h"
#include "settings.h"
#include "stream.h"

namespace fern {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t max_line = 64 * 1024;

bool blank(const std::string& s) {
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\r')
            return false;
    return true;
}

class Session {
public:
    Session(Backend& backend, const SessionIo& io, const SessionOptions& options)
        : io_(io), options_(options), receiver_(backend), lines_(max_line) {}

    ~Session() {
        if (notify_fd_ >= 0 && !leaked_)
            ::close(notify_fd_);
    }

    SessionResult run();

private:
    enum class State { waiting, streaming };

    SessionIo io_;
    SessionOptions options_;
    Receiver receiver_;
    std::unique_ptr<Stream> stream_;
    LineReader lines_;
    int notify_fd_ = -1;
    State state_ = State::waiting;
    std::optional<int> exit_;
    bool commands_open_ = true;
    bool events_broken_ = false;
    bool device_lost_ = false;
    bool device_stalled_ = false;
    bool leaked_ = false;
    Clock::time_point next_stats_;
    Clock::time_point next_drop_log_;
    uint64_t logged_drops_ = 0;

    void finish(int status) {
        if (!exit_)
            exit_ = status;
    }
    bool send(const json::Value& event);
    void fail(const Failure& f);
    void read_commands();
    void handle_line(const LineReader::Line& line);
    void handle_open(const json::Value& message);
    void handle_set(const json::Value& message);
    void check_stream();
    void on_tick();
    SessionResult shut_down();
};

bool Session::send(const json::Value& event) {
    if (events_broken_)
        return false;
    std::string line = json::serialize(event);
    if (line.size() >= max_line) {
        // Nothing the module writes comes near this; keep the line limit even
        // if something does.
        log_line("an event of %zu bytes is longer than FernSDR accepts; sent an error instead", line.size());
        json::Value e = json::Value::object();
        e.set("type", "error");
        e.set("code", "internal");
        e.set("message", "an event was too long to send");
        const json::Value* fatal = event.find("fatal");
        e.set("fatal", fatal && fatal->is_bool() && fatal->as_bool());
        line = json::serialize(e);
    }
    line += '\n';
    const int err = write_all(io_.events, line.data(), line.size());
    if (err == 0)
        return true;
    events_broken_ = true;
    if (err == EPIPE) {
        log_line("FernSDR closed fd 3; stopping");
        finish(exit_status::stopped);
    } else {
        log_line("writing an event to fd 3 failed: %s", std::strerror(err));
        finish(exit_status::internal);
    }
    return false;
}

void Session::fail(const Failure& f) {
    log_line("%s", f.message.c_str());
    json::Value e = json::Value::object();
    e.set("type", "error");
    e.set("code", error_code_name(f.code));
    e.set("message", f.message);
    e.set("fatal", true);
    send(e);
    finish(exit_status_for(f.code));
}

void Session::read_commands() {
    char buf[16384];
    const ssize_t n = ::read(io_.commands, buf, sizeof buf);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN)
            return;
        log_line("reading fd 0 failed: %s; stopping", std::strerror(errno));
        commands_open_ = false;
        finish(exit_status::stopped);
        return;
    }
    if (n == 0) {
        commands_open_ = false;
        lines_.finish();
    } else {
        lines_.feed(buf, static_cast<size_t>(n));
    }
    LineReader::Line line;
    while (!exit_ && lines_.next(line))
        handle_line(line);
    if (!commands_open_ && !exit_) {
        log_line("fd 0 was closed; stopping");
        finish(exit_status::stopped);
    }
}

void Session::handle_line(const LineReader::Line& line) {
    if (line.too_long) {
        log_line("ignored a command longer than %zu bytes", max_line);
        return;
    }
    if (blank(line.text))
        return;
    json::Value message;
    std::string error;
    if (!json::parse(line.text, message, error)) {
        log_line("ignored a command that is not valid JSON: %s", error.c_str());
        return;
    }
    const json::Value* type = message.find("type");
    if (!type || !type->is_string()) {
        log_line("ignored a command without a type");
        return;
    }
    const std::string& t = type->as_string();
    if (t == "open") {
        handle_open(message);
    } else if (t == "set") {
        handle_set(message);
    } else if (t == "stop") {
        log_line("stopping as FernSDR asked");
        finish(exit_status::stopped);
    }
    // Any other type is ignored, so that the protocol can grow.
}

void Session::handle_open(const json::Value& message) {
    if (state_ != State::waiting) {
        log_line("ignored a second open");
        return;
    }
    OpenRequest request;
    if (auto f = parse_open(message, request)) {
        fail(*f);
        return;
    }
    if (auto f = receiver_.open(request)) {
        fail(*f);
        return;
    }

    // A bigger pipe rides out short pauses in FernSDR; the default is kept
    // when the kernel refuses.
    (void)::fcntl(io_.samples, F_SETPIPE_SZ, 1 << 20);

    const Effective& e = receiver_.effective();
    json::Value ready = json::Value::object();
    ready.set("type", "ready");
    ready.set("format", "u8");
    ready.set("signal", "iq");
    ready.set("sample_rate", e.sample_rate);
    ready.set("center", e.center);
    ready.set("device", receiver_.device_json());
    ready.set("settings", receiver_.settings_json());
    if (!send(ready))
        return;

    stream_ = std::make_unique<Stream>(receiver_.device(), io_.samples, notify_fd_, options_.ring_bytes, e.buffers,
                                       transfer_bytes_for_rate(request.sample_rate));
    if (!stream_->start()) {
        stream_.reset();
        fail(Failure{ErrorCode::internal, "could not start the streaming threads"});
        return;
    }
    state_ = State::streaming;
    next_stats_ = Clock::now() + options_.stats_interval;
    next_drop_log_ = Clock::now();
}

void Session::handle_set(const json::Value& message) {
    const json::Value* id = message.find("id");
    // FernSDR's ids are small numbers; an absurd one cannot be echoed within
    // the line limit, so the answer goes without it.
    if (id && json::serialize(*id).size() > 1024) {
        log_line("a set carried an id longer than 1024 bytes");
        id = nullptr;
    }
    auto refuse = [&](const Failure& f) {
        log_line("refused a set: %s", f.message.c_str());
        json::Value e = json::Value::object();
        e.set("type", "error");
        if (id)
            e.set("id", *id);
        e.set("code", error_code_name(f.code));
        e.set("message", f.message);
        e.set("fatal", false);
        send(e);
    };
    if (state_ != State::streaming) {
        refuse(Failure{ErrorCode::invalid, "set arrived before the RTL-SDR was opened"});
        return;
    }
    LiveChange change;
    if (const json::Value* settings = message.find("settings"))
        if (auto f = parse_set(*settings, change)) {
            refuse(*f);
            return;
        }
    if (auto f = receiver_.apply(change)) {
        refuse(*f);
        return;
    }
    json::Value applied = json::Value::object();
    applied.set("type", "applied");
    if (id)
        applied.set("id", *id);
    applied.set("settings", receiver_.settings_json(change));
    send(applied);
}

void Session::check_stream() {
    if (!stream_)
        return;
    switch (stream_->writer_end()) {
    case Stream::WriterEnd::host_gone:
        log_line("FernSDR closed fd 1; stopping");
        finish(exit_status::stopped);
        return;
    case Stream::WriterEnd::failed:
        fail(Failure{ErrorCode::internal,
                     std::string("writing samples to fd 1 failed: ") + std::strerror(stream_->writer_errno())});
        return;
    case Stream::WriterEnd::running:
    case Stream::WriterEnd::stopped:
        break;
    }
    if (stream_->reader_ended() && !stream_->stop_requested()) {
        device_lost_ = true;
        if (stream_->bytes_received() == 0)
            fail(Failure{ErrorCode::lost,
                         "the RTL-SDR did not start streaming. If this repeats, the host may lack USB memory for "
                         "the transfers: lower module.buffers, or raise "
                         "/sys/module/usbcore/parameters/usbfs_memory_mb"});
        else
            fail(Failure{ErrorCode::lost,
                         "the RTL-SDR stopped streaming: it was unplugged or its USB connection failed. Check "
                         "the cable and the power supply"});
    }
}

void Session::on_tick() {
    const Clock::time_point now = Clock::now();
    if (now - stream_->last_data() > options_.stall_timeout) {
        device_lost_ = true;
        device_stalled_ = true;
        const double seconds = std::chrono::duration<double>(options_.stall_timeout).count();
        char text[200];
        std::snprintf(text, sizeof text,
                      "the RTL-SDR sent no samples for %.1f seconds; its USB connection has probably failed. "
                      "Check the cable and the power supply",
                      seconds);
        fail(Failure{ErrorCode::lost, text});
        return;
    }
    if (now >= next_stats_) {
        json::Value stats = json::Value::object();
        stats.set("type", "stats");
        stats.set("samples", stream_->samples_delivered());
        stats.set("dropped", stream_->samples_dropped());
        if (!send(stats))
            return;
        next_stats_ += options_.stats_interval;
        if (next_stats_ <= now)
            next_stats_ = now + options_.stats_interval;
    }
    const uint64_t dropped = stream_->samples_dropped();
    if (dropped > logged_drops_ && now >= next_drop_log_) {
        log_line("%llu samples dropped so far because FernSDR did not read them fast enough",
                 static_cast<unsigned long long>(dropped));
        logged_drops_ = dropped;
        next_drop_log_ = now + std::chrono::seconds(10);
    }
}

SessionResult Session::run() {
    std::signal(SIGPIPE, SIG_IGN);
    notify_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (notify_fd_ < 0) {
        log_line("could not create an eventfd: %s", std::strerror(errno));
        return SessionResult{exit_status::internal, true};
    }

    json::Value hello = json::Value::object();
    hello.set("type", "hello");
    hello.set("api", module_api);
    hello.set("id", module_id);
    hello.set("version", module_version());
    hello.set("kind", "input");
    send(hello);

    while (!exit_) {
        struct pollfd fds[3];
        int count = 0;
        int commands = -1;
        int stop = -1;
        if (commands_open_) {
            fds[count] = {io_.commands, POLLIN, 0};
            commands = count++;
        }
        if (io_.stop >= 0) {
            fds[count] = {io_.stop, POLLIN, 0};
            stop = count++;
        }
        fds[count] = {notify_fd_, POLLIN, 0};
        const int notify = count++;

        // While streaming, wake up often enough for stats and the stall check.
        const int timeout = state_ == State::streaming ? 100 : -1;
        const int r = ::poll(fds, static_cast<nfds_t>(count), timeout);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            fail(Failure{ErrorCode::internal, std::string("poll failed: ") + std::strerror(errno)});
            break;
        }
        if (stop >= 0 && fds[stop].revents != 0) {
            char buf[128];
            const ssize_t ignored = ::read(io_.stop, buf, sizeof buf);
            (void)ignored;
            log_line("stopping on a signal");
            finish(exit_status::stopped);
            break;
        }
        if (commands >= 0 && fds[commands].revents != 0)
            read_commands();
        if (exit_)
            break;
        if (fds[notify].revents != 0) {
            uint64_t n;
            const ssize_t ignored = ::read(notify_fd_, &n, sizeof n);
            (void)ignored;
            check_stream();
        }
        if (!exit_ && state_ == State::streaming)
            on_tick();
    }
    return shut_down();
}

SessionResult Session::shut_down() {
    SessionResult result{exit_.value_or(exit_status::stopped), true};
    // Stopping and closing share one budget, so that the module is gone
    // before FernSDR resorts to SIGTERM.
    const Clock::time_point deadline = Clock::now() + options_.shutdown_timeout;
    if (stream_) {
        stream_->request_stop();
        if (!stream_->wait(deadline)) {
            // A thread is still inside librtlsdr or blocked on fd 1. Closing
            // the device now could hang or crash; the kernel releases it when
            // the process exits.
            log_line("streaming did not stop within %lld ms; exiting without closing the RTL-SDR",
                     static_cast<long long>(options_.shutdown_timeout.count()));
            receiver_.abandon();
            (void)stream_.release();
            leaked_ = true;
            result.clean = false;
            return result;
        }
        stream_.reset();
    }
    // A dongle that stopped streaming but is still attached may not answer
    // control transfers either, and rtlsdr_close() would wait out a 300 ms
    // timeout for each of them. The kernel releases it when the process exits.
    if (device_stalled_) {
        receiver_.abandon();
    } else if (!receiver_.close_by(std::max(deadline, Clock::now() + std::chrono::milliseconds(250)),
                                   !device_lost_)) {
        log_line("closing the RTL-SDR did not finish in time; exiting without waiting for it");
        result.clean = false;
    }
    return result;
}

}  // namespace

SessionResult run_session(Backend& backend, const SessionIo& io, const SessionOptions& options) {
    Session session(backend, io, options);
    return session.run();
}

}  // namespace fern
