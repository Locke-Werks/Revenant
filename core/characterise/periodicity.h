// What repeats, and how often: cyclic prefixes and frame periods.
//
// TWO READINGS OF ONE MEASUREMENT
//
// Both of these come out of the same normalised autocorrelation, and the
// thing that separates them is HOW MUCH of the signal repeats rather than
// how far apart the repeats are.
//
// A cyclic prefix is a PARTIAL repeat. An OFDM transmitter copies the last
// Ncp samples of each useful symbol in front of it, so the waveform
// correlates with itself at a lag of Nu, the useful symbol length, and the
// correlation is Ncp/(Nu + Ncp) because that is the share of each symbol
// period the copy occupies. A one-eighth guard gives 0.111. That fraction
// is the measurement Ncp comes back out of, which is why the ratio is
// reported and not only the lag.
//
// A frame period is a WHOLE repeat, or close to it. Something in the
// waveform comes round again: a sync word, a training sequence, a probe. It
// reaches whatever share of the frame that pattern occupies, and for a
// synthetic signal whose entire payload repeats it reaches one.
//
// The two overlap between 0.10 and 0.50 and the ceiling is what keeps them
// apart. A guard interval as long as its own useful symbol is not a guard
// interval, so a correlation at or above 0.5 is framing however OFDM-shaped
// the lag looks. Without that ceiling a BPSK signal whose payload repeats
// every 5120 samples is reported as an OFDM waveform with a 5120-sample
// symbol, which is the case tests/characterise/test_periodicity.cpp pins.
//
// WHY THIS IS FREE ON THE HARD TARGETS
//
// A frame period costs nothing on a waveform that will never be decoded,
// and that is the point of putting it in the characteriser rather than in a
// decoder. MIL-STD-188-110 serial tone interleaves a known probe with the
// data, so the superframe shows up in autocorrelation BECAUSE the probe is
// the part that is not encrypted. The same holds for any waveform with a
// training sequence: the framing travels in the clear under the payload,
// which docs/modes.md names as in scope for exactly this reason.
//
// Written from Proakis, Digital Communications, chapter 12, for the
// multicarrier signal and the guard interval; and from van de Beek,
// Sandell and Borjesson, "ML estimation of time and frequency offset in
// OFDM systems", IEEE Transactions on Signal Processing 45(7), July 1997,
// for the correlation at a lag of the useful symbol length that the cyclic
// prefix produces. No GPL-licensed implementation was read; see
// docs/clean-room.md.
//
// COST
//
// The autocorrelation is computed through the transform rather than lag by
// lag. Direct evaluation is one pass over the buffer per lag, which at
// 131072 samples and 16384 lags is two billion complex multiplies; through
// a zero-padded transform it is three transforms of twice the buffer,
// whatever the lag range, and the lag range then costs nothing. Both
// readings share one Autocorrelation for that reason.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/characterise/transform.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::characterise {

// Normalised magnitude autocorrelation, indexed by lag in samples.
struct Autocorrelation {
    // ratio[0] is 1 by construction. ratio[tau] is
    // |sum x[n] conj(x[n+tau])| divided by the same sum's value at zero lag
    // AND by the share of the buffer that overlaps at that lag, so a long
    // lag is not penalised for having fewer terms in it.
    //
    // Magnitude, not the complex value. A repeat that arrives with a
    // constant phase rotation on it is still a repeat: core/dsp/synth's
    // FSK carries its payload across a cycle boundary phase-continuously
    // but not phase-identically, and a real transmitter with any carrier
    // offset at all does the same.
    std::vector<double> ratio;

    std::size_t max_lag = 0;
    SampleRate rate = 0;
    std::size_t sample_count = 0;
};

// Largest lag the default search covers: one eighth of the buffer, capped.
//
// An eighth because the estimator divides by the overlapping share, and
// past an eighth there are too few terms left for the division to be
// stabilising rather than amplifying. Capped because the profile is
// searched linearly and nothing in docs/modes.md has a frame period longer
// than this at any rate a receiver extract runs at: 1.4 seconds at 48 kS/s,
// against WSPR's 110 s, which is the one case that genuinely wants a
// decimated extract.
inline constexpr std::size_t kMaxProfileLag = 1U << 16;

// max_lag of zero means min(samples/8, kMaxProfileLag).
[[nodiscard]] Expected<Autocorrelation> autocorrelation(std::span<const Complex64> samples,
                                                        SampleRate rate,
                                                        std::size_t max_lag = 0);

struct OfdmSearch {
    // Shortest and longest useful symbol the search will consider, in
    // samples. Zero means 32 and 8192, which at 48 kS/s is 0.67 ms to
    // 170 ms and covers everything multicarrier in docs/modes.md by a wide
    // margin: DAB Mode I's useful symbol is 1 ms and DRM30's longest is
    // 26.66 ms.
    std::size_t min_symbol_samples = 0;
    std::size_t max_symbol_samples = 0;

    // Smallest correlation that counts as a guard interval.
    //
    // 0.01 is a 1/99 guard, far shorter than any system in use, and that
    // is deliberate: the working floor should be what is MEASURABLE in the
    // extract rather than what somebody decided was a reasonable guard.
    // The measurable floor is floor_multiple times the profile's own
    // median, which falls as the extract lengthens, and this constant only
    // stops the search running away on a very long buffer where that
    // median goes to nothing.
    //
    // Set higher than deployment would suggest, it costs range for no
    // reason. Measured 2026-09-21 on a one-eighth-guard burst at 48 kS/s:
    // at 0.02 the detector stopped at 10 dB in 2500 Hz and at 0.01 it
    // reaches 5 dB, with the symbol period right at both.
    double min_prefix_ratio = 0.01;

