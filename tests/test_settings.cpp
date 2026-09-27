// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <set>
#include <string>

#include "json.h"
#include "listing.h"
#include "receiver.h"
#include "settings.h"
#include "test.h"

using fern::json::Value;

namespace {

Value json(const std::string& text) {
    Value v;
    std::string error;
    if (!fern::json::parse(text, v, error))
        test::report(__FILE__, __LINE__, "bad test JSON " + text + ": " + error);
    return v;
}

// An open message with the given settings object text.
std::string open_with(const std::string& settings, const std::string& rate = "2400000",
                      const std::string& center = "14200000", const std::string& signal = "\"iq\"") {
    return "{\"type\":\"open\",\"sample_rate\":" + rate + ",\"center\":" + center + ",\"signal\":" + signal +
           ",\"settings\":" + settings + "}";
}

std::optional<fern::Failure> try_open(const std::string& text, fern::OpenRequest& out) {
    return fern::parse_open(json(text), out);
}

bool open_ok(const std::string& text) {
    fern::OpenRequest r;
    return !try_open(text, r);
}

// The failure message for an open that must be refused as invalid.
std::string refusal(const std::string& text) {
    fern::OpenRequest r;
    const auto f = try_open(text, r);
    if (!f) {
        test::report(__FILE__, __LINE__, "accepted: " + text);
        return "";
    }
    if (f->code != fern::ErrorCode::invalid)
        test::report(__FILE__, __LINE__, "not refused as invalid: " + text);
    return f->message;
}

}  // namespace

TEST(open_defaults) {
    fern::OpenRequest r;
    REQUIRE(!try_open(open_with("{}"), r));
    CHECK_EQ(r.sample_rate, 2400000u);
    CHECK_EQ(r.center, 14200000u);
    const fern::ModuleSettings& s = r.settings;
    CHECK(s.device.kind == fern::DeviceSelector::Kind::only);
    CHECK(s.gain.automatic());
    CHECK_EQ(s.ppm, 0);
    CHECK(!s.rtl_agc);
    CHECK(!s.bias_tee);
    CHECK(s.direct_sampling == fern::DirectSampling::off);
    CHECK(!s.offset_tuning);
    CHECK_EQ(s.bandwidth, 0u);
    CHECK_EQ(s.buffers, 16u);
    // settings may be missing entirely; unknown top-level fields are ignored.
    CHECK(open_ok("{\"type\":\"open\",\"sample_rate\":2048000,\"center\":100000000,\"signal\":\"iq\",\"x\":1}"));
}

TEST(open_reads_every_setting) {
    fern::OpenRequest r;
    REQUIRE(!try_open(open_with("{\"device\":\"serial:ABC 1\",\"gain\":\"38.6\",\"ppm\":-12,\"rtl_agc\":true,"
                                "\"bias_tee\":true,\"direct_sampling\":\"q\",\"offset_tuning\":true,"
                                "\"bandwidth\":1500000,\"buffers\":32}"),
                      r));
    const fern::ModuleSettings& s = r.settings;
    CHECK(s.device.kind == fern::DeviceSelector::Kind::serial);
    CHECK_EQ(s.device.serial, std::string("ABC 1"));
    CHECK(!s.gain.automatic());
    CHECK_EQ(s.gain.db, 38.6);
    CHECK_EQ(s.ppm, -12);
    CHECK(s.rtl_agc);
    CHECK(s.bias_tee);
    CHECK(s.direct_sampling == fern::DirectSampling::q);
    CHECK(s.offset_tuning);
    CHECK_EQ(s.bandwidth, 1500000u);
    CHECK_EQ(s.buffers, 32u);

    REQUIRE(!try_open(open_with("{\"device\":\"index:3\",\"gain\":42,\"direct_sampling\":\"i\"}"), r));
    CHECK(r.settings.device.kind == fern::DeviceSelector::Kind::index);
    CHECK_EQ(r.settings.device.index, 3u);
    CHECK_EQ(r.settings.gain.db, 42.0);
    CHECK(r.settings.direct_sampling == fern::DirectSampling::i);
    REQUIRE(!try_open(open_with("{\"device\":\"\",\"gain\":\"AUTO\",\"direct_sampling\":\"off\"}"), r));
    CHECK(r.settings.device.kind == fern::DeviceSelector::Kind::only);
    CHECK(r.settings.gain.automatic());
    REQUIRE(!try_open(open_with("{\"gain\":\"-1.5\"}"), r));
    CHECK_EQ(r.settings.gain.db, -1.5);
}

