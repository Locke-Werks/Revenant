// M17: complex baseband to link setup frames and stream frames.
//
// SPECIFICATION
//
// "M17 Protocol Specification, Part I - Air Interface", Version 2.0.4, 21
// January 2026, the M17 Project, from spec.m17project.org/files/M17_spec.pdf.
// Cited below as "M17" with a section, table or appendix. The document text is
// under the GNU Free Documentation License 1.3 or later, per its Licenses
// page. The same page puts the software listings inside the document under
// GPL-2.0-or-later, which is why this file is written from the prose, the
// tables and the equations and not from any listing; see CLEAN ROOM below.
//
// WHAT IS DECODED
//
//   - the 4FSK physical layer, M17 1.1 to 1.4, through core/decode/dv_phy.h's
//     discriminator, root raised cosine and square-law timing, the same input
//     shape P25 and D-STAR take;
//   - the four synchronisation bursts of Table 2.3 and the End of
//     Transmission marker of 1.4.5;
//   - the Link Setup Frame, 2.5 to 2.7: randomiser, QPP interleaver, P1
//     puncturing, the K=5 convolutional code through dv_codes' Viterbi
//     decoder, and the CRC of 2.6;
//   - its address fields in the base-40 encoding of Appendix A, and the TYPE
//     field of 3.2.1;
//   - Stream Frames, 2.8.1: the Link Information Channel through the
//     Golay(24,12) code of Appendix D, the frame number and its end-of-stream
//     bit, and the 128 payload bits through P2 puncturing; and the LSF
//     reassembled from six LICH chunks for a receiver that joined late.
//
// WHERE IT STOPS
//
// At the 128 stream bits. Voice in them is Codec 2, which docs/modes.md
// covers separately, and this file does not decode it. Packet Mode, 2.9, and
// BERT Mode, 2.10, are recognised by their sync bursts and not decoded;
// BERT's case is recorded in docs/modes.md, because its PRBS9 receiver was
// read in the specification's embedded listing and so is not written here by
// the person who read it.
//
// CLEAN ROOM
//
// No M17 implementation was read, and none of the specification's embedded
// software listings was used. Two exposures to those listings happened while
// reading the document and are recorded rather than waved through: the last
// line of the Appendix A.5 decoder example, a closing brace and return, was
// visible above Appendix B in a text dump; and the PRBS9 generator and
// synchroniser listings of Appendix G, figures G.3 to G.5, were read in full
// before their licence was noticed. Nothing in this file implements Appendix
// G. The Golay code here is built from Appendix D's generator polynomial and
// checked against its printed matrix. The MATLAB snippet under that matrix
// was not read as such, though its first lines showed in a search of the text
// dump for section headings; they build the same matrix from the same
// polynomial, which the appendix prose already states.
//
// NOTHING HERE IS BIT EXACT AGAINST A GPU TWIN
//
// Host code with no kernel behind it. The round trip in
// tests/decode/test_m17.cpp against core/dsp/synth/m17_mod.h stands in its
// place, with the specification's own test vectors for the CRC and the
// address encoding.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/decode/dv_phy.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::SampleIndex;

// ---------------------------------------------------------------------------
// Physical layer constants
// ---------------------------------------------------------------------------

// M17 1.1: 4FSK at 4800 symbols per second.
inline constexpr double kM17SymbolRate = 4800.0;

// M17 Table 1.1: +3 and +1 deviate by +2.4 and +0.8 kHz, so 800 Hz per unit.
inline constexpr double kM17DeviationPerUnitHz = 800.0;

// M17 1.3: root raised cosine, roll-off 0.5, spanning at least 8 symbols,
// "81 taps at the recommended up-sample rate" of 10.
inline constexpr double kM17RrcRollOff = 0.5;
inline constexpr std::size_t kM17RrcSpanSymbols = 8;

// M17 Table 1.1, as the 1.2 worked example fixes it: the byte 0xB4, dibits
// 10 11 01 00, "would be sent as the symbols (-1, -3, +3, +1)". Indexed by
// dibit value, most significant bit first.
//
// THE TABLE'S OWN LAYOUT IS SHIFTED BY A ROW in a text extraction of the PDF,
// which puts 00 beside +3. The worked example is unambiguous and is what the
// test checks.
inline constexpr std::array<int, 4> kM17DibitToSymbol = {+1, +3, -1, -3};

// M17 2.1: every frame is a 16-bit sync burst and 368 payload bits, 192
// symbols, 40 ms.
inline constexpr std::size_t kM17SyncSymbols = 8;
inline constexpr std::size_t kM17PayloadBits = 368;
inline constexpr std::size_t kM17FrameSymbols = 192;

// M17 1.4.1 and 1.4.5: preamble and End of Transmission are 192 symbols each.
inline constexpr std::size_t kM17PreambleSymbols = 192;
inline constexpr std::size_t kM17EotSymbols = 192;

