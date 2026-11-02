#include "core/dsp/synth/grb_mod.h"

#include <algorithm>

#include "core/decode/dvbs2.h"
#include "core/dsp/synth/ccsds_downlink_mod.h"
#include "core/dsp/synth/psk_mod.h"

namespace revenant::siggen {

namespace {

using namespace revenant::decode;

void append_plframe(std::vector<std::uint8_t>& out, std::uint8_t modcod, std::uint8_t type,
                    std::span<const std::uint8_t> body, std::span<const std::uint8_t> scrambling) {
    const auto header = dvbs2_plheader(modcod, type);
    out.insert(out.end(), header.begin(), header.end());
    // 5.5.4: j^R is two steps of pi/4 per unit of R.
    for (std::size_t i = 0; i < body.size(); ++i) {
        out.push_back(static_cast<std::uint8_t>((body[i] + 2U * scrambling[i]) & 7U));
    }
}

void append_dummy(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> scrambling) {
    // 5.5.1: 36 slots of (1 + j)/sqrt 2, grid index 1.
    const std::vector<std::uint8_t> body(36 * kDvbs2SlotSymbols, 1);
    append_plframe(out, kDvbs2ModcodDummy, 0, body, scrambling);
}

}  // namespace

std::vector<std::uint8_t> grb_cadus(const GrbTxSpec& spec, std::span<const std::vector<std::uint8_t>> packets) {
    AosPackConfig pc;
    pc.spacecraft_id = spec.spacecraft_id;
    pc.virtual_channel = spec.virtual_channel;
    pc.frame_octets = kGrbFrameOctets - 2;
    auto frames = aos_pack_mpdu(pc, packets);
    std::vector<std::uint8_t> out;
    for (auto& f : frames) {
        const std::uint16_t crc = ccsds_fecf(f);
        f.push_back(static_cast<std::uint8_t>(crc >> 8U));
        f.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
        for (int s = 24; s >= 0; s -= 8) {
            out.push_back(static_cast<std::uint8_t>(kGrbMarker >> static_cast<unsigned>(s)));
        }
        out.insert(out.end(), f.begin(), f.end());
    }
    return out;
}

Expected<std::vector<std::uint8_t>> grb_symbols(const GrbTxSpec& spec, std::span<const std::uint8_t> cadus) {
    const bool psk8 = spec.modulation == GrbModulation::Psk8Rate23;
    const std::uint8_t modcod = psk8 ? kDvbs2Modcod8psk23 : kDvbs2ModcodQpsk910;
    auto fec = Dvbs2Fec::create(modcod);
    if (!fec) {
        return std::unexpected(fec.error());
    }
    const auto& code = fec->code();
    const PskModulation mod = code.modulation;
    const unsigned bits = psk_bits_per_symbol(mod);
    const std::uint8_t type = spec.pilots ? 1 : 0;
    const auto scrambling = dvbs2_pl_scrambling(spec.gold_code, dvbs2_plframe_symbols(modcod, type));

    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < spec.leading_dummy_frames; ++i) {
        append_dummy(out, scrambling);
    }

    std::vector<std::uint8_t> stream;
    stream.reserve(cadus.size() * 8);
    for (const std::uint8_t o : cadus) {
        for (int b = 7; b >= 0; --b) {
            stream.push_back(static_cast<std::uint8_t>((o >> static_cast<unsigned>(b)) & 1U));
        }
    }
    const std::size_t max_dfl = code.kbch - kDvbs2BbHeaderBits;
    std::vector<std::uint8_t> bb(code.kbch);
    std::vector<std::uint8_t> fecframe(kDvbs2NormalFrameBits);
    std::vector<std::uint8_t> tx(kDvbs2NormalFrameBits);
    std::vector<std::uint8_t> body;
    const std::size_t slots = kDvbs2NormalFrameBits / bits / kDvbs2SlotSymbols;
    for (std::size_t off = 0; off < stream.size(); off += max_dfl) {
        const std::size_t dfl = std::min(max_dfl, stream.size() - off);
        Dvbs2BbHeader h;
        h.matype1 = kGrbMatype1;
        h.dfl = static_cast<std::uint16_t>(dfl);
        std::fill(bb.begin(), bb.end(), std::uint8_t{0});
        dvbs2_write_bbheader(h, bb);
        std::copy_n(stream.begin() + static_cast<std::ptrdiff_t>(off), dfl,
                    bb.begin() + static_cast<std::ptrdiff_t>(kDvbs2BbHeaderBits));
        dvbs2_bb_scramble(bb);
        fec->encode(bb, fecframe);
        if (psk8) {
            dvbs2_interleave_8psk(fecframe, tx);
        } else {
            tx = fecframe;
        }
        body.clear();
        for (std::size_t s = 0; s < slots; ++s) {
            if (spec.pilots && s > 0 && s % 16 == 0) {
                // 5.5.3: unmodulated (1 + j)/sqrt 2.
                body.insert(body.end(), kDvbs2PilotSymbols, std::uint8_t{1});
            }
            for (std::size_t i = 0; i < kDvbs2SlotSymbols; ++i) {
                const std::size_t sym = s * kDvbs2SlotSymbols + i;
                unsigned label = 0;
                for (unsigned b = 0; b < bits; ++b) {
                    label = (label << 1U) | tx[sym * bits + b];
                }
                body.push_back(dvbs2_grid_index(mod, label));
            }
        }
        append_plframe(out, modcod, type, body, scrambling);
    }
    for (std::size_t i = 0; i < spec.trailing_dummy_frames; ++i) {
        append_dummy(out, scrambling);
    }
    return out;
}

Expected<std::vector<dsp::Complex32>> grb_baseband(const GrbTxSpec& spec,
                                                   std::span<const std::vector<std::uint8_t>> packets) {
    const auto cadus = grb_cadus(spec, packets);
    auto syms = grb_symbols(spec, cadus);
    if (!syms) {
        return std::unexpected(syms.error());
    }
    const bool psk8 = spec.modulation == GrbModulation::Psk8Rate23;
    PskModConfig mc;
    mc.rate = static_cast<SampleRate>(spec.sample_rate);
    mc.symbol_rate = spec.symbol_rate > 0.0 ? spec.symbol_rate : (psk8 ? kGrb8pskSymbolRate : kGrbQpskSymbolRate);
    mc.modulation = PskModulation::Psk8;
    mc.roll_off = kGrbRollOff;
    mc.carrier_offset_hz = spec.carrier_offset_hz;
    mc.initial_phase = spec.initial_phase;
    mc.timing_offset_samples = spec.timing_offset_samples;
    mc.clock_error = spec.clock_error;
    return psk_render(mc, *syms);
}

}  // namespace revenant::siggen
