// TETRA V+D: the BSCH chain against EN 300 392-2, and the demodulator against
// the transmitter.
//
// The same two groups as the other two mode suites. What is different here is
// that the whole chain is specified end to end in one clause, 8.3.1.2, as a
// sequence of five named transformations, so the cases below can check each
// transformation separately as well as checking the chain.
//
// WHY THE BSCH AND NOT A TRAFFIC CHANNEL
//
// Clause 8.2.5.2 says the broadcast synchronisation channel is scrambled with
// an all-zero extended colour code, deliberately, so a receiver that does not
// yet know the cell can read it. That makes it the one channel whose contents
// are recoverable with no key and no part 7 of the standard, and it carries
// the colour code, the country and network codes, and the frame numbering.
// core/decode/tetra.h says the same at more length.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <numbers>
#include <set>
#include <string>
#include <vector>

#include "core/decode/dv_codes.h"
#include "core/decode/dv_phy.h"
#include "core/decode/tetra.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dv_mod.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 72'000;
constexpr double kPi = std::numbers::pi;

decode::TetraSyncPdu sample_pdu() {
    decode::TetraSyncPdu pdu;
    // Table 21.76. System code 0011 is the range of editions this document
    // belongs to; colour code 37 is an ordinary operator value; timeslot 00
    // is timeslot 1.
    pdu.system_code = 0b0011;
    pdu.colour_code = 37;
    pdu.timeslot = 0;
    pdu.frame_number = 17;
    pdu.multiframe_number = 42;
    pdu.sharing_mode = 0;  // continuous transmission
    pdu.reserved_frames = 0;
    pdu.uplane_dtx_allowed = true;
    pdu.frame18_extension = false;
    // Table 18.17. 234 is the United Kingdom's mobile country code.
    pdu.mobile_country_code = 234;
    pdu.mobile_network_code = 1'234;
    pdu.neighbour_cell_broadcast = 1;
    pdu.cell_load = 2;
    pdu.late_entry_supported = true;
    return pdu;
}

}  // namespace

// ---------------------------------------------------------------------------
// The chain, clause by clause
// ---------------------------------------------------------------------------

TEST_CASE("the phase transitions are table 5.1", "[decode][tetra]") {
    // B(2k-1) B(2k) D(k): 11 is -3pi/4, 01 is +3pi/4, 00 is +pi/4, 10 is
    // -pi/4. The first column in the table is B(2k-1), which is the first bit
    // of the pair.
    CHECK(decode::tetra_phase_transition(1, 1) == -3.0 * kPi / 4.0);
    CHECK(decode::tetra_phase_transition(0, 1) == 3.0 * kPi / 4.0);
    CHECK(decode::tetra_phase_transition(0, 0) == kPi / 4.0);
    CHECK(decode::tetra_phase_transition(1, 0) == -kPi / 4.0);
}

TEST_CASE("the (K1+16,K1) block code is the X.25 frame check sequence", "[decode][tetra]") {
    // Clause 8.2.3.3 cites ITU-T X.25 and gives G(X) = X^16 + X^12 + X^5 + 1
    // with the ones preset and postset its two sums of X^i supply. A code word
    // formed as information followed by the check bits verifies, and one bit
    // flipped anywhere in it does not.
    std::vector<std::uint8_t> information(decode::kTetraBschInformationBits, 0);
    for (std::size_t i = 0; i < information.size(); ++i) {
        information[i] = static_cast<std::uint8_t>((i * 5 + 1) & 1U);
    }

    const std::vector<std::uint8_t> parity = decode::tetra_block_code_parity(information);
    REQUIRE(parity.size() == 16);

    std::vector<std::uint8_t> word = information;
    word.insert(word.end(), parity.begin(), parity.end());
    CHECK(decode::tetra_block_code_verify(word));

    for (std::size_t i = 0; i < word.size(); ++i) {
        INFO("bit " << i << " flipped");
        std::vector<std::uint8_t> damaged = word;
        damaged[i] ^= 1U;
        CHECK_FALSE(decode::tetra_block_code_verify(damaged));
    }
}

