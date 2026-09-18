// The channel simulator: calibrated impairments applied to a generated signal.
//
// The generator makes the signal, this makes the conditions, and the BER/SNR
// harness sweeps one against the other. Everything here is a pure function of
// its arguments plus an explicit seed, so a sweep runs faster than realtime, in
// batch, and reproduces exactly from the seed printed in its own log.
//
// The one thing in this file that has to be exactly right is the SNR
// calibration, because every sensitivity number Revenant ever publishes is
// derived from it.
//
// WSJT-X, and through it the whole weak-signal field, reports SNR as signal
// power over the noise power in a 2500 Hz reference bandwidth. That number is
// not the signal-to-noise ratio in the full sample-rate bandwidth and it is not
// the ratio in the signal's own occupied bandwidth. At 48 kHz the full-band
// noise sits 10*log10(48000/2500) = 12.83 dB above the reference-bandwidth
// figure, so treating the two as interchangeable is a 12.83 dB error, which is
// most of the distance between an ordinary decoder and a world-class one.
//
// Nothing in this API takes a parameter called "snr_db". Every level names the
// bandwidth or the bit rate it is measured against. See docs/snr-convention.md.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::Complex32;
using dsp::ComplexSpan;
using dsp::ConstComplexSpan;
using dsp::Hertz;
using dsp::SampleRate;

// The reference bandwidth WSJT-X reports against. It is the width of a typical
// SSB receiver audio passband, which is what the field standardised on when
// WSJT-X chose it, and it is why a reported -24 dB from two different programs
// means the same thing.
inline constexpr Hertz kWsjtxReferenceBandwidthHz = 2500;

// ---------------------------------------------------------------------------
// SNR conventions and conversions between them
// ---------------------------------------------------------------------------
//
// Let S be the mean power of the wanted signal, N0 the one-sided noise power
// spectral density in watts per hertz, fs the sample rate, B the reference
// bandwidth and Rb the information bit rate. For a complex baseband stream the
// represented spectrum is fs hertz wide, so the total noise power in the buffer
// is N0*fs and the noise in a B-hertz slice of it is N0*B.
//
//   SNR_full = S / (N0 * fs)
//   SNR_B    = S / (N0 * B)
//   Eb/N0    = (S / Rb) / N0 = SNR_full * fs / Rb
//
// which gives, in decibels:
//
//   SNR_B_dB    = SNR_full_dB + 10*log10(fs / B)
//   SNR_full_dB = SNR_B_dB    - 10*log10(fs / B)
//   EbN0_dB     = SNR_full_dB + 10*log10(fs / Rb)
//   EbN0_dB     = SNR_B_dB    + 10*log10(B  / Rb)
//
// Note that the reference-bandwidth figure and Eb/N0 are related without any
// reference to the sample rate at all. That is the point of both: they describe
// the channel, not the buffer the channel was written into.

enum class SnrBasis {
    // Signal power over noise power in reference_bandwidth_hz. The default, and
    // the convention every Revenant sensitivity claim is reported in.
    ReferenceBandwidth,

    // Signal power over noise power across the whole sample-rate bandwidth.
    // Convenient for a test that wants to reason about the buffer directly.
    FullSampleRate,

    // Energy per information bit over noise power spectral density. The BER
    // literature uses this, so a reader comparing Revenant against a published
    // curve wants it.
    EbN0,
};

// A noise level, always carrying the basis it was stated in. Construct one
// through a named factory so the call site reads as the convention it meant.
struct NoiseLevel {
    SnrBasis basis = SnrBasis::ReferenceBandwidth;
    double value_db = 0.0;

    // Used when basis is ReferenceBandwidth.
    Hertz reference_bandwidth_hz = kWsjtxReferenceBandwidthHz;

    // Used when basis is EbN0. Information bits per second, not coded bits: the
    // BER literature plots against information rate and mixing the two shifts
    // every curve by the code rate.
    double bits_per_second = 0.0;

    [[nodiscard]] static NoiseLevel snr_in_2500_hz_db(double db);
    [[nodiscard]] static NoiseLevel snr_in_reference_bandwidth_db(double db, Hertz bandwidth_hz);
    [[nodiscard]] static NoiseLevel snr_in_full_sample_rate_db(double db);
    [[nodiscard]] static NoiseLevel eb_over_n0_db(double db, double bits_per_second);

    // Resolves whatever basis this level carries into the full-band figure the
    // noise generator actually needs.
    [[nodiscard]] Expected<double> to_full_sample_rate_db(SampleRate rate) const;
};

[[nodiscard]] Expected<double> reference_bandwidth_to_full_band_db(
    double snr_in_reference_bandwidth_db,
    SampleRate rate,
    Hertz reference_bandwidth_hz = kWsjtxReferenceBandwidthHz);

