// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "rtlsdr_backend.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libusb.h>
#include <rtl-sdr.h>

#include "log.h"

extern "C" {
#include <tuner_r82xx.h>

// Exported by librtlsdr.c but not declared in rtl-sdr.h; the tuner drivers
// talk to the tuner through them.
void rtlsdr_set_i2c_repeater(rtlsdr_dev_t* dev, int on);
int rtlsdr_i2c_write_fn(void* dev, uint8_t addr, uint8_t* buf, int len);
int rtlsdr_i2c_read_fn(void* dev, uint8_t addr, uint8_t* buf, int len);
}

namespace fern {

namespace {

Tuner tuner_of(enum rtlsdr_tuner t) {
    switch (t) {
    case RTLSDR_TUNER_E4000: return Tuner::e4000;
    case RTLSDR_TUNER_FC0012: return Tuner::fc0012;
    case RTLSDR_TUNER_FC0013: return Tuner::fc0013;
    case RTLSDR_TUNER_FC2580: return Tuner::fc2580;
    case RTLSDR_TUNER_R820T: return Tuner::r820t;
    case RTLSDR_TUNER_R828D: return Tuner::r828d;
    case RTLSDR_TUNER_UNKNOWN: break;
    }
    return Tuner::unknown;
}

class RtlsdrDevice : public Device {
public:
    explicit RtlsdrDevice(rtlsdr_dev_t* dev) : dev_(dev) {}
    ~RtlsdrDevice() override {
        if (rtlsdr_close(dev_) != 0)
            log_line("rtlsdr_close failed");
    }
    RtlsdrDevice(const RtlsdrDevice&) = delete;
    RtlsdrDevice& operator=(const RtlsdrDevice&) = delete;

    Tuner tuner() override { return tuner_of(rtlsdr_get_tuner_type(dev_)); }

    std::vector<int> tuner_gains() override {
        const int n = rtlsdr_get_tuner_gains(dev_, nullptr);
        if (n <= 0)
            return {};
        std::vector<int> gains(static_cast<size_t>(n));
        if (rtlsdr_get_tuner_gains(dev_, gains.data()) != n)
            return {};
        return gains;
    }

    int usb_strings(UsbStrings& out) override {
        char manufacturer[256] = {};
        char product[256] = {};
        char serial[256] = {};
        const int r = rtlsdr_get_usb_strings(dev_, manufacturer, product, serial);
        if (r != 0)
            return r;
        out = UsbStrings{manufacturer, product, serial};
        return 0;
    }

    int read_eeprom(uint8_t* data, uint8_t offset, uint16_t len) override {
        return rtlsdr_read_eeprom(dev_, data, offset, len);
    }
    int xtal_freq(uint32_t& rtl_hz) override { return rtlsdr_get_xtal_freq(dev_, &rtl_hz, nullptr); }
    int set_freq_correction(int ppm) override { return rtlsdr_set_freq_correction(dev_, ppm); }
    int set_direct_sampling(int mode) override { return rtlsdr_set_direct_sampling(dev_, mode); }
    int direct_sampling() override { return rtlsdr_get_direct_sampling(dev_); }
    int set_offset_tuning(bool on) override { return rtlsdr_set_offset_tuning(dev_, on ? 1 : 0); }
    int offset_tuning() override { return rtlsdr_get_offset_tuning(dev_); }
    int set_center_freq(uint32_t hz) override { return rtlsdr_set_center_freq(dev_, hz); }
    uint32_t center_freq() override { return rtlsdr_get_center_freq(dev_); }
    int set_sample_rate(uint32_t hz) override { return rtlsdr_set_sample_rate(dev_, hz); }
    uint32_t sample_rate() override { return rtlsdr_get_sample_rate(dev_); }
    int set_tuner_bandwidth(uint32_t hz) override { return rtlsdr_set_tuner_bandwidth(dev_, hz); }
    int set_tuner_gain_mode(bool manual) override { return rtlsdr_set_tuner_gain_mode(dev_, manual ? 1 : 0); }
    int set_tuner_gain(int tenth_db) override { return rtlsdr_set_tuner_gain(dev_, tenth_db); }
    int set_agc_mode(bool on) override { return rtlsdr_set_agc_mode(dev_, on ? 1 : 0); }
    int set_bias_tee(bool on) override { return rtlsdr_set_bias_tee(dev_, on ? 1 : 0); }
    int reset_buffer() override { return rtlsdr_reset_buffer(dev_); }

