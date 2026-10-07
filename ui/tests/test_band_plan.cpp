// kBands, its region and overlap helpers, and band_reachable: the table behind
// the band bar, the band menu and the shortcuts, and which of its rows the open
// device can be put on.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows. The table's failures are transcription: an edge typed
// with a digit missing, a centre outside its own band, a row with no citation
// or no region, a channel with no submenu to file it under. The helpers'
// failures are an overlap test that drops a row straddling an edge, and an
// order that depends on where a row happens to sit in the table. The
// reachability rule's failure is greying out every band on a source that
// simply has not answered yet.
//
// THE SPOT CHECKS TOLERATE A MISSING ROW. The rows are written per service in
// models/band_plan/*.inc, and a check that failed for want of a row would
// fail for the wrong reason. A row that is present has to be right.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <string_view>
#include <utility>

#include "models/band_plan.h"

using revenant::ui::Band;
using revenant::ui::band_reachable;
using revenant::ui::BandKind;
using revenant::ui::bands_for_region;
using revenant::ui::bands_overlapping;
using revenant::ui::kBands;
using revenant::ui::kRegionCA;
using revenant::ui::kRegionUS;
using revenant::ui::kRegionUSCA;

namespace {

// An R820T's tuning range.
constexpr std::int64_t kR820tLow = 24'000'000;
constexpr std::int64_t kR820tHigh = 1'766'000'000;

// The first row covering [low, high] whose name contains the text, or null.
const Band* covering(std::string_view text, std::int64_t low, std::int64_t high)
{
    for (const Band& band : kBands) {
        if (band.name.find(text) != std::string_view::npos && band.low_hz <= low &&
            band.high_hz >= high) {
            return &band;
        }
    }
    return nullptr;
}

const Band* named(std::string_view name)
{
    for (const Band& band : kBands) {
        if (band.name == name) {
            return &band;
        }
    }
    return nullptr;
}

}  // namespace

// Rejects a centre copied from the wrong row, an edge pair typed backwards, a
// row that no region shows, an uncited row, and a channel with nowhere to go
// in the menu.
TEST_CASE("every row is well formed", "[bands]")
{
    REQUIRE(kBands.size() > 0);
    const std::set<std::string_view> modes{"",    "am",  "nfm",   "wfm",   "usb",
                                           "lsb", "dsb", "cw",    "raw",   "sam",
                                           "p25p1", "dstar", "tetra", "dmr"};
    for (const Band& band : kBands) {
        INFO(band.group << " / " << band.name);
        CHECK_FALSE(band.name.empty());
        CHECK_FALSE(band.source.empty());
        CHECK(band.low_hz < band.high_hz);
        CHECK(band.centre_hz >= band.low_hz);
        CHECK(band.centre_hz <= band.high_hz);
        CHECK(band.regions != 0);
        CHECK((band.regions & ~kRegionUSCA) == 0);
        CHECK(modes.count(band.mode) == 1);
        if (band.kind == BandKind::kChannel) {
            CHECK_FALSE(band.parent.empty());
        }
    }
}

// Rejects the same row written twice for the same region, which the menu would
// list twice. One name may appear once per region where the edges differ.
TEST_CASE("no row is listed twice in one region", "[bands]")
{
    for (const std::uint32_t region : {static_cast<std::uint32_t>(kRegionUS),
                                       static_cast<std::uint32_t>(kRegionCA)}) {
        std::set<std::pair<std::string_view, std::string_view>> seen;
        for (const Band* band : bands_for_region(region)) {
            INFO(band->group << " / " << band->name);
            CHECK(seen.insert({band->parent, band->name}).second);
        }
    }
}

// Spot checks against the regulation, so a transcription slip in a data file
// does not agree with a slip in the test.
TEST_CASE("edges match the rules they cite", "[bands]")
{
    if (const Band* two = named("2 m")) {
        CHECK(two->low_hz == 144'000'000);
        CHECK(two->high_hz == 148'000'000);
        CHECK(two->centre_hz == 145'000'000);
        CHECK(two->favourite);
    }
    // 47 CFR 95.973: CB channel 19 is 27.185 MHz.
    if (const Band* cb19 = covering("19", 27'185'000, 27'185'000)) {
        CHECK(cb19->kind == BandKind::kChannel);
        CHECK(cb19->centre_hz == 27'185'000);
    }
    // 47 CFR 95.2763: the five MURS channels run 151.82 to 154.6 MHz.
    for (const Band& band : kBands) {
        if (band.name.find("MURS") != std::string_view::npos) {
            INFO(band.name);
            CHECK(band.low_hz >= 151'800'000);
            CHECK(band.high_hz <= 154'612'500);
        }
    }
    // 47 CFR 95.1763: GMRS is in the 462 and 467 MHz groups.
    for (const Band& band : kBands) {
        if (band.name.find("GMRS") != std::string_view::npos) {
            INFO(band.name);
            CHECK(band.low_hz >= 462'000'000);
            CHECK(band.high_hz <= 468'000'000);
        }
    }
    if (const Band* gmrs1 = covering("GMRS", 462'562'500, 462'562'500)) {
        CHECK(gmrs1->centre_hz >= 462'550'000);
        CHECK(gmrs1->centre_hz <= 462'575'000);
    }
}

