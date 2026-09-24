// Where the ticks go on the vertical dBFS scale beside each spectrum trace,
// and what the labelled ones say.
//
// The level counterpart of models/ruler.h, in the same shape and for the same
// reason: a scale is read by matching a tick to a height on the trace, so the
// two ways it can be wrong are both invisible until someone relies on it. A
// tick placed by a mapping other than the trace's own drifts from the level it
// names toward one end, and two labels run into each other at a pane height
// nobody tried. Both are arithmetic, so they live here with cases in
// ui/tests/test_level_scale.cpp and the QML only draws what this returns.
//
// THE MAPPING IS THE TRACE'S. A trace column at floor_db is drawn on the
// bottom edge and one at ceiling_db on the top, linear in decibels between,
// which is what SpectrumItem and PassbandItem do with the ends they report as
// drawFloorDb and drawCeilingDb. Those ends follow the auto-scale and the
// operator's pins alike, so a scale planned from them follows both.
//
// THE STEP IS 1, 2 OR 5 TIMES A POWER OF TEN DECIBELS, the smallest one that
// leaves a label's height plus a gap between neighbours. Labels are centred
// on their ticks, so that spacing is exactly the guarantee that no two touch.
// The caller passes the label height, because the scale never guesses at a
// font it cannot see; this is the ruler's char_px turned on its side.
//
// Steps below a decibel exist for one case only: two pins set close together,
// which render/spectrum_scale.h allows down to kMinPinnedSpanDb. Everything
// is counted in tenths of a decibel so that such a step is an integer and a
// label never reads -40.000001.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace revenant::ui {

// Clear air between two labels, in pixels: the ruler's gap. Four would keep
// the labels apart, and on a 300 px span it would also put a number every
// five decibels, fourteen of them in a column down the edge of the trace. A
// scale that dense is read as texture rather than looked up, and the owner's
// design pass asks for the chrome to recede.
inline constexpr double kLevelLabelGapPx = 14.0;

// Minor ticks closer than this are not drawn. A comb of marks two pixels
// apart is a grey smear down the edge, not a scale.
inline constexpr double kLevelMinorMinPx = 4.0;

struct LevelTick {
    double db = 0.0;
    double y_px = 0.0;
    bool major = false;

    // Empty on a minor tick, and on a major one whose label would not fit
    // whole inside the room the caller left; the tick is still drawn.
    std::string label;
};

struct LevelScalePlan {
    // The spacing of labelled ticks and of unlabelled ones, in tenths of a
    // decibel. Zero minor means none are drawn; zero major means the pane had
    // no room for a scale at all.
    std::int64_t major_tenths = 0;
    std::int64_t minor_tenths = 0;
    int decimals = 0;

    std::vector<LevelTick> ticks;
};

// y of a level on a trace height_px tall, by the trace's own mapping: the
// ceiling on the top edge, the floor on the bottom. Not clamped, because a
// caller placing a marker for a level outside the ends needs to know which
// side it fell off.
[[nodiscard]] inline double level_y(double db, double floor_db, double ceiling_db,
                                    double height_px)
{
    const double span = ceiling_db - floor_db;
    if (!(span > 0.0)) {
        return height_px;
    }
    return (ceiling_db - db) / span * height_px;
}

// A level to the decimals a step needs: none for a whole-decibel step, one
// for a step in tenths. Integer arithmetic on tenths, so a tick at -40.5 dBFS
// prints as that and not as -40.499999.
[[nodiscard]] inline std::string level_label(std::int64_t tenths, int decimals)
{
    const bool negative = tenths < 0;
    const std::int64_t magnitude = negative ? -tenths : tenths;
    std::string out;
    if (decimals <= 0) {
        // Rounded half away from zero, which only matters if a caller asks
        // for no decimals on a value that has some.
        const std::int64_t whole = (magnitude + 5) / 10;
        out = std::to_string(whole);
        return negative && whole != 0 ? "-" + out : out;
    }
    out = std::to_string(magnitude / 10) + "." + std::to_string(magnitude % 10);
    return negative ? "-" + out : out;
}

// The next step up in the 1, 2, 5 sequence, in tenths.
[[nodiscard]] inline std::int64_t level_next_step(std::int64_t step)
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

// The minor spacing for a major one: fifths of a 1 or a 5 and halves of a 2,
// so every minor tick lands on a round number, 10 dB majors taking 2 dB
// minors and 20 dB majors 10 dB ones. Zero below a tenth, where there is
// nothing finer to draw.
[[nodiscard]] inline std::int64_t level_minor_step(std::int64_t major)
{
    std::int64_t decade = 1;
    while (decade * 10 <= major) {
        decade *= 10;
    }
    const std::int64_t mantissa = major / decade;
    if (mantissa == 2) {
        return major % 2 == 0 ? major / 2 : 0;
    }
    return major % 5 == 0 ? major / 5 : 0;
}

// The whole scale for a trace height_px tall drawn between floor_db and
// ceiling_db, with labels label_height_px tall.
//
// A label that would reach into reserve_top_px or reserve_bottom_px, or off
// either edge, is dropped rather than pulled in or clipped: pulled in it can
// land on its neighbour, and clipped it names a level by half its digits.
// The reserves are for what the pane already draws along its edges: the
// detection labels along the top of the span, and nothing so far along the
// bottom.
[[nodiscard]] inline LevelScalePlan plan_level_scale(double floor_db, double ceiling_db,
                                                     double height_px, double label_height_px,
                                                     double reserve_top_px = 0.0,
                                                     double reserve_bottom_px = 0.0)
{
    LevelScalePlan plan;
    const double span = ceiling_db - floor_db;
    if (!(span > 0.0) || !(height_px > 0.0) || !(label_height_px > 0.0) ||
        !std::isfinite(floor_db) || !std::isfinite(ceiling_db)) {
        return plan;
    }

    const double wanted_px = label_height_px + kLevelLabelGapPx;
    std::int64_t step = 1;
    for (int guard = 0; guard < 32; ++guard) {
        const double step_px = static_cast<double>(step) / 10.0 / span * height_px;
        if (step_px >= wanted_px) {
            plan.major_tenths = step;
            break;
        }
        step = level_next_step(step);
    }
    if (plan.major_tenths == 0) {
        return plan;
    }
    plan.decimals = plan.major_tenths % 10 == 0 ? 0 : 1;

    const std::int64_t minor = level_minor_step(plan.major_tenths);
    if (minor > 0 && static_cast<double>(minor) / 10.0 / span * height_px >= kLevelMinorMinPx) {
        plan.minor_tenths = minor;
    }

    const std::int64_t tick_step = plan.minor_tenths > 0 ? plan.minor_tenths : plan.major_tenths;
    const double step_db = static_cast<double>(tick_step) / 10.0;
    const auto first = static_cast<std::int64_t>(std::ceil(floor_db / step_db));
    const auto last = static_cast<std::int64_t>(std::floor(ceiling_db / step_db));
    const double half = label_height_px / 2.0;
    for (std::int64_t k = first; k <= last; ++k) {
        const std::int64_t tenths = k * tick_step;
        LevelTick tick;
        tick.db = static_cast<double>(tenths) / 10.0;
        tick.y_px = level_y(tick.db, floor_db, ceiling_db, height_px);
        tick.major = tenths % plan.major_tenths == 0;
        if (tick.major && tick.y_px - half >= reserve_top_px &&
            tick.y_px + half <= height_px - reserve_bottom_px) {
            tick.label = level_label(tenths, plan.decimals);
        }
        plan.ticks.push_back(std::move(tick));
    }
    return plan;
}

}  // namespace revenant::ui
