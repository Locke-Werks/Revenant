// IL2P: AX.25 frames carried in a scrambled, Reed-Solomon protected packet
// found by a 24-bit sync word, in place of HDLC.
//
// SPECIFICATION
//
// "IL2P Specification Draft v0.6, 16 March 2024", "Improved Layer 2
// Protocol", Nino Carrillo KK4HEJ. Seventeen pages. Retrieved 2026-10-02
// from https://tarpn.net/t/il2p/il2p-specification_draft_v0-6.pdf.
//
// The document has no numbered clauses. Sections are cited by their headings:
// "Interface to Physical Layer" with its "FM Audio Frequency Shift Keying
// Symbol Map", "Reed Solomon Forward Error Correction", "Data Scrambling",
// "Packet-Synchronized LFSR", "Scrambling Inside RS Code Block", "Extracting
// All Data from LFSR Memory" with the "Transmit LFSR Schematic and Initial
// Conditions" and "Receive LFSR Schematic and Initial Conditions" figures,
// "Packet Structure", "Sync Word", "FEC Level (Removed)", "IL2P Header
// Types", "IL2P Type 0 Header", "IL2P Type 1 Header" with the "Control and
// Addressing Field Map for IL2P Type 1 Header" figure, "UI Subfield", "PID
// Subfield" with the "IL2P AX.25 PID Code Mapping" table, the three
// "Translated AX.25 ... Control Subfield Map" tables, "Payload Block Size
// Computations", the transmit and receive procedures, "Optional Trailing CRC
// with Hamming Encoding" with its encode and decode tables and "Encoded CRC
// Format", and "Example Encoded Packets".
//
// The three example packets in that last section were checked against the
// reading below before any of this was written, and tests/decode/
// test_il2p.cpp checks them again in both directions: they fixed the
// Reed-Solomon generator's first root, the scrambler's bit delay on receive,
// the bit numbering of the header figure, and the trailing CRC's value and
// nibble order, each of which the prose alone leaves open to more than one
// reading.
//
// WHAT THE DOCUMENT DOES NOT SAY
//
// "Payload Block Size Computations" says "The encoder will always append 16
// parity symbols per payload block", and "FEC Level (Removed)" retires the
// header bit that once chose between that and a "Baseline" level with fewer
// parity symbols, whose sizes v0.6 no longer gives. A packet from a
// transmitter still sending Baseline FEC is therefore refused here: its
// payload blocks are shorter than this receiver collects. No on-air IL2P
// recording has been checked.
//
// The S-frame control map gives bits 5 to 0 and leaves bit 6 blank, while an
// AX.25 S frame has a P/F bit. The examples do not settle it, since the one
// S frame printed has P/F clear. ENGINEERING CHOICE: P/F is bit 6, where the
// I-frame and U-frame maps both put it.
//
// "AX.25 Layer 3", IL2P PID 0x2, stands for every AX.25 PID of the forms
// yy10yyyy and yy01yyyy, so it cannot be turned back into one octet.
// ENGINEERING CHOICE: the transmitter here never uses it, sending such a
// frame as Type 0, and the receiver reconstructs PID 0x2 as 0x20, the first
// value of that form.
//
// Whether the optional trailing CRC is present is "coordinated between
// participating stations", so a receiver cannot know from the packet. The
// handling is an engineering choice, set and argued at kIl2pCrcOctets below.
//
// CLEAN ROOM
//
// No IL2P implementation was read.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/decode/reed_solomon.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::SampleIndex;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// "Sync Word": "The IL2P Sync Word is 0xF15E48". "Packet Structure": "All
// bytes are sent Most Significant Bit first", so bit 23 goes to air first.
inline constexpr std::uint32_t kIl2pSyncWord = 0xF15E48;
inline constexpr std::size_t kIl2pSyncBits = 24;

// "Sync Word": "the receiver will declare a match if 23 out of the last 24
// bits received match the Sync Word (any single bit flipped)."
inline constexpr unsigned kIl2pSyncTolerance = 1;

// "Packet Structure": a 13-byte Control and Addressing field and 2 bytes of
// header parity. "IL2P Receive Decoding Procedure", step 2: "Collect next 15
// bytes as IL2P Header".
inline constexpr std::size_t kIl2pHeaderOctets = 13;
inline constexpr std::size_t kIl2pHeaderParity = 2;

// "Payload Block Size Computations": "parity_symbols_per_block = 16" and
// "payload_block_count = Ceiling(payload_byte_count / 239)".
inline constexpr std::size_t kIl2pPayloadParity = 16;
inline constexpr std::size_t kIl2pMaximumBlockData = 239;

// "IL2P Header Types": "a 10-bit payload count, enabling packet sizes up to
// 1023 payload bytes after the header."
inline constexpr std::size_t kIl2pMaximumPayload = 1023;

