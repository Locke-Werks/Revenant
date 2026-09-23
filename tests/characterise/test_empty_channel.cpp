// What the characteriser says about a channel with nothing in it, and what a
// PSK call with no symbol rate is worth.
//
// docs/detection.md measured both on real 40 m and 20 m. Empty band, narrowed
// by the CLI's low pass, came back 2-PSK at 0.67 and 0.81 with no symbol rate
// and a carrier just past the filter's own cutoff; a weak carrier buried in
// noise came back PSK at 0.98. The document's standing line is that anything
// feeding core/characterise a tighter extract should be able to show, on a
// channel with nothing in it, that the answer stays unknown. This file is that
// test, and the measurement it rests on.
//
// The noise is built here rather than recorded, so every case knows there is
// nothing in it. Two kinds: circularly symmetric Gaussian, which is what every
// threshold in core/characterise was stated against, and the same with sparse
// impulses mixed in at a level that puts the normalised power variance near
// the 4.79 and 4.82 empty 40 m read, because docs/detection.md found that real
// HF noise is not Gaussian and the difference moved the answers.
//
// The narrowing is the CLI's own, copied rather than called because it lives
// in the executable: a 101-tap Hann-windowed sinc cutting at half the asked
// width, then decimation by the whole number that leaves about three times the
// width, never below the stage's sample floor.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <numbers>
#include <print>
#include <random>
#include <string>
#include <vector>

#include "core/characterise/characterise.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/modulators.h"
#include "tests/characterise/signal_lab.h"

using namespace revenant;
using characterise::ModulationFamily;

namespace {

constexpr std::uint64_t kSeed = 20260922;

// The HF coarse channel rate docs/detection.md measured through: 96 kS/s over
// 64 channels.
constexpr dsp::SampleRate kChannelRate = 3000;

enum class NoiseKind { Gaussian, Impulsive };

[[nodiscard]] const char* noise_name(NoiseKind kind)
{
    return kind == NoiseKind::Gaussian ? "gaussian" : "impulsive";
}

// Gaussian noise of unit power, plus, for the impulsive kind, an impulse of
// power 25 on one sample in a hundred at a random phase. That mixture has a
// normalised power variance of 4.92 by arithmetic, (2 + 1 + 6.25) / 1.25^2 - 1,
// against the 4.79 and 4.82 two empty 40 m channels read.
[[nodiscard]] std::vector<dsp::Complex32> empty_channel(NoiseKind kind, std::size_t count,
                                                        double level, std::uint64_t seed)
{
    std::vector<dsp::Complex32> out = characterise_test::gaussian_noise(count, level, seed);
    if (kind == NoiseKind::Impulsive) {
        std::mt19937_64 engine(siggen::derive_seed(seed, 7));
        const double amplitude = 5.0 * std::sqrt(level);
        for (dsp::Complex32& sample : out) {
            const std::uint64_t draw = engine();
            // One in a hundred from the low bits, the phase from the high.
            if (draw % 100 != 0) {
                continue;
            }
            const double phase = 2.0 * std::numbers::pi *
                                 static_cast<double>(draw >> 11) / 9007199254740992.0;
            sample += dsp::Complex32(static_cast<float>(amplitude * std::cos(phase)),
                                     static_cast<float>(amplitude * std::sin(phase)));
        }
    }
    return out;
}

// The CLI's --characterise-width, as tools/cli/main.cpp does it. Zero width
// hands the extract back unchanged.
[[nodiscard]] std::vector<dsp::Complex32> narrowed(const std::vector<dsp::Complex32>& extract,
                                                   dsp::SampleRate rate, double width_hz,
                                                   dsp::SampleRate& out_rate)
{
    out_rate = rate;
    if (width_hz <= 0.0) {
        return extract;
    }
    auto decimation = static_cast<std::size_t>(
        std::floor(static_cast<double>(rate) / (3.0 * width_hz)));
    decimation = std::max<std::size_t>(decimation, 1);
    while (decimation > 1 &&
           extract.size() / decimation < characterise::kMinCharacteriseSamples) {
        --decimation;
    }
    const double survives = static_cast<double>(rate) / static_cast<double>(decimation);
    const double cutoff = std::min(0.5 * width_hz, 0.45 * survives);
    const double normalised = cutoff / static_cast<double>(rate);

    constexpr int kHalf = 50;
    std::vector<double> taps(2 * kHalf + 1, 0.0);
    double sum = 0.0;
    for (int n = -kHalf; n <= kHalf; ++n) {
        const double x = 2.0 * normalised * static_cast<double>(n);
        const double sinc =
            n == 0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
        const double window =
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n + kHalf) /
                                 static_cast<double>(2 * kHalf));
        taps[static_cast<std::size_t>(n + kHalf)] = sinc * window;
        sum += sinc * window;
    }
    for (double& tap : taps) {
        tap /= sum;
    }

    std::vector<dsp::Complex32> out;
    out.reserve(extract.size() / decimation + 1);
    for (std::size_t i = 0; i < extract.size(); i += decimation) {
        std::complex<double> acc(0.0, 0.0);
        for (int n = -kHalf; n <= kHalf; ++n) {
            const auto j = static_cast<std::ptrdiff_t>(i) + n;
            if (j < 0 || j >= static_cast<std::ptrdiff_t>(extract.size())) {
                continue;
            }
            const dsp::Complex32 x = extract[static_cast<std::size_t>(j)];
            acc += taps[static_cast<std::size_t>(n + kHalf)] *
                   std::complex<double>(x.real(), x.imag());
        }
        out.emplace_back(static_cast<float>(acc.real()), static_cast<float>(acc.imag()));
    }
    out_rate = static_cast<dsp::SampleRate>(std::llround(survives));
    return out;
}