    // And the ceiling, which is the rule that keeps this apart from
    // find_frame_period. A guard interval as long as its useful symbol
    // would be a 50 percent overhead nobody pays, and a correlation above
    // it means the whole waveform repeats.
    double max_prefix_ratio = 0.50;

    // How far over the profile's own median across the searched lags the
    // peak has to stand. That median is what an uncorrelated signal reads,
    // roughly one over the square root of the overlap, so this is the same
    // kind of statement as a margin over a spectral baseline.
    double floor_multiple = 6.0;

    // And how far over the lags immediately beside it, which is a
    // different test and was the one missing.
    //
    // A shaped signal's pulse correlates with itself out to its whole
    // span, so a root raised cosine over eight symbols leaves a decaying
    // ripple reaching eight symbol periods: measured 2026-09-21 on a 2400
    // baud BPSK signal with no framing at all, that ripple read 0.129 at
    // lag 32, sixteen times the median, and the reading came back as a
    // 32-sample OFDM symbol. A repeat is isolated and a shoulder is not.
    //
    // Three rather than six, because the neighbourhood is read at its
    // ninetieth percentile rather than its median, so it already sits
    // above the floor the other multiple is stated against. Against a
    // shoulder the ratio is near one and three is plenty; against noise a
    // sixfold bar here costs real range, and did: at six the detector
    // stopped at 10 dB in 2500 Hz on a burst it locates correctly at 5.
    double isolation_multiple = 3.0;
};

struct OfdmStructure {
    bool found = false;

    // Empty when found. Otherwise it names the number, the cause and the
    // fix.
    std::string refusal;

    // Useful symbol length: the lag the correlation peaked at.
    std::size_t symbol_samples = 0;
    double symbol_seconds = 0.0;

    // rate/symbol_samples. Together with the total symbol period this is
    // usually enough to name the system outright, which is what makes
    // this worth measuring on a waveform nobody can decode.
    double subcarrier_spacing_hz = 0.0;

    // Guard interval, from the correlation: ratio = Ncp/(Nu + Ncp), so
    // Ncp = ratio*Nu/(1 - ratio).
    //
    // THIS ONE IS BIASED BY NOISE AND THE SYMBOL LENGTH ABOVE IS NOT.
    //
    // The correlation's numerator is the signal matching itself and its
    // denominator is the whole buffer's power, noise included, so what
    // comes back is the true fraction times S/(S+N). Measured on a
    // one-eighth guard, which is 0.111 clean: 0.0947 at 20 dB in 2500 Hz,
    // 0.0376 at 10 dB, 0.0149 at 5 dB. The guard estimated from those is
    // 27, 10 and 4 samples against a true 32.
    //
    // The lag is unaffected, because where the peak sits does not move
    // when it shrinks. So a characterisation at low signal-to-noise is a
    // symbol period with a guard that is a lower bound, and
    // prefix_fraction is carried beside prefix_samples so a reader can see
    // which regime the number came from.
    std::size_t prefix_samples = 0;
    double prefix_fraction = 0.0;

    double total_symbol_seconds = 0.0;

    double correlation = 0.0;
    double floor_ratio = 0.0;
    double confidence = 0.0;
};

[[nodiscard]] OfdmStructure find_cyclic_prefix(const Autocorrelation& profile,
                                               const OfdmSearch& search = {});

struct FrameSearch {
    // Zero means 64 and the profile's own max_lag.
    std::size_t min_period_samples = 0;
    std::size_t max_period_samples = 0;

    // A frame repeats a pattern that is a real share of it. Below this the
    // peak is more likely the tail of a pulse shape than a frame.
    double min_repeat_ratio = 0.10;

    double floor_multiple = 6.0;

    // As OfdmSearch::isolation_multiple, and for the same reason.
    //
    // A frame's neighbourhood is not always empty even when the frame is
    // real. A payload of 256 random symbols correlates with a shifted copy
    // of itself at around one over the square root of 256, and the largest
    // of the shifts in the window reaches further: measured 2026-09-21 on
    // exactly that signal, the lags beside a perfect repeat reached 0.17.
    // A whole repeat still stands nearly six times over that, so this is
    // not the rule that carries the decision here. It is the rule that
    // stops a pulse shoulder being read as a frame, and min_repeat_ratio
    // already stops most of them.
    double isolation_multiple = 3.0;

    // Largest divisor the period walk will apply, for the same reason
    // CyclicSearch::max_harmonic_divisor exists: the strongest peak of a
    // repeating waveform can land on the second or third repeat rather
    // than the first, and reporting that is reporting a multiple of the
    // frame period as the frame period.
    std::size_t max_period_divisor = 8;
};

struct FramePeriod {
    bool found = false;
    std::string refusal;

    std::size_t period_samples = 0;
    double period_seconds = 0.0;

    // The correlation at the period, which is roughly the share of the
    // frame that repeats unchanged. One means the whole waveform comes
    // round again; a tenth means a sync pattern a tenth of the frame long.
    double repeat_fraction = 0.0;

    double floor_ratio = 0.0;
    std::size_t harmonic_divisor = 1;

    // The strongest peak before the walk, so a reader can see what the
    // answer was divided down from.
    std::size_t strongest_lag = 0;

    double confidence = 0.0;
};

[[nodiscard]] FramePeriod find_frame_period(const Autocorrelation& profile,
                                            const FrameSearch& search = {});

}  // namespace revenant::characterise
