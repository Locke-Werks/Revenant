#include "core/dsp/synth/dpmr_mod.h"

#include <cmath>
#include <format>
#include <numbers>

#include "core/decode/dv_phy.h"

namespace revenant::siggen {
namespace {

constexpr double kPi = std::numbers::pi;

void append_bits(std::vector<int>& symbols, std::span<const std::uint8_t> bits) {
    for (std::size_t i = 0; i + 1 < bits.size(); i += 2) {
        const auto dibit = static_cast<std::size_t>((bits[i] << 1U) | bits[i + 1]);
        symbols.push_back(decode::kDpmrDibitToSymbol[dibit]);
    }
}

void append_word(std::vector<int>& symbols, std::uint64_t word, std::size_t bits) {
    for (const float symbol : decode::dpmr_word_symbols(word, bits)) {
        symbols.push_back(static_cast<int>(symbol));
    }
}

}  // namespace

std::vector<int> dpmr_symbols(const DpmrMessage& message) {
    std::vector<int> symbols;
    // 490 6.1.6: nine octets of 0x5F.
    for (int i = 0; i < 9; ++i) {
        append_word(symbols, decode::kDpmrPreambleOctet, 8);
    }
    decode::DpmrHeaderInfo hi;
    hi.header_type = message.header_type;
    hi.called_id = message.called_id;
    hi.own_id = message.own_id;
    hi.mode = message.mode;
    hi.format = message.format;
    const auto hi_bits = decode::dpmr_encode_hi(hi);
    const std::uint32_t code = decode::dpmr_channel_code_bits(message.channel_code);
    // 490 9.5: FS1, HI, the channel code, HI again.
    append_word(symbols, decode::kDpmrFs1, 48);
    append_bits(symbols, hi_bits);
    append_word(symbols, code, 24);
    append_bits(symbols, hi_bits);

    std::size_t frames = (message.payload.size() + 3) / 4 * 4;
    const std::array<std::uint16_t, 4> parts = {
        static_cast<std::uint16_t>(message.called_id >> 12U),
        static_cast<std::uint16_t>(message.called_id & 0xFFFU),
        static_cast<std::uint16_t>(message.own_id >> 12U),
        static_cast<std::uint16_t>(message.own_id & 0xFFFU),
    };
    for (std::size_t f = 0; f < frames; ++f) {
        const std::size_t fn = f % 4;
        // 490 9.1: FS2 on frames 00 and 10, the channel code on 01 and 11.
        if (fn % 2 == 0) {
            append_word(symbols, decode::kDpmrFs2, 24);
        } else {
            append_word(symbols, code, 24);
        }
        decode::DpmrCch cch;
        cch.frame_number = static_cast<std::uint8_t>(fn);
        cch.id_part = parts[fn];
        cch.mode = message.mode;
        cch.format = message.format;
        cch.slow_data = message.slow_data;
        append_bits(symbols, decode::dpmr_encode_cch(cch));
        DpmrTchFrame tch{};
        if (f < message.payload.size()) {
            tch = message.payload[f];
        }
        for (const auto& block : tch) {
            for (const std::uint8_t byte : block) {
                append_word(symbols, byte, 8);
            }
        }
    }
    append_word(symbols, decode::kDpmrFs3, 24);
    append_bits(symbols, decode::dpmr_encode_end(message.end));
    return symbols;
}

Expected<std::vector<Complex32>> dpmr_render_symbols(const DpmrModConfig& config,
                                                     std::span<const int> symbols) {
    const double exact = static_cast<double>(config.rate) / decode::kDpmrSymbolRate;
    const auto sps = static_cast<std::size_t>(std::lround(exact));
    if (sps < 2 || std::abs(exact - static_cast<double>(sps)) > 1e-9) {
        return fail(std::format(
            "dpmr_render_symbols needs a sample rate that is a whole multiple of 2400, at "
            "least 4800; got {}",
            config.rate));
    }
    const std::size_t count = (decode::kDpmrRrcSpanSymbols * sps) | 1U;
    auto taps =
        decode::design_rrc(config.rate, decode::kDpmrSymbolRate, decode::kDpmrRrcRollOff, count);
    if (!taps) {
        return std::unexpected(with_context(taps.error(), "designing the dPMR 12.2.2.2 filter"));
    }
    double sum = 0.0;
    for (const float tap : *taps) {
        sum += static_cast<double>(tap);
    }
    for (float& tap : *taps) {
        tap = static_cast<float>(static_cast<double>(tap) / sum);
    }
    std::vector<float> impulses(symbols.size() * sps + taps->size(), 0.0F);
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        impulses[i * sps] = static_cast<float>(symbols[i] * decode::kDpmrDeviationPerUnitHz *
                                               static_cast<double>(sps));
    }
    std::vector<float> hertz(impulses.size(), 0.0F);
    if (auto status = decode::filter_real(impulses, *taps, hertz); !status) {
        return std::unexpected(with_context(status.error(), "dPMR transmit filtering"));
    }
    std::vector<Complex32> out(hertz.size());
    double phase = 0.0;
    const double step = 2.0 * kPi / static_cast<double>(config.rate);
    for (std::size_t i = 0; i < hertz.size(); ++i) {
        phase = std::remainder(phase + step * static_cast<double>(hertz[i]), 2.0 * kPi);
        out[i] = Complex32{static_cast<float>(config.amplitude * std::cos(phase)),
                           static_cast<float>(config.amplitude * std::sin(phase))};
    }
    return out;
}

Expected<std::vector<Complex32>> dpmr_render(const DpmrModConfig& config,
                                             const DpmrMessage& message) {
    const auto symbols = dpmr_symbols(message);
    return dpmr_render_symbols(config, symbols);
}

}  // namespace revenant::siggen
