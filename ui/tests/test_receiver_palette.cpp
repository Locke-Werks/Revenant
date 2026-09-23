// The receiver colours in models/receiver_palette.h, held to the claim that
// eight receivers stay distinguishable in normal vision and in simulated
// protan, deutan and tritan vision, and that none of them is magenta.
//
// Each case names the wrong palette or the wrong arithmetic it rejects, on the
// convention test_receiver_marker.cpp states.

#include <cstddef>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "models/receiver_palette.h"

using Catch::Approx;
using revenant::ui::derive_receiver_palette;
using revenant::ui::kMinReceiverColourDelta;
using revenant::ui::kMinThemeDelta;
using revenant::ui::kMinTuneDelta;
using revenant::ui::kReceiverPalette;
using revenant::ui::kReceiverPaletteSize;
using revenant::ui::kThemeMeaningColours;
using revenant::ui::kTuneColour;
using revenant::ui::kTuneHueExclusionDeg;
using revenant::ui::palette_min_delta;
using revenant::ui::rgb8_hex;
using revenant::ui::Rgb8;
namespace colour = revenant::ui::colour;

namespace {

[[nodiscard]] std::vector<Rgb8> table()
{
    return {kReceiverPalette.begin(), kReceiverPalette.end()};
}

}  // namespace

// Rejects a CIEDE2000 with a transcription slip, which is the usual way this
// formula goes wrong: the hue-mean branch, the rotation term, or G. Four
// pairs from Sharma, Wu and Dalal's supplementary data, chosen to reach the
// hue-wrap branch (1), a zero-chroma colour (7), a large difference (17) and
// an ordinary small one (25). Their published answers to four places.
TEST_CASE("CIEDE2000 agrees with Sharma, Wu and Dalal's test data")
{
    CHECK(colour::ciede2000({50.0, 2.6772, -79.7751}, {50.0, 0.0, -82.7485}) ==
          Approx(2.0425).margin(1e-4));
    CHECK(colour::ciede2000({50.0, 0.0, 0.0}, {50.0, -1.0, 2.0}) ==
          Approx(2.3669).margin(1e-4));
    CHECK(colour::ciede2000({50.0, 2.5, 0.0}, {73.0, 25.0, -18.0}) ==
          Approx(27.1492).margin(1e-4));
    CHECK(colour::ciede2000({60.2574, -34.0099, 36.2677}, {60.4626, -34.1751, 39.4387}) ==
          Approx(1.2644).margin(1e-4));
}

// Rejects a simulation applied to gamma-encoded values, or a matrix row
// copied into the wrong place. Each Machado matrix maps white to white,
// because every row sums to one, and a dichromat sees grey as grey. A matrix
// with a row out of order does not.
TEST_CASE("each simulated vision keeps white and grey where they are")
{
    for (const colour::Vision vision : colour::kVisions) {
        const colour::Linear grey = colour::simulate({0.5, 0.5, 0.5}, vision);
        CHECK(grey.r == Approx(0.5).margin(1e-5));
        CHECK(grey.g == Approx(0.5).margin(1e-5));
        CHECK(grey.b == Approx(0.5).margin(1e-5));
    }
}

// Rejects a simulation that is not simulating anything. Red and green, which
// every protanope and deuteranope confuses, are far apart in normal vision and
// close in both of those, and blue and green are what a tritanope confuses.
TEST_CASE("the simulated visions collapse the pairs each one is known to confuse")
{
    const auto views = [](Rgb8 c) { return colour::views_of(c); };
    const colour::Views red = views({0xd0, 0x40, 0x30});
    const colour::Views green = views({0x60, 0x90, 0x20});
    const double normal = colour::delta_in(red, green, colour::Vision::Normal);
    CHECK(normal > 40.0);
    CHECK(colour::delta_in(red, green, colour::Vision::Protan) < normal / 3.0);
    CHECK(colour::delta_in(red, green, colour::Vision::Deutan) < normal / 3.0);

    const colour::Views blue = views({0x30, 0x70, 0xd0});
    const colour::Views teal = views({0x20, 0x98, 0x90});
    CHECK(colour::delta_in(blue, teal, colour::Vision::Tritan) <
          colour::delta_in(blue, teal, colour::Vision::Normal) / 2.0);
}

