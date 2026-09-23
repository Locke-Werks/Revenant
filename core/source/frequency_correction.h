// A crystal's frequency error, applied in integer hertz.
//
// WHAT IS BEING CORRECTED. A cheap receiver derives its local oscillator and
// its sample clock from one crystal, and that crystal is off by some parts per
// million. On an RTL-SDR the R820T's synthesiser and the RTL2832U's resampler
// both run from the same 28.8 MHz part, so a crystal fast by e puts the LO at
// f * (1 + e) when f was asked for, and makes every sample come (1 + e) times
// sooner than the nominal rate says. A station on 162.550 MHz then reads at
// 162.550 MHz / (1 + e): at +20 ppm, 3.25 kHz low, which is the whole width
// of a narrowband voice channel.
//
// THE SIGN, which is the part that gets written backwards. Positive means the
// crystal runs FAST, so labels read LOW and the correction raises them. It is
// the sign librtlsdr's rtlsdr_set_freq_correction takes, so ppm=N on an
// rtlsdr URI and N * 1000 here describe the same device.
//
// PARTS PER BILLION AND NOT A DOUBLE. docs/conventions.md puts every frequency
// in integer hertz and names crystal correction as one of the steps that turns
// a double chain into a tone 3 Hz off with nobody able to say which stage put
// it there. A ppb count is an integer, a correction of it is a rational with a
// denominator of 10^9, and each function below rounds exactly once, half away
// from zero, in integer arithmetic. The same inputs give the same hertz on
// every machine.
//
// A ppm would not do as the integer: one ppm is 100 Hz at 100 MHz and 1.7 kHz
// at the top of an R820T's range, coarser than a narrowband channel is wide.
// librtlsdr's own correction is whole ppm for exactly that reason and is why
// the engine does not use it; see docs/calibration.md.
//
// Header-only and standard-library-only, because the Qt client computes a
// measured correction with the same arithmetic the engine applies, and it
// links nothing of the engine's. core/rpc/types.h is the precedent.

#pragma once

#include <cstdint>

namespace revenant::source {

// Parts per billion in one part.
inline constexpr std::int64_t kPartsPerBillion = 1'000'000'000;

// The largest correction anything here accepts, in either direction: a
// thousand ppm. The RTL-SDR backend refuses the same figure for ppm= with the
// sentence that a correction that large is not a crystal error, it is a
// different radio. The bound is also what keeps every product below inside 64
// bits: a remainder under 10^9 times a factor under 1.001 * 10^9.
inline constexpr std::int64_t kMaxCorrectionPpb = 1'000'000;

[[nodiscard]] constexpr bool correction_in_range(std::int64_t ppb) {
    return ppb >= -kMaxCorrectionPpb && ppb <= kMaxCorrectionPpb;
}

// a / b rounded half away from zero, for b > 0.
[[nodiscard]] constexpr std::int64_t divide_rounded(std::int64_t a, std::int64_t b) {
    return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b);
}

// Where a device tuned to `device_hz` is really listening: device * (1 + e),
// rounded to the hertz.
//
// Split at a billion so the product cannot overflow for any int64 input: the
// whole billions scale exactly and only the remainder, under 10^9, is
// multiplied by a ppb count under 10^6.
[[nodiscard]] constexpr std::int64_t device_to_true(std::int64_t device_hz, std::int64_t ppb) {
    const std::int64_t whole = device_hz / kPartsPerBillion;
    const std::int64_t rest = device_hz % kPartsPerBillion;
    return device_hz + whole * ppb + divide_rounded(rest * ppb, kPartsPerBillion);
}

// What to ask a device for so that it listens on `true_hz`: true / (1 + e),
// rounded to the hertz.
//
// Not the inverse of device_to_true in every last hertz, and it cannot be: the
// device takes whole hertz of its own scale and each of those is 1 + e true
// hertz wide, so a round trip can land one hertz away when the division falls
// within e of a half. The error is under half a hertz plus e hertz either way.
[[nodiscard]] constexpr std::int64_t true_to_device(std::int64_t true_hz, std::int64_t ppb) {
    const std::int64_t scale = kPartsPerBillion + ppb;
    const std::int64_t whole = true_hz / scale;
    const std::int64_t rest = true_hz % scale;
    return whole * kPartsPerBillion + divide_rounded(rest * kPartsPerBillion, scale);
}

// The correction a carrier measures, in ppb, from where it should be and where
// the engine placed it.
//
//   known_hz          the carrier's true frequency, which the operator knows
//   observed_hz       where the engine labels it now, with `current_ppb`
//                     already applied to the centre
//   source_center_hz  EngineInfo::source_center, the corrected centre that
//                     label was built from
//
// The label is the corrected centre plus a baseband offset, and the offset is
// counted on the device's own sample clock, which the correction does not
// touch. So the uncorrected label is the device's centre plus the same offset,
// and the ratio of the known frequency to that is exactly 1 + e whether the
// carrier sits on the centre or at the edge of the span.
//
// Returns the new total correction, which replaces current_ppb rather than
// adding to it. A result outside correction_in_range is returned as computed
// and left for the caller to refuse, because "the carrier you chose is 4000
// ppm away" is worth saying in words.
[[nodiscard]] constexpr std::int64_t measured_correction_ppb(std::int64_t known_hz,
                                                             std::int64_t observed_hz,
                                                             std::int64_t source_center_hz,
                                                             std::int64_t current_ppb) {
    const std::int64_t device_center = true_to_device(source_center_hz, current_ppb);
    const std::int64_t uncorrected = device_center + (observed_hz - source_center_hz);
    if (uncorrected == 0) {
        return current_ppb;
    }

    // (known - uncorrected) is at most a thousand ppm of the frequency, so
    // under 10^7 Hz below 10 GHz, and times 10^9 stays inside 64 bits. The
    // division is signed on both sides, so it is written on magnitudes.
    const std::int64_t difference = known_hz - uncorrected;
    const bool negative = (difference < 0) != (uncorrected < 0);
    const std::int64_t magnitude =
        divide_rounded(difference < 0 ? -difference * kPartsPerBillion
                                      : difference * kPartsPerBillion,
                       uncorrected < 0 ? -uncorrected : uncorrected);
    return negative ? -magnitude : magnitude;
}

static_assert(device_to_true(100'000'000, 0) == 100'000'000);
static_assert(device_to_true(100'000'000, 1'000) == 100'000'100);
static_assert(device_to_true(100'000'000, -2'500) == 99'999'750);
static_assert(true_to_device(100'000'100, 1'000) == 100'000'000);
static_assert(true_to_device(162'550'000, 20'000) == 162'546'749);
static_assert(device_to_true(162'546'749, 20'000) == 162'550'000);
static_assert(measured_correction_ppb(100'000'100, 100'000'000, 100'000'000, 0) == 1'000);
static_assert(measured_correction_ppb(100'000'000, 100'000'000, 100'000'000, 0) == 0);

}  // namespace revenant::source
