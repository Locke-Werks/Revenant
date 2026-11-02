// DVB-S, core/decode/dvbs.h, against the transmitter in
// core/dsp/synth/dvbs_mod.h.
//
// Spec vectors are what EN 300 421 V1.1.2 prints: the first PRBS bits of
// figure 2, the 1503-byte dispersal period of 4.4.1, the 2244-byte
// interleaver delay of 4.4.2 and the I and Q columns of table 2. The round
// trips run every code rate noiseless, rate 1/2 at the table 3 Eb/N0 with a
// carrier offset, and a split-anywhere chunking check.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "core/decode/dvbs.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dvbs_mod.h"

using namespace revenant;
using decode::DvbsCodeRate;
using dsp::Complex32;

namespace {

constexpr dsp::SampleRate kRate = 200000;
constexpr double kSymbolRate = 50000.0;
constexpr std::uint16_t kDataPid = 0x0100;

siggen::TsPacketBytes data_packet(std::uint32_t seq, std::uint8_t cc) {
    siggen::TsPacketBytes p{};
    p[0] = 0x47;
    p[1] = static_cast<std::uint8_t>(kDataPid >> 8U);
    p[2] = static_cast<std::uint8_t>(kDataPid);
    p[3] = static_cast<std::uint8_t>(0x10U | (cc & 0x0FU));
    p[4] = static_cast<std::uint8_t>(seq >> 24U);
    p[5] = static_cast<std::uint8_t>(seq >> 16U);
    p[6] = static_cast<std::uint8_t>(seq >> 8U);
    p[7] = static_cast<std::uint8_t>(seq);
    for (std::size_t i = 8; i < p.size(); ++i) {
        p[i] = static_cast<std::uint8_t>(seq * 7 + i);
    }
    return p;
}

std::vector<decode::TsService> services() {
    decode::TsService a;
    a.service_id = 0x0101;
    a.service_type = 0x01;
    a.running_status = 4;
    a.provider = "Locke Werks";
    a.name = "Test Card";
    return {a};
}

// Lead-in nulls, PSI, numbered data packets, trailing nulls.
std::vector<siggen::TsPacketBytes> stream(std::uint32_t data_packets) {
    std::vector<siggen::TsPacketBytes> out(16, siggen::ts_null_packet());
    std::uint8_t cc_pat = 0;
    std::uint8_t cc_pmt = 0;
    std::uint8_t cc_sdt = 0;
    const std::vector<decode::TsProgram> programs{{0x0101, 0x0020}};
    const std::vector<decode::TsElementaryStream> es{{0x02, 0x0100}, {0x04, 0x0101}};
    const auto pat = siggen::ts_build_pat(0x1234, 3, programs);
    const auto pmt = siggen::ts_build_pmt(0x0101, 1, 0x0100, es);
    const auto svcs = services();
    const auto sdt = siggen::ts_build_sdt(0x1234, 0x0055, 2, svcs);
    for (std::uint32_t i = 0; i < data_packets; ++i) {
        if (i % 10 == 0) {
            for (const auto& p : siggen::ts_section_packets(0x0000, pat, cc_pat)) out.push_back(p);
            for (const auto& p : siggen::ts_section_packets(0x0020, pmt, cc_pmt)) out.push_back(p);
            for (const auto& p : siggen::ts_section_packets(0x0011, sdt, cc_sdt)) out.push_back(p);
        }
        out.push_back(data_packet(i, static_cast<std::uint8_t>(i)));
    }
    out.insert(out.end(), 24, siggen::ts_null_packet());
    return out;
}

decode::DvbsConfig rx_config(std::optional<DvbsCodeRate> rate) {
    decode::DvbsConfig c;
    c.rate = kRate;
    c.symbol_rate = kSymbolRate;
    c.code_rate = rate;
    return c;
}

std::vector<Complex32> render(DvbsCodeRate rate, const std::vector<siggen::TsPacketBytes>& ts,
                              double offset_hz = 0.0) {
    siggen::DvbsModConfig mc;
    mc.psk.rate = kRate;
    mc.psk.symbol_rate = kSymbolRate;
    mc.psk.timing_offset_samples = 1.37;
    mc.psk.initial_phase = 0.6;
    mc.psk.carrier_offset_hz = offset_hz;
    mc.rate = rate;
    auto iq = siggen::dvbs_render(mc, ts);
    REQUIRE(iq);
    return *iq;
}

struct Result {
    std::vector<decode::DvbsPacket> packets;
    decode::TsPsiParser psi;
    std::uint32_t data_seen = 0;
    std::uint32_t data_in_order = 0;
    std::uint64_t rs_failed = 0;
};

Result run(decode::DvbsDecoder& rx, std::span<const Complex32> iq, std::size_t chunk_seed = 0) {
    Result r;
    std::mt19937 rng(static_cast<unsigned>(chunk_seed));
    std::size_t pos = 0;
    while (pos < iq.size()) {
        std::size_t n = iq.size() - pos;
        if (chunk_seed != 0) {
            n = std::min<std::size_t>(n, 1 + rng() % 5000);
        }
        for (const auto& p : rx.process(iq.subspan(pos, n))) {
            r.packets.push_back(p);
        }
        pos += n;
    }
    std::int64_t last = -1;
    for (const auto& p : r.packets) {
        r.psi.push(p.bytes);
        if (!p.rs_ok) {
            ++r.rs_failed;
        }
        const auto h = decode::ts_parse_header(p.bytes);
        if (h && h->pid == kDataPid && !h->transport_error) {
            const std::uint32_t seq = (std::uint32_t{p.bytes[4]} << 24U) |
                                      (std::uint32_t{p.bytes[5]} << 16U) |
                                      (std::uint32_t{p.bytes[6]} << 8U) | p.bytes[7];
            if (p.bytes == data_packet(seq, static_cast<std::uint8_t>(seq))) {
                ++r.data_seen;
                if (static_cast<std::int64_t>(seq) == last + 1 || last < 0) {
                    ++r.data_in_order;
                }
                last = seq;
            }
        }
    }
    return r;
}

}  // namespace

