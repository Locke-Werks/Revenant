// Where the ticks go on the frequency ruler between the spectrum and the
// waterfall, and what the labelled ones say.
//
// WHY A HEADER AND NOT QML. A ruler is read by matching a tick to a column,
// so the two ways it can be wrong are both invisible until someone relies on
// it: a tick a fraction of a pixel off the column it names, which grows
// toward the edges if the mapping is not the displays' own, and two labels
// running into each other at a width nobody tried. Both are arithmetic, so
// they live here with cases in ui/tests and the QML only draws what this
// returns.
//
// THE MAPPING IS THE DISPLAYS'. low_hz and high_hz are the frequencies at the
// left edge of the first bin and the right edge of the last, which is what
// EngineLink::frequencyAtFraction answers at 0 and 1 and what the spectrum
// and the waterfall stretch across their width. A tick at low_hz is at x = 0
// and a tick at high_hz is at x = width, exactly.
//
// THE STEP IS 1, 2 OR 5 TIMES A POWER OF TEN, the smallest one whose widest
// label fits between two neighbours with a gap to spare. Labels are measured
// in characters of the monospace family, which is why the caller passes the
// width of one: the ruler never guesses at a font it cannot see.
//
// The unit is chosen from the largest magnitude in view, so a broadcast FM
// span reads in megahertz, an MW span in kilohertz, and ADS-B in megahertz
// with the thousands separated rather than in gigahertz with a leading zero.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace revenant::ui {

// Clear air between two labels, in pixels. Two labels that touch read as one
// number, which is worse than a label fewer.
inline constexpr double kRulerLabelGapPx = 14.0;

// Minor ticks closer than this are not drawn at all. A comb of lines a pixel
// or two apart is a grey band, not a scale.
inline constexpr double kRulerMinorMinPx = 5.0;

struct RulerTick {
    std::int64_t hz = 0;
    double x_px = 0.0;
    bool major = false;

    // Empty on a minor tick.
    std::string label;
};

struct RulerPlan {
    // The spacing of labelled ticks and of unlabelled ones, in hertz. Zero
    // minor means none are drawn.
    std::int64_t major_hz = 0;
    std::int64_t minor_hz = 0;

    // "Hz", "kHz", "MHz" or "GHz", said once at the start of the ruler rather
    // than on every label.
    std::string unit;
    std::int64_t unit_hz = 1;
    int decimals = 0;

    std::vector<RulerTick> ticks;
};

// The unit that suits the largest magnitude in view.
[[nodiscard]] inline std::int64_t ruler_unit_hz(double low_hz, double high_hz)
{
    const double largest = std::max(std::abs(low_hz), std::abs(high_hz));
    if (largest >= 10.0e9) {
        return 1'000'000'000;
    }
    if (largest >= 1.0e6) {
        return 1'000'000;
    }
    if (largest >= 1.0e3) {
        return 1'000;
    }
    return 1;
}

[[nodiscard]] inline std::string ruler_unit_name(std::int64_t unit_hz)
{
    switch (unit_hz) {
        case 1'000'000'000: return "GHz";
        case 1'000'000: return "MHz";
        case 1'000: return "kHz";
        default: return "Hz";
    }
}

// How many decimal places of the unit a step needs so that two neighbouring
// labels never print the same.
[[nodiscard]] inline int ruler_decimals(std::int64_t step_hz, std::int64_t unit_hz)
{
    int decimals = 0;
    std::int64_t resolution = unit_hz;
    while (resolution > 1 && step_hz % resolution != 0) {
        resolution /= 10;
        ++decimals;
    }
    return decimals;
}

// A frequency as a label: the unit's whole part with its thousands set apart
// by a space, then as many decimals as the step needs, in groups of three.
// Integer arithmetic throughout, so 100.3 MHz is never printed as 100.299.
[[nodiscard]] inline std::string ruler_label(std::int64_t hz, std::int64_t unit_hz, int decimals)
{
    const bool negative = hz < 0;
    const std::int64_t magnitude = negative ? -hz : hz;
    const std::int64_t whole = magnitude / unit_hz;
    std::int64_t fraction = magnitude % unit_hz;

    std::string digits = std::to_string(whole);
    std::string grouped;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) {
            grouped += ' ';
        }
        grouped += digits[i];
    }

    std::string out = negative ? "-" + grouped : grouped;
    if (decimals <= 0) {
        return out;
    }

    // The fraction as the full width of the unit, then cut to the decimals
    // the step needs. The cut is exact because the step is a multiple of
    // what is being cut away.
    int places = 0;
    for (std::int64_t u = unit_hz; u > 1; u /= 10) {
        ++places;
    }
    std::string tail = std::to_string(fraction);
    tail.insert(0, static_cast<std::size_t>(places) - std::min(tail.size(),
                                                              static_cast<std::size_t>(places)),
                '0');
    tail.resize(static_cast<std::size_t>(std::min(decimals, places)));

    out += '.';
    for (std::size_t i = 0; i < tail.size(); ++i) {
        if (i > 0 && i % 3 == 0) {
            out += ' ';
        }
        out += tail[i];
    }
    return out;
}

