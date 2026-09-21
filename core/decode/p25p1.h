// P25 Phase 1 FDMA: complex baseband to frames, and the frame metadata the
// standard carries in clear.
//
// SPECIFICATION
//
// Implements ANSI/TIA-102.BAAA-A, "Project 25 FDMA Common Air Interface", the
// parts of it that are physical layer and framing: clause 8 (channel access,
// frame synchronisation, status symbols and the Network Identifier) and
// clause 9 (modulation), plus the clause 10.2 transmit-order annex for the
// Header Data Unit. Reserved field values are ANSI/TIA-102.BAAC, "Common Air
// Interface Reserved Values".
//
// Both documents were read from the public archive at
// archive.org/details/TIA-102_Series_Documents, items
// "TIA-102-BAAA-A_Project_25_FDMA_CAI.pdf" and
// "TIA-102.BAAC-A_CAI_Reserved_Values.pdf", which is the same collection
// core/decode/imbe.h names for TIA-102.BABA. Every clause number below is the
// document's own.
//
// WHERE THIS STOPS
//
// At the bits, and deliberately. The voice payload is IMBE and is
// core/decode/imbe.cpp's job; this file recovers the channel bits and the
// header, and hands over. It does not decrypt: where the header says a call
// is encrypted this reports that and stops, per docs/modes.md.
//
// It also stops short of the Link Control word in LDU1 and the encryption
// sync in LDU2. Both are reachable and neither is here; see the note on
// P25Frame::header for what that costs and why.
//
// CLEAN ROOM
//
// No P25 implementation was read. Every constant names the clause, table or
// annex it came from. Where a number is an engineering choice rather than a
// specified value, the comment says so.
//
// NOTHING HERE IS BIT EXACT AGAINST A GPU TWIN
//
// Host code with no kernel behind it, the same as the RDS physical layer, so
// the project's zero-ULP rule does not reach it. What stands in its place is
// the transmitter in core/dsp/synth/dv_mod.h, written from the same clauses,
// and the round trip in tests/decode/test_p25p1.cpp.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
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

// TIA-102.BAAA-A clause 9.2: "The modulation sends 4800 symbols/sec with each
// symbol conveying 2 bits of information."
inline constexpr double kP25SymbolRate = 4800.0;
inline constexpr double kP25BitRate = 9600.0;

// Table 9-1, the C4FM deviation column. The symbol column runs +3, +1, -1, -3
// against dibits 01, 00, 10, 11, and the deviations are +1.80, +0.60, -0.60
// and -1.80 kHz, so one symbol unit is 600 Hz of deviation.
//
// Indexed by the dibit as an integer, so kP25DibitToSymbol[0b01] is +3.
inline constexpr double kP25DeviationPerSymbolUnitHz = 600.0;
inline constexpr int kP25DibitToSymbol[4] = {+1, +3, -1, -3};

// The inverse of the above: the dibit a symbol level represents, indexed by
// (level + 3) / 2, so index 0 is level -3.
inline constexpr std::uint8_t kP25SymbolToDibit[4] = {0b11, 0b10, 0b00, 0b01};

// Clause 9.3, the Nyquist raised cosine filter, and clause 9.4, the shaping
// filter cascaded with it. Both are flat in group delay below 2880 Hz and the
// raised cosine is zero above it.
inline constexpr double kP25NyquistFlatEdgeHz = 1920.0;
inline constexpr double kP25NyquistStopHz = 2880.0;

// Table 8-1. The frame sync word, 48 bits, transmitted left to right.
inline constexpr std::uint64_t kP25FrameSync = 0x5575F5FF77FFULL;
inline constexpr std::size_t kP25FrameSyncBits = 48;
inline constexpr std::size_t kP25FrameSyncSymbols = kP25FrameSyncBits / 2;

// Clause 8.5. The Network Identifier is 64 bits: 12 of Network Access Code,
// 4 of Data Unit ID, 47 of BCH parity and one more parity bit.
inline constexpr std::size_t kP25NidBits = 64;
inline constexpr std::size_t kP25NidSymbols = kP25NidBits / 2;

