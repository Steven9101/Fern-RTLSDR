// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Where an RTL-SDR really tunes, and how close the RTL2832U can be told to
// bring that to the frequency asked for.
//
// librtlsdr sets the R820T and R828D synthesizer with integer arithmetic in
// kilohertz, so its local oscillator lands beside the frequency asked for:
// up to about 1.4 kHz, typically a few hundred hertz, less at HF, where the
// synthesizer divides more. The RTL2832U then mixes the tuner's IF to 0 Hz
// with an oscillator set in 22-bit steps of its crystal frequency, about
// 6.9 Hz, which librtlsdr programs truncated. The module repeats librtlsdr's
// arithmetic, which tells it exactly where the synthesizer is, and sets the
// RTL2832U's oscillator to put the frequency asked for at 0 Hz instead of
// the nominal IF. Half an oscillator step, 3.4 Hz, is what can remain. In
// direct sampling the tuner is not used, and only the rounding is left to
// improve.
//
// Frequencies are those of the crystal as the ppm correction states it.
// librtlsdr computes register values from that frequency truncated to whole
// hertz, while the hardware runs at the frequency itself, and the difference
// is worth a hertz or two at 50 MHz; both are kept apart here.
#pragma once

#include <cstdint>
#include <optional>

#include "device.h"

namespace fern {

// The IF r82xx_set_bandwidth() chooses for the R820T and R828D for the
// bandwidth librtlsdr gives it: the module's bandwidth setting, or the
// sample rate.
uint32_t r82xx_if_freq(uint32_t bandwidth_hz);

// The synthesizer settings r82xx_set_pll() computes for a local oscillator
// frequency and the reference crystal frequency it is given.
struct R82xxPll {
    uint32_t mix_div = 0;  // output divider, 2 to 64
    uint32_t nint = 0;     // integer part of the multiplier
    uint32_t sdm = 0;      // fractional part, in 65536ths
};
// Empty where r82xx_set_pll() finds no settings. vco_power_ref_1 is true for
// the R828D and the RTL-SDR Blog V4L, which the driver allows a larger
// multiplier.
std::optional<R82xxPll> r82xx_pll(uint32_t lo_hz, uint32_t xtal_hz, bool vco_power_ref_1);
// The frequency the synthesizer produces with those settings from a
// reference of xtal_hz.
double r82xx_lo(const R82xxPll& pll, double xtal_hz);

// The RTL2832U's IF oscillator runs in steps of its crystal frequency /
// 2^22. rtlsdr_set_if_freq() sets it to a frequency truncated to a step;
// these are that number of steps for if_hz and a crystal of rtl_xtal_hz, the
// frequency a number of steps gives with a crystal of rtl_xtal_hz, and the
// 22-bit register value that holds it. Frequencies above half the crystal
// wrap around in the register, as they do in the ADC's sampling.
int64_t rtl_if_steps(uint32_t if_hz, uint32_t rtl_xtal_hz);
double rtl_if_frequency(int64_t steps, double rtl_xtal_hz);
uint32_t rtl_if_register(int64_t steps);

struct TuningInput {
    Tuner tuner = Tuner::unknown;
    // RTL-SDR Blog V4 and V4L: below 28.8 MHz librtlsdr tunes the
    // frequency plus 28.8 MHz, which a mixer on the dongle shifts HF to.
    bool upconverter = false;
    bool v4l = false;
    bool direct_sampling = false;
    uint32_t center = 0;          // asked for, Hz
    uint32_t rtl_xtal = 0;        // nominal crystal frequencies, Hz
    uint32_t tuner_xtal = 0;
    int ppm = 0;
    uint32_t filter_request = 0;  // what librtlsdr last gave the tuner's filter
};

struct Tuning {
    double landed = 0;    // what librtlsdr's settings put at 0 Hz
    int64_t if_steps = 0; // the RTL2832U IF that brings center closest to 0 Hz
    double center = 0;    // what that IF puts at 0 Hz
};

// Empty for the tuners whose synthesizer the module does not follow: the
// E4000, FC0012, FC0013 and FC2580 outside direct sampling.
std::optional<Tuning> tuning_for(const TuningInput& in);

}  // namespace fern
