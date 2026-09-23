// The span displays' colour map: a level in [0, 1] to a colour, with
// perceptual lightness rising in a straight line from the window's background
// to a warm near-white.
//
// WHY IT WAS REDONE. The map it replaces was seven sRGB stops whose comment
// used to say they were "chosen for monotone luminance", and they were not: Rec. 709 luma fell from
// about 182 at the yellow-green stop to about 168 at the orange one, measured
// when ui/tests/test_spectrum_scale.cpp was first written on 2026-09-22. A dip
// in lightness is a false edge, and on a waterfall a false edge reads as a
// band boundary that is not there. The owner asked the same day for a map
// whose lightness rises monotonically from the lowest level to the highest.
//
// HOW IT IS BUILT
//
// In OKLab, Björn Ottosson's perceptual colour space ("A perceptual color
// space for image processing", bottosson.github.io/posts/oklab, 2020), using
// the matrices published there to go from OKLab to linear sRGB, and the sRGB
// transfer function from IEC 61966-2-1 to go from linear to the 8-bit values
// the displays upload. Nothing here is copied from another project's table:
// the path is chosen below and every colour is computed from it.
//
// The path is in OKLCh, the polar form of OKLab. L, the lightness, rises
// LINEARLY with the level from the lightness of the window's own background,
// Theme.background #06080e, to 0.97. Hue and chroma follow a handful of
// control points and are interpolated between them:
//
//   level  0      the background's own hue and chroma, so an empty band
//                 recedes into the window rather than sitting on it as a slab
//   0.20   blue, hue 250
//   0.45   teal, hue 200
//   0.70   green, hue 135, the most saturated point
//   0.88   yellow, hue 100
//   1      a warm near-white, hue 95 at low chroma
//
// Where a chroma is outside what sRGB can show at that lightness it is reduced,
// by bisection, until the colour fits. Only chroma gives: lightness is never
// traded for colour, which is the whole point of the map.
//
// NOT THROUGH MAGENTA, which a blue, magenta, orange, yellow path would be and
// which suits a heat map. Magenta is the detection colour, Theme.inkTune, the
// one hue chosen because the map never produces it, and the detection boxes
// are drawn over this map. A map that passed through it would put boxes on
// top of pixels of their own colour. Blue through green to yellow keeps the
// map on the blue-yellow axis that the common red-green colour vision
// deficiencies preserve, and the straight lightness ramp carries the order of
// levels for everyone, including those who see no hue at all.
//
// This header holds no Qt; ui/tests links it.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace revenant::ui::colour_map {

struct Lab {
    double l = 0.0;
    double a = 0.0;
    double b = 0.0;
};

struct Srgb8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

// Theme.background in qml/Theme.qml, #06080e. The bottom of the map is this
// colour exactly, so the two have to be changed together.
inline constexpr Srgb8 kBottom{6, 8, 14};

// The lightness the top of the map reaches, and the hue and chroma it has
// there.
inline constexpr double kTopLightness = 0.97;
inline constexpr double kTopHue = 95.0;
inline constexpr double kTopChroma = 0.045;

// ---------------------------------------------------------------------------
// sRGB and OKLab, per the definitions cited above.
// ---------------------------------------------------------------------------

[[nodiscard]] inline double srgb_to_linear(double encoded)
{
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}