TEST_CASE("the rate 2/3 puncturing keeps mother bits 1, 2 and 5 of every eight",
          "[decode][tetra]") {
    // Clause 8.2.3.1.3: t = 3, P(1) = 1, P(2) = 2, P(3) = 5, i = j, and
    // k = 8*((i-1) div t) + P(i - t*((i-1) div t)). The clause is one-based
    // and the map is zero-based, so the first three entries are 0, 1 and 4.
    const std::vector<std::uint32_t> map = decode::tetra_rate_two_thirds_map(12);
    REQUIRE(map.size() == 12);
    const std::uint32_t expected[12] = {0, 1, 4, 8, 9, 12, 16, 17, 20, 24, 25, 28};
    for (std::size_t i = 0; i < map.size(); ++i) {
        INFO("output bit " << (i + 1));
        CHECK(map[i] == expected[i]);
    }

    // Three output bits for every two input bits, which is the rate.
    const std::vector<std::uint32_t> full =
        decode::tetra_rate_two_thirds_map(decode::kTetraBschType3Bits);
    CHECK(full.size() == decode::kTetraBschType3Bits);
    CHECK(full.back() < decode::kTetraBschType2Bits * 4);
}

TEST_CASE("the (120,11) block interleaver is a permutation", "[decode][tetra]") {
    // Clause 8.2.4.1: b4(k) = b3(i) with k = 1 + ((a*i) mod K). That is a
    // permutation only when a and K are coprime, and 11 and 120 are.
    auto map = decode::tetra_block_interleave_map(decode::kTetraBschType3Bits,
                                                  decode::kTetraBschInterleaveStep);
    INFO((map.has_value() ? std::string{} : map.error().message));
    REQUIRE(map.has_value());
    REQUIRE(map->size() == decode::kTetraBschType3Bits);

    std::set<std::uint32_t> seen(map->begin(), map->end());
    CHECK(seen.size() == map->size());

    // A step that shares a factor with the block length is refused rather
    // than silently dropping bits, which is the failure this clause makes
    // easy to write.
    auto bad = decode::tetra_block_interleave_map(120, 10);
    CHECK_FALSE(bad.has_value());
}

TEST_CASE("the BSCH scrambling sequence is the clause 8.2.5.2 register",
          "[decode][tetra]") {
    // For BSCH all thirty bits of the extended colour code are zero, so the
    // register starts from p(-31) = p(-30) = 1 and thirty zeros. The sequence
    // that produces is fixed and is the same for every cell, which is the
    // property that makes BSCH readable before the colour code is known.
    const std::vector<std::uint8_t> zero(30, 0);
    auto first = decode::tetra_scrambling_sequence(zero, 120);
    REQUIRE(first.has_value());
    auto second = decode::tetra_scrambling_sequence(zero, 120);
    REQUIRE(second.has_value());
    CHECK(*first == *second);

    // Not all zero, which is what an all-zero colour code would give if the
    // p(-31) and p(-30) ones in equation 8.42 had been missed.
    std::size_t ones = 0;
    for (const std::uint8_t bit : *first) {
        ones += bit;
    }
    INFO("ones in the first 120 bits of the BSCH scrambling sequence: " << ones);
    CHECK(ones > 0);
    CHECK(ones < 120);

    // A non-zero colour code gives a different sequence, which is the whole
    // mechanism the other channels rely on.
    std::vector<std::uint8_t> coloured(30, 0);
    coloured[29] = 1;
    auto third = decode::tetra_scrambling_sequence(coloured, 120);
    REQUIRE(third.has_value());
    CHECK(*third != *first);

    // A colour code of the wrong length is refused.
    CHECK_FALSE(decode::tetra_scrambling_sequence(std::vector<std::uint8_t>(29, 0), 10)
                    .has_value());
}

