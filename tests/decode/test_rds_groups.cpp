// RDS and RBDS group decoding, against the worked examples the standards
// print and against errors chosen rather than sampled.
//
// The bits in this file are generated here, by encoding blocks with
// decode::make_block and shifting them out most significant bit first. Nothing
// in it touches the 57 kHz subcarrier, the biphase decoder or the bit clock.
// That is the seam stated in rds_groups.h and it is what makes these tests
// mean something: a failure here is a framing, CRC or parsing failure and
// cannot be the physical layer having a bad day.
//
// Where a test asserts an arithmetic fact from a standard, the citation is
// beside the number. Three of them contradict a source document and say so at
// the assertion rather than in a commit message nobody will find.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/decode/rds_groups.h"

namespace {

using revenant::decode::BlockOffset;
using revenant::decode::PtyWidth;
using revenant::decode::RdsDecoder;
using revenant::decode::Region;

constexpr std::uint32_t kBlockMask = 0x03FFFFFFu;

// Polynomial arithmetic over GF(2), written out here rather than reused from
// the implementation. A test that calls the code under test to compute its own
// expected value proves only that the code is self-consistent.
[[nodiscard]] std::uint32_t poly_mod(std::uint32_t value) {
    constexpr std::uint32_t kG = 0x5B9;  // x^10+x^8+x^7+x^5+x^4+x^3+1
    for (int bit = 31; bit >= 10; --bit) {
        if ((value & (1u << bit)) != 0) {
            value ^= kG << (bit - 10);
        }
    }
    return value & 0x3FFu;
}

[[nodiscard]] std::uint32_t poly_mul_mod(std::uint32_t a, std::uint32_t b) {
    std::uint32_t out = 0;
    while (b != 0) {
        if ((b & 1u) != 0) {
            out ^= a;
        }
        b >>= 1;
        a <<= 1;
        if ((a & 0x80000000u) != 0) {
            a = poly_mod(a);
        }
    }
    return poly_mod(out);
}

// x^n mod g, by repeated squaring so 325 does not need a 325-iteration loop
// with an overflow in it.
[[nodiscard]] std::uint32_t x_pow_mod(int n) {
    std::uint32_t result = 1;
    std::uint32_t base = 2;  // x
    while (n > 0) {
        if ((n & 1) != 0) {
            result = poly_mul_mod(result, base);
        }
        base = poly_mul_mod(base, base);
        n >>= 1;
    }
    return result;
}

[[nodiscard]] std::string bits_of(std::uint32_t value, int width) {
    std::string out;
    out.reserve(static_cast<std::size_t>(width));
    for (int i = width - 1; i >= 0; --i) {
        out.push_back(((value >> i) & 1u) != 0 ? '1' : '0');
    }
    return out;
}

void feed_word(RdsDecoder& decoder, std::uint32_t block26) {
    for (int i = 25; i >= 0; --i) {
        decoder.feed(((block26 >> i) & 1u) != 0);
    }
}

struct GroupWords {
    std::uint16_t b1 = 0;
    std::uint16_t b2 = 0;
    std::uint16_t b3 = 0;
    std::uint16_t b4 = 0;
    bool version_b = false;
};

[[nodiscard]] std::array<std::uint32_t, 4> encode_group(const GroupWords& words) {
    return {
        revenant::decode::make_block(words.b1, BlockOffset::kA),
        revenant::decode::make_block(words.b2, BlockOffset::kB),
        revenant::decode::make_block(
            words.b3, words.version_b ? BlockOffset::kCPrime : BlockOffset::kC),
        revenant::decode::make_block(words.b4, BlockOffset::kD),
    };
}

void feed_group(RdsDecoder& decoder, const GroupWords& words) {
    for (const std::uint32_t block : encode_group(words)) {
        feed_word(decoder, block);
    }
}

// A benign group used only to get the decoder framed. Acquisition costs four
// blocks and discards them, so exactly one of these is needed before the group
// a test actually cares about, and its contents never reach the state.
constexpr GroupWords kPrimer{0x1234, 0x0000, 0x0000, 0x0000, false};

void prime(RdsDecoder& decoder) {
    feed_group(decoder, kPrimer);
    REQUIRE(decoder.synced());
    REQUIRE(decoder.groups_decoded() == 0);
}

// Block 2 of a type 0A group, assembled field by field so the tests below read
// as the standard does rather than as a hexadecimal constant.
[[nodiscard]] std::uint16_t type0_block2(std::uint8_t pty, bool tp, bool ta, bool music,
                                         bool di_bit, std::uint8_t address,
                                         bool version_b = false) {
    std::uint16_t word = 0;
    word = static_cast<std::uint16_t>(word | (version_b ? 0x0800u : 0x0000u));
    word = static_cast<std::uint16_t>(word | (tp ? 0x0400u : 0x0000u));
    word = static_cast<std::uint16_t>(word | ((pty & 0x1Fu) << 5));
    word = static_cast<std::uint16_t>(word | (ta ? 0x0010u : 0x0000u));
    word = static_cast<std::uint16_t>(word | (music ? 0x0008u : 0x0000u));
    word = static_cast<std::uint16_t>(word | (di_bit ? 0x0004u : 0x0000u));
    word = static_cast<std::uint16_t>(word | (address & 0x03u));
    return word;
}

[[nodiscard]] std::uint16_t chars_to_word(char high, char low) {
    return static_cast<std::uint16_t>((static_cast<unsigned char>(high) << 8) |
                                      static_cast<unsigned char>(low));
}

}  // namespace

// ---------------------------------------------------------------------------
// The code itself
// ---------------------------------------------------------------------------

TEST_CASE("the checkword reproduces the worked code vectors in EN 50067 Annex B", "[rds]") {
    // Annex B clause B.1.1 prints two complete code vectors. These are the
    // only two the standard gives and they are the anchor for everything else
    // in this file.
    CHECK(revenant::decode::checkword(0x0001) == 0b0110111001);
    CHECK(revenant::decode::checkword(0xFFFF) == 0b0011001101);

    const std::uint32_t first = revenant::decode::make_block(0x0001, BlockOffset::kB);
    const std::uint32_t second = revenant::decode::make_block(0xFFFF, BlockOffset::kB);
    CHECK(bits_of(first, 26) == "00000000000000010000100001");
    CHECK(bits_of(second, 26) == "11111111111111110101010101");
}

TEST_CASE("offset words match EN 50067 Annex A Table A.1", "[rds]") {
    CHECK(bits_of(revenant::decode::offset_word(BlockOffset::kA), 10) == "0011111100");
    CHECK(bits_of(revenant::decode::offset_word(BlockOffset::kB), 10) == "0110011000");
    CHECK(bits_of(revenant::decode::offset_word(BlockOffset::kC), 10) == "0101101000");
    CHECK(bits_of(revenant::decode::offset_word(BlockOffset::kCPrime), 10) == "1101010000");
    CHECK(bits_of(revenant::decode::offset_word(BlockOffset::kD), 10) == "0110110100");
    CHECK(bits_of(revenant::decode::offset_word(BlockOffset::kE), 10) == "0000000000");

    // d1 and d0 are always zero, which is a property of the whole table rather
    // than of any one row.
    for (const BlockOffset offset :
         {BlockOffset::kA, BlockOffset::kB, BlockOffset::kC, BlockOffset::kCPrime,
          BlockOffset::kD, BlockOffset::kE}) {
        INFO("offset " << revenant::decode::offset_name(offset));
        CHECK((revenant::decode::offset_word(offset) & 0x3u) == 0);
    }
}

