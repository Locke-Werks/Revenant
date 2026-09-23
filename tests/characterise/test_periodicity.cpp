// Cyclic prefixes and frame periods.
//
// The two readings come out of one autocorrelation and the case that makes
// each of them mean anything is the other one's signal. A BPSK waveform
// whose payload repeats has a correlation peak that an OFDM reader will
// call a symbol period unless something stops it, and an OFDM waveform has
// a peak a frame reader will call a frame period unless something stops
// that. Both directions are asserted here, against the same two signals,
// because an implementation can pass either one alone by picking a rule.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numbers>
#include <print>
#include <random>
#include <string>
#include <vector>

#include "core/characterise/periodicity.h"
#include "core/characterise/transform.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/modulators.h"
#include "tests/characterise/signal_lab.h"

using namespace revenant;
using characterise::Complex64;
using Catch::Approx;

namespace {

constexpr std::uint64_t kSeed = 20260921;
constexpr dsp::SampleRate kRate = 48000;

// The test OFDM waveform. 256-point useful symbol and a 32-sample guard is
// a one-eighth prefix, so the correlation at lag 256 should read
// 32/288 = 0.111 and nothing else in the lag range should read anything.
constexpr std::size_t kUseful = 256;
constexpr std::size_t kPrefix = 32;
constexpr std::size_t kOfdmSymbols = 600;

[[nodiscard]] std::vector<Complex64> analysis_of(const std::vector<dsp::Complex32>& samples)
{
    return characterise::to_analysis(dsp::ConstComplexSpan(samples));
}

[[nodiscard]] std::vector<dsp::Complex32> ofdm_burst(std::uint64_t seed)
{
    characterise_test::OfdmSpec spec;
    spec.useful_samples = kUseful;
    spec.prefix_samples = kPrefix;
    spec.used_carriers = 100;
    spec.symbol_count = kOfdmSymbols;
    spec.seed = seed;
    return characterise_test::ofdm_signal(spec);
}

// BPSK whose payload repeats inside the buffer, which is what
// core/dsp/synth/modulators.h does by default and what a framed waveform
// does for real. 256 symbols at 20 samples each closes the cycle on 5120.
constexpr std::size_t kFrameSamples = 5120;

[[nodiscard]] siggen::GeneratedSignal framed_bpsk(std::uint64_t seed, std::size_t count)
{
    siggen::ModulatorConfig common;
    common.rate = kRate;
    common.seed = seed;

    siggen::PskParams psk;
    psk.symbol_rate = 2400.0;
    psk.rolloff = 0.35;
    psk.symbol_count = 256;

    auto generated = siggen::generate_bpsk(common, psk, count);
    REQUIRE(generated.has_value());
    REQUIRE(generated->cycle_samples == kFrameSamples);
    return *generated;
}

}  // namespace

// REJECTS: an implementation that reports the TOTAL symbol period as the
// useful symbol length. 256 and 288 are both real properties of this
// waveform and both plausible answers, and the subcarrier spacing derived
// from them differs by 12 percent, which is the difference between naming
// a system and not. The correlation peaks at the useful length, because
// that is the lag at which the guard sits over the copy it was taken from.
//
// Also rejects one that reports the guard length as the lag, which is the
// other thing at that end of the waveform.
TEST_CASE("an OFDM burst gives up its useful symbol and its guard", "[characterise]")
{
    std::println("test_periodicity ofdm: seed {}", kSeed);
    const auto samples = ofdm_burst(kSeed);
    REQUIRE(samples.size() == (kUseful + kPrefix) * kOfdmSymbols);

    const auto profile = characterise::autocorrelation(analysis_of(samples), kRate);
    REQUIRE(profile.has_value());

    const auto ofdm = characterise::find_cyclic_prefix(*profile);
    std::println("  useful {} samples, guard {} samples, correlation {:.4f}, floor {:.4f}, "
                 "spacing {:.1f} Hz",
                 ofdm.symbol_samples, ofdm.prefix_samples, ofdm.correlation, ofdm.floor_ratio,
                 ofdm.subcarrier_spacing_hz);
    CAPTURE(ofdm.symbol_samples, ofdm.prefix_samples, ofdm.correlation, ofdm.refusal);
    REQUIRE(ofdm.found);
    REQUIRE(ofdm.symbol_samples == kUseful);
    REQUIRE(ofdm.symbol_samples != kUseful + kPrefix);

    // 32/288, from the share of each symbol period the guard occupies.
    REQUIRE(ofdm.correlation == Approx(static_cast<double>(kPrefix) /
                                       static_cast<double>(kUseful + kPrefix))
                                    .epsilon(0.15));
    // The guard comes back out of that ratio, so it is the softest number
    // here and the tolerance says so.
    REQUIRE(static_cast<double>(ofdm.prefix_samples) ==
            Approx(static_cast<double>(kPrefix)).epsilon(0.25));
    REQUIRE(ofdm.subcarrier_spacing_hz ==
            Approx(static_cast<double>(kRate) / static_cast<double>(kUseful)).epsilon(0.01));
}

