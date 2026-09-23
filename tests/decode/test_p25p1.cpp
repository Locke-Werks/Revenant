// P25 Phase 1: the codes against the document, and the demodulator against the
// transmitter.
//
// WHAT THE CASES HERE ARE FOR, IN TWO GROUPS
//
// The first group checks the codes against TIA-102.BAAA-A's own printed
// tables. Those cases are the reason core/decode/dv_codes.h carries a
// generator polynomial rather than a copied matrix: the polynomial is the
// rule, the matrix is sixteen instances of it, and a case that regenerates the
// matrix from the polynomial establishes that the rule in the code is the rule
// in the document. Retyping the matrix would establish only that it was
// retyped correctly.
//
// The second group is the round trip. It is what stands in place of the
// project's bit-exact-against-a-scalar-twin rule, which does not reach a
// decoder with no GPU kernel behind it. tests/decode/CMakeLists.txt states
// that reasoning for RDS and it applies here unchanged.
//
// EVERY ERROR RATE HERE IS MEASURED AND REPORTED, NEVER ASSERTED TIGHT
//
// The thresholds are loose on purpose. A bit error rate asserted to three
// decimal places is a test that fails when somebody improves the receiver, and
// the number that matters is in the INFO line either way. What the assertions
// hold is the shape: clean at high signal to noise, degraded and still
// framing at low, and the frame metadata recovered exactly in the first case.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <ios>
#include <print>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "core/decode/dv_codes.h"
#include "core/decode/dv_phy.h"
#include "core/decode/p25p1.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dv_mod.h"

using namespace revenant;
using decode::P25Duid;

namespace {

constexpr dsp::SampleRate kRate = 48'000;

// TIA-102.BAAA-A clause 8.5.2 prints the generator matrix for the full 64-bit
// code in octal: sixteen rows, each a 16-bit identity part and a 48-bit parity
// part. Transcribed here as the document's own strings, because the whole
// point of the case below is to compare the implementation against them.
struct MatrixRow {
    const char* identity;
    const char* parity;
};

constexpr MatrixRow kClause852Matrix[16] = {
    {"100000", "6331141367235452"}, {"040000", "5265521614723276"},
    {"020000", "4603711461164164"}, {"010000", "2301744630472072"},
    {"004000", "7271623073000466"}, {"002000", "5605650752635660"},
    {"001000", "2702724365316730"}, {"000400", "1341352172547354"},
    {"000200", "0560565075263566"}, {"000100", "6141333751704220"},
    {"000040", "3060555764742110"}, {"000020", "1430266772361044"},
    {"000010", "0614133375170422"}, {"000004", "6037114611641642"},
    {"000002", "5326507063515373"}, {"000001", "4662302756473127"},
};

std::uint64_t from_octal(const char* digits) {
    std::uint64_t value = 0;
    for (const char* p = digits; *p != '\0'; ++p) {
        value = (value << 3U) | static_cast<std::uint64_t>(*p - '0');
    }
    return value;
}

// The 64 bits the encoder produces, as one integer, most significant first.
std::uint64_t encoded_nid(std::uint16_t nac, std::uint8_t duid) {
    std::array<std::uint8_t, 64> bits{};
    REQUIRE(decode::p25_nid_encode(nac, duid, bits).has_value());
    std::uint64_t value = 0;
    for (const std::uint8_t bit : bits) {
        value = (value << 1U) | bit;
    }
    return value;
}

decode::P25Header sample_header(bool encrypted) {
    decode::P25Header header;
    for (std::size_t i = 0; i < header.message_indicator.size(); ++i) {
        header.message_indicator[i] =
            encrypted ? static_cast<std::uint8_t>(0x11 * (i + 1)) : std::uint8_t{0};
    }
    header.manufacturer_id = 0x00;
    header.algorithm_id = encrypted ? std::uint8_t{0x84} : decode::kP25AlgidUnencrypted;
    header.key_id = encrypted ? std::uint16_t{0x1234} : std::uint16_t{0};
    header.talkgroup_id = 0x02A7;
    return header;
}

}  // namespace

// ---------------------------------------------------------------------------
// The codes, against the document
// ---------------------------------------------------------------------------

TEST_CASE("the NID generator polynomial regenerates the clause 8.5.2 matrix",
          "[decode][p25]") {
    // Clause 8.5.2 states the polynomial's shape in prose before printing it:
    // "of 47-th degree with 27 non-zero terms". Both are checkable.
    std::uint64_t terms = 0;
    for (unsigned bit = 0; bit < 64U; ++bit) {
        terms += (decode::kP25NidGenerator >> bit) & 1ULL;
    }
    CHECK(terms == 27);
    CHECK((decode::kP25NidGenerator >> 47U) == 1ULL);

    for (std::size_t row = 0; row < 16; ++row) {
        INFO("row " << (row + 1) << " of the clause 8.5.2 generator matrix");

        const auto identity = static_cast<std::uint16_t>(from_octal(kClause852Matrix[row].identity));
        const std::uint64_t parity48 = from_octal(kClause852Matrix[row].parity);

        // The identity part must be a single bit, in row order from the most
        // significant. If this fails the transcription is wrong rather than
        // the code.
        REQUIRE(identity == static_cast<std::uint16_t>(1U << (15U - row)));

        // The document's 48 parity columns are the code's 47 BCH parity bits
        // followed by the appended 64th bit.
        const std::uint64_t word = encoded_nid(static_cast<std::uint16_t>(identity >> 4U),
                                               static_cast<std::uint8_t>(identity & 0x0FU));
        const std::uint64_t got_information = word >> 48U;
        const std::uint64_t got_parity48 = word & 0xFFFF'FFFF'FFFFULL;

        CHECK(got_information == identity);
        CHECK(got_parity48 == parity48);
    }
}