TEST_CASE("the syndrome convention here agrees with both published tables", "[rds]") {
    // THE POINT OF THIS TEST is not to check three lines of arithmetic. It is
    // that a reader who checks rds_groups.cpp against EN 50067 Annex B or
    // Annex C finds neither table and needs to be able to see, here, that all
    // three conventions describe the same code.
    //
    // rds_groups.cpp uses plain d mod g, so a clean block's syndrome is its
    // own offset word and Annex A Table A.1 doubles as the syndrome table.
    for (const std::uint16_t info : {std::uint16_t{0x0000}, std::uint16_t{0x0001},
                                     std::uint16_t{0x1234}, std::uint16_t{0xFFFF},
                                     std::uint16_t{0xA5A5}}) {
        for (const BlockOffset offset :
             {BlockOffset::kA, BlockOffset::kB, BlockOffset::kC, BlockOffset::kCPrime,
              BlockOffset::kD, BlockOffset::kE}) {
            INFO(std::format("info {:04X} offset {}", info,
                             revenant::decode::offset_name(offset)));
            const std::uint32_t block = revenant::decode::make_block(info, offset);
            CHECK(revenant::decode::block_syndrome(block) ==
                  revenant::decode::offset_word(offset));
            CHECK(revenant::decode::error_syndrome(block, offset) == 0);
        }
    }

    // Annex B clause B.2.2 prints the premultiplier for the parity-matrix
    // convention: the code is a shortened cyclic code of natural length 341,
    // and x^325 mod g(x) is x^9 + x^8 + x^4 + x^3 + x + 1.
    CHECK(bits_of(x_pow_mod(325), 11) == "01100011011");

    const std::uint32_t premultiplier = x_pow_mod(325);
    struct Row {
        BlockOffset offset;
        std::string_view annex_b;  // Annex B Table B.1
        std::string_view annex_c;  // Annex C Table C.1
    };
    // Annex C carries its own warning that these two differ: "the syndromes
    // obtained with this polynomial division register are different from that
    // resulting from the matrix of figure B.3 or the circuit of figure B.4".
    constexpr std::array<Row, 5> kRows = {{
        {BlockOffset::kA, "1111011000", "0101111111"},
        {BlockOffset::kB, "1111010100", "0000001110"},
        {BlockOffset::kC, "1001011100", "0100101111"},
        {BlockOffset::kCPrime, "1111001100", "1011101100"},
        {BlockOffset::kD, "1001011000", "1010010111"},
    }};
    for (const Row& row : kRows) {
        INFO("offset " << revenant::decode::offset_name(row.offset));
        const std::uint32_t d = revenant::decode::offset_word(row.offset);
        CHECK(bits_of(poly_mul_mod(premultiplier, d), 10) == row.annex_b);
        CHECK(bits_of(poly_mod(d << 10), 10) == row.annex_c);
    }
}

// ---------------------------------------------------------------------------
// Error correction: what it fixes and, more importantly, what it refuses
// ---------------------------------------------------------------------------

namespace {

struct Burst {
    std::uint32_t pattern;
    int span;
};

[[nodiscard]] std::vector<Burst> all_bursts(int max_span) {
    std::vector<Burst> out;
    for (int span = 1; span <= max_span; ++span) {
        const int interior_count = span >= 2 ? (1 << (span - 2)) : 1;
        for (int start = 0; start + span <= 26; ++start) {
            for (int interior = 0; interior < interior_count; ++interior) {
                std::uint32_t pattern = 1u << start;
                if (span >= 2) {
                    pattern |= 1u << (start + span - 1);
                    pattern |= static_cast<std::uint32_t>(interior) << (start + 1);
                }
                out.push_back({pattern, span});
            }
        }
    }
    return out;
}

}  // namespace

TEST_CASE("every burst of span five or less has a syndrome of its own", "[rds]") {
    // EN 50067 clause 2.3 claims correction of any single burst spanning 5
    // bits or less and calls it optimal for the code. That claim is exactly
    // the statement that these syndromes do not collide, and the lookup table
    // in rds_groups.cpp is only safe because of it, so it is checked rather
    // than believed.
    const std::vector<Burst> bursts = all_bursts(5);
    CHECK(bursts.size() == 367);

    std::set<std::uint32_t> seen;
    for (const Burst& burst : bursts) {
        const std::uint32_t syndrome = poly_mod(burst.pattern);
        INFO(std::format("span {} pattern {}", burst.span, bits_of(burst.pattern, 26)));
        CHECK(syndrome != 0);
        CHECK(seen.insert(syndrome).second);

        const auto found = revenant::decode::burst_for_syndrome(
            static_cast<std::uint16_t>(syndrome), 5);
        REQUIRE(found.has_value());
        CHECK(found->pattern == burst.pattern);
        CHECK(found->span == burst.span);
    }
    CHECK(seen.size() == 367);
}

TEST_CASE("the corrector fixes every burst it claims and refuses every one it does not",
          "[rds]") {
    // 51 patterns at the default limit: 26 single-bit errors and 25 adjacent
    // pairs. Kopitz and Marks section 12.2.3 records this restriction as
    // receiver field practice, not as a requirement of the standard.
    const std::vector<Burst> correctable = all_bursts(2);
    CHECK(correctable.size() == 51);

    SECTION("what it fixes") {
        for (const Burst& burst : correctable) {
            RdsDecoder decoder;
            prime(decoder);

            GroupWords words{0xC0FF, 0x0000, 0x0000, 0x0000, false};
            std::array<std::uint32_t, 4> blocks = encode_group(words);
            blocks[0] ^= burst.pattern;
            for (const std::uint32_t block : blocks) {
                feed_word(decoder, block);
            }

            INFO(std::format("span {} pattern {}", burst.span, bits_of(burst.pattern, 26)));
            REQUIRE(decoder.last_group().has_value());
            const auto& group = *decoder.last_group();
            CHECK(group.blocks[0].valid);
            CHECK(group.blocks[0].corrected);
            CHECK(group.blocks[0].value == 0xC0FF);
            CHECK(decoder.state().pi == 0xC0FF);
        }
    }

    SECTION("what it refuses") {
        // Every burst of span 3, 4 or 5 has a syndrome that belongs to no
        // burst of span 1 or 2, which the test above establishes. So at the
        // default limit the corrector must drop all of them, and this asserts
        // the one thing that matters: it never delivers a rewritten block.
        //
        // An over-eager corrector would deliver a valid-looking block holding
        // a different information word, and on a PS or RadioText field that is
        // a plausible wrong character with nothing downstream able to tell.
        int refused = 0;
        for (const Burst& burst : all_bursts(5)) {
            if (burst.span <= 2) {
                continue;
            }
            RdsDecoder decoder;
            prime(decoder);

            GroupWords words{0xC0FF, 0x0000, 0x0000, 0x0000, false};
            std::array<std::uint32_t, 4> blocks = encode_group(words);
            blocks[0] ^= burst.pattern;
            for (const std::uint32_t block : blocks) {
                feed_word(decoder, block);
            }

            INFO(std::format("span {} pattern {}", burst.span, bits_of(burst.pattern, 26)));
            REQUIRE(decoder.last_group().has_value());
            CHECK_FALSE(decoder.last_group()->blocks[0].valid);
            CHECK_FALSE(decoder.state().pi_valid);
            ++refused;
        }
        CHECK(refused == 367 - 51);
    }

    SECTION("raising the limit corrects what the default refused") {
        RdsDecoder::Options options;
        options.correctable_burst_span = 5;
        RdsDecoder decoder(options);
        prime(decoder);

        GroupWords words{0xC0FF, 0x0000, 0x0000, 0x0000, false};
        std::array<std::uint32_t, 4> blocks = encode_group(words);
        blocks[0] ^= 0b10011u << 7;  // span 5, interior bits set
        for (const std::uint32_t block : blocks) {
            feed_word(decoder, block);
        }
        REQUIRE(decoder.last_group().has_value());
        CHECK(decoder.last_group()->blocks[0].valid);
        CHECK(decoder.last_group()->blocks[0].value == 0xC0FF);
    }

    SECTION("a limit of zero corrects nothing at all") {
        RdsDecoder::Options options;
        options.correctable_burst_span = 0;
        RdsDecoder decoder(options);
        prime(decoder);

        GroupWords words{0xC0FF, 0x0000, 0x0000, 0x0000, false};
        std::array<std::uint32_t, 4> blocks = encode_group(words);
        blocks[0] ^= 1u << 3;  // one bit, the easiest thing there is to fix
        for (const std::uint32_t block : blocks) {
            feed_word(decoder, block);
        }
        REQUIRE(decoder.last_group().has_value());
        CHECK_FALSE(decoder.last_group()->blocks[0].valid);
    }
}