// REJECTS: an OFDM reader that takes the strongest autocorrelation peak as
// a symbol period. This waveform has one, at lag 5120, and it is a frame.
// The ceiling on the prefix ratio is the only thing between a
// characteriser and reporting every framed single-carrier signal as OFDM
// with a 107 ms symbol, which is a sentence that would survive review
// because the number is real.
//
// The second half asserts the same peak IS read, by the reader whose job
// it is. One signal, two readers, opposite answers.
TEST_CASE("a framed single-carrier signal is a frame, not a cyclic prefix", "[characterise]")
{
    std::println("test_periodicity framed bpsk: seed {}", kSeed + 1);
    const auto generated = framed_bpsk(kSeed + 1, 1U << 17);

    const auto profile = characterise::autocorrelation(analysis_of(generated.samples), kRate);
    REQUIRE(profile.has_value());

    const auto ofdm = characterise::find_cyclic_prefix(*profile);
    CAPTURE(ofdm.symbol_samples, ofdm.correlation, ofdm.refusal);
    REQUIRE_FALSE(ofdm.found);
    // Refused on the ceiling, not on the floor. The distinction matters:
    // a refusal on the floor would mean nothing repeated, and something
    // very much did.
    REQUIRE(ofdm.correlation > 0.5);
    REQUIRE(ofdm.refusal.find("find_frame_period") != std::string::npos);

    const auto frame = characterise::find_frame_period(*profile);
    std::println("  frame {} samples ({:.2f} ms), repeat {:.4f}, floor {:.4f}, strongest lag {}, "
                 "divisor {}",
                 frame.period_samples, frame.period_seconds * 1000.0, frame.repeat_fraction,
                 frame.floor_ratio, frame.strongest_lag, frame.harmonic_divisor);
    CAPTURE(frame.period_samples, frame.repeat_fraction, frame.strongest_lag,
            frame.harmonic_divisor, frame.refusal);
    REQUIRE(frame.found);
    REQUIRE(frame.period_samples == kFrameSamples);
    REQUIRE(frame.period_seconds ==
            Approx(static_cast<double>(kFrameSamples) / static_cast<double>(kRate))
                .epsilon(1e-9));
    REQUIRE(frame.repeat_fraction > 0.8);
}

// REJECTS: a frame reader that reports the strongest peak. A waveform that
// repeats twenty-five times over the buffer has a peak at every multiple
// of its period, and which of them comes out strongest is not a property
// of the framing. The assertion that makes this real is the one on
// harmonic_divisor: whichever multiple the scan landed on, the answer has
// to be the first repeat.
//
// Also rejects an OFDM reader with no frame reader beside it, which is the
// shape of characteriser that reports a 5120-sample OFDM symbol here.
TEST_CASE("the frame period is the first repeat, not the strongest", "[characterise]")
{
    std::println("test_periodicity frame walk: seed {}", kSeed + 2);
    const auto generated = framed_bpsk(kSeed + 2, 1U << 18);

    const auto profile = characterise::autocorrelation(analysis_of(generated.samples), kRate);
    REQUIRE(profile.has_value());

    const auto frame = characterise::find_frame_period(*profile);
    std::println("  frame {} samples, strongest lag {}, divisor {}, repeat {:.4f}",
                 frame.period_samples, frame.strongest_lag, frame.harmonic_divisor,
                 frame.repeat_fraction);
    CAPTURE(frame.period_samples, frame.strongest_lag, frame.harmonic_divisor, frame.refusal);
    REQUIRE(frame.found);
    REQUIRE(frame.period_samples == kFrameSamples);

    // Self-consistency: the scan either landed on the first repeat, or it
    // landed on a multiple of it and the walk divided by exactly that
    // multiple. There is no third answer that is right.
    REQUIRE(frame.strongest_lag == frame.period_samples * frame.harmonic_divisor);
}

