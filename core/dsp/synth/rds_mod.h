// The RDS transmitter: an arbitrary bit sequence rendered onto a 57 kHz
// subcarrier on an FM composite that carries the 19 kHz pilot.
//
// SPECIFICATION
//
// EN 50067:1998 clauses 1.1 through 1.7, the same text the demodulator in
// core/decode/rds_bits.h implements. NRSC-4 (April 1998) clauses 1.1 through
// 1.7 are word for word identical, so this transmitter is both RDS and RBDS:
// the two standards do not diverge until the group layer.
//
// WHY A TRANSMITTER EXISTS AT ALL
//
// The same reason core/dsp/synth/modulators.h gives for every other mode, and
// it applies harder here. Writing the modulator beside the demodulator is how
// the specification gets checked: if the pair does not round-trip, one of the
// two misread the document. That is the only check this decoder gets, because
// it is host code with no GPU kernel behind it and therefore no bit-exact
// scalar twin standing over it the way every kernel in this engine has one.
//
// It is also a signal generator mode. A composite with a known bit sequence
// on it, at a known injection level, in a known amount of noise, is what a
// receiver chain gets scored against.
//
// CLEAN ROOM
//
// No RDS implementation was read. Every constant names its clause.
//
// WHAT "1.0" MEANS IN THE OUTPUT
//
// The composite is normalised so that 1.0 is 75 kHz of FM carrier deviation,
// which EN 50067:1998 clause 1.3 gives as the cap on the whole multiplex.
// Every injection level below is therefore stated as a deviation in hertz and
// divided by 75000 on the way in, so the numbers in a spec read the same as
// the numbers in the standard.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/decode/rds_bits.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/types.h"
#include "core/error.h"

// The include of core/decode/rds_bits.h above is deliberate and is the only
// place in core/dsp that reaches into core/decode.
//
// EN 50067:1998 clause 1.7 splits the data-spectrum shaping equally between
// transmitter and receiver. That is one filter specified once and used at both
// ends, so it is defined once, in the decoder's header beside the clause
// citation, and used here. Two copies of a filter that the standard says are
// the same halves of one response is how a round trip starts passing because
// both ends made the same mistake.

namespace revenant::siggen {

using dsp::Hertz;
using dsp::SampleIndex;
using dsp::SampleRate;

// EN 50067:1998 clause 1.3: the composite multiplex deviation is capped at
// +/- 75 kHz, which is what the output of this file is scaled against.
inline constexpr Hertz kCompositePeakDeviationHz = 75000;

// ---------------------------------------------------------------------------
// Spec
// ---------------------------------------------------------------------------

struct RdsModSpec {
    // Output sample rate of the composite. At or above
    // decode::kMinimumRateHz, because the composite reaches 59375 Hz.
    SampleRate rate = 171000;

    // EN 50067:1998 clause 1.1. The subcarrier is the third harmonic of the
    // pilot during stereo. With the pilot switched off the subcarrier still
    // runs at 57 kHz, which is the mono case clause 1.1 also permits and the
    // case a decoder that needs a pilot fails.
    bool pilot_enabled = true;

    // Peak deviation contributed by the pilot. Not an RDS number: EN 50067
    // says nothing about the pilot level, which belongs to the FM stereo
    // system. 6750 Hz is 9 percent of 75 kHz, the middle of the 8 to 10
    // percent that broadcast practice uses.
    Hertz pilot_deviation_hz = 6750;

    // EN 50067:1998 clause 1.3: deviation due to the subcarrier ranges from
    // +/- 1.0 kHz to +/- 7.5 kHz, with +/- 2.0 kHz named as the recommended
    // best compromise.
    //
    // WHAT THE CLAUSE SAYS AND WHAT THIS SCALES
    //
    // Clause 1.3 states the deviation "due to the unmodulated subcarrier".
    // Clause 1.4 suppresses the subcarrier, so there is no unmodulated state
    // to measure and the phrase names a reference level rather than anything
    // transmitted. This field is the PEAK deviation of the modulated RDS
    // component as rendered, which is the reading an FM deviation meter gives
    // and the stricter of the two. RdsComposite reports the achieved RMS
    // beside the peak so a reader who wants the equal-power reading of the
    // same phrase can convert rather than guess which one was meant.
    Hertz rds_deviation_hz = 2000;

