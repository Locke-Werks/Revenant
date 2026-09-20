// RDS and RBDS baseband coding and group decoding.
//
// PROVENANCE. Clean room, per docs/clean-room.md. Everything here was written
// from four published documents and from nothing else. No source code from any
// other RDS implementation was read while writing it.
//
//   EN 50067:1998   The CENELEC RDS specification, read in full. The direct
//                   ancestor of IEC 62106 and freely available, which is why
//                   it is the document cited throughout rather than the IEC
//                   text nobody here has opened. Clause numbering moved on the
//                   way into IEC 62106: EN 50067 clause 3.1.5.1 is IEC 62106
//                   clause 6.1.5.1. A citation below naming an IEC clause
//                   would be a citation to a document this code was not
//                   written from, so there are none.
//   NRSC-4-B        April 2011, United States RBDS Standard, free from
//                   nrscstandards.org. A delta against IEC 62106 Edition 2.0
//                   (2009-07): it replaces some sections outright and is
//                   silent on the rest, and the sections it is silent on are
//                   the ones EN 50067 already covers. Retired May 2021, folded
//                   into the IEC series as IEC 62106-9:2021, which is
//                   paywalled; the archived NRSC-4-B remains the freely
//                   readable text of that content.
//   NRSC-4          April 1998, the last full RBDS text before NRSC went
//                   delta-only. Cited where NRSC-4-B and it disagree.
//   Kopitz & Marks  RDS: The Radio Data System, Artech House 1999. A textbook,
//                   not a standard. Cited only for receiver design practice,
//                   and marked as practice rather than as a requirement every
//                   time.
//
// THE REGION SPLIT. The physical layer is not region-parameterised at all:
// NRSC-4 (1998) clauses 1.1 through 1.7 are word for word EN 50067:1998. The
// CRC, the offset words, sync acquisition and every group parser are common
// text as well. The region reaches exactly four places, and each of them is a
// switch over Region with no default label, so /w14062 turns a third region
// into four build errors rather than four silent fallbacks:
//
//   1. the PTY name table, which shares nothing between the two regions;
//   2. PI interpretation: whether the coverage-area nibble means anything, and
//      whether a call sign can be recovered;
//   3. the LF/MF alternative-frequency code table, 9 kHz against 10 kHz;
//   4. whether offset word E is tolerated on the sync path.
//
// The VHF alternative-frequency table, despite being the one people expect to
// differ, is identical in both regions.
//
// SCOPE. This half of the decoder starts at differentially decoded data bits
// and ends at decoded groups. The 57 kHz subcarrier, the biphase symbol
// decoder, the bit clock and the differential decoder are core/decode/
// rds_bits.h and are deliberately not included here: the seam is one bool per
// bit through RdsDecoder::feed, so neither half can break the other's tests.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// ---------------------------------------------------------------------------
// Region
// ---------------------------------------------------------------------------

// kRds is EN 50067 / IEC 62106, ITU regions 1 and 3. kRbds is NRSC-4-B, ITU
// region 2: the USA, Canada and Mexico.
//
// HOW A CALLER CHOOSES. Explicitly, from configuration, and never by inference
// from the bitstream. Three reasons, in order of weight.
//
// No field names the region. The nearest thing is the Extended Country Code in
// a type 1A variant 0 group, and that is optional to transmit and lives in a
// group type that is about a tenth of traffic under the standard's own
// recommended mix. A decoder that waits for it shows nothing, or shows the
// wrong PTY name, for the first several seconds of every tune.
//
// The PI code cannot carry the decision either. NRSC-4-B section D.7.4 states
// that about 16 percent of stations in the western half of the USA compute to
// a first nibble of 0x01, and that broadcasters carrying TMC may substitute
// 0x01 deliberately, so 0x01 is ambiguous in both directions. The US call sign
// range spans first nibbles 1 through 9 and A, which collide with European
// country codes across the board.
//
// Guessing wrong is silently wrong rather than loudly wrong. PTY 26 is
// National Music in Europe and Hip-Hop in North America. Both render, neither
// faults, and nothing downstream can tell.
//
// So the region is a setting. Seeding its default from the tuned frequency is
// a reasonable convenience for a user interface as long as the interface shows
// it as a setting that can be overridden rather than as an inference. When a
// 1A variant 0 group does arrive, RdsDecoder cross-checks the ECC against the
// configured region and raises a diagnostic; it never switches on its own. See
// StationState::ecc_contradicts_region.
enum class Region : std::uint8_t {
    kRds,
    kRbds,
};

