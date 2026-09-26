// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Runs the session loop in a thread with real pipes in place of fds 0 to 3
// and plays FernSDR's part.
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "fake_backend.h"
#include "json.h"
#include "session.h"
#include "test.h"

using fern::json::Value;
using Clock = std::chrono::steady_clock;

namespace {

struct Pipe {
    int r = -1;
    int w = -1;
    Pipe() {
        int fds[2];
        if (::pipe2(fds, O_CLOEXEC) == 0) {
            r = fds[0];
            w = fds[1];
        }
    }
    ~Pipe() {
        close_r();
        close_w();
    }
    void close_r() {
        if (r >= 0)
            ::close(r);
        r = -1;
    }
    void close_w() {
        if (w >= 0)
            ::close(w);
        w = -1;
    }
};

fern::SessionOptions quick_options() {
    fern::SessionOptions o;
    o.stats_interval = std::chrono::milliseconds(100);
    o.stall_timeout = std::chrono::milliseconds(800);
    o.shutdown_timeout = std::chrono::milliseconds(1500);
    o.ring_bytes = 1 << 20;
    return o;
}

const char* const open_line =
    "{\"type\":\"open\",\"sample_rate\":2400000,\"center\":100000000,\"signal\":\"iq\",\"settings\":{}}";

class Harness {
public:
    // With nonblocking, the module's ends of fds 0, 1 and 3 are handed over
    // with O_NONBLOCK set, as a host that uses pipe2(O_NONBLOCK) would.
    explicit Harness(fake::Backend& backend, fern::SessionOptions options = quick_options(),
                     bool nonblocking = false) {
        REQUIRE(commands_.r >= 0 && samples_.r >= 0 && events_.r >= 0 && stop_.r >= 0);
        if (nonblocking)
            for (int fd : {commands_.r, samples_.w, events_.w})
                REQUIRE(::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK) == 0);
        fern::SessionIo io;
        io.commands = commands_.r;
        io.samples = samples_.w;
        io.events = events_.w;
        io.stop = stop_.r;
        thread_ = std::thread([this, &backend, io, options] {
            result_ = fern::run_session(backend, io, options);
            done_.store(true);
        });
    }

    ~Harness() {
        // Whatever happened, make the session end so that the thread can be
        // joined: this is what FernSDR's leaving looks like.
        commands_.close_w();
        samples_.close_r();
        events_.close_r();
        if (thread_.joinable())
            thread_.join();
    }

    void send(const std::string& line) {
        const std::string text = line + "\n";
        REQUIRE(::write(commands_.w, text.data(), text.size()) == static_cast<ssize_t>(text.size()));
    }
    void close_commands() { commands_.close_w(); }
    void close_samples() { samples_.close_r(); }
    void signal_stop() { REQUIRE(::write(stop_.w, "x", 1) == 1); }
    // Makes the events pipe hold only one page, so that a few unread events
    // fill it.
    void shrink_events() { REQUIRE(::fcntl(events_.w, F_SETPIPE_SZ, 4096) >= 0); }

