// TETRA V+D downlink: complex baseband to synchronisation bursts and the
// network identity they broadcast in clear.
//
// SPECIFICATION
//
// Implements ETSI EN 300 392-2 V3.8.1 (2016-08), "Terrestrial Trunked Radio
// (TETRA); Voice plus Data (V+D); Part 2: Air Interface (AI)", the physical
// layer and the synchronisation path: clause 5 (modulation), clause 8.2
// (channel coding, interleaving and scrambling), clause 8.3.1.2 (the
// broadcast synchronisation channel), clause 9.4.4 (burst structure and the
// training sequences) and clause 21.4.4.2 (the SYNC PDU), with the MLE part
// of that PDU from clause 18.4.2.1.
//
// The document is a free download from etsi.org and is the version named
// above. Every clause number below is its own.
//
// WHY THE DOWNLINK AND WHY THE SYNCHRONISATION BURST
//
// Because that is where TETRA puts the information that travels in clear. The
// broadcast synchronisation channel is scrambled with an all-zero extended
// colour code by clause 8.2.5.2, deliberately, so that a receiver which does
// not yet know the cell can read it. Everything it carries, the colour code,
// the mobile country and network codes, the frame and multiframe numbers, is
// therefore recoverable without any key and without any of the security
// specification. That is the whole of what this file does.
//
// WHAT THIS DOES NOT DO
//
// It does not decrypt. TEA1 through TEA4 are restricted-distribution
// algorithms, docs/modes.md says they are not to be sought or implemented,
// and nothing here reaches for them. An encrypted TETRA call still frames,
// and the framing is the useful part.
//
// It also does not decode traffic or the signalling channels above BSCH.
// Those are scrambled with the cell's actual colour code, which this file
// recovers, so they are reachable; they are simply not here.
//
// CLEAN ROOM
//
// No TETRA implementation was read. Every constant names its clause.
//
// NOTHING HERE IS BIT EXACT AGAINST A GPU TWIN
//
// Host code with no kernel behind it, the same as the RDS physical layer. The
// round trip in tests/decode/test_tetra.cpp against core/dsp/synth/dv_mod.h
// is what stands in place of the project's zero-ULP rule.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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

// Clause 5.3: "The modulation rate shall be 36 kbit/s for pi/4-DQPSK". Two
// bits per symbol, so 18000 symbols per second.
inline constexpr double kTetraBitRate = 36000.0;
inline constexpr double kTetraSymbolRate = 18000.0;

// Clause 5.5, equation 5.4: the symbol waveform is the inverse transform of a
// square root raised cosine spectrum whose roll-off "shall be 0,35".
inline constexpr double kTetraRollOff = 0.35;

// Clause 9.4.4.2.6, table 9.9: the synchronisation continuous downlink burst
// is 510 bits, which at two bits per symbol is 255 symbols. That is the
// timeslot, and clause 9 puts four of them in a frame of 56.67 ms.
inline constexpr std::size_t kTetraBurstBits = 510;
inline constexpr std::size_t kTetraBurstSymbols = kTetraBurstBits / 2;

// Clause 9.4.4.3.4, equation 9.11. The 38 bit synchronisation training
// sequence, which is what distinguishes a synchronisation downlink burst from
// every other burst on the carrier.
inline constexpr std::array<std::uint8_t, 38> kTetraSyncTrainingSequence = {
    1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0,
    1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1,
};

// Clause 9.4.4.3.2, equations 9.4 through 9.6. Three 22 bit normal training
// sequences for pi/4-DQPSK. The third is spread over two consecutive downlink
// bursts and is what table 9.9 puts at each end of the synchronisation burst,
// q11 through q22 at the head and q1 through q10 at the tail.
inline constexpr std::array<std::uint8_t, 22> kTetraNormalTrainingSequence1 = {
    1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0, 1, 0, 0,
};
inline constexpr std::array<std::uint8_t, 22> kTetraNormalTrainingSequence2 = {
    0, 1, 1, 1, 1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0,
};
inline constexpr std::array<std::uint8_t, 22> kTetraNormalTrainingSequence3 = {
    1, 0, 1, 1, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1,
};