TEST_CASE("the 64th NID bit reproduces every P value in Table 8-4", "[decode][p25]") {
    // Clause 8.5 says a single parity bit is appended and does not say what of.
    // Table 8-4 prints its value for each of the six defined Data Unit IDs,
    // which is what core/decode/dv_codes.cpp's rule was derived to satisfy.
    struct Row {
        std::uint8_t duid;
        std::uint8_t p;
        const char* usage;
    };
    constexpr Row kTable84[] = {
        {0b0000, 0, "Header Data Unit"},
        {0b0011, 0, "Terminator without subsequent Link Control"},
        {0b0101, 1, "Logical Link Data Unit 1"},
        {0b1010, 1, "Logical Link Data Unit 2"},
        {0b1100, 0, "Packet Data Unit"},
        {0b1111, 0, "Terminator with subsequent Link Control"},
    };

    for (const Row& row : kTable84) {
        INFO(row.usage);
        // Table 8-4's P column is printed against the DUID alone, so the NAC
        // is zero for the comparison to mean what the table means.
        const auto information = static_cast<std::uint16_t>(row.duid);
        CHECK(decode::p25_nid_trailing_parity(information) == row.p);
        CHECK((encoded_nid(0, row.duid) & 1ULL) == row.p);
    }
}

TEST_CASE("the NID code corrects up to eleven errors and is a distance 23 code",
          "[decode][p25]") {
    // Clause 8.5 names it a (63,16,23) BCH code, so the minimum distance of
    // the 64-bit word is at least 23 and up to 11 errors are correctable.
    // Measuring the true minimum over all 65535 non-zero code words is the
    // only way to say that about the implementation rather than about the
    // clause.
    std::uint32_t minimum = 64;
    for (std::uint32_t information = 1; information < 0x1'0000U; ++information) {
        const std::uint64_t word = encoded_nid(
            static_cast<std::uint16_t>(information >> 4U),
            static_cast<std::uint8_t>(information & 0x0FU));
        std::uint32_t weight = 0;
        for (unsigned bit = 0; bit < 64U; ++bit) {
            weight += static_cast<std::uint32_t>((word >> bit) & 1ULL);
        }
        minimum = std::min(minimum, weight);
    }
    INFO("minimum weight measured over all 65535 non-zero code words: " << minimum);
    CHECK(minimum == 23);

    // Eleven errors, placed at the eleven positions a decoder finds hardest to
    // tell from another code word: the low bits of the parity.
    std::array<std::uint8_t, 64> bits{};
    REQUIRE(decode::p25_nid_encode(0x293, static_cast<std::uint8_t>(P25Duid::LogicalLinkDataUnit1),
                                   bits)
                .has_value());
    for (std::size_t i = 0; i < 11; ++i) {
        bits[53 + i] ^= 1U;
    }
    auto decoded = decode::p25_nid_decode(bits);
    REQUIRE(decoded.has_value());
    CHECK(decoded->nac == 0x293);
    CHECK(decoded->duid == static_cast<std::uint8_t>(P25Duid::LogicalLinkDataUnit1));
    CHECK(decoded->corrected_bits == 11);
}

TEST_CASE("the shortened Golay code is systematic and has distance 8", "[decode][p25]") {
    // Clause 5.7's Table 5-3 says the left six columns of the (18,6,8) matrix
    // are the identity, so the top six bits of a code word are the
    // information.
    for (std::uint8_t information = 0; information < 64U; ++information) {
        const std::uint32_t word = decode::p25_golay18_encode(information);
        CHECK(((word >> 12U) & 0x3FU) == information);
    }

    std::uint32_t minimum = 18;
    for (std::uint8_t information = 1; information < 64U; ++information) {
        std::uint32_t weight = 0;
        const std::uint32_t word = decode::p25_golay18_encode(information);
        for (unsigned bit = 0; bit < 18U; ++bit) {
            weight += (word >> bit) & 1U;
        }
        minimum = std::min(minimum, weight);
    }
    INFO("minimum weight over the 63 non-zero code words: " << minimum);
    CHECK(minimum == 8);

    // Three errors are corrected, which is what distance 8 guarantees.
    for (std::uint8_t information = 0; information < 64U; ++information) {
        const std::uint32_t word = decode::p25_golay18_encode(information);
        const std::uint32_t damaged = word ^ 0b000'000'000'000'000'111U;
        const decode::Golay18Decode decoded = decode::p25_golay18_decode(damaged);
        CHECK(decoded.information == information);
        CHECK(decoded.corrected_bits == 3);
    }
}

// ---------------------------------------------------------------------------
// The voice code words, against the document
// ---------------------------------------------------------------------------

TEST_CASE("GF(2^6) reproduces the clause 5.9 exponential and logarithm tables",
          "[decode][p25][rs]") {
    // Clause 5.9 prints both tables in octal, eight to a row. Transcribed as
    // printed; the field arithmetic builds its own from alpha^6 + alpha + 1.
    constexpr std::uint8_t kExponential[64] = {
        001, 002, 004, 010, 020, 040, 003, 006, 014, 030, 060, 043, 005, 012, 024, 050,
        023, 046, 017, 036, 074, 073, 065, 051, 021, 042, 007, 016, 034, 070, 063, 045,
        011, 022, 044, 013, 026, 054, 033, 066, 057, 035, 072, 067, 055, 031, 062, 047,
        015, 032, 064, 053, 025, 052, 027, 056, 037, 076, 077, 075, 071, 061, 041, 001,
    };
    // Index is the field element; the table prints "-" for zero.
    constexpr int kLogarithm[64] = {
        -1, 0,  1,  6,  2,  12, 7,  26, 3,  32, 13, 35, 8,  48, 27, 18,
        4,  24, 33, 16, 14, 52, 36, 54, 9,  45, 49, 38, 28, 41, 19, 56,
        5,  62, 25, 11, 34, 31, 17, 47, 15, 23, 53, 51, 37, 44, 55, 40,
        10, 61, 46, 30, 50, 22, 39, 43, 29, 60, 42, 21, 20, 59, 57, 58,
    };
    for (unsigned e = 0; e < 64U; ++e) {
        INFO("alpha^" << e);
        CHECK(decode::p25_gf64_exp(e) == kExponential[e]);
    }
    for (unsigned b = 0; b < 64U; ++b) {
        INFO("log of octal " << std::oct << b);
        CHECK(decode::p25_gf64_log(static_cast<std::uint8_t>(b)) == kLogarithm[b]);
    }
}