// Clause 8.4 and the clause 10.2 annex. A status symbol is inserted after
// every 35 information symbols, which the annex shows as absolute symbol
// positions 35, 71, 107 and so on: one every 36th, counting the status
// symbols themselves, with the first at index 35.
inline constexpr std::size_t kP25StatusSymbolInterval = 36;
inline constexpr std::size_t kP25FirstStatusSymbol = 35;

// Table 8-4, the six Data Unit Identifier values this document defines. The
// other ten are reserved for trunking.
enum class P25Duid : std::uint8_t {
    HeaderDataUnit = 0x0,
    TerminatorWithoutLinkControl = 0x3,
    LogicalLinkDataUnit1 = 0x5,
    LogicalLinkDataUnit2 = 0xA,
    PacketDataUnit = 0xC,
    TerminatorWithLinkControl = 0xF,
};

[[nodiscard]] std::string_view p25_duid_name(std::uint8_t duid);

// The clause 10.2 annex, read end to end: the Header Data Unit is 396 symbols
// including 11 status symbols and 5 null symbols, so 385 carry information.
// Those 385 are 24 of frame sync, 32 of Network Identifier, 324 of Golay
// coded header, and the 5 nulls.
inline constexpr std::size_t kP25HduTotalSymbols = 396;
inline constexpr std::size_t kP25HduStatusSymbols = 11;
inline constexpr std::size_t kP25HduNullSymbols = 5;
inline constexpr std::size_t kP25HduGolayWords = 36;
inline constexpr std::size_t kP25HduSymbolsPerGolayWord = 9;

// The other three annexes, read the same way. Clause 10.5's simple terminator
// runs to symbol 71 and clause 10.6's terminator with Link Control to 215;
// clauses 10.3 and 10.4 both run to 863, which is 180 ms at 4800 symbols per
// second, the figure clause 10.3 states in prose.
inline constexpr std::size_t kP25SimpleTerminatorSymbols = 72;
inline constexpr std::size_t kP25TerminatorWithLcSymbols = 216;
inline constexpr std::size_t kP25LduSymbols = 864;

// The simple terminator is the frame sync, the Network Identifier, two status
// symbols and nulls for the rest.
inline constexpr std::size_t kP25SimpleTerminatorNullSymbols =
    kP25SimpleTerminatorSymbols - kP25FrameSyncSymbols - kP25NidSymbols - 2;

// Total symbols in a data unit of this type, including its status symbols, or
// zero where this document does not define the type.
//
// Used to step past a decoded data unit rather than past its sync word alone,
// so a sync pattern appearing inside a payload cannot start a second
// overlapping frame.
[[nodiscard]] std::size_t p25_data_unit_symbols(std::uint8_t duid);

// TIA-102.BAAC clause 2.8: "The ALGID has a standard value for unencrypted
// messages, which is $80."
inline constexpr std::uint8_t kP25AlgidUnencrypted = 0x80;

// ---------------------------------------------------------------------------
// What comes out
// ---------------------------------------------------------------------------

struct P25Nid {
    std::uint16_t network_access_code = 0;
    std::uint8_t duid = 0;

    // Bits the BCH decode had to change. The code's minimum distance is 23, so
    // up to 11 is a correction it guarantees; above that the answer is the
    // nearest code word rather than the transmitted one. Carried out so a
    // caller can decide what to trust instead of being told.
    std::uint32_t corrected_bits = 0;
};

// The Header Data Unit's contents, clause 10.2 read through the Golay code.
struct P25Header {
    // Clause 10.2 and TIA-102.BAAC clause 2.6. 72 bits, most significant
    // first, the initialisation vector for encryption. All zero means the call
    // is unencrypted, and the null value is never used for an encrypted one.
    std::array<std::uint8_t, 9> message_indicator{};

    std::uint8_t manufacturer_id = 0;
    std::uint8_t algorithm_id = 0;
    std::uint16_t key_id = 0;
    std::uint16_t talkgroup_id = 0;

    // TIA-102.BAAC clause 2.8. False only when the algorithm identifier holds
    // the standard unencrypted value.
    //
    // This is the status surface docs/modes.md requires: an encrypted call is
    // identified, its talkgroup and network are reported, and nothing tries to
    // turn its payload into audio.
    bool encrypted = false;

    // The worst Golay correction across the 36 code words, and the number of
    // words that needed any. A header whose worst word took three corrections
    // is at the edge of what a distance-8 code can do.
    std::uint32_t worst_golay_correction = 0;
    std::uint32_t golay_words_corrected = 0;
};

