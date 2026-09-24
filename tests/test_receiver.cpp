// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <string>

#include "fake_backend.h"
#include "receiver.h"
#include "test.h"

using fern::DirectSampling;
using fern::ErrorCode;
using fern::Tuner;

namespace {

fern::OpenRequest request(uint32_t center = 100000000, uint32_t rate = 2400000) {
    fern::OpenRequest r;
    r.sample_rate = rate;
    r.center = center;
    return r;
}

fake::Spec spec(const std::string& serial, Tuner tuner = Tuner::r820t) {
    fake::Spec s;
    s.serial = serial;
    s.tuner = tuner;
    return s;
}

fake::Spec blog_v4(const std::string& serial = "00000001") {
    fake::Spec s;
    s.manufacturer = "RTLSDRBlog";
    s.product = "Blog V4";
    s.serial = serial;
    s.tuner = Tuner::r828d;
    return s;
}

// Opens with the request and expects a failure with the given code.
std::string refused(fake::Backend& backend, const fern::OpenRequest& r, ErrorCode code) {
    fern::Receiver receiver(backend);
    const auto f = receiver.open(r);
    if (!f) {
        test::report(__FILE__, __LINE__, "open succeeded but should have failed");
        return "";
    }
    if (f->code != code)
        test::report(__FILE__, __LINE__, "wrong code " + std::string(fern::error_code_name(f->code)) + ": " + f->message);
    CHECK(!receiver.is_open());
    return f->message;
}

size_t position(const std::vector<std::string>& calls, const std::string& prefix) {
    for (size_t i = 0; i < calls.size(); ++i)
        if (calls[i].rfind(prefix, 0) == 0)
            return i;
    return calls.size();
}

bool called(const std::vector<std::string>& calls, const std::string& prefix) {
    return position(calls, prefix) < calls.size();
}

}  // namespace

TEST(receiver_opens_the_only_device) {
    fake::Backend backend({blog_v4()});
    fern::Receiver receiver(backend);
    REQUIRE(!receiver.open(request(14200000)));
    CHECK_EQ(receiver.info().name, std::string("RTL-SDR Blog V4"));
    CHECK_EQ(receiver.info().serial, std::string("00000001"));
    CHECK(receiver.info().tuner == Tuner::r828d);
    const fern::Effective& e = receiver.effective();
    CHECK_EQ(e.sample_rate, 2400000.0);
    CHECK_EQ(e.center, 14200000u);
    CHECK(e.gain.automatic);
    CHECK(e.bias_tee_effective.has_value());
    CHECK(!*e.bias_tee_effective);
    const auto st = backend.last();
    CHECK_EQ(st->sample_rate, 2400000u);
    CHECK_EQ(st->center, 14200000u);
    CHECK_EQ(st->reset_buffer_calls, 1);
    CHECK(!st->manual_gain);
    const std::string device = fern::json::serialize(receiver.device_json());
    CHECK_EQ(device, std::string("{\"name\":\"RTL-SDR Blog V4\",\"serial\":\"00000001\",\"tuner\":\"R828D\",\"index\":0}"));
    receiver.close();
    CHECK(st->closed);
}

TEST(receiver_tunes_before_setting_the_rate) {
    fake::Backend backend({spec("A")});
    fern::Receiver receiver(backend);
    fern::OpenRequest r = request(100000000, 2048000);
    r.settings.ppm = 17;
    r.settings.bandwidth = 1000000;
    REQUIRE(!receiver.open(r));
    const auto calls = backend.last()->calls;
    const size_t ppm = position(calls, "set_freq_correction 17");
    const size_t tune = position(calls, "set_center_freq 100000000");
    const size_t rate = position(calls, "set_sample_rate 2048000");
    const size_t bw = position(calls, "set_tuner_bandwidth 1000000");
    const size_t reset = position(calls, "reset_buffer");
    CHECK(ppm < tune);
    CHECK(tune < rate);
    CHECK(rate < bw);
    CHECK(bw < reset);
    CHECK(reset < calls.size());
    CHECK(!called(calls, "set_offset_tuning"));
    CHECK(!called(calls, "set_direct_sampling"));
}

