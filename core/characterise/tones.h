// Tone structure and envelope statistics: how many tones, how far apart,
// and whether the envelope carries anything at all.
//
// WHY THE HISTOGRAM AND NOT THE SPECTRUM
//
// A power spectrum shows where the energy is. An FSK signal's energy is at
// its tones, so a spectrum looks like it should answer this, and for a
// slow, wide shift it very nearly does. It stops answering as soon as the
// modulation index falls: at h below about 1 the tones merge into one lump
// whose peaks sit at neither tone, and at h = 0.5, which is MSK and every
// GMSK waveform in docs/modes.md, the spectrum has a single maximum in the
// middle and no tone structure visible at all.
//
// The instantaneous frequency has the tones whatever the index is, because
// the signal really does sit at one tone at a time and the histogram counts
// how long it spent there. What the index changes is how much of the time
// is spent in transit between them, which is the valley depth below, and
// that is a number this file reports rather than a threshold it hides.
//
// Written from Proakis, Digital Communications, chapter 4, for continuous
// phase frequency shift keying and the modulation index; and from Anderson,
// Aulin and Sundberg, Digital Phase Modulation, chapter 2, for what happens
// to the spectrum as the index falls, which is the reason this file exists
// at all. No GPL-licensed implementation was read; see docs/clean-room.md.
//
// THE ONE THING THIS HAS TO GET RIGHT, AND THE WAY IT GETS IT WRONG
//
// A broadcast FM carrier is not a two-tone FSK signal, and a mode counter
// that finds local maxima in the histogram and counts them says it is. The
// instantaneous frequency of an FM signal modulated by a tone has an
// arcsine distribution: it spends longest at the extremes of its swing, so
// the histogram has a maximum at each end with a dip between them. Two
// maxima, one dip, and every structural test a naive counter applies
// passes.
//
// What separates them is how deep the dip goes. An FSK signal is AT a tone
// except during the transition, so the valley between its modes is almost
// empty: measured on a clean 1200 baud 2-FSK signal at 40 samples a symbol,
// the valley is under a fortieth of either peak. An arcsine density's
// minimum is a bounded fraction of its maxima, because the modulating
// waveform passes through the middle of its range at finite speed on every
// cycle. ToneSearch::valley_fraction is where the line is drawn and
// ToneStructure::valley_ratio is the measurement, so a caller can see how
// close the call was rather than only which side of it the answer fell.
//
// THIS IS A HIGH SIGNAL-TO-NOISE INSTRUMENT, AND THE FIGURE IS WORSE THAN
// IT LOOKS
//
// The instantaneous frequency is a per-sample quantity computed on the
// extract as handed in, so the noise on it is set by the extract's
// FULL-BAND signal-to-noise ratio and by its sample rate, not by the
// signal's own bandwidth. One sample of phase difference carries a noise
// of roughly 1/sqrt(SNR) radians, which is rate/(2*pi*sqrt(SNR)) hertz,
// and that has to stay well under half the tone spacing for the modes to
// stay apart.
//
// Measured 2026-09-21 by tests/characterise/test_tones.cpp, on 4FSK at
// 4800 baud with 1296 Hz between tones, in an unfiltered 48 kS/s extract:
// four tones at 45 dB in 2500 Hz and above, a refusal on the mode width
// at 40, and one merged mode from 35 down. The estimators in
// core/characterise/cyclostationary.h work thirty decibels below that on
// the same signal, so this is the weakest link in the stage by a wide
// margin, and it is where the work goes next.
//
// Two levers, in the order they are worth taking. The extract:
// docs/detection.md's probe-receiver section already concludes that a
// classifier wants a real DemodStage rather than a raw coarse channel,
// and the 7.8 dB this case throws away by carrying 48 kHz of noise for an
// 8 kHz signal is exactly that argument in numbers. Then averaging the
// instantaneous frequency across a symbol, which divides the noise by the
// square root of the samples per symbol and costs nothing at the
// transitions IF the symbol timing is known. Neither is done here: this
// file takes a span and a rate and knows no symbol timing, and inventing
// one would make the estimator depend on another estimator's output
// without saying so.
//
// What it does do is refuse rather than report a merged pair as one tone
// at the average of two spacings. tests/characterise/test_characterise.cpp
// asserts that a 4FSK signal past this limit comes back as a
// constant-envelope waveform with an unattributed cycle frequency, which
// is a weaker claim and a true one.

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

// What the envelope is doing, in numbers that do not move with the level.
struct EnvelopeStats {
    double mean_power = 0.0;

    // var(|x|^2) / mean(|x|^2)^2. Dimensionless and independent of the
    // extract's gain, which is the whole point: an absolute variance says
    // more about the receiver's gain setting than about the waveform.
    //
    // Zero for a constant-envelope signal. One for circularly symmetric
    // complex Gaussian noise, exactly, because the squared magnitude is
    // then exponential and an exponential's variance equals the square of
    // its mean. A root-raised-cosine PSK signal sits between the two, and
    // so does a real constant-envelope signal in noise, which is why this
    // is evidence rather than a classifier on its own.
    double normalised_power_variance = 0.0;

    double peak_to_average_db = 0.0;
};

[[nodiscard]] EnvelopeStats envelope_stats(std::span<const Complex64> samples);

struct ToneMode {
    // Offset from the extract's own DC, in hertz. Interpolated by a
    // weighted centroid over the mode's basin rather than taken as the
    // centre of the tallest bin.
    double offset_hz = 0.0;