    // The next event on fd 3, or null after the timeout.
    Value event(int timeout_ms = 3000) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            const size_t nl = events_buf_.find('\n');
            if (nl != std::string::npos) {
                const std::string line = events_buf_.substr(0, nl);
                events_buf_.erase(0, nl + 1);
                Value v;
                std::string error;
                if (!fern::json::parse(line, v, error))
                    test::report(__FILE__, __LINE__, "event is not valid JSON: " + line + ": " + error);
                if (line.size() > 65536)
                    test::report(__FILE__, __LINE__, "event longer than 64 KiB");
                lines_.push_back(line);
                return v;
            }
            const int left = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
            if (left <= 0)
                return Value();
            struct pollfd pfd = {events_.r, POLLIN, 0};
            if (::poll(&pfd, 1, left) <= 0)
                continue;
            char buf[4096];
            const ssize_t n = ::read(events_.r, buf, sizeof buf);
            if (n <= 0)
                return Value();
            events_buf_.append(buf, static_cast<size_t>(n));
        }
    }

    // The next event of the given type, skipping stats. The timeout covers
    // all of them, so a stream of stats cannot keep a test waiting.
    Value expect(const std::string& type, int timeout_ms = 3000) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            Value v = left > 0 ? event(static_cast<int>(left)) : Value();
            if (v.is_null()) {
                test::report(__FILE__, __LINE__, "no " + type + " event");
                throw test::Stop{};
            }
            const std::string t = v.find("type") ? v.find("type")->as_string() : "";
            if (t == type)
                return v;
            if (t != "stats") {
                test::report(__FILE__, __LINE__, "expected " + type + ", got " + lines_.back());
                throw test::Stop{};
            }
        }
    }

    // Reads exactly n sample bytes.
    std::vector<uint8_t> samples(size_t n, int timeout_ms = 3000) {
        std::vector<uint8_t> out;
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (out.size() < n && Clock::now() < deadline) {
            struct pollfd pfd = {samples_.r, POLLIN, 0};
            if (::poll(&pfd, 1, 50) <= 0)
                continue;
            std::vector<uint8_t> buf(n - out.size());
            const ssize_t got = ::read(samples_.r, buf.data(), buf.size());
            if (got <= 0)
                break;
            out.insert(out.end(), buf.begin(), buf.begin() + got);
        }
        return out;
    }

    // Reads and discards sample bytes for a while; returns how many.
    size_t drain_samples(int ms) {
        size_t total = 0;
        const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
        std::vector<uint8_t> buf(1 << 16);
        while (Clock::now() < deadline) {
            struct pollfd pfd = {samples_.r, POLLIN, 0};
            if (::poll(&pfd, 1, 20) <= 0)
                continue;
            const ssize_t got = ::read(samples_.r, buf.data(), buf.size());
            if (got <= 0)
                break;
            total += static_cast<size_t>(got);
        }
        return total;
    }

    bool samples_pending() {
        struct pollfd pfd = {samples_.r, POLLIN, 0};
        return ::poll(&pfd, 1, 0) > 0;
    }

    // Waits for the session to end and returns its exit status.
    int exit_status(int timeout_ms = 4000, bool clean = true) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!done_.load() && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(done_.load());
        thread_.join();
        CHECK_EQ(result_.clean, clean);
        return result_.status;
    }

    bool done() const { return done_.load(); }
    const std::vector<std::string>& lines() const { return lines_; }

private:
    Pipe commands_;
    Pipe samples_;
    Pipe events_;
    Pipe stop_;
    std::thread thread_;
    std::atomic<bool> done_{false};
    fern::SessionResult result_;
    std::string events_buf_;
    std::vector<std::string> lines_;
};

void check_hello(Harness& h) {
    const Value hello = h.event();
    REQUIRE(hello.is_object());
    CHECK_EQ(fern::json::serialize(hello),
             std::string("{\"type\":\"hello\",\"api\":1,\"id\":\"rtlsdr\",\"version\":\"") + FERN_RTLSDR_VERSION +
                 "\",\"kind\":\"input\"}");
}

std::string text_of(const Value& v, const char* key) {
    const Value* f = v.find(key);
    return f && f->is_string() ? f->as_string() : "";
}

}  // namespace