TEST(receiver_selects_by_serial_and_index) {
    fake::Backend backend({spec("A"), spec("B"), spec("C")});
    {
        fern::Receiver receiver(backend);
        fern::OpenRequest r = request();
        r.settings.device.kind = fern::DeviceSelector::Kind::serial;
        r.settings.device.serial = "B";
        REQUIRE(!receiver.open(r));
        CHECK_EQ(receiver.info().index, 1u);
        CHECK_EQ(receiver.info().serial, std::string("B"));
    }
    {
        fern::Receiver receiver(backend);
        fern::OpenRequest r = request();
        r.settings.device.kind = fern::DeviceSelector::Kind::index;
        r.settings.device.index = 2;
        REQUIRE(!receiver.open(r));
        CHECK_EQ(receiver.info().serial, std::string("C"));
    }
    fern::OpenRequest r = request();
    const std::string several = refused(backend, r, ErrorCode::invalid);
    CHECK_HAS(several, "3 RTL-SDRs are plugged in");
    CHECK_HAS(several, "index 0 serial A, index 1 serial B, index 2 serial C");
    CHECK_HAS(several, "module.device = serial:<serial>");

    r.settings.device.kind = fern::DeviceSelector::Kind::serial;
    r.settings.device.serial = "Z";
    const std::string missing = refused(backend, r, ErrorCode::no_device);
    CHECK_HAS(missing, "no RTL-SDR with serial Z is plugged in");
    CHECK_HAS(missing, "serial A");

    r.settings.device.kind = fern::DeviceSelector::Kind::index;
    r.settings.device.index = 3;
    CHECK_HAS(refused(backend, r, ErrorCode::no_device), "there is no RTL-SDR at index 3");
}

TEST(receiver_refuses_ambiguous_serials) {
    fake::Backend backend({spec("00000001"), spec("00000001")});
    fern::OpenRequest r = request();
    r.settings.device.kind = fern::DeviceSelector::Kind::serial;
    r.settings.device.serial = "00000001";
    const std::string m = refused(backend, r, ErrorCode::invalid);
    CHECK_HAS(m, "2 RTL-SDRs have the serial 00000001 (indexes 0, 1)");
    CHECK_HAS(m, "rtl_eeprom -s");
    CHECK_EQ(backend.opens(), 0);
}

TEST(receiver_reports_missing_busy_and_forbidden_devices) {
    {
        fake::Backend backend;
        CHECK_HAS(refused(backend, request(), ErrorCode::no_device), "no RTL-SDR is plugged in");
    }
    {
        fake::Backend backend;
        backend.enumeration_error = -99;
        backend.enumeration_diagnostic = "add AF_NETLINK to RestrictAddressFamilies";
        CHECK_HAS(refused(backend, request(), ErrorCode::usb), "AF_NETLINK");
    }
    {
        fake::Spec s = spec("A");
        s.open_error = fern::usb_error::busy;
        fake::Backend backend({s});
        CHECK_HAS(refused(backend, request(), ErrorCode::busy), "in use by another program");
    }
    {
        fake::Spec s = spec("A");
        s.open_error = fern::usb_error::kernel_driver;
        fake::Backend backend({s});
        CHECK_HAS(refused(backend, request(), ErrorCode::busy), "dvb_usb_rtl28xxu");
    }
    {
        fake::Spec s = spec("A");
        s.open_error = fern::usb_error::access;
        fake::Backend backend({s});
        CHECK_HAS(refused(backend, request(), ErrorCode::usb), "udev rule");
    }
    {
        fake::Spec a = spec("A");
        a.strings_error = fern::usb_error::access;
        fake::Backend backend({a, spec("B")});
        fern::OpenRequest r = request();
        r.settings.device.kind = fern::DeviceSelector::Kind::serial;
        r.settings.device.serial = "A";
        const std::string m = refused(backend, r, ErrorCode::no_device);
        CHECK_HAS(m, "index 0 serial unreadable (permission denied)");
        CHECK_HAS(m, "udev rule");
    }
}

