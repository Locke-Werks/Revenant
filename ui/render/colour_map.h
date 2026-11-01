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
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

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

// The bottom under the OLED black interface, where Theme.background is
// #000000: an empty band is then an unlit pixel. The lightness ramp starts
// from whichever bottom is in force, so it stays linear either way.
inline constexpr Srgb8 kBlackBottom{0, 0, 0};

// Whether the black bottom is in force; set from ScaleSettings with the theme.
inline std::atomic<bool> g_black{false};

[[nodiscard]] inline bool black_bottom()
{
    return g_black.load(std::memory_order_relaxed);
}

inline void set_black_bottom(bool on)
{
    g_black.store(on, std::memory_order_relaxed);
}

[[nodiscard]] inline Srgb8 bottom(bool black)
{
    return black ? kBlackBottom : kBottom;
}

// The bottom in force, which every display fills an empty row with.
[[nodiscard]] inline Srgb8 bottom()
{
    return bottom(black_bottom());
}

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

// ---------------------------------------------------------------------------
// Themes
// ---------------------------------------------------------------------------
//
// The operator picks one of these; the owner asked for a wide choice on
// 2026-10-31. Every theme is a path of the same kind as the one described at
// the top: the lightness ramp is shared and only hue and chroma differ. Three
// things depend on that and would break for a theme that did otherwise:
//
//   the ramp is linear, so equal steps in level look like equal steps in
//   brightness whatever the theme;
//   shaders/hdr_map.frag recovers the level from a texel's lightness against
//   kBottom and kTopLightness, so a theme with another ramp would be lifted
//   at the wrong levels on an HDR display;
//   the bottom is the window background, so an empty band recedes.
//
// And every theme keeps more than 60 degrees of hue away from the detection
// magenta wherever it has visible chroma, for the reason given above. That
// rules out the purple-through-red heat maps; the warm themes start at orange.
//
// The first stretch, from the background to a theme's first control point, is
// interpolated in a and b rather than in hue, so a theme whose first hue is
// far round the circle from the background's blue does not sweep through
// every hue between them on the way; it passes near grey instead, where the
// chroma is small anyway.

inline constexpr std::size_t kMaxInterior = 6;

struct Theme {
    const char* id;    // stored in the settings; never change one
    const char* name;  // shown to the operator
    std::size_t count;
    std::array<ControlPoint, kMaxInterior> interior;
    double top_hue;
    double top_chroma;
};

inline constexpr std::array<Theme, 14> kThemes{{
    // The original map, and the default.
    {"aurora", "Aurora", 4,
     {{{0.20, 250.0, 0.10}, {0.45, 200.0, 0.12}, {0.70, 135.0, 0.17}, {0.88, 100.0, 0.15}}},
     kTopHue, kTopChroma},
    {"glacier", "Glacier", 3,
     {{{0.25, 262.0, 0.11}, {0.55, 225.0, 0.12}, {0.82, 205.0, 0.08}}},
     200.0, 0.025},
    {"deep_sea", "Deep sea", 4,
     {{{0.20, 255.0, 0.10}, {0.45, 235.0, 0.12}, {0.70, 190.0, 0.13}, {0.88, 170.0, 0.10}}},
     160.0, 0.03},
    {"phosphor", "Phosphor", 3,
     {{{0.25, 150.0, 0.08}, {0.60, 145.0, 0.20}, {0.85, 140.0, 0.18}}},
     135.0, 0.06},
    {"amber", "Amber", 3,
     {{{0.25, 65.0, 0.07}, {0.60, 70.0, 0.15}, {0.85, 78.0, 0.15}}},
     90.0, 0.05},
    {"ember", "Ember", 4,
     {{{0.20, 55.0, 0.06}, {0.45, 58.0, 0.12}, {0.70, 68.0, 0.17}, {0.88, 85.0, 0.16}}},
     95.0, 0.05},
    {"dusk", "Dusk", 4,
     {{{0.20, 265.0, 0.11}, {0.45, 215.0, 0.08}, {0.70, 90.0, 0.12}, {0.88, 75.0, 0.12}}},
     80.0, 0.04},
    {"prism", "Prism", 5,
     {{{0.15, 262.0, 0.12},
       {0.35, 215.0, 0.13},
       {0.55, 145.0, 0.18},
       {0.75, 105.0, 0.17},
       {0.90, 65.0, 0.14}}},
     70.0, 0.05},
    {"toxic", "Toxic", 3,
     {{{0.25, 195.0, 0.08}, {0.60, 130.0, 0.20}, {0.85, 110.0, 0.19}}},
     105.0, 0.07},
    {"cobalt", "Cobalt", 3,
     {{{0.25, 262.0, 0.13}, {0.60, 255.0, 0.15}, {0.85, 245.0, 0.08}}},
     240.0, 0.02},
    {"copper", "Copper", 3,
     {{{0.25, 55.0, 0.05}, {0.60, 60.0, 0.09}, {0.85, 70.0, 0.08}}},
     80.0, 0.03},
    {"jade", "Jade", 3,
     {{{0.25, 200.0, 0.07}, {0.60, 170.0, 0.12}, {0.85, 160.0, 0.08}}},
     150.0, 0.03},
    {"sand", "Sand", 3,
     {{{0.25, 80.0, 0.02}, {0.60, 85.0, 0.05}, {0.85, 90.0, 0.06}}},
     95.0, 0.03},
    {"greyscale", "Greyscale", 1, {{{0.5, 0.0, 0.0}}}, 0.0, 0.0},
}};

