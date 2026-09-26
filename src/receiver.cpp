// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "receiver.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <system_error>
#include <thread>

#include "log.h"

namespace fern {

const char* usb_error_text(int code) {
    switch (code) {
    case -1: return "input/output error";
    case -2: return "invalid parameter";
    case -3: return "permission denied";
    case -4: return "no such device";
    case -5: return "not found";
    case -6: return "busy";
    case -7: return "timeout";
    case -8: return "overflow";
    case -9: return "pipe error";
    case -10: return "interrupted";
    case -11: return "out of memory";
    case -12: return "not supported";
    case usb_error::kernel_driver: return "kernel driver could not be detached";
    default: return "error";
    }
}

double achieved_sample_rate(uint32_t xtal_hz, uint32_t requested_hz) {
    const double two_pow_22 = 4194304.0;
    uint32_t ratio = static_cast<uint32_t>((xtal_hz * two_pow_22) / requested_hz);
    ratio &= 0x0ffffffc;
    const uint32_t real_ratio = ratio | ((ratio & 0x08000000) << 1);
    return (xtal_hz * two_pow_22) / real_ratio;
}

int nearest_gain(const std::vector<int>& tenths, double db) {
    const double want = db * 10.0;
    int best = -1;
    double best_distance = 0;
    for (size_t i = 0; i < tenths.size(); ++i) {
        const double distance = std::fabs(tenths[i] - want);
        const bool tie = best >= 0 && std::fabs(distance - best_distance) < 1e-6;
        if (best < 0 || (!tie && distance < best_distance) || (tie && tenths[i] < tenths[best])) {
            best = static_cast<int>(i);
            best_distance = distance;
        }
    }
    return best;
}

namespace {

uint32_t r82xx_filter(uint32_t bw) {
    static const uint32_t low_pass[] = {1700000, 1600000, 1550000, 1450000, 1200000,
                                        900000,  700000,  550000,  450000,  350000};
    constexpr uint32_t high_pass_1 = 350000;
    constexpr uint32_t high_pass_2 = 380000;
    if (bw > 7000000)
        return 8000000;
    if (bw > 6000000)
        return 7000000;
    if (bw > low_pass[0] + high_pass_1 + high_pass_2)
        return 6000000;
    uint32_t real = 0;
    if (bw > low_pass[0] + high_pass_1) {
        bw -= high_pass_2;
        real += high_pass_2;
    }
    if (bw > low_pass[0]) {
        bw -= high_pass_1;
        real += high_pass_1;
    }
    // bw is now at most low_pass[0]: take the narrowest filter not below it.
    size_t i = 1;
    while (i < sizeof low_pass / sizeof low_pass[0] && bw <= low_pass[i])
        ++i;
    return real + low_pass[i - 1];
}

uint32_t closest(const uint32_t* table, size_t n, uint32_t hz) {
    uint32_t best = table[0];
    uint32_t best_delta = UINT32_MAX;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t delta = hz > table[i] ? hz - table[i] : table[i] - hz;
        if (delta < best_delta) {
            best_delta = delta;
            best = table[i];
        }
    }
    return best;
}

uint32_t e4000_filter(uint32_t bw) {
    static const uint32_t mixer[] = {27000000, 4600000, 4200000, 3800000, 3400000, 3300000, 2700000, 2300000,
                                     1900000};
    static const uint32_t rc[] = {21400000, 21000000, 17600000, 14700000, 12400000, 10600000, 9000000, 7700000,
                                  6400000,  5300000,  4400000,  3400000,  2600000,  1800000,  1200000, 1000000};
    static const uint32_t channel[] = {5500000, 5300000, 5000000, 4800000, 4600000, 4400000, 4300000, 4100000,
                                       3900000, 3800000, 3700000, 3600000, 3400000, 3300000, 3200000, 3100000,
                                       3000000, 2950000, 2900000, 2800000, 2750000, 2700000, 2600000, 2550000,
                                       2500000, 2450000, 2400000, 2300000, 2280000, 2240000, 2200000, 2150000};
    return std::min({closest(mixer, sizeof mixer / sizeof mixer[0], bw), closest(rc, sizeof rc / sizeof rc[0], bw),
                     closest(channel, sizeof channel / sizeof channel[0], bw)});
}

}  // namespace