TEST_CASE("block 3 gets no more chances at a correction than any other block", "[rds]") {
    // THE BAR, and it is deliberately wider than the one bug that prompted it.
    //
    // Block 3 is the only block with two candidate offsets, so it is the only
    // one that can be offered a second correction attempt after the first
    // refuses. Any rule that offers it one hands block 3 a lower effective
    // error threshold than blocks 1, 2 and 4, and the block it delivers is
    // read as a repeat of PI. So the property worth asserting is not that one
    // burst is refused, it is that block 3 accepts no more syndromes than
    // anything else once block 2 has said which offset to expect.
    //
    // The sweep runs every error pattern confined to the ten checkword bits,
    // which is a bijection onto the 1023 ways a received block's syndrome can
    // be wrong. What comes back out of a corrected block is not asserted here
    // and is often not what was sent: the corrector flips the one burst that
    // owns that syndrome, and for most of the 51 that burst reaches into the
    // information word rather than staying in the checkword. That is the
    // inherent miscorrection the header describes and it is not what this
    // test is about. What this test is about is how many of the 1023 get
    // through at all.
    constexpr std::uint16_t kPi = 0x2345;

    auto accepted = [](std::size_t index, bool check_pi) {
        int count = 0;
        for (std::uint32_t error = 1; error < 1024; ++error) {
            RdsDecoder decoder;
            prime(decoder);

            const GroupWords words{kPi, type0_block2(10, true, false, true, false, 0),
                                   0xE0E0, 0x2020, false};
            auto blocks = encode_group(words);
            blocks[index] ^= error;
            for (const std::uint32_t block : blocks) {
                feed_word(decoder, block);
            }

            INFO(std::format("block {} error {:03X}", index + 1, error));
            REQUIRE(decoder.last_group().has_value());
            const auto& group = *decoder.last_group();
            if (group.blocks[index].valid) {
                ++count;
            }
            if (check_pi) {
                // Block 2 is intact in every one of these and says version A,
                // so block 3 is AF codes and must never reach the PI.
                CHECK_FALSE(group.c_prime);
                CHECK(decoder.state().pi == kPi);
            }
        }
        return count;
    };

    // 26 single-bit errors and 25 adjacent pairs, the whole of what the span-2
    // default can repair.
    CHECK(accepted(0, false) == 51);
    CHECK(accepted(2, true) == 51);

    SECTION("the burst that used to be promoted into a PI") {
        // Bits 3, 4 and 5, entirely inside the checkword. Against C the
        // residue is 0x038, a span-3 burst that the span-2 default refuses.
        // Against C' it is 0x200, which is bit 9 on its own, so a retry
        // against C' turns an uncorrectable block into a clean C' block
        // holding the AF pair, and a C' block 3 is read as PI.
        RdsDecoder decoder;
        prime(decoder);

        const GroupWords words{kPi, type0_block2(10, true, false, true, false, 0),
                               0xE0E0, 0x2020, false};
        auto blocks = encode_group(words);
        blocks[2] ^= 0x38u;
        for (const std::uint32_t block : blocks) {
            feed_word(decoder, block);
        }

        REQUIRE(decoder.last_group().has_value());
        CHECK_FALSE(decoder.last_group()->blocks[2].valid);
        CHECK_FALSE(decoder.last_group()->c_prime);
        CHECK(decoder.state().pi == kPi);
    }

    SECTION("and the exact C' coincidence, which needs no corrector at all") {
        // 0x238 is offset C XOR offset C', so this block arrives as a
        // faultless C' codeword while block 2 says version A. Reading it as C'
        // would put 0xE0E0 on the display as the station.
        RdsDecoder decoder;
        prime(decoder);

        const GroupWords words{kPi, type0_block2(10, true, false, true, false, 0),
                               0xE0E0, 0x2020, false};
        auto blocks = encode_group(words);
        blocks[2] ^= 0x238u;
        for (const std::uint32_t block : blocks) {
            feed_word(decoder, block);
        }

        REQUIRE(decoder.last_group().has_value());
        CHECK_FALSE(decoder.last_group()->c_prime);
        CHECK(decoder.state().pi == kPi);
    }

    SECTION("a version B group still has its block 3 corrected against C'") {
        // The retry exists for this and deleting it would be the wrong fix.
        // Block 2 says version B, so C' is the offset block 3 is measured
        // against and a two-bit burst in its checkword is repaired.
        RdsDecoder decoder;
        prime(decoder);

        const std::uint16_t b2 = type0_block2(10, true, false, true, false, 0, true);
        GroupWords words{kPi, b2, 0x7788, 0x2020, true};
        auto blocks = encode_group(words);
        blocks[2] ^= 0x03u;
        for (const std::uint32_t block : blocks) {
            feed_word(decoder, block);
        }

        REQUIRE(decoder.last_group().has_value());
        CHECK(decoder.last_group()->blocks[2].valid);
        CHECK(decoder.last_group()->blocks[2].corrected);
        CHECK(decoder.last_group()->c_prime);
        CHECK(decoder.state().pi == 0x7788);
    }

    SECTION("c_prime and version_b agree on every group that has a type") {
        // apply_group reads PI out of block 3 on c_prime while apply_type0 and
        // apply_type2 branch on version_b. They can only stay in step if the
        // two fields are the same answer, which they are once block 2 selects
        // the offset.
        for (const bool version_b : {false, true}) {
            for (std::uint32_t error = 0; error < 1024; ++error) {
                RdsDecoder decoder;
                prime(decoder);

                const std::uint16_t b2 =
                    type0_block2(10, true, false, true, false, 0, version_b);
                GroupWords words{kPi, b2, 0x7788, 0x2020, version_b};
                auto blocks = encode_group(words);
                blocks[2] ^= error;
                for (const std::uint32_t block : blocks) {
                    feed_word(decoder, block);
                }

                INFO(std::format("version_b {} error {:03X}", version_b, error));
                REQUIRE(decoder.last_group().has_value());
                const auto& group = *decoder.last_group();
                REQUIRE(group.type_valid);
                // A dropped block 3 leaves c_prime false, which is the answer
                // apply_group wants: it needs both before it touches the PI.
                CHECK(group.c_prime == (group.version_b && group.blocks[2].valid));
            }
        }
    }
}

TEST_CASE("with block 2 lost, block 3 is read against C before C'", "[rds]") {
    // Two syndromes are correctable against both offsets, 0x020 and 0x218, and
    // the choice between them is arbitrary. It is asserted because it is
    // arbitrary: reverse it and a version A block 3 becomes a C' block whose
    // payload is written over the station PI, which is the one direction of
    // the coin flip that damages something.
    //
    // 0x020 is reached by a span-2 burst on bits 23 and 24. Against C that is
    // exactly the transmitted error. Against C' the residue is 0x370, which is
    // bit 20 alone, so a C'-first decoder would deliver a different word.
    RdsDecoder decoder;
    prime(decoder);

    constexpr std::uint16_t kBlock3 = 0x4C71;
    GroupWords words{0x2345, type0_block2(10, true, false, true, false, 0), kBlock3,
                     0x2020, false};
    auto blocks = encode_group(words);
    blocks[1] ^= kBlockMask;    // block 2 arrives as its own complement
    blocks[2] ^= 0x1800000u;    // bits 23 and 24

    REQUIRE(poly_mod(blocks[2]) == 0x020u);
    for (const std::uint32_t block : blocks) {
        feed_word(decoder, block);
    }

    REQUIRE(decoder.last_group().has_value());
    const auto& group = *decoder.last_group();
    CHECK_FALSE(group.blocks[1].valid);
    REQUIRE(group.blocks[2].valid);
    CHECK(group.blocks[2].corrected);
    CHECK_FALSE(group.c_prime);
    CHECK(group.blocks[2].value == kBlock3);
    // The C' reading would have been this, and would also have been written
    // over the PI from block 1.
    CHECK(group.blocks[2].value != static_cast<std::uint16_t>(kBlock3 ^ 0x6400u));
    CHECK(decoder.state().pi == 0x2345);
}

// ---------------------------------------------------------------------------
// Synchronisation
// ---------------------------------------------------------------------------

TEST_CASE("sync acquires from a start in the middle of a group", "[rds]") {
    // The stream opens on block 3 of a group, which is what happens every time
    // a receiver is tuned: nothing marks the start of a group and the decoder
    // has to find the rhythm from the offset words alone.
    RdsDecoder decoder;

    const GroupWords partial{0x0000, 0x0000, 0x0000, 0x0000, false};
    const auto blocks = encode_group(partial);
    feed_word(decoder, blocks[2]);
    feed_word(decoder, blocks[3]);
    CHECK_FALSE(decoder.synced());

    // The acquisition rule needs the anchor plus three more blocks in
    // sequence, so the first whole group after the fragment is consumed by it.
    feed_group(decoder, kPrimer);
    CHECK(decoder.synced());
    CHECK(decoder.sync_acquisitions() == 1);

    feed_group(decoder, GroupWords{0xDEAD, 0x0000, 0x0000, 0x0000, false});
    CHECK(decoder.groups_decoded() == 1);
    CHECK(decoder.state().pi == 0xDEAD);
}

