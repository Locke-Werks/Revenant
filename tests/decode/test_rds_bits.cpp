// The RDS physical layer, against a transmitter written from the same clauses.
//
// WHY A ROUND TRIP IS THE TEST HERE
//
// Every GPU kernel in this engine is scored against a scalar twin, zero
// tolerance, ULP diffed. This decoder has no kernel, so that rule does not
// reach it and nothing about the arrangement below should be read as though
// it does. What replaces it is that the modulator and the demodulator were
// written from EN 50067:1998 clauses 1.1 through 1.7 independently of each
// other: a disagreement in how the standard was read shows up as a round trip
// that does not close, and that is the failure mode a clean-room decoder
// actually has. A real off-air capture would not localise it, because a
// capture has no ground truth in it.
//
// WHAT EACH CASE IS FOR
//
//   The shaping filter case checks the closed form against the frequency
//   response the standard prints, because the closed form is this project's
//   own derivation from equation (3) and nothing else in the suite would
//   catch an algebra slip in it.
//
//   The round trip is the baseline: a PRBS out and the same PRBS back, bit
//   exact, no errors permitted at all.
//
//   The phase cases are the sharp ones. EN 50067 clause 1.2 lets a
//   transmitter lock the subcarrier either IN PHASE or IN QUADRATURE with the
//   third harmonic of the pilot, and clause 1.6's differential coding exists
//   to absorb the 180 degree ambiguity a suppressed-carrier recovery loop
//   leaves on top of that. Both properties are silent when they break: an
//   inverted decoder produces a clean eye, a confident lock and a bitstream
//   that is wrong everywhere.
//
//   The timing cases move the sample grid off the bit grid and the
//   transmitter's clock off the receiver's, separately and together.
//
//   The BER sweep is the sensitivity number, stated against Eb/N0 so it is
//   comparable with the published curve for coherent BPSK.
//
//   The two negative cases are the contract the group layer depends on: an
//   unlocked decoder emits nothing. A decoder that hands noise to the block
//   layer spends its whole error-correction budget on it and then reports a
//   station that is not there.
//
// Every random input is seeded from a printed constant, per
// docs/conventions.md. A failure here reproduces from the seed in its own
// output.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "core/decode/rds_bits.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/modulators.h"
#include "core/dsp/synth/rds_mod.h"
#include "core/dsp/types.h"

using namespace revenant;
using Catch::Approx;

namespace {

// Printed with every failure. Changing it changes every payload in the file,
// which is the point: a bound that only holds for one PRBS is not a bound.
constexpr std::uint64_t kSeed = 20260920;

// 171000 is 144 samples per bit exactly and three per subcarrier cycle, which
// makes a hand check of any intermediate value possible. The engine will not
// hand over a rate this tidy, so the timing case also runs at 150000, where
// there are 126.3158 samples per bit and nothing divides.
constexpr dsp::SampleRate kTidyRate = 171000;
constexpr dsp::SampleRate kAwkwardRate = 150000;

struct Decoded {
    std::vector<std::uint8_t> bits;
    decode::RdsBitsStatus status;
};

[[nodiscard]] Decoded decode_all(const std::vector<float>& composite,
                                 const decode::RdsBitsConfig& config)
{
    auto sync = decode::RdsBitSync::create(config);
    REQUIRE(sync.has_value());
    sync->process(dsp::ConstRealSpan(composite));
    return Decoded{sync->drain(), sync->status()};
}

struct Alignment {
    bool found = false;
    std::size_t offset = 0;
    std::size_t compared = 0;
    std::size_t errors = 0;