[[nodiscard]] characterise::CharacteriseConfig config_at(dsp::SampleRate rate)
{
    characterise::CharacteriseConfig out;
    out.rate = rate;
    return out;
}

struct Answer {
    ModulationFamily family = ModulationFamily::Unknown;
    double confidence = 0.0;
    bool rate_found = false;
    double carrier_hz = 0.0;
    double band_low = 0.0;
    double band_high = 0.0;
    bool band_found = false;
    double order_margin_db = 0.0;
    int order = 0;
    double carrier_level = -1.0;
};

[[nodiscard]] Answer answer_of(const characterise::Characterisation& result)
{
    Answer out;
    out.family = result.family;
    out.confidence = result.family_confidence;
    out.rate_found = result.symbol_rate.found;
    out.carrier_hz = result.order.carrier_offset_hz;
    out.band_low = result.band.low_hz;
    out.band_high = result.band.high_hz;
    out.band_found = result.band.found;
    out.order_margin_db = result.order.margin_db;
    out.order = result.order.order;
    out.carrier_level = result.psk_carrier_level;
    return out;
}

// One empty channel of the given kind, length, narrowing and level,
// characterised the way the CLI would. The seed is a function of every axis
// but the level, so the two levels of one cell are the same noise scaled and
// the level control is exact.
[[nodiscard]] characterise::Characterisation characterise_empty(NoiseKind kind, double seconds,
                                                               double width, double level,
                                                               std::uint64_t trial)
{
    const auto count = static_cast<std::size_t>(seconds * static_cast<double>(kChannelRate));
    const std::uint64_t domain = 1000 * trial + static_cast<std::uint64_t>(width) +
                                 7 * static_cast<std::uint64_t>(seconds) +
                                 (kind == NoiseKind::Impulsive ? 500000 : 0);
    const auto extract = empty_channel(kind, count, level, siggen::derive_seed(kSeed, domain));
    dsp::SampleRate rate = 0;
    const auto input = narrowed(extract, kChannelRate, width, rate);
    auto result = characterise::characterise(dsp::ConstComplexSpan(input), config_at(rate));
    REQUIRE(result.has_value());
    return *result;
}

}  // namespace

// ---------------------------------------------------------------------------
// Surveys
// ---------------------------------------------------------------------------