inline constexpr std::size_t kThemeCount = kThemes.size();
inline constexpr std::size_t kDefaultTheme = 0;

// The theme in force for every display, set from ScaleSettings. Read on the
// render thread and written on the GUI thread; the tables it indexes are
// immutable once built, so the index is all that has to be atomic.
inline std::atomic<std::size_t> g_theme{kDefaultTheme};

[[nodiscard]] inline std::size_t theme_index()
{
    return g_theme.load(std::memory_order_relaxed);
}

inline void set_theme(std::size_t index)
{
    g_theme.store(index < kThemeCount ? index : kDefaultTheme, std::memory_order_relaxed);
}

// The index of the theme with this id, or the default for one not known, such
// as a hand-edited setting or one from a later build.
[[nodiscard]] inline std::size_t theme_by_id(std::string_view id)
{
    for (std::size_t i = 0; i < kThemeCount; ++i) {
        if (id == kThemes[i].id) {
            return i;
        }
    }
    return kDefaultTheme;
}

// Where the path wants to be at a level, before any chroma is given up to fit
// sRGB: L, C and h.
struct Lch {
    double l = 0.0;
    double c = 0.0;
    double h = 0.0;
};

[[nodiscard]] inline Lch path_at(double level, std::size_t theme_at = kDefaultTheme,
                            bool black = false)
{
    constexpr double kDegrees = 180.0 / 3.14159265358979323846;
    const Theme& theme = kThemes[theme_at < kThemeCount ? theme_at : kDefaultTheme];
    const double t = std::clamp(level, 0.0, 1.0);
    const Lab bottom = srgb8_to_oklab(colour_map::bottom(black));

    std::array<ControlPoint, kMaxInterior + 1> points{};
    std::copy_n(theme.interior.begin(), theme.count, points.begin());
    points[theme.count] = ControlPoint{1.0, theme.top_hue, theme.top_chroma};
    const std::size_t last = theme.count;

    Lch out;
    out.l = bottom.l + (kTopLightness - bottom.l) * t;

    // From the background to the first point, straight across in a and b.
    if (t <= points[0].level) {
        const double u = t / points[0].level;
        const double h = points[0].hue_degrees / kDegrees;
        const double a = bottom.a + (points[0].chroma * std::cos(h) - bottom.a) * u;
        const double b = bottom.b + (points[0].chroma * std::sin(h) - bottom.b) * u;
        out.c = std::hypot(a, b);
        out.h = std::atan2(b, a) * kDegrees;
        return out;
    }
    for (std::size_t i = 1; i <= last; ++i) {
        if (t <= points[i].level) {
            const ControlPoint& from = points[i - 1];
            const ControlPoint& to = points[i];
            const double u = (t - from.level) / (to.level - from.level);
            out.h = from.hue_degrees + (to.hue_degrees - from.hue_degrees) * u;
            out.c = from.chroma + (to.chroma - from.chroma) * u;
            return out;
        }
    }
    out.h = theme.top_hue;
    out.c = theme.top_chroma;
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
[[nodiscard]] inline Lab oklab_at(double level, std::size_t theme = kDefaultTheme,
                            bool black = false)
{
    const Lch want = path_at(level, theme, black);
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

[[nodiscard]] inline Table build_table(std::size_t theme = kDefaultTheme, bool black = false)
{
    Table table{};
    for (std::size_t i = 0; i < kTableSize; ++i) {
        table[i] = to_srgb8(oklab_at(static_cast<double>(i) / (kTableSize - 1), theme, black));
    }
    // The ends exactly, rather than whatever a round trip through OKLab and
    // back lands on, because the bottom has to be the window's background to
    // the bit.
    table.front() = bottom(black);
    return table;
}

// Every theme's table, built together on first use and shared, since C++11
// statics are initialised once and thread-safely. Each theme twice, once per
// bottom: 28 tables of 3 KB.
inline constexpr std::size_t kTableCount = 2 * kThemeCount;

[[nodiscard]] inline std::size_t table_slot(std::size_t theme, bool black)
{
    return (theme < kThemeCount ? theme : kDefaultTheme) + (black ? kThemeCount : 0);
}

[[nodiscard]] inline const std::array<Table, kTableCount>& all_tables()
{
    static const std::array<Table, kTableCount> built = [] {
        std::array<Table, kTableCount> tables{};
        for (std::size_t i = 0; i < kThemeCount; ++i) {
            tables[table_slot(i, false)] = build_table(i, false);
            tables[table_slot(i, true)] = build_table(i, true);
        }
        return tables;
    }();
    return built;
}

[[nodiscard]] inline const Table& table(std::size_t theme, bool black)
{
    return all_tables()[table_slot(theme, black)];
}

[[nodiscard]] inline const Table& table(std::size_t theme)
{
    return table(theme, black_bottom());
}

// The table of the theme and bottom in force.
[[nodiscard]] inline const Table& table()
{
    return table(theme_index());
}

[[nodiscard]] inline Srgb8 colour_at(float level, std::size_t theme)
{
    const float t = std::clamp(level, 0.0F, 1.0F);
    const auto index = static_cast<std::size_t>(std::lround(t * static_cast<float>(kTableSize - 1)));
    return table(theme)[index];
}

[[nodiscard]] inline Srgb8 colour_at(float level)
{
    return colour_at(level, theme_index());
}

}  // namespace revenant::ui::colour_map