namespace {

// Octal hexbits separated by spaces, as clause 5.9 prints them.
std::vector<std::uint8_t> from_octal_hexbits(const char* text) {
    std::vector<std::uint8_t> out;
    unsigned value = 0;
    bool in_number = false;
    for (const char* p = text;; ++p) {
        if (*p >= '0' && *p <= '7') {
            value = value * 8U + static_cast<unsigned>(*p - '0');
            in_number = true;
        } else {
            if (in_number) {
                out.push_back(static_cast<std::uint8_t>(value));
            }
            value = 0;
            in_number = false;
            if (*p == '\0') {
                break;
            }
        }
    }
    return out;
}

// Clause 5.9's generator matrices, the parity columns only; the identity
// columns are the identity, which the case checks by construction. Extracted
// from the PDF's text layer by script rather than retyped, like the IMBE
// annexes in core/decode/imbe.cpp.
constexpr const char* kGlcParity[12] = {
    "62 44 03 25 14 16 27 03 53 04 36 47", "11 12 11 11 16 64 67 55 01 76 26 73",
    "03 01 05 75 14 06 20 44 66 06 70 66", "21 70 27 45 16 67 23 64 73 33 44 21",
    "30 22 03 75 15 15 33 15 51 03 53 50", "01 41 27 56 76 64 21 53 04 25 01 12",
    "61 76 21 55 76 01 63 35 30 13 64 70", "24 22 71 56 21 35 73 42 57 74 43 76",
    "72 42 05 20 43 47 33 56 01 16 13 76", "72 14 65 54 35 25 41 16 15 40 71 26",
    "73 65 36 61 42 22 17 04 44 20 25 05", "71 05 55 03 71 34 60 11 74 02 41 50",
};
constexpr const char* kGesParity[16] = {
    "51 45 67 15 64 67 52 12", "57 25 63 73 71 22 40 15", "05 01 31 04 16 54 25 76",
    "73 07 47 14 41 77 47 11", "75 15 51 51 17 67 17 57", "20 32 14 42 75 42 70 54",
    "02 75 43 05 01 40 12 64", "24 74 15 72 24 26 74 61", "42 64 07 22 61 20 40 65",
    "32 32 55 41 57 66 21 77", "65 36 25 07 50 16 40 51", "64 06 54 32 76 46 14 36",
    "62 63 74 70 05 27 37 46", "55 43 34 71 57 76 50 64", "24 23 23 05 50 70 42 23",
    "67 75 45 60 57 24 06 26",
};
constexpr const char* kPhdrParity[20] = {
    "74 37 34 06 02 07 44 64 26 14 26 44 54 13 77 05",
    "04 17 50 24 11 05 30 57 33 03 02 02 15 16 25 26",
    "07 23 37 46 56 75 43 45 55 21 50 31 45 27 71 62",
    "26 05 07 63 63 27 63 40 06 04 40 45 47 30 75 07",
    "23 73 73 41 72 34 21 51 67 16 31 74 11 21 12 21",
    "24 51 25 23 22 41 74 66 74 65 70 36 67 45 64 01",
    "52 33 14 02 20 06 14 25 52 23 35 74 75 75 43 27",
    "55 62 56 25 73 60 15 30 13 17 20 02 70 55 14 47",
    "54 51 32 65 77 12 54 13 35 32 56 12 75 01 72 63",
    "74 41 30 41 43 22 51 06 64 33 03 47 27 12 55 47",
    "54 70 11 03 13 22 16 57 03 45 72 31 30 56 35 22",
    "51 07 72 30 65 54 06 21 36 63 50 61 64 52 01 60",
    "01 65 32 70 13 44 73 24 12 52 21 55 12 35 14 72",
    "11 70 05 10 65 24 15 77 22 24 24 74 07 44 07 46",
    "06 02 65 11 41 20 45 42 46 54 35 12 40 64 65 33",
    "34 31 01 15 44 64 16 24 52 16 06 62 20 13 55 57",
    "63 43 25 44 77 63 17 17 64 14 40 74 31 72 54 06",
    "71 21 70 44 56 04 30 74 04 23 71 70 63 45 56 43",
    "02 01 53 74 02 14 52 74 12 57 24 63 15 42 52 33",
    "34 35 02 23 21 27 22 33 64 42 05 73 51 46 73 60",
};

struct PrintedCode {
    const char* name;
    decode::P25ReedSolomon code;
    std::span<const char* const> parity;
    // Clause 5.9's printed generator polynomial, lowest degree first.
    const char* generator;
};

std::array<PrintedCode, 3> printed_codes() {
    return {{
        {"(36,20,17) header", decode::kP25RsHeader, kPhdrParity,
         "60 73 46 51 73 05 42 64 33 22 27 21 23 02 35 34 01"},
        {"(24,12,13) Link Control", decode::kP25RsLinkControl, kGlcParity,
         "50 41 02 74 11 60 34 71 03 55 05 71 01"},
        {"(24,16,9) encryption sync", decode::kP25RsEncryptionSync, kGesParity,
         "26 06 24 57 60 45 75 67 01"},
    }};
}

}  // namespace