TEST(receiver_refuses_hf_on_r820t_without_direct_sampling) {
    fake::Backend backend({spec("A")});
    const std::string m = refused(backend, request(7100000), ErrorCode::invalid);
    CHECK_HAS(m, "cannot receive 7100000 Hz");
    CHECK_HAS(m, "module.direct_sampling = q");
    CHECK(backend.last()->closed);

    fern::Receiver receiver(backend);
    fern::OpenRequest r = request(7100000);
    r.settings.direct_sampling = DirectSampling::q;
    REQUIRE(!receiver.open(r));
    CHECK(receiver.effective().direct_sampling == DirectSampling::q);
    CHECK_EQ(backend.last()->direct_sampling, 2);
    CHECK_HAS(fern::json::serialize(receiver.settings_json()), "\"direct_sampling\":\"q\"");
}

TEST(receiver_tunes_hf_on_blog_v4_without_direct_sampling) {
    fake::Backend backend({blog_v4()});
    fern::Receiver receiver(backend);
    REQUIRE(!receiver.open(request(7100000)));
    CHECK(receiver.effective().direct_sampling == DirectSampling::off);
}

TEST(receiver_limits_direct_sampling_to_the_adc_clock) {
    fake::Backend backend({spec("A")});
    fern::OpenRequest r = request(28800000);
    r.settings.direct_sampling = DirectSampling::i;
    CHECK_HAS(refused(backend, r, ErrorCode::invalid), "direct sampling receives below 28800000 Hz");
    r.center = 28799999;
    fern::Receiver receiver(backend);
    CHECK(!receiver.open(r));
}

TEST(receiver_refuses_tuning_outside_the_tuner_range) {
    fake::Backend backend({spec("A", Tuner::e4000)});
    const std::string m = refused(backend, request(2500000000u), ErrorCode::invalid);
    CHECK_HAS(m, "the E4000 tuner could not tune to 2500000000 Hz");
    CHECK_HAS(m, "52 to 2200 MHz");
}

TEST(receiver_refuses_an_unlocked_pll) {
    fake::Spec s = blog_v4();
    s.pll_lock = false;
    fake::Backend backend({s});
    CHECK_HAS(refused(backend, request(1700000000), ErrorCode::invalid), "could not lock its PLL at 1700000000 Hz");
}

TEST(receiver_never_asks_r82xx_for_offset_tuning) {
    // librtlsdr would switch the bias tee instead.
    fake::Backend backend({spec("A")});
    fern::OpenRequest r = request();
    r.settings.offset_tuning = true;
    CHECK_HAS(refused(backend, r, ErrorCode::invalid), "offset tuning is not available with the R820T tuner");
    CHECK_EQ(backend.opens(), 1);
    CHECK(!called(backend.last()->calls, "set_offset_tuning"));
    CHECK(!backend.last()->bias_tee);

    fake::Backend e4k({spec("A", Tuner::e4000)});
    fern::Receiver receiver(e4k);
    REQUIRE(!receiver.open(r));
    CHECK(e4k.last()->offset_tuning);
    CHECK(receiver.effective().offset_tuning);

    r.settings.direct_sampling = DirectSampling::q;
    CHECK_HAS(refused(e4k, r, ErrorCode::invalid), "cannot be combined with direct sampling");
}

TEST(receiver_limits_bandwidth_to_tuners_with_filters) {
    fern::OpenRequest r = request();
    r.settings.bandwidth = 1000000;
    fake::Backend fc({spec("A", Tuner::fc0012)});
    CHECK_HAS(refused(fc, r, ErrorCode::invalid), "this RTL-SDR has an FC0012");
    fake::Backend r820t({spec("A")});
    fern::OpenRequest ds = request(7000000);
    ds.settings.bandwidth = 1000000;
    ds.settings.direct_sampling = DirectSampling::q;
    CHECK_HAS(refused(r820t, ds, ErrorCode::invalid), "no effect in direct sampling mode");
}

