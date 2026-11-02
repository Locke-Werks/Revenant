// DECT: the S-field and R-CRC against ETSI EN 300 175-2 and -3 V2.9.1, and
// the decoder against the transmitter in core/dsp/synth/dect_mod.h.
//
// Neither part prints an A-field with its R-CRC, so the CRC case checks the
// register implementation against the clause 6.2.5.2 definition computed by
// long division, and the generator against its printed octal form. Error
// rates are measured, printed, and loosely asserted.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/decode/dect.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dect_mod.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 4'608'000;
constexpr std::array<std::uint8_t, 4> kX{1, 0, 1, 1};

std::vector<std::uint8_t> b_bits(std::uint64_t seed) {
    std::vector<std::uint8_t> out(decode::kDectBFieldBits);
    for (auto& b : out) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        b = static_cast<std::uint8_t>((seed >> 40U) & 1U);
    }
    return out;
}

// An RFPI NT tail: TA 011, BA 000.
constexpr std::uint64_t kRfpi = 0x80'1234'5678ULL;  // E bit a8 set

// A QT static system information tail, Figure 7.6: QH 000, NR 0, SN 5, SP 0,
// esc 0, Txs 1, Mc 0, RF carriers 0x3FF, spr 00, CN 7, ext 0, spr 0, PSCN 3.
std::uint64_t static_info_tail() {
    std::uint64_t t = 0;
    const auto put = [&t](unsigned value, unsigned bits) { t = (t << bits) | value; };
    put(0, 3);      // QH a8-a10
    put(0, 1);      // NR a11
    put(5, 4);      // SN a12-a15
    put(0, 2);      // SP a16-a17
    put(0, 1);      // esc a18
    put(1, 2);      // Txs a19-a20
    put(0, 1);      // Mc a21
    put(0x3FF, 10); // RF carriers a22-a31
    put(0, 2);      // spr a32-a33
    put(7, 6);      // CN a34-a39
    put(0, 1);      // ext a40
    put(0, 1);      // spr a41
    put(3, 6);      // PSCN a42-a47
    return t;
}

std::vector<dsp::Complex32> burst(decode::DectPolarity pol, std::uint64_t a_field, const siggen::DectTxConfig& tx,
                                  std::size_t gap = 4000, std::uint64_t seed = 1) {
    const auto b = b_bits(seed);
    const auto bits = siggen::dect_p32_bits(pol, a_field, b, kX);
    std::vector<dsp::Complex32> out(gap);
    const auto body = siggen::dect_modulate(bits, tx).value();
    out.insert(out.end(), body.begin(), body.end());
    out.insert(out.end(), gap, dsp::Complex32{});
    return out;
}

std::vector<decode::DectBurst> decode_all(const std::vector<dsp::Complex32>& samples, std::size_t chunk = 0,
                                          dsp::SampleRate rate = kRate, double offset = 0.0) {
    decode::DectConfig c;
    c.sample_rate = rate;
    c.frequency_offset_hz = offset;
    auto decoder = decode::DectDecoder::create(c).value();
    std::vector<decode::DectBurst> out;
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

// Clause 6.2.5.2 by long division of r(x) x^16 over the 17-bit generator.
std::uint16_t rcrc_by_division(std::uint64_t data48) {
    std::array<unsigned, 64> r{};
    for (unsigned i = 0; i < 48; ++i) {
        r[i] = static_cast<unsigned>((data48 >> (47U - i)) & 1U);
    }
    for (unsigned i = 0; i < 48; ++i) {
        if (r[i] != 0) {
            for (unsigned k = 0; k < 17; ++k) {
                r[i + k] ^= (decode::kDectRCrcGenerator >> (16U - k)) & 1U;
            }
        }
    }
    unsigned rem = 0;
    for (unsigned i = 48; i < 64; ++i) {
        rem = (rem << 1U) | r[i];
    }
    return static_cast<std::uint16_t>(rem ^ 1U);
}

}  // namespace