TEST_CASE("the Reed-Solomon generators are the polynomials clause 5.9 prints",
          "[decode][p25][rs]") {
    for (const PrintedCode& printed : printed_codes()) {
        INFO(printed.name);
        CHECK(decode::p25_rs_generator(printed.code) == from_octal_hexbits(printed.generator));
    }
}

TEST_CASE("the Reed-Solomon encoders regenerate the clause 5.9 generator matrices",
          "[decode][p25][rs]") {
    // Each matrix row is the code word for a single information hexbit of
    // value 1, so a row is the document's own example of a code word, and
    // the decoder must accept every one of them untouched.
    for (const PrintedCode& printed : printed_codes()) {
        REQUIRE(printed.parity.size() == printed.code.k);
        for (std::size_t row = 0; row < printed.code.k; ++row) {
            INFO(printed.name << ", row " << (row + 1));
            std::vector<std::uint8_t> information(printed.code.k, 0);
            information[row] = 1;
            auto word = decode::p25_rs_encode(printed.code, information);
            REQUIRE(word.has_value());

            const std::vector<std::uint8_t> parity(word->begin() +
                                                       static_cast<std::ptrdiff_t>(printed.code.k),
                                                   word->end());
            CHECK(parity == from_octal_hexbits(printed.parity[row]));
            CHECK(std::equal(information.begin(), information.end(), word->begin()));

            auto decoded = decode::p25_rs_decode(printed.code, *word);
            REQUIRE(decoded.has_value());
            CHECK(decoded->decoded);
            CHECK(decoded->corrected == 0);
            CHECK(decoded->codeword == *word);
        }
    }
}

TEST_CASE("the Reed-Solomon decoders correct everything within their distance",
          "[decode][p25][rs]") {
    // Exhaustive where it is cheap enough to be: every single hexbit error at
    // every position with every non-zero value, and every pair of positions
    // for double errors. Above that the patterns are drawn from a seeded
    // generator, including every split of the redundancy between errors and
    // erasures that 2v + f <= n - k allows.
    constexpr std::uint64_t kSeed = 0x25'5EED'0005'2025ULL;
    std::println("test_p25p1 Reed-Solomon sweep seed {}", kSeed);
    std::mt19937_64 rng(kSeed);

    for (const PrintedCode& printed : printed_codes()) {
        const decode::P25ReedSolomon& code = printed.code;
        const std::size_t parity = code.n - code.k;
        INFO(printed.name);

        std::vector<std::uint8_t> information(code.k, 0);
        for (std::uint8_t& hexbit : information) {
            hexbit = static_cast<std::uint8_t>(rng() & 0x3FU);
        }
        auto sent = decode::p25_rs_encode(code, information);
        REQUIRE(sent.has_value());

        std::size_t wrong = 0;
        std::size_t tried = 0;
        const auto attempt = [&](const std::vector<std::uint8_t>& received,
                                 const std::vector<std::size_t>& erasures) {
            auto decoded = decode::p25_rs_decode(code, received, erasures);
            REQUIRE(decoded.has_value());
            ++tried;
            if (!decoded->decoded || decoded->codeword != *sent) {
                ++wrong;
            }
        };

        for (std::size_t position = 0; position < code.n; ++position) {
            for (std::uint8_t value = 1; value < 64U; ++value) {
                std::vector<std::uint8_t> received = *sent;
                received[position] ^= value;
                attempt(received, {});
            }
        }
        for (std::size_t a = 0; a < code.n; ++a) {
            for (std::size_t b = a + 1; b < code.n; ++b) {
                for (int draw = 0; draw < 4; ++draw) {
                    std::vector<std::uint8_t> received = *sent;
                    received[a] ^= static_cast<std::uint8_t>(1U + rng() % 63U);
                    received[b] ^= static_cast<std::uint8_t>(1U + rng() % 63U);
                    attempt(received, {});
                }
            }
        }
        for (std::size_t erased = 0; erased <= parity; ++erased) {
            const std::size_t errors = (parity - erased) / 2;
            for (int draw = 0; draw < 300; ++draw) {
                std::vector<std::size_t> positions(code.n);
                for (std::size_t i = 0; i < code.n; ++i) {
                    positions[i] = i;
                }
                std::shuffle(positions.begin(), positions.end(), rng);
                std::vector<std::uint8_t> received = *sent;
                std::vector<std::size_t> erasures(positions.begin(),
                                                  positions.begin() +
                                                      static_cast<std::ptrdiff_t>(erased));
                // An erased hexbit may or may not be wrong; the decoder must
                // not care which.
                for (const std::size_t position : erasures) {
                    received[position] ^= static_cast<std::uint8_t>(rng() % 64U);
                }
                for (std::size_t i = erased; i < erased + errors; ++i) {
                    received[positions[i]] ^= static_cast<std::uint8_t>(1U + rng() % 63U);
                }
                attempt(received, erasures);
            }
        }
        INFO("decoded wrongly or not at all: " << wrong << " of " << tried);
        CHECK(wrong == 0);

        // One error past the guarantee. The code cannot correct it, and what
        // matters is that it says so rather than handing back a different
        // code word as though it were the one sent.
        std::size_t refused = 0;
        std::size_t miscorrected = 0;
        constexpr int kBeyond = 2000;
        for (int draw = 0; draw < kBeyond; ++draw) {
            std::vector<std::size_t> positions(code.n);
            for (std::size_t i = 0; i < code.n; ++i) {
                positions[i] = i;
            }
            std::shuffle(positions.begin(), positions.end(), rng);
            std::vector<std::uint8_t> received = *sent;
            for (std::size_t i = 0; i < parity / 2 + 1; ++i) {
                received[positions[i]] ^= static_cast<std::uint8_t>(1U + rng() % 63U);
            }
            auto decoded = decode::p25_rs_decode(code, received);
            REQUIRE(decoded.has_value());
            refused += decoded->decoded ? 0U : 1U;
            miscorrected += (decoded->decoded && decoded->codeword != *sent) ? 1U : 0U;
        }
        std::println("test_p25p1 {}: {} errors, refused {}, miscorrected {} of {}", printed.name,
                     parity / 2 + 1, refused, miscorrected, kBeyond);
        CHECK(miscorrected * 100 <= static_cast<std::size_t>(kBeyond));
    }
}

