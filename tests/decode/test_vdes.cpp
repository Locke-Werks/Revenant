// VDES ASM-TER: ITU-R M.2092-2's printed Link ID code words, then bursts
// through the transmitter, noise, multipath and a carrier offset, and back.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "core/decode/vdes.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/vdes_mod.h"

using namespace revenant;

namespace {

std::uint32_t parse_bits(const std::string& s) {
    std::uint32_t v = 0;
    for (const char c : s) {
        if (c == '0' || c == '1') {
            v = (v << 1U) | static_cast<std::uint32_t>(c - '0');
        }
    }
    return v;
}

std::vector<std::uint8_t> random_payload(unsigned link_id, std::mt19937& rng) {
    const auto f = decode::vdes_asm_format(link_id);
    REQUIRE(f.has_value());
    std::vector<std::uint8_t> p(f->packet_bits / 8 - 4);
    for (auto& b : p) {
        b = static_cast<std::uint8_t>(rng() & 0xFFU);
    }
    return p;
}

std::vector<decode::VdesAsmBurst> run(const std::vector<dsp::Complex32>& iq, dsp::SampleRate rate,
                                      std::size_t block = 0) {
    decode::VdesAsmConfig config;
    config.rate = rate;
    auto decoder = decode::VdesAsmDecoder::create(config);
    REQUIRE(decoder.has_value());
    std::vector<decode::VdesAsmBurst> out;
    if (block == 0) {
        decoder->process(iq, out);
    } else {
        std::mt19937 rng(7);
        std::size_t i = 0;
        while (i < iq.size()) {
            const std::size_t n = std::min<std::size_t>(iq.size() - i, 1 + rng() % block);
            decoder->process(dsp::ConstComplexSpan(iq.data() + i, n), out);
            i += n;
        }
    }
    return out;
}

void add_noise(std::vector<dsp::Complex32>& iq, double es_n0_db, dsp::SampleRate rate,
               std::uint64_t seed) {
    double sum = 0.0;
    std::size_t on = 0;
    for (const auto& v : iq) {
        if (std::norm(v) > 1e-6F) {
            sum += static_cast<double>(std::norm(v));
            ++on;
        }
    }
    // Es/N0 is the signal to noise ratio in the symbol rate's bandwidth.
    auto noise = siggen::awgn_power_for(
        siggen::NoiseLevel::snr_in_reference_bandwidth_db(es_n0_db, 9600),
        sum / static_cast<double>(on), rate);
    REQUIRE(noise.has_value());
    REQUIRE(siggen::add_awgn_at_power(iq, *noise, seed).has_value());
}

std::size_t count_good(const std::vector<decode::VdesAsmBurst>& got,
                       const std::vector<siggen::VdesAsmBurstSpec>& sent) {
    std::size_t good = 0;
    std::size_t j = 0;
    for (const auto& b : got) {
        if (!b.crc_ok) {
            continue;
        }
        while (j < sent.size() && sent[j].payload != b.payload) {
            ++j;
        }
        if (j < sent.size()) {
            ++good;
            ++j;
        }
    }
    return good;
}

}  // namespace

TEST_CASE("VDES Link ID code words match Table 3", "[vdes]") {
    // M.2092-2 Table 3, printed as bit pairs. Link IDs 0, 1, 32 and 63.
    CHECK(decode::vdes_link_id_codeword(0) ==
          parse_bits("11 00 00 10 11 10 00 10 10 00 11 10 01 00 11 11"));
    CHECK(decode::vdes_link_id_codeword(1) ==
          parse_bits("11 00 01 10 11 10 00 10 11 11 00 01 10 11 00 00"));
    CHECK(decode::vdes_link_id_codeword(32) ==
          parse_bits("01 00 00 00 00 00 10 11 01 10 01 11 11 01 10 01"));
    CHECK(decode::vdes_link_id_codeword(63) ==
          parse_bits("00 11 11 01 00 01 11 01 01 11 00 01 10 11 00 00"));
    // A first order Reed-Muller (32,6) code: every pair of words is 16 apart
    // or, for complements, 32.
    for (unsigned a = 0; a < 64; ++a) {
        for (unsigned b = a + 1; b < 64; ++b) {
            const int d = std::popcount(decode::vdes_link_id_codeword(a) ^
                                        decode::vdes_link_id_codeword(b));
            CHECK((d == 16 || d == 32));
        }
    }
}