// HOW OFTEN AN EMPTY CHANNEL IS GIVEN A FAMILY, across the axes docs/
// detection.md found moving the answer: the kind of noise, the width the CLI
// narrows to, the extract length and the level. The level is there as a
// control, because every statistic the stage uses is normalised and a result
// that moved with it would be a defect of its own.
TEST_CASE("empty survey: what an empty channel is called", "[.empty-survey]")
{
    constexpr std::array<double, 4> kWidths{0.0, 800.0, 300.0, 100.0};
    constexpr std::array<double, 2> kSeconds{20.0, 55.0};
    constexpr std::array<double, 2> kLevels{1.0e-4, 1.0e4};
    constexpr std::uint64_t kTrials = 3;

    std::println("empty survey: seed {}", kSeed);
    std::size_t total = 0;
    std::size_t named = 0;
    std::size_t lines = 0;
    double loudest_line = 0.0;
    for (const NoiseKind kind : {NoiseKind::Gaussian, NoiseKind::Impulsive}) {
        for (const double seconds : kSeconds) {
            for (const double width : kWidths) {
                std::map<std::string, std::size_t> families;
                std::size_t cell_named = 0;
                std::size_t cell_total = 0;
                std::string details;
                for (const double level : kLevels) {
                    for (std::uint64_t trial = 0; trial < kTrials; ++trial) {
                        const Answer answer =
                            answer_of(characterise_empty(kind, seconds, width, level, trial));
                        ++cell_total;
                        ++families[std::string(characterise::modulation_family_name(answer.family))];
                        if (answer.family != ModulationFamily::Unknown) {
                            ++cell_named;
                        }
                        // Every order line, named or set aside, with the power
                        // the carrier it named sits on.
                        if (answer.order > 0) {
                            ++lines;
                            loudest_line = std::max(loudest_line, answer.carrier_level);
                            details += std::format(
                                " [{} {:.2f}{} order {} carrier {:+.1f} Hz band {:.1f}..{:.1f} "
                                "margin {:.1f} dB, carrier on {:.3f} of the band's median, "
                                "level {:g}]",
                                characterise::modulation_family_name(answer.family),
                                answer.confidence, answer.rate_found ? " rate" : " no-rate",
                                answer.order, answer.carrier_hz, answer.band_low,
                                answer.band_high, answer.order_margin_db, answer.carrier_level,
                                level);
                        }
                    }
                }
                total += cell_total;
                named += cell_named;
                std::string tally;
                for (const auto& [name, count] : families) {
                    tally += std::format(" {} {}", name, count);
                }
                std::println("  {:9} {:4.0f} s width {:4.0f}: {} of {} named;{}{}",
                             noise_name(kind), seconds, width, cell_named, cell_total, tally,
                             details);
            }
        }
    }
    std::println("  {} of {} empty channels given a family; {} order lines, the carrier on at "
                 "most {:.3f} of the band's median power",
                 named, total, lines, loudest_line);
    CHECK(total > 0);
}

