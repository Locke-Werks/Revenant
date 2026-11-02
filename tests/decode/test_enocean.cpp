// EnOcean ERP1: the codes against EnOcean Radio Protocol 1 V1.2, and the
// decoder against the transmitter in core/dsp/synth/enocean_mod.h.
//
// The document prints no sample telegram with its hash, so the hash cases
// check the two algorithms against their clause 4.3 definitions computed a
// second way, and the encapsulation case uses Figure 3's example DESTID.
// Error rates are measured, printed, and loosely asserted.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include "core/decode/enocean.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/enocean_mod.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 1'000'000;

// A 4BS-shaped telegram: RORG 0xA5, four data bytes, CRC selected by STATUS.
std::vector<std::uint8_t> sample_subtelegram(std::uint8_t status = 0x80) {
    const std::array<std::uint8_t, 4> data{0x10, 0x08, 0x46, 0x80};
    return siggen::enocean_subtelegram(0xA5, data, 0x0180'2B3CU, status);
}

std::vector<dsp::Complex32> burst(const std::vector<std::uint8_t>& sub, const siggen::EnoceanTxConfig& tx,
                                  std::size_t gap = 3000) {
    std::vector<dsp::Complex32> out(gap);
    const auto body = siggen::enocean_modulate(sub, tx).value();
    out.insert(out.end(), body.begin(), body.end());
    out.insert(out.end(), gap, dsp::Complex32{});
    return out;
}

std::vector<decode::EnoceanTelegram> decode_all(const std::vector<dsp::Complex32>& samples,
                                                std::size_t chunk = 0, dsp::SampleRate rate = kRate) {
    auto decoder = decode::EnoceanDecoder::create({rate, decode::kEnoceanBitRate}).value();
    std::vector<decode::EnoceanTelegram> out;
    if (chunk == 0) {
        decoder.process(samples, out);
        return out;
    }
    for (std::size_t i = 0; i < samples.size(); i += chunk) {
        const std::size_t n = std::min(chunk, samples.size() - i);
        decoder.process(dsp::ConstComplexSpan(samples.data() + i, n), out);
    }
    return out;
}

// Clause 4.3.3 as written: polynomial long division, bit by bit, of x^8 times
// the running value.
std::uint8_t crc_by_division(const std::vector<std::uint8_t>& bytes) {
    unsigned r = 0;
    for (const std::uint8_t b : bytes) {
        unsigned v = (r ^ b) << 8U;
        for (int bit = 15; bit >= 8; --bit) {
            if ((v >> static_cast<unsigned>(bit)) & 1U) {
                v ^= 0x107U << static_cast<unsigned>(bit - 8);
            }
        }
        r = v & 0xFFU;
    }
    return static_cast<std::uint8_t>(r);
}

}  // namespace

TEST_CASE("EnOcean hashes follow ERP1 clause 4.3", "[decode][enocean]") {
    const std::vector<std::uint8_t> body{0xA5, 0x10, 0x08, 0x46, 0x80, 0x01, 0x80, 0x2B, 0x3C, 0x80};
    CHECK(decode::enocean_crc8(body) == crc_by_division(body));
    unsigned sum = 0;
    for (const auto b : body) {
        sum += b;
    }
    CHECK(decode::enocean_checksum(body) == static_cast<std::uint8_t>(sum % 256U));

    const auto crc = sample_subtelegram(0x80);
    CHECK(crc.back() == crc_by_division(std::vector<std::uint8_t>(crc.begin(), crc.end() - 1)));
    const auto sum_sub = sample_subtelegram(0x00);
    CHECK(sum_sub.back() == decode::enocean_checksum(std::span(sum_sub.data(), sum_sub.size() - 1)));
}

TEST_CASE("EnOcean 8-to-12 coding follows the clause 1.7 table", "[decode][enocean]") {
    // 0xA5 = 1010 0101: 1 0 1 !1 0 0 1 !1 0 1 then 01 or 10.
    CHECK(decode::enocean_encode_byte(0xA5, false) == 0b1010'0010'0101U);
    CHECK(decode::enocean_encode_byte(0xA5, true) == 0b1010'0010'0110U);
    // Every encoding has a transition in each half, which is the DC argument
    // the clause makes for the code.
    for (unsigned b = 0; b < 256; ++b) {
        const unsigned g = decode::enocean_encode_byte(static_cast<std::uint8_t>(b), false);
        CHECK(((g >> 9U) & 1U) != ((g >> 8U) & 1U));
        CHECK(((g >> 5U) & 1U) != ((g >> 4U) & 1U));
    }
    // Clauses 1.8 and 1.9: the frame opens 10101010 then 1001.
    const auto bits = siggen::enocean_frame_bits(sample_subtelegram());
    const std::vector<std::uint8_t> head{1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1};
    CHECK(std::vector<std::uint8_t>(bits.begin(), bits.begin() + 12) == head);
    CHECK(bits.size() == 12 + 12 * sample_subtelegram().size());
}

