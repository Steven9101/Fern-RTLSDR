// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "settings.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <string_view>
#include <vector>

#ifndef FERN_RTLSDR_VERSION
#error "FERN_RTLSDR_VERSION must be defined by the build"
#endif

namespace fern {

const char* module_version() { return FERN_RTLSDR_VERSION; }

const char* direct_sampling_name(DirectSampling mode) {
    switch (mode) {
    case DirectSampling::i: return "i";
    case DirectSampling::q: return "q";
    case DirectSampling::automatic: return "auto";
    case DirectSampling::off: break;
    }
    return "off";
}

bool sample_rate_supported(uint32_t hz) {
    return (hz > 225000 && hz <= 300000) || (hz > 900000 && hz <= 3200000);
}

namespace {

enum class Key { device, gain, ppm, rtl_agc, bias_tee, direct_sampling, offset_tuning, bandwidth, buffers };

struct KeyInfo {
    Key key;
    const char* name;
    bool live;
};

// The order here is the order of --describe.
constexpr KeyInfo all_keys[] = {
    {Key::device, "device", false},
    {Key::gain, "gain", true},
    {Key::ppm, "ppm", false},
    {Key::rtl_agc, "rtl_agc", true},
    {Key::bias_tee, "bias_tee", true},
    {Key::direct_sampling, "direct_sampling", false},
    {Key::offset_tuning, "offset_tuning", false},
    {Key::bandwidth, "bandwidth", false},
    {Key::buffers, "buffers", false},
};

const KeyInfo* find_key(std::string_view name) {
    for (const KeyInfo& k : all_keys)
        if (name == k.name)
            return &k;
    return nullptr;
}

std::string known_keys() {
    std::string s;
    const size_t n = sizeof all_keys / sizeof all_keys[0];
    for (size_t i = 0; i < n; ++i) {
        if (i > 0)
            s += i + 1 == n ? " and " : ", ";
        s += all_keys[i].name;
    }
    return s;
}

// A value as the operator would recognise it in a message, kept short.
std::string shown(const json::Value& v) {
    std::string s = json::serialize(v);
    if (s.size() > 60)
        s = s.substr(0, 57) + "...";
    return s;
}

std::optional<Failure> invalid(std::string message) {
    return Failure{ErrorCode::invalid, std::move(message)};
}

bool whole_in_range(const json::Value& v, double lo, double hi, double& out) {
    if (!v.is_number() || !json::is_whole(v.as_number()))
        return false;
    const double d = v.as_number();
    if (d < lo || d > hi)
        return false;
    out = d;
    return true;
}

std::optional<Failure> parse_device(const json::Value& v, DeviceSelector& out) {
    const std::string wrong = "module.device must be serial:<serial>, index:<n> or empty, not ";
    if (!v.is_string())
        return invalid(wrong + shown(v));
    const std::string& s = v.as_string();
    DeviceSelector sel;
    if (s.empty()) {
        sel.kind = DeviceSelector::Kind::only;
    } else if (s.rfind("serial:", 0) == 0 && s.size() > 7) {
        sel.kind = DeviceSelector::Kind::serial;
        sel.serial = s.substr(7);
    } else if (s.rfind("index:", 0) == 0 && s.size() > 6 && s.size() <= 16) {
        const std::string_view digits(s.data() + 6, s.size() - 6);
        uint64_t n = 0;
        for (char c : digits) {
            if (c < '0' || c > '9')
                return invalid(wrong + shown(v));
            n = n * 10 + static_cast<uint64_t>(c - '0');
        }
        if (n > UINT32_MAX)
            return invalid(wrong + shown(v));
        sel.kind = DeviceSelector::Kind::index;
        sel.index = static_cast<uint32_t>(n);
    } else {
        return invalid(wrong + shown(v));
    }
    out = sel;
    return std::nullopt;
}

bool same_word(const std::string& s, const char* word) {
    size_t i = 0;
    for (; word[i] != '\0'; ++i)
        if (i >= s.size() || std::tolower(static_cast<unsigned char>(s[i])) != word[i])
            return false;
    return i == s.size();
}

// The schema declares gain as a string ("auto", "tuner" or a number of dB);
// a JSON number is accepted as well because set may carry one.
std::optional<Failure> parse_gain(const json::Value& v, GainSetting& out) {
    const std::string wrong = "module.gain must be auto, tuner or a gain in dB such as 38.6, not ";
    double db;
    if (v.is_number()) {
        db = v.as_number();
    } else if (v.is_string()) {
        const std::string& s = v.as_string();
        if (same_word(s, "auto")) {
            out = GainSetting();
            return std::nullopt;
        }
        if (same_word(s, "tuner")) {
            out = GainSetting::tuner_agc();
            return std::nullopt;
        }
        const char* first = s.data();
        const char* last = s.data() + s.size();
        const auto res = std::from_chars(first, last, db, std::chars_format::fixed);
        if (s.empty() || res.ec != std::errc() || res.ptr != last)
            return invalid(wrong + shown(v));
    } else {
        return invalid(wrong + shown(v));
    }
    if (!std::isfinite(db) || db < -100 || db > 100)
        return invalid(wrong + shown(v));
    out = GainSetting::manual(db);
    return std::nullopt;
}

std::optional<Failure> parse_bool(const char* key, const json::Value& v, bool& out) {
    if (!v.is_bool())
        return invalid(std::string("module.") + key + " must be yes or no (a JSON boolean), not " + shown(v));
    out = v.as_bool();
    return std::nullopt;
}

std::optional<Failure> parse_value(const KeyInfo& k, const json::Value& v, ModuleSettings& s) {
    double d;
    switch (k.key) {
    case Key::device:
        return parse_device(v, s.device);
    case Key::gain:
        return parse_gain(v, s.gain);
    case Key::ppm:
        if (!whole_in_range(v, min_ppm, max_ppm, d))
            return invalid("module.ppm must be a whole number from " + std::to_string(min_ppm) + " to " +
                           std::to_string(max_ppm) + ", not " + shown(v));
        s.ppm = static_cast<int>(d);
        return std::nullopt;
    case Key::rtl_agc:
        return parse_bool(k.name, v, s.rtl_agc);
    case Key::bias_tee:
        return parse_bool(k.name, v, s.bias_tee);
    case Key::direct_sampling:
        if (v.is_string() && v.as_string() == "auto")
            s.direct_sampling = DirectSampling::automatic;
        else if (v.is_string() && v.as_string() == "off")
            s.direct_sampling = DirectSampling::off;
        else if (v.is_string() && v.as_string() == "i")
            s.direct_sampling = DirectSampling::i;
        else if (v.is_string() && v.as_string() == "q")
            s.direct_sampling = DirectSampling::q;
        else
            return invalid("module.direct_sampling must be auto, off, i or q, not " + shown(v));
        return std::nullopt;
    case Key::offset_tuning:
        return parse_bool(k.name, v, s.offset_tuning);
    case Key::bandwidth:
        if (!whole_in_range(v, 0, max_bandwidth, d))
            return invalid("module.bandwidth must be 0 (automatic) or a whole number of Hz up to " +
                           std::to_string(max_bandwidth) + ", not " + shown(v));
        s.bandwidth = static_cast<uint32_t>(d);
        return std::nullopt;
    case Key::buffers:
        if (!whole_in_range(v, min_buffers, max_buffers, d))
            return invalid("module.buffers must be a whole number from " + std::to_string(min_buffers) + " to " +
                           std::to_string(max_buffers) + ", not " + shown(v));
        s.buffers = static_cast<uint32_t>(d);
        return std::nullopt;
    }
    return invalid("internal: unhandled setting");
}

// Unknown keys are refused all at once, so that the operator can fix every
// one of them in one go.
std::optional<Failure> check_keys(const json::Value& settings) {
    std::vector<std::string> unknown;
    for (const json::Member& m : settings.members())
        if (!find_key(m.key))
            unknown.push_back(m.key);
    if (unknown.empty())
        return std::nullopt;
    std::string names;
    for (size_t i = 0; i < unknown.size() && i < 8; ++i) {
        if (i > 0)
            names += ", ";
        const std::string& key = unknown[i];
        names += "module." + (key.size() > 40 ? key.substr(0, 37) + "..." : key);
    }
    if (unknown.size() > 8)
        names += " and " + std::to_string(unknown.size() - 8) + " more";
    return invalid(std::string(unknown.size() == 1 ? "unknown setting " : "unknown settings ") + names +
                   "; this module takes " + known_keys());
}

}  // namespace

std::optional<Failure> parse_open(const json::Value& message, OpenRequest& out) {
    OpenRequest req;
    double d;

    const json::Value* rate = message.find("sample_rate");
    if (!rate)
        return invalid("open carries no sample_rate");
    if (!whole_in_range(*rate, 0, UINT32_MAX, d))
        return invalid("sample_rate must be a whole number of Hz, not " + shown(*rate));
    if (!sample_rate_supported(static_cast<uint32_t>(d)))
        return invalid("sample_rate " + shown(*rate) +
                       " Hz is not possible with an RTL2832U; use a whole number of Hz from 225001 to 300000 "
                       "or from 900001 to 3200000, such as 2400000 or 2048000");
    req.sample_rate = static_cast<uint32_t>(d);

    const json::Value* center = message.find("center");
    if (!center)
        return invalid("open carries no center");
    if (!whole_in_range(*center, 0, UINT32_MAX, d))
        return invalid("center must be a whole number of Hz from 0 to 4294967295, not " + shown(*center));
    req.center = static_cast<uint32_t>(d);

    const json::Value* signal = message.find("signal");
    if (!signal)
        return invalid("open carries no signal");
    if (!signal->is_string() || (signal->as_string() != "iq" && signal->as_string() != "real"))
        return invalid("signal must be iq or real, not " + shown(*signal));
    if (signal->as_string() != "iq")
        return invalid("an RTL-SDR delivers I/Q samples; set signal = iq for this band");

    const json::Value* settings = message.find("settings");
    if (settings) {
        if (!settings->is_object())
            return invalid("settings in open must be an object, not " + shown(*settings));
        if (auto f = check_keys(*settings))
            return f;
        for (const json::Member& m : settings->members())
            if (auto f = parse_value(*find_key(m.key), m.value, req.settings))
                return f;
    }
    out = req;
    return std::nullopt;
}

std::optional<Failure> parse_set(const json::Value& settings, LiveChange& out) {
    if (!settings.is_object())
        return invalid("settings in set must be an object, not " + shown(settings));
    if (auto f = check_keys(settings))
        return f;
    LiveChange change;
    ModuleSettings scratch;
    for (const json::Member& m : settings.members()) {
        const KeyInfo& k = *find_key(m.key);
        if (!k.live)
            return invalid("module." + m.key + " cannot change while the band runs; restart the band to apply it");
        if (auto f = parse_value(k, m.value, scratch))
            return f;
        switch (k.key) {
        case Key::gain: change.gain = scratch.gain; break;
        case Key::rtl_agc: change.rtl_agc = scratch.rtl_agc; break;
        case Key::bias_tee: change.bias_tee = scratch.bias_tee; break;
        default: break;
        }
    }
    out = change;
    return std::nullopt;
}

json::Value settings_schema() {
    json::Value list = json::Value::array();
    for (const KeyInfo& k : all_keys) {
        json::Value s = json::Value::object();
        s.set("key", k.name);
        switch (k.key) {
        case Key::device:
            s.set("type", "string");
            s.set("label", "Device");
            s.set("default", "");
            s.set("help",
                  "Which RTL-SDR to use: serial:<serial> or index:<n>. Leave empty when exactly one is "
                  "plugged in. fern-rtlsdr --list-devices shows the serials.");
            break;
        case Key::gain:
            s.set("type", "string");
            s.set("label", "Gain");
            s.set("default", "auto");
            s.set("help",
                  "auto chooses the highest gain that keeps the converter out of clipping with 6 dB to "
                  "spare, and lowers it at once when something clips; tuner leaves it to the tuner's own "
                  "AGC; or a gain in dB such as 38.6, of which the nearest the tuner supports is used and "
                  "reported.");
            break;
        case Key::ppm:
            s.set("type", "number");
            s.set("label", "Frequency correction");
            s.set("default", 0);
            s.set("min", min_ppm);
            s.set("max", max_ppm);
            s.set("unit", "ppm");
            s.set("help",
                  "Crystal error of the dongle in parts per million, as a whole number. Leave it at 0 "
                  "for dongles with a TCXO, such as the RTL-SDR Blog V3 and V4.");
            break;
        case Key::rtl_agc:
            s.set("type", "boolean");
            s.set("label", "RTL2832 AGC");
            s.set("default", false);
            s.set("help", "Digital AGC in the RTL2832U. Usually off; it can help in direct sampling mode.");
            break;
        case Key::bias_tee:
            s.set("type", "boolean");
            s.set("label", "Bias tee");
            s.set("default", false);
            s.set("help",
                  "Puts 4.5 V on the antenna input to power an LNA (RTL-SDR Blog V3 and V4). The "
                  "dongle's EEPROM can force it on regardless of this setting; the module reports "
                  "the result as bias_tee_effective.");
            break;
        case Key::direct_sampling:
            s.set("type", "choice");
            s.set("label", "Direct sampling");
            s.set("choices", json::Value::array().push("auto").push("off").push("i").push("q"));
            s.set("default", "auto");
            s.set("help",
                  "Samples the antenna input directly, for HF below 28.8 MHz. auto does what the "
                  "dongle needs: off on the RTL-SDR Blog V4, which receives HF through its own "
                  "upconverter, and q (the HF input of the Blog V3 and most dongles) for a band "
                  "below where the tuner starts. i is for dongles modified on the I branch.");
            break;
        case Key::offset_tuning:
            s.set("type", "boolean");
            s.set("label", "Offset tuning");
            s.set("default", false);
            s.set("help",
                  "Moves the DC spike of zero-IF tuners out of the band. E4000, FC0012, FC0013 and "
                  "FC2580 tuners only.");
            break;
        case Key::bandwidth:
            s.set("type", "number");
            s.set("label", "Tuner bandwidth");
            s.set("default", 0);
            s.set("min", 0);
            s.set("max", max_bandwidth);
            s.set("unit", "Hz");
            s.set("help",
                  "IF filter bandwidth of the tuner; 0 matches it to the sample rate. The tuner uses "
                  "its nearest filter. R820T, R828D and E4000 tuners only.");
            break;
        case Key::buffers:
            s.set("type", "number");
            s.set("label", "USB buffers");
            s.set("default", default_buffers);
            s.set("min", min_buffers);
            s.set("max", max_buffers);
            s.set("help",
                  "Advanced. Number of USB transfers kept in flight, each holding about 20 ms of "
                  "samples. More of them ride out longer pauses of a busy host.");
            break;
        }
        s.set("live", k.live);
        list.push(std::move(s));
    }
    return list;
}

json::Value describe_module() {
    json::Value d = json::Value::object();
    d.set("api", module_api);
    d.set("id", module_id);
    d.set("name", module_name);
    d.set("version", module_version());
    d.set("kind", "input");
    d.set("settings", settings_schema());
    // What the radio can be set to, for FernSDR's suggestions of bands.
    // HF too: direct_sampling = auto takes it through the Q branch, or the
    // Blog V4's upconverter. 2.4 Msps is the highest most computers sustain.
    json::Value tuning = json::Value::object();
    tuning.set("ranges", json::Value::array().push(json::Value::array().push(500000.0).push(1766000000.0)));
    tuning.set("rates", json::Value::array().push(2400000.0).push(2048000.0).push(1024000.0));
    tuning.set("signal", "iq");
    d.set("tuning", tuning);
    return d;
}

}  // namespace fern