uint32_t tuner_filter_bandwidth(Tuner tuner, uint32_t requested_hz) {
    switch (tuner) {
    case Tuner::r820t:
    case Tuner::r828d: return r82xx_filter(requested_hz);
    case Tuner::e4000: return e4000_filter(requested_hz);
    case Tuner::fc0012:
    case Tuner::fc0013: return 6000000;
    case Tuner::fc2580: return 1530000;
    case Tuner::unknown: break;
    }
    return 0;
}

uint32_t transfer_bytes_for_rate(uint32_t sample_rate) {
    constexpr uint64_t unit = 16384;
    uint64_t bytes = static_cast<uint64_t>(sample_rate) * 2 / 50;
    bytes = (bytes + unit - 1) / unit * unit;
    return static_cast<uint32_t>(std::clamp<uint64_t>(bytes, unit, 16 * unit));
}

std::string display_name(const UsbStrings& s) {
    if (s.manufacturer == "RTLSDRBlog" && s.product.rfind("Blog ", 0) == 0)
        return "RTL-SDR " + s.product;
    std::string name = s.manufacturer;
    if (!s.product.empty())
        name += (name.empty() ? "" : " ") + s.product;
    return name;
}

namespace {

Failure invalid(std::string m) { return Failure{ErrorCode::invalid, std::move(m)}; }
Failure usb_failure(const std::string& what, int code) {
    return Failure{ErrorCode::usb, what + " failed (" + usb_error_text(code) + "); check the USB connection"};
}

std::string db_text(double db) { return json::serialize(json::Value(std::round(db * 10) / 10)); }

std::string range_hint(const DeviceInfo& info) {
    switch (info.tuner) {
    case Tuner::r820t: return "; the R820T covers about 24 to 1766 MHz";
    case Tuner::r828d:
        return info.blog_v4 ? "; the RTL-SDR Blog V4 covers about 0.5 to 1766 MHz"
                            : "; the R828D covers about 24 to 1766 MHz";
    case Tuner::e4000: return "; the E4000 covers about 52 to 2200 MHz, with a gap near 1100 to 1250 MHz";
    case Tuner::fc0012: return "; the FC0012 covers about 22 to 948 MHz";
    case Tuner::fc0013: return "; the FC0013 covers about 22 to 1100 MHz";
    case Tuner::fc2580: return "; the FC2580 covers about 146 to 308 and 438 to 924 MHz";
    case Tuner::unknown: break;
    }
    return "";
}

}  // namespace

Receiver::~Receiver() { close(); }

