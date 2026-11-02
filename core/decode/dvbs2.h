// DVB-S2 physical layer and FEC, for the two normal-frame MODCODs the GOES-R
// Rebroadcast uses: QPSK 9/10 and 8PSK 2/3. The pieces here are what
// core/decode/grb.h and core/dsp/synth/grb_mod.h share.
//
// SPECIFICATION
//
// ETSI EN 302 307-1 V1.4.1 (2014-11), "Digital Video Broadcasting (DVB);
// Second generation framing structure, channel coding and modulation
// systems ... Part 1: DVB-S2", free from etsi.org, fetched 2026-11-02 from
// www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/. Cited as
// "EN 302 307" with a clause, table or figure.
//
//   - 5.1.4 figure 2: CRC-8, g(X) = X^8 + X^7 + X^6 + X^4 + X^2 + 1, register
//     zeroed, here over the first nine BBHEADER octets as 5.1.6 says.
//   - 5.1.6 and table 3: the ten-octet BBHEADER, MATYPE, UPL, DFL, SYNC,
//     SYNCD, CRC-8, transmitted MSB first.
//   - 5.2.2 figure 5: BB scrambling over the whole BBFRAME by 1 + X^14 + X^15
//     loaded with 100101010000000 at each frame. Same generator and load as
//     DVB-S energy dispersal, so core/decode/dvbs.h's DvbsPrbs produces it.
//   - 5.3 tables 5a and 6a: Kbch, Nbch, t and the BCH polynomials g1 to g12.
//     The generator is g1 * ... * gt, the code over GF(2^16) whose field
//     polynomial is g1 (it is the primitive one, and with it alpha^1 to
//     alpha^2t are the generator's roots, which core/decode/bch.h checks).
//   - 5.3.2.1 and table 7a: the LDPC parity accumulator construction, with
//     the addresses of annex B tables B.6 and B.11 (core/decode/
//     dvbs2_tables.cpp). The parity check matrix the decoder runs on is read
//     off the same construction: parity check j sums the information bits
//     accumulated at address j, p_j and p_(j-1).
//   - 5.3.3 table 8 figure 7: the 8PSK bit interleaver, 21 600 rows by 3
//     columns, written down the columns and read along the rows.
//   - 5.4.1 figure 9 and 5.4.2 figure 10: the QPSK and 8PSK Gray mappings.
//     Read off the figures: QPSK labels 00, 10, 11, 01 at pi/4, 3pi/4,
//     5pi/4, 7pi/4; 8PSK labels 001, 000, 100, 110, 010, 011, 111, 101 at
//     0, pi/4, ..., 7pi/4.
//   - 5.5 table 11 and figure 13: slots of 90 symbols, S = 360 (QPSK) or
//     240 (8PSK) per normal XFECFRAME, a 36-symbol pilot block after every
//     16 slots when TYPE says so, except where it would meet the next SOF.
//     5.5.1: a dummy PLFRAME is a PLHEADER and 36 slots.
//   - 5.5.2: the PLHEADER, SOF 0x18D2E82 in 26 symbols and the (64,7) PLS
//     code of 5.5.2.4 (figure 13b's generator, the b7 repetition rule and the
//     64-bit scrambling sequence), sent pi/2-BPSK: symbol 2i-1 (one-based)
//     on (1 + j)(1 - 2y)/sqrt 2 and symbol 2i on (-1 + j)(1 - 2y)/sqrt 2.
//     Table 12 gives MODCOD 11 for QPSK 9/10 and 13 for 8PSK 2/3; 5.5.2.3
//     the TYPE bits, MSB short frame, LSB pilots.
//   - 5.5.4 and figure 15: physical layer scrambling by the n-th Gold code,
//     restarted after each PLHEADER, over data and pilots alike.
//
// Every symbol of a PLFRAME, the pi/2-BPSK header included, lands on one of
// the eight points k * pi/4. That is what lets one coherent 8PSK (or QPSK)
// demodulator run straight through header, data and pilots, and lets the
// transmitter render a whole frame as 8PSK indices.
//
// SCOPE
//
// Normal frames at the two GRB MODCODs. Other MODCODs are recognised in the
// header so that a receiver can skip them at the right length; their codes
// are not here. No ACM, no multiple streams, no 16APSK or 32APSK.
//
// CLEAN ROOM
//
// Written from EN 302 307 alone. No DVB-S2 implementation was read: not
// leandvb, gr-dvbs2rx, gr-dtv, SatDump or any other.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/decode/bch.h"
#include "core/decode/ldpc.h"
#include "core/decode/psk_demod.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// 5.5: slot, header and pilot block lengths in symbols.
inline constexpr std::size_t kDvbs2SlotSymbols = 90;
inline constexpr std::size_t kDvbs2PilotSymbols = 36;
inline constexpr std::size_t kDvbs2SofSymbols = 26;
// 5.5.2.1.
inline constexpr std::uint32_t kDvbs2Sof = 0x18D2E82;
// 5.3, table 5a.
inline constexpr std::size_t kDvbs2NormalFrameBits = 64800;
// 5.1.6.
inline constexpr std::size_t kDvbs2BbHeaderBits = 80;
// Table 12.
inline constexpr std::uint8_t kDvbs2ModcodDummy = 0;
inline constexpr std::uint8_t kDvbs2ModcodQpsk910 = 11;
inline constexpr std::uint8_t kDvbs2Modcod8psk23 = 13;