[[nodiscard]] Expected<double> full_band_to_reference_bandwidth_db(
    double snr_in_full_band_db,
    SampleRate rate,
    Hertz reference_bandwidth_hz = kWsjtxReferenceBandwidthHz);

[[nodiscard]] Expected<double> full_band_to_eb_over_n0_db(double snr_in_full_band_db,
                                                          SampleRate rate,
                                                          double bits_per_second);

[[nodiscard]] Expected<double> eb_over_n0_to_full_band_db(double eb_over_n0_db,
                                                          SampleRate rate,
                                                          double bits_per_second);

[[nodiscard]] Expected<double> reference_bandwidth_to_eb_over_n0_db(
    double snr_in_reference_bandwidth_db,
    double bits_per_second,
    Hertz reference_bandwidth_hz = kWsjtxReferenceBandwidthHz);

[[nodiscard]] Expected<double> eb_over_n0_to_reference_bandwidth_db(
    double eb_over_n0_db,
    double bits_per_second,
    Hertz reference_bandwidth_hz = kWsjtxReferenceBandwidthHz);

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

// Mean of |x|^2 over the buffer. Zero for an empty span. Nothing here assumes
// unit amplitude anywhere: every calibration measures the buffer it was given.
[[nodiscard]] double mean_power(ConstComplexSpan samples);

struct SnrMeasurement {
    double signal_power = 0.0;
    double noise_power = 0.0;
    double snr_in_full_band_db = 0.0;
    double snr_in_2500_hz_db = 0.0;

    // Only meaningful when measure_snr was given a positive bit rate. Left at
    // zero otherwise, and measured_eb_over_n0 says which.
    double eb_over_n0_db = 0.0;
    bool measured_eb_over_n0 = false;
};

// Measures what the channel actually produced by differencing the impaired
// buffer against the clean one that went in, so a test can assert the simulator
// delivered the SNR it was asked for. The two spans must be the same length,
// which means measuring after timing drift requires the clean signal resampled
// the same way. Use the clean-path output of apply_channel for that.
[[nodiscard]] Expected<SnrMeasurement> measure_snr(ConstComplexSpan clean,
                                                   ConstComplexSpan impaired,
                                                   SampleRate rate,
                                                   double bits_per_second = 0.0);

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

// Every stage draws from its own stream, derived from the caller's master seed
// and a fixed per-stage domain tag. Without this, adding a multipath tap would
// shift the noise stream and a previously recorded failure would stop
// reproducing. The mixer is the SplitMix64 finaliser, chosen because it is
// short, has no state, and is specified bit for bit.
[[nodiscard]] std::uint64_t derive_seed(std::uint64_t master_seed, std::uint64_t domain);

// ---------------------------------------------------------------------------
// Additive white Gaussian noise
// ---------------------------------------------------------------------------

struct NoiseReport {
    double signal_power = 0.0;
    double noise_power_full_band = 0.0;
    double noise_power_in_reference_bandwidth = 0.0;
    Hertz reference_bandwidth_hz = kWsjtxReferenceBandwidthHz;
    double snr_in_reference_bandwidth_db = 0.0;
    double snr_in_full_band_db = 0.0;
};

// The calibration on its own, with no randomness in it, so a test can check the
// arithmetic without generating a single sample. Returns the total noise power
// to place across the sample-rate bandwidth.
[[nodiscard]] Expected<double> awgn_power_for(const NoiseLevel& level,
                                              double signal_power,
                                              SampleRate rate);

// Measures the signal power of the buffer, then adds noise to it in place. The
// buffer must hold the wanted signal alone: anything already added to it counts
// as signal and silently raises the reference power.
[[nodiscard]] Expected<NoiseReport> add_awgn(ComplexSpan signal,
                                             const NoiseLevel& level,
                                             SampleRate rate,
                                             std::uint64_t seed);

// Adds noise of an already-decided total power, for the chain case where the
// reference power was measured at a different point than where the noise lands.
[[nodiscard]] Status add_awgn_at_power(ComplexSpan signal,
                                       double noise_power_full_band,
                                       std::uint64_t seed);

// ---------------------------------------------------------------------------
// Multipath
// ---------------------------------------------------------------------------

struct MultipathTap {
    // Delay of this path relative to the start of the buffer. Fractional sample
    // delays are honoured through band-limited interpolation, because a real
    // delay spread is not an integer number of sample periods and rounding it
    // to one moves every null in the frequency response.
    double delay_seconds = 0.0;

    double gain_db = 0.0;
    double phase_radians = 0.0;
};

