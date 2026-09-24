// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

#include "device.h"

namespace fern {

// The Backend on top of librtlsdr and libusb.
class RtlsdrBackend : public Backend {
public:
    Enumeration enumerate() override;
    int usb_strings(uint32_t index, UsbStrings& out) override;
    std::string device_name(uint32_t index) override;
    int open(uint32_t index, std::unique_ptr<Device>& out) override;
};

// e.g. "libusb 1.0.27"
std::string libusb_version_text();

}  // namespace fern