[[nodiscard]] std::string_view region_name(Region region) noexcept;

// ---------------------------------------------------------------------------
// Block coding: EN 50067 clause 2.3 and Annexes A, B and C
// ---------------------------------------------------------------------------

// A group is four blocks and a block is 26 bits: a 16-bit information word
// followed by a 10-bit checkword. Transmission is fully synchronous with no
// gaps, most significant bit first, block 1 first. 104 bits at 1187.5 bit/s is
// 87.6 ms per group. EN 50067 clauses 2.1 and 2.2, Figures 8 and 9.
inline constexpr int kBitsPerBlock = 26;
inline constexpr int kBlocksPerGroup = 4;
inline constexpr int kBitsPerGroup = kBitsPerBlock * kBlocksPerGroup;

// g(x) = x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1, EN 50067 clause 2.3 and
// Annex B clause B.1.1. Eleven bits, so the leading 1 is the x^10 term.
inline constexpr std::uint16_t kCrcGenerator = 0x5B9;

// Which offset word a block carries. A, B, C or C', D are added to blocks 1
// through 4. Version A groups use C in block 3, version B groups use C' and
// repeat the PI code there. EN 50067 clause 2.4 and Annex A Table A.1.
//
// E is MMBS, a North American feature, and EN 50067 Annex A footnote 1
// forbids it outright in RDS: "Offset word E must not be used in RDS
// implementations corresponding to this specification." In the USA it appears
// in multiples of four blocks when RDS and MMBS run together. See
// RdsDecoder for what this decoder does with it, which is not to decode it.
enum class BlockOffset : std::uint8_t {
    kA,
    kB,
    kC,
    kCPrime,
    kD,
    kE,
};

[[nodiscard]] std::uint16_t offset_word(BlockOffset offset) noexcept;
[[nodiscard]] std::string_view offset_name(BlockOffset offset) noexcept;

// The 10-bit checkword for an information word: the remainder after
// multiplying by x^10 and dividing modulo 2 by g(x). The offset word is added
// modulo 2 afterwards, which make_block does.
[[nodiscard]] std::uint16_t checkword(std::uint16_t info) noexcept;

// A complete 26-bit block, information word in the top 16 bits.
[[nodiscard]] std::uint32_t make_block(std::uint16_t info, BlockOffset offset) noexcept;

// SYNDROME CONVENTION, and this comment is load-bearing because a reader who
// checks it against the wrong annex will conclude the table is corrupt.
//
// There are THREE conventions in play for the same five offset words and all
// three are correct for their own circuit. EN 50067 Annex C says so itself:
// "the syndromes obtained with this polynomial division register are different
// from that resulting from the matrix of figure B.3 or the circuit of figure
// B.4."
//
//   Annex B Table B.1   (x^325 * d) mod g, the parity-matrix convention, which
//                       premultiplies by x^325 because the code is a shortened
//                       cyclic code of natural length 341. A = 1111011000,
//                       B = 1111010100, C = 1001011100, C' = 1111001100,
//                       D = 1001011000.
//   Annex C Table C.1   (d * x^10) mod g, the polynomial-division-register
//                       convention. A = 0101111111, B = 0000001110,
//                       C = 0100101111, C' = 1011101100, D = 1010010111.
//   Here               plain d mod g, which for a 10-bit offset word is the
//                       offset word itself.
//
// This file uses the third, and it is not in either annex. A clean block
// satisfies block = info*x^10 + crc(info) + offset with crc(info) =
// (info*x^10) mod g, so block mod g is exactly the offset word. That makes
// Annex A Table A.1 serve as the syndrome table with no second table to keep
// in step, and it makes the error syndrome below a plain XOR against the
// offset the decoder was expecting. The two published tables are reproduced in
// the tests so that a reader checking either annex finds the arithmetic here
// agreeing with it rather than apparently contradicting it.
[[nodiscard]] std::uint16_t block_syndrome(std::uint32_t block) noexcept;

