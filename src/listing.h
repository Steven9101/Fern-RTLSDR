// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>

#include "device.h"
#include "json.h"

namespace fern {

// FernSDR reads at most this much from --describe and --list-devices.
constexpr size_t max_report_bytes = 64 * 1024;

struct Listing {
    json::Value report;  // {"devices":[...]}
    // False when the budget ran out: a worker thread may still be inside
    // librtlsdr and still uses the backend, so the caller must keep the
    // backend alive and should leave with _exit().
    bool complete = true;
};

// Lists the RTL-SDRs, opening each briefly to read its tuner and gains.
// Devices not examined within the budget are reported as not usable.
Listing list_devices(Backend& backend, std::chrono::milliseconds budget);

// Serializes a report, dropping detail and then devices until it fits into
// max_report_bytes including the newline.
std::string report_text(const json::Value& report);

}  // namespace fern