// WHAT A PSK CALL WITH NO SYMBOL RATE LOOKS LIKE against one with a rate, so
// that a cap on its confidence is read off margins rather than chosen.
//
// Three populations. Real BPSK and QPSK walked down in SNR, which is what a
// genuine PSK call looks like as it weakens. A carrier walked down in noise,
// which is the known artefact: squaring a tone gives a tone. And the empty
// channels above, narrowed, which is the other artefact.
TEST_CASE("psk survey: calls with and without a symbol rate", "[.psk-survey]")
{
    constexpr dsp::SampleRate kRate = 48000;
    constexpr std::size_t kSamples = 1U << 17;
    std::println("psk survey: seed {}", kSeed);

    const auto line = [](const char* what, const characterise::Characterisation& result) {
        std::println("  {:28} {} {:.3f} {} order {} margin {:.1f} dB, carrier {:+.1f} Hz in "
                     "{:.1f}..{:.1f}",
                     what, characterise::modulation_family_name(result.family),
                     result.family_confidence,
                     result.symbol_rate.found
                         ? std::format("at {:.1f} baud", result.symbol_rate.symbol_rate_hz)
                         : std::string("NO RATE"),
                     result.order.order, result.order.margin_db, result.order.carrier_offset_hz,
                     result.band.low_hz, result.band.high_hz);
    };

    for (const bool qpsk : {false, true}) {
        for (const double snr : {30.0, 20.0, 10.0, 5.0, 0.0, -5.0}) {
            siggen::ModulatorConfig common;
            common.rate = kRate;
            common.seed = kSeed + (qpsk ? 100 : 0) + static_cast<std::uint64_t>(snr + 10);
            siggen::PskParams psk;
            psk.symbol_rate = 2400.0;
            psk.rolloff = 0.35;
            psk.symbol_count = 8192;
            auto generated = qpsk ? siggen::generate_qpsk(common, psk, kSamples)
                                  : siggen::generate_bpsk(common, psk, kSamples);
            REQUIRE(generated.has_value());
            REQUIRE(siggen::add_awgn(dsp::ComplexSpan(generated->samples),
                                     siggen::NoiseLevel::snr_in_2500_hz_db(snr), kRate,
                                     siggen::derive_seed(common.seed, 1))
                        .has_value());
            const auto result = characterise::characterise(
                dsp::ConstComplexSpan(generated->samples), config_at(kRate));
            REQUIRE(result.has_value());
            line(std::format("{} at {:.0f} dB in 2500 Hz", qpsk ? "qpsk" : "bpsk", snr).c_str(),
                 *result);
        }
    }

    for (const double variance : {0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0}) {
        for (std::uint64_t trial = 0; trial < 3; ++trial) {
            auto samples = characterise_test::pure_tone(kSamples, kRate, 1200, 1.0);
            const auto noise =
                characterise_test::gaussian_noise(kSamples, variance, kSeed + 17 * trial + 3);
            for (std::size_t n = 0; n < samples.size(); ++n) {
                samples[n] += noise[n];
            }
            const auto result =
                characterise::characterise(dsp::ConstComplexSpan(samples), config_at(kRate));
            REQUIRE(result.has_value());
            line(std::format("carrier at {:+.1f} dB, trial {}", -10.0 * std::log10(variance),
                             trial)
                     .c_str(),
                 *result);
        }
    }

    CHECK(kSamples > 0);
}

// HOW MUCH POWER A REAL PSK CARRIER SITS ON, which is what the rule in
// CharacteriseConfig::psk_carrier_level_fraction is measured against. The
// artefacts in the empty survey name a carrier where the band's power has
// fallen away; this is the other side of that bar, across offsets and noise,
// for both orders.
TEST_CASE("psk survey: where a real carrier sits in its band", "[.psk-survey]")
{
    constexpr dsp::SampleRate kRate = 48000;
    constexpr std::size_t kSamples = 1U << 17;
    std::println("psk band survey: seed {}", kSeed + 60);

    double weakest = 1.0e300;
    for (const bool qpsk : {false, true}) {
        for (const dsp::Hertz offset : {0, 3000, -7000}) {
            for (const double snr : {30.0, 20.0, 10.0, 5.0, 0.0}) {
                siggen::ModulatorConfig common;
                common.rate = kRate;
                common.carrier_offset = offset;
                common.seed = kSeed + 60 + static_cast<std::uint64_t>(snr) +
                              (qpsk ? 1000 : 0) + static_cast<std::uint64_t>(offset + 10000);
                siggen::PskParams psk;
                psk.symbol_rate = 2400.0;
                psk.rolloff = 0.35;
                psk.symbol_count = 8192;
                auto generated = qpsk ? siggen::generate_qpsk(common, psk, kSamples)
                                      : siggen::generate_bpsk(common, psk, kSamples);
                REQUIRE(generated.has_value());
                REQUIRE(siggen::add_awgn(dsp::ComplexSpan(generated->samples),
                                         siggen::NoiseLevel::snr_in_2500_hz_db(snr), kRate,
                                         siggen::derive_seed(common.seed, 1))
                            .has_value());
                const auto result = characterise::characterise(
                    dsp::ConstComplexSpan(generated->samples), config_at(kRate));
                REQUIRE(result.has_value());
                const double level = result->psk_carrier_level;
                if (result->order.found) {
                    weakest = std::min(weakest, level);
                }
                std::println("  {} {:+6} Hz {:4.0f} dB: {} {:.3f}{}, order {} carrier {:+.1f} in "
                             "{:.1f}..{:.1f}, carrier on {:.2f} of the band's median power",
                             qpsk ? "qpsk" : "bpsk", offset, snr,
                             characterise::modulation_family_name(result->family),
                             result->family_confidence,
                             result->symbol_rate.found ? "" : " no rate", result->order.order,
                             result->order.carrier_offset_hz, result->band.low_hz,
                             result->band.high_hz, level);
            }
        }
    }
    std::println("  the weakest carrier a found order named sat on {:.2f} of its band's median",
                 weakest);
    CHECK(weakest > 0.0);
}