// THE CLAIM. Rejects any palette in which two receivers can be mistaken for
// each other by somebody in one of the four visions: the placeholder that
// preceded this one spread eight hues at similar lightness, and under deutan
// its first colour, the azure, and its last, a pink, were 1.9 apart.
TEST_CASE("every pair of the eight stays apart in all four visions")
{
    const std::vector<Rgb8> palette = table();
    REQUIRE(palette.size() == kReceiverPaletteSize);
    for (const colour::Vision vision : colour::kVisions) {
        INFO("vision " << colour::vision_name(vision));
        CHECK(palette_min_delta(palette, vision) >= kMinReceiverColourDelta);
    }

    // And the old palette fails it, so the threshold is one that bites.
    const std::vector<Rgb8> placeholder{
        {0x80, 0xc4, 0xff}, {0xff, 0xa9, 0x4d}, {0x69, 0xdb, 0x7c}, {0xff, 0xd4, 0x3b},
        {0xb1, 0x97, 0xfc}, {0xff, 0x87, 0x87}, {0x63, 0xe6, 0xbe}, {0xe5, 0x99, 0xf7}};
    CHECK(palette_min_delta(placeholder, colour::Vision::Deutan) < kMinReceiverColourDelta);
}

// Rejects a receiver colour that reads as a detection. The placeholder's
// eighth colour was a pink 24 degrees of hue from the detection magenta.
TEST_CASE("no receiver colour is magenta or near the detection colour")
{
    const double tune_hue = colour::linear_to_oklch(colour::to_linear(kTuneColour)).h_deg;
    const colour::Lab tune = colour::to_lab(colour::to_linear(kTuneColour));
    for (const Rgb8& c : kReceiverPalette) {
        INFO("colour " << rgb8_hex(c));
        const colour::Linear linear = colour::to_linear(c);
        CHECK(colour::hue_distance(colour::linear_to_oklch(linear).h_deg, tune_hue) >=
              kTuneHueExclusionDeg);
        CHECK(colour::ciede2000(colour::to_lab(linear), tune) >= kMinTuneDelta);
        for (const Rgb8& meaning : kThemeMeaningColours) {
            CHECK(colour::ciede2000(colour::to_lab(linear),
                                    colour::to_lab(colour::to_linear(meaning))) >=
                  kMinThemeDelta);
        }
    }
    const Rgb8 placeholder_pink{0xe5, 0x99, 0xf7};
    CHECK_FALSE(revenant::ui::receiver_colour_allowed(placeholder_pink));
}

// Rejects a table edited by hand away from the procedure that justifies it.
// The derivation is the argument; the table is only its output.
TEST_CASE("the table is what the derivation produces")
{
    const std::vector<Rgb8> derived = derive_receiver_palette();
    REQUIRE(derived.size() == kReceiverPaletteSize);
    for (std::size_t i = 0; i < kReceiverPaletteSize; ++i) {
        INFO("slot " << i << ": table " << rgb8_hex(kReceiverPalette[i]) << ", derived "
                     << rgb8_hex(derived[i]));
        CHECK(derived[i] == kReceiverPalette[i]);
    }
}

// Rejects a first colour that moved. The azure is what the receiver marker
// has been drawn in since there was one, and an operator's first receiver
// should look as it did.
TEST_CASE("the first receiver keeps the azure")
{
    const double d = colour::ciede2000(colour::to_lab(colour::to_linear(kReceiverPalette[0])),
                                       colour::to_lab(colour::to_linear({0x80, 0xc4, 0xff})));
    CHECK(d < 3.0);
    CHECK(rgb8_hex(kReceiverPalette[0]) == "#81bef4");
}