// Zero when the block is error free under that offset.
[[nodiscard]] std::uint16_t error_syndrome(std::uint32_t block, BlockOffset offset) noexcept;

// An error pattern the code can correct, and the span of the burst it covers.
// Span is the distance from the first wrong bit to the last inclusive, so a
// span of 2 is two adjacent bits and a span of 5 may leave the three bits
// between the ends correct.
struct BurstCorrection {
    std::uint32_t pattern = 0;
    std::uint8_t span = 0;
};

// WHAT THE STANDARD PERMITS, WHAT THIS CORRECTS, AND WHY THEY DIFFER.
//
// EN 50067 clause 2.3 gives the code's correction capability as any single
// burst of span 5 bits or less, and notes that this is optimal for the code:
// ten check bits, Rieger bound 2l <= n-k, so l = 5 exactly. All 367 bursts of
// span 1 through 5 in a 26-bit block have distinct syndromes, which the tests
// assert rather than assume, so a full-capability corrector is buildable and
// this function knows how to build it.
//
// It is nevertheless not what a receiver should run at full stretch. Using the
// whole 5-bit capability greatly increases the UNDETECTED error rate, because
// the syndrome that a wrong 6-bit burst lands on is very often the syndrome of
// some correctable 5-bit burst, and the corrector then rewrites a block into a
// different valid codeword. On a text field that produces a plausible wrong
// character rather than a gap, and a plausible wrong character is worse than a
// gap: nothing downstream can tell it happened. Kopitz and Marks section
// 12.2.3 records the field-trial practice that followed from exactly this: no
// correction at all while acquiring sync, and once synchronised, correction
// restricted to bursts spanning one or two bits, with longer bursts discarded.
//
// So RdsDecoder defaults to span 2 and applies nothing during acquisition. The
// limit is a setting rather than a constant because the restriction is
// receiver design practice from a textbook and not a normative requirement,
// and a caller decoding a clean recording offline has different arithmetic to
// do than a caller chasing a mobile fade.
//
// The restriction reduces miscorrection and does not abolish it. The code has
// minimum distance 5, so some weight-3 error is the sum of a weight-2 pattern
// and a weight-5 codeword and will be "corrected" into the wrong block. That
// is inherent in the code, not in this choice, and it is the reason the
// default is 2 rather than 5.
inline constexpr std::uint8_t kMaxCorrectableBurstSpan = 5;
inline constexpr std::uint8_t kDefaultCorrectableBurstSpan = 2;

// Nullopt when no burst of span at most max_span produces this syndrome, which
// is the answer that matters: a refusal leaves the block dropped rather than
// rewritten.
[[nodiscard]] std::optional<BurstCorrection> burst_for_syndrome(std::uint16_t syndrome,
                                                                std::uint8_t max_span) noexcept;

// ---------------------------------------------------------------------------
// Programme type names: EN 50067 Annex F Table F.1, NRSC-4-B Table F.2
// ---------------------------------------------------------------------------

enum class PtyWidth : std::uint8_t {
    kShort,  // the 8-character display
    kLong,   // the 16-character display
};

struct PtyEntry {
    std::string_view name;        // the row's prose name
    std::string_view short_name;  // 8-character display form
    std::string_view long_name;   // 16-character display form
};

// code is masked to five bits. The two tables agree on nothing except codes 0,
// 1, 30 and 31, and even those differ in wording, so there is no partial
// sharing to exploit.
[[nodiscard]] const PtyEntry& pty_entry(Region region, std::uint8_t code) noexcept;
[[nodiscard]] std::string_view pty_name(Region region, std::uint8_t code, PtyWidth width) noexcept;

// ---------------------------------------------------------------------------
// PI codes: EN 50067 Annex D, NRSC-4-B Annex D
// ---------------------------------------------------------------------------

// Nibble 1 is the country code, nibble 2 the programme area coverage, nibbles
// 3 and 4 the programme reference number. EN 50067 Annex D.
[[nodiscard]] constexpr std::uint8_t pi_country_code(std::uint16_t pi) noexcept {
    return static_cast<std::uint8_t>((pi >> 12) & 0x0F);
}