struct Dvbs2Code {
    std::uint8_t modcod = 0;
    PskModulation modulation = PskModulation::Qpsk;
    std::size_t kbch = 0;  // table 5a
    std::size_t nbch = 0;  // = kldpc
    unsigned t = 0;        // BCH correction
    unsigned q = 0;        // table 7a
};

// The code of a normal-frame MODCOD this file implements, or nothing.
[[nodiscard]] std::optional<Dvbs2Code> dvbs2_code(std::uint8_t modcod);

// Bits per symbol for a MODCOD by its table 12 range (2 to 5), 0 for the
// dummy frame and the reserved values.
[[nodiscard]] unsigned dvbs2_modcod_bits(std::uint8_t modcod);

// Symbols in a PLFRAME, header included, for any MODCOD and TYPE (5.5,
// table 11, figure 13); 0 for reserved MODCODs.
[[nodiscard]] std::size_t dvbs2_plframe_symbols(std::uint8_t modcod, std::uint8_t type);

// The 8PSK grid index, angle k * pi/4, of a label in figure 9 or 10. QPSK
// labels are two bits, 8PSK three, MSB first.
[[nodiscard]] std::uint8_t dvbs2_grid_index(PskModulation modulation, unsigned label);

// The 90 PLHEADER symbols as grid indices (5.5.2).
[[nodiscard]] std::array<std::uint8_t, kDvbs2SlotSymbols> dvbs2_plheader(std::uint8_t modcod,
                                                                         std::uint8_t type);

struct Dvbs2Header {
    std::uint8_t modcod = 0;
    std::uint8_t type = 0;
    // |correlation| / (90 * mean |symbol|): 1 for a clean header.
    float metric = 0.0F;
    // The unit phasor the received header sits at relative to the sent one.
    dsp::Complex32 phase{1.0F, 0.0F};
};

// Correlation of the 26 SOF symbols alone, normalised as Dvbs2Header::metric,
// and its phase.
[[nodiscard]] Dvbs2Header dvbs2_sof(std::span<const dsp::Complex32> symbols);
// The best of all 128 PLS code words over 90 symbols, maximum likelihood by
// correlation.
[[nodiscard]] Dvbs2Header dvbs2_decode_plheader(std::span<const dsp::Complex32> symbols);

// 5.5.4: R_n(i), 0 to 3, for i from 0 to length - 1. The scrambled symbol
// is the symbol times j^R.
[[nodiscard]] std::vector<std::uint8_t> dvbs2_pl_scrambling(std::uint32_t gold_code, std::size_t length);

// 5.1.4: CRC-8 over octets.
[[nodiscard]] std::uint8_t dvbs2_crc8(std::span<const std::uint8_t> octets);

// 5.2.2: XORs the BB scrambling sequence over bits, one per byte, from the
// sequence's start. Its own inverse.
void dvbs2_bb_scramble(std::span<std::uint8_t> bits);

struct Dvbs2BbHeader {
    std::uint8_t matype1 = 0;
    std::uint8_t matype2 = 0;
    std::uint16_t upl = 0;
    std::uint16_t dfl = 0;
    std::uint8_t sync = 0;
    std::uint16_t syncd = 0;
};

// The 80 header bits, CRC-8 computed, one bit per byte.
void dvbs2_write_bbheader(const Dvbs2BbHeader& header, std::span<std::uint8_t> bits);
// Nothing when the CRC-8 disagrees.
[[nodiscard]] std::optional<Dvbs2BbHeader> dvbs2_parse_bbheader(std::span<const std::uint8_t> bits);

// 5.3.3: the 8PSK interleaver on a normal FECFRAME, and its inverse on
// soft values.
void dvbs2_interleave_8psk(std::span<const std::uint8_t> in, std::span<std::uint8_t> out);
void dvbs2_deinterleave_8psk(std::span<const float> in, std::span<float> out);

struct Dvbs2DecodeResult {
    bool ok = false;
    std::size_t ldpc_iterations = 0;
    bool ldpc_converged = false;
    std::size_t bch_corrected = 0;
};

// BCH and LDPC for one MODCOD.
class Dvbs2Fec {
public:
    [[nodiscard]] static Expected<Dvbs2Fec> create(std::uint8_t modcod, std::size_t max_iterations = 50);

    [[nodiscard]] const Dvbs2Code& code() const { return code_; }

    // Kbch BBFRAME bits in, 64 800 FECFRAME bits out, before interleaving.
    void encode(std::span<const std::uint8_t> bbframe, std::span<std::uint8_t> fecframe) const;

    // 64 800 LLRs (positive favours 0), deinterleaved, in; Kbch bits out.
    Dvbs2DecodeResult decode(std::span<const float> llr, std::span<std::uint8_t> bbframe);

private:
    Dvbs2Fec() = default;

    Dvbs2Code code_;
    // Flattened annex B table for this rate.
    std::span<const std::uint16_t> table_;
    std::optional<BchCodec> bch_;
    std::optional<LdpcDecoder> ldpc_;
    std::vector<std::uint8_t> bits_;
};

// Max-log LLRs (positive favours 0) for derotated, descrambled data symbols
// at unit amplitude, `n0` the complex noise variance, per figure 9 or 10.
void dvbs2_demap(PskModulation modulation, std::span<const dsp::Complex32> symbols, float n0,
                 std::span<float> llr);

}  // namespace revenant::decode
