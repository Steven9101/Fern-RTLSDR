// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Where an RTL-SDR tunes: tuning.cpp against hand-worked examples, and
// against the vendored R820T/R828D driver itself, which runs here on a
// register file of its own so that every register it writes can be read.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "test.h"
#include "tuning.h"

extern "C" {
#include "../third_party/rtl-sdr-blog/include/tuner_r82xx.h"

// What tuner_r82xx.c needs from librtlsdr.c, answered by the tests.
int rtlsdr_check_dongle_model(void* dev, char* manufacturer, char* product);
int rtlsdr_i2c_write_fn(void* dev, uint8_t addr, uint8_t* buf, int len);
int rtlsdr_i2c_read_fn(void* dev, uint8_t addr, uint8_t* buf, int len);
int rtlsdr_set_bias_tee_gpio(void* dev, int gpio, int on);
}

using fern::Tuner;

namespace {

constexpr double step_hz = 28800000.0 / 4194304.0;  // the RTL2832U's IF step at 28.8 MHz

// The dongle tuner_r82xx.c believes it drives.
struct Chip {
    std::string product;          // "Blog V4", "Blog V4L", or anything else
    uint8_t vco_fine_tune = 0x20;  // register 4, bits 5 and 4, as the chip reports them
};

uint8_t bit_reverse(uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; ++i)
        if (b & (1u << i))
            r = static_cast<uint8_t>(r | (0x80u >> i));
    return r;
}

}  // namespace

extern "C" {

int rtlsdr_check_dongle_model(void* dev, char* manufacturer, char* product) {
    const Chip* chip = static_cast<const Chip*>(dev);
    return std::string(manufacturer) == "RTLSDRBlog" && chip->product == product;
}

int rtlsdr_i2c_write_fn(void*, uint8_t, uint8_t*, int len) { return len; }

// Reads start at register 0 and come back bit-reversed, which
// r82xx_read() undoes: the PLL is locked, and the VCO fine tune is what
// the chip was given.
int rtlsdr_i2c_read_fn(void* dev, uint8_t, uint8_t* buf, int len) {
    const Chip* chip = static_cast<const Chip*>(dev);
    for (int i = 0; i < len; ++i) {
        uint8_t value = 0;
        if (i == 2)
            value = 0x40;
        if (i == 4)
            value = chip->vco_fine_tune;
        buf[i] = bit_reverse(value);
    }
    return len;
}

int rtlsdr_set_bias_tee_gpio(void*, int, int) { return 0; }

}  // extern "C"

TEST(tuning_r82xx_if_follows_the_bandwidth) {
    CHECK_EQ(fern::r82xx_if_freq(8000000), 4570000u);
    CHECK_EQ(fern::r82xx_if_freq(6500000), 4570000u);
    CHECK_EQ(fern::r82xx_if_freq(3200000), 3570000u);
    // 2400000: both high-pass steps and the 1.7 MHz low pass, 2.43 MHz wide.
    CHECK_EQ(fern::r82xx_if_freq(2400000), 1815000u);
    CHECK_EQ(fern::r82xx_if_freq(2048000), 1625000u);
    CHECK_EQ(fern::r82xx_if_freq(1000000), 1700000u);
    CHECK_EQ(fern::r82xx_if_freq(300000), 2125000u);
}

TEST(tuning_r82xx_pll_worked_by_hand) {
    // 100 MHz at 2.4 Msps: an LO of 101.815 MHz, a VCO of 3.25808 GHz with
    // a divider of 32, 56 whole multiples of 57.6 MHz and 32480 kHz over,
    // which the driver's halving steps turn into 36954/65536.
    const auto pll = fern::r82xx_pll(101815000, 28800000, false);
    REQUIRE(pll.has_value());
    CHECK_EQ(pll->mix_div, 32u);
    CHECK_EQ(pll->nint, 56u);
    CHECK_EQ(pll->sdm, 36954u);
    // 28 Hz below what was asked for.
    CHECK(std::fabs(fern::r82xx_lo(*pll, 28800000.0) - 101814971.92) < 0.01);
    // An LO no divider brings into the VCO's range.
    CHECK(!fern::r82xx_pll(26000000, 28800000, false).has_value());
}