[[nodiscard]] constexpr std::uint8_t pi_coverage_area(std::uint16_t pi) noexcept {
    return static_cast<std::uint8_t>((pi >> 8) & 0x0F);
}

[[nodiscard]] constexpr std::uint8_t pi_reference(std::uint16_t pi) noexcept {
    return static_cast<std::uint8_t>(pi & 0xFF);
}

// Coverage area codes are valid for every PI code under IEC 62106 and for the
// B, D and E blocks only in North America, where the rest of the space is
// consumed by the call sign arithmetic. NRSC-4-B sections D.7 and D.7.3 call
// this a subtle yet significant difference that both broadcasters and receiver
// makers must note, which is why it is a function rather than a footnote.
[[nodiscard]] bool coverage_area_applies(Region region, std::uint16_t pi) noexcept;

// North American PI to call sign and back, NRSC-4-B section D.7.1. Nullopt in
// kRds, and in kRbds for any PI outside the call sign space.
[[nodiscard]] std::optional<std::string> callsign_from_pi(Region region, std::uint16_t pi);
[[nodiscard]] std::optional<std::uint16_t> pi_from_callsign(std::string_view call);

// ---------------------------------------------------------------------------
// Alternative frequencies: EN 50067 clause 3.2.1.6, NRSC-4-B section 6.2.1.6
// ---------------------------------------------------------------------------

// Meanings of the AF codes that are not frequencies. Tables 10 and 11 are
// identical in both regions.
inline constexpr std::uint8_t kAfCodeFiller = 205;
inline constexpr std::uint8_t kAfCodeNoneExists = 224;
inline constexpr std::uint8_t kAfCodeCountBase = 224;  // 225..249 mean 1..25 follow
inline constexpr std::uint8_t kAfCodeLfMfFollows = 250;

// Codes 1 to 204 are 87.6 MHz through 107.9 MHz in 100 kHz steps. Zero for any
// other code. EN 50067 Table 10, NRSC-4-B Table 10.
[[nodiscard]] dsp::Hertz af_vhf_frequency(std::uint8_t code) noexcept;

// The one genuinely region-dependent frequency table. EN 50067 Table 12 covers
// ITU regions 1 and 3 at 9 kHz spacing: codes 1 to 15 are LF 153 to 279 kHz
// and codes 16 to 135 are MF 531 to 1602 kHz. NRSC-4-B adds Table 12b for ITU
// region 2 at 10 kHz spacing: codes 17 to 133 are MF 540 to 1700 kHz. Zero
// outside the region's own range.
[[nodiscard]] dsp::Hertz af_lf_mf_frequency(Region region, std::uint8_t code) noexcept;

// ---------------------------------------------------------------------------
// Modified Julian Day: EN 50067 Annex G
// ---------------------------------------------------------------------------

struct CalendarDate {
    int year = 0;  // full year. Annex G's Y is years since 1900; this is not.
    int month = 0;
    int day = 0;
};

// Annex G's formulas are valid only from 1 March 1900 to 28 February 2100
// inclusive and produce wrong answers outside it without saying so, which is
// why these bounds are checked rather than trusted. MJD 0 is 17 November 1858,
// derived here from the round trip rather than printed in the annex.
inline constexpr int kMjdFirstValid = 15079;  // 1900-03-01
inline constexpr int kMjdLastValid = 88127;   // 2100-02-28

[[nodiscard]] Expected<CalendarDate> date_from_mjd(int mjd);
[[nodiscard]] Expected<int> mjd_from_date(CalendarDate date);

// 1 is Monday, per Annex G item c).
[[nodiscard]] Expected<int> day_of_week_from_mjd(int mjd);

// ---------------------------------------------------------------------------
// Decoded group and station state
// ---------------------------------------------------------------------------

struct Block {
    std::uint16_t value = 0;
    bool valid = false;      // received, and either clean or corrected
    bool corrected = false;  // a burst was repaired, so trust it less
};

struct Group {
    std::array<Block, kBlocksPerGroup> blocks{};

    // Which offset block 3 actually carried. A version B group uses C'. Kept
    // separate from version_b because block 2 can be lost while block 3
    // arrives clean, and then the offset is the only evidence there is.
    bool c_prime = false;

