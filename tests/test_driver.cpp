// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// librtlsdr.c itself, run against the libusb of fake_libusb.cpp: the paths
// inside the driver that the module's fake Device cannot reach.
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

#include <libusb.h>
#include <rtl-sdr.h>

#include "fake_libusb.h"
#include "test.h"

namespace {

// librtlsdr reports on stderr; keep that out of the test output unless
// FERN_TEST_LOG asks for it, as the module's own log is.
struct QuietStderr {
    QuietStderr() {
        std::fflush(stderr);
        saved = ::dup(STDERR_FILENO);
        const int null = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
        ::dup2(null, STDERR_FILENO);
        ::close(null);
    }
    ~QuietStderr() {
        std::fflush(stderr);
        ::dup2(saved, STDERR_FILENO);
        ::close(saved);
    }
    int saved;
};

// Leaves zeros where rtlsdr_open() is about to keep its EEPROM buffer, so
// that reading that buffer uninitialized behaves the same on every run.
__attribute__((noinline)) void zero_stack() {
    volatile unsigned char junk[64 * 1024];
    for (auto& b : junk)
        b = 0;
}

rtlsdr_dev_t* open_device() {
    rtlsdr_dev_t* dev = nullptr;
    int r;
    {
        QuietStderr quiet;
        zero_stack();
        r = rtlsdr_open(&dev, 0);
    }
    if (r != 0)
        return nullptr;
    return dev;
}

void close_device(rtlsdr_dev_t* dev) {
    QuietStderr quiet;
    rtlsdr_close(dev);
}

bool bias_tee_on() { return (fake_usb::reg(fake_usb::sys_block, fake_usb::gpo) & 0x01) != 0; }

bool is_eeprom_read(const fake_usb::Control& c) {
    return c.in && c.block == fake_usb::i2c_block && c.address == fake_usb::eeprom_i2c_address;
}

}  // namespace

TEST(driver_forces_the_bias_tee_only_as_a_readable_eeprom_says) {
    // Bit 1 of byte 7 cleared: the RTL-SDR Blog way to keep the bias tee on.
    fake_usb::reset();
    fake_usb::set_eeprom(7, 0xfd);
    rtlsdr_dev_t* dev = open_device();
    REQUIRE(dev);
    CHECK(bias_tee_on());
    CHECK_EQ(rtlsdr_set_bias_tee(dev, 0), 0);
    CHECK(bias_tee_on());
    close_device(dev);

    // An EEPROM that does not answer forces nothing.
    fake_usb::reset();
    fake_usb::control_hook = [](const fake_usb::Control& c) { return is_eeprom_read(c) ? LIBUSB_ERROR_PIPE : 1; };
    dev = open_device();
    REQUIRE(dev);
    CHECK(!bias_tee_on());
    CHECK_EQ(rtlsdr_set_bias_tee(dev, 0), 0);
    CHECK(!bias_tee_on());
    close_device(dev);

    // Nor does one whose transfers come back empty.
    fake_usb::reset();
    fake_usb::control_hook = [](const fake_usb::Control& c) { return is_eeprom_read(c) ? 0 : 1; };
    dev = open_device();
    REQUIRE(dev);
    CHECK(!bias_tee_on());
    uint8_t eeprom[8] = {};
    CHECK(rtlsdr_read_eeprom(dev, eeprom, 0, sizeof eeprom) < 0);
    close_device(dev);
}
