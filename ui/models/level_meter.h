// The signal meter on a receiver strip: how far along the bar a level is.
//
// A receiver's level arrives as dBFS on rpc::VrxStatus, and -200 or below is
// how the engine says it has not measured one yet; the detail pane has always
// hidden its number below -199 for that reason, and the meter follows the same
// rule rather than drawing an empty bar that reads as silence.
//
// The scale is fixed rather than tracking the signal, because a meter that
// rescales to whatever is loudest shows every signal as full. Sixty decibels,
// from -80 to -20 dBFS, which covers a receiver's noise floor at the bottom
// and a strong local station near the top on the RTL-SDR this was drawn
// against; a level outside it pins the bar at an end rather than wrapping.

#pragma once

#include <algorithm>

namespace revenant::ui {

inline constexpr double kMeterFloorDbfs = -80.0;
inline constexpr double kMeterCeilingDbfs = -20.0;

// Below this there is no measurement, only the engine's placeholder.
inline constexpr double kMeterNoReadingDbfs = -199.0;

[[nodiscard]] constexpr bool meter_has_reading(double dbfs)
{
    return dbfs > kMeterNoReadingDbfs;
}

// 0 at the floor, 1 at the ceiling, clamped; 0 with no reading.
[[nodiscard]] constexpr double meter_fraction(double dbfs)
{
    if (!meter_has_reading(dbfs)) {
        return 0.0;
    }
    const double t = (dbfs - kMeterFloorDbfs) / (kMeterCeilingDbfs - kMeterFloorDbfs);
    return std::clamp(t, 0.0, 1.0);
}

}  // namespace revenant::ui