    // Recovered bits past the end of the transmitted sequence, which are
    // decoded out of the shaping filter's run-out and are not scored.
    std::size_t overhang = 0;
};

// The most run-out the decoder should ever decode before its lock window
// notices the data stopped. One window plus a little.
constexpr std::size_t kMaxOverhangBits = 320;

// The recovered stream is a contiguous run of the transmitted one starting
// wherever the decoder locked, so the offset is found rather than assumed.
//
// It can also run PAST the end of the transmitted one. The modulator's last
// bit is followed by the shaping filter's run-out, during which the data
// signal decays to nothing while the pilot and the programme tone carry on,
// and the decoder's lock window takes a couple of hundred milliseconds to
// notice. Those trailing bits are decoded from a signal that is not there.
// Only the overlap is scored, and the caller checks the overhang separately,
// because silently truncating it would let a decoder that emits a thousand
// junk bits look identical to one that emits two.
//
// probe bounds how much of the recovered stream the search itself scores.
// Aligning on the whole thing would be circular in the BER sweep, where the
// stream being aligned is the one under measurement: at a BER of 0.1 the
// right offset still beats every wrong one over 256 bits by a factor of five,
// and scoring the remainder at that fixed offset is then an honest count.
[[nodiscard]] Alignment align(std::span<const std::uint8_t> sent,
                              std::span<const std::uint8_t> recovered,
                              std::size_t probe)
{
    Alignment best;
    const std::size_t window = std::min({probe, recovered.size(), sent.size()});
    if (window == 0) {
        return best;
    }

    std::size_t best_probe_errors = window + 1;
    for (std::size_t offset = 0; offset + window <= sent.size(); ++offset) {
        std::size_t errors = 0;
        for (std::size_t i = 0; i < window; ++i) {
            errors += (recovered[i] != sent[offset + i]) ? 1U : 0U;
        }
        if (errors < best_probe_errors) {
            best_probe_errors = errors;
            best.offset = offset;
            best.found = true;
        }
    }
    if (!best.found) {
        return best;
    }

    best.compared = std::min(recovered.size(), sent.size() - best.offset);
    best.overhang = recovered.size() - best.compared;
    best.errors = 0;
    for (std::size_t i = 0; i < best.compared; ++i) {
        best.errors += (recovered[i] != sent[best.offset + i]) ? 1U : 0U;
    }
    return best;
}

[[nodiscard]] siggen::RdsModSpec base_spec(dsp::SampleRate rate, std::size_t bit_count)
{
    siggen::RdsModSpec spec;
    spec.rate = rate;
    spec.bits = siggen::random_bits(bit_count, kSeed);
    // A programme tone so the decoder is asked to reject something. 1 kHz at
    // 40 kHz of deviation is an ordinary loud mono broadcast, and it sits
    // 24 dB above the subcarrier.
    spec.mono_tone_hz = 1000;
    spec.mono_deviation_hz = 40000;
    return spec;
}

void render(const siggen::RdsModSpec& spec, siggen::RdsComposite& out)
{
    auto composite = siggen::generate_rds(spec);
    REQUIRE(composite.has_value());
    out = *composite;
}

}  // namespace

