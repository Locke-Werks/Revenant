#include "core/decode/dmr.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace revenant::decode {
namespace {

constexpr double kPi = std::numbers::pi;

// The smallest sync gain, against Table 10.3's deviation, taken as DMR. The
// engineering choice core/decode/p25p1.cpp makes for the same modulation:
// below a tenth, restoring the levels multiplies the noise by more than ten.
constexpr double kMinimumSyncGain = 0.1;

// And the largest. Clause 10.2.2.3 holds a transmitter's deviation to 10 per
// cent either side of nominal; 1.5 is five times that margin, an engineering
// choice. A pattern matched in noise fits whatever gain the noise has: one
// matched in the guard time of a direct mode test at 35 dB fitted 1.76, while
// this decoder still timed its symbols with dv_phy.h's SymbolSync.
constexpr double kMaximumSyncGain = 1.5;

// Grid slots in a row that carried nothing this decoder recognised before
// the grid is dropped and the search starts again. An engineering choice:
// twelve slots is 360 ms, one voice superframe on both timeslots, so a
// transmission that merely paused between superframes keeps its framing and
// one that ended lets the search take over within a superframe's time.
constexpr std::size_t kDropMisses = 12;

// The five patterns whose complements are the other five, Table 9.2's note.
constexpr std::array<DmrSyncType, 5> kBaseSyncs = {
    DmrSyncType::BsVoice, DmrSyncType::MsVoice, DmrSyncType::MsReverseChannel,
    DmrSyncType::DirectVoiceSlot1, DmrSyncType::DirectVoiceSlot2,
};

DmrSyncType complement(DmrSyncType type) {
    switch (type) {
        case DmrSyncType::BsVoice: return DmrSyncType::BsData;
        case DmrSyncType::BsData: return DmrSyncType::BsVoice;
        case DmrSyncType::MsVoice: return DmrSyncType::MsData;
        case DmrSyncType::MsData: return DmrSyncType::MsVoice;
        case DmrSyncType::MsReverseChannel: return DmrSyncType::Reserved;
        case DmrSyncType::Reserved: return DmrSyncType::MsReverseChannel;
        case DmrSyncType::DirectVoiceSlot1: return DmrSyncType::DirectDataSlot1;
        case DmrSyncType::DirectDataSlot1: return DmrSyncType::DirectVoiceSlot1;
        case DmrSyncType::DirectVoiceSlot2: return DmrSyncType::DirectDataSlot2;
        case DmrSyncType::DirectDataSlot2: return DmrSyncType::DirectVoiceSlot2;
    }
    return type;
}

// Nearest of Table 10.3's four levels, as a dibit.
std::uint8_t slice_dibit(double v) {
    if (v >= 2.0) {
        return 0b01;  // +3
    }
    if (v >= 0.0) {
        return 0b00;  // +1
    }
    if (v >= -2.0) {
        return 0b10;  // -1
    }
    return 0b11;  // -3
}

std::uint32_t octets_to_u24(std::span<const std::uint8_t> octets, std::size_t first) {
    return (static_cast<std::uint32_t>(octets[first]) << 16U) |
           (static_cast<std::uint32_t>(octets[first + 1]) << 8U) | octets[first + 2];
}

std::uint32_t bits_to_word(std::span<const std::uint8_t> bits) {
    std::uint32_t word = 0;
    for (const std::uint8_t bit : bits) {
        word = (word << 1U) | (bit & 1U);
    }
    return word;
}

template <std::size_t N>
std::array<std::uint8_t, N> bits_to_octets(std::span<const std::uint8_t> bits) {
    std::array<std::uint8_t, N> out{};
    for (std::size_t i = 0; i < N * 8 && i < bits.size(); ++i) {
        out[i / 8] = static_cast<std::uint8_t>((out[i / 8] << 1U) | (bits[i] & 1U));
    }
    return out;
}

std::array<std::uint8_t, kDmrBptcInformationBits> octets_to_bits(std::span<const std::uint8_t, 12> octets) {
    std::array<std::uint8_t, kDmrBptcInformationBits> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>((octets[i / 8] >> (7U - i % 8)) & 1U);
    }
    return out;
}