struct MultipathProfile {
    // Empty means one unit-gain path at zero delay, which is the no-multipath
    // case and costs nothing.
    std::vector<MultipathTap> taps;

    // Scales the profile so the sum of tap powers is one. On by default so that
    // adding a reflection changes the channel's shape without also changing the
    // received signal level, which would move the SNR at the same time.
    bool normalize_power = true;
};

// Output is the same length as the input. Energy delayed past the end of the
// buffer is dropped, as it would be by a receiver that stopped capturing.
[[nodiscard]] Expected<std::vector<Complex32>> apply_multipath(ConstComplexSpan input,
                                                               const MultipathProfile& profile,
                                                               SampleRate rate);

// ---------------------------------------------------------------------------
// Fading
// ---------------------------------------------------------------------------

enum class FadingModel {
    None,

    // Zero-mean complex Gaussian gain: no line of sight component.
    Rayleigh,

    // A specular component plus a diffuse one, in the power ratio K.
    Rician,
};

struct FadingConfig {
    FadingModel model = FadingModel::None;

    // Ratio of specular power to diffuse power, in dB. Rician only. Large K is
    // close to no fading at all; K at minus infinity is Rayleigh.
    double rician_k_db = 6.0;

    // Maximum Doppler frequency of the diffuse component, which is the fading
    // rate. Zero gives a fixed random gain rather than a time-varying one,
    // which is the right degenerate case for a static channel.
    double doppler_spread_hz = 0.0;

    // Doppler on the specular component. Rician only.
    Hertz line_of_sight_doppler_hz = 0;
};

// Multiplies the buffer by a unit-mean-power fading process in place, so fading
// alone does not change the average received level and therefore does not move
// the SNR out from under the calibration.
[[nodiscard]] Status apply_fading(ComplexSpan signal,
                                  const FadingConfig& config,
                                  SampleRate rate,
                                  std::uint64_t seed);

// Multipath and fading together, which is the physically meaningful
// combination: every tap fades independently, so the channel is frequency
// selective rather than flat. With Rician the first tap carries the specular
// component and the rest are Rayleigh, which is the usual convention because
// the line of sight arrives first.
[[nodiscard]] Expected<std::vector<Complex32>> apply_propagation(ConstComplexSpan input,
                                                                 const MultipathProfile& multipath,
                                                                 const FadingConfig& fading,
                                                                 SampleRate rate,
                                                                 std::uint64_t seed);

// ---------------------------------------------------------------------------
// Carrier frequency: Doppler shift and drift
// ---------------------------------------------------------------------------

struct FrequencyConfig {
    // Constant offset. Integer hertz per the project convention.
    Hertz doppler_shift_hz = 0;

    // Deterministic linear wander of the carrier, in hertz per second. A drifty
    // free-running oscillator warming up looks like this.
    double drift_hz_per_second = 0.0;

    // Random wander on top of the linear term. Stated per root second because
    // the accumulated deviation of a random walk grows as the square root of
    // elapsed time, so this is the only unit that stays constant across buffer
    // lengths.
    double random_walk_hz_per_root_second = 0.0;
};

[[nodiscard]] Status apply_frequency_offset(ComplexSpan signal,
                                            const FrequencyConfig& config,
                                            SampleRate rate,
                                            std::uint64_t seed);

// ---------------------------------------------------------------------------
// Sample clock error
// ---------------------------------------------------------------------------

struct TimingConfig {
    // Receiver sample clock error in parts per million. Positive means the
    // receiver clock runs fast, so it reads the transmitter's waveform at
    // positions slightly further apart than one sample.
    double clock_error_ppm = 0.0;

    // Fixed fractional offset applied before the drift, in samples.
    double initial_offset_samples = 0.0;
};

// Resamples at the drifted rate. This is fractional resampling and not sample
// dropping on purpose. A real clock error is a continuous stretch of the time
// base, so the correct output is the waveform evaluated at non-integer
// positions. Dropping or repeating a whole sample instead inserts a one-sample
// discontinuity: it is a step in the signal's phase, it spreads energy across
// the whole band as a broadband click, and a timing recovery loop that would
// have tracked a slow continuous ramp without noticing is knocked out of lock
// by the jump. The measured impairment would then be the shortcut rather than
// the clock error under test.
//
// The output length follows the drift, so it is not the input length: a fast
// receiver clock produces fewer samples over the same interval and a slow one
// produces more, bounded by the input actually available.
[[nodiscard]] Expected<std::vector<Complex32>> apply_timing_drift(ConstComplexSpan input,
                                                                  const TimingConfig& config);

// ---------------------------------------------------------------------------
// Impulsive noise
// ---------------------------------------------------------------------------

enum class ImpulseAmplitude {
    // Every impulse the same magnitude, random phase.
    Constant,