TEST_CASE("the clause 1.7 shaping filter matches the response EN 50067 prints", "[decode][rds]")
{
    constexpr double kPi = std::numbers::pi;
    const double td = decode::kBitPeriodSeconds;

    // The two values the closed form has to get right on its own. h(0) is the
    // integral of HT(f) over all f, which is 8/(pi*td) by direct integration
    // of cos(pi*f*td/4) over the band. h(td/8) is the removable singularity:
    // both the numerator and the denominator vanish there and the limit is
    // 2/td.
    CHECK(decode::shaping_impulse(0.0, td) == Approx(8.0 / (kPi * td)).epsilon(1e-12));
    CHECK(decode::shaping_impulse(td / 8.0, td) == Approx(2.0 / td).epsilon(1e-6));
    CHECK(decode::shaping_impulse(-td / 8.0, td) == Approx(2.0 / td).epsilon(1e-6));

    // Approaching the singularity from either side lands on the same value,
    // which is what makes it removable rather than papered over.
    CHECK(decode::shaping_impulse(td / 8.0 - 1e-7 * td, td) ==
          Approx(2.0 / td).epsilon(1e-4));
    CHECK(decode::shaping_impulse(td / 8.0 + 1e-7 * td, td) ==
          Approx(2.0 / td).epsilon(1e-4));

    // Even, as a real symmetric frequency response demands.
    for (const double u : {0.03, 0.2, 0.7, 1.9, 3.3}) {
        CHECK(decode::shaping_impulse(u * td, td) ==
              Approx(decode::shaping_impulse(-u * td, td)).epsilon(1e-12));
    }

    // And the response itself. EN 50067:1998 clause 1.7 equation (3):
    //
    //     HT(f) = cos(pi * f * td / 4)   for 0 <= f <= 2/td
    //     HT(f) = 0                      for f > 2/td
    //
    // Numerically transformed from the closed form, over 64 bit periods
    // either side at 64 steps per bit period. The tails fall as 1/t^2 and
    // oscillate, so the truncation residue is a couple of parts in a
    // thousand, which is what the tolerances below allow for.
    const double step = td / 64.0;
    const int half = 64 * 64;
    const auto response = [&](double frequency) {
        double sum = 0.0;
        for (int n = -half; n <= half; ++n) {
            const double t = static_cast<double>(n) * step;
            sum += decode::shaping_impulse(t, td) * std::cos(2.0 * kPi * frequency * t);
        }
        return sum * step;
    };

    for (const double frequency : {0.0, 400.0, 900.0, 1500.0, 2000.0, 2300.0}) {
        const double expected = std::cos(kPi * frequency * td / 4.0);
        INFO(std::format("HT at {} Hz", frequency));
        CHECK(response(frequency) == Approx(expected).margin(0.01));
    }

    // Zero at the band edge. This is the property the cutoff has to be 2375
    // for: cos(pi * 2375 * td / 4) is cos(pi/2) exactly, and a filter designed
    // to 2400 does not reach zero there and rings when it is truncated.
    CHECK(std::abs(response(static_cast<double>(decode::kShapingCutoffHz))) < 0.01);
    for (const double frequency : {2800.0, 3500.0, 5000.0}) {
        INFO(std::format("HT stopband at {} Hz", frequency));
        CHECK(std::abs(response(frequency)) < 0.03);
    }
}

TEST_CASE("the composite carries the injection levels the standard names", "[decode][rds]")
{
    INFO(std::format("seed {}", kSeed));

    siggen::RdsModSpec spec = base_spec(kTidyRate, 512);
    spec.mono_deviation_hz = 0;
    siggen::RdsComposite composite;
    render(spec, composite);

    // EN 50067:1998 clause 1.3, the recommended best compromise: the RDS
    // component peaks at 2 kHz of the 75 kHz multiplex cap.
    const double expected_peak = 2000.0 / 75000.0;
    CHECK(composite.rds_envelope_peak == Approx(expected_peak).epsilon(1e-9));

    // And the rendered samples fall short of that envelope by exactly the
    // amount the sample grid costs. 171000 Hz is three samples per 57 kHz
    // cycle, so the subcarrier is only ever caught at sin of 0 and +/- 120
    // degrees and the largest sample is 0.866 of the crest. A test that
    // asserted the two were equal would be asserting a property of the grid.
    CHECK(composite.rds_peak < composite.rds_envelope_peak);
    CHECK(composite.rds_peak == Approx(expected_peak * std::sqrt(3.0) / 2.0).epsilon(0.02));

    // The pilot at 9 percent, which is an FM stereo number rather than an RDS
    // one and is checked here only because the subcarrier is locked to it.
    CHECK(composite.rds_rms < composite.rds_peak);
    CHECK(composite.composite_peak > 6750.0 / 75000.0);

    // Clause 1.6 leaves the transmitted stream the same length as the input.
    CHECK(composite.transmitted_bits.size() == composite.data_bits.size());

    // And Table 1 literally: input 0 leaves the output unchanged, input 1
    // complements it.
    bool state = false;
    for (std::size_t i = 0; i < spec.bits.size(); ++i) {
        state = state != (spec.bits[i] != 0);
        REQUIRE(composite.transmitted_bits[i] == static_cast<std::uint8_t>(state ? 1 : 0));
    }
}

