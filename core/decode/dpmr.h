// dPMR: complex baseband to header, payload and end frames, for dPMR446
// (Mode 1) and the licensed Mode 2 peer-to-peer air interface.
//
// SPECIFICATION
//
// ETSI TS 102 490 V1.9.1 (2016-08), "Peer-to-Peer Digital Private Mobile
// Radio using FDMA with a channel spacing of 6,25 kHz with e.r.p. of up to
// 500 mW", from www.etsi.org/deliver/etsi_ts/102400_102499/102490/01.09.01_60/,
// fetched 2026-11-02. Cited below as "490" with a clause, table or figure.
//
// ETSI TS 102 658 V2.6.1 (2019-01), "Digital Private Mobile Radio (dPMR)
// using FDMA with a channel spacing of 6,25 kHz", from
// www.etsi.org/deliver/etsi_ts/102600_102699/102658/02.06.01_60/, fetched
// 2026-11-02. Cited as "658". Its clauses 6 and 7 restate 490's sync words,
// CRCs, Hamming code, scrambler and interleavers with the same values, and its
// tables 5.5 to 5.8 give the same 41-bit CCH with 490's two reserved bits
// split into an Emergency Priority bit and one reserved bit.
//
// WHAT IS DECODED
//
//   - the 4FSK physical layer of 490 clause 12.2, 2400 symbols per second,
//     through core/decode/dv_phy.h;
//   - the four frame syncs of 490 clause 6.1 and the channel code of 6.1.5;
//   - the Header (490 5.2 and 9.5), its HI field through the CRC8, the
//     (12,8) Hamming code, the 12 x 10 interleaver and the scrambler;
//   - payload frames (490 5.1 and 9.1): the CCH through CRC7, Hamming, the
//     12 x 6 interleaver and scrambler, with the 24-bit called and own IDs
//     reassembled across a superframe, and the four 72-bit TCH blocks handed
//     out untouched;
//   - the End frame (490 5.3 and 9.6), both copies.
//
// WHERE IT STOPS
//
// At the TCH. In a voice frame those 288 bits are AMBE+2 with its own FEC,
// which no document describes (docs/modes.md, "Digital voice, collected"),
// so they go to the vocoder plugin socket the way DMR's do. Type 1, 2 and 3
// data payloads are not unpacked. TS 102 658 Mode 2 headers are Message
// Frames whose MI field has per-message layouts (658 clause 5.2); the 72 MI
// bits are delivered after their FEC and CRC with 490's HI field reading
// applied, which is right for a 490 header and only a raw view for a 658
// message. Mode 3 trunking (658's beacon, SYScast and control messages) is
// not decoded. The 490 annex A seven-digit dialable address is not decoded;
// the 24-bit ID is.
//
// DEVIATIONS AND GAPS IN THE DOCUMENTS
//
// Neither document gives a test vector for anything above the sync words, and
// neither states the CRC register preset, reflection or final XOR: 490 7.2
// gives only the polynomials. This file assumes preset zero, no reflection, no
// final XOR, message bits most significant first, check bits appended most
// significant first. That assumption is untested against a real transmitter
// and is the first thing to check against a capture: a CRC that never passes
// on air with clean Hamming words is this.
//
// 490 clause 9.1 says FN 11 carries the lower 12 bits of the OWN ID; Table
// 5.1d, a copy of 5.1b's layout labelled "FRAME 2", says called ID. 9.1 and
// 658 Table 5.8 agree with each other, and this file follows them.
//
// 490 7.5 says the receive de-interleaver of the 12 x 10 matrix outputs
// [1,11,22,33,...]. The matrix of Table 7.3 gives [1,11,21,31,...]; the
// printed sequence does not follow from any 12 x 10 matrix, so the table is
// what is implemented.
//
// 490 Table 6.1 maps the 64 channel codes to RF channels. That mapping is not
// carried here: the 12-bit code is reported, which identifies the channel to
// anyone holding the table.
//
// CLEAN ROOM
//
// Written from the two ETSI documents only. No dPMR implementation was read.
//
// NOTHING HERE IS BIT EXACT AGAINST A GPU TWIN
//
// Host code with no kernel behind it. The round trip in
// tests/decode/test_dpmr.cpp against core/dsp/synth/dpmr_mod.h stands in its
// place.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/decode/dv_phy.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::SampleIndex;

// ---------------------------------------------------------------------------
// Physical layer
// ---------------------------------------------------------------------------

// 490 12.2.1: 2400 symbols per second, two bits each.
inline constexpr double kDpmrSymbolRate = 2400.0;

