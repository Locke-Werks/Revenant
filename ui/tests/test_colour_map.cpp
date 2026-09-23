// render/colour_map.h: the span displays' colour map, in OKLab.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows. The map this replaced claimed monotone luminance in a
// comment and dipped between two of its stops; nothing tested it, and the
// picture it drew looked fine, which is how a false band edge survives.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

#include "render/colour_map.h"
#include "render/spectrum_scale.h"

namespace cm = revenant::ui::colour_map;
using Catch::Matchers::WithinAbs;

// Rejects any dip in lightness, which is a false edge. Sampled at 1024 points
// on the map as computed, before the eight-bit rounding the displays upload,
// because that is where the lightness is defined; the rounded table is
// checked below against what rounding can do.
TEST_CASE("lightness strictly increases across the map", "[colourmap]")
{
    double previous = -1.0;
    for (int i = 0; i < 1024; ++i) {
        const cm::Lab lab = cm::oklab_at(static_cast<double>(i) / 1023.0);
        INFO(i);
        CHECK(lab.l > previous);
        previous = lab.l;
    }
}

// And linearly, which is what the header claims and what makes equal steps
// in level equal steps in how much brighter a pixel looks.
TEST_CASE("lightness is linear in the level", "[colourmap]")
{
    const double bottom = cm::srgb8_to_oklab(cm::kBottom).l;
    for (const double level : {0.0, 0.1, 0.33, 0.5, 0.9, 1.0}) {
        CHECK_THAT(cm::oklab_at(level).l,
                   WithinAbs(bottom + (cm::kTopLightness - bottom) * level, 1e-9));
    }
}

// The eight-bit colours the displays draw, measured back into OKLab: rounding
// a channel can nudge lightness down by a hair between neighbouring entries,
// never by more than half a rounding step, and across a coarser sampling the
// rise is strict.
TEST_CASE("the drawn colours keep the order after rounding", "[colourmap]")
{
    double previous = -1.0;
    for (int i = 0; i < 1024; ++i) {
        const auto c = cm::colour_at(static_cast<float>(i) / 1023.0F);
        const double l = cm::srgb8_to_oklab(c).l;
        INFO(i);
        CHECK(l > previous - 0.002);
        previous = l;
    }

    previous = -1.0;
    for (int i = 0; i < 64; ++i) {
        const double l =
            cm::srgb8_to_oklab(cm::colour_at(static_cast<float>(i) / 63.0F)).l;
        INFO(i);
        CHECK(l > previous);
        previous = l;
    }
}

// The bottom of the map is the window's own background, #06080e in
// qml/Theme.qml, so an empty band recedes into the window.
TEST_CASE("the bottom is the window background and the top is near white",
          "[colourmap]")
{
    const auto bottom = cm::colour_at(0.0F);
    CHECK(bottom.r == 0x06);
    CHECK(bottom.g == 0x08);
    CHECK(bottom.b == 0x0e);

    const auto top = cm::colour_at(1.0F);
    CHECK(cm::srgb8_to_oklab(top).l > 0.96);
    CHECK(top.r >= 240);
    CHECK(top.g >= 235);
    CHECK(top.b >= 200);

    // Beyond the ends clamps rather than wraps.
    const auto above = cm::colour_at(4.0F);
    CHECK(above.r == top.r);
    CHECK(above.g == top.g);
    CHECK(above.b == top.b);
}

// Rejects a map through the detection colour. Theme.inkTune, #ff58c8, is the
// hue the map is kept away from so a box drawn over the waterfall never sits
// on pixels of its own colour. Its OKLCh hue is about 350 degrees; nothing on
// the map comes within 60 degrees of it with visible chroma.
TEST_CASE("the map never passes through the detection magenta", "[colourmap]")
{
    const cm::Lab tune = cm::srgb8_to_oklab(cm::Srgb8{0xff, 0x58, 0xc8});
    const double tune_hue = std::atan2(tune.b, tune.a) * 180.0 / 3.14159265358979323846;
    for (int i = 0; i < 256; ++i) {
        const cm::Lab lab = cm::oklab_at(static_cast<double>(i) / 255.0);
        const double chroma = std::hypot(lab.a, lab.b);
        if (chroma < 0.03) {
            continue;
        }
        double hue = std::atan2(lab.b, lab.a) * 180.0 / 3.14159265358979323846;
        double gap = std::fabs(hue - tune_hue);
        gap = std::fmin(gap, 360.0 - gap);
        INFO(i << " hue " << hue);
        CHECK(gap > 60.0);
    }
}

// The displays read the map through spectrum_scale.h's colour_at, so the two
// must be the same function.
TEST_CASE("the span displays read this map", "[colourmap]")
{
    for (const float level : {0.0F, 0.25F, 0.5F, 0.77F, 1.0F}) {
        const auto direct = cm::colour_at(level);
        const auto shown = revenant::ui::colour_at(level);
        CHECK(shown.r == direct.r);
        CHECK(shown.g == direct.g);
        CHECK(shown.b == direct.b);
    }
}