// Table B.21's mask for the Data Types that carry a sixteen-bit CRC, or none.
std::optional<std::uint16_t> crc16_mask(std::uint8_t data_type) {
    switch (static_cast<DmrDataType>(data_type)) {
        case DmrDataType::PiHeader: return kDmrMaskPiHeader;
        case DmrDataType::Csbk: return kDmrMaskCsbk;
        case DmrDataType::MbcHeader: return kDmrMaskMbcHeader;
        case DmrDataType::DataHeader: return kDmrMaskDataHeader;
        case DmrDataType::UnifiedSingleBlockData: return kDmrMaskUnifiedSingleBlock;
        default: return std::nullopt;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// The physical layer
// ---------------------------------------------------------------------------

double dmr_rrc_response(double hertz, const void* /*context*/) {
    const double f = std::abs(hertz);
    if (f <= kDmrRrcFlatEdgeHz) {
        return 1.0;
    }
    if (f <= kDmrRrcStopHz) {
        return std::abs(std::cos(kPi * f / kDmrRrcFlatEdgeHz));
    }
    return 0.0;
}

// ---------------------------------------------------------------------------
// Sync patterns and names
// ---------------------------------------------------------------------------

std::uint64_t dmr_sync_bits(DmrSyncType type) {
    switch (type) {
        case DmrSyncType::BsVoice: return kDmrSyncBsVoice;
        case DmrSyncType::BsData: return kDmrSyncBsData;
        case DmrSyncType::MsVoice: return kDmrSyncMsVoice;
        case DmrSyncType::MsData: return kDmrSyncMsData;
        case DmrSyncType::MsReverseChannel: return kDmrSyncMsReverseChannel;
        case DmrSyncType::DirectVoiceSlot1: return kDmrSyncDirectVoiceSlot1;
        case DmrSyncType::DirectDataSlot1: return kDmrSyncDirectDataSlot1;
        case DmrSyncType::DirectVoiceSlot2: return kDmrSyncDirectVoiceSlot2;
        case DmrSyncType::DirectDataSlot2: return kDmrSyncDirectDataSlot2;
        case DmrSyncType::Reserved: return kDmrSyncReserved;
    }
    return 0;
}

std::string_view dmr_sync_name(DmrSyncType type) {
    switch (type) {
        case DmrSyncType::BsVoice: return "bs_voice";
        case DmrSyncType::BsData: return "bs_data";
        case DmrSyncType::MsVoice: return "ms_voice";
        case DmrSyncType::MsData: return "ms_data";
        case DmrSyncType::MsReverseChannel: return "ms_rc";
        case DmrSyncType::DirectVoiceSlot1: return "direct_voice_1";
        case DmrSyncType::DirectDataSlot1: return "direct_data_1";
        case DmrSyncType::DirectVoiceSlot2: return "direct_voice_2";
        case DmrSyncType::DirectDataSlot2: return "direct_data_2";
        case DmrSyncType::Reserved: return "reserved";
    }
    return "unknown";
}

bool dmr_sync_is_voice(DmrSyncType type) {
    return type == DmrSyncType::BsVoice || type == DmrSyncType::MsVoice ||
           type == DmrSyncType::DirectVoiceSlot1 || type == DmrSyncType::DirectVoiceSlot2;
}

bool dmr_sync_is_data(DmrSyncType type) {
    return type == DmrSyncType::BsData || type == DmrSyncType::MsData ||
           type == DmrSyncType::DirectDataSlot1 || type == DmrSyncType::DirectDataSlot2;
}

bool dmr_sync_is_base_station(DmrSyncType type) {
    return type == DmrSyncType::BsVoice || type == DmrSyncType::BsData;
}

std::uint8_t dmr_sync_slot(DmrSyncType type) {
    switch (type) {
        case DmrSyncType::DirectVoiceSlot1:
        case DmrSyncType::DirectDataSlot1: return 1;
        case DmrSyncType::DirectVoiceSlot2:
        case DmrSyncType::DirectDataSlot2: return 2;
        default: return 0;
    }
}

std::array<float, kDmrSyncSymbols> dmr_sync_pattern(DmrSyncType type) {
    const std::uint64_t bits = dmr_sync_bits(type);
    std::array<float, kDmrSyncSymbols> out{};
    for (std::size_t i = 0; i < kDmrSyncSymbols; ++i) {
        const auto dibit = static_cast<std::size_t>((bits >> (kDmrCentreBits - 2 - 2 * i)) & 0x3U);
        out[i] = static_cast<float>(kDmrDibitToSymbol[dibit]);
    }
    return out;
}

std::string_view dmr_data_type_name(std::uint8_t data_type) {
    switch (data_type) {
        case 0b0000: return "pi_header";
        case 0b0001: return "voice_lc_header";
        case 0b0010: return "terminator_with_lc";
        case 0b0011: return "csbk";
        case 0b0100: return "mbc_header";
        case 0b0101: return "mbc_continuation";
        case 0b0110: return "data_header";
        case 0b0111: return "rate_1_2_data";
        case 0b1000: return "rate_3_4_data";
        case 0b1001: return "idle";
        case 0b1010: return "rate_1_data";
        case 0b1011: return "unified_single_block_data";
        default: return "reserved";
    }
}

std::string_view dmr_flco_name(std::uint8_t fid, std::uint8_t flco) {
    if (fid != kDmrStandardFid) {
        return "manufacturer";
    }
    switch (flco) {
        case kDmrFlcoGroupVoice: return "group_voice";
        case kDmrFlcoUnitToUnitVoice: return "unit_to_unit_voice";
        case kDmrFlcoTalkerAliasHeader: return "talker_alias_header";
        case kDmrFlcoTalkerAliasBlock1: return "talker_alias_block_1";
        case kDmrFlcoTalkerAliasBlock2: return "talker_alias_block_2";
        case kDmrFlcoTalkerAliasBlock3: return "talker_alias_block_3";
        case kDmrFlcoGpsInfo: return "gps_info";
        default: return "unknown";
    }
}

std::string_view dmr_csbko_name(std::uint8_t fid, std::uint8_t csbko) {
    if (fid != kDmrStandardFid) {
        return "manufacturer";
    }
    switch (csbko) {
        case kDmrCsbkoUnitToUnitVoiceRequest: return "unit_to_unit_voice_request";
        case kDmrCsbkoUnitToUnitAnswerResponse: return "unit_to_unit_answer_response";
        case kDmrCsbkoChannelTiming: return "channel_timing";
        case kDmrCsbkoNegativeAcknowledge: return "negative_acknowledge";
        case kDmrCsbkoBsOutboundActivation: return "bs_outbound_activation";
        case kDmrCsbkoPreamble: return "preamble";
        default: return "unknown";
    }
}

std::string_view dmr_dpf_name(std::uint8_t dpf) {
    switch (dpf) {
        case kDmrDpfUnifiedDataTransport: return "unified_data_transport";
        case kDmrDpfResponse: return "response";
        case kDmrDpfUnconfirmed: return "unconfirmed";
        case kDmrDpfConfirmed: return "confirmed";
        case kDmrDpfShortDataDefined: return "short_data_defined";
        case kDmrDpfShortDataRawOrStatus: return "short_data_raw_or_status";
        case kDmrDpfProprietary: return "proprietary";
        default: return "reserved";
    }
}

std::string_view dmr_sap_name(std::uint8_t sap) {
    switch (sap) {
        case 0b0000: return "unified_data_transport";
        case 0b0010: return "tcp_ip_header_compression";
        case 0b0011: return "udp_ip_header_compression";
        case 0b0100: return "ip_packet_data";
        case 0b0101: return "arp";
        case 0b1001: return "proprietary";
        case 0b1010: return "short_data";
        default: return "reserved";
    }
}

// ---------------------------------------------------------------------------
// Parsers
// ---------------------------------------------------------------------------

DmrServiceOptions dmr_parse_service_options(std::uint8_t raw) {
    DmrServiceOptions out;
    out.raw = raw;
    out.emergency = (raw & 0x80U) != 0;
    out.privacy = (raw & 0x40U) != 0;
    // Bits 5 and 4 are reserved.
    out.broadcast = (raw & 0x08U) != 0;
    out.open_voice_call_mode = (raw & 0x04U) != 0;
    out.priority = static_cast<std::uint8_t>(raw & 0x03U);
    return out;
}

DmrFullLc dmr_parse_full_lc(std::span<const std::uint8_t, 9> octets, DmrLcCarrier carrier) {
    DmrFullLc lc;
    std::copy(octets.begin(), octets.end(), lc.octets.begin());
    lc.carrier = carrier;
    // Figure 7.1: PF, a reserved bit and the FLCO in octet 0, the FID in 1.
    lc.protect_flag = (octets[0] & 0x80U) != 0;
    lc.flco = static_cast<std::uint8_t>(octets[0] & 0x3FU);
    lc.fid = octets[1];
    if (lc.fid != kDmrStandardFid ||
        (lc.flco != kDmrFlcoGroupVoice && lc.flco != kDmrFlcoUnitToUnitVoice)) {
        return lc;
    }
    lc.service_options = dmr_parse_service_options(octets[2]);
    lc.group = lc.flco == kDmrFlcoGroupVoice;
    lc.destination = octets_to_u24(octets, 3);
    lc.source = octets_to_u24(octets, 6);
    return lc;
}

DmrCsbk dmr_parse_csbk(std::span<const std::uint8_t, 10> octets, bool mbc_header) {
    DmrCsbk csbk;
    std::copy(octets.begin(), octets.end(), csbk.octets.begin());
    csbk.mbc_header = mbc_header;
    // Figure 7.8: LB, PF and the CSBKO in octet 0, the FID in 1.
    csbk.last_block = (octets[0] & 0x80U) != 0;
    csbk.protect_flag = (octets[0] & 0x40U) != 0;
    csbk.opcode = static_cast<std::uint8_t>(octets[0] & 0x3FU);
    csbk.fid = octets[1];
    if (csbk.fid != kDmrStandardFid || mbc_header) {
        return csbk;
    }
    switch (csbk.opcode) {
        // Tables 7.5b and 7.5c: Service Options in octet 2, the target in 4
        // to 6, the source in 7 to 9.
        case kDmrCsbkoUnitToUnitVoiceRequest:
        case kDmrCsbkoUnitToUnitAnswerResponse:
            csbk.service_options = dmr_parse_service_options(octets[2]);
            csbk.target = octets_to_u24(octets, 4);
            csbk.source = octets_to_u24(octets, 7);
            break;
        // Table 7.6 puts the source, as the Additional Information Field,
        // before the target.
        case kDmrCsbkoNegativeAcknowledge:
            csbk.source = octets_to_u24(octets, 4);
            csbk.target = octets_to_u24(octets, 7);
            break;
        // Table 7.5a: sixteen reserved bits, the BS address, the source.
        case kDmrCsbkoBsOutboundActivation:
            csbk.target = octets_to_u24(octets, 4);
            csbk.source = octets_to_u24(octets, 7);
            break;
        // Table 7.7.
        case kDmrCsbkoPreamble:
            csbk.preamble_data_follows = (octets[2] & 0x80U) != 0;
            csbk.group = (octets[2] & 0x40U) != 0;
            csbk.blocks_to_follow = octets[3];
            csbk.target = octets_to_u24(octets, 4);
            csbk.source = octets_to_u24(octets, 7);
            break;
        default: break;
    }
    return csbk;
}

DmrDataHeader dmr_parse_data_header(std::span<const std::uint8_t, 10> octets, bool proprietary_second) {
    DmrDataHeader header;
    std::copy(octets.begin(), octets.end(), header.octets.begin());
    header.proprietary_second = proprietary_second;
    if (proprietary_second) {
        // Figure 8.6: SAP and DPF in octet 0, the MFID in octet 1, and eight
        // octets the manufacturer defines.
        header.sap = static_cast<std::uint8_t>(octets[0] >> 4U);
        header.format = static_cast<std::uint8_t>(octets[0] & 0x0FU);
        header.manufacturer_id = octets[1];
        return header;
    }
    // Figures 8.3 to 8.10: G/I and A in octet 0's top bits, the DPF in its
    // low four, the SAP in octet 1's top four, the destination LLID in
    // octets 2 to 4 and the source in 5 to 7.
    header.group = (octets[0] & 0x80U) != 0;
    header.response_requested = (octets[0] & 0x40U) != 0;
    header.format = static_cast<std::uint8_t>(octets[0] & 0x0FU);
    header.sap = static_cast<std::uint8_t>(octets[1] >> 4U);
    header.destination = octets_to_u24(octets, 2);
    header.source = octets_to_u24(octets, 5);
    if (header.format == kDmrDpfResponse || header.format == kDmrDpfUnconfirmed ||
        header.format == kDmrDpfConfirmed) {
        header.blocks_to_follow = static_cast<std::uint8_t>(octets[8] & 0x7FU);
    }
    return header;
}

// ---------------------------------------------------------------------------
// Burst codecs
// ---------------------------------------------------------------------------

std::array<std::uint8_t, 2 * kDmrSlotTypeHalfBits> dmr_slot_type_bits(std::uint8_t colour_code,
                                                                      std::uint8_t data_type) {
    const std::uint32_t information =
        (static_cast<std::uint32_t>(colour_code & 0x0FU) << 4U) | (data_type & 0x0FU);
    const std::uint32_t word = dmr_block_encode(kDmrGolay20, information);
    std::array<std::uint8_t, 2 * kDmrSlotTypeHalfBits> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>((word >> (out.size() - 1 - i)) & 1U);
    }
    return out;
}

std::array<std::uint8_t, 2 * kDmrEmbHalfBits> dmr_emb_bits(std::uint8_t colour_code, bool pi,
                                                           std::uint8_t lcss) {
    const std::uint32_t information = (static_cast<std::uint32_t>(colour_code & 0x0FU) << 3U) |
                                       (pi ? 0x4U : 0x0U) | (lcss & 0x3U);
    const std::uint32_t word = dmr_block_encode(kDmrQr16, information);
    std::array<std::uint8_t, 2 * kDmrEmbHalfBits> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>((word >> (out.size() - 1 - i)) & 1U);
    }
    return out;
}

