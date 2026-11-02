#include "core/dsp/synth/vdes_mod.h"

#include <cmath>
#include <format>
#include <numbers>

#include "core/decode/dv_phy.h"

namespace revenant::siggen {

namespace {

using dsp::Complex32;

constexpr std::size_t kFilterSpan = 8;

void put_bits(std::vector<std::uint8_t>& octets, std::size_t first, unsigned width,
              std::uint32_t value) {
    for (unsigned i = 0; i < width; ++i) {
        const std::size_t bit = first + i;
        if (((value >> (width - 1U - i)) & 1U) != 0U) {
            octets[bit / 8] = static_cast<std::uint8_t>(octets[bit / 8] | (0x80U >> (bit % 8)));
        }
    }
}

}  // namespace

Expected<std::vector<std::uint8_t>> vdes_asm_build_payload(
    unsigned link_id, const decode::VdesAsmMessage& header,
    std::span<const std::uint8_t> application_data) {
    const auto format = decode::vdes_asm_format(link_id);
    if (!format) {
        return fail(std::format("VDES ASM Link ID {} is not an ASM-TER format", link_id));
    }
    const std::size_t octets = format->packet_bits / 8 - 4;
    const std::size_t room = octets * 8 - decode::kVdesAsmHeaderBits;
    if (header.data_count > room || application_data.size() * 8 < header.data_count) {
        return fail("VDES ASM application data does not fit the format");
    }
    std::vector<std::uint8_t> out(octets, 0);
    put_bits(out, 0, 4, header.message_id);
    put_bits(out, 4, 1, header.retransmit ? 1U : 0U);
    put_bits(out, 5, 2, header.repeat);
    put_bits(out, 7, 6, header.session_id);
    put_bits(out, 13, 32, header.source_id);
    put_bits(out, 45, 11, header.data_count);
    for (std::size_t i = 0; i < header.data_count; ++i) {
        put_bits(out, decode::kVdesAsmHeaderBits + i, 1,
                 (application_data[i / 8] >> (7U - i % 8)) & 1U);
    }
    return out;
}

Expected<std::vector<Complex32>> vdes_asm_render(const VdesAsmModConfig& config,
                                                 std::span<const VdesAsmBurstSpec> bursts) {
    if (config.rate <= 0 || config.rate % 9600 != 0) {
        return fail("VDES ASM transmitter needs a whole number of samples per symbol");
    }
    const auto sps = static_cast<std::size_t>(config.rate / 9600);

    // Symbol-rate values, silence as zero.
    std::vector<Complex32> symbols(config.gap_symbols, Complex32{});
    for (const VdesAsmBurstSpec& b : bursts) {
        const auto format = decode::vdes_asm_format(b.link_id);
        const decode::TurboCode* turbo =
            format && !format->uncoded ? decode::vdes_asm_turbo(b.link_id) : nullptr;
        if (!format || (!format->uncoded && turbo == nullptr)) {
            return fail(std::format("VDES ASM Link ID {} is not an ASM-TER format", b.link_id));
        }
        const std::size_t octets = format->packet_bits / 8 - 4;
        if (b.payload.size() != octets) {
            return fail(std::format("VDES ASM Link ID {} carries {} payload octets, not {}",
                                    b.link_id, octets, b.payload.size()));
        }
        std::vector<std::uint8_t> packet = b.payload;
        const std::uint32_t crc = decode::vdes_crc32(b.payload);
        for (int s = 24; s >= 0; s -= 8) {
            packet.push_back(static_cast<std::uint8_t>(crc >> static_cast<unsigned>(s)));
        }
        std::vector<std::uint8_t> bits;
        bits.reserve(format->data_symbols * 2);
        for (std::size_t i = 0; i < format->packet_bits; ++i) {
            bits.push_back(static_cast<std::uint8_t>((packet[i / 8] >> (7U - i % 8)) & 1U));
        }
        if (turbo != nullptr) {
            // Figure 6: FEC, then the scrambler over the coded bits.
            bits = turbo->encode(bits);
        }
        bits.resize(format->data_symbols * 2, 0);
        decode::vdes_scramble(bits);

        auto ramp = [&](std::size_t i, bool up) {
            const double x = (static_cast<double>(i) + 0.5) /
                             static_cast<double>(decode::kVdesRampSymbols);
            const double e = 0.5 - 0.5 * std::cos(std::numbers::pi * (up ? x : 1.0 - x));
            return static_cast<float>(e);
        };
        for (std::size_t i = 0; i < decode::kVdesRampSymbols; ++i) {
            // Ramp symbols take the parity the syncword continues.
            symbols.push_back(decode::vdes_pi4qpsk_point(i, 1, 1) * ramp(i, true));
        }
        std::size_t n = 0;
        for (const std::uint8_t s : decode::kVdesTerSyncword) {
            symbols.push_back(decode::vdes_pi4qpsk_point(n++, s, s));
        }
        const std::uint32_t word = decode::vdes_link_id_codeword(b.link_id);
        for (std::size_t s = 0; s < decode::kVdesLinkIdSymbols; ++s) {
            symbols.push_back(decode::vdes_pi4qpsk_point(n++, (word >> (31U - 2 * s)) & 1U,
                                                         (word >> (30U - 2 * s)) & 1U));
        }
        for (std::size_t s = 0; s < format->data_symbols; ++s) {
            symbols.push_back(decode::vdes_pi4qpsk_point(n++, bits[2 * s], bits[2 * s + 1]));
        }
        for (std::size_t i = 0; i < decode::kVdesRampSymbols; ++i) {
            symbols.push_back(decode::vdes_pi4qpsk_point(n++, 0, 0) * ramp(i, false));
        }
        symbols.insert(symbols.end(), config.gap_symbols, Complex32{});
    }

    std::vector<Complex32> impulses(symbols.size() * sps + kFilterSpan * sps, Complex32{});
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        impulses[i * sps] = symbols[i];
    }
    auto taps = decode::design_rrc(config.rate, decode::kVdesAsmSymbolRate,
                                   decode::kVdesAsmRollOff, kFilterSpan * sps + 1);
    if (!taps) {
        return std::unexpected(taps.error());
    }
    // Unit energy per symbol through the pair of filters: scale the taps so
    // the transmit pulse's peak through the receive filter is one.
    double energy = 0.0;
    for (const float t : *taps) {
        energy += static_cast<double>(t) * static_cast<double>(t);
    }
    std::vector<Complex32> out(impulses.size());
    if (auto s = decode::filter_complex(impulses, *taps, out); !s) {
        return std::unexpected(s.error());
    }
    const double scale = config.amplitude / std::sqrt(energy);
    const double w = 2.0 * std::numbers::pi * config.carrier_offset_hz / config.rate;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const double ph = w * static_cast<double>(i);
        out[i] *= Complex32(static_cast<float>(scale * std::cos(ph)),
                            static_cast<float>(scale * std::sin(ph)));
    }
    return out;
}

}  // namespace revenant::siggen
