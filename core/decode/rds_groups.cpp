// RDS and RBDS baseband coding and group decoding. See rds_groups.h for the
// provenance block, the region split and the syndrome convention.

#include "core/decode/rds_groups.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <utility>

namespace revenant::decode {

namespace {

constexpr std::uint32_t kBlockMask = 0x03FFFFFFu;  // 26 bits

// Division modulo 2 by g(x). Not a table, because it runs once per block
// rather than once per bit and the shift loop is short enough that a 256-entry
// table would be the slower thing to keep correct.
[[nodiscard]] constexpr std::uint32_t poly_mod(std::uint32_t value) noexcept {
    for (int bit = 25; bit >= 10; --bit) {
        if ((value & (1u << bit)) != 0) {
            value ^= static_cast<std::uint32_t>(kCrcGenerator) << (bit - 10);
        }
    }
    return value & 0x03FFu;
}

// Which offset word block n of a group carries, counting from zero. Block 3
// is answered as C here and receive_block overrides that to C' when it has
// evidence of a version B group: block 2's version bit when block 2 arrived,
// and otherwise block 3's own syndrome matching C' exactly.
//
// WHAT THIS COMMENT USED TO SAY. Until 2026-09-20 it said the C' case was
// "resolved against the received syndrome, because a version B group is only
// distinguishable by that offset or by a block 2 that may have been lost".
// Block 2 is no longer the fallback: when it arrived, its version bit is what
// chooses, because block 2 was measured against one offset and so had no
// second hypothesis to pick from. The sentence survived the change that
// replaced it. See RdsDecoder::receive_block.
[[nodiscard]] constexpr BlockOffset offset_for_index(int index) noexcept {
    switch (index) {
        case 0: return BlockOffset::kA;
        case 1: return BlockOffset::kB;
        case 2: return BlockOffset::kC;
        default: return BlockOffset::kD;
    }
}

// The 367 bursts of span 1 through 5 that fit in a 26-bit block, indexed by
// their syndrome.
//
// The table is only safe because every one of those bursts has a syndrome of
// its own, which is EN 50067 clause 2.3's correction claim restated: ten check
// bits, Rieger bound 2l <= n-k, so l = 5 is exactly achievable and exactly the
// limit. The two static_asserts below are that claim, checked at build time
// against the table actually constructed, and test_rds_groups.cpp checks it
// again from its own arithmetic over the whole set.
//
// WHAT THIS PARAGRAPH USED TO SAY. Until 2026-09-20 it said "the builder
// asserts it rather than assuming it". The builder recorded a collision in a
// bool and a count in an int and nothing anywhere read either of them, so a
// colliding table would have been built in silence and the first entry to
// reach a syndrome would have won. The fields are read now, which is what
// made the sentence true.
struct BurstTable {
    std::array<BurstCorrection, 1024> by_syndrome{};
    int entries = 0;
    bool collided = false;
};

[[nodiscard]] constexpr BurstTable build_burst_table() {
    BurstTable table;
    for (std::uint8_t span = 1; span <= kMaxCorrectableBurstSpan; ++span) {
        const int interior_bits = span >= 2 ? span - 2 : 0;
        const int interior_count = 1 << interior_bits;
        for (int start = 0; start + span <= kBitsPerBlock; ++start) {
            for (int interior = 0; interior < interior_count; ++interior) {
                std::uint32_t pattern = 1u << start;
                if (span >= 2) {
                    pattern |= 1u << (start + span - 1);
                    pattern |= static_cast<std::uint32_t>(interior) << (start + 1);
                }
                const std::uint32_t syndrome = poly_mod(pattern);
                BurstCorrection& slot = table.by_syndrome[syndrome];
                if (slot.span != 0) {
                    table.collided = true;
                    continue;
                }
                slot.pattern = pattern;
                slot.span = span;
                ++table.entries;
            }
        }
    }
    return table;
}

constexpr BurstTable kBurstTable = build_burst_table();

static_assert(!kBurstTable.collided,
              "two bursts of span 5 or less share a syndrome, so the corrector cannot tell "
              "them apart and would rewrite blocks into the wrong codeword");
static_assert(kBurstTable.entries == 367,
              "26 bits hold 26 bursts of span 1, 25 of span 2, 48 of span 3, 92 of span 4 "
              "and 176 of span 5; a different total means the builder's enumeration moved");

// EN 50067 Annex F Table F.1 (normative), all 32 rows: prose name, the
// 8-character display and the 16-character display.
//
// Codes 30 and 31 are control functions for a consumer receiver rather than
// labels (clause 3.2.1.2). 31 is for dynamic switching only and must never be
// used as a search criterion. Code 12 was "M.O.R. Music" in editions before
// this one.
constexpr std::array<PtyEntry, 32> kPtyRds = {{
    {"No programme type or undefined", "None", "None"},
    {"News", "News", "News"},
    {"Current Affairs", "Affairs", "Current Affairs"},
    {"Information", "Info", "Information"},
    {"Sport", "Sport", "Sport"},
    {"Education", "Educate", "Education"},
    {"Drama", "Drama", "Drama"},
    {"Culture", "Culture", "Cultures"},
    {"Science", "Science", "Science"},
    {"Varied", "Varied", "Varied Speech"},
    {"Pop Music", "Pop M", "Pop Music"},
    {"Rock Music", "Rock M", "Rock Music"},
    {"Easy Listening Music", "Easy M", "Easy Listening"},
    {"Light classical", "Light M", "Light Classics M"},
    {"Serious classical", "Classics", "Serious Classics"},
    {"Other Music", "Other M", "Other Music"},
    {"Weather", "Weather", "Weather & Metr"},
    {"Finance", "Finance", "Finance"},
    {"Children's programmes", "Children", "Children's Progs"},
    {"Social Affairs", "Social", "Social Affairs"},
    {"Religion", "Religion", "Religion"},
    {"Phone In", "Phone In", "Phone In"},
    {"Travel", "Travel", "Travel & Touring"},
    {"Leisure", "Leisure", "Leisure & Hobby"},
    {"Jazz Music", "Jazz", "Jazz Music"},
    {"Country Music", "Country", "Country Music"},
    {"National Music", "Nation M", "National Music"},
    {"Oldies Music", "Oldies", "Oldies Music"},
    {"Folk Music", "Folk M", "Folk Music"},
    {"Documentary", "Document", "Documentary"},
    {"Alarm Test", "TEST", "Alarm Test"},
    {"Alarm", "Alarm !", "Alarm - Alarm !"},
}};

// NRSC-4-B Table F.2, all 32 rows, now IEC 62106-9:2021.
//
// Three of the 16-character strings carry a stray space in the source
// document: "Top_ 40", "Soft_ R_&_B" and "Musica _Espanol". They are
// reproduced exactly rather than tidied, because the standard is the authority
// on what a receiver displays and a later reader comparing this table against
// it would otherwise conclude the code is wrong.
//
// Codes 27 and 28 have NO display string in the table, which is not the same
// as having an empty one. They are empty here because C++ has no third thing,
// and pty_entry's name field says "Unassigned" so the distinction survives.
//
// VINTAGE. This is the 2011 table. NRSC-4 (April 1998) Annex F Table F.1 left
// codes 24 through 28 all unassigned, and NRSC-4-B assigned 24 Spanish Talk,
// 25 Spanish Music and 26 Hip-Hop. There is no in-band vintage indicator, so a
// decoder set to kRbds labels a 1990s recording wrongly at those three codes
// and cannot know it. Adding a third Region enumerator for the older table
// would fix it and would light up all four region switches at build time,
// which is the shape the enum is designed for; it is not done here because
// nothing in this project decodes a 1990s recording yet.
constexpr std::array<PtyEntry, 32> kPtyRbds = {{
    {"No program type or undefined", "None", "None"},
    {"News", "News", "News"},
    {"Information", "Inform", "Information"},
    {"Sports", "Sports", "Sports"},
    {"Talk", "Talk", "Talk"},
    {"Rock", "Rock", "Rock"},
    {"Classic Rock", "Cls_Rock", "Classic_Rock"},
    {"Adult Hits", "Adlt_Hit", "Adult_Hits"},
    {"Soft Rock", "Soft_Rck", "Soft_Rock"},
    {"Top 40", "Top_40", "Top_ 40"},
    {"Country", "Country", "Country"},
    {"Oldies", "Oldies", "Oldies"},
    {"Soft", "Soft", "Soft"},
    {"Nostalgia", "Nostalga", "Nostalgia"},
    {"Jazz", "Jazz", "Jazz"},
    {"Classical", "Classicl", "Classical"},
    {"Rhythm and Blues", "R_&_B", "Rhythm_and_Blues"},
    {"Soft Rhythm and Blues", "Soft_R&B", "Soft_ R_&_B"},
    {"Foreign Language", "Language", "Foreign_Language"},
    {"Religious Music", "Rel_Musc", "Religious_Music"},
    {"Religious Talk", "Rel_Talk", "Religious_Talk"},
    {"Personality", "Persnlty", "Personality"},
    {"Public", "Public", "Public"},
    {"College", "College", "College"},
    {"Spanish Talk", "Habl_Esp", "Hablar_Espanol"},
    {"Spanish Music", "Musc_Esp", "Musica _Espanol"},
    {"Hip-Hop", "Hip hop", "Hip hop"},
    {"Unassigned", "", ""},
    {"Unassigned", "", ""},
    {"Weather", "Weather", "Weather"},
    {"Emergency Test", "Test", "Emergency_Test"},
    {"Emergency", "ALERT !", "ALERT!_ALERT!"},
}};

// Three-letter call signs, NRSC-4-B Table D.7 continued, in the reserved range
// 9950 to 9EFF (section D.7.1 exception 4). None of these is derivable from
// the arithmetic; they are hand assignments and have to be a table.
//
// INCOMPLETE, AND IT SAYS SO ON PURPOSE. The table in NRSC-4-B holds exactly
// 72 rows. These 36 are the ones transcribed by coordinate extraction from
// page 22 of the PDF, where pdftotext scrambles the column pairing badly
// enough that a plain text read produces wrong pairs. The other 36 were not
// transcribed, so a PI inside the reserved range and outside this table
// returns no call sign rather than a guess: a wrong call sign on a display is
// worse than a blank one, and inventing rows would be the one thing
// docs/clean-room.md most clearly forbids.
//
// Sorted by PI so the lookups below can binary search.
//
// WHAT THIS PARAGRAPH USED TO SAY. Until 2026-09-20 it said the test asserted
// the ordering. No test did. std::lower_bound on an unsorted range has
// undefined behaviour, and the visible shape of it here is quiet: a row placed
// out of order stops being found and its station returns no call sign, which
// looks exactly like one of the 36 rows nobody transcribed. The ordering is
// asserted at build time now, which is where a table literal belongs, and
// test_rds_groups.cpp walks the whole reserved range and counts the rows it
// can reach.
struct CallsignException {
    std::uint16_t pi;
    std::string_view call;  // the whole three-letter call sign, prefix included
};

constexpr std::array<CallsignException, 36> kCallsignExceptions = {{
    {0x9950, "KGO"},  {0x9951, "KGU"},  {0x9952, "KGW"},  {0x9953, "KGY"},
    {0x9954, "KID"},  {0x9964, "KQV"},  {0x9965, "KSL"},  {0x9966, "KUJ"},
    {0x9967, "KVI"},  {0x9968, "KWG"},  {0x9978, "WHO"},  {0x997A, "WIP"},
    {0x997B, "WJR"},  {0x997C, "WKY"},  {0x997D, "WLS"},  {0x9983, "WOL"},
    {0x9984, "WOR"},  {0x9988, "WWJ"},  {0x9989, "WWL"},  {0x9990, "KDB"},
    {0x9991, "KHQ"},  {0x9992, "KOY"},  {0x9993, "KPQ"},  {0x9994, "KSD"},
    {0x9999, "WBT"},  {0x999A, "WGH"},  {0x999B, "WGY"},  {0x999C, "WHP"},
    {0x999D, "WIL"},  {0x99A5, "KBW"},  {0x99A6, "KCY"},  {0x99A7, "KDF"},
    {0x99AA, "KLZ"},  {0x99AB, "KOB"},  {0x99B5, "WJZ"},  {0x99B9, "WRC"},
}};

// Strictly increasing, so the binary search is well defined and no PI appears
// twice with two different call signs.
[[nodiscard]] constexpr bool callsign_exceptions_ordered() noexcept {
    for (std::size_t i = 1; i < kCallsignExceptions.size(); ++i) {
        if (kCallsignExceptions[i - 1].pi >= kCallsignExceptions[i].pi) {
            return false;
        }
    }
    return true;
}

static_assert(callsign_exceptions_ordered(),
              "kCallsignExceptions must stay strictly increasing by PI: callsign_from_pi "
              "binary searches it, and a row out of order is a station that silently "
              "stops resolving");

// NRSC-4-B section D.7.1 step 3. The two ranges are exactly contiguous:
// KZZZ = 0x54A7 and WAAA = 0x54A8, and the reserved three-letter range starts
// one past WZZZ = 0x994F, so the whole space partitions with no gap and no
// overlap.
constexpr std::uint16_t kPiKBase = 4096;    // 0x1000
constexpr std::uint16_t kPiWBase = 21672;   // 0x54A8
constexpr std::uint16_t kPiKTop = 0x54A7;   // KZZZ
constexpr std::uint16_t kPiWTop = 0x994F;   // WZZZ
constexpr std::uint16_t kPiExceptionLow = 0x9950;
constexpr std::uint16_t kPiExceptionHigh = 0x9EFF;

// The ITU Region 2 Extended Country Codes named in EN 50067 Annex N clause
// N.3: USA (and Puerto Rico and the US Virgin Islands) A0, Canada A1, Mexico
// A5. Only these three were read, so only these three are treated as evidence.
// The rest of the A block is very likely region 2 as well and is deliberately
// not assumed, because the cross-check below is allowed to say "this is North
// America" and is not allowed to say "this is not".
[[nodiscard]] constexpr bool ecc_is_region_two(std::uint8_t ecc) noexcept {
    return ecc == 0xA0 || ecc == 0xA1 || ecc == 0xA5;
}

// Gregorian, so 1900 is not a leap year and 2000 is. Both fall inside Annex
// G's window, and 1900 is the one people get wrong.
[[nodiscard]] constexpr bool is_leap_year(int year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

[[nodiscard]] constexpr int days_in_month(int year, int month) noexcept {
    constexpr std::array<int, 12> kLengths = {31, 28, 31, 30, 31, 30,
                                              31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return kLengths[static_cast<std::size_t>(month - 1)];
}

[[nodiscard]] ProgrammeItemNumber decode_pin(std::uint16_t word) noexcept {
    ProgrammeItemNumber pin;
    pin.day = static_cast<int>((word >> 11) & 0x1F);
    pin.hour = static_cast<int>((word >> 6) & 0x1F);
    pin.minute = static_cast<int>(word & 0x3F);
    // Day zero means no valid PIN, and EN 50067 clause 3.1.5.2 note 3 says a
    // receiver evaluating PIN must then ignore the rest of block 4, which is
    // what refusing to mark it valid amounts to.
    pin.valid = pin.day >= 1 && pin.day <= 31 && pin.hour <= 23 && pin.minute <= 59;
    return pin;
}

[[nodiscard]] constexpr char high_char(std::uint16_t word) noexcept {
    return static_cast<char>((word >> 8) & 0xFF);
}

[[nodiscard]] constexpr char low_char(std::uint16_t word) noexcept {
    return static_cast<char>(word & 0xFF);
}

}  // namespace

// ---------------------------------------------------------------------------
// Region
// ---------------------------------------------------------------------------

std::string_view region_name(Region region) noexcept {
    switch (region) {
        case Region::kRds: return "RDS";
        case Region::kRbds: return "RBDS";
    }
    // MSVC cannot prove a switch over a scoped enum is exhaustive, because the
    // underlying type holds values that are not enumerators, so C4715 fires
    // without this. It is not a fallback for a region somebody forgot: /w14062
    // makes an unhandled enumerator a build error. See core/rpc/convert.cpp,
    // which carries the same shape and the measurement behind it.
    return "RDS";
}

// ---------------------------------------------------------------------------
// Block coding
// ---------------------------------------------------------------------------

std::uint16_t offset_word(BlockOffset offset) noexcept {
    // EN 50067 Annex A (normative) Table A.1, d9 down to d0, with d1 and d0
    // always zero. Verified two ways before being written down: by coordinate
    // extraction of the bit grid on page 59 of the PDF, because pdftotext
    // shifts the label column by one row, and against Annex B clause B.1.1's
    // worked example, whose offset B of 0110011000 is the only value that
    // reproduces the printed code vector.
    switch (offset) {
        case BlockOffset::kA: return 0x0FC;       // 0011111100
        case BlockOffset::kB: return 0x198;       // 0110011000
        case BlockOffset::kC: return 0x168;       // 0101101000
        case BlockOffset::kCPrime: return 0x350;  // 1101010000
        case BlockOffset::kD: return 0x1B4;       // 0110110100
        case BlockOffset::kE: return 0x000;       // 0000000000
    }
    return 0x000;  // C4715, as region_name explains
}

std::string_view offset_name(BlockOffset offset) noexcept {
    switch (offset) {
        case BlockOffset::kA: return "A";
        case BlockOffset::kB: return "B";
        case BlockOffset::kC: return "C";
        case BlockOffset::kCPrime: return "C'";
        case BlockOffset::kD: return "D";
        case BlockOffset::kE: return "E";
    }
    return "A";  // C4715
}

std::uint16_t checkword(std::uint16_t info) noexcept {
    return static_cast<std::uint16_t>(poly_mod(static_cast<std::uint32_t>(info) << 10));
}

std::uint32_t make_block(std::uint16_t info, BlockOffset offset) noexcept {
    const std::uint16_t check = static_cast<std::uint16_t>(checkword(info) ^ offset_word(offset));
    return ((static_cast<std::uint32_t>(info) << 10) | check) & kBlockMask;
}

std::uint16_t block_syndrome(std::uint32_t block) noexcept {
    return static_cast<std::uint16_t>(poly_mod(block & kBlockMask));
}

std::uint16_t error_syndrome(std::uint32_t block, BlockOffset offset) noexcept {
    return static_cast<std::uint16_t>(block_syndrome(block) ^ offset_word(offset));
}

std::optional<BurstCorrection> burst_for_syndrome(std::uint16_t syndrome,
                                                  std::uint8_t max_span) noexcept {
    if (syndrome == 0 || max_span == 0 || syndrome >= 1024) {
        return std::nullopt;
    }
    const BurstCorrection& slot = kBurstTable.by_syndrome[syndrome];
    if (slot.span == 0 || slot.span > max_span) {
        return std::nullopt;
    }
    return slot;
}

// ---------------------------------------------------------------------------
// Programme type names
// ---------------------------------------------------------------------------

const PtyEntry& pty_entry(Region region, std::uint8_t code) noexcept {
    const std::size_t index = code & 0x1F;
    switch (region) {
        case Region::kRds: return kPtyRds[index];
        case Region::kRbds: return kPtyRbds[index];
    }
    return kPtyRds[index];  // C4715
}

std::string_view pty_name(Region region, std::uint8_t code, PtyWidth width) noexcept {
    const PtyEntry& entry = pty_entry(region, code);
    switch (width) {
        case PtyWidth::kShort: return entry.short_name;
        case PtyWidth::kLong: return entry.long_name;
    }
    return entry.short_name;  // C4715
}

// ---------------------------------------------------------------------------
// PI codes
// ---------------------------------------------------------------------------

bool coverage_area_applies(Region region, std::uint16_t pi) noexcept {
    switch (region) {
        case Region::kRds:
            return true;
        case Region::kRbds: {
            // NRSC-4-B section D.7.3 lists exactly B_01 to B_FF, D_01 to D_FF
            // and E_01 to E_FF. Everywhere else in the North American space
            // the second nibble is an artefact of the call sign arithmetic and
            // means nothing about coverage.
            const std::uint8_t country = pi_country_code(pi);
            const bool linked_block = country == 0xB || country == 0xD || country == 0xE;
            return linked_block && pi_coverage_area(pi) != 0;
        }
    }
    return true;  // C4715
}

std::optional<std::string> callsign_from_pi(Region region, std::uint16_t pi) {
    switch (region) {
        case Region::kRds:
            // Call signs are a North American construction. A European PI in
            // the same numeric range is a country code and a programme
            // reference, and rendering it as four letters would be fiction.
            return std::nullopt;
        case Region::kRbds:
            break;
    }

    std::uint16_t value = pi;

    // Undo exception 2 before exception 1, because the nine codes 0x1000
    // through 0x9000 were remapped twice on the way out (P1 0 0 0 becomes
    // A P1 0 0 becomes A F A P1) and have to be unwound in the opposite order.
    // NRSC-4-B section D.7.1 exception 2 and its NOTE.
    //
    // BOTH EXCEPTIONS LEAVE A HOLE IN THE PI SPACE, AND THE HOLES HAVE TO BE
    // REFUSED RATHER THAN ANSWERED.
    //
    // Each exception rewrites a band of computed codes into somewhere else
    // before they are ever transmitted, so the band it rewrote is not a band a
    // station can be heard on. The arithmetic below runs happily on one of
    // those codes and produces the same four letters as the code that replaced
    // it, which puts two PI codes on one call sign. A wrong call sign on a
    // display is worse than a blank one, and two PI codes on one call sign is
    // the version of that with no way to tell which station is on the air.
    if ((value & 0xFF00) == 0xAF00) {
        value = static_cast<std::uint16_t>((value & 0x00FF) << 8);
    } else if ((value & 0x00FF) == 0) {
        // Exception 2's hole. A computed PI with a zero low byte became
        // 0xAF P1 P2 before transmission, so 0x1100 is not a code off the air
        // and 0xAF11 is the one that answers for it.
        return std::nullopt;
    }

    // Exception 1: a computed PI whose second nibble is zero was reassigned so
    // European receivers would not read it as a local station and refuse to AF
    // switch. A P1 P3 P4 goes back to P1 0 P3 P4.
    if (pi_country_code(value) == 0xA) {
        const std::uint8_t second = pi_coverage_area(value);
        if (second == 0 || second > 9) {
            // The computed range's first nibble only ever runs 1 through 9, so
            // A0xx and AAxx through AExx are not codes the algorithm produces.
            return std::nullopt;
        }
        value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(second) << 12) |
                                           (value & 0x00FF));
    } else if (pi_coverage_area(value) == 0) {
        // Exception 1's hole, and it is the larger of the two: 2304 codes.
        // 0x1050 was reassigned to 0xA150 before transmission, so 0xA150 is
        // the code that answers "KADC".
        //
        // Nine of these arrive by way of the exception 2 undo above rather
        // than off the air directly. 0xAF10 through 0xAF90 unwind to 0x1000
        // through 0x9000, which exception 1 had already taken to 0xA100
        // through 0xA900, and each of the nine answered with the same call
        // sign as the reachable 0xAFA1 through 0xAFA9. Exception 2's NOTE
        // says outright that those nine went through exception 1 first.
        return std::nullopt;
    }

