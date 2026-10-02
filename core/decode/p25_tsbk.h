// P25 Phase 1 trunking control channel: the Trunking Signaling Data Unit and
// the Trunking Signaling Blocks it carries.
//
// SPECIFICATION
//
// Implements TIA-102.AABB-B, "Trunking Control Channel Formats" (2011):
// clause 4 (packet structure, the TSDU's DUID and the 70-bit micro-slot),
// clause 4.4 (every block through the CAI's rate 1/2 trellis code), clause 5
// with Figures 5-1 and 5-2 (the single block packet: up to three TSBKs after
// one frame sync and NID, null padded to a micro-slot boundary), clause 5.1
// and Figure 5.1-1 (the TSBK's twelve octets) and clause 5.2 (what a
// protected TSBK leaves in clear).
//
// The coding is TIA-102.BAAA-A clause 6.2 (the CRC every TSBK carries) and
// clause 7 with Tables 7-1 to 7-4 (the trellis code and its interleaver),
// which live in core/decode/dv_codes.h beside the CAI's other codes.
//
// The messages are TIA-102.AABC-B, "Trunking Control Channel Messages":
// clause 2.1 (the TSBK fields again), clause 2.3 (the field definitions this
// file reads: 2.3.5 Base Frequency, 2.3.6 BW, 2.3.7 BW VU, 2.3.9 Channel,
// 2.3.10 Channel Spacing, 2.3.19 Identifier, 2.3.23 RF Sub-System ID, 2.3.24
// Service Options, 2.3.26 Site ID, 2.3.31 System ID, 2.3.32 System Service
// Class, 2.3.35 Transmit Offset, 2.3.36 Transmit Offset VU, 2.3.37 WACN ID),
// Table 4.2-1 and clauses 4.2.1 to 4.2.3 (the group voice grants), and Table
// 6.2-1 with clauses 6.2.2, 6.2.9, 6.2.11, 6.2.15 and 6.2.29 (the adjacent,
// network and RFSS status broadcasts and both identifier updates).
//
// All three were read from the public archive at
// archive.org/details/TIA-102_Series_Documents, items
// "TIA-102.AABB-B_Trunking_Control_Channel_Formats.pdf",
// "TIA-102.AABC-B_Trunking_Control_Channel_Messages.pdf" and
// "TIA-102-BAAA-A_Project_25_FDMA_CAI.pdf", the collection core/decode/p25p1.h
// cites. The AABC-B item is a scanned 2005 edition with an OCR text layer;
// where the layer garbles a figure's octet labels, the octet positions below
// come from the field widths clause 2.3 states, which add up to the ten
// octets each figure has, and the comment at each parse says so.
//
// CLEAN ROOM
//
// No P25 implementation was read. Every constant names the clause, table or
// figure it came from, and engineering choices are labelled as such.
//
// WHAT THIS DOES NOT DO
//
// It does not decrypt. AABB-B clause 5.2: a protected TSBK sends its opcode
// and arguments encrypted and only LB, P, the MFID and the CRC in clear. Such
// a block is reported as protected, its CRC checked, and nothing past the
// MFID is read as a message. The same goes for a non-standard MFID, whose
// meaning AABC-B clause 2.1 leaves to the manufacturer. The multiple block
// formats (DUID $C, AABB-B clause 6) are not decoded here.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include "core/decode/dv_codes.h"

