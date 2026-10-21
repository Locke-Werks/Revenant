// The time-shift scrub bar's arithmetic: where "live" is on the slider, what a
// position means in seconds behind live, and what the label says.
//
// The slider runs from the oldest sample held at 0 to live at 1, so moving
// left goes back in time the way a waterfall's history does. Positions within
// half a second of live snap to live: a replay that close is live with a
// second graph's cost, and a drag released near the right end means "back to
// now" rather than "four hundred milliseconds ago".
//
// This header holds no Qt; ui/tests links it.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace revenant::ui {

inline constexpr double kTimeShiftLiveSnapSeconds = 0.5;

// Seconds behind live for a slider position, given how many seconds are held.
// Zero means live.
[[nodiscard]] inline double timeshift_seconds_ago(double position, double held_seconds)
{
    if (!std::isfinite(position) || !std::isfinite(held_seconds) || held_seconds <= 0.0) {
        return 0.0;
    }
    const double ago = (1.0 - std::clamp(position, 0.0, 1.0)) * held_seconds;
    return ago < kTimeShiftLiveSnapSeconds ? 0.0 : ago;
}

// The inverse, for drawing the handle where a replay already is.
[[nodiscard]] inline double timeshift_position(double seconds_ago, double held_seconds)
{
    if (!std::isfinite(seconds_ago) || !std::isfinite(held_seconds) || held_seconds <= 0.0 ||
        seconds_ago <= 0.0) {
        return 1.0;
    }
    return std::clamp(1.0 - seconds_ago / held_seconds, 0.0, 1.0);
}

// "live", or "-m:ss" behind it, whole seconds rounded to nearest.
[[nodiscard]] inline std::string timeshift_label(double seconds_ago)
{
    if (!std::isfinite(seconds_ago) || seconds_ago < kTimeShiftLiveSnapSeconds) {
        return "live";
    }
    const auto whole = static_cast<std::int64_t>(std::llround(seconds_ago));
    const std::int64_t minutes = whole / 60;
    const std::int64_t seconds = whole % 60;
    return "-" + std::to_string(minutes) + ":" + (seconds < 10 ? "0" : "") +
           std::to_string(seconds);
}

// The wall clock a replay should start from, in Unix epoch nanoseconds, from
// the newest sample held and the seconds behind it.
[[nodiscard]] inline std::int64_t timeshift_target_ns(std::int64_t newest_ns, double seconds_ago)
{
    if (!std::isfinite(seconds_ago) || seconds_ago <= 0.0) {
        return newest_ns;
    }
    return newest_ns - static_cast<std::int64_t>(seconds_ago * 1.0e9);
}

}  // namespace revenant::ui
