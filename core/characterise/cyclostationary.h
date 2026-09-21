// Symbol rate and modulation order, from cyclic features rather than data.
//
// WHAT A CYCLIC FEATURE IS, AND WHY IT IS THE RIGHT INSTRUMENT HERE
//
// A digital signal is not stationary. Its statistics repeat at the symbol
// rate, because the pulse train that carries the data repeats at the symbol
// rate whatever the data is. A second-order statistic of the signal
// therefore has discrete Fourier components at multiples of the symbol rate,
// and those components are there whether the payload is plain text, a
// compressed voice frame or a block of ciphertext. That is the whole reason
// core/characterise exists as a stage separate from core/decode: the
// features below survive encryption, because encryption changes the bits and
// the bits are not what carries them.
//
// Written from Gardner, "Exploitation of spectral redundancy in
// cyclostationary signals", IEEE Signal Processing Magazine 8(2), April
// 1991, for the cyclic-feature framework; Oerder and Meyr, "Digital filter
// and square timing recovery", IEEE Transactions on Communications 36(5),
// May 1988, for the squared-envelope line at the symbol rate and the
// condition on excess bandwidth that has to hold for it to exist; and
// Mengali and D'Andrea, Synchronization Techniques for Digital Receivers,
// chapter 5, for the M-th power law that recovers a PSK carrier and, with
// it, the modulation order. No GPL-licensed implementation was read; see
// docs/clean-room.md.
//
// THE TWO DETECTORS, AND WHY NEITHER ONE IS ENOUGH
//
// SquaredEnvelope takes the spectrum of |x|^2. For a linearly modulated
// signal, x(t) = sum a_k p(t - kT), the expected squared envelope is
// sigma^2 * sum p^2(t - kT), which is periodic in T and therefore has a line
// at 1/T. Oerder and Meyr's condition is that p must have energy beyond
// 1/(2T) for p^2 to have any at 1/T, which is to say the excess bandwidth
// has to be non-zero. A root raised cosine at rolloff 0.35, which is this
// project's default everywhere, clears that comfortably, and the same
// condition is why there is no line at 2/T unless the rolloff exceeds 1.
//
// It reads nothing at all on a constant-envelope signal, because |x|^2 is a
// constant. Every FSK and CPM waveform in docs/modes.md is constant
// envelope, which is most of the list.
//
// FrequencyTransition takes the spectrum of the absolute second difference
// of the phase, which is how fast the instantaneous frequency is changing.
// On a constant-envelope signal that is a train of impulses at the symbol
// boundaries, one per transition, so it has lines at every multiple of the
// symbol rate. It is the detector for the half of the list SquaredEnvelope
// cannot see.
//
// Because it produces a whole comb rather than one line, the strongest peak
// it finds is often a harmonic. The comb walk in estimate_symbol_rate is
// what turns that back into the fundamental, and it is written to refuse a
// division it cannot support rather than to halve hopefully.
//
// A REPEATING PAYLOAD IS A COMB, AND IT IS NOT THE SYMBOL RATE
//
// Anything periodic in the signal is periodic in the feature. A payload
// that repeats every P samples makes |x|^2 repeat every P samples, which
// puts a line at every multiple of rate/P, and those lines are as sharp as
// the one at the symbol rate because they are lines for the same reason.
//
// This is not hypothetical and it is not only a test artefact.
// core/dsp/synth/modulators.h repeats its payload for the life of an
// emitter, deliberately, so a BER harness has something to compare against,
// and measured 2026-09-21 a 256-symbol payload at 2400 baud put its
// strongest squared-envelope line at 3008 Hz rather than 2400 Hz: a comb
// line of the 9.375 Hz payload repeat, 18.9 dB up. The characteriser was
// right about what stood highest and wrong about what it meant.
//
// A real signal's framing does the same thing more weakly, because a frame
// repeats a sync pattern rather than the whole payload, so the comb carries
// a fraction of the energy rather than all of it. core/characterise/
// periodicity.h is what finds that frame, and the two readings belong
// together: a symbol rate reported without saying whether a frame comb was
// also present is a number with a known way of being wrong.
//
// The tests in tests/characterise use a payload longer than the buffer, so
// nothing repeats inside the analysis, and they assert that rather than
// assuming it.
//
// WHAT THIS FILE DOES NOT CLAIM
//
// The M-th power test separates BPSK from QPSK from 8-PSK. It does not
// separate PSK from FSK, and reading it as though it did is the mistake to
// avoid: squaring a two-tone FSK signal gives two lines at twice each tone,
// both of which clear the threshold and neither of which is a carrier, so
// the order it reports for an FSK signal is 2 and means nothing.
//
// ModulationOrder::order is therefore only meaningful on a signal already
// known not to be multi-tone. core/characterise/tones.h is what establishes
// that, from the instantaneous-frequency histogram, and
// core/characterise/characterise.cpp runs it first for this reason.
// PowerLawLine::line_count is reported as the corroborating evidence, a
// count of lines within ten decibels of the strongest, and it is NOT used
// as a gate here: gating on it refused a QPSK signal this function had
// already identified correctly, because the pedestal of a fourth-power
// spectrum carries maxima of its own.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/characterise/transform.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::characterise {

