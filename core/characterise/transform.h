// Spectral primitives for the waveform characteriser.
//
// WHY THIS DOES NOT USE reference_fft_radix2
//
// core/dsp/pfb_fft_reference.h already carries a radix-2 transform, and it is
// the right one for what it is: a referee written to match the butterfly
// order of core/shaders/pfb_fft.comp bit for bit. Two properties make it the
// wrong tool here. It is float32, because the kernel it refereees is, and it
// takes its twiddle circle from dsp::build_twiddles, which core/dsp/pfb.h
// caps at the channelizer's 2048-point grid. The cyclic-feature search below
// wants tens of thousands of points, in double, and it is not refereeing
// anything: no kernel computes a characterisation, so there is no twin for
// this code to be bit-exact against and no reason to inherit a cap that
// exists to keep a transform inside a compute shader's shared memory.
//
// So this is an ordinary Cooley-Tukey transform, written from Oppenheim and
// Schafer, Discrete-Time Signal Processing, chapter 9, for the radix-2
// decimation-in-time graph and its bit-reversed load, and from Harris, "On
// the use of windows for harmonic analysis with the discrete Fourier
// transform", Proceedings of the IEEE 66(1), January 1978, for the Hann
// window and its coherent and noise gains. Welch's method is Welch, "The use
// of fast Fourier transform for the estimation of power spectra", IEEE
// Transactions on Audio and Electroacoustics AU-15(2), June 1967. No
// GPL-licensed implementation was read; see docs/clean-room.md.
//
// PRECISION, AND WHY THE DENORMAL RULE DOES NOT REACH HERE
//
// Everything is double. docs/conventions.md requires a reference translation
// unit to open a ScopedDenormalFlush so that it models a GPU that has no
// choice about flushing. Nothing in core/characterise is a GPU twin and
// nothing compares it against a device, so there is no divergence to model.
// The inputs are normalised sample buffers whose smallest interesting
// quantity is a noise floor tens of decibels down, which is nowhere near
// 2.2e-308.
//
// PURITY
//
// Every function here is a pure function of its arguments. No clock is read,
// no state is kept between calls, and nothing is random. docs/conventions.md,
// "Purity in the sample path".

#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::characterise {

using dsp::ConstComplexSpan;
using dsp::SampleRate;

// The analysis works in double throughout. Complex32 is the sample buffer's
// type and it is converted on the way in, once.
using Complex64 = std::complex<double>;

// Largest transform this file will run. 2^20 complex doubles is 16 MiB of
// scratch, which is the point at which a characterisation stops being a thing
// you run per detection. Nothing here needs it: the segment picker below tops
// out four orders of magnitude lower.
inline constexpr std::size_t kMaxTransform = 1u << 20;