// 490 12.2.2.1: deviation index 0.29, +3 at 1050 Hz and +1 at 350 Hz.
inline constexpr double kDpmrDeviationPerUnitHz = 350.0;

// 490 12.2.2.2: root raised cosine, alpha 0.2.
inline constexpr double kDpmrRrcRollOff = 0.2;

// NOT A SPECIFIED VALUE. The channel filter ahead of the discriminator: flat
// to 3 kHz, gone by 4.5 kHz. 490 12.2.2.2 puts the 4FSK filter after the
// frequency demodulator and says nothing of the IF, but a discriminator fed
// the whole of a 48 kS/s tap sits below its FM threshold long before the
// symbols do: 12 dB in 4 kHz is 1.2 dB across 48 kHz, which with no channel
// filter decoded nothing on 2026-11-02. Carson's rule for 1050 Hz peak
// deviation and 1440 Hz of RRC bandwidth is about 2.5 kHz either side.
inline constexpr double kDpmrChannelPassHz = 3000.0;
inline constexpr double kDpmrChannelStopHz = 4500.0;
inline constexpr std::size_t kDpmrChannelSpanSymbols = 4;

// NOT A SPECIFIED VALUE. 490 gives the filter's response, not a length. Eight
// symbols either way of the peak keeps the truncation error of an alpha 0.2
// pulse, whose tails decay slowly, below what the slicer can see.
inline constexpr std::size_t kDpmrRrcSpanSymbols = 16;

// 490 Figure 12b: dibit 01 is +3, 00 is +1, 10 is -1, 11 is -3. Indexed by
// dibit value, the first transmitted bit most significant (490 9.0).
inline constexpr std::array<int, 4> kDpmrDibitToSymbol = {+1, +3, -1, -3};

// 490 4.2.2: an 80 ms payload frame is 384 bits, 192 symbols: 24 bits of FS2
// or channel code, 72 of CCH, then four 72-bit TCH blocks.
inline constexpr std::size_t kDpmrFrameSymbols = 192;
inline constexpr std::size_t kDpmrSyncSymbols = 12;
inline constexpr std::size_t kDpmrLongSyncSymbols = 24;
inline constexpr std::size_t kDpmrCchBits = 72;
inline constexpr std::size_t kDpmrTchBits = 72;
inline constexpr std::size_t kDpmrTchBlocks = 4;

// 490 4.2.2 and Table 5.2: the header is a preamble of at least 72 bits, FS1
// (48), HI0 (120), the channel code (24) and HI1 (120).
inline constexpr std::size_t kDpmrHiBits = 120;
inline constexpr std::size_t kDpmrPreambleSymbols = 36;
inline constexpr std::size_t kDpmrHeaderSymbols = 24 + 60 + 12 + 60;  // from FS1 on

// 490 4.2.2 and 9.6: the End frame is FS3 (24 bits) and two 36-bit copies.
inline constexpr std::size_t kDpmrEndSymbols = 12 + 36;

// 490 6.1.1 to 6.1.4.
inline constexpr std::uint64_t kDpmrFs1 = 0x57FF5F75D577ULL;
inline constexpr std::uint32_t kDpmrFs2 = 0x5FF77DU;
inline constexpr std::uint32_t kDpmrFs3 = 0x7DDFF5U;
inline constexpr std::uint64_t kDpmrFs4 = 0xFD55F5DF7FDDULL;

// 490 6.1.6: the preamble repeats 0x5F.
inline constexpr std::uint8_t kDpmrPreambleOctet = 0x5F;

// 490 7.2.
inline constexpr std::uint8_t kDpmrCrc7Polynomial = 0x09;  // x^7 + x^3 + 1
inline constexpr std::uint8_t kDpmrCrc8Polynomial = 0x07;  // x^8 + x^2 + x + 1

// 490 7.4: x^9 + x^5 + 1, preset all ones, restarted at each block.
inline constexpr std::uint16_t kDpmrScramblerPreset = 0x1FF;

// 490 Table 7.1: the check nibble C3..C0 each data bit contributes, X7 first.
inline constexpr std::array<std::uint8_t, 8> kDpmrHammingParity = {
    0b1110, 0b0111, 0b1010, 0b0101, 0b1011, 0b1100, 0b0110, 0b0011,
};

// ---------------------------------------------------------------------------
// The codes, exposed so the transmitter and the tests share them
// ---------------------------------------------------------------------------

// 490 7.3: eight data bits to the 12-bit word X7..X0 C3..C0, X7 most
// significant.
[[nodiscard]] std::uint16_t dpmr_hamming_encode(std::uint8_t data);

