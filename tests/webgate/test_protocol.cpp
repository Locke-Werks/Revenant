#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "tools/webgate/protocol.h"

namespace wg = revenant::webgate;

namespace {

using Bytes = std::vector<std::uint8_t>;

bool contains(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("Spectrum frame has the exact little-endian layout", "[webgate][protocol]") {
    wg::ReducedFrame f;
    f.px = {0x10, 0x20, 0x30, 0x40};
    f.offset_db = 1.0f;       // 0x3F800000
    f.scale_db = -2.0f;       // 0xC0000000
    f.source_start = 0x0102030405060708ull;
    f.view = {1.0, 2.0, 4};   // 1.0 = 0x3FF0000000000000, 2.0 = 0x4000000000000000

    const Bytes got = wg::encode(f);
    const Bytes want = {
        0x01,                                            // tag
        0x00, 0x00, 0x80, 0x3F,                          // offset_db
        0x00, 0x00, 0x00, 0xC0,                          // scale_db
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,  // source_start
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F,  // center_hz
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,  // span_hz
        0x04, 0x00, 0x00, 0x00,                          // width_px
        0x10, 0x20, 0x30, 0x40,                          // px
    };
    CHECK(got == want);
    CHECK(got.size() == wg::kSpectrumHeaderSize + 4);
}

// The trace row follows the waterfall row with its own window, so a phone that
// reads only the first part still finds the header where it always was.
TEST_CASE("Spectrum frame carries the trace row after the waterfall row", "[webgate][protocol]") {
    wg::ReducedFrame f;
    f.px = {0x10, 0x20};
    f.trace_px = {0x30, 0x40};
    f.trace_offset_db = 1.0f;
    f.trace_scale_db = -2.0f;
    f.view = {1.0, 2.0, 2};

    const Bytes got = wg::encode(f);
    REQUIRE(got.size() == wg::kSpectrumHeaderSize + 2 + wg::kSpectrumTraceHeaderSize + 2);
    const Bytes tail(got.begin() + static_cast<std::ptrdiff_t>(wg::kSpectrumHeaderSize), got.end());
    const Bytes want = {
        0x10, 0x20,              // px
        0x00, 0x00, 0x80, 0x3F,  // trace_offset_db
        0x00, 0x00, 0x00, 0xC0,  // trace_scale_db
        0x30, 0x40,              // trace_px
    };
    CHECK(tail == want);
}

TEST_CASE("Spectrum width on the wire is the pixel count", "[webgate][protocol]") {
    wg::ReducedFrame f;
    f.px = {1, 2, 3};
    f.view = {0.0, 0.0, 999};  // disagrees with px; px wins
    const Bytes got = wg::encode(f);
    REQUIRE(got.size() == wg::kSpectrumHeaderSize + 3);
    CHECK(got[33] == 3);
    CHECK(got[34] == 0);
}

TEST_CASE("Opus frame has the exact little-endian layout", "[webgate][protocol]") {
    const std::uint8_t pkt[] = {0xAA, 0xBB, 0xCC};
    const Bytes got = wg::encode_opus(0x1122334455667788ull, 0xA1B2C3D4u, pkt);
    const Bytes want = {
        0x02,
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,  // vrx
        0xD4, 0xC3, 0xB2, 0xA1,                          // seq
        0xAA, 0xBB, 0xCC,
    };
    CHECK(got == want);

    CHECK(wg::encode_opus(1, 2, {}).size() == wg::kOpusHeaderSize);
}

TEST_CASE("parse_command accepts a well-formed command", "[webgate][protocol]") {
    auto c = wg::parse_command(R"({"id":7,"op":"tune","args":{"vrx":3,"hz":146520000.5,)"
                               R"("mode":"nfm","on":true,"gain":-12}})");
    REQUIRE(c.has_value());
    CHECK(c->id == 7);
    CHECK(c->op == "tune");
    CHECK(c->has("vrx"));
    CHECK_FALSE(c->has("nope"));
    CHECK(c->get_u64("vrx").value() == 3);
    CHECK(c->get_double("hz").value() == 146520000.5);
    CHECK(c->get_string("mode").value() == "nfm");
    CHECK(c->get_bool("on").value() == true);
    CHECK(c->get_i64("gain").value() == -12);
}

TEST_CASE("Command args survive a move and a copy", "[webgate][protocol]") {
    auto c = wg::parse_command(R"({"id":1,"op":"setGain","args":{"db":4}})");
    REQUIRE(c.has_value());
    wg::Command moved = std::move(*c);
    wg::Command copy = moved;
    std::vector<wg::Command> v;
    for (int i = 0; i < 50; ++i) v.push_back(copy);  // forces reallocation
    CHECK(moved.get_u64("db").value() == 4);
    CHECK(v.back().get_u64("db").value() == 4);
}

TEST_CASE("Missing args means an empty object", "[webgate][protocol]") {
    auto c = wg::parse_command(R"({"id":2,"op":"listSources"})");
    REQUIRE(c.has_value());
    CHECK_FALSE(c->has("x"));
    auto r = c->get_u64("x");
    REQUIRE_FALSE(r.has_value());
    CHECK(contains(r.error().message, "x"));
}

TEST_CASE("Argument getters name the key on a type or range error", "[webgate][protocol]") {
    auto c = wg::parse_command(R"({"id":1,"op":"tune","args":{"s":"a","n":-1,"f":1.5,)"
                               R"("big":1e30,"b":1}})");
    REQUIRE(c.has_value());
    for (auto* key : {"s", "n", "f", "big"}) {
        auto r = c->get_u64(key);
        REQUIRE_FALSE(r.has_value());
        CHECK(contains(r.error().message, key));
    }
    CHECK_FALSE(c->get_string("n").has_value());
    CHECK_FALSE(c->get_bool("b").has_value());
    CHECK_FALSE(c->get_double("s").has_value());
    CHECK_FALSE(c->get_i64("f").has_value());
    CHECK(c->get_double("f").value() == 1.5);
    CHECK(c->get_i64("n").value() == -1);
}

TEST_CASE("Every named op parses and the list is exact", "[webgate][protocol]") {
    CHECK(std::size(wg::kOps) == 22);
    for (auto op : wg::kOps) {
        auto c = wg::parse_command(R"({"id":1,"op":")" + std::string(op) + R"("})");
        INFO(op);
        CHECK(c.has_value());
    }
    CHECK(std::size(wg::kEvents) == 14);
}

TEST_CASE("get_string_list reads an array of strings and refuses anything else",
          "[webgate][protocol]") {
    auto c = wg::parse_command(
        R"({"id":1,"op":"setDecoders","args":{"ok":["a","b"],"none":[],"mixed":["a",1],)"
        R"("s":"a","long":[")" + std::string(wg::kMaxListItemBytes + 1, 'x') + R"("]}})");
    REQUIRE(c.has_value());
    CHECK(c->get_string_list("ok").value() == std::vector<std::string>{"a", "b"});
    CHECK(c->get_string_list("none").value().empty());
    for (auto* key : {"mixed", "s", "long", "absent"}) {
        INFO(key);
        auto r = c->get_string_list(key);
        REQUIRE_FALSE(r.has_value());
        CHECK(contains(r.error().message, key));
    }
}

TEST_CASE("parse_command refuses an unknown op and names it", "[webgate][protocol]") {
    auto c = wg::parse_command(R"({"id":1,"op":"selfDestruct"})");
    REQUIRE_FALSE(c.has_value());
    CHECK(contains(c.error().message, "selfDestruct"));
}

TEST_CASE("parse_command refuses a bad id", "[webgate][protocol]") {
    CHECK_FALSE(wg::parse_command(R"({"op":"tune"})").has_value());
    CHECK_FALSE(wg::parse_command(R"({"id":"1","op":"tune"})").has_value());
    CHECK_FALSE(wg::parse_command(R"({"id":1.5,"op":"tune"})").has_value());
    CHECK_FALSE(wg::parse_command(R"({"id":-1,"op":"tune"})").has_value());
    CHECK_FALSE(wg::parse_command(R"({"id":null,"op":"tune"})").has_value());
    CHECK_FALSE(wg::parse_command(R"({"id":1e300,"op":"tune"})").has_value());
    CHECK_FALSE(wg::parse_command(R"({"id":true,"op":"tune"})").has_value());
}

TEST_CASE("parse_command refuses a bad op", "[webgate][protocol]") {
    CHECK_FALSE(wg::parse_command(R"({"id":1})").has_value());
    CHECK_FALSE(wg::parse_command(R"({"id":1,"op":5})").has_value());
    CHECK_FALSE(wg::parse_command(R"({"id":1,"op":null})").has_value());
}

TEST_CASE("parse_command refuses non-object JSON", "[webgate][protocol]") {
    for (auto* text : {"[]", "[1,2]", "5", "\"tune\"", "null", "true"}) {
        INFO(text);
        CHECK_FALSE(wg::parse_command(text).has_value());
    }
}

TEST_CASE("parse_command refuses non-object args", "[webgate][protocol]") {
    CHECK_FALSE(wg::parse_command(R"({"id":1,"op":"tune","args":[1]})").has_value());
    CHECK_FALSE(wg::parse_command(R"({"id":1,"op":"tune","args":5})").has_value());
    CHECK_FALSE(wg::parse_command(R"({"id":1,"op":"tune","args":null})").has_value());
}

TEST_CASE("parse_command never throws on garbage", "[webgate][protocol]") {
    const std::string nul("{\"id\":1,\0\"op\":\"tune\"}", 20);
    const std::string deep = std::string(5000, '[') + std::string(5000, ']');
    const std::string deep_obj = [] {
        std::string s;
        for (int i = 0; i < 5000; ++i) s += "{\"a\":";
        return s;
    }();
    const std::vector<std::string> inputs = {
        "",
        " ",
        "{",
        "}",
        "{\"id\":",
        "{\"id\":1,\"op\":\"tune\"",
        "{\"id\":1,\"op\":\"tune\"} trailing",
        "{'id':1}",
        "\xff\xfe\xfd",
        "{\"id\":1,\"op\":\"\xff\"}",
        "{\"id\":1,\"op\":\"\\ud800\"}",
        "{\"id\":NaN}",
        nul,
        deep,
        deep_obj,
    };
    for (const auto& s : inputs) {
        CHECK_NOTHROW(wg::parse_command(s));
        CHECK_NOTHROW(wg::peek_id(s));
        CHECK_FALSE(wg::parse_command(s).has_value());
    }
}

TEST_CASE("parse_command refuses an oversized message", "[webgate][protocol]") {
    // Valid JSON, so only the size cap can be the reason it is refused.
    std::string big = R"({"id":1,"op":"tune","args":{"pad":")";
    big.append(wg::kMaxCommandBytes, 'x');
    big += R"("}})";
    REQUIRE(big.size() > wg::kMaxCommandBytes);
    auto c = wg::parse_command(big);
    REQUIRE_FALSE(c.has_value());
    CHECK(contains(c.error().message, "too large"));

    // Exactly at the cap is allowed.
    std::string at = R"({"id":1,"op":"tune","args":{"pad":")";
    const std::string tail = R"("}})";
    at.append(wg::kMaxCommandBytes - at.size() - tail.size(), 'x');
    at += tail;
    REQUIRE(at.size() == wg::kMaxCommandBytes);
    CHECK(wg::parse_command(at).has_value());
}