TEST(session_full_exchange) {
    fake::Backend backend({fake::Spec{}});
    Harness h(backend);
    check_hello(h);
    h.send("{\"type\":\"open\",\"sample_rate\":2400000,\"center\":100000000,\"signal\":\"iq\","
           "\"settings\":{\"gain\":\"auto\",\"bias_tee\":false}}");
    const Value ready = h.expect("ready");
    CHECK_EQ(text_of(ready, "format"), std::string("u8"));
    CHECK_EQ(text_of(ready, "signal"), std::string("iq"));
    CHECK_EQ(ready.find("sample_rate")->as_number(), 2400000.0);
    CHECK_EQ(ready.find("center")->as_number(), 100000000.0);
    CHECK_EQ(fern::json::serialize(*ready.find("device")),
             std::string("{\"name\":\"Realtek RTL2838UHIDIR\",\"serial\":\"00000001\",\"tuner\":\"R820T\",\"index\":0}"));
    CHECK_EQ(text_of(*ready.find("settings"), "gain"), std::string("auto"));

    // Every byte on fd 1 is the fake's tone, in order, from the first one.
    const std::vector<uint8_t> s = h.samples(300000);
    REQUIRE(s.size() == 300000);
    bool exact = true;
    for (size_t i = 0; i < s.size(); ++i)
        exact = exact && s[i] == fake::tone_byte(i);
    CHECK(exact);

    const Value stats = h.expect("stats");
    CHECK(stats.find("samples")->as_number() > 0);
    CHECK_EQ(stats.find("dropped")->as_number(), 0.0);

    h.send("{\"type\":\"set\",\"id\":7,\"settings\":{\"gain\":38.6}}");
    const Value applied = h.expect("applied");
    CHECK_EQ(fern::json::serialize(applied), std::string("{\"type\":\"applied\",\"id\":7,\"settings\":{\"gain\":38.6}}"));
    CHECK_EQ(backend.last()->gain, 386);

    h.send("{\"type\":\"set\",\"id\":\"eight\",\"settings\":{\"rtl_agc\":true,\"bias_tee\":true}}");
    CHECK_EQ(fern::json::serialize(h.expect("applied")),
             std::string("{\"type\":\"applied\",\"id\":\"eight\",\"settings\":{\"rtl_agc\":true,\"bias_tee\":true,"
                         "\"bias_tee_effective\":true}}"));
    CHECK(backend.last()->bias_tee);

    // Refused sets answer with a non-fatal error carrying the id, and the
    // stream goes on.
    h.send("{\"type\":\"set\",\"id\":9,\"settings\":{\"ppm\":3}}");
    const Value refused = h.expect("error");
    CHECK_EQ(refused.find("id")->as_number(), 9.0);
    CHECK_EQ(text_of(refused, "code"), std::string("invalid"));
    CHECK_HAS(text_of(refused, "message"), "module.ppm cannot change while the band runs");
    CHECK(!refused.find("fatal")->as_bool());
    h.send("{\"type\":\"set\",\"id\":10,\"settings\":{\"gain\":\"loud\"}}");
    CHECK_EQ(h.expect("error").find("id")->as_number(), 10.0);
    h.send("{\"type\":\"set\",\"id\":11,\"settings\":{\"volume\":1}}");
    CHECK_HAS(text_of(h.expect("error"), "message"), "unknown setting module.volume");
    CHECK(h.drain_samples(200) > 0);

    // stats only grow.
    const double before = h.expect("stats").find("samples")->as_number();
    h.drain_samples(250);
    const double after = h.expect("stats").find("samples")->as_number();
    CHECK(after > before);

    h.send("{\"type\":\"stop\"}");
    CHECK_EQ(h.exit_status(), 0);
    const auto st = backend.last();
    CHECK(st->closed);
    CHECK(!st->bias_tee);  // switched off again before closing
    CHECK_EQ(st->buf_num, 16u);
    CHECK_EQ(st->buf_len, 98304u);
}

TEST(session_works_with_nonblocking_fds) {
    fake::Spec fast;
    fast.realtime = false;
    fake::Backend backend({fast});
    fern::SessionOptions o = quick_options();
    o.ring_bytes = 1 << 18;
    Harness h(backend, o, true);
    check_hello(h);
    h.send(open_line);
    h.expect("ready");
    // Not reading fd 1 fills the pipe; the module must wait, not fail.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const std::vector<uint8_t> s = h.samples(100000);
    REQUIRE(s.size() == 100000);
    bool exact = true;
    for (size_t i = 0; i < s.size(); ++i)
        exact = exact && s[i] == fake::tone_byte(i);
    CHECK(exact);
    h.send("{\"type\":\"set\",\"id\":1,\"settings\":{\"gain\":20}}");
    CHECK_EQ(fern::json::serialize(h.expect("applied")),
             std::string("{\"type\":\"applied\",\"id\":1,\"settings\":{\"gain\":19.7}}"));
    CHECK(h.expect("stats").find("dropped")->as_number() > 0);
    const auto start = Clock::now();
    h.send("{\"type\":\"stop\"}");
    CHECK_EQ(h.exit_status(), 0);
    CHECK(Clock::now() - start < std::chrono::milliseconds(500));
}