TEST_CASE("DECT constants follow EN 300 175-2 clause 4.6 and -3 clause 6.2.5.2", "[decode][dect]") {
    // 202 611 octal.
    CHECK(decode::kDectRCrcGenerator == 0202611U);
    // The PP pattern is the inverse of the RFP pattern, and both are balanced.
    CHECK(decode::kDectSFieldPp == 0b0101'0101'0101'0101'0001'0110'0111'0101U);
    CHECK(std::popcount(decode::kDectSFieldRfp) == 16);
    for (const std::uint64_t data : {0ULL, 1ULL, 0x6'0000'0000'00ULL, 0xFFFF'FFFF'FFFFULL, 0x3'1234'5678'9AULL}) {
        INFO("data " << data);
        CHECK(decode::dect_rcrc(data) == rcrc_by_division(data));
    }
    // The inverted last bit means an all-zero A-field is not a codeword.
    CHECK(decode::dect_rcrc(0) == 1U);
}

TEST_CASE("a DECT RFP burst round trips with its RFPI", "[decode][dect]") {
    const std::uint64_t a = siggen::dect_a_field(3, false, 0, false, kRfpi);
    const auto out = decode_all(burst(decode::DectPolarity::Rfp, a, {}));
    REQUIRE(out.size() == 1);
    CHECK(out[0].crc_ok);
    CHECK(out[0].polarity == decode::DectPolarity::Rfp);
    CHECK(out[0].a_field == a);
    CHECK(out[0].tail_id == decode::DectTail::Nt);
    REQUIRE(out[0].rfpi.has_value());
    CHECK(*out[0].rfpi == kRfpi);
    CHECK(out[0].rfpi_e);
    CHECK(out[0].b_field_present);
    // The burst starts after the 4000-sample gap; s0 centre is half a symbol in.
    INFO("sample " << out[0].sample);
    CHECK(out[0].sample >= 4000);
    CHECK(out[0].sample <= 4006);
    CHECK(out[0].b_field_sample == out[0].sample + 96 * 4);
}

TEST_CASE("a DECT PP burst and QT static system information", "[decode][dect]") {
    const std::uint64_t a = siggen::dect_a_field(4, false, 7, false, static_info_tail());
    const auto out = decode_all(burst(decode::DectPolarity::Pp, a, {}));
    REQUIRE(out.size() == 1);
    CHECK(out[0].polarity == decode::DectPolarity::Pp);
    CHECK_FALSE(out[0].b_field_present);
    REQUIRE(out[0].qt.has_value());
    CHECK(out[0].qt->qh == 0);
    REQUIRE(out[0].qt->static_info.has_value());
    const auto& s = *out[0].qt->static_info;
    CHECK(s.slot_number == 5);
    CHECK(s.transceivers == 1);
    CHECK(s.rf_carriers == 0x3FF);
    CHECK(s.carrier_number == 7);
    CHECK(s.primary_scan_carrier == 3);
}

TEST_CASE("DECT tolerates offset, deviation limits and a 2.4 MS/s tap", "[decode][dect]") {
    const std::uint64_t a = siggen::dect_a_field(3, false, 0, false, kRfpi);
    siggen::DectTxConfig off;
    off.frequency_offset_hz = 60e3;
    auto out = decode_all(burst(decode::DectPolarity::Rfp, a, off));
    REQUIRE(out.size() == 1);
    INFO("estimated " << out[0].frequency_hz);
    CHECK(std::abs(out[0].frequency_hz - 60e3) < 15e3);

    // A carrier 600 kHz off centre, mixed down by the configured offset.
    siggen::DectTxConfig far;
    far.frequency_offset_hz = 600e3;
    out = decode_all(burst(decode::DectPolarity::Rfp, a, far), 0, kRate, 600e3);
    CHECK(out.size() == 1);

    // EN 300 175-2 clause 5.4.3, case B limits.
    for (const double dev : {202e3, 403e3}) {
        siggen::DectTxConfig d;
        d.deviation_hz = dev;
        INFO("deviation " << dev);
        CHECK(decode_all(burst(decode::DectPolarity::Rfp, a, d)).size() == 1);
    }

    siggen::DectTxConfig r24;
    r24.sample_rate = 2'400'000;
    out = decode_all(burst(decode::DectPolarity::Rfp, a, r24, 2000), 0, 2'400'000);
    REQUIRE(out.size() == 1);
    CHECK(out[0].a_field == a);
}

TEST_CASE("the DECT receiver does not depend on how its input is blocked", "[decode][dect]") {
    std::vector<dsp::Complex32> samples;
    for (std::uint64_t i = 0; i < 4; ++i) {
        const std::uint64_t a = siggen::dect_a_field(3, false, 0, false, kRfpi + i);
        const auto one = burst(i % 2 == 0 ? decode::DectPolarity::Rfp : decode::DectPolarity::Pp, a, {}, 3000, i);
        samples.insert(samples.end(), one.begin(), one.end());
    }
    REQUIRE(siggen::add_awgn(samples, siggen::NoiseLevel::snr_in_reference_bandwidth_db(18.0, 1'152'000), kRate,
                             0x5EED'DEC7ULL)
                .has_value());
    const auto whole = decode_all(samples);
    CHECK(whole.size() == 4);
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{13}, std::size_t{333}, std::size_t{4099}}) {
        const auto split = decode_all(samples, chunk);
        INFO("chunk " << chunk);
        REQUIRE(split.size() == whole.size());
        for (std::size_t i = 0; i < whole.size(); ++i) {
            CHECK(split[i].a_field == whole[i].a_field);
            CHECK(split[i].sample == whole[i].sample);
        }
    }
}

TEST_CASE("DECT A-field error rate against noise, measured", "[decode][dect]") {
    constexpr int kTrials = 100;
    const std::uint64_t a = siggen::dect_a_field(3, false, 0, false, kRfpi);
    double previous = 1.0;
    for (const double snr : {5.0, 8.0, 11.0, 14.0, 18.0}) {
        int lost = 0;
        for (int trial = 0; trial < kTrials; ++trial) {
            auto samples = burst(decode::DectPolarity::Rfp, a, {}, 1000, static_cast<std::uint64_t>(trial));
            REQUIRE(siggen::add_awgn(samples, siggen::NoiseLevel::snr_in_reference_bandwidth_db(snr, 1'152'000),
                                     kRate, 0x5EED'DE00ULL + static_cast<std::uint64_t>(trial))
                        .has_value());
            const auto out = decode_all(samples);
            if (out.size() != 1 || out[0].a_field != a) {
                ++lost;
            }
        }
        const double per = static_cast<double>(lost) / kTrials;
        INFO(snr << " dB in 1.152 MHz: A-field error rate " << per << " over " << kTrials);
        CHECK(per <= previous + 0.1);
        previous = per;
        if (snr >= 18.0) {
            CHECK(per <= 0.1);
        }
    }
}