enum class CyclicDetector : std::uint8_t {
    // Spectrum of |x|^2. Linear modulations with non-zero excess bandwidth.
    SquaredEnvelope,

    // Spectrum of the absolute second difference of phase. Constant-envelope
    // modulations, where the envelope carries nothing.
    FrequencyTransition,
};

[[nodiscard]] std::string_view cyclic_detector_name(CyclicDetector detector);

// The band of cycle frequencies a search covers, and the bar a peak has to
// clear inside it.
struct CyclicSearch {
    // Zero means rate/1024, which at 48 kS/s is 47 Hz.
    //
    // Not lower, and the reason is the pedestal rather than arithmetic. The
    // squared envelope of a shaped signal has a broad lump reaching down to
    // DC, and a local baseline over a block of bins cannot follow its slope
    // where the slope is steepest, which is the first few tens of hertz. A
    // symbol rate genuinely below this wants a decimated extract rather than
    // a wider search: PSK31 at 31.25 baud is 1.5 kS/s of signal, and asking
    // for it at 48 kS/s is asking the estimator to find a line 1/1536 of the
    // way along its own axis.
    double min_symbol_rate_hz = 0.0;

    // Zero means rate/4. A symbol rate above that is not resolvable from a
    // buffer at this rate in any case: at rate/2 the pulse train is at
    // Nyquist.
    double max_symbol_rate_hz = 0.0;

    double threshold_db = kDetectionMarginDb;

    // Largest divisor the comb walk may apply to the strongest peak. Zero
    // means the detector's own default, which is 1 for SquaredEnvelope and 4
    // for FrequencyTransition.
    //
    // The asymmetry is the theory rather than a tuning. Oerder and Meyr's
    // line exists at 1/T because p^2 has energy there, and it has none at
    // 2/T unless the excess bandwidth exceeds 100 percent, which no root
    // raised cosine reaches. So a linear modulation's squared envelope
    // carries ONE line and a walk over it can only ever turn a right answer
    // into a wrong one. Measured 2026-09-21 on a 2400 baud BPSK signal at
    // 30 dB in 2500 Hz: the pedestal under the line is rough enough that a
    // bin near 1200 Hz stood 6.2 dB over its own block median, so a walk
    // that was allowed to divide reported half the symbol rate at high SNR
    // and the right one at 10 dB, which is the worst possible way for an
    // estimator to fail.
    //
    // The transition detector's feature is a train of impulses at the symbol
    // boundaries, whose harmonics are equal in expectation, so which one
    // comes out strongest is decided by estimation noise and the walk is
    // what makes the answer stable.
    //
    // An on-off keyed signal is the case that wants this raised on the
    // squared envelope: its |x|^2 is a rectangular pulse train with real
    // harmonics at every multiple. Nothing sets it automatically, because
    // knowing the signal is OOK is a characterisation and this is one of its
    // inputs.
    std::size_t max_harmonic_divisor = 0;
};

// Smallest feature the estimators will work on, as a dimensionless ripple.
//
// A constant-envelope signal's squared envelope is constant, so the
// squared-envelope feature is nothing but float32 rounding, and rounding is
// not white: a phase stepping between two exact rates rounds in a pattern
// periodic at those rates. Measured 2026-09-21 on a clean 1200 baud 2-FSK
// signal, the squared-envelope search reported a cycle frequency of 9600 Hz
// at a margin of 81 dB, entirely out of quantisation structure 1e-16 below
// the carrier. The margin was real. What it was a margin over was nothing.
//
// A margin is a ratio and a ratio cannot tell that its numerator is noise
// from the last bit of a float. This floor is what does, and it is why the
// estimators report a refusal naming the detector rather than a number
// nobody can source.
inline constexpr double kMinFeatureRipple = 1.0e-3;

// A detector's feature signal, with the one scalar that says whether there
// is anything in it.
struct CyclicFeature {
    // Demeaned and dimensionless. For SquaredEnvelope that is the envelope's
    // fractional departure from its own mean power; for FrequencyTransition
    // it is radians of phase curvature per sample.
    std::vector<double> values;

    // Root mean square of values. Compared against kMinFeatureRipple.
    double ripple = 0.0;

    // What ripple is measured in, for the refusal text.
    std::string_view units;
};

struct SymbolRateEstimate {
    bool found = false;

    // Empty when found. Otherwise it names the number, the cause and the
    // fix, which is the rule this project learned on a real radio: a decoder
    // that is not locked has to look different from a signal that is not
    // there, and a caller handed a bare false cannot tell those apart.
    std::string refusal;

    // Root mean square of the detector's feature, and the floor it was
    // compared against. Carried whether or not the estimate succeeded,
    // because it is the first thing to look at when it did not.
    double feature_ripple = 0.0;
    std::string_view feature_units;