// x of a frequency on a ruler width_px wide, by the displays' own mapping.
[[nodiscard]] inline double ruler_x(double hz, double low_hz, double high_hz, double width_px)
{
    const double span = high_hz - low_hz;
    if (!(span > 0.0)) {
        return 0.0;
    }
    return (hz - low_hz) / span * width_px;
}

// The next step up in the 1, 2, 5 sequence.
[[nodiscard]] inline std::int64_t ruler_next_step(std::int64_t step)
{
    std::int64_t decade = 1;
    while (decade * 10 <= step) {
        decade *= 10;
    }
    const std::int64_t mantissa = step / decade;
    if (mantissa < 2) {
        return 2 * decade;
    }
    if (mantissa < 5) {
        return 5 * decade;
    }
    return 10 * decade;
}

// The minor spacing for a major one, chosen so every minor tick lands on a
// round number: fifths of a 1 or a 5, quarters of a 2, so 2 MHz majors take
// 0.5 MHz minors. Zero when the division is not whole, at the bottom of the
// sequence, where there is nothing finer than a hertz to draw.
[[nodiscard]] inline std::int64_t ruler_minor_step(std::int64_t major)
{
    std::int64_t decade = 1;
    while (decade * 10 <= major) {
        decade *= 10;
    }
    const std::int64_t mantissa = major / decade;
    if (mantissa == 2) {
        return major / 4 > 0 && (major % 4) == 0 ? major / 4 : 0;
    }
    return major % 5 == 0 ? major / 5 : 0;
}

// The whole ruler for a span drawn width_px wide, with labels measured at
// char_px per character.
//
// A label is centred on its tick, and one that would not fit whole inside the
// ruler is dropped rather than pulled in or clipped: pulled in, it can land on
// its neighbour, and clipped, it names a frequency by half its digits. Its tick
// stays. reserve_left_px keeps labels off the start of the ruler, where the
// unit is written.
[[nodiscard]] inline RulerPlan plan_ruler(double low_hz, double high_hz, double width_px,
                                          double char_px, double reserve_left_px = 0.0)
{
    RulerPlan plan;
    const double span = high_hz - low_hz;
    if (!(span > 0.0) || !(width_px > 0.0) || !(char_px > 0.0)) {
        return plan;
    }

    plan.unit_hz = ruler_unit_hz(low_hz, high_hz);
    plan.unit = ruler_unit_name(plan.unit_hz);

    // Walk up the sequence until the widest label a step would print fits
    // in the room one step gives it. The widest label is one of the two
    // ends, because the magnitude only grows toward them, so both are
    // measured rather than a representative one.
    std::int64_t step = 1;
    for (int guard = 0; guard < 64; ++guard) {
        const int decimals = ruler_decimals(step, plan.unit_hz);
        const auto low_label =
            ruler_label(static_cast<std::int64_t>(std::floor(low_hz)), plan.unit_hz, decimals);
        const auto high_label =
            ruler_label(static_cast<std::int64_t>(std::ceil(high_hz)), plan.unit_hz, decimals);
        const double label_px =
            static_cast<double>(std::max(low_label.size(), high_label.size())) * char_px;
        const double step_px = static_cast<double>(step) / span * width_px;
        if (step_px >= label_px + kRulerLabelGapPx) {
            plan.major_hz = step;
            plan.decimals = decimals;
            break;
        }
        step = ruler_next_step(step);
    }
    if (plan.major_hz == 0) {
        return plan;
    }

    const std::int64_t minor = ruler_minor_step(plan.major_hz);
    if (minor > 0 && static_cast<double>(minor) / span * width_px >= kRulerMinorMinPx) {
        plan.minor_hz = minor;
    }

    const std::int64_t tick_step = plan.minor_hz > 0 ? plan.minor_hz : plan.major_hz;
    const auto first = static_cast<std::int64_t>(std::ceil(low_hz / static_cast<double>(tick_step)));
    const auto last = static_cast<std::int64_t>(std::floor(high_hz / static_cast<double>(tick_step)));
    for (std::int64_t k = first; k <= last; ++k) {
        RulerTick tick;
        tick.hz = k * tick_step;
        tick.x_px = ruler_x(static_cast<double>(tick.hz), low_hz, high_hz, width_px);
        tick.major = tick.hz % plan.major_hz == 0;
        if (tick.major) {
            std::string label = ruler_label(tick.hz, plan.unit_hz, plan.decimals);
            const double half = static_cast<double>(label.size()) * char_px / 2.0;
            if (tick.x_px - half >= reserve_left_px && tick.x_px + half <= width_px) {
                tick.label = std::move(label);
            }
        }
        plan.ticks.push_back(std::move(tick));
    }
    return plan;
}

}  // namespace revenant::ui
