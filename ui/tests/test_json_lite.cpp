// The JSON reader and writer under the frequency manager's file and two of
// its imports.
//
// EVERY CASE NAMES THE WRONG READER IT REJECTS, on the rule the rest of
// ui/tests follows. The one that matters most is a reader that goes through a
// double: it reads every frequency anybody has saved so far correctly and is
// still the conversion docs/conventions.md forbids, so it is caught here by
// literals a double cannot carry rather than by luck.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "models/json_lite.h"

namespace json = revenant::ui::json;

// Rejects a reader that parses the number as a double and casts. The first
// three are one frequency written three ways, SDR++ writing the last; the
// fourth is one hertz above 2^53, where a double cannot hold it.
TEST_CASE("a number is read as an integer from its literal, exactly", "[json]")
{
    CHECK(json::literal_to_integer("98500000") == 98'500'000);
    CHECK(json::literal_to_integer("9.85e7") == 98'500'000);
    CHECK(json::literal_to_integer("98500000.0") == 98'500'000);
    CHECK(json::literal_to_integer("9007199254740993") == 9'007'199'254'740'993LL);
    CHECK(json::literal_to_integer("146.52e6") == 146'520'000);
    CHECK(json::literal_to_integer("-250") == -250);
    CHECK(json::literal_to_integer("0.0") == 0);
    CHECK(json::literal_to_integer("0.0005e4") == 5);
}

// Rejects a reader that truncates a fraction into a frequency nobody saved,
// and one that wraps on overflow into a negative one.
TEST_CASE("a fraction or an overflow is not an integer unless rounding is asked for", "[json]")
{
    CHECK_FALSE(json::literal_to_integer("1.5").has_value());
    CHECK_FALSE(json::literal_to_integer("1e30").has_value());
    CHECK_FALSE(json::literal_to_integer("99999999999999999999").has_value());
    CHECK_FALSE(json::literal_to_integer("12abc").has_value());
    CHECK_FALSE(json::literal_to_integer("").has_value());

    CHECK(json::literal_to_integer("224298.0625", true) == 224'298);
    CHECK(json::literal_to_integer("2.5", true) == 3);
    CHECK(json::literal_to_integer("-2.5", true) == -3);
    CHECK(json::literal_to_integer("0.4", true) == 0);
}

// Rejects a writer that reorders members or mangles text: the file is read by
// people, and a name in any script has to come back as it went in.
TEST_CASE("a document written and read back is the same document", "[json]")
{
    json::Value root = json::object();
    root.set("zeta", json::integer(1));
    root.set("alpha", json::string("Ch\xC3\xA2teau \"quoted\" back\\slash\nline"));
    json::Value list = json::array();
    list.items.push_back(json::boolean(true));
    list.items.push_back(json::null());
    list.items.push_back(json::integer(-7));
    root.set("list", std::move(list));

    const std::string text = json::write(root);
    const auto back = json::parse(text);
    REQUIRE(back.has_value());
    REQUIRE(back->members.size() == 3);
    CHECK(back->members[0].key == "zeta");
    CHECK(back->members[1].key == "alpha");
    CHECK(back->find("alpha")->text == "Ch\xC3\xA2teau \"quoted\" back\\slash\nline");
    CHECK(back->find("list")->items.size() == 3);
    CHECK(json::as_integer(&back->find("list")->items[2]) == -7);

    // Flat arrays stay on one line, which is what keeps a tag list readable.
    CHECK(text.find("[true, null, -7]") != std::string::npos);
}

// Rejects a reader that drops the second half of a surrogate pair, or keeps
// the byte order mark a Windows editor saves as part of the first key.
TEST_CASE("escapes and a byte order mark are read as text", "[json]")
{
    const auto value = json::parse("\xEF\xBB\xBF{\"n\": \"\\u00e9\\ud83d\\udce1\"}");
    REQUIRE(value.has_value());
    CHECK(value->find("n")->text == "\xC3\xA9\xF0\x9F\x93\xA1");
}

// Rejects a reader that half-reads a broken file, and one whose error does not
// say where: a hand-edited memory file is fixed by going to the line.
TEST_CASE("a broken document is refused with the line it broke on", "[json]")
{
    const auto broken = json::parse("{\n  \"a\": 1,\n  \"b\": ,\n}");
    REQUIRE_FALSE(broken.has_value());
    CHECK(broken.error().message.starts_with("line 3:"));

    CHECK_FALSE(json::parse("[1, 2,]").has_value());
    CHECK_FALSE(json::parse("{} trailing").has_value());
    CHECK_FALSE(json::parse("\"unterminated").has_value());
    CHECK_FALSE(json::parse("{\"raw\": \"new\nline\"}").has_value());
}

// Rejects a reader that recurses without a limit, which a file of ten thousand
// brackets turns into a crash instead of a refusal.
TEST_CASE("nesting is bounded", "[json]")
{
    const std::string deep = std::string(1000, '[') + std::string(1000, ']');
    CHECK_FALSE(json::parse(deep).has_value());
    const std::string fine = std::string(10, '[') + std::string(10, ']');
    CHECK(json::parse(fine).has_value());
}

// Rejects values that do not know where they came from, which is what lets an
// import name the line it skipped.
TEST_CASE("each value knows the line it starts on", "[json]")
{
    const auto value = json::parse("{\n\"a\": 1,\n\n\"b\": {\n\"c\": 2}}");
    REQUIRE(value.has_value());
    CHECK(value->find("a")->line == 2);
    CHECK(value->find("b")->line == 4);
    CHECK(value->find("b")->find("c")->line == 5);
}