namespace revenant::decode {

// ---------------------------------------------------------------------------
// Structure
// ---------------------------------------------------------------------------

// AABB-B clause 5.1: "10 octets of address and control information, followed
// by 2 octets of CRC", through the rate 1/2 code into the 196 bits Figure 5-1
// shows for the TSBK.
inline constexpr std::size_t kP25TsbkOctets = kP25TrellisOctets;
inline constexpr std::size_t kP25TsbkDibits = kP25TrellisBlockDibits;

// AABB-B clause 5: the outbound packet "allows for up to two (2) additional
// TSBKs to follow without an additional preamble", three in all.
inline constexpr std::size_t kP25MaxTsbksPerTsdu = 3;

// AABB-B clause 4: "The packet is integral multiples of 70 bit micro-slots",
// a status symbol after each. 35 information dibits then one status symbol,
// the same rhythm TIA-102.BAAA-A clause 8.4 gives every data unit.
inline constexpr std::size_t kP25MicroSlotInformationDibits = 35;
inline constexpr std::size_t kP25MicroSlotSymbols = kP25MicroSlotInformationDibits + 1;

// The symbols a TSDU carrying `blocks` TSBKs occupies on the air, status
// symbols and the null padding to the end of its last micro-slot included:
// the frame sync's 24 dibits, the NID's 32 and 98 per block, rounded up to
// whole micro-slots. One block is 5 micro-slots, 180 symbols, which is
// Figure 5-1's "5 micro-slots ( 37.5 ms )"; two are 8 and three are 10.
[[nodiscard]] constexpr std::size_t p25_tsdu_symbols(std::size_t blocks) {
    const std::size_t information = 24 + 32 + blocks * kP25TsbkDibits;
    const std::size_t slots =
        (information + kP25MicroSlotInformationDibits - 1) / kP25MicroSlotInformationDibits;
    return slots * kP25MicroSlotSymbols;
}

// AABB-B clause 5.1, Figure 5.1-1, decoded.
struct P25Tsbk {
    // All twelve octets after the trellis decode, CRC included.
    std::array<std::uint8_t, kP25TsbkOctets> octets{};

    // Octet 0 bit 7: "= 1 last TSBK in this packet".
    bool last_block = false;
    // Octet 0 bit 6: "= 1 - protected packet mode".
    bool protected_block = false;
    // Octet 0 bits 5-0. Meaningless when protected_block is set, because
    // AABB-B clause 5.3.1 sends it encrypted.
    std::uint8_t opcode = 0;
    // Octet 1, sent in clear either way.
    std::uint8_t manufacturer_id = 0;

    // Octets 10 and 11 against TIA-102.BAAA-A clause 6.2 over octets 0 to 9.
    bool crc_ok = false;

    // What the trellis decoder had to change to reach this block.
    std::uint32_t corrected_bits = 0;

    // Octets 2 to 9, AABB-B clause 5.1's "Arguments".
    [[nodiscard]] std::span<const std::uint8_t, 8> arguments() const {
        return std::span<const std::uint8_t, 8>(octets.data() + 2, 8);
    }
};

// Reads the header fields of 12 octets and checks their CRC.
[[nodiscard]] P25Tsbk p25_tsbk_from_octets(const std::array<std::uint8_t, kP25TsbkOctets>& octets);

// Decodes one TSBK from its 98 dibits in transmission order.
[[nodiscard]] P25Tsbk p25_decode_tsbk(std::span<const std::uint8_t, kP25TsbkDibits> dibits);

// The ten information octets of a TSBK with TIA-102.BAAA-A clause 6.2's CRC
// appended, ready for p25_trellis12_encode.
[[nodiscard]] std::array<std::uint8_t, kP25TsbkOctets> p25_tsbk_with_crc(
    std::span<const std::uint8_t, 10> information);

// ---------------------------------------------------------------------------
// Opcodes, MFID $00
// ---------------------------------------------------------------------------

// AABC-B clause 2.1: messages are defined against "the Standard Project 25
// Manufacturer's ID of $00 ... unless an explicit value is given". None of
// the ones below gives one.
inline constexpr std::uint8_t kP25TsbkMfidStandard = 0x00;

// AABC-B Table 4.2-1, Voice Service OSPs.
inline constexpr std::uint8_t kP25OpGroupVoiceGrant = 0x00;             // %000000
inline constexpr std::uint8_t kP25OpGroupVoiceGrantUpdate = 0x02;       // %000010
inline constexpr std::uint8_t kP25OpGroupVoiceGrantUpdateExplicit = 0x03;  // %000011

// AABC-B Table 6.2-1, Control and Status OSPs.
inline constexpr std::uint8_t kP25OpIdentifierUpdateVu = 0x34;   // %110100
inline constexpr std::uint8_t kP25OpRfssStatus = 0x3A;           // %111010
inline constexpr std::uint8_t kP25OpNetworkStatus = 0x3B;        // %111011
inline constexpr std::uint8_t kP25OpAdjacentStatus = 0x3C;       // %111100
inline constexpr std::uint8_t kP25OpIdentifierUpdate = 0x3D;     // %111101

// The alias AABC-B gives an outbound opcode under MFID $00, from Tables
// 4.2-1, 5.2-1 and 6.2-1, or empty for an opcode none of them lists. Used to
// name an opcode this file does not parse, so it is reported by number and
// by name rather than dropped. Outbound only: the inbound tables reuse the
// same numbers for different messages, and a receiver on a control channel's
// downlink hears OSPs.
[[nodiscard]] std::string_view p25_osp_alias(std::uint8_t opcode);

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

// AABC-B clause 2.3.9: a 4-bit Channel Identifier, "a maximum of 16 unique
// channel identifier values" (and clause 2.3.19's 4-bit Identifier), and a
// Channel Number. AABB-B Figure 5.3-1 shows the whole field in two octets, so
// the number is the other 12 bits.
struct P25Channel {
    std::uint8_t identifier = 0;
    std::uint16_t number = 0;