std::optional<Failure> Receiver::select(const DeviceSelector& selector, uint32_t& index) {
    const Enumeration e = backend_.enumerate();
    if (e.error != 0)
        return Failure{ErrorCode::usb, e.diagnostic};

    const std::string check_cable =
        "Check the USB cable; lsusb should list the dongle as 0bda:2838 or 0bda:2832.";
    if (e.count == 0) {
        if (selector.kind == DeviceSelector::Kind::serial)
            return Failure{ErrorCode::no_device,
                           "no RTL-SDR with serial " + selector.serial + " is plugged in, nor any other. " + check_cable};
        return Failure{ErrorCode::no_device, "no RTL-SDR is plugged in. " + check_cable};
    }
    if (selector.kind == DeviceSelector::Kind::only && e.count == 1) {
        index = 0;
        return std::nullopt;
    }

    // Everything below needs to say which devices there are.
    std::vector<std::string> serials(e.count);
    std::vector<int> errors(e.count, 0);
    std::string inventory;
    bool unreadable = false;
    for (uint32_t i = 0; i < e.count; ++i) {
        UsbStrings s;
        errors[i] = backend_.usb_strings(i, s);
        serials[i] = s.serial;
        if (i < 8) {
            inventory += (i > 0 ? ", " : "") + std::string("index ") + std::to_string(i) + " ";
            if (errors[i] == 0) {
                inventory += "serial " + (s.serial.empty() ? std::string("(none)") : s.serial);
            } else {
                inventory += std::string("serial unreadable (") + usb_error_text(errors[i]) + ")";
                unreadable = true;
            }
        }
    }
    if (e.count > 8)
        inventory += " and " + std::to_string(e.count - 8) + " more";
    const std::string permissions =
        unreadable ? ". An unreadable serial usually means missing permissions; see the udev rule in the README" : "";

    switch (selector.kind) {
    case DeviceSelector::Kind::only:
        return invalid(std::to_string(e.count) + " RTL-SDRs are plugged in (" + inventory +
                       "); set module.device = serial:<serial> to choose one" + permissions);
    case DeviceSelector::Kind::index:
        if (selector.index >= e.count)
            return Failure{ErrorCode::no_device, "there is no RTL-SDR at index " + std::to_string(selector.index) +
                                                     "; plugged in: " + inventory + permissions};
        index = selector.index;
        return std::nullopt;
    case DeviceSelector::Kind::serial: {
        std::vector<uint32_t> matches;
        for (uint32_t i = 0; i < e.count; ++i)
            if (errors[i] == 0 && serials[i] == selector.serial)
                matches.push_back(i);
        if (matches.empty())
            return Failure{ErrorCode::no_device, "no RTL-SDR with serial " + selector.serial +
                                                     " is plugged in; plugged in: " + inventory + permissions};
        if (matches.size() > 1) {
            std::string which;
            for (size_t k = 0; k < matches.size(); ++k)
                which += (k > 0 ? ", " : "") + std::to_string(matches[k]);
            return invalid(std::to_string(matches.size()) + " RTL-SDRs have the serial " + selector.serial +
                           " (indexes " + which +
                           "); give each its own serial with rtl_eeprom -s, or choose one with "
                           "module.device = index:<n>");
        }
        index = matches[0];
        return std::nullopt;
    }
    }
    return Failure{ErrorCode::internal, "unhandled device selector"};
}

std::optional<Failure> Receiver::open(const OpenRequest& request) {
    close();
    const DeviceSelector& selector = request.settings.device;
    uint32_t index = 0;
    if (auto f = select(selector, index))
        return f;

    std::unique_ptr<Device> device;
    const int r = backend_.open(index, device);
    const std::string which = "the RTL-SDR at index " + std::to_string(index);
    if (r == 0 && !device)
        return Failure{ErrorCode::internal, "opening " + which + " returned no device"};
    switch (r) {
    case 0:
        break;
    case usb_error::busy:
        return Failure{ErrorCode::busy, which +
                                            " is in use by another program, such as rtl_tcp or another FernSDR "
                                            "band. Stop that program, or choose another dongle with module.device"};
    case usb_error::kernel_driver:
        return Failure{ErrorCode::busy, "the dvb_usb_rtl28xxu kernel driver holds " + which +
                                            " and could not be detached. Blacklist the driver as the README "
                                            "describes, then unplug and replug the dongle"};
    case usb_error::access:
        return Failure{ErrorCode::usb, "no permission to open " + which +
                                           ". Run FernSDR's install.sh --service --usb, which lets the receiver's "
                                           "own user open RTL-SDR dongles, or see the udev rule in the README for a "
                                           "receiver installed another way; then replug the dongle"};
    case usb_error::no_device:
        return Failure{ErrorCode::no_device, which + " disappeared while it was being opened"};
    default:
        return Failure{ErrorCode::usb, "could not open " + which + " (" + usb_error_text(r) +
                                           "); check the USB connection"};
    }
    device_ = std::move(device);

    UsbStrings strings;
    if (device_->usb_strings(strings) != 0)
        strings = UsbStrings{};
    if (selector.kind == DeviceSelector::Kind::serial && strings.serial != selector.serial) {
        close();
        return Failure{ErrorCode::no_device,
                       which + " no longer has serial " + selector.serial + "; the USB devices changed while it was opened"};
    }
    info_ = DeviceInfo{};
    info_.index = index;
    info_.serial = strings.serial;
    info_.name = display_name(strings);
    if (info_.name.empty())
        info_.name = backend_.device_name(index);
    if (info_.name.empty())
        info_.name = "RTL2832U";
    info_.tuner = device_->tuner();
    info_.blog_v4 = strings.manufacturer == "RTLSDRBlog" && strings.product == "Blog V4";
    gains_ = device_->tuner_gains();
    log_line("opened %s at index %u, serial %s, tuner %s", info_.name.c_str(), index,
             info_.serial.empty() ? "(none)" : info_.serial.c_str(), tuner_name(info_.tuner));

    if (auto f = configure(request)) {
        close();
        return f;
    }
    return std::nullopt;
}