[[nodiscard]] constexpr bool is_power_of_two(std::size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

// Largest power of two not exceeding value. Zero for zero.
[[nodiscard]] std::size_t floor_power_of_two(std::size_t value);

// In-place radix-2 decimation-in-time transform. The span's length must be a
// power of two, which the caller has already arranged; a length that is not
// is a programming error rather than a runtime condition, so this returns
// void and the callers below validate.
void fft_in_place(std::span<Complex64> data);

// Periodic Hann window, w[n] = 0.5 - 0.5*cos(2*pi*n/N).
//
// Periodic rather than symmetric because these windows are used for spectral
// estimation with overlap-add averaging, where the periodic form is the one
// whose overlapped copies sum flat. The symmetric form is for filter design.
[[nodiscard]] std::vector<double> hann_window(std::size_t length);

// The segment length the analysis uses for a buffer of the given size.
//
// One sixteenth of the buffer, rounded down to a power of two, clamped to
// [512, 32768]. The ratio is the whole decision and it is a bias-variance
// trade: a longer segment resolves two cycle frequencies that sit close
// together, and a shorter one averages more segments so that the noise floor
// of the estimate is flatter and a peak-over-baseline margin means something.
//
// One sixteenth gives about thirty-one half-overlapped segments, so the
// estimate's relative standard deviation is roughly 1/sqrt(31), 18 percent,
// and the largest of a few thousand such bins sits about 1.5 dB over their
// median. The detection threshold in cyclostationary.h is 6 dB, which is four
// times that. Measured rather than assumed: tests/characterise/
// test_transform.cpp runs the baseline against pure noise and asserts the
// margin stays under 4 dB, which is the number that has to hold for a
// refusal on noise to be a refusal rather than luck.
[[nodiscard]] std::size_t analysis_segment(std::size_t sample_count);

// An averaged power spectrum, with everything a caller needs to turn a bin
// back into a frequency.
struct PowerSpectrum {
    // Mean periodogram over the segments, on the noise-equivalent
    // normalisation: a bin reads the per-sample variance of white noise,
    // whatever the segment length, and a tone on a bin centre reads
    // 2*segment/3 times its power under the Hann window. Only ratios reach a
    // decision in this directory, so the constant never matters and the
    // invariance across segment lengths does.
    std::vector<double> bins;

    double bin_width_hz = 0.0;
    SampleRate rate = 0;
    std::size_t segment = 0;
    std::size_t segments_averaged = 0;

    // True when the input was real and only bins 0 through segment/2 are
    // present, so bin k is always the positive frequency k*bin_width_hz.
    // False when the input was complex and all segment bins are present in
    // natural transform order, where bin k above segment/2 is the negative
    // frequency (k - segment)*bin_width_hz.
    bool one_sided = false;

    [[nodiscard]] double frequency_at(double bin) const;

    // Nearest bin to a frequency, wrapping negative frequencies into the
    // upper half for a two-sided spectrum. Returns bins.size() when the
    // frequency is outside what this spectrum represents.
    [[nodiscard]] std::size_t bin_at(double frequency_hz) const;
};

// Welch's method over a complex signal. hop of zero means segment/2.
//
// Fails rather than guessing when the signal is shorter than one segment: a
// characterisation computed from a single unaveraged periodogram has a
// baseline whose largest excursion is 8 to 10 dB, which is above the
// detection threshold, so it would find a symbol rate in anything.
[[nodiscard]] Expected<PowerSpectrum> welch_spectrum(std::span<const Complex64> signal,
                                                     SampleRate rate,
                                                     std::size_t segment,
                                                     std::size_t hop = 0);

// The same over a real signal, returning the one-sided spectrum.
[[nodiscard]] Expected<PowerSpectrum> welch_spectrum_real(std::span<const double> signal,
                                                          SampleRate rate,
                                                          std::size_t segment,
                                                          std::size_t hop = 0);

// A local noise-and-pedestal estimate, one value per bin.
//
// A median over a block of neighbouring bins rather than a mean, because the
// thing being estimated is what the spectrum would read without the peak, and
// a mean over a region containing the peak is raised by the peak it is
// measuring. That is the same argument core/detect/detector.h makes for its
// noise floor and it is the same mistake in a different axis.
//
// Local rather than global because a cyclic feature sits on top of a pedestal
// that is not flat. The squared envelope of a root-raised-cosine PSK signal
// has a broad lump reaching to the symbol rate, and the line this file is
// looking for sits on the shoulder of it. Measured against a global median
// the line's margin reads several decibels higher than it is, which is the
// direction that invents detections.
//
// block is the number of bins in one median. Blocks do not overlap and every
// bin in a block gets that block's median, which is a step function rather
// than a smooth curve; the alternative costs a sort per bin and buys nothing
// a 6 dB threshold can tell apart.
[[nodiscard]] std::vector<double> local_baseline(std::span<const double> spectrum,
                                                 std::size_t block);

// The margin every estimator in this directory states its finding against,
// and the threshold every one of them refuses below.
//
// Six decibels, from the measurement in tests/characterise/
// test_transform.cpp: over a Welch estimate averaged the way
// analysis_segment arranges, the largest bin in pure noise stands under 4 dB
// over its own local baseline, so 6 dB is half as much again as the loudest
// thing an empty channel produces. It is not a probability and it is not
// derived from one; it is a measured distance from what nothing looks like.
inline constexpr double kDetectionMarginDb = 6.0;

// Turns a margin into a confidence, by a map that is written down rather
// than tuned until the numbers looked good.
//
//   confidence = 1 - 0.5*exp(-(margin - threshold)/6)
//
// which is 0.5 exactly at the threshold, 0.82 six decibels above it, 0.93 at
// twelve and approaches 1 without reaching it. Below the threshold it is
// zero, because the caller is refusing rather than reporting.
//
// docs/detection.md asks for this to be calibrated, on the grounds that a
// classifier reporting 0.9 for everything makes the operator's confidence
// threshold a no-op. What is claimed here is narrower than calibration
// against a labelled corpus, and saying so is the point: this is a stated
// monotone function of a measured margin, so two findings can be ordered and
// a threshold on it is a threshold on how far the evidence stood above the
// noise. tests/characterise/test_cyclostationary.cpp asserts the ordering
// holds across an SNR sweep, which is the property a constant would fail.
[[nodiscard]] double margin_confidence(double margin_db,
                                       double threshold_db = kDetectionMarginDb);

struct SpectralPeak {
    bool found = false;

    // Interpolated, so this is not bin_width_hz times an integer.
    double frequency_hz = 0.0;

    // The bin the search landed on, before interpolation.
    std::size_t bin = 0;

    double power = 0.0;
    double baseline = 0.0;

    // 10*log10(power/baseline). The one number every threshold in this
    // directory is stated in.
    double margin_db = 0.0;
};

// Strongest bin, by margin over its local baseline, in [low_hz, high_hz].
//
// Margin rather than raw power, deliberately. A search by raw power over the
// squared-envelope spectrum of a shaped PSK signal returns the top of the
// pedestal near DC every time, which is a real feature of the signal and is
// not its symbol rate.
[[nodiscard]] SpectralPeak strongest_peak(const PowerSpectrum& spectrum,
                                          std::span<const double> baseline,
                                          double low_hz,
                                          double high_hz);

// Best bin within tolerance_bins of a target frequency. Used to ask whether a
// harmonic or a subharmonic of an already-found peak is also present, which
// is how the fundamental is separated from the comb above it.
[[nodiscard]] SpectralPeak peak_near(const PowerSpectrum& spectrum,
                                     std::span<const double> baseline,
                                     double target_hz,
                                     std::size_t tolerance_bins);

// Fraction of the spectrum's total power in its strongest three adjacent
// bins.
//
// This is the unmodulated-carrier test and it is a spectral one on purpose.
// The obvious test, a small spread of instantaneous frequency, is not
// scale-free: at 20 dB in the full band the phase noise on a clean carrier
// alone puts several hundred hertz of spread on the instantaneous frequency
// at 48 kS/s, so any absolute threshold on it is really a threshold on
// signal-to-noise ratio wearing a disguise. A power fraction is the same
// number at any sample rate and degrades the way a reader expects: it is
// S/(S+N) for a carrier, and roughly one bin's share of the occupied
// bandwidth for anything modulated.
//
// Three bins rather than one because a Hann-windowed tone that does not sit
// on a bin centre spreads over three, and the worst case between centres
// otherwise costs about 4 dB of apparent concentration.
[[nodiscard]] double spectral_concentration(const PowerSpectrum& spectrum);

struct OccupiedBand {
    bool found = false;
    double low_hz = 0.0;
    double high_hz = 0.0;
    double centre_hz = 0.0;
    double bandwidth_hz = 0.0;

    // The level the containment was measured above, as a linear power per
    // bin. Reported because a bandwidth is meaningless without it.
    double noise_floor = 0.0;
};

// Smallest contiguous run of bins holding the given fraction of the power
// that stands above the noise floor.
//
// The floor is the 20th percentile across the spectrum, which is a percentile
// for the reason local_baseline is: a mean is raised by the signal. The run
// does not wrap across the Nyquist edge, and a signal that straddles that
// edge therefore reads as two pieces and is reported as the wider one. That
// is a real limitation and it is acceptable here because the characteriser's
// input is a receiver extract that has already been mixed onto the signal;
// core/detect/detector.h is what works on a span where a signal can sit
// anywhere.
[[nodiscard]] OccupiedBand occupied_band(const PowerSpectrum& spectrum, double fraction);

// Converts an interleaved Complex32 sample buffer to the analysis type.
[[nodiscard]] std::vector<Complex64> to_analysis(ConstComplexSpan samples);

}  // namespace revenant::characterise