TEST_CASE("VDES scrambler and CRC", "[vdes]") {
    // Figure 9: a 15-stage maximal length register, so the sequence repeats
    // after 32767 bits and not before.
    std::vector<std::uint8_t> zeros(2 * 32767, 0);
    decode::vdes_scramble(zeros);
    CHECK(std::equal(zeros.begin(), zeros.begin() + 32767, zeros.begin() + 32767));
    CHECK(!std::equal(zeros.begin(), zeros.begin() + 100, zeros.begin() + 1000));
    // The first bits: the preset's cells 14 and 15 are both 0.
    CHECK(zeros[0] == 0);

    // Clause A2-1.2.5: the check sequence appended MSB first leaves a zero
    // remainder. The CRC-32/MPEG-2 catalogue check value over "123456789".
    const std::string digits = "123456789";
    const std::vector<std::uint8_t> v(digits.begin(), digits.end());
    CHECK(decode::vdes_crc32(v) == 0x0376E6E7U);
}

TEST_CASE("VDES ASM round trip, all uncoded formats", "[vdes]") {
    for (const dsp::SampleRate rate : {38400, 48000, 96000}) {
        std::mt19937 rng(static_cast<unsigned>(rate));
        std::vector<siggen::VdesAsmBurstSpec> sent;
        for (unsigned id : {1U, 2U, 3U, 1U}) {
            sent.push_back({id, random_payload(id, rng)});
        }
        siggen::VdesAsmModConfig mod;
        mod.rate = rate;
        auto iq = siggen::vdes_asm_render(mod, sent);
        REQUIRE(iq.has_value());
        // A noise floor 40 dB down, so the gaps are not exact zeros.
        std::vector<dsp::Complex32> rx = *iq;
        add_noise(rx, 40.0, rate, 5);
        const auto all = run(rx, rate);
        for (const auto& b : all) {
            if (!b.crc_ok) {
                WARN("false start at " << b.sync_sample << " metric " << b.sync_metric
                                       << " id " << b.link_id << " q " << b.link_id_quality);
            }
        }
        std::vector<decode::VdesAsmBurst> got;
        std::copy_if(all.begin(), all.end(), std::back_inserter(got),
                     [](const decode::VdesAsmBurst& b) { return b.crc_ok; });
        REQUIRE(got.size() == sent.size());
        for (std::size_t i = 0; i < sent.size(); ++i) {
            CHECK(got[i].crc_ok);
            CHECK(got[i].link_id == sent[i].link_id);
            CHECK(got[i].payload == sent[i].payload);
            CHECK(got[i].cqi >= 150);
        }
    }
}

TEST_CASE("VDES ASM message header", "[vdes]") {
    decode::VdesAsmMessage h;
    h.message_id = 2;
    h.repeat = 1;
    h.session_id = 42;
    h.source_id = 0xDEADBEEF;
    h.data_count = 40;
    const std::vector<std::uint8_t> app{0x01, 0x23, 0x45, 0x67, 0x89};
    auto payload = siggen::vdes_asm_build_payload(1, h, app);
    REQUIRE(payload.has_value());
    siggen::VdesAsmModConfig mod;
    std::vector<siggen::VdesAsmBurstSpec> sent{{1, *payload}};
    auto iq = siggen::vdes_asm_render(mod, sent);
    REQUIRE(iq.has_value());
    std::vector<dsp::Complex32> rx = *iq;
    add_noise(rx, 40.0, 48000, 6);
    const auto got = run(rx, 48000);
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].message.has_value());
    const auto& m = *got[0].message;
    CHECK(m.message_id == 2);
    CHECK(m.repeat == 1);
    CHECK(m.session_id == 42);
    CHECK(m.source_id == 0xDEADBEEFU);
    CHECK(m.data_count == 40);
    REQUIRE(m.application_data.has_value());
    CHECK(*m.application_data == app);
    CHECK(m.asm_identifier == std::uint16_t{0x0123});
}