TEST(tuning_rtl2832_if_steps) {
    // rtlsdr_set_if_freq() truncates: 1.815 MHz is 264328.53 steps.
    CHECK_EQ(fern::rtl_if_steps(1815000, 28800000), 264328);
    CHECK(std::fabs(fern::rtl_if_frequency(264328, 28800000.0) - 1814996.34) < 0.01);
    // The register holds -steps in 22 bits.
    CHECK_EQ(fern::rtl_if_register(264328), static_cast<uint32_t>(4194304 - 264328));
    // Above 14.4 MHz the register wraps around, as librtlsdr's writes do.
    CHECK_EQ(fern::rtl_if_register(3058346), 1135958u);
}

TEST(tuning_puts_the_band_where_it_was_asked_for) {
    fern::TuningInput in;
    in.tuner = Tuner::r820t;
    in.center = 100000000;
    in.rtl_xtal = 28800000;
    in.tuner_xtal = 28800000;
    in.filter_request = 2400000;
    const auto t = fern::tuning_for(in);
    REQUIRE(t.has_value());
    // librtlsdr: the LO 28.08 Hz low, the IF truncated 3.66 Hz low.
    CHECK(std::fabs(t->landed - (100000000 - 24.41)) < 0.01);
    // Four steps less of IF: 3.05 Hz above.
    CHECK_EQ(t->if_steps, 264324);
    CHECK(std::fabs(t->center - 100000003.05) < 0.01);
}

TEST(tuning_handles_hf_on_the_blog_v4) {
    fern::TuningInput in;
    in.tuner = Tuner::r828d;
    in.upconverter = true;
    in.center = 14200000;
    in.rtl_xtal = 28800000;
    in.tuner_xtal = 28800000;
    in.ppm = 1;
    in.filter_request = 2400000;
    const auto t = fern::tuning_for(in);
    REQUIRE(t.has_value());
    CHECK(std::fabs(t->center - 14200000) <= step_hz / 2);
    // Without the V4's mixer 14.2 MHz is out of the synthesizer's reach.
    in.upconverter = false;
    CHECK(!fern::tuning_for(in).has_value());
}

TEST(tuning_rounds_direct_sampling) {
    fern::TuningInput in;
    in.tuner = Tuner::r820t;
    in.direct_sampling = true;
    in.center = 7100000;
    in.rtl_xtal = 28800000;
    in.filter_request = 2400000;
    const auto t = fern::tuning_for(in);
    REQUIRE(t.has_value());
    CHECK(t->landed <= 7100000 && t->landed > 7100000 - step_hz);
    CHECK(std::fabs(t->center - 7100000) <= step_hz / 2);
    // The tuners the module does not follow are left alone, but in direct
    // sampling, where no tuner is used.
    in.tuner = Tuner::e4000;
    CHECK(fern::tuning_for(in).has_value());
    in.direct_sampling = false;
    CHECK(!fern::tuning_for(in).has_value());
}

