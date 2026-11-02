// Opulent Voice: soft bits to 134-byte frames, header parsed.
//
// SPECIFICATION
//
// "Opulent Voice Protocol Specification", Version 1.1, January 2026, Open
// Research Institute: opulent_voice_protocol.md in
// github.com/OpenResearchInstitute/interlocutor at commit 1fa86584 (29 January
// 2026), fetched 2026-11-02. Cited below as "OPV" with a section. The
// repository's software is GPL-2.0; this file is written from the document's
// prose and tables, which clean-room.md allows as documentation, and no
// Interlocutor, Dialogus, Locutus or HDL source was read.
//
// WHAT IS DECODED
//
// The frame layer of OPV 8.2 to 8.8, from a stream of soft bits: the 24-bit
// sync word, the 67 x 32 deinterleaver, the K=7 rate 1/2 code through
// dv_codes' Viterbi decoder, the CCSDS randomiser, and the 12-byte header of
// OPV 2.1 with the base-40 station identifier of 2.3.
//
// WHERE IT STOPS
//
// Before the modulation and after the header.
//
// Modulation: OPV 8.1 says MSK at 27100 symbols per second, which does not
// add up with 2168 bits per 40 ms (54200 bit/s), and docs/modes.md records
// that the modulation as published is self-inconsistent and that ORI should
// be asked before a demodulator is written. So this takes soft bits from
// whatever demodulator a caller has, and the tests drive it through an
// antipodal channel with siggen::add_awgn rather than through MSK.
//
// Payload: OPV 3 and 4 put COBS-framed IP, UDP and RTP in the 122 payload
// bytes, with Opus voice inside. libopus is not a dependency of this project
// (vcpkg.json), so the payload is handed out as bytes.
//
// DEVIATIONS AND GAPS IN THE DOCUMENT
//
// OPV gives no worked frame, so only the base-40 test cases of 2.3 are
// checked against the document; the coding chain is checked by round trip.
// Three readings had to be chosen and are the first things to check against
// a capture:
//
//   - OPV 8.5 gives G1 = 171 and G2 = 133 octal, 1111001 and 1011011, and
//     does not say which end multiplies the current input. This takes the
//     leftmost bit as the current input, the usual reading of those octal
//     names. Nor does it say G2 is inverted, so it is not.
//   - OPV 8.4 and 8.5 do not say in what order a byte's bits enter the
//     encoder. Most significant first is taken, as 8.2 says of the sync word
//     and 8.4 of the randomiser.
//   - OPV 2.3's "char[i] x 40^position" does not say whether position counts
//     from the first character. Its test cases settle it: W1AW is 0x1680B7,
//     which is W + 1*40 + A*40^2 + W*40^3, so the first character is the
//     least significant digit.
//
// OPV 8.2 calls the sync word "balanced 0/1 count"; 0x02B8DB has eleven ones
// and thirteen zeros. The hex and binary it prints agree, so the value is
// what is implemented.
//
// OPV 8.4 names the randomiser CCSDS but gives its own register description,
// x^8 + x^7 + x^5 + x^3 + 1, seed all ones, output the top bit, feedback
// s7 ^ s6 ^ s4 ^ s2; that description is what is implemented, and no CCSDS
// document was consulted.
//
// The frame has no CRC (OPV 2.1). A sync match alone, 24 bits at the 8.2
// hunting threshold, is a chance event about every 7000 bits of noise, so a
// frame is accepted only when re-encoding the Viterbi output agrees with the
// received hard bits; that check is this file's, not the document's.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.h"