    // EN 50067:1998 clause 1.2: the subcarrier phase is locked either IN
    // PHASE or IN QUADRATURE with the third harmonic of the pilot, to within
    // +/- 10 degrees, and both are permitted. Radians, relative to the third
    // harmonic. Zero is in phase, pi/2 is in quadrature, pi is in phase and
    // inverted. A decoder is not allowed to care which, and this field is how
    // that gets tested.
    double subcarrier_phase_radians = 0.0;

    // An optional mono programme tone, so the composite is not pilot and data
    // alone. Deviation zero disables it. Nothing in RDS depends on this; it
    // is here because a decoder that only ever sees a bare subcarrier has not
    // been asked to reject anything.
    Hertz mono_tone_hz = 0;
    Hertz mono_deviation_hz = 0;

    // EN 50067:1998 clause 1.6: the transmitted bit is the previous
    // transmitted bit XOR the new input bit, so an input 0 leaves the output
    // unchanged and an input 1 complements it.
    //
    // Off renders the input bits straight into biphase symbols. That is not a
    // conformant transmitter, and it exists only so a test can drive the
    // biphase layer on its own without clause 1.6 in the way.
    bool differential_encode = true;
    bool initial_differential_state = false;

    // Transmitter clock error, in parts per million. Scales the pilot, the
    // subcarrier and the bit clock together, which is what a real clock error
    // does and what keeps clause 1.5's coherence (subcarrier = 48 times the
    // bit rate, pilot = subcarrier / 3) true under the error.
    double clock_error_ppm = 0.0;

    // An extra subcarrier offset that is NOT coherent with the pilot or the
    // bit clock. Not physical, and not something a conformant transmitter
    // does. It is here so a test can move the carrier recovery loop without
    // moving the timing loop, which a coherent clock error cannot do.
    Hertz subcarrier_offset_hz = 0;

    // Fractional sample offset of the whole composite. A real receiver's
    // sample grid has no reason to line up with the transmitter's bit grid,
    // and a decoder tested only at zero offset has not been asked to
    // interpolate.
    double start_offset_samples = 0.0;

    // One bit per element, value 0 or 1, in transmission order. Deliberately
    // not packed, for the reason modulators.h gives: a packed payload invites
    // an MSB-versus-LSB disagreement between the two ends and a bit-vector
    // comparison has nowhere to hide it.
    std::vector<std::uint8_t> bits{};
};

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

struct RdsComposite {
    // The composite, real, at spec.rate. 1.0 is 75 kHz of deviation.
    std::vector<float> samples{};

    // The bits as handed in, and the bits as actually put on the subcarrier
    // after clause 1.6. transmitted.size() == data.size().
    std::vector<std::uint8_t> data_bits{};
    std::vector<std::uint8_t> transmitted_bits{};

    // The RDS component alone, measured over the rendered buffer rather than
    // assumed. rms is the equal-power reading of the clause 1.3 phrase; the
    // two peaks are the two readings of its peak sense, and they differ for a
    // reason worth knowing.
    //
    // envelope_peak is the peak of the shaped biphase signal before it
    // modulates the subcarrier, which is what rds_deviation_hz was scaled to
    // and what a deviation meter watching the continuous waveform reports.
    //
    // rds_peak is the largest sample actually rendered, which is lower
    // whenever the sample grid misses the subcarrier's own crest. At
    // 171000 Hz there are exactly three samples per 57 kHz cycle, so the
    // subcarrier is only ever sampled at sin of 0 and +/- 120 degrees and the
    // rendered peak reads 0.866 of the envelope. Nothing is wrong when those
    // two disagree: one is a property of the signal and the other of the
    // grid it was written onto.
    double rds_peak = 0.0;
    double rds_envelope_peak = 0.0;
    double rds_rms = 0.0;
    double rds_mean_power = 0.0;

    double composite_mean_power = 0.0;
    double composite_peak = 0.0;

    // Nominal is decode::kBitRateHz; clock_error_ppm moves it.
    double bit_rate_hz = 0.0;

    // Sample index at which bit i's decision instant falls, as a fractional
    // position in the output buffer. A test aligning recovered bits against
    // transmitted ones wants this rather than a guess.
    [[nodiscard]] double bit_instant_samples(std::size_t bit) const;

