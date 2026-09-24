// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <unistd.h>

namespace fern {

namespace {
std::atomic<int> log_fd{2};
}

void set_log_fd(int fd) { log_fd.store(fd); }

void log_line(const char* format, ...) {
    const int fd = log_fd.load();
    if (fd < 0)
        return;

    // FernSDR accepts log lines of up to 1 KiB, newline included. Formatting
    // into a larger buffer keeps the first byte past the cut available, so a
    // UTF-8 sequence is never split.
    constexpr size_t max_text = 1023;
    char buf[2048];
    va_list ap;
    va_start(ap, format);
    const int n = std::vsnprintf(buf, sizeof buf, format, ap);
    va_end(ap);
    if (n < 0)
        return;

    size_t len = static_cast<size_t>(n) < sizeof buf ? static_cast<size_t>(n) : sizeof buf - 1;
    if (len > max_text) {
        len = max_text;
        while (len > 0 && (static_cast<unsigned char>(buf[len]) & 0xC0) == 0x80)
            --len;
    }
    for (size_t i = 0; i < len; ++i)
        if (buf[i] == '\n' || buf[i] == '\r')
            buf[i] = ' ';
    buf[len++] = '\n';
    const ssize_t ignored = ::write(fd, buf, len);
    (void)ignored;
}

}  // namespace fern