TEST_CASE("the SYNC PDU round trips through the whole BSCH chain", "[decode][tetra]") {
    const decode::TetraSyncPdu pdu = sample_pdu();
    const auto information = decode::tetra_sync_pdu_bits(pdu);
    CHECK(information.size() == decode::kTetraBschInformationBits);

    auto encoded = decode::tetra_bsch_encode(information);
    INFO((encoded.has_value() ? std::string{} : encoded.error().message));
    REQUIRE(encoded.has_value());
    REQUIRE(encoded->size() == decode::kTetraBschType3Bits);

    std::vector<float> soft(encoded->size(), 0.0F);
    for (std::size_t i = 0; i < encoded->size(); ++i) {
        soft[i] = (*encoded)[i] ? -1.0F : 1.0F;
    }

    auto decoded = decode::tetra_bsch_decode(soft);
    INFO((decoded.has_value() ? std::string{} : decoded.error().message));
    REQUIRE(decoded.has_value());
    CHECK(decoded->block_code_verified);

    const decode::TetraSyncPdu got = decode::tetra_parse_sync_pdu(decoded->information);
    CHECK(got.system_code == pdu.system_code);
    CHECK(got.colour_code == pdu.colour_code);
    CHECK(got.timeslot == pdu.timeslot);
    CHECK(got.frame_number == pdu.frame_number);
    CHECK(got.multiframe_number == pdu.multiframe_number);
    CHECK(got.sharing_mode == pdu.sharing_mode);
    CHECK(got.uplane_dtx_allowed == pdu.uplane_dtx_allowed);
    CHECK(got.mobile_country_code == pdu.mobile_country_code);
    CHECK(got.mobile_network_code == pdu.mobile_network_code);
    CHECK(got.neighbour_cell_broadcast == pdu.neighbour_cell_broadcast);
    CHECK(got.cell_load == pdu.cell_load);
    CHECK(got.late_entry_supported == pdu.late_entry_supported);
}

TEST_CASE("the BSCH recovers from errors the RCPC code covers", "[decode][tetra]") {
    // A rate 2/3 code over a 16-state trellis, so a handful of scattered bit
    // errors in 120 should still come out, and the block code says whether
    // they did rather than leaving the caller to guess.
    const auto information = decode::tetra_sync_pdu_bits(sample_pdu());
    auto encoded = decode::tetra_bsch_encode(information);
    REQUIRE(encoded.has_value());

    std::vector<float> soft(encoded->size(), 0.0F);
    for (std::size_t i = 0; i < encoded->size(); ++i) {
        soft[i] = (*encoded)[i] ? -1.0F : 1.0F;
    }
    for (const std::size_t position : {7U, 31U, 58U, 94U}) {
        soft[position] = -soft[position];
    }

    auto decoded = decode::tetra_bsch_decode(soft);
    REQUIRE(decoded.has_value());
    INFO("block code verified: " << decoded->block_code_verified);
    CHECK(decoded->block_code_verified);
    const decode::TetraSyncPdu got = decode::tetra_parse_sync_pdu(decoded->information);
    CHECK(got.colour_code == 37);
    CHECK(got.mobile_country_code == 234);
}

TEST_CASE("the synchronisation burst is 510 bits with its fields where table 9.9 puts them",
          "[decode][tetra]") {
    auto bits = siggen::tetra_sync_burst_bits(sample_pdu(), 0xA11CE);
    INFO((bits.has_value() ? std::string{} : bits.error().message));
    REQUIRE(bits.has_value());
    REQUIRE(bits->size() == decode::kTetraBurstBits);
    CHECK(decode::kTetraBurstBits == 510);
    CHECK(decode::kTetraBurstSymbols == 255);

    // Clause 9.4.4.3.4, the 38 bit synchronisation training sequence, at bit
    // numbers 215 to 252 which is offset 214 zero-based.
    for (std::size_t i = 0; i < decode::kTetraSyncTrainingSequence.size(); ++i) {
        INFO("training sequence bit " << i);
        CHECK((*bits)[decode::kTetraSyncBurstTraining.offset + i] ==
              decode::kTetraSyncTrainingSequence[i]);
    }

    // Clause 9.4.4.3.1, the frequency correction field: eight ones, sixty-four
    // zeros, eight ones.
    const std::size_t fc = decode::kTetraSyncBurstFrequencyCorrection.offset;
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK((*bits)[fc + i] == 1);
        CHECK((*bits)[fc + 72 + i] == 1);
    }
    for (std::size_t i = 8; i < 72; ++i) {
        CHECK((*bits)[fc + i] == 0);
    }

    // The fields add up to the burst with nothing left over.
    CHECK(decode::kTetraSyncBurstTrainingTail.offset +
              decode::kTetraSyncBurstTrainingTail.length ==
          decode::kTetraBurstBits);
}