TEST_CASE("sync holds through a short fade and is dropped by a long one", "[rds]") {
    // A heavy fixed corruption pattern rather than a random one, so the test
    // is the same on every run. Asserting it is neither a codeword nor a
    // correctable burst is what makes "these blocks are all dropped" a fact
    // rather than an expectation.
    constexpr std::uint32_t kFade = 0x03333333u & kBlockMask;
    REQUIRE(poly_mod(kFade) != 0);
    REQUIRE_FALSE(
        revenant::decode::burst_for_syndrome(static_cast<std::uint16_t>(poly_mod(kFade)), 5)
            .has_value());
    // Block 3 gets a second attempt against offset C', so the pattern has to
    // be uncorrectable there too or one block in four would survive the fade
    // and the counts below would be off by a quarter.
    {
        const std::uint32_t against_c_prime =
            poly_mod(kFade) ^ revenant::decode::offset_word(BlockOffset::kC) ^
            revenant::decode::offset_word(BlockOffset::kCPrime);
        REQUIRE(against_c_prime != 0);
        REQUIRE_FALSE(revenant::decode::burst_for_syndrome(
                          static_cast<std::uint16_t>(against_c_prime), 5)
                          .has_value());
    }

    const GroupWords good{0xC0FF, 0x0000, 0x0000, 0x0000, false};

    SECTION("a few bad blocks do not cost the framing") {
        RdsDecoder decoder;
        prime(decoder);

        // Ten groups of nothing but errors: 40 dropped blocks, ten short of
        // the threshold. Losing framing here would mean paying the
        // re-acquisition cost every time a car passes a bridge.
        for (int group = 0; group < 10; ++group) {
            for (std::uint32_t block : encode_group(good)) {
                feed_word(decoder, block ^ kFade);
            }
        }
        CHECK(decoder.synced());
        CHECK(decoder.sync_losses() == 0);
        CHECK(decoder.blocks_dropped() == 40);

        // And the framing is still right, so the next clean group decodes with
        // no acquisition delay at all.
        feed_group(decoder, good);
        CHECK(decoder.state().pi == 0xC0FF);
        CHECK(decoder.sync_acquisitions() == 1);
    }

    SECTION("a long outage drops it, and the decoder recovers afterwards") {
        RdsDecoder decoder;
        prime(decoder);

        for (int group = 0; group < 14; ++group) {
            for (std::uint32_t block : encode_group(good)) {
                feed_word(decoder, block ^ kFade);
            }
        }
        CHECK_FALSE(decoder.synced());
        CHECK(decoder.sync_losses() == 1);

        // Signal returns. One group pays for acquisition, the next decodes.
        feed_group(decoder, kPrimer);
        feed_group(decoder, GroupWords{0xBEEF, 0x0000, 0x0000, 0x0000, false});
        CHECK(decoder.synced());
        CHECK(decoder.sync_acquisitions() == 2);
        CHECK(decoder.state().pi == 0xBEEF);
    }
}

TEST_CASE("noise alone does not produce decoded groups for long", "[rds]") {
    // Seeded and printed, per docs/conventions.md. The claim under test is not
    // "noise never syncs": EN 50067 Annex C clause C.2 says a false anchor
    // turns up about six times a second and the whole acquisition rule exists
    // because of it. The claim is that a decoder handed nothing but noise for
    // a whole second does not sit in sync producing confident garbage.
    constexpr std::uint64_t kSeed = 20260920;
    INFO("seed " << kSeed);

    RdsDecoder decoder;
    std::mt19937_64 generator(kSeed);
    std::uniform_int_distribution<int> coin(0, 1);
    for (int i = 0; i < 1187; ++i) {  // one second at 1187.5 bit/s
        decoder.feed(coin(generator) != 0);
    }

    INFO(std::format("acquisitions {} losses {} groups {}", decoder.sync_acquisitions(),
                     decoder.sync_losses(), decoder.groups_decoded()));
    CHECK_FALSE(decoder.synced());
    CHECK_FALSE(decoder.state().pi_valid);
}

// ---------------------------------------------------------------------------
// Programme service name and RadioText
// ---------------------------------------------------------------------------

TEST_CASE("PS assembles from segments that arrive out of order", "[rds]") {
    RdsDecoder decoder;
    prime(decoder);

    // "REVENANT" in four two-character segments, delivered 2, 0, 3, 1.
    const std::array<std::pair<std::uint8_t, std::uint16_t>, 4> segments = {{
        {std::uint8_t{2}, chars_to_word('N', 'A')},
        {std::uint8_t{0}, chars_to_word('R', 'E')},
        {std::uint8_t{3}, chars_to_word('N', 'T')},
        {std::uint8_t{1}, chars_to_word('V', 'E')},
    }};

    for (const auto& [address, chars] : segments) {
        feed_group(decoder, GroupWords{
                                0x2345,
                                type0_block2(10, true, false, true, false, address),
                                0xE0E0,  // two AF codes, not under test here
                                chars,
                                false,
                            });
    }

    CHECK(decoder.state().ps_complete());
    CHECK(decoder.state().ps_text() == "REVENANT");
    CHECK(decoder.state().pi == 0x2345);
    CHECK(decoder.state().pty == 10);
    CHECK(decoder.state().tp);
    CHECK_FALSE(decoder.state().ta);
    CHECK(decoder.state().music);
}

TEST_CASE("a partially received PS shows blanks rather than terminators", "[rds]") {
    RdsDecoder decoder;
    prime(decoder);

    feed_group(decoder, GroupWords{0x2345, type0_block2(0, false, false, true, false, 1),
                                   0xE0E0, chars_to_word('V', 'E'), false});

    CHECK_FALSE(decoder.state().ps_complete());
    CHECK(decoder.state().ps_text() == "  VE    ");
}

TEST_CASE("decoder identification bits arrive one per group, d3 first", "[rds]") {
    // EN 50067 clause 3.1.5.1 note 5 with clause 3.2.1.5 Table 9: segment
    // address 00 carries d3 and the index counts down as the address counts
    // up, which is the easiest thing in the whole specification to get
    // backwards.
    RdsDecoder decoder;
    prime(decoder);

    // d3 dynamic PTY = 1, d2 compressed = 0, d1 artificial head = 1,
    // d0 stereo = 1.
    const std::array<std::pair<std::uint8_t, bool>, 4> bits = {{
        {std::uint8_t{0}, true},
        {std::uint8_t{1}, false},
        {std::uint8_t{2}, true},
        {std::uint8_t{3}, true},
    }};
    for (const auto& [address, value] : bits) {
        feed_group(decoder, GroupWords{0x2345,
                                       type0_block2(0, false, false, true, value, address),
                                       0xE0E0, 0x2020, false});
    }

    const auto& di = decoder.state().di;
    CHECK(di.received == 0x0F);
    CHECK(di.dynamic_pty);
    CHECK_FALSE(di.compressed);
    CHECK(di.artificial_head);
    CHECK(di.stereo);
}

TEST_CASE("RadioText clears on an A/B toggle instead of splicing two messages", "[rds]") {
    // This is the failure the A/B flag exists to prevent and the one that is
    // invisible when it happens: the tail of the old message stays in the
    // buffer, the head of the new one overwrites the front, and the result
    // reads as a complete sentence that was never transmitted.
    RdsDecoder decoder;
    prime(decoder);

    auto send_2a = [&](bool ab, std::uint8_t address, const char* four) {
        const std::uint16_t b2 = static_cast<std::uint16_t>(
            (2u << 12) | (ab ? 0x0010u : 0x0000u) | (address & 0x0Fu));
        feed_group(decoder, GroupWords{0x2345, b2, chars_to_word(four[0], four[1]),
                                       chars_to_word(four[2], four[3]), false});
    };

    send_2a(false, 0, "THE ");
    send_2a(false, 1, "LONG");
    send_2a(false, 2, "ER M");
    send_2a(false, 3, "ESSA");
    send_2a(false, 4, "GE\r ");  // 0x0D terminates
    CHECK(decoder.state().rt_text() == "THE LONGER MESSAGE");

    // The flag toggles and a shorter message starts. Only its first segment
    // has arrived.
    send_2a(true, 0, "NEW\r");
    CHECK(decoder.state().rt_text() == "NEW");

    // Same flag, another segment: this one overwrites in place and leaves
    // segments not received alone, which is the other half of clause 3.1.5.3.
    send_2a(true, 0, "HERE");
    send_2a(true, 1, " NOW");
    CHECK(decoder.state().rt_text() == "HERE NOW");
}