struct DpmrHammingDecode {
    std::uint8_t data = 0;
    bool corrected = false;
    // A syndrome no single error produces: two or more errors.
    bool failed = false;
};

[[nodiscard]] DpmrHammingDecode dpmr_hamming_decode(std::uint16_t word);

// 490 7.2, over one bit per byte. See DEVIATIONS for the preset.
[[nodiscard]] std::uint8_t dpmr_crc7(std::span<const std::uint8_t> bits);
[[nodiscard]] std::uint8_t dpmr_crc8(std::span<const std::uint8_t> bits);

// 490 7.4 and Figure 3, in place, one bit per byte, from the preset. Self
// inverse.
void dpmr_scramble(std::span<std::uint8_t> bits);

// 490 7.5: written down columns of 12, read across rows. `columns` is 6 for
// the CCH (Table 7.2) and 10 for HI (Table 7.3); the input is 12 * columns.
[[nodiscard]] std::vector<std::uint8_t> dpmr_interleave(std::span<const std::uint8_t> bits,
                                                        std::size_t columns);
[[nodiscard]] std::vector<std::uint8_t> dpmr_deinterleave(std::span<const std::uint8_t> bits,
                                                          std::size_t columns);

// 490 6.1.5 and 7.1: the 12-bit channel code to its 24 transmitted bits,
// 0 as 01 and 1 as 11, and back. Back fails for a dibit that is neither.
[[nodiscard]] std::uint32_t dpmr_channel_code_bits(std::uint16_t code);
[[nodiscard]] std::optional<std::uint16_t> dpmr_channel_code_value(std::uint32_t bits);

// The symbols of a word of `bits` bits, most significant dibit first.
[[nodiscard]] std::vector<float> dpmr_word_symbols(std::uint64_t word, std::size_t bits);

// 490 5.1 and 9.1, with 658 Tables 5.5 to 5.8's naming of the reserved bits.
struct DpmrCch {
    std::uint8_t frame_number = 0;  // 5.6, 0 to 3
    std::uint16_t id_part = 0;      // 12 bits of an ID, per 9.1
    std::uint8_t mode = 0;          // 5.7, 3 bits
    std::uint8_t version = 0;       // 5.16, 2 bits
    std::uint8_t format = 0;        // 5.8, 2 bits
    std::uint8_t reserved = 0;      // 2 bits; in 658 the first is Emergency Priority
    std::uint32_t slow_data = 0;    // 5.9, 18 bits
    bool crc_valid = false;
    std::uint32_t corrected_words = 0;
};

// 490 5.2, 5.10, 5.11 and 9.5.
struct DpmrHeaderInfo {
    std::uint8_t header_type = 0;  // 5.11, 4 bits
    std::uint32_t called_id = 0;   // 24 bits
    std::uint32_t own_id = 0;      // 24 bits
    std::uint8_t mode = 0;
    std::uint8_t version = 0;
    std::uint8_t format = 0;
    std::uint8_t reserved = 0;
    std::uint16_t call_info = 0;  // 5.10, 11 bits: 3 type then 8 information
    bool crc_valid = false;
    std::uint32_t corrected_words = 0;
};

// 490 5.3, 5.12 to 5.15 and 9.6.
struct DpmrEnd {
    std::uint8_t end_type = 0;  // 2 bits
    std::uint8_t arq = 0;       // 2 bits
    std::uint8_t wait = 0;      // 4 bits
    std::uint8_t status = 0;    // 5 bits
    std::uint8_t reserved = 0;  // 4 bits
    bool crc_valid = false;
    std::uint32_t corrected_words = 0;
};

// Forward chains: fields to the transmitted (scrambled) bits.
[[nodiscard]] std::vector<std::uint8_t> dpmr_encode_cch(const DpmrCch& cch);
[[nodiscard]] std::vector<std::uint8_t> dpmr_encode_hi(const DpmrHeaderInfo& hi);
[[nodiscard]] std::vector<std::uint8_t> dpmr_encode_end(const DpmrEnd& end);

// And back, from the transmitted bits.
[[nodiscard]] DpmrCch dpmr_decode_cch(std::span<const std::uint8_t> bits);
[[nodiscard]] DpmrHeaderInfo dpmr_decode_hi(std::span<const std::uint8_t> bits);
[[nodiscard]] DpmrEnd dpmr_decode_end(std::span<const std::uint8_t> bits);

// 490 8.1.1: an Initial Services address is the 8-bit common ID followed by
// sixteen ones; 255 is All Call.
[[nodiscard]] constexpr std::uint32_t dpmr_isf_address(std::uint8_t common_id) {
    return (static_cast<std::uint32_t>(common_id) << 16U) | 0xFFFFU;
}

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

