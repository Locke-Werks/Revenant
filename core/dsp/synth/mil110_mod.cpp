#include "core/dsp/synth/mil110_mod.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

#include "core/decode/dv_phy.h"

namespace revenant::siggen {

Expected<std::vector<float>> mil110_render(const Mil110ModConfig& config,
                                           std::span<const std::uint8_t> tribits) {
    std::vector<std::complex<float>> points(tribits.size());
    for (std::size_t s = 0; s < tribits.size(); ++s) {
        const auto p = std::polar(1.0, std::numbers::pi / 4.0 * (tribits[s] % 8));
        points[s] = std::complex<float>(p);
    }
    return mil110_render_points(config, points);
}

Expected<std::vector<float>> mil110_render_points(const Mil110ModConfig& config,
                                                  std::span<const std::complex<float>> symbols) {
    const auto sym_rate = static_cast<dsp::SampleRate>(decode::kMil110SymbolRate);
    if (config.rate <= 0 || config.rate % sym_rate != 0) {
        return fail("MIL-STD-188-110 modulator: rate must be a multiple of 2400");
    }
    const auto sps = static_cast<std::size_t>(config.rate / sym_rate);
    const std::size_t taps_n = 8 * sps + 1;
    auto taps = decode::design_rrc(config.rate, decode::kMil110SymbolRate, 0.35, taps_n);
    if (!taps) {
        return std::unexpected(taps.error());
    }

    const std::size_t body = symbols.size() * sps + taps_n;
    std::vector<std::complex<double>> base(body);
    for (std::size_t s = 0; s < symbols.size(); ++s) {
        const std::complex<double> p(symbols[s]);
        for (std::size_t k = 0; k < taps_n; ++k) {
            base[s * sps + k] += p * static_cast<double>((*taps)[k]);
        }
    }
    double power = 0.0;
    for (const auto& b : base) {
        power += std::norm(b);
    }
    power /= static_cast<double>(std::max<std::size_t>(symbols.size() * sps, 1));
    // A carrier of peak A has power A^2 / 2; Re{b e^jwt} has power |b|^2 / 2.
    const double gain = power > 0.0 ? config.amplitude / std::sqrt(power) : 0.0;

    std::vector<float> out(config.lead_in_samples + body + config.tail_samples, 0.0F);
    const double w = 2.0 * std::numbers::pi * (decode::kMil110CarrierHz + config.offset_hz) /
                     static_cast<double>(config.rate);
    for (std::size_t i = 0; i < body; ++i) {
        const double ph = w * static_cast<double>(i);
        out[config.lead_in_samples + i] =
            static_cast<float>(gain * (base[i] * std::polar(1.0, ph)).real());
    }
    return out;
}

Expected<std::vector<float>> mil110_render_bits(const Mil110ModConfig& config,
                                                decode::Mil110Mode mode,
                                                std::span<const std::uint8_t> bits) {
    auto symbols = decode::mil110_transmit_symbols(mode, bits);
    if (!symbols) {
        return std::unexpected(symbols.error());
    }
    return mil110_render(config, *symbols);
}

Expected<std::vector<float>> mil110c_render_bits(const Mil110ModConfig& config,
                                                 decode::Mil110cMode mode,
                                                 std::span<const std::uint8_t> bits,
                                                 int agc_blocks) {
    auto symbols = decode::mil110c_transmit_symbols(mode, bits, agc_blocks);
    if (!symbols) {
        return std::unexpected(symbols.error());
    }
    return mil110_render_points(config, *symbols);
}

}  // namespace revenant::siggen
