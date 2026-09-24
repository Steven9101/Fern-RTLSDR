// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "io.h"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <unistd.h>

namespace fern {

int write_all(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        const ssize_t n = ::write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // The fd was handed over non-blocking; wait the way a
                // blocking write would.
                struct pollfd pfd = {fd, POLLOUT, 0};
                if (::poll(&pfd, 1, -1) < 0 && errno != EINTR)
                    return errno;
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