TEST_CASE("DVB-S PRBS matches EN 300 421 figure 2", "[dvbs]") {
    decode::DvbsPrbs prbs;
    // Figure 2: "PRBS sequence: 0 0 0 0 0 0 1 1 ...".
    const std::array<int, 8> expected{0, 0, 0, 0, 0, 0, 1, 1};
    for (const int e : expected) {
        CHECK(prbs.next_bit() == e);
    }
    // 1 + X^14 + X^15 is primitive: period 2^15 - 1 bits.
    decode::DvbsPrbs a;
    std::vector<std::uint8_t> seq(40000);
    for (auto& b : seq) b = a.next_bit();
    for (std::size_t i = 0; i < 100; ++i) {
        CHECK(seq[i] == seq[i + 32767]);
    }
}

TEST_CASE("DVB-S energy dispersal inverts the first sync and round trips", "[dvbs]") {
    std::vector<std::uint8_t> group(8 * 188);
    for (std::size_t i = 0; i < group.size(); ++i) group[i] = static_cast<std::uint8_t>(i * 31);
    for (std::size_t p = 0; p < 8; ++p) group[p * 188] = 0x47;
    const auto original = group;
    decode::dvbs_energy_dispersal(group, true);
    CHECK(group[0] == 0xB8);
    for (std::size_t p = 1; p < 8; ++p) CHECK(group[p * 188] == 0x47);
    CHECK(group != original);
    decode::dvbs_energy_dispersal(group, false);
    CHECK(group == original);

    // 4.4.1: 1503 PRBS bytes per group, 187 + 7 * (1 + 187).
    std::vector<std::uint8_t> zeros(8 * 188, 0);
    for (std::size_t p = 0; p < 8; ++p) zeros[p * 188] = 0x47;
    decode::dvbs_energy_dispersal(zeros, true);
    decode::DvbsPrbs prbs;
    std::size_t applied = 0;
    for (std::size_t p = 0; p < 8; ++p) {
        if (p > 0) (void)prbs.next_byte();
        for (std::size_t i = 1; i < 188; ++i) {
            CHECK(zeros[p * 188 + i] == prbs.next_byte());
            ++applied;
        }
    }
    CHECK(applied + 7 == 1503);
}

TEST_CASE("DVB-S interleaver delay and sync routing per 4.4.2", "[dvbs]") {
    decode::DvbsInterleaver tx(false);
    decode::DvbsInterleaver rx(true);
    std::vector<std::uint8_t> in(204 * 30);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = static_cast<std::uint8_t>(i % 204 == 0 ? 0x47 : (i * 13 + 5));
    std::vector<std::uint8_t> mid;
    std::vector<std::uint8_t> out;
    for (const auto b : in) {
        mid.push_back(tx.push(b));
        out.push_back(rx.push(mid.back()));
    }
    // Branch 0 has no delay, so sync bytes keep their 204-byte spacing.
    for (std::size_t i = 0; i < mid.size(); i += 204) CHECK(mid[i] == 0x47);
    CHECK(decode::kDvbsInterleaveDelayBytes == 2244);
    for (std::size_t i = 2244; i < out.size(); ++i) {
        REQUIRE(out[i] == in[i - 2244]);
    }
}

