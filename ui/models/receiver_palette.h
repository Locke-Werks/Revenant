// The receiver colours: one per rack slot, eight of them, and the arithmetic
// that chose them and holds them to their claim.
//
// WHAT THE CLAIM IS. A receiver's colour is how its strip in the rack, its
// marker on the span and its passband on the ruler are read as one thing, so
// two receivers must never be mistaken for each other at a glance, with eight
// of them up, and that has to stay true for an operator with one of the three
// common colour vision deficiencies. Stated as a number: every pair of the
// eight colours differs by at least kMinReceiverColourDelta in CIEDE2000, in
// normal vision and in simulated protan, deutan and tritan vision each.
//
// THE MEASURE. CIEDE2000 on CIELAB under D65, as specified in CIE 142-2001
// and implemented after G. Sharma, W. Wu and E. N. Dalal, "The CIEDE2000
// Color-Difference Formula: Implementation Notes, Supplementary Test Data, and
// Mathematical Observations", Color Research and Application 30(1), 2005.
// ui/tests/test_receiver_palette.cpp checks this implementation against pairs
// from that paper's test data before trusting it with anything else.
//
// THE SIMULATION. G. M. Machado, M. M. Oliveira and L. A. F. Fernandes, "A
// Physiologically-based Model for Simulation of Color Vision Deficiency",
// IEEE Transactions on Visualization and Computer Graphics 15(6), 2009, at
// severity 1.0, which is protanopia, deuteranopia and tritanopia rather than
// the milder anomalous trichromacies. The three matrices are the paper's
// published ones and are applied to LINEAR sRGB, which is the space the
// model is defined in; applying them to gamma-encoded values, which is a
// common shortcut, understates how far colours collapse.
//
// NO MAGENTA. Theme.inkTune, #ff58c8, is the detection colour and nothing
// else on the span may look like it. Every candidate whose OKLCH hue is
// within kTuneHueExclusionDeg of the detection colour's is refused before the
// search sees it, and every chosen colour must also stand at least
// kMinTuneDelta from it in normal vision. Three other theme colours carry a
// meaning of their own, the accent for what can be operated and the warning
// and fault inks, and a receiver colour must stand kMinThemeDelta clear of
// each of them for the same reason.
//
// PERCEPTUALLY UNIFORM, WHICH MEANS WHERE THE CANDIDATES COME FROM. They are
// a grid in OKLCH (B. Ottosson, "A perceptual color space for image
// processing", 2020), lightness 0.66 to 0.90 and chroma 0.10 to 0.16, so no
// colour is much louder than another on the near-black window and none is
// dark enough to vanish into it. Holding lightness to one value would be
// more uniform and cannot meet the claim: deuteranopia collapses hue onto a
// single blue to yellow axis, and eight colours at one lightness have nowhere
// to be apart on it. So lightness varies over a band and hue does the rest.
//
// THE SEARCH, WHICH IS derive_receiver_palette BELOW AND WHICH THE TABLE IS
// THE OUTPUT OF. Slot 0 is the candidate nearest #80c4ff, the azure the
// receiver marker has always been drawn in, so the first receiver looks as
// it always has. Each further slot is the candidate whose smallest
// difference from the slots already chosen, taken as the worst of the four
// visions, is largest. Then each slot but the first is swapped for any
// candidate that raises the palette's worst pair, until no swap does. The
// test runs the search and requires kReceiverPalette to be exactly what it
// returns, so the table cannot be hand-edited away from the procedure.
//
// Measured when the table was derived: the worst pair is 10.18 in CIEDE2000,
// the azure against the teal blue in tritan vision. The worst in each of the
// other three is 17.1 normal, 13.9 protan and 12.8 deutan. The eight colours
// this replaced were never checked, and under deutan the first and the last
// of them were 1.9 apart.
//
// Qt-free, so ui/tests links it.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>

namespace revenant::ui {

inline constexpr std::size_t kReceiverPaletteSize = 8;

// The eight, as 8-bit sRGB. What was tested is the quantised colour, not the
// grid point it came from, so these bytes are the claim.
struct Rgb8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    friend bool operator==(const Rgb8&, const Rgb8&) = default;
};

