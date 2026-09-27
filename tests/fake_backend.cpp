// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "fake_backend.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <thread>

namespace fake {

uint8_t tone_byte(uint64_t i) {
    static const std::array<uint8_t, 32> table = [] {
        std::array<uint8_t, 32> t{};
        for (int n = 0; n < 16; ++n) {
            const double phase = 2 * 3.14159265358979323846 * n / 16;
            t[2 * n] = static_cast<uint8_t>(std::lround(127.5 + 100 * std::cos(phase)));
            t[2 * n + 1] = static_cast<uint8_t>(std::lround(127.5 + 100 * std::sin(phase)));
        }
        return t;
    }();
    return table[i % table.size()];
}

namespace {

// librtlsdr's gain tables, tenths of a dB.
std::vector<int> gains_of(fern::Tuner t) {
    switch (t) {
    case fern::Tuner::e4000: return {-10, 15, 40, 65, 90, 115, 140, 165, 190, 215, 240, 290, 340, 420};
    case fern::Tuner::fc0012: return {-99, -40, 71, 179, 192};
    case fern::Tuner::fc0013:
        return {-99, -73, -65, -63, -60, -58, -54, 58, 61, 63, 65, 67, 68, 70, 71, 179, 181, 182, 184, 186, 188, 191,
                197};
    case fern::Tuner::r820t:
    case fern::Tuner::r828d:
        return {0,   9,   14,  27,  37,  77,  87,  125, 144, 157, 166, 197, 207, 229, 254,
                280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496};
    case fern::Tuner::fc2580:
    case fern::Tuner::unknown: break;
    }
    return {0};
}

}  // namespace

class Device : public fern::Device {
public:
    Device(Backend& owner, const Spec& spec, std::shared_ptr<State> state)
        : owner_(owner), spec_(spec), state_(std::move(state)) {
        std::lock_guard<std::mutex> lock(owner_.mutex_);
        owner_.live_.insert(this);
        // Like rtlsdr_open(): the EEPROM flag forces the bias tee on.
        forced_bias_ = spec_.eeprom_byte7 >= 0 && (spec_.eeprom_byte7 & 0x02) == 0;
        state_->bias_tee = forced_bias_;
    }

    ~Device() override {
        {
            std::lock_guard<std::mutex> lock(owner_.mutex_);
            owner_.live_.erase(this);
        }
        // A slow close may outlive the backend, so only state_ is used from
        // here on.
        if (spec_.close_delay_ms)
            std::this_thread::sleep_for(std::chrono::milliseconds(spec_.close_delay_ms));
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->closed = true;
        state_->calls.push_back("close");
    }

    fern::Tuner tuner() override { return spec_.tuner; }
    std::vector<int> tuner_gains() override { return gains_of(spec_.tuner); }

    int usb_strings(fern::UsbStrings& out) override {
        out = fern::UsbStrings{spec_.manufacturer, spec_.product, spec_.serial};
        return 0;
    }

    int read_eeprom(uint8_t* data, uint8_t offset, uint16_t len) override {
        if (spec_.eeprom_byte7 < 0)
            return -3;
        const uint8_t image[8] = {0x28, 0x32, 0xda, 0x0b, 0x38, 0x28, 0xa5, static_cast<uint8_t>(spec_.eeprom_byte7)};
        for (uint16_t i = 0; i < len; ++i)
            data[i] = offset + i < 8 ? image[offset + i] : 0xff;
        return 1;
    }

    int xtal_freq(uint32_t& rtl_hz, uint32_t& tuner_hz) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const double scale = 1.0 + state_->ppm / 1e6;
        rtl_hz = static_cast<uint32_t>(28800000 * scale);
        // librtlsdr gives an R828D a 16 MHz crystal, but for the RTL-SDR
        // Blog V4's.
        const bool v4 = spec_.manufacturer == "RTLSDRBlog" && spec_.product == "Blog V4";
        tuner_hz = static_cast<uint32_t>((spec_.tuner == fern::Tuner::r828d && !v4 ? 16000000 : 28800000) * scale);
        return 0;
    }

