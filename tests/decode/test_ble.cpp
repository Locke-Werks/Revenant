// Bluetooth LE advertising: the specification's own examples, then round trips
// through core/dsp/synth/ble_mod.h.
//
// Core 6.1 prints no LE advertising packet as sample data, so the CRC is
// checked two independent ways (the shift register of Figure 3.4 against the
// table-driven generic CRC configured from the same paragraph) and the
// whitening seed against the channel 23 example of Vol 6 Part B 3.2.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "core/decode/ble.h"
#include "core/decode/crc.h"
#include "core/dsp/synth/ble_mod.h"
#include "core/dsp/synth/channel.h"

using namespace revenant;
using decode::BleAddress;
using decode::BlePacket;
using decode::BlePduType;

namespace {

const BleAddress kAdvA = {0x11, 0x22, 0x33, 0x44, 0x55, 0xC6};
const BleAddress kTarget = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6};

// Flags (type 0x01) and a Complete Local Name (type 0x09), Vol 3 Part C 11.
std::vector<std::uint8_t> adv_data() {
    std::vector<std::uint8_t> d = {0x02, 0x01, 0x06};
    const std::string name = "Revenant";
    d.push_back(static_cast<std::uint8_t>(name.size() + 1));
    d.push_back(0x09);
    d.insert(d.end(), name.begin(), name.end());
    return d;
}

std::vector<std::vector<std::uint8_t>> three_pdus() {
    std::vector<std::vector<std::uint8_t>> pdus;
    const auto data = adv_data();
    pdus.push_back(*siggen::ble_advertising_pdu(BlePduType::AdvInd, true, false,
                                                siggen::ble_adv_payload(kAdvA, data)));
    std::vector<std::uint8_t> direct(kAdvA.begin(), kAdvA.end());
    direct.insert(direct.end(), kTarget.begin(), kTarget.end());
    pdus.push_back(*siggen::ble_advertising_pdu(BlePduType::AdvDirectInd, false, true, direct));
    const std::vector<std::uint8_t> rsp = {0x03, 0xFF, 0x34, 0x12};
    pdus.push_back(*siggen::ble_advertising_pdu(BlePduType::ScanRsp, true, false,
                                                siggen::ble_adv_payload(kAdvA, rsp)));
    return pdus;
}

std::vector<BlePacket> decode_all(const decode::BleConfig& cfg, const std::vector<dsp::Complex32>& iq,
                                  std::size_t block) {
    auto dec = decode::BleDecoder::create(cfg);
    REQUIRE(dec.has_value());
    std::vector<BlePacket> out;
    for (std::size_t i = 0; i < iq.size(); i += block) {
        const std::size_t n = std::min(block, iq.size() - i);
        REQUIRE(dec->push(dsp::ConstComplexSpan(iq).subspan(i, n), out).has_value());
    }
    return out;
}

}  // namespace

TEST_CASE("BLE whitening seed follows the channel 23 example", "[decode][ble]") {
    // Vol 6 Part B 3.2: positions 0 to 6 are 1, 0, 1, 0, 1, 1, 1.
    CHECK(decode::ble_whitening_seed(23) == 0b1110101U);
    std::vector<std::uint8_t> bits(300, 0);
    decode::ble_whiten(bits, 37);
    // A maximal 7-bit sequence repeats every 127 bits.
    for (std::size_t i = 0; i + 127 < bits.size(); ++i) {
        CHECK(bits[i] == bits[i + 127]);
    }
    std::vector<std::uint8_t> copy = bits;
    decode::ble_whiten(copy, 37);
    for (const auto b : copy) {
        CHECK(b == 0);
    }
}

TEST_CASE("BLE CRC-24 agrees with the generic CRC and checks to zero", "[decode][ble]") {
    const decode::Crc generic(decode::CrcParams{24, decode::kBleCrcPolynomial,
                                                decode::kBleAdvertisingCrcInit, false, false, 0});
    for (const auto& pdu : three_pdus()) {
        std::vector<std::uint8_t> bits;
        for (const auto o : pdu) {
            for (unsigned i = 0; i < 8; ++i) {
                bits.push_back(static_cast<std::uint8_t>((o >> i) & 1U));
            }
        }
        const std::uint32_t crc = decode::ble_crc24_bits(bits, decode::kBleAdvertisingCrcInit);
        CHECK(crc == generic.compute_bits(bits));
        CHECK(crc == decode::ble_crc24(pdu, decode::kBleAdvertisingCrcInit));
        for (unsigned i = 0; i < 24; ++i) {
            bits.push_back(static_cast<std::uint8_t>((crc >> (23U - i)) & 1U));
        }
        CHECK(decode::ble_crc24_bits(bits, decode::kBleAdvertisingCrcInit) == 0U);
    }
}

TEST_CASE("BLE AD structures parse and stop at the non-significant part", "[decode][ble]") {
    auto d = adv_data();
    d.push_back(0);
    d.push_back(0);
    auto ad = decode::ble_parse_ad(d);
    REQUIRE(ad.has_value());
    REQUIRE(ad->size() == 2);
    CHECK((*ad)[0].type == 0x01);
    CHECK((*ad)[1].type == 0x09);
    CHECK(std::string((*ad)[1].data.begin(), (*ad)[1].data.end()) == "Revenant");
    const std::vector<std::uint8_t> bad = {0x05, 0x09, 'a'};
    CHECK_FALSE(decode::ble_parse_ad(bad).has_value());
}