    std::uint8_t type = 0;   // A3..A0 from block 2, meaningless unless type_valid
    bool version_b = false;  // B0 from block 2
    bool type_valid = false;

    // Set when the group was framed correctly but every block in it carried
    // offset E. MMBS traffic in North America, not RDS. See RdsDecoder.
    bool mmbs = false;
};

// Decoder identification, EN 50067 clause 3.2.1.5 Table 9. Transmitted d3
// first, one bit per type 0 group, addressed by the same C1 C0 that addresses
// the PS segment.
struct DecoderIdentification {
    bool stereo = false;           // d0: 0 mono, 1 stereo
    bool artificial_head = false;  // d1
    bool compressed = false;       // d2
    bool dynamic_pty = false;      // d3: 0 static PTY, 1 dynamic
    std::uint8_t received = 0;     // bit n set once d(n) has arrived
};

// EN 50067 clause 3.1.5.2 notes 2 and 3 and clause 3.2.1.7. A day of zero
// means no valid PIN, and a receiver evaluating PIN must then ignore the rest
// of block 4, which is what valid == false records.
struct ProgrammeItemNumber {
    int day = 0;  // 1..31
    int hour = 0;
    int minute = 0;
    bool valid = false;
};

// EN 50067 clause 3.1.5.6. The information relates to the epoch immediately
// following the start of the next group, and the minute edge falls within
// 0.1 s of the end of the group carrying it.
struct ClockTime {
    int mjd = 0;
    CalendarDate date{};  // UTC, converted from mjd
    int hour = 0;         // UTC
    int minute = 0;       // UTC

    // Local time offset in half hours, signed. EN 50067 note 2 gives the range
    // as -12 h to +12 h; NRSC-4-B, tracking IEC 62106 Edition 2.0, gives
    // -15.5 h to +15.5 h. That is a VERSION difference and not a region
    // difference, the field is 6 bits either way, so the wider range is
    // accepted in both regions rather than being clamped per region.
    int offset_half_hours = 0;

    bool valid = false;
};

// An Open Data Application announcement from a type 3A group. EN 50067 clause
// 3.1.5.4 and Figure 18.
struct OdaAnnouncement {
    std::uint8_t group_type = 0;  // the group this application will use
    bool version_b = false;
    std::uint16_t message = 0;
    std::uint16_t aid = 0;  // 0x0000 means the group is used for its normal feature
};

// One other network, assembled across type 14A groups. EN 50067 clause
// 3.1.5.19 Figure 37.
struct EonEntry {
    std::uint16_t pi = 0;

    std::array<char, 8> ps{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    std::uint8_t ps_received = 0;  // one bit per character pair position 0..3

    bool tp = false;
    bool ta = false;
    bool ta_valid = false;

    std::uint8_t pty = 0;
    bool pty_valid = false;

    ProgrammeItemNumber pin{};

    std::uint16_t linkage = 0;
    bool linkage_valid = false;

    std::vector<dsp::Hertz> af;  // variant 4 and the mapped-frequency variants
};

struct StationState {
    // Programme identification, block 1 of every group and block 3 of every
    // version B group.
    std::uint16_t pi = 0;
    bool pi_valid = false;

    std::uint8_t pty = 0;
    bool pty_valid = false;

    bool tp = false;  // EN 50067 clause 3.2.1.3 Table 8 for the TP/TA pairings
    bool ta = false;
    bool tp_valid = false;
    bool ta_valid = false;

    // EN 50067 clause 3.2.1.4: 0 is speech, 1 is music, and 1 is also what a
    // broadcaster not using the feature transmits. So "music" here does not
    // mean a broadcaster said music.
    bool music = false;
    bool music_valid = false;

    DecoderIdentification di{};

