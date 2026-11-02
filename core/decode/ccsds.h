// CCSDS telemetry: channel symbols to Transfer Frames to Space Packets, as a
// library the satellite decoders share.
//
// SPECIFICATION
//
// Four CCSDS Blue Books, fetched from public.ccsds.org/Pubs on 2026-11-02:
//
//   - 131.0-B-5, "TM Synchronization and Channel Coding", Issue 5, September
//     2023 (131x0b5.pdf). Cited as "131.0" with a section.
//   - 132.0-B-3, "TM Space Data Link Protocol", Issue 3, October 2021
//     (132x0b3.pdf). Cited as "132.0".
//   - 732.0-B-4, "AOS Space Data Link Protocol", Issue 4, October 2021
//     (732x0b4.pdf). Cited as "732.0".
//   - 133.0-B-2, "Space Packet Protocol", Issue 2, June 2020
//     (133x0b2e2.pdf). Cited as "133.0".
//
// WHAT IS DECODED
//
//   - the Attached Sync Marker 0x1ACFFC1D, 131.0 section 9.3 and figure 9-1,
//     in either polarity, which resolves BPSK's 180 degree ambiguity as the
//     note to 131.0 4.3.10 describes;
//   - the basic rate 1/2, K = 7 convolutional code, 131.0 3.3, with G2
//     inverted, through core/decode/viterbi_stream.h, with both symbol-pair
//     alignments tried because nothing in the symbol stream marks which
//     symbol of a pair is C1;
//   - NRZ-M, removed after the convolutional decoder per 131.0 3.3.3 note 1;
//   - both pseudo-randomisers of 131.0 section 10: the 131071-bit sequence of
//     x^17 + x^14 + 1 from the seed of 10.4.3, and the legacy 255-bit
//     sequence of x^8 + x^7 + x^5 + x^3 + 1 from all ones;
//   - Reed-Solomon (255,223) and (255,239), 131.0 section 4, interleaving
//     depths 1, 2, 3, 4, 5 and 8 (4.3.5.1) and virtual fill (4.3.7, 4.3.8),
//     in the dual basis representation of 4.3.9;
//   - the TM Transfer Frame primary header, 132.0 4.1.2, and its Frame Error
//     Control Field, 132.0 4.1.6;
//   - the AOS Transfer Frame primary header, 732.0 4.1.2, and the M_PDU
//     header, 732.0 4.1.4.2;
//   - Space Packets, 133.0 4.1.3, delimited across frames by the First
//     Header Pointer (132.0 4.1.2.7.6, 732.0 4.1.4.2.3).
//
// THE REED-SOLOMON FIELD
//
// 131.0 4.3.4 roots the generator at alpha^(11j), j = 128 - E to 127 + E,
// with alpha a root of F(x) = x^8 + x^7 + x^2 + x + 1 (4.3.3). The
// Rs8Codec of core/decode/reed_solomon.h roots its generator at consecutive
// powers of x in whatever field polynomial it is given. The two are the same
// code under a change of field representation: gamma = alpha^11 is primitive
// (131.0 4.3.4 note 1), so GF(2)[y]/m(y), with m the minimal polynomial of
// gamma, is the same field with gamma written y, and the generator's roots
// become y^(128-E) onwards. The map sends alpha to y^116, because
// 11 * 116 = 1 mod 255, and is linear over GF(2), so it is an 8x8 bit matrix
// built from the field and not typed in.
//
// The dual basis of 4.3.9 is built the same way, from the definition in
// 4.4.2: beta = alpha^117, the dual basis l0 to l7 of (1, beta, ...,
// beta^7) under the trace, so a symbol w has z_j = Tr(w beta^j). The tests
// regenerate the first matrix of 4.3.9.3 from that rule and compare it with
// the printed one, and the code generator against Annex G's coefficients.
// The second matrix of 4.3.9.3, the inverse, did not survive text extraction
// from the PDF and is not used; the inverse table here is the first one
// inverted by enumeration.
//
// DEVIATIONS
//
//   - QPSK's 90 degree ambiguity is not resolved here. A QPSK caller feeds
//     each of the rotations it cannot tell apart, or knows its mapping.
//   - Turbo and LDPC codes, 131.0 sections 6 to 8, are not decoded; the
//     receiver takes the 32-bit marker only.
//   - Frame sync declares lock on one marker within `asm_search_errors` bit
//     errors and keeps it while the marker at the expected place is within
//     `asm_locked_errors`. 131.0 leaves the strategy to the receiver (9.2.1.3
//     note: "synchronization may be verified by making further checks"); the
//     thresholds are chosen, not cited.
//
// CLEAN ROOM
//
// No CCSDS implementation was read: not libcorrect, gr-ccsds, gr-satellites,
// SatDump, or any other. Written from the four documents above and from
// textbook field arithmetic.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/decode/dv_codes.h"
#include "core/decode/reed_solomon.h"
#include "core/decode/viterbi_stream.h"
#include "core/error.h"