TEST_CASE("an EnOcean subtelegram round trips", "[decode][enocean]") {
    for (const std::uint8_t status : {std::uint8_t{0x00}, std::uint8_t{0x80}, std::uint8_t{0x81}}) {
        const auto sub = sample_subtelegram(status);
        const auto out = decode_all(burst(sub, {}));
        REQUIRE(out.size() == 1);
        CHECK(out[0].bytes == sub);
        CHECK(out[0].rorg == 0xA5);
        CHECK(out[0].sender_id == 0x0180'2B3CU);
        CHECK(out[0].data == std::vector<std::uint8_t>{0x10, 0x08, 0x46, 0x80});
        CHECK(out[0].crc8 == ((status & 0x80U) != 0));
        CHECK(out[0].repeater_level == (status & 0x0FU));
        CHECK_FALSE(out[0].destination_id.has_value());
    }
}

TEST_CASE("an addressed EnOcean telegram yields Figure 3's DESTID", "[decode][enocean]") {
    // Figure 3: 0xA6, RORG, DATA, DESTID 0xF1F2F3F4, TXID, STATUS, HASH.
    const std::vector<std::uint8_t> inner{0xD2, 0x01, 0x02, 0xF1, 0xF2, 0xF3, 0xF4};
    const auto sub = siggen::enocean_subtelegram(decode::kEnoceanRorgAddressed, inner, 0xFFAA0011U, 0x80);
    const auto out = decode_all(burst(sub, {}));
    REQUIRE(out.size() == 1);
    CHECK(out[0].inner_rorg == 0xD2);
    REQUIRE(out[0].destination_id.has_value());
    CHECK(*out[0].destination_id == 0xF1F2F3F4U);
    CHECK(out[0].sender_id == 0xFFAA0011U);
    CHECK(out[0].data == std::vector<std::uint8_t>{0x01, 0x02});
}

TEST_CASE("EnOcean triplicate subtelegrams are flagged as repeats", "[decode][enocean]") {
    const auto sub = sample_subtelegram();
    std::vector<dsp::Complex32> samples;
    for (int i = 0; i < 3; ++i) {
        const auto one = burst(sub, {}, 2000);
        samples.insert(samples.end(), one.begin(), one.end());
    }
    const auto out = decode_all(samples);
    REQUIRE(out.size() == 3);
    CHECK_FALSE(out[0].repeat);
    CHECK(out[1].repeat);
    CHECK(out[2].repeat);
}

TEST_CASE("EnOcean tolerates offset, rate error and other sample rates", "[decode][enocean]") {
    const auto sub = sample_subtelegram();
    siggen::EnoceanTxConfig tx;
    tx.frequency_offset_hz = 150e3;
    CHECK(decode_all(burst(sub, tx)).size() == 1);
    // Clause 1.6: 5 % data rate tolerance.
    for (const double scale : {0.95, 1.05}) {
        siggen::EnoceanTxConfig skew;
        skew.bit_rate = decode::kEnoceanBitRate * scale;
        INFO("bit rate scale " << scale);
        CHECK(decode_all(burst(sub, skew)).size() == 1);
    }
    for (const dsp::SampleRate rate : {dsp::SampleRate{500'000}, dsp::SampleRate{2'400'000}}) {
        siggen::EnoceanTxConfig r;
        r.sample_rate = rate;
        INFO("rate " << rate);
        const auto out = decode_all(burst(sub, r), 0, rate);
        REQUIRE(out.size() == 1);
        CHECK(out[0].bytes == sub);
    }
    // Clause 1.5: the depth limits.
    for (const double depth : {20.0, 36.0}) {
        siggen::EnoceanTxConfig d;
        d.depth_db = depth;
        INFO("depth " << depth);
        CHECK(decode_all(burst(sub, d)).size() == 1);
    }
}

TEST_CASE("the EnOcean receiver does not depend on how its input is blocked", "[decode][enocean]") {
    std::vector<dsp::Complex32> samples;
    for (std::uint8_t s = 0; s < 4; ++s) {
        const auto one = burst(sample_subtelegram(static_cast<std::uint8_t>(0x80U | s)), {}, 1500);
        samples.insert(samples.end(), one.begin(), one.end());
    }
    REQUIRE(siggen::add_awgn(samples, siggen::NoiseLevel::snr_in_reference_bandwidth_db(16.0, 250000), kRate,
                             0x5EED'E001ULL)
                .has_value());
    const auto whole = decode_all(samples);
    CHECK(whole.size() == 4);
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{97}, std::size_t{1000}}) {
        const auto split = decode_all(samples, chunk);
        INFO("chunk " << chunk);
        REQUIRE(split.size() == whole.size());
        for (std::size_t i = 0; i < whole.size(); ++i) {
            CHECK(split[i].bytes == whole[i].bytes);
            CHECK(split[i].sample == whole[i].sample);
        }
    }
}

TEST_CASE("EnOcean packet error rate against noise, measured", "[decode][enocean]") {
    constexpr int kTrials = 100;
    const auto sub = sample_subtelegram();
    double previous = 1.0;
    for (const double snr : {8.0, 11.0, 14.0, 18.0}) {
        int lost = 0;
        for (int trial = 0; trial < kTrials; ++trial) {
            auto samples = burst(sub, {}, 1000);
            REQUIRE(siggen::add_awgn(samples, siggen::NoiseLevel::snr_in_reference_bandwidth_db(snr, 250000),
                                     kRate, 0x5EED'E100ULL + static_cast<std::uint64_t>(trial))
                        .has_value());
            const auto out = decode_all(samples);
            if (out.size() != 1 || out[0].bytes != sub) {
                ++lost;
            }
        }
        const double per = static_cast<double>(lost) / kTrials;
        INFO(snr << " dB in 250 kHz: packet error rate " << per << " over " << kTrials);
        CHECK(per <= previous + 0.1);
        previous = per;
        if (snr >= 18.0) {
            CHECK(per <= 0.1);
        }
    }
}