// M17 Table 2.3, the sync bursts as 16-bit words.
inline constexpr std::uint16_t kM17SyncLsf = 0x55F7;
inline constexpr std::uint16_t kM17SyncBert = 0xDF55;
inline constexpr std::uint16_t kM17SyncStream = 0xFF5D;
inline constexpr std::uint16_t kM17SyncPacket = 0x75FF;

// M17 1.4.5: the End of Transmission marker repeats 0x555D.
inline constexpr std::uint16_t kM17EotWord = 0x555D;

// M17 Appendix B, Table B.1: the 46-byte randomiser sequence, XORed into the
// 368 payload bits most significant bit first per 1.4.4.
inline constexpr std::array<std::uint8_t, 46> kM17Randomizer = {
    0xD6, 0xB5, 0xE2, 0x30, 0x82, 0xFF, 0x84, 0x62, 0xBA, 0x4E, 0x96, 0x90,
    0xD8, 0x98, 0xDD, 0x5D, 0x0C, 0xC8, 0x52, 0x43, 0x91, 0x1D, 0xF8, 0x6E,
    0x68, 0x2F, 0x35, 0xDA, 0x14, 0xEA, 0xCD, 0x76, 0x19, 0x8D, 0xD5, 0x80,
    0xD1, 0x33, 0x87, 0x13, 0x57, 0x18, 0x2D, 0x29, 0x78, 0xC3,
};

// ---------------------------------------------------------------------------
// Coding constants
// ---------------------------------------------------------------------------

// M17 Appendix C: rate 1/2, K=5, G1(D) = 1 + D^3 + D^4 and
// G2(D) = 1 + D + D^2 + D^4, "read alternately", G1 first. Coefficient of
// D^0 in the least significant bit, as core/decode/dv_codes.h writes them.
inline constexpr std::uint32_t kM17ConvGenerators[2] = {0b11001U, 0b10111U};
inline constexpr std::uint32_t kM17ConvMemory = 4;

// M17 Appendix E: P1 is a leading 1 then fifteen copies of the rate 2/3
// matrix M = [1 0 1 1], 61 entries with 46 ones, used eight times across the
// LSF's 488 bits. P2 keeps eleven of every twelve.
[[nodiscard]] std::array<std::uint8_t, 61> m17_puncture_p1();
inline constexpr std::array<std::uint8_t, 12> kM17PunctureP2 = {1, 1, 1, 1, 1, 1,
                                                                 1, 1, 1, 1, 1, 0};

// M17 Appendix F: the QPP interleaver pi(x) = (45x + 92x^2) mod 368. It is an
// involution, pi(pi(x)) = x for every x, which the test checks, so reading
// the appendix's "input index, output index" table in either direction gives
// the same permutation.
[[nodiscard]] std::uint32_t m17_interleave(std::uint32_t index);

// M17 Appendix D: the Golay(23,12) generator x^11 + x^10 + x^6 + x^5 + x^4 +
// x^2 + 1, 0xC75, extended by a parity bit to (24,12). Table D.1: data in bits
// 23..12, check bits in 11..1, parity in bit 0.
inline constexpr std::uint32_t kM17GolayGenerator = 0xC75;

// M17 2.6: CRC-16, polynomial 0x5935, initial value 0xFFFF, neither input
// nor output reflected, no final XOR.
inline constexpr std::uint16_t kM17CrcPolynomial = 0x5935;
inline constexpr std::uint16_t kM17CrcInitial = 0xFFFF;

// M17 2.5.2 and 2.8.1: the LSF is 30 bytes, 240 bits; a stream frame carries
// a 16-bit frame number and 128 bits of stream data; the LICH is 48 bits,
// a 40-bit LSF chunk then a 3-bit counter.
inline constexpr std::size_t kM17LsfBytes = 30;
inline constexpr std::size_t kM17StreamPayloadBytes = 16;
inline constexpr std::size_t kM17LichChunkBytes = 5;
inline constexpr std::size_t kM17LichChunks = 6;

// ---------------------------------------------------------------------------
// The codes, exposed so the transmitter and the tests share them
// ---------------------------------------------------------------------------

[[nodiscard]] std::uint16_t m17_crc(std::span<const std::uint8_t> bytes);

// Encodes 12 data bits to the 24-bit codeword of Table D.1.
[[nodiscard]] std::uint32_t m17_golay_encode(std::uint16_t data);

struct M17GolayDecode {
    std::uint16_t data = 0;
    std::uint32_t corrected_bits = 0;
};

// Soft maximum likelihood over all 4096 code words: the one that best agrees
// with 24 soft bits in the dv_codes sign convention, positive for zero. The
// code's minimum distance is 8, so three errors are always corrected and a
// fourth is at best detected.
[[nodiscard]] M17GolayDecode m17_golay_decode(std::span<const float> soft);