// Rejects a region filter that returns rows the mask excludes, or drops rows
// that hold on both sides of the border.
TEST_CASE("bands_for_region returns exactly the rows the mask names", "[bands]")
{
    CHECK(bands_for_region(kRegionUSCA).size() == kBands.size());
    CHECK(bands_for_region(0).empty());
    std::size_t us = 0;
    std::size_t ca = 0;
    std::size_t both = 0;
    for (const Band& band : kBands) {
        us += (band.regions & kRegionUS) != 0 ? 1U : 0U;
        ca += (band.regions & kRegionCA) != 0 ? 1U : 0U;
        both += band.regions == kRegionUSCA ? 1U : 0U;
    }
    const auto us_rows = bands_for_region(kRegionUS);
    CHECK(us_rows.size() == us);
    CHECK(bands_for_region(kRegionCA).size() == ca);
    for (const Band* band : us_rows) {
        CHECK((band->regions & kRegionUS) != 0);
    }
    CHECK(us + ca - both == kBands.size());
}

// Rejects an overlap test that only finds rows wholly inside the span, and an
// order that is the table's rather than widest first.
TEST_CASE("bands_overlapping finds straddling rows, widest first", "[bands]")
{
    CHECK(bands_overlapping(0, 1, kRegionUSCA).empty());

    // Every row straddles a one hertz span around its own centre.
    for (const Band& band : kBands) {
        const auto hits = bands_overlapping(band.centre_hz, band.centre_hz + 1, band.regions);
        bool found = false;
        for (const Band* hit : hits) {
            found = found || hit == &band;
        }
        INFO(band.name);
        CHECK(found);
    }

    // A span with the low edge inside a row and the high edge past it.
    const Band& first = kBands.front();
    const auto straddle =
        bands_overlapping(first.high_hz - 1, first.high_hz + 1'000'000, first.regions);
    bool found = false;
    for (const Band* hit : straddle) {
        found = found || hit == &first;
    }
    CHECK(found);

    // Touching is not overlapping: a span that ends on the low edge.
    const auto touching = bands_overlapping(first.low_hz - 1'000, first.low_hz, first.regions);
    for (const Band* hit : touching) {
        CHECK(hit != &first);
    }

    // Widest first, then low edge, then name, across the whole receivable span.
    const auto all = bands_overlapping(0, 300'000'000'000, kRegionUSCA);
    CHECK(all.size() == kBands.size());
    for (std::size_t i = 1; i < all.size(); ++i) {
        const Band& a = *all[i - 1];
        const Band& b = *all[i];
        const std::int64_t wa = a.high_hz - a.low_hz;
        const std::int64_t wb = b.high_hz - b.low_hz;
        INFO(a.name << " then " << b.name);
        CHECK(wa >= wb);
        if (wa == wb) {
            CHECK(a.low_hz <= b.low_hz);
            if (a.low_hz == b.low_hz) {
                CHECK(a.name <= b.name);
            }
        }
    }

    // The mask applies here too.
    for (const Band* hit : bands_overlapping(0, 300'000'000'000, kRegionCA)) {
        CHECK((hit->regions & kRegionCA) != 0);
    }
}

TEST_CASE("an R820T reaches VHF and UHF and not HF", "[bands]")
{
    for (const Band& band : kBands) {
        INFO(band.name);
        const bool inside = band.centre_hz >= kR820tLow && band.centre_hz <= kR820tHigh;
        CHECK(band_reachable(band, kR820tLow, kR820tHigh) == inside);
    }
}

// Rejects greying everything out against an unanswered range. Every source
// starts there, and a menu that is dead until the first answer arrives reads
// as a device that tunes nothing.
TEST_CASE("an unknown range refuses nothing", "[bands]")
{
    for (const Band& band : kBands) {
        CHECK(band_reachable(band, 0, 0));
        CHECK(band_reachable(band, 5, 5));
    }
}

// The edges of the range are inside it, the same rule the engine applies.
TEST_CASE("a centre exactly on the range's edge is reachable", "[bands]")
{
    const Band& band = kBands.front();
    CHECK(band_reachable(band, band.centre_hz, band.centre_hz + 1));
    CHECK(band_reachable(band, band.centre_hz - 1, band.centre_hz));
    CHECK_FALSE(band_reachable(band, band.centre_hz + 1, band.centre_hz + 2));
}