namespace revenant::decode {

// 131.0 9.3.1 and figure 9-1: the 32-bit ASM, first transmitted bit most
// significant.
inline constexpr std::uint32_t kCcsdsAsm = 0x1ACFFC1DU;
inline constexpr std::size_t kCcsdsAsmBits = 32;

// ---------------------------------------------------------------------------
// Pseudo-randomiser, 131.0 section 10
// ---------------------------------------------------------------------------

enum class CcsdsRandomizer : std::uint8_t {
    None,
    // 10.4.2: x^8 + x^7 + x^5 + x^3 + 1 from all ones, period 255. Legacy,
    // and what most spacecraft flying in 2026 still use.
    Legacy255,
    // 10.4.1: x^17 + x^14 + 1 from 11000111000111000, period 131071.
    Long131071,
};

// The first `bits` bits of the sequence, one per byte.
[[nodiscard]] std::vector<std::uint8_t> ccsds_pn_bits(CcsdsRandomizer randomizer, std::size_t bits);

// XORs the sequence into `octets`, first bit at the first octet's most
// significant bit (10.3.2). Its own inverse.
void ccsds_randomize(std::span<std::uint8_t> octets, CcsdsRandomizer randomizer);

// ---------------------------------------------------------------------------
// Reed-Solomon, 131.0 section 4
// ---------------------------------------------------------------------------

struct CcsdsRsConfig {
    // 4.3.1 b): 16 or 8.
    unsigned e = 16;
    // 4.3.5.1: 1, 2, 3, 4, 5 or 8.
    unsigned interleave = 1;
    // Q of 4.3.7.3: virtual fill symbols over the whole codeblock, a multiple
    // of `interleave` and less than k * interleave.
    std::size_t virtual_fill = 0;
};

struct CcsdsRsResult {
    // Per interleaved codeword, the symbols corrected, or nothing when that
    // codeword was refused.
    std::vector<std::optional<std::size_t>> corrected;
    [[nodiscard]] bool ok() const;
    [[nodiscard]] std::size_t total_corrected() const;
};

class CcsdsReedSolomon {
public:
    [[nodiscard]] static Expected<CcsdsReedSolomon> create(const CcsdsRsConfig& config);

    [[nodiscard]] const CcsdsRsConfig& config() const { return config_; }
    // k*I - Q: the Transfer Frame octets a transmitted codeblock carries.
    [[nodiscard]] std::size_t data_octets() const;
    // n*I - Q: the transmitted codeblock, frame plus 2EI check symbols.
    [[nodiscard]] std::size_t codeblock_octets() const;

    // `data` must be data_octets() long; returns the transmitted codeblock,
    // dual basis, interleaved per 4.4.1.
    [[nodiscard]] std::vector<std::uint8_t> encode(std::span<const std::uint8_t> data) const;

    // Corrects a transmitted codeblock in place. A refused codeword is left
    // as received.
    [[nodiscard]] CcsdsRsResult decode(std::span<std::uint8_t> codeblock) const;