// ---------------------------------------------------------------------------
// The round trip
// ---------------------------------------------------------------------------

TEST_CASE("a synchronisation burst round trips through pi/4-DQPSK", "[decode][tetra]") {
    const decode::TetraSyncPdu pdu = sample_pdu();

    // FOUR BURSTS, NOT ONE, AND THE REASON IS NOT MARGIN.
    //
    // Clause 5.4's equation 5.1 makes the modulation differential across an
    // arbitrary number of bursts, with S(0) = 1 as the phase reference before
    // the first symbol of the first one, so concatenated bursts are a single
    // continuous stream and rendering them that way is what the clause
    // describes. A receiver needs that: the matched filter has a group delay
    // and the timing estimator averages over a window, so a capture holding
    // exactly one 255 symbol burst and nothing else has no burst left by the
    // time both have taken their share.
    //
    // A real downlink puts other bursts between synchronisation bursts rather
    // than repeating this one. That changes what is in block 2 and changes
    // nothing about the training sequence correlation or the BSCH decode,
    // which is what this case is about.
    std::vector<std::uint8_t> stream;
    for (std::size_t burst = 0; burst < 4; ++burst) {
        auto bits = siggen::tetra_sync_burst_bits(pdu, 0xA11CE + burst);
        INFO((bits.has_value() ? std::string{} : bits.error().message));
        REQUIRE(bits.has_value());
        stream.insert(stream.end(), bits->begin(), bits->end());
    }

    siggen::TetraModConfig mod;
    mod.rate = kRate;
    auto samples = siggen::tetra_render_bits(mod, stream);
    INFO((samples.has_value() ? std::string{} : samples.error().message));
    REQUIRE(samples.has_value());

    decode::TetraConfig config;
    config.rate = kRate;
    auto decoder = decode::Tetra::create(config);
    INFO((decoder.has_value() ? std::string{} : decoder.error().message));
    REQUIRE(decoder.has_value());

    std::vector<decode::TetraBurst> bursts;
    REQUIRE(decoder->process(*samples, bursts).has_value());

    INFO("bursts recovered: " << bursts.size());
    REQUIRE_FALSE(bursts.empty());

    for (const decode::TetraBurst& burst : bursts) {
        INFO("burst at symbol " << burst.first_symbol << ", training sequence correlation "
                                << burst.sync_score);
        CHECK(burst.block_code_verified);
        REQUIRE(burst.sync.has_value());

        CHECK(burst.sync->colour_code == pdu.colour_code);
        CHECK(burst.sync->system_code == pdu.system_code);
        CHECK(burst.sync->timeslot == pdu.timeslot);
        CHECK(burst.sync->frame_number == pdu.frame_number);
        CHECK(burst.sync->multiframe_number == pdu.multiframe_number);
        CHECK(burst.sync->sharing_mode == pdu.sharing_mode);
        CHECK(burst.sync->uplane_dtx_allowed == pdu.uplane_dtx_allowed);
        CHECK(burst.sync->mobile_country_code == pdu.mobile_country_code);
        CHECK(burst.sync->mobile_network_code == pdu.mobile_network_code);
        CHECK(burst.sync->neighbour_cell_broadcast == pdu.neighbour_cell_broadcast);
        CHECK(burst.sync->cell_load == pdu.cell_load);
        CHECK(burst.sync->late_entry_supported == pdu.late_entry_supported);
    }
}

