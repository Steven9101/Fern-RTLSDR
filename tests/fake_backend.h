// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// A fake RTL-SDR. It follows librtlsdr's return conventions, including its
// quirks, and streams a known tone as u8 I/Q so that tests can check every
// byte that reaches fd 1.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "device.h"

namespace fake {

// Byte i of the stream every fake device sends: a complex tone at a
// sixteenth of the sample rate, I before Q.
uint8_t tone_byte(uint64_t i);

struct Spec {
    std::string manufacturer = "Realtek";
    std::string product = "RTL2838UHIDIR";
    std::string serial = "00000001";
    fern::Tuner tuner = fern::Tuner::r820t;
    int open_error = 0;           // what Backend::open returns
    int strings_error = 0;        // what Backend::usb_strings returns
    int eeprom_byte7 = 0x16;      // < 0: unreadable; bit 1 clear forces the bias tee on
    bool pll_lock = true;
    uint64_t unplug_after = 0;    // bytes streamed before the device vanishes; 0 never
    uint64_t stall_after = 0;     // bytes streamed before the device goes quiet; 0 never
    bool fail_start = false;      // read_async() returns at once without data
    int fail_gain = 0;            // what set_tuner_gain() returns
    bool realtime = true;         // pace the samples at the sample rate
    unsigned open_delay_ms = 0;
    unsigned close_delay_ms = 0;  // how long closing takes
};

// What a fake device was told, for the tests to inspect.
struct State {
    std::mutex mutex;
    bool closed = false;
    uint32_t sample_rate = 0;
    uint32_t center = 0;
    int ppm = 0;
    int direct_sampling = 0;
    int direct_sampling_mode = 0;
    bool offset_tuning = false;
    uint32_t bandwidth = 0;
    bool manual_gain = false;
    int gain = 0;
    bool agc = false;
    bool bias_tee = false;
    int reset_buffer_calls = 0;
    uint32_t buf_num = 0;
    uint32_t buf_len = 0;
    std::vector<std::string> calls;  // "name argument", in order
};

class Device;

class Backend : public fern::Backend {
public:
    Backend() = default;
    explicit Backend(std::vector<Spec> specs) : devices(std::move(specs)) {}
    ~Backend() override;

    std::vector<Spec> devices;
    int enumeration_error = 0;
    std::string enumeration_diagnostic;

    fern::Enumeration enumerate() override;
    int usb_strings(uint32_t index, fern::UsbStrings& out) override;
    std::string device_name(uint32_t index) override;
    int open(uint32_t index, std::unique_ptr<fern::Device>& out) override;

    // State of the device opened last; null when none was opened.
    std::shared_ptr<State> last();
    int opens() const;

private:
    friend class Device;
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<State>> states_;
    // Devices the module abandoned are deleted when the backend goes, so
    // that leak checks stay meaningful.
    std::set<Device*> live_;
};

}  // namespace fake
