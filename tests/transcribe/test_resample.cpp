// core/transcribe/resample.h: every rate a receiver produces, to 16000 S/s.
//
// Every signal here is generated in this file, from a fixed seed where it is
// random. The tolerances are set from the filter's design, 80 dB of stopband
// and a passband ripple of the same order (10^(-80/20) = 1e-4 of the
// amplitude), with room for float rounding; the hidden case at the bottom
// measures both properly and prints them.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "core/transcribe/resample.h"

namespace {

using revenant::transcribe::kRecogniserRateHz;
using revenant::transcribe::Resampler;

constexpr std::uint64_t kSeed = 20261003;

std::vector<float> tone(std::uint32_t rate, double hz, double amplitude, double seconds)
{
    const auto n = static_cast<std::size_t>(std::llround(seconds * rate));
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate));
    }
    return x;
}

std::vector<float> noise(std::size_t n, std::uint64_t seed)
{
    std::mt19937_64 rng(seed);
    std::normal_distribution<float> dist(0.0F, 0.3F);
    std::vector<float> x(n);
    for (float& v : x) {
        v = dist(rng);
    }
    return x;
}

std::vector<float> whole(std::uint32_t rate, std::span<const float> in)
{
    Resampler r(rate);
    std::vector<float> out;
    r.process(in, out);
    r.finish(out);
    return out;
}

std::vector<float> pieces(std::uint32_t rate, std::span<const float> in, std::uint64_t seed, std::size_t largest)
{
    Resampler r(rate);
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<std::size_t> size(1, largest);
    std::vector<float> out;
    std::size_t at = 0;
    while (at < in.size()) {
        const std::size_t n = std::min(size(rng), in.size() - at);
        r.process(in.subspan(at, n), out);
        at += n;
    }
    r.finish(out);
    return out;
}

std::size_t owed(std::size_t inputs, std::uint32_t rate)
{
    return static_cast<std::size_t>((static_cast<std::uint64_t>(inputs) * kRecogniserRateHz + rate - 1) / rate);
}

// Largest difference from the ideal 16 kHz sinusoid, away from the two ends
// where the stream starts and stops abruptly.
double worst_error(std::span<const float> out, double hz, double amplitude)
{
    const std::size_t margin = kRecogniserRateHz / 20;
    double worst = 0.0;
    for (std::size_t n = margin; n + margin < out.size(); ++n) {
        const double ideal =
            amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / kRecogniserRateHz);
        worst = std::max(worst, std::abs(out[n] - ideal));
    }
    return worst;
}

// RMS of the output away from the ends, in dB against an input sinusoid's.
double residual_db(std::span<const float> out, double amplitude)
{
    const std::size_t margin = kRecogniserRateHz / 20;
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t n = margin; n + margin < out.size(); ++n) {
        sum += static_cast<double>(out[n]) * out[n];
        ++count;
    }
    const double rms = std::sqrt(sum / static_cast<double>(count));
    return 20.0 * std::log10(std::max(rms, 1e-30) / (amplitude / std::numbers::sqrt2));
}

// The amplitude of the component at `hz` in the output, by projection, in dB
// against `amplitude`.
double gain_db(std::span<const float> out, double hz, double amplitude)
{
    const std::size_t margin = kRecogniserRateHz / 20;
    double s = 0.0;
    double c = 0.0;
    std::size_t count = 0;
    for (std::size_t n = margin; n + margin < out.size(); ++n) {
        const double w = 2.0 * std::numbers::pi * hz * static_cast<double>(n) / kRecogniserRateHz;
        s += out[n] * std::sin(w);
        c += out[n] * std::cos(w);
        ++count;
    }
    const double measured = 2.0 * std::sqrt(s * s + c * c) / static_cast<double>(count);
    return 20.0 * std::log10(measured / amplitude);
}

}  // namespace

TEST_CASE("a passband sinusoid comes out at its amplitude, frequency and time", "[transcribe][resample]") {
    // REJECTS: a ratio taken the wrong way up or rounded (the frequency
    // moves and the error grows to the full amplitude within a second), a
    // filter with gain other than one in the passband, a group delay left in
    // the output (a 1 kHz tone a single 16 kHz sample late is 0.39 of its
    // amplitude out), images left in when going up from 8000, and the
    // interpolated phase path at 22050 drifting from the exact one.
    const std::uint32_t rate = GENERATE(8000U, 24000U, 48000U, 44100U, 171000U, 22050U);
    Resampler probe(rate);
    const double edge = probe.passband_hz();
    for (const double hz : {1000.0, 0.9 * edge}) {
        constexpr double kAmplitude = 0.5;
        const auto in = tone(rate, hz, kAmplitude, 1.0);
        const auto out = whole(rate, in);
        INFO("rate " << rate << ", tone " << hz << " Hz, " << probe.taps() << " taps, "
                     << probe.phases() << (probe.exact_phases() ? " exact" : " interpolated") << " phases");
        REQUIRE(out.size() == owed(in.size(), rate));
        const double worst = worst_error(out, hz, kAmplitude);
        INFO("worst error " << 20.0 * std::log10(worst / kAmplitude) << " dB of the amplitude");
        CHECK(worst < kAmplitude * 1e-3);
    }
}

