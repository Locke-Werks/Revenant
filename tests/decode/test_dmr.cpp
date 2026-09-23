// DMR: the Annex B codes against the tables TS 102 361-1 prints, and the
// decoder against the transmitter.
//
// The same two groups as the other digital voice suites. The first checks
// each code against the document's own printed matrices and worked example,
// so the generator in core/decode/dmr_codes.h is shown to be the document's
// rule rather than a matrix somebody retyped. The second is round trips
// through core/dsp/synth/dmr_mod.h for every burst type the decoder reads, on
// a base station channel with its CACH, on a direct mode channel with the
// carrier off between bursts, and with both timeslots carrying different
// calls at once.
//
// Error rates are reported in INFO lines and the assertions around them are
// loose, for the reason tests/decode/CMakeLists.txt gives.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <print>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "core/decode/dmr.h"
#include "core/decode/dmr_codes.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dmr_mod.h"

using namespace revenant;
using decode::DmrDataType;
using decode::DmrSyncType;

namespace {

constexpr dsp::SampleRate kRate = 48'000;

// WHY THE PRINTED TABLES ARE FINGERPRINTS HERE AND NOT TRANSCRIPTIONS
//
// ETSI publishes TS 102 361 at no charge and keeps its copyright, and
// docs/modes.md's rule for such a document is that a clause number goes in a
// comment and a table does not go in the tree. So each table this suite
// checks against was read out of the PDF's text layer by script on
// 2026-09-23, compared row for row with what the code regenerates, and then
// reduced to the FNV-1a 64-bit hash of its text: every row's digits with no
// spaces, then a newline. The case below builds the same text from the
// implementation and compares hashes, which fails on any single digit and
// carries none of the table.
std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t hash = 0xCBF2'9CE4'8422'2325ULL;
    for (const char c : text) {
        hash = (hash ^ static_cast<std::uint8_t>(c)) * 0x0000'0100'0000'01B3ULL;
    }
    return hash;
}

// A code's generator matrix as that text: one row per information bit, most
// significant first, each row the n bits of its code word.
std::string generator_text(const decode::DmrBlockCode& code) {
    std::string out;
    for (std::size_t i = 0; i < code.k; ++i) {
        const std::uint32_t word = decode::dmr_block_encode(code, 1U << (code.k - 1 - i));
        for (std::size_t bit = 0; bit < code.n; ++bit) {
            out.push_back(((word >> (code.n - 1 - bit)) & 1U) != 0 ? '1' : '0');
        }
        out.push_back('\n');
    }
    return out;
}

std::uint32_t minimum_distance(const decode::DmrBlockCode& code) {
    std::uint32_t best = code.n;
    for (std::uint32_t i = 1; i < (1U << code.k); ++i) {
        best = std::min(best, static_cast<std::uint32_t>(std::popcount(decode::dmr_block_encode(code, i))));
    }
    return best;
}

}  // namespace

// ---------------------------------------------------------------------------
// Annex B, code by code
// ---------------------------------------------------------------------------