TEST_CASE("RadioText 2B addresses two characters per segment", "[rds]") {
    RdsDecoder decoder;
    prime(decoder);

    auto send_2b = [&](std::uint8_t address, char a, char b) {
        const std::uint16_t b2 =
            static_cast<std::uint16_t>((2u << 12) | 0x0800u | (address & 0x0Fu));
        feed_group(decoder,
                   GroupWords{0x2345, b2, 0x2345, chars_to_word(a, b), true});
    };

    send_2b(0, 'O', 'N');
    send_2b(1, ' ', 'A');
    send_2b(2, 'I', 'R');
    CHECK(decoder.state().rt_text() == "ON AIR");
    CHECK(decoder.state().rt_version_b);

    // Clause 3.1.5.3 forbids mixing 2A and 2B inside one message because the
    // same segment address names different character positions in each. A
    // transmitter that switches has broken the contract, and clearing is the
    // only reading that cannot splice.
    const std::uint16_t b2a = static_cast<std::uint16_t>(2u << 12);
    feed_group(decoder, GroupWords{0x2345, b2a, chars_to_word('X', 'X'),
                                   chars_to_word('X', 'X'), false});
    CHECK(decoder.state().rt_text() == "XXXX");
}

// ---------------------------------------------------------------------------
// Regions
// ---------------------------------------------------------------------------

TEST_CASE("the two PTY tables are complete and disagree", "[rds]") {
    for (std::uint8_t code = 0; code < 32; ++code) {
        INFO("code " << static_cast<int>(code));
        CHECK_FALSE(revenant::decode::pty_entry(Region::kRds, code).name.empty());
        CHECK_FALSE(revenant::decode::pty_entry(Region::kRbds, code).name.empty());
        CHECK(revenant::decode::pty_name(Region::kRds, code, PtyWidth::kShort).size() <= 8);
        CHECK(revenant::decode::pty_name(Region::kRds, code, PtyWidth::kLong).size() <= 16);
        CHECK(revenant::decode::pty_name(Region::kRbds, code, PtyWidth::kShort).size() <= 8);
        CHECK(revenant::decode::pty_name(Region::kRbds, code, PtyWidth::kLong).size() <= 16);
    }

    // EN 50067 Annex F Table F.1 against NRSC-4-B Table F.2. The tables agree
    // on nothing except codes 0, 1, 30 and 31, and even those differ in
    // wording, so there is no partial sharing to exploit and no shortcut
    // available to an implementation.
    CHECK(revenant::decode::pty_name(Region::kRds, 26, PtyWidth::kLong) == "National Music");
    CHECK(revenant::decode::pty_name(Region::kRbds, 26, PtyWidth::kLong) == "Hip hop");
    CHECK(revenant::decode::pty_name(Region::kRds, 5, PtyWidth::kLong) == "Education");
    CHECK(revenant::decode::pty_name(Region::kRbds, 5, PtyWidth::kLong) == "Rock");
    CHECK(revenant::decode::pty_entry(Region::kRds, 31).name == "Alarm");
    CHECK(revenant::decode::pty_entry(Region::kRbds, 31).name == "Emergency");

    // Three strings in NRSC-4-B carry a stray space. They are reproduced
    // verbatim on purpose: the standard decides what a receiver displays, and
    // tidying them would make this code disagree with every other RBDS
    // receiver in exactly the way nobody would ever look for.
    CHECK(revenant::decode::pty_name(Region::kRbds, 9, PtyWidth::kLong) == "Top_ 40");
    CHECK(revenant::decode::pty_name(Region::kRbds, 17, PtyWidth::kLong) == "Soft_ R_&_B");
    CHECK(revenant::decode::pty_name(Region::kRbds, 25, PtyWidth::kLong) == "Musica _Espanol");

    // Codes 27 and 28 have no display string at all in Table F.2, which is not
    // the same as having an empty one. The name field is what preserves the
    // distinction.
    CHECK(revenant::decode::pty_entry(Region::kRbds, 27).name == "Unassigned");
    CHECK(revenant::decode::pty_name(Region::kRbds, 27, PtyWidth::kShort).empty());
}

TEST_CASE("the same group decodes to a different programme type in each region", "[rds]") {
    // The whole argument for a region parameter in one case: the bits are
    // identical, both readings render, neither faults, and a decoder set to
    // the wrong one is silently wrong.
    const GroupWords words{0x2345, type0_block2(26, false, false, true, false, 0), 0xE0E0,
                           chars_to_word('R', 'E'), false};

    RdsDecoder europe(Region::kRds);
    prime(europe);
    feed_group(europe, words);

    RdsDecoder america(Region::kRbds);
    prime(america);
    feed_group(america, words);

    REQUIRE(europe.state().pty_valid);
    REQUIRE(america.state().pty_valid);
    CHECK(europe.state().pty == america.state().pty);
    CHECK(revenant::decode::pty_name(europe.region(), europe.state().pty, PtyWidth::kLong) !=
          revenant::decode::pty_name(america.region(), america.state().pty, PtyWidth::kLong));
    CHECK(revenant::decode::pty_name(europe.region(), 26, PtyWidth::kLong) == "National Music");
    CHECK(revenant::decode::pty_name(america.region(), 26, PtyWidth::kLong) == "Hip hop");
}

TEST_CASE("call signs come back out of North American PI codes", "[rds]") {
    using revenant::decode::callsign_from_pi;
    using revenant::decode::pi_from_callsign;

    // NRSC-4-B section D.7.2, the standard's own two worked examples. Example
    // 1 is also what settles the weighting: section D.7.1 step 2 labels the
    // columns "3rd letter position", "2nd", "1st", and KGTB shows the
    // LEFTMOST of the three letters taking the 676 weight.
    CHECK(pi_from_callsign("KGTB") == 0x21C7);
    CHECK(pi_from_callsign("WKTI") == 0x7106);
    CHECK(callsign_from_pi(Region::kRbds, 0x21C7) == "KGTB");
    CHECK(callsign_from_pi(Region::kRbds, 0x7106) == "WKTI");

    // The range boundaries from Table D.7. The K and W ranges are exactly
    // contiguous and the reserved three-letter range starts one past the top
    // of the W range, so the space partitions with no gap and no overlap.
    CHECK(pi_from_callsign("KZZZ") == 0x54A7);
    CHECK(pi_from_callsign("WAAA") == 0x54A8);
    CHECK(pi_from_callsign("WZZZ") == 0x994F);
    CHECK(*pi_from_callsign("KZZZ") + 1 == *pi_from_callsign("WAAA"));
    CHECK(*pi_from_callsign("WZZZ") + 1 == 0x9950);

    // Exception 1, section D.7.1: a computed PI with a zero second nibble is
    // reassigned so a European receiver does not read it as a local station
    // and refuse to AF switch.
    CHECK(pi_from_callsign("KEOE") == 0xAF1C);  // 0x1C00, both exceptions
    CHECK(callsign_from_pi(Region::kRbds, 0xAF1C) == "KEOE");

    // Exception 2's NOTE: the nine codes 0x1000 through 0x9000 satisfy both
    // rules and map twice, landing on 0xAFA1 through 0xAFA9.
    CHECK(pi_from_callsign("KAAA") == 0xAFA1);
    CHECK(callsign_from_pi(Region::kRbds, 0xAFA1) == "KAAA");

    // WHAT NRSC-4-B SAYS HERE, AND WHY THIS ASSERTION CONTRADICTS IT
    //
    // Section D.7.1 exception 3 states that WYAY maps to 4F78 and WYAI to
    // 4F68. Both are wrong and have been since 1998: NRSC-4 (April 1998), the
    // NRSC-4-A 2005 annexes and NRSC-4-B all print them unchanged. Under the
    // algorithm the same document gives three paragraphs earlier, WYAY is
    // 24*676 + 0*26 + 24 = 16248 plus the W constant 21672, which is 37920 =
    // 0x9420. 0x4F78 is what the K constant of 4096 produces, and WYAY starts
    // with W.
    //
    // Asserted here, with the contradiction in view, so that a future reader
    // who checks the standard, finds 4F78 and concludes the code is broken
    // reads this first.
    CHECK(pi_from_callsign("WYAY") == 0x9420);
    CHECK(pi_from_callsign("WYAI") == 0x9410);
    CHECK(pi_from_callsign("WYAY") != 0x4F78);

    // Every four-letter call sign round trips, which is the property that
    // matters more than any single vector.
    for (char first : {'K', 'W'}) {
        for (int a = 0; a < 26; ++a) {
            for (int b = 0; b < 26; b += 5) {
                for (int c = 0; c < 26; c += 7) {
                    std::string call;
                    call.push_back(first);
                    call.push_back(static_cast<char>('A' + a));
                    call.push_back(static_cast<char>('A' + b));
                    call.push_back(static_cast<char>('A' + c));
                    INFO("call " << call);
                    const auto pi = pi_from_callsign(call);
                    REQUIRE(pi.has_value());
                    CHECK(callsign_from_pi(Region::kRbds, *pi) == call);
                }
            }
        }
    }

    // A call sign in the hand-assigned three-letter table, and one that is
    // not. The table in NRSC-4-B holds 72 rows and 36 of them were
    // transcribed; a PI inside the reserved range and outside the transcribed
    // rows answers with nothing rather than with a guess, because a wrong call
    // sign on a display is worse than a blank one.
    CHECK(callsign_from_pi(Region::kRbds, 0x9950) == "KGO");
    CHECK(callsign_from_pi(Region::kRbds, 0x99B9) == "WRC");
    CHECK(pi_from_callsign("KGO") == 0x9950);
    CHECK_FALSE(callsign_from_pi(Region::kRbds, 0x9955).has_value());
    CHECK_FALSE(pi_from_callsign("QQQ").has_value());

    // Above the reserved range there is no call sign at all: B, D and E are
    // the nationally and regionally linked blocks, C is Canada and F is
    // Mexico.
    CHECK_FALSE(callsign_from_pi(Region::kRbds, 0xB103).has_value());
    CHECK_FALSE(callsign_from_pi(Region::kRbds, 0xC123).has_value());
    CHECK_FALSE(callsign_from_pi(Region::kRbds, 0xF456).has_value());

    // And none of it applies in Europe, where the same sixteen bits are a
    // country code and a programme reference.
    CHECK_FALSE(callsign_from_pi(Region::kRds, 0x21C7).has_value());
}