TEST_CASE("BLE advertising round trips at 2.4 and 4 MS/s", "[decode][ble]") {
    for (const dsp::SampleRate rate : {dsp::SampleRate{2'400'000}, dsp::SampleRate{4'000'000}}) {
        INFO("rate " << rate);
        siggen::BleModConfig mc;
        mc.sample_rate = rate;
        mc.channel_index = 38;
        const auto pdus = three_pdus();
        auto iq = siggen::ble_render(mc, pdus);
        REQUIRE(iq.has_value());
        decode::BleConfig cfg;
        cfg.sample_rate = rate;
        cfg.channel_index = 38;
        const auto got = decode_all(cfg, *iq, 4096);
        REQUIRE(got.size() == 3);
        CHECK(got[0].pdu == pdus[0]);
        CHECK(got[0].pdu_type == static_cast<std::uint8_t>(BlePduType::AdvInd));
        CHECK(got[0].tx_add);
        CHECK(got[0].adv_address == kAdvA);
        REQUIRE(got[0].ad.size() == 2);
        CHECK(got[0].ad[1].type == 0x09);
        CHECK(got[0].access_address_errors == 0);
        CHECK(got[1].target_address == kTarget);
        CHECK(got[1].rx_add);
        CHECK(got[2].pdu_type == static_cast<std::uint8_t>(BlePduType::ScanRsp));
        REQUIRE(got[2].ad.size() == 1);
        CHECK(got[2].ad[0].type == 0xFF);
        CHECK(got[0].sample < got[1].sample);
    }
}

TEST_CASE("BLE decoding on the wrong channel index fails the CRC", "[decode][ble]") {
    siggen::BleModConfig mc;
    mc.channel_index = 37;
    auto iq = siggen::ble_render(mc, three_pdus());
    REQUIRE(iq.has_value());
    decode::BleConfig cfg;
    cfg.channel_index = 39;
    CHECK(decode_all(cfg, *iq, 4096).empty());
}

TEST_CASE("BLE survives the carrier offsets Part A allows", "[decode][ble]") {
    for (const dsp::Hertz offset : {dsp::Hertz{-150'000}, dsp::Hertz{100'000}, dsp::Hertz{150'000}}) {
        INFO("offset " << offset);
        siggen::BleModConfig mc;
        auto iq = siggen::ble_render(mc, three_pdus());
        REQUIRE(iq.has_value());
        siggen::FrequencyConfig fc;
        fc.doppler_shift_hz = offset;
        REQUIRE(siggen::apply_frequency_offset(*iq, fc, mc.sample_rate, 7).has_value());
        const auto got = decode_all(decode::BleConfig{}, *iq, 4096);
        REQUIRE(got.size() == 3);
        CHECK(std::abs(got[0].frequency_offset_hz - static_cast<double>(offset)) < 15'000.0);
    }
}

TEST_CASE("the BLE receiver does not depend on how its input is blocked", "[decode][ble]") {
    siggen::BleModConfig mc;
    auto pdus = three_pdus();
    pdus.insert(pdus.end(), pdus.begin(), pdus.end());
    auto iq = siggen::ble_render(mc, pdus);
    REQUIRE(iq.has_value());
    REQUIRE(siggen::add_awgn(*iq, siggen::NoiseLevel::eb_over_n0_db(12.0, 1e6), mc.sample_rate,
                             0x5EED'0B1EULL)
                .has_value());
    const auto whole = decode_all(decode::BleConfig{}, *iq, iq->size());
    INFO("decoded whole " << whole.size() << " of 6");
    CHECK(whole.size() >= 2);
    for (const std::size_t block : {std::size_t{1}, std::size_t{7}, std::size_t{333}, std::size_t{4096}}) {
        INFO("block " << block);
        const auto split = decode_all(decode::BleConfig{}, *iq, block);
        REQUIRE(split.size() == whole.size());
        for (std::size_t i = 0; i < whole.size(); ++i) {
            CHECK(split[i].sample == whole[i].sample);
            CHECK(split[i].pdu == whole[i].pdu);
        }
    }
}

TEST_CASE("BLE packet error rate against noise, measured", "[decode][ble]") {
    // Eb/N0 at 1 Mbit/s, 2.4 MS/s, 37-octet ADV_IND PDUs with a +60 kHz
    // carrier offset. Loose assertion; the figures are in the log. Measured
    // 2026-11-02: 0.99, 0.905, 0.51 and 0.175 at 8, 10, 12 and 14 dB.
    constexpr int kPackets = 200;
    siggen::BleModConfig mc;
    const auto pdu = three_pdus()[0];
    std::vector<std::vector<std::uint8_t>> pdus(kPackets, pdu);
    for (const double ebn0 : {8.0, 10.0, 12.0, 14.0}) {
        auto iq = siggen::ble_render(mc, pdus);
        REQUIRE(iq.has_value());
        siggen::FrequencyConfig fc;
        fc.doppler_shift_hz = 60'000;
        REQUIRE(siggen::apply_frequency_offset(*iq, fc, mc.sample_rate, 3).has_value());
        REQUIRE(siggen::add_awgn(*iq, siggen::NoiseLevel::eb_over_n0_db(ebn0, 1e6), mc.sample_rate,
                                 0x5EED'B1E0ULL + static_cast<std::uint64_t>(ebn0))
                    .has_value());
        const auto got = decode_all(decode::BleConfig{}, *iq, 65536);
        std::size_t good = 0;
        for (const auto& p : got) {
            good += p.pdu == pdu ? 1U : 0U;
        }
        CHECK(good == got.size());
        const double per = 1.0 - static_cast<double>(good) / kPackets;
        WARN("BLE Eb/N0 " << ebn0 << " dB: PER " << per);
        if (ebn0 >= 14.0) {
            CHECK(per < 0.35);
        }
    }
}
