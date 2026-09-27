// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "tuning.h"

#include <cmath>

namespace fern {

namespace {

constexpr double two_pow_22 = 4194304.0;
// The RTL-SDR Blog V4's HF mixer, and the frequency librtlsdr adds for it.
constexpr uint32_t upconverter_hz = 28800000;

}  // namespace

uint32_t r82xx_if_freq(uint32_t bandwidth_hz) {
    // r82xx_set_bandwidth() in tuner_r82xx.c, returning priv->int_freq.
    static const int low_pass[] = {1700000, 1600000, 1550000, 1450000, 1200000,
                                   900000,  700000,  550000,  450000,  350000};
    constexpr int high_pass_1 = 350000;
    constexpr int high_pass_2 = 380000;
    int bw = static_cast<int>(bandwidth_hz);
    if (bw > 6000000)
        return 4570000;
    if (bw > low_pass[0] + high_pass_1 + high_pass_2)
        return 3570000;
    uint32_t int_freq = 2300000;
    int real_bw = 0;
    if (bw > low_pass[0] + high_pass_1) {
        bw -= high_pass_2;
        int_freq += high_pass_2;
        real_bw += high_pass_2;
    }
    if (bw > low_pass[0]) {
        bw -= high_pass_1;
        int_freq += high_pass_1;
        real_bw += high_pass_1;
    }
    // bw is at most low_pass[0] now, so the loop always passes the first entry.
    unsigned i = 0;
    for (; i < sizeof low_pass / sizeof low_pass[0]; ++i)
        if (bw > low_pass[i])
            break;
    --i;
    real_bw += low_pass[i];
    return int_freq - static_cast<uint32_t>(real_bw / 2);
}

std::optional<R82xxPll> r82xx_pll(uint32_t lo_hz, uint32_t xtal_hz, bool vco_power_ref_1) {
    // r82xx_set_pll() in tuner_r82xx.c, down to the driver's integer types:
    // its narrow ones are where the fractional part loses its last hertz.
    const uint32_t freq_khz = (lo_hz + 500) / 1000;
    const uint32_t pll_ref = xtal_hz;
    const uint32_t pll_ref_khz = (xtal_hz + 500) / 1000;
    constexpr uint32_t vco_min = 1770000;
    constexpr uint32_t vco_max = vco_min * 2;

    uint32_t mix_div = 2;
    while (mix_div <= 64) {
        if (freq_khz * mix_div >= vco_min && freq_khz * mix_div < vco_max)
            break;
        mix_div <<= 1;
    }
    // The driver goes on with a divider of 128 that the chip does not have.
    if (mix_div > 64)
        return std::nullopt;

    const uint64_t vco_freq = static_cast<uint64_t>(lo_hz) * mix_div;
    const uint8_t nint = static_cast<uint8_t>(vco_freq / (2 * pll_ref));
    // 2 * pll_ref * nint is computed in 32 bits by the driver.
    const uint32_t whole = 2 * pll_ref * nint;
    uint32_t vco_fra = static_cast<uint32_t>((vco_freq - whole) / 1000);
    const unsigned power_ref = vco_power_ref_1 ? 1 : 2;
    if (nint > 128 / power_ref - 1)
        return std::nullopt;

    uint16_t sdm = 0;
    uint16_t n_sdm = 2;
    while (vco_fra > 1) {
        if (vco_fra > 2 * pll_ref_khz / n_sdm) {
            sdm = static_cast<uint16_t>(sdm + 32768 / (n_sdm / 2));
            vco_fra -= 2 * pll_ref_khz / n_sdm;
            if (n_sdm >= 0x8000)
                break;
        }
        n_sdm = static_cast<uint16_t>(n_sdm << 1);
        // Where the driver would divide by the wrapped-around zero next.
        if (n_sdm == 0)
            return std::nullopt;
    }
    return R82xxPll{mix_div, nint, sdm};
}

double r82xx_lo(const R82xxPll& pll, double xtal_hz) {
    return 2.0 * xtal_hz * (pll.nint + pll.sdm / 65536.0) / pll.mix_div;
}

int64_t rtl_if_steps(uint32_t if_hz, uint32_t rtl_xtal_hz) {
    // rtlsdr_set_if_freq(): if_freq = ((freq * TWO_POW(22)) / rtl_xtal) * (-1),
    // in double and truncated toward zero; the register holds -steps.
    return static_cast<int64_t>(if_hz * two_pow_22 / rtl_xtal_hz);
}

double rtl_if_frequency(int64_t steps, double rtl_xtal_hz) { return steps * rtl_xtal_hz / two_pow_22; }

uint32_t rtl_if_register(int64_t steps) { return static_cast<uint32_t>(-steps) & 0x3fffff; }

std::optional<Tuning> tuning_for(const TuningInput& in) {
    if (in.rtl_xtal == 0)
        return std::nullopt;
    const double scale = 1.0 + in.ppm / 1e6;
    // What librtlsdr computes register values with, and what the crystals
    // are taken to run at: rtlsdr_get_xtal_freq() truncates the first.
    const uint32_t rtl_xtal_truncated = static_cast<uint32_t>(in.rtl_xtal * scale);
    const uint32_t tuner_xtal_truncated = static_cast<uint32_t>(in.tuner_xtal * scale);
    const double rtl_xtal = in.rtl_xtal * scale;
    const double tuner_xtal = in.tuner_xtal * scale;

    Tuning t;
    if (in.direct_sampling) {
        // The ADC samples the antenna; the IF oscillator is set to the
        // frequency itself.
        t.landed = rtl_if_frequency(rtl_if_steps(in.center, rtl_xtal_truncated), rtl_xtal);
        t.if_steps = std::llround(in.center * two_pow_22 / rtl_xtal);
        t.center = rtl_if_frequency(t.if_steps, rtl_xtal);
        return t;
    }
    if (in.tuner != Tuner::r820t && in.tuner != Tuner::r828d)
        return std::nullopt;
    if (in.tuner_xtal == 0)
        return std::nullopt;

    // r82xx_set_freq(): below 28.8 MHz the V4 tunes 28.8 MHz higher, where
    // its mixer has put HF, with the local oscillator the tuner's IF above.
    const bool shifted = in.upconverter && in.center < upconverter_hz;
    const uint32_t tuned = shifted ? in.center + upconverter_hz : in.center;
    const uint32_t int_freq = r82xx_if_freq(in.filter_request);
    const auto pll = r82xx_pll(tuned + int_freq, tuner_xtal_truncated, in.tuner == Tuner::r828d || in.v4l);
    if (!pll)
        return std::nullopt;
    const double lo = r82xx_lo(*pll, tuner_xtal);
    // The V4's mixer runs from the same 28.8 MHz crystal as the rest.
    const double shift = shifted ? upconverter_hz * scale : 0.0;
    // High-side injection: the frequency at 0 Hz is the local oscillator less
    // the IF the RTL2832U mixes down from.
    t.landed = lo - shift - rtl_if_frequency(rtl_if_steps(int_freq, rtl_xtal_truncated), rtl_xtal);
    t.if_steps = std::llround((lo - shift - in.center) * two_pow_22 / rtl_xtal);
    t.center = lo - shift - rtl_if_frequency(t.if_steps, rtl_xtal);
    return t;
}

}  // namespace fern
