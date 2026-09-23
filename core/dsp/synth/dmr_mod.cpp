#include "core/dsp/synth/dmr_mod.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

#include "core/decode/dmr_codes.h"
#include "core/decode/dv_phy.h"

namespace revenant::siggen {
namespace {

using decode::DmrDataType;
using decode::DmrSyncType;

constexpr double kPi = std::numbers::pi;

void put_sync(DmrBurstBits& burst, DmrSyncType sync) {
    const std::uint64_t bits = decode::dmr_sync_bits(sync);
    for (std::size_t i = 0; i < decode::kDmrCentreBits; ++i) {
        burst[decode::kDmrCentreFirstBit + i] =
            static_cast<std::uint8_t>((bits >> (decode::kDmrCentreBits - 1 - i)) & 1U);
    }
}

// Figure 6.2: VS(215) to VS(108) before the centre field, VS(107) to VS(0)
// after it.
void put_voice(DmrBurstBits& burst, const DmrVoiceBits& voice) {
    std::copy_n(voice.begin(), decode::kDmrCentreFirstBit, burst.begin());
    std::copy_n(voice.begin() + decode::kDmrCentreFirstBit, decode::kDmrCentreFirstBit,
                burst.begin() + decode::kDmrCentreFirstBit + decode::kDmrCentreBits);
}

// The core of both renderers: dibits into 4FSK, the carrier gated off where
// `on` says so.
Expected<std::vector<Complex32>> render(const DmrModConfig& config, std::span<const std::uint8_t> dibits,
                                        std::span<const std::uint8_t> on) {
    const double exact = static_cast<double>(config.rate) / decode::kDmrSymbolRate;
    const auto sps = static_cast<std::size_t>(std::lround(exact));
    if (sps < 2 || std::abs(exact - static_cast<double>(sps)) > 1e-9) {
        return fail(std::format(
            "the DMR transmitter needs the sample rate to be a whole multiple of 4800 symbols per "
            "second (TS 102 361-1 clause 10.2.1), at least two; {} Hz is {:.6f}",
            config.rate, exact));
    }
    auto taps = decode::design_from_response(config.rate, config.filter_taps, decode::dmr_rrc_response,
                                             nullptr);
    if (!taps) {
        return std::unexpected(with_context(taps.error(), "designing the clause 10.2.2.2 filter"));
    }
    // Unit sum, and the impulses carry the samples per symbol, for the reason
    // core/dsp/synth/dv_mod.cpp's normalise_unit_sum gives: an impulse train
    // through a unit-sum filter otherwise averages a symbol's deviation over
    // the zeros between impulses.
    double sum = 0.0;
    for (const float tap : *taps) {
        sum += static_cast<double>(tap);
    }
    for (float& tap : *taps) {
        tap = static_cast<float>(static_cast<double>(tap) / sum);
    }

    // Table 10.3: each dibit's deviation in hertz, so the filter's output is
    // an instantaneous frequency.
    std::vector<float> impulses(dibits.size() * sps + config.filter_taps, 0.0F);
    for (std::size_t i = 0; i < dibits.size(); ++i) {
        if (on[i] == 0) {
            continue;
        }
        const auto index = static_cast<std::size_t>(dibits[i] & 0x3U);
        impulses[i * sps] = static_cast<float>(decode::kDmrDibitToSymbol[index] *
                                               decode::kDmrDeviationPerSymbolUnitHz *
                                               static_cast<double>(sps));
    }
    std::vector<float> shaped(impulses.size(), 0.0F);
    if (auto status = decode::filter_real(impulses, *taps, shaped); !status) {
        return std::unexpected(with_context(status.error(), "DMR transmit filtering"));
    }

    // The carrier's envelope. Clause 10.2.3.1.1, figure 10.3: the power
    // ramps in the 1,5 ms either side of a burst's 27,5 ms of symbols, Regions
    // A and C, and is at full power for every symbol. The ramp here is a
    // raised cosine over those 1,5 ms, which sits inside the mask; its shape
    // is an engineering choice, its length is the figure's. Symbol s occupies
    // [s - 0.5, s + 0.5] in symbol time, counted through the filter's delay.
    // A carrier switched at the first and last symbol instead puts the
    // discriminator's noise from the guard time into a burst's edge symbols
    // through the receive filter, which no transmitter within the mask does.
    const double ramp_symbols = 1.5e-3 * decode::kDmrSymbolRate;
    const auto count = static_cast<std::ptrdiff_t>(on.size());
    std::vector<std::ptrdiff_t> left(on.size(), -1);
    std::vector<std::ptrdiff_t> right(on.size(), -1);
    for (std::ptrdiff_t s = 0, last = -1; s < count; ++s) {
        last = on[static_cast<std::size_t>(s)] != 0 ? s : last;
        left[static_cast<std::size_t>(s)] = last;
    }
    for (std::ptrdiff_t s = count - 1, next = -1; s >= 0; --s) {
        next = on[static_cast<std::size_t>(s)] != 0 ? s : next;
        right[static_cast<std::size_t>(s)] = next;
    }
    const auto envelope = [&](double u) {
        if (count == 0) {
            return 0.0;
        }
        const auto s = static_cast<std::ptrdiff_t>(std::clamp(std::floor(u + 0.5), 0.0,
                                                              static_cast<double>(count - 1)));
        double distance = ramp_symbols;
        const std::ptrdiff_t l = left[static_cast<std::size_t>(s)];
        const std::ptrdiff_t r = right[static_cast<std::size_t>(s)];
        if (l >= 0) {
            distance = std::min(distance, std::max(0.0, u - (static_cast<double>(l) + 0.5)));
        }
        if (r >= 0) {
            distance = std::min(distance, std::max(0.0, (static_cast<double>(r) - 0.5) - u));
        }
        return distance >= ramp_symbols ? 0.0 : 0.5 * (1.0 + std::cos(kPi * distance / ramp_symbols));
    };

    const double delay = static_cast<double>(config.filter_taps - 1) / 2.0;
    std::vector<Complex32> out(shaped.size());
    double phase = 0.0;
    const double step = 2.0 * kPi / static_cast<double>(config.rate);
    for (std::size_t n = 0; n < shaped.size(); ++n) {
        phase += step * static_cast<double>(shaped[n]);
        if (phase > kPi) {
            phase -= 2.0 * kPi;
        } else if (phase < -kPi) {
            phase += 2.0 * kPi;
        }
        const double u = (static_cast<double>(n) - delay) / static_cast<double>(sps);
        const double amplitude = config.amplitude * envelope(u);
        out[n] = Complex32{static_cast<float>(amplitude * std::cos(phase)),
                           static_cast<float>(amplitude * std::sin(phase))};
    }
    return out;
}

void append_dibits(std::span<const std::uint8_t> bits, bool on, std::vector<std::uint8_t>& dibits,
                   std::vector<std::uint8_t>& mask) {
    for (std::size_t i = 0; i + 1 < bits.size(); i += 2) {
        dibits.push_back(static_cast<std::uint8_t>(((bits[i] & 1U) << 1U) | (bits[i + 1] & 1U)));
        mask.push_back(on ? 1 : 0);
    }
}

}  // namespace

DmrBurstBits dmr_voice_burst_with_sync(const DmrVoiceBits& voice, DmrSyncType sync) {
    DmrBurstBits burst{};
    put_voice(burst, voice);
    put_sync(burst, sync);
    return burst;
}

DmrBurstBits dmr_voice_burst_embedded(const DmrVoiceBits& voice, std::uint8_t colour_code, bool pi,
                                      std::uint8_t lcss,
                                      std::span<const std::uint8_t, decode::kDmrEmbeddedFragmentBits> fragment) {
    DmrBurstBits burst{};
    put_voice(burst, voice);
    // Figure 6.4 and Table E.6: eight EMB bits, the 32 embedded bits, the
    // other eight.
    const auto emb = decode::dmr_emb_bits(colour_code, pi, lcss);
    std::size_t at = decode::kDmrCentreFirstBit;
    std::copy_n(emb.begin(), decode::kDmrEmbHalfBits, burst.begin() + static_cast<std::ptrdiff_t>(at));
    at += decode::kDmrEmbHalfBits;
    std::copy(fragment.begin(), fragment.end(), burst.begin() + static_cast<std::ptrdiff_t>(at));
    at += decode::kDmrEmbeddedFragmentBits;
    std::copy_n(emb.begin() + decode::kDmrEmbHalfBits, decode::kDmrEmbHalfBits,
                burst.begin() + static_cast<std::ptrdiff_t>(at));
    return burst;
}

DmrBurstBits dmr_data_burst(DmrSyncType sync, std::uint8_t colour_code, DmrDataType data_type,
                            std::span<const std::uint8_t, decode::kDmrBptcBits> info) {
    DmrBurstBits burst{};
    // Figure 6.5 and Table E.1: 98 information bits, ten slot type bits, the
    // sync, the other ten, the other 98.
    const auto slot_type = decode::dmr_slot_type_bits(colour_code, static_cast<std::uint8_t>(data_type));
    std::copy_n(info.begin(), decode::kDmrInfoHalfBits, burst.begin());
    std::copy_n(slot_type.begin(), decode::kDmrSlotTypeHalfBits,
                burst.begin() + decode::kDmrInfoHalfBits);
    put_sync(burst, sync);
    const std::size_t after = decode::kDmrCentreFirstBit + decode::kDmrCentreBits;
    std::copy_n(slot_type.begin() + decode::kDmrSlotTypeHalfBits, decode::kDmrSlotTypeHalfBits,
                burst.begin() + after);
    std::copy_n(info.begin() + decode::kDmrInfoHalfBits, decode::kDmrInfoHalfBits,
                burst.begin() + after + decode::kDmrSlotTypeHalfBits);
    return burst;
}

Expected<DmrBurstBits> dmr_bptc_burst(DmrSyncType sync, std::uint8_t colour_code, DmrDataType data_type,
                                      std::span<const std::uint8_t> octets) {
    auto block = decode::dmr_data_block_bits(data_type, octets);
    if (!block) {
        return std::unexpected(block.error());
    }
    const auto info = decode::dmr_bptc196_encode(*block);
    return dmr_data_burst(sync, colour_code, data_type, info);
}

DmrBurstBits dmr_idle_burst(DmrSyncType sync, std::uint8_t colour_code) {
    const auto info = decode::dmr_bptc196_encode(decode::dmr_idle_bits());
    return dmr_data_burst(sync, colour_code, DmrDataType::Idle, info);
}

DmrCachBurstBits dmr_cach(bool access_busy, std::uint8_t channel, std::uint8_t lcss,
                          std::span<const std::uint8_t, decode::kDmrCachPayloadBits> payload) {
    // Table 9.5: AT, TC, LCSS, then the Hamming (7,4) parity. Table 9.24's
    // TC is 0 for channel 1 and 1 for channel 2.
    const std::uint32_t information = (access_busy ? 0x8U : 0x0U) | (channel == 2 ? 0x4U : 0x0U) |
                                      (lcss & 0x3U);
    decode::DmrCachBits bits;
    bits.tact = decode::dmr_block_encode(decode::kDmrHamming7, information);
    std::copy(payload.begin(), payload.end(), bits.payload.begin());
    return decode::dmr_cach_interleave(bits);
}

DmrShortLcFragments dmr_short_lc_fragments(std::uint8_t slco, std::uint32_t data) {
    std::array<std::uint8_t, 28> lc{};
    const std::uint32_t word = (static_cast<std::uint32_t>(slco & 0xFU) << 24U) | (data & 0xFF'FFFFU);
    for (std::size_t i = 0; i < lc.size(); ++i) {
        lc[i] = static_cast<std::uint8_t>((word >> (27 - i)) & 1U);
    }
    const auto encoded = decode::dmr_short_lc_encode(lc);
    DmrShortLcFragments out;
    for (std::size_t f = 0; f < decode::kDmrShortLcFragments; ++f) {
        std::copy_n(encoded.begin() + static_cast<std::ptrdiff_t>(f * decode::kDmrCachPayloadBits),
                    decode::kDmrCachPayloadBits, out.payload[f].begin());
    }
    // Table 9.20: first, continuation, continuation, last.
    out.lcss = {decode::kDmrLcssFirst, decode::kDmrLcssContinuation, decode::kDmrLcssContinuation,
                decode::kDmrLcssLast};
    return out;
}

Expected<std::vector<DmrBurstBits>> dmr_voice_call_bursts(const DmrVoiceCall& call) {
    if (call.voice.empty() || call.voice.size() % decode::kDmrSuperframeBursts != 0) {
        return fail(std::format(
            "a DMR voice call carries its voice six bursts to a superframe (TS 102 361-1 clause "
            "5.1.2.1); got {} bursts",
            call.voice.size()));
    }
    if (!decode::dmr_sync_is_voice(call.voice_sync)) {
        return fail(std::format("a voice call's A bursts carry a voice sync, and {} is not one",
                                decode::dmr_sync_name(call.voice_sync)));
    }
    // Table 9.2's note: the data sync is the voice sync's complement.
    const DmrSyncType data_sync = [&] {
        switch (call.voice_sync) {
            case DmrSyncType::MsVoice: return DmrSyncType::MsData;
            case DmrSyncType::DirectVoiceSlot1: return DmrSyncType::DirectDataSlot1;
            case DmrSyncType::DirectVoiceSlot2: return DmrSyncType::DirectDataSlot2;
            default: return DmrSyncType::BsData;
        }
    }();

    std::vector<DmrBurstBits> out;
    if (call.header) {
        auto header = dmr_bptc_burst(data_sync, call.colour_code, DmrDataType::VoiceLcHeader, call.lc);
        if (!header) {
            return std::unexpected(with_context(header.error(), "building the voice LC header"));
        }
        out.push_back(*header);
    }

    const auto embedded = decode::dmr_embedded_encode(call.lc);
    // Annex D.1: the Null embedded message is all zeros, FEC included.
    const std::array<std::uint8_t, decode::kDmrEmbeddedFragmentBits> null_message{};
    for (std::size_t b = 0; b < call.voice.size(); ++b) {
        const std::size_t letter = b % decode::kDmrSuperframeBursts;
        if (letter == 0) {
            out.push_back(dmr_voice_burst_with_sync(call.voice[b], call.voice_sync));
            continue;
        }
        if (letter == 5) {
            out.push_back(dmr_voice_burst_embedded(call.voice[b], call.colour_code, false,
                                                   decode::kDmrLcssSingle, null_message));
            continue;
        }
        // Clause 7.1.3: B first, C and D continuation, E last.
        const std::uint8_t lcss = letter == 1   ? decode::kDmrLcssFirst
                                  : letter == 4 ? decode::kDmrLcssLast
                                                : decode::kDmrLcssContinuation;
        const auto fragment = std::span<const std::uint8_t, decode::kDmrEmbeddedFragmentBits>(
            embedded.data() + (letter - 1) * decode::kDmrEmbeddedFragmentBits,
            decode::kDmrEmbeddedFragmentBits);
        out.push_back(dmr_voice_burst_embedded(call.voice[b], call.colour_code, false, lcss, fragment));
    }

    if (call.terminator) {
        auto terminator =
            dmr_bptc_burst(data_sync, call.colour_code, DmrDataType::TerminatorWithLc, call.lc);
        if (!terminator) {
            return std::unexpected(with_context(terminator.error(), "building the terminator"));
        }
        out.push_back(*terminator);
    }
    return out;
}

Expected<std::vector<Complex32>> dmr_render_slots(const DmrModConfig& config, std::span<const DmrSlot> slots) {
    std::vector<std::uint8_t> dibits;
    std::vector<std::uint8_t> mask;
    dibits.reserve(slots.size() * decode::kDmrSlotSymbols);
    mask.reserve(slots.size() * decode::kDmrSlotSymbols);
    const std::array<std::uint8_t, decode::kDmrCachBits> quiet_cach{};
    const DmrBurstBits quiet_burst{};
    for (const DmrSlot& slot : slots) {
        append_dibits(slot.cach ? std::span<const std::uint8_t>(*slot.cach) : quiet_cach,
                      slot.cach.has_value(), dibits, mask);
        append_dibits(slot.burst ? std::span<const std::uint8_t>(*slot.burst) : quiet_burst,
                      slot.burst.has_value(), dibits, mask);
    }
    return render(config, dibits, mask);
}

Expected<std::vector<Complex32>> dmr_render_dibits(const DmrModConfig& config, std::span<const std::uint8_t> dibits) {
    const std::vector<std::uint8_t> on(dibits.size(), 1);
    return render(config, dibits, on);
}

}  // namespace revenant::siggen
