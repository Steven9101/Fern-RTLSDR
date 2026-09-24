// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Opens the RTL-SDR that the settings select, applies the settings in an
// order that librtlsdr handles correctly, and checks what the hardware
// reports before anything is announced to FernSDR.
#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "device.h"
#include "failure.h"
#include "json.h"
#include "settings.h"

namespace fern {

struct DeviceInfo {
    uint32_t index = 0;
    std::string name;
    std::string serial;
    Tuner tuner = Tuner::unknown;
    bool blog_v4 = false;
};

// What the hardware was actually set to.
struct Effective {
    double sample_rate = 0;
    uint32_t center = 0;
    GainSetting gain;
    int ppm = 0;
    bool rtl_agc = false;
    bool bias_tee = false;                   // as requested
    std::optional<bool> bias_tee_effective;  // empty when it cannot be known
    DirectSampling direct_sampling = DirectSampling::off;
    bool offset_tuning = false;
    uint32_t bandwidth = 0;  // the tuner's IF filter; 0 in direct sampling
    uint32_t buffers = default_buffers;
};

// The rate the RTL2832U runs at for a request, computed the way
// rtlsdr_set_sample_rate() programs the resampler.
double achieved_sample_rate(uint32_t xtal_hz, uint32_t requested_hz);

// Index of the table entry (tenths of a dB) nearest to db; a tie goes to the
// lower gain. -1 for an empty table.
int nearest_gain(const std::vector<int>& tenths, double db);

// The IF filter bandwidth librtlsdr's tuner driver sets for a requested
// bandwidth: r82xx_set_bandwidth() for the R820T and R828D, the narrowest of
// the three filters e4000_set_bw() sets for the E4000, and the fixed filters
// of the FC0012, FC0013 (6 MHz) and FC2580 (1.53 MHz). 0 for an unknown tuner.
uint32_t tuner_filter_bandwidth(Tuner tuner, uint32_t requested_hz);

// Bytes per USB transfer: about 20 ms of samples, a multiple of 16 KiB.
uint32_t transfer_bytes_for_rate(uint32_t sample_rate);

// A name for the operator from the USB strings, e.g. "RTL-SDR Blog V4".
std::string display_name(const UsbStrings& strings);

class Receiver {
public:
    explicit Receiver(Backend& backend) : backend_(backend) {}
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    // Selects and opens the device, applies every setting and verifies the
    // result. On failure the device is closed again.
    std::optional<Failure> open(const OpenRequest& request);

    // Applies a set. Validation failures change nothing; a failure with
    // ErrorCode::usb may leave the change half done.
    std::optional<Failure> apply(const LiveChange& change);

    bool is_open() const { return device_ != nullptr; }
    Device& device() { return *device_; }
    const DeviceInfo& info() const { return info_; }
    const Effective& effective() const { return effective_; }

    json::Value device_json() const;
    json::Value settings_json() const;
    json::Value settings_json(const LiveChange& change) const;

    // Closes the device. When it still answers, a bias tee the module
    // switched on is switched off first.
    void close(bool device_answers = true);
    // The same, but gives up waiting at the deadline, leaving the close to
    // finish (or hang) in a thread of its own; the process must then leave
    // with _exit(). Returns whether the close finished.
    bool close_by(std::chrono::steady_clock::time_point deadline, bool device_answers = true);
    // Drops the device without closing it, for when another thread may still
    // be inside librtlsdr. The process must exit soon after.
    void abandon();

private:
    Backend& backend_;
    std::unique_ptr<Device> device_;
    DeviceInfo info_;
    Effective effective_;
    std::vector<int> gains_;
    std::optional<bool> bias_forced_;
    bool bias_on_by_module_ = false;
    bool manual_gain_ = false;

    std::optional<Failure> select(const DeviceSelector& selector, uint32_t& index);
    std::optional<Failure> configure(const OpenRequest& request);
    std::optional<Failure> check_gain(const GainSetting& gain, bool direct_sampling, int& tenths) const;
    std::optional<Failure> set_gain(const GainSetting& gain);
    std::optional<Failure> set_bias(bool on);
    Failure tune_failure(uint32_t center) const;
};

}  // namespace fern
