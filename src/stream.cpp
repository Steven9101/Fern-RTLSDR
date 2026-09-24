// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "stream.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <system_error>
#include <unistd.h>

#include "log.h"

namespace fern {

namespace {

size_t round_up_pow2(size_t n) {
    size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

extern "C" void wake_writer(int) {}

// SIGUSR1 interrupts a writer blocked in write(); without SA_RESTART the call
// returns EINTR, and the writer then sees that it should stop.
void install_wake_handler() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof sa);
        sa.sa_handler = wake_writer;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGUSR1, &sa, nullptr);
    });
}

}  // namespace

RingBuffer::RingBuffer(size_t capacity)
    : buf_(new uint8_t[round_up_pow2(std::max<size_t>(capacity, 2))]),
      mask_(round_up_pow2(std::max<size_t>(capacity, 2)) - 1) {}

size_t RingBuffer::size() const {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
}

bool RingBuffer::push(const uint8_t* data, size_t len) {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    if (len > capacity() - (head - tail))
        return false;
    const size_t pos = head & mask_;
    const size_t first = std::min(len, capacity() - pos);
    std::memcpy(buf_.get() + pos, data, first);
    std::memcpy(buf_.get(), data + first, len - first);
    head_.store(head + len, std::memory_order_release);
    return true;
}

size_t RingBuffer::peek(const uint8_t** data) const {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t available = head_.load(std::memory_order_acquire) - tail;
    if (available == 0)
        return 0;
    const size_t pos = tail & mask_;
    *data = buf_.get() + pos;
    return std::min(available, capacity() - pos);
}

void RingBuffer::consume(size_t len) {
    tail_.store(tail_.load(std::memory_order_relaxed) + len, std::memory_order_release);
}

Stream::Stream(Device& device, int samples_fd, int notify_fd, size_t ring_bytes, uint32_t transfers,
               uint32_t transfer_bytes)
    : device_(device),
      samples_fd_(samples_fd),
      notify_fd_(notify_fd),
      transfers_(transfers),
      transfer_bytes_(transfer_bytes),
      ring_(ring_bytes),
      data_event_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {}

Stream::~Stream() {
    if (reader_.joinable() || writer_.joinable()) {
        // The owner waits for both threads first and never destroys a stream
        // whose threads may still run; destroying one would crash anyway.
        request_stop();
        if (!wait(std::chrono::steady_clock::now() + std::chrono::seconds(2))) {
            log_line("internal error: a streaming thread did not stop");
            std::abort();
        }
    }
    if (data_event_ >= 0)
        ::close(data_event_);
}

void Stream::notify(int fd) {
    if (fd < 0)
        return;
    const uint64_t one = 1;
    const ssize_t ignored = ::write(fd, &one, sizeof one);
    (void)ignored;
}

std::chrono::steady_clock::time_point Stream::last_data() const {
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(last_data_ns_.load()));
}

bool Stream::start() {
    install_wake_handler();
    last_data_ns_.store(now_ns());
    try {
        writer_ = std::thread(&Stream::writer_main, this);
    } catch (const std::system_error& e) {
        log_line("could not start the writer thread: %s", e.what());
        return false;
    }
    try {
        reader_ = std::thread(&Stream::reader_main, this);
    } catch (const std::system_error& e) {
        log_line("could not start the USB reader thread: %s", e.what());
        writer_stop_.store(true);
        notify(data_event_);
        writer_.join();
        return false;
    }
    return true;
}

void Stream::on_samples(unsigned char* buf, uint32_t len, void* ctx) {
    Stream* s = static_cast<Stream*>(ctx);
    s->bytes_received_.fetch_add(len, std::memory_order_relaxed);
    s->last_data_ns_.store(now_ns(), std::memory_order_relaxed);
    if (s->stop_requested_.load(std::memory_order_relaxed))
        return;
    if (!s->ring_.push(buf, len)) {
        s->bytes_dropped_.fetch_add(len, std::memory_order_relaxed);
        return;
    }
    s->notify(s->data_event_);
}

void Stream::reader_main() {
    const int r = device_.read_async(&Stream::on_samples, this, transfers_, transfer_bytes_);
    if (r != 0 && !stop_requested_.load())
        log_line("reading samples ended with librtlsdr error %d", r);
    reader_ended_.store(true);
    notify(notify_fd_);
}

void Stream::writer_main() {
    // Large enough to keep the syscall rate low, small enough that a stop is
    // noticed between writes.
    constexpr size_t max_write = 256 * 1024;
    WriterEnd end = WriterEnd::stopped;
    while (!writer_stop_.load()) {
        const uint8_t* data = nullptr;
        size_t n = ring_.peek(&data);
        if (n == 0) {
            // The timeout only guards against a lost wakeup; the producer
            // signals data_event_ after every push.
            struct pollfd pfd = {data_event_, POLLIN, 0};
            ::poll(&pfd, 1, 50);
            uint64_t count;
            if (data_event_ >= 0) {
                const ssize_t ignored = ::read(data_event_, &count, sizeof count);
                (void)ignored;
            }
            continue;
        }
        n = std::min(n, max_write);
        const ssize_t w = ::write(samples_fd_, data, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // fd 1 was handed over non-blocking and the pipe is full.
                struct pollfd pfd = {samples_fd_, POLLOUT, 0};
                ::poll(&pfd, 1, 50);
                continue;
            }
            if (errno == EPIPE) {
                end = WriterEnd::host_gone;
            } else {
                end = WriterEnd::failed;
                writer_errno_.store(errno);
            }
            break;
        }
        ring_.consume(static_cast<size_t>(w));
        bytes_written_.fetch_add(static_cast<uint64_t>(w), std::memory_order_relaxed);
    }
    writer_end_.store(end);
    notify(notify_fd_);
}

void Stream::request_stop() {
    stop_requested_.store(true);
    // -2 means read_async() is not running (yet or any more); wait() repeats
    // the cancel until the reader thread has ended, which is what counts.
    (void)device_.cancel_async();
    writer_stop_.store(true);
    notify(data_event_);
}

bool Stream::wait(std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        const bool reader_done = !reader_.joinable() || reader_ended_.load();
        const bool writer_done = !writer_.joinable() || writer_end_.load() != WriterEnd::running;
        if (reader_done && writer_done) {
            if (reader_.joinable())
                reader_.join();
            if (writer_.joinable())
                writer_.join();
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        // librtlsdr ignores a cancel that arrives before read_async() has
        // started, so repeat it until the reader has ended.
        if (!reader_done)
            (void)device_.cancel_async();
        if (!writer_done)
            pthread_kill(writer_.native_handle(), SIGUSR1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

}  // namespace fern
