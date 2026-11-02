#include "core/dsp/synth/dect_mod.h"

#include <cmath>
#include <numbers>

namespace revenant::siggen {

std::uint64_t dect_a_field(unsigned ta, bool q1, unsigned ba, bool q2, std::uint64_t tail40) {
    // EN 300 175-3 Figure 7.2: TA a0-a2, Q1 a3, BA a4-a6, Q2 a7.
    const std::uint64_t header = (static_cast<std::uint64_t>(ta & 7U) << 5U) |
                                 (static_cast<std::uint64_t>(q1 ? 1U : 0U) << 4U) |
                                 (static_cast<std::uint64_t>(ba & 7U) << 1U) | (q2 ? 1U : 0U);
    const std::uint64_t data48 = (header << 40U) | (tail40 & ((1ULL << 40U) - 1ULL));
    return (data48 << 16U) | decode::dect_rcrc(data48);
}

std::vector<std::uint8_t> dect_p32_bits(decode::DectPolarity polarity, std::uint64_t a_field,
                                        std::span<const std::uint8_t> b_field,
                                        std::span<const std::uint8_t, 4> x_field) {
    std::vector<std::uint8_t> bits;
    const std::uint32_t s = polarity == decode::DectPolarity::Rfp ? decode::kDectSFieldRfp : decode::kDectSFieldPp;
    for (int i = 31; i >= 0; --i) {
        bits.push_back(static_cast<std::uint8_t>((s >> static_cast<unsigned>(i)) & 1U));
    }
    for (int i = 63; i >= 0; --i) {
        bits.push_back(static_cast<std::uint8_t>((a_field >> static_cast<unsigned>(i)) & 1U));
    }
    for (std::size_t i = 0; i < decode::kDectBFieldBits; ++i) {
        bits.push_back(i < b_field.size() ? static_cast<std::uint8_t>(b_field[i] & 1U) : std::uint8_t{0});
    }
    for (const std::uint8_t x : x_field) {
        bits.push_back(static_cast<std::uint8_t>(x & 1U));
    }
    // EN 300 175-2 clause 4.8: z0 to z3 equal the last 4 D-field symbols.
    for (const std::uint8_t x : x_field) {
        bits.push_back(static_cast<std::uint8_t>(x & 1U));
    }
    return bits;
}

Expected<std::vector<dsp::Complex32>> dect_modulate(std::span<const std::uint8_t> bits, const DectTxConfig& config) {
    if (config.sample_rate <= 0 || bits.empty()) {
        return fail("DECT transmitter: rate must be positive and bits non-empty");
    }
    const double rate = static_cast<double>(config.sample_rate);
    const double sps = rate / decode::kDectSymbolRate;
    const auto count = static_cast<std::size_t>(std::ceil(static_cast<double>(bits.size()) * sps));
    // Gaussian filtered rectangular pulse, BT 0,5 (EN 300 175-2 clause
    // 5.4.1): 0,5 [erf(a(t/T + 1/2)) - erf(a(t/T - 1/2))], a = pi BT
    // sqrt(2 / ln 2).
    const double alpha = std::numbers::pi * decode::kDectBt * std::sqrt(2.0 / std::numbers::ln2);
    std::vector<dsp::Complex32> out(count);
    double phase = 0.0;
    const auto nbits = static_cast<long long>(bits.size());
    for (std::size_t n = 0; n < count; ++n) {
        const double t = static_cast<double>(n) / sps;  // in symbols
        const auto centre = static_cast<long long>(std::floor(t));
        double f = 0.0;
        for (long long k = centre - 3; k <= centre + 3; ++k) {
            const double sym = k < 0 || k >= nbits ? 0.0 : (bits[static_cast<std::size_t>(k)] != 0 ? 1.0 : -1.0);
            const double u = t - static_cast<double>(k) - 0.5;
            f += sym * 0.5 * (std::erf(alpha * (u + 0.5)) - std::erf(alpha * (u - 0.5)));
        }
        phase += 2.0 * std::numbers::pi * (config.deviation_hz * f + config.frequency_offset_hz) / rate;
        phase = std::remainder(phase, 2.0 * std::numbers::pi);
        out[n] = dsp::Complex32(static_cast<float>(config.amplitude * std::cos(phase)),
                                static_cast<float>(config.amplitude * std::sin(phase)));
    }
    return out;
}

}  // namespace revenant::siggen