inline constexpr std::array<Rgb8, kReceiverPaletteSize> kReceiverPalette{{
    {0x81, 0xbe, 0xf4},  // azure
    {0xdb, 0x72, 0x13},  // orange
    {0xd5, 0xe8, 0x9d},  // pale green
    {0x79, 0x86, 0xf2},  // periwinkle
    {0x7f, 0xad, 0x70},  // sage
    {0x69, 0xfa, 0xe1},  // aqua
    {0xe3, 0xc3, 0x27},  // yellow
    {0x30, 0xa4, 0xae},  // teal blue
}};

// The thresholds the table is held to.
inline constexpr double kMinReceiverColourDelta = 10.0;
inline constexpr double kMinTuneDelta = 20.0;
inline constexpr double kMinThemeDelta = 12.0;
inline constexpr double kTuneHueExclusionDeg = 40.0;

// The theme colours a receiver must not be taken for. Theme.qml holds the
// same values; the detection colour is also render/spectrum_item.cpp's.
inline constexpr Rgb8 kTuneColour{0xff, 0x58, 0xc8};
inline constexpr std::array<Rgb8, 3> kThemeMeaningColours{{
    {0x45, 0xc4, 0xb0},  // Theme.accent
    {0xd6, 0xa2, 0x4a},  // Theme.inkWarn
    {0xd6, 0x62, 0x4a},  // Theme.inkBad
}};

// "#81bef4".
[[nodiscard]] inline std::string rgb8_hex(const Rgb8& colour)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out = "#";
    for (const std::uint8_t byte : {colour.r, colour.g, colour.b}) {
        out.push_back(kDigits[byte >> 4U]);
        out.push_back(kDigits[byte & 0x0fU]);
    }
    return out;
}

// The colour for a rack slot. A slot past the table wraps rather than
// failing, though the rack never hands one out; see models/receiver_rack.h.
[[nodiscard]] constexpr Rgb8 receiver_colour(std::size_t slot)
{
    return kReceiverPalette[slot % kReceiverPaletteSize];
}

// ---------------------------------------------------------------------------
// The colour arithmetic
// ---------------------------------------------------------------------------

