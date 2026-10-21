// The constellation and eye tap, core/engine/symbol_tap.h, and the timing
// loop under it, core/dsp/gardner.h. Synthetic signals only, no device.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <random>
#include <span>
#include <vector>

#include "core/dsp/gardner.h"
#include "core/engine/symbol_tap.h"

using namespace revenant;

namespace {

constexpr double kPi = std::numbers::pi;

double sinc(double x) { return std::abs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x); }

double raised_cosine(double t, double beta) {
    const double d = 1.0 - (2.0 * beta * t) * (2.0 * beta * t);
    if (std::abs(d) < 1e-9) {
        return kPi / 4.0 * sinc(1.0 / (2.0 * beta));
    }
    return sinc(t) * std::cos(kPi * beta * t) / d;
}

double root_raised_cosine(double t, double beta) {
    if (std::abs(t) < 1e-9) {
        return 1.0 - beta + 4.0 * beta / kPi;
    }
    if (std::abs(std::abs(t) - 1.0 / (4.0 * beta)) < 1e-9) {
        const double a = kPi / (4.0 * beta);
        return beta / std::numbers::sqrt2 *
               ((1.0 + 2.0 / kPi) * std::sin(a) + (1.0 - 2.0 / kPi) * std::cos(a));
    }
    return (std::sin(kPi * t * (1.0 - beta)) + 4.0 * beta * t * std::cos(kPi * t * (1.0 + beta))) /
           (kPi * t * (1.0 - (4.0 * beta * t) * (4.0 * beta * t)));
}

// Shapes a symbol train at `sps` with a pulse spanning six symbols each way,
// starting `delay` samples late so the tap has to find the timing.
template <typename Pulse>
std::vector<std::complex<double>> shape(const std::vector<std::complex<double>>& symbols, double sps,
                                        double delay, Pulse pulse) {
    const auto length = static_cast<std::size_t>(static_cast<double>(symbols.size()) * sps);
    std::vector<std::complex<double>> out(length);
    for (std::size_t n = 0; n < length; ++n) {
        const double t = (static_cast<double>(n) - delay) / sps;
        const auto centre = static_cast<std::int64_t>(std::floor(t));
        for (std::int64_t k = centre - 6; k <= centre + 6; ++k) {
            if (k < 0 || k >= static_cast<std::int64_t>(symbols.size())) {
                continue;
            }
            out[n] += symbols[static_cast<std::size_t>(k)] * pulse(t - static_cast<double>(k));
        }
    }
    return out;
}

std::vector<float> interleave(const std::vector<std::complex<double>>& in) {
    std::vector<float> out;
    out.reserve(2 * in.size());
    for (const auto& z : in) {
        out.push_back(static_cast<float>(z.real()));
        out.push_back(static_cast<float>(z.imag()));
    }
    return out;
}

void feed(engine::SymbolTap& tap, engine::Demod mode, const std::vector<float>& iq, dsp::SampleRate rate) {
    // In chunks the size a frame delivers, so state carries across calls.
    constexpr std::size_t kChunk = 2 * 960;
    for (std::size_t at = 0; at < iq.size(); at += kChunk) {
        const std::size_t n = std::min(kChunk, iq.size() - at);
        tap.push(mode, std::span<const float>(iq.data() + at, n), rate);
    }
}

// C4FM at 4800 baud and 600 Hz a level, 48 kS/s.
std::vector<float> c4fm(std::size_t count, double delay, double offset_hz, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> pick(0, 3);
    std::vector<std::complex<double>> levels(count);
    for (auto& l : levels) {
        l = static_cast<double>(2 * pick(rng) - 3);
    }
    const auto freq = shape(levels, 10.0, delay, [](double t) { return raised_cosine(t, 0.2); });
    std::vector<std::complex<double>> iq(freq.size());
    double phase = 0.0;
    for (std::size_t n = 0; n < freq.size(); ++n) {
        phase += 2.0 * kPi * (freq[n].real() * 600.0 + offset_hz) / 48'000.0;
        iq[n] = std::polar(0.5, phase);
    }
    return interleave(iq);
}

