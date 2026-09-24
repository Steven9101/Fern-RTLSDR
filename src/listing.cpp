// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "listing.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "log.h"
#include "receiver.h"

namespace fern {

namespace {

struct Shared {
    std::mutex mutex;
    std::condition_variable changed;
    bool enumerated = false;
    Enumeration enumeration;
    std::vector<json::Value> devices;
    bool finished = false;
};

std::string open_error(int r) {
    switch (r) {
    case 0:
        return "could not be opened";
    case usb_error::busy:
        return "in use by another program";
    case usb_error::kernel_driver:
        return "held by the dvb_usb_rtl28xxu kernel driver, which could not be detached; see the README";
    case usb_error::access:
        return "no permission to open it; see the udev rule in the README";
    case usb_error::no_device:
        return "it disappeared while it was being opened";
    default:
        return std::string("could not be opened (") + usb_error_text(r) + ")";
    }
}

json::Value examine(Backend& backend, uint32_t index) {
    json::Value d = json::Value::object();
    d.set("index", index);
    UsbStrings strings;
    if (backend.usb_strings(index, strings) != 0)
        strings = UsbStrings{};
    std::unique_ptr<Device> device;
    const int r = backend.open(index, device);
    if (r == 0 && device) {
        UsbStrings opened;
        if (device->usb_strings(opened) == 0)
            strings = opened;
    }
    std::string name = display_name(strings);
    if (name.empty())
        name = backend.device_name(index);
    if (name.empty())
        name = "RTL2832U";
    d.set("name", name);
    d.set("serial", strings.serial);
    if (r == 0 && device) {
        const Tuner tuner = device->tuner();
        d.set("tuner", tuner_name(tuner));
        json::Value gains = json::Value::array();
        if (tuner != Tuner::fc2580 && tuner != Tuner::unknown)
            for (int g : device->tuner_gains())
                gains.push(g / 10.0);
        d.set("gains", std::move(gains));
        d.set("usable", true);
        device.reset();
    } else {
        d.set("usable", false);
        d.set("error", open_error(r));
    }
    return d;
}

void list_into(Backend& backend, Shared& shared) {
    const Enumeration e = backend.enumerate();
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.enumeration = e;
        shared.enumerated = true;
    }
    shared.changed.notify_all();
    if (e.error == 0) {
        for (uint32_t i = 0; i < e.count; ++i) {
            json::Value d = examine(backend, i);
            {
                std::lock_guard<std::mutex> lock(shared.mutex);
                shared.devices.push_back(std::move(d));
            }
            shared.changed.notify_all();
        }
    }
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.finished = true;
    }
    shared.changed.notify_all();
}

}  // namespace

Listing list_devices(Backend& backend, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    auto shared = std::make_shared<Shared>();
    std::thread worker;
    try {
        worker = std::thread([shared, &backend] { list_into(backend, *shared); });
    } catch (const std::system_error&) {
        list_into(backend, *shared);
    }

    std::unique_lock<std::mutex> lock(shared->mutex);
    const bool finished = shared->changed.wait_until(lock, deadline, [&] { return shared->finished; });
    json::Value devices = json::Value::array();
    for (const json::Value& d : shared->devices)
        devices.push(d);
    json::Value report = json::Value::object();
    if (!shared->enumerated) {
        report.set("devices", std::move(devices));
        report.set("error", "the list of USB devices could not be read within the time limit");
    } else {
        for (size_t i = shared->devices.size(); i < shared->enumeration.count; ++i) {
            json::Value d = json::Value::object();
            d.set("index", i);
            d.set("usable", false);
            d.set("error", "did not answer within the time limit");
            devices.push(std::move(d));
        }
        report.set("devices", std::move(devices));
        if (shared->enumeration.error != 0)
            report.set("error", shared->enumeration.diagnostic);
    }
    lock.unlock();

    if (worker.joinable()) {
        if (finished)
            worker.join();
        else
            worker.detach();
    }
    return Listing{std::move(report), finished};
}

std::string report_text(const json::Value& report) {
    std::string text = json::serialize(report) + "\n";
    if (text.size() <= max_report_bytes)
        return text;

    std::vector<json::Value> kept;
    if (const json::Value* list = report.find("devices")) {
        for (const json::Value& d : list->items()) {
            json::Value slim = json::Value::object();
            for (const json::Member& m : d.members())
                if (m.key != "gains")
                    slim.set(m.key, m.value);
            kept.push_back(std::move(slim));
        }
    }
    for (;;) {
        json::Value out = json::Value::object();
        json::Value list = json::Value::array();
        for (const json::Value& d : kept)
            list.push(d);
        out.set("devices", std::move(list));
        for (const json::Member& m : report.members())
            if (m.key != "devices")
                out.set(m.key, m.value);
        text = json::serialize(out) + "\n";
        if (text.size() <= max_report_bytes || kept.empty())
            break;
        kept.pop_back();
    }
    log_line("the device list was shortened to fit into %zu bytes", max_report_bytes);
    return text;
}

}  // namespace fern
