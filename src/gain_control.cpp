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
      window_samples_(samples),
      window_clipped_(clipped) {
    start_hold(settled_);
}

void GainControl::start_hold(Clock::time_point now) {
    hold_since_ = now;
    hold_windows_ = 0;
    hold_clipping_ = 0;
    hold_peaks_.fill(0);
}

void GainControl::change(size_t to, Clock::time_point now, std::string why) {
    reason_ = db_text(steps_[step_]) + " to " + db_text(steps_[to]) + ": " + why;
    step_ = to;
    settled_ = now + timing_.settle;
    recent_count_ = 0;
    recent_next_ = 0;
    start_hold(settled_);
}

size_t GainControl::update(Clock::time_point now, uint64_t samples, uint64_t clipped, unsigned peak) {
    if (steps_.size() < 2)
        return step_;
    window_peak_ = std::max(window_peak_, peak);
    if (now - window_start_ < timing_.window)
        return step_;
    const uint64_t seen = samples - window_samples_;
    const uint64_t cut = clipped - window_clipped_;
    const unsigned window_peak = std::min(window_peak_, 128u);
    window_start_ = now;
    window_samples_ = samples;
    window_clipped_ = clipped;
    window_peak_ = 0;
    // Samples still taken at the gain before the last change say nothing
    // about this one.
    if (now < settled_ || seen == 0)
        return step_;

    const double fraction = static_cast<double>(cut) / static_cast<double>(seen);
    recent_[recent_next_] = fraction;
    recent_next_ = (recent_next_ + 1) % recent_.size();
    recent_count_ = std::min(recent_count_ + 1, recent_.size());
    int clipping = 0;
    int heavy = 0;
    double worst = 0;
    for (size_t i = 0; i < recent_count_; ++i) {
        clipping += recent_[i] > clip_limit;
        heavy += recent_[i] > clip_heavy;
        worst = std::max(worst, recent_[i]);
    }
    if (clipping >= clip_windows) {
        size_t to = step_;
        if (heavy >= clip_windows) {
            // 6 dB down, and at least one step.
            while (to > 0 && steps_[step_] - steps_[to] < 60)
                --to;
        } else if (to > 0) {
            --to;
        }
        clipping_at_lowest_ = to == step_;
        if (to != step_) {
            char why[128];
            std::snprintf(why, sizeof why, "%d tenths of the last second clipped, up to %.2g%% of their samples",
                          clipping, worst * 100);
            change(to, now, why);
        } else {
            start_hold(now);
        }
        return step_;
    }
    clipping_at_lowest_ = false;

    ++hold_windows_;
    if (cut > 0)
        ++hold_clipping_;
    else
        ++hold_peaks_[window_peak];
    const auto hold = now - started_ < timing_.start_phase ? timing_.hold_start : timing_.hold;
    if (now - hold_since_ < hold)
        return step_;
    // The peaks of all but the highest twentieth of the windows that did not
    // clip, so that the odd crash does not stand for the band.
    const unsigned counted = hold_windows_ - hold_clipping_;
    unsigned peak_95 = 128;
    for (unsigned level = 0, below = 0; level <= 128; ++level) {
        below += hold_peaks_[level];
        if (below * 20 >= counted * 19) {
            peak_95 = level;
            break;
        }
    }
    const bool mostly_quiet = hold_clipping_ * 20 <= hold_windows_;
    if (mostly_quiet && counted > 0 && step_ + 1 < steps_.size()) {
        const double rise = std::pow(10.0, (steps_[step_ + 1] - steps_[step_]) / 200.0);
        if (peak_95 * rise < peak_limit) {
            const double peak_dbfs = 20 * std::log10(std::max(peak_95, 1u) / 128.0);
            char why[128];
            std::snprintf(why, sizeof why, "the band left room, its peaks at %.1f dBFS", peak_dbfs);
            change(step_ + 1, now, why);
            return step_;
        }
    }
    // Not this time: the next look covers the time from here, so that peaks
    // long past do not hold the gain down for good.
    start_hold(now);
    return step_;
}

}  // namespace fern
