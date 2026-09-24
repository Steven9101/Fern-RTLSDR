// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The narrow interface between the module and librtlsdr. Methods mirror the
// librtlsdr calls one to one, with the same return conventions, so that the
// real implementation stays a thin wrapper and all decisions live in code
// that the tests can drive with a fake device.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fern {

enum class Tuner { unknown, e4000, fc0012, fc0013, fc2580, r820t, r828d };

inline const char* tuner_name(Tuner t) {
    switch (t) {
    case Tuner::e4000: return "E4000";
    case Tuner::fc0012: return "FC0012";
    case Tuner::fc0013: return "FC0013";
    case Tuner::fc2580: return "FC2580";
    case Tuner::r820t: return "R820T";
    case Tuner::r828d: return "R828D";
    case Tuner::unknown: break;
    }
    return "unknown";
}

// libusb error codes, which librtlsdr passes through from rtlsdr_open() and
// rtlsdr_get_device_usb_strings(). Repeated here so that the module logic
// does not depend on libusb headers.
namespace usb_error {
constexpr int io = -1;
constexpr int access = -3;
constexpr int no_device = -4;
constexpr int not_found = -5;
constexpr int busy = -6;
constexpr int timeout = -7;
// Not a libusb code: rtlsdr_open() found a kernel driver on the device and
// could not detach it.
constexpr int kernel_driver = -1000;
}  // namespace usb_error

const char* usb_error_text(int code);

struct UsbStrings {
    std::string manufacturer;
    std::string product;
    std::string serial;
};

using SampleCallback = void (*)(unsigned char* buf, uint32_t len, void* ctx);

// An opened RTL2832U. Destroying it closes the device; that must not happen
// while read_async() is running in another thread.
class Device {
public:
    virtual ~Device() = default;

    virtual Tuner tuner() = 0;
    virtual std::vector<int> tuner_gains() = 0;  // tenths of a dB
    virtual int usb_strings(UsbStrings& out) = 0;
    virtual int read_eeprom(uint8_t* data, uint8_t offset, uint16_t len) = 0;  // < 0 on failure
    virtual int xtal_freq(uint32_t& rtl_hz) = 0;
    virtual int set_freq_correction(int ppm) = 0;  // -2 when unchanged
    virtual int set_direct_sampling(int mode) = 0;  // 0 off, 1 I branch, 2 Q branch
    virtual int direct_sampling() = 0;
    virtual int set_offset_tuning(bool on) = 0;
    virtual int offset_tuning() = 0;
    virtual int set_center_freq(uint32_t hz) = 0;
    virtual uint32_t center_freq() = 0;
    virtual int set_sample_rate(uint32_t hz) = 0;
    virtual uint32_t sample_rate() = 0;
    virtual int set_tuner_bandwidth(uint32_t hz) = 0;
    virtual int set_tuner_gain_mode(bool manual) = 0;
    virtual int set_tuner_gain(int tenth_db) = 0;
    virtual int set_agc_mode(bool on) = 0;
    virtual int set_bias_tee(bool on) = 0;
    virtual int reset_buffer() = 0;
    // Blocks, calling cb from the calling thread, until cancel_async() or
    // until the device fails.
    virtual int read_async(SampleCallback cb, void* ctx, uint32_t buf_num, uint32_t buf_len) = 0;
    virtual int cancel_async() = 0;
    // 1 when the tuner reports its PLL locked, 0 when it reports it unlocked,
    // -1 when this tuner or mode offers no way to tell.
    virtual int pll_locked() = 0;
};

struct Enumeration {
    int error = 0;           // 0, or a libusb code when USB is unusable here
    std::string diagnostic;  // what to do about error, for the operator
    uint32_t count = 0;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual Enumeration enumerate() = 0;
    virtual int usb_strings(uint32_t index, UsbStrings& out) = 0;
    virtual std::string device_name(uint32_t index) = 0;
    // 0 with a device, or a negative librtlsdr/libusb error code.
    virtual int open(uint32_t index, std::unique_ptr<Device>& out) = 0;
};

}  // namespace fern