TEST_CASE("DVB-S RS(204,188) corrects eight bytes", "[dvbs]") {
    const auto& rs = decode::dvbs_rs();
    CHECK(rs.generator().size() == 17);
    std::array<std::uint8_t, 204> word{};
    for (std::size_t i = 0; i < 188; ++i) word[i] = static_cast<std::uint8_t>(i * 7 + 1);
    rs.encode(std::span(word).first(188), std::span(word).subspan(188));
    auto clean = word;
    CHECK(rs.decode(clean) == std::optional<std::size_t>(0));
    auto bad = word;
    for (std::size_t k = 0; k < 8; ++k) bad[k * 25] ^= 0x5A;
    CHECK(rs.decode(bad) == std::optional<std::size_t>(8));
    CHECK(bad == word);
}

TEST_CASE("DVB-S puncturing matches table 2", "[dvbs]") {
    // Label mother-code outputs X1 Y1 X2 Y2 ... and check the I/Q columns.
    auto columns = [](DvbsCodeRate rate) {
        // Two periods, so a three-bit period (2/3) fills whole symbols.
        auto pattern = decode::dvbs_puncture_pattern(rate);
        const auto once = pattern;
        pattern.insert(pattern.end(), once.begin(), once.end());
        std::string serial;
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            if (pattern[i] != 0) {
                serial += (i % 2 == 0 ? 'X' : 'Y');
                serial += static_cast<char>('1' + i / 2);
            }
        }
        if (std::count(once.begin(), once.end(), 1) % 2 == 0) {
            serial.resize(serial.size() / 2);
        }
        std::string iq[2];
        for (std::size_t k = 0; k < serial.size() / 2; ++k) {
            iq[k % 2] += serial.substr(2 * k, 2);
        }
        return std::pair{iq[0], iq[1]};
    };
    CHECK(columns(DvbsCodeRate::R1_2) == std::pair<std::string, std::string>{"X1", "Y1"});
    CHECK(columns(DvbsCodeRate::R2_3) == std::pair<std::string, std::string>{"X1Y2Y3", "Y1X3Y4"});
    CHECK(columns(DvbsCodeRate::R3_4) == std::pair<std::string, std::string>{"X1Y2", "Y1X3"});
    CHECK(columns(DvbsCodeRate::R5_6) == std::pair<std::string, std::string>{"X1Y2Y4", "Y1X3X5"});
    CHECK(columns(DvbsCodeRate::R7_8) == std::pair<std::string, std::string>{"X1Y2Y4Y6", "Y1Y3X5X7"});
}

TEST_CASE("DVB-S PSI: PAT, PMT and SDT parse with CRC_32", "[dvbs]") {
    decode::TsPsiParser psi;
    for (const auto& p : stream(5)) psi.push(p);
    REQUIRE(psi.pat());
    CHECK(psi.pat()->transport_stream_id == 0x1234);
    CHECK(psi.pat()->version == 3);
    REQUIRE(psi.pat()->programs.size() == 1);
    CHECK(psi.pat()->programs[0].pid == 0x0020);
    REQUIRE(psi.pmts().count(0x0101) == 1);
    const auto& pmt = psi.pmts().at(0x0101);
    CHECK(pmt.pcr_pid == 0x0100);
    REQUIRE(pmt.streams.size() == 2);
    CHECK(pmt.streams[1].stream_type == 0x04);
    CHECK(pmt.streams[1].pid == 0x0101);
    REQUIRE(psi.sdt());
    CHECK(psi.sdt()->original_network_id == 0x0055);
    REQUIRE(psi.sdt()->services.size() == 1);
    CHECK(psi.sdt()->services[0].name == "Test Card");
    CHECK(psi.sdt()->services[0].provider == "Locke Werks");
    CHECK(psi.sdt()->services[0].running_status == 4);
    CHECK(psi.crc_errors() == 0);

    // A flipped bit fails the CRC.
    auto pat = siggen::ts_build_pat(1, 0, std::vector<decode::TsProgram>{{1, 0x30}});
    pat[9] ^= 1;
    std::uint8_t cc = 0;
    decode::TsPsiParser bad;
    for (const auto& p : siggen::ts_section_packets(0, pat, cc)) bad.push(p);
    CHECK(bad.crc_errors() == 1);
    CHECK_FALSE(bad.pat());
}