Expected<std::array<std::uint8_t, kDmrBptcInformationBits>> dmr_data_block_bits(
    DmrDataType data_type, std::span<const std::uint8_t> octets) {
    std::array<std::uint8_t, 12> block{};
    const auto type = static_cast<std::uint8_t>(data_type);
    if (data_type == DmrDataType::VoiceLcHeader || data_type == DmrDataType::TerminatorWithLc) {
        if (octets.size() != 9) {
            return fail(std::format("a {} carries the nine Full LC octets of figure 7.1; got {}",
                                    dmr_data_type_name(type), octets.size()));
        }
        std::copy(octets.begin(), octets.end(), block.begin());
        const std::array<std::uint8_t, 3> parity = dmr_rs_parity(std::span<const std::uint8_t, 9>(block.data(), 9));
        const std::uint32_t mask = data_type == DmrDataType::VoiceLcHeader ? kDmrMaskVoiceLcHeader
                                                                           : kDmrMaskTerminatorWithLc;
        block[9] = static_cast<std::uint8_t>(parity[0] ^ ((mask >> 16U) & 0xFFU));
        block[10] = static_cast<std::uint8_t>(parity[1] ^ ((mask >> 8U) & 0xFFU));
        block[11] = static_cast<std::uint8_t>(parity[2] ^ (mask & 0xFFU));
        return octets_to_bits(block);
    }
    const std::optional<std::uint16_t> mask = crc16_mask(type);
    if (!mask) {
        return fail(std::format("a {} is not a BPTC block this transmitter builds from octets",
                                dmr_data_type_name(type)));
    }
    if (octets.size() != 10) {
        return fail(std::format("a {} carries ten octets ahead of its CRC; got {}",
                                dmr_data_type_name(type), octets.size()));
    }
    std::copy(octets.begin(), octets.end(), block.begin());
    const auto crc = static_cast<std::uint16_t>(dmr_crc_ccitt(octets) ^ *mask);
    block[10] = static_cast<std::uint8_t>(crc >> 8U);
    block[11] = static_cast<std::uint8_t>(crc & 0xFFU);
    return octets_to_bits(block);
}

