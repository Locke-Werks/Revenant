// Cospas-Sarsat 406 MHz first generation, core/decode/cospas_sarsat.h,
// against C/S T.001 Issue 4 Rev. 7's worked examples (Annex B) and through
// the burst transmitter of core/dsp/synth/cospas_sarsat_mod.h with noise,
// Doppler, its drift and an off-nominal bit rate.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "core/decode/bch.h"
#include "core/decode/cospas_sarsat.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/cospas_sarsat_mod.h"

using namespace revenant;
using namespace revenant::decode;

namespace {

std::vector<std::uint8_t> from_bits(const std::string& s) {
    std::vector<std::uint8_t> out;
    for (const char c : s) {
        if (c == '0' || c == '1') {
            out.push_back(static_cast<std::uint8_t>(c - '0'));
        }
    }
    return out;
}

std::vector<std::uint8_t> from_hex(const std::string& s) {
    std::vector<std::uint8_t> out;
    for (const char c : s) {
        if (c == ' ') {
            continue;
        }
        const unsigned v = c <= '9' ? static_cast<unsigned>(c - '0') : static_cast<unsigned>(c - 'A' + 10);
        for (int i = 3; i >= 0; --i) {
            out.push_back(static_cast<std::uint8_t>((v >> i) & 1U));
        }
    }
    return out;
}

std::vector<std::uint8_t> sync_header() {
    // 2.2.8.1 and 2.2.8.2: fifteen ones, then 000101111.
    return from_bits("111111111111111000101111");
}

}  // namespace

TEST_CASE("Cospas-Sarsat BCH generators are the ones Annex B prints", "[decode][cospas]") {
    const auto g1 = BchCodec::create({7, 0x89, 7, {}}).value().generator();
    CHECK(g1 == from_bits("1001101101100111100011"));
    // (1 + x3 + x4 + x5 + x8 + x10 + x12), highest power first.
    const auto g2 = BchCodec::create({6, 0x43, 5, {}}).value().generator();
    CHECK(g2 == from_bits("1010100111001"));
}

TEST_CASE("Cospas-Sarsat Annex B1 short message decodes as printed", "[decode][cospas]") {
    auto bits = sync_header();
    const auto body = from_hex("56E6804002202009655250");  // bits 25-112
    bits.insert(bits.end(), body.begin(), body.end());
    REQUIRE(bits.size() == 112);
    // The printed BCH-1, bits 86-106.
    const std::vector<std::uint8_t> bch(bits.begin() + 85, bits.begin() + 106);
    CHECK(bch == from_bits("001011001010101001001"));

    auto refilled = bits;
    std::fill(refilled.begin() + 85, refilled.begin() + 106, 0);
    cs_fill_bch(refilled);
    CHECK(refilled == bits);

    const auto m = cs_parse(bits).value();
    CHECK_FALSE(m.long_message);
    CHECK(m.user_protocol);
    CHECK(m.country_code == 366);
    CHECK(m.protocol == CsProtocol::Serial);
    CHECK(m.hex_id == "ADCD00800440401");
    CHECK(*m.bch1 == 0);
    CHECK(m.identity.find("float free EPIRB") != std::string::npos);
    CHECK(m.homing == 1u);  // 121.5 MHz homing, per the example's table
    std::printf("Annex B1: %s, %s, hex %s\n", cs_protocol_name(m.protocol), m.identity.c_str(), m.hex_id.c_str());

    // Three errors anywhere in bits 25-106 are corrected (section 3.2).
    auto hit = bits;
    hit[30] ^= 1U;
    hit[70] ^= 1U;
    hit[100] ^= 1U;
    const auto fixed = cs_parse(hit).value();
    CHECK(*fixed.bch1 == 3);
    CHECK(fixed.hex_id == "ADCD00800440401");
}

TEST_CASE("Cospas-Sarsat Annex B2 PDF-2 BCH is the printed one", "[decode][cospas]") {
    std::vector<std::uint8_t> bits(144, 0);
    const auto head = sync_header();
    std::copy(head.begin(), head.end(), bits.begin());
    bits[24] = 1;
    const auto pdf2 = from_bits("10 0101 0111 0000 0000 0001 0111");  // bits 107-132
    std::copy(pdf2.begin(), pdf2.end(), bits.begin() + 106);
    cs_fill_bch(bits);
    const std::vector<std::uint8_t> bch2(bits.begin() + 132, bits.end());
    CHECK(bch2 == from_bits("0001 0101 0001"));
}