TEST(receiver_snaps_and_checks_gain) {
    fake::Backend backend({spec("A")});
    fern::Receiver receiver(backend);
    fern::OpenRequest r = request();
    r.settings.gain = fern::GainSetting{false, 38.0};
    REQUIRE(!receiver.open(r));
    CHECK(!receiver.effective().gain.automatic);
    CHECK_EQ(receiver.effective().gain.db, 38.6);
    const auto st = backend.last();
    CHECK(st->manual_gain);
    CHECK_EQ(st->gain, 386);
    CHECK_HAS(fern::json::serialize(receiver.settings_json()), "\"gain\":38.6");

    // Changing a manual gain does not pass through the lowest step again.
    fern::LiveChange c;
    c.gain = fern::GainSetting{false, 20};
    REQUIRE(!receiver.apply(c));
    CHECK_EQ(st->gain, 197);
    CHECK_EQ(std::count(st->calls.begin(), st->calls.end(), std::string("set_tuner_gain_mode 1")), 1L);
    CHECK_EQ(fern::json::serialize(receiver.settings_json(c)), std::string("{\"gain\":19.7}"));

    c.gain = fern::GainSetting{false, 55};
    auto f = receiver.apply(c);
    REQUIRE(f);
    CHECK(f->code == ErrorCode::invalid);
    CHECK_HAS(f->message, "gain 55 dB is outside the range of the R820T tuner, 0 to 49.6 dB");
    CHECK_EQ(st->gain, 197);
    c.gain = fern::GainSetting{false, 50.5};
    CHECK(!receiver.apply(c));
    CHECK_EQ(st->gain, 496);

    c.gain = fern::GainSetting{true, 0};
    REQUIRE(!receiver.apply(c));
    CHECK(!st->manual_gain);
    CHECK_EQ(fern::json::serialize(receiver.settings_json(c)), std::string("{\"gain\":\"auto\"}"));
}

TEST(receiver_refuses_gain_that_cannot_work) {
    fern::OpenRequest r = request(400000000);
    r.settings.gain = fern::GainSetting{false, 10};
    fake::Backend fc2580({spec("A", Tuner::fc2580)});
    CHECK_HAS(refused(fc2580, r, ErrorCode::invalid), "the FC2580 tuner has no adjustable gain");
    fake::Backend r820t({spec("A")});
    fern::OpenRequest ds = request(7000000);
    ds.settings.gain = fern::GainSetting{false, 10};
    ds.settings.direct_sampling = DirectSampling::q;
    CHECK_HAS(refused(r820t, ds, ErrorCode::invalid), "module.gain has no effect");
}

TEST(receiver_reports_a_failed_gain_write) {
    fake::Spec s = spec("A");
    s.fail_gain = -1;
    fake::Backend backend({s});
    fern::OpenRequest r = request();
    r.settings.gain = fern::GainSetting{false, 30};
    const std::string m = refused(backend, r, ErrorCode::usb);
    CHECK_HAS(m, "setting the tuner gain to 29.7 dB failed");
}

TEST(receiver_reports_bias_tee_from_eeprom) {
    {
        fake::Backend backend({blog_v4()});
        fern::Receiver receiver(backend);
        fern::OpenRequest r = request();
        r.settings.bias_tee = true;
        REQUIRE(!receiver.open(r));
        CHECK(backend.last()->bias_tee);
        CHECK_HAS(fern::json::serialize(receiver.settings_json()), "\"bias_tee\":true,\"bias_tee_effective\":true");
        fern::LiveChange c;
        c.bias_tee = false;
        REQUIRE(!receiver.apply(c));
        CHECK(!backend.last()->bias_tee);
        CHECK_EQ(fern::json::serialize(receiver.settings_json(c)),
                 std::string("{\"bias_tee\":false,\"bias_tee_effective\":false}"));
        c.bias_tee = true;
        REQUIRE(!receiver.apply(c));
        // Closing switches off what the module switched on.
        const auto st = backend.last();
        receiver.close();
        CHECK(!st->bias_tee);
    }
    {
        fake::Spec s = blog_v4();
        s.eeprom_byte7 = 0x14;  // the flag that forces the bias tee on
        fake::Backend backend({s});
        fern::Receiver receiver(backend);
        REQUIRE(!receiver.open(request()));
        CHECK_HAS(fern::json::serialize(receiver.settings_json()), "\"bias_tee\":false,\"bias_tee_effective\":true");
    }
    {
        fake::Spec s = spec("A");
        s.eeprom_byte7 = -1;
        fake::Backend backend({s});
        fern::Receiver receiver(backend);
        REQUIRE(!receiver.open(request()));
        CHECK_HAS(fern::json::serialize(receiver.settings_json()), "\"bias_tee_effective\":\"unknown\"");
    }
}

