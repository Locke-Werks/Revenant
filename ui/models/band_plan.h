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

#include <array>
#include <cstdint>
#include <string_view>

namespace revenant::ui {

struct Band {
    std::string_view group;
    std::string_view name;
    std::int64_t low_hz = 0;
    std::int64_t high_hz = 0;
    std::int64_t centre_hz = 0;

    // A demodulator name as core/engine/vrx.h spells it.
    std::string_view mode;

    // Shown as a one-click shortcut beside the dial. The rest are in the menu.
    bool favourite = false;
};

// Grouped, and in frequency order within a group, which is the order the menu
// draws them in.
inline constexpr std::array kBands{
    Band{"Broadcast", "MW AM", 530'000, 1'700'000, 1'000'000, "am", false},
    Band{"Broadcast", "FM", 88'000'000, 108'000'000, 98'100'000, "wfm", true},

    Band{"Shortwave broadcast", "SW 120 m", 2'300'000, 2'495'000, 2'400'000, "am", false},
    Band{"Shortwave broadcast", "SW 90 m", 3'200'000, 3'400'000, 3'300'000, "am", false},
    Band{"Shortwave broadcast", "SW 75 m", 3'900'000, 4'000'000, 3'950'000, "am", false},
    Band{"Shortwave broadcast", "SW 60 m", 4'750'000, 5'060'000, 4'900'000, "am", false},
    Band{"Shortwave broadcast", "SW 49 m", 5'900'000, 6'200'000, 6'050'000, "am", false},
    Band{"Shortwave broadcast", "SW 41 m", 7'200'000, 7'450'000, 7'325'000, "am", false},
    Band{"Shortwave broadcast", "SW 31 m", 9'400'000, 9'900'000, 9'650'000, "am", false},
    Band{"Shortwave broadcast", "SW 25 m", 11'600'000, 12'100'000, 11'850'000, "am", false},
    Band{"Shortwave broadcast", "SW 22 m", 13'570'000, 13'870'000, 13'720'000, "am", false},
    Band{"Shortwave broadcast", "SW 19 m", 15'100'000, 15'800'000, 15'450'000, "am", false},
    Band{"Shortwave broadcast", "SW 16 m", 17'480'000, 17'900'000, 17'690'000, "am", false},
    Band{"Shortwave broadcast", "SW 15 m", 18'900'000, 19'020'000, 18'960'000, "am", false},
    Band{"Shortwave broadcast", "SW 13 m", 21'450'000, 21'850'000, 21'650'000, "am", false},
    Band{"Shortwave broadcast", "SW 11 m", 25'670'000, 26'100'000, 25'885'000, "am", false},

    // Lower sideband below 10 MHz and upper above it, which is the phone
    // convention and not a rule; 30 m carries no phone at all.
    Band{"Amateur HF", "160 m", 1'800'000, 2'000'000, 1'900'000, "lsb", false},
    Band{"Amateur HF", "80 m", 3'500'000, 4'000'000, 3'750'000, "lsb", false},
    Band{"Amateur HF", "60 m", 5'330'500, 5'406'400, 5'357'000, "usb", false},
    Band{"Amateur HF", "40 m", 7'000'000, 7'300'000, 7'150'000, "lsb", false},
    Band{"Amateur HF", "30 m", 10'100'000, 10'150'000, 10'125'000, "cw", false},
    Band{"Amateur HF", "20 m", 14'000'000, 14'350'000, 14'175'000, "usb", false},
    Band{"Amateur HF", "17 m", 18'068'000, 18'168'000, 18'118'000, "usb", false},
    Band{"Amateur HF", "15 m", 21'000'000, 21'450'000, 21'225'000, "usb", false},
    Band{"Amateur HF", "12 m", 24'890'000, 24'990'000, 24'940'000, "usb", false},
    Band{"Amateur HF", "10 m", 28'000'000, 29'700'000, 28'500'000, "usb", false},

    Band{"Amateur VHF and up", "6 m", 50'000'000, 54'000'000, 50'200'000, "usb", false},
    Band{"Amateur VHF and up", "2 m", 144'000'000, 148'000'000, 145'000'000, "nfm", true},
    Band{"Amateur VHF and up", "1.25 m", 222'000'000, 225'000'000, 223'500'000, "nfm", false},
    Band{"Amateur VHF and up", "70 cm", 420'000'000, 450'000'000, 435'000'000, "nfm", true},
    Band{"Amateur VHF and up", "33 cm", 902'000'000, 928'000'000, 915'000'000, "nfm", false},
    Band{"Amateur VHF and up", "23 cm", 1'240'000'000, 1'300'000'000, 1'270'000'000, "nfm",
         false},

    Band{"Aviation and marine", "Airband", 118'000'000, 137'000'000, 124'000'000, "am", true},
    Band{"Aviation and marine", "Marine VHF", 156'000'000, 162'025'000, 156'800'000, "nfm",
         false},
    Band{"Aviation and marine", "ADS-B", 1'087'000'000, 1'093'000'000, 1'090'000'000, "raw",
         false},

    Band{"Land mobile and weather", "NOAA", 162'400'000, 162'550'000, 162'475'000, "nfm", true},
    Band{"Land mobile and weather", "GMRS / FRS", 462'550'000, 467'725'000, 462'562'500, "nfm",
         true},
    Band{"Land mobile and weather", "460-464 MHz", 460'000'000, 464'000'000, 462'000'000, "nfm",
         false},

    Band{"ISM", "ISM 433", 433'050'000, 434'790'000, 433'920'000, "am", false},
    Band{"ISM", "ISM 915", 902'000'000, 928'000'000, 915'000'000, "am", false},
};

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
