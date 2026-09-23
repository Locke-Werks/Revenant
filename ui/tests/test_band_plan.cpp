// kBands and band_reachable: the table behind the band shortcuts and the band
// menu, and which of its entries the open device can be put on.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows. The table's failures are transcription: an edge typed
// with a digit missing, a centre outside its own band, a group split in two
// by an entry filed out of order. The reachability rule's failure is greying
// out every band on a source that simply has not answered yet.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <string_view>

#include "models/band_plan.h"

using revenant::ui::Band;
using revenant::ui::band_reachable;
using revenant::ui::kBands;

namespace {

// An R820T's tuning range.
constexpr std::int64_t kR820tLow = 24'000'000;
constexpr std::int64_t kR820tHigh = 1'766'000'000;

const Band& find(std::string_view name)
{
    for (const Band& band : kBands) {
        if (band.name == name) {
            return band;
        }
    }
    FAIL("no band named " << name);
    return kBands.front();
}

}  // namespace

// Rejects a centre copied from the wrong row, which tunes the front end to a
// frequency that is not in the band the button names.
TEST_CASE("every band's centre is inside its own edges", "[bands]")
{
    for (const Band& band : kBands) {
        INFO(band.name);
        CHECK(band.low_hz < band.high_hz);
        CHECK(band.centre_hz >= band.low_hz);
        CHECK(band.centre_hz <= band.high_hz);
    }
}

// Rejects an entry filed out of order, which the menu would draw as the same
// group heading twice.
TEST_CASE("a group's bands are together", "[bands]")
{
    std::set<std::string_view> finished;
    std::string_view current;
    for (const Band& band : kBands) {
        if (band.group != current) {
            INFO(band.group);
            CHECK(finished.count(band.group) == 0);
            if (!current.empty()) {
                finished.insert(current);
            }
            current = band.group;
        }
    }
}

TEST_CASE("names are unique and modes are demodulators", "[bands]")
{
    const std::set<std::string_view> modes{"am", "nfm", "wfm", "usb", "lsb", "dsb", "cw", "raw"};
    std::set<std::string_view> names;
    for (const Band& band : kBands) {
        INFO(band.name);
        CHECK(names.insert(band.name).second);
        CHECK(modes.count(band.mode) == 1);
    }
}

// The five shortcuts the tuning row always had keep their frequencies, so an
// operator's muscle memory lands where it used to.
TEST_CASE("the old shortcuts are favourites at the same frequencies", "[bands]")
{
    CHECK(find("FM").favourite);
    CHECK(find("FM").centre_hz == 98'100'000);
    CHECK(find("Airband").centre_hz == 124'000'000);
    CHECK(find("2 m").centre_hz == 145'000'000);
    CHECK(find("70 cm").centre_hz == 435'000'000);
    CHECK(find("GMRS / FRS").centre_hz == 462'562'500);
    CHECK(find("GMRS / FRS").favourite);
}

// Spot checks against the regulation, one per source, so a transcription
// slip in the table does not agree with a slip in the test.
TEST_CASE("edges match the band plans they cite", "[bands]")
{
    CHECK(find("20 m").low_hz == 14'000'000);
    CHECK(find("20 m").high_hz == 14'350'000);
    CHECK(find("10 m").high_hz == 29'700'000);
    CHECK(find("2 m").low_hz == 144'000'000);
    CHECK(find("2 m").high_hz == 148'000'000);
    CHECK(find("Airband").low_hz == 118'000'000);
    CHECK(find("Airband").high_hz == 137'000'000);
    CHECK(find("NOAA").low_hz == 162'400'000);
    CHECK(find("NOAA").high_hz == 162'550'000);
    CHECK(find("ADS-B").centre_hz == 1'090'000'000);
}

TEST_CASE("an R820T reaches VHF and UHF and not HF", "[bands]")
{
    CHECK(band_reachable(find("FM"), kR820tLow, kR820tHigh));
    CHECK(band_reachable(find("ADS-B"), kR820tLow, kR820tHigh));
    CHECK(band_reachable(find("10 m"), kR820tLow, kR820tHigh));
    CHECK_FALSE(band_reachable(find("20 m"), kR820tLow, kR820tHigh));
    CHECK_FALSE(band_reachable(find("MW AM"), kR820tLow, kR820tHigh));
}

// Rejects greying everything out against an unanswered range. Every source
// starts there, and a menu that is dead until the first answer arrives reads
// as a device that tunes nothing.
TEST_CASE("an unknown range refuses nothing", "[bands]")
{
    CHECK(band_reachable(find("20 m"), 0, 0));
    CHECK(band_reachable(find("23 cm"), 5, 5));
}

// The edges of the range are inside it, the same rule the engine applies.
TEST_CASE("a centre exactly on the range's edge is reachable", "[bands]")
{
    const Band& fm = find("FM");
    CHECK(band_reachable(fm, fm.centre_hz, fm.centre_hz + 1));
    CHECK(band_reachable(fm, fm.centre_hz - 1, fm.centre_hz));
    CHECK_FALSE(band_reachable(fm, fm.centre_hz + 1, fm.centre_hz + 2));
}