TEST_CASE("the shortened Hamming code is Table 5-4 and flags what it cannot correct",
          "[decode][p25]") {
    // Table 5-4 prints the (15,11,3) code beside the (10,6,3) one and clause
    // 5.8 says the short code is cut from it. Every short row's parity nibble
    // must therefore be one of the long code's, which is a check on the
    // transcription against the other column of the same table.
    constexpr std::uint8_t kStandardParity[11] = {
        0b1111, 0b1110, 0b1101, 0b1100, 0b1011, 0b1010, 0b1001, 0b0111, 0b0110, 0b0101, 0b0011,
    };
    for (const std::uint16_t row : decode::kP25Hamming10Rows) {
        const auto nibble = static_cast<std::uint8_t>(row & 0xFU);
        CHECK(std::find(std::begin(kStandardParity), std::end(kStandardParity), nibble) !=
              std::end(kStandardParity));
    }

    std::uint32_t minimum = 10;
    for (std::uint8_t hexbit = 0; hexbit < 64U; ++hexbit) {
        const std::uint16_t word = decode::p25_hamming10_encode(hexbit);
        CHECK(((word >> 4U) & 0x3FU) == hexbit);
        if (hexbit != 0) {
            minimum = std::min(minimum, static_cast<std::uint32_t>(std::popcount(word)));
        }
        for (unsigned bit = 0; bit < 10U; ++bit) {
            const auto decoded =
                decode::p25_hamming10_decode(static_cast<std::uint16_t>(word ^ (1U << bit)));
            CHECK(decoded.information == hexbit);
            CHECK(decoded.distance == 1);
            CHECK_FALSE(decoded.detected);
        }
    }
    CHECK(minimum == 3);

    // Clause 5.8 gives error detection as the reason the rows were chosen the
    // way they were. Measured over every code word and every double error:
    // the share flagged rather than silently miscorrected.
    std::uint32_t flagged = 0;
    std::uint32_t doubles = 0;
    for (std::uint8_t hexbit = 0; hexbit < 64U; ++hexbit) {
        const std::uint16_t word = decode::p25_hamming10_encode(hexbit);
        for (unsigned a = 0; a < 10U; ++a) {
            for (unsigned b = a + 1; b < 10U; ++b) {
                const auto decoded = decode::p25_hamming10_decode(
                    static_cast<std::uint16_t>(word ^ (1U << a) ^ (1U << b)));
                ++doubles;
                flagged += decoded.detected ? 1U : 0U;
            }
        }
    }
    std::println("test_p25p1 (10,6,3) Hamming: double errors flagged {} of {}", flagged, doubles);
    CHECK(flagged > 0);
}

TEST_CASE("the low speed data code regenerates Table 5-2 and the clause 5.6 example",
          "[decode][p25]") {
    // Table 5-2, the parity half of each row, row 1 the most significant bit.
    constexpr std::uint8_t kTable52Parity[8] = {
        0b0100'1110, 0b0010'0111, 0b1000'1111, 0b1101'1011,
        0b1111'0001, 0b1110'0100, 0b0111'0010, 0b0011'1001,
    };
    for (std::size_t row = 0; row < 8; ++row) {
        INFO("row " << (row + 1));
        const std::uint16_t word = decode::p25_lsd_encode(static_cast<std::uint8_t>(0x80U >> row));
        CHECK(static_cast<unsigned>(word >> 8U) == (0x80U >> row));
        CHECK((word & 0xFFU) == static_cast<unsigned>(kTable52Parity[row]));
    }

    // Clause 5.6: '"A" = $41 encodes to $41 1e'.
    CHECK(decode::p25_lsd_encode(0x41) == 0x411E);

    std::uint32_t minimum = 16;
    for (unsigned octet = 1; octet < 256U; ++octet) {
        minimum = std::min(minimum, static_cast<std::uint32_t>(std::popcount(
                                        decode::p25_lsd_encode(static_cast<std::uint8_t>(octet)))));
    }
    CHECK(minimum == 5);

    // d = 5 corrects two.
    const std::uint16_t damaged = static_cast<std::uint16_t>(0x411E ^ 0x8001U);
    const decode::LsdDecode decoded = decode::p25_lsd_decode(damaged);
    CHECK(decoded.octet == 0x41);
    CHECK(decoded.distance == 2);
}

TEST_CASE("the LDU layout puts every field where the clause 10.3 and 10.4 annexes do",
          "[decode][p25]") {
    // The annexes number symbols absolutely, status symbols included. These
    // are the symbols they label "IMBE 1" through "IMBE 9", the first symbol
    // of each block of four Hamming words, and the first low speed data
    // symbol, read off the LDU1 annex; the LDU2 annex has the same numbers
    // 864 higher.
    constexpr std::size_t kImbeStarts[9] = {57, 131, 226, 320, 415, 510, 604, 699, 789};
    constexpr std::size_t kHammingBlockStarts[6] = {205, 300, 394, 489, 584, 678};
    constexpr std::size_t kLowSpeedDataStart = 773;

    // An absolute symbol number to its index among the information symbols.
    const auto information_index = [](std::size_t absolute) {
        std::size_t status = 0;
        for (std::size_t s = decode::kP25FirstStatusSymbol; s < absolute;
             s += decode::kP25StatusSymbolInterval) {
            ++status;
        }
        return absolute - status;
    };

    constexpr decode::P25LduLayout layout = decode::p25_ldu_layout();
    for (std::size_t frame = 0; frame < 9; ++frame) {
        INFO("IMBE " << (frame + 1));
        CHECK(layout.voice[frame] == information_index(kImbeStarts[frame]));
    }
    for (std::size_t block = 0; block < 6; ++block) {
        INFO("Hamming block " << (block + 1));
        CHECK(layout.hamming[block * 4] == information_index(kHammingBlockStarts[block]));
        for (std::size_t w = 1; w < 4; ++w) {
            CHECK(layout.hamming[block * 4 + w] ==
                  layout.hamming[block * 4] + w * decode::kP25HammingWordSymbols);
        }
    }
    CHECK(layout.low_speed_data == information_index(kLowSpeedDataStart));

    // And the fields tile the unit: the last voice frame ends on the last
    // information symbol.
    CHECK(layout.voice[8] + decode::kP25VoiceFrameSymbols == decode::kP25LduInformationSymbols);
}