TEST_CASE("a PRBS round-trips through the subcarrier bit exact", "[decode][rds]")
{
    INFO(std::format("seed {}", kSeed));

    const siggen::RdsModSpec spec = base_spec(kTidyRate, 3000);
    siggen::RdsComposite composite;
    render(spec, composite);
    const std::vector<float>& samples = composite.samples;

    decode::RdsBitsConfig config;
    config.rate = kTidyRate;
    const Decoded decoded = decode_all(samples, config);

    INFO(std::format("lock {} quality {:.3f} coherence {:.3f} bits {}",
                     decode::lock_name(decoded.status.lock), decoded.status.quality,
                     decoded.status.carrier_coherence, decoded.bits.size()));

    REQUIRE(decoded.status.lock == decode::RdsLock::Locked);
    REQUIRE(decoded.status.pilot_locked);

    // Acquisition costs the carrier loop's settling, the timing scan and one
    // lock window, which together are a few hundred bits. Everything after
    // that has to be right.
    REQUIRE(decoded.bits.size() > spec.bits.size() / 2);

    const Alignment match = align(spec.bits, decoded.bits, 256);
    INFO(std::format("offset {} compared {} errors {} overhang {}", match.offset,
                     match.compared, match.errors, match.overhang));
    REQUIRE(match.found);
    CHECK(match.errors == 0);
    CHECK(match.overhang <= kMaxOverhangBits);

    // The recovered clock is the transmitted one. Nominal, because this spec
    // asks for no clock error.
    CHECK(decoded.status.bit_rate_hz == Approx(decode::kBitRateHz).epsilon(1e-3));
    CHECK(std::abs(decoded.status.carrier_offset_hz) < 2.0);
    CHECK(decoded.status.quality > 0.95);
}

TEST_CASE("the subcarrier phase does not reach the decoded bits", "[decode][rds]")
{
    INFO(std::format("seed {}", kSeed));

    // EN 50067:1998 clause 1.2 permits in phase and in quadrature, to within
    // +/- 10 degrees, and says nothing that would let a receiver assume
    // either. Clause 1.6's differential coding then absorbs the further 180
    // degrees that a suppressed-carrier recovery loop cannot resolve, because
    // (NOT d_i) XOR (NOT d_i-1) is d_i XOR d_i-1.
    //
    // This is the property most likely to be silently wrong, because getting
    // it wrong costs nothing visible: the eye is open, the loop reports lock,
    // and every bit is inverted.
    constexpr double kPi = std::numbers::pi;
    const struct {
        double radians;
        const char* what;
    } cases[] = {
        {0.0, "in phase, clause 1.2 first option"},
        {kPi, "in phase and inverted, the 180 degree ambiguity itself"},
        {kPi / 2.0, "in quadrature, clause 1.2 second option"},
        {3.0 * kPi / 2.0, "in quadrature and inverted"},
        {kPi / 2.0 + 10.0 * kPi / 180.0, "quadrature at the +10 degree tolerance edge"},
        {-10.0 * kPi / 180.0, "in phase at the -10 degree tolerance edge"},
    };

    for (const auto& phase_case : cases) {
        siggen::RdsModSpec spec = base_spec(kTidyRate, 2400);
        spec.subcarrier_phase_radians = phase_case.radians;

        siggen::RdsComposite composite;
        render(spec, composite);
        const std::vector<float>& samples = composite.samples;

        decode::RdsBitsConfig config;
        config.rate = kTidyRate;
        const Decoded decoded = decode_all(samples, config);

        INFO(std::format("{} ({:.4f} rad): lock {} bits {}", phase_case.what,
                         phase_case.radians, decode::lock_name(decoded.status.lock),
                         decoded.bits.size()));

        REQUIRE(decoded.status.lock == decode::RdsLock::Locked);
        const Alignment match = align(spec.bits, decoded.bits, 256);
        REQUIRE(match.found);
        INFO(std::format("offset {} compared {} errors {} overhang {}", match.offset,
                         match.compared, match.errors, match.overhang));

        // Every phase recovers the payload that went in, which is the whole
        // claim. The comparison is against spec.bits and not against another
        // case's output, because each case locks wherever its own acquisition
        // happened to finish and the streams start at different bits.
        CHECK(match.errors == 0);
        CHECK(match.compared > spec.bits.size() / 2);
        CHECK(match.overhang <= kMaxOverhangBits);
    }
}

