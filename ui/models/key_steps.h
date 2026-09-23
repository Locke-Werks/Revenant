// The arithmetic behind the keys that move something by a step: which digit
// the tuning keys step, how far a page moves the radio, and how far a press
// moves the volume.
//
// Qt-free so ui/tests can hold it. The keys themselves are
// models/key_actions.h; ui/qml/Commands.qml calls these through KeyMap.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "models/frequency_dial.h"

namespace revenant::ui {

// The digit the tuning keys step when nothing has moved it: the kilohertz
// digit, counted from the hertz digit as models/frequency_dial.h counts. One
// kilohertz is the step an operator reaches for across most of HF and VHF;
// the hertz digit would take a thousand presses to cross one broadcast
// channel, and the megahertz digit skips whole bands.
inline constexpr int kDefaultTuningDigit = 3;

// The tuning digit moved one place, positive towards the more significant
// end, and held inside the digits the dial draws. A dial showing eight digits
// has places 0 to 7, and a digit left pointing past them would step a place
// nobody can see.
[[nodiscard]] constexpr int move_tuning_digit(int digit, int places, int digit_count)
{
    const int top = std::max(digit_count, 1) - 1;
    return std::clamp(digit + places, 0, top);
}

// A tenth of the span a page, the same fraction whatever the source rate, so
// 2.4 MS/s and 20 MS/s feel the same to the same key. Ten pages cross the
// span once, which leaves each new view overlapping the last by nine tenths:
// a page is for walking along a band, not jumping out of it.
inline constexpr int kPageSpanDivisor = 10;

// Where one page puts the front end: a tenth of the span up for a positive
// direction and down for a negative one, rounded to whole hertz and stopped at
// the source's tuning limits the way a dial step is. A span that is not a
// span yet moves nothing.
[[nodiscard]] inline std::int64_t page_tune_hz(std::int64_t centre_hz, std::int64_t span_low_hz,
                                               std::int64_t span_high_hz, int direction,
                                               const DialLimits& limits)
{
    const std::int64_t span = span_high_hz - span_low_hz;
    if (span <= 0 || direction == 0) {
        return centre_hz;
    }
    const std::int64_t step = (span + kPageSpanDivisor / 2) / kPageSpanDivisor;
    const std::int64_t next = centre_hz + (direction > 0 ? step : -step);
    if (!limits.valid()) {
        return next;
    }
    return std::clamp(next, limits.low_hz, limits.high_hz);
}

// Five hundredths a press. Twenty presses cross the whole slider, and each
// lands on a value the slider itself can hold: audio/audio_player.h persists
// the volume on every distinct value and the slider in ui/qml/AudioPane.qml
// steps in hundredths, so a key that moved it by a thousandth would write
// values the slider cannot show.
inline constexpr double kVolumeKeyStep = 0.05;

// The volume after some presses, in [0, 1] and on the hundredths grid.
[[nodiscard]] inline double step_volume(double volume, int presses)
{
    const double next = std::clamp(volume + presses * kVolumeKeyStep, 0.0, 1.0);
    return std::round(next * 100.0) / 100.0;
}

}  // namespace revenant::ui