// Clause 9.4.4.3.5, equation 9.12.
inline constexpr std::array<std::uint8_t, 4> kTetraTailBits = {1, 1, 0, 0};

// Table 9.9, as bit numbers counted from one. Held as zero-based offsets and
// lengths, which is how the code below indexes them.
struct TetraBurstField {
    std::size_t offset = 0;
    std::size_t length = 0;
};

inline constexpr TetraBurstField kTetraSyncBurstTrainingHead{0, 12};
inline constexpr TetraBurstField kTetraSyncBurstPhaseHead{12, 2};
inline constexpr TetraBurstField kTetraSyncBurstFrequencyCorrection{14, 80};
inline constexpr TetraBurstField kTetraSyncBurstBlock1{94, 120};
inline constexpr TetraBurstField kTetraSyncBurstTraining{214, 38};
inline constexpr TetraBurstField kTetraSyncBurstBroadcast{252, 30};
inline constexpr TetraBurstField kTetraSyncBurstBlock2{282, 216};
inline constexpr TetraBurstField kTetraSyncBurstPhaseTail{498, 2};
inline constexpr TetraBurstField kTetraSyncBurstTrainingTail{500, 10};

// Clause 8.3.1.2, the BSCH chain: 60 type-1 bits, a (76,60) block code, four
// zero tail bits to 80 type-2, a 16-state RCPC of rate 2/3 to 120 type-3, a
// (120,11) block interleave to 120 type-4, and scrambling to the 120 bits of
// the synchronisation block.
inline constexpr std::size_t kTetraBschInformationBits = 60;
inline constexpr std::size_t kTetraBschType2Bits = 80;
inline constexpr std::size_t kTetraBschType3Bits = 120;
inline constexpr std::size_t kTetraBschInterleaveStep = 11;

// ---------------------------------------------------------------------------
// What comes out
// ---------------------------------------------------------------------------

// Clause 21.4.4.2, table 21.76, with the 29 bit TM-SDU parsed per clause
// 18.4.2.1, table 18.17.
struct TetraSyncPdu {
    // Table 21.76. The system code names which edition of the standard and of
    // EN 300 392-7 the cell conforms to.
    std::uint8_t system_code = 0;

    // Six bits. Note 1 of table 21.76: the value meaning "predefined
    // scrambling" is the one for which all thirty bits of the scrambling
    // vector are zero.
    std::uint8_t colour_code = 0;

    // Two bits, 00 through 11 for timeslots 1 through 4.
    std::uint8_t timeslot = 0;

    // Five bits, with 00000 reserved and 10010 frame 18.
    std::uint8_t frame_number = 0;

    // Six bits, with 000000 reserved and 111100 multiframe 60.
    std::uint8_t multiframe_number = 0;

    // Two bits: continuous transmission, carrier sharing, MCCH sharing or
    // traffic carrier sharing.
    std::uint8_t sharing_mode = 0;

    std::uint8_t reserved_frames = 0;
    bool uplane_dtx_allowed = false;
    bool frame18_extension = false;

    // Table 18.17, the D-MLE-SYNC PDU carried as the 29 bit TM-SDU.
    std::uint16_t mobile_country_code = 0;  // 10 bits
    std::uint16_t mobile_network_code = 0;  // 14 bits
    std::uint8_t neighbour_cell_broadcast = 0;
    std::uint8_t cell_load = 0;
    bool late_entry_supported = false;
};

struct TetraBurst {
    // The differential symbol at which the burst's first bit pair sits,
    // counted from the first one the stream produced since create() or
    // reset(), however many process() calls it took to get there.
    //
    // WHAT THIS USED TO SAY, until 2026-09-23: "Index into the recovered
    // symbol run at which the burst's first symbol sits." The run was the
    // decoder's own buffer, trimmed as the stream went by, so the same burst
    // had a different number under a different blocking.
    std::size_t first_symbol = 0;

