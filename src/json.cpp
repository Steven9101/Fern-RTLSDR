// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "json.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <unordered_set>

namespace fern::json {

Value Value::array() {
    Value v;
    v.type_ = Type::array;
    return v;
}

Value Value::object() {
    Value v;
    v.type_ = Type::object;
    return v;
}

const std::vector<Member>& Value::members() const { return members_; }

const Value* Value::find(std::string_view key) const {
    if (type_ != Type::object)
        return nullptr;
    for (const Member& m : members_)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

Value& Value::push(Value v) {
    items_.push_back(std::move(v));
    return *this;
}

Value& Value::set(std::string key, Value v) {
    for (Member& m : members_) {
        if (m.key == key) {
            m.value = std::move(v);
            return *this;
        }
    }
    members_.push_back(Member{std::move(key), std::move(v)});
    return *this;
}

Value& Value::append_member(std::string key, Value v) {
    members_.push_back(Member{std::move(key), std::move(v)});
    return *this;
}

bool is_whole(double d) { return std::isfinite(d) && std::floor(d) == d; }

namespace {

// Length of the well-formed UTF-8 sequence that starts at s[i], or 0 when the
// bytes there are not one (overlong forms and surrogates included).
size_t utf8_sequence(std::string_view s, size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80)
        return 1;
    size_t len;
    uint32_t cp;
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
        cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    if (i + len > s.size())
        return 0;
    for (size_t k = 1; k < len; ++k) {
        const unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }
    if (len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)))
        return 0;
    if (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))
        return 0;
    return len;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) : t_(text) {}

    bool run(Value& out, std::string& error) {
        skip_ws();
        bool ok = value(out, 0);
        if (ok) {
            skip_ws();
            if (pos_ != t_.size())
                ok = fail("unexpected data after the value");
        }
        if (!ok)
            error = msg_;
        return ok;
    }

private:
    std::string_view t_;
    size_t pos_ = 0;
    std::string msg_;

    bool fail(const char* what) {
        if (msg_.empty())
            msg_ = std::string(what) + " at byte " + std::to_string(pos_);
        return false;
    }

    bool at_end() const { return pos_ >= t_.size(); }
    char peek() const { return t_[pos_]; }

    void skip_ws() {
        while (!at_end()) {
            const char c = peek();
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                break;
            ++pos_;
        }
    }

    bool value(Value& out, int depth) {
        if (at_end())
            return fail("unexpected end of input");
        switch (peek()) {
        case '{':
            return object(out, depth);
        case '[':
            return array(out, depth);
        case '"': {
            std::string s;
            if (!string(s))
                return false;
            out = Value(std::move(s));
            return true;
        }
        case 't':
            return literal("true", Value(true), out);
        case 'f':
            return literal("false", Value(false), out);
        case 'n':
            return literal("null", Value(), out);
        default:
            if (peek() == '-' || (peek() >= '0' && peek() <= '9'))
                return number(out);
            return fail("unexpected character");
        }
    }

    bool literal(std::string_view word, Value v, Value& out) {
        if (t_.substr(pos_, word.size()) != word)
            return fail("unexpected character");
        pos_ += word.size();
        out = std::move(v);
        return true;
    }

    bool digits() {
        const size_t start = pos_;
        while (!at_end() && peek() >= '0' && peek() <= '9')
            ++pos_;
        return pos_ > start;
    }

    bool number(Value& out) {
        const size_t start = pos_;
        if (peek() == '-')
            ++pos_;
        if (at_end())
            return fail("incomplete number");
        if (peek() == '0') {
            ++pos_;
        } else if (!digits()) {
            return fail("invalid number");
        }
        if (!at_end() && peek() == '.') {
            ++pos_;
            if (!digits())
                return fail("invalid number");
        }
        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            ++pos_;
            if (!at_end() && (peek() == '+' || peek() == '-'))
                ++pos_;
            if (!digits())
                return fail("invalid number");
        }
        double d = 0;
        const char* first = t_.data() + start;
        const char* last = t_.data() + pos_;
        const auto res = std::from_chars(first, last, d);
        if (res.ec != std::errc() || res.ptr != last || !std::isfinite(d)) {
            pos_ = start;
            return fail("number out of range");
        }
        out = Value(d);
        return true;
    }

    bool hex4(uint32_t& cp) {
        if (pos_ + 4 > t_.size())
            return fail("incomplete \\u escape");
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = t_[pos_++];
            cp <<= 4;
            if (c >= '0' && c <= '9')
                cp |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                cp |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                cp |= static_cast<uint32_t>(c - 'A' + 10);
            else
                return fail("invalid \\u escape");
        }
        return true;
    }

    bool escape(std::string& out) {
        ++pos_;  // the backslash
        if (at_end())
            return fail("unterminated string");
        const char c = t_[pos_++];
        switch (c) {
        case '"': out += '"'; return true;
        case '\\': out += '\\'; return true;
        case '/': out += '/'; return true;
        case 'b': out += '\b'; return true;
        case 'f': out += '\f'; return true;
        case 'n': out += '\n'; return true;
        case 'r': out += '\r'; return true;
        case 't': out += '\t'; return true;
        case 'u': break;
        default:
            --pos_;
            return fail("invalid escape");
        }
        uint32_t cp = 0;
        if (!hex4(cp))
            return false;
        if (cp >= 0xDC00 && cp <= 0xDFFF)
            return fail("unpaired surrogate in \\u escape");
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (t_.substr(pos_, 2) != "\\u")
                return fail("unpaired surrogate in \\u escape");
            pos_ += 2;
            uint32_t low = 0;
            if (!hex4(low))
                return false;
            if (low < 0xDC00 || low > 0xDFFF)
                return fail("unpaired surrogate in \\u escape");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        }
        append_utf8(out, cp);
        return true;
    }

    bool string(std::string& out) {
        ++pos_;  // the opening quote
        for (;;) {
            if (at_end())
                return fail("unterminated string");
            const unsigned char c = static_cast<unsigned char>(peek());
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c == '\\') {
                if (!escape(out))
                    return false;
            } else if (c < 0x20) {
                return fail("control character in string");
            } else if (c < 0x80) {
                out += static_cast<char>(c);
                ++pos_;
            } else {
                const size_t len = utf8_sequence(t_, pos_);
                if (len == 0)
                    return fail("invalid UTF-8");
                out.append(t_.substr(pos_, len));
                pos_ += len;
            }
        }
    }

    bool array(Value& out, int depth) {
        if (depth >= max_depth)
            return fail("nesting too deep");
        ++pos_;
        out = Value::array();
        skip_ws();
        if (!at_end() && peek() == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_ws();
            Value item;
            if (!value(item, depth + 1))
                return false;
            out.push(std::move(item));
            skip_ws();
            if (at_end())
                return fail("unterminated array");
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                return true;
            }
            return fail("expected , or ]");
        }
    }

    bool object(Value& out, int depth) {
        if (depth >= max_depth)
            return fail("nesting too deep");
        ++pos_;
        out = Value::object();
        std::unordered_set<std::string> seen;
        skip_ws();
        if (!at_end() && peek() == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_ws();
            if (at_end() || peek() != '"')
                return fail("expected a string key");
            const size_t key_pos = pos_;
            std::string key;
            if (!string(key))
                return false;
            if (!seen.insert(key).second) {
                pos_ = key_pos;
                return fail("duplicate key");
            }
            skip_ws();
            if (at_end() || peek() != ':')
                return fail("expected :");
            ++pos_;
            skip_ws();
            Value v;
            if (!value(v, depth + 1))
                return false;
            out.append_member(std::move(key), std::move(v));
            skip_ws();
            if (at_end())
                return fail("unterminated object");
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                return true;
            }
            return fail("expected , or }");
        }
    }
};

