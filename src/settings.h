// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The settings the module declares (printed by --describe and copied into
// the package manifest) and the validation of open and set against them.
// Validation here needs no hardware; checks that depend on the tuner happen
// in receiver.cpp.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "failure.h"
#include "json.h"

namespace fern {

constexpr const char* module_id = "rtlsdr";
constexpr const char* module_name = "RTL-SDR";
constexpr int module_api = 1;
const char* module_version();

enum class DirectSampling { off = 0, i = 1, q = 2 };
const char* direct_sampling_name(DirectSampling mode);

struct DeviceSelector {
    enum class Kind { only, serial, index };
    Kind kind = Kind::only;
    std::string serial;
    uint32_t index = 0;
};

struct GainSetting {
    bool automatic = true;
    double db = 0;  // when not automatic
};

struct ModuleSettings {
    DeviceSelector device;
    GainSetting gain;
    int ppm = 0;
    bool rtl_agc = false;
    bool bias_tee = false;
    DirectSampling direct_sampling = DirectSampling::off;
    bool offset_tuning = false;
    uint32_t bandwidth = 0;  // Hz, 0 = automatic
    uint32_t buffers = 16;
};

struct OpenRequest {
    uint32_t sample_rate = 0;
    uint32_t center = 0;
    ModuleSettings settings;
};

// The settings that may change while samples flow.
struct LiveChange {
    std::optional<GainSetting> gain;
    std::optional<bool> rtl_agc;
    std::optional<bool> bias_tee;
};

// rtlsdr_set_sample_freq_correction() stores the correction as a 14-bit
// signed register value of ppm * 2^24 / 10^6; beyond +-488 it wraps around.
constexpr int min_ppm = -488;
constexpr int max_ppm = 488;
constexpr uint32_t max_bandwidth = 8000000;
constexpr uint32_t min_buffers = 2;
constexpr uint32_t max_buffers = 64;
constexpr uint32_t default_buffers = 16;
// Above this rate many hosts lose samples without any error being reported.
constexpr uint32_t reliable_sample_rate = 2400000;

// The ranges the RTL2832U resampler accepts, as librtlsdr checks them.
bool sample_rate_supported(uint32_t hz);

// Validates an open message. Fails with ErrorCode::invalid.
std::optional<Failure> parse_open(const json::Value& message, OpenRequest& out);

// Validates the settings object of a set message: only live settings may
// appear. Fails with ErrorCode::invalid.
std::optional<Failure> parse_set(const json::Value& settings, LiveChange& out);

// The settings list, identical in --describe and in the package manifest.
json::Value settings_schema();
// The complete --describe object.
json::Value describe_module();

}  // namespace fern