TEST_CASE("a fractional sample offset and a clock error are both recovered", "[decode][rds]")
{
    INFO(std::format("seed {}", kSeed));

    const struct {
        dsp::SampleRate rate;
        double offset_samples;
        double clock_ppm;
        dsp::Hertz subcarrier_offset;
        const char* what;
    } cases[] = {
        {kTidyRate, 0.37, 0.0, 0, "a third of a sample of timing offset"},
        {kTidyRate, 0.0, 50.0, 0, "50 ppm fast transmitter clock"},
        {kTidyRate, 0.0, -50.0, 0, "50 ppm slow transmitter clock"},
        {kTidyRate, 0.63, 0.0, 30, "30 Hz of incoherent subcarrier error"},
        // EN 50067 clause 1.1 caps the subcarrier error at +/- 6 Hz, so 30 Hz
        // is five times what a conformant transmitter does and stands in for
        // the receiver's own local oscillator on top of it.
        {kAwkwardRate, 0.41, 40.0, -25, "everything at once, at a rate that does not divide"},
    };

    for (const auto& timing_case : cases) {
        siggen::RdsModSpec spec = base_spec(timing_case.rate, 2400);
        spec.start_offset_samples = timing_case.offset_samples;
        spec.clock_error_ppm = timing_case.clock_ppm;
        spec.subcarrier_offset_hz = timing_case.subcarrier_offset;

        siggen::RdsComposite composite;
        render(spec, composite);
        const std::vector<float>& samples = composite.samples;

        decode::RdsBitsConfig config;
        config.rate = timing_case.rate;
        const Decoded decoded = decode_all(samples, config);

        INFO(std::format("{}: lock {} bits {} rate {:.4f} offset {:.2f} Hz",
                         timing_case.what, decode::lock_name(decoded.status.lock),
                         decoded.bits.size(), decoded.status.bit_rate_hz,
                         decoded.status.carrier_offset_hz));

        REQUIRE(decoded.status.lock == decode::RdsLock::Locked);
        const Alignment match = align(spec.bits, decoded.bits, 256);
        REQUIRE(match.found);
        INFO(std::format("offset {} compared {} errors {} overhang {}", match.offset,
                         match.compared, match.errors, match.overhang));
        CHECK(match.errors == 0);
        CHECK(match.compared > spec.bits.size() / 2);
        CHECK(match.overhang <= kMaxOverhangBits);

        // The timing loop's integrator has to hold the clock error, not
        // merely survive it: a first order loop would track with a standing
        // phase offset and would fail this while still decoding.
        const double expected_rate =
            decode::kBitRateHz * (1.0 + timing_case.clock_ppm * 1e-6);
        CHECK(decoded.status.bit_rate_hz == Approx(expected_rate).epsilon(3e-4));

        // The carrier loop holds the INCOHERENT offset and nothing else,
        // and that is the clause 1.1 third-harmonic lock being demonstrated
        // rather than an accident. A clock error moves the pilot to
        // 19000*(1+ppm) and the subcarrier to three times that, so a decoder
        // deriving its reference from the pilot follows the subcarrier for
        // free and has no residual to hold. 50 ppm would otherwise leave
        // 2.85 Hz on the loop, and a decoder running a free 57 kHz
        // oscillator would show exactly that.
        const double expected_carrier = static_cast<double>(timing_case.subcarrier_offset);
        CHECK(decoded.status.carrier_offset_hz == Approx(expected_carrier).margin(2.0));
        CHECK(decoded.status.pilot_locked);
    }
}