    double symbol_rate_hz = 0.0;

    // Margin of the fundamental over its own local baseline, which is the
    // line the estimate rests on and not necessarily the strongest peak in
    // the search band.
    double margin_db = 0.0;

    double confidence = 0.0;

    CyclicDetector detector = CyclicDetector::SquaredEnvelope;

    // Cycle-frequency bin width. The parabolic interpolation lands well
    // inside one of these, but nothing here resolves two cycle frequencies
    // closer together than this, and a caller comparing against a catalogue
    // row needs to know it.
    double resolution_hz = 0.0;

    double searched_low_hz = 0.0;
    double searched_high_hz = 0.0;

    // The strongest peak in the band, before the comb walk, and how many
    // times it was divided to reach the fundamental. Reported because a
    // divisor above one is the estimate most likely to be wrong, and a
    // reader looking at a suspicious answer should not have to re-run the
    // search to see what it started from.
    double strongest_alpha_hz = 0.0;
    double strongest_margin_db = 0.0;
    std::size_t harmonic_divisor = 1;
};

// Cycle frequency of the fundamental, from one detector.
//
// Fails on a buffer the analysis cannot work on at all: too short, a
// non-positive rate, a search band that is empty or outside the spectrum.
// Returns a record with found == false when the analysis ran and nothing in
// the band cleared the threshold, which is a result rather than an error and
// carries the numbers a refusal has to quote.
[[nodiscard]] Expected<SymbolRateEstimate> estimate_symbol_rate(
    std::span<const Complex64> samples,
    SampleRate rate,
    CyclicDetector detector,
    const CyclicSearch& search = {});

// One exponent's worth of the M-th power test.
struct PowerLawLine {
    bool found = false;

    // 2, 4 or 8.
    int exponent = 0;

    // Where the line sits in the spectrum of x^exponent.
    double frequency_hz = 0.0;

    // frequency_hz/exponent, which is the carrier offset MODULO rate/exponent.
    // Raising to a power wraps the frequency axis, and nothing in this file
    // can undo that: at exponent 4 a carrier at +9 kHz and one at -3 kHz in a
    // 48 kS/s buffer both put their line at 36 kHz, which is -12 kHz. The
    // ambiguity is real and it is reported rather than resolved, because
    // resolving it needs a second observation this function does not have.
    double carrier_offset_hz = 0.0;

    double margin_db = 0.0;

    // Lines comparable with the strongest: local maxima at least four bins
    // apart, clearing the detection threshold AND sitting within ten
    // decibels of the strongest line. One is a PSK carrier. Two is what
    // squaring a two-tone FSK signal produces, because its two tones carry
    // the same power, and that is the case this count exists for.
    //
    // Comparable rather than simply present. A count of every maximum over
    // the threshold counts the pedestal's own roughness alongside the line,
    // which on a clean 2400 baud BPSK signal came back well above one
    // against a line standing 53 dB up.
    std::size_t line_count = 0;
};

struct ModulationOrder {
    bool found = false;

    // The exponent whose line stands highest, which for a PSK constellation
    // is its order: 2 for BPSK, 4 for QPSK, 8 for 8-PSK.
    //
    // Highest, not smallest, and the first version of this said smallest.
    // The reasoning was that BPSK has a line at exponent 4 as well, since
    // squaring a tone gives another tone, so the smallest exponent with a
    // line ought to be the order. It measured wrong in the other direction:
    // QPSK stood 14.5 dB up at exponent 2 on pedestal structure alone,
    // cleared the 6 dB threshold there, and came out BPSK. Highest is the
    // physical statement, because at the constellation's own order the
    // modulation cancels exactly and above it the tone is squared together
    // with its own noise.
    //
    // Zero when no exponent beat the others by kOrderSeparationDb, which is
    // an ambiguous answer reported as one rather than a coin flip.
    int order = 0;

    double carrier_offset_hz = 0.0;
    double margin_db = 0.0;
    double confidence = 0.0;

    // Exponents 2, 4 and 8, in that order, whether or not each was taken.
    std::array<PowerLawLine, 3> lines{};
};

// The M-th power test at exponents 2, 4 and 8.
//
// The buffer is normalised to unit mean power first. x^8 on a buffer whose
// samples run to 30 is 6.5e11, which is fine in double and is not fine in
// the float32 a caller may have handed in; normalising costs one pass and
// removes the question.
[[nodiscard]] Expected<ModulationOrder> estimate_modulation_order(
    std::span<const Complex64> samples,
    SampleRate rate,
    double threshold_db = kDetectionMarginDb);

// The feature signals, exposed because a test that asserts against the
// estimate alone cannot tell a broken feature from a broken search, and
// because a caller wanting a different search over the same feature should
// not pay for it twice.
[[nodiscard]] CyclicFeature squared_envelope_feature(std::span<const Complex64> samples);
[[nodiscard]] CyclicFeature frequency_transition_feature(std::span<const Complex64> samples);

}  // namespace revenant::characterise