std::array<std::uint8_t, kDmrBptcInformationBits> dmr_idle_bits() {
    // Table D.2, I(95) to I(0), read as twelve octets.
    constexpr std::array<std::uint8_t, 12> kIdle = {0xFF, 0x83, 0xDF, 0x17, 0x32, 0x09,
                                                    0x4E, 0xD1, 0xE7, 0xCD, 0x8A, 0x91};
    return octets_to_bits(kIdle);
}

// ---------------------------------------------------------------------------
// Sync scoring
// ---------------------------------------------------------------------------

DmrSyncScore dmr_sync_score(std::span<const float> symbols, double threshold) {
    DmrSyncScore out;
    if (symbols.size() < kDmrSyncSymbols) {
        return out;
    }
    std::array<std::array<float, kDmrSyncSymbols>, kBaseSyncs.size()> patterns{};
    for (std::size_t p = 0; p < kBaseSyncs.size(); ++p) {
        patterns[p] = dmr_sync_pattern(kBaseSyncs[p]);
    }
    std::vector<std::size_t> hits;
    for (std::size_t offset = 0; offset + kDmrSyncSymbols <= symbols.size(); ++offset) {
        double best = 0.0;
        std::size_t which = 0;
        for (std::size_t p = 0; p < patterns.size(); ++p) {
            const double score = centred_correlation_at(symbols, patterns[p], offset);
            if (std::abs(score) > std::abs(best)) {
                best = score;
                which = p;
            }
        }
        if (std::abs(best) >= threshold) {
            hits.push_back(offset);
        }
        if (std::abs(best) > out.score) {
            out.score = std::abs(best);
            out.type = best > 0.0 ? kBaseSyncs[which] : complement(kBaseSyncs[which]);
            out.position = offset;
        }
    }
    out.hits = hits.size();
    for (const std::size_t hit : hits) {
        const std::size_t distance = hit > out.position ? hit - out.position : out.position - hit;
        const std::size_t phase = distance % kDmrSlotSymbols;
        if (distance != 0 && (phase <= 1 || phase + 1 >= kDmrSlotSymbols)) {
            ++out.hits_on_slot_grid;
        }
    }
    return out;
}