    int set_freq_correction(int ppm) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->ppm == ppm)
            return -2;
        state_->ppm = ppm;
        state_->calls.push_back("set_freq_correction " + std::to_string(ppm));
        return 0;
    }

    int set_direct_sampling(int mode) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->direct_sampling_mode = mode;
        state_->direct_sampling = mode;
        state_->calls.push_back("set_direct_sampling " + std::to_string(mode));
        return 0;
    }

    int direct_sampling() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->direct_sampling;
    }

    int set_offset_tuning(bool on) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->calls.push_back(std::string("set_offset_tuning ") + (on ? "1" : "0"));
        if (spec_.tuner == fern::Tuner::r820t || spec_.tuner == fern::Tuner::r828d) {
            // librtlsdr's RTL-SDR Blog hack: this switches the bias tee.
            state_->bias_tee = on || forced_bias_;
            return -2;
        }
        if (state_->direct_sampling)
            return -3;
        state_->offset_tuning = on;
        return 0;
    }

    int offset_tuning() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->offset_tuning ? 1 : 0;
    }

    int set_center_freq(uint32_t hz) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->calls.push_back("set_center_freq " + std::to_string(hz));
        // The RTL-SDR Blog library switches R820T dongles to the Q branch
        // below 24 MHz unless a direct sampling mode was chosen.
        if (state_->direct_sampling_mode == 0)
            state_->direct_sampling =
                hz < 24000000 && spec_.tuner == fern::Tuner::r820t && spec_.product != "Blog V4L" ? 2 : 0;
        if (state_->direct_sampling) {
            state_->center = hz;
            return 0;
        }
        if (!in_range(hz)) {
            state_->center = 0;
            return -1;
        }
        state_->center = hz;
        return 0;
    }

    uint32_t center_freq() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->center;
    }

    int set_sample_rate(uint32_t hz) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->calls.push_back("set_sample_rate " + std::to_string(hz));
        if (hz <= 225000 || hz > 3200000 || (hz > 300000 && hz <= 900000))
            return -EINVAL;
        const double two_pow_22 = 4194304.0;
        uint32_t ratio = static_cast<uint32_t>((28800000 * two_pow_22) / hz);
        ratio &= 0x0ffffffc;
        const uint32_t real_ratio = ratio | ((ratio & 0x08000000) << 1);
        state_->sample_rate = static_cast<uint32_t>((28800000 * two_pow_22) / real_ratio);
        return 0;
    }

    uint32_t sample_rate() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->sample_rate;
    }

    int set_tuner_bandwidth(uint32_t hz) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->calls.push_back("set_tuner_bandwidth " + std::to_string(hz));
        state_->bandwidth = hz;
        return 0;
    }

    int set_tuner_gain_mode(bool manual) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->calls.push_back(std::string("set_tuner_gain_mode ") + (manual ? "1" : "0"));
        state_->manual_gain = manual;
        state_->gain = 0;
        return 0;
    }

    int set_tuner_gain(int tenth_db) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->calls.push_back("set_tuner_gain " + std::to_string(tenth_db));
        if (spec_.fail_gain != 0)
            return spec_.fail_gain;
        state_->gain = tenth_db;
        return 0;
    }

    int set_agc_mode(bool on) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->calls.push_back(std::string("set_agc_mode ") + (on ? "1" : "0"));
        state_->agc = on;
        return 0;
    }

    int set_bias_tee(bool on) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->calls.push_back(std::string("set_bias_tee ") + (on ? "1" : "0"));
        state_->bias_tee = on || forced_bias_;
        return 0;
    }

    int reset_buffer() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->calls.push_back("reset_buffer");
        ++state_->reset_buffer_calls;
        return 0;
    }

    int set_if_register(uint32_t value) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->if_register = value;
        state_->calls.push_back("set_if_register " + std::to_string(value));
        return 0;
    }

    int read_async(fern::SampleCallback cb, void* ctx, uint32_t buf_num, uint32_t buf_len) override {
        uint32_t rate;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->buf_num = buf_num;
            state_->buf_len = buf_len;
            rate = state_->sample_rate;
        }
        {
            std::lock_guard<std::mutex> lock(async_mutex_);
            if (running_)
                return -2;
            if (halted_)
                return -1;
            running_ = true;
            cancel_ = false;
        }
        if (buf_len == 0 || buf_len % 512 != 0)
            buf_len = 262144;
        std::vector<unsigned char> buf(buf_len);
        const auto block = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(buf_len / (2.0 * (rate ? rate : 2400000))));
        auto next = std::chrono::steady_clock::now();

        std::unique_lock<std::mutex> lock(async_mutex_);
        while (!cancel_ && !spec_.fail_start) {
            if (spec_.unplug_after && produced_ >= spec_.unplug_after)
                break;
            if (spec_.stall_after && produced_ >= spec_.stall_after) {
                cv_.wait(lock, [&] { return cancel_; });
                break;
            }
            if (spec_.realtime) {
                cv_.wait_until(lock, next, [&] { return cancel_; });
                if (cancel_)
                    break;
                next += block;
            }
            lock.unlock();
            if (spec_.tone_at_0_db > 0) {
                double gain_db;
                {
                    std::lock_guard<std::mutex> state_lock(state_->mutex);
                    gain_db = state_->manual_gain ? state_->gain / 10.0 : 29.7;
                }
                const double amplitude = spec_.tone_at_0_db * std::pow(10.0, gain_db / 20);
                for (uint32_t i = 0; i < buf_len; ++i) {
                    const uint64_t n = produced_ + i;
                    const double phase = 2 * 3.14159265358979323846 * static_cast<double>(n / 2 % 16) / 16;
                    const double v = 127.5 + amplitude * (n % 2 == 0 ? std::cos(phase) : std::sin(phase));
                    buf[i] = static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
                }
            } else {
                for (uint32_t i = 0; i < buf_len; ++i)
                    buf[i] = tone_byte(produced_ + i);
            }
            cb(buf.data(), buf_len, ctx);
            produced_ += buf_len;
            lock.lock();
        }
        running_ = false;
        cv_.notify_all();
        return 0;
    }

    // Ends a stream the module left running, so that a test that leaks a
    // reader thread fails instead of hanging.
    void halt() {
        std::unique_lock<std::mutex> lock(async_mutex_);
        halted_ = true;
        cancel_ = true;
        cv_.notify_all();
        cv_.wait(lock, [&] { return !running_; });
    }

    int cancel_async() override {
        std::lock_guard<std::mutex> lock(async_mutex_);
        if (!running_ || cancel_)
            return -2;
        cancel_ = true;
        cv_.notify_all();
        return 0;
    }

    int pll_locked() override {
        const bool r82xx = spec_.tuner == fern::Tuner::r820t || spec_.tuner == fern::Tuner::r828d;
        if (!r82xx || direct_sampling() != 0)
            return -1;
        return spec_.pll_lock ? 1 : 0;
    }

