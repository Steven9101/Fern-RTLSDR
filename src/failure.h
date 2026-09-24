// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

namespace fern {

// The error codes of the module contract, each with the exit status that
// follows it when the error is fatal.
enum class ErrorCode { no_device, busy, usb, lost, invalid, internal };

struct Failure {
    ErrorCode code;
    std::string message;
};

namespace exit_status {
constexpr int stopped = 0;
constexpr int internal = 1;
constexpr int usage = 2;
constexpr int no_device = 3;
constexpr int busy = 4;
constexpr int usb = 5;
constexpr int invalid = 6;
}  // namespace exit_status

inline const char* error_code_name(ErrorCode code) {
    switch (code) {
    case ErrorCode::no_device: return "no-device";
    case ErrorCode::busy: return "busy";
    case ErrorCode::usb: return "usb";
    case ErrorCode::lost: return "lost";
    case ErrorCode::invalid: return "invalid";
    case ErrorCode::internal: return "internal";
    }
    return "internal";
}

inline int exit_status_for(ErrorCode code) {
    switch (code) {
    case ErrorCode::no_device: return exit_status::no_device;
    case ErrorCode::busy: return exit_status::busy;
    case ErrorCode::usb: return exit_status::usb;
    case ErrorCode::lost: return exit_status::usb;
    case ErrorCode::invalid: return exit_status::invalid;
    case ErrorCode::internal: return exit_status::internal;
    }
    return exit_status::internal;
}

}  // namespace fern