// REJECTS: a frame reader that always finds a frame. This waveform's
// payload is longer than the buffer, so nothing in it comes round again,
// and the honest answer is no. The refusal has to name the strongest
// repeat it did see and the bar that repeat failed, because "no frame" and
// "a frame just under the bar" want different next moves from the caller
// and a bare false cannot tell them apart.
TEST_CASE("a waveform with no framing is refused, with the number", "[characterise]")
{
    siggen::ModulatorConfig common;
    common.rate = kRate;
    common.seed = kSeed + 3;
    std::println("test_periodicity unframed: seed {}", common.seed);

    siggen::PskParams psk;
    psk.symbol_rate = 2400.0;
    psk.rolloff = 0.35;
    psk.symbol_count = 8192;

    const auto generated = siggen::generate_bpsk(common, psk, 1U << 17);
    REQUIRE(generated.has_value());
    REQUIRE(generated->cycle_samples > (1U << 17));

    const auto profile = characterise::autocorrelation(analysis_of(generated->samples), kRate);
    REQUIRE(profile.has_value());

    const auto frame = characterise::find_frame_period(*profile);
    CAPTURE(frame.refusal, frame.floor_ratio);
    REQUIRE_FALSE(frame.found);
    REQUIRE(frame.refusal.find("comes round again") != std::string::npos);

    const auto ofdm = characterise::find_cyclic_prefix(*profile);
    CAPTURE(ofdm.refusal, ofdm.correlation);
    REQUIRE_FALSE(ofdm.found);
    // And not on the ceiling this time, which is the other half of the
    // distinction the framed case asserted. This waveform DOES correlate
    // with itself inside the OFDM lag range: a root raised cosine spanning
    // eight symbols correlates out to eight symbol periods, and that
    // ripple reads 0.129 at lag 32, sixteen times the profile's median.
    // What it is not is a repeat, because it is a slope reaching back
    // toward zero lag rather than an isolated maximum. The refusal names
    // both facts, since a reader shown only "not OFDM" beside a
    // correlation of 0.129 will not believe it.
    REQUIRE(ofdm.refusal.find("local maximum") != std::string::npos);
    REQUIRE(ofdm.refusal.find("tallest lag in the band") != std::string::npos);
}