TEST(session_ignores_what_it_does_not_know) {
    fake::Backend backend({fake::Spec{}});
    Harness h(backend);
    check_hello(h);
    h.send("not json at all");
    h.send("[1,2,3]");
    h.send("{\"no\":\"type\"}");
    h.send("{\"type\":\"future-command\",\"payload\":{\"x\":1}}");
    h.send("   ");
    h.send(std::string(70000, 'x'));
    h.send("{\"type\":\"open\",\"sample_rate\":2400000,\"center\":100000000,\"signal\":\"iq\",\"settings\":{},"
           "\"future_field\":[1]}");
    h.expect("ready");
    h.send("{\"type\":\"open\",\"sample_rate\":2400000,\"center\":100000000,\"signal\":\"iq\"}");
    h.send("{\"type\":\"set\",\"id\":1,\"settings\":{\"gain\":\"auto\"},\"extra\":true}");
    CHECK_EQ(h.expect("applied").find("id")->as_number(), 1.0);
    CHECK_EQ(backend.opens(), 1);
    h.close_commands();
    CHECK_EQ(h.exit_status(), 0);
}

TEST(session_answers_set_before_open) {
    fake::Backend backend({fake::Spec{}});
    Harness h(backend);
    check_hello(h);
    h.send("{\"type\":\"set\",\"id\":3,\"settings\":{\"gain\":\"auto\"}}");
    const Value e = h.expect("error");
    CHECK_EQ(e.find("id")->as_number(), 3.0);
    CHECK(!e.find("fatal")->as_bool());
    h.send("{\"type\":\"stop\"}");
    CHECK_EQ(h.exit_status(), 0);
    CHECK_EQ(backend.opens(), 0);
}

TEST(session_exits_on_eof_before_open) {
    fake::Backend backend({fake::Spec{}});
    Harness h(backend);
    check_hello(h);
    h.close_commands();
    CHECK_EQ(h.exit_status(), 0);
}

TEST(session_exits_on_eof_while_streaming) {
    fake::Backend backend({fake::Spec{}});
    Harness h(backend);
    check_hello(h);
    h.send(open_line);
    h.expect("ready");
    CHECK(h.samples(10000).size() == 10000);
    const auto start = Clock::now();
    h.close_commands();
    CHECK_EQ(h.exit_status(), 0);
    CHECK(Clock::now() - start < std::chrono::seconds(1));
    CHECK(backend.last()->closed);
}

TEST(session_exits_on_epipe) {
    fake::Backend backend({fake::Spec{}});
    Harness h(backend);
    check_hello(h);
    h.send(open_line);
    h.expect("ready");
    CHECK(h.samples(10000).size() == 10000);
    const auto start = Clock::now();
    h.close_samples();
    CHECK_EQ(h.exit_status(), 0);
    CHECK(Clock::now() - start < std::chrono::seconds(1));
    CHECK(backend.last()->closed);
}

TEST(session_stops_on_signal_with_a_blocked_writer) {
    // FernSDR stops reading fd 1 but keeps it open: the writer blocks in
    // write() and must still be stopped promptly.
    fake::Spec fast;
    fast.realtime = false;
    fake::Backend backend({fast});
    Harness h(backend);
    check_hello(h);
    h.send(open_line);
    h.expect("ready");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto start = Clock::now();
    h.signal_stop();
    CHECK_EQ(h.exit_status(), 0);
    CHECK(Clock::now() - start < std::chrono::milliseconds(1500));
    CHECK(backend.last()->closed);
}

TEST(session_stops_on_signal_while_fernsdr_does_not_read_events) {
    // FernSDR alive but not reading fd 3: the module used to wait for room
    // there with no limit, where SIGTERM could not reach it.
    fake::Backend backend({fake::Spec{}});
    Harness h(backend);
    check_hello(h);
    h.send(open_line);
    h.expect("ready");
    h.shrink_events();
    for (int i = 0; i < 200; ++i)
        h.send("{\"type\":\"set\",\"id\":" + std::to_string(i) + ",\"settings\":{\"gain\":38.6}}");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto start = Clock::now();
    h.signal_stop();
    CHECK_EQ(h.exit_status(), 0);
    CHECK(Clock::now() - start < std::chrono::milliseconds(1500));
    CHECK(backend.last()->closed);
}

TEST(session_gives_up_on_events_nobody_reads) {
    // And without a signal, it does not wait forever either.
    fake::Backend backend({fake::Spec{}});
    Harness h(backend);
    check_hello(h);
    h.send(open_line);
    h.expect("ready");
    h.shrink_events();
    for (int i = 0; i < 200; ++i)
        h.send("{\"type\":\"set\",\"id\":" + std::to_string(i) + ",\"settings\":{\"gain\":38.6}}");
    CHECK_EQ(h.exit_status(6000), 1);  // exit_status::internal
}

