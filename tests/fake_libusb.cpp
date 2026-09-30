// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "fake_libusb.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

#include <libusb.h>

struct libusb_context {
    int unused;
};
struct libusb_device {
    int unused;
};
struct libusb_device_handle {
    int unused;
};

namespace fake_usb {

std::function<int(const Control&)> control_hook;
int fail_alloc_transfer_from = -1;
std::atomic<uint64_t> transfers_completed{0};

namespace {

libusb_context the_context;
libusb_device the_device;
libusb_device_handle the_handle;

std::mutex lock;
std::condition_variable wake;
bool interrupted = false;
std::deque<libusb_transfer*> submitted;
std::map<libusb_transfer*, bool> cancelled;
int transfers_allocated = 0;

std::map<uint32_t, uint8_t> registers;
uint8_t eeprom_bytes[256];
uint8_t eeprom_pointer = 0;

uint32_t key(uint16_t block, uint16_t address) { return static_cast<uint32_t>(block) << 16 | address; }

}  // namespace

void reset() {
    std::lock_guard<std::mutex> hold(lock);
    control_hook = nullptr;
    fail_alloc_transfer_from = -1;
    transfers_allocated = 0;
    interrupted = false;
    submitted.clear();
    cancelled.clear();
    registers.clear();
    std::memset(eeprom_bytes, 0xff, sizeof eeprom_bytes);
    eeprom_pointer = 0;
    transfers_completed = 0;
}

uint8_t eeprom(int index) { return eeprom_bytes[index]; }
void set_eeprom(int index, uint8_t value) { eeprom_bytes[index] = value; }

uint8_t reg(uint16_t block, uint16_t address) {
    std::lock_guard<std::mutex> hold(lock);
    const auto it = registers.find(key(block, address));
    return it == registers.end() ? 0 : it->second;
}

}  // namespace fake_usb

using namespace fake_usb;

extern "C" {

int LIBUSB_CALL libusb_init(libusb_context** ctx) {
    *ctx = &the_context;
    return 0;
}
void LIBUSB_CALL libusb_exit(libusb_context*) {}

ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context*, libusb_device*** list) {
    *list = static_cast<libusb_device**>(std::calloc(2, sizeof(libusb_device*)));
    (*list)[0] = &the_device;
    return 1;
}
void LIBUSB_CALL libusb_free_device_list(libusb_device** list, int) { std::free(list); }

int LIBUSB_CALL libusb_get_device_descriptor(libusb_device*, struct libusb_device_descriptor* desc) {
    std::memset(desc, 0, sizeof *desc);
    desc->idVendor = 0x0bda;
    desc->idProduct = 0x2838;
    desc->iManufacturer = 1;
    desc->iProduct = 2;
    desc->iSerialNumber = 3;
    return 0;
}

int LIBUSB_CALL libusb_open(libusb_device*, libusb_device_handle** handle) {
    *handle = &the_handle;
    return 0;
}
void LIBUSB_CALL libusb_close(libusb_device_handle*) {}
libusb_device* LIBUSB_CALL libusb_get_device(libusb_device_handle*) { return &the_device; }
int LIBUSB_CALL libusb_kernel_driver_active(libusb_device_handle*, int) { return 0; }
int LIBUSB_CALL libusb_detach_kernel_driver(libusb_device_handle*, int) { return 0; }
int LIBUSB_CALL libusb_attach_kernel_driver(libusb_device_handle*, int) { return 0; }
int LIBUSB_CALL libusb_claim_interface(libusb_device_handle*, int) { return 0; }
int LIBUSB_CALL libusb_release_interface(libusb_device_handle*, int) { return 0; }
int LIBUSB_CALL libusb_reset_device(libusb_device_handle*) { return 0; }

int LIBUSB_CALL libusb_get_string_descriptor_ascii(libusb_device_handle*, uint8_t index, unsigned char* data,
                                                   int length) {
    const char* text = index == 1 ? "Generic" : index == 2 ? "RTL2832U" : "00000001";
    const int n = std::min(static_cast<int>(std::strlen(text)), length - 1);
    std::memcpy(data, text, static_cast<size_t>(n));
    data[n] = 0;
    return n;
}