struct P25Frame {
    P25Nid nid;

    // Present only for a Header Data Unit. Link Control in LDU1 and encryption
    // sync in LDU2 carry the source and destination identifiers and a second
    // copy of the talkgroup, and neither is decoded here: both sit under a
    // Reed-Solomon code over GF(2^6) and an interleave that this lane did not
    // reach. The consequence is that a receiver joining a call mid-transmission
    // sees the DUID sequence and the NAC but not the talkgroup until the next
    // header, which for voice is up to the length of the transmission.
    std::optional<P25Header> header;

    // Index into the symbol run at which this frame's sync word starts.
    std::size_t first_symbol = 0;

    // Normalised correlation the sync word scored, and whether it matched
    // inverted. An inverted match means the discriminator polarity is
    // reversed, which depends on which sideband the tuner landed on and is
    // corrected here rather than reported as a failure.
    double sync_score = 0.0;
    bool inverted = false;

    // Every information dibit of the data unit after the status symbols are
    // removed, including the sync word and the NID, one dibit per byte in the
    // low two bits. Handed out so a caller can take the payload somewhere this
    // file does not go, which is how the IMBE decoder will be fed.
    std::vector<std::uint8_t> dibits;
};

struct P25Config {
    SampleRate rate = 48000;

    // Correlation a sync word must reach to be called a frame. An engineering
    // choice, not a specified value: 0.9 admits a sync word with two of its
    // 24 symbols wrong, which is about where a C4FM link stops producing
    // usable voice anyway, and rejects the correlation sidelobes of the sync
    // pattern against itself, whose largest is well below it.
    double sync_threshold = 0.9;

    // Taps in the receive filter. Odd, so the group delay is a whole number of
    // samples. 121 at 48 kHz spans 2.5 ms, which is twelve symbol periods.
    //
    // Longer than the transmitter's 65 because this filter does have a corner
    // to resolve. The clause 9.6 response is a sinc that is still at half
    // amplitude when it reaches the edge of the band the clause specifies, so
    // the out-of-band taper starts from a step rather than from zero, and a
    // 65 tap design smears it back into the passband. Measured: 65 taps put
    // the recovered constellation's inner and outer levels at 1.2 and 2.8
    // instead of 1 and 3.
    std::size_t filter_taps = 121;
};

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

class P25Phase1 {
   public:
    [[nodiscard]] static Expected<P25Phase1> create(const P25Config& config);

    // Consumes complex baseband centred on the carrier and appends every frame
    // whose sync word and NID were recovered from it. State carries across
    // calls, so a caller may feed the stream in any blocking.
    //
    // A frame straddling a block boundary is held until the rest of it
    // arrives, so no frame is lost and none is reported twice.
    [[nodiscard]] Status process(ConstComplexSpan samples, std::vector<P25Frame>& out);

    // The soft symbol values the last call recovered, in units where a
    // Table 9-1 symbol is +3, +1, -1 or -3. Exposed for the tests and for a
    // caller measuring modulation quality; not needed to decode.
    [[nodiscard]] std::span<const float> last_symbols() const { return last_symbols_; }

    void reset();

   private:
    P25Phase1() = default;

    [[nodiscard]] Status decode_at(std::size_t offset, bool inverted, double score,
                                   P25Frame& out) const;

    P25Config config_{};
    std::vector<float> filter_taps_;
    SymbolSync sync_{};

    // Soft symbols carried across calls, with everything before the last
    // possible frame start trimmed off.
    std::vector<float> symbols_;
    std::vector<float> last_symbols_;
    std::size_t consumed_ = 0;

    // Scratch, kept so the hot path does not allocate per block.
    std::vector<RecoveredSymbol> recovered_;
    std::vector<float> discriminated_;
    std::vector<float> filtered_;
};

// The frame sync word as soft symbol values, for a correlator. Table 8-1 read
// through Table 9-1.
[[nodiscard]] std::array<float, kP25FrameSyncSymbols> p25_frame_sync_pattern();

// Removes the status symbols from a run of symbols that starts at a data
// unit's first sync symbol, per clause 8.4.
[[nodiscard]] std::vector<float> p25_strip_status_symbols(std::span<const float> symbols);

}  // namespace revenant::decode
