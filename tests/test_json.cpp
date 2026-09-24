// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdint>
#include <limits>
#include <string>

#include "io.h"
#include "json.h"
#include "test.h"

using fern::json::Value;

namespace {

Value parsed(const std::string& text) {
    Value v;
    std::string error;
    const bool ok = fern::json::parse(text, v, error);
    if (!ok)
        test::report(__FILE__, __LINE__, "parse failed for " + text + ": " + error);
    return v;
}

bool rejects(const std::string& text) {
    Value v;
    std::string error;
    return !fern::json::parse(text, v, error) && !error.empty();
}

std::string round_trip(const std::string& text) { return fern::json::serialize(parsed(text)); }

}  // namespace

TEST(json_parses_values) {
    CHECK(parsed("null").is_null());
    CHECK(parsed("true").as_bool());
    CHECK(!parsed(" false ").as_bool());
    CHECK_EQ(parsed("0").as_number(), 0.0);
    CHECK_EQ(parsed("-12.5e1").as_number(), -125.0);
    CHECK_EQ(parsed("2400000").as_number(), 2400000.0);
    CHECK_EQ(parsed("1E3").as_number(), 1000.0);
    CHECK_EQ(parsed("\"text\"").as_string(), std::string("text"));

    const Value open = parsed(
        "{\"type\":\"open\",\"sample_rate\":2400000,\"center\":14200000,\"signal\":\"iq\","
        "\"settings\":{\"device\":\"serial:00000001\",\"gain\":\"auto\",\"list\":[1,[2,{}],[]]}}");
    REQUIRE(open.is_object());
    CHECK_EQ(open.find("type")->as_string(), std::string("open"));
    CHECK_EQ(open.find("center")->as_number(), 14200000.0);
    const Value* settings = open.find("settings");
    REQUIRE(settings && settings->is_object());
    CHECK_EQ(settings->members().size(), size_t(3));
    const Value* list = settings->find("list");
    REQUIRE(list && list->is_array());
    CHECK_EQ(list->items().size(), size_t(3));
    CHECK(list->items()[1].items()[1].is_object());
    CHECK(open.find("missing") == nullptr);
    CHECK(parsed("[]").find("type") == nullptr);
}

TEST(json_parses_escapes_and_utf8) {
    CHECK_EQ(parsed("\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\"").as_string(), std::string("a\"b\\c/d\b\f\n\r\t"));
    CHECK_EQ(parsed("\"\\u0041\\u00e9\\u20ac\"").as_string(), std::string("A\xC3\xA9\xE2\x82\xAC"));
    CHECK_EQ(parsed("\"\\ud83d\\ude00\"").as_string(), std::string("\xF0\x9F\x98\x80"));
    CHECK_EQ(parsed("\"gr\xC3\xBC\xC3\x9F\"").as_string(), std::string("gr\xC3\xBC\xC3\x9F"));
    CHECK_EQ(parsed("\"\\u0000\"").as_string().size(), size_t(1));
}

TEST(json_rejects_malformed_input) {
    const char* bad[] = {
        "",           " ",         "{",          "}",           "[1,]",         "[1 2]",       "{\"a\":1,}",
        "{\"a\" 1}",  "{a:1}",     "{\"a\":}",   "'text'",      "tru",          "nul",         "True",
        "01",         "-",         "1.",         ".5",          "1e",           "+1",          "0x10",
        "NaN",        "Infinity",  "1e999",      "\"abc",       "\"\\x\"",      "\"\\u12\"",   "\"\\uzzzz\"",
        "\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u0041\"", "\"a\tb\"", "\"a\nb\"", "{} {}", "[1]x",
        "{\"a\":1,\"a\":2}",
    };
    for (const char* text : bad)
        if (!rejects(text))
            test::report(__FILE__, __LINE__, std::string("accepted malformed JSON: ") + text);
    // Invalid UTF-8: stray continuation, overlong, surrogate, truncated.
    CHECK(rejects("\"\x80\""));
    CHECK(rejects("\"\xC0\xAF\""));
    CHECK(rejects("\"\xED\xA0\x80\""));
    CHECK(rejects("\"\xE2\x82\""));
    CHECK(rejects("\"\xF5\x80\x80\x80\""));
}

TEST(json_limits_nesting) {
    std::string deep(fern::json::max_depth, '[');
    deep += std::string(fern::json::max_depth, ']');
    CHECK(!rejects(deep));
    std::string deeper(fern::json::max_depth + 1, '[');
    deeper += std::string(fern::json::max_depth + 1, ']');
    CHECK(rejects(deeper));
    // A hostile line of only brackets must fail cleanly, not overflow the stack.
    CHECK(rejects(std::string(65536, '[')));
}

TEST(json_reports_error_offset) {
    Value v;
    std::string error;
    CHECK(!fern::json::parse("{\"a\":1,\"b\":x}", v, error));
    CHECK_HAS(error, "byte 11");
}