private:
    bool in_range(uint32_t hz) const {
        const bool blog = spec_.manufacturer == "RTLSDRBlog";
        switch (spec_.tuner) {
        case fern::Tuner::r820t:
            return hz >= (blog && spec_.product == "Blog V4L" ? 500000u : 24000000u) && hz <= 1766000000u;
        case fern::Tuner::r828d:
            return hz >= (blog && spec_.product == "Blog V4" ? 500000u : 24000000u) && hz <= 1766000000u;
        case fern::Tuner::e4000: return hz >= 52000000u && hz <= 2200000000u;
        case fern::Tuner::fc0012: return hz >= 22000000u && hz <= 948600000u;
        case fern::Tuner::fc0013: return hz >= 22000000u && hz <= 1100000000u;
        case fern::Tuner::fc2580: return hz >= 146000000u && hz <= 924000000u;
        case fern::Tuner::unknown: break;
        }
        return false;
    }

    Backend& owner_;
    Spec spec_;
    std::shared_ptr<State> state_;
    bool forced_bias_ = false;

    std::mutex async_mutex_;
    std::condition_variable cv_;
    bool running_ = false;
    bool cancel_ = false;
    bool halted_ = false;
    uint64_t produced_ = 0;
};

Backend::~Backend() {
    std::set<Device*> left;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        left = live_;
    }
    for (Device* d : left) {
        d->halt();
        delete d;
    }
}

fern::Enumeration Backend::enumerate() {
    fern::Enumeration e;
    e.error = enumeration_error;
    e.diagnostic = enumeration_diagnostic;
    e.count = enumeration_error ? 0 : static_cast<uint32_t>(devices.size());
    return e;
}

int Backend::usb_strings(uint32_t index, fern::UsbStrings& out) {
    if (index >= devices.size())
        return fern::usb_error::no_device;
    const Spec& s = devices[index];
    if (s.strings_error)
        return s.strings_error;
    out = fern::UsbStrings{s.manufacturer, s.product, s.serial};
    return 0;
}

std::string Backend::device_name(uint32_t index) {
    return index < devices.size() ? "Generic RTL2832U OEM" : "";
}

int Backend::open(uint32_t index, std::unique_ptr<fern::Device>& out) {
    if (index >= devices.size())
        return -1;
    const Spec spec = devices[index];
    if (spec.open_delay_ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(spec.open_delay_ms));
    if (spec.open_error)
        return spec.open_error;
    auto state = std::make_shared<State>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        states_.push_back(state);
    }
    out = std::make_unique<Device>(*this, spec, state);
    return 0;
}

std::shared_ptr<State> Backend::last() {
    std::lock_guard<std::mutex> lock(mutex_);
    return states_.empty() ? nullptr : states_.back();
}

int Backend::opens() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(states_.size());
}

}  // namespace fake