TEST_CASE("Cospas-Sarsat location protocols round trip positions", "[decode][cospas]") {
    // Standard location, EPIRB MMSI (Table A2-B 0010): MMSI 366123456 is
    // 123456 in bits 41-60 and beacon 3 in 61-64.
    const auto std_bits =
        siggen::cs_standard_location(366, 0b0010, (123456U << 4U) | 3U, 43.56055, -1.48083, true, true);
    const auto m = cs_parse(std_bits).value();
    CHECK(m.protocol == CsProtocol::StandardMmsi);
    CHECK(m.mmsi == 366123456u);
    REQUIRE(m.position.has_value());
    CHECK(m.position->refined);
    CHECK(std::abs(m.position->latitude - 43.56055) < 2.0 / 3600.0 + 1e-9);
    CHECK(std::abs(m.position->longitude + 1.48083) < 2.0 / 3600.0 + 1e-9);
    CHECK(m.position->internal_source == true);
    CHECK(m.homing == 1u);
    CHECK(*m.bch2 == 0);
    // The 15 Hex ID carries the default position, so it does not move with
    // the beacon.
    const auto moved = cs_parse(siggen::cs_standard_location(366, 0b0010, (123456U << 4U) | 3U, -12.0, 100.2,
                                                             true, true))
                           .value();
    CHECK(moved.hex_id == m.hex_id);
    CHECK(moved.position->latitude < 0.0);

    // Two errors in PDF-2 are corrected; the position survives.
    auto hit = std_bits;
    hit[115] ^= 1U;
    hit[140] ^= 1U;
    const auto fixed = cs_parse(hit).value();
    CHECK(*fixed.bch2 == 2);
    CHECK(fixed.position->latitude == m.position->latitude);

    // User-location on the serial protocol, PLB (110).
    const auto ul = cs_parse(siggen::cs_serial_user_location(232, 0b110, 54321, -33.9, 151.2)).value();
    CHECK(ul.protocol == CsProtocol::Serial);
    CHECK(ul.country_code == 232);
    REQUIRE(ul.position.has_value());
    CHECK(std::abs(ul.position->latitude + 33.9) <= 2.0 / 60.0);
    CHECK(std::abs(ul.position->longitude - 151.2) <= 2.0 / 60.0);
    CHECK(ul.identity.find("PLB serial 54321") != std::string::npos);

    // Self-test frame sync, 2.2.8.2.
    const auto st = cs_parse(siggen::cs_standard_location(366, 0b0011, 0xABCDEF, 10, 20, false, false, true)).value();
    CHECK(st.self_test);
    CHECK(st.aircraft_address == 0xABCDEFu);

    // Modified-Baudot round trip, Table A3.
    for (const char c : std::string("ABCXYZ0189 -/")) {
        CHECK(cs_baudot_decode(*cs_baudot_encode(c)) == c);
    }
}

namespace {

std::vector<CsBurst> run(const std::vector<dsp::Complex32>& x, dsp::SampleRate rate, std::size_t chunk) {
    auto dec = CsDecoder::create({rate, 16.0}).value();
    std::vector<CsBurst> out;
    for (std::size_t i = 0; i < x.size(); i += chunk) {
        dec.push(std::span(x).subspan(i, std::min(chunk, x.size() - i)), out);
    }
    return out;
}

}  // namespace