TEST(open_checks_sample_rate) {
    CHECK(open_ok(open_with("{}", "225001")));
    CHECK(open_ok(open_with("{}", "300000")));
    CHECK(open_ok(open_with("{}", "900001")));
    CHECK(open_ok(open_with("{}", "3200000")));
    CHECK(open_ok(open_with("{}", "2.048e6")));
    CHECK_HAS(refusal(open_with("{}", "225000")), "225001 to 300000");
    CHECK_HAS(refusal(open_with("{}", "300001")), "not possible");
    CHECK_HAS(refusal(open_with("{}", "900000")), "not possible");
    CHECK_HAS(refusal(open_with("{}", "3200001")), "not possible");
    CHECK_HAS(refusal(open_with("{}", "0")), "not possible");
    CHECK_HAS(refusal(open_with("{}", "2400000.5")), "whole number");
    CHECK_HAS(refusal(open_with("{}", "-2400000")), "whole number");
    CHECK_HAS(refusal(open_with("{}", "\"2400000\"")), "whole number");
    CHECK_HAS(refusal("{\"type\":\"open\",\"center\":1,\"signal\":\"iq\"}"), "no sample_rate");
}

TEST(open_checks_center_and_signal) {
    CHECK(open_ok(open_with("{}", "2400000", "0")));
    CHECK(open_ok(open_with("{}", "2400000", "4294967295")));
    CHECK_HAS(refusal(open_with("{}", "2400000", "4294967296")), "whole number of Hz");
    CHECK_HAS(refusal(open_with("{}", "2400000", "-1")), "whole number of Hz");
    CHECK_HAS(refusal(open_with("{}", "2400000", "14200000.5")), "whole number of Hz");
    CHECK_HAS(refusal(open_with("{}", "2400000", "null")), "whole number of Hz");
    CHECK_HAS(refusal("{\"type\":\"open\",\"sample_rate\":2400000,\"signal\":\"iq\"}"), "no center");
    CHECK_HAS(refusal(open_with("{}", "2400000", "14200000", "\"real\"")), "set signal = iq");
    CHECK_HAS(refusal(open_with("{}", "2400000", "14200000", "\"complex\"")), "iq or real");
    CHECK_HAS(refusal(open_with("{}", "2400000", "14200000", "1")), "iq or real");
    CHECK_HAS(refusal("{\"type\":\"open\",\"sample_rate\":2400000,\"center\":1}"), "no signal");
    CHECK_HAS(refusal("{\"type\":\"open\",\"sample_rate\":2400000,\"center\":1,\"signal\":\"iq\",\"settings\":[]}"),
              "must be an object");
}

TEST(open_refuses_wrong_types_and_values) {
    // Each setting with values of the wrong type or out of range.
    CHECK_HAS(refusal(open_with("{\"device\":0}")), "module.device must be");
    CHECK_HAS(refusal(open_with("{\"device\":\"00000001\"}")), "module.device must be");
    CHECK_HAS(refusal(open_with("{\"device\":\"serial:\"}")), "module.device must be");
    CHECK_HAS(refusal(open_with("{\"device\":\"index:\"}")), "module.device must be");
    CHECK_HAS(refusal(open_with("{\"device\":\"index:-1\"}")), "module.device must be");
    CHECK_HAS(refusal(open_with("{\"device\":\"index:1a\"}")), "module.device must be");
    CHECK_HAS(refusal(open_with("{\"device\":\"index:4294967296\"}")), "module.device must be");
    CHECK_HAS(refusal(open_with("{\"gain\":\"loud\"}")), "module.gain must be auto, tuner or a gain in dB");
    CHECK_HAS(refusal(open_with("{\"gain\":\"\"}")), "module.gain");
    CHECK_HAS(refusal(open_with("{\"gain\":\"38.6 dB\"}")), "module.gain");
    CHECK_HAS(refusal(open_with("{\"gain\":\"nan\"}")), "module.gain");
    CHECK_HAS(refusal(open_with("{\"gain\":true}")), "module.gain");
    CHECK_HAS(refusal(open_with("{\"gain\":101}")), "module.gain");
    CHECK(open_ok(open_with("{\"ppm\":488}")));
    CHECK(open_ok(open_with("{\"ppm\":-488}")));
    CHECK_HAS(refusal(open_with("{\"ppm\":489}")), "module.ppm must be a whole number from -488 to 488, not 489");
    CHECK_HAS(refusal(open_with("{\"ppm\":-489}")), "module.ppm");
    CHECK_HAS(refusal(open_with("{\"ppm\":1.5}")), "module.ppm");
    CHECK_HAS(refusal(open_with("{\"ppm\":\"5\"}")), "not \"5\"");
    for (const char* key : {"rtl_agc", "bias_tee", "offset_tuning"}) {
        const std::string k = key;
        CHECK_HAS(refusal(open_with("{\"" + k + "\":\"yes\"}")), "module." + k + " must be yes or no");
        CHECK_HAS(refusal(open_with("{\"" + k + "\":1}")), "module." + k);
        CHECK_HAS(refusal(open_with("{\"" + k + "\":null}")), "module." + k);
    }
    CHECK_HAS(refusal(open_with("{\"direct_sampling\":\"Q\"}")), "module.direct_sampling must be off, i or q");
    CHECK_HAS(refusal(open_with("{\"direct_sampling\":false}")), "module.direct_sampling");
    CHECK_HAS(refusal(open_with("{\"direct_sampling\":2}")), "module.direct_sampling");
    CHECK(open_ok(open_with("{\"bandwidth\":8000000}")));
    CHECK_HAS(refusal(open_with("{\"bandwidth\":8000001}")), "module.bandwidth");
    CHECK_HAS(refusal(open_with("{\"bandwidth\":-1}")), "module.bandwidth");
    CHECK_HAS(refusal(open_with("{\"bandwidth\":1000.5}")), "module.bandwidth");
    CHECK(open_ok(open_with("{\"buffers\":2}")));
    CHECK(open_ok(open_with("{\"buffers\":64}")));
    CHECK_HAS(refusal(open_with("{\"buffers\":1}")), "module.buffers must be a whole number from 2 to 64");
    CHECK_HAS(refusal(open_with("{\"buffers\":65}")), "module.buffers");
    CHECK_HAS(refusal(open_with("{\"buffers\":16.5}")), "module.buffers");
}