TEST_CASE("TETRA bit error rate against noise, measured", "[decode][tetra]") {
    // The payload under test rides in block 2 of a run of synchronisation
    // bursts, which is the only way to measure this mode's error rate through
    // its own framing: a bare pi/4-DQPSK stream has no training sequence, so
    // the burst decoder has nothing to lock onto and there is no alignment to
    // compare against.
    //
    // What that measures is 216 bits per 255 symbol burst, so the comparison
    // covers about two bits in five of what was sent. The rest is the
    // training sequences, the frequency correction field and the BSCH, all of
    // which are known patterns rather than payload.
    constexpr std::size_t kBursts = 24;
    constexpr std::size_t kPayload = decode::kTetraSyncBurstBlock2.length;

    std::vector<std::uint8_t> sent(kBursts * kPayload, 0);
    std::uint64_t state = 0x7E77'A000'1234'0001ULL;
    for (std::uint8_t& bit : sent) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        bit = static_cast<std::uint8_t>((state >> 41U) & 1ULL);
    }

    struct Point {
        double snr_db;
        double allowed_bit_error_rate;
    };
    // Differential detection costs about two decibels against coherent, and
    // pi/4-DQPSK carries two bits per symbol, so the low point here is at a
    // higher signal to noise than the two binary modes' and the allowance is
    // still looser.
    const Point points[] = {{30.0, 0.002}, {12.0, 0.25}};

    for (const Point& point : points) {
        INFO("signal to noise " << point.snr_db << " dB across the whole " << kRate
                                << " Hz sample rate");

        std::vector<std::uint8_t> stream;
        stream.reserve(kBursts * decode::kTetraBurstBits);
        for (std::size_t burst = 0; burst < kBursts; ++burst) {
            decode::TetraSyncPdu pdu = sample_pdu();
            // Clause 9 numbers frames 1 to 18, so this walks the field the
            // way a real downlink does rather than repeating one value.
            pdu.frame_number = static_cast<std::uint8_t>(1 + (burst % 18));

            auto bits = siggen::tetra_sync_burst_bits(pdu, 0xA11CE + burst);
            REQUIRE(bits.has_value());
            for (std::size_t i = 0; i < kPayload; ++i) {
                (*bits)[decode::kTetraSyncBurstBlock2.offset + i] = sent[burst * kPayload + i];
            }
            stream.insert(stream.end(), bits->begin(), bits->end());
        }

        siggen::TetraModConfig mod;
        mod.rate = kRate;
        auto samples = siggen::tetra_render_bits(mod, stream);
        REQUIRE(samples.has_value());

        const auto level = siggen::NoiseLevel::snr_in_full_sample_rate_db(point.snr_db);
        auto report = siggen::add_awgn(*samples, level, kRate, 0x0F1E'2D3C'4B5A'6978ULL);
        INFO((report.has_value() ? std::string{} : report.error().message));
        REQUIRE(report.has_value());

        decode::TetraConfig config;
        config.rate = kRate;
        auto decoder = decode::Tetra::create(config);
        REQUIRE(decoder.has_value());

        std::vector<decode::TetraBurst> found;
        REQUIRE(decoder->process(*samples, found).has_value());
        INFO("bursts found: " << found.size() << " of " << kBursts);
        REQUIRE_FALSE(found.empty());

        // Each recovered burst is matched to the one it came from by where it
        // started, which is exact: bursts are a fixed 255 symbols and the
        // decoder reports the symbol each one began on.
        std::size_t errors = 0;
        std::size_t compared = 0;
        std::size_t matched = 0;
        for (const decode::TetraBurst& burst : found) {
            const std::size_t index = (burst.first_symbol * 2) / decode::kTetraBurstBits;
            if (index >= kBursts) {
                continue;
            }
            ++matched;
            for (std::size_t i = 0; i < kPayload; ++i) {
                ++compared;
                errors += static_cast<std::size_t>(
                    burst.bits[decode::kTetraSyncBurstBlock2.offset + i] !=
                    sent[index * kPayload + i]);
            }
        }

        REQUIRE(compared > 0);
        const double ber = static_cast<double>(errors) / static_cast<double>(compared);
        INFO("compared " << compared << " payload bits across " << matched
                         << " bursts: bit error rate " << ber);
        CHECK(ber <= point.allowed_bit_error_rate);
    }
}