    [[nodiscard]] static P25Channel from(std::uint8_t high, std::uint8_t low) {
        return P25Channel{static_cast<std::uint8_t>(high >> 4U),
                          static_cast<std::uint16_t>(((high & 0x0FU) << 8U) | low)};
    }
    [[nodiscard]] std::uint16_t raw() const {
        return static_cast<std::uint16_t>((identifier << 12U) | (number & 0x0FFFU));
    }
    friend bool operator==(const P25Channel&, const P25Channel&) = default;
};

// AABC-B clause 2.3.24, Figure 2.3.24-1.
struct P25ServiceOptions {
    std::uint8_t raw = 0;
    // Bit 7.
    [[nodiscard]] bool emergency() const { return (raw & 0x80U) != 0; }
    // Bit 6: the call's working channel is presented in protected mode, that
    // is the voice is encrypted. Reported, never acted on.
    [[nodiscard]] bool protected_mode() const { return (raw & 0x40U) != 0; }
    // Bit 5, full duplex, and bit 4, packet mode.
    [[nodiscard]] bool duplex() const { return (raw & 0x20U) != 0; }
    [[nodiscard]] bool packet_mode() const { return (raw & 0x10U) != 0; }
    // Bits 2-0.
    [[nodiscard]] std::uint8_t priority() const { return raw & 0x07U; }
};

// AABC-B clause 4.2.1.1, Figure 4.2.1-1, GRP_V_CH_GRANT: service options in
// octet 2, the channel in 3 and 4, the 16-bit group address (clause 2.3.16)
// in 5 and 6, and the 24-bit source address (clause 2.3.27) in 7 to 9.
struct P25GroupVoiceGrant {
    P25ServiceOptions options;
    P25Channel channel;
    std::uint16_t group_address = 0;
    std::uint32_t source_address = 0;
};

// AABC-B clause 4.2.2, Figure 4.2.2-1, GRP_V_CH_GRANT_UPDT: "The Channel in
// Octets 2-3 is to be associated with the Group Address in Octets 4-5. The
// Channel in Octets 6-7 is to be associated with the Group Address in Octets
// 8-9."
struct P25GroupVoiceGrantUpdate {
    std::array<P25Channel, 2> channel{};
    std::array<std::uint16_t, 2> group_address{};
};

// AABC-B clause 4.2.3, Figure 4.2.3-1, GRP_V_CH_GRANT_UPDT_EXP: service
// options in octet 2, octet 3 reserved, Channel (T) in 4 and 5, Channel (R)
// in 6 and 7, the group address in 8 and 9.
struct P25GroupVoiceGrantUpdateExplicit {
    P25ServiceOptions options;
    P25Channel transmit;
    P25Channel receive;
    std::uint16_t group_address = 0;
};

// The channel parameters an identifier stands for, from either identifier
// update. Raw fields as sent, and the values they work out to in hertz.
struct P25IdentifierUpdate {
    // True for the VHF/UHF layout of AABC-B Figure 6.2.9-2 and clause 6.2.29,
    // false for Figure 6.2.9-1.
    bool vhf_uhf = false;

