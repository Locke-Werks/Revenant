// The span displays' settings as plain values, with the rules that bound them.
//
// models/scale_settings.h holds these for the desktop and stores them in
// QSettings; tools/webgate reads the same stored values back for the phone,
// which follows the desktop's display. Both apply the same clamps from here,
// so a hand-edited or out-of-range value reads the same on either.
//
// This header holds no Qt; ui/tests and tools/webgate link it.

#pragma once

#include <algorithm>
#include <cstddef>

#include "render/colour_map.h"
#include "render/spectrum_scale.h"
#include "render/waterfall_scale.h"

namespace revenant::ui {

// Every field at the value a fresh install starts with.
struct DisplaySettings {
    ScalePins pins;
    double waterfall_contrast = 0.0;
    double waterfall_pivot = kDefaultCurvePivot;
    bool waterfall_auto_pivot = false;
    std::size_t waterfall_theme = colour_map::kDefaultTheme;
    bool oled_black = false;
    double spectrum_range_fit = 0.0;
    double spectrum_smoothing = kDefaultSmoothDecaySeconds;
    bool spectrum_floor_smoothing = true;

    [[nodiscard]] bool operator==(const DisplaySettings&) const = default;
};

// A value outside 0 to 1, or not a number, reads as the nearest end rather
// than extrapolating the blend past either look.
[[nodiscard]] inline double clamp_range_fit(double value)
{
    return value > 0.0 ? std::min(value, 1.0) : 0.0;
}

// The contrast runs on past tight to the hot ends; see HistoryLevels::ends.
[[nodiscard]] inline double clamp_waterfall_contrast(double value)
{
    return value > 0.0 ? std::min(value, static_cast<double>(kMaxWaterfallContrast)) : 0.0;
}

[[nodiscard]] inline double clamp_waterfall_pivot(double value)
{
    return value == value ? std::clamp(value, static_cast<double>(kMinCurvePivot),
                                       static_cast<double>(kMaxCurvePivot))
                          : static_cast<double>(kDefaultCurvePivot);
}

[[nodiscard]] inline double clamp_smoothing(double seconds)
{
    return seconds > 0.0 ? std::min(seconds, kMaxSmoothDecaySeconds) : 0.0;
}

}  // namespace revenant::ui