    // Symbol representation conversions, exposed for the tests. The
    // conventional form has u_i, the coefficient of alpha^i, in bit i; the
    // dual form has z_0 in the most significant bit, which is the first
    // transmitted (4.3.9.2).
    [[nodiscard]] static std::uint8_t conventional_to_dual(std::uint8_t u);
    [[nodiscard]] static std::uint8_t dual_to_conventional(std::uint8_t z);
    // alpha^exponent in the conventional representation, F(x) of 4.3.3.
    [[nodiscard]] static std::uint8_t alpha_power(unsigned exponent);
    // The code generator of 4.3.4, conventional representation, coefficient
    // of x^0 first: 2E + 1 values.
    [[nodiscard]] static std::vector<std::uint8_t> generator(unsigned e);

private:
    CcsdsReedSolomon() = default;
    CcsdsRsConfig config_{};
    std::optional<Rs8Codec> codec_;
};

// ---------------------------------------------------------------------------
// Transfer frames and packets
// ---------------------------------------------------------------------------

// 132.0 4.1.6: CRC-16, G(X) = X^16 + X^12 + X^5 + 1, register preset to
// ones, no final inversion; the first transmitted bit is the most
// significant.
[[nodiscard]] std::uint16_t ccsds_fecf(std::span<const std::uint8_t> octets);
// True when the last two octets of `frame` are the FECF of the rest.
[[nodiscard]] bool ccsds_fecf_ok(std::span<const std::uint8_t> frame);

// 132.0 4.1.2.7.6.4 and .5, 732.0 4.1.4.2.3.4 and .5.
inline constexpr std::uint16_t kCcsdsNoPacketStarts = 0x7FF;
inline constexpr std::uint16_t kCcsdsOnlyIdleData = 0x7FE;

// 132.0 4.1.2, six octets.
struct TmFrameHeader {
    std::uint8_t version = 0;             // bits 0-1, '00' (4.1.2.2.2)
    std::uint16_t spacecraft_id = 0;      // bits 2-11
    std::uint8_t virtual_channel = 0;     // bits 12-14
    bool ocf_present = false;             // bit 15
    std::uint8_t master_frame_count = 0;  // bits 16-23
    std::uint8_t vc_frame_count = 0;      // bits 24-31
    bool secondary_header = false;        // bit 32
    bool sync_flag = false;               // bit 33
    bool packet_order = false;            // bit 34
    std::uint8_t segment_length_id = 3;   // bits 35-36
    std::uint16_t first_header_pointer = 0;  // bits 37-47
};
inline constexpr std::size_t kTmFrameHeaderOctets = 6;

[[nodiscard]] std::optional<TmFrameHeader> parse_tm_frame_header(std::span<const std::uint8_t> frame);
void write_tm_frame_header(const TmFrameHeader& header, std::span<std::uint8_t> out);

// 732.0 4.1.2, six octets without the optional header error control.
struct AosFrameHeader {
    std::uint8_t version = 1;          // bits 0-1, '01' (4.1.2.2.2)
    std::uint8_t spacecraft_id = 0;    // bits 2-9
    std::uint8_t virtual_channel = 0;  // bits 10-15; 63 is idle (4.1.2.3)
    std::uint32_t vc_frame_count = 0;  // bits 16-39
    bool replay = false;               // bit 40
    bool vc_count_cycle_used = false;  // bit 41
    std::uint8_t vc_count_cycle = 0;   // bits 44-47
};
inline constexpr std::size_t kAosFrameHeaderOctets = 6;

[[nodiscard]] std::optional<AosFrameHeader> parse_aos_frame_header(std::span<const std::uint8_t> frame);
void write_aos_frame_header(const AosFrameHeader& header, std::span<std::uint8_t> out);

// 732.0 4.1.4.2.1.4: the M_PDU header's First Header Pointer, bits 5-15.
[[nodiscard]] std::optional<std::uint16_t> parse_mpdu_first_header_pointer(
    std::span<const std::uint8_t> mpdu);

// 133.0 4.1.3, six octets.
struct SpacePacketHeader {
    std::uint8_t version = 0;          // bits 0-2, '000'
    bool telecommand = false;          // bit 3, packet type
    bool secondary_header = false;     // bit 4
    std::uint16_t apid = 0;            // bits 5-15; 0x7FF is idle (4.1.3.3.4.4)
    std::uint8_t sequence_flags = 3;   // bits 16-17
    std::uint16_t sequence_count = 0;  // bits 18-31
    std::uint16_t data_length = 0;     // bits 32-47: data field octets less one
    [[nodiscard]] std::size_t total_octets() const { return 6U + data_length + 1U; }
};
inline constexpr std::size_t kSpacePacketHeaderOctets = 6;
inline constexpr std::uint16_t kIdleApid = 0x7FF;

[[nodiscard]] std::optional<SpacePacketHeader> parse_space_packet_header(
    std::span<const std::uint8_t> packet);
void write_space_packet_header(const SpacePacketHeader& header, std::span<std::uint8_t> out);

struct SpacePacket {
    SpacePacketHeader header;
    std::vector<std::uint8_t> octets;  // header and data field
};

// Reassembles Space Packets from the packet zones of one virtual channel's
// successive frames. Keep one per virtual channel.
class SpacePacketExtractor {
public:
    // `zone` is the frame's data field (TM) or M_PDU packet zone (AOS), and
    // `first_header_pointer` the frame's. `continuous` false says frames were
    // lost since the last call, which drops any packet in progress.
    void push(std::span<const std::uint8_t> zone, std::uint16_t first_header_pointer,
              bool continuous, std::vector<SpacePacket>& out);
    void reset();
    // Packets dropped because a frame went missing or a pointer disagreed
    // with the length of the packet in progress.
    [[nodiscard]] std::size_t dropped() const { return dropped_; }

private:
    void consume(std::span<const std::uint8_t> bytes, std::vector<SpacePacket>& out);
    std::vector<std::uint8_t> partial_;
    bool in_sync_ = false;
    std::size_t dropped_ = 0;
};

// ---------------------------------------------------------------------------
// The receiver: soft symbols to codeblocks
// ---------------------------------------------------------------------------

struct CcsdsReceiverConfig {
    // Transfer Frame octets when Reed-Solomon is off. With it on, the frame
    // is CcsdsReedSolomon::data_octets() and this is ignored.
    std::size_t frame_octets = 0;
    bool reed_solomon = false;
    CcsdsRsConfig rs;
    CcsdsRandomizer randomizer = CcsdsRandomizer::Legacy255;
    // The basic convolutional code of 131.0 3.3: input is then channel
    // symbols, two per bit.
    bool convolutional = false;
    // Differential (NRZ-M) data, decoded after the convolutional code.
    bool nrzm = false;
    unsigned asm_search_errors = 3;
    unsigned asm_locked_errors = 8;
    // Viterbi traceback; zero picks the decoder's default.
    std::size_t traceback = 0;
};

struct CcsdsFrame {
    // The Transfer Frame: derandomised, Reed-Solomon corrected and with its
    // check symbols removed.
    std::vector<std::uint8_t> frame;
    // The marker matched complemented, so the data were inverted.
    bool inverted = false;
    unsigned asm_errors = 0;
    // Empty when Reed-Solomon is off.
    CcsdsRsResult rs;
    // Decoded bit count, after the convolutional code, at the frame's first
    // bit.
    std::uint64_t bit_position = 0;
};

class CcsdsReceiver {
public:
    [[nodiscard]] static Expected<CcsdsReceiver> create(const CcsdsReceiverConfig& config);