Expected<DmrSyncScore> dmr_sync_score(ConstComplexSpan baseband, SampleRate rate) {
    DmrConfig config;
    config.rate = rate;
    auto decoder = Dmr::create(config);
    if (!decoder) {
        return std::unexpected(with_context(decoder.error(), "scoring DMR sync"));
    }
    decoder->collect_hits_ = true;
    std::vector<DmrBurst> ignored;
    if (auto status = decoder->process(baseband, ignored); !status) {
        return std::unexpected(with_context(status.error(), "scoring DMR sync"));
    }
    DmrSyncScore out = decoder->seen_;
    const double slot = static_cast<double>(kDmrSlotSymbols) * decoder->samples_per_symbol_;

    // One sync, one hit. The search records the sync it starts the slot grid
    // on, and the grid's first read_slot lands on the same instant and records
    // it again, so without this the grid count came out one high whenever the
    // best sync was not the first, and identify's three-sync rule passed on
    // two. Duplicates are hits within a symbol of each other.
    std::vector<double> hits = decoder->hits_;
    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end(),
                           [&](double a, double b) {
                               return b - a <= decoder->samples_per_symbol_;
                           }),
               hits.end());

    out.hits = hits.size();
    for (const double hit : hits) {
        const double distance = std::abs(hit - decoder->best_time_);
        const double phase = std::fmod(distance, slot);
        if (distance > decoder->samples_per_symbol_ &&
            (phase <= decoder->samples_per_symbol_ || slot - phase <= decoder->samples_per_symbol_)) {
            ++out.hits_on_slot_grid;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

Expected<Dmr> Dmr::create(const DmrConfig& config) {
    if (config.rate <= 0) {
        return fail(std::format("Dmr needs a positive sample rate; got {}", config.rate));
    }
    const double samples_per_symbol = static_cast<double>(config.rate) / kDmrSymbolRate;
    if (samples_per_symbol < 2.0) {
        return fail(std::format(
            "DMR is 4800 symbols per second (TS 102 361-1 clause 10.2.1), so a rate of {} Hz "
            "gives {:.3f} samples per symbol, below the two a sync's peak can be found between. "
            "9600 Hz is the floor and 48000 is the usual choice",
            config.rate, samples_per_symbol));
    }
    if (config.sync_threshold <= 0.0 || config.sync_threshold > 1.0 ||
        config.tracking_threshold <= 0.0 || config.tracking_threshold > 1.0) {
        return fail(std::format("Dmr needs thresholds in (0, 1]; got {} and {}",
                                config.sync_threshold, config.tracking_threshold));
    }
    if (config.filter_taps % 2 == 0) {
        return fail(std::format("Dmr needs an odd number of filter taps; got {}", config.filter_taps));
    }

    auto taps = design_from_response(config.rate, config.filter_taps, dmr_rrc_response, nullptr);
    if (!taps) {
        return std::unexpected(
            with_context(taps.error(), "designing the clause 10.2.2.2 receive filter"));
    }
    auto filter = RealFir::create(std::move(*taps));
    if (!filter) {
        return std::unexpected(with_context(filter.error(), "building the DMR receive filter"));
    }
    auto discriminator = FmDiscriminator::create(config.rate);
    if (!discriminator) {
        return std::unexpected(
            with_context(discriminator.error(), "building the DMR frequency detector"));
    }

    Dmr decoder;
    decoder.config_ = config;
    decoder.discriminator_ = *discriminator;
    decoder.filter_ = std::move(*filter);
    decoder.samples_per_symbol_ = samples_per_symbol;
    decoder.filter_delay_ = static_cast<double>(config.filter_taps - 1) / 2.0;
    return decoder;
}

void Dmr::reset() {
    discriminator_.reset();
    filter_.reset();
    shaped_.clear();
    trimmed_ = 0;
    search_ = 0;
    grid_.reset();
    lanes_ = {};
    seen_ = {};
    best_time_ = 0.0;
    hits_.clear();
}

double Dmr::value_at(double time) const {
    const double whole = std::floor(time);
    const auto i = static_cast<std::size_t>(whole) - trimmed_;
    const double f = time - whole;
    if (f == 0.0) {
        return static_cast<double>(shaped_[i]);
    }
    // Catmull-Rom through the four samples around the point.
    const double p0 = shaped_[i - 1];
    const double p1 = shaped_[i];
    const double p2 = shaped_[i + 1];
    const double p3 = shaped_[i + 2];
    return p1 + 0.5 * f *
                    (p2 - p0 + f * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + f * (3.0 * (p1 - p2) + p3 - p0)));
}

void Dmr::symbols_at(double time, std::span<float> out) const {
    for (std::size_t k = 0; k < out.size(); ++k) {
        out[k] = static_cast<float>(value_at(time + static_cast<double>(k) * samples_per_symbol_));
    }
}

std::optional<Dmr::SyncFound> Dmr::score_at(double time) const {
    static const std::array<std::array<float, kDmrSyncSymbols>, kBaseSyncs.size()> patterns = [] {
        std::array<std::array<float, kDmrSyncSymbols>, kBaseSyncs.size()> out{};
        for (std::size_t p = 0; p < kBaseSyncs.size(); ++p) {
            out[p] = dmr_sync_pattern(kBaseSyncs[p]);
        }
        return out;
    }();
    std::array<float, kDmrSyncSymbols> values{};
    symbols_at(time, values);
    double best = 0.0;
    std::size_t which = 0;
    for (std::size_t p = 0; p < patterns.size(); ++p) {
        const double score = centred_correlation_at(values, patterns[p], 0);
        if (std::abs(score) > std::abs(best)) {
            best = score;
            which = p;
        }
    }
    // Table 9.2's note: the sign of a single correlator tells voice from data.
    SyncFound found;
    found.type = best > 0.0 ? kBaseSyncs[which] : complement(kBaseSyncs[which]);
    found.score = std::abs(best);
    found.time = time;
    return found;
}

std::optional<Dmr::SyncFound> Dmr::peak_near(double time, std::size_t reach, double threshold) const {
    const auto span = static_cast<std::ptrdiff_t>(reach);
    std::vector<double> scores(2 * reach + 3, 0.0);
    // One more either side than the reach, for the refinement.
    for (std::ptrdiff_t j = -span - 1; j <= span + 1; ++j) {
        scores[static_cast<std::size_t>(j + span + 1)] = score_at(time + static_cast<double>(j))->score;
    }
    std::ptrdiff_t best = 0;
    for (std::ptrdiff_t j = -span; j <= span; ++j) {
        if (scores[static_cast<std::size_t>(j + span + 1)] > scores[static_cast<std::size_t>(best + span + 1)]) {
            best = j;
        }
    }
    const double b = scores[static_cast<std::size_t>(best + span + 1)];
    if (b < threshold) {
        return std::nullopt;
    }
    // A parabola through the peak and its neighbours puts the symbol instant
    // between samples.
    const double a = scores[static_cast<std::size_t>(best + span)];
    const double c = scores[static_cast<std::size_t>(best + span + 2)];
    const double curvature = a - 2.0 * b + c;
    double delta = 0.0;
    if (curvature < 0.0) {
        delta = std::clamp(0.5 * (a - c) / curvature, -0.5, 0.5);
    }
    auto found = score_at(time + static_cast<double>(best) + delta);
    if (found->score < threshold) {
        found = score_at(time + static_cast<double>(best));
    }
    return found;
}

void Dmr::read_bits(double time, std::size_t symbols, const LevelFit& fit,
                    std::vector<std::uint8_t>& bits) const {
    bits.clear();
    bits.reserve(symbols * 2);
    for (std::size_t k = 0; k < symbols; ++k) {
        const double value = value_at(time + static_cast<double>(k) * samples_per_symbol_);
        // Annex E: Bit 1 of each dibit, then Bit 0.
        const std::uint8_t dibit = slice_dibit((value - fit.level) / fit.gain);
        bits.push_back(static_cast<std::uint8_t>((dibit >> 1U) & 1U));
        bits.push_back(static_cast<std::uint8_t>(dibit & 1U));
    }
}

void Dmr::start_grid(double start) {
    grid_ = Grid{};
    grid_->anchor = start;
    lanes_ = {};
}

void Dmr::note_sync(const SyncFound& found) {
    if (found.score > seen_.score) {
        seen_.score = found.score;
        seen_.type = found.type;
        seen_.position = static_cast<std::size_t>(std::max(0.0, std::round(found.time - filter_delay_)));
        best_time_ = found.time;
    }
    if (collect_hits_ && found.score >= config_.sync_threshold) {
        hits_.push_back(found.time);
    }
}

void Dmr::read_cach(double start, const LevelFit& fit, DmrBurst& burst) {
    std::vector<std::uint8_t> bits;
    read_bits(start - static_cast<double>(kDmrCachSymbols) * samples_per_symbol_, kDmrCachSymbols, fit,
              bits);
    const DmrCachBits raw = dmr_cach_deinterleave(std::span<const std::uint8_t, kDmrCachBits>(bits.data(), kDmrCachBits));
    const DmrBlockDecode tact = dmr_block_decode(kDmrHamming7, raw.tact);

    // Table 9.5: AT, TC and LCSS, most significant first.
    DmrCach cach;
    cach.access_busy = ((tact.information >> 3U) & 1U) != 0;
    cach.channel = static_cast<std::uint8_t>(((tact.information >> 2U) & 1U) + 1U);
    cach.lcss = static_cast<std::uint8_t>(tact.information & 0x3U);
    cach.tact_corrected = tact.distance;
    cach.payload = raw.payload;
    burst.cach = cach;

    // Clause 7.1.4 and figure B.6: four CACH payloads, first, two
    // continuations and last, make one Short LC. Table 9.20's note: there is
    // no single fragment Short LC.
    Grid& grid = *grid_;
    const auto place = [&](std::size_t fragment) {
        std::copy(raw.payload.begin(), raw.payload.end(),
                  grid.short_lc.begin() + static_cast<std::ptrdiff_t>(fragment * kDmrCachPayloadBits));
    };
    switch (cach.lcss) {
        case kDmrLcssFirst:
            place(0);
            grid.short_lc_fragments = 1;
            break;
        case kDmrLcssContinuation:
            if (grid.short_lc_fragments == 1 || grid.short_lc_fragments == 2) {
                place(grid.short_lc_fragments);
                ++grid.short_lc_fragments;
            } else {
                grid.short_lc_fragments = 0;
            }
            break;
        case kDmrLcssLast:
            if (grid.short_lc_fragments == 3) {
                place(3);
                const DmrShortLcDecode decoded = dmr_short_lc_decode(grid.short_lc);
                if (decoded.crc_valid) {
                    DmrShortLc lc;
                    lc.slco = static_cast<std::uint8_t>(bits_to_word(std::span(decoded.lc).subspan(0, 4)));
                    lc.data = bits_to_word(std::span(decoded.lc).subspan(4, 24));
                    lc.corrected = decoded.corrected;
                    if (lc.slco == kDmrSlcoActivityUpdate) {
                        // TS 102 361-2 Table 7.10.
                        lc.activity = std::array<std::uint8_t, 2>{
                            static_cast<std::uint8_t>((lc.data >> 20U) & 0xFU),
                            static_cast<std::uint8_t>((lc.data >> 16U) & 0xFU)};
                        lc.hashed_address = std::array<std::uint8_t, 2>{
                            static_cast<std::uint8_t>((lc.data >> 8U) & 0xFFU),
                            static_cast<std::uint8_t>(lc.data & 0xFFU)};
                    }
                    burst.short_lc = lc;
                }
            }
            grid.short_lc_fragments = 0;
            break;
        default: grid.short_lc_fragments = 0; break;
    }
}

void Dmr::read_data(const std::vector<std::uint8_t>& bits, DmrBurst& burst, Lane& lane) {
    // Figure 6.5 and Table E.1: the slot type's first ten bits end the first
    // half of the burst and its last ten begin the second.
    std::array<std::uint8_t, 2 * kDmrSlotTypeHalfBits> slot_type{};
    std::copy_n(bits.begin() + kDmrInfoHalfBits, kDmrSlotTypeHalfBits, slot_type.begin());
    std::copy_n(bits.begin() + kDmrCentreFirstBit + kDmrCentreBits, kDmrSlotTypeHalfBits,
                slot_type.begin() + kDmrSlotTypeHalfBits);
    const DmrBlockDecode decoded = dmr_block_decode(kDmrGolay20, bits_to_word(slot_type));
    if (decoded.detected) {
        return;
    }
    const auto colour_code = static_cast<std::uint8_t>((decoded.information >> 4U) & 0x0FU);
    const auto data_type = static_cast<std::uint8_t>(decoded.information & 0x0FU);
    burst.colour_code = colour_code;
    burst.data_type = data_type;
    burst.slot_type_corrected = decoded.distance;

    std::array<std::uint8_t, kDmrBptcBits> info{};
    std::copy_n(bits.begin(), kDmrInfoHalfBits, info.begin());
    std::copy_n(bits.begin() + kDmrCentreFirstBit + kDmrCentreBits + kDmrSlotTypeHalfBits,
                kDmrInfoHalfBits, info.begin() + kDmrInfoHalfBits);
    burst.payload.assign(info.begin(), info.end());

    const auto type = static_cast<DmrDataType>(data_type);
    const bool full_lc = type == DmrDataType::VoiceLcHeader || type == DmrDataType::TerminatorWithLc;
    const std::optional<std::uint16_t> mask = crc16_mask(data_type);
    if (type == DmrDataType::TerminatorWithLc) {
        // Clause 5.1.2.3: the data burst after a superframe ends the speech
        // item.
        lane.voice = false;
    }
    if (!full_lc && !mask) {
        // Idle, the data continuations, MBC continuation and the reserved
        // values: reported by type and not decoded.
        return;
    }

    const DmrBptcDecode bptc = dmr_bptc196_decode(info);
    burst.bptc_corrected = bptc.corrected;
    const std::array<std::uint8_t, 12> block = bits_to_octets<12>(bptc.information);

    if (full_lc) {
        // Figures 7.3 and 7.4 and B.3.6: the three parity octets carry the
        // Table B.21 mask, and come off before the Reed-Solomon check.
        const std::uint32_t lc_mask =
            type == DmrDataType::VoiceLcHeader ? kDmrMaskVoiceLcHeader : kDmrMaskTerminatorWithLc;
        std::array<std::uint8_t, 12> word = block;
        word[9] ^= static_cast<std::uint8_t>((lc_mask >> 16U) & 0xFFU);
        word[10] ^= static_cast<std::uint8_t>((lc_mask >> 8U) & 0xFFU);
        word[11] ^= static_cast<std::uint8_t>(lc_mask & 0xFFU);
        const DmrRsDecode rs = dmr_rs_decode(word);
        if (!rs.decoded) {
            burst.payload_failed = true;
            return;
        }
        DmrFullLc lc = dmr_parse_full_lc(std::span<const std::uint8_t, 9>(rs.codeword.data(), 9),
                                         type == DmrDataType::VoiceLcHeader ? DmrLcCarrier::VoiceHeader
                                                                            : DmrLcCarrier::Terminator);
        lc.corrected = rs.corrected;
        burst.full_lc = lc;
        return;
    }

    // Clauses 7.2.1 and 8.2.1.0: ten octets and a CRC-CCITT with its mask.
    const auto received = static_cast<std::uint16_t>(((block[10] << 8U) | block[11]) ^ *mask);
    if (received != dmr_crc_ccitt(std::span<const std::uint8_t>(block.data(), 10))) {
        burst.payload_failed = true;
        return;
    }
    const std::span<const std::uint8_t, 10> octets(block.data(), 10);
    switch (type) {
        case DmrDataType::PiHeader: burst.pi_header = true; break;
        case DmrDataType::Csbk: burst.csbk = dmr_parse_csbk(octets, false); break;
        case DmrDataType::MbcHeader: burst.csbk = dmr_parse_csbk(octets, true); break;
        case DmrDataType::DataHeader: {
            const DmrDataHeader header = dmr_parse_data_header(octets, lane.proprietary_expected);
            // Clause 8.2.1.4: a first header whose SAP is 1001 has a second.
            lane.proprietary_expected = !header.proprietary_second && header.sap == kDmrSapProprietary;
            burst.data_header = header;
            break;
        }
        default: break;
    }
}

bool Dmr::read_slot(double start, std::size_t lane_index, std::vector<DmrBurst>& out) {
    Lane& lane = lanes_[lane_index];
    Lane& other = lanes_[1 - lane_index];
    const bool in_superframe = lane.voice && lane.letter + 1 < kDmrSuperframeBursts;
    const double sps = samples_per_symbol_;

    // A sync where the grid says the centre field is, within a quarter of a
    // symbol, which is more than the phase moves between syncs. Inside a
    // superframe the search threshold applies, because bursts B to F carry
    // embedded signalling there and a looser one could read it as a sync.
    const auto reach = static_cast<std::size_t>(std::ceil(sps / 4.0));
    std::optional<SyncFound> found =
        peak_near(start + static_cast<double>(kDmrCentreFirstSymbol) * sps, reach,
                  in_superframe ? config_.sync_threshold : config_.tracking_threshold);

    std::optional<LevelFit> fit;
    if (found) {
        std::array<float, kDmrSyncSymbols> values{};
        symbols_at(found->time, values);
        auto measured = fit_levels(values, dmr_sync_pattern(found->type), 0);
        if (measured && measured->gain > kMinimumSyncGain && measured->gain < kMaximumSyncGain) {
            fit = *measured;
        } else {
            found.reset();
        }
    }

    DmrBurst burst;
    if (found) {
        note_sync(*found);
        start = found->time - static_cast<double>(kDmrCentreFirstSymbol) * sps;
        // Re-anchor on every sync, so the grid follows the transmitter.
        grid_->anchor = start;
        grid_->lane_of_anchor = lane_index;
        grid_->next = 0;
        if (found->type != DmrSyncType::MsReverseChannel && found->type != DmrSyncType::Reserved) {
            grid_->base_station = dmr_sync_is_base_station(found->type);
        }
        lane.fit = *fit;
        lane.have_fit = true;
        lane.sync_score = found->score;
        burst.sync = found->type;
        if (const std::uint8_t slot = dmr_sync_slot(found->type); slot != 0) {
            lane.slot = slot;
            other.slot = static_cast<std::uint8_t>(3 - slot);
        }
    } else if (in_superframe && lane.have_fit) {
        fit = lane.fit;
    } else {
        // Nothing is due here, or a superframe ended without its next burst
        // A: the speech item is over as far as this slot can tell.
        lane.voice = false;
        return false;
    }

    burst.first_sample = static_cast<std::size_t>(std::max(0.0, std::round(start - filter_delay_)));
    burst.sync_score = lane.sync_score;
    burst.carrier_offset_hz = fit->level * kDmrDeviationPerSymbolUnitHz;
    burst.deviation_ratio = fit->gain;

    // Clause 4.5: on the outbound channel the CACH precedes every burst and
    // names the burst after it.
    if (grid_->base_station &&
        start - static_cast<double>(kDmrCachSymbols) * sps >= static_cast<double>(trimmed_) + 2.0) {
        read_cach(start, *fit, burst);
        if (burst.cach) {
            lane.slot = burst.cach->channel;
            other.slot = static_cast<std::uint8_t>(3 - burst.cach->channel);
        }
    }

    std::vector<std::uint8_t> bits;
    read_bits(start, kDmrBurstSymbols, *fit, bits);
    const auto voice_payload = [&] {
        burst.payload.assign(bits.begin(), bits.begin() + kDmrCentreFirstBit);
        burst.payload.insert(burst.payload.end(), bits.begin() + kDmrCentreFirstBit + kDmrCentreBits,
                             bits.end());
    };

    if (found && dmr_sync_is_voice(found->type)) {
        // Clause 5.1.2.1: burst A.
        lane.voice = true;
        lane.letter = 0;
        lane.fragments = 0;
        burst.voice_burst = 1;
        voice_payload();
    } else if (found && dmr_sync_is_data(found->type)) {
        lane.voice = false;
        read_data(bits, burst, lane);
    } else if (found) {
        // The standalone RC burst and the reserved pattern: recognised and
        // not read.
    } else {
        // Bursts B to F: the EMB either side of 32 bits of embedded
        // signalling, figure 6.4 and Table E.6.
        ++lane.letter;
        burst.voice_burst = static_cast<std::uint8_t>(lane.letter + 1);
        std::array<std::uint8_t, 2 * kDmrEmbHalfBits> emb_bits{};
        std::copy_n(bits.begin() + kDmrCentreFirstBit, kDmrEmbHalfBits, emb_bits.begin());
        std::copy_n(bits.begin() + kDmrCentreFirstBit + kDmrEmbHalfBits + kDmrEmbeddedFragmentBits,
                    kDmrEmbHalfBits, emb_bits.begin() + kDmrEmbHalfBits);
        const DmrBlockDecode emb = dmr_block_decode(kDmrQr16, bits_to_word(emb_bits));
        if (emb.detected) {
            // Not this transmission's burst, or too damaged to say. The
            // letter still moves, so the superframe stays where it was.
            lane.fragments = 0;
            return false;
        }
        DmrEmb decoded;
        decoded.colour_code = static_cast<std::uint8_t>((emb.information >> 3U) & 0x0FU);
        decoded.pi = ((emb.information >> 2U) & 1U) != 0;
        decoded.lcss = static_cast<std::uint8_t>(emb.information & 0x3U);
        decoded.corrected = emb.distance;
        burst.emb = decoded;
        burst.colour_code = decoded.colour_code;
        voice_payload();

        // Clause 7.1.3: the LC starts on burst B and all four fragments are
        // in one superframe, framed by LCSS.
        const auto fragment = std::span(bits).subspan(kDmrCentreFirstBit + kDmrEmbHalfBits,
                                                      kDmrEmbeddedFragmentBits);
        const auto place = [&](std::size_t index) {
            std::copy(fragment.begin(), fragment.end(),
                      lane.embedded.begin() + static_cast<std::ptrdiff_t>(index * kDmrEmbeddedFragmentBits));
        };
        switch (decoded.lcss) {
            case kDmrLcssFirst:
                place(0);
                lane.fragments = 1;
                break;
            case kDmrLcssContinuation:
                if (lane.fragments == 1 || lane.fragments == 2) {
                    place(lane.fragments);
                    ++lane.fragments;
                } else {
                    lane.fragments = 0;
                }
                break;
            case kDmrLcssLast:
                if (lane.fragments == 3) {
                    place(3);
                    const DmrEmbeddedDecode lc = dmr_embedded_decode(lane.embedded);
                    if (lc.clean && lc.checksum_valid) {
                        DmrFullLc full = dmr_parse_full_lc(lc.lc, DmrLcCarrier::Embedded);
                        full.corrected = lc.corrected;
                        burst.full_lc = full;
                    } else {
                        burst.payload_failed = true;
                    }
                }
                lane.fragments = 0;
                break;
            default:
                // A single fragment: the Null embedded message or RC, Annex
                // D.1 and clause 7.1.3.1. Not LC.
                break;
        }
    }

    burst.slot = lane.slot;
    out.push_back(std::move(burst));
    return true;
}

Status Dmr::process(ConstComplexSpan samples, std::vector<DmrBurst>& out) {
    if (samples.empty()) {
        return {};
    }

    // Both stages carry their state across calls, the lesson
    // core/decode/dv_phy.h records from P25, and everything after them is
    // indexed by stream sample, so a stream gives the same bursts however it
    // is split.
    discriminated_.resize(samples.size());
    if (auto status = discriminator_.process(samples, discriminated_); !status) {
        return std::unexpected(with_context(status.error(), "DMR frequency discrimination"));
    }
    filtered_.resize(discriminated_.size());
    if (auto status = filter_.process(discriminated_, filtered_); !status) {
        return std::unexpected(with_context(status.error(), "DMR receive filtering"));
    }

    // Scaled so a Table 10.3 symbol comes out at its symbol-column value. No
    // carrier offset is removed: every burst's sync measures its own.
    //
    // NOT LIMITED, which the D-STAR and POCSAG paths are, and the difference
    // is measured. Their limits protect a symbol timing estimate that weighs
    // every sample by its energy, and this decoder has none. A limit at five
    // symbol units, 3240 Hz, was tried on 2026-09-23 and bought nothing: 28
    // CSBKs of 60 at 16 dB in 2500 Hz against 27 without, and 2 bursts in a
    // minute of noise against none. It cost a raw tap everything, since a
    // carrier 5 kHz off DC sits past it: 0 CSBKs of 40 at 30 dB through the
    // engine, against all 40 without.
    const double sign = config_.invert ? -1.0 : 1.0;
    const auto scale = static_cast<float>(sign / kDmrDeviationPerSymbolUnitHz);
    for (const float value : filtered_) {
        shaped_.push_back(value * scale);
    }

    const double sps = samples_per_symbol_;
    const double slot = static_cast<double>(kDmrSlotSymbols) * sps;
    const double to_centre = static_cast<double>(kDmrCentreFirstSymbol) * sps;
    // Samples a burst needs after its first symbol, with the interpolator's
    // two and the sync peak's reach on top.
    const double burst_span = static_cast<double>(kDmrBurstSymbols) * sps + sps + 3.0;

    // The search looks at every tenth of a symbol, not every sample. At
    // 48000 S/s that is every sample; on a raw tap at 192000 it is every
    // fourth, where scoring every sample made this the costliest adapter on
    // the tap, 127.2 ms of a core per second of input on 2026-09-23 against
    // 76.9 with the stride and 64.4 for P25 in the same run. A tenth of
    // a symbol off the peak still scores near it, and a trigger nine tenths
    // of the threshold sends anything close to peak_near, which looks at
    // every sample either side and applies the threshold itself.
    const auto stride = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(sps / 10.0)));
    const double trigger = stride > 1 ? 0.9 * config_.sync_threshold : config_.sync_threshold;

    while (true) {
        const auto end = static_cast<double>(trimmed_ + shaped_.size());
        const double search_start = static_cast<double>(search_) - to_centre;

        if (grid_) {
            const double due = grid_->anchor + static_cast<double>(grid_->next) * slot;
            if (due <= search_start + sps) {
                if (due + burst_span > end) {
                    break;
                }
                const std::size_t lane = (grid_->lane_of_anchor + grid_->next) % 2;
                const bool recognised = due >= static_cast<double>(trimmed_) + 2.0 && read_slot(due, lane, out);
                ++grid_->next;
                grid_->misses = recognised ? 0 : grid_->misses + 1;
                if (grid_->misses >= kDropMisses) {
                    grid_.reset();
                    lanes_ = {};
                }
                continue;
            }
        }

        if (search_start + burst_span + sps > end) {
            break;
        }
        // Bursts starting before the stream did cannot be read.
        if (search_start < static_cast<double>(trimmed_) + 2.0) {
            search_ += stride;
            continue;
        }
        // A position the grid already reads is left to it.
        if (grid_) {
            double phase = std::fmod(search_start - grid_->anchor, slot);
            if (phase < 0.0) {
                phase += slot;
            }
            if (phase <= 3.0 * sps || slot - phase <= 3.0 * sps) {
                search_ += stride;
                continue;
            }
        }
        const auto here = score_at(static_cast<double>(search_));
        if (here->score < trigger) {
            note_sync(*here);
            search_ += stride;
            continue;
        }
        // Past the threshold: the peak is somewhere in the next symbol.
        const auto half = static_cast<std::size_t>(std::ceil(sps / 2.0));
        const auto peak = peak_near(static_cast<double>(search_ + half), half, config_.sync_threshold);
        if (peak) {
            note_sync(*peak);
            // A sync off the grid starts framing again, unless the grid is
            // reading bursts: a new transmission at another phase can only
            // begin once the old one has stopped producing them.
            if (!grid_ || grid_->misses > 0) {
                start_grid(peak->time - to_centre);
            }
        }
        search_ += 2 * half + 1;
    }

    // Trim what nothing can read again: the search's burst and CACH, and the
    // grid's anchor and CACH, whichever is earlier, with the interpolator's
    // margin.
    double keep = static_cast<double>(search_) - to_centre;
    if (grid_) {
        keep = std::min(keep, grid_->anchor);
    }
    keep -= static_cast<double>(kDmrCachSymbols) * sps + 4.0;
    if (keep > static_cast<double>(trimmed_)) {
        const auto drop = std::min(static_cast<std::size_t>(keep) - trimmed_, shaped_.size());
        shaped_.erase(shaped_.begin(), shaped_.begin() + static_cast<std::ptrdiff_t>(drop));
        trimmed_ += drop;
    }
    return {};
}

}  // namespace revenant::decode
