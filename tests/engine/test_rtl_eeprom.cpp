// core/source/rtl_eeprom.h: the parse and rebuild tools/rtl-serial writes to a
// dongle. No device here; the images are built by hand in the layout a stock
// RTL2838 ships with.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>

#include "core/source/rtl_eeprom.h"

namespace eeprom = revenant::source::rtl_eeprom;

namespace {

std::size_t put_string(eeprom::Image& image, std::size_t at, std::string_view text)
{
    image[at] = static_cast<std::uint8_t>(2 + 2 * text.size());
    image[at + 1] = 0x03;
    for (std::size_t i = 0; i < text.size(); ++i) {
        image[at + 2 + 2 * i] = static_cast<std::uint8_t>(text[i]);
        image[at + 3 + 2 * i] = 0x00;
    }
    return at + 2 + 2 * text.size();
}

// Realtek / RTL2838UHIDIR / 00000001 at VID 0x0BDA PID 0x2838, the image
// rtl_eeprom -g realtek writes, with an erased tail.
eeprom::Image stock(std::string_view serial = "00000001")
{
    eeprom::Image image{};
    image.fill(0xFF);
    const std::uint8_t head[] = {0x28, 0x32, 0xDA, 0x0B, 0x38, 0x28, 0xA5, 0x16, 0x02};
    std::copy(std::begin(head), std::end(head), image.begin());
    std::size_t at = put_string(image, eeprom::kStringsOffset, "Realtek");
    at = put_string(image, at, "RTL2838UHIDIR");
    put_string(image, at, serial);
    return image;
}

}  // namespace

TEST_CASE("rtl_eeprom parses the stock layout", "[rtl_eeprom]")
{
    const auto parsed = eeprom::parse(stock());
    REQUIRE(parsed);
    CHECK(parsed->vid == 0x0BDA);
    CHECK(parsed->pid == 0x2838);
    CHECK(parsed->manufacturer.text == "Realtek");
    CHECK(parsed->product.text == "RTL2838UHIDIR");
    CHECK(parsed->serial.text == "00000001");
}

TEST_CASE("rtl_eeprom rebuilding with the same serial changes nothing", "[rtl_eeprom]")
{
    const auto original = stock();
    const auto rebuilt = eeprom::with_serial(original, "00000001");
    REQUIRE(rebuilt);
    CHECK(*rebuilt == original);
}

TEST_CASE("rtl_eeprom a longer serial lands intact and keeps the other strings", "[rtl_eeprom]")
{
    const auto original = stock();
    const auto rebuilt = eeprom::with_serial(original, "revenant-hf_0001");
    REQUIRE(rebuilt);
    CHECK(*rebuilt == stock("revenant-hf_0001"));

    const auto parsed = eeprom::parse(*rebuilt);
    REQUIRE(parsed);
    CHECK(parsed->serial.text == "revenant-hf_0001");
    for (std::size_t i = 0; i < parsed->serial.offset; ++i) {
        CHECK((*rebuilt)[i] == original[i]);
    }
}

TEST_CASE("rtl_eeprom a shorter serial 0xFF-fills to the old end and no further", "[rtl_eeprom]")
{
    auto original = stock();
    const auto old = eeprom::parse(original);
    REQUIRE(old);
    // A non-0xFF byte past the old end, which must survive untouched.
    original[old->strings_end()] = 0x5A;

    const auto rebuilt = eeprom::with_serial(original, "A");
    REQUIRE(rebuilt);
    const auto parsed = eeprom::parse(*rebuilt);
    REQUIRE(parsed);
    CHECK(parsed->serial.text == "A");
    for (std::size_t i = parsed->strings_end(); i < old->strings_end(); ++i) {
        CHECK((*rebuilt)[i] == 0xFF);
    }
    CHECK((*rebuilt)[old->strings_end()] == 0x5A);
}

TEST_CASE("rtl_eeprom refuses an image without the signature", "[rtl_eeprom]")
{
    auto image = stock();
    image[1] = 0x33;
    CHECK_FALSE(eeprom::parse(image));
    CHECK_FALSE(eeprom::with_serial(image, "X"));

    eeprom::Image erased{};
    erased.fill(0xFF);
    CHECK_FALSE(eeprom::with_serial(erased, "X"));
}

TEST_CASE("rtl_eeprom refuses malformed descriptors", "[rtl_eeprom]")
{
    SECTION("odd length")
    {
        auto image = stock();
        image[eeprom::kStringsOffset] = 15;
        CHECK_FALSE(eeprom::parse(image));
    }
    SECTION("length below the header")
    {
        auto image = stock();
        image[eeprom::kStringsOffset] = 0;
        CHECK_FALSE(eeprom::parse(image));
    }
    SECTION("wrong descriptor type")
    {
        auto image = stock();
        const auto parsed = eeprom::parse(image);
        REQUIRE(parsed);
        image[parsed->product.offset + 1] = 0x02;
        CHECK_FALSE(eeprom::parse(image));
        CHECK_FALSE(eeprom::with_serial(image, "X"));
    }
    SECTION("serial runs off the end")
    {
        auto image = stock();
        const auto parsed = eeprom::parse(image);
        REQUIRE(parsed);
        image[parsed->serial.offset] = 254;
        CHECK_FALSE(eeprom::parse(image));
    }
}

TEST_CASE("rtl_eeprom refuses a rebuild past 256 bytes", "[rtl_eeprom]")
{
    // A product string long enough that a 32-character serial no longer fits
    // after it, while the stock 8-character one still does.
    eeprom::Image image{};
    image.fill(0xFF);
    const std::uint8_t head[] = {0x28, 0x32, 0xDA, 0x0B, 0x38, 0x28, 0xA5, 0x16, 0x02};
    std::copy(std::begin(head), std::end(head), image.begin());
    std::size_t at = put_string(image, eeprom::kStringsOffset, "Realtek");
    at = put_string(image, at, std::string(100, 'P'));
    put_string(image, at, "00000001");
    REQUIRE(eeprom::parse(image));

    const auto rebuilt = eeprom::with_serial(image, std::string(32, 'S'));
    CHECK_FALSE(rebuilt);
}

TEST_CASE("rtl_eeprom validates the new serial", "[rtl_eeprom]")
{
    CHECK(eeprom::valid_serial("a"));
    CHECK(eeprom::valid_serial("Revenant-HF_01"));
    CHECK(eeprom::valid_serial(std::string(32, 'x')));
    CHECK_FALSE(eeprom::valid_serial(""));
    CHECK_FALSE(eeprom::valid_serial(std::string(33, 'x')));
    CHECK_FALSE(eeprom::valid_serial("has space"));
    CHECK_FALSE(eeprom::valid_serial("dot.ted"));
}