TEST_CASE("a tone above 8 kHz does not fold back into the band going down", "[transcribe][resample]") {
    // REJECTS: decimation with no anti-alias filter, or one whose stopband
    // starts above the output Nyquist, where 8.5 kHz folds to 7.5 kHz at
    // full amplitude, and a filter cut off too gently to reach the design's
    // 80 dB.
    const std::uint32_t rate = GENERATE(24000U, 44100U, 48000U, 171000U, 192000U);
    constexpr double kAmplitude = 0.5;
    for (const double hz : {8500.0, 9000.0, 12000.0, 19000.0, 0.45 * rate}) {
        if (hz >= rate / 2.0) {
            continue;
        }
        const auto out = whole(rate, tone(rate, hz, kAmplitude, 0.5));
        const double level = residual_db(out, kAmplitude);
        INFO("rate " << rate << ", tone " << hz << " Hz, out at " << level << " dB");
        CHECK(level < -75.0);
    }
}

TEST_CASE("a stream resampled in pieces equals the stream resampled whole", "[transcribe][resample]") {
    // REJECTS: history dropped or restarted at a call boundary, the output
    // position rounded per call, and the buffer compaction losing or
    // repeating a sample when a call ends mid-filter. Each of those changes
    // samples near the boundaries; the comparison is exact because the same
    // samples meet the same taps in the same order however they arrived.
    const std::uint32_t rate = GENERATE(8000U, 22050U, 44100U, 48000U, 171000U, 192000U);
    INFO("rate " << rate << ", seed " << kSeed);
    const auto in = noise(static_cast<std::size_t>(rate) * 3 / 2, kSeed + rate);
    const auto reference = whole(rate, in);
    for (const std::size_t largest : {std::size_t{1}, std::size_t{37}, std::size_t{5000}, std::size_t{40000}}) {
        if (largest == 1 && rate > 8000) {
            continue;
        }
        INFO("pieces of 1 to " << largest);
        const auto split = pieces(rate, in, kSeed ^ largest, largest);
        REQUIRE(split.size() == reference.size());
        CHECK(split == reference);
    }
}

TEST_CASE("reset forgets the stream and keeps the filter", "[transcribe][resample]") {
    // REJECTS: a reset that leaves history in the buffer, which bleeds the
    // last stream into the first few outputs of the next, or leaves the
    // output position where it was.
    constexpr std::uint32_t kRate = 44100;
    const auto first = noise(10000, kSeed);
    const auto second = tone(kRate, 1000.0, 0.5, 0.2);
    Resampler r(kRate);
    std::vector<float> discarded;
    r.process(first, discarded);
    r.reset();
    std::vector<float> after;
    r.process(second, after);
    r.finish(after);
    CHECK(after == whole(kRate, second));
}

TEST_CASE("equal rates pass straight through", "[transcribe][resample]") {
    // REJECTS: a filter run at 16000 in, which costs a dot product per
    // sample and takes the top 1.5 kHz off for nothing.
    const auto in = noise(16000, kSeed);
    const auto out = whole(kRecogniserRateHz, in);
    CHECK(out == in);
}

TEST_CASE("resampler passband ripple, stopband and cost, measured", "[.][transcribe][resample][measure]") {
    // Run by name: prints the measured passband ripple and worst stopband
    // level per rate, and the time to resample ten seconds.
    std::printf("%8s %6s %10s %12s %12s %10s\n", "rate", "taps", "phases", "ripple dB", "stop dB", "ms/10s");
    for (const std::uint32_t rate : {8000U, 11025U, 22050U, 24000U, 44100U, 48000U, 96000U, 171000U, 192000U}) {
        Resampler probe(rate);
        constexpr double kAmplitude = 0.5;
        double ripple = 0.0;
        for (double hz = 100.0; hz <= probe.passband_hz(); hz += 50.0) {
            const auto out = whole(rate, tone(rate, hz, kAmplitude, 0.5));
            ripple = std::max(ripple, std::abs(gain_db(out, hz, kAmplitude)));
        }
        double stop = -400.0;
        if (rate > kRecogniserRateHz) {
            for (double hz = probe.stopband_hz(); hz < 0.49 * rate; hz += 100.0) {
                stop = std::max(stop, residual_db(whole(rate, tone(rate, hz, kAmplitude, 0.5)), kAmplitude));
            }
        }
        const auto in = noise(static_cast<std::size_t>(rate) * 10, kSeed);
        const auto begin = std::chrono::steady_clock::now();
        const auto out = whole(rate, in);
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin);
        std::printf("%8u %6zu %6u%4s %12.6f %12.1f %10.1f\n", rate, probe.taps(), probe.phases(),
                    probe.exact_phases() ? "" : "i", ripple, stop, elapsed.count());
        CHECK(out.size() == owed(in.size(), rate));
    }
}
