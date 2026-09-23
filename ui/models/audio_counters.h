// What the audio section says about the mix's clock trim and the two mix
// counters: the trim as a quiet fixed-width readout, and late audio skipped
// and streams realigned as chips that exist only while they are non-zero.
//
// WHY THE TWO COUNTERS ARE CHIPS AND THE TRIM IS NOT. The trim is the drift
// loop working, audio/drift_trim.h, and a number that is always there and
// always moving is a readout; it sits beside the three depths. The counters
// are each a place where the operator heard something jump: audio that
// reached the mix after its instant had been played and was passed over, and
// a stream moved to where the lead puts it, which is a click. Zero is their
// healthy state, so they are not on screen then, per the owner's rule for
// the design pass of 2026-09-22 that the chip says what is wrong and the
// detail says why.
//
// Qt-free, so ui/tests holds the wording and the arithmetic; ui/models/
// ui_rules.h hands them to QML.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace revenant::ui {

// The widest the trim readout prints: DriftTrim's 500 ppm clamp, negative.
inline constexpr std::string_view kTrimWidest = "-500.0 ppm";

// "+12.3 ppm". The sign is always printed, so the readout's width does not
// move as the trim crosses zero, and a trim that rounds to zero prints
// "+0.0 ppm" rather than "-0.0 ppm".
[[nodiscard]] inline std::string format_trim_ppm(double ppm)
{
    if (!std::isfinite(ppm)) {
        return "+0.0 ppm";
    }
    double shown = std::round(ppm * 10.0) / 10.0;
    if (shown == 0.0) {
        shown = 0.0;
    }
    char text[32] = {};
    std::snprintf(text, sizeof text, "%+.1f ppm", shown);
    return text;
}

// One heard stream's skipped frames at its own rate.
struct SkippedFrames {
    std::uint64_t frames = 0;
    std::uint32_t rate = 0;
};

// Late audio skipped across the heard streams, in milliseconds. Each stream
// is converted at its own rate before the sum, since a P25 receiver's 8000
// S/s frame is six of a 48000 S/s receiver's.
[[nodiscard]] inline double skipped_millis(std::span<const SkippedFrames> streams)
{
    double total = 0.0;
    for (const SkippedFrames& one : streams) {
        if (one.rate > 0) {
            total += 1000.0 * static_cast<double>(one.frames) / static_cast<double>(one.rate);
        }
    }
    return total;
}

[[nodiscard]] inline bool show_late_skipped(double millis)
{
    return millis > 0.0;
}

[[nodiscard]] inline bool show_realigned(std::uint64_t count)
{
    return count > 0;
}

// The chips' sentences, verbatim on hover.
[[nodiscard]] inline std::string late_skipped_detail(double millis)
{
    char text[320] = {};
    std::snprintf(text, sizeof text,
                  "%.0f ms of audio arrived after the mix had played its instant and was "
                  "skipped, across the receivers heard now. A receiver joining the mix or "
                  "the focus moving can skip a little once; a count that keeps climbing is "
                  "a receiver whose audio arrives late.",
                  millis);
    return text;
}

[[nodiscard]] inline std::string realigned_detail(std::uint64_t count)
{
    char text[320] = {};
    std::snprintf(text, sizeof text,
                  "a stream was moved to where the focused receiver puts it %llu time%s since "
                  "the output opened, each one a jump in what that receiver plays: a stream "
                  "more than 5 ms off the focused one's instant, or the focused receiver moved "
                  "past audio its own ring had to drop.",
                  static_cast<unsigned long long>(count), count == 1 ? "" : "s");
    return text;
}

}  // namespace revenant::ui
