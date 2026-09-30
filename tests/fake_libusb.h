// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// A libusb for librtlsdr.c itself to run against: one RTL2832U whose
// registers remember what was written, an EEPROM on its I2C bus, and bulk
// transfers that complete at once. The driver tests use it to reach the
// failure paths no fake Device can, since those lie inside librtlsdr.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

namespace fake_usb {

// Block numbers in the RTL2832U's wIndex, as librtlsdr.c numbers them.
constexpr uint16_t usb_block = 1;
constexpr uint16_t sys_block = 2;
constexpr uint16_t i2c_block = 6;
constexpr uint16_t gpo = 0x3001;
constexpr uint16_t eeprom_i2c_address = 0xa0;

struct Control {
    bool in = false;
    uint16_t block = 0;  // for register and I2C access; the demodulator's page otherwise
    uint16_t address = 0;
    uint16_t length = 0;
};

// Resets every register, the EEPROM (all 0xff, as shipped) and the hooks.
void reset();

// Returns a libusb result to answer a control transfer with instead of
// running it, or 1 to run it.
extern std::function<int(const Control&)> control_hook;
// libusb_alloc_transfer() returns NULL from this call on, counting from 0;
// -1 never.
extern int fail_alloc_transfer_from;

uint8_t eeprom(int index);
void set_eeprom(int index, uint8_t value);
// The last value written to a one-byte register of a block.
uint8_t reg(uint16_t block, uint16_t address);

extern std::atomic<uint64_t> transfers_completed;

}  // namespace fake_usb
