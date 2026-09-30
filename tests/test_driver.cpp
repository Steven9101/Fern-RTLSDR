// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// librtlsdr.c itself, run against the libusb of fake_libusb.cpp: the paths
// inside the driver that the module's fake Device cannot reach.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <thread>
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

int switch_quietly(rtlsdr_dev_t* dev, int on) {
    QuietStderr quiet;
    return rtlsdr_set_bias_tee(dev, on);
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

TEST(driver_reports_a_bias_tee_switch_that_failed) {
    fake_usb::reset();
    rtlsdr_dev_t* dev = open_device();
    REQUIRE(dev);
    REQUIRE(rtlsdr_set_bias_tee(dev, 1) == 0);
    REQUIRE(bias_tee_on());
    // The output register stops answering: switching off must not report
    // success while the antenna stays powered.
    fake_usb::control_hook = [](const fake_usb::Control& c) {
        return c.block == fake_usb::sys_block && c.address == fake_usb::gpo ? LIBUSB_ERROR_TIMEOUT : 1;
    };
    CHECK(switch_quietly(dev, 0) != 0);
    CHECK(bias_tee_on());
    // So must a failed write alone, after a read that worked.
    fake_usb::control_hook = [](const fake_usb::Control& c) {
        return !c.in && c.block == fake_usb::sys_block && c.address == fake_usb::gpo ? LIBUSB_ERROR_IO : 1;
    };
    CHECK(switch_quietly(dev, 0) != 0);
    CHECK(bias_tee_on());
    fake_usb::control_hook = nullptr;
    CHECK_EQ(rtlsdr_set_bias_tee(dev, 0), 0);
    CHECK(!bias_tee_on());
    close_device(dev);
}

namespace {

struct Reading {
    rtlsdr_dev_t* dev = nullptr;
    int callbacks = 0;
    int stop_after = 0;
};

void count_and_stop(unsigned char*, uint32_t, void* ctx) {
    Reading* reading = static_cast<Reading*>(ctx);
    if (++reading->callbacks == reading->stop_after)
        rtlsdr_cancel_async(reading->dev);
}

}  // namespace

TEST(driver_reports_transfers_it_could_not_allocate) {
    fake_usb::reset();
    rtlsdr_dev_t* dev = open_device();
    REQUIRE(dev);
    Reading reading;
    reading.dev = dev;
    reading.stop_after = 10;
    // The third of four transfers cannot be allocated: read_async() must
    // fail without submitting anything, rather than fill a NULL transfer.
    fake_usb::fail_alloc_transfer_from = 2;
    int r;
    {
        QuietStderr quiet;
        r = rtlsdr_read_async(dev, count_and_stop, &reading, 4, 16384);
    }
    CHECK(r < 0);
    CHECK_EQ(reading.callbacks, 0);
    // And leave the device able to stream once memory is there again.
    fake_usb::fail_alloc_transfer_from = -1;
    {
        QuietStderr quiet;
        r = rtlsdr_read_async(dev, count_and_stop, &reading, 4, 16384);
    }
    CHECK_EQ(r, 0);
    CHECK(reading.callbacks >= 10);
    close_device(dev);
}

namespace {

void count(unsigned char*, uint32_t, void* ctx) { static_cast<std::atomic<int>*>(ctx)->fetch_add(1); }

}  // namespace

// The module cancels from its session thread while the reader thread runs
// the event loop, as Stream::request_stop() does. make test-tsan runs this
// under ThreadSanitizer, which sees any unsynchronised access to the state
// the two threads share.
TEST(driver_cancels_streaming_from_another_thread) {
    fake_usb::reset();
    rtlsdr_dev_t* dev = open_device();
    REQUIRE(dev);
    for (int round = 0; round < 20; ++round) {
        std::atomic<int> callbacks{0};
        std::atomic<bool> ended{false};
        int r = -100;
        std::thread reader([&] {
            r = rtlsdr_read_async(dev, count, &callbacks, 4, 16384);
            ended = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (callbacks.load() < 8 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        CHECK(callbacks.load() >= 8);
        // Repeated, as Stream::wait() repeats it, until the reader has ended.
        while (!ended.load() && std::chrono::steady_clock::now() < deadline) {
            rtlsdr_cancel_async(dev);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        REQUIRE(ended.load());
        reader.join();
        CHECK_EQ(r, 0);
    }
    close_device(dev);
}