    double first_bit_instant_samples = 0.0;
    double samples_per_bit = 0.0;
};

// Samples the whole bit sequence plus the shaping filter's run-in and run-out.
// Rendering fewer than this truncates the last bits; rendering more appends
// silence on the data channel while the pilot and any mono tone continue.
[[nodiscard]] Expected<std::size_t> rds_nominal_sample_count(const RdsModSpec& spec);

[[nodiscard]] Status validate(const RdsModSpec& spec);

// ---------------------------------------------------------------------------
// The modulator
// ---------------------------------------------------------------------------

// Pure, and blocking independent: render(start, out) writes absolute samples
// [start, start + out.size()) and the same absolute range always produces the
// same samples whatever partition the caller used. Nothing reads a clock and
// nothing is cached between calls.
//
// The exception to modulators.h's third property, exact phase from integer
// arithmetic, is deliberate and bounded. clock_error_ppm and
// start_offset_samples both make the realised frequencies non-integer, so the
// phase is carried in double and reduced with fmod. Over ten seconds at 57 kHz
// that is 5.7e5 turns, and double's relative precision puts the accumulated
// phase error near 1e-10 turns. The integer path in modulators.h exists
// because a wideband scene runs for hours; a test instrument does not.
class RdsModulator {
public:
    [[nodiscard]] static Expected<RdsModulator> create(RdsModSpec spec);

    void render(SampleIndex start, dsp::RealSpan out) const;

    // The RDS component alone, without the pilot or the mono tone, at the
    // same scale it has in the composite. This is what the SNR calibration is
    // measured against and what a test wants when it needs to know what the
    // decoder was actually given.
    void render_rds_only(SampleIndex start, dsp::RealSpan out) const;

    [[nodiscard]] const RdsModSpec& spec() const { return spec_; }
    [[nodiscard]] const std::vector<std::uint8_t>& transmitted_bits() const { return encoded_; }
    [[nodiscard]] std::size_t nominal_sample_count() const { return nominal_samples_; }
    [[nodiscard]] double bit_rate_hz() const { return bit_rate_; }
    [[nodiscard]] double samples_per_bit() const { return samples_per_bit_; }

    // Peak of the shaped biphase envelope, measured at construction over the
    // whole bit sequence. This is the quantity rds_deviation_hz scales, so it
    // equals that deviation over 75000 by construction.
    [[nodiscard]] double envelope_peak() const { return envelope_peak_; }

    // Decision instant of bit i as a fractional position in the output.
    [[nodiscard]] double bit_instant_samples(std::size_t bit) const;

    // ---------------------------------------------------------------------
    // What an FM modulator upstream of this needs
    // ---------------------------------------------------------------------
    //
    // core/dsp/synth/wfm_mod.h puts this composite on an FM carrier. Doing
    // that needs two things from here that render() alone cannot supply, and
    // both are exposed rather than reconstructed on the other side, because
    // reconstructing either is copying arithmetic that the standard says is
    // shared.

    // Turns of the 19 kHz pilot at an absolute sample index, unreduced, on
    // the same time base render() uses: start_offset_samples included,
    // clock_error_ppm included.
    //
    // The FM stereo subcarrier is the SECOND harmonic of this pilot and the
    // RDS subcarrier is the third, so a composite that generates its own
    // 38 kHz from a nominal 38000 Hz stops being coherent with the data the
    // moment clock_error_ppm is non-zero. That failure is silent: the stereo
    // decoder still works, the RDS decoder still works, and the one property
    // clause 1.5 is about is gone.
    [[nodiscard]] double pilot_turns_at(SampleIndex index) const;
    [[nodiscard]] double pilot_turns_per_sample() const { return pilot_turns_per_sample_; }

    // The time integral of render(), in SAMPLES, from the buffer's own origin
    // to the given index. Exactly zero at index zero.
    //
    // FM is the integral of the modulating signal, so this is what an FM
    // modulator needs in order to stay a pure function of the absolute sample
    // index. Accumulating the integral at render time instead would make the
    // output depend on where the caller's blocks fell, and tabulating it
    // per sample would cost eight bytes a sample for a signal that has to run
    // for hours in a wideband scene.
    //
    // It is not an approximation of the integral of the samples render()
    // writes. It is the exact integral of the continuous signal those samples
    // are point evaluations of, which is a different and stricter claim: the
    // pilot and the mono tone integrate in closed form, and the RDS term
    // integrates in closed form too, for the reason under
    // subcarrier_offset_hz below.
    //
    // WHAT THIS COSTS WHEN subcarrier_offset_hz IS NON-ZERO: nothing is
    // returned, because nothing correct can be. See the note on
    // supports_integral().
    [[nodiscard]] double composite_integral(SampleIndex index) const;