    if (value >= kPiExceptionLow && value <= kPiExceptionHigh) {
        const auto it = std::lower_bound(
            kCallsignExceptions.begin(), kCallsignExceptions.end(), value,
            [](const CallsignException& row, std::uint16_t key) { return row.pi < key; });
        if (it != kCallsignExceptions.end() && it->pi == value) {
            return std::string(it->call);
        }
        return std::nullopt;
    }

    char prefix = 'K';
    int offset = 0;
    if (value >= kPiKBase && value <= kPiKTop) {
        prefix = 'K';
        offset = static_cast<int>(value) - static_cast<int>(kPiKBase);
    } else if (value >= kPiWBase && value <= kPiWTop) {
        prefix = 'W';
        offset = static_cast<int>(value) - static_cast<int>(kPiWBase);
    } else {
        return std::nullopt;
    }

    // NRSC-4-B section D.7.1 step 2 labels the letter columns "3rd letter
    // position", "2nd", "1st", which reads as though the rightmost letter
    // carries the 676 weight. It does not. Example 1 in section D.7.2 settles
    // it: KGTB is G=6*676 plus T=19*26 plus B=1, so the LEFTMOST of the three
    // letters after the prefix takes the 676 weight.
    std::string call;
    call.reserve(4);
    call.push_back(prefix);
    call.push_back(static_cast<char>('A' + offset / 676));
    call.push_back(static_cast<char>('A' + (offset / 26) % 26));
    call.push_back(static_cast<char>('A' + offset % 26));
    return call;
}

std::optional<std::uint16_t> pi_from_callsign(std::string_view call) {
    if (call.size() != 4 && call.size() != 3) {
        return std::nullopt;
    }

    std::string upper;
    upper.reserve(call.size());
    for (const char c : call) {
        upper.push_back(static_cast<char>(
            std::toupper(static_cast<unsigned char>(c))));
    }

    if (upper.size() == 3) {
        // Only the hand-assigned three-letter stations have a PI at all, and
        // only the ones actually transcribed are answerable. See the note on
        // kCallsignExceptions: the table is 36 of the standard's 72 rows.
        for (const CallsignException& row : kCallsignExceptions) {
            if (row.call == upper) {
                return row.pi;
            }
        }
        return std::nullopt;
    }

    if (upper[0] != 'K' && upper[0] != 'W') {
        return std::nullopt;
    }
    for (std::size_t i = 1; i < 4; ++i) {
        if (upper[i] < 'A' || upper[i] > 'Z') {
            return std::nullopt;
        }
    }

    const int value = 676 * (upper[1] - 'A') + 26 * (upper[2] - 'A') + (upper[3] - 'A');
    int pi = value + (upper[0] == 'K' ? static_cast<int>(kPiKBase) : static_cast<int>(kPiWBase));

    // WHAT NRSC-4-B SAYS HERE, AND WHY THIS CODE DOES NOT DO IT
    //
    // Section D.7.1 exception 3 gives a worked example stating that WYAY maps
    // to PI 4F78 and WYAI to 4F68. Both printed values are wrong, and they
    // have been wrong since 1998: the same numbers appear unchanged in
    // NRSC-4 (April 1998), in the NRSC-4-A 2005 annexes and in NRSC-4-B.
    //
    // Under the algorithm the same document specifies three paragraphs
    // earlier, WYAY is Y=24, A=0, Y=24, so VAL = 24*676 + 0*26 + 24 = 16248,
    // and a W call sign adds 21672, giving 37920 = 0x9420. WYAI gives 0x9410.
    // 0x4F78 is what the K constant of 4096 produces, and WYAY starts with W.
    //
    // This is recorded here rather than silently skipped because a reader who
    // checks the standard will find 4F78, conclude the code has a bug, and
    // "fix" working arithmetic. The test suite asserts 0x9420 and says the
    // same thing beside the vector.
    if ((pi & 0x0F00) == 0) {
        pi = 0xA000 | ((pi & 0xF000) >> 4) | (pi & 0x00FF);
    }
    if ((pi & 0x00FF) == 0) {
        pi = 0xAF00 | (pi >> 8);
    }
    return static_cast<std::uint16_t>(pi);
}

// ---------------------------------------------------------------------------
// Alternative frequencies
// ---------------------------------------------------------------------------

dsp::Hertz af_vhf_frequency(std::uint8_t code) noexcept {
    // EN 50067 Table 10 and NRSC-4-B Table 10, identical: code 0 is not to be
    // used, 1 to 204 are 87.6 MHz through 107.9 MHz in 100 kHz steps.
    //
    // Codes above 204 are the special-meaning table and stay that way here.
    // IEC 62106-1:2018 extends the band down to 64.0 MHz against EN 50067's
    // 87.5 MHz, and that text has not been read, so whether the code table was
    // extended to cover the OIRT band is an open question rather than an
    // assumption. NRSC-4-B's Table 10 still stops at 204 in 2011.
    if (code < 1 || code > 204) {
        return 0;
    }
    return 87'600'000 + static_cast<dsp::Hertz>(code - 1) * 100'000;
}

dsp::Hertz af_lf_mf_frequency(Region region, std::uint8_t code) noexcept {
    switch (region) {
        case Region::kRds:
            // EN 50067 Table 12, ITU regions 1 and 3, 9 kHz spacing.
            if (code >= 1 && code <= 15) {
                return 153'000 + static_cast<dsp::Hertz>(code - 1) * 9'000;
            }
            if (code >= 16 && code <= 135) {
                return 531'000 + static_cast<dsp::Hertz>(code - 16) * 9'000;
            }
            return 0;
        case Region::kRbds:
            // NRSC-4-B Table 12b, ITU region 2, 10 kHz spacing. This table is
            // an addition in a replacement section, so its absence from the
            // IEC text is deliberate rather than an oversight.
            if (code >= 17 && code <= 133) {
                return 540'000 + static_cast<dsp::Hertz>(code - 17) * 10'000;
            }
            return 0;
    }
    return 0;  // C4715
}

// ---------------------------------------------------------------------------
// Modified Julian Day
// ---------------------------------------------------------------------------

Expected<CalendarDate> date_from_mjd(int mjd) {
    if (mjd < kMjdFirstValid || mjd > kMjdLastValid) {
        return fail(std::format(
            "MJD {} is outside the validity window of EN 50067 Annex G, which runs from "
            "{} (1900-03-01) to {} (2100-02-28); outside it the annex formulas produce a "
            "wrong date rather than no date",
            mjd, kMjdFirstValid, kMjdLastValid));
    }

    // EN 50067 Annex G item a), written literally with integer truncation.
    // Verified against a Gregorian calendar for every day in the window above,
    // zero mismatches, which is why it is transcribed rather than replaced
    // with a general algorithm.
    const double m = static_cast<double>(mjd);
    const int yp = static_cast<int>((m - 15078.2) / 365.25);
    const int yp_days = static_cast<int>(static_cast<double>(yp) * 365.25);
    const int mp = static_cast<int>((m - 14956.1 - static_cast<double>(yp_days)) / 30.6001);
    const int mp_days = static_cast<int>(static_cast<double>(mp) * 30.6001);
    const int day = mjd - 14956 - yp_days - mp_days;
    const int k = (mp == 14 || mp == 15) ? 1 : 0;

    CalendarDate date;
    date.year = 1900 + yp + k;  // Annex G's Y counts from 1900; this does not
    date.month = mp - 1 - k * 12;
    date.day = day;
    return date;
}

Expected<int> mjd_from_date(CalendarDate date) {
    // The day is checked against the month's own length rather than against
    // 31. Annex G item b) is arithmetic and has no opinion: 2026-02-31 walks
    // through it and comes out as MJD 61102, which date_from_mjd then reads
    // back as 2026-03-03. A caller round-tripping a date it typed in gets a
    // different date with no error anywhere on the path, which is the shape
    // of wrong this decoder refuses everywhere else. 1900-02-29 is the same
    // failure at the window's edge: it lands on 15079, the first valid MJD,
    // which is 1900-03-01.
    if (date.month < 1 || date.month > 12 || date.day < 1 ||
        date.day > days_in_month(date.year, date.month)) {
        return fail(std::format("{:04}-{:02}-{:02} is not a calendar date", date.year,
                                date.month, date.day));
    }

    // The year goes through the window check BEFORE the arithmetic, not after
    // it with the rest. Item b) multiplies the year by 365.25 in double and
    // truncates to int, and a conversion whose value does not fit in an int
    // is undefined rather than wrapped: anything past about year 5.88 million
    // overflows. This is a public entry point taking an int year from
    // whatever a caller typed, so it is reachable rather than theoretical,
    // and the check below cannot catch it because by then the damage is in
    // the value being checked.
    if (date.year < kFirstValidYear || date.year > kLastValidYear) {
        return fail(std::format(
            "{:04}-{:02}-{:02} is outside the validity window of EN 50067 Annex G, "
            "1900-03-01 to 2100-02-28",
            date.year, date.month, date.day));
    }

    // EN 50067 Annex G item b). Y is years since 1900 in the annex.
    const int y = date.year - 1900;
    const int l = (date.month == 1 || date.month == 2) ? 1 : 0;
    const int mjd = 14956 + date.day +
                    static_cast<int>(static_cast<double>(y - l) * 365.25) +
                    static_cast<int>(static_cast<double>(date.month + 1 + l * 12) * 30.6001);

    if (mjd < kMjdFirstValid || mjd > kMjdLastValid) {
        return fail(std::format(
            "{:04}-{:02}-{:02} is outside the validity window of EN 50067 Annex G, "
            "1900-03-01 to 2100-02-28",
            date.year, date.month, date.day));
    }
    return mjd;
}

Expected<int> day_of_week_from_mjd(int mjd) {
    if (mjd < kMjdFirstValid || mjd > kMjdLastValid) {
        return fail(std::format("MJD {} is outside the EN 50067 Annex G validity window", mjd));
    }
    // Annex G item c): WD = ((MJD + 2) mod 7) + 1, Monday = 1.
    return ((mjd + 2) % 7) + 1;
}

// ---------------------------------------------------------------------------
// StationState views
// ---------------------------------------------------------------------------

std::string_view StationState::ps_text() const noexcept {
    return std::string_view(ps.data(), ps.size());
}

std::string_view StationState::rt_text() const noexcept {
    return std::string_view(rt.data(), rt_length);
}

std::string_view StationState::ptyn_text() const noexcept {
    return std::string_view(ptyn.data(), ptyn.size());
}

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

namespace {

// ACQUISITION POLICY, and the reasoning matters more than the number.
//
// EN 50067 Annex C clause C.1.1 gives the minimum: compute the syndrome of the
// 26-bit sliding window on every data clock and declare sync when two
// syndromes matching valid offset words in a valid group sequence are found
// n*26 bits apart. Two blocks is enough to declare, and it is not enough to be
// right. Annex C clause C.2 does the arithmetic: a random 26-bit window
// matches one of the five offsets with probability 5/1024, which at 1187.5
// bit/s is about six false anchors per second. After a false anchor the chance
// the next block at +26 also matches the one specific offset the sequence
// demands is 1/1024, so a two-block rule falsely declares sync roughly six
// times per thousand seconds of noise, or several times an hour. Every one of
// those produces a burst of garbage PS and RadioText before the loss-of-sync
// rule throws it away.
//
// This decoder requires the anchor plus three further blocks in sequence: one
// full group's worth, 104 bits, 87.6 ms. That puts the false declaration rate
// at 5/1024 * (1/1024)^3, which is nothing, and it costs at most one extra
// group of acquisition latency against the minimum rule. On a signal clean
// enough to be worth decoding, four consecutive clean blocks are the normal
// case, not a demand.
//
// No error correction runs during acquisition. Kopitz and Marks section 12.2.3
// records this as field-trial practice for the reason the arithmetic above
// gives: a corrector applied to a window that is not a block at all turns the
// 5/1024 false-match probability into something much larger, because it lets
// 367 more syndromes count as a match.
constexpr int kAcquireBlocks = 4;

// LOSS POLICY. EN 50067 Annex C clause C.1.2 says loss should be judged over
// several blocks, for example up to 50, and leaves the rule to the
// implementer. This is a leaky bucket: a block that cannot be decoded adds
// one, a clean block removes one, and sync is dropped at 50.
//
// Holding on through a fade is the whole point. The framing here is purely
// positional: once the bit clock upstream keeps running, a block boundary 26
// bits later is still a block boundary whether or not anything was audible in
// between. A decoder that drops sync after a handful of bad blocks then has to
// pay the acquisition cost above every time a car goes under a bridge, and the
// PS and RadioText assembly starts again from nothing each time. A total
// outage reaches 50 in 50 blocks, about 1.1 seconds, which is the standard's
// own suggested horizon; a signal at 50 percent block error hovers and keeps
// its framing, which is right, because half its blocks are still arriving.
//
// The thing this trades away is recovery from a clock slip. If the bit clock
// upstream gains or loses a bit during the fade, the framing is now wrong and
// every block fails, so the bucket fills at full rate and the 1.1 seconds is
// the worst case for noticing. EN 50067 Annex C clause C.1.2 has a cheaper
// detector for that case, noticing a known PI code arriving shifted one bit
// left or right, which belongs with the bit clock rather than here.
constexpr int kSyncLossThreshold = 50;

}  // namespace

RdsDecoder::RdsDecoder(Region region) {
    options_.region = region;
}

RdsDecoder::RdsDecoder(const Options& options) : options_(options) {
    options_.correctable_burst_span =
        std::min(options_.correctable_burst_span, kMaxCorrectableBurstSpan);
}

void RdsDecoder::reset() {
    window_ = 0;
    sync_ = SyncState::kHunting;
    bits_to_boundary_ = 0;
    expected_block_ = 0;
    confirmations_ = 0;
    error_credit_ = 0;
    group_ = Group{};
    group_open_ = false;
    last_group_.reset();
    state_ = StationState{};
    af_pending_lf_mf_ = false;
    af_first_of_pair_ = 0;

    // All eight counters, where this once cleared bits_fed_ alone. They are
    // the numerator and denominator of every rate a caller derives here, and
    // clearing one of the eight makes those rates wrong rather than stale: a
    // block error rate computed after a retune divided the previous station's
    // dropped blocks by the new station's bits.
    bits_fed_ = 0;
    groups_decoded_ = 0;
    blocks_good_ = 0;
    blocks_corrected_ = 0;
    blocks_dropped_ = 0;
    mmbs_blocks_ = 0;
    acquisitions_ = 0;
    losses_ = 0;
}

void RdsDecoder::feed(bool bit) {
    window_ = ((window_ << 1) | static_cast<std::uint32_t>(bit ? 1 : 0)) & kBlockMask;
    ++bits_fed_;

    // Nothing to test until the window holds a whole block.
    if (bits_fed_ < static_cast<std::uint64_t>(kBitsPerBlock)) {
        return;
    }

    switch (sync_) {
        case SyncState::kHunting:
            hunt();
            return;
        case SyncState::kPreSync:
            if (--bits_to_boundary_ <= 0) {
                confirm();
            }
            return;
        case SyncState::kSynced:
            if (--bits_to_boundary_ <= 0) {
                receive_block();
            }
            return;
    }
}

void RdsDecoder::hunt() noexcept {
    const std::uint16_t syndrome = block_syndrome(window_);

    // Only A, B, C, C' and D anchor. Offset E is not an anchor even in kRbds:
    // its value is zero, so every window that happens to be a codeword of the
    // bare cyclic code matches it, and it carries no block position to
    // sequence from. MMBS blocks are recognised once framing exists, not used
    // to establish it.
    int index = -1;
    if (syndrome == offset_word(BlockOffset::kA)) {
        index = 0;
    } else if (syndrome == offset_word(BlockOffset::kB)) {
        index = 1;
    } else if (syndrome == offset_word(BlockOffset::kC) ||
               syndrome == offset_word(BlockOffset::kCPrime)) {
        index = 2;
    } else if (syndrome == offset_word(BlockOffset::kD)) {
        index = 3;
    }

    if (index < 0) {
        return;
    }

    sync_ = SyncState::kPreSync;
    confirmations_ = 1;
    expected_block_ = (index + 1) % kBlocksPerGroup;
    bits_to_boundary_ = kBitsPerBlock;
}

void RdsDecoder::confirm() noexcept {
    const std::uint16_t syndrome = block_syndrome(window_);
    const BlockOffset expected = offset_for_index(expected_block_);

    bool matched = syndrome == offset_word(expected);
    if (!matched && expected_block_ == 2) {
        matched = syndrome == offset_word(BlockOffset::kCPrime);
    }

    if (!matched) {
        // Back to hunting from this bit, not from this block boundary. The
        // anchor was wrong, so nothing about the position it implied is worth
        // keeping, and the next bit could be the real one.
        sync_ = SyncState::kHunting;
        confirmations_ = 0;
        return;
    }

    ++confirmations_;
    expected_block_ = (expected_block_ + 1) % kBlocksPerGroup;
    bits_to_boundary_ = kBitsPerBlock;

    if (confirmations_ < kAcquireBlocks) {
        return;
    }

    // The confirming blocks are discarded rather than decoded. They are worth
    // at most one group and keeping them would mean assembling a group that
    // started before the decoder believed it was synchronised.
    sync_ = SyncState::kSynced;
    error_credit_ = 0;
    group_ = Group{};
    group_open_ = false;
    ++acquisitions_;
}

void RdsDecoder::receive_block() {
    const int index = expected_block_;
    if (index == 0) {
        group_ = Group{};
        group_open_ = true;
    }

    const std::uint16_t syndrome = block_syndrome(window_);

    // Block 3 carries C in a version A group and C' in a version B group.
    //
    // ONE OFFSET, CHOSEN BEFORE ANY CORRECTION RUNS.
    //
    // Block 3 is the only block with two candidate offsets, so it is the only
    // one that could be handed a second correction attempt when the first
    // refuses. It is not handed one. Every block in the group, block 3
    // included, is measured against exactly one offset and corrected at most
    // once, because a second attempt is a second chance at a correctable
    // residue and that is a lower effective error threshold for this block
    // position than for the other three.
    //
    // The offset is picked from whatever evidence exists, in this order.
    //
    // Block 2's version bit, whenever block 2 arrived. Block 2 was measured
    // against one offset and had no second hypothesis to choose from, so its
    // answer is evidence rather than something the corrector selected.
    //
    // Otherwise block 3's own syndrome, and only on an exact match with C'.
    // A block that needs correcting is corrected against C. Taking C' on an
    // exact match costs one acceptance and buys the only way a version B
    // group announces itself when block 2 has faded: of the 1023 ways a
    // block's syndrome can arrive wrong, 51 are correctable for blocks 1, 2
    // and 4, 51 are correctable here, and one more is the arithmetic
    // coincidence where offset C XOR offset C' is exactly the error. That
    // one is a clean codeword and not a correction, and apply_group refuses
    // to read a PI out of it. All four of those counts are swept in
    // test_rds_groups.cpp rather than reasoned about here.
    //
    // WHAT THIS COMMENT USED TO SAY. Until 2026-09-20 it said "only a lost
    // block 2 leaves block 3's own syndrome to answer, and then both offsets
    // are tested as before", which was the retry this paragraph now refuses.
    // Testing both accepted 101 of the 1023 against 51 everywhere else, and
    // fifty of the extra came back flagged C' and rewrote the station PI with
    // block 3's payload. The version-bit rule that arrived with that sentence
    // closed the case where block 2 was valid and left the case it was
    // written about wide open.
    BlockOffset offset = offset_for_index(index);
    bool c_prime = false;
    if (index == 2) {
        if (group_.blocks[1].valid) {
            c_prime = ((group_.blocks[1].value >> 11) & 0x01) != 0;
        } else {
            c_prime = syndrome == offset_word(BlockOffset::kCPrime);
        }
        offset = c_prime ? BlockOffset::kCPrime : BlockOffset::kC;
    }

    std::uint16_t residue = static_cast<std::uint16_t>(syndrome ^ offset_word(offset));

    // MMBS. EN 50067 Annex A footnote 1 records that offset word E is used in
    // the USA in multiples of four blocks when RDS and MMBS run together, and
    // forbids it in RDS. MMBS itself is out of scope here: these blocks are
    // counted, discarded, and charged nothing against the loss-of-sync bucket,
    // because they are somebody else's data arriving on schedule rather than
    // this decoder failing. In kRds a block matching E is simply a block whose
    // syndrome is wrong, and falls through to the correction path below.
    if (residue != 0 && syndrome == offset_word(BlockOffset::kE) &&
        options_.region == Region::kRbds) {
        group_.mmbs = true;
        ++mmbs_blocks_;
        expected_block_ = (index + 1) % kBlocksPerGroup;
        bits_to_boundary_ = kBitsPerBlock;
        if (index == kBlocksPerGroup - 1 && group_open_) {
            finish_group();
        }
        return;
    }

    std::uint32_t corrected_window = window_;
    bool corrected = false;
    if (residue != 0) {
        if (const auto burst = burst_for_syndrome(residue, options_.correctable_burst_span)) {
            corrected_window ^= burst->pattern;
            residue = 0;
            corrected = true;
        }
    }

    Block& block = group_.blocks[static_cast<std::size_t>(index)];
    if (residue == 0) {
        block.value = static_cast<std::uint16_t>((corrected_window >> 10) & 0xFFFF);
        block.valid = true;
        block.corrected = corrected;
        if (corrected) {
            ++blocks_corrected_;
        } else {
            ++blocks_good_;
            error_credit_ = std::max(0, error_credit_ - 1);
        }
        if (index == 2) {
            group_.c_prime = c_prime;
        }
    } else {
        ++blocks_dropped_;
        ++error_credit_;
    }

    expected_block_ = (index + 1) % kBlocksPerGroup;
    bits_to_boundary_ = kBitsPerBlock;

    if (index == kBlocksPerGroup - 1 && group_open_) {
        finish_group();
    }

    if (error_credit_ >= kSyncLossThreshold) {
        lose_sync();
    }
}

void RdsDecoder::finish_group() {
    if (group_.blocks[1].valid) {
        group_.type = static_cast<std::uint8_t>((group_.blocks[1].value >> 12) & 0x0F);
        group_.version_b = ((group_.blocks[1].value >> 11) & 0x01) != 0;
        group_.type_valid = true;
    }

    ++groups_decoded_;
    apply_group(group_);
    last_group_ = group_;
    group_open_ = false;
}

void RdsDecoder::lose_sync() noexcept {
    sync_ = SyncState::kHunting;
    confirmations_ = 0;
    error_credit_ = 0;
    group_open_ = false;
    ++losses_;
}

// ---------------------------------------------------------------------------
// Group parsers. Every one of these is region free; see the header.
// ---------------------------------------------------------------------------

void RdsDecoder::apply_group(const Group& group) {
    if (group.blocks[0].valid) {
        state_.pi = group.blocks[0].value;
        state_.pi_valid = true;
    }

    if (!group.type_valid) {
        // Without block 2 there is no group type, and every field below is
        // addressed by it. Block 1's PI above is the only thing recoverable,
        // block 3's PI repeat included: see the paragraph under the gate.
        return;
    }

    // A version B group repeats PI in block 3, and block 2 is what says the
    // group is version B. Nothing else is allowed to say it.
    //
    // WHY NOT c_prime, WHICH IS THE FIELD THAT NAMES THE OFFSET. Because with
    // block 2 lost, c_prime is block 3's opinion of itself. A version A block
    // 3 whose error happens to be offset C XOR offset C' arrives as a
    // faultless C' codeword, and there is no second witness to say otherwise,
    // so reading a PI out of it puts an AF pair or two characters of
    // RadioText on the display as the station. The block is still accepted,
    // because a clean C' codeword is the only announcement a version B group
    // makes once block 2 has faded and refusing it would drop block 3 of
    // every version B transmission in a fade. It is accepted as a block and
    // refused as an identity.
    //
    // TWO NARROWER RULES THAT WERE CONSIDERED AND DO NOT HOLD. Refusing the
    // write when block 1 already supplied a PI this group closes the shape
    // where block 1 arrived and leaves the one where it did not: block 1 and
    // block 2 both lost, block 3 promoted, and the PI is written from it with
    // nothing to contradict it. Refusing the write when block 3 was corrected
    // is wrong in both directions at once: it admits the exact C' coincidence
    // above, which needs no corrector at all, and it refuses the real version
    // B group whose block 3 took a burst, which is the case the correction
    // exists for.
    //
    // With block 2 arrived, c_prime and version_b are the same answer on a
    // valid block 3, so this reads version_b and the version_b branches in
    // apply_type0 and apply_type2 cannot disagree with it about one group.
    if (group.version_b && group.blocks[2].valid) {
        state_.pi = group.blocks[2].value;
        state_.pi_valid = true;
    }

    const std::uint16_t b2 = group.blocks[1].value;
    state_.tp = ((b2 >> 10) & 0x01) != 0;
    state_.tp_valid = true;
    state_.pty = static_cast<std::uint8_t>((b2 >> 5) & 0x1F);
    state_.pty_valid = true;

    switch (group.type) {
        case 0: apply_type0(group); break;
        case 1: if (!group.version_b) { apply_type1a(group); } break;
        case 2: apply_type2(group); break;
        case 3: if (!group.version_b) { apply_type3a(group); } break;
        case 4: if (!group.version_b) { apply_type4a(group); } break;
        case 10: if (!group.version_b) { apply_type10a(group); } break;
        case 14:
            if (group.version_b) {
                apply_type14b(group);
            } else {
                apply_type14a(group);
            }
            break;
        case 15: if (group.version_b) { apply_type15b(group); } break;
        default:
            // Types 5 through 9, 11, 12, 13, the B versions of 1, 3, 4 and 10,
            // and 15A are all real group types that this decoder does not
            // parse. A default is correct here and is not the exhaustiveness
            // hole /w14062 exists to catch: group.type is a four-bit field off
            // the air, not an enum, and every one of the sixteen values is a
            // value a transmitter may legally send.
            //
            // Type 8A is TMC and is specified in CEN ENV 12313-1 rather than
            // in the RDS standard at all. Type 15A was Fast PS in North
            // America and NRSC-4 (April 1998) clause 3.1.5.20 ordered encoder
            // makers to stop emitting it and receiver makers to stop
            // recognising it; NRSC-4-B reassigned it to Open Data
            // Applications while EN 50067 Table 3 still lists it as defined in
            // RBDS only. Not recognising it is the behaviour the 1998 text
            // asked for.
            break;
    }
}

void RdsDecoder::apply_switching_payload(std::uint16_t word) noexcept {
    // The block 2 shape shared by type 0 groups and by both halves of a type
    // 15B group: TP at b10, PTY at b9..b5, TA at b4, M/S at b3, one DI bit at
    // b2 and the DI and PS segment address at b1..b0.
    state_.tp = ((word >> 10) & 0x01) != 0;
    state_.tp_valid = true;
    state_.pty = static_cast<std::uint8_t>((word >> 5) & 0x1F);
    state_.pty_valid = true;
    state_.ta = ((word >> 4) & 0x01) != 0;
    state_.ta_valid = true;

    // EN 50067 clause 3.2.1.4: 0 is speech, 1 is music, and 1 is also what a
    // broadcaster who is not using the feature transmits.
    state_.music = ((word >> 3) & 0x01) != 0;
    state_.music_valid = true;

    const std::uint8_t address = static_cast<std::uint8_t>(word & 0x03);
    const bool di_bit = ((word >> 2) & 0x01) != 0;

    // EN 50067 clause 3.1.5.1 note 5 with clause 3.2.1.5 Table 9: d3 is
    // transmitted first, with segment address 00, so the bit index counts down
    // as the address counts up.
    const int di_index = 3 - static_cast<int>(address);
    switch (di_index) {
        case 0: state_.di.stereo = di_bit; break;
        case 1: state_.di.artificial_head = di_bit; break;
        case 2: state_.di.compressed = di_bit; break;
        default: state_.di.dynamic_pty = di_bit; break;
    }
    state_.di.received = static_cast<std::uint8_t>(state_.di.received | (1u << di_index));
}

void RdsDecoder::apply_type0(const Group& group) {
    const std::uint16_t b2 = group.blocks[1].value;
    apply_switching_payload(b2);

    const std::size_t address = static_cast<std::size_t>(b2 & 0x03);

    // Version A carries two AF codes in block 3. Version B repeats PI there
    // and carries no AF at all.
    if (!group.version_b && group.blocks[2].valid) {
        apply_af_pair(group.blocks[2].value);
    }

    if (group.blocks[3].valid) {
        const std::uint16_t b4 = group.blocks[3].value;
        state_.ps[address * 2] = high_char(b4);
        state_.ps[address * 2 + 1] = low_char(b4);
        state_.ps_received = static_cast<std::uint8_t>(state_.ps_received | (1u << address));

        // BLOCK 2 COUNTS AS WELL AS BLOCK 4, because `address` came out of
        // block 2. A mis-corrected block 2 is rewritten into a different
        // valid codeword, and two of the bits that moved may be the segment
        // address: the station's own characters are then written into a
        // segment they do not belong to, and marking only block 4 says the
        // pair arrived clean, which they did. What is wrong is where they
        // landed, and the block that decided that is block 2.
        //
        // WHAT THIS USED TO READ: group.blocks[3].corrected alone. The
        // display then showed a segment nothing had touched in this
        // rotation as a clean reception of the station's name.
        const bool corrected = group.blocks[1].corrected || group.blocks[3].corrected;

        // Assigned rather than accumulated, so a clean reception of the same
        // segment clears the doubt the corrected one raised. See
        // StationState::ps_corrected.
        if (corrected) {
            state_.ps_corrected = static_cast<std::uint8_t>(state_.ps_corrected | (1u << address));
        } else {
            state_.ps_corrected =
                static_cast<std::uint8_t>(state_.ps_corrected & ~(1u << address));
        }
    }
}

void RdsDecoder::apply_type1a(const Group& group) {
    if (group.blocks[2].valid) {
        const std::uint16_t b3 = group.blocks[2].value;
        state_.linkage_actuator = ((b3 >> 15) & 0x01) != 0;
        state_.linkage_actuator_valid = true;

        const std::uint8_t variant = static_cast<std::uint8_t>((b3 >> 12) & 0x07);
        const std::uint16_t payload = static_cast<std::uint16_t>(b3 & 0x0FFF);

        // EN 50067 clause 3.1.5.2 Figure 14 footnotes 1 to 8. Variants 4 and 5
        // are unassigned; 6 is for broadcasters' own use and note 7 says
        // consumer receivers must ignore it entirely, which is why it is not
        // stored anywhere; 7 is EWS channel identification, which needs an
        // EWS decoder this project does not have.
        switch (variant) {
            case 0: {
                state_.ecc = static_cast<std::uint8_t>(payload & 0xFF);
                state_.ecc_valid = true;
                // The cross-check, never a switch. See the comment on Region.
                if (options_.region == Region::kRds && ecc_is_region_two(state_.ecc)) {
                    state_.ecc_contradicts_region = true;
                }
                break;
            }
            case 3:
                state_.language = static_cast<std::uint8_t>(payload & 0xFF);
                state_.language_valid = true;
                break;
            default:
                break;
        }
    }

    if (group.blocks[3].valid) {
        const ProgrammeItemNumber pin = decode_pin(group.blocks[3].value);
        if (pin.valid) {
            state_.pin = pin;
        }
    }
}

void RdsDecoder::apply_type2(const Group& group) {
    const std::uint16_t b2 = group.blocks[1].value;
    const bool ab = ((b2 >> 4) & 0x01) != 0;
    const std::size_t address = static_cast<std::size_t>(b2 & 0x0F);

    // THE A/B FLAG, which is the one piece of RadioText handling that produces
    // a visibly wrong answer when it is got wrong rather than a missing one.
    //
    // EN 50067 clause 3.1.5.3: on a change of the flag in either direction the
    // whole display is cleared before the new segments are written; with no
    // change, received segments overwrite in place and segments not received
    // are left alone. Skipping the clear splices the tail of the previous
    // message onto the head of the new one, and the result reads as a complete
    // sentence, so nobody downstream can tell it is two messages.
    //
    // A change of version does the same thing, for a different reason. Clause
    // 3.1.5.3 forbids mixing 2A and 2B within one message because the segment
    // addressing means four characters in one and two in the other, so the
    // same address names different character positions. A transmitter that
    // switches mid-message has already broken the contract; clearing is the
    // only reading that cannot splice.
    const bool changed = !state_.rt_ab_valid || state_.rt_ab != ab ||
                         state_.rt_version_b != group.version_b;
    if (changed) {
        state_.rt.fill('\0');
        state_.rt_received = 0;
        state_.rt_corrected = 0;
        state_.rt_length = 0;
        state_.rt_terminator = kNoRtTerminator;
        state_.rt_high_water = 0;
    }
    state_.rt_ab = ab;
    state_.rt_ab_valid = true;
    state_.rt_version_b = group.version_b;

    const std::size_t chars = group.version_b ? 2u : 4u;
    const std::size_t base = address * chars;

    auto put = [&](std::size_t index, char c) {
        if (index >= state_.rt.size()) {
            return;
        }
        state_.rt[index] = c;
        state_.rt_high_water = std::max(state_.rt_high_water, index + 1);
    };

    bool segment_complete = false;

    // BLOCK 2 IS PART OF EVERY SEGMENT IT ADDRESSES. `address` is four bits
    // of block 2 and the A/B flag is a fifth, so a mis-corrected block 2
    // writes the station's own characters into the wrong segment, or clears
    // a message that had not ended. Either way the segment this group
    // touches is not one the station sent, and marking only the character
    // blocks reported it clean.
    //
    // WHAT THIS USED TO READ: the two character blocks alone. The 2A
    // segment written by a rewritten address then showed as a clean
    // reception in the middle of a RadioText nobody transmitted.
    bool segment_corrected = group.blocks[1].corrected;
    if (group.version_b) {
        if (group.blocks[3].valid) {
            put(base, high_char(group.blocks[3].value));
            put(base + 1, low_char(group.blocks[3].value));
            segment_complete = true;
            segment_corrected = segment_corrected || group.blocks[3].corrected;
        }
    } else {
        if (group.blocks[2].valid) {
            put(base, high_char(group.blocks[2].value));
            put(base + 1, low_char(group.blocks[2].value));
        }
        if (group.blocks[3].valid) {
            put(base + 2, high_char(group.blocks[3].value));
            put(base + 3, low_char(group.blocks[3].value));
        }
        segment_complete = group.blocks[2].valid && group.blocks[3].valid;
        segment_corrected = segment_corrected ||
                            (group.blocks[2].valid && group.blocks[2].corrected) ||
                            (group.blocks[3].valid && group.blocks[3].corrected);
    }

    if (segment_complete) {
        state_.rt_received |= 1u << address;
    }

    // The mark is set on any write and cleared only on a complete clean one,
    // and the asymmetry is the whole of it. put() writes whatever blocks
    // arrived, so a 2A group whose block 3 was corrected and whose block 4
    // was dropped puts two rewritten characters into a segment that
    // rt_received already marks as received from an earlier rotation. Tying
    // the mark to segment_complete would leave those two characters shown as
    // clean. Clearing needs the whole segment rewritten from good blocks,
    // because half a clean segment says nothing about the other half.
    if (segment_corrected) {
        state_.rt_corrected |= 1u << address;
    } else if (segment_complete) {
        state_.rt_corrected &= ~(1u << address);
    }

    // THE TERMINATOR BELONGS TO THE MESSAGE, NOT TO THE GROUP.
    //
    // 0x0D ends a message shorter than the full sixteen segments. 0x0A is a
    // preferred line break and 0x0B and 0x1F are an end-of-headline and a
    // soft hyphen that NRSC-4-B section 6.1.5.3 adds with a note that at
    // least one RDS IC vendor does not support them. None of the three ends
    // the message, so only 0x0D is looked for.
    //
    // WHAT THIS USED TO DO: look for 0x0D among the two or four characters
    // THIS group carried, and fall back to max(rt_length, this group's
    // highest index plus one) when it found none. A terminator that arrived
    // in an earlier group was then forgotten the moment any later segment
    // landed behind it, and rt_length grew past it. That publishes the 0x0D
    // and whatever follows it as message text, and it breaks the invariant
    // core/rpc/revenant.capnp states for rtLength on the wire, which clients
    // are told they can rely on because the terminator is inside the payload
    // and they cannot tell an unreceived NUL from a short message.
    //
    // Found by scanning the buffer rather than kept incrementally, because a
    // segment can be overwritten: same A/B flag, a shorter message, and the
    // group carrying the 0x0D is rewritten with ordinary characters. An
    // incremental terminator would then have to be invalidated and the
    // buffer rescanned anyway, and there can be a second 0x0D behind the
    // first. Sixty-four bytes at most every 87.6 ms.
    state_.rt_terminator = kNoRtTerminator;
    for (std::size_t i = 0; i < state_.rt_high_water; ++i) {
        if (state_.rt[i] == '\r') {
            state_.rt_terminator = i;
            break;
        }
    }
    state_.rt_length =
        state_.rt_terminator == kNoRtTerminator ? state_.rt_high_water : state_.rt_terminator;
}

void RdsDecoder::apply_type3a(const Group& group) {
    if (!group.blocks[1].valid || !group.blocks[3].valid) {
        return;
    }

    const std::uint16_t b2 = group.blocks[1].value;
    OdaAnnouncement announcement;
    // EN 50067 Figure 18: the five low bits of block 2 are the application
    // group type code, four bits of type plus the version bit.
    announcement.group_type = static_cast<std::uint8_t>((b2 >> 1) & 0x0F);
    announcement.version_b = (b2 & 0x01) != 0;
    announcement.message = group.blocks[2].valid ? group.blocks[2].value : 0;
    announcement.aid = group.blocks[3].value;

    // Application group code 00000 means the application is not carried in an
    // associated group, and 11111 means a temporary data fault in the encoder.
    // Neither names a group, so neither belongs in the table.
    const std::uint8_t code = static_cast<std::uint8_t>(b2 & 0x1F);
    if (code == 0x00 || code == 0x1F) {
        return;
    }

    for (OdaAnnouncement& existing : state_.oda) {
        if (existing.group_type == announcement.group_type &&
            existing.version_b == announcement.version_b) {
            existing = announcement;
            return;
        }
    }
    state_.oda.push_back(announcement);
}

void RdsDecoder::apply_type4a(const Group& group) {
    if (!group.blocks[1].valid || !group.blocks[2].valid || !group.blocks[3].valid) {
        return;
    }

    // EN 50067 Figure 20. Seventeen bits of MJD split two and fifteen across
    // blocks 2 and 3, then five bits of hour split one and four across blocks
    // 3 and 4, then six bits of minute, a sign bit and five bits of magnitude.
    const std::uint16_t b2 = group.blocks[1].value;
    const std::uint16_t b3 = group.blocks[2].value;
    const std::uint16_t b4 = group.blocks[3].value;

    const int mjd = static_cast<int>(((b2 & 0x0003u) << 15) | (b3 >> 1));
    const int hour = static_cast<int>(((b3 & 0x0001u) << 4) | (b4 >> 12));
    const int minute = static_cast<int>((b4 >> 6) & 0x3F);
    const bool negative = ((b4 >> 5) & 0x01) != 0;
    const int magnitude = static_cast<int>(b4 & 0x1F);

    if (hour > 23 || minute > 59) {
        return;
    }

    const auto date = date_from_mjd(mjd);
    if (!date) {
        // Outside Annex G's window the conversion is wrong rather than
        // absent, so the whole clock is refused instead of being published
        // with a plausible date attached.
        return;
    }

    ClockTime clock;
    clock.mjd = mjd;
    clock.date = *date;
    clock.hour = hour;
    clock.minute = minute;
    clock.offset_half_hours = negative ? -magnitude : magnitude;
    clock.valid = true;
    state_.clock = clock;
}

void RdsDecoder::apply_type10a(const Group& group) {
    const std::uint16_t b2 = group.blocks[1].value;
    const bool ab = ((b2 >> 4) & 0x01) != 0;
    const std::size_t address = static_cast<std::size_t>(b2 & 0x01);

    if (!state_.ptyn_ab_valid || state_.ptyn_ab != ab) {
        state_.ptyn.fill(' ');
        state_.ptyn_received = 0;
    }
    state_.ptyn_ab = ab;
    state_.ptyn_ab_valid = true;

    const std::size_t base = address * 4;
    if (group.blocks[2].valid) {
        state_.ptyn[base] = high_char(group.blocks[2].value);
        state_.ptyn[base + 1] = low_char(group.blocks[2].value);
    }
    if (group.blocks[3].valid) {
        state_.ptyn[base + 2] = high_char(group.blocks[3].value);
        state_.ptyn[base + 3] = low_char(group.blocks[3].value);
    }
    if (group.blocks[2].valid && group.blocks[3].valid) {
        state_.ptyn_received = static_cast<std::uint8_t>(state_.ptyn_received | (1u << address));
    }
}

void RdsDecoder::apply_type14a(const Group& group) {
    // Block 4 always carries PI(ON). Without it there is no way to know whose
    // information block 3 holds, so a lost block 4 discards the whole group
    // rather than attaching its payload to the previous network.
    if (!group.blocks[3].valid) {
        return;
    }

    EonEntry* entry = eon_for(group.blocks[3].value);
    if (entry == nullptr) {
        return;
    }

    const std::uint16_t b2 = group.blocks[1].value;
    entry->tp = ((b2 >> 4) & 0x01) != 0;

    if (!group.blocks[2].valid) {
        return;
    }
    const std::uint16_t b3 = group.blocks[2].value;
    const std::uint8_t variant = static_cast<std::uint8_t>(b2 & 0x0F);

    // EN 50067 clause 3.1.5.19 Figure 37. Variants 10 and 11 are unallocated
    // and 15 is reserved for broadcasters' own use.
    switch (variant) {
        case 0:
        case 1:
        case 2:
        case 3: {
            const std::size_t pair = variant;
            entry->ps[pair * 2] = high_char(b3);
            entry->ps[pair * 2 + 1] = low_char(b3);
            entry->ps_received = static_cast<std::uint8_t>(entry->ps_received | (1u << pair));
            break;
        }
        case 4:
            add_af(entry->af, af_vhf_frequency(static_cast<std::uint8_t>(b3 >> 8)));
            add_af(entry->af, af_vhf_frequency(static_cast<std::uint8_t>(b3 & 0xFF)));
            break;
        case 5:
        case 6:
        case 7:
        case 8:
            // A mapped frequency pair: the tuning frequency on this network
            // and the corresponding frequency on the other one. Both are VHF
            // and both are alternatives worth keeping.
            add_af(entry->af, af_vhf_frequency(static_cast<std::uint8_t>(b3 >> 8)));
            add_af(entry->af, af_vhf_frequency(static_cast<std::uint8_t>(b3 & 0xFF)));
            break;
        case 9:
            // A mapped AM frequency: VHF on this network, LF or MF on the
            // other, which is the one place the region-dependent table reaches
            // into EON.
            add_af(entry->af, af_vhf_frequency(static_cast<std::uint8_t>(b3 >> 8)));
            add_af(entry->af, af_lf_mf_frequency(options_.region,
                                                 static_cast<std::uint8_t>(b3 & 0xFF)));
            break;
        case 12:
            entry->linkage = b3;
            entry->linkage_valid = true;
            break;
        case 13:
            entry->pty = static_cast<std::uint8_t>((b3 >> 11) & 0x1F);
            entry->pty_valid = true;
            entry->ta = (b3 & 0x01) != 0;
            entry->ta_valid = true;
            break;
        case 14: {
            const ProgrammeItemNumber pin = decode_pin(b3);
            if (pin.valid) {
                entry->pin = pin;
            }
            break;
        }
        default:
            break;
    }
}

void RdsDecoder::apply_type14b(const Group& group) {
    // EN 50067 Figure 38. The fast TA-change signal: sent only when the
    // referenced service's TA flag changes, as four to eight groups inside two
    // seconds, which is why it exists alongside variant 13 of 14A.
    if (!group.blocks[3].valid) {
        return;
    }

    if (group.blocks[2].valid) {
        // Block 3 is PI(TN), the tuned network's own PI. Worth taking when
        // block 1 was lost, and identical to it otherwise.
        state_.pi = group.blocks[2].value;
        state_.pi_valid = true;
    }

    EonEntry* entry = eon_for(group.blocks[3].value);
    if (entry == nullptr) {
        return;
    }

    const std::uint16_t b2 = group.blocks[1].value;
    entry->tp = ((b2 >> 4) & 0x01) != 0;
    entry->ta = ((b2 >> 3) & 0x01) != 0;
    entry->ta_valid = true;
}

void RdsDecoder::apply_type15b(const Group& group) {
    // EN 50067 clause 3.1.5.21 Figure 39. The whole block 2 switching payload
    // appears twice in one group, in blocks 2 and 4, with PI in blocks 1 and
    // 3. It carries no AF and no PS and supplements type 0 groups rather than
    // replacing them, which is why it writes the same fields and nothing else.
    apply_switching_payload(group.blocks[1].value);
    if (group.blocks[3].valid) {
        apply_switching_payload(group.blocks[3].value);
    }
}

void RdsDecoder::apply_af_pair(std::uint16_t pair) {
    const std::array<std::uint8_t, 2> codes = {
        static_cast<std::uint8_t>(pair >> 8),
        static_cast<std::uint8_t>(pair & 0xFF),
    };

    for (std::size_t i = 0; i < codes.size(); ++i) {
        const std::uint8_t code = codes[i];

        if (af_pending_lf_mf_) {
            af_pending_lf_mf_ = false;
            add_af(state_.af, af_lf_mf_frequency(options_.region, code));
            continue;
        }

        if (code == kAfCodeLfMfFollows) {
            af_pending_lf_mf_ = true;
            continue;
        }

        if (code > kAfCodeCountBase && code <= 249) {
            state_.af_announced = static_cast<std::uint8_t>(code - kAfCodeCountBase);
            continue;
        }

        // 0 not to be used, 205 filler, 206 to 223 not assigned, 224 no AF
        // exists, 251 to 255 not assigned. All of them carry no frequency.
        const dsp::Hertz hz = af_vhf_frequency(code);
        if (hz == 0) {
            continue;
        }

        if (i == 0) {
            // EN 50067 clause 3.2.1.6.5: method B is deduced from the frequent
            // repetition of the tuning frequency as the first of a pair. This
            // counts that repetition and reports it, and does not act on it,
            // because acting on it needs the frequency the radio is tuned to
            // and this decoder is never told what that is. A method A list
            // that has wrapped raises the count as well; see the comment on
            // StationState::af_repeats for what the number can and cannot be
            // read to mean.
            if (af_first_of_pair_ == hz) {
                ++state_.af_repeats;
            } else if (af_first_of_pair_ == 0) {
                af_first_of_pair_ = hz;
            }
        }

        add_af(state_.af, hz);
    }
}

void RdsDecoder::add_af(std::vector<dsp::Hertz>& list, dsp::Hertz hz) {
    if (hz == 0) {
        return;
    }
    // Method B lists run to a count code plus the tuning frequency plus up to
    // twelve pairs per list and there may be several lists, so the bound is
    // generous rather than tight. It exists so a stuck or hostile transmitter
    // cannot make the decoder allocate without limit.
    constexpr std::size_t kMaxAlternatives = 128;
    if (std::find(list.begin(), list.end(), hz) != list.end()) {
        return;
    }
    if (list.size() >= kMaxAlternatives) {
        return;
    }
    list.push_back(hz);
}

EonEntry* RdsDecoder::eon_for(std::uint16_t pi) {
    for (EonEntry& entry : state_.eon) {
        if (entry.pi == pi) {
            return &entry;
        }
    }
    if (state_.eon.size() >= options_.max_eon_entries) {
        return nullptr;
    }
    EonEntry entry;
    entry.pi = pi;
    entry.ps.fill(' ');
    state_.eon.push_back(std::move(entry));
    return &state_.eon.back();
}

}  // namespace revenant::decode