// librtlsdr addresses registers and I2C devices with wValue and a block in
// the high byte of wIndex, and sets bit 4 of wIndex to write.
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle*, uint8_t request_type, uint8_t, uint16_t value,
                                        uint16_t index, unsigned char* data, uint16_t length, unsigned int) {
    Control c;
    c.in = (request_type & LIBUSB_ENDPOINT_IN) != 0;
    c.block = static_cast<uint16_t>(index >> 8);
    c.address = value;
    c.length = length;
    if (control_hook) {
        const int r = control_hook(c);
        if (r != 1)
            return r;
    }
    std::lock_guard<std::mutex> hold(lock);
    const bool eeprom_access = c.block == i2c_block && value == eeprom_i2c_address;
    if (c.in) {
        for (uint16_t i = 0; i < length; ++i) {
            if (eeprom_access) {
                data[i] = eeprom_bytes[eeprom_pointer++];
            } else {
                const auto it = registers.find(key(c.block, static_cast<uint16_t>(value + i)));
                data[i] = it == registers.end() ? 0 : it->second;
            }
        }
    } else if (eeprom_access) {
        if (length >= 1)
            eeprom_pointer = data[0];
    } else {
        for (uint16_t i = 0; i < length; ++i)
            registers[key(c.block, static_cast<uint16_t>(value + i))] = data[i];
    }
    return length;
}

int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle*, unsigned char, unsigned char*, int, int*, unsigned int) {
    return LIBUSB_ERROR_IO;
}

struct libusb_transfer* LIBUSB_CALL libusb_alloc_transfer(int) {
    std::lock_guard<std::mutex> hold(lock);
    if (fail_alloc_transfer_from >= 0 && transfers_allocated++ >= fail_alloc_transfer_from)
        return nullptr;
    return static_cast<libusb_transfer*>(std::calloc(1, sizeof(libusb_transfer)));
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer* transfer) {
    {
        std::lock_guard<std::mutex> hold(lock);
        submitted.erase(std::remove(submitted.begin(), submitted.end(), transfer), submitted.end());
        cancelled.erase(transfer);
    }
    std::free(transfer);
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer* transfer) {
    std::lock_guard<std::mutex> hold(lock);
    submitted.push_back(transfer);
    cancelled[transfer] = false;
    return 0;
}

int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer* transfer) {
    std::lock_guard<std::mutex> hold(lock);
    if (std::find(submitted.begin(), submitted.end(), transfer) == submitted.end())
        return LIBUSB_ERROR_NOT_FOUND;
    cancelled[transfer] = true;
    return 0;
}

void LIBUSB_CALL libusb_interrupt_event_handler(libusb_context*) {
    std::lock_guard<std::mutex> hold(lock);
    interrupted = true;
    wake.notify_all();
}

// Completes every submitted transfer after a millisecond, the way a
// streaming RTL2832U keeps the event loop busy. Like libusb, it reads the
// completion flag without any lock of the caller's.
int LIBUSB_CALL libusb_handle_events_timeout_completed(libusb_context*, struct timeval* tv, int* completed) {
    std::deque<libusb_transfer*> ready;
    std::map<libusb_transfer*, bool> was_cancelled;
    {
        std::unique_lock<std::mutex> hold(lock);
        if (completed && *completed)
            return 0;
        const bool wait = tv && (tv->tv_sec > 0 || tv->tv_usec > 0);
        if (wait && !interrupted)
            wake.wait_for(hold, std::chrono::milliseconds(1), [] { return interrupted; });
        interrupted = false;
        if (completed && *completed)
            return 0;
        ready.swap(submitted);
        for (libusb_transfer* t : ready)
            was_cancelled[t] = cancelled[t];
    }
    for (libusb_transfer* t : ready) {
        t->status = was_cancelled[t] ? LIBUSB_TRANSFER_CANCELLED : LIBUSB_TRANSFER_COMPLETED;
        t->actual_length = was_cancelled[t] ? 0 : t->length;
        if (!was_cancelled[t])
            ++transfers_completed;
        t->callback(t);
    }
    return 0;
}

unsigned char* LIBUSB_CALL libusb_dev_mem_alloc(libusb_device_handle*, size_t) { return nullptr; }
int LIBUSB_CALL libusb_dev_mem_free(libusb_device_handle*, unsigned char*, size_t) { return 0; }

}  // extern "C"
