// D-STAR DV: complex baseband to the radio header and the voice frame stream.
//
// SPECIFICATION
//
// Implements the JARL standard "アマチュア無線のデジタル化技術の標準方式"
// (Standard Specifications for Digitization Technology of Amateur Radio),
// abbreviated D-STAR, version 7.0, from jarl.com/d-star/STD7_0.pdf. The
// clauses used are 4.1.1 (data packet frame structure, which clause 4.1.2 a
// says the voice packet's radio header shares), 4.1.2 (voice packet frame
// structure), Ap1 (the scrambler and the transmission order) and Ap2 (error
// correction and interleaving).
//
// THE ENGLISH EDITION DOES NOT EXIST ANY MORE
//
// docs/modes.md used to say the English edition was free at jarl.org. On
// 2026-09-21 it is not: the sentence offering it is still on
// jarl.com/d-star/shogen.htm, commented out in the HTML with an empty href.
// Version 7.0 in Japanese is the first-party document and is what this file is
// written from, so the clause numbers here are that document's.
//
// WHAT THE DOCUMENT DOES NOT SAY, AND THIS THEREFORE DOES NOT ASSUME
//
// It names GMSK as the modulation and never states a bandwidth-time product.
// docs/modes.md carried "BT 0.5" and that number is not in the standard. The
// demodulator here slices the discriminator output and does not depend on
// which BT the transmitter used; DStarConfig::bandwidth_time exists only so
// the test's transmitter and this share one number, and its default is
// labelled as an engineering choice rather than a citation.
//
// WHERE THIS STOPS
//
// At the bits. The voice payload is AMBE, which has no published algorithm at
// any price, so this recovers the 72 payload bits per frame and hands them
// over without attempting to render them. docs/modes.md has the reasoning and
// it is a permanent exclusion rather than a pending one.
//
// CLEAN ROOM
//
// No D-STAR implementation was read. Every constant names its clause. Two
// places where the standard contradicts itself are recorded at the constant
// rather than quietly resolved; see core/decode/dv_codes.h on the Ap2.1
// generator polynomial.
//
// NOTHING HERE IS BIT EXACT AGAINST A GPU TWIN
//
// Host code with no kernel behind it. The round trip in
// tests/decode/test_dstar.cpp against core/dsp/synth/dv_mod.h is what stands
// in place of the project's zero-ULP rule, exactly as for RDS.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/decode/dv_phy.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::ConstComplexSpan;
using dsp::SampleRate;

// ---------------------------------------------------------------------------
// Specified constants
// ---------------------------------------------------------------------------

// Clause 4.1.2 b: a 72 bit voice signal every 20 ms, with a 24 bit data frame
// beside it, so the channel runs at 96 bits per 20 ms.
inline constexpr double kDStarBitRate = 4800.0;
inline constexpr std::size_t kDStarVoiceBits = 72;
inline constexpr std::size_t kDStarDataBits = 24;
inline constexpr std::size_t kDStarFrameBits = kDStarVoiceBits + kDStarDataBits;

// Clause 4.1.1 a: for GMSK the bit sync is "1010" repeated, 64 bits standard.
inline constexpr std::size_t kDStarBitSyncBits = 64;

// Clause 4.1.1 b: the frame sync is the 15 bits below, sent with the leftmost
// bit first. Note that this field is explicitly most significant first, unlike
// the byte-oriented fields that Ap1.4 sends least significant first.
inline constexpr std::array<std::uint8_t, 15> kDStarFrameSync = {
    1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0,
};

// Clause 4.1.2 d: the resynchronisation signal is a 10 bit bit-sync followed
// by the 7 bit third-order maximal length sequence "1101000" twice, 24 bits
// in all, and the clause writes the whole thing out for GMSK as
// "101010101011010001101000". It occupies the 24 bit data frame slot.
inline constexpr std::array<std::uint8_t, 24> kDStarResync = {
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0,
};

// Clause 4.1.2 c: the first data frame, and then every 21st voice frame,
// carries the resynchronisation signal in place of data.
inline constexpr std::size_t kDStarResyncInterval = 21;

// Clause 4.1.2 h: the last frame is 32 bits of the repeated sync pattern, then
// the 15 bits "000100110101111", then a single "0", 48 bits in all.
inline constexpr std::array<std::uint8_t, 16> kDStarLastFrameTail = {
    0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1, 1, 1, 1, 0,
};
inline constexpr std::size_t kDStarLastFrameBits = 48;

// Clause 4.1.2's frame diagram: flag 1, flag 2 and flag 3 at one byte each,
// four callsign fields at 8, 8, 8 and 8 bytes, own callsign 2 at 4 bytes, and
// P_FCS at 2 bytes. 41 bytes, and the diagram labels the error-corrected span
// 660 bits, which is (41*8 + 2) * 2.
inline constexpr std::size_t kDStarCallsignBytes = 8;
inline constexpr std::size_t kDStarOwnCallsign2Bytes = 4;

// ---------------------------------------------------------------------------
// What comes out
// ---------------------------------------------------------------------------

// Clause 4.1.1 c, the flag 1 byte. Bit 7 is the most significant.
struct DStarFlags {
    bool data = false;           // bit 7: set for data, clear for voice
    bool via_repeater = false;   // bit 6: set when addressed to a repeater
    bool interrupted = false;    // bit 5
    bool control = false;        // bit 4: set for a control signal
    bool emergency = false;      // bit 3

