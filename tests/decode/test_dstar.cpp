// D-STAR DV: the header chain against the JARL standard, and the demodulator
// against the transmitter.
//
// The same two groups as tests/decode/test_p25p1.cpp and for the same
// reasons: the codes are checked against what the document states, and the
// round trip stands in for the bit-exact scalar twin a decoder with no GPU
// kernel behind it does not have.
//
// ONE OF THESE CASES CHECKS A DEFECT IN THE STANDARD
//
// Clause Ap2.1's text and its own figure disagree about the convolutional
// code's generator, and the figure is the one that fits. That is recorded in
// core/decode/dv_codes.h and the case below pins the consequence: a rate 1/2
// constraint length 3 code encoding 330 bits to exactly 660, which is the
// length the Ap2.2 interleave matrix and the clause 4.1.2 frame diagram both
// independently state. If the text's 0x8408 were the generator none of those
// three numbers would agree.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "core/decode/dstar.h"
#include "core/decode/dv_codes.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dv_mod.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 48'000;

decode::DStarHeader sample_header() {
    decode::DStarHeader header;
    // Clause 4.1.1 c: bit 7 clear is voice, bit 6 set is addressed to a
    // repeater, and the low three bits are the null response code.
    header.flag1 = 0b0100'0000;
    header.flag2 = 0x00;
    header.flag3 = 0x00;
    header.destination_repeater = "JP1YIU A";
    header.departure_repeater = "JP1YIU G";
    header.companion = "CQCQCQ";
    header.own_callsign = "JA1RL";
    header.own_suffix = "MOBL";
    return header;
}