TEST(session_stop_right_after_ready) {
    // The stop may arrive before read_async() has even started.
    for (int i = 0; i < 20; ++i) {
        fake::Backend backend({fake::Spec{}});
        Harness h(backend);
        check_hello(h);
        h.send(open_line);
        h.send("{\"type\":\"stop\"}");
        h.expect("ready");
        CHECK_EQ(h.exit_status(), 0);
        CHECK(backend.last()->closed);
    }
}

TEST(session_does_not_wait_for_a_slow_close) {
    // rtlsdr_close() can take long or hang; the module leaves in time anyway
    // and says that it did not shut down cleanly.
    fake::Spec s;
    s.close_delay_ms = 800;
    fake::Backend backend({s});
    fern::SessionOptions o = quick_options();
    o.shutdown_timeout = std::chrono::milliseconds(300);
    Harness h(backend, o);
    check_hello(h);
    h.send(open_line);
    h.expect("ready");
    const auto start = Clock::now();
    h.send("{\"type\":\"stop\"}");
    CHECK_EQ(h.exit_status(3000, false), 0);
    const auto took = Clock::now() - start;
    CHECK(took >= std::chrono::milliseconds(250));
    CHECK(took < std::chrono::milliseconds(700));
    // The close still runs in its own thread, so read its state under the lock.
    const auto st = backend.last();
    auto closed = [&] {
        std::lock_guard<std::mutex> lock(st->mutex);
        return st->closed;
    };
    CHECK(!closed());
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    CHECK(closed());
}

TEST(session_reports_unplug) {
    fake::Spec s;
    s.unplug_after = 200000;
    fake::Backend backend({s});
    Harness h(backend);
    check_hello(h);
    h.send(open_line);
    h.expect("ready");
    h.drain_samples(100);
    const Value e = h.expect("error");
    CHECK_EQ(text_of(e, "code"), std::string("lost"));
    CHECK(e.find("fatal")->as_bool());
    CHECK_HAS(text_of(e, "message"), "unplugged");
    CHECK_EQ(h.exit_status(), 5);
    CHECK(backend.last()->closed);
}

TEST(session_reports_a_transfer_start_failure) {
    fake::Spec s;
    s.fail_start = true;
    fake::Backend backend({s});
    Harness h(backend);
    check_hello(h);
    h.send(open_line);
    h.expect("ready");
    const Value e = h.expect("error");
    CHECK_EQ(text_of(e, "code"), std::string("lost"));
    CHECK_HAS(text_of(e, "message"), "usbfs_memory_mb");
    CHECK_EQ(h.exit_status(), 5);
}

TEST(session_reports_a_stall) {
    fake::Spec s;
    s.stall_after = 100000;
    fake::Backend backend({s});
    Harness h(backend);
    check_hello(h);
    h.send(open_line);
    h.expect("ready");
    const auto start = Clock::now();
    const Value e = h.expect("error", 3000);
    const auto waited = Clock::now() - start;
    CHECK_EQ(text_of(e, "code"), std::string("lost"));
    CHECK_HAS(text_of(e, "message"), "sent no samples for 0.8 seconds");
    CHECK(waited >= std::chrono::milliseconds(700));
    CHECK(waited < std::chrono::milliseconds(2000));
    CHECK_EQ(h.exit_status(), 5);
    // A stalled dongle is left to the kernel instead of being closed.
    CHECK(!backend.last()->closed);
}

TEST(session_reports_busy_and_missing_devices) {
    {
        fake::Spec s;
        s.open_error = fern::usb_error::busy;
        fake::Backend backend({s});
        Harness h(backend);
        check_hello(h);
        h.send(open_line);
        const Value e = h.expect("error");
        CHECK_EQ(text_of(e, "code"), std::string("busy"));
        CHECK(e.find("fatal")->as_bool());
        CHECK_EQ(h.exit_status(), 4);
        CHECK(!h.samples_pending());
    }
    {
        fake::Backend backend;
        Harness h(backend);
        check_hello(h);
        h.send(open_line);
        const Value e = h.expect("error");
        CHECK_EQ(text_of(e, "code"), std::string("no-device"));
        CHECK_EQ(text_of(e, "message"),
                 std::string("no RTL-SDR is plugged in. Check the USB cable; lsusb should list the dongle as "
                             "0bda:2838 or 0bda:2832."));
        CHECK_EQ(h.exit_status(), 3);
    }
}