TEST_CASE("blocking the input does not change the bits", "[decode][rds]")
{
    INFO(std::format("seed {}", kSeed));

    const siggen::RdsModSpec spec = base_spec(kTidyRate, 1024);
    siggen::RdsComposite composite;
    render(spec, composite);
    const std::vector<float>& samples = composite.samples;

    decode::RdsBitsConfig config;
    config.rate = kTidyRate;
    const Decoded whole = decode_all(samples, config);

    // Chunk sizes that are coprime with the decimation factor and with the
    // samples per bit, so no boundary lands in the same place twice.
    for (const std::size_t chunk : {1U, 7U, 997U, 4099U}) {
        auto sync = decode::RdsBitSync::create(config);
        REQUIRE(sync.has_value());
        std::vector<std::uint8_t> bits;
        const decode::BitSink sink = [&bits](bool bit) {
            bits.push_back(static_cast<std::uint8_t>(bit ? 1 : 0));
        };
        for (std::size_t offset = 0; offset < samples.size(); offset += chunk) {
            const std::size_t count = std::min(chunk, samples.size() - offset);
            sync->process(dsp::ConstRealSpan(samples.data() + offset, count), sink);
        }
        INFO(std::format("chunk {}: {} bits against {}", chunk, bits.size(),
                         whole.bits.size()));
        CHECK(bits == whole.bits);
    }
}