TEST(open_refuses_unknown_settings) {
    CHECK_HAS(refusal(open_with("{\"gain\":\"auto\",\"frequency\":1}")), "unknown setting module.frequency");
    // The band's own keys are not module settings.
    CHECK_HAS(refusal(open_with("{\"sample_rate\":2400000}")), "unknown setting module.sample_rate");
    const std::string m = refusal(open_with("{\"a\":1,\"b\":2}"));
    CHECK_HAS(m, "unknown settings module.a, module.b");
    CHECK_HAS(m, "device, gain, ppm, rtl_agc, bias_tee, direct_sampling, offset_tuning, bandwidth and buffers");
}

TEST(set_accepts_only_live_settings) {
    fern::LiveChange c;
    REQUIRE(!fern::parse_set(json("{\"gain\":38.6,\"rtl_agc\":true,\"bias_tee\":false}"), c));
    REQUIRE(c.gain && c.rtl_agc && c.bias_tee);
    CHECK_EQ(c.gain->db, 38.6);
    CHECK(*c.rtl_agc);
    CHECK(!*c.bias_tee);
    REQUIRE(!fern::parse_set(json("{\"gain\":\"auto\"}"), c));
    CHECK(c.gain && c.gain->automatic());
    CHECK(!c.rtl_agc && !c.bias_tee);
    REQUIRE(!fern::parse_set(json("{}"), c));
    CHECK(!c.gain && !c.rtl_agc && !c.bias_tee);

    for (const char* key : {"device", "ppm", "direct_sampling", "offset_tuning", "bandwidth", "buffers"}) {
        const auto f = fern::parse_set(json(std::string("{\"") + key + "\":0}"), c);
        REQUIRE(f);
        CHECK(f->code == fern::ErrorCode::invalid);
        CHECK_HAS(f->message, std::string("module.") + key + " cannot change while the band runs");
    }
    auto f = fern::parse_set(json("{\"volume\":3}"), c);
    REQUIRE(f);
    CHECK_HAS(f->message, "unknown setting module.volume");
    f = fern::parse_set(json("{\"gain\":\"high\"}"), c);
    REQUIRE(f);
    CHECK_HAS(f->message, "module.gain");
    f = fern::parse_set(json("{\"bias_tee\":\"on\"}"), c);
    REQUIRE(f);
    CHECK_HAS(f->message, "module.bias_tee");
    f = fern::parse_set(json("[]"), c);
    REQUIRE(f);
    CHECK_HAS(f->message, "must be an object");
}