TEST_CASE("DVB-S round trip at every code rate", "[dvbs]") {
    const auto ts = stream(40);
    for (const auto rate : {DvbsCodeRate::R1_2, DvbsCodeRate::R2_3, DvbsCodeRate::R3_4,
                            DvbsCodeRate::R5_6, DvbsCodeRate::R7_8}) {
        const auto iq = render(rate, ts);
        auto rx = decode::DvbsDecoder::create(rx_config(rate));
        REQUIRE(rx);
        const auto r = run(*rx, iq);
        std::printf("DVB-S %s noiseless: %zu packets, %u data, %llu RS failed\n",
                    decode::dvbs_code_rate_name(rate), r.packets.size(), r.data_seen,
                    static_cast<unsigned long long>(r.rs_failed));
        CHECK(rx->locked());
        CHECK(r.rs_failed == 0);
        CHECK(r.data_seen >= 30);
        CHECK(r.data_in_order == r.data_seen);
        CHECK(r.psi.sdt());
    }
}

TEST_CASE("DVB-S finds the code rate and spectral inversion itself", "[dvbs]") {
    const auto ts = stream(30);
    auto iq = render(DvbsCodeRate::R3_4, ts);
    for (auto& z : iq) z = std::conj(z);
    auto rx = decode::DvbsDecoder::create(rx_config(std::nullopt));
    REQUIRE(rx);
    const auto r = run(*rx, iq);
    REQUIRE(rx->code_rate());
    CHECK(*rx->code_rate() == DvbsCodeRate::R3_4);
    CHECK(rx->spectrum_inverted());
    CHECK(r.data_seen >= 20);
    CHECK(r.rs_failed == 0);
}

TEST_CASE("DVB-S rate 1/2 in AWGN with Doppler", "[dvbs]") {
    const auto ts = stream(60);
    const auto clean = render(DvbsCodeRate::R1_2, ts);
    // Table 3: 4.5 dB Eb/N0 for QEF at rate 1/2, against the useful bit rate
    // before RS coding.
    const double useful = 2.0 * kSymbolRate * 0.5 * 188.0 / 204.0;
    for (const double ebn0 : {4.5, 3.5}) {
        siggen::ChannelConfig ch;
        ch.frequency.doppler_shift_hz = 2500;
        ch.noise = siggen::NoiseLevel::eb_over_n0_db(ebn0, useful);
        auto out = siggen::apply_channel(clean, ch, kRate, 77);
        REQUIRE(out);
        auto rx = decode::DvbsDecoder::create(rx_config(DvbsCodeRate::R1_2));
        REQUIRE(rx);
        const auto r = run(*rx, out->samples);
        std::printf("DVB-S 1/2 Eb/N0 %.1f dB, +2500 Hz: %zu packets, %u data, %llu RS failed, "
                    "%llu bytes corrected, offset %.0f Hz\n",
                    ebn0, r.packets.size(), r.data_seen,
                    static_cast<unsigned long long>(r.rs_failed),
                    static_cast<unsigned long long>(rx->stats().rs_corrected_bytes),
                    rx->frequency_offset_hz());
        if (ebn0 >= 4.5) {
            CHECK(r.rs_failed == 0);
            CHECK(r.data_seen >= 50);
        }
    }
}

TEST_CASE("DVB-S output is independent of chunking", "[dvbs]") {
    const auto iq = render(DvbsCodeRate::R2_3, stream(25));
    auto a = decode::DvbsDecoder::create(rx_config(DvbsCodeRate::R2_3));
    auto b = decode::DvbsDecoder::create(rx_config(DvbsCodeRate::R2_3));
    REQUIRE(a);
    REQUIRE(b);
    const auto ra = run(*a, iq);
    const auto rb = run(*b, iq, 12345);
    REQUIRE(ra.packets.size() == rb.packets.size());
    CHECK(!ra.packets.empty());
    for (std::size_t i = 0; i < ra.packets.size(); ++i) {
        REQUIRE(ra.packets[i].bytes == rb.packets[i].bytes);
    }
}