Failure Receiver::tune_failure(uint32_t center) const {
    return invalid("the " + std::string(tuner_name(info_.tuner)) + " tuner could not tune to " +
                   std::to_string(center) + " Hz" + range_hint(info_) + ". Choose a center inside that range");
}

std::optional<Failure> Receiver::check_gain(const GainSetting& gain, bool direct_sampling, int& tenths) const {
    if (gain.automatic)
        return std::nullopt;
    const std::string tuner = tuner_name(info_.tuner);
    if (direct_sampling)
        return invalid("in direct sampling mode the tuner is not used, so module.gain has no effect; use gain = auto");
    if (info_.tuner == Tuner::fc2580 || info_.tuner == Tuner::unknown || gains_.empty())
        return invalid("the " + tuner + " tuner has no adjustable gain; use gain = auto");
    const auto [lo, hi] = std::minmax_element(gains_.begin(), gains_.end());
    // A request up to 1 dB beyond either end still means that end.
    if (gain.db * 10 < *lo - 10 || gain.db * 10 > *hi + 10)
        return invalid("gain " + db_text(gain.db) + " dB is outside the range of the " + tuner + " tuner, " +
                       db_text(*lo / 10.0) + " to " + db_text(*hi / 10.0) + " dB");
    tenths = gains_[static_cast<size_t>(nearest_gain(gains_, gain.db))];
    return std::nullopt;
}

std::optional<Failure> Receiver::set_gain(const GainSetting& gain) {
    int r;
    if (gain.automatic) {
        if ((r = device_->set_tuner_gain_mode(false)) != 0)
            return usb_failure("switching the tuner to automatic gain", r);
        manual_gain_ = false;
        effective_.gain = GainSetting{true, 0};
        return std::nullopt;
    }
    int tenths = 0;
    if (auto f = check_gain(gain, false, tenths))
        return f;
    // Switching to manual resets the gain to its lowest step, so only do it
    // when coming from automatic.
    if (!manual_gain_) {
        if ((r = device_->set_tuner_gain_mode(true)) != 0)
            return usb_failure("switching the tuner to manual gain", r);
        manual_gain_ = true;
    }
    if ((r = device_->set_tuner_gain(tenths)) != 0)
        return usb_failure("setting the tuner gain to " + db_text(tenths / 10.0) + " dB", r);
    effective_.gain = GainSetting{false, tenths / 10.0};
    return std::nullopt;
}

