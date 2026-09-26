// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "io.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <unistd.h>

namespace fern {

int write_all(int fd, const void* data, size_t len, int timeout_ms, int stop_fd) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        const ssize_t n = ::write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // The fd was handed over non-blocking; wait the way a
                // blocking write would, within the limits given.
                int wait_ms = -1;
                if (timeout_ms >= 0) {
                    const auto left =
                        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
                    if (left <= 0)
                        return ETIMEDOUT;
                    wait_ms = static_cast<int>(left);
                }
                struct pollfd pfd[2] = {{fd, POLLOUT, 0}, {stop_fd, POLLIN, 0}};
                const int r = ::poll(pfd, stop_fd >= 0 ? 2 : 1, wait_ms);
                if (r < 0 && errno != EINTR)
                    return errno;
                if (stop_fd >= 0 && pfd[1].revents != 0)
                    return ECANCELED;
                continue;
            }
            return errno;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return 0;
}

void LineReader::feed(const char* data, size_t len) {
    size_t i = 0;
    while (i < len) {
        const void* found = std::memchr(data + i, '\n', len - i);
        const size_t end = found ? static_cast<size_t>(static_cast<const char*>(found) - data) : len;
        if (skipping_) {
            if (found)
                skipping_ = false;
        } else if (partial_.size() + (end - i) > max_len_) {
            partial_.clear();
            lines_.push_back(Line{true, {}});
            skipping_ = !found;
        } else {
            partial_.append(data + i, end - i);
            if (found) {
                lines_.push_back(Line{false, std::move(partial_)});
                partial_.clear();
            }
        }
        i = found ? end + 1 : len;
    }
}

void LineReader::finish() {
    if (!skipping_ && !partial_.empty())
        lines_.push_back(Line{false, std::move(partial_)});
    partial_.clear();
    skipping_ = false;
}

bool LineReader::next(Line& out) {
    if (lines_.empty())
        return false;
    out = std::move(lines_.front());
    lines_.pop_front();
    return true;
}

}  // namespace fern