TEST_CASE("bit error rate against Eb/N0", "[decode][rds]")
{
    INFO(std::format("seed {}", kSeed));

    // Stated against Eb/N0 rather than a reference bandwidth, because the BER
    // literature plots against Eb/N0 and the point of the numbers below is
    // that they are comparable with the published curve for coherent BPSK.
    // docs/snr-convention.md gives the conversion and notes that neither
    // figure carries a sample rate, which is what makes both safe to report.
    //
    // The curve to beat: matched-filter BPSK is Q(sqrt(2*Eb/N0)), and clause
    // 1.6's differential decoding roughly doubles it, because one channel
    // error corrupts the two decoded bits either side of it.
    //
    //   Eb/N0   2*Q(sqrt(2*Eb/N0))   measured   ceiling
    //    4 dB        2.49e-2         3.00e-2    5.0e-2
    //    6 dB        4.77e-3         6.39e-3    1.2e-2
    //    8 dB        3.80e-4         5.32e-4    1.5e-3
    //   10 dB        7.74e-6         0          5.5e-4
    //   12 dB        1.0e-8          0          5.5e-4
    //
    // The measured column is this decoder on this payload with this seed, and
    // it runs about 1.4 times theory, which is 0.4 dB of implementation loss
    // across the two loops, the cubic interpolator and both filter
    // truncations. The ceilings are roughly twice measured, so a regression
    // that costs another decibel fails here and ordinary numerical drift does
    // not.
    //
    // The bottom two rows are not a rate measurement and should not be read
    // as one. 3757 bits cannot resolve anything below 2.7e-4, so a ceiling of
    // 5.5e-4 is "at most two errors" and what those rows actually assert is
    // that lock holds and the curve has not turned back up.
    const struct {
        double eb_n0_db;
        double ceiling;
    } points[] = {
        {4.0, 0.050},
        {6.0, 0.012},
        {8.0, 0.0015},
        {10.0, 0.00055},
        {12.0, 0.00055},
    };

    constexpr std::size_t kBitCount = 4096;
    const siggen::RdsModSpec spec = base_spec(kTidyRate, kBitCount);
    siggen::RdsComposite clean;
    render(spec, clean);
    const std::vector<float>& reference = clean.samples;

    double previous = 1.0;
    for (const auto& point : points) {
        std::vector<float> samples = reference;
        const siggen::NoiseLevel level =
            siggen::NoiseLevel::eb_over_n0_db(point.eb_n0_db, decode::kBitRateHz);

        auto report = siggen::add_real_awgn(dsp::RealSpan(samples), clean.rds_mean_power, level,
                                            kTidyRate,
                                            siggen::derive_seed(kSeed, 0x4245'5200));
        REQUIRE(report.has_value());

        // The channel delivered what it was asked for. Round-tripping the
        // request through the report is what catches a calibration that is
        // out by the factor of two a real signal costs.
        CHECK(report->eb_over_n0_db == Approx(point.eb_n0_db).margin(1e-6));

        decode::RdsBitsConfig config;
        config.rate = kTidyRate;
        const Decoded decoded = decode_all(samples, config);

        const Alignment match = align(spec.bits, decoded.bits, 256);
        const double ber = (match.compared > 0)
                               ? static_cast<double>(match.errors) /
                                     static_cast<double>(match.compared)
                               : 1.0;

        INFO(std::format(
            "Eb/N0 {:.1f} dB (SNR {:.2f} dB in {} Hz): lock {} quality {:.3f} bits {} "
            "errors {} BER {:.3e}, ceiling {:.3e}",
            point.eb_n0_db, report->snr_in_reference_bandwidth_db,
            report->reference_bandwidth_hz, decode::lock_name(decoded.status.lock),
            decoded.status.quality, match.compared, match.errors, ber, point.ceiling));

        REQUIRE(decoded.status.lock == decode::RdsLock::Locked);
        REQUIRE(match.compared > kBitCount / 3);
        CHECK(ber <= point.ceiling);

        // Monotone. A curve that improves and then does not is a loop falling
        // out of lock at high SNR, which no amount of ceiling would catch.
        CHECK(ber <= previous);
        previous = ber;
    }
}

TEST_CASE("no pilot on the composite", "[decode][rds]")
{
    INFO(std::format("seed {}", kSeed));

    // EN 50067:1998 clause 1.1 puts the subcarrier at 57 kHz +/- 6 Hz during
    // MONOPHONIC transmission, where there is no pilot to lock it to. A
    // decoder that needs one is wrong about the standard, so the contract is
    // that it either locks or says plainly that it did not.
    siggen::RdsModSpec spec = base_spec(kTidyRate, 2400);
    spec.pilot_enabled = false;
    spec.mono_deviation_hz = 40000;
    // Five times the clause 1.1 tolerance, because with no pilot there is
    // nothing pinning the subcarrier and the receiver's own oscillator error
    // rides on top of the transmitter's.
    spec.subcarrier_offset_hz = 30;

    siggen::RdsComposite composite;
    render(spec, composite);
    const std::vector<float>& samples = composite.samples;

    decode::RdsBitsConfig config;
    config.rate = kTidyRate;
    const Decoded decoded = decode_all(samples, config);

    INFO(std::format("lock {} pilot_locked {} pilot_level {:.4f} bits {} offset {:.2f} Hz",
                     decode::lock_name(decoded.status.lock), decoded.status.pilot_locked,
                     decoded.status.pilot_level, decoded.bits.size(),
                     decoded.status.carrier_offset_hz));

    // The pilot report has to be honest whichever way the decode went.
    CHECK_FALSE(decoded.status.pilot_locked);

    if (decoded.status.lock == decode::RdsLock::Locked) {
        const Alignment match = align(spec.bits, decoded.bits, 256);
        REQUIRE(match.found);
        INFO(std::format("offset {} compared {} errors {} overhang {}", match.offset,
                         match.compared, match.errors, match.overhang));
        CHECK(match.errors == 0);
        CHECK(match.overhang <= kMaxOverhangBits);
    } else {
        // The other half of the contract, and the one that matters more: a
        // decoder that did not lock hands the group layer nothing rather than
        // handing it noise.
        CHECK(decoded.bits.empty());
    }

    // And the same composite must still decode when the decoder is told not
    // to look for a pilot at all, which is the configuration a mono-only
    // deployment would use.
    decode::RdsBitsConfig no_pilot = config;
    no_pilot.track_pilot = false;
    const Decoded without = decode_all(samples, no_pilot);
    INFO(std::format("track_pilot off: lock {} bits {}",
                     decode::lock_name(without.status.lock), without.bits.size()));
    REQUIRE(without.status.lock == decode::RdsLock::Locked);
    const Alignment match = align(spec.bits, without.bits, 256);
    REQUIRE(match.found);
    INFO(std::format("offset {} compared {} errors {} overhang {}", match.offset,
                     match.compared, match.errors, match.overhang));
    CHECK(match.errors == 0);
    CHECK(match.compared > spec.bits.size() / 2);
}

TEST_CASE("noise alone never reports lock and never emits a bit", "[decode][rds]")
{
    INFO(std::format("seed {}", kSeed));

    // The contract the group layer rests on. EN 50067 Annex C.2 puts a false
    // syndrome match at roughly six per second against random data, which the
    // block layer's flywheel is sized to survive. It is not sized to survive
    // a physical layer that keeps feeding it random data while reporting a
    // lock, and neither is anything a user reads off the result.
    constexpr std::size_t kSampleCount = 171000 * 3;
    std::vector<float> samples(kSampleCount, 0.0F);

    // Calibrated against a unit signal that is not there, which is the point:
    // this is the noise a -6 dB RDS signal would sit in, with the signal
    // removed.
    auto report = siggen::add_real_awgn(dsp::RealSpan(samples), 1.0,
                                        siggen::NoiseLevel::snr_in_full_sample_rate_db(6.0),
                                        kTidyRate, siggen::derive_seed(kSeed, 0x4E4F'4953));
    REQUIRE(report.has_value());

    decode::RdsBitsConfig config;
    config.rate = kTidyRate;
    const Decoded decoded = decode_all(samples, config);

    INFO(std::format("lock {} quality {:.3f} coherence {:.3f} consistency {:.3f} bits {}",
                     decode::lock_name(decoded.status.lock), decoded.status.quality,
                     decoded.status.carrier_coherence, decoded.status.biphase_consistency,
                     decoded.bits.size()));

    CHECK(decoded.status.lock != decode::RdsLock::Locked);
    CHECK(decoded.bits.empty());
    CHECK(decoded.status.bits_emitted == 0);
    CHECK_FALSE(decoded.status.pilot_locked);

    // Biphase sign consistency is the quality figure, and on noise it has to
    // read as a coin toss. Anything materially above 0.5 here would mean the
    // metric is measuring the decoder rather than the channel.
    if (decoded.status.biphase_consistency > 0.0) {
        CHECK(decoded.status.biphase_consistency < 0.65);
    }
    CHECK(decoded.status.quality < 0.3);
}

TEST_CASE("the decoder refuses a rate it cannot carry the subcarrier at", "[decode][rds]")
{
    decode::RdsBitsConfig config;
    config.rate = 96000;
    auto sync = decode::RdsBitSync::create(config);
    REQUIRE_FALSE(sync.has_value());

    // The message has to name the number, because the caller's next question
    // is what rate to ask the receiver for.
    CHECK(sync.error().message.find("125000") != std::string::npos);

    config.rate = decode::kMinimumRateHz;
    CHECK(decode::RdsBitSync::create(config).has_value());
}