TEST_CASE("peek_id recovers the id of a refused command", "[webgate][protocol]") {
    CHECK(wg::peek_id(R"({"id":9,"op":"nope"})") == 9);
    CHECK_FALSE(wg::peek_id(R"({"op":"tune"})").has_value());
    CHECK_FALSE(wg::peek_id("[1]").has_value());
}

TEST_CASE("Replies have the exact shape", "[webgate][protocol]") {
    CHECK(wg::ok_reply(5) == R"({"id":5,"ok":true,"result":null})");
    CHECK(wg::ok_reply(5, R"({"a":1})") == R"({"id":5,"ok":true,"result":{"a":1}})");
    CHECK(wg::error_reply(6, "device busy") == R"({"id":6,"ok":false,"error":"device busy"})");
    CHECK(wg::ok_reply(18446744073709551615ull).find("18446744073709551615") !=
          std::string::npos);
}

TEST_CASE("Error text goes through verbatim apart from escaping", "[webgate][protocol]") {
    CHECK(wg::error_reply(1, "say \"hi\"\\ \n\t") ==
          R"({"id":1,"ok":false,"error":"say \"hi\"\\ \n\t"})");
    CHECK(wg::error_reply(1, "caf\xC3\xA9 \xE2\x82\xAC") ==
          "{\"id\":1,\"ok\":false,\"error\":\"caf\xC3\xA9 \xE2\x82\xAC\"}");
    // Invalid UTF-8 becomes U+FFFD so the text frame stays valid.
    CHECK(wg::error_reply(1, "a\xFF" "b") ==
          "{\"id\":1,\"ok\":false,\"error\":\"a\xEF\xBF\xBD" "b\"}");
    CHECK(wg::error_reply(1, std::string("a\0b", 3)) ==
          R"({"id":1,"ok":false,"error":"a\u0000b"})");
    // A truncated multibyte sequence: one U+FFFD per orphaned byte.
    CHECK(wg::json_string("x\xE2\x82") == "\"x\xEF\xBF\xBD\xEF\xBF\xBD\"");
}