    std::uint8_t identifier = 0;

    // Figure 6.2.9-1's 9-bit BW, or the 4-bit BW VU of the VHF/UHF layout.
    std::uint16_t bandwidth_raw = 0;
    // The 9-bit Transmit Offset, or the 14-bit Transmit Offset VU.
    std::uint16_t transmit_offset_raw = 0;
    std::uint16_t channel_spacing_raw = 0;
    std::uint32_t base_frequency_raw = 0;

    // Clause 2.3.5: Base Frequency x 0.000005 MHz, 5 Hz a unit.
    std::uint64_t base_frequency_hz = 0;
    // Clause 2.3.10: Channel Spacing x 0.125 kHz.
    std::uint32_t channel_spacing_hz = 0;
    // Clause 2.3.6, BW x 0.125 kHz, or clause 2.3.7's two defined BW VU
    // values. Empty for a reserved value.
    std::optional<std::uint32_t> bandwidth_hz;
    // SU transmit minus SU receive, clause 2.3.35 or 2.3.36. Empty when
    // clause 2.3.35's note says there is no standard offset.
    std::optional<std::int64_t> transmit_offset_hz;
};

// AABC-B clause 2.3.9.2: SU RX = Base Frequency + Channel Number x Channel
// Spacing, which clause 2.3.9 says is the FNE's transmit frequency, the one a
// receiver tunes. Empty when the identifier does not match.
[[nodiscard]] std::optional<std::uint64_t> p25_channel_frequency_hz(
    const P25IdentifierUpdate& iden, const P25Channel& channel);

// AABC-B clause 6.2.15.1, Figure 6.2.15-1, RFSS_STS_BCST abbreviated format.
struct P25RfssStatus {
    std::uint8_t lra = 0;
    // Octet 3 bit 4, "A": "a valid RFSS network connection is active".
    bool network_active = false;
    std::uint16_t system_id = 0;
    std::uint8_t rfss_id = 0;
    std::uint8_t site_id = 0;
    // "addresses the Primary control channel of this site".
    P25Channel channel;
    std::uint8_t system_service_class = 0;
};

// AABC-B clause 6.2.11.1, Figure 6.2.11-1, NET_STS_BCST abbreviated format.
struct P25NetworkStatus {
    std::uint8_t lra = 0;
    std::uint32_t wacn_id = 0;
    std::uint16_t system_id = 0;
    P25Channel channel;
    std::uint8_t system_service_class = 0;
};

// AABC-B clause 6.2.2.1, Figure 6.2.2-1, ADJ_STS_BCST abbreviated format.
struct P25AdjacentStatus {
    std::uint8_t lra = 0;
    // Octet 3 bits 7 to 4, C, F, V and A.
    bool conventional = false;
    bool failure = false;
    bool valid = false;
    bool network_active = false;
    std::uint16_t system_id = 0;
    std::uint8_t rfss_id = 0;
    std::uint8_t site_id = 0;
    P25Channel channel;
    std::uint8_t system_service_class = 0;
};

// What a TSBK says, when this file reads it. monostate for everything else:
// a failed CRC, a protected block, a non-standard MFID, or an opcode not
// parsed here, which the caller still has by number in P25Tsbk.
using P25TsbkMessage =
    std::variant<std::monostate, P25GroupVoiceGrant, P25GroupVoiceGrantUpdate,
                 P25GroupVoiceGrantUpdateExplicit, P25IdentifierUpdate, P25RfssStatus,
                 P25NetworkStatus, P25AdjacentStatus>;

[[nodiscard]] P25TsbkMessage p25_parse_tsbk(const P25Tsbk& tsbk);

}  // namespace revenant::decode