std::optional<Failure> Receiver::set_bias(bool on) {
    const int r = device_->set_bias_tee(on);
    if (r != 0)
        return usb_failure(on ? "switching the bias tee on" : "switching the bias tee off", r);
    effective_.bias_tee = on;
    bias_on_by_module_ = on;
    if (bias_forced_) {
        effective_.bias_tee_effective = *bias_forced_ || on;
        if (*bias_forced_ && !on)
            log_line("the EEPROM of this RTL-SDR forces the bias tee on, so it stays on; rtl_eeprom -b 0 clears "
                     "that flag");
    } else {
        effective_.bias_tee_effective.reset();
    }
    return std::nullopt;
}

std::optional<Failure> Receiver::configure(const OpenRequest& request) {
    const ModuleSettings& s = request.settings;
    const Tuner t = info_.tuner;
    const std::string tuner = tuner_name(t);
    const bool r82xx = t == Tuner::r820t || t == Tuner::r828d;
    const bool ds = s.direct_sampling != DirectSampling::off;

    // Refuse what this tuner cannot do before touching the hardware.
    if (!ds && t == Tuner::unknown)
        return invalid("librtlsdr does not know the tuner of this RTL-SDR; it can only receive with "
                       "module.direct_sampling = i or q");
    if (s.offset_tuning && ds)
        return invalid("module.offset_tuning cannot be combined with direct sampling; remove one of them");
    // librtlsdr answers offset tuning on these tuners by switching the bias
    // tee instead, so it must never be asked.
    if (s.offset_tuning && r82xx)
        return invalid("offset tuning is not available with the " + tuner + " tuner; remove module.offset_tuning");
    if (s.bandwidth != 0 && ds)
        return invalid("module.bandwidth has no effect in direct sampling mode; remove it");
    if (s.bandwidth != 0 && !r82xx && t != Tuner::e4000)
        return invalid("module.bandwidth can only be set with the R820T, R828D and E4000 tuners, and this "
                       "RTL-SDR has an " + tuner + "; remove it");
    int tenths = 0;
    if (auto f = check_gain(s.gain, ds, tenths))
        return f;

    uint32_t xtal = 0;
    if (device_->xtal_freq(xtal) != 0 || xtal == 0)
        return Failure{ErrorCode::internal, "librtlsdr did not report its crystal frequency"};
    if (ds && request.center >= xtal)
        return invalid("direct sampling receives below " + std::to_string(xtal) + " Hz, and center is " +
                       std::to_string(request.center) + " Hz; choose a lower center or switch direct sampling off");

    // rtlsdr_open() already applied this EEPROM flag; reading it here is the
    // only way to know whether the bias tee can be switched off.
    uint8_t eeprom[8] = {};
    if (device_->read_eeprom(eeprom, 0, sizeof eeprom) >= 0)
        bias_forced_ = (eeprom[7] & 0x02) == 0;
    else
        bias_forced_.reset();

    int r;
    if (s.ppm != 0 && (r = device_->set_freq_correction(s.ppm)) != 0 && r != -2)
        return usb_failure("setting the frequency correction", r);
    if (ds && (r = device_->set_direct_sampling(static_cast<int>(s.direct_sampling))) != 0)
        return usb_failure("switching to direct sampling", r);

    // Tune before setting the rate. Setting the rate makes librtlsdr retune an
    // R820T to its current frequency; before the first tune that is 0 Hz, which
    // makes the RTL-SDR Blog library switch to direct sampling and back, and
    // switching back re-initialises the tuner and loses the IF filter chosen
    // for the rate.
    if (device_->set_center_freq(request.center) != 0 || device_->center_freq() != request.center)
        return tune_failure(request.center);

    if ((r = device_->set_sample_rate(request.sample_rate)) != 0)
        return usb_failure("setting the sample rate", r);
    const double achieved = achieved_sample_rate(xtal, request.sample_rate);
    if (device_->sample_rate() != static_cast<uint32_t>(achieved))
        return Failure{ErrorCode::internal, "librtlsdr reports a sample rate of " +
                                                std::to_string(device_->sample_rate()) + " Hz where " +
                                                std::to_string(static_cast<uint32_t>(achieved)) + " Hz was expected"};
    if (request.sample_rate > reliable_sample_rate)
        log_line("above 2400000 samples/s many hosts lose samples without any error being reported");

    if (s.bandwidth != 0 && (r = device_->set_tuner_bandwidth(s.bandwidth)) != 0)
        return usb_failure("setting the tuner bandwidth", r);
    if (s.offset_tuning) {
        if ((r = device_->set_offset_tuning(true)) != 0)
            return usb_failure("switching offset tuning on", r);
        if (device_->offset_tuning() != 1)
            return Failure{ErrorCode::internal, "offset tuning did not switch on"};
    }

    // The rate, the bandwidth and offset tuning all retune.
    if (device_->center_freq() != request.center)
        return tune_failure(request.center);
    const int mode = device_->direct_sampling();
    if (mode < 0)
        return Failure{ErrorCode::internal, "librtlsdr did not report the direct sampling mode"};
    if (mode != static_cast<int>(s.direct_sampling)) {
        if (!ds && t == Tuner::r820t)
            return invalid("the R820T tuner cannot receive " + std::to_string(request.center) +
                           " Hz (it starts at about 24 MHz), and the RTL-SDR Blog library would switch to direct "
                           "sampling on its own. For HF through the HF input of an RTL-SDR Blog V3, set "
                           "module.direct_sampling = q");
        return Failure{ErrorCode::internal, "direct sampling is in mode " + std::to_string(mode) +
                                                " instead of " + std::to_string(static_cast<int>(s.direct_sampling))};
    }
    if (!ds && device_->pll_locked() == 0)
        return invalid("the " + tuner + " tuner could not lock its PLL at " + std::to_string(request.center) +
                       " Hz, so it would not really be tuned there" + range_hint(info_) +
                       ". Choose a center inside that range");

    if (!ds) {
        if (auto f = set_gain(s.gain))
            return f;
    } else {
        effective_.gain = s.gain;
    }
    if ((r = device_->set_agc_mode(s.rtl_agc)) != 0)
        return usb_failure("switching the RTL2832 AGC", r);
    if (auto f = set_bias(s.bias_tee))
        return f;
    if ((r = device_->reset_buffer()) != 0)
        return usb_failure("resetting the sample buffer", r);

    effective_.sample_rate = achieved;
    effective_.center = device_->center_freq();
    effective_.ppm = s.ppm;
    effective_.rtl_agc = s.rtl_agc;
    effective_.direct_sampling = s.direct_sampling;
    effective_.offset_tuning = s.offset_tuning;
    // What librtlsdr last passed to the tuner's filter: offset tuning widens
    // it to cover the shifted band, otherwise the bandwidth or the rate.
    uint32_t filter_request = s.bandwidth != 0 ? s.bandwidth : device_->sample_rate();
    if (s.offset_tuning)
        filter_request = 2 * (device_->sample_rate() / 2 * 170 / 100);
    effective_.bandwidth = ds ? 0 : tuner_filter_bandwidth(t, filter_request);
    effective_.buffers = s.buffers;
    log_line("tuned to %u Hz at %s samples/s, gain %s, direct sampling %s", effective_.center,
             json::serialize(json::Value(achieved)).c_str(),
             effective_.gain.automatic ? "auto" : (db_text(effective_.gain.db) + " dB").c_str(),
             direct_sampling_name(effective_.direct_sampling));
    return std::nullopt;
}