TEST_CASE("the DMR block codes regenerate the Annex B.3 generator matrices", "[decode][dmr]") {
    // Tables B.11 to B.17, each as a fingerprint of its printed text.
    struct Printed {
        const char* table;
        decode::DmrBlockCode code;
        std::uint64_t fingerprint;
    };
    const Printed printed[] = {
        {"Table B.11, Golay (20,8)", decode::kDmrGolay20, 0xBE94'8CCC'B332'3A53ULL},
        {"Table B.12, quadratic residue (16,7,6)", decode::kDmrQr16, 0x0B68'FF48'49DA'090DULL},
        {"Table B.13, Hamming (17,12,3)", decode::kDmrHamming17, 0xED21'F576'912B'AC05ULL},
        {"Table B.14, Hamming (13,9,3)", decode::kDmrHamming13, 0xA77F'F331'F322'ADF3ULL},
        {"Table B.15, Hamming (15,11,3)", decode::kDmrHamming15, 0x3966'1082'2FE0'3134ULL},
        {"Table B.16, Hamming (16,11,4)", decode::kDmrHamming16, 0xC187'41AA'8578'F745ULL},
        {"Table B.17, Hamming (7,4,3)", decode::kDmrHamming7, 0x6FBD'4695'B0DF'2C36ULL},
    };
    for (const Printed& table : printed) {
        INFO(table.table);
        const std::string regenerated = generator_text(table.code);
        INFO("regenerated:\n" << regenerated);
        CHECK(fnv1a(regenerated) == table.fingerprint);

        // And systematic, which the fingerprint implies and a reader should
        // not have to take from a hash: the identity is the first k columns.
        for (std::size_t i = 0; i < table.code.k; ++i) {
            const std::uint32_t word = decode::dmr_block_encode(table.code, 1U << (table.code.k - 1 - i));
            CHECK((word >> (table.code.n - table.code.k)) == (1U << (table.code.k - 1 - i)));
        }
    }
}

TEST_CASE("each DMR block code has the distance its name gives and corrects within it",
          "[decode][dmr]") {
    const decode::DmrBlockCode codes[] = {decode::kDmrGolay20,   decode::kDmrQr16,
                                          decode::kDmrHamming17, decode::kDmrHamming13,
                                          decode::kDmrHamming15, decode::kDmrHamming16,
                                          decode::kDmrHamming7};
    std::mt19937_64 random(0xD312'0001ULL);
    for (const decode::DmrBlockCode& code : codes) {
        INFO("(" << code.n << "," << code.k << ")");
        CHECK(minimum_distance(code) == code.distance);

        const unsigned t = (code.distance - 1) / 2;
        for (int trial = 0; trial < 200; ++trial) {
            const auto information = static_cast<std::uint32_t>(random() & ((1U << code.k) - 1U));
            std::uint32_t word = decode::dmr_block_encode(code, information);
            std::uint32_t flipped = 0;
            while (static_cast<unsigned>(std::popcount(flipped)) < t) {
                flipped |= 1U << (random() % code.n);
            }
            word ^= flipped;
            const decode::DmrBlockDecode decoded = decode::dmr_block_decode(code, word);
            CHECK(decoded.information == information);
            CHECK(decoded.distance == t);
            CHECK_FALSE(decoded.detected);
        }
    }
}

TEST_CASE("GF(2^8) and the Reed-Solomon (12,9) code are what B.3.6 prints", "[decode][dmr]") {
    // Table B.19's first row and a scattering of the rest.
    const std::uint8_t first_row[] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
                                      0x1D, 0x3A, 0x74, 0xE8, 0xCD, 0x87, 0x13, 0x26};
    for (unsigned e = 0; e < 16; ++e) {
        CHECK(decode::dmr_gf256_exp(e) == first_row[e]);
    }
    CHECK(decode::dmr_gf256_exp(80) == 0xFD);
    CHECK(decode::dmr_gf256_exp(160) == 0xE6);
    CHECK(decode::dmr_gf256_exp(254) == 0x8E);
    // Table B.20.
    CHECK(decode::dmr_gf256_log(0x03) == 25);
    CHECK(decode::dmr_gf256_log(0x80) == 7);
    CHECK(decode::dmr_gf256_log(0xFF) == 175);
    CHECK(decode::dmr_gf256_log(0x00) == -1);

    // Formula B.10: x^3 + 0e x^2 + 38 x + 40.
    const std::array<std::uint8_t, 4> g = decode::dmr_rs_generator();
    CHECK(g[0] == 0x40);
    CHECK(g[1] == 0x38);
    CHECK(g[2] == 0x0E);
    CHECK(g[3] == 0x01);

    // Table B.18's parity columns, the three octets of each row in hex, as a
    // fingerprint for the reason the top of this file gives.
    std::string parity_text;
    for (std::size_t row = 0; row < 9; ++row) {
        std::array<std::uint8_t, 9> message{};
        message[row] = 0x01;
        const auto parity = decode::dmr_rs_parity(message);
        parity_text += std::format("{:02X}{:02X}{:02X}\n", parity[0], parity[1], parity[2]);
    }
    INFO("regenerated:\n" << parity_text);
    CHECK(fnv1a(parity_text) == 0x338D'EDA3'9BFF'702EULL);
    // Row 9's parity is g(x) itself, x^3 mod g(x), which formula B.10 states
    // outright.
    std::array<std::uint8_t, 9> last{};
    last[8] = 0x01;
    CHECK(decode::dmr_rs_parity(last) == std::array<std::uint8_t, 3>{0x0E, 0x38, 0x40});

    // Distance 4 corrects one octet, anywhere and of any value.
    std::mt19937_64 random(0xD312'0002ULL);
    for (int trial = 0; trial < 500; ++trial) {
        std::array<std::uint8_t, 12> word{};
        for (std::size_t i = 0; i < 9; ++i) {
            word[i] = static_cast<std::uint8_t>(random());
        }
        const auto parity = decode::dmr_rs_parity(std::span<const std::uint8_t, 9>(word.data(), 9));
        std::copy(parity.begin(), parity.end(), word.begin() + 9);
        std::array<std::uint8_t, 12> received = word;
        received[random() % 12] ^= static_cast<std::uint8_t>(1 + random() % 255);
        const decode::DmrRsDecode decoded = decode::dmr_rs_decode(received);
        REQUIRE(decoded.decoded);
        CHECK(decoded.codeword == word);
        CHECK(decoded.corrected == 1);
    }
}

TEST_CASE("the DMR CRCs and the checksum are the B.3.7, B.3.8 and B.3.11 formulas", "[decode][dmr]") {
    // The polynomials' standard check values over "123456789": x^16 + x^12
    // + x^5 + 1 from zero gives 0x31C3, and formula B.20's inversion makes it
    // 0xCE3C; x^8 + x^2 + x + 1 from zero gives 0xF4.
    const std::string text = "123456789";
    const std::vector<std::uint8_t> octets(text.begin(), text.end());
    CHECK(decode::dmr_crc_ccitt(octets) == 0xCE3C);
    std::vector<std::uint8_t> bits;
    for (const std::uint8_t octet : octets) {
        for (int b = 7; b >= 0; --b) {
            bits.push_back(static_cast<std::uint8_t>((octet >> b) & 1U));
        }
    }
    CHECK(decode::dmr_crc8(bits) == 0xF4);

    // Formula B.26: the octet sum modulo 31.
    const std::array<std::uint8_t, 9> lc = {0x00, 0x00, 0x20, 0x00, 0x0C, 0x30, 0x00, 0x30, 0x39};
    CHECK(decode::dmr_checksum5(lc) == (0x20 + 0x0C + 0x30 + 0x30 + 0x39) % 31);
}

TEST_CASE("BPTC (196,96) encodes the Idle message to figure D.1", "[decode][dmr]") {
    // Figure D.1, the 13 by 15 encoded matrix of the Table D.2 bits, as a
    // fingerprint for the reason the top of this file gives.
    const auto encoded = decode::dmr_bptc196_encode(decode::dmr_idle_bits());
    std::string matrix;
    for (std::size_t r = 0; r < 13; ++r) {
        for (std::size_t c = 0; c < 15; ++c) {
            // Formula B.1: matrix bit n, counted from 1 behind R(3), goes to
            // n * 181 mod 196.
            matrix.push_back(encoded[((1 + r * 15 + c) * 181U) % 196U] != 0 ? '1' : '0');
        }
        matrix.push_back('\n');
    }
    INFO("regenerated:\n" << matrix);
    CHECK(fnv1a(matrix) == 0x9406'2FF3'3216'2A67ULL);
    // Its first row is R(2), R(1), R(0), I(95) to I(88), which Table D.2
    // gives as eight ones, and then H_R1.
    CHECK(matrix.substr(0, 11) == "00011111111");
    // R(3) is index 0 and goes to index 0.
    CHECK(encoded[0] == 0);

    // Table B.3, a few rows: interleaved index 1 is H_R1(2), matrix bit 13;
    // 2 is I(77), matrix bit 26; 136 is I(95), matrix bit 4.
    CHECK((13U * 181U) % 196U == 1U);
    CHECK((26U * 181U) % 196U == 2U);
    CHECK((4U * 181U) % 196U == 136U);

    const decode::DmrBptcDecode decoded = decode::dmr_bptc196_decode(encoded);
    CHECK(decoded.clean);
    CHECK(decoded.corrected == 0);
    CHECK(decoded.information == decode::dmr_idle_bits());
}

TEST_CASE("BPTC (196,96) corrects the errors a product of two distance 3 codes spreads",
          "[decode][dmr]") {
    std::mt19937_64 random(0xD312'0003ULL);
    std::size_t recovered[4] = {};
    constexpr int kTrials = 300;
    for (int trial = 0; trial < kTrials; ++trial) {
        std::array<std::uint8_t, 96> information{};
        for (std::uint8_t& bit : information) {
            bit = static_cast<std::uint8_t>(random() & 1U);
        }
        const auto encoded = decode::dmr_bptc196_encode(information);
        for (std::size_t errors = 1; errors <= 4; ++errors) {
            auto received = encoded;
            std::vector<std::size_t> positions;
            while (positions.size() < errors) {
                // Index 0 is R(3), which no code covers; leave it.
                const std::size_t at = 1 + random() % 195;
                if (std::find(positions.begin(), positions.end(), at) == positions.end()) {
                    positions.push_back(at);
                    received[at] ^= 1U;
                }
            }
            const decode::DmrBptcDecode decoded = decode::dmr_bptc196_decode(received);
            recovered[errors - 1] += decoded.information == information ? 1U : 0U;
        }
    }
    std::println("test_dmr BPTC(196,96), {} random words: recovered with 1, 2, 3, 4 errors: {}, {}, {}, {}",
                 kTrials, recovered[0], recovered[1], recovered[2], recovered[3]);
    // One and two errors are always inside what the row and column codes
    // clear between them; three can land as three in one row.
    CHECK(recovered[0] == kTrials);
    CHECK(recovered[1] == kTrials);
    CHECK(recovered[2] >= kTrials * 9 / 10);
}

TEST_CASE("the embedded signalling BPTC is figure B.3 and carries its checksum", "[decode][dmr]") {
    const std::array<std::uint8_t, 9> lc = {0x00, 0x00, 0x00, 0x00, 0x0C, 0x31, 0x00, 0x30, 0x39};
    const auto encoded = decode::dmr_embedded_encode(lc);

    // Figure B.3's transmit matrix: burst 1 begins LC(71), LC(60), LC(49),
    // LC(39), LC(29), LC(19), LC(9), PC(15), LC(70). LC(71) is octet 0's MSB.
    const auto lc_bit = [&](int n) { return static_cast<std::uint8_t>((lc[(71 - n) / 8] >> (7 - (71 - n) % 8)) & 1); };
    CHECK(encoded[0] == lc_bit(71));
    CHECK(encoded[1] == lc_bit(60));
    CHECK(encoded[2] == lc_bit(49));
    CHECK(encoded[3] == lc_bit(39));
    CHECK(encoded[6] == lc_bit(9));
    CHECK(encoded[8] == lc_bit(70));

    auto decoded = decode::dmr_embedded_decode(encoded);
    CHECK(decoded.clean);
    CHECK(decoded.checksum_valid);
    CHECK(decoded.lc == lc);

    // One error in every row is corrected; the column parity then checks.
    auto received = encoded;
    for (std::size_t row = 0; row < 7; ++row) {
        // Column-major: row r, column c sits at c * 8 + r.
        received[(row * 2 % 16) * 8 + row] ^= 1U;
    }
    decoded = decode::dmr_embedded_decode(received);
    CHECK(decoded.lc == lc);
    CHECK(decoded.corrected == 7);
    CHECK(decoded.checksum_valid);
}

TEST_CASE("Short LC in the CACH is figure B.6 and figure B.9", "[decode][dmr]") {
    std::array<std::uint8_t, 28> lc{};
    std::mt19937_64 random(0xD312'0004ULL);
    for (std::uint8_t& bit : lc) {
        bit = static_cast<std::uint8_t>(random() & 1U);
    }
    const auto encoded = decode::dmr_short_lc_encode(lc);
    // Figure B.6, CACH 1: LC(27), LC(15), LC(3), PC(16), LC(26). LC(27) is
    // the first of the 28.
    CHECK(encoded[0] == lc[0]);
    CHECK(encoded[1] == lc[12]);
    CHECK(encoded[2] == lc[24]);
    CHECK(encoded[4] == lc[1]);
    const auto decoded = decode::dmr_short_lc_decode(encoded);
    CHECK(decoded.clean);
    CHECK(decoded.crc_valid);
    CHECK(decoded.lc == lc);

    // Figure B.9: AT, P(16), P(15), P(14), TC, and so on.
    decode::DmrCachBits cach;
    cach.tact = 0b1'0'10'000U;  // AT, TC, LCSS(1), LCSS(0), then three zeros
    for (std::size_t i = 0; i < cach.payload.size(); ++i) {
        cach.payload[i] = static_cast<std::uint8_t>(i % 3 == 0);
    }
    const auto bits = decode::dmr_cach_interleave(cach);
    CHECK(bits[0] == 1);                 // AT
    CHECK(bits[1] == cach.payload[0]);   // P(16)
    CHECK(bits[4] == 0);                 // TC
    CHECK(bits[8] == 1);                 // LCSS(1)
    CHECK(bits[12] == 0);                // LCSS(0)
    CHECK(bits[23] == cach.payload[16]); // P(0)
    const auto back = decode::dmr_cach_deinterleave(bits);
    CHECK(back.tact == cach.tact);
    CHECK(back.payload == cach.payload);
}

TEST_CASE("the sync patterns are Table 9.2 through Table 10.3", "[decode][dmr]") {
    const DmrSyncType all[] = {
        DmrSyncType::BsVoice,          DmrSyncType::BsData,          DmrSyncType::MsVoice,
        DmrSyncType::MsData,           DmrSyncType::MsReverseChannel, DmrSyncType::DirectVoiceSlot1,
        DmrSyncType::DirectDataSlot1,  DmrSyncType::DirectVoiceSlot2, DmrSyncType::DirectDataSlot2,
        DmrSyncType::Reserved,
    };
    for (const DmrSyncType type : all) {
        INFO(decode::dmr_sync_name(type));
        for (const float symbol : decode::dmr_sync_pattern(type)) {
            CHECK((symbol == 3.0F || symbol == -3.0F));
        }
    }
    // The note under Table 9.2: voice and data are complements.
    CHECK((decode::kDmrSyncBsVoice ^ decode::kDmrSyncBsData) == 0xAAAA'AAAA'AAAAULL);
    CHECK((decode::kDmrSyncMsVoice ^ decode::kDmrSyncMsData) == 0xAAAA'AAAA'AAAAULL);
    CHECK((decode::kDmrSyncDirectVoiceSlot1 ^ decode::kDmrSyncDirectDataSlot1) == 0xAAAA'AAAA'AAAAULL);
    CHECK((decode::kDmrSyncDirectVoiceSlot2 ^ decode::kDmrSyncDirectDataSlot2) == 0xAAAA'AAAA'AAAAULL);
    CHECK((decode::kDmrSyncMsReverseChannel ^ decode::kDmrSyncReserved) == 0xAAAA'AAAA'AAAAULL);

    // Patterns that are not complements hardly correlate, which is what the
    // tracking threshold rests on.
    double worst = 0.0;
    for (const DmrSyncType a : all) {
        for (const DmrSyncType b : all) {
            const auto pa = decode::dmr_sync_pattern(a);
            const auto pb = decode::dmr_sync_pattern(b);
            const double score = decode::centred_correlation_at(pa, pb, 0);
            if (std::abs(std::abs(score) - 1.0) > 1e-9) {
                worst = std::max(worst, std::abs(score));
            }
        }
    }
    INFO("largest correlation between distinct patterns " << worst);
    CHECK(worst < decode::DmrConfig{}.tracking_threshold / 2.0);
}

TEST_CASE("the slot type and EMB are Golay and quadratic residue words, CC first", "[decode][dmr]") {
    const auto slot = decode::dmr_slot_type_bits(0b1010, static_cast<std::uint8_t>(DmrDataType::Csbk));
    // Table E.1: CC(3) to CC(0), DT(3) to DT(0), then Golay(11) onwards.
    const std::uint8_t head[8] = {1, 0, 1, 0, 0, 0, 1, 1};
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(slot[i] == head[i]);
    }
    const auto emb = decode::dmr_emb_bits(0b0110, true, decode::kDmrLcssLast);
    // Table E.6: CC(3) to CC(0), PI, LCSS(1), LCSS(0), then QR(8).
    const std::uint8_t emb_head[7] = {0, 1, 1, 0, 1, 1, 0};
    for (std::size_t i = 0; i < 7; ++i) {
        CHECK(emb[i] == emb_head[i]);
    }
}

// ---------------------------------------------------------------------------
// Round trips through 4FSK
// ---------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kTalkgroup = 0x00'0C31;  // 3121
constexpr std::uint32_t kSource = 0x30'3930;
constexpr std::uint8_t kColourCode = 7;

// TS 102 361-2 Table 7.1, Grp_V_Ch_Usr: PF and a reserved bit and FLCO 0,
// FID 0, Service Options, group, source.
std::array<std::uint8_t, 9> group_lc(std::uint32_t group, std::uint32_t source, std::uint8_t options) {
    return {0x00,
            0x00,
            options,
            static_cast<std::uint8_t>(group >> 16U),
            static_cast<std::uint8_t>(group >> 8U),
            static_cast<std::uint8_t>(group),
            static_cast<std::uint8_t>(source >> 16U),
            static_cast<std::uint8_t>(source >> 8U),
            static_cast<std::uint8_t>(source)};
}

std::vector<siggen::DmrVoiceBits> voice_frames(std::size_t bursts, std::uint64_t seed) {
    std::mt19937_64 random(seed);
    std::vector<siggen::DmrVoiceBits> out(bursts);
    for (auto& frame : out) {
        for (std::uint8_t& bit : frame) {
            bit = static_cast<std::uint8_t>(random() & 1U);
        }
    }
    return out;
}

// A base station channel: every slot has its CACH carrying Short LC
// Activity Updates, `one` on timeslot 1 and `two` on timeslot 2, each padded
// with Idle bursts to the longer, with Idle for `lead` slots either end so
// the receiver's filter and timing have something to settle on.
std::vector<siggen::DmrSlot> base_station_channel(const std::vector<siggen::DmrBurstBits>& one,
                                                  const std::vector<siggen::DmrBurstBits>& two,
                                                  std::size_t lead = 8) {
    const std::size_t length = std::max(one.size(), two.size()) + 2 * lead;
    // Activity Update: group voice on both slots, TS 102 361-2 Table 7.10.
    const siggen::DmrShortLcFragments short_lc =
        siggen::dmr_short_lc_fragments(decode::kDmrSlcoActivityUpdate, 0x88'1234U);
    std::vector<siggen::DmrSlot> slots;
    for (std::size_t i = 0; i < 2 * length; ++i) {
        const std::size_t frame = i / 2;
        const bool first = i % 2 == 0;
        const auto& calls = first ? one : two;
        siggen::DmrSlot slot;
        const std::size_t fragment = i % decode::kDmrShortLcFragments;
        slot.cach = siggen::dmr_cach(true, first ? 1 : 2, short_lc.lcss[fragment], short_lc.payload[fragment]);
        if (frame >= lead && frame - lead < calls.size()) {
            slot.burst = calls[frame - lead];
        } else {
            slot.burst = siggen::dmr_idle_burst(DmrSyncType::BsData, kColourCode);
        }
        slots.push_back(slot);
    }
    return slots;
}

std::vector<decode::DmrBurst> decode_all(std::span<const dsp::Complex32> samples,
                                         decode::DmrConfig config = {}) {
    config.rate = kRate;
    auto decoder = decode::Dmr::create(config);
    REQUIRE(decoder.has_value());
    std::vector<decode::DmrBurst> bursts;
    REQUIRE(decoder->process(samples, bursts).has_value());
    return bursts;
}

struct CallSeen {
    std::size_t headers = 0;
    std::size_t terminators = 0;
    std::size_t embedded = 0;
    std::size_t voice = 0;
    std::size_t voice_payload_correct = 0;
    std::size_t lc_wrong = 0;
};

CallSeen tally(const std::vector<decode::DmrBurst>& bursts, std::uint8_t slot,
               const std::array<std::uint8_t, 9>& lc, const std::vector<siggen::DmrVoiceBits>& voice) {
    CallSeen seen;
    for (const decode::DmrBurst& burst : bursts) {
        if (burst.slot != slot) {
            continue;
        }
        if (burst.full_lc) {
            if (burst.full_lc->octets != lc) {
                ++seen.lc_wrong;
            }
            switch (burst.full_lc->carrier) {
                case decode::DmrLcCarrier::VoiceHeader: ++seen.headers; break;
                case decode::DmrLcCarrier::Terminator: ++seen.terminators; break;
                case decode::DmrLcCarrier::Embedded: ++seen.embedded; break;
            }
        }
        if (burst.voice_burst != 0) {
            ++seen.voice;
            if (std::any_of(voice.begin(), voice.end(), [&](const siggen::DmrVoiceBits& v) {
                    return std::equal(v.begin(), v.end(), burst.payload.begin(), burst.payload.end());
                })) {
                ++seen.voice_payload_correct;
            }
        }
    }
    return seen;
}

}  // namespace

TEST_CASE("a DMR voice call round trips on a base station channel with its CACH", "[decode][dmr]") {
    constexpr std::size_t kSuperframes = 3;
    siggen::DmrVoiceCall call;
    call.colour_code = kColourCode;
    // Emergency and privacy set, TS 102 361-2 Table 7.11, to see both come out.
    call.lc = group_lc(kTalkgroup, kSource, 0xC0);
    call.voice = voice_frames(kSuperframes * decode::kDmrSuperframeBursts, 0xD312'0100ULL);
    auto bursts = siggen::dmr_voice_call_bursts(call);
    INFO((bursts.has_value() ? std::string{} : bursts.error().message));
    REQUIRE(bursts.has_value());

    const auto slots = base_station_channel(*bursts, {});
    auto samples = siggen::dmr_render_slots(siggen::DmrModConfig{}, slots);
    REQUIRE(samples.has_value());

    const auto out = decode_all(*samples);
    REQUIRE_FALSE(out.empty());
    const CallSeen seen = tally(out, 1, call.lc, call.voice);
    INFO("headers " << seen.headers << ", terminators " << seen.terminators << ", embedded "
                    << seen.embedded << ", voice bursts " << seen.voice << " of which "
                    << seen.voice_payload_correct << " exact, LCs wrong " << seen.lc_wrong);
    CHECK(seen.headers == 1);
    CHECK(seen.terminators == 1);
    CHECK(seen.embedded == kSuperframes);
    CHECK(seen.voice == call.voice.size());
    CHECK(seen.voice_payload_correct == call.voice.size());
    CHECK(seen.lc_wrong == 0);

    std::size_t idle_on_two = 0;
    std::size_t short_lc = 0;
    for (const decode::DmrBurst& burst : out) {
        CHECK(burst.slot != 0);
        if (burst.colour_code) {
            CHECK(*burst.colour_code == kColourCode);
        }
        if (burst.slot == 2 && burst.data_type == static_cast<std::uint8_t>(DmrDataType::Idle)) {
            ++idle_on_two;
        }
        if (burst.short_lc) {
            ++short_lc;
            CHECK(burst.short_lc->slco == decode::kDmrSlcoActivityUpdate);
            REQUIRE(burst.short_lc->activity.has_value());
            CHECK((*burst.short_lc->activity)[0] == 0b1000);
            CHECK((*burst.short_lc->hashed_address)[0] == 0x12);
        }
        if (burst.full_lc && burst.full_lc->carrier == decode::DmrLcCarrier::VoiceHeader) {
            const decode::DmrFullLc& lc = *burst.full_lc;
            CHECK(lc.flco == decode::kDmrFlcoGroupVoice);
            CHECK(lc.group);
            CHECK(lc.destination == kTalkgroup);
            CHECK(lc.source == kSource);
            REQUIRE(lc.service_options.has_value());
            CHECK(lc.service_options->emergency);
            CHECK(lc.service_options->privacy);
        }
    }
    INFO("idle bursts on slot 2 " << idle_on_two << ", short LCs " << short_lc);
    CHECK(idle_on_two > 10);
    CHECK(short_lc > 10);
}

TEST_CASE("the two timeslots carry two calls and neither is attributed to the other",
          "[decode][dmr]") {
    siggen::DmrVoiceCall first;
    first.colour_code = kColourCode;
    first.lc = group_lc(kTalkgroup, kSource, 0x00);
    first.voice = voice_frames(2 * decode::kDmrSuperframeBursts, 0xD312'0200ULL);
    siggen::DmrVoiceCall second = first;
    second.lc = group_lc(91, 0x12'3456, 0x00);
    second.voice = voice_frames(3 * decode::kDmrSuperframeBursts, 0xD312'0201ULL);
    // The second slot joined late: no header, so its identity has to come
    // from the embedded LC.
    second.header = false;

    auto one = siggen::dmr_voice_call_bursts(first);
    auto two = siggen::dmr_voice_call_bursts(second);
    REQUIRE(one.has_value());
    REQUIRE(two.has_value());
    auto samples = siggen::dmr_render_slots(siggen::DmrModConfig{}, base_station_channel(*one, *two));
    REQUIRE(samples.has_value());
    const auto out = decode_all(*samples);

    const CallSeen a = tally(out, 1, first.lc, first.voice);
    const CallSeen b = tally(out, 2, second.lc, second.voice);
    INFO("slot 1: " << a.headers << " headers, " << a.embedded << " embedded, " << a.voice
                    << " voice, " << a.lc_wrong << " wrong; slot 2: " << b.headers << " headers, "
                    << b.embedded << " embedded, " << b.voice << " voice, " << b.lc_wrong << " wrong");
    CHECK(a.headers == 1);
    CHECK(a.embedded == 2);
    CHECK(a.voice == first.voice.size());
    CHECK(a.lc_wrong == 0);
    CHECK(b.headers == 0);
    CHECK(b.embedded == 3);
    CHECK(b.voice == second.voice.size());
    CHECK(b.voice_payload_correct == second.voice.size());
    CHECK(b.lc_wrong == 0);
}

TEST_CASE("a CSBK, a data header and a PI header round trip", "[decode][dmr]") {
    // TS 102 361-2 Table 7.7, a Preamble CSBK: LB set, CSBKO 111101, FID 0,
    // data follows to a group, 5 blocks to follow, target and source.
    const std::array<std::uint8_t, 10> preamble = {0xBD, 0x00, 0xC0, 0x05, 0x00,
                                                   0x0C, 0x31, 0x30, 0x39, 0x30};
    // Figure 8.3, an unconfirmed data header: group, no response, POC 0, DPF
    // 0010; SAP 0100 IP; destination and source; F set and 3 blocks to
    // follow; FSN 0.
    const std::array<std::uint8_t, 10> header = {0x82, 0x40, 0x00, 0x0C, 0x31,
                                                 0x30, 0x39, 0x30, 0x83, 0x00};
    const std::array<std::uint8_t, 10> pi = {0x21, 0x00, 0x00, 0x00, 0x00,
                                             0x00, 0x00, 0x00, 0x00, 0x00};

    std::vector<siggen::DmrBurstBits> blocks;
    for (const auto& [type, octets] :
         {std::pair{DmrDataType::Csbk, std::span<const std::uint8_t>(preamble)},
          std::pair{DmrDataType::DataHeader, std::span<const std::uint8_t>(header)},
          std::pair{DmrDataType::PiHeader, std::span<const std::uint8_t>(pi)}}) {
        auto burst = siggen::dmr_bptc_burst(DmrSyncType::BsData, kColourCode, type, octets);
        REQUIRE(burst.has_value());
        blocks.push_back(*burst);
    }
    auto samples = siggen::dmr_render_slots(siggen::DmrModConfig{}, base_station_channel(blocks, {}));
    REQUIRE(samples.has_value());
    const auto out = decode_all(*samples);

    std::size_t csbks = 0;
    std::size_t headers = 0;
    std::size_t pis = 0;
    for (const decode::DmrBurst& burst : out) {
        CHECK_FALSE(burst.payload_failed);
        if (burst.csbk) {
            ++csbks;
            const decode::DmrCsbk& c = *burst.csbk;
            CHECK(burst.slot == 1);
            CHECK(c.last_block);
            CHECK(c.opcode == decode::kDmrCsbkoPreamble);
            CHECK(c.preamble_data_follows == true);
            CHECK(c.group == true);
            CHECK(c.blocks_to_follow == 5);
            CHECK(c.target == kTalkgroup);
            CHECK(c.source == kSource);
        }
        if (burst.data_header) {
            ++headers;
            const decode::DmrDataHeader& h = *burst.data_header;
            CHECK(h.group);
            CHECK_FALSE(h.response_requested);
            CHECK(h.format == decode::kDmrDpfUnconfirmed);
            CHECK(h.sap == 0b0100);
            CHECK(h.destination == kTalkgroup);
            CHECK(h.source == kSource);
            CHECK(h.blocks_to_follow == 3);
        }
        pis += burst.pi_header ? 1U : 0U;
    }
    CHECK(csbks == 1);
    CHECK(headers == 1);
    CHECK(pis == 1);
}

TEST_CASE("a direct mode call with the carrier off between bursts round trips", "[decode][dmr]") {
    siggen::DmrVoiceCall call;
    call.colour_code = kColourCode;
    call.lc = group_lc(kTalkgroup, kSource, 0x00);
    call.voice = voice_frames(3 * decode::kDmrSuperframeBursts, 0xD312'0300ULL);

    struct Case {
        DmrSyncType sync;
        std::uint8_t slot;
        std::size_t position;  // which of the two slots of a frame it occupies
    };
    // An MS talking simplex names no timeslot; TDMA direct mode names its own.
    const Case cases[] = {{DmrSyncType::MsVoice, 0, 0},
                          {DmrSyncType::DirectVoiceSlot1, 1, 0},
                          {DmrSyncType::DirectVoiceSlot2, 2, 1}};
    for (const Case& c : cases) {
        INFO(decode::dmr_sync_name(c.sync));
        call.voice_sync = c.sync;
        auto bursts = siggen::dmr_voice_call_bursts(call);
        REQUIRE(bursts.has_value());
        std::vector<siggen::DmrSlot> slots(4);  // carrier off to start
        for (const auto& burst : *bursts) {
            siggen::DmrSlot on;
            on.burst = burst;
            siggen::DmrSlot off;
            slots.push_back(c.position == 0 ? on : off);
            slots.push_back(c.position == 0 ? off : on);
        }
        slots.resize(slots.size() + 4);
        auto samples = siggen::dmr_render_slots(siggen::DmrModConfig{}, slots);
        REQUIRE(samples.has_value());
        // A little noise, so the gaps are a channel and not digital silence.
        REQUIRE(siggen::add_awgn(*samples, siggen::NoiseLevel::snr_in_2500_hz_db(35.0), kRate,
                                 0xD312'0301ULL)
                    .has_value());
        const auto out = decode_all(*samples);
        const CallSeen seen = tally(out, c.slot, call.lc, call.voice);
        INFO("headers " << seen.headers << ", embedded " << seen.embedded << ", voice " << seen.voice
                        << ", exact " << seen.voice_payload_correct << ", terminators "
                        << seen.terminators);
        CHECK(seen.headers == 1);
        CHECK(seen.embedded == 3);
        CHECK(seen.terminators == 1);
        CHECK(seen.voice == call.voice.size());
        CHECK(seen.voice_payload_correct == call.voice.size());
    }
}

TEST_CASE("a conjugated capture decodes with invert set and not without", "[decode][dmr]") {
    const std::array<std::uint8_t, 10> preamble = {0xBD, 0x00, 0x00, 0x01, 0x00,
                                                   0x00, 0x05, 0x00, 0x00, 0x09};
    std::vector<siggen::DmrBurstBits> blocks;
    for (int i = 0; i < 4; ++i) {
        auto burst = siggen::dmr_bptc_burst(DmrSyncType::BsData, kColourCode, DmrDataType::Csbk, preamble);
        REQUIRE(burst.has_value());
        blocks.push_back(*burst);
    }
    auto samples = siggen::dmr_render_slots(siggen::DmrModConfig{}, base_station_channel(blocks, {}));
    REQUIRE(samples.has_value());
    for (dsp::Complex32& sample : *samples) {
        sample = std::conj(sample);
    }
    const auto count = [](const std::vector<decode::DmrBurst>& bursts) {
        return std::count_if(bursts.begin(), bursts.end(), [](const decode::DmrBurst& b) { return b.csbk.has_value(); });
    };
    decode::DmrConfig inverted;
    inverted.invert = true;
    CHECK(count(decode_all(*samples, inverted)) == 4);
    CHECK(count(decode_all(*samples)) == 0);
}

TEST_CASE("dmr_sync_score finds DMR and does not find noise", "[decode][dmr]") {
    std::vector<siggen::DmrBurstBits> blocks;
    for (int i = 0; i < 6; ++i) {
        blocks.push_back(siggen::dmr_idle_burst(DmrSyncType::BsData, kColourCode));
    }
    auto samples = siggen::dmr_render_slots(siggen::DmrModConfig{}, base_station_channel(blocks, {}, 2));
    REQUIRE(samples.has_value());
    auto found = decode::dmr_sync_score(*samples, kRate);
    REQUIRE(found.has_value());
    INFO("score " << found->score << ", hits " << found->hits << ", on the slot grid "
                  << found->hits_on_slot_grid);
    CHECK(found->score > 0.95);
    CHECK(found->type == DmrSyncType::BsData);
    CHECK(found->hits_on_slot_grid >= 10);

    std::vector<dsp::Complex32> noise(samples->size(), dsp::Complex32{});
    REQUIRE(siggen::add_awgn_at_power(noise, 1.0, 0xD312'0400ULL).has_value());
    auto none = decode::dmr_sync_score(noise, kRate);
    REQUIRE(none.has_value());
    INFO("noise: score " << none->score << ", hits " << none->hits << ", on the grid "
                         << none->hits_on_slot_grid);
    CHECK(none->hits_on_slot_grid == 0);
}

TEST_CASE("a minute of noise gives no DMR bursts", "[decode][dmr]") {
    // What the search does with nothing to find: a sync must reach 0.9, the
    // largest sidelobe measured on Table 9.2's patterns is 0.858, and every
    // burst it reports still has to fit a gain inside what clause 10.2.2.3
    // allows. Noise across the whole 48 kHz, as a squelch-open receiver on a
    // quiet channel delivers it, fed in 20 ms blocks as the engine does.
    constexpr std::size_t kSeconds = 60;
    decode::DmrConfig config;
    config.rate = kRate;
    auto decoder = decode::Dmr::create(config);
    REQUIRE(decoder.has_value());
    std::vector<decode::DmrBurst> bursts;
    constexpr std::size_t kBlock = kRate / 50;
    std::vector<dsp::Complex32> block(kBlock);
    for (std::size_t b = 0; b < kSeconds * 50; ++b) {
        std::fill(block.begin(), block.end(), dsp::Complex32{});
        REQUIRE(siggen::add_awgn_at_power(block, 1.0, 0xD312'0600ULL + b).has_value());
        REQUIRE(decoder->process(block, bursts).has_value());
    }
    std::size_t with_payload = 0;
    for (const decode::DmrBurst& burst : bursts) {
        with_payload += (burst.full_lc || burst.csbk || burst.data_header || burst.short_lc) ? 1U : 0U;
    }
    std::println("test_dmr noise: {} s gave {} bursts, {} carrying a decoded payload", kSeconds,
                 bursts.size(), with_payload);
    // Measured 2026-09-23: no bursts in the minute. With the discriminator
    // limited at five symbol units, which core/decode/dmr.cpp says why it is
    // not, there were 2, neither carrying anything a CRC, a checksum or the
    // Reed-Solomon code passed.
    CHECK(with_payload == 0);
    CHECK(bursts.size() <= 4);
}

TEST_CASE("DMR CSBK error rate against noise, measured", "[decode][dmr]") {
    // A base station channel carrying CSBKs on both timeslots, each with its
    // own data, and the rate at which they fail to come back whole: the
    // frame error rate a user of this decoder sees on control signalling.
    // Beside it the raw bit error rate of every data burst's 196 channel
    // bits before the BPTC, which is the demodulator's own figure.
    constexpr std::size_t kCsbks = 60;
    std::mt19937_64 random(0xD312'0500ULL);
    std::vector<std::array<std::uint8_t, 10>> sent(kCsbks);
    std::vector<siggen::DmrBurstBits> one;
    std::vector<siggen::DmrBurstBits> two;
    for (std::size_t i = 0; i < kCsbks; ++i) {
        // FID 0x10, a manufacturer's feature set, so the octets are raw.
        sent[i] = {0xBF, 0x10};
        for (std::size_t o = 2; o < 10; ++o) {
            sent[i][o] = static_cast<std::uint8_t>(random());
        }
        auto burst = siggen::dmr_bptc_burst(DmrSyncType::BsData, kColourCode, DmrDataType::Csbk, sent[i]);
        REQUIRE(burst.has_value());
        (i % 2 == 0 ? one : two).push_back(*burst);
    }
    constexpr std::size_t kLead = 8;
    const auto slots = base_station_channel(one, two, kLead);
    auto clean = siggen::dmr_render_slots(siggen::DmrModConfig{}, slots);
    REQUIRE(clean.has_value());

    // Where each CSBK's first symbol lands at the receiver, in input samples:
    // slot i's burst starts 12 symbols into it, and the transmit filter's
    // delay, 60 samples at 121 taps, is the one the decoder does not take
    // off. The decoder takes off its own.
    constexpr double kSamplesPerSymbol = 10.0;
    const double transmit_delay = static_cast<double>(siggen::DmrModConfig{}.filter_taps - 1) / 2.0;
    const auto csbk_at = [&](std::size_t first_sample) -> std::optional<std::size_t> {
        const double slot = (static_cast<double>(first_sample) - transmit_delay -
                             static_cast<double>(decode::kDmrCachSymbols) * kSamplesPerSymbol) /
                            (static_cast<double>(decode::kDmrSlotSymbols) * kSamplesPerSymbol);
        const auto index = static_cast<long long>(std::llround(slot)) - 2 * static_cast<long long>(kLead);
        if (index < 0 || index >= static_cast<long long>(kCsbks)) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(index);
    };
    std::vector<std::array<std::uint8_t, decode::kDmrBptcBits>> channel_bits;
    for (const auto& octets : sent) {
        auto block = decode::dmr_data_block_bits(DmrDataType::Csbk, octets);
        REQUIRE(block.has_value());
        channel_bits.push_back(decode::dmr_bptc196_encode(*block));
    }

    struct Point {
        double snr_db;
        double allowed_frame_error_rate;
    };
    // SNR in 2500 Hz. Measured 2026-09-23: 60 of 60 whole at 30 dB, with 0
    // of 11760 channel bits wrong before the BPTC; 27 of 60 at 16 dB, a
    // frame error rate of 0.550, with 658 of 11172 channel bits wrong,
    // 0.0589. The curve between is tools/bench's dmr subject.
    const Point points[] = {{30.0, 0.0}, {16.0, 0.8}};
    for (const Point& point : points) {
        std::vector<dsp::Complex32> samples = *clean;
        REQUIRE(siggen::add_awgn(samples, siggen::NoiseLevel::snr_in_2500_hz_db(point.snr_db), kRate,
                                 0xD312'0501ULL)
                    .has_value());
        const auto out = decode_all(samples);
        std::size_t good = 0;
        std::size_t bits = 0;
        std::size_t wrong = 0;
        for (const decode::DmrBurst& burst : out) {
            if (burst.csbk && std::find(sent.begin(), sent.end(), burst.csbk->octets) != sent.end()) {
                ++good;
            }
            const auto index = csbk_at(burst.first_sample);
            if (!burst.sync || !index || burst.payload.size() != decode::kDmrBptcBits) {
                continue;
            }
            for (std::size_t i = 0; i < decode::kDmrBptcBits; ++i) {
                wrong += burst.payload[i] != channel_bits[*index][i] ? 1U : 0U;
            }
            bits += decode::kDmrBptcBits;
        }
        const double fer = 1.0 - static_cast<double>(good) / static_cast<double>(kCsbks);
        const double ber = bits == 0 ? 1.0 : static_cast<double>(wrong) / static_cast<double>(bits);
        std::println("test_dmr CSBK at {} dB in 2500 Hz: {} of {} whole, frame error rate {:.3f}; "
                     "channel bits {} of {} wrong, {:.4f}",
                     point.snr_db, good, kCsbks, fer, wrong, bits, ber);
        INFO("frame error rate " << fer << ", channel bit error rate " << ber << " at "
                                 << point.snr_db << " dB");
        CHECK(fer <= point.allowed_frame_error_rate);
        CHECK(bits >= kCsbks * decode::kDmrBptcBits / 2);
    }
}