[[nodiscard]] inline double linear_to_srgb(double linear)
{
    const double c = std::clamp(linear, 0.0, 1.0);
    return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

// Linear sRGB in [0, 1] to OKLab.
[[nodiscard]] inline Lab linear_srgb_to_oklab(double r, double g, double b)
{
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return Lab{0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
               1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
               0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

struct LinearRgb {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

// OKLab to linear sRGB, which may be outside [0, 1] for a colour sRGB cannot
// show.
[[nodiscard]] inline LinearRgb oklab_to_linear_srgb(const Lab& lab)
{
    const double l = std::pow(lab.l + 0.3963377774 * lab.a + 0.2158037573 * lab.b, 3.0);
    const double m = std::pow(lab.l - 0.1055613458 * lab.a - 0.0638541728 * lab.b, 3.0);
    const double s = std::pow(lab.l - 0.0894841775 * lab.a - 1.2914855480 * lab.b, 3.0);
    return LinearRgb{4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
                     -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                     -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s};
}

[[nodiscard]] inline Lab srgb8_to_oklab(const Srgb8& colour)
{
    return linear_srgb_to_oklab(srgb_to_linear(colour.r / 255.0),
                                srgb_to_linear(colour.g / 255.0),
                                srgb_to_linear(colour.b / 255.0));
}

// ---------------------------------------------------------------------------
// The path.
// ---------------------------------------------------------------------------

struct ControlPoint {
    double level;
    double hue_degrees;
    double chroma;
};

// Everything but the two ends, which come from kBottom and the kTop constants.
inline constexpr std::array<ControlPoint, 4> kInterior{{
    {0.20, 250.0, 0.10},
    {0.45, 200.0, 0.12},
    {0.70, 135.0, 0.17},
    {0.88, 100.0, 0.15},
}};

// Where the path wants to be at a level, before any chroma is given up to fit
// sRGB: L, C and h.
struct Lch {
    double l = 0.0;
    double c = 0.0;
    double h = 0.0;
};

[[nodiscard]] inline Lch path_at(double level)
{
    const double t = std::clamp(level, 0.0, 1.0);
    const Lab bottom = srgb8_to_oklab(kBottom);
    const double bottom_chroma = std::hypot(bottom.a, bottom.b);
    const double bottom_hue = std::atan2(bottom.b, bottom.a) * 180.0 / 3.14159265358979323846;

    std::array<ControlPoint, kInterior.size() + 2> points{};
    points.front() = ControlPoint{0.0, bottom_hue < 0.0 ? bottom_hue + 360.0 : bottom_hue,
                                  bottom_chroma};
    std::copy(kInterior.begin(), kInterior.end(), points.begin() + 1);
    points.back() = ControlPoint{1.0, kTopHue, kTopChroma};

    Lch out;
    out.l = bottom.l + (kTopLightness - bottom.l) * t;
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (t <= points[i].level) {
            const ControlPoint& from = points[i - 1];
            const ControlPoint& to = points[i];
            const double u = (t - from.level) / (to.level - from.level);
            out.h = from.hue_degrees + (to.hue_degrees - from.hue_degrees) * u;
            out.c = from.chroma + (to.chroma - from.chroma) * u;
            return out;
        }
    }
    out.h = kTopHue;
    out.c = kTopChroma;
    return out;
}

[[nodiscard]] inline Lab lch_to_lab(double l, double c, double h_degrees)
{
    const double h = h_degrees * 3.14159265358979323846 / 180.0;
    return Lab{l, c * std::cos(h), c * std::sin(h)};
}

[[nodiscard]] inline bool in_gamut(const LinearRgb& rgb)
{
    constexpr double kSlack = 1e-7;
    return rgb.r >= -kSlack && rgb.r <= 1.0 + kSlack && rgb.g >= -kSlack &&
           rgb.g <= 1.0 + kSlack && rgb.b >= -kSlack && rgb.b <= 1.0 + kSlack;
}

// The colour at a level, in OKLab, after any chroma sRGB cannot show has been
// given up. Its L is the path's L exactly: this is the unquantised map, which
// is what the lightness test samples.
[[nodiscard]] inline Lab oklab_at(double level)
{
    const Lch want = path_at(level);
    double low = 0.0;
    double high = want.c;
    if (in_gamut(oklab_to_linear_srgb(lch_to_lab(want.l, want.c, want.h)))) {
        low = want.c;
    } else {
        for (int i = 0; i < 40; ++i) {
            const double mid = 0.5 * (low + high);
            if (in_gamut(oklab_to_linear_srgb(lch_to_lab(want.l, mid, want.h)))) {
                low = mid;
            } else {
                high = mid;
            }
        }
    }
    return lch_to_lab(want.l, low, want.h);
}

[[nodiscard]] inline Srgb8 to_srgb8(const Lab& lab)
{
    const LinearRgb linear = oklab_to_linear_srgb(lab);
    const auto channel = [](double c) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(linear_to_srgb(c), 0.0, 1.0) * 255.0));
    };
    return Srgb8{channel(linear.r), channel(linear.g), channel(linear.b)};
}

// ---------------------------------------------------------------------------
// The table the displays read.
// ---------------------------------------------------------------------------

// Entries in the table. A waterfall row colours thousands of pixels a frame,
// so the map is computed once into this and read by index; 1024 is finer
// than the eight bits a channel is uploaded in can show.
inline constexpr std::size_t kTableSize = 1024;

using Table = std::array<Srgb8, kTableSize>;

[[nodiscard]] inline Table build_table()
{
    Table table{};
    for (std::size_t i = 0; i < kTableSize; ++i) {
        table[i] = to_srgb8(oklab_at(static_cast<double>(i) / (kTableSize - 1)));
    }
    // The ends exactly, rather than whatever a round trip through OKLab and
    // back lands on, because the bottom has to be the window's background to
    // the bit.
    table.front() = kBottom;
    return table;
}

// Built on first use and shared, since C++11 statics are initialised once and
// thread-safely.
[[nodiscard]] inline const Table& table()
{
    static const Table built = build_table();
    return built;
}

[[nodiscard]] inline Srgb8 colour_at(float level)
{
    const float t = std::clamp(level, 0.0F, 1.0F);
    const auto index = static_cast<std::size_t>(std::lround(t * static_cast<float>(kTableSize - 1)));
    return table()[index];
}

}  // namespace revenant::ui::colour_map