std::optional<Failure> Receiver::apply(const LiveChange& change) {
    if (!device_)
        return Failure{ErrorCode::internal, "no RTL-SDR is open"};
    const bool ds = effective_.direct_sampling != DirectSampling::off;
    int tenths = 0;
    if (change.gain)
        if (auto f = check_gain(*change.gain, ds, tenths))
            return f;

    if (change.gain) {
        if (ds)
            effective_.gain = *change.gain;
        else if (auto f = set_gain(*change.gain))
            return f;
    }
    if (change.rtl_agc) {
        const int r = device_->set_agc_mode(*change.rtl_agc);
        if (r != 0)
            return usb_failure("switching the RTL2832 AGC", r);
        effective_.rtl_agc = *change.rtl_agc;
    }
    if (change.bias_tee)
        if (auto f = set_bias(*change.bias_tee))
            return f;
    return std::nullopt;
}

namespace {

json::Value gain_json(const GainSetting& g) {
    if (g.automatic)
        return json::Value("auto");
    return json::Value(std::round(g.db * 10) / 10);
}

json::Value bias_effective_json(const std::optional<bool>& b) {
    if (!b)
        return json::Value("unknown");
    return json::Value(*b);
}

}  // namespace

json::Value Receiver::device_json() const {
    json::Value d = json::Value::object();
    d.set("name", info_.name);
    d.set("serial", info_.serial);
    d.set("tuner", tuner_name(info_.tuner));
    d.set("index", info_.index);
    return d;
}