    // Programme service name, eight characters in four two-character segments
    // addressed by C1 C0. ps_received carries one bit per segment.
    //
    // EN 50067 clause 3.1.5.1 says PS is intended for static display and that
    // "The use of PS to transmit text other than a single eight character name
    // is not permitted". NRSC-4-B's replacement text for the same clause drops
    // both the static-display wording and the prohibition, which makes
    // scrolling PS legal in North America and not in Europe. That is a real
    // region difference, and it is a difference in what a transmitter may do
    // rather than in how a receiver assembles the segments, so it changes
    // nothing here and is recorded so nobody goes looking for the branch.
    // Spaces rather than NULs, because a half-received PS is displayed and the
    // characters that have not arrived are blanks on that display rather than
    // string terminators. ps_received says which of the four segments are real.
    std::array<char, 8> ps{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    std::uint8_t ps_received = 0;

    // RadioText, 64 characters over 16 four-character segments in type 2A or
    // 32 characters over 16 two-character segments in type 2B. rt_length is
    // the position of the 0x0D terminator when one has arrived, or the highest
    // character index received plus one when it has not.
    std::array<char, 64> rt{};
    std::uint32_t rt_received = 0;  // one bit per segment address 0..15
    std::size_t rt_length = 0;
    bool rt_ab = false;
    bool rt_ab_valid = false;
    bool rt_version_b = false;  // which of 2A and 2B is carrying it

    // Programme type name, eight characters in two four-character segments.
    // EN 50067 clause 3.1.5.14: it may only enhance the PTY and must not carry
    // sequential information.
    std::array<char, 8> ptyn{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    std::uint8_t ptyn_received = 0;
    bool ptyn_ab = false;
    bool ptyn_ab_valid = false;

    ProgrammeItemNumber pin{};
    ClockTime clock{};

    // Type 1A variant 0. The ECC is the standard's own in-band country
    // indicator, EN 50067 Annex N clause N.3.
    std::uint8_t ecc = 0;
    bool ecc_valid = false;

    // Type 1A variant 3.
    std::uint8_t language = 0;
    bool language_valid = false;

    bool linkage_actuator = false;
    bool linkage_actuator_valid = false;

    // Raised when the ECC says ITU region 2 and the decoder is configured for
    // kRds. It is a diagnostic for an operator and never switches the region:
    // see the comment on Region. The converse is deliberately not raised,
    // because only the three region 2 allocations were read from Annex N and
    // an ECC outside them is absence of evidence rather than evidence of
    // Europe.
    bool ecc_contradicts_region = false;

    std::vector<dsp::Hertz> af;  // VHF and LF/MF alternatives, deduplicated
    std::uint8_t af_announced = 0;  // from a count code 225..249, 0 if none seen

    // AF method A against method B is not signalled. EN 50067 clause 3.2.1.6.5:
    // it "can easily be deduced by receivers from the frequent repetition of
    // the tuning frequency in the transmitted AF pairs in the case of AF method
    // B". af_repeats counts how often one frequency reappeared as the first of
    // a pair, which is the evidence that deduction rests on. It is reported
    // rather than acted on, because acting on it needs the tuned frequency and
    // this decoder is not told what the radio is tuned to.
    std::uint32_t af_repeats = 0;

    std::vector<OdaAnnouncement> oda;
    std::vector<EonEntry> eon;

    [[nodiscard]] std::string_view ps_text() const noexcept;
    [[nodiscard]] bool ps_complete() const noexcept { return ps_received == 0x0F; }
    [[nodiscard]] std::string_view rt_text() const noexcept;
    [[nodiscard]] std::string_view ptyn_text() const noexcept;
    [[nodiscard]] bool ptyn_complete() const noexcept { return ptyn_received == 0x03; }
};

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

enum class SyncState : std::uint8_t {
    kHunting,   // sliding the 26-bit window, no candidate
    kPreSync,   // a candidate anchor found, confirming it block by block
    kSynced,    // framed, decoding groups
};

class RdsDecoder {
public:
    struct Options {
        Region region = Region::kRds;

        // See kDefaultCorrectableBurstSpan. Zero disables correction entirely,
        // which is the honest setting for anything being archived.
        std::uint8_t correctable_burst_span = kDefaultCorrectableBurstSpan;

        // Bounds the EON table. A transmission referencing more networks than
        // this keeps the first ones seen rather than growing without limit.
        std::size_t max_eon_entries = 16;
    };

    RdsDecoder() = default;
    explicit RdsDecoder(Region region);
    explicit RdsDecoder(const Options& options);

    // One differentially decoded data bit, in transmission order, most
    // significant bit of each block first. This is the whole input surface:
    // the physical layer is on the far side of it and no type from it is
    // visible here.
    //
    // Not noexcept, and deliberately so. The AF, ODA and EON lists are all
    // bounded but they are still allocations, and per docs/conventions.md an
    // allocation failure is one of the few things allowed to leave as an
    // exception. Marking this noexcept would convert a bad_alloc into a
    // terminate inside the sample path.
    void feed(bool bit);

    // Drops framing and every partially assembled field, keeping the
    // configuration. For a retune, not for a fade.
    void reset();

    [[nodiscard]] Region region() const noexcept { return options_.region; }
    [[nodiscard]] SyncState sync_state() const noexcept { return sync_; }
    [[nodiscard]] bool synced() const noexcept { return sync_ == SyncState::kSynced; }

    [[nodiscard]] const StationState& state() const noexcept { return state_; }

    // The most recently completed group. A group completes once per 104 bits
    // at most, so a caller polling after every bit, every block or every group
    // sees all of them.
    [[nodiscard]] const std::optional<Group>& last_group() const noexcept { return last_group_; }

    [[nodiscard]] std::uint64_t bits_fed() const noexcept { return bits_fed_; }
    [[nodiscard]] std::uint64_t groups_decoded() const noexcept { return groups_decoded_; }
    [[nodiscard]] std::uint64_t blocks_good() const noexcept { return blocks_good_; }
    [[nodiscard]] std::uint64_t blocks_corrected() const noexcept { return blocks_corrected_; }
    [[nodiscard]] std::uint64_t blocks_dropped() const noexcept { return blocks_dropped_; }
    [[nodiscard]] std::uint64_t mmbs_blocks() const noexcept { return mmbs_blocks_; }
    [[nodiscard]] std::uint64_t sync_acquisitions() const noexcept { return acquisitions_; }
    [[nodiscard]] std::uint64_t sync_losses() const noexcept { return losses_; }

private:
    void hunt() noexcept;
    void confirm() noexcept;
    void receive_block();
    void finish_group();
    void lose_sync() noexcept;

    void apply_group(const Group& group);
    void apply_type0(const Group& group);
    void apply_type1a(const Group& group);
    void apply_type2(const Group& group);
    void apply_type3a(const Group& group);
    void apply_type4a(const Group& group);
    void apply_type10a(const Group& group);
    void apply_type14a(const Group& group);
    void apply_type14b(const Group& group);
    void apply_type15b(const Group& group);
    void apply_switching_payload(std::uint16_t word) noexcept;
    void apply_af_pair(std::uint16_t pair);
    void add_af(std::vector<dsp::Hertz>& list, dsp::Hertz hz);

    // Null when the table is full and this PI is not already in it, which is
    // the bound on how much a hostile or broken transmission can make the
    // decoder allocate.
    [[nodiscard]] EonEntry* eon_for(std::uint16_t pi);

    Options options_{};

    std::uint32_t window_ = 0;  // the last 26 bits, most recent in bit 0
    std::uint64_t bits_fed_ = 0;

    SyncState sync_ = SyncState::kHunting;
    int bits_to_boundary_ = 0;   // countdown to the next block boundary
    int expected_block_ = 0;     // 0..3, which block arrives at that boundary
    int confirmations_ = 0;      // consecutive good blocks while in kPreSync
    int error_credit_ = 0;       // the loss-of-sync leaky bucket

    Group group_{};
    bool group_open_ = false;  // a block 1 has arrived since sync was declared
    std::optional<Group> last_group_{};

    // AF assembly carries two bits of state across pairs: code 250 says the
    // NEXT code is an LF/MF frequency rather than a VHF one, and method B is
    // deduced from the tuning frequency reappearing as the first of a pair.
    bool af_pending_lf_mf_ = false;
    dsp::Hertz af_first_of_pair_ = 0;

    StationState state_{};

    std::uint64_t groups_decoded_ = 0;
    std::uint64_t blocks_good_ = 0;
    std::uint64_t blocks_corrected_ = 0;
    std::uint64_t blocks_dropped_ = 0;
    std::uint64_t mmbs_blocks_ = 0;
    std::uint64_t acquisitions_ = 0;
    std::uint64_t losses_ = 0;
};

}  // namespace revenant::decode