// REJECTS: a claim about where the cyclic-prefix detector stops working
// that nobody measured. The table printed here is the answer, and the
// assertions pin both ends: it has to survive an SNR a real extract sees,
// and it has to refuse rather than invent a symbol period when it cannot.
//
// The failure mode a weak-signal case has to rule out is the interesting
// one. A correlation peak that drops under the bar makes the detector
// refuse, which is fine. A correlation peak that drops under the FLOOR
// while some other lag rises above it makes the detector confident and
// wrong, and that is what the per-point assertion on the lag catches.
TEST_CASE("the cyclic-prefix detector degrades measurably, not silently", "[characterise]")
{
    std::println("test_periodicity ofdm snr sweep: seed {}", kSeed + 4);
    const auto clean = ofdm_burst(kSeed + 4);
    REQUIRE_FALSE(clean.empty());

    double lowest_working_db = 1000.0;
    bool refused_somewhere = false;
    for (const double snr_db : {20.0, 10.0, 5.0, 0.0, -5.0, -10.0}) {
        auto noisy = clean;
        const auto report = siggen::add_awgn(dsp::ComplexSpan(noisy),
                                             siggen::NoiseLevel::snr_in_2500_hz_db(snr_db), kRate,
                                             siggen::derive_seed(kSeed + 4, 1));
        REQUIRE(report.has_value());

        const auto profile = characterise::autocorrelation(analysis_of(noisy), kRate);
        REQUIRE(profile.has_value());
        const auto ofdm = characterise::find_cyclic_prefix(*profile);
        std::println("  snr_2500 {:>6.1f} dB  found {}  useful {} samples  correlation {:.4f}  "
                     "floor {:.4f}",
                     snr_db, ofdm.found ? "yes" : " no", ofdm.symbol_samples, ofdm.correlation,
                     ofdm.floor_ratio);

        CAPTURE(snr_db, ofdm.symbol_samples, ofdm.correlation, ofdm.refusal);
        if (ofdm.found) {
            // Every answer it gives has to be the right one. An estimator
            // that keeps answering as the noise rises and starts answering
            // WRONG is worse than one that stops.
            REQUIRE(ofdm.symbol_samples == kUseful);
            lowest_working_db = std::min(lowest_working_db, snr_db);
        } else {
            refused_somewhere = true;
        }
    }

    // It has to reach at least 5 dB in 2500 Hz. This burst occupies 100 of
    // 256 subcarriers at 48 kS/s, so 18.75 kHz, and 5 dB in 2500 Hz is
    // 3.7 dB BELOW the noise in the signal's own bandwidth. The cyclic
    // prefix survives that because the correlation integrates over every
    // symbol in the extract and noise does not correlate with itself at a
    // lag of 256.
    CAPTURE(lowest_working_db);
    REQUIRE(lowest_working_db <= 5.0);
    // And it has to stop somewhere in the swept range rather than
    // answering all the way down, or the assertion above is vacuous.
    REQUIRE(refused_somewhere);
}

// ---------------------------------------------------------------------------
// The guard bar against the number of lags searched
// ---------------------------------------------------------------------------

namespace {

// Noise the size of the OFDM burst above, so a false alarm and a detection
// are read off profiles of the same length.
constexpr std::size_t kBarSamples = (kUseful + kPrefix) * kOfdmSymbols;

enum class BarNoise { Gaussian, Impulsive, Coloured };

[[nodiscard]] const char* bar_noise_name(BarNoise kind)
{
    switch (kind) {
        case BarNoise::Gaussian: return "gaussian";
        case BarNoise::Impulsive: return "impulsive";
        case BarNoise::Coloured: return "coloured";
    }
    return "?";
}

// Gaussian; Gaussian with an impulse of power 25 on one sample in a hundred,
// which puts the normalised power variance near the 4.8 empty 40 m reads; or
// Gaussian through a 101-tap Hann-windowed sinc at a tenth of the rate and
// not decimated, which is the oversampled noise docs/detection.md found the
// CLI's first narrowing produced.
[[nodiscard]] std::vector<dsp::Complex32> bar_noise(BarNoise kind, std::uint64_t seed)
{
    std::vector<dsp::Complex32> out = characterise_test::gaussian_noise(kBarSamples, 1.0, seed);
    if (kind == BarNoise::Impulsive) {
        std::mt19937_64 engine(siggen::derive_seed(seed, 7));
        for (dsp::Complex32& sample : out) {
            const std::uint64_t draw = engine();
            if (draw % 100 != 0) {
                continue;
            }
            const double phase = 2.0 * std::numbers::pi *
                                 static_cast<double>(draw >> 11) / 9007199254740992.0;
            sample += dsp::Complex32(static_cast<float>(5.0 * std::cos(phase)),
                                     static_cast<float>(5.0 * std::sin(phase)));
        }
    } else if (kind == BarNoise::Coloured) {
        constexpr int kHalf = 50;
        constexpr double kCutoff = 0.1;
        std::vector<double> taps(2 * kHalf + 1, 0.0);
        for (int n = -kHalf; n <= kHalf; ++n) {
            const double x = 2.0 * kCutoff * static_cast<double>(n);
            const double sinc =
                n == 0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
            const double window =
                0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n + kHalf) /
                                     static_cast<double>(2 * kHalf));
            taps[static_cast<std::size_t>(n + kHalf)] = sinc * window;
        }
        std::vector<dsp::Complex32> filtered(out.size());
        for (std::size_t i = 0; i < out.size(); ++i) {
            std::complex<double> acc(0.0, 0.0);
            for (int n = -kHalf; n <= kHalf; ++n) {
                const auto j = static_cast<std::ptrdiff_t>(i) + n;
                if (j < 0 || j >= static_cast<std::ptrdiff_t>(out.size())) {
                    continue;
                }
                const dsp::Complex32 v = out[static_cast<std::size_t>(j)];
                acc += taps[static_cast<std::size_t>(n + kHalf)] *
                       std::complex<double>(v.real(), v.imag());
            }
            filtered[i] = dsp::Complex32(static_cast<float>(acc.real()),
                                         static_cast<float>(acc.imag()));
        }
        out = std::move(filtered);
    }
    return out;
}