// pi/4-DQPSK at `sps` with a root raised cosine, a carrier offset and AWGN.
std::vector<float> dqpsk(std::size_t count, double sps, double delay, double offset_cycles_per_sample,
                         double noise, bool pi4, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> pick(0, 3);
    std::normal_distribution<double> gauss(0.0, noise);
    std::vector<std::complex<double>> symbols(count);
    double phase = 0.3;
    for (auto& s : symbols) {
        const double step = pi4 ? (kPi / 4.0 + kPi / 2.0 * pick(rng)) : (kPi / 2.0 * pick(rng));
        phase += step;
        s = std::polar(1.0, pi4 ? phase : (kPi / 4.0 + kPi / 2.0 * pick(rng)));
    }
    auto iq = shape(symbols, sps, delay, [](double t) { return root_raised_cosine(t, 0.35); });
    for (std::size_t n = 0; n < iq.size(); ++n) {
        iq[n] *= 0.3 * std::polar(1.0, 2.0 * kPi * offset_cycles_per_sample * static_cast<double>(n));
        iq[n] += std::complex<double>(gauss(rng), gauss(rng));
    }
    return interleave(iq);
}

}  // namespace

TEST_CASE("nothing is shown before a symbol and on an analogue mode", "[engine][symbol_tap]") {
    engine::SymbolTap tap;
    CHECK(tap.scope() == nullptr);
    const auto iq = c4fm(400, 3.0, 0.0, 1);
    feed(tap, engine::Demod::Nfm, iq, 48'000);
    CHECK(tap.scope() == nullptr);
}

TEST_CASE("C4FM gives four clustered levels and an open eye", "[engine][symbol_tap]") {
    engine::SymbolTap tap;
    // Three and a half samples late and 150 Hz off frequency.
    const auto iq = c4fm(3'000, 3.5, 150.0, 7);
    feed(tap, engine::Demod::P25p1, iq, 48'000);
    const auto* scope = tap.scope();
    REQUIRE(scope != nullptr);
    CHECK(scope->kind == engine::SymbolKind::Levels);
    CHECK(scope->points.size() == engine::kSymbolTapMaxSymbols);
    CHECK(scope->ideal.size() == 4);
    CHECK(scope->error_rms < 0.08F);
    CHECK(std::abs(scope->rate_error_ppm) < 2'000.0F);

    // Every level is used and each stays near its ideal.
    int seen[4] = {};
    for (const float v : scope->points) {
        const int nearest = std::clamp(static_cast<int>(std::lround((v + 3.0F) / 2.0F)), 0, 3);
        ++seen[nearest];
        CHECK(std::abs(v - static_cast<float>(2 * nearest - 3)) < 0.6F);
    }
    for (const int count : seen) {
        CHECK(count > 64);
    }

    // The eye is open at its centre and not at its edges: every trace sits
    // near a level at the strobe, and some are mid transition half a symbol
    // away.
    REQUIRE(scope->eye_trace_length == engine::kSymbolEyeTraceLength);
    const std::size_t traces = scope->eye.size() / scope->eye_trace_length;
    REQUIRE(traces == engine::kSymbolEyeTraces);
    const auto miss = [&](std::size_t index) {
        double worst = 0.0;
        for (std::size_t t = 0; t < traces; ++t) {
            const float v = scope->eye[t * scope->eye_trace_length + index];
            const float level = std::clamp(std::round((v + 3.0F) / 2.0F), 0.0F, 3.0F) * 2.0F - 3.0F;
            worst = std::max(worst, static_cast<double>(std::abs(v - level)));
        }
        return worst;
    };
    CHECK(miss(engine::kSymbolEyePerSymbol) < 0.6);
    CHECK(miss(engine::kSymbolEyePerSymbol / 2) > 0.6);
}

TEST_CASE("TETRA pi/4-DQPSK clusters on four points with a low EVM", "[engine][symbol_tap]") {
    engine::SymbolTap tap;
    // 72 kS/s at 18000 baud, 1.3 samples late, 200 Hz off and about 30 dB SNR.
    const auto iq = dqpsk(4'000, 4.0, 1.3, 200.0 / 72'000.0, 0.3 * 0.03, true, 3);
    feed(tap, engine::Demod::Tetra, iq, 72'000);
    const auto* scope = tap.scope();
    REQUIRE(scope != nullptr);
    CHECK(scope->kind == engine::SymbolKind::Constellation);
    CHECK(scope->points.size() == 2 * engine::kSymbolTapMaxSymbols);
    CHECK(scope->ideal.size() == 8);
    CHECK(scope->error_rms < 0.1F);

    int quadrant[4] = {};
    for (std::size_t i = 0; i < scope->points.size(); i += 2) {
        const float x = scope->points[i];
        const float y = scope->points[i + 1];
        ++quadrant[(x >= 0.0F ? 0 : 1) + (y >= 0.0F ? 0 : 2)];
        CHECK(std::hypot(std::abs(x) - 0.7071F, std::abs(y) - 0.7071F) < 0.35F);
    }
    for (const int count : quadrant) {
        CHECK(count > 64);
    }
}

TEST_CASE("noise raises the EVM", "[engine][symbol_tap]") {
    engine::SymbolTap clean;
    engine::SymbolTap noisy;
    feed(clean, engine::Demod::Tetra, dqpsk(3'000, 4.0, 0.0, 0.0, 0.003, true, 5), 72'000);
    feed(noisy, engine::Demod::Tetra, dqpsk(3'000, 4.0, 0.0, 0.0, 0.06, true, 5), 72'000);
    REQUIRE(clean.scope() != nullptr);
    REQUIRE(noisy.scope() != nullptr);
    CHECK(noisy.scope()->error_rms > 2.0F * clean.scope()->error_rms);
}

TEST_CASE("a raw receiver finds a QPSK signal's rate and constellation", "[engine][symbol_tap]") {
    engine::SymbolTap tap;
    // 48 kS/s at 2400 baud, 20 samples a symbol.
    const auto iq = dqpsk(3'000, 20.0, 7.0, 0.0, 0.003, false, 9);
    feed(tap, engine::Demod::Raw, iq, 48'000);
    const auto* scope = tap.scope();
    REQUIRE(scope != nullptr);
    CHECK(scope->kind == engine::SymbolKind::Constellation);
    CHECK_THAT(scope->symbol_rate_hz, Catch::Matchers::WithinRel(2'400.0, 0.02));
    // QPSK differentially is still QPSK, rotated by the scope onto the
    // diagonal points.
    CHECK(scope->error_rms < 0.15F);
}

TEST_CASE("a change of mode restarts the tap", "[engine][symbol_tap]") {
    engine::SymbolTap tap;
    feed(tap, engine::Demod::P25p1, c4fm(600, 0.0, 0.0, 2), 48'000);
    REQUIRE(tap.scope() != nullptr);
    tap.push(engine::Demod::Nfm, std::vector<float>(64, 0.0F), 48'000);
    CHECK(tap.scope() == nullptr);
}

TEST_CASE("the Gardner loop pulls in a clock error", "[dsp][gardner]") {
    // BPSK-like real levels at 0.2 percent slow, 8 samples a symbol nominal.
    std::mt19937 rng(11);
    std::uniform_int_distribution<int> pick(0, 1);
    std::vector<std::complex<double>> bits(4'000);
    for (auto& b : bits) {
        b = pick(rng) != 0 ? 1.0 : -1.0;
    }
    const double actual = 8.0 * 1.002;
    const auto wave = shape(bits, actual, 2.0, [](double t) { return raised_cosine(t, 0.5); });
    std::vector<dsp::Complex32> samples;
    for (const auto& z : wave) {
        samples.emplace_back(static_cast<float>(z.real()), 0.0F);
    }
    dsp::GardnerTiming loop(8.0, 0.01);
    std::size_t strobes = 0;
    double worst_late = 0.0;
    for (std::size_t at = 0; at < samples.size(); at += 512) {
        const auto n = std::min<std::size_t>(512, samples.size() - at);
        for (const auto& s : loop.push(std::span<const dsp::Complex32>(samples.data() + at, n))) {
            ++strobes;
            if (strobes > 1'000) {
                worst_late = std::max(worst_late, static_cast<double>(std::abs(std::abs(s.symbol.real()) - 1.0F)));
            }
        }
    }
    CHECK_THAT(loop.rate_error(), Catch::Matchers::WithinAbs(0.002, 0.0007));
    CHECK(worst_late < 0.15);
}