    // Complex Gaussian, so the magnitude is Rayleigh distributed.
    Rayleigh,

    // Log-normal magnitude, random phase. Power line and ignition noise are
    // usually modelled this way because the amplitude spans decades.
    LogNormal,
};

struct ImpulsiveNoiseConfig {
    // Poisson arrival rate. Zero disables impulsive noise entirely.
    double events_per_second = 0.0;

    // Impulse RMS relative to the RMS of the wanted signal, in dB. Stated
    // relative so the setting is independent of the generator's output level.
    double amplitude_db_relative_to_signal_rms = 0.0;

    ImpulseAmplitude distribution = ImpulseAmplitude::Rayleigh;

    // Spread of the log-normal magnitude, in dB. LogNormal only.
    double log_normal_sigma_db = 6.0;

    // Width of one impulse. Zero means a single sample.
    double duration_seconds = 0.0;
};

struct ImpulseReport {
    std::size_t event_count = 0;
    std::size_t affected_samples = 0;

    // Mean power the impulses contributed across the whole buffer.
    double added_power = 0.0;
};

// reference_amplitude_rms is the RMS of the wanted signal, passed in rather
// than measured here so the caller controls which point in the chain the
// impulses are scaled against.
[[nodiscard]] Expected<ImpulseReport> add_impulsive_noise(ComplexSpan signal,
                                                          const ImpulsiveNoiseConfig& config,
                                                          SampleRate rate,
                                                          double reference_amplitude_rms,
                                                          std::uint64_t seed);

// ---------------------------------------------------------------------------
// Adjacent channel interference
// ---------------------------------------------------------------------------

enum class InterfererKind {
    // An unmodulated carrier at the offset. The worst case for a decoder's
    // front end and the easiest to reason about.
    Tone,

    // Band-limited complex Gaussian noise, which stands in for a neighbouring
    // modulated transmission without needing a second modulator.
    BandLimitedNoise,
};

struct InterfererConfig {
    InterfererKind kind = InterfererKind::BandLimitedNoise;

    // Centre of the interferer relative to the wanted signal's centre. Negative
    // is below.
    Hertz offset_hz = 0;

    double power_db_relative_to_signal = -20.0;

    // Occupied bandwidth. BandLimitedNoise only.
    Hertz bandwidth_hz = 2500;
};

// Returns the power actually added, which is exact rather than approximate: the
// generated interferer is measured and scaled to the requested power before it
// is summed in.
[[nodiscard]] Expected<double> add_interferer(ComplexSpan signal,
                                              const InterfererConfig& config,
                                              SampleRate rate,
                                              double signal_power,
                                              std::uint64_t seed);

// ---------------------------------------------------------------------------
// The whole chain
// ---------------------------------------------------------------------------

enum class SignalPowerReference {
    // Calibrate against the power of the buffer handed in.
    InputBuffer,

    // Calibrate against the wanted signal measured after propagation, carrier
    // and timing impairments and before anything additive. This is the default
    // because it is what a receiver sees, and it is what "SNR at the receiver"
    // means everywhere else in the field.
    AfterPropagation,
};

struct ChannelConfig {
    MultipathProfile multipath;
    FadingConfig fading;
    FrequencyConfig frequency;
    TimingConfig timing;
    std::vector<InterfererConfig> interferers;
    ImpulsiveNoiseConfig impulsive;

    NoiseLevel noise;
    bool add_noise = true;

    SignalPowerReference power_reference = SignalPowerReference::AfterPropagation;
};

struct ChannelReport {
    double input_signal_power = 0.0;

    // The power the additive impairments were calibrated against.
    double wanted_signal_power = 0.0;

    double awgn_power_full_band = 0.0;
    double awgn_power_in_reference_bandwidth = 0.0;
    Hertz reference_bandwidth_hz = kWsjtxReferenceBandwidthHz;

    double requested_snr_in_reference_bandwidth_db = 0.0;
    double requested_snr_in_full_band_db = 0.0;

    double interference_power = 0.0;
    double impulse_power = 0.0;
    std::size_t impulse_event_count = 0;
    std::size_t output_sample_count = 0;
};

struct ChannelOutput {
    std::vector<Complex32> samples;

    // The wanted signal alone, carried through exactly the same propagation,
    // carrier and timing stages as samples but with nothing additive applied.
    // Same length as samples, so measure_snr can difference the two and report
    // what the channel actually delivered.
    std::vector<Complex32> clean;

    ChannelReport report;
};

[[nodiscard]] Expected<ChannelOutput> apply_channel(ConstComplexSpan input,
                                                    const ChannelConfig& config,
                                                    SampleRate rate,
                                                    std::uint64_t seed);

}  // namespace revenant::siggen