    // Soft channel symbols in the SoftSpan convention; channel bits when the
    // convolutional code is off. Chunks may split anywhere.
    void push(SoftSpan soft, std::vector<CcsdsFrame>& out);
    // Releases what the Viterbi decoder still holds.
    void flush(std::vector<CcsdsFrame>& out);
    void reset();

    [[nodiscard]] bool locked() const;
    [[nodiscard]] std::size_t codeblock_octets() const { return codeblock_octets_; }
    [[nodiscard]] std::size_t frame_octets() const { return frame_octets_; }

private:
    struct Branch {
        std::optional<StreamingViterbi> viterbi;
        std::uint8_t nrzm_previous = 0;
        std::uint32_t shift = 0;
        std::uint64_t bits_seen = 0;
        enum class State : std::uint8_t { Search, Collect, Verify } state = State::Search;
        bool inverted = false;
        unsigned asm_errors = 0;
        std::vector<std::uint8_t> block;  // packed octets
        std::size_t block_bits = 0;
        std::size_t verify_bits = 0;
        std::uint64_t block_start = 0;
    };

    CcsdsReceiver() = default;
    void bits(std::size_t branch, std::span<const std::uint8_t> decided, std::vector<CcsdsFrame>& out);
    // False when the block disproved the lock.
    bool finish_block(std::size_t branch, std::vector<CcsdsFrame>& out);

    CcsdsReceiverConfig config_;
    std::optional<CcsdsReedSolomon> rs_;
    std::size_t frame_octets_ = 0;
    std::size_t codeblock_octets_ = 0;
    std::vector<Branch> branches_;
    // The second branch drops the stream's first symbol, once, so its pairs
    // start one symbol later.
    bool skew_done_ = false;
    // The branch whose output is trusted, or -1 before any lock.
    int active_ = -1;
    std::vector<std::uint8_t> scratch_;
};

}  // namespace revenant::decode