// WHAT THE RULES DO TO RTTY, which lane 3 found called 4-PSK at 0.92 at 8 dB
// in 2500 Hz through the CLI's HF path. 45.45 baud and 170 Hz shift, made
// straight at the 3 kS/s channel rate, 20 seconds, at the two levels it
// measured and one between.
TEST_CASE("psk survey: RTTY through the family rules", "[.psk-survey]")
{
    std::println("psk rtty survey: seed {}", kSeed + 80);
    for (const double snr : {15.0, 11.0, 8.0}) {
        for (std::uint64_t trial = 0; trial < 3; ++trial) {
            siggen::ModulatorConfig common;
            common.rate = kChannelRate;
            common.seed = kSeed + 80 + 10 * trial + static_cast<std::uint64_t>(snr);
            siggen::Fsk2Params fsk;
            fsk.symbol_rate = 45.45;
            fsk.deviation = 85;
            fsk.symbol_count = 1024;
            auto generated = siggen::generate_fsk2(common, fsk, 60000);
            REQUIRE(generated.has_value());
            REQUIRE(siggen::add_awgn(dsp::ComplexSpan(generated->samples),
                                     siggen::NoiseLevel::snr_in_2500_hz_db(snr), kChannelRate,
                                     siggen::derive_seed(common.seed, 1))
                        .has_value());
            const auto result = characterise::characterise(
                dsp::ConstComplexSpan(generated->samples), config_at(kChannelRate));
            REQUIRE(result.has_value());
            std::println("  rtty {:4.1f} dB trial {}: {} {:.3f}{}{}, order {} margin {:.1f} dB, "
                         "carrier on {:.2f} of the band's median, tones {}",
                         snr, trial, characterise::modulation_family_name(result->family),
                         result->family_confidence,
                         result->symbol_rate.found ? "" : " no rate",
                         result->psk_without_symbol_rate ? " flagged" : "", result->order.order,
                         result->order.margin_db, result->psk_carrier_level,
                         result->tones.found ? result->tones.tone_count : 0);
        }
    }
    CHECK(kChannelRate > 0);
}

// ---------------------------------------------------------------------------
// A PSK call with no symbol rate: kept, flagged, capped, excluded
// ---------------------------------------------------------------------------

namespace {

constexpr dsp::SampleRate kLabRate = 48000;
constexpr std::size_t kLabSamples = 1U << 17;

// BPSK at 2400 baud with noise at a stated SNR in 2500 Hz. The psk survey
// above measured 0 dB as the level where the order is still found and the
// rate is not.
[[nodiscard]] characterise::Characterisation bpsk_at(double snr_db, std::uint64_t seed)
{
    siggen::ModulatorConfig common;
    common.rate = kLabRate;
    common.seed = seed;
    siggen::PskParams psk;
    psk.symbol_rate = 2400.0;
    psk.rolloff = 0.35;
    psk.symbol_count = 8192;
    auto generated = siggen::generate_bpsk(common, psk, kLabSamples);
    REQUIRE(generated.has_value());
    if (std::isfinite(snr_db)) {
        REQUIRE(siggen::add_awgn(dsp::ComplexSpan(generated->samples),
                                 siggen::NoiseLevel::snr_in_2500_hz_db(snr_db), kLabRate,
                                 siggen::derive_seed(seed, 1))
                    .has_value());
    }
    auto result =
        characterise::characterise(dsp::ConstComplexSpan(generated->samples), config_at(kLabRate));
    REQUIRE(result.has_value());
    return *result;
}

// A 1200 Hz carrier at unit amplitude under noise of the given total power.
[[nodiscard]] characterise::Characterisation carrier_under(double noise_power,
                                                          std::uint64_t seed)
{
    auto samples = characterise_test::pure_tone(kLabSamples, kLabRate, 1200, 1.0);
    const auto noise = characterise_test::gaussian_noise(kLabSamples, noise_power, seed);
    for (std::size_t n = 0; n < samples.size(); ++n) {
        samples[n] += noise[n];
    }
    auto result = characterise::characterise(dsp::ConstComplexSpan(samples), config_at(kLabRate));
    REQUIRE(result.has_value());
    return *result;
}

}  // namespace

