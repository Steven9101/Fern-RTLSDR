// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <string>
#include <thread>

#include "fake_backend.h"
#include "json.h"
#include "listing.h"
#include "test.h"

using fern::json::Value;

TEST(listing_without_devices) {
    fake::Backend backend;
    const fern::Listing l = fern::list_devices(backend, std::chrono::milliseconds(2000));
    CHECK(l.complete);
    CHECK_EQ(fern::report_text(l.report), std::string("{\"devices\":[]}\n"));
}

TEST(listing_describes_each_device) {
    fake::Spec v4;
    v4.manufacturer = "RTLSDRBlog";
    v4.product = "Blog V4";
    v4.serial = "00000001";
    v4.tuner = fern::Tuner::r828d;
    fake::Spec busy;
    busy.serial = "00000002";
    busy.open_error = fern::usb_error::busy;
    fake::Spec forbidden;
    forbidden.serial = "00000003";
    forbidden.strings_error = fern::usb_error::access;
    forbidden.open_error = fern::usb_error::access;
    fake::Spec fc2580;
    fc2580.serial = "00000004";
    fc2580.tuner = fern::Tuner::fc2580;
    fake::Backend backend({v4, busy, forbidden, fc2580});

    const fern::Listing l = fern::list_devices(backend, std::chrono::milliseconds(2000));
    CHECK(l.complete);
    const std::string text = fern::report_text(l.report);
    Value parsed;
    std::string error;
    REQUIRE(fern::json::parse(text, parsed, error));
    const Value* devices = parsed.find("devices");
    REQUIRE(devices && devices->items().size() == 4);
    CHECK_EQ(fern::json::serialize(devices->items()[0]),
             std::string("{\"index\":0,\"name\":\"RTL-SDR Blog V4\",\"serial\":\"00000001\",\"tuner\":\"R828D\","
                         "\"gains\":[0,0.9,1.4,2.7,3.7,7.7,8.7,12.5,14.4,15.7,16.6,19.7,20.7,22.9,25.4,28,29.7,"
                         "32.8,33.8,36.4,37.2,38.6,40.2,42.1,43.4,43.9,44.5,48,49.6],\"usable\":true}"));
    CHECK_EQ(fern::json::serialize(devices->items()[1]),
             std::string("{\"index\":1,\"name\":\"Realtek RTL2838UHIDIR\",\"serial\":\"00000002\",\"usable\":false,"
                         "\"error\":\"in use by another program\"}"));
    const Value& f = devices->items()[2];
    CHECK_EQ(f.find("serial")->as_string(), std::string(""));
    CHECK(!f.find("usable")->as_bool());
    CHECK_HAS(f.find("error")->as_string(), "udev rule");
    CHECK_EQ(fern::json::serialize(*devices->items()[3].find("gains")), std::string("[]"));
    // Every opened device was closed again.
    CHECK(backend.last()->closed);
}

TEST(listing_reports_usb_trouble) {
    fake::Backend backend;
    backend.enumeration_error = -99;
    backend.enumeration_diagnostic = "/dev/bus/usb does not exist";
    const fern::Listing l = fern::list_devices(backend, std::chrono::milliseconds(2000));
    CHECK_EQ(fern::report_text(l.report),
             std::string("{\"devices\":[],\"error\":\"/dev/bus/usb does not exist\"}\n"));
}

TEST(listing_keeps_its_time_limit) {
    fake::Spec slow;
    slow.open_delay_ms = 600;
    fake::Spec quick;
    quick.serial = "2";
    fake::Backend backend({quick, slow});
    const auto start = std::chrono::steady_clock::now();
    const fern::Listing l = fern::list_devices(backend, std::chrono::milliseconds(200));
    const auto took = std::chrono::steady_clock::now() - start;
    CHECK(took < std::chrono::milliseconds(500));
    CHECK(!l.complete);
    const std::string text = fern::report_text(l.report);
    CHECK_HAS(text, "\"serial\":\"2\"");
    CHECK_HAS(text, "{\"index\":1,\"usable\":false,\"error\":\"did not answer within the time limit\"}");
    // The worker still uses the backend; let it finish before the backend goes.
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
}

TEST(listing_fits_into_64_kib) {
    // About 300 bytes per device with gains and 100 without: first the gains
    // go, then the devices that still do not fit.
    std::vector<fake::Spec> many(1000);
    for (size_t i = 0; i < many.size(); ++i)
        many[i].serial = "serial-number-" + std::to_string(i);
    fake::Backend backend(many);
    const fern::Listing l = fern::list_devices(backend, std::chrono::milliseconds(5000));
    CHECK(l.complete);
    const std::string text = fern::report_text(l.report);
    CHECK(text.size() <= fern::max_report_bytes);
    Value parsed;
    std::string error;
    REQUIRE(fern::json::parse(text, parsed, error));
    const size_t kept = parsed.find("devices")->items().size();
    CHECK(kept > 500);
    CHECK(kept < 1000);
    CHECK(parsed.find("devices")->items()[0].find("gains") == nullptr);
    CHECK_EQ(parsed.find("devices")->items()[kept - 1].find("index")->as_number(), double(kept - 1));
}