// "Reed Solomon Forward Error Correction": "The Galois Field is defined by
// reducing polynomial x^8+x^4+x^3+x^2+1" and "The RS encoder uses zero as its
// first root." The generator's roots are consecutive powers of alpha = x, as
// the shared codec in core/decode/reed_solomon.h builds them; the example
// packets' parity matches with that and not with first root 1.
inline constexpr unsigned kIl2pFieldPolynomial = 0x11D;
inline constexpr unsigned kIl2pFirstRoot = 0;

// "Optional Trailing CRC with Hamming Encoding": four octets after the last
// payload block, each a (7,4) Hamming codeword of one nibble of the AX.25
// CRC with a zero above it, high nibble first ("Encoded CRC Format").
//
// ENGINEERING CHOICE, the document leaving its presence to agreement between
// stations: the receiver holds every packet for the 32 bits after it. When
// those four octets Hamming-decode to the CRC of the reconstructed AX.25
// frame, the packet is reported with its CRC verified. When all four are
// exact codewords with the top bit clear and decode to some other CRC, the
// trailer is taken to be a real CRC that disagrees, which "the CRC is used as
// a final validity test" makes a refusal; four arbitrary octets are all exact
// codewords one time in about 65,000. Anything else is taken as no trailer,
// and the packet is reported on the strength of its Reed-Solomon decode
// alone. A sync word arriving inside those 32 bits means no trailer was sent
// and the next packet began at once; "Preamble and Packet Termination" omits
// the preamble between back-to-back packets.
inline constexpr std::size_t kIl2pCrcOctets = 4;

// "Hamming Encode Table".
inline constexpr std::array<std::uint8_t, 16> kIl2pHammingEncode = {
    0x00, 0x71, 0x62, 0x13, 0x54, 0x25, 0x36, 0x47,
    0x38, 0x49, 0x5A, 0x2B, 0x6C, 0x1D, 0x0E, 0x7F};

// "Hamming Decode Table": the nibble nearest each 7-bit word.
inline constexpr std::array<std::uint8_t, 128> kIl2pHammingDecode = {
    0x0, 0x0, 0x0, 0x3, 0x0, 0x5, 0xe, 0x7, 0x0, 0x9, 0xe, 0xb, 0xe, 0xd, 0xe, 0xe,
    0x0, 0x3, 0x3, 0x3, 0x4, 0xd, 0x6, 0x3, 0x8, 0xd, 0xa, 0x3, 0xd, 0xd, 0xe, 0xd,
    0x0, 0x5, 0x2, 0xb, 0x5, 0x5, 0x6, 0x5, 0x8, 0xb, 0xb, 0xb, 0xc, 0x5, 0xe, 0xb,
    0x8, 0x1, 0x6, 0x3, 0x6, 0x5, 0x6, 0x6, 0x8, 0x8, 0x8, 0xb, 0x8, 0xd, 0x6, 0xf,
    0x0, 0x9, 0x2, 0x7, 0x4, 0x7, 0x7, 0x7, 0x9, 0x9, 0xa, 0x9, 0xc, 0x9, 0xe, 0x7,
    0x4, 0x1, 0xa, 0x3, 0x4, 0x4, 0x4, 0x7, 0xa, 0x9, 0xa, 0xa, 0x4, 0xd, 0xa, 0xf,
    0x2, 0x1, 0x2, 0x2, 0xc, 0x5, 0x2, 0x7, 0xc, 0x9, 0x2, 0xb, 0xc, 0xc, 0xc, 0xf,
    0x1, 0x1, 0x2, 0x1, 0x4, 0x1, 0x6, 0xf, 0x8, 0x1, 0xa, 0xf, 0xc, 0xf, 0xf, 0xf};

// "IL2P AX.25 PID Code Mapping": the AX.25 PID octet for each IL2P PID that
// has one. 0x0 and 0x1 are S and U frames without a PID octet, 0x7 to 0xA
// are "Future", and 0x2 is the engineering choice the header describes.
[[nodiscard]] std::optional<std::uint8_t> il2p_ax25_pid(std::uint8_t il2p_pid);
// The IL2P PID for an AX.25 PID octet, for the transmitter; nothing for an
// octet the table does not map one to one, 0x2's forms included.
[[nodiscard]] std::optional<std::uint8_t> il2p_pid_for(std::uint8_t ax25_pid);

// ---------------------------------------------------------------------------
// Scrambling
// ---------------------------------------------------------------------------

// "Transmit LFSR Schematic and Initial Conditions": x^9 + x^4 + 1 in Galois
// form, five stages loaded with zeros, the output tap, then four stages
// loaded with ones, the last stage feeding back to both XORs. "Extracting All
// Data from LFSR Memory": the output is taken after the five-bit delay and
// the register flushed at the end, so a block scrambles to the same length.
// Bits most significant first, per "Packet Structure"; the register starts
// afresh for every block, per "Packet-Synchronized LFSR".
[[nodiscard]] std::vector<std::uint8_t> il2p_scramble(std::span<const std::uint8_t> block);

