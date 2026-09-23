// The BER/SNR sweep harness.
//
// Sweep SNR across a range, run trials at each point, report a bit error rate
// per point. Stored per commit, the resulting curve is what lets CI fail on a
// numerical regression: a decoder that quietly loses a decibel of sensitivity
// passes every unit test ever written for it and is obvious here.
//
// This is the discipline WSJT-X used to reach -24 dB, and it is the only route
// to anywhere near it. Running the sweep on every commit is affordable only
// because the engine works offline and faster than realtime.
//
// Everything in this file is a pure function of its arguments and an explicit
// seed. Nothing reads a clock and nothing touches global state. The subject and
// the generator are invoked concurrently from several threads, so a caller's
// implementation of either must be pure as well: no shared mutable state, no
// lazily initialised caches.
//
// The unit under test is an interface. Two implementations are in tree: a
// coherent BPSK detector whose BER against AWGN has a closed form, below, and
// the RDS decoder against its own transmitter, in tools/bench/rds_subject.h.
// The BPSK one stays because a harness validated against Q(sqrt(2*Eb/N0)) has
// proven its SNR calibration, its generator and its accounting all at once,
// and no decoder's curve can do that.
//
// This paragraph used to say "There are no decoders yet", which stopped being
// true when core/decode/rds_bits.h landed and stayed in place until the RDS
// subject was added here on 2026-09-22.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::bench {

// What one trial recovered. Bit counts drive the BER curve; `decoded` carries
// frame-oriented modes, where a partially correct frame is worth nothing and
// the metric is the fraction of frames that came back at all.
struct TrialResult {
    std::uint64_t bits_total = 0;
    std::uint64_t bits_wrong = 0;
    bool decoded = false;
};

// A subject under test: given impaired complex baseband and the ground-truth
// payload, report what it recovered. A decoder in M4 adapts to this. Called
// concurrently, so it must be pure.
using Subject = std::function<TrialResult(dsp::ConstComplexSpan, std::span<const std::uint8_t>)>;

// Produces the impaired baseband for one trial: modulate the payload, apply the
// channel at the given SNR, using only the supplied seed for randomness.
//
// This is the seam core/dsp/synth plugs into: tools/bench/rds_subject.cpp
// builds one from rds_mod.h and channel.h's noise. Called concurrently, so it
// must be pure.
using Generator = std::function<std::vector<dsp::Complex32>(std::span<const std::uint8_t> payload,
                                                            double snr_db,
                                                            std::uint64_t seed)>;

// The SplitMix64 finalizer. Used rather than something ad hoc because it is
// published, has no fixed points that matter here, and avalanches a counter
// input properly: consecutive trial indices must not produce correlated noise
// streams or the trials stop being independent samples.
[[nodiscard]] std::uint64_t splitmix64(std::uint64_t x);

// Seed for trial `trial_index` at SNR point `point_index`.
//
// The derivation is: mix the base seed with the point index, mix that result
// with the trial index, both through the SplitMix64 finalizer and each index
// pre-multiplied by a distinct odd constant so (j, k) and (k, j) cannot
// collide. It depends on nothing but the three arguments, which is what makes a
// single trial reproducible in isolation from the seed printed by a sweep, and
// what makes the sweep's result independent of how many threads ran it.
[[nodiscard]] std::uint64_t trial_seed(std::uint64_t base_seed,
                                       std::uint64_t point_index,
                                       std::uint64_t trial_index);

// Two independent streams are derived from the trial seed so that changing the
// payload length does not shift the noise, and vice versa. The tags are ASCII
// so a seed appearing in a log is traceable to the stream that consumed it.
inline constexpr std::uint64_t kPayloadStreamTag = 0x5041594C4F414400ULL;  // "PAYLOAD\0"
inline constexpr std::uint64_t kChannelStreamTag = 0x4348414E4E454C00ULL;  // "CHANNEL\0"

struct SweepConfig {
    double snr_start_db = -4.0;
    double snr_stop_db = 10.0;
    double snr_step_db = 1.0;

    std::uint64_t trials_per_point = 1000;
    std::size_t payload_bytes = 64;
    std::uint64_t base_seed = 0;

    // Stop a point once this many bit errors have accumulated. A BER estimate
    // built from k observed errors has a relative standard deviation of
    // 1/sqrt(k), so 100 errors is roughly 10 percent and further trials buy
    // very little. Without this rule a high SNR point spends the entire trial
    // budget confirming zero errors, which is where almost all the runtime of a
    // naive sweep goes. Zero disables early termination.
    std::uint64_t min_bit_errors = 100;