TEST_CASE("Cospas-Sarsat bursts decode through Doppler, drift, bit rate error and noise",
          "[decode][cospas]") {
    constexpr dsp::SampleRate kRate = 16000;
    const std::vector<std::vector<std::uint8_t>> messages = {
        siggen::cs_standard_location(366, 0b0010, (123456U << 4U) | 1U, 43.56055, -1.48083, true, true),
        siggen::cs_serial_user_location(232, 0b110, 54321, -33.9, 151.2),
        siggen::cs_standard_location(503, 0b0111, (245U << 14U) | 777U, -27.4, 153.1, false, false),
    };
    for (const double eb_n0_db : {9.0, 12.0, 20.0}) {
        std::vector<dsp::Complex32> stream;
        for (std::size_t i = 0; i < messages.size(); ++i) {
            siggen::CsBurstConfig bc;
            bc.rate = kRate;
            bc.bit_rate = 400.0 * (1.0 + (static_cast<double>(i) - 1.0) * 0.009);  // -0.9%, 0, +0.9%
            bc.initial_phase = 0.5 + static_cast<double>(i);
            bc.lead_seconds = 0.8;
            auto burst = siggen::cs_render_burst(bc, messages[i]).value();
            stream.insert(stream.end(), burst.begin(), burst.end());
        }
        // LEOSAR Doppler at 406 MHz: 3 kHz off and moving 60 Hz/s.
        siggen::FrequencyConfig fc;
        fc.doppler_shift_hz = 3000;
        fc.drift_hz_per_second = 60.0;
        REQUIRE(siggen::apply_frequency_offset(stream, fc, kRate, 9).has_value());
        // Eb = A^2 / 400 with A = 1; noise over the full band.
        const double n0 = (1.0 / 400.0) / std::pow(10.0, eb_n0_db / 10.0);
        REQUIRE(siggen::add_awgn_at_power(stream, n0 * kRate, static_cast<std::uint64_t>(eb_n0_db * 3)).has_value());

        const auto whole = run(stream, kRate, stream.size());
        std::size_t good = 0;
        for (const auto& b : whole) {
            for (const auto& m : messages) {
                if (std::equal(m.begin() + 24, m.end(), b.message.bits.begin() + 24)) {
                    ++good;
                }
            }
        }
        std::printf("Cospas-Sarsat Eb/N0 %.1f dB, 3 kHz + 60 Hz/s, bit rate +-0.9%%: %zu/%zu bursts exact",
                    eb_n0_db, good, messages.size());
        for (const auto& b : whole) {
            std::printf(" [%.0f Hz, %.1f bit/s, BCH %zu/%zu]", b.frequency_offset_hz, b.bit_rate,
                        b.message.bch1.value_or(99), b.message.bch2.value_or(99));
        }
        std::printf("\n");
        if (eb_n0_db >= 12.0) {
            CHECK(good == messages.size());
        }
        if (eb_n0_db >= 20.0) {
            const auto pieces = run(stream, kRate, 777);
            const auto single = run(stream, kRate, 1);
            REQUIRE(pieces.size() == whole.size());
            REQUIRE(single.size() == whole.size());
            for (std::size_t i = 0; i < whole.size(); ++i) {
                CHECK(pieces[i].message.bits == whole[i].message.bits);
                CHECK(single[i].message.bits == whole[i].message.bits);
                CHECK(pieces[i].sample_position == whole[i].sample_position);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Second generation, C/S T.018 Issue 1 Rev. 7
// ---------------------------------------------------------------------------

namespace {

std::string chips_hex(const std::vector<std::uint8_t>& chips) {
    std::string s;
    for (std::size_t i = 0; i < 64; i += 4) {
        const unsigned v = (chips[i] << 3U) | (chips[i + 1] << 2U) | (chips[i + 2] << 1U) | chips[i + 3];
        s.push_back("0123456789ABCDEF"[v]);
    }
    return s;
}

std::vector<SgbBurst> run_sgb(const std::vector<dsp::Complex32>& x, dsp::SampleRate rate,
                              std::size_t chunk) {
    SgbDecoderConfig cfg;
    cfg.rate = rate;
    auto dec = SgbDecoder::create(cfg).value();
    std::vector<SgbBurst> out;
    for (std::size_t i = 0; i < x.size(); i += chunk) {
        dec.push(std::span(x).subspan(i, std::min(chunk, x.size() - i)), out);
    }
    return out;
}

// Noise for a carrier to noise density ratio, the signal at unit power.
void add_cn0(std::vector<dsp::Complex32>& x, double cn0_db, dsp::SampleRate rate,
             std::uint64_t seed) {
    const double n0 = 1.0 / std::pow(10.0, cn0_db / 10.0);
    REQUIRE(siggen::add_awgn_at_power(x, n0 * static_cast<double>(rate), seed).has_value());
}

}  // namespace

TEST_CASE("Cospas-Sarsat SGB PRN, BCH and message match T.018", "[decode][cospas][sgb]") {
    // Table 2.2: the first 64 chips of each sequence.
    CHECK(chips_hex(sgb_prn(SgbMode::Normal, false)) == "80000108421284A1");
    CHECK(chips_hex(sgb_prn(SgbMode::Normal, true)) == "3F8358BAD030F231");
    CHECK(chips_hex(sgb_prn(SgbMode::SelfTest, false)) == "0F934A4D4CF3028D");
    CHECK(chips_hex(sgb_prn(SgbMode::SelfTest, true)) == "14973DC716CDE124");

    // Appendix B.1: g(X) is the narrow-sense t = 6 generator over the field
    // of m1(X); and the worked example's 48 check bits.
    const auto narrow = BchCodec::create({8, 0x11D, 13, {}}).value();
    CHECK(narrow.generator() == from_bits("1110001111110101110000101110111110011110010010111"));
    auto bits = from_bits(
        "0000 0000 1110 0110 0000 1000 1111 0100 1100 1001 1000 0110 0001 1001 0110 0001"
        "1000 1000 1010 0000 0100 0111 1100 0000 0000 0000 0000 0000 0000 0000 0000 0000"
        "0000 0000 0000 1111 1111 1111 1100 0000 0001 0000 0000 1100 0001 1010 0000 0000"
        "1001 0110 00");
    REQUIRE(bits.size() == 202);
    bits.resize(250, 0);
    sgb_fill_bch(bits);
    CHECK(std::vector<std::uint8_t>(bits.begin() + 202, bits.end()) ==
          from_bits("010010010010101001001111110001010111101001001001"));

    // The same example's fields, Appendix B.1, and its 23 Hex ID, B.2.
    auto damaged = bits;
    for (const std::size_t i : {3U, 60U, 99U, 150U, 201U, 240U}) {
        damaged[i] ^= 1U;
    }
    const auto m = sgb_parse(damaged).value();
    CHECK(m.bch_corrected == 6);
    CHECK(m.bits == bits);
    CHECK(m.tac == 230);
    CHECK(m.serial == 573);
    CHECK(m.country_code == 201);
    CHECK(m.homing);
    CHECK(!m.rls);
    CHECK(!m.test_protocol);
    REQUIRE(m.latitude.has_value());
    CHECK(std::abs(*m.latitude - 48.793153539336956) < 1.0 / 32768.0);
    CHECK(std::abs(*m.longitude - 69.00875866413116) < 1.0 / 32768.0);
    CHECK(m.vessel_id_type == 0);
    CHECK(m.beacon_type == 0);
    CHECK(m.rotating_field == 0);
    CHECK(m.hex_id == "9934039823D000000000000");
    // A seventh error is beyond t = 6.
    damaged[10] ^= 1U;
    CHECK(!sgb_parse(damaged).has_value());
}

TEST_CASE("Cospas-Sarsat SGB message builder round trip", "[decode][cospas][sgb]") {
    siggen::SgbMessageSpec s;
    s.tac = 12345;
    s.serial = 9876;
    s.country = 366;
    s.latitude = -33.8567;
    s.longitude = -151.2153;
    s.vessel_id_type = 1;
    s.vessel_id = (std::uint64_t{366123456} << 14U) | 10922U;
    s.beacon_type = 1;
    auto m = sgb_parse(siggen::sgb_message(s)).value();
    CHECK(m.tac == 12345);
    CHECK(m.serial == 9876);
    CHECK(m.country_code == 366);
    REQUIRE(m.latitude.has_value());
    CHECK(std::abs(*m.latitude + 33.8567) < 1e-4);
    CHECK(std::abs(*m.longitude + 151.2153) < 1e-4);
    CHECK(m.mmsi == 366123456U);
    CHECK(m.identity == "MMSI 366123456");
    CHECK(std::string(sgb_beacon_type_name(m.beacon_type)) == "EPIRB");

    // No location: the defaults read as none. Radio call sign, seven
    // modified-Baudot characters, Table 3.2, then two spare bits.
    s.latitude.reset();
    s.longitude.reset();
    s.vessel_id_type = 2;
    s.vessel_id = 0;
    for (const char c : std::string("WDC1234")) {
        s.vessel_id = (s.vessel_id << 6U) | *cs_baudot_encode(c);
    }
    s.vessel_id <<= 2U;
    s.rotating_field = 15;
    m = sgb_parse(siggen::sgb_message(s)).value();
    CHECK(!m.latitude.has_value());
    CHECK(m.identity == "call sign WDC1234");
    CHECK(m.cancellation);

    // Aircraft 24-bit address and a three-letter designator in 5-bit codes.
    s.vessel_id_type = 4;
    std::uint64_t op = 0;
    for (const char c : std::string("BAW")) {
        op = (op << 5U) | (*cs_baudot_encode(c) & 0x1FU);
    }
    s.vessel_id = (std::uint64_t{0xA1B2C3} << 20U) | (op << 5U);
    m = sgb_parse(siggen::sgb_message(s)).value();
    CHECK(m.aircraft_address == 0xA1B2C3U);
    CHECK(m.identity == "aircraft address A1B2C3, operator BAW");
}

TEST_CASE("Cospas-Sarsat SGB bursts through Doppler, chip rate error and noise",
          "[decode][cospas][sgb]") {
    constexpr dsp::SampleRate kRate = 153600;
    siggen::SgbMessageSpec s;
    s.tac = 230;
    s.serial = 573;
    s.country = 201;
    s.latitude = 48.7931;
    s.longitude = 69.0087;
    const auto m1 = siggen::sgb_message(s);
    s.serial = 574;
    s.beacon_type = 2;
    const auto m2 = siggen::sgb_message(s);

    std::vector<dsp::Complex32> stream;
    for (int i = 0; i < 3; ++i) {
        siggen::SgbBurstConfig bc;
        bc.rate = kRate;
        // 2.3.1.2 allows 0.6 chip/s; twice that, as Doppler on the chip
        // clock adds to it.
        bc.chip_rate = 38400.0 + (i - 1) * 1.2;
        bc.initial_phase = 0.7 * i;
        bc.self_test = i == 2;
        bc.lead_seconds = 0.3;
        bc.tail_seconds = i == 2 ? 1.0 : 0.3;
        auto b = siggen::sgb_render_burst(bc, i == 1 ? m2 : m1).value();
        stream.insert(stream.end(), b.begin(), b.end());
    }
    // 2.3.1.1's 1.2 kHz tolerance plus LEO Doppler, moving.
    siggen::FrequencyConfig fc;
    fc.doppler_shift_hz = 2500;
    fc.drift_hz_per_second = 60.0;
    REQUIRE(siggen::apply_frequency_offset(stream, fc, kRate, 3).has_value());
    add_cn0(stream, 50.0, kRate, 17);

    const auto whole = run_sgb(stream, kRate, stream.size());
    for (const auto& b : whole) {
        UNSCOPED_INFO("SGB burst at " << b.sample_position << ": " << b.frequency_offset_hz
                                      << " Hz, " << b.chip_rate << " chip/s, metric "
                                      << b.sync_metric << ", BCH " << b.message.bch_corrected);
        // The offset at the burst, 60 Hz/s on from 2.5 kHz.
        const double expect = 2500.0 + 60.0 * static_cast<double>(b.sample_position) / kRate;
        CHECK(std::abs(b.frequency_offset_hz - expect) < 30.0);
    }
    REQUIRE(whole.size() == 3);
    CHECK(whole[0].message.bits == m1);
    CHECK(whole[1].message.bits == m2);
    CHECK(whole[2].message.bits == m1);
    CHECK(!whole[0].message.self_test);
    CHECK(whole[2].message.self_test);
    CHECK(std::abs(whole[0].chip_rate - 38398.8) < 0.5);
    CHECK(std::abs(whole[2].chip_rate - 38401.2) < 0.5);

    for (const std::size_t chunk : {std::size_t{1}, std::size_t{4093}}) {
        const auto split = run_sgb(stream, kRate, chunk);
        REQUIRE(split.size() == whole.size());
        for (std::size_t i = 0; i < whole.size(); ++i) {
            CHECK(split[i].message.bits == whole[i].message.bits);
            CHECK(split[i].sample_position == whole[i].sample_position);
            CHECK(split[i].frequency_offset_hz == whole[i].frequency_offset_hz);
        }
    }
}

TEST_CASE("Cospas-Sarsat SGB sensitivity", "[decode][cospas][sgb][.measure]") {
    constexpr dsp::SampleRate kRate = 153600;
    siggen::SgbMessageSpec s;
    s.tac = 1;
    s.serial = 2;
    s.country = 3;
    const auto m = siggen::sgb_message(s);
    for (const double cn0 : {30.0, 32.0, 34.0, 36.0, 38.0}) {
        std::size_t good = 0;
        constexpr int kBursts = 10;
        for (int i = 0; i < kBursts; ++i) {
            siggen::SgbBurstConfig bc;
            bc.rate = kRate;
            bc.initial_phase = i;
            bc.tail_seconds = 1.0;
            auto x = siggen::sgb_render_burst(bc, m).value();
            siggen::FrequencyConfig fc;
            fc.doppler_shift_hz = 1000;
            REQUIRE(siggen::apply_frequency_offset(x, fc, kRate, 3).has_value());
            add_cn0(x, cn0, kRate, static_cast<std::uint64_t>(i + 100));
            const auto got = run_sgb(x, kRate, x.size());
            good += (got.size() == 1 && got[0].message.bits == m) ? 1 : 0;
        }
        WARN("SGB C/N0 " << cn0 << " dB-Hz: " << good << "/" << kBursts << " bursts");
    }
}