namespace colour {

struct Linear {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

struct Lab {
    double l = 0.0;
    double a = 0.0;
    double b = 0.0;
};

struct Lch {
    double l = 0.0;
    double c = 0.0;
    double h_deg = 0.0;
};

[[nodiscard]] inline double srgb_to_linear(double c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

[[nodiscard]] inline double linear_to_srgb(double c)
{
    return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

[[nodiscard]] inline Linear to_linear(const Rgb8& colour)
{
    return {srgb_to_linear(colour.r / 255.0), srgb_to_linear(colour.g / 255.0),
            srgb_to_linear(colour.b / 255.0)};
}

[[nodiscard]] inline Rgb8 to_rgb8(const Linear& colour)
{
    const auto byte = [](double linear) {
        const double encoded = std::clamp(linear_to_srgb(std::clamp(linear, 0.0, 1.0)), 0.0, 1.0);
        return static_cast<std::uint8_t>(std::lround(encoded * 255.0));
    };
    return {byte(colour.r), byte(colour.g), byte(colour.b)};
}

// Linear sRGB to CIELAB, D65 white.
[[nodiscard]] inline Lab to_lab(const Linear& c)
{
    const double x = 0.4124564 * c.r + 0.3575761 * c.g + 0.1804375 * c.b;
    const double y = 0.2126729 * c.r + 0.7151522 * c.g + 0.0721750 * c.b;
    const double z = 0.0193339 * c.r + 0.1191920 * c.g + 0.9503041 * c.b;
    const auto f = [](double t) {
        constexpr double kEpsilon = 216.0 / 24389.0;
        constexpr double kKappa = 24389.0 / 27.0;
        return t > kEpsilon ? std::cbrt(t) : (kKappa * t + 16.0) / 116.0;
    };
    const double fx = f(x / 0.95047);
    const double fy = f(y / 1.0);
    const double fz = f(z / 1.08883);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

// OKLCH from Ottosson's published matrices, for the candidate grid and the
// magenta test. Hue in degrees, [0, 360).
[[nodiscard]] inline Linear oklch_to_linear(const Lch& lch)
{
    const double h = lch.h_deg * std::numbers::pi / 180.0;
    const double a = lch.c * std::cos(h);
    const double b = lch.c * std::sin(h);
    const double l_ = lch.l + 0.3963377774 * a + 0.2158037573 * b;
    const double m_ = lch.l - 0.1055613458 * a - 0.0638541728 * b;
    const double s_ = lch.l - 0.0894841775 * a - 1.2914855480 * b;
    const double l = l_ * l_ * l_;
    const double m = m_ * m_ * m_;
    const double s = s_ * s_ * s_;
    return {4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
            -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
            -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s};
}

[[nodiscard]] inline Lch linear_to_oklch(const Linear& c)
{
    const double l = std::cbrt(0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b);
    const double m = std::cbrt(0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b);
    const double s = std::cbrt(0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b);
    const double big_l = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    const double a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    const double b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    double h = std::atan2(b, a) * 180.0 / std::numbers::pi;
    if (h < 0.0) {
        h += 360.0;
    }
    return {big_l, std::hypot(a, b), h};
}

// The smaller angle between two hues, in degrees.
[[nodiscard]] inline double hue_distance(double a_deg, double b_deg)
{
    const double d = std::fmod(std::abs(a_deg - b_deg), 360.0);
    return d > 180.0 ? 360.0 - d : d;
}

// CIEDE2000 with kL = kC = kH = 1, after Sharma, Wu and Dalal (2005).
[[nodiscard]] inline double ciede2000(const Lab& one, const Lab& two)
{
    constexpr double kDeg = std::numbers::pi / 180.0;
    const double pow25_7 = 6103515625.0;  // 25^7

    const double c1 = std::hypot(one.a, one.b);
    const double c2 = std::hypot(two.a, two.b);
    const double c_mean = (c1 + c2) / 2.0;
    const double c_mean7 = std::pow(c_mean, 7.0);
    const double g = 0.5 * (1.0 - std::sqrt(c_mean7 / (c_mean7 + pow25_7)));

    const double a1 = (1.0 + g) * one.a;
    const double a2 = (1.0 + g) * two.a;
    const double c1p = std::hypot(a1, one.b);
    const double c2p = std::hypot(a2, two.b);

    const auto hue = [](double b, double a) {
        if (a == 0.0 && b == 0.0) {
            return 0.0;
        }
        double h = std::atan2(b, a) * 180.0 / std::numbers::pi;
        return h < 0.0 ? h + 360.0 : h;
    };
    const double h1p = hue(one.b, a1);
    const double h2p = hue(two.b, a2);

    const double dl = two.l - one.l;
    const double dc = c2p - c1p;
    double dh = 0.0;
    if (c1p * c2p != 0.0) {
        dh = h2p - h1p;
        if (dh > 180.0) {
            dh -= 360.0;
        } else if (dh < -180.0) {
            dh += 360.0;
        }
    }
    const double big_dh = 2.0 * std::sqrt(c1p * c2p) * std::sin(dh * kDeg / 2.0);

    const double l_mean = (one.l + two.l) / 2.0;
    const double cp_mean = (c1p + c2p) / 2.0;
    double h_mean = h1p + h2p;
    if (c1p * c2p != 0.0) {
        if (std::abs(h1p - h2p) <= 180.0) {
            h_mean = (h1p + h2p) / 2.0;
        } else if (h1p + h2p < 360.0) {
            h_mean = (h1p + h2p + 360.0) / 2.0;
        } else {
            h_mean = (h1p + h2p - 360.0) / 2.0;
        }
    }

    const double t = 1.0 - 0.17 * std::cos((h_mean - 30.0) * kDeg) +
                     0.24 * std::cos(2.0 * h_mean * kDeg) +
                     0.32 * std::cos((3.0 * h_mean + 6.0) * kDeg) -
                     0.20 * std::cos((4.0 * h_mean - 63.0) * kDeg);
    const double d_theta = 30.0 * std::exp(-std::pow((h_mean - 275.0) / 25.0, 2.0));
    const double cp_mean7 = std::pow(cp_mean, 7.0);
    const double rc = 2.0 * std::sqrt(cp_mean7 / (cp_mean7 + pow25_7));
    const double l50 = (l_mean - 50.0) * (l_mean - 50.0);
    const double sl = 1.0 + 0.015 * l50 / std::sqrt(20.0 + l50);
    const double sc = 1.0 + 0.045 * cp_mean;
    const double sh = 1.0 + 0.015 * cp_mean * t;
    const double rt = -std::sin(2.0 * d_theta * kDeg) * rc;

    const double tl = dl / sl;
    const double tc = dc / sc;
    const double th = big_dh / sh;
    return std::sqrt(tl * tl + tc * tc + th * th + rt * tc * th);
}

enum class Vision : std::uint8_t {
    Normal,
    Protan,
    Deutan,
    Tritan,
};

inline constexpr std::array kVisions{Vision::Normal, Vision::Protan, Vision::Deutan,
                                     Vision::Tritan};

[[nodiscard]] constexpr const char* vision_name(Vision vision)
{
    switch (vision) {
        case Vision::Normal:
            return "normal";
        case Vision::Protan:
            return "protan";
        case Vision::Deutan:
            return "deutan";
        case Vision::Tritan:
            return "tritan";
    }
    return "";
}

using Matrix3 = std::array<std::array<double, 3>, 3>;

// Machado, Oliveira and Fernandes (2009), severity 1.0.
inline constexpr Matrix3 kProtanopia{{{0.152286, 1.052583, -0.204868},
                                      {0.114503, 0.786281, 0.099216},
                                      {-0.003882, -0.048116, 1.051998}}};
inline constexpr Matrix3 kDeuteranopia{{{0.367322, 0.860646, -0.227968},
                                        {0.280085, 0.672501, 0.047413},
                                        {-0.011820, 0.042940, 0.968881}}};
inline constexpr Matrix3 kTritanopia{{{1.255528, -0.076749, -0.178779},
                                      {-0.078411, 0.930809, 0.147602},
                                      {0.004733, 0.691367, 0.303900}}};

// What a colour looks like in one vision, still linear. Clamped into the
// gamut afterwards, because the simulated colour has to be a colour a screen
// could show for its Lab value to mean anything.
[[nodiscard]] inline Linear simulate(const Linear& c, Vision vision)
{
    const Matrix3* m = nullptr;
    switch (vision) {
        case Vision::Normal:
            return c;
        case Vision::Protan:
            m = &kProtanopia;
            break;
        case Vision::Deutan:
            m = &kDeuteranopia;
            break;
        case Vision::Tritan:
            m = &kTritanopia;
            break;
    }
    const Matrix3& mm = *m;
    const auto row = [&c](const std::array<double, 3>& r) {
        return std::clamp(r[0] * c.r + r[1] * c.g + r[2] * c.b, 0.0, 1.0);
    };
    return {row(mm[0]), row(mm[1]), row(mm[2])};
}

// One colour as it appears in each of the four visions, in Lab.
using Views = std::array<Lab, kVisions.size()>;

[[nodiscard]] inline Views views_of(const Rgb8& colour)
{
    const Linear linear = to_linear(colour);
    Views out{};
    for (std::size_t i = 0; i < kVisions.size(); ++i) {
        out[i] = to_lab(simulate(linear, kVisions[i]));
    }
    return out;
}

[[nodiscard]] inline double delta_in(const Views& one, const Views& two, Vision vision)
{
    const auto i = static_cast<std::size_t>(vision);
    return ciede2000(one[i], two[i]);
}

// The worst of the four: how different two colours look to whoever sees
// them least differently.
[[nodiscard]] inline double worst_delta(const Views& one, const Views& two)
{
    double worst = ciede2000(one[0], two[0]);
    for (std::size_t i = 1; i < kVisions.size(); ++i) {
        worst = std::min(worst, ciede2000(one[i], two[i]));
    }
    return worst;
}

}  // namespace colour

// The smallest pairwise difference in a palette, in one vision.
[[nodiscard]] inline double palette_min_delta(const std::vector<Rgb8>& palette,
                                              colour::Vision vision)
{
    double worst = 1.0e9;
    for (std::size_t i = 0; i < palette.size(); ++i) {
        const colour::Views a = colour::views_of(palette[i]);
        for (std::size_t j = i + 1; j < palette.size(); ++j) {
            worst = std::min(worst, colour::delta_in(a, colour::views_of(palette[j]), vision));
        }
    }
    return worst;
}

// ---------------------------------------------------------------------------
// The derivation
// ---------------------------------------------------------------------------

// Whether a colour may be a receiver's at all: away from the detection hue,
// clear of the detection colour, and clear of the three theme colours that
// mean something. Normal vision for these, because they are about what the
// colour is taken to mean and not about telling receivers apart.
[[nodiscard]] inline bool receiver_colour_allowed(const Rgb8& candidate)
{
    const colour::Linear linear = colour::to_linear(candidate);
    const double tune_hue = colour::linear_to_oklch(colour::to_linear(kTuneColour)).h_deg;
    if (colour::hue_distance(colour::linear_to_oklch(linear).h_deg, tune_hue) <
        kTuneHueExclusionDeg) {
        return false;
    }
    const colour::Lab lab = colour::to_lab(linear);
    if (colour::ciede2000(lab, colour::to_lab(colour::to_linear(kTuneColour))) < kMinTuneDelta) {
        return false;
    }
    for (const Rgb8& meaning : kThemeMeaningColours) {
        if (colour::ciede2000(lab, colour::to_lab(colour::to_linear(meaning))) <
            kMinThemeDelta) {
            return false;
        }
    }
    return true;
}

// The candidate grid, quantised to 8-bit, in grid order: lightness, then
// chroma, then hue in 6 degree steps. Out-of-gamut points are dropped rather
// than clipped, because a clipped point is a different colour from the one
// the grid describes.
[[nodiscard]] inline std::vector<Rgb8> receiver_colour_candidates()
{
    std::vector<Rgb8> out;
    for (const double l : {0.66, 0.70, 0.74, 0.78, 0.82, 0.86, 0.90}) {
        for (const double c : {0.10, 0.13, 0.16}) {
            for (int h = 0; h < 360; h += 6) {
                const colour::Linear linear =
                    colour::oklch_to_linear({l, c, static_cast<double>(h)});
                if (std::min({linear.r, linear.g, linear.b}) < 0.0 ||
                    std::max({linear.r, linear.g, linear.b}) > 1.0) {
                    continue;
                }
                const Rgb8 quantised = colour::to_rgb8(linear);
                if (receiver_colour_allowed(quantised)) {
                    out.push_back(quantised);
                }
            }
        }
    }
    return out;
}

// The search the header's opening describes. Deterministic: ties go to the
// earlier candidate, so the same grid gives the same palette every time.
[[nodiscard]] inline std::vector<Rgb8> derive_receiver_palette()
{
    const std::vector<Rgb8> candidates = receiver_colour_candidates();
    const std::size_t n = candidates.size();
    if (n < kReceiverPaletteSize) {
        return {};
    }

    std::vector<colour::Views> views;
    views.reserve(n);
    for (const Rgb8& one : candidates) {
        views.push_back(colour::views_of(one));
    }

    // Every pair once, because the local search below revisits them many
    // times.
    std::vector<double> pair(n * n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            const double d = colour::worst_delta(views[i], views[j]);
            pair[i * n + j] = d;
            pair[j * n + i] = d;
        }
    }

    const colour::Views azure = colour::views_of(Rgb8{0x80, 0xc4, 0xff});
    std::size_t seed = 0;
    double nearest = 1.0e9;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = colour::ciede2000(views[i][0], azure[0]);
        if (d < nearest) {
            nearest = d;
            seed = i;
        }
    }

    std::vector<std::size_t> chosen{seed};
    const auto taken = [&chosen](std::size_t i) {
        return std::find(chosen.begin(), chosen.end(), i) != chosen.end();
    };
    while (chosen.size() < kReceiverPaletteSize) {
        std::size_t best = n;
        double best_score = -1.0;
        for (std::size_t i = 0; i < n; ++i) {
            if (taken(i)) {
                continue;
            }
            double score = 1.0e9;
            for (const std::size_t j : chosen) {
                score = std::min(score, pair[i * n + j]);
            }
            if (score > best_score) {
                best_score = score;
                best = i;
            }
        }
        chosen.push_back(best);
    }

    const auto evaluate = [&pair, n](const std::vector<std::size_t>& set) {
        double worst = 1.0e9;
        for (std::size_t i = 0; i < set.size(); ++i) {
            for (std::size_t j = i + 1; j < set.size(); ++j) {
                worst = std::min(worst, pair[set[i] * n + set[j]]);
            }
        }
        return worst;
    };

    double current = evaluate(chosen);
    bool improved = true;
    while (improved) {
        improved = false;
        for (std::size_t slot = 1; slot < chosen.size(); ++slot) {
            for (std::size_t i = 0; i < n; ++i) {
                if (taken(i)) {
                    continue;
                }
                std::vector<std::size_t> trial = chosen;
                trial[slot] = i;
                const double score = evaluate(trial);
                if (score > current + 1.0e-9) {
                    chosen = std::move(trial);
                    current = score;
                    improved = true;
                }
            }
        }
    }

    std::vector<Rgb8> out;
    out.reserve(chosen.size());
    for (const std::size_t i : chosen) {
        out.push_back(candidates[i]);
    }
    return out;
}

}  // namespace revenant::ui