std::vector<std::array<std::uint8_t, decode::kDStarVoiceBits>> sample_voice(std::size_t frames,
                                                                            std::uint64_t seed) {
    std::vector<std::array<std::uint8_t, decode::kDStarVoiceBits>> out(frames);
    std::uint64_t state = seed;
    for (auto& frame : out) {
        for (std::uint8_t& bit : frame) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            bit = static_cast<std::uint8_t>((state >> 40U) & 1ULL);
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The header chain, against the document
// ---------------------------------------------------------------------------

TEST_CASE("the header encodes to the 660 bits three clauses agree on", "[decode][dstar]") {
    // Clause 4.1.2's frame diagram gives the radio header as 41 bytes and
    // labels the error-corrected span 660 bits. Ap2.1 step 3 adds two flush
    // bits. Ap2.2's interleave matrix has 660 cells. All three have to agree
    // and they do only for a rate 1/2 code: (41*8 + 2) * 2 = 660.
    CHECK(decode::kDStarHeaderBits == 328);
    CHECK(decode::kDStarHeaderEncodedBits == 660);

    const std::vector<std::uint16_t> map = decode::dstar_interleave_map();
    CHECK(map.size() == decode::kDStarHeaderEncodedBits);

    // A permutation, which is what an interleaver has to be. The matrix in
    // Ap2.2 is 24 rows by 28 columns with the last four rows a column short,
    // so every index from 0 to 659 appears exactly once.
    std::set<std::uint16_t> seen(map.begin(), map.end());
    CHECK(seen.size() == map.size());
    CHECK(*seen.begin() == 0);
    CHECK(*seen.rbegin() == decode::kDStarHeaderEncodedBits - 1);

    // The matrix's first printed row is 0, 24, 48, 72 and so on, which is the
    // column stride of 24 the figure shows.
    CHECK(map[0] == 0);
    CHECK(map[1] == 24);
    CHECK(map[2] == 48);
    CHECK(map[27] == 648);

    // Row 12 in the figure ends at 636 in column 26, because column 27 would
    // be index 660 and the matrix stops at 659. Row 12 starts at transmitted
    // position 12 * 28 = 336.
    CHECK(map[336] == 12);
    CHECK(map[336 + 26] == 636);
}

TEST_CASE("the scrambler is the Ap1.1 polynomial and is self-inverse", "[decode][dstar]") {
    // S(x) = x^7 + x^4 + 1, initialised to all ones. Applying it twice
    // returns the input, which is what makes one function serve both ends.
    std::vector<std::uint8_t> original(200, 0);
    for (std::size_t i = 0; i < original.size(); ++i) {
        original[i] = static_cast<std::uint8_t>((i * 7 + 3) & 1U);
    }

    std::vector<std::uint8_t> working = original;
    decode::dstar_scramble(working);
    CHECK(working != original);
    decode::dstar_scramble(working);
    CHECK(working == original);

    // The sequence itself: scrambling a run of zeros exposes it. A maximal
    // length sequence from a 7-stage register repeats every 127 bits and
    // never goes all-zero.
    std::vector<std::uint8_t> sequence(254, 0);
    decode::dstar_scramble(sequence);
    for (std::size_t i = 0; i < 127; ++i) {
        INFO("sequence position " << i);
        CHECK(sequence[i] == sequence[i + 127]);
    }
    std::size_t ones = 0;
    for (std::size_t i = 0; i < 127; ++i) {
        ones += sequence[i];
    }
    INFO("ones in one period of 127: " << ones);
    CHECK(ones == 64);
}

TEST_CASE("the header round trips through the FEC chain untouched", "[decode][dstar]") {
    const decode::DStarHeader header = sample_header();
    const std::array<std::uint8_t, 41> bytes = decode::dstar_header_bytes(header);

    // Clause 4.1.1 k: the FCS covers flag 1 through own callsign 2, which is
    // the first 39 bytes, and occupies the last two.
    const std::uint16_t fcs = decode::dstar_header_fcs(std::span(bytes).subspan(0, 39));
    CHECK(bytes[39] == static_cast<std::uint8_t>(fcs & 0xFFU));
    CHECK(bytes[40] == static_cast<std::uint8_t>((fcs >> 8U) & 0xFFU));

    auto encoded = decode::dstar_encode_header(bytes);
    INFO((encoded.has_value() ? std::string{} : encoded.error().message));
    REQUIRE(encoded.has_value());
    REQUIRE(encoded->size() == decode::kDStarHeaderEncodedBits);

    std::vector<float> soft(encoded->size(), 0.0F);
    for (std::size_t i = 0; i < encoded->size(); ++i) {
        soft[i] = (*encoded)[i] ? -1.0F : 1.0F;
    }

    auto decoded = decode::dstar_decode_header(soft);
    INFO((decoded.has_value() ? std::string{} : decoded.error().message));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == bytes);

    const decode::DStarHeader parsed = decode::dstar_parse_header(*decoded);
    CHECK(parsed.fcs_valid);
    CHECK(parsed.destination_repeater == "JP1YIU A");
    CHECK(parsed.departure_repeater == "JP1YIU G");
    CHECK(parsed.companion == "CQCQCQ");
    CHECK(parsed.own_callsign == "JA1RL");
    CHECK(parsed.own_suffix == "MOBL");
    CHECK_FALSE(parsed.flags.data);
    CHECK(parsed.flags.via_repeater);
    CHECK(parsed.flags.response == 0);
}

TEST_CASE("the header FEC corrects a burst the interleaver spreads", "[decode][dstar]") {
    // The whole point of the Ap2.2 interleave is that a burst on the air
    // arrives at the Viterbi decoder spread out. Twelve consecutive
    // transmitted bits are twelve bits at least 24 apart in the code word,
    // which a constraint length 3 code handles comfortably.
    const std::array<std::uint8_t, 41> bytes = decode::dstar_header_bytes(sample_header());
    auto encoded = decode::dstar_encode_header(bytes);
    REQUIRE(encoded.has_value());

    std::vector<float> soft(encoded->size(), 0.0F);
    for (std::size_t i = 0; i < encoded->size(); ++i) {
        soft[i] = (*encoded)[i] ? -1.0F : 1.0F;
    }
    for (std::size_t i = 300; i < 312; ++i) {
        soft[i] = -soft[i];
    }

    auto decoded = decode::dstar_decode_header(soft);
    REQUIRE(decoded.has_value());
    const decode::DStarHeader parsed = decode::dstar_parse_header(*decoded);
    INFO("callsign came back as '" << parsed.own_callsign << "'");
    CHECK(parsed.fcs_valid);
    CHECK(parsed.own_callsign == "JA1RL");
}

// ---------------------------------------------------------------------------
// The round trip
// ---------------------------------------------------------------------------

TEST_CASE("a D-STAR voice transmission round trips through GMSK", "[decode][dstar]") {
    siggen::DStarMessage message;
    message.header = sample_header();
    message.voice_frames = sample_voice(25, 0xD57A'4001ULL);

    siggen::DStarModConfig mod;
    mod.rate = kRate;
    auto samples = siggen::dstar_render(mod, message);
    INFO((samples.has_value() ? std::string{} : samples.error().message));
    REQUIRE(samples.has_value());
    // A tenth of a second of carrier-off after the last frame, as any
    // receiver's stream has: the receive filter and the timing recovery hold
    // the last few bits back until samples behind them arrive.
    samples->insert(samples->end(), static_cast<std::size_t>(kRate / 10), dsp::Complex32{});

    decode::DStarConfig config;
    config.rate = kRate;
    auto decoder = decode::DStar::create(config);
    INFO((decoder.has_value() ? std::string{} : decoder.error().message));
    REQUIRE(decoder.has_value());

    std::vector<decode::DStarTransmission> transmissions;
    REQUIRE(decoder->process(*samples, transmissions).has_value());

    INFO("transmissions recovered: " << transmissions.size());
    REQUIRE_FALSE(transmissions.empty());

    // The transmission arrives as a piece with the header and the first
    // superframe, then a piece per superframe after it; dstar.h says why.
    decode::DStarTransmission tx = transmissions.front();
    for (std::size_t i = 1; i < transmissions.size(); ++i) {
        REQUIRE_FALSE(transmissions[i].header.has_value());
        CHECK(transmissions[i].first_bit == tx.first_bit);
        tx.frames.insert(tx.frames.end(), transmissions[i].frames.begin(),
                         transmissions[i].frames.end());
        tx.ended = transmissions[i].ended;
    }
    INFO("sync correlation " << tx.sync_score << ", frames " << tx.frames.size());
    REQUIRE(tx.header.has_value());
    CHECK(transmissions.front().frames.size() == decode::kDStarResyncInterval);

    // Clause 4.1.2 h: dstar_render ends the transmission with the last
    // frame, and every voice frame before it comes back.
    CHECK(tx.ended);
    CHECK(tx.frames.size() == message.voice_frames.size());
    CHECK(tx.header->fcs_valid);
    CHECK(tx.header->own_callsign == "JA1RL");
    CHECK(tx.header->companion == "CQCQCQ");
    CHECK(tx.header->destination_repeater == "JP1YIU A");

    // Clause 4.1.2 c: the first data frame and every 21st carry the
    // resynchronisation signal, which is what lets a receiver join a
    // transmission already in progress.
    REQUIRE(tx.frames.size() >= 22);
    CHECK(tx.frames[0].carried_resync);
    CHECK_FALSE(tx.frames[1].carried_resync);
    CHECK(tx.frames[decode::kDStarResyncInterval].carried_resync);

    // The 72 payload bits come back unchanged. They are not interpreted:
    // AMBE has no published algorithm and docs/modes.md says so.
    std::size_t compared = 0;
    std::size_t errors = 0;
    for (std::size_t i = 0; i < tx.frames.size() && i < message.voice_frames.size(); ++i) {
        for (std::size_t b = 0; b < decode::kDStarVoiceBits; ++b) {
            ++compared;
            errors += static_cast<std::size_t>(tx.frames[i].voice[b] !=
                                               message.voice_frames[i][b]);
        }
    }
    INFO("payload bits compared " << compared << ", errors " << errors);
    REQUIRE(compared > 1'000);
    CHECK(errors == 0);
}

TEST_CASE("D-STAR bit error rate against noise, measured", "[decode][dstar]") {
    constexpr std::size_t kBits = 12'000;
    std::vector<std::uint8_t> sent(kBits, 0);
    std::uint64_t state = 0xD57A'0F0F'1234'0001ULL;
    for (std::uint8_t& bit : sent) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        bit = static_cast<std::uint8_t>((state >> 37U) & 1ULL);
    }

    struct Point {
        double snr_db;
        double allowed_bit_error_rate;
    };
    // Measured on 2026-09-21: 0 bit errors in 11994 at 30 dB, and a bit
    // error rate of 0.056 at 2 dB.
    const Point points[] = {{30.0, 0.002}, {2.0, 0.09}};

    for (const Point& point : points) {
        INFO("signal to noise " << point.snr_db << " dB across the whole " << kRate
                                << " Hz sample rate");

        siggen::DStarModConfig mod;
        mod.rate = kRate;
        auto samples = siggen::dstar_render_bits(mod, sent);
        REQUIRE(samples.has_value());

        const auto level = siggen::NoiseLevel::snr_in_full_sample_rate_db(point.snr_db);
        auto report = siggen::add_awgn(*samples, level, kRate, 0x1234'5678'9ABC'DEF0ULL);
        INFO((report.has_value() ? std::string{} : report.error().message));
        REQUIRE(report.has_value());

        decode::DStarConfig config;
        config.rate = kRate;
        auto decoder = decode::DStar::create(config);
        REQUIRE(decoder.has_value());

        std::vector<decode::DStarTransmission> transmissions;
        REQUIRE(decoder->process(*samples, transmissions).has_value());

        const std::span<const std::uint8_t> got = decoder->last_bits();
        REQUIRE(got.size() > kBits / 2);

        // Find the alignment by correlating the first 64 sent bits, the same
        // way the P25 case does and for the same reason: the filters have a
        // group delay and the timing loop has an acquisition.
        std::vector<float> recovered(got.size(), 0.0F);
        for (std::size_t i = 0; i < got.size(); ++i) {
            recovered[i] = got[i] ? 1.0F : -1.0F;
        }
        std::vector<float> head(128, 0.0F);
        for (std::size_t i = 0; i < head.size(); ++i) {
            head[i] = sent[i] ? 1.0F : -1.0F;
        }
        auto hit = decode::correlate_pattern(recovered, head);
        REQUIRE(hit.has_value());
        INFO("stream found at recovered bit " << hit->offset << ", correlation " << hit->score);
        REQUIRE(std::abs(hit->score) > 0.5);

        const std::size_t compare = std::min(sent.size(), recovered.size() - hit->offset);
        REQUIRE(compare > kBits / 2);

        std::size_t errors = 0;
        for (std::size_t i = 0; i < compare; ++i) {
            const std::uint8_t value = hit->inverted
                                           ? static_cast<std::uint8_t>(1U - got[hit->offset + i])
                                           : got[hit->offset + i];
            errors += static_cast<std::size_t>(value != sent[i]);
        }
        const double ber = static_cast<double>(errors) / static_cast<double>(compare);
        INFO("compared " << compare << " bits: bit error rate " << ber);
        CHECK(ber <= point.allowed_bit_error_rate);
    }
}