// REJECTS: a stage that refuses a PSK call for lacking a rate, which throws
// away real signals, and one that reports it at the confidence the order line
// alone would give it, which is how a carrier in noise came back PSK at 0.98.
//
// Two signals, both called PSK with no rate: a real BPSK signal at 0 dB in
// 2500 Hz, and a carrier 3 dB under its noise. The first is right and the
// second is the artefact, and nothing in the call separates them, so both are
// kept, both are flagged, both are held to the cap and neither may drive
// detection.
TEST_CASE("a PSK call with no symbol rate is kept, flagged, capped and excluded",
          "[characterise]")
{
    std::println("test_empty_channel psk without rate: seed {}", kSeed + 40);
    const characterise::Characterisation weak = bpsk_at(0.0, kSeed + 40);
    const characterise::Characterisation carrier = carrier_under(2.0, kSeed + 41);

    for (const characterise::Characterisation* result : {&weak, &carrier}) {
        CAPTURE(result->summary, result->order.margin_db);

        // Kept.
        REQUIRE(result->family == ModulationFamily::Psk);
        REQUIRE_FALSE(result->refused);
        REQUIRE_FALSE(result->symbol_rate.found);

        // Flagged, and the summary an operator reads says why.
        CHECK(result->psk_without_symbol_rate);
        CHECK(result->summary.find("No symbol rate was measured") != std::string::npos);
        CHECK(result->summary.find("not to be used to drive detection") != std::string::npos);

        // Capped. Both measured well over it uncapped: the order margin alone
        // gives the carrier 0.98.
        CHECK(result->family_confidence <= characterise::kPskWithoutRateConfidence);
        CHECK(result->order.confidence > characterise::kPskWithoutRateConfidence);

        // Excluded.
        CHECK_FALSE(characterise::may_drive_detection(*result));
    }
}

// REJECTS: a flag or a cap that lands on PSK calls that did find their rate,
// and a gate that refuses everything. The same signal clean is PSK at its
// rate, unflagged, at the confidence its margin earned, and may drive
// detection; an empty channel may not.
TEST_CASE("a PSK call with its rate is untouched, and unknown is excluded", "[characterise]")
{
    std::println("test_empty_channel psk with rate: seed {}", kSeed + 42);
    const characterise::Characterisation clean =
        bpsk_at(std::numeric_limits<double>::infinity(), kSeed + 42);
    CAPTURE(clean.summary);
    REQUIRE(clean.family == ModulationFamily::Psk);
    REQUIRE(clean.symbol_rate.found);
    CHECK_FALSE(clean.psk_without_symbol_rate);
    CHECK(clean.family_confidence == clean.order.confidence);
    CHECK(clean.family_confidence > characterise::kPskWithoutRateConfidence);
    CHECK(characterise::may_drive_detection(clean));

    const auto noise = characterise_test::gaussian_noise(kLabSamples, 1.0, kSeed + 43);
    const auto empty =
        characterise::characterise(dsp::ConstComplexSpan(noise), config_at(kLabRate));
    REQUIRE(empty.has_value());
    REQUIRE(empty->family == ModulationFamily::Unknown);
    CHECK_FALSE(characterise::may_drive_detection(*empty));
}

// ---------------------------------------------------------------------------
// The empty-channel gate
// ---------------------------------------------------------------------------

