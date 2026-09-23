// The arithmetic behind a frequency dial: which digit is which, what one
// notch of the wheel on a digit does, and where the limits stop it.
//
// A dial shows a frequency as a row of digits and lets the operator step any
// one of them with the wheel. What that step does is the part that can be
// wrong in a way nobody notices until they are somewhere else: whether a
// nine rolls over into the next digit up or wraps to zero on its own, what a
// step below zero does, and what happens at the edge of what the radio can
// tune. So it lives here, with no Qt, and ui/tests asserts it.
//
// CARRY AND BORROW, NOT WRAP. A step is an addition of one place value, so a
// nine stepped up becomes a zero and the digit above goes up by one, exactly
// as it would on paper. Some radios wrap the digit alone instead, which keeps
// the rest of the number still and makes the dial disagree with the spectrum
// under it: stepping the kilohertz digit past nine would jump the receiver
// nine kilohertz back down. The dial here moves the frequency by the amount
// the digit says, always.
//
// THE LIMITS CLAMP RATHER THAN REFUSE. A step that would leave the range
// lands on the edge of it, so the last notch of a fast spin toward the top of
// a tuner still gets there, and the result says it was clamped so the dial can
// show that it stopped against something.
//
// Frequencies are integer hertz, per docs/conventions.md, and may be negative:
// a synthetic scene centred on zero puts half its receivers below it. A digit
// is a digit of the magnitude, and the sign is shown separately.

#pragma once

#include <algorithm>
#include <cstdint>

#include "models/scroll_tune.h"

namespace revenant::ui {

// Twelve digits reach 999.999999999 GHz, which is past anything a front end
// this client talks to can tune and keeps the arithmetic below well inside an
// int64.
inline constexpr int kDialMaxDigits = 12;

// The fewest digits a dial shows, which is enough for a frequency in the tens
// of megahertz: two digits of megahertz, three of kilohertz, three of hertz.
// Fewer than that and a dial on an HF receiver would change width as it was
// tuned across ten megahertz.
inline constexpr int kDialMinDigits = 8;

// What the dial may be tuned to. A pair that is not a range means nothing is
// known about the limits yet, and nothing is clamped.
struct DialLimits {
    std::int64_t low_hz = 0;
    std::int64_t high_hz = 0;

    [[nodiscard]] constexpr bool valid() const { return high_hz > low_hz; }
};

struct DialStep {
    std::int64_t hz = 0;

    // The step was stopped by a limit, so hz is the limit and not the sum.
    bool clamped = false;
};

// Ten to the power digit, where digit 0 is the hertz digit. Zero for a digit
// outside what a dial shows, which makes a step on it a step of nothing.
[[nodiscard]] constexpr std::int64_t dial_place(int digit)
{
    if (digit < 0 || digit >= kDialMaxDigits) {
        return 0;
    }
    std::int64_t place = 1;
    for (int i = 0; i < digit; ++i) {
        place *= 10;
    }
    return place;
}

// The digit at a place, of the magnitude.
[[nodiscard]] constexpr int dial_digit(std::int64_t hz, int digit)
{
    const std::int64_t place = dial_place(digit);
    if (place == 0) {
        return 0;
    }
    const std::int64_t magnitude = hz < 0 ? -hz : hz;
    return static_cast<int>((magnitude / place) % 10);
}

// How many digits the magnitude has, counting a zero as one digit.
[[nodiscard]] constexpr int dial_significant_digits(std::int64_t hz)
{
    std::int64_t magnitude = hz < 0 ? -hz : hz;
    int count = 1;
    while (magnitude >= 10 && count < kDialMaxDigits) {
        magnitude /= 10;
        ++count;
    }
    return count;
}

// How many digits to draw: enough for the value and for the top of the range
// together, so the dial does not change width while it is tuned toward the
// top, and never fewer than kDialMinDigits.
[[nodiscard]] constexpr int dial_digit_count(std::int64_t hz, const DialLimits& limits)
{
    int count = std::max(kDialMinDigits, dial_significant_digits(hz));
    if (limits.valid()) {
        count = std::max(count, dial_significant_digits(limits.high_hz));
        count = std::max(count, dial_significant_digits(limits.low_hz));
    }
    return std::min(count, kDialMaxDigits);
}

// One wheel notch, or several, on one digit.
[[nodiscard]] constexpr DialStep step_dial(std::int64_t hz, int digit, int notches,
                                           const DialLimits& limits)
{
    DialStep out;
    out.hz = hz + static_cast<std::int64_t>(notches) * dial_place(digit);
    if (limits.valid()) {
        const std::int64_t held = std::clamp(out.hz, limits.low_hz, limits.high_hz);
        out.clamped = held != out.hz;
        out.hz = held;
    }
    return out;
}

// What one wheel event over a digit does: whole notches to step it by, and
// the travel left over for the next event.
struct DialWheel {
    int notches = 0;
    double carry_eighths = 0.0;
};

// The wheel turns a digit the way it turns everything else that tunes, which
// is scroll_tune_eighths in models/scroll_tune.h: a wheel rolled away from the
// operator steps the digit down. The dial used to read angleDelta.y raw in
// its QML, so it would have kept the old direction on its own when the span
// displays were turned round; resolving it here is what keeps the two
// together.
//
// A touchpad's fraction of a notch is carried, and a notch is counted towards
// zero, so travel the other way first cancels what is carried before it
// steps anything.
[[nodiscard]] inline DialWheel dial_wheel(double carry_eighths, double delta_x, double delta_y)
{
    DialWheel out;
    const double travel = carry_eighths + scroll_tune_eighths(delta_x, delta_y);
    out.notches = static_cast<int>(travel / kWheelNotchEighths);
    out.carry_eighths = travel - static_cast<double>(out.notches) * kWheelNotchEighths;
    return out;
}

// Where a separator goes after a digit, reading from the most significant
// down. After the megahertz digit comes the decimal point of a reading in
// megahertz; after the kilohertz digit and after the gigahertz digit, a thin
// gap, so the eye can count groups of three without counting digits.
enum class DialSeparator : std::uint8_t {
    None,
    Point,
    Gap,
};

[[nodiscard]] constexpr DialSeparator dial_separator_after(int digit)
{
    if (digit == 6) {
        return DialSeparator::Point;
    }
    if (digit == 3 || digit == 9) {
        return DialSeparator::Gap;
    }
    return DialSeparator::None;
}

}  // namespace revenant::ui