TEST_CASE("the frame sync pattern is Table 8-1 read through Table 9-1", "[decode][p25]") {
    // Table 8-1's expanded vector, written out as the document prints it.
    const std::string expected = "010101010111010111110101111111110111011111111111";
    REQUIRE(expected.size() == decode::kP25FrameSyncBits);

    std::uint64_t word = 0;
    for (const char c : expected) {
        word = (word << 1U) | static_cast<std::uint64_t>(c == '1' ? 1 : 0);
    }
    CHECK(word == decode::kP25FrameSync);

    // Table 9-1 maps dibit 01 to symbol +3 and 11 to -3, which are the only
    // two dibits the sync word uses, so every pattern entry is +3 or -3.
    const auto pattern = decode::p25_frame_sync_pattern();
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        INFO("symbol " << i);
        CHECK((pattern[i] == 3.0F || pattern[i] == -3.0F));
    }
    CHECK(pattern[0] == 3.0F);   // dibit 01
    CHECK(pattern[5] == -3.0F);  // dibit 11
}

TEST_CASE("status symbols come out every 36th symbol from the 35th", "[decode][p25]") {
    // Clause 8.4 and the clause 10.2 annex, which prints them at absolute
    // symbol positions 35, 71, 107 and so on through 395.
    std::vector<float> symbols(decode::kP25HduTotalSymbols, 0.0F);
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        symbols[i] = static_cast<float>(i);
    }
    const std::vector<float> stripped = decode::p25_strip_status_symbols(symbols);

    CHECK(stripped.size() == decode::kP25HduTotalSymbols - decode::kP25HduStatusSymbols);
    for (const float value : stripped) {
        const auto index = static_cast<std::size_t>(value);
        const bool was_status =
            index >= decode::kP25FirstStatusSymbol &&
            (index - decode::kP25FirstStatusSymbol) % decode::kP25StatusSymbolInterval == 0;
        CHECK_FALSE(was_status);
    }
}

// ---------------------------------------------------------------------------
// The round trip
// ---------------------------------------------------------------------------

TEST_CASE("a header data unit round trips through C4FM with no errors", "[decode][p25]") {
    siggen::P25HeaderMessage message;
    message.network_access_code = 0x293;
    message.header = sample_header(false);

    siggen::P25ModConfig mod;
    mod.rate = kRate;

    // Two copies of the message back to back. A receiver loses the tail of a
    // capture to its filter delay and to the timing estimator's window, so
    // the second copy is what makes the first one's terminator land inside
    // the part that decodes. A real transmission has whatever comes next
    // there; a test has to put something.
    auto dibits = siggen::p25_header_message_dibits(message);
    INFO((dibits.has_value() ? std::string{} : dibits.error().message));
    REQUIRE(dibits.has_value());
    std::vector<std::uint8_t> stream = *dibits;
    stream.insert(stream.end(), dibits->begin(), dibits->end());

    auto samples = siggen::p25_render_dibits(mod, stream);
    INFO((samples.has_value() ? std::string{} : samples.error().message));
    REQUIRE(samples.has_value());

    decode::P25Config config;
    config.rate = kRate;
    auto decoder = decode::P25Phase1::create(config);
    INFO((decoder.has_value() ? std::string{} : decoder.error().message));
    REQUIRE(decoder.has_value());

    std::vector<decode::P25Frame> frames;
    REQUIRE(decoder->process(*samples, frames).has_value());

    INFO("frames recovered: " << frames.size());
    REQUIRE_FALSE(frames.empty());

    const decode::P25Frame& hdu = frames.front();
    INFO("sync correlation " << hdu.sync_score << ", NID corrections "
                             << hdu.nid.corrected_bits);
    CHECK(hdu.nid.network_access_code == 0x293);
    CHECK(hdu.nid.duid == static_cast<std::uint8_t>(P25Duid::HeaderDataUnit));
    CHECK(hdu.nid.corrected_bits == 0);

    REQUIRE(hdu.header.has_value());
    CHECK(hdu.header->talkgroup_id == 0x02A7);
    CHECK(hdu.header->algorithm_id == decode::kP25AlgidUnencrypted);
    CHECK_FALSE(hdu.header->encrypted);

    // The terminator the transmitter appends, so the DUID sequence is what a
    // real short transmission looks like rather than one frame in isolation.
    REQUIRE(frames.size() >= 2);
    CHECK(frames[1].nid.duid ==
          static_cast<std::uint8_t>(P25Duid::TerminatorWithoutLinkControl));

    // THE FIRST HEADER IS NOT ERROR FREE AND THE SECOND ONE IS.
    //
    // The first data unit of a capture sits inside the receiver's own
    // transient: the transmit filter is ringing up, and the first window the
    // timing estimator averages over is part turn-on and part signal, so its
    // estimate is pulled off by a fraction of a symbol. That costs a handful
    // of Golay corrections in the header and nothing else, because the code
    // has distance 8 and is spending one of its three.
    //
    // It is a real property of any burst receiver and not a defect, so the
    // case measures it rather than hiding it, and holds the standard against
    // the second header, which is past the transient.
    INFO("first header: worst Golay correction " << hdu.header->worst_golay_correction
                                                 << " over "
                                                 << hdu.header->golay_words_corrected
                                                 << " of 36 words");
    CHECK(hdu.header->worst_golay_correction <= 3);

    REQUIRE(frames.size() >= 3);
    const decode::P25Frame& second = frames[2];
    CHECK(second.nid.duid == static_cast<std::uint8_t>(P25Duid::HeaderDataUnit));
    CHECK(second.nid.corrected_bits == 0);
    REQUIRE(second.header.has_value());
    INFO("second header: worst Golay correction " << second.header->worst_golay_correction);
    CHECK(second.header->worst_golay_correction == 0);
    CHECK(second.header->golay_words_corrected == 0);
    CHECK(second.header->talkgroup_id == 0x02A7);
}