json::Value Receiver::settings_json() const {
    json::Value s = json::Value::object();
    s.set("gain", gain_json(effective_.gain));
    s.set("ppm", effective_.ppm);
    s.set("rtl_agc", effective_.rtl_agc);
    s.set("bias_tee", effective_.bias_tee);
    s.set("bias_tee_effective", bias_effective_json(effective_.bias_tee_effective));
    s.set("direct_sampling", direct_sampling_name(effective_.direct_sampling));
    s.set("offset_tuning", effective_.offset_tuning);
    s.set("bandwidth", effective_.bandwidth);
    s.set("buffers", effective_.buffers);
    return s;
}

json::Value Receiver::settings_json(const LiveChange& change) const {
    json::Value s = json::Value::object();
    if (change.gain)
        s.set("gain", gain_json(effective_.gain));
    if (change.rtl_agc)
        s.set("rtl_agc", effective_.rtl_agc);
    if (change.bias_tee) {
        s.set("bias_tee", effective_.bias_tee);
        s.set("bias_tee_effective", bias_effective_json(effective_.bias_tee_effective));
    }
    return s;
}

namespace {

struct CloseJob {
    std::unique_ptr<Device> device;
    bool switch_bias_off = false;
    std::atomic<bool> done{false};
};

void run_close(CloseJob& job) {
    if (job.switch_bias_off && job.device->set_bias_tee(false) != 0)
        log_line("could not switch the bias tee off before closing the RTL-SDR");
    job.device.reset();
    job.done.store(true);
}

}  // namespace

void Receiver::close(bool device_answers) {
    if (!device_)
        return;
    CloseJob job;
    job.device = std::move(device_);
    job.switch_bias_off = device_answers && bias_on_by_module_;
    bias_on_by_module_ = false;
    manual_gain_ = false;
    run_close(job);
}

bool Receiver::close_by(std::chrono::steady_clock::time_point deadline, bool device_answers) {
    if (!device_)
        return true;
    // rtlsdr_close() waits without a limit for librtlsdr's streaming state to
    // settle, and a dongle that no longer answers costs it 300 ms per control
    // transfer. The job is shared so that it outlives this receiver if the
    // close does not finish in time.
    auto job = std::make_shared<CloseJob>();
    job->device = std::move(device_);
    job->switch_bias_off = device_answers && bias_on_by_module_;
    bias_on_by_module_ = false;
    manual_gain_ = false;
    std::thread worker;
    try {
        worker = std::thread([job] { run_close(*job); });
    } catch (const std::system_error&) {
        run_close(*job);
        return true;
    }
    while (!job->done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (job->done.load()) {
        worker.join();
        return true;
    }
    worker.detach();
    return false;
}

void Receiver::abandon() {
    (void)device_.release();
    bias_on_by_module_ = false;
}

}  // namespace fern
