#include "core/dsp/synth/psk_mod.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace revenant::siggen {

double rrc_pulse(double t, double alpha) {
    constexpr double pi = std::numbers::pi;
    if (std::abs(t) < 1e-9) {
        return 1.0 - alpha + 4.0 * alpha / pi;
    }
    if (alpha > 0.0 && std::abs(std::abs(t) - 1.0 / (4.0 * alpha)) < 1e-9) {
        return alpha / std::numbers::sqrt2 *
               ((1.0 + 2.0 / pi) * std::sin(pi / (4.0 * alpha)) +
                (1.0 - 2.0 / pi) * std::cos(pi / (4.0 * alpha)));
    }
    const double num =
        std::sin(pi * t * (1.0 - alpha)) + 4.0 * alpha * t * std::cos(pi * t * (1.0 + alpha));
    const double den = pi * t * (1.0 - (4.0 * alpha * t) * (4.0 * alpha * t));
    return num / den;
}

Expected<std::vector<Complex32>> psk_render(const PskModConfig& config,
                                            std::span<const std::uint8_t> symbols) {
    if (config.rate <= 0 || !(config.symbol_rate > 0.0)) {
        return fail("PSK modulator: rate and symbol rate must be positive");
    }
    if (!(config.roll_off >= 0.0 && config.roll_off <= 1.0)) {
        return fail("PSK modulator: roll-off must be in [0, 1]");
    }
    const double sps = static_cast<double>(config.rate) / config.symbol_rate *
                       (1.0 + config.clock_error);
    const auto half = static_cast<double>(config.half_span_symbols);
    const double start = half * sps + config.timing_offset_samples;
    const bool offset_rail = config.modulation == decode::PskModulation::Oqpsk;
    const double tail = (half + (offset_rail ? 0.5 : 0.0)) * sps;
    const auto length = static_cast<std::size_t>(
        std::ceil(start + static_cast<double>(symbols.size()) * sps + tail)) + 1;

    std::vector<std::complex<double>> base(length);
    for (std::size_t k = 0; k < symbols.size(); ++k) {
        const Complex32 point = decode::psk_point(config.modulation, symbols[k]);
        const double centre = start + static_cast<double>(k) * sps;
        const auto add_rail = [&](double c, std::complex<double> weight) {
            const auto first = static_cast<long long>(std::ceil(c - half * sps));
            const auto last = static_cast<long long>(std::floor(c + half * sps));
            for (long long n = std::max(first, 0LL);
                 n <= last && n < static_cast<long long>(length); ++n) {
                base[static_cast<std::size_t>(n)] +=
                    weight * rrc_pulse((static_cast<double>(n) - c) / sps, config.roll_off);
            }
        };
        if (offset_rail) {
            add_rail(centre, {point.real(), 0.0});
            add_rail(centre + 0.5 * sps, {0.0, point.imag()});
        } else {
            add_rail(centre, {point.real(), point.imag()});
        }
    }

    std::vector<Complex32> out(length);
    const double step = 2.0 * std::numbers::pi * config.carrier_offset_hz /
                        static_cast<double>(config.rate);
    for (std::size_t n = 0; n < length; ++n) {
        const double phase = config.initial_phase + step * static_cast<double>(n);
        const auto v = base[n] * std::polar(config.amplitude, phase);
        out[n] = Complex32(static_cast<float>(v.real()), static_cast<float>(v.imag()));
    }
    return out;
}

}  // namespace revenant::siggen
