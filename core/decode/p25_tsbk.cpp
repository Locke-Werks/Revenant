#include "core/decode/p25_tsbk.h"

#include <algorithm>

namespace revenant::decode {
namespace {

std::uint16_t u16(std::uint8_t high, std::uint8_t low) {
    return static_cast<std::uint16_t>((high << 8U) | low);
}

std::uint32_t u24(std::uint8_t a, std::uint8_t b, std::uint8_t c) {
    return (static_cast<std::uint32_t>(a) << 16U) | (static_cast<std::uint32_t>(b) << 8U) | c;
}

std::uint32_t u32(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    return (static_cast<std::uint32_t>(a) << 24U) | u24(b, c, d);
}

// AABC-B clause 6.2.9 and 6.2.29: "Valid Base Frequencies have to fall into
// the VHF band in the range of 136.000 - 172.000 MHz ... or fall into the UHF
// band in the range of 380.000 - 512 MHz", and the clause prints the field
// values as $019F0A00 - $020CE700 and $0487AB00 - $061A8000, which are those
// frequencies over clause 2.3.5's 5 Hz unit. A base frequency in either range
// selects Figure 6.2.9-2's layout for opcode $3D.
constexpr std::uint32_t kVhfBaseLow = 136'000'000U / 5U;
constexpr std::uint32_t kVhfBaseHigh = 172'000'000U / 5U;
constexpr std::uint32_t kUhfBaseLow = 380'000'000U / 5U;
constexpr std::uint32_t kUhfBaseHigh = 512'000'000U / 5U;
static_assert(kVhfBaseLow == 0x019F0A00U && kVhfBaseHigh == 0x020CE700U);
static_assert(kUhfBaseLow == 0x0487AB00U && kUhfBaseHigh == 0x061A8000U);

bool vhf_uhf_base(std::uint32_t base) {
    return (base >= kVhfBaseLow && base <= kVhfBaseHigh) ||
           (base >= kUhfBaseLow && base <= kUhfBaseHigh);
}

// Clause 2.3.5, 2.3.6 and 2.3.10's units.
constexpr std::uint64_t kBaseFrequencyUnitHz = 5;
constexpr std::uint32_t kKiloHertzEighthHz = 125;
// Clause 2.3.35: "(offset value) x (0.250MHz)".
constexpr std::int64_t kTransmitOffsetUnitHz = 250'000;

// AABC-B Figures 6.2.9-1, 6.2.9-2 and 6.2.29-1 all carry the identifier in
// octet 2's upper nibble, the channel spacing in the ten bits that end octet
// 5 and the 32-bit base frequency in octets 6 to 9; they differ in the middle
// 18 bits. Octets 2 to 5 read as one 32-bit word, most significant first.
P25IdentifierUpdate parse_identifier_update(const P25Tsbk& tsbk, bool vhf_uhf) {
    const auto& o = tsbk.octets;
    const std::uint32_t word = u32(o[2], o[3], o[4], o[5]);

    P25IdentifierUpdate iden;
    iden.vhf_uhf = vhf_uhf;
    iden.identifier = static_cast<std::uint8_t>(word >> 28U);
    iden.channel_spacing_raw = static_cast<std::uint16_t>(word & 0x3FFU);
    iden.base_frequency_raw = u32(o[6], o[7], o[8], o[9]);
    iden.base_frequency_hz = iden.base_frequency_raw * kBaseFrequencyUnitHz;
    iden.channel_spacing_hz = iden.channel_spacing_raw * kKiloHertzEighthHz;

    if (!vhf_uhf) {
        // Figure 6.2.9-1: Identifier (4), BW (9, clause 2.3.6), Transmit
        // Offset (9, clause 2.3.35), Channel Spacing (10), which is the 32
        // bits of octets 2 to 5.
        iden.bandwidth_raw = static_cast<std::uint16_t>((word >> 19U) & 0x1FFU);
        iden.transmit_offset_raw = static_cast<std::uint16_t>((word >> 10U) & 0x1FFU);
        // Clause 2.3.6: "The BW value of zero is reserved".
        if (iden.bandwidth_raw != 0) {
            iden.bandwidth_hz = iden.bandwidth_raw * kKiloHertzEighthHz;
        }
        // Clause 2.3.35: b8 is the sign, b7-b0 the offset in 250 kHz.
        //
        // The note says "$00 is reserved to indicate that transmit and
        // receive occur on the same frequency" and "$80 is reserved to
        // indicate that there is no standard transmit offset". Both are
        // written as eight bits, so they are read here as values of the
        // 8-bit offset value b7-b0, whatever b8 says. That is a reading of
        // the note, not something it states; $00 comes out as zero offset
        // under any reading, and $80 is the one it decides.
        const std::uint16_t value = iden.transmit_offset_raw & 0xFFU;
        if (value != 0x80U) {
            const std::int64_t magnitude = static_cast<std::int64_t>(value) * kTransmitOffsetUnitHz;
            iden.transmit_offset_hz = (iden.transmit_offset_raw & 0x100U) != 0 ? magnitude
                                                                               : -magnitude;
        }
    } else {
        // Figure 6.2.9-2 and 6.2.29-1: Identifier (4), BW VU (4, "octet 2,
        // bits 3-0"), Transmit Offset VU (14, "octet 3 and octet 4, bits
        // 7-2"), Channel Spacing (10).
        iden.bandwidth_raw = static_cast<std::uint16_t>((word >> 24U) & 0x0FU);
        iden.transmit_offset_raw = static_cast<std::uint16_t>((word >> 10U) & 0x3FFFU);
        // Clause 2.3.7, Table 2.3.7-1.
        if (iden.bandwidth_raw == 0b0100U) {
            iden.bandwidth_hz = 6'250U;
        } else if (iden.bandwidth_raw == 0b0101U) {
            iden.bandwidth_hz = 12'500U;
        }
        // Clause 2.3.36: b13 the sign, b12-b0 a multiple of the channel
        // spacing. Clause 6.2.29's formula, Sign (Transmit Offset VU x
        // Channel Spacing x 0.125 kHz).
        const std::int64_t magnitude = static_cast<std::int64_t>(iden.transmit_offset_raw & 0x1FFFU) *
                                       static_cast<std::int64_t>(iden.channel_spacing_hz);
        iden.transmit_offset_hz =
            (iden.transmit_offset_raw & 0x2000U) != 0 ? magnitude : -magnitude;
    }
    return iden;
}

}  // namespace

P25Tsbk p25_tsbk_from_octets(const std::array<std::uint8_t, kP25TsbkOctets>& octets) {
    P25Tsbk tsbk;
    tsbk.octets = octets;
    // AABB-B clause 5.1, Figure 5.1-1.
    tsbk.last_block = (octets[0] & 0x80U) != 0;
    tsbk.protected_block = (octets[0] & 0x40U) != 0;
    tsbk.opcode = static_cast<std::uint8_t>(octets[0] & 0x3FU);
    tsbk.manufacturer_id = octets[1];
    // "TSBK CRC - (octets 10-11) - this is the CRC parity check as described
    // in [BAAA] subclause 6.2", over the ten octets before it.
    const std::uint16_t crc = p25_header_crc(std::span<const std::uint8_t>(octets.data(), 10));
    tsbk.crc_ok = crc == u16(octets[10], octets[11]);
    return tsbk;
}

P25Tsbk p25_decode_tsbk(std::span<const std::uint8_t, kP25TsbkDibits> dibits) {
    const P25TrellisDecode decoded = p25_trellis12_decode(dibits);
    P25Tsbk tsbk = p25_tsbk_from_octets(decoded.octets);
    tsbk.corrected_bits = decoded.corrected_bits;
    return tsbk;
}

std::array<std::uint8_t, kP25TsbkOctets> p25_tsbk_with_crc(
    std::span<const std::uint8_t, 10> information) {
    std::array<std::uint8_t, kP25TsbkOctets> octets{};
    std::copy(information.begin(), information.end(), octets.begin());
    const std::uint16_t crc = p25_header_crc(information);
    octets[10] = static_cast<std::uint8_t>(crc >> 8U);
    octets[11] = static_cast<std::uint8_t>(crc & 0xFFU);
    return octets;
}

std::string_view p25_osp_alias(std::uint8_t opcode) {
    switch (opcode) {
        // Table 4.2-1, Voice Service OSPs.
        case 0x00: return "GRP_V_CH_GRANT";
        case 0x02: return "GRP_V_CH_GRANT_UPDT";
        case 0x03: return "GRP_V_CH_GRANT_UPDT_EXP";
        case 0x04: return "UU_V_CH_GRANT";
        case 0x05: return "UU_ANS_REQ";
        case 0x06: return "UU_V_CH_GRANT_UPDT";
        case 0x08: return "TELE_INT_CH_GRANT";
        case 0x09: return "TELE_INT_CH_GRANT_UPDT";
        case 0x0A: return "TELE_INT_ANS_REQ";
        // Table 5.2-1, Data Service OSPs. The first four are marked obsolete
        // there, and clause 1.7 has their opcodes never reused.
        case 0x10: return "IND_DATA_CH_GRANT";
        case 0x11: return "GRP_DATA_CH_GRANT";
        case 0x12: return "GRP_DATA_CH_ANN";
        case 0x13: return "GRP_DATA_CH_ANN_EXP";
        case 0x14: return "SN-DATA_CHN_GNT";
        case 0x15: return "SN-DATA_PAGE_REQ";
        case 0x16: return "SN-DATA_CHN_ANN_EXP";
        // Table 6.2-1, Control and Status OSPs.
        case 0x18: return "STS_UPDT";
        case 0x1A: return "STS_Q";
        case 0x1C: return "MSG_UPDT";
        case 0x1D: return "RAD_MON_CMD";
        case 0x1F: return "CALL_ALRT";
        case 0x20: return "ACK_RSP_FNE";
        case 0x21: return "QUE_RSP";
        case 0x24: return "EXT_FNCT_CMD";
        case 0x27: return "DENY_RSP";
        case 0x28: return "GRP_AFF_RSP";
        case 0x29: return "SCCB_EXP";
        case 0x2A: return "GRP_AFF_Q";
        case 0x2B: return "LOC_REG_RSP";
        case 0x2C: return "U_REG_RSP";
        case 0x2D: return "U_REG_CMD";
        case 0x2E: return "AUTH_CMD";
        case 0x2F: return "U_DE_REG_ACK";
        case 0x34: return "IDEN_UP_VU";
        case 0x35: return "TIME_DATE_ANN";
        case 0x36: return "ROAM_ADDR_CMD";
        case 0x37: return "ROAM_ADDR_UPDT";
        case 0x38: return "SYS_SRV_BCST";
        case 0x39: return "SCCB";
        case 0x3A: return "RFSS_STS_BCST";
        case 0x3B: return "NET_STS_BCST";
        case 0x3C: return "ADJ_STS_BCST";
        case 0x3D: return "IDEN_UP";
        case 0x3E: return "P_PARM_BCST";
        case 0x3F: return "P_PARM_UPDT";
        default: return {};
    }
}

std::optional<std::uint64_t> p25_channel_frequency_hz(const P25IdentifierUpdate& iden,
                                                      const P25Channel& channel) {
    if (channel.identifier != iden.identifier) {
        return std::nullopt;
    }
    return iden.base_frequency_hz +
           static_cast<std::uint64_t>(channel.number) * iden.channel_spacing_hz;
}

P25TsbkMessage p25_parse_tsbk(const P25Tsbk& tsbk) {
    if (!tsbk.crc_ok || tsbk.protected_block || tsbk.manufacturer_id != kP25TsbkMfidStandard) {
        return std::monostate{};
    }
    const auto& o = tsbk.octets;
    switch (tsbk.opcode) {
        case kP25OpGroupVoiceGrant: {
            // Figure 4.2.1-1. The scan's text layer labels the channel,
            // group and source rows at octets 4, 6 and 8, the middle of each
            // field; clause 2.3's widths (8, 16, 16, 24 bits) fill octets 2
            // to 9 exactly in the figure's order.
            P25GroupVoiceGrant grant;
            grant.options.raw = o[2];
            grant.channel = P25Channel::from(o[3], o[4]);
            grant.group_address = u16(o[5], o[6]);
            grant.source_address = u24(o[7], o[8], o[9]);
            return grant;
        }
        case kP25OpGroupVoiceGrantUpdate: {
            P25GroupVoiceGrantUpdate update;
            update.channel[0] = P25Channel::from(o[2], o[3]);
            update.group_address[0] = u16(o[4], o[5]);
            update.channel[1] = P25Channel::from(o[6], o[7]);
            update.group_address[1] = u16(o[8], o[9]);
            return update;
        }
        case kP25OpGroupVoiceGrantUpdateExplicit: {
            P25GroupVoiceGrantUpdateExplicit update;
            update.options.raw = o[2];
            update.transmit = P25Channel::from(o[4], o[5]);
            update.receive = P25Channel::from(o[6], o[7]);
            update.group_address = u16(o[8], o[9]);
            return update;
        }
        case kP25OpIdentifierUpdate:
            // Clause 6.2.9: the same opcode for both layouts, "determined by
            // the Base Frequency".
            return parse_identifier_update(tsbk, vhf_uhf_base(u32(o[6], o[7], o[8], o[9])));
        case kP25OpIdentifierUpdateVu:
            return parse_identifier_update(tsbk, true);
        case kP25OpRfssStatus: {
            // Figure 6.2.15-1: LRA in octet 2; octet 3 holds reserved bits,
            // A at bit 4 and System ID b11-b8; octet 4 System ID b7-b0, then
            // RFSS ID, Site ID, the 16-bit channel and the System Service
            // Class.
            P25RfssStatus status;
            status.lra = o[2];
            status.network_active = (o[3] & 0x10U) != 0;
            status.system_id = static_cast<std::uint16_t>(((o[3] & 0x0FU) << 8U) | o[4]);
            status.rfss_id = o[5];
            status.site_id = o[6];
            status.channel = P25Channel::from(o[7], o[8]);
            status.system_service_class = o[9];
            return status;
        }
        case kP25OpNetworkStatus: {
            // Figure 6.2.11-1: LRA in octet 2, the 20-bit WACN ID in octets
            // 3, 4 and the top of 5 ("(b3-b0)"), the 12-bit System ID in the
            // bottom of 5 ("(b11-b8)") and 6, "The Channel field (octets
            // 7-8)", and the System Service Class in 9.
            P25NetworkStatus status;
            status.lra = o[2];
            status.wacn_id = (static_cast<std::uint32_t>(o[3]) << 12U) |
                             (static_cast<std::uint32_t>(o[4]) << 4U) | (o[5] >> 4U);
            status.system_id = static_cast<std::uint16_t>(((o[5] & 0x0FU) << 8U) | o[6]);
            status.channel = P25Channel::from(o[7], o[8]);
            status.system_service_class = o[9];
            return status;
        }
        case kP25OpAdjacentStatus: {
            // Figure 6.2.2-1, laid out as RFSS_STS_BCST with C, F, V and A in
            // octet 3 bits 7 to 4.
            P25AdjacentStatus status;
            status.lra = o[2];
            status.conventional = (o[3] & 0x80U) != 0;
            status.failure = (o[3] & 0x40U) != 0;
            status.valid = (o[3] & 0x20U) != 0;
            status.network_active = (o[3] & 0x10U) != 0;
            status.system_id = static_cast<std::uint16_t>(((o[3] & 0x0FU) << 8U) | o[4]);
            status.rfss_id = o[5];
            status.site_id = o[6];
            status.channel = P25Channel::from(o[7], o[8]);
            status.system_service_class = o[9];
            return status;
        }
        default: return std::monostate{};
    }
}

}  // namespace revenant::decode