TEST_CASE("a header split across two calls is held until it has arrived", "[decode][p25]") {
    // The round trip above feeds the whole capture in one call, which is the
    // one blocking in which a data unit can never straddle a call. The engine
    // hands a receiver's stream over one block at a time, and until
    // 2026-09-22 a Header Data Unit whose sync and NID were in one call and
    // whose body was in the next came back with no header and was never
    // tried again. The split below lands in the middle of the first header.
    siggen::P25HeaderMessage message;
    message.network_access_code = 0x293;
    message.header = sample_header(false);

    auto dibits = siggen::p25_header_message_dibits(message);
    REQUIRE(dibits.has_value());
    std::vector<std::uint8_t> stream = *dibits;
    stream.insert(stream.end(), dibits->begin(), dibits->end());

    siggen::P25ModConfig mod;
    mod.rate = kRate;
    auto samples = siggen::p25_render_dibits(mod, stream);
    REQUIRE(samples.has_value());

    decode::P25Config config;
    config.rate = kRate;
    auto decoder = decode::P25Phase1::create(config);
    REQUIRE(decoder.has_value());

    // Ten samples a symbol at 48000, so symbol 200 of the 396 in the header.
    const std::size_t split = 200 * static_cast<std::size_t>(kRate / 4'800);
    REQUIRE(split < samples->size());
    const std::span<const dsp::Complex32> all(*samples);

    std::vector<decode::P25Frame> frames;
    REQUIRE(decoder->process(all.first(split), frames).has_value());
    CHECK(frames.empty());
    REQUIRE(decoder->process(all.subspan(split), frames).has_value());

    INFO("frames recovered: " << frames.size());
    REQUIRE_FALSE(frames.empty());
    CHECK(frames.front().nid.duid == static_cast<std::uint8_t>(P25Duid::HeaderDataUnit));
    REQUIRE(frames.front().header.has_value());
    CHECK(frames.front().header->talkgroup_id == 0x02A7);

    // Reported once, not once without a header and again with one.
    std::size_t headers = 0;
    for (const decode::P25Frame& frame : frames) {
        headers += frame.nid.duid == static_cast<std::uint8_t>(P25Duid::HeaderDataUnit) ? 1U : 0U;
    }
    CHECK(headers == 2);
}

TEST_CASE("an encrypted header is identified and its payload is left alone", "[decode][p25]") {
    // docs/modes.md: where a frame is encrypted, say so on the status surface
    // and stop. The talkgroup and the network are in clear either way, which
    // is the whole reason framing an encrypted call is worth doing.
    siggen::P25HeaderMessage message;
    message.network_access_code = 0x4D8;
    message.header = sample_header(true);

    siggen::P25ModConfig mod;
    mod.rate = kRate;
    auto samples = siggen::p25_render_header_message(mod, message);
    REQUIRE(samples.has_value());

    decode::P25Config config;
    config.rate = kRate;
    auto decoder = decode::P25Phase1::create(config);
    REQUIRE(decoder.has_value());

    std::vector<decode::P25Frame> frames;
    REQUIRE(decoder->process(*samples, frames).has_value());
    REQUIRE_FALSE(frames.empty());
    REQUIRE(frames.front().header.has_value());

    const decode::P25Header& header = *frames.front().header;
    CHECK(header.encrypted);
    CHECK(header.algorithm_id == 0x84);  // AES, per TIA-102.BAAC clause 2.8
    CHECK(header.key_id == 0x1234);

    // Still reported, because the standard puts them outside the encryption.
    CHECK(frames.front().nid.network_access_code == 0x4D8);
    CHECK(header.talkgroup_id == 0x02A7);
}

TEST_CASE("an inverted discriminator still frames", "[decode][p25]") {
    // Whether a positive deviation comes out of the discriminator positive
    // depends on which sideband the tuner landed on, and no standard can fix
    // it. core/decode/p25p1.cpp corrects it from the sync correlation's sign.
    siggen::P25HeaderMessage message;
    message.network_access_code = 0x293;
    message.header = sample_header(false);

    siggen::P25ModConfig mod;
    mod.rate = kRate;
    auto samples = siggen::p25_render_header_message(mod, message);
    REQUIRE(samples.has_value());

    // Conjugating the baseband negates the instantaneous frequency, which is
    // exactly what a spectral inversion does.
    for (dsp::Complex32& sample : *samples) {
        sample = std::conj(sample);
    }

    decode::P25Config config;
    config.rate = kRate;
    auto decoder = decode::P25Phase1::create(config);
    REQUIRE(decoder.has_value());

    std::vector<decode::P25Frame> frames;
    REQUIRE(decoder->process(*samples, frames).has_value());
    REQUIRE_FALSE(frames.empty());
    CHECK(frames.front().inverted);
    CHECK(frames.front().nid.network_access_code == 0x293);
    REQUIRE(frames.front().header.has_value());
    CHECK(frames.front().header->talkgroup_id == 0x02A7);
}

TEST_CASE("P25 symbol error rate against noise, measured", "[decode][p25]") {
    // A long pseudorandom dibit stream through the modulator, the channel and
    // the demodulator, with the error rate measured rather than asserted to a
    // figure. The comparison is per dibit and is reported as both a symbol
    // error rate and the bit error rate a caller would see.
    constexpr std::size_t kSymbols = 6'000;
    std::vector<std::uint8_t> sent(kSymbols, 0);
    std::uint64_t state = 0x5EED'1234'ABCD'0001ULL;
    for (std::uint8_t& dibit : sent) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        dibit = static_cast<std::uint8_t>((state >> 33U) & 0x3U);
    }

    struct Point {
        double snr_db;
        double allowed_symbol_error_rate;
    };
    // Full-sample-rate signal to noise, per docs/snr-convention.md, which is
    // the basis that names its bandwidth rather than assuming one.
    // Measured on 2026-09-21: 0 symbol errors in 5911 at 30 dB, and a
    // symbol error rate of 0.047 at 4 dB. The allowances are set above the
    // measured figures rather than at them, because the number that matters
    // is in the INFO line and an assertion pinned to three decimals is one
    // that fails when somebody improves the receiver.
    const Point points[] = {{30.0, 0.001}, {4.0, 0.08}};

    for (const Point& point : points) {
        INFO("signal to noise " << point.snr_db << " dB across the whole "
                                << kRate << " Hz sample rate");

        siggen::P25ModConfig mod;
        mod.rate = kRate;
        auto samples = siggen::p25_render_dibits(mod, sent);
        REQUIRE(samples.has_value());

        const auto level = siggen::NoiseLevel::snr_in_full_sample_rate_db(point.snr_db);
        auto report = siggen::add_awgn(*samples, level, kRate, 0x9E37'79B9'7F4A'7C15ULL);
        INFO((report.has_value() ? std::string{} : report.error().message));
        REQUIRE(report.has_value());

        decode::P25Config config;
        config.rate = kRate;
        auto decoder = decode::P25Phase1::create(config);
        REQUIRE(decoder.has_value());

        std::vector<decode::P25Frame> frames;
        REQUIRE(decoder->process(*samples, frames).has_value());

        // The frame layer has nothing to lock onto in a bare symbol stream, so
        // the comparison is against the recovered soft symbols directly. The
        // timing loop needs a few tens of symbols to pull in, and the filters
        // have a group delay, so the alignment is found by correlation rather
        // than assumed.
        const std::span<const float> recovered = decoder->last_symbols();
        REQUIRE(recovered.size() > kSymbols / 2);

        std::vector<float> reference(sent.size(), 0.0F);
        for (std::size_t i = 0; i < sent.size(); ++i) {
            reference[i] = static_cast<float>(decode::kP25DibitToSymbol[sent[i]]);
        }

        // Correlate the first 64 transmitted symbols against the recovered run
        // to find where the stream starts.
        const std::span<const float> head(reference.data(), 128);
        auto hit = decode::correlate_pattern(recovered, head);
        REQUIRE(hit.has_value());
        INFO("stream found at recovered symbol " << hit->offset << ", correlation "
                                                 << hit->score);
        REQUIRE(std::abs(hit->score) > 0.5);

        const std::size_t compare =
            std::min(sent.size(), recovered.size() - hit->offset);
        REQUIRE(compare > kSymbols / 2);

        // Counted twice: over the whole run, and over the run after the
        // first two estimator windows.
        //
        // The split is the point rather than a way to get a nicer number.
        // The timing estimator averages over a window, so the first window
        // of a capture is part transmitter turn-on and part signal and its
        // estimate is pulled off by a fraction of a symbol. Every error at
        // 30 dB is in that window. Reporting one figure would either hide
        // the acquisition cost or attribute it to the channel, and both are
        // wrong about where the errors are.
        constexpr std::size_t kSettled = 64;
        std::size_t symbol_errors = 0;
        std::size_t bit_errors = 0;
        std::size_t settled_errors = 0;
        std::size_t settled_compared = 0;
        for (std::size_t i = 0; i < compare; ++i) {
            float value = recovered[hit->offset + i];
            if (hit->inverted) {
                value = -value;
            }
            std::uint8_t got = 0;
            if (value >= 2.0F) {
                got = 0b01;
            } else if (value >= 0.0F) {
                got = 0b00;
            } else if (value >= -2.0F) {
                got = 0b10;
            } else {
                got = 0b11;
            }
            const bool wrong = got != sent[i];
            if (wrong) {
                ++symbol_errors;
            }
            if (i >= kSettled) {
                ++settled_compared;
                settled_errors += wrong ? 1U : 0U;
            }
            bit_errors += static_cast<std::size_t>(((got >> 1U) & 1U) != ((sent[i] >> 1U) & 1U));
            bit_errors += static_cast<std::size_t>((got & 1U) != (sent[i] & 1U));
        }

        REQUIRE(settled_compared > 0);
        const double ser = static_cast<double>(symbol_errors) / static_cast<double>(compare);
        const double ber = static_cast<double>(bit_errors) / static_cast<double>(2 * compare);
        const double settled_ser =
            static_cast<double>(settled_errors) / static_cast<double>(settled_compared);
        INFO("compared " << compare << " symbols: symbol error rate " << ser
                         << ", bit error rate " << ber << "; after the first " << kSettled
                         << " symbols, " << settled_compared << " compared at a symbol "
                         << "error rate of " << settled_ser);
        CHECK(settled_ser <= point.allowed_symbol_error_rate);
    }
}