    // Trials are run in fixed-size batches and the termination rule is tested
    // only at a batch boundary. The size is fixed by configuration rather than
    // by the thread count on purpose: it is what keeps the number of trials
    // actually executed, and therefore the result, identical on a 4 core
    // machine and a 64 core one.
    std::uint64_t trial_batch = 64;

    // Zero means hardware_concurrency. Never serialized into a curve, because
    // it cannot change the result and recording it would make two identical
    // sweeps on different machines produce different files.
    unsigned thread_count = 0;

    [[nodiscard]] Status validate() const;

    // Computed by multiplication rather than by accumulating the step, so the
    // last point does not drift by a few ulps over a long sweep.
    [[nodiscard]] double snr_db_at(std::size_t point_index) const;

    [[nodiscard]] std::size_t point_count() const;
};

struct SweepPoint {
    double snr_db = 0.0;
    std::uint64_t trials = 0;
    std::uint64_t bits_total = 0;
    std::uint64_t bit_errors = 0;
    std::uint64_t decoded_count = 0;

    [[nodiscard]] double ber() const;
    [[nodiscard]] double decode_rate() const;
};

// Called once per completed point, on the calling thread, so a long sweep can
// print as it goes. Optional.
using ProgressFn = std::function<void(std::size_t point_index, const SweepPoint& point)>;

[[nodiscard]] Expected<std::vector<SweepPoint>> run_sweep(const SweepConfig& config,
                                                          const Subject& subject,
                                                          const Generator& generator,
                                                          const ProgressFn& progress = {});

// One trial, standalone. Given the config and the two indices this reproduces
// exactly the trial the sweep ran, which is the whole point of deriving seeds
// from indices: a failure at 3 dB on trial 417 can be re-run on its own under a
// debugger.
[[nodiscard]] TrialResult run_trial(const SweepConfig& config,
                                    const Subject& subject,
                                    const Generator& generator,
                                    std::size_t point_index,
                                    std::uint64_t trial_index);

// Payload bits are numbered MSB first within each byte, which is the ordering
// every framed radio protocol worth naming uses on the wire.
[[nodiscard]] bool payload_bit(std::span<const std::uint8_t> payload, std::size_t bit_index);

// Uniformly random payload from a seed. std::mt19937_64 with the caller's seed,
// taking eight bytes per draw least significant first, so the bytes are fully
// specified by the standard and do not vary with the standard library.
[[nodiscard]] std::vector<std::uint8_t> random_payload(std::size_t bytes, std::uint64_t seed);

// ---------------------------------------------------------------------------
// The reference BPSK subject.
//
// Its waveform and its AWGN are generated here rather than by
// core/dsp/synth, and that is now a choice rather than a stopgap. This block
// used to say it was temporary and would be deleted once modulators.h and
// channel.h existed. Both exist, and it stays: the reference exists to check
// the harness against a closed form, and a check that routed through the
// same noise generator as the subjects it referees would share their faults.
// ---------------------------------------------------------------------------

struct ReferenceBpsk {
    // One sample per symbol makes the closed form obvious; more exercises the
    // matched filter. The detector and the generator must be built from the
    // same value.
    std::uint32_t samples_per_symbol = 1;

    float amplitude = 1.0f;
};

// BPSK over AWGN. `snr_db` is Eb/N0 in dB, the only SNR definition under which
// a BER curve can be compared against anything published.
//
// Energy per bit is amplitude^2 * samples_per_symbol, so N0 = Eb / (Eb/N0) and
// the noise standard deviation is sqrt(N0/2) in each of the two dimensions.
// That calibration is what the theory check verifies.
[[nodiscard]] Generator make_bpsk_awgn_generator(ReferenceBpsk options = {});

// Coherent BPSK detection: matched filter over each symbol, decide on the sign
// of the real part. Bit 1 maps to +amplitude and bit 0 to -amplitude.
[[nodiscard]] Subject make_bpsk_reference_subject(ReferenceBpsk options = {});

// Q(x), the tail probability of the standard normal. Expressed through erfc
// because writing it as an integral or a series is how a reference acquires a
// bug of its own.
[[nodiscard]] double q_function(double x);

// Coherent BPSK over AWGN: Pb = Q(sqrt(2*Eb/N0)), which is 0.5*erfc(sqrt(Eb/N0)).
[[nodiscard]] double bpsk_theoretical_ber(double ebno_db);

}  // namespace revenant::bench