// Appendix A: a callsign of up to nine characters from the 40-character
// alphabet of Table A.1 to its 48-bit address, and back. Encoding maps any
// character outside the alphabet to 0, as Table A.1 says ("also, any invalid
// character"), after upper-casing letters.
[[nodiscard]] Expected<std::uint64_t> m17_encode_callsign(std::string_view callsign);

enum class M17AddressKind {
    Reserved,   // 0x000000000000, Table A.2
    Standard,   // 0x000000000001 to 0xEE6B27FFFFFF
    Extended,   // 0xEE6B28000000 to 0xFFFFFFFFFFFE
    Broadcast,  // 0xFFFFFFFFFFFF
};

struct M17Address {
    std::uint64_t value = 0;
    M17AddressKind kind = M17AddressKind::Reserved;

    // The decoded callsign, trailing spaces removed, when kind is Standard.
    std::string callsign;
};

[[nodiscard]] M17Address m17_decode_address(std::uint64_t value);

// M17 3.2.1, Table 3.2: the TYPE field of a stream LSF.
struct M17Type {
    std::uint16_t raw = 0;
    bool stream = false;               // bit 0, Table 3.3
    std::uint8_t data_type = 0;        // bits 2..1, Table 3.4
    std::uint8_t encryption_type = 0;  // bits 4..3, Table 3.5
    std::uint8_t encryption_subtype = 0;  // bits 6..5, Table 3.6
    std::uint8_t channel_access_number = 0;  // bits 10..7
    bool signed_stream = false;        // bit 11
};

[[nodiscard]] M17Type m17_parse_type(std::uint16_t raw);

struct M17Lsf {
    M17Address destination;
    M17Address source;
    M17Type type;
    std::array<std::uint8_t, 14> meta{};
    std::uint16_t crc = 0;
    bool crc_valid = false;
};

[[nodiscard]] M17Lsf m17_parse_lsf(std::span<const std::uint8_t> bytes);

// The 30 LSF bytes of 2.5.2, CRC computed and filled in.
[[nodiscard]] std::array<std::uint8_t, kM17LsfBytes> m17_lsf_bytes(std::uint64_t destination,
                                                                   std::uint64_t source,
                                                                   std::uint16_t type,
                                                                   std::span<const std::uint8_t> meta);

// 2.7 forward: 30 LSF bytes to the 368 Type 4 bits, before the randomiser.
[[nodiscard]] Expected<std::vector<std::uint8_t>> m17_encode_lsf(
    std::span<const std::uint8_t> lsf);

// 2.7 in reverse, from 368 soft Type 4 bits already de-randomised.
[[nodiscard]] Expected<std::array<std::uint8_t, kM17LsfBytes>> m17_decode_lsf(
    std::span<const float> soft);

// 2.8.1 forward: a LICH chunk, its counter, the frame number and 16 payload
// bytes to the 368 Type 4 bits, before the randomiser.
[[nodiscard]] Expected<std::vector<std::uint8_t>> m17_encode_stream_frame(
    std::span<const std::uint8_t> lich_chunk, std::uint8_t lich_count, std::uint16_t frame_number,
    std::span<const std::uint8_t> payload);

struct M17StreamFrame {
    std::array<std::uint8_t, kM17LichChunkBytes> lich_chunk{};
    std::uint8_t lich_count = 0;

    // Largest correction any of the four LICH Golay words took.
    std::uint32_t lich_worst_correction = 0;

    // 2.8.1: the frame number is 15 bits and its top bit marks the last
    // frame of the stream.
    std::uint16_t frame_number = 0;
    bool last = false;
    std::array<std::uint8_t, kM17StreamPayloadBytes> payload{};
};

// 2.8.1 in reverse.
[[nodiscard]] Expected<M17StreamFrame> m17_decode_stream_frame(std::span<const float> soft);

// XORs the Appendix B sequence into 368 bits, in place. Self-inverse.
void m17_randomize(std::span<std::uint8_t> bits);

// The eight symbols of a 16-bit word, 1.2's order.
[[nodiscard]] std::array<float, kM17SyncSymbols> m17_word_symbols(std::uint16_t word);

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

enum class M17FrameKind {
    LinkSetup,
    Stream,
    Packet,  // recognised by its sync burst; not decoded
    Bert,    // recognised by its sync burst; not decoded
    EndOfTransmission,
};

struct M17Frame {
    M17FrameKind kind = M17FrameKind::LinkSetup;

    // Input sample index of the first symbol of the sync burst.
    SampleIndex first_sample = 0;

    // Normalised correlation of the sync burst, and whether the discriminator
    // polarity was inverted.
    double sync_score = 0.0;
    bool inverted = false;