    // False when subcarrier_offset_hz is non-zero, and composite_integral()
    // must not be called in that case.
    //
    // The RDS term integrates in closed form only because EN 50067:1998
    // clause 1.5 makes the subcarrier exactly 48 times the bit rate. Bit i's
    // shaped impulse pair therefore sits under a subcarrier whose phase at
    // the bit's own centre is the same for every i, so one tabulated running
    // integral serves every bit and the sum is over the same bounded
    // window data_signal() already walks. subcarrier_offset_hz is the one
    // field that deliberately breaks that coherence, and with it broken each
    // bit would need its own integral. clock_error_ppm does NOT break it: it
    // scales the subcarrier and the bit clock together, which is the whole
    // point of that field.
    [[nodiscard]] bool supports_integral() const { return integrable_; }

private:
    RdsModulator() = default;

    // The shaped biphase data signal at an absolute sample index, before the
    // injection scaling. Unit amplitude symbols.
    [[nodiscard]] double data_signal(SampleIndex index) const;

    [[nodiscard]] double shaping_at(double bit_periods) const;

    // The time integral of render_rds_only(), in samples, from minus infinity
    // to the given index. Bounded work, and identically zero before the first
    // bit's shaping tail and after the last one's.
    [[nodiscard]] double rds_integral(SampleIndex index) const;

    RdsModSpec spec_{};
    std::vector<std::uint8_t> encoded_{};

    double bit_rate_ = 0.0;
    double bit_period_seconds_ = 0.0;
    double samples_per_bit_ = 0.0;
    double clock_scale_ = 1.0;
    std::size_t nominal_samples_ = 0;

    // Turns per sample, already carrying the clock scale.
    double pilot_turns_per_sample_ = 0.0;
    double subcarrier_extra_turns_per_sample_ = 0.0;
    double mono_turns_per_sample_ = 0.0;

    double pilot_amplitude_ = 0.0;
    double mono_amplitude_ = 0.0;
    double rds_scale_ = 0.0;
    double envelope_peak_ = 0.0;

    // The clause 1.7 impulse response, tabulated in bit periods and linearly
    // interpolated. Evaluating the closed form per tap per sample is two
    // transcendental calls and a divide inside the inner loop, which is the
    // same trap modulators.h records against the root raised cosine.
    std::vector<double> shaping_table_{};
    double shaping_steps_per_bit_ = 0.0;
    double shaping_span_bits_ = 0.0;

    // What composite_integral() is built on, on a grid running from
    // -shaping_span_bits_ to +shaping_span_bits_ + 0.5 at
    // shaping_steps_per_bit_ steps per bit period. Both empty when the spec
    // is not integrable.
    //
    // subcarrier_pair_ is the shaped impulse pair itself at the grid points,
    // which is piecewise linear between them because shaping_at() is.
    // subcarrier_integral_ is its running integral against the modulated
    // subcarrier, which is exact at every grid point.
    //
    // THE TABLE IS NOT INTERPOLATED BETWEEN GRID POINTS AND MUST NOT BE.
    // The subcarrier turns 48 times per bit period and the grid is 512 steps
    // per bit period, so it is sampled ten times a cycle: a straight line
    // between two grid points of the integral has a derivative that is the
    // average of the oscillation over the step rather than its value, and
    // the error is 30 percent of the RDS term. Measured, not estimated. What
    // the lookup does instead is add the remaining part-interval in closed
    // form, from the pair's two endpoints, which is exact for the piecewise
    // linear pair and costs no table at all.
    std::vector<double> subcarrier_pair_{};
    std::vector<double> subcarrier_integral_{};

    // Grid steps per bit period as an integer, so that one bit period is an
    // exact stride through both tables.
    std::size_t subcarrier_steps_per_bit_ = 0;

    // The subcarrier phase at a bit's own centre, which EN 50067 clause 1.5's
    // coherence makes the same for every bit. Folded into the table above,
    // which is why there is one table and not a sine one and a cosine one.
    double subcarrier_anchor_phase_ = 0.0;