// tuning.cpp against tuner_r82xx.c: for every chip, crystal correction,
// bandwidth and a spread of frequencies, the driver is run and the registers
// it wrote are held against what the module computes, and the correction is
// held to half an IF step.
TEST(tuning_matches_the_r82xx_driver) {
    struct Kind {
        const char* name;
        enum r82xx_chip chip;
        uint8_t address;
        uint32_t xtal;
        std::string product;
        double low, high;
    };
    const Kind kinds[] = {
        {"R820T", CHIP_R820T, R820T_I2C_ADDR, 28800000, "RTL2838UHIDIR", 24e6, 1766e6},
        {"RTL-SDR Blog V4", CHIP_R828D, R828D_I2C_ADDR, 28800000, "Blog V4", 0.5e6, 1766e6},
        {"RTL-SDR Blog V4L", CHIP_R820T, R820T_I2C_ADDR, 28800000, "Blog V4L", 0.5e6, 1766e6},
        {"R828D", CHIP_R828D, R828D_I2C_ADDR, 16000000, "RTL2838UHIDIR", 24e6, 1766e6},
    };
    const uint32_t filters[] = {240000, 300000, 1000000, 1024000, 2048000, 2400000, 3200000, 6500000, 8000000};
    const int ppms[] = {0, -37, 50, 123};
    std::mt19937 random(20260927);
    int cases = 0, compared = 0;
    double worst_after = 0;
    for (const Kind& kind : kinds) {
        const bool v4 = kind.product == "Blog V4" || kind.product == "Blog V4L";
        const bool power_ref_1 = kind.chip == CHIP_R828D || kind.product == "Blog V4L";
        Chip chip{kind.product, static_cast<uint8_t>((power_ref_1 ? 1 : 2) << 4)};
        double worst_before = 0;
        std::vector<double> errors;
        for (int ppm : ppms) {
            const uint32_t xtal = static_cast<uint32_t>(kind.xtal * (1.0 + ppm / 1e6));
            for (uint32_t filter : filters) {
                std::uniform_real_distribution<double> spread(std::log(kind.low), std::log(kind.high));
                for (int i = 0; i < 60; ++i) {
                    const uint32_t center = static_cast<uint32_t>(std::exp(spread(random)));
                    ++cases;
                    r82xx_config config{kind.address, xtal, kind.chip, 8, 0};
                    r82xx_priv priv{};
                    priv.cfg = &config;
                    priv.rtl_dev = &chip;
                    const int int_freq = r82xx_set_bandwidth(&priv, static_cast<int>(filter), 2400000);
                    CHECK_EQ(static_cast<uint32_t>(int_freq), fern::r82xx_if_freq(filter));
                    if (r82xx_set_freq(&priv, center) != 0)
                        continue;
                    const uint32_t tuned = v4 && center < 28800000 ? center + 28800000 : center;
                    const auto pll = fern::r82xx_pll(tuned + int_freq, xtal, power_ref_1);
                    // Below 27.66 MHz no divider brings the LO into the VCO's
                    // range, and the driver goes on with one the chip lacks.
                    if ((tuned + int_freq + 500) / 1000 * 64 < 1770000) {
                        CHECK(!pll.has_value());
                        continue;
                    }
                    REQUIRE(pll.has_value());
                    const uint8_t r14 = priv.regs[0x14 - REG_SHADOW_START];
                    const uint32_t nint = 4u * (r14 & 0x3fu) + (r14 >> 6) + 13u;
                    const uint32_t sdm = static_cast<uint32_t>(priv.regs[0x16 - REG_SHADOW_START]) << 8 |
                                         priv.regs[0x15 - REG_SHADOW_START];
                    const uint32_t div_num = priv.regs[0x10 - REG_SHADOW_START] >> 5;
                    CHECK_EQ(pll->nint, nint);
                    CHECK_EQ(pll->sdm, sdm);
                    CHECK_EQ(pll->mix_div, 2u << div_num);
                    ++compared;

                    fern::TuningInput in;
                    in.tuner = kind.chip == CHIP_R828D ? Tuner::r828d : Tuner::r820t;
                    in.upconverter = v4;
                    in.v4l = kind.product == "Blog V4L";
                    in.center = center;
                    in.rtl_xtal = 28800000;
                    in.tuner_xtal = kind.xtal;
                    in.ppm = ppm;
                    in.filter_request = filter;
                    const auto t = fern::tuning_for(in);
                    REQUIRE(t.has_value());
                    const double step = 28800000 * (1.0 + ppm / 1e6) / 4194304.0;
                    const double after = std::fabs(t->center - center);
                    CHECK(after <= step / 2 + 1e-6);
                    worst_after = std::max(worst_after, after);
                    worst_before = std::max(worst_before, std::fabs(t->landed - center));
                    errors.push_back(std::fabs(t->landed - center));
                }
            }
        }
        std::sort(errors.begin(), errors.end());
        if (!errors.empty())
            std::printf("    %s: librtlsdr lands up to %.0f Hz away, half the time %.0f Hz or more\n", kind.name,
                        worst_before, errors[errors.size() / 2]);
    }
    std::printf("    %d cases, %d held against the driver's registers, at most %.2f Hz away when corrected\n", cases,
                compared, worst_after);
    CHECK(compared > cases * 9 / 10);
}