// "Receive LFSR Schematic and Initial Conditions": five stages loaded with
// ones, an XOR with the input, four stages loaded with zeros and a final XOR
// with the input. It has no delay of its own: the examples descramble
// correctly with the first output bit taken as the first data bit.
[[nodiscard]] std::vector<std::uint8_t> il2p_descramble(std::span<const std::uint8_t> block);

// ---------------------------------------------------------------------------
// The header
// ---------------------------------------------------------------------------

// The Control and Addressing field's subfields, unscrambled. A Type 0 header
// carries only the payload count and the header type, its other bits zero.
struct Il2pHeader {
    // "IL2P Header Types": 0 transparent, 1 translated.
    std::uint8_t type = 0;
    // "Payload Byte Count Subfield", 0 to 1023.
    std::uint16_t payload_count = 0;
    // Byte 0 bit 7, "RESERVED" since "FEC Level (Removed)".
    bool reserved = false;

    // Type 1 only. Call signs as DEC SIXBIT decoded to ASCII, six characters
    // each with any space padding kept.
    std::string destination;
    std::string source;
    std::uint8_t destination_ssid = 0;
    std::uint8_t source_ssid = 0;
    bool ui = false;
    std::uint8_t pid = 0;
    std::uint8_t control = 0;
};

// "Control and Addressing Field Map for IL2P Type 1 Header", bit 7 being the
// most significant bit of each byte as the examples confirm: bits 0 to 5 of
// bytes 0 to 5 and 6 to 11 the two call signs a character each; bit 6 of byte
// 0 UI, of bytes 1 to 4 the PID and of bytes 5 to 11 the control, most
// significant first; bit 7 of byte 0 reserved, of byte 1 the header type and
// of bytes 2 to 11 the payload count, most significant first; byte 12 the
// destination SSID in its high nibble and the source SSID in its low.
[[nodiscard]] std::array<std::uint8_t, kIl2pHeaderOctets> il2p_pack_header(const Il2pHeader& h);
[[nodiscard]] Il2pHeader il2p_unpack_header(std::span<const std::uint8_t, kIl2pHeaderOctets> octets);

// "Payload Block Size Computations".
struct Il2pBlockLayout {
    std::size_t block_count = 0;
    std::size_t small_size = 0;
    std::size_t large_size = 0;
    std::size_t large_count = 0;
    std::size_t small_count = 0;

    // Data octets in block `i`; "large blocks closest to header" (transmit
    // procedure step 10).
    [[nodiscard]] std::size_t size_of(std::size_t i) const {
        return i < large_count ? large_size : small_size;
    }
    // Payload octets on the air, data and parity together.
    [[nodiscard]] std::size_t air_octets(std::size_t payload_count) const {
        return payload_count + block_count * kIl2pPayloadParity;
    }
};
[[nodiscard]] Il2pBlockLayout il2p_block_layout(std::size_t payload_count);

// ---------------------------------------------------------------------------
// Translation to and from AX.25
// ---------------------------------------------------------------------------

// What "IL2P Transmit Encoding Procedure" steps 2 to 4 make of an AX.25
// frame, address through information with no FCS: a Type 1 header and the
// information field as payload when "IL2P Type 1 Header" can carry the
// frame, otherwise a Type 0 header and the whole frame as payload. Fails only
// on an empty frame or one longer than kIl2pMaximumPayload.
struct Il2pTranslation {
    Il2pHeader header;
    std::vector<std::uint8_t> payload;
};
[[nodiscard]] Expected<Il2pTranslation> il2p_translate(std::span<const std::uint8_t> ax25);

// The reverse, receive procedure step 6 and step 11: the AX.25 frame, address
// through information with no FCS. A Type 0 payload is returned as it is.
// Fails on a Type 1 I or UI header whose PID the mapping table leaves to the
// future. Call signs are rebuilt as sent, so one AX.25 does not allow is left
// for ax25_parse to refuse.
[[nodiscard]] Expected<std::vector<std::uint8_t>> il2p_to_ax25(const Il2pHeader& header,
                                                              std::span<const std::uint8_t> payload);

// ---------------------------------------------------------------------------
// Packets
// ---------------------------------------------------------------------------

// The Reed-Solomon parameters for header (2) or payload (16) blocks.
[[nodiscard]] Rs8Params il2p_rs_params(std::size_t check_symbols);

// The Hamming-encoded trailing CRC of an AX.25 frame, CRC3 first.
[[nodiscard]] std::array<std::uint8_t, kIl2pCrcOctets> il2p_encode_crc(
    std::span<const std::uint8_t> ax25);

