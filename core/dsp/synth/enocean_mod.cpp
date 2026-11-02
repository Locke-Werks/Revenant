#include "core/dsp/synth/enocean_mod.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "core/decode/enocean.h"

namespace revenant::siggen {

std::vector<std::uint8_t> enocean_subtelegram(std::uint8_t rorg, std::span<const std::uint8_t> data,
                                              std::uint32_t sender_id, std::uint8_t status) {
    std::vector<std::uint8_t> out;
    out.push_back(rorg);
    out.insert(out.end(), data.begin(), data.end());
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(sender_id >> static_cast<unsigned>(shift)));
    }
    out.push_back(status);
    const bool crc = (status & 0x80U) != 0;
    out.push_back(crc ? decode::enocean_crc8(out) : decode::enocean_checksum(out));
    return out;
}

std::vector<std::uint8_t> enocean_frame_bits(std::span<const std::uint8_t> subtelegram) {
    std::vector<std::uint8_t> bits;
    for (int i = static_cast<int>(decode::kEnoceanPreambleSofBits) - 1; i >= 0; --i) {
        bits.push_back(static_cast<std::uint8_t>((decode::kEnoceanPreambleSof >> static_cast<unsigned>(i)) & 1U));
    }
    for (std::size_t n = 0; n < subtelegram.size(); ++n) {
        const std::uint16_t code = decode::enocean_encode_byte(subtelegram[n], n + 1 == subtelegram.size());
        for (int i = static_cast<int>(decode::kEnoceanEncodedBits) - 1; i >= 0; --i) {
            bits.push_back(static_cast<std::uint8_t>((code >> static_cast<unsigned>(i)) & 1U));
        }
    }
    return bits;
}

Expected<std::vector<dsp::Complex32>> enocean_modulate(std::span<const std::uint8_t> subtelegram,
                                                       const EnoceanTxConfig& config) {
    if (config.sample_rate <= 0 || config.bit_rate <= 0.0) {
        return fail("EnOcean transmitter: rate must be positive");
    }
    std::vector<std::uint8_t> bits(config.pre_preamble_bits, 1U);
    const auto frame = enocean_frame_bits(subtelegram);
    bits.insert(bits.end(), frame.begin(), frame.end());

    const double rate = static_cast<double>(config.sample_rate);
    const auto count = static_cast<std::size_t>(std::ceil(static_cast<double>(bits.size()) * rate / config.bit_rate));
    // Clause 1.7: a one is low power, a zero high power.
    const double low = config.amplitude * std::pow(10.0, -config.depth_db / 20.0);
    std::vector<dsp::Complex32> out(count);
    for (std::size_t n = 0; n < count; ++n) {
        const double t = static_cast<double>(n) / rate;
        auto index = static_cast<std::size_t>(t * config.bit_rate);
        index = std::min(index, bits.size() - 1);
        const double a = bits[index] != 0 ? low : config.amplitude;
        const double phase = 2.0 * std::numbers::pi * config.frequency_offset_hz * t;
        out[n] = dsp::Complex32(static_cast<float>(a * std::cos(phase)), static_cast<float>(a * std::sin(phase)));
    }
    return out;
}

}  // namespace revenant::siggen
