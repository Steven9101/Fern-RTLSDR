// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The protocol loop of docs/MODULES.md in FernSDR: commands on one fd,
// samples on another, events on a third.
#pragma once

#include <chrono>
#include <cstddef>

#include "device.h"
#include "gain_control.h"

namespace fern {

struct SessionIo {
    int commands = 0;
    int samples = 1;
    int events = 3;
    // Readable when the module should stop (a signalfd for SIGTERM and SIGINT
    // in the real program); -1 for none.
    int stop = -1;
};

struct SessionOptions {
    std::chrono::milliseconds stats_interval{1000};
    // FernSDR gives up on a module after 2 s without samples; noticing a
    // stalled dongle a little earlier lets the module say why.
    std::chrono::milliseconds stall_timeout{1500};
    // How long stopping the stream and closing the device may take before
    // the module leaves the device to the kernel. FernSDR sends SIGTERM
    // after 2 s.
    std::chrono::milliseconds shutdown_timeout{1500};
    size_t ring_bytes = 8u << 20;
    // gain = auto; the settle time is at least what the USB transfers in
    // flight hold.
    GainControlTiming gain_timing;
};

struct SessionResult {
    int status = 0;
    // False when a thread may still be inside librtlsdr: the caller must then
    // leave with _exit() instead of returning from main().
    bool clean = true;
};

SessionResult run_session(Backend& backend, const SessionIo& io, const SessionOptions& options = SessionOptions());

}  // namespace fern