TEST_CASE("Events carry their name first", "[webgate][protocol]") {
    CHECK(wg::event("vrxRemoved") == R"({"event":"vrxRemoved"})");

    wg::JsonObject o;
    o.add_u64("vrx", 3).add_string("mode", "nfm").add_bool("open", false).add_double("hz", 1.5);
    o.add_i64("db", -7).add_null("x").add_raw("list", "[1,2]");
    CHECK(wg::event("vrxStatus", o) ==
          R"({"event":"vrxStatus","vrx":3,"mode":"nfm","open":false,"hz":1.5,)"
          R"("db":-7,"x":null,"list":[1,2]})");

    wg::JsonObject bad;
    bad.add_string("event", "hijack").add_u64("n", 1);
    CHECK(wg::event("link", bad) == R"({"event":"link","n":1})");
}

TEST_CASE("Non-finite doubles are written as null", "[webgate][protocol]") {
    wg::JsonObject o;
    o.add_double("a", std::numeric_limits<double>::quiet_NaN());
    o.add_double("b", std::numeric_limits<double>::infinity());
    CHECK(o.str() == R"({"a":null,"b":null})");
}

TEST_CASE("A built reply parses back through the codec", "[webgate][protocol]") {
    wg::JsonObject o;
    o.add_string("note", "q\"\\\n\xE2\x82\xAC").add_u64("n", 42);
    const std::string text = wg::event("source", o);

    capnp::JsonCodec codec;
    capnp::MallocMessageBuilder mb;
    auto root = mb.initRoot<capnp::JsonValue>();
    codec.decodeRaw(kj::arrayPtr(text.data(), text.size()), root);
    REQUIRE(root.which() == capnp::JsonValue::OBJECT);
    CHECK(root.getObject().size() == 3);
}

TEST_CASE("A realtime factor is reported when it moves a step or starts", "[webgate][protocol]") {
    CHECK(wg::realtime_report_due(0.0, 0.5));
    CHECK(wg::realtime_report_due(1.0, 0.0));
    CHECK_FALSE(wg::realtime_report_due(0.0, 0.0));
    CHECK_FALSE(wg::realtime_report_due(1.0, 0.995));
    CHECK(wg::realtime_report_due(1.0, 0.97));
    CHECK(wg::realtime_report_due(0.97, 1.0));
    CHECK_FALSE(wg::realtime_report_due(0.97, 0.975));
}

TEST_CASE("A pace change is reported even when the factor holds", "[webgate][protocol]") {
    CHECK(wg::pacing_report_due(1.0, 1.0, 1.0, 0.5));
    CHECK(wg::pacing_report_due(1.0, 0.0, 1.0, 1.0));
    CHECK_FALSE(wg::pacing_report_due(1.0, 1.0, 1.002, 1.0));
    CHECK(wg::pacing_report_due(1.0, 1.0, 0.9, 1.0));
}