TEST(receiver_settings_json_lists_effective_values) {
    fake::Backend backend({spec("A")});
    fern::Receiver receiver(backend);
    fern::OpenRequest r = request();
    r.settings.ppm = -3;
    r.settings.rtl_agc = true;
    r.settings.buffers = 8;
    REQUIRE(!receiver.open(r));
    CHECK_EQ(fern::json::serialize(receiver.settings_json()),
             std::string("{\"gain\":\"auto\",\"ppm\":-3,\"rtl_agc\":true,\"bias_tee\":false,\"bias_tee_effective\":"
                         "false,\"direct_sampling\":\"off\",\"offset_tuning\":false,\"bandwidth\":2430000,\"buffers\":8}"));
    CHECK(backend.last()->agc);
}

TEST(filter_bandwidth_follows_the_tuner_drivers) {
    // r82xx_set_bandwidth(): the 6, 7 and 8 MHz filters, else high-pass
    // corners of 380 and 350 kHz plus the narrowest low-pass filter that is
    // not below what remains.
    CHECK_EQ(fern::tuner_filter_bandwidth(Tuner::r820t, 2400000), 2430000u);
    CHECK_EQ(fern::tuner_filter_bandwidth(Tuner::r820t, 2048000), 2050000u);
    CHECK_EQ(fern::tuner_filter_bandwidth(Tuner::r828d, 1000000), 1200000u);
    CHECK_EQ(fern::tuner_filter_bandwidth(Tuner::r828d, 250000), 350000u);
    CHECK_EQ(fern::tuner_filter_bandwidth(Tuner::r820t, 3000000), 6000000u);
    CHECK_EQ(fern::tuner_filter_bandwidth(Tuner::r820t, 6500000), 7000000u);
    CHECK_EQ(fern::tuner_filter_bandwidth(Tuner::r820t, 8000000), 8000000u);
    // e4000_set_bw(): the narrowest of the mixer, RC and channel filters.
    CHECK_EQ(fern::tuner_filter_bandwidth(Tuner::e4000, 2400000), 2300000u);
    CHECK_EQ(fern::tuner_filter_bandwidth(Tuner::e4000, 4080000), 4100000u);
    CHECK_EQ(fern::tuner_filter_bandwidth(Tuner::fc0012, 2400000), 6000000u);
    CHECK_EQ(fern::tuner_filter_bandwidth(Tuner::fc0013, 1000000), 6000000u);
    CHECK_EQ(fern::tuner_filter_bandwidth(Tuner::fc2580, 2400000), 1530000u);
    CHECK_EQ(fern::tuner_filter_bandwidth(Tuner::unknown, 2400000), 0u);
}

TEST(receiver_reports_the_filter_in_use) {
    {
        fake::Backend backend({spec("A")});
        fern::Receiver receiver(backend);
        fern::OpenRequest r = request();
        r.settings.bandwidth = 1000000;
        REQUIRE(!receiver.open(r));
        CHECK_EQ(receiver.effective().bandwidth, 1200000u);
    }
    {
        // Offset tuning widens the filter to cover the shifted band.
        fake::Backend backend({spec("A", Tuner::e4000)});
        fern::Receiver receiver(backend);
        fern::OpenRequest r = request();
        r.settings.offset_tuning = true;
        REQUIRE(!receiver.open(r));
        CHECK_EQ(receiver.effective().bandwidth, 4100000u);
    }
    {
        fake::Backend backend({spec("A")});
        fern::Receiver receiver(backend);
        fern::OpenRequest r = request(7000000);
        r.settings.direct_sampling = DirectSampling::q;
        REQUIRE(!receiver.open(r));
        CHECK_EQ(receiver.effective().bandwidth, 0u);
    }
}