enum class Il2pCrc : std::uint8_t {
    // No trailer the receiver could recognise as a CRC.
    Absent,
    // The trailer decoded to the reconstructed frame's CRC.
    Verified,
    // Four exact codewords naming a different CRC: refused.
    Mismatch,
};

// The trailer's verdict for a reconstructed frame, by the rule at
// kIl2pCrcOctets.
[[nodiscard]] Il2pCrc il2p_check_crc(std::span<const std::uint8_t, kIl2pCrcOctets> trailer,
                                     std::span<const std::uint8_t> ax25);

// One packet received after a sync word whose header decoded.
struct Il2pPacket {
    Il2pHeader header;
    unsigned sync_errors = 0;
    // Symbols the Reed-Solomon decoder changed in the header and in all the
    // payload blocks together.
    std::size_t header_corrected = 0;
    std::size_t payload_corrected = 0;
    // False when a payload block was refused; the payload is then empty.
    bool payload_ok = false;
    std::vector<std::uint8_t> payload;
    // The AX.25 frame, address through information, when the payload decoded
    // and translated; otherwise empty, with `error` saying why.
    std::vector<std::uint8_t> ax25;
    std::string error;
    Il2pCrc crc = Il2pCrc::Absent;
    // Audio samples of the first header bit and of the last bit the packet
    // used, the trailer included when it was a CRC.
    SampleIndex first_sample = 0;
    SampleIndex last_sample = 0;
};

// Decodes the octets that follow a sync word: header, payload blocks and,
// when `trailer` holds four octets, the trailing CRC. For the document's
// example packets and for tests; the receiver below does the same a bit at a
// time. Nothing when the header is refused or `octets` is too short.
class Il2pCodec {
public:
    [[nodiscard]] static Expected<Il2pCodec> create();

    // The header, 15 octets as received: unscrambled fields and the number of
    // symbols corrected, or nothing when refused.
    struct HeaderResult {
        Il2pHeader header;
        std::size_t corrected = 0;
    };
    [[nodiscard]] std::optional<HeaderResult> decode_header(
        std::span<const std::uint8_t> octets) const;

    // The payload blocks as received, header.payload_count data octets and
    // their parity: the unscrambled payload and symbols corrected, or nothing
    // when any block is refused.
    struct PayloadResult {
        std::vector<std::uint8_t> payload;
        std::size_t corrected = 0;
    };
    [[nodiscard]] std::optional<PayloadResult> decode_payload(
        std::size_t payload_count, std::span<const std::uint8_t> octets) const;

    // Header and payload, with the trailer judged when four octets follow.
    [[nodiscard]] std::optional<Il2pPacket> decode(std::span<const std::uint8_t> octets) const;

    // The octets that follow the sync word for one AX.25 frame: transmit
    // procedure steps 2 to 10, then the trailing CRC when `crc` is set.
    [[nodiscard]] Expected<std::vector<std::uint8_t>> encode(std::span<const std::uint8_t> ax25,
                                                             bool crc) const;

private:
    Il2pCodec() = default;
    std::vector<Rs8Codec> codecs_;
    [[nodiscard]] const Rs8Codec& header_codec() const { return codecs_[0]; }
    [[nodiscard]] const Rs8Codec& payload_codec() const { return codecs_[1]; }

    friend class Il2pReceiver;
    void finish(Il2pPacket& packet) const;
};

// The bit-level receiver. It reads the line bits as they are, with no NRZI:
// "FM Audio Frequency Shift Keying Symbol Map" sends a one as mark and a zero
// as space and says "Differential encoding is not used."
class Il2pReceiver {
public:
    [[nodiscard]] static Expected<Il2pReceiver> create();

    void push(std::uint8_t bit, SampleIndex sample, std::vector<Il2pPacket>& out);

    // Sync matches whose header decoded.
    [[nodiscard]] std::uint64_t headers() const { return headers_; }

    void reset();

private:
    Il2pReceiver() = default;

    enum class State : std::uint8_t { Hunting, Header, Payload, Trailer };

    void begin_packet(unsigned sync_errors);
    void complete(std::vector<Il2pPacket>& out, bool with_trailer);
    [[nodiscard]] std::optional<unsigned> sync_match() const;

    Il2pCodec codec_ = *Il2pCodec::create();
    State state_ = State::Hunting;
    std::uint32_t window_ = 0;
    std::size_t window_fill_ = 0;
    unsigned sync_errors_ = 0;
    std::vector<std::uint8_t> octets_;
    std::uint8_t partial_ = 0;
    std::size_t partial_bits_ = 0;
    std::size_t wanted_ = 0;
    SampleIndex first_sample_ = 0;
    SampleIndex last_sample_ = 0;
    Il2pPacket packet_;
    std::uint64_t headers_ = 0;
};

}  // namespace revenant::decode
