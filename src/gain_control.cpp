// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gain_control.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fern {

namespace {

std::string db_text(int tenths) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f dB", tenths / 10.0);
    return text;
}

}  // namespace

GainControl::GainControl(std::vector<int> steps, size_t start, Clock::time_point now, uint64_t samples,
                         uint64_t clipped, GainControlTiming timing)
    : steps_(std::move(steps)),
      step_(std::min(start, steps_.empty() ? size_t{0} : steps_.size() - 1)),
      timing_(timing),
      started_(now),
      window_start_(now),
      settled_(now + timing.settle),
      quiet_since_(now + timing.settle),
      window_samples_(samples),
      window_clipped_(clipped) {}

void GainControl::change(size_t to, Clock::time_point now, std::string why) {
    reason_ = db_text(steps_[step_]) + " to " + db_text(steps_[to]) + ": " + why;
    step_ = to;
    settled_ = now + timing_.settle;
    quiet_since_ = settled_;
    quiet_peak_ = 0;
}

size_t GainControl::update(Clock::time_point now, uint64_t samples, uint64_t clipped, unsigned peak) {
    if (steps_.empty())
        return step_;
    window_peak_ = std::max(window_peak_, peak);
    if (now - window_start_ < timing_.window)
        return step_;
    const uint64_t seen = samples - window_samples_;
    const uint64_t cut = clipped - window_clipped_;
    const unsigned window_peak = window_peak_;
    window_start_ = now;
    window_samples_ = samples;
    window_clipped_ = clipped;
    window_peak_ = 0;
    // Samples still taken at the gain before the last change say nothing
    // about this one.
    if (now < settled_ || seen == 0)
        return step_;

    const double fraction = static_cast<double>(cut) / static_cast<double>(seen);
    if (fraction > clip_limit) {
        size_t to = step_;
        if (fraction > clip_heavy) {
            // 6 dB down, and at least one step.
            while (to > 0 && steps_[step_] - steps_[to] < 60)
                --to;
        } else if (to > 0) {
            --to;
        }
        char why[96];
        std::snprintf(why, sizeof why, "%.2g%% of the samples clipped", fraction * 100);
        if (to != step_)
            change(to, now, why);
        else
            quiet_since_ = now;
        return step_;
    }
    if (cut > 0) {
        // A crash of static or a spark: nothing to come down for, but no
        // reason to go up either.
        quiet_since_ = now;
        quiet_peak_ = 0;
        return step_;
    }

    quiet_peak_ = std::max(quiet_peak_, window_peak);
    const auto hold = now - started_ < timing_.start_phase ? timing_.hold_start : timing_.hold;
    if (now - quiet_since_ < hold)
        return step_;
    if (step_ + 1 < steps_.size()) {
        const double rise = std::pow(10.0, (steps_[step_ + 1] - steps_[step_]) / 200.0);
        if (quiet_peak_ * rise < peak_limit) {
            const double peak_dbfs = 20 * std::log10(std::max(quiet_peak_, 1u) / 128.0);
            char why[96];
            std::snprintf(why, sizeof why, "nothing clipped, and the peaks were at %.1f dBFS", peak_dbfs);
            change(step_ + 1, now, why);
            return step_;
        }
    }
    // Not this time: the next look covers the time from here, so that a peak
    // long past does not hold the gain down for good.
    quiet_since_ = now;
    quiet_peak_ = 0;
    return step_;
}

}  // namespace fern
