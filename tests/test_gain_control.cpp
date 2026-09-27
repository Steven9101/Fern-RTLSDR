// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// gain = auto, fed looks at an imagined stream: 240000 samples a window, as
// at 2.4 Msps, with as many clipped and a peak as each test says.
#include <algorithm>
#include <chrono>
#include <cmath>
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

// A stream whose level follows the gain, as a real one does: its peaks at
// 29.7 dB given, and every `every` a window with `clipped` samples of static
// that reach full scale.
size_t feed_band(GainControl& control, Stream& s, milliseconds duration, double peak_at_29_7, milliseconds every,
                 uint64_t clipped = 2400) {
    milliseconds since{0};
    for (milliseconds t{0}; t < duration; t += milliseconds(100)) {
        s.now += milliseconds(100);
        s.samples += 240000;
        since += milliseconds(100);
        const double gain = r820t[control.step()] / 10.0;
        unsigned peak = static_cast<unsigned>(std::min(128.0, peak_at_29_7 * std::pow(10.0, (gain - 29.7) / 20)));
        if (every.count() > 0 && since >= every) {
            since = milliseconds(0);
            s.clipped += clipped;
            peak = 128;
        }
        control.update(s.now, s.samples, s.clipped, peak);
    }
    return control.step();
}

}  // namespace

TEST(gain_control_comes_down_when_the_converter_keeps_clipping) {
    Stream s;
    GainControl control(r820t, start_step, s.now, s.samples, s.clipped, timing());
    // Nothing is judged while the samples in flight are the old gain's.
    CHECK_EQ(feed(control, s, milliseconds(400), 50000, 128), start_step);
    // A little, in three tenths of a second: one step.
    CHECK_EQ(feed(control, s, milliseconds(200), 100, 128), start_step);
    CHECK_EQ(feed(control, s, milliseconds(100), 100, 128), start_step - 1);
    CHECK_HAS(control.reason(), "29.7 dB to 28.0 dB: 3 tenths of the last second clipped");
    // And then nothing while this one settles, whatever arrives.
    CHECK_EQ(feed(control, s, milliseconds(400), 50000, 128), start_step - 1);
    // A lot: 6 dB at once, from 28.0 to 20.7.
    CHECK_EQ(feed(control, s, milliseconds(300), 50000, 128), 12u);
    CHECK_HAS(control.reason(), "28.0 dB to 20.7 dB: 3 tenths of the last second clipped, up to 21% of their samples");
    CHECK_EQ(control.gain_db(), 20.7);
}

TEST(gain_control_comes_down_for_speech_that_clips_on_its_peaks) {
    Stream s;
    GainControl control(r820t, start_step, s.now, s.samples, s.clipped, timing());
    // Four tenths of every second clip, the syllables of a strong voice.
    for (int second = 0; second < 3; ++second)
        for (int tenth = 0; tenth < 10; ++tenth)
            feed(control, s, milliseconds(100), tenth % 5 < 2 ? 500 : 0, tenth % 5 < 2 ? 128 : 60);
    CHECK(control.step() < start_step);
}

TEST(gain_control_goes_up_only_with_room_to_spare) {
    Stream s;
    GainControl control(r820t, start_step, s.now, s.samples, s.clipped, timing());
    // Quiet, peaks at a quarter of full scale: up after 5 s in the first
    // minutes, the next step being 3.1 dB higher.
    CHECK_EQ(feed(control, s, milliseconds(5400), 0, 32), start_step);
    CHECK_EQ(feed(control, s, milliseconds(200), 0, 32), start_step + 1);
    CHECK_HAS(control.reason(), "29.7 dB to 32.8 dB: the band left room, its peaks at -12.0 dBFS");
    // Peaks at half of full scale leave no room for another step.
    CHECK_EQ(feed(control, s, seconds(20), 0, 64), start_step + 1);
    // Peaks long past do not hold the gain down for good.
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

TEST(gain_control_is_not_pushed_down_by_crashes_of_static) {
    Stream s;
    GainControl control(r820t, start_step, s.now, s.samples, s.clipped, timing());
    // Peaks at 20 of 128 at 29.7 dB leave room up to 38.6 dB, where the next
    // step would put them over half of full scale. A crash every 20 s that
    // clips for a millisecond does not change that.
    size_t lowest = control.step();
    for (int minute = 0; minute < 30; ++minute) {
        feed_band(control, s, seconds(60), 20, milliseconds(20000));
        lowest = std::min(lowest, control.step());
    }
    CHECK_EQ(lowest, start_step);
    CHECK_EQ(control.gain_db(), 38.6);
    // Nor are a few crashes in a row, as a flash of lightning brings.
    Stream t;
    GainControl lightning(r820t, start_step, t.now, t.samples, t.clipped, timing());
    feed_band(lightning, t, seconds(10), 20, milliseconds(0));
    feed_band(lightning, t, milliseconds(200), 20, milliseconds(100), 24000);
    CHECK(lightning.step() >= start_step);
}

TEST(gain_control_stays_down_while_crashes_keep_coming) {
    Stream s;
    GainControl control(r820t, start_step, s.now, s.samples, s.clipped, timing());
    // A clipped tenth every second, a tenth of the time: no step up, though
    // the peaks between would allow one.
    CHECK_EQ(feed_band(control, s, seconds(30), 20, milliseconds(1000), 10), start_step);
}

TEST(gain_control_stays_within_the_tuner_steps) {
    Stream s;
    GainControl low(r820t, 0, s.now, s.samples, s.clipped, timing());
    CHECK_EQ(feed(low, s, seconds(10), 100000, 128), 0u);
    // At the lowest step only an attenuator helps, which it says.
    CHECK(low.clipping_at_lowest());
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
    // A tuner with one step has nothing to control.
    Stream u;
    GainControl single({0}, 0, u.now, u.samples, u.clipped, timing());
    CHECK_EQ(feed(single, u, seconds(10), 100000, 128), 0u);
    CHECK(!single.clipping_at_lowest());
}