TEST_CASE("VDES ASM with noise, multipath and carrier offset", "[vdes]") {
    constexpr dsp::SampleRate kRate = 48000;
    constexpr std::size_t kBursts = 100;
    std::mt19937 rng(11);
    std::vector<siggen::VdesAsmBurstSpec> sent;
    for (std::size_t i = 0; i < kBursts; ++i) {
        sent.push_back({1, random_payload(1, rng)});
    }

    struct Case {
        double es_n0;
        int cfo;
        bool multipath;
        double max_loss;
    };
    // Table 7 gives 11.0 dB Es/N0 on AWGN for the uncoded formats.
    for (const Case c : {Case{14.0, 0, false, 0.0}, Case{11.0, 0, false, 0.2},
                         Case{11.0, 480, false, 0.2}, Case{11.0, -480, false, 0.2},
                         Case{14.0, 200, true, 0.1}}) {
        siggen::VdesAsmModConfig mod;
        mod.rate = kRate;
        mod.carrier_offset_hz = c.cfo;
        auto iq = siggen::vdes_asm_render(mod, sent);
        REQUIRE(iq.has_value());
        std::vector<dsp::Complex32> rx = *iq;
        if (c.multipath) {
            siggen::MultipathProfile profile;
            profile.taps = {{0.0, 0.0, 0.0}, {15e-6, -6.0, 1.3}};
            auto mp = siggen::apply_multipath(rx, profile, kRate);
            REQUIRE(mp.has_value());
            rx = *mp;
        }
        add_noise(rx, c.es_n0, kRate, 1234);
        const auto got = run(rx, kRate);
        const std::size_t good = count_good(got, sent);
        const double loss = 1.0 - static_cast<double>(good) / kBursts;
        UNSCOPED_INFO("Es/N0 " << c.es_n0 << " dB, " << c.cfo << " Hz, multipath "
                               << c.multipath << ": packet error rate " << loss);
        CHECK(loss <= c.max_loss);
    }
}

TEST_CASE("VDES ASM packet error rate", "[vdes][.measure]") {
    constexpr dsp::SampleRate kRate = 48000;
    constexpr std::size_t kBursts = 500;
    std::mt19937 rng(5);
    std::vector<siggen::VdesAsmBurstSpec> sent;
    for (std::size_t i = 0; i < kBursts; ++i) {
        sent.push_back({1, random_payload(1, rng)});
    }
    siggen::VdesAsmModConfig mod;
    auto iq = siggen::vdes_asm_render(mod, sent);
    REQUIRE(iq.has_value());
    for (double es : {8.0, 9.0, 10.0, 11.0, 12.0, 13.0}) {
        std::vector<dsp::Complex32> rx = *iq;
        add_noise(rx, es, kRate, 99);
        const auto got = run(rx, kRate);
        WARN("Es/N0 " << es << " dB: PER "
                      << 1.0 - static_cast<double>(count_good(got, sent)) / kBursts);
    }
}

TEST_CASE("VDES ASM is the same however the stream is split", "[vdes]") {
    constexpr dsp::SampleRate kRate = 48000;
    std::mt19937 rng(3);
    std::vector<siggen::VdesAsmBurstSpec> sent;
    for (unsigned id : {1U, 3U, 2U, 1U, 1U}) {
        sent.push_back({id, random_payload(id, rng)});
    }
    siggen::VdesAsmModConfig mod;
    mod.carrier_offset_hz = 150.0;
    auto iq = siggen::vdes_asm_render(mod, sent);
    REQUIRE(iq.has_value());
    std::vector<dsp::Complex32> rx = *iq;
    add_noise(rx, 12.0, kRate, 77);
    const auto whole = run(rx, kRate);
    for (std::size_t block : {1U, 17U, 480U, 5000U}) {
        const auto split = run(rx, kRate, block);
        REQUIRE(split.size() == whole.size());
        for (std::size_t i = 0; i < whole.size(); ++i) {
            CHECK(split[i].sync_sample == whole[i].sync_sample);
            CHECK(split[i].crc_ok == whole[i].crc_ok);
            CHECK(split[i].payload == whole[i].payload);
            CHECK(split[i].cfo_hz == whole[i].cfo_hz);
        }
    }
}
