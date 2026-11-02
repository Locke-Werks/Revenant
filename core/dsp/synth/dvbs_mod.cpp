// DVB-S transmitter. See dvbs_mod.h.

#include "core/dsp/synth/dvbs_mod.h"

#include <algorithm>

#include "core/decode/dv_codes.h"

namespace revenant::siggen {

using namespace decode;

std::vector<std::uint8_t> dvbs_symbols(std::span<const TsPacketBytes> packets, DvbsCodeRate rate) {
    // 4.4.1 energy dispersal, a group of eight at a time.
    std::vector<std::uint8_t> ts;
    ts.reserve(packets.size() * kTsPacketBytes);
    for (const auto& p : packets) {
        ts.insert(ts.end(), p.begin(), p.end());
    }
    for (std::size_t g = 0; g < ts.size(); g += 8 * kTsPacketBytes) {
        const std::size_t n = std::min(ts.size() - g, 8 * kTsPacketBytes);
        dvbs_energy_dispersal(std::span<std::uint8_t>(ts).subspan(g, n), true);
    }

    // 4.4.2 RS(204,188) then the interleaver, sync byte on branch 0.
    DvbsInterleaver interleaver(false);
    std::vector<std::uint8_t> bits;
    bits.reserve(packets.size() * kDvbsCodedPacketBytes * 8);
    std::array<std::uint8_t, 16> check{};
    for (std::size_t p = 0; p < packets.size(); ++p) {
        const auto info = std::span<const std::uint8_t>(ts).subspan(p * kTsPacketBytes, kTsPacketBytes);
        dvbs_rs().encode(info, check);
        auto put = [&](std::uint8_t b) {
            const auto o = interleaver.push(b);
            for (int i = 7; i >= 0; --i) {
                bits.push_back(static_cast<std::uint8_t>((o >> i) & 1U));
            }
        };
        for (const auto b : info) {
            put(b);
        }
        for (const auto b : check) {
            put(b);
        }
    }

    // 4.4.3 the mother code and table 2's puncturing.
    const ConvolutionalCode code{kDvbsMemory, kDvbsGenerators};
    const auto encoded = *convolutional_encode(code, bits);
    const auto pattern = dvbs_puncture_pattern(rate);
    const auto sent = puncture_bits(encoded, pattern);

    // 4.5 figure 5: pairs onto I then Q.
    std::vector<std::uint8_t> symbols;
    symbols.reserve(sent.size() / 2);
    for (std::size_t i = 0; i + 1 < sent.size(); i += 2) {
        symbols.push_back(kDvbsQpskIndex[(sent[i] << 1U) | sent[i + 1]]);
    }
    return symbols;
}

Expected<std::vector<Complex32>> dvbs_render(const DvbsModConfig& config,
                                             std::span<const TsPacketBytes> packets) {
    auto psk = config.psk;
    psk.modulation = PskModulation::Qpsk;
    const auto symbols = dvbs_symbols(packets, config.rate);
    return psk_render(psk, symbols);
}

TsPacketBytes ts_null_packet() {
    TsPacketBytes p{};
    p.fill(0xFF);
    p[0] = kTsSync;
    p[1] = 0x1F;
    p[2] = 0xFF;
    p[3] = 0x10;  // payload only, continuity 0
    return p;
}

std::vector<TsPacketBytes> ts_section_packets(std::uint16_t pid, std::span<const std::uint8_t> section,
                                              std::uint8_t& continuity) {
    std::vector<TsPacketBytes> out;
    std::size_t pos = 0;
    bool first = true;
    while (pos < section.size() || first) {
        TsPacketBytes p{};
        p.fill(0xFF);
        p[0] = kTsSync;
        p[1] = static_cast<std::uint8_t>((first ? 0x40U : 0U) | ((pid >> 8U) & 0x1FU));
        p[2] = static_cast<std::uint8_t>(pid & 0xFFU);
        p[3] = static_cast<std::uint8_t>(0x10U | (continuity & 0x0FU));
        continuity = static_cast<std::uint8_t>((continuity + 1) & 0x0FU);
        std::size_t at = 4;
        if (first) {
            p[at++] = 0;  // pointer_field
        }
        while (at < kTsPacketBytes && pos < section.size()) {
            p[at++] = section[pos++];
        }
        out.push_back(p);
        first = false;
    }
    return out;
}

namespace {

void put16(std::vector<std::uint8_t>& v, unsigned x) {
    v.push_back(static_cast<std::uint8_t>(x >> 8U));
    v.push_back(static_cast<std::uint8_t>(x));
}

// Long-form header with section_length patched in by finish().
std::vector<std::uint8_t> start(std::uint8_t table_id, std::uint16_t extension, std::uint8_t version) {
    std::vector<std::uint8_t> s{table_id, 0xB0, 0x00};
    put16(s, extension);
    s.push_back(static_cast<std::uint8_t>(0xC1U | ((version & 0x1FU) << 1U)));
    s.push_back(0);  // section_number
    s.push_back(0);  // last_section_number
    return s;
}

std::vector<std::uint8_t> finish(std::vector<std::uint8_t> s) {
    const std::size_t length = s.size() - 3 + 4;
    s[1] = static_cast<std::uint8_t>((s[1] & 0xF0U) | ((length >> 8U) & 0x0FU));
    s[2] = static_cast<std::uint8_t>(length);
    const auto crc = ts_crc32(s);
    s.push_back(static_cast<std::uint8_t>(crc >> 24U));
    s.push_back(static_cast<std::uint8_t>(crc >> 16U));
    s.push_back(static_cast<std::uint8_t>(crc >> 8U));
    s.push_back(static_cast<std::uint8_t>(crc));
    return s;
}

}  // namespace

std::vector<std::uint8_t> ts_build_pat(std::uint16_t tsid, std::uint8_t version,
                                       std::span<const TsProgram> programs) {
    auto s = start(0x00, tsid, version);
    for (const auto& p : programs) {
        put16(s, p.program_number);
        put16(s, 0xE000U | p.pid);
    }
    return finish(std::move(s));
}

std::vector<std::uint8_t> ts_build_pmt(std::uint16_t program_number, std::uint8_t version,
                                       std::uint16_t pcr_pid,
                                       std::span<const TsElementaryStream> streams) {
    auto s = start(0x02, program_number, version);
    put16(s, 0xE000U | pcr_pid);
    put16(s, 0xF000U);  // program_info_length 0
    for (const auto& e : streams) {
        s.push_back(e.stream_type);
        put16(s, 0xE000U | e.pid);
        put16(s, 0xF000U);
    }
    return finish(std::move(s));
}

std::vector<std::uint8_t> ts_build_sdt(std::uint16_t tsid, std::uint16_t onid, std::uint8_t version,
                                       std::span<const TsService> services) {
    auto s = start(0x42, tsid, version);
    s[1] = 0xF0;  // EN 300 468 table 5: reserved_future_use bit set
    put16(s, onid);
    s.push_back(0xFF);
    for (const auto& svc : services) {
        put16(s, svc.service_id);
        s.push_back(0xFC);  // no EIT flags
        std::vector<std::uint8_t> d{0x48, 0, svc.service_type,
                                    static_cast<std::uint8_t>(svc.provider.size())};
        d.insert(d.end(), svc.provider.begin(), svc.provider.end());
        d.push_back(static_cast<std::uint8_t>(svc.name.size()));
        d.insert(d.end(), svc.name.begin(), svc.name.end());
        d[1] = static_cast<std::uint8_t>(d.size() - 2);
        const unsigned flags = (static_cast<unsigned>(svc.running_status & 7U) << 13U) |
                               (svc.free_ca_mode ? 0x1000U : 0U) | static_cast<unsigned>(d.size());
        put16(s, flags);
        s.insert(s.end(), d.begin(), d.end());
    }
    return finish(std::move(s));
}

}  // namespace revenant::siggen