    double integral_base_ = 0.0;  // composite_integral() at index zero
    bool integrable_ = false;
};

// ---------------------------------------------------------------------------
// One-shot generation
// ---------------------------------------------------------------------------

// sample_count of zero means rds_nominal_sample_count(spec).
[[nodiscard]] Expected<RdsComposite> generate_rds(const RdsModSpec& spec,
                                                  std::size_t sample_count = 0);

// ---------------------------------------------------------------------------
// Noise
// ---------------------------------------------------------------------------

// AWGN on a REAL composite, calibrated per docs/snr-convention.md.
//
// THE ONE THING THAT CHANGES FOR A REAL SIGNAL, AND WHY IT IS NOT A SECOND
// CONVENTION
//
// docs/snr-convention.md and channel.h state the conversions for a complex
// baseband stream, where a stream at `rate` represents `rate` hertz of
// spectrum. A real stream at the same rate represents half that, 0 to rate/2,
// so the same formulas apply with rate/2 substituted for rate. Nothing else
// moves: SNR_B is still signal power over noise power in the reference
// bandwidth, the default reference bandwidth is still 2500 Hz, and a number
// reported from here is still directly comparable with one reported from
// channel.h.
//
// The other half of the difference is the one docs/snr-convention.md warns
// about costing 3 dB. A complex sample carries its noise across two
// independent quadratures, so the per-component variance is half the total
// power. A real sample has one component, so its variance IS the total power.
//
// signal_power is passed in rather than measured, because the wanted signal
// here is the RDS component alone. Calibrating against the whole composite
// would make the SNR move with the pilot and the programme audio, which says
// nothing about whether the data survives. RdsComposite::rds_mean_power is
// the number to hand in.
[[nodiscard]] Expected<double> real_awgn_power_for(const NoiseLevel& level,
                                                   double signal_power,
                                                   SampleRate rate);

struct RdsNoiseReport {
    double signal_power = 0.0;
    double noise_power = 0.0;
    double noise_power_in_reference_bandwidth = 0.0;
    Hertz reference_bandwidth_hz = kWsjtxReferenceBandwidthHz;
    double snr_in_reference_bandwidth_db = 0.0;
    double snr_in_full_band_db = 0.0;

    // The same channel stated the way the BER literature states it, using the
    // RDS information bit rate. docs/snr-convention.md notes that this figure
    // and the reference-bandwidth one are related with no sample rate in
    // either, which is what makes both safe to report.
    double eb_over_n0_db = 0.0;
};

[[nodiscard]] Expected<RdsNoiseReport> add_real_awgn(dsp::RealSpan composite,
                                                     double signal_power,
                                                     const NoiseLevel& level,
                                                     SampleRate rate,
                                                     std::uint64_t seed);

// Measures what add_real_awgn actually put in the buffer, by differencing the
// impaired composite against the clean one that went in. The real counterpart
// of measure_snr() in channel.h, named differently for the same reason
// real_mean_power is.
//
// WHY THIS EXISTS AND WHAT IT IS FOR
//
// Asking add_real_awgn for a level and then reading that level back out of
// its own report proves nothing: the report computes the SNR from the same
// noise power the request produced, so the round trip closes whatever sigma
// the generator actually used. Changing sigma from sqrt(power) to
// sqrt(power/2) leaves every reported figure untouched and makes every real
// Eb/N0 3 dB better than its label, which is the exact mistake
// docs/snr-convention.md warns a real signal costs. Only a measurement of the
// buffer can see it, and that is this.
//
// signal_power is handed in rather than measured off the clean buffer, for
// the reason add_real_awgn takes it: the wanted signal on an FM composite is
// the RDS component alone, and the clean buffer also carries the pilot and
// the programme audio. RdsComposite::rds_mean_power is the number to pass.
// SnrMeasurement::signal_power echoes it back unchanged; the measurement is
// in noise_power and everything derived from it.
[[nodiscard]] Expected<SnrMeasurement> measure_real_snr(dsp::ConstRealSpan clean,
                                                        dsp::ConstRealSpan impaired,
                                                        double signal_power,
                                                        SampleRate rate,
                                                        double bits_per_second = 0.0);

// Mean of x^2 over the buffer. Zero for an empty span. The complex
// counterpart is mean_power() in channel.h, and this is deliberately named
// differently rather than overloaded, because the two differ by the factor of
// two above and an overload set is where that difference goes unnoticed.
[[nodiscard]] double real_mean_power(dsp::ConstRealSpan samples);
[[nodiscard]] double real_peak(dsp::ConstRealSpan samples);

}  // namespace revenant::siggen
