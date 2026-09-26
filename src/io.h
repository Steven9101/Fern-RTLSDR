// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <deque>
#include <string>

namespace fern {

// Writes all len bytes, continuing after EINTR and partial writes and
// waiting for room when the fd is non-blocking. Returns 0, or the errno of
// the write that failed.
//
// With timeout_ms >= 0 it waits no longer than that in all and then returns
// ETIMEDOUT. With stop_fd >= 0 it returns ECANCELED as soon as that fd is
// readable, without reading it, so a stop signal that arrives while FernSDR
// is not reading still ends the wait.
int write_all(int fd, const void* data, size_t len, int timeout_ms = -1, int stop_fd = -1);

// Splits a byte stream into lines. A line longer than max_len bytes (not
// counting the newline) is not kept: the reader reports it once as too_long
// and skips the rest of it up to the next newline.
class LineReader {
public:
    explicit LineReader(size_t max_len) : max_len_(max_len) {}

    struct Line {
        bool too_long = false;
        std::string text;
    };

    void feed(const char* data, size_t len);
    // End of input. A last line without a newline still counts.
    void finish();
    bool next(Line& out);

private:
    size_t max_len_;
    std::string partial_;
    bool skipping_ = false;
    std::deque<Line> lines_;
};

}  // namespace fern