TEST(json_serializes_and_escapes) {
    Value o = Value::object();
    o.set("type", "error");
    o.set("message", "quote \" backslash \\ newline \n tab \t bell \x07 del \x7f");
    o.set("fatal", true);
    o.set("nothing", nullptr);
    o.set("list", Value::array().push(1).push(-2.5).push("x"));
    CHECK_EQ(fern::json::serialize(o),
             std::string("{\"type\":\"error\",\"message\":\"quote \\\" backslash \\\\ newline \\n tab \\t bell "
                         "\\u0007 del \\u007f\",\"fatal\":true,\"nothing\":null,\"list\":[1,-2.5,\"x\"]}"));
    // set() replaces and keeps the position.
    o.set("type", "stats");
    CHECK_EQ(fern::json::serialize(o).substr(0, 17), std::string("{\"type\":\"stats\",\""));
}

TEST(json_serialized_strings_are_valid_utf8) {
    const std::string raw = std::string("ok \xC3\xA9 bad \xFF\xC3 end \xE2\x82");
    const std::string out = fern::json::serialize(Value(raw));
    CHECK_EQ(out, std::string("\"ok \xC3\xA9 bad \xEF\xBF\xBD\xEF\xBF\xBD end \xEF\xBF\xBD\xEF\xBF\xBD\""));
    Value back;
    std::string error;
    CHECK(fern::json::parse(out, back, error));
}

TEST(json_formats_numbers) {
    CHECK_EQ(fern::json::serialize(Value(2400000.0)), std::string("2400000"));
    CHECK_EQ(fern::json::serialize(Value(38.6)), std::string("38.6"));
    CHECK_EQ(fern::json::serialize(Value(-9.9)), std::string("-9.9"));
    CHECK_EQ(fern::json::serialize(Value(0.0)), std::string("0"));
    CHECK_EQ(fern::json::serialize(Value(uint64_t(9007199254740991ull))), std::string("9007199254740991"));
    CHECK_EQ(fern::json::serialize(Value(4294967295u)), std::string("4294967295"));
    const double rate = 28800000 * 4194304.0 / 120795952.0;
    Value back;
    std::string error;
    REQUIRE(fern::json::parse(fern::json::serialize(Value(rate)), back, error));
    CHECK_EQ(back.as_number(), rate);
    CHECK_EQ(fern::json::serialize(Value(std::numeric_limits<double>::infinity())), std::string("null"));
}

TEST(json_round_trips) {
    const char* texts[] = {
        "{\"a\":[1,2,{\"b\":null}],\"c\":\"\\u00e9\\\"\"}",
        "[true,false,null,0,-1,0.5,\"\"]",
        "{}",
        "[]",
    };
    for (const char* t : texts) {
        const std::string once = round_trip(t);
        CHECK_EQ(round_trip(once), once);
    }
}

TEST(line_reader_splits_lines) {
    fern::LineReader r(64);
    fern::LineReader::Line line;
    r.feed("one\ntw", 6);
    REQUIRE(r.next(line));
    CHECK_EQ(line.text, std::string("one"));
    CHECK(!line.too_long);
    CHECK(!r.next(line));
    r.feed("o\n\nthree\r\n", 10);
    REQUIRE(r.next(line));
    CHECK_EQ(line.text, std::string("two"));
    REQUIRE(r.next(line));
    CHECK_EQ(line.text, std::string(""));
    REQUIRE(r.next(line));
    CHECK_EQ(line.text, std::string("three\r"));
    r.feed("last", 4);
    CHECK(!r.next(line));
    r.finish();
    REQUIRE(r.next(line));
    CHECK_EQ(line.text, std::string("last"));
    CHECK(!r.next(line));
}

TEST(line_reader_drops_long_lines) {
    fern::LineReader r(8);
    fern::LineReader::Line line;
    const std::string exact(8, 'x');
    r.feed((exact + "\n").data(), 9);
    REQUIRE(r.next(line));
    CHECK_EQ(line.text, exact);

    // Nine bytes arrive in pieces; the line is reported once as too long and
    // everything up to its newline is skipped.
    r.feed("12345", 5);
    r.feed("6789", 4);
    REQUIRE(r.next(line));
    CHECK(line.too_long);
    CHECK(line.text.empty());
    r.feed("more of the same line", 21);
    CHECK(!r.next(line));
    r.feed(" still\nnext\n", 12);
    REQUIRE(r.next(line));
    CHECK_EQ(line.text, std::string("next"));
    CHECK(!line.too_long);

    // A long line that ends within one chunk.
    r.feed("0123456789\nok\n", 14);
    REQUIRE(r.next(line));
    CHECK(line.too_long);
    REQUIRE(r.next(line));
    CHECK_EQ(line.text, std::string("ok"));

    // A long unterminated line at the end of input is dropped.
    r.feed("abcdefghijk", 11);
    r.finish();
    REQUIRE(r.next(line));
    CHECK(line.too_long);
    CHECK(!r.next(line));
}
