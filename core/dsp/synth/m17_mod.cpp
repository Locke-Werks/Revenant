#include "core/dsp/synth/m17_mod.h"

#include <cmath>
#include <format>
#include <numbers>

#include "core/decode/dv_phy.h"

namespace revenant::siggen {
namespace {

constexpr double kPi = std::numbers::pi;

void append_word(std::vector<int>& symbols, std::uint16_t word) {
    for (const float symbol : decode::m17_word_symbols(word)) {
        symbols.push_back(static_cast<int>(symbol));
    }
}

void append_frame(std::vector<int>& symbols, std::uint16_t sync,
                  std::vector<std::uint8_t> type4) {
    append_word(symbols, sync);
    decode::m17_randomize(type4);
    for (std::size_t i = 0; i + 1 < type4.size(); i += 2) {
        const auto dibit = static_cast<std::size_t>((type4[i] << 1U) | type4[i + 1]);
        symbols.push_back(decode::kM17DibitToSymbol[dibit]);
    }
}

}  // namespace

Expected<std::vector<int>> m17_stream_symbols(const M17StreamMessage& message) {
    std::vector<int> symbols;

    // 1.4.1 and Table 2.3: before an LSF the preamble alternates +3, -3, and
    // its last symbol is opposite the burst's first, which is +3.
    for (std::size_t i = 0; i < decode::kM17PreambleSymbols; ++i) {
        symbols.push_back((i % 2 == 0) ? 3 : -3);
    }

    const auto lsf = decode::m17_lsf_bytes(message.destination, message.source, message.type,
                                           message.meta);
    auto lsf_bits = decode::m17_encode_lsf(lsf);
    if (!lsf_bits) {
        return std::unexpected(lsf_bits.error());
    }
    append_frame(symbols, decode::kM17SyncLsf, std::move(*lsf_bits));

    for (std::size_t i = 0; i < message.payloads.size(); ++i) {
        // Table 2.9: frame i carries LSF chunk i mod 6. 2.8.1: the frame
        // number counts from 0 and its top bit marks the last frame.
        const auto count = static_cast<std::uint8_t>(i % decode::kM17LichChunks);
        auto number = static_cast<std::uint16_t>(i & 0x7FFFU);
        if (i + 1 == message.payloads.size()) {
            number = static_cast<std::uint16_t>(number | 0x8000U);
        }
        auto bits = decode::m17_encode_stream_frame(
            std::span<const std::uint8_t>(lsf).subspan(count * decode::kM17LichChunkBytes,
                                                        decode::kM17LichChunkBytes),
            count, number, message.payloads[i]);
        if (!bits) {
            return std::unexpected(bits.error());
        }
        append_frame(symbols, decode::kM17SyncStream, std::move(*bits));
    }

    // 1.4.5: 192 symbols of repeated 0x555D.
    for (std::size_t i = 0; i < decode::kM17EotSymbols / decode::kM17SyncSymbols; ++i) {
        append_word(symbols, decode::kM17EotWord);
    }
    return symbols;
}

Expected<std::vector<Complex32>> m17_render_symbols(const M17ModConfig& config,
                                                    std::span<const int> symbols) {
    const double exact = static_cast<double>(config.rate) / decode::kM17SymbolRate;
    const auto sps = static_cast<std::size_t>(std::lround(exact));
    if (sps < 2 || std::abs(exact - static_cast<double>(sps)) > 1e-9) {
        return fail(std::format(
            "m17_render_symbols needs a sample rate that is a whole multiple of 4800, at "
            "least 9600; got {}",
            config.rate));
    }
    const std::size_t taps_count =
        static_cast<std::size_t>(decode::kM17RrcSpanSymbols * sps) | 1U;
    auto taps = decode::design_rrc(config.rate, decode::kM17SymbolRate, decode::kM17RrcRollOff,
                                   taps_count);
    if (!taps) {
        return std::unexpected(with_context(taps.error(), "designing the M17 1.3 filter"));
    }
    // Unit gain at zero hertz: see the header.
    double sum = 0.0;
    for (const float tap : *taps) {
        sum += static_cast<double>(tap);
    }
    for (float& tap : *taps) {
        tap = static_cast<float>(static_cast<double>(tap) / sum);
    }

    std::vector<float> impulses(symbols.size() * sps + taps->size(), 0.0F);
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        impulses[i * sps] = static_cast<float>(symbols[i] * decode::kM17DeviationPerUnitHz *
                                               static_cast<double>(sps));
    }
    std::vector<float> hertz(impulses.size(), 0.0F);
    if (auto status = decode::filter_real(impulses, *taps, hertz); !status) {
        return std::unexpected(with_context(status.error(), "M17 transmit filtering"));
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

Expected<std::vector<Complex32>> m17_render_stream(const M17ModConfig& config,
                                                   const M17StreamMessage& message) {
    auto symbols = m17_stream_symbols(message);
    if (!symbols) {
        return std::unexpected(symbols.error());
    }
    return m17_render_symbols(config, *symbols);
}

}  // namespace revenant::siggen