    // Set for LinkSetup, and for a Stream frame that completed an LSF from
    // six LICH chunks, in which case `lsf_from_lich` says so.
    std::optional<M17Lsf> lsf;
    bool lsf_from_lich = false;

    std::optional<M17StreamFrame> stream;
};

struct M17Config {
    SampleRate rate = 48000;

    // Normalised correlation a sync burst must reach to start a transmission.
    // NOT A SPECIFIED VALUE. The bursts are eight outer symbols, so one
    // symbol wrong costs 0.25 of correlation: 0.8 admits no wrong symbol and
    // some noise on the rest. The preamble that must stand in front of an LSF
    // or BERT burst, checked over 32 of its 1.4.1 192 symbols to the same
    // threshold, or for a stream joined late a second stream burst a frame on
    // whose Frame Number is one more than the first's, is what keeps a chance
    // match in noise or in the payload from starting anything.
    //
    // WHAT THIS USED TO SAY, until 2026-09-23: "the preamble check that goes
    // with an LSF burst, or a second burst 192 symbols on for a stream joined
    // late, is what keeps a chance match in the payload from starting
    // anything." The preamble check was eight symbols, and on noise alone the
    // two together started 1615 LSFs an hour and let 9808 frames through;
    // tests/decode/test_m17_noise.cpp.
    double sync_threshold = 0.8;

    // The same, for a burst where an ongoing transmission says one is due.
    // Lower, because its position is already known and only its kind is
    // being read, and stated from Table 2.3's distances. Clause 2.4 lets a
    // transmission started by an LSF carry stream or packet bursts, one mode
    // to a transmission, then the end marker; one started on BERT, BERT
    // bursts and the end marker. Among stream, packet and the 1.4.5 end
    // marker the nearest two differ in four of eight symbols and correlate at
    // 0, so 0.5 is halfway from a perfect match to the nearest other word.
    // Until 2026-09-23 BERT was a candidate after an LSF too, and BERT is two
    // symbols from a stream burst, correlating at 0.5 with it: this threshold
    // then separated nothing, and noise kept a false transmission going on
    // BERT and packet bursts. 0.625, between one and two wrong symbols, was
    // measured as well, before the late join asked for counting Frame
    // Numbers: on noise it gave 29 frames an hour against 31, and at 10 dB it
    // lost 7 more of 600 stream payloads.
    double tracking_threshold = 0.5;
};

class M17 {
   public:
    [[nodiscard]] static Expected<M17> create(const M17Config& config);

    // Consumes complex baseband and appends every frame completed by it.
    [[nodiscard]] Status process(ConstComplexSpan samples, std::vector<M17Frame>& out);

    // Every recovered symbol from the last call, normalised so the four
    // levels sit near +/-1 and +/-3 before any per-frame calibration. For
    // symbol error measurement.
    [[nodiscard]] std::span<const float> last_symbols() const { return last_symbols_; }

    void reset();

   private:
    M17() = default;

    void search(std::vector<M17Frame>& out);
    void decode_stream(std::size_t offset, M17Frame& frame);
    [[nodiscard]] double score_at(std::size_t offset, std::uint16_t word) const;

    // The 368 soft payload bits of the frame whose burst starts at `offset`,
    // calibrated against the burst and de-randomised. Updates the running
    // level calibration, which is why it is not const.
    [[nodiscard]] std::vector<float> soft_payload(std::size_t offset, bool inverted,
                                                  std::uint16_t word);

    M17Config config_{};
    std::vector<float> taps_;
    SymbolSync sync_{};

    // Discriminator and filter state carried across calls.
    Complex32 previous_{1.0F, 0.0F};
    std::vector<float> history_;
    std::uint64_t samples_in_ = 0;

    std::vector<float> symbols_;
    std::vector<SampleIndex> positions_;
    std::size_t cursor_ = 0;

    bool in_transmission_ = false;
    bool inverted_ = false;
    bool have_lsf_ = false;

    // Which bursts clause 2.4 lets the transmission in hand carry: Unknown
    // after an LSF whose CRC failed, until its first stream or packet burst
    // says.
    enum class Mode : std::uint8_t { Unknown, Stream, Packet, Bert };
    Mode mode_ = Mode::Unknown;

    // Running symbol level calibration within a transmission: the gain that
    // puts the outer levels at +/-3 and the offset a carrier error leaves.
    bool calibrated_ = false;
    double gain_ = 1.0;
    double level_offset_ = 0.0;
    std::array<std::array<std::uint8_t, kM17LichChunkBytes>, kM17LichChunks> chunks_{};
    std::uint8_t chunk_mask_ = 0;

    std::vector<float> last_symbols_;
    std::vector<RecoveredSymbol> recovered_;
};

}  // namespace revenant::decode
