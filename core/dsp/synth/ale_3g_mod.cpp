#include "core/dsp/synth/ale_3g_mod.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <utility>

namespace revenant::siggen {

namespace {

constexpr double kPi = std::numbers::pi;

// Root raised cosine impulse response, t in symbol periods, unit gain at
// DC over a symbol period (textbook closed form, Proakis section 9.2).
double rrc(double t, double a) {
    if (std::abs(t) < 1e-9) {
        return 1.0 - a + 4.0 * a / kPi;
    }
    if (a > 0.0 && std::abs(std::abs(t) - 1.0 / (4.0 * a)) < 1e-9) {
        return a / std::sqrt(2.0) *
               ((1.0 + 2.0 / kPi) * std::sin(kPi / (4.0 * a)) + (1.0 - 2.0 / kPi) * std::cos(kPi / (4.0 * a)));
    }
    const double num = std::sin(kPi * t * (1.0 - a)) + 4.0 * a * t * std::cos(kPi * t * (1.0 + a));
    const double den = kPi * t * (1.0 - (4.0 * a * t) * (4.0 * a * t));
    return num / den;
}

}  // namespace

Expected<std::vector<float>> ale3g_render(const Ale3gModConfig& config, const std::vector<Ale3gModBurst>& bursts) {
    if (config.rate < 7200) {
        return fail("3G ALE modulator: sample rate too low");
    }
    const double fs = static_cast<double>(config.rate);
    const double sps = fs / decode::kAle3gSymbolRate;
    // Symbols with their start times in samples.
    std::vector<std::pair<double, std::complex<double>>> syms;
    double t = config.lead_in_samples;
    for (const auto& b : bursts) {
        for (const auto s : decode::ale3g_burst(b.waveform, b.payload)) {
            syms.emplace_back(t, std::polar(1.0, kPi / 4.0 * s));
            t += sps;
        }
        t += b.gap_samples;
    }
    const auto n = static_cast<std::size_t>(std::ceil(t + config.tail_samples));
    std::vector<std::complex<double>> base(n);
    constexpr int kSpan = 6;  // symbols either side
    for (const auto& [ts, v] : syms) {
        const auto lo = static_cast<std::int64_t>(std::ceil(ts - kSpan * sps));
        const auto hi = static_cast<std::int64_t>(std::floor(ts + kSpan * sps));
        for (std::int64_t i = std::max<std::int64_t>(lo, 0); i <= hi && i < static_cast<std::int64_t>(n); ++i) {
            base[static_cast<std::size_t>(i)] += v * rrc((static_cast<double>(i) - ts) / sps, config.roll_off);
        }
    }
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * kPi * std::fmod(static_cast<double>(i) * (decode::kAle3gCarrierHz + config.offset_hz) / fs, 1.0);
        out[i] = static_cast<float>(config.amplitude * std::real(base[i] * std::polar(1.0, ph)));
    }
    return out;
}

}  // namespace revenant::siggen
