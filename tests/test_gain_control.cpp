// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// gain = auto, fed looks at an imagined stream: 240000 samples a window, as
// at 2.4 Msps, with as many clipped and a peak as each test says.
#include <chrono>
#include <cstdint>
#include <vector>

#include "gain_control.h"
#include "test.h"

using fern::GainControl;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {

// The R820T's gain steps, in tenths of a dB.
const std::vector<int> r820t = {0,   9,   14,  27,  37,  77,  87,  125, 144, 157, 166, 197, 207, 229, 254,
                                280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496};
constexpr size_t start_step = 16;  // 29.7 dB

struct Stream {
    GainControl::Clock::time_point now = GainControl::Clock::time_point() + seconds(1000);
    uint64_t samples = 0;
    uint64_t clipped = 0;
};

fern::GainControlTiming timing() {
    fern::GainControlTiming t;
    t.settle = milliseconds(500);
    return t;
}

// Feeds `duration` of windows of 100 ms, each with `clipped_per_window` of
// its samples clipped and `peak`, and returns the step at the end.
size_t feed(GainControl& control, Stream& s, milliseconds duration, uint64_t clipped_per_window, unsigned peak) {
    size_t step = control.step();
    for (milliseconds t{0}; t < duration; t += milliseconds(100)) {
        s.now += milliseconds(100);
        s.samples += 240000;
        s.clipped += clipped_per_window;
        step = control.update(s.now, s.samples, s.clipped, peak);
    }
    return step;
}

}  // namespace

TEST(gain_control_comes_down_at_once_when_the_converter_clips) {
    Stream s;
    GainControl control(r820t, start_step, s.now, s.samples, s.clipped, timing());
    // Nothing is judged while the samples in flight are the old gain's.
    CHECK_EQ(feed(control, s, milliseconds(400), 50000, 128), start_step);
    // A little clipping: one step.
    CHECK_EQ(feed(control, s, milliseconds(200), 100, 128), start_step - 1);
    CHECK_HAS(control.reason(), "29.7 dB to 28.0 dB");
    // And then nothing while this one settles, whatever arrives.
    CHECK_EQ(feed(control, s, milliseconds(300), 50000, 128), start_step - 1);
    // A lot: 6 dB at once, from 28.0 to 20.7.
    CHECK_EQ(feed(control, s, milliseconds(200), 50000, 128), 12u);
    CHECK_HAS(control.reason(), "28.0 dB to 20.7 dB: 21% of the samples clipped");
    CHECK_EQ(control.gain_db(), 20.7);
}

TEST(gain_control_goes_up_only_with_room_to_spare) {
    Stream s;
    GainControl control(r820t, start_step, s.now, s.samples, s.clipped, timing());
    // Quiet, peaks at a quarter of full scale: up after 5 s in the first
    // minutes, the next step being 3.1 dB higher.
    CHECK_EQ(feed(control, s, milliseconds(5400), 0, 32), start_step);
    CHECK_EQ(feed(control, s, milliseconds(200), 0, 32), start_step + 1);
    CHECK_HAS(control.reason(), "29.7 dB to 32.8 dB: nothing clipped, and the peaks were at -12.0 dBFS");
    // Peaks at half of full scale leave no room for another step.
    CHECK_EQ(feed(control, s, seconds(20), 0, 64), start_step + 1);
    // A peak long past does not hold the gain down for good.
    CHECK_EQ(feed(control, s, milliseconds(5600), 0, 30), start_step + 2);
}

TEST(gain_control_waits_a_minute_after_the_first_two) {
    Stream s;
    GainControl control(r820t, start_step, s.now, s.samples, s.clipped, timing());
    // Peaks too high for a step up through the first two minutes, which
    // looked every 5 s.
    CHECK_EQ(feed(control, s, seconds(130), 0, 100), start_step);
    // From then on it looks every minute: the look 45 s from now still sees
    // the peaks before, the one a minute after that sees only the new ones.
    CHECK_EQ(feed(control, s, seconds(100), 0, 20), start_step);
    CHECK_EQ(feed(control, s, seconds(10), 0, 20), start_step + 1);
}

TEST(gain_control_waits_out_a_crash_of_static) {
    Stream s;
    GainControl control(r820t, start_step, s.now, s.samples, s.clipped, timing());
    // One clipped sample in 240000 is not worth a step down, but it starts
    // the wait for a step up afresh.
    for (int i = 0; i < 20; ++i) {
        CHECK_EQ(feed(control, s, milliseconds(3000), 0, 32), start_step);
        CHECK_EQ(feed(control, s, milliseconds(100), 1, 128), start_step);
    }
    CHECK_EQ(feed(control, s, milliseconds(5800), 0, 32), start_step + 1);
}

TEST(gain_control_stays_within_the_tuner_steps) {
    Stream s;
    GainControl low(r820t, 0, s.now, s.samples, s.clipped, timing());
    CHECK_EQ(feed(low, s, seconds(10), 100000, 128), 0u);
    GainControl high(r820t, r820t.size() - 1, s.now, s.samples, s.clipped, timing());
    CHECK_EQ(feed(high, s, seconds(20), 0, 1), r820t.size() - 1);
    // Looks less than a window apart add up to one.
    Stream t;
    GainControl control(r820t, start_step, t.now, t.samples, t.clipped, timing());
    for (int i = 0; i < 90; ++i) {
        t.now += milliseconds(10);
        t.samples += 24000;
        t.clipped += 10;
        control.update(t.now, t.samples, t.clipped, 128);
    }
    CHECK_EQ(control.step(), start_step - 1);
}
