// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace fern {

// Human-readable log lines, fd 2 unless changed; -1 silences them.
void set_log_fd(int fd);

// One line of at most 1 KiB including the newline, written with a single
// write() so that lines from different threads do not interleave.
void log_line(const char* format, ...) __attribute__((format(printf, 1, 2)));

}  // namespace fern