    // Share of the kept samples that fell in this mode's basin. Sums to one
    // across the modes plus whatever fell outside all of them.
    double weight = 0.0;

    // Standard deviation of the instantaneous frequency within the basin.
    // A clean tone's width is the noise on the phase difference; a wide one
    // means the mode is a lump rather than a tone.
    double width_hz = 0.0;
};

struct ToneSearch {
    // Bins across the histogram's range. 256 over a range set by the
    // signal's own spread, so the resolution follows the shift rather than
    // the sample rate: a 200 Hz shift and a 50 kHz shift both get about a
    // hundred bins between their tones.
    std::size_t bins = 256;

    // Moving average applied to the histogram before any maximum is looked
    // for. Without it the shot noise on a bin count produces maxima
    // wherever it likes, and the merge rule below then has to clean up
    // after it.
    std::size_t smoothing_bins = 5;

    // Samples whose power is below this fraction of the mean are dropped
    // before the histogram, along with their neighbour, because the phase
    // difference across a deep envelope null is noise with the sign of
    // whatever was left. A constant-envelope signal never trips it. A
    // shaped linear modulation trips it exactly where its own phase is
    // moving fastest, which is the part that would smear the histogram.
    double amplitude_gate = 0.25;

    // A maximum has to reach this share of the tallest one to be a mode.
    double mode_height_fraction = 0.15;

    // And this multiple of the histogram's mean height, which is what stops
    // a flat histogram of noise producing modes out of its own ripple. A
    // flat histogram has a maximum about a tenth above its mean once
    // smoothed, so 1.6 is a wide margin over the case it rejects.
    double mode_prominence = 1.6;

    // The valley between two modes has to fall below this fraction of the
    // weaker of them, or the two are one mode with a dip in it.
    //
    // 0.40. The two cases it separates are measured: a clean 2-FSK signal
    // at 40 samples a symbol leaves under 0.03 between its tones, and a
    // broadcast FM carrier's composite leaves far more, because an FM
    // signal is in transit between the extremes of its swing on every cycle
    // of every tone in its modulation.
    double valley_fraction = 0.40;

    // Largest a mode may be, as a fraction of the spacing between modes,
    // measured as the standard deviation of the instantaneous frequency
    // inside that mode's basin.
    //
    // This is the second half of the broadcast FM test and the valley rule
    // on its own is not enough. Measured 2026-09-21 on a stereo station
    // with RDS at 684 kS/s, the composite's instantaneous frequency broke
    // into six modes whose deepest valley was 0.25, comfortably inside
    // valley_fraction, because a sum of a few sinusoids has a rippled
    // density rather than a smooth arcsine one. What those six modes are
    // not is tones: each is a broad lump, because the signal sweeps
    // through the values around it rather than sitting on one.
    //
    // A tone is a value the signal IS at, so its histogram mass is a spike
    // whose width is the noise on the phase difference and nothing else.
    // Measured on clean 2-FSK and 4-FSK, the widest mode is a few percent
    // of the spacing.
    double tone_width_fraction = 0.25;

    // Refuse rather than report a comb nobody asked for. MFSK-64 exists;
    // anything above this is more likely a histogram that fell apart.
    std::size_t max_tones = 64;
};

struct ToneStructure {
    bool found = false;

    // Empty when found. Otherwise it names the number, the cause and the
    // fix.
    std::string refusal;

    std::size_t tone_count = 0;
    std::vector<ToneMode> tones;

    // Mean spacing between adjacent modes, and the largest departure from
    // it. An MFSK waveform's tones are evenly spaced by construction, so a
    // spread that is a large share of the spacing means the modes found are
    // not tones.
    double spacing_hz = 0.0;
    double spacing_spread_hz = 0.0;

    // Mean of the tone offsets, which for a symmetric tone set is the
    // carrier. Not the same as the centre of the occupied band for an
    // asymmetric set, and that is why it is reported separately.
    double centre_offset_hz = 0.0;

    // Deepest valley between adjacent modes, as a fraction of the weaker of
    // the two. Zero is a perfectly separated pair. Compared against
    // ToneSearch::valley_fraction.
    double valley_ratio = 0.0;

    // The widest mode's standard deviation over the mean spacing. Compared
    // against ToneSearch::tone_width_fraction.
    double widest_tone_fraction = 0.0;

    double confidence = 0.0;

    // Evidence, carried whether or not the structure was found, because it
    // is what a refusal has to quote.
    double histogram_low_hz = 0.0;
    double histogram_high_hz = 0.0;
    std::size_t samples_kept = 0;
    std::size_t samples_gated = 0;

    // Standard deviation of the whole instantaneous-frequency
    // distribution, taken as the interquartile range over 1.349, which is
    // the factor that makes the two agree for a Gaussian. A percentile
    // rather than a moment, so one sample of phase noise across an
    // envelope null does not set it. An unmodulated carrier's is the phase
    // noise and nothing else.
    double frequency_spread_hz = 0.0;
};

// Modes of the instantaneous-frequency histogram.
//
// Fails on a buffer the analysis cannot run on at all. Returns a record
// with found == false and a populated refusal when it ran and the answer is
// that the signal is not multi-tone, which is a result: a broadcast FM
// carrier, an unmodulated carrier and a linear modulation all land there,
// and the refusal says which of those the numbers look like.
[[nodiscard]] Expected<ToneStructure> estimate_tone_structure(
    std::span<const Complex64> samples,
    SampleRate rate,
    const ToneSearch& search = {});

}  // namespace revenant::characterise