TEST_CASE("coverage area codes mean something everywhere in RDS and only in B, D and E here",
          "[rds]") {
    // NRSC-4-B sections D.7 and D.7.3 call this a subtle yet significant
    // difference. In North America the second nibble of a computed PI is a
    // by-product of the call sign arithmetic and says nothing about coverage.
    CHECK(revenant::decode::coverage_area_applies(Region::kRds, 0x21C7));
    CHECK(revenant::decode::coverage_area_applies(Region::kRds, 0xB103));
    CHECK_FALSE(revenant::decode::coverage_area_applies(Region::kRbds, 0x21C7));
    CHECK(revenant::decode::coverage_area_applies(Region::kRbds, 0xB103));
    CHECK(revenant::decode::coverage_area_applies(Region::kRbds, 0xD2FF));
    CHECK(revenant::decode::coverage_area_applies(Region::kRbds, 0xE1AB));
    CHECK_FALSE(revenant::decode::coverage_area_applies(Region::kRbds, 0xB003));
}

TEST_CASE("the VHF alternative frequency table is shared and the LF/MF one is not", "[rds]") {
    // EN 50067 Table 10 and NRSC-4-B Table 10 are the same table, which is the
    // opposite of what people expect and the reason it is asserted.
    CHECK(revenant::decode::af_vhf_frequency(1) == 87'600'000);
    CHECK(revenant::decode::af_vhf_frequency(204) == 107'900'000);
    CHECK(revenant::decode::af_vhf_frequency(0) == 0);    // not to be used
    CHECK(revenant::decode::af_vhf_frequency(205) == 0);  // filler
    CHECK(revenant::decode::af_vhf_frequency(224) == 0);  // no AF exists
    CHECK(revenant::decode::af_vhf_frequency(250) == 0);  // an LF/MF code follows

    // EN 50067 Table 12, ITU regions 1 and 3, 9 kHz spacing.
    CHECK(revenant::decode::af_lf_mf_frequency(Region::kRds, 1) == 153'000);
    CHECK(revenant::decode::af_lf_mf_frequency(Region::kRds, 15) == 279'000);
    CHECK(revenant::decode::af_lf_mf_frequency(Region::kRds, 16) == 531'000);
    CHECK(revenant::decode::af_lf_mf_frequency(Region::kRds, 135) == 1'602'000);
    CHECK(revenant::decode::af_lf_mf_frequency(Region::kRds, 136) == 0);

    // NRSC-4-B Table 12b, ITU region 2, 10 kHz spacing.
    CHECK(revenant::decode::af_lf_mf_frequency(Region::kRbds, 17) == 540'000);
    CHECK(revenant::decode::af_lf_mf_frequency(Region::kRbds, 133) == 1'700'000);
    CHECK(revenant::decode::af_lf_mf_frequency(Region::kRbds, 16) == 0);
    CHECK(revenant::decode::af_lf_mf_frequency(Region::kRbds, 134) == 0);

    // The same code reads as a different station in each region.
    CHECK(revenant::decode::af_lf_mf_frequency(Region::kRds, 100) !=
          revenant::decode::af_lf_mf_frequency(Region::kRbds, 100));
}

TEST_CASE("AF codes in a type 0A group become a frequency list", "[rds]") {
    RdsDecoder decoder;
    prime(decoder);

    auto send_af = [&](std::uint8_t first, std::uint8_t second) {
        feed_group(decoder,
                   GroupWords{0x2345, type0_block2(0, false, false, true, false, 0),
                              static_cast<std::uint16_t>((first << 8) | second), 0x2020,
                              false});
    };

    send_af(227, 1);    // "3 frequencies follow", then 87.6 MHz
    send_af(100, 205);  // 97.5 MHz, then filler
    send_af(250, 20);   // an LF/MF code follows, then code 20

    const auto& state = decoder.state();
    CHECK(state.af_announced == 3);
    CHECK(std::find(state.af.begin(), state.af.end(), 87'600'000) != state.af.end());
    CHECK(std::find(state.af.begin(), state.af.end(), 97'500'000) != state.af.end());
    // Code 20 in ITU regions 1 and 3 is 531 + 4*9 kHz.
    CHECK(std::find(state.af.begin(), state.af.end(), 567'000) != state.af.end());
    CHECK(state.af.size() == 3);
}

TEST_CASE("the ECC cross-check reports a region contradiction and never acts on it", "[rds]") {
    // EN 50067 Annex N clause N.3: USA A0, Canada A1, Mexico A5.
    auto send_ecc = [](RdsDecoder& decoder, std::uint8_t ecc) {
        const std::uint16_t b2 = static_cast<std::uint16_t>(1u << 12);  // type 1A
        const std::uint16_t b3 = static_cast<std::uint16_t>(0x0000u | ecc);  // variant 0
        feed_group(decoder, GroupWords{0x2345, b2, b3, 0x0000, false});
    };

    SECTION("a North American ECC against a European setting") {
        RdsDecoder decoder(Region::kRds);
        prime(decoder);
        send_ecc(decoder, 0xA0);
        CHECK(decoder.state().ecc_valid);
        CHECK(decoder.state().ecc == 0xA0);
        CHECK(decoder.state().ecc_contradicts_region);
        // The diagnostic is for an operator. The region is a setting and the
        // decoder does not move it.
        CHECK(decoder.region() == Region::kRds);
        CHECK(revenant::decode::pty_name(decoder.region(), 26, PtyWidth::kLong) ==
              "National Music");
    }

    SECTION("the converse is not raised") {
        // Only the three region 2 allocations were read from Annex N, so an
        // ECC outside them is absence of evidence rather than evidence of
        // Europe. A decoder set to RBDS hearing a German ECC says nothing.
        RdsDecoder decoder(Region::kRbds);
        prime(decoder);
        send_ecc(decoder, 0xE0);
        CHECK(decoder.state().ecc == 0xE0);
        CHECK_FALSE(decoder.state().ecc_contradicts_region);
    }
}

TEST_CASE("offset word E is MMBS in North America and an error in Europe", "[rds]") {
    // EN 50067 Annex A footnote 1: E is used in the USA in multiples of four
    // blocks when RDS and MMBS run together, and "must not be used in RDS
    // implementations corresponding to this specification". MMBS itself is out
    // of scope, so the difference is whether these blocks cost the framing.
    const std::uint32_t mmbs = revenant::decode::make_block(0x1111, BlockOffset::kE);

    SECTION("North America counts them and keeps sync") {
        RdsDecoder decoder(Region::kRbds);
        prime(decoder);
        for (int i = 0; i < 40; ++i) {
            feed_word(decoder, mmbs);
        }
        CHECK(decoder.synced());
        CHECK(decoder.mmbs_blocks() == 40);
        CHECK(decoder.blocks_dropped() == 0);
        CHECK(decoder.sync_losses() == 0);
    }

    SECTION("Europe treats them as errors") {
        RdsDecoder decoder(Region::kRds);
        prime(decoder);
        for (int i = 0; i < 40; ++i) {
            feed_word(decoder, mmbs);
        }
        CHECK(decoder.mmbs_blocks() == 0);
        CHECK(decoder.blocks_dropped() > 0);
    }
}

// ---------------------------------------------------------------------------
// Clock time
// ---------------------------------------------------------------------------

TEST_CASE("the MJD conversion matches EN 50067 Annex G and refuses to leave its window",
          "[rds]") {
    using revenant::decode::CalendarDate;

    // Annex G's own worked example.
    const auto date = revenant::decode::date_from_mjd(45218);
    REQUIRE(date.has_value());
    CHECK(date->year == 1982);
    CHECK(date->month == 9);
    CHECK(date->day == 6);

    const auto weekday = revenant::decode::day_of_week_from_mjd(45218);
    REQUIRE(weekday.has_value());
    CHECK(*weekday == 1);  // Monday

    const auto back = revenant::decode::mjd_from_date(CalendarDate{1982, 9, 6});
    REQUIRE(back.has_value());
    CHECK(*back == 45218);

    // A date this project can check against a wall calendar.
    const auto today = revenant::decode::mjd_from_date(CalendarDate{2026, 9, 20});
    REQUIRE(today.has_value());
    CHECK(*today == 61303);

    // The window bounds. Annex G is valid from 1 March 1900 to 28 February
    // 2100 and outside it the formulas produce a wrong answer rather than no
    // answer, which is why the bounds are enforced rather than documented.
    const auto first = revenant::decode::date_from_mjd(revenant::decode::kMjdFirstValid);
    REQUIRE(first.has_value());
    CHECK(first->year == 1900);
    CHECK(first->month == 3);
    CHECK(first->day == 1);

    const auto last = revenant::decode::date_from_mjd(revenant::decode::kMjdLastValid);
    REQUIRE(last.has_value());
    CHECK(last->year == 2100);
    CHECK(last->month == 2);
    CHECK(last->day == 28);

    CHECK_FALSE(revenant::decode::date_from_mjd(revenant::decode::kMjdFirstValid - 1));
    CHECK_FALSE(revenant::decode::date_from_mjd(revenant::decode::kMjdLastValid + 1));
    CHECK_FALSE(revenant::decode::mjd_from_date(CalendarDate{1900, 2, 28}));
    CHECK_FALSE(revenant::decode::mjd_from_date(CalendarDate{2100, 3, 1}));

    // The round trip over the whole window, both directions plus the weekday,
    // because a formula transcribed with integer truncation either works
    // everywhere or fails somewhere nobody sampled.
    int weekday_expected = *revenant::decode::day_of_week_from_mjd(
        revenant::decode::kMjdFirstValid);
    int mismatches = 0;
    for (int mjd = revenant::decode::kMjdFirstValid; mjd <= revenant::decode::kMjdLastValid;
         ++mjd) {
        const auto d = revenant::decode::date_from_mjd(mjd);
        if (!d) {
            ++mismatches;
            continue;
        }
        const auto m = revenant::decode::mjd_from_date(*d);
        if (!m || *m != mjd) {
            ++mismatches;
        }
        const auto wd = revenant::decode::day_of_week_from_mjd(mjd);
        if (!wd || *wd != weekday_expected) {
            ++mismatches;
        }
        weekday_expected = weekday_expected == 7 ? 1 : weekday_expected + 1;
    }
    CHECK(mismatches == 0);
}

TEST_CASE("type 4A carries the clock", "[rds]") {
    RdsDecoder decoder;
    prime(decoder);

    constexpr int kMjd = 45218;      // 1982-09-06
    constexpr int kHour = 13;
    constexpr int kMinute = 47;
    constexpr int kOffset = -5;      // two and a half hours west

    const std::uint16_t b2 = static_cast<std::uint16_t>(
        (4u << 12) | static_cast<std::uint32_t>((kMjd >> 15) & 0x03));
    const std::uint16_t b3 = static_cast<std::uint16_t>(
        ((static_cast<std::uint32_t>(kMjd) & 0x7FFFu) << 1) |
        static_cast<std::uint32_t>((kHour >> 4) & 0x01));
    const std::uint16_t b4 = static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(kHour & 0x0F) << 12) |
        (static_cast<std::uint32_t>(kMinute) << 6) | 0x0020u |
        static_cast<std::uint32_t>(-kOffset));

    feed_group(decoder, GroupWords{0x2345, b2, b3, b4, false});

    const auto& clock = decoder.state().clock;
    REQUIRE(clock.valid);
    CHECK(clock.mjd == kMjd);
    CHECK(clock.date.year == 1982);
    CHECK(clock.date.month == 9);
    CHECK(clock.date.day == 6);
    CHECK(clock.hour == kHour);
    CHECK(clock.minute == kMinute);
    CHECK(clock.offset_half_hours == kOffset);
}

TEST_CASE("a clock outside the Annex G window is refused whole", "[rds]") {
    // Publishing a wrong date attached to a right hour and minute would be the
    // worst of the three available answers.
    RdsDecoder decoder;
    prime(decoder);

    const std::uint16_t b2 = static_cast<std::uint16_t>(4u << 12);
    const std::uint16_t b3 = 0x0002;  // MJD 1, far below the window
    const std::uint16_t b4 = 0x0000;
    feed_group(decoder, GroupWords{0x2345, b2, b3, b4, false});
    CHECK_FALSE(decoder.state().clock.valid);
}

// ---------------------------------------------------------------------------
// The remaining group types
// ---------------------------------------------------------------------------

TEST_CASE("type 1A carries the programme item number and the linkage actuator", "[rds]") {
    RdsDecoder decoder;
    prime(decoder);

    const std::uint16_t b2 = static_cast<std::uint16_t>(1u << 12);
    // Linkage actuator set, variant 3, language code 0x09.
    const std::uint16_t b3 = static_cast<std::uint16_t>(0x8000u | (3u << 12) | 0x09u);
    // Day 6, hour 13, minute 47.
    const std::uint16_t b4 = static_cast<std::uint16_t>((6u << 11) | (13u << 6) | 47u);

    feed_group(decoder, GroupWords{0x2345, b2, b3, b4, false});

    CHECK(decoder.state().linkage_actuator);
    CHECK(decoder.state().language_valid);
    CHECK(decoder.state().language == 0x09);
    REQUIRE(decoder.state().pin.valid);
    CHECK(decoder.state().pin.day == 6);
    CHECK(decoder.state().pin.hour == 13);
    CHECK(decoder.state().pin.minute == 47);
}

TEST_CASE("a PIN with a day of zero is refused", "[rds]") {
    // EN 50067 clause 3.1.5.2 note 3: day zero means no valid PIN and a
    // receiver must then ignore the rest of block 4, which is exactly the
    // trap of reading an hour and a minute that look reasonable.
    RdsDecoder decoder;
    prime(decoder);

    const std::uint16_t b2 = static_cast<std::uint16_t>(1u << 12);
    const std::uint16_t b4 = static_cast<std::uint16_t>((0u << 11) | (13u << 6) | 47u);
    feed_group(decoder, GroupWords{0x2345, b2, 0x0000, b4, false});
    CHECK_FALSE(decoder.state().pin.valid);
}

TEST_CASE("type 3A announces an open data application", "[rds]") {
    RdsDecoder decoder;
    prime(decoder);

    // Application group type 8A, message 0x1234, AID 0xCD46.
    const std::uint16_t b2 = static_cast<std::uint16_t>((3u << 12) | (8u << 1) | 0u);
    feed_group(decoder, GroupWords{0x2345, b2, 0x1234, 0xCD46, false});

    REQUIRE(decoder.state().oda.size() == 1);
    CHECK(decoder.state().oda[0].group_type == 8);
    CHECK_FALSE(decoder.state().oda[0].version_b);
    CHECK(decoder.state().oda[0].message == 0x1234);
    CHECK(decoder.state().oda[0].aid == 0xCD46);

    // A repeat of the same application updates in place rather than growing
    // the table.
    feed_group(decoder, GroupWords{0x2345, b2, 0x5678, 0xCD46, false});
    CHECK(decoder.state().oda.size() == 1);
    CHECK(decoder.state().oda[0].message == 0x5678);

    // Application group code 00000 means the application is not carried in an
    // associated group and 11111 is an encoder fault. Neither names a group.
    const std::uint16_t none = static_cast<std::uint16_t>(3u << 12);
    feed_group(decoder, GroupWords{0x2345, none, 0x0000, 0x0000, false});
    CHECK(decoder.state().oda.size() == 1);
}

TEST_CASE("type 10A carries the programme type name", "[rds]") {
    RdsDecoder decoder;
    prime(decoder);

    auto send_ptyn = [&](bool ab, std::uint8_t address, const char* four) {
        const std::uint16_t b2 = static_cast<std::uint16_t>(
            (10u << 12) | (ab ? 0x0010u : 0x0000u) | (address & 0x01u));
        feed_group(decoder, GroupWords{0x2345, b2, chars_to_word(four[0], four[1]),
                                       chars_to_word(four[2], four[3]), false});
    };

    send_ptyn(false, 0, "CHAR");
    send_ptyn(false, 1, "TSHO");
    CHECK(decoder.state().ptyn_complete());
    CHECK(decoder.state().ptyn_text() == "CHARTSHO");

    // The flag toggles when the name changes, and the display clears first.
    send_ptyn(true, 1, "TALK");
    CHECK(decoder.state().ptyn_text() == "    TALK");
    CHECK_FALSE(decoder.state().ptyn_complete());
}

TEST_CASE("type 14A assembles other networks", "[rds]") {
    RdsDecoder decoder;
    prime(decoder);

    constexpr std::uint16_t kOtherPi = 0xD3F1;
    auto send_14a = [&](std::uint8_t variant, std::uint16_t info) {
        const std::uint16_t b2 =
            static_cast<std::uint16_t>((14u << 12) | 0x0010u | (variant & 0x0Fu));
        feed_group(decoder, GroupWords{0x2345, b2, info, kOtherPi, false});
    };

    send_14a(0, chars_to_word('B', 'B'));
    send_14a(1, chars_to_word('C', ' '));
    send_14a(2, chars_to_word('R', ' '));
    send_14a(3, chars_to_word('1', ' '));
    send_14a(4, static_cast<std::uint16_t>((1u << 8) | 204u));  // 87.6 and 107.9 MHz
    send_14a(13, static_cast<std::uint16_t>((7u << 11) | 1u));  // PTY 7, TA set
    send_14a(12, 0xBEEF);

    REQUIRE(decoder.state().eon.size() == 1);
    const auto& entry = decoder.state().eon[0];
    CHECK(entry.pi == kOtherPi);
    CHECK(entry.tp);
    CHECK(entry.ps_received == 0x0F);
    CHECK(std::string(entry.ps.data(), entry.ps.size()) == "BBC R 1 ");
    CHECK(entry.pty_valid);
    CHECK(entry.pty == 7);
    CHECK(entry.ta_valid);
    CHECK(entry.ta);
    CHECK(entry.linkage_valid);
    CHECK(entry.linkage == 0xBEEF);
    CHECK(entry.af.size() == 2);
    CHECK(std::find(entry.af.begin(), entry.af.end(), 87'600'000) != entry.af.end());
    CHECK(std::find(entry.af.begin(), entry.af.end(), 107'900'000) != entry.af.end());
}

TEST_CASE("type 14B is the fast traffic announcement change", "[rds]") {
    RdsDecoder decoder;
    prime(decoder);

    constexpr std::uint16_t kOtherPi = 0xD3F1;
    // Version B group, so block 3 carries offset C' and repeats PI(TN).
    const std::uint16_t b2 = static_cast<std::uint16_t>((14u << 12) | 0x0800u | 0x0010u |
                                                        0x0008u);
    feed_group(decoder, GroupWords{0x2345, b2, 0x2345, kOtherPi, true});

    REQUIRE(decoder.state().eon.size() == 1);
    CHECK(decoder.state().eon[0].pi == kOtherPi);
    CHECK(decoder.state().eon[0].tp);
    CHECK(decoder.state().eon[0].ta_valid);
    CHECK(decoder.state().eon[0].ta);
    CHECK(decoder.state().pi == 0x2345);
}

TEST_CASE("type 15B repeats the switching payload twice in one group", "[rds]") {
    RdsDecoder decoder;
    prime(decoder);

    // Blocks 2 and 4 both carry the block 2 shape. Different DI segment
    // addresses in each, which is what makes the group worth sending: two DI
    // bits per group instead of one.
    const std::uint16_t b2 =
        static_cast<std::uint16_t>((15u << 12) | 0x0800u |
                                   type0_block2(9, true, true, false, true, 0));
    const std::uint16_t b4 =
        static_cast<std::uint16_t>((15u << 12) | 0x0800u |
                                   type0_block2(9, true, true, false, false, 1));

    feed_group(decoder, GroupWords{0x2345, b2, 0x2345, b4, true});

    const auto& state = decoder.state();
    CHECK(state.pi == 0x2345);
    CHECK(state.pty == 9);
    CHECK(state.tp);
    CHECK(state.ta);
    CHECK_FALSE(state.music);
    CHECK(state.di.dynamic_pty);      // d3, from segment address 00
    CHECK_FALSE(state.di.compressed);  // d2, from segment address 01
    CHECK(state.di.received == 0b1100);
}

TEST_CASE("a version B group repeats PI in block three", "[rds]") {
    // The offset that block 3 actually carried decides this, not block 2's
    // version bit: the offset is checked by block 3's own CRC and block 2 may
    // never arrive.
    RdsDecoder decoder;
    prime(decoder);

    const std::uint16_t b2 = type0_block2(0, false, false, true, false, 0, true);
    feed_group(decoder, GroupWords{0x7788, b2, 0x7788, chars_to_word('O', 'K'), true});

    REQUIRE(decoder.last_group().has_value());
    CHECK(decoder.last_group()->c_prime);
    CHECK(decoder.last_group()->version_b);
    CHECK(decoder.state().pi == 0x7788);
    CHECK(decoder.state().ps_text() == "OK      ");
}

TEST_CASE("a lost block 2 leaves the group type unknown and only PI recoverable", "[rds]") {
    RdsDecoder decoder;
    prime(decoder);

    GroupWords words{0x2345, type0_block2(10, true, false, true, false, 0), 0xE0E0,
                     chars_to_word('R', 'E'), false};
    auto blocks = encode_group(words);
    blocks[1] ^= 0x03FFFFFFu;  // block 2 arrives as its own complement

    for (const std::uint32_t block : blocks) {
        feed_word(decoder, block);
    }

    REQUIRE(decoder.last_group().has_value());
    CHECK_FALSE(decoder.last_group()->type_valid);
    CHECK(decoder.state().pi == 0x2345);
    CHECK_FALSE(decoder.state().pty_valid);
    CHECK(decoder.state().ps_received == 0);
}

TEST_CASE("reset clears the state and keeps the configuration", "[rds]") {
    RdsDecoder decoder(Region::kRbds);
    prime(decoder);
    feed_group(decoder, GroupWords{0x2345, type0_block2(3, true, false, true, false, 0),
                                   0xE0E0, chars_to_word('K', 'X'), false});
    REQUIRE(decoder.state().pi_valid);

    decoder.reset();
    CHECK(decoder.region() == Region::kRbds);
    CHECK_FALSE(decoder.synced());
    CHECK_FALSE(decoder.state().pi_valid);
    CHECK(decoder.state().ps_text() == "        ");
    CHECK(decoder.bits_fed() == 0);
}
