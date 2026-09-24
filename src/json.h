// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// A small JSON value type with a strict RFC 8259 reader and a writer that
// always produces valid UTF-8.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace fern::json {

struct Member;

class Value {
public:
    enum class Type { null, boolean, number, string, array, object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::boolean), bool_(b) {}
    template <typename T, typename = std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>>>
    Value(T n) : type_(Type::number), number_(static_cast<double>(n)) {}
    Value(const char* s) : type_(Type::string), string_(s) {}
    Value(std::string s) : type_(Type::string), string_(std::move(s)) {}
    Value(std::string_view s) : type_(Type::string), string_(s) {}

    static Value array();
    static Value object();

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::null; }
    bool is_bool() const { return type_ == Type::boolean; }
    bool is_number() const { return type_ == Type::number; }
    bool is_string() const { return type_ == Type::string; }
    bool is_array() const { return type_ == Type::array; }
    bool is_object() const { return type_ == Type::object; }

    // The accessors below require the matching type.
    bool as_bool() const { return bool_; }
    double as_number() const { return number_; }
    const std::string& as_string() const { return string_; }
    const std::vector<Value>& items() const { return items_; }
    const std::vector<Member>& members() const;

    // nullptr unless this is an object holding key.
    const Value* find(std::string_view key) const;

    // Appends to an array.
    Value& push(Value v);
    // Adds or replaces a member of an object, keeping insertion order.
    Value& set(std::string key, Value v);
    // Appends a member without looking for an existing one; the caller
    // guarantees that key is new.
    Value& append_member(std::string key, Value v);

private:
    Type type_ = Type::null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<Value> items_;
    std::vector<Member> members_;
};

struct Member {
    std::string key;
    Value value;
};

// Nesting deeper than this is refused so that a hostile line cannot exhaust
// the stack.
constexpr int max_depth = 64;

// Parses exactly one JSON value surrounded by optional whitespace. On failure
// returns false and describes the problem, with its byte offset, in error.
bool parse(std::string_view text, Value& out, std::string& error);

// Compact serialization. Invalid UTF-8 in strings becomes U+FFFD and
// non-finite numbers become null, so the output is always valid JSON.
std::string serialize(const Value& v);
void append_quoted(std::string& out, std::string_view s);

// Whether d is an integer that a double represents exactly.
bool is_whole(double d);

}  // namespace fern::json
