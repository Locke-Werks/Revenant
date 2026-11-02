#include "core/dsp/synth/dab_mod.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <format>
#include <utility>

#include "core/characterise/transform.h"

namespace revenant::siggen {

using namespace decode;

namespace {

using characterise::Complex64;

void put16(std::vector<std::uint8_t>& out, unsigned value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

std::vector<std::uint8_t> fig(unsigned type, std::vector<std::uint8_t> data) {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>((type << 5U) | (data.size() & 0x1FU)));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::vector<std::uint8_t> bytes_to_bits(std::span<const std::uint8_t> bytes) {
    std::vector<std::uint8_t> bits(bytes.size() * 8);
    for (std::size_t i = 0; i < bits.size(); ++i) {
        bits[i] = static_cast<std::uint8_t>((bytes[i / 8] >> (7U - (i % 8))) & 1U);
    }
    return bits;
}

std::size_t bin_of(int carrier) {
    const int n = static_cast<int>(kDabFftSize);
    return static_cast<std::size_t>(((carrier % n) + n) % n);
}

// One OFDM symbol with its guard (clause 14.2): unitary inverse transform,
// guard copied from the tail.
void emit_symbol(std::span<const Complex32> carriers_by_bin, std::vector<Complex32>& out) {
    std::vector<Complex64> x(kDabFftSize);
    for (std::size_t i = 0; i < kDabFftSize; ++i) {
        x[i] = std::conj(Complex64(carriers_by_bin[i]));
    }
    characterise::fft_in_place(x);
    // K of N carriers occupied: scale to unit mean power per sample.
    const double scale = 1.0 / std::sqrt(static_cast<double>(kDabCarriers));
    for (auto& v : x) {
        v = std::conj(v) * scale;
    }
    for (std::size_t i = kDabFftSize - kDabGuardSamples; i < kDabFftSize; ++i) {
        out.emplace_back(x[i]);
    }
    for (std::size_t i = 0; i < kDabFftSize; ++i) {
        out.emplace_back(x[i]);
    }
}

}  // namespace

std::vector<std::uint8_t> dab_fig_ensemble(std::uint16_t eid, unsigned cif_count) {
    std::vector<std::uint8_t> d{0x00};  // C/N, OE, P/D zero, extension 0
    put16(d, eid);
    d.push_back(static_cast<std::uint8_t>((cif_count / 250) & 0x1FU));
    d.push_back(static_cast<std::uint8_t>(cif_count % 250));
    return fig(0, std::move(d));
}

std::vector<std::uint8_t> dab_fig_subchannels(std::span<const DabSubchannel> subchannels) {
    std::vector<std::uint8_t> d{0x01};
    for (const auto& sc : subchannels) {
        put16(d, (sc.id << 10U) | (sc.start_cu & 0x3FFU));
        const unsigned option = sc.protection == DabProtection::EepB ? 1 : 0;
        put16(d, 0x8000U | (option << 12U) | (((sc.level - 1) & 3U) << 10U) | (sc.size_cu & 0x3FFU));
    }
    return fig(0, std::move(d));
}

std::vector<std::uint8_t> dab_fig_service(const DabService& service) {
    std::vector<std::uint8_t> d{static_cast<std::uint8_t>(service.data_sid ? 0x22 : 0x02)};
    if (service.data_sid) {
        put16(d, service.sid >> 16U);
    }
    put16(d, service.sid & 0xFFFFU);
    d.push_back(static_cast<std::uint8_t>(service.components.size() & 0x0FU));
    for (const auto& c : service.components) {
        unsigned word = c.transport << 14U;
        if (c.transport == 3) {
            word |= (c.scid & 0xFFFU) << 2U;
        } else {
            word |= ((c.type & 0x3FU) << 8U) | ((c.subchannel & 0x3FU) << 2U);
        }
        word |= (c.primary ? 2U : 0U) | (c.ca ? 1U : 0U);
        put16(d, word);
    }
    return fig(0, std::move(d));
}

std::vector<std::uint8_t> dab_fig_packet(const DabPacketComponent& c) {
    std::vector<std::uint8_t> d{0x03};
    put16(d, (c.scid & 0xFFFU) << 4U);
    d.push_back(static_cast<std::uint8_t>((c.data_groups ? 0U : 0x80U) | (c.dscty & 0x3FU)));
    put16(d, ((c.subchannel & 0x3FU) << 10U) | (c.address & 0x3FFU));
    return fig(0, std::move(d));
}

std::vector<std::uint8_t> dab_fig_label(unsigned ext, std::uint32_t id, std::string_view text) {
    std::vector<std::uint8_t> d{static_cast<std::uint8_t>(ext & 0x7U)};  // charset 0
    if (ext == 5) {
        put16(d, id >> 16U);
    }
    put16(d, id & 0xFFFFU);
    std::uint16_t flags = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        d.push_back(static_cast<std::uint8_t>(i < text.size() ? text[i] : ' '));
        if (i < 8) {
            flags = static_cast<std::uint16_t>(flags | (0x8000U >> i));
        }
    }
    put16(d, flags);
    return fig(1, std::move(d));
}

Expected<std::array<std::uint8_t, 30>> dab_pack_fib(std::span<const std::vector<std::uint8_t>> figs) {
    std::array<std::uint8_t, 30> fib{};
    std::size_t at = 0;
    for (const auto& f : figs) {
        if (at + f.size() > fib.size()) {
            return fail("dab_mod: FIGs do not fit in one FIB");
        }
        std::copy(f.begin(), f.end(), fib.begin() + static_cast<std::ptrdiff_t>(at));
        at += f.size();
    }
    if (at < fib.size()) {
        fib[at] = 0xFF;  // end marker; the rest stays zero padding
    }
    return fib;
}

std::vector<std::uint8_t> dab_data_group(const DabDataGroupSpec& spec,
                                         std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> g;
    g.push_back(static_cast<std::uint8_t>(0x40U | (spec.segmented ? 0x20U : 0U) |
                                          (spec.has_transport_id ? 0x10U : 0U) | (spec.type & 0xFU)));
    g.push_back(static_cast<std::uint8_t>(((spec.continuity & 0xFU) << 4U) | (spec.repetition & 0xFU)));
    if (spec.segmented) {
        put16(g, (spec.last ? 0x8000U : 0U) | (spec.segment & 0x7FFFU));
    }
    if (spec.has_transport_id) {
        g.push_back(0x12);  // Transport Id flag, length 2
        put16(g, spec.transport_id);
    }
    g.insert(g.end(), data.begin(), data.end());
    put16(g, dab_crc16(g));
    return g;
}

std::vector<std::vector<std::uint8_t>> dab_packetize(std::span<const std::uint8_t> group,
                                                     unsigned address, std::size_t packet_length,
                                                     unsigned& continuity) {
    std::vector<std::vector<std::uint8_t>> out;
    unsigned length_code = 0;
    while (length_code < 3 && (length_code + 1) * 24 != packet_length) {
        ++length_code;
    }
    const std::size_t capacity = packet_length - 5;
    std::size_t at = 0;
    do {
        const std::size_t n = std::min(capacity, group.size() - at);
        const bool first = at == 0;
        const bool last = at + n == group.size();
        std::vector<std::uint8_t> p;
        put16(p, (length_code << 14U) | ((continuity & 3U) << 12U) | (first ? 0x800U : 0U) |
                     (last ? 0x400U : 0U) | (address & 0x3FFU));
        p.push_back(static_cast<std::uint8_t>(n));
        p.insert(p.end(), group.begin() + static_cast<std::ptrdiff_t>(at),
                 group.begin() + static_cast<std::ptrdiff_t>(at + n));
        p.resize(packet_length - 2, 0);
        put16(p, dab_crc16(p));
        out.push_back(std::move(p));
        continuity = (continuity + 1) % 4;
        at += n;
    } while (at < group.size());
    return out;
}

std::vector<std::uint8_t> dab_padding_packet(std::size_t packet_length) {
    unsigned continuity = 0;
    const std::vector<std::uint8_t> nothing;
    auto packets = dab_packetize(nothing, 0, packet_length, continuity);
    return packets.front();
}

std::vector<std::uint8_t> mot_header(std::size_t body_size, unsigned content_type,
                                     unsigned content_subtype, std::string_view content_name) {
    // Parameter: PLI 11, ParamId 0x0C, Ext 0, 7-bit length, charset byte, name.
    const std::size_t parameter = 2 + 1 + content_name.size();
    const std::size_t header_size = 7 + parameter;
    std::uint64_t core = (static_cast<std::uint64_t>(body_size & 0xFFFFFFFU) << 28U) |
                         (static_cast<std::uint64_t>(header_size & 0x1FFFU) << 15U) |
                         (static_cast<std::uint64_t>(content_type & 0x3FU) << 9U) |
                         (content_subtype & 0x1FFU);
    std::vector<std::uint8_t> h;
    for (int shift = 48; shift >= 0; shift -= 8) {
        h.push_back(static_cast<std::uint8_t>((core >> static_cast<unsigned>(shift)) & 0xFFU));
    }
    h.push_back(0xC0 | 0x0C);
    h.push_back(static_cast<std::uint8_t>((1 + content_name.size()) & 0x7FU));
    h.push_back(0x00);  // character set indicator and Rfa, both zero
    h.insert(h.end(), content_name.begin(), content_name.end());
    return h;
}

std::vector<std::uint8_t> mot_segment(std::span<const std::uint8_t> segment, unsigned repetition_count) {
    std::vector<std::uint8_t> out;
    put16(out, ((repetition_count & 7U) << 13U) | (segment.size() & 0x1FFFU));
    out.insert(out.end(), segment.begin(), segment.end());
    return out;
}

Expected<DabTransmitter> DabTransmitter::create(std::vector<DabSubchannel> subchannels) {
    DabTransmitter tx;
    for (const auto& sc : subchannels) {
        auto profile = dab_eep_profile(sc);
        if (!profile) {
            return std::unexpected(profile.error());
        }
        Channel ch;
        ch.config = sc;
        ch.profile = std::move(*profile);
        ch.history.assign(16, std::vector<std::uint8_t>(sc.size_cu * kDabCuBits, 0));
        tx.channels_.emplace(sc.id, std::move(ch));
    }
    return tx;
}

std::size_t DabTransmitter::logical_frame_bytes(unsigned id) const {
    const auto it = channels_.find(id);
    return it == channels_.end() ? 0 : it->second.profile.information_bits / 8;
}

Expected<std::vector<Complex32>> DabTransmitter::frame(
    std::span<const std::array<std::uint8_t, 30>> fibs,
    const std::map<unsigned, std::array<std::vector<std::uint8_t>, 4>>& frames) {
    if (fibs.size() != kDabFibsPerFrame) {
        return fail("dab_mod: a frame takes 12 FIBs");
    }
    std::vector<std::uint8_t> bits;  // p(l, n) for l = 2..76, back to back
    bits.reserve((kDabSymbols - 1) * kDabBitsPerSymbol);

    // FIC: clause 5.2.1 CRC, 10.2 scrambling, 11.2.1 coding, 14.4.1.1 order.
    const auto prbs768 = dab_prbs(768);
    const auto fic_runs = dab_fic_runs();
    for (std::size_t r = 0; r < 4; ++r) {
        std::vector<std::uint8_t> group;
        for (std::size_t f = 0; f < 3; ++f) {
            const auto& data = fibs[r * 3 + f];
            group.insert(group.end(), data.begin(), data.end());
            put16(group, dab_crc16(data));
        }
        auto info = bytes_to_bits(group);
        for (std::size_t i = 0; i < info.size(); ++i) {
            info[i] ^= prbs768[i];
        }
        auto coded = dab_convolutional_encode(info, fic_runs);
        if (!coded) {
            return std::unexpected(coded.error());
        }
        bits.insert(bits.end(), coded->begin(), coded->end());
    }

    // MSC: four CIFs (clause 13), padding CUs from the PRBS.
    const auto cif_prbs = dab_prbs(kDabCifBits);
    for (std::size_t j = 0; j < kDabCifsPerFrame; ++j) {
        std::vector<std::uint8_t> cif(cif_prbs);
        for (auto& [id, ch] : channels_) {
            std::vector<std::uint8_t> info(ch.profile.information_bits, 0);
            const auto it = frames.find(id);
            if (it != frames.end()) {
                const auto& bytes = it->second[j];
                if (bytes.size() * 8 != info.size()) {
                    return fail(std::format("dab_mod: sub-channel {} needs {} bytes per logical frame",
                                            id, info.size() / 8));
                }
                info = bytes_to_bits(bytes);
            }
            const auto prbs = dab_prbs(info.size());
            for (std::size_t i = 0; i < info.size(); ++i) {
                info[i] ^= prbs[i];
            }
            auto coded = dab_convolutional_encode(info, ch.profile.runs);
            if (!coded) {
                return std::unexpected(coded.error());
            }
            const std::uint64_t r = ch.written;
            ch.history[r % 16] = std::move(*coded);
            ++ch.written;
            // Clause 12: c(r, i) = b(r - delay(i mod 16), i); before the first
            // codeword the interleaver holds zeros.
            const std::size_t base = ch.config.start_cu * kDabCuBits;
            for (std::size_t i = 0; i < ch.history[r % 16].size(); ++i) {
                const unsigned d = dab_time_interleave_delay(i);
                cif[base + i] = d > r ? 0 : ch.history[(r - d) % 16][i];
            }
        }
        bits.insert(bits.end(), cif.begin(), cif.end());
    }

    // Symbols: null, phase reference, then D-QPSK (clauses 14.3 to 14.7).
    std::vector<Complex32> out(kDabNullSamples, Complex32{0.0F, 0.0F});
    out.reserve(kDabFrameSamples);
    std::vector<Complex32> z(kDabFftSize, Complex32{0.0F, 0.0F});
    for (int k = -static_cast<int>(kDabCarriers / 2); k <= static_cast<int>(kDabCarriers / 2); ++k) {
        z[bin_of(k)] = dab_prs_value(k);
    }
    emit_symbol(z, out);
    const auto& interleaver = dab_frequency_interleaver();
    const float h = 1.0F / std::sqrt(2.0F);
    for (std::size_t l = 0; l + 1 < kDabSymbols; ++l) {
        const std::uint8_t* p = bits.data() + l * kDabBitsPerSymbol;
        for (std::size_t n = 0; n < kDabCarriers; ++n) {
            const Complex32 q{h * (1.0F - 2.0F * static_cast<float>(p[n])),
                              h * (1.0F - 2.0F * static_cast<float>(p[n + kDabCarriers]))};
            const std::size_t bin = bin_of(interleaver[n]);
            z[bin] *= q;
        }
        emit_symbol(z, out);
    }
    return out;
}

}  // namespace revenant::siggen