void append_number(std::string& out, double d) {
    if (!std::isfinite(d)) {
        out += "null";
        return;
    }
    char buf[64];
    std::to_chars_result res;
    if (is_whole(d) && std::fabs(d) < 9007199254740992.0)
        res = std::to_chars(buf, buf + sizeof buf, static_cast<long long>(d));
    else
        res = std::to_chars(buf, buf + sizeof buf, d);
    out.append(buf, res.ptr);
}

void append_value(std::string& out, const Value& v) {
    switch (v.type()) {
    case Value::Type::null:
        out += "null";
        break;
    case Value::Type::boolean:
        out += v.as_bool() ? "true" : "false";
        break;
    case Value::Type::number:
        append_number(out, v.as_number());
        break;
    case Value::Type::string:
        append_quoted(out, v.as_string());
        break;
    case Value::Type::array: {
        out += '[';
        bool first = true;
        for (const Value& item : v.items()) {
            if (!first)
                out += ',';
            first = false;
            append_value(out, item);
        }
        out += ']';
        break;
    }
    case Value::Type::object: {
        out += '{';
        bool first = true;
        for (const Member& m : v.members()) {
            if (!first)
                out += ',';
            first = false;
            append_quoted(out, m.key);
            out += ':';
            append_value(out, m.value);
        }
        out += '}';
        break;
    }
    }
}

}  // namespace

bool parse(std::string_view text, Value& out, std::string& error) {
    Parser p(text);
    Value v;
    if (!p.run(v, error))
        return false;
    out = std::move(v);
    return true;
}

void append_quoted(std::string& out, std::string_view s) {
    static const char hex[] = "0123456789abcdef";
    out += '"';
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\b') {
            out += "\\b";
        } else if (c == '\f') {
            out += "\\f";
        } else if (c < 0x20 || c == 0x7F) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 0xF];
        } else if (c >= 0x80) {
            const size_t len = utf8_sequence(s, i);
            if (len == 0) {
                out += "\xEF\xBF\xBD";
                ++i;
                continue;
            }
            out.append(s.substr(i, len));
            i += len;
            continue;
        } else {
            out += static_cast<char>(c);
        }
        ++i;
    }
    out += '"';
}

std::string serialize(const Value& v) {
    std::string out;
    append_value(out, v);
    return out;
}

}  // namespace fern::json