    double sync_score = 0.0;

    // Present when the broadcast synchronisation channel decoded and its
    // clause 8.2.3.3 block code verified. Absent means the burst was found by
    // its training sequence but its BSCH did not check out, which is worth
    // reporting: it is a TETRA carrier either way.
    std::optional<TetraSyncPdu> sync;

    // True when the block code verified. Carried separately from the optional
    // so a caller can tell "no BSCH" from "BSCH failed".
    bool block_code_verified = false;

    // The 510 demodulated bits of the burst, in transmission order.
    std::array<std::uint8_t, kTetraBurstBits> bits{};
};

struct TetraConfig {
    SampleRate rate = 72000;

    // Correlation the synchronisation training sequence must reach. An
    // engineering choice. The sequence is 38 bits, so 0.75 admits about four
    // wrong bits, which is roughly where the rate 2/3 RCPC code below it stops
    // recovering the PDU anyway.
    double sync_threshold = 0.75;

    // Taps in the root raised cosine matched filter. Odd, so the group delay
    // is whole. 65 at 72 kHz spans nine milliseconds, which is about sixteen
    // symbol periods and puts the truncated tails of an alpha 0.35 pulse well
    // below the noise.
    std::size_t filter_taps = 65;
};

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

class Tetra {
   public:
    [[nodiscard]] static Expected<Tetra> create(const TetraConfig& config);

    [[nodiscard]] Status process(ConstComplexSpan samples, std::vector<TetraBurst>& out);

    void reset();

   private:
    Tetra() = default;

    TetraConfig config_{};
    ComplexFir filter_{};
    SymbolSync sync_{};

    // Soft bits, two per recovered symbol, from the clause 5.4 differential
    // phase. The sign convention is core/decode/dv_codes.h's: positive leans
    // towards zero. `trimmed_` counts the bits dropped off the front, so a
    // burst's position can be given from the start of the stream.
    std::vector<float> soft_bits_;
    std::size_t consumed_ = 0;
    std::size_t trimmed_ = 0;

    std::vector<RecoveredSymbol> recovered_;
    std::vector<Complex32> filtered_;
    Complex32 previous_symbol_{1.0F, 0.0F};
    bool have_previous_ = false;
};

// ---------------------------------------------------------------------------
// The BSCH codec, exposed so the transmitter and the tests share it
// ---------------------------------------------------------------------------

// The 60 information bits of the SYNC PDU, in the table 21.76 and table 18.17
// field order.
[[nodiscard]] std::array<std::uint8_t, kTetraBschInformationBits> tetra_sync_pdu_bits(
    const TetraSyncPdu& pdu);

[[nodiscard]] TetraSyncPdu tetra_parse_sync_pdu(
    std::span<const std::uint8_t> bits);

// Clause 8.3.1.2 forward: 60 information bits to the 120 bits of the
// synchronisation block, through the block code, the RCPC code, the interleave
// and the all-zero scrambling clause 8.2.5.2 requires for BSCH.
[[nodiscard]] Expected<std::vector<std::uint8_t>> tetra_bsch_encode(
    std::span<const std::uint8_t> information);

struct TetraBschDecode {
    std::array<std::uint8_t, kTetraBschInformationBits> information{};
    bool block_code_verified = false;
};

// The same chain in reverse, from 120 soft bits.
[[nodiscard]] Expected<TetraBschDecode> tetra_bsch_decode(std::span<const float> soft);

// Clause 5.4, table 5.1. The differential phase transition for a bit pair, in
// radians. B(2k-1) is `first` and B(2k) is `second`.
[[nodiscard]] double tetra_phase_transition(std::uint8_t first, std::uint8_t second);

}  // namespace revenant::decode