    // Bits 2 to 0, the response code clause 4.1.1 c tabulates: 111 repeater
    // control, 110 automatic response, 101 unused, 100 retransmit request,
    // 011 ACK, 010 no response, 001 relay not possible, 000 null.
    std::uint8_t response = 0;
};

struct DStarHeader {
    std::uint8_t flag1 = 0;
    std::uint8_t flag2 = 0;
    std::uint8_t flag3 = 0;
    DStarFlags flags;

    // Clause 4.1.1 f through j. ASCII, space padded. Held as strings with the
    // trailing spaces removed, because a trailing space is padding the clause
    // specifies rather than part of a callsign.
    std::string destination_repeater;  // 送り先レピータ局コールサイン
    std::string departure_repeater;    // 送り元レピータ局コールサイン
    std::string companion;             // 相手局コールサイン
    std::string own_callsign;          // 自局コールサイン1
    std::string own_suffix;            // 自局コールサイン2, 4 characters

    std::uint16_t fcs = 0;
    bool fcs_valid = false;
};

struct DStarVoiceFrame {
    // Clause 4.1.2 b. 72 bits of AMBE, one bit per byte, in transmission
    // order. Not interpreted here: see the header comment.
    std::array<std::uint8_t, kDStarVoiceBits> voice{};

    // The 24 bit slot beside it, which clause 4.1.2 e says carries whatever
    // the user put there and clause 4.1.2 c says carries the resynchronisation
    // signal on every 21st frame instead.
    std::array<std::uint8_t, kDStarDataBits> data{};

    bool carried_resync = false;
};

struct DStarTransmission {
    std::optional<DStarHeader> header;
    std::vector<DStarVoiceFrame> frames;

    // True when the clause 4.1.2 h last-frame pattern closed the stream, as
    // opposed to the capture simply running out.
    bool ended = false;

    // Index into the recovered bit stream at which the frame sync was found,
    // and whether the discriminator polarity was inverted.
    std::size_t first_bit = 0;
    double sync_score = 0.0;
    bool inverted = false;
};

struct DStarConfig {
    SampleRate rate = 48000;

    // NOT A SPECIFIED VALUE. The JARL standard names GMSK and states no
    // bandwidth-time product. 0.5 is the value docs/modes.md carried and is
    // kept as the default so the transmitter in core/dsp/synth/dv_mod.h and
    // this agree, which is all the round trip needs. A real capture will have
    // whatever BT its transmitter used and this demodulator does not depend on
    // the number: it slices the discriminator output, and BT changes how much
    // intersymbol interference it has to slice through rather than what the
    // levels mean.
    double bandwidth_time = 0.5;

    // Correlation the frame sync must reach. An engineering choice. The sync
    // word is 15 bits, so 0.8 admits one wrong bit and rejects the sidelobes
    // of the alternating bit-sync pattern that precedes it.
    double sync_threshold = 0.8;

    std::size_t filter_taps = 65;
};

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

class DStar {
   public:
    [[nodiscard]] static Expected<DStar> create(const DStarConfig& config);

    // Consumes complex baseband and appends every transmission whose frame
    // sync and radio header were recovered from it.
    [[nodiscard]] Status process(ConstComplexSpan samples, std::vector<DStarTransmission>& out);

    [[nodiscard]] std::span<const std::uint8_t> last_bits() const { return last_bits_; }

    void reset();

   private:
    DStar() = default;

    [[nodiscard]] Expected<DStarHeader> decode_header(std::span<const float> soft) const;

    DStarConfig config_{};
    std::vector<float> filter_taps_;
    SymbolSync sync_{};

    std::vector<float> soft_bits_;
    std::vector<std::uint8_t> last_bits_;
    std::size_t consumed_ = 0;

    std::vector<RecoveredSymbol> recovered_;
    std::vector<float> discriminated_;
    std::vector<float> filtered_;
};

// ---------------------------------------------------------------------------
// The header codec, exposed so the transmitter and the tests share it
// ---------------------------------------------------------------------------

// The 41 header bytes, in the clause 4.1.2 frame order.
[[nodiscard]] std::array<std::uint8_t, 41> dstar_header_bytes(const DStarHeader& header);

// Ap2.1 and Ap2.2 forward: 41 header bytes to the 660 bits that go on the air,
// through the rate 1/2 convolutional code, the 24 bit interleave and the Ap1.1
// scrambler, with each byte fed to the encoder least significant bit first per
// Ap2.1 step 2 and Ap1.4.
[[nodiscard]] Expected<std::vector<std::uint8_t>> dstar_encode_header(
    std::span<const std::uint8_t> bytes);

// The same chain in reverse, from 660 soft bits to 41 bytes. Soft rather than
// hard because the Viterbi decoder is worth about two decibels over a hard
// one and the caller has the soft values anyway.
[[nodiscard]] Expected<std::array<std::uint8_t, 41>> dstar_decode_header(
    std::span<const float> soft);

// Parses the 41 bytes into fields and checks the FCS.
[[nodiscard]] DStarHeader dstar_parse_header(std::span<const std::uint8_t> bytes);

}  // namespace revenant::decode