namespace revenant::decode {

// OPV 2.1 and 8.1.
inline constexpr std::size_t kOpvFrameBytes = 134;
inline constexpr std::size_t kOpvHeaderBytes = 12;
inline constexpr std::size_t kOpvPayloadBytes = 122;
inline constexpr std::size_t kOpvEncodedBits = 2144;

// OPV 8.2 and 8.7.
inline constexpr std::uint32_t kOpvSync = 0x02B8DB;
inline constexpr std::size_t kOpvSyncBits = 24;
inline constexpr std::size_t kOpvFrameBits = kOpvSyncBits + kOpvEncodedBits;  // 2168

// OPV 8.4.
inline constexpr std::uint8_t kOpvRandomizerSeed = 0xFF;

// OPV 8.5: G1 = 171 and G2 = 133 octal, leftmost bit the current input (see
// DEVIATIONS), written here with the coefficient of D^0 in the least
// significant bit as core/decode/dv_codes.h wants: 1111001 reversed is
// 1001111, 1011011 reversed is 1101101.
inline constexpr std::uint32_t kOpvConvGenerators[2] = {0b1001111U, 0b1101101U};
inline constexpr std::uint32_t kOpvConvMemory = 6;

// OPV 8.6.
inline constexpr std::size_t kOpvInterleaveRows = 67;
inline constexpr std::size_t kOpvInterleaveColumns = 32;

// OPV 2.1, implementation example: Interlocutor's default token.
inline constexpr std::uint32_t kOpvDefaultToken = 0xBBAADD;

// OPV 2.3: base-40, the first character the least significant digit. Fails
// on a character outside the table or a value that does not fit 48 bits.
[[nodiscard]] Expected<std::uint64_t> opv_encode_station(std::string_view station);
[[nodiscard]] std::string opv_decode_station(std::uint64_t value);

// OPV 8.4, in place over bytes, from the seed. Self-inverse.
void opv_randomize(std::span<std::uint8_t> bytes);

// OPV 8.6: input position p to output position (p mod 32) * 67 + p / 32.
[[nodiscard]] std::size_t opv_interleave_position(std::size_t p);

// OPV 8.3 forward: 134 bytes to the 2168 transmitted bits, sync first.
[[nodiscard]] Expected<std::vector<std::uint8_t>> opv_encode_frame(
    std::span<const std::uint8_t> frame);

struct OpvHeader {
    std::uint64_t station_value = 0;
    std::string station;
    std::uint32_t token = 0;
    std::uint32_t reserved = 0;
};

[[nodiscard]] OpvHeader opv_parse_header(std::span<const std::uint8_t> frame);

struct OpvFrame {
    // Index, in the soft bit stream, of the first sync bit.
    std::uint64_t first_bit = 0;
    double sync_score = 0.0;
    bool inverted = false;

    // Fraction of the 2144 received hard bits that re-encoding the decoded
    // frame reproduces: 1.0 on a clean frame, near 0.5 on noise.
    double agreement = 0.0;

    std::array<std::uint8_t, kOpvFrameBytes> bytes{};
    OpvHeader header;
};

struct OpvConfig {
    // OPV 8.2: "Hunting threshold: ~70-80% of peak correlation".
    double hunting_threshold = 0.75;
    // OPV 8.2: "Locked threshold: ~40-50% of peak (flywheel mode)".
    double locked_threshold = 0.45;
    // NOT A SPECIFIED VALUE. See DEVIATIONS: the agreement a frame needs to
    // be accepted. Measured on 2026-11-02 in tests/decode/test_opulent_voice.cpp:
    // the Viterbi decoder finds a path that agrees with pure noise far more
    // than half the time, and 60 s of noise put 1168 frames through at 0.8,
    // the highest at 0.846. Through the antipodal channel the least agreement
    // of 40 frames was 0.931 at 1 dB per coded bit and 0.907 at 0 dB, where
    // 6 of the 40 were already wrong.
    double agreement_threshold = 0.9;
};

// Soft bits in, frames out. Soft values follow core/decode/dv_codes.h:
// positive for a zero bit.
class OpvDeframer {
   public:
    [[nodiscard]] static Expected<OpvDeframer> create(const OpvConfig& config);

    [[nodiscard]] Status process(std::span<const float> soft, std::vector<OpvFrame>& out);

    void reset();

   private:
    OpvDeframer() = default;

    [[nodiscard]] double score_at(std::size_t offset) const;
    [[nodiscard]] std::optional<OpvFrame> decode_at(std::size_t offset, double score);

    OpvConfig config_{};
    std::vector<float> buffer_;
    std::uint64_t buffer_start_ = 0;
    std::size_t cursor_ = 0;
    bool locked_ = false;
    bool inverted_ = false;
};

}  // namespace revenant::decode