// The proposal docs/detection.md wrote down for task 29 and did not make: a
// bar that grows with the number of lags searched the way the largest of that
// many draws does. For a Rayleigh-distributed correlation magnitude the
// largest of L draws sits near sigma * sqrt(2 ln(L / alpha)) at a false-alarm
// probability alpha per search, and the median of one draw near
// sigma * sqrt(2 ln 2), so the multiple of the median is
// sqrt(ln(L / alpha) / ln 2). Anchored so it equals the shipped 6 at the
// default band's 8161 lags, 32 to 8192, which is alpha of 1.2e-7 per search.
// So it is 5.65 at 481 lags and 6.08 at 16353: a Gaussian tail grows that
// slowly.
constexpr double kShippedMultiple = 6.0;
constexpr double kAnchorLags = 8161.0;

[[nodiscard]] double extreme_value_multiple(double lags)
{
    return std::sqrt(kShippedMultiple * kShippedMultiple + std::log2(lags / kAnchorLags));
}

}  // namespace

// TASK 29, THE OWNER'S CALL, MEASURED AND NOT MADE.
//
// OfdmSearch::floor_multiple is a fixed six times the median of the searched
// lags, compared against the largest local maximum over all of them, and the
// largest of a set grows with the set. docs/detection.md argued that is why
// empty 40 m read OFDM at forty seconds and not at eleven, twenty or
// fifty-eight. This measures the argument: the false-alarm rate on three kinds
// of noise against the number of lags searched, under the shipped bar and
// under the extreme-value bar above, and the detection rate on the synthetic
// OFDM burst under both. Nothing here changes either constant.
//
// The lag count is moved by OfdmSearch::max_symbol_samples on one profile, so
// the extract length and everything else stay put and only the number of
// draws changes.
//
// WHAT IT MEASURED, 2026-09-22. No false alarm in any cell under either bar,
// and detections identical under both at every lag count: 10 of 10 at 10 dB
// in 2500 Hz, 8 at 5, 1 at 3, 0 at 0. The tallest lag over the median grows
// with the lag count, 3.15 to 3.86 typical on Gaussian noise from 481 lags to
// 16353, which is the size sqrt(ln L / ln 2) predicts, and never passes 4.48
// on any of the three noises. Empty real 40 m reached 6.0 at 8161 lags, where
// this bar is exactly the shipped 6, so the proposal would not have refused
// it. docs/detection.md has the table and the real extract lengths.
TEST_CASE("ofdm bar survey: false alarms and detections against lag count", "[.ofdm-bar]")
{
    constexpr std::array<std::size_t, 4> kHighs{512, 2048, 8192, 16384};
    constexpr std::uint64_t kTrials = 30;
    std::println("ofdm bar survey: seed {}, {} samples, {} trials a cell", kSeed + 20, kBarSamples,
                 kTrials);

    for (const BarNoise kind : {BarNoise::Gaussian, BarNoise::Impulsive, BarNoise::Coloured}) {
        std::array<std::size_t, kHighs.size()> shipped_alarms{};
        std::array<std::size_t, kHighs.size()> proposed_alarms{};
        std::array<std::vector<double>, kHighs.size()> spreads{};
        for (std::uint64_t trial = 0; trial < kTrials; ++trial) {
            const auto noise = bar_noise(kind, siggen::derive_seed(kSeed + 20, trial));
            const auto profile = characterise::autocorrelation(analysis_of(noise), kRate);
            REQUIRE(profile.has_value());
            for (std::size_t h = 0; h < kHighs.size(); ++h) {
                const double lags = static_cast<double>(kHighs[h] - 31);
                characterise::OfdmSearch shipped;
                shipped.max_symbol_samples = kHighs[h];
                characterise::OfdmSearch proposed = shipped;
                proposed.floor_multiple = extreme_value_multiple(lags);

                const auto a = characterise::find_cyclic_prefix(*profile, shipped);
                const auto b = characterise::find_cyclic_prefix(*profile, proposed);
                shipped_alarms[h] += a.found ? 1 : 0;
                proposed_alarms[h] += b.found ? 1 : 0;
                if (a.floor_ratio > 0.0) {
                    spreads[h].push_back(a.correlation / a.floor_ratio);
                }
            }
        }
        for (std::size_t h = 0; h < kHighs.size(); ++h) {
            std::sort(spreads[h].begin(), spreads[h].end());
            const double lags = static_cast<double>(kHighs[h] - 31);
            std::println("  {:9} {:5.0f} lags: false alarms {:2} of {} at 6.00, {:2} at {:.2f}; "
                         "tallest over median {:.2f} median, {:.2f} worst",
                         bar_noise_name(kind), lags, shipped_alarms[h], kTrials,
                         proposed_alarms[h], extreme_value_multiple(lags),
                         spreads[h].empty() ? 0.0 : spreads[h][spreads[h].size() / 2],
                         spreads[h].empty() ? 0.0 : spreads[h].back());
        }
    }

    const auto clean = ofdm_burst(kSeed + 21);
    REQUIRE_FALSE(clean.empty());
    for (const double snr_db : {10.0, 5.0, 3.0, 0.0}) {
        std::array<std::size_t, kHighs.size()> shipped_hits{};
        std::array<std::size_t, kHighs.size()> proposed_hits{};
        constexpr std::uint64_t kBursts = 10;
        for (std::uint64_t trial = 0; trial < kBursts; ++trial) {
            auto noisy = clean;
            REQUIRE(siggen::add_awgn(dsp::ComplexSpan(noisy),
                                     siggen::NoiseLevel::snr_in_2500_hz_db(snr_db), kRate,
                                     siggen::derive_seed(kSeed + 21, 100 + trial))
                        .has_value());
            const auto profile = characterise::autocorrelation(analysis_of(noisy), kRate);
            REQUIRE(profile.has_value());
            for (std::size_t h = 0; h < kHighs.size(); ++h) {
                characterise::OfdmSearch shipped;
                shipped.max_symbol_samples = kHighs[h];
                characterise::OfdmSearch proposed = shipped;
                proposed.floor_multiple =
                    extreme_value_multiple(static_cast<double>(kHighs[h] - 31));
                const auto a = characterise::find_cyclic_prefix(*profile, shipped);
                const auto b = characterise::find_cyclic_prefix(*profile, proposed);
                shipped_hits[h] += (a.found && a.symbol_samples == kUseful) ? 1 : 0;
                proposed_hits[h] += (b.found && b.symbol_samples == kUseful) ? 1 : 0;
            }
        }
        std::string row = std::format("  ofdm {:4.1f} dB:", snr_db);
        for (std::size_t h = 0; h < kHighs.size(); ++h) {
            row += std::format("  {} lags {}/{} and {}/{}", kHighs[h] - 31, shipped_hits[h],
                               kBursts, proposed_hits[h], kBursts);
        }
        std::println("{}", row);
    }

    CHECK(kBarSamples == 172800);
}