enum class DpmrFrameKind {
    Header,        // FS1
    PacketHeader,  // FS4
    Payload,       // FS2 or channel code
    End,           // FS3
};

struct DpmrFrame {
    DpmrFrameKind kind = DpmrFrameKind::Payload;

    // Input sample index of the first symbol of the frame's sync field.
    SampleIndex first_sample = 0;
    double sync_score = 0.0;
    bool inverted = false;

    // Header and PacketHeader: the copy of HI whose CRC passed, or HI0.
    std::optional<DpmrHeaderInfo> header;

    // Header, and payload frames 2 and 4: the 12-bit code of 490 6.1.5.
    std::optional<std::uint16_t> channel_code;

    // Payload.
    std::optional<DpmrCch> cch;
    std::array<std::array<std::uint8_t, kDpmrTchBits / 8>, kDpmrTchBlocks> tch{};

    // 490 5.7: modes 000 and 001 carry vocoder data in the TCH. Taken from
    // the CCH when its CRC passed, otherwise from the transmission's header.
    bool voice = false;

    // Payload frame 4 when all four CCHs of its superframe passed their CRC.
    std::optional<std::uint32_t> called_id;
    std::optional<std::uint32_t> own_id;

    std::optional<DpmrEnd> end;
};

struct DpmrConfig {
    SampleRate rate = 48000;

    // NOT A SPECIFIED VALUE. FS1 and FS4 are 24 outer symbols, so 0.8 admits
    // two wrong symbols. A header is then accepted only when an HI copy
    // passes its CRC8, which is also what picks the polarity, since FS4 is
    // the symbol-wise complement of FS1 (658 6.1 note) and an inverted
    // discriminator cannot otherwise tell them apart.
    double header_threshold = 0.8;

    // NOT A SPECIFIED VALUE. For joining a transmission late on an FS2: 12
    // outer symbols, its CCH's CRC7, and the FS2 two frames on with a CCH
    // that passes and counts two frames on. With the second frame left out,
    // 300 s of noise started three transmissions on 2026-11-02.
    double late_entry_threshold = 0.85;

    // NOT A SPECIFIED VALUE. Where a transmission in hand says a sync or
    // channel code is due, the frame is decoded whatever the score; a frame
    // under this score whose CCH fails its CRC counts as missed, and two in a
    // row end the transmission.
    double tracking_threshold = 0.5;
};

class Dpmr {
   public:
    [[nodiscard]] static Expected<Dpmr> create(const DpmrConfig& config);

    // Consumes complex baseband and appends every frame completed by it.
    [[nodiscard]] Status process(ConstComplexSpan samples, std::vector<DpmrFrame>& out);

    // Every recovered symbol from the last call, scaled so the levels sit
    // near +/-1 and +/-3 before per-frame calibration.
    [[nodiscard]] std::span<const float> last_symbols() const { return last_symbols_; }

    void reset();

   private:
    Dpmr() = default;

    void search(std::vector<DpmrFrame>& out);
    [[nodiscard]] bool try_header(std::size_t offset, double score, std::vector<DpmrFrame>& out);
    [[nodiscard]] bool try_late_entry(std::size_t offset, double score,
                                      std::vector<DpmrFrame>& out);
    // Decodes the payload frame at `offset`; returns whether its CCH passed.
    bool payload_frame(std::size_t offset, std::span<const float> sync, double score,
                       DpmrFrame& frame);
    [[nodiscard]] std::vector<std::uint8_t> slice(std::size_t offset, std::size_t symbols,
                                                  std::span<const float> known,
                                                  std::size_t known_offset, float sign) const;

    DpmrConfig config_{};
    ComplexFir channel_{};
    FmDiscriminator discriminator_{};
    RealFir filter_{};
    SymbolSync sync_{};
    double scale_ = 1.0;
    double delay_ = 0.0;

    std::vector<float> symbols_;
    std::vector<SampleIndex> positions_;
    std::size_t cursor_ = 0;

    bool in_transmission_ = false;
    bool inverted_ = false;
    std::size_t next_frame_ = 0;  // frame number expected next, 0 to 3
    std::size_t missed_ = 0;
    std::optional<std::uint16_t> channel_code_;
    bool header_voice_ = false;
    std::array<std::optional<std::uint16_t>, 4> id_parts_{};

    std::vector<float> last_symbols_;
    std::vector<RecoveredSymbol> recovered_;
    std::vector<float> scratch_;
    std::vector<Complex32> channelled_;
    std::vector<Complex32> shaped_;
};

}  // namespace revenant::decode