// REJECTS: a characteriser that names a family on a channel with nothing in
// it once the extract has been narrowed, which is what docs/detection.md
// found on real 40 m and what the empty survey above reproduced on synthetic
// noise: before the carrier-power rule, 62 of 96 narrowed or unfiltered empty
// channels came back PSK, at up to 0.98 and 22 of them with a symbol rate,
// every one naming a carrier at the band's edge.
//
// A slice of that survey, both kinds of noise at the three widths the CLI was
// measured at and unfiltered, two seeds each. Every one has to be Unknown,
// and where the power law found a line the refusal has to say it was set
// aside because the band's power had fallen away under its carrier.
TEST_CASE("an empty channel stays unknown however the extract is narrowed", "[characterise]")
{
    constexpr std::array<double, 4> kWidths{0.0, 800.0, 300.0, 100.0};
    std::println("test_empty_channel gate: seed {}", kSeed);

    std::size_t set_aside = 0;
    for (const NoiseKind kind : {NoiseKind::Gaussian, NoiseKind::Impulsive}) {
        for (const double width : kWidths) {
            for (std::uint64_t trial = 0; trial < 2; ++trial) {
                const auto result = characterise_empty(kind, 20.0, width, 1.0, trial);
                CAPTURE(noise_name(kind), width, trial, result.summary,
                        result.psk_carrier_level);
                CHECK(result.family == ModulationFamily::Unknown);
                CHECK_FALSE(characterise::may_drive_detection(result));

                // Whatever a cyclic detector reports, it reports inside the
                // band it searched. 20 m at 1603 UT came back with two PSK
                // calls "at 0.00 baud", a found rate at DC.
                for (const characterise::SymbolRateEstimate* rate :
                     {&result.squared_envelope, &result.frequency_transition}) {
                    if (rate->found) {
                        CAPTURE(rate->symbol_rate_hz, rate->searched_low_hz);
                        CHECK(rate->symbol_rate_hz >= rate->searched_low_hz);
                    }
                }
                if (result.psk_carrier_outside_band) {
                    ++set_aside;
                    CHECK(result.refusal.find("So no PSK call was made from it") !=
                          std::string::npos);
                }
            }
        }
    }

    // The rule has to be what did it: without it most of these are PSK, so
    // a gate that passed with the rule never firing would be passing on
    // something else.
    CHECK(set_aside >= 8);
}

// REJECTS: a carrier rule that costs real signals, which the first form of
// it did. BPSK and QPSK at -7 kHz and 20 dB in 2500 Hz: the BPSK one's
// occupied band comes back centred at +6 kHz because noise fills the extract,
// which a rule on distance from the band's centre refused, and the QPSK one's
// carrier is reported at +5 kHz, an alias rate/4 from the real one. The level
// is read at the loudest alias, so both have to stay PSK.
TEST_CASE("the carrier rule keeps an off-centre carrier and an aliased one", "[characterise]")
{
    constexpr dsp::SampleRate kRate = 48000;
    constexpr std::size_t kSamples = 1U << 17;
    std::println("test_empty_channel off centre: seed {}", kSeed + 70);

    for (const bool qpsk : {false, true}) {
        siggen::ModulatorConfig common;
        common.rate = kRate;
        common.carrier_offset = -7000;
        common.seed = kSeed + 70 + (qpsk ? 1 : 0);
        siggen::PskParams psk;
        psk.symbol_rate = 2400.0;
        psk.rolloff = 0.35;
        psk.symbol_count = 8192;
        auto generated = qpsk ? siggen::generate_qpsk(common, psk, kSamples)
                              : siggen::generate_bpsk(common, psk, kSamples);
        REQUIRE(generated.has_value());
        REQUIRE(siggen::add_awgn(dsp::ComplexSpan(generated->samples),
                                 siggen::NoiseLevel::snr_in_2500_hz_db(20.0), kRate,
                                 siggen::derive_seed(common.seed, 1))
                    .has_value());
        const auto result = characterise::characterise(
            dsp::ConstComplexSpan(generated->samples), config_at(kRate));
        REQUIRE(result.has_value());
        CAPTURE(qpsk, result->summary, result->order.carrier_offset_hz,
                result->psk_carrier_level);
        CHECK(result->family == ModulationFamily::Psk);
        CHECK_FALSE(result->psk_carrier_outside_band);
        CHECK(result->psk_carrier_level > 1.0);
    }
}