TEST(session_refuses_invalid_settings) {
    fake::Backend backend({fake::Spec{}});
    Harness h(backend);
    check_hello(h);
    h.send("{\"type\":\"open\",\"sample_rate\":2400000,\"center\":100000000,\"signal\":\"iq\","
           "\"settings\":{\"gain\":\"auto\",\"squelch\":3}}");
    const Value e = h.expect("error");
    CHECK_EQ(text_of(e, "code"), std::string("invalid"));
    CHECK_HAS(text_of(e, "message"), "unknown setting module.squelch");
    CHECK(e.find("fatal")->as_bool());
    CHECK_EQ(h.exit_status(), 6);
    CHECK_EQ(backend.opens(), 0);
    CHECK(!h.samples_pending());
}

TEST(session_refuses_real_signal) {
    fake::Backend backend({fake::Spec{}});
    Harness h(backend);
    check_hello(h);
    h.send("{\"type\":\"open\",\"sample_rate\":2400000,\"center\":100000000,\"signal\":\"real\"}");
    CHECK_HAS(text_of(h.expect("error"), "message"), "set signal = iq");
    CHECK_EQ(h.exit_status(), 6);
}

TEST(session_counts_dropped_samples) {
    // FernSDR does not read fd 1 for a while: the pipe and the ring fill up
    // and newer samples are dropped and counted.
    fake::Spec fast;
    fast.realtime = false;
    fake::Backend backend({fast});
    fern::SessionOptions o = quick_options();
    o.ring_bytes = 1 << 18;
    Harness h(backend, o);
    check_hello(h);
    h.send(open_line);
    h.expect("ready");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    double dropped = 0;
    double samples = 0;
    for (int i = 0; i < 20 && dropped == 0; ++i) {
        const Value stats = h.expect("stats");
        dropped = stats.find("dropped")->as_number();
        samples = stats.find("samples")->as_number();
    }
    CHECK(dropped > 0);
    // Nothing delivered was lost: fd 1 holds exactly the first samples.
    const std::vector<uint8_t> s = h.samples(4096);
    REQUIRE(s.size() == 4096);
    bool exact = true;
    for (size_t i = 0; i < s.size(); ++i)
        exact = exact && s[i] == fake::tone_byte(i);
    CHECK(exact);
    CHECK(samples > 0);
    h.send("{\"type\":\"stop\"}");
    CHECK_EQ(h.exit_status(), 0);
}

TEST(session_keeps_events_within_64_kib) {
    fake::Backend backend({fake::Spec{}});
    Harness h(backend);
    check_hello(h);
    h.send(open_line);
    h.expect("ready");
    // An absurd id is not echoed; an absurd key is shortened in the message.
    const std::string huge(20000, '\x7f');
    h.send("{\"type\":\"set\",\"id\":\"" + huge + "\",\"settings\":{\"gain\":\"auto\"}}");
    const Value applied = h.expect("applied");
    CHECK(applied.find("id") == nullptr);
    h.send("{\"type\":\"set\",\"id\":2,\"settings\":{\"" + huge + "\":1}}");
    const Value refused = h.expect("error");
    CHECK_EQ(refused.find("id")->as_number(), 2.0);
    CHECK_HAS(text_of(refused, "message"), "unknown setting module.");
    h.send("{\"type\":\"stop\"}");
    CHECK_EQ(h.exit_status(), 0);
    for (const std::string& line : h.lines())
        CHECK(line.size() < 2048);
}

TEST(session_events_are_valid_json_lines) {
    fake::Spec s;
    s.serial = "odd \"serial\"\n\\ \xFF";
    fake::Backend backend({s});
    Harness h(backend);
    check_hello(h);
    h.send(open_line);
    const Value ready = h.expect("ready");
    CHECK_EQ(text_of(*ready.find("device"), "serial"), std::string("odd \"serial\"\n\\ \xEF\xBF\xBD"));
    h.send("{\"type\":\"stop\"}");
    CHECK_EQ(h.exit_status(), 0);
    for (const std::string& line : h.lines())
        CHECK(line.find('\n') == std::string::npos);
}