    int read_async(SampleCallback cb, void* ctx, uint32_t buf_num, uint32_t buf_len) override {
        return rtlsdr_read_async(dev_, cb, ctx, buf_num, buf_len);
    }
    int cancel_async() override { return rtlsdr_cancel_async(dev_); }

    // The R82xx driver in librtlsdr logs "PLL not locked" but still reports
    // success, so the lock flag is read back here. The chip always answers
    // from register 0 and sends every byte bit-reversed; the lock flag, bit 6
    // of register 2, therefore arrives as bit 1 of the third byte (see
    // r82xx_set_pll() and r82xx_read()).
    int pll_locked() override {
        const enum rtlsdr_tuner t = rtlsdr_get_tuner_type(dev_);
        if ((t != RTLSDR_TUNER_R820T && t != RTLSDR_TUNER_R828D) || rtlsdr_get_direct_sampling(dev_) != 0)
            return -1;
        const uint8_t addr = t == RTLSDR_TUNER_R828D ? R828D_I2C_ADDR : R820T_I2C_ADDR;
        uint8_t reg = 0;
        uint8_t data[3] = {};
        rtlsdr_set_i2c_repeater(dev_, 1);
        int r = rtlsdr_i2c_write_fn(dev_, addr, &reg, 1);
        if (r == 1)
            r = rtlsdr_i2c_read_fn(dev_, addr, data, sizeof data);
        rtlsdr_set_i2c_repeater(dev_, 0);
        if (r != static_cast<int>(sizeof data))
            return -1;
        return (data[2] & 0x02) ? 1 : 0;
    }

private:
    rtlsdr_dev_t* dev_;
};

// libusb fails to start in a sandbox that hides /dev/bus/usb or forbids the
// netlink socket it watches for hotplug events on. Say which, since both
// look like "no RTL-SDR plugged in" otherwise.
std::string usb_diagnostic(int code) {
    if (::access("/dev/bus/usb", F_OK) != 0)
        return "USB devices are not visible to this process: /dev/bus/usb does not exist. If FernSDR runs "
               "under systemd, its unit must not set PrivateDevices=yes";
    const int s = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (s < 0)
        return std::string("libusb needs a netlink socket to watch for USB devices, and this process may not "
                           "open one (") +
               std::strerror(errno) +
               "). If FernSDR runs under systemd, add AF_NETLINK to RestrictAddressFamilies in its unit";
    ::close(s);
    return std::string("libusb could not start (") + libusb_error_name(code) + ")";
}

}  // namespace

Enumeration RtlsdrBackend::enumerate() {
    Enumeration e;
    // rtlsdr_get_device_count() returns 0 when libusb cannot start, which
    // would hide the reason; try it here first.
    libusb_context* ctx = nullptr;
    const int r = libusb_init(&ctx);
    if (r < 0) {
        e.error = r;
        e.diagnostic = usb_diagnostic(r);
        return e;
    }
    libusb_exit(ctx);
    e.count = rtlsdr_get_device_count();
    return e;
}

int RtlsdrBackend::usb_strings(uint32_t index, UsbStrings& out) {
    char manufacturer[256] = {};
    char product[256] = {};
    char serial[256] = {};
    const int r = rtlsdr_get_device_usb_strings(index, manufacturer, product, serial);
    if (r != 0)
        return r;
    out = UsbStrings{manufacturer, product, serial};
    return 0;
}

std::string RtlsdrBackend::device_name(uint32_t index) {
    const char* name = rtlsdr_get_device_name(index);
    return name ? name : "";
}

int RtlsdrBackend::open(uint32_t index, std::unique_ptr<Device>& out) {
    rtlsdr_dev_t* dev = nullptr;
    const int r = rtlsdr_open(&dev, index);
    if (r != 0)
        return r;
    // With DETACH_KERNEL_DRIVER, a failed detach makes rtlsdr_open() free the
    // device and still return 0.
    if (!dev)
        return usb_error::kernel_driver;
    out = std::make_unique<RtlsdrDevice>(dev);
    return 0;
}

std::string libusb_version_text() {
    const struct libusb_version* v = libusb_get_version();
    return "libusb " + std::to_string(v->major) + "." + std::to_string(v->minor) + "." + std::to_string(v->micro);
}

}  // namespace fern