TEST(describe_lists_every_setting) {
    const Value d = fern::describe_module();
    CHECK_EQ(d.find("api")->as_number(), 1.0);
    CHECK_EQ(d.find("id")->as_string(), std::string("rtlsdr"));
    CHECK_EQ(d.find("name")->as_string(), std::string("RTL-SDR"));
    CHECK_EQ(d.find("kind")->as_string(), std::string("input"));
    CHECK_EQ(d.find("version")->as_string(), std::string(fern::module_version()));
    const Value* settings = d.find("settings");
    REQUIRE(settings && settings->is_array());

    std::set<std::string> keys;
    std::set<std::string> live;
    for (const Value& s : settings->items()) {
        REQUIRE(s.is_object());
        const std::string key = s.find("key")->as_string();
        keys.insert(key);
        const std::string type = s.find("type")->as_string();
        CHECK(type == "string" || type == "number" || type == "boolean" || type == "choice");
        CHECK(s.find("label") && s.find("label")->is_string());
        CHECK(s.find("help") && !s.find("help")->as_string().empty());
        REQUIRE(s.find("live") && s.find("live")->is_bool());
        if (s.find("live")->as_bool())
            live.insert(key);
        const Value* def = s.find("default");
        REQUIRE(def);
        if (type == "number") {
            CHECK(def->is_number());
            CHECK(s.find("min") && s.find("max"));
            CHECK(def->as_number() >= s.find("min")->as_number());
            CHECK(def->as_number() <= s.find("max")->as_number());
        } else if (type == "boolean") {
            CHECK(def->is_bool());
        } else if (type == "choice") {
            const Value* choices = s.find("choices");
            REQUIRE(choices && choices->is_array());
            bool found = false;
            for (const Value& c : choices->items())
                found = found || c.as_string() == def->as_string();
            CHECK(found);
        } else {
            CHECK(def->is_string());
        }
        // Every declared default is accepted by open.
        const std::string one = "{\"" + key + "\":" + fern::json::serialize(*def) + "}";
        CHECK(open_ok(open_with(one)));
        // And a live setting's default by set.
        fern::LiveChange c;
        if (s.find("live")->as_bool())
            CHECK(!fern::parse_set(json(one), c));
    }
    CHECK_EQ(keys, (std::set<std::string>{"device", "gain", "ppm", "rtl_agc", "bias_tee", "direct_sampling",
                                          "offset_tuning", "bandwidth", "buffers"}));
    CHECK_EQ(live, (std::set<std::string>{"gain", "rtl_agc", "bias_tee"}));
    CHECK(fern::json::serialize(d).size() < fern::max_report_bytes);
}

TEST(achieved_sample_rate_matches_librtlsdr) {
    CHECK_EQ(fern::achieved_sample_rate(28800000, 2400000), 2400000.0);
    CHECK_EQ(fern::achieved_sample_rate(28800000, 2048000), 2048000.0);
    CHECK_EQ(fern::achieved_sample_rate(28800000, 3200000), 3200000.0);
    CHECK_EQ(fern::achieved_sample_rate(28800000, 1800000), 1800000.0);
    // 1 MHz is not a whole divisor: librtlsdr prints 1000000.026491 Hz.
    const double r = fern::achieved_sample_rate(28800000, 1000000);
    CHECK(r > 1000000.026 && r < 1000000.027);
    // Every allowed rate lands within 1 ppm of the request.
    for (uint32_t hz = 225001; hz <= 3200000; hz += 997) {
        if (!fern::sample_rate_supported(hz))
            continue;
        const double a = fern::achieved_sample_rate(28800000, hz);
        if (std::abs(a - hz) / hz > 1e-6)
            test::report(__FILE__, __LINE__, "rate " + std::to_string(hz) + " -> " + std::to_string(a));
    }
}

TEST(gain_snapping) {
    const std::vector<int> r82xx = {0,   9,   14,  27,  37,  77,  87,  125, 144, 157, 166, 197, 207, 229, 254,
                                    280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496};
    auto snapped = [&](double db) { return r82xx[static_cast<size_t>(fern::nearest_gain(r82xx, db))]; };
    CHECK_EQ(snapped(38.6), 386);
    CHECK_EQ(snapped(38.0), 386);
    CHECK_EQ(snapped(37.8), 372);
    CHECK_EQ(snapped(0), 0);
    CHECK_EQ(snapped(-3), 0);
    CHECK_EQ(snapped(60), 496);
    CHECK_EQ(snapped(5.7), 37);  // 3.7 and 7.7 are equally near; the lower wins
    const std::vector<int> e4000 = {-10, 15, 40, 65, 90, 115, 140, 165, 190, 215, 240, 290, 340, 420};
    CHECK_EQ(e4000[static_cast<size_t>(fern::nearest_gain(e4000, -2))], -10);
    CHECK_EQ(e4000[static_cast<size_t>(fern::nearest_gain(e4000, 38))], 340);  // tie between 34 and 42
    CHECK_EQ(e4000[static_cast<size_t>(fern::nearest_gain(e4000, 39))], 420);
    CHECK_EQ(fern::nearest_gain({}, 10), -1);
}

TEST(transfer_size_follows_rate) {
    CHECK_EQ(fern::transfer_bytes_for_rate(2400000), 98304u);
    CHECK_EQ(fern::transfer_bytes_for_rate(2048000), 81920u);
    CHECK_EQ(fern::transfer_bytes_for_rate(3200000), 131072u);
    CHECK_EQ(fern::transfer_bytes_for_rate(250000), 16384u);
    for (uint32_t hz : {225001u, 1000000u, 2400000u, 3200000u})
        CHECK(fern::transfer_bytes_for_rate(hz) % 512 == 0);
}
