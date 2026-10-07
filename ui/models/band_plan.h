// The bands the tuning controls offer, with their edges, where a click puts
// the front end, and the mode a receiver in them usually wants.
//
// WHY A TABLE IN A HEADER AND NOT A LIST IN QML. It was five entries written
// inline in the tuning row. A band plan is data with a source, and the two
// things that go wrong with one are both checkable: an edge copied wrong, and
// a place-the-front-end-here frequency that is not inside the band it names.
// So it lives here, ui/tests asserts its shape, and the QML draws whatever
// this says.
//
// WHERE THE NUMBERS COME FROM
//
// US amateur allocations are FCC Part 97, section 97.301, for a General or
// higher licensee; the 60 m entry spans the five channels 97.303(h) sets out
// and is not a band an operator may use edge to edge. Broadcast, aviation and
// maritime edges are the ITU Radio Regulations, Article 5, Region 2, with the
// shortwave broadcasting bands as that article allocates them to the
// broadcasting service. Maritime VHF channels are Appendix 18. NOAA Weather
// Radio is the seven NWS frequencies. GMRS and FRS are FCC Part 95. The ISM
// bands are ITU RR 5.138 (433 MHz, Region 1, used in the US under Part 15) and
// 5.150 (902 to 928 MHz, Region 2). ADS-B is the 1090 MHz Mode S downlink.
//
// These were transcribed for a tuning menu, not for compliance. Check the
// current edition of the regulation before relying on an edge to decide
// whether a transmission is lawful.
//
// THE CENTRE IS WHERE THE FRONT END GOES, not the middle of the allocation.
// A 2.4 MS/s dongle sees 2.4 MHz, so on a band wider than that the centre is
// the part of it worth landing on: 98.1 MHz for FM, 145 MHz for 2 m, which
// is where the shortcut has always put it, 462.5625 MHz for GMRS channel 1, which is the
// standing real-radio test frequency in docs/.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace revenant::ui {

// Which administrations a row holds for. A bit each, so a row identical on
// both sides of the border is one row and not two.
enum Region : std::uint32_t { kRegionUS = 1u << 0, kRegionCA = 1u << 1 };
inline constexpr std::uint32_t kRegionUSCA = kRegionUS | kRegionCA;

// An allocation is a whole band, a sub-band a segment of one, and a channel a
// single assignment inside one. They overlap on purpose.
enum class BandKind : std::uint8_t { kAllocation, kSubBand, kChannel };

enum class BandColour : std::uint8_t {
    kAmateur, kBroadcast, kAviation, kMarine, kLandMobile, kPublicSafety,
    kPersonal, kWeather, kSatellite, kIsm, kGovernment, kOther };

struct Band {
    std::string_view group;
    std::string_view name;
    std::int64_t low_hz = 0;
    std::int64_t high_hz = 0;
    std::int64_t centre_hz = 0;

    // A demodulator name as core/engine/vrx.h spells it, "" if none fits.
    std::string_view mode;

    // Shown as a one-click shortcut beside the dial. The rest are in the menu.
    bool favourite = false;

    std::uint32_t regions = kRegionUSCA;
    BandKind kind = BandKind::kAllocation;
    BandColour colour = BandColour::kOther;

    // The rule or plan the edges were read from, e.g. "47 CFR 97.301".
    std::string_view source;

    // "" for a top-level row; for a channel, the submenu it is listed under.
    std::string_view parent;
};

// The rows live one file per service under models/band_plan/, each holding
// nothing but initialisers, so the files can be sourced and reviewed apart.
// Grouped, and in frequency order within a group, which is the order the menu
// draws them in.
inline constexpr auto kBands = std::to_array<Band>({
#include "band_plan/amateur.inc"
#include "band_plan/personal_radio.inc"
#include "band_plan/aviation.inc"
#include "band_plan/marine.inc"
#include "band_plan/land_mobile_public_safety.inc"
#include "band_plan/broadcast.inc"
#include "band_plan/weather_sat_space.inc"
#include "band_plan/ism_unlicensed.inc"
#include "band_plan/canada_only.inc"
});

// The rows that hold in any of the regions in the mask, in table order.
[[nodiscard]] inline std::vector<const Band*> bands_for_region(std::uint32_t mask)
{
    std::vector<const Band*> out;
    for (const Band& band : kBands) {
        if ((band.regions & mask) != 0) {
            out.push_back(&band);
        }
    }
    return out;
}

// The rows in the mask that share any frequency with [lo_hz, hi_hz], widest
// first so a drawing pass lays allocations under their sub-bands and channels.
// Ties go to the lower edge and then the name, so the order never depends on
// where a row sits in the table.
[[nodiscard]] inline std::vector<const Band*> bands_overlapping(std::int64_t lo_hz,
                                                                std::int64_t hi_hz,
                                                                std::uint32_t mask)
{
    std::vector<const Band*> out;
    for (const Band& band : kBands) {
        if ((band.regions & mask) != 0 && band.low_hz < hi_hz && band.high_hz > lo_hz) {
            out.push_back(&band);
        }
    }
    std::sort(out.begin(), out.end(), [](const Band* a, const Band* b) {
        const std::int64_t wa = a->high_hz - a->low_hz;
        const std::int64_t wb = b->high_hz - b->low_hz;
        if (wa != wb) {
            return wa > wb;
        }
        if (a->low_hz != b->low_hz) {
            return a->low_hz < b->low_hz;
        }
        return a->name < b->name;
    });
    return out;
}

// Whether the front end can be put at this band's centre.
//
// The centre and not the edges, because the centre is what a click asks for:
// a band whose far edge the tuner cannot reach is still worth landing on if
// the place a click lands is inside the range. A range that is not a range is
// every source that has not said what it tunes, and nothing is refused on it;
// the engine refuses anything it cannot do in its own words.
[[nodiscard]] constexpr bool band_reachable(const Band& band, std::int64_t tune_low_hz,
                                            std::int64_t tune_high_hz)
{
    if (tune_high_hz <= tune_low_hz) {
        return true;
    }
    return band.centre_hz >= tune_low_hz && band.centre_hz <= tune_high_hz;
}

}  // namespace revenant::ui
