// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The module's own gain control, what gain = auto means: the highest of the
// tuner's gain steps that keeps the RTL2832U's 8-bit converter out of
// clipping with room to spare.
//
// A signal that keeps clipping is lost to every listener of the band, and
// the splatter of a converter driven past its limit lands all over the band,
// so a gain that keeps clipping comes down soon: a step once three tenths of
// a second in the last second clipped, 6 dB when those clipped heavily. A
// crash of static or a spark clips for a millisecond or so, which costs
// nobody much, while a gain lowered for it would cost the whole band its
// sensitivity for good: a tenth or two of a second that clip do not bring
// the gain down, and the gain still goes up past them. It goes up a step
// once nothing but such crashes has clipped for a while, and the peaks,
// all but the highest twentieth of them, would stay 6 dB under full scale
// one step higher. The while is 5 s in the first minutes, so that a band
// finds its gain soon after it starts, and a minute afterwards, so that a
// listener hears the level change rarely.
//
// The tuner's own AGC (gain = tuner) watches the tuner's power detectors,
// not the converter, and leaves it clipping on strong signals it does not
// see; that was the default before this.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fern {

struct GainControlTiming {
    // Clipping is judged over windows of this length.
    std::chrono::milliseconds window{100};
    // After a change the USB transfers in flight still carry samples taken
    // at the old gain: up to 64 of about 20 ms each.
    std::chrono::milliseconds settle{1500};
    // Nothing but crashes clipped for this long before a step up: at first,
    // and later.
    std::chrono::milliseconds hold_start{5000};
    std::chrono::milliseconds hold{60000};
    std::chrono::milliseconds start_phase{120000};
};

class GainControl {
public:
    using Clock = std::chrono::steady_clock;

    // steps: the tuner's gains in tenths of a dB, ascending; start: the index
    // in use now. samples and clipped are the stream's counters at `now`.
    GainControl(std::vector<int> steps, size_t start, Clock::time_point now, uint64_t samples, uint64_t clipped,
                GainControlTiming timing = GainControlTiming());

    // One look at the stream: its counters, which only grow, and the largest
    // distance of I or Q from the converter's midpoint since the last look,
    // 0 to 128. Returns the step to use from now on; reason() says why it
    // changed.
    size_t update(Clock::time_point now, uint64_t samples, uint64_t clipped, unsigned peak);

    size_t step() const { return step_; }
    double gain_db() const { return steps_[step_] / 10.0; }
    const std::string& reason() const { return reason_; }
    // True while the lowest step keeps clipping: then only less signal helps,
    // an attenuator in front of the dongle.
    bool clipping_at_lowest() const { return clipping_at_lowest_; }

    // A window clips when more than this share of its samples do.
    static constexpr double clip_limit = 1e-4;
    // And clips heavily beyond this share.
    static constexpr double clip_heavy = 1e-2;
    // How many clipping windows of the last ten bring the gain down.
    static constexpr int clip_windows = 3;
    // A step up must leave the peaks under this distance from the midpoint:
    // 6 dB under full scale.
    static constexpr unsigned peak_limit = 64;

private:
    void change(size_t to, Clock::time_point now, std::string why);
    void start_hold(Clock::time_point now);

    std::vector<int> steps_;
    size_t step_;
    GainControlTiming timing_;
    Clock::time_point started_;
    Clock::time_point window_start_;
    Clock::time_point settled_;
    uint64_t window_samples_;
    uint64_t window_clipped_;
    unsigned window_peak_ = 0;
    // The shares clipped in the last ten windows, newest last.
    std::array<double, 10> recent_{};
    size_t recent_count_ = 0;
    size_t recent_next_ = 0;
    // The hold before a step up: when it began, how many windows it has seen
    // and how many of them clipped, and the peaks of the others.
    Clock::time_point hold_since_;
    unsigned hold_windows_ = 0;
    unsigned hold_clipping_ = 0;
    std::array<unsigned, 129> hold_peaks_{};
    bool clipping_at_lowest_ = false;
    std::string reason_;
};

}  // namespace fern
