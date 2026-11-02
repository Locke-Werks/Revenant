// FT8 and the 77-bit message layer: the paper's own tables and examples, the
// public-domain matrices against each other, and the decoder against the
// transmitter in core/dsp/synth/ft8_mod.h.
//
// Source: Franke, Somerville and Taylor, "The FT4 and FT8 Communication
// Protocols", QEX July/August 2020 pp. 7-17, and its reference [14].
//
// Decode rates are measured and reported in WARN lines with loose
// assertions, for the reason tests/decode/CMakeLists.txt gives.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/decode/ft8.h"
#include "core/decode/ft_ldpc.h"
#include "core/decode/wsjt77.h"
#include "core/dsp/synth/ft8_mod.h"

using namespace revenant;

namespace {

decode::WsjtHashTable known_calls() {
    decode::WsjtHashTable h;
    for (const char* c : {"KH1/KH7Z", "W9XYZ", "G4ABC", "PA9XYZ", "K1ABC", "PJ4/K1ABC"}) {
        h.remember(c);
    }
    return h;
}

// Real white noise whose power in 2500 Hz sits `snr_db` below a sinusoid of
// amplitude `amplitude`: the paper's SNR convention.
void add_noise(std::vector<float>& x, double amplitude, double snr_db, std::uint32_t seed) {
    const double s = amplitude * amplitude / 2.0;
    const double n2500 = s / std::pow(10.0, snr_db / 10.0);
    const double sigma = std::sqrt(n2500 * 6000.0 / 2500.0);
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0.0, sigma);
    for (auto& v : x) v += static_cast<float>(g(rng));
}

std::vector<std::string> texts(const std::vector<decode::FtDecode>& d) {
    std::vector<std::string> out;
    for (const auto& x : d) out.push_back(x.message.text);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST_CASE("Table 1 examples pack and unpack to themselves", "[ft8][wsjt77]") {
    struct Row {
        const char* text;
        int i3;
        int n3;
    };
    // Table 1, every row, as printed.
    const Row rows[] = {
        {"TNX BOB 73 GL", 0, 0},
        {"K1ABC RR73; W9XYZ <KH1/KH7Z> -08", 0, 1},
        {"K1ABC W9XYZ 6A WI", 0, 3},
        {"W9XYZ K1ABC R 17B EMA", 0, 4},
        {"K1ABC/R W9XYZ/R R EN37", 1, -1},
        {"G4ABC/P PA9XYZ JO22", 2, -1},
        {"K1ABC W9XYZ 579 WI", 3, -1},
        {"<W9XYZ> PJ4/K1ABC RRR", 4, -1},
        {"<G4ABC> <PA9XYZ> R 570007 JO22DB", 5, -1},
    };
    for (const auto& r : rows) {
        INFO(r.text);
        auto bits = decode::wsjt_pack(r.text);
        REQUIRE(bits);
        auto h = known_calls();
        const auto m = decode::wsjt_unpack(*bits, &h);
        CHECK(m.i3 == r.i3);
        if (r.n3 >= 0) CHECK(m.n3 == r.n3);
        CHECK(m.text == r.text);
    }
    // Type 0.5 has its own entry point; Table 1's example.
    auto t = decode::wsjt_pack_telemetry("123456789ABCDEF012");
    REQUIRE(t);
    const auto m = decode::wsjt_unpack(*t);
    CHECK(m.i3 == 0);
    CHECK(m.n3 == 5);
    CHECK(m.text == "123456789ABCDEF012");
}

TEST_CASE("Common QSO messages survive packing", "[ft8][wsjt77]") {
    for (const char* text : {"CQ K1ABC FN42", "CQ DX K1ABC FN42", "CQ 145 K1ABC FN42",
                             "K1ABC W9XYZ -11", "K1ABC W9XYZ R-11", "K1ABC W9XYZ RRR",
                             "K1ABC W9XYZ RR73", "K1ABC W9XYZ 73", "QRZ W9XYZ", "CQ PJ4/K1ABC",
                             "TU; K1ABC W9XYZ R 559 0013", "K1ABC W9XYZ 539 MA",
                             "PJ4/K1ABC <W9XYZ> 73", "<K1ABC> KH1/KH7Z", "HELLO WORLD"}) {
        INFO(text);
        auto bits = decode::wsjt_pack(text);
        REQUIRE(bits);
        auto h = known_calls();
        CHECK(decode::wsjt_unpack(*bits, &h).text == text);
    }
    // A call that is not standard goes in a type 1 message as a hash.
    auto h = known_calls();
    auto bits = decode::wsjt_pack("K1ABC <KH1/KH7Z> -15");
    REQUIRE(bits);
    CHECK(decode::wsjt_unpack(*bits, &h).text == "K1ABC <KH1/KH7Z> -15");
    decode::WsjtHashTable empty;
    CHECK(decode::wsjt_unpack(*bits, &empty).text == "K1ABC <...> -15");
}

TEST_CASE("Table 7 c28 ranges", "[ft8][wsjt77]") {
    const std::pair<const char*, std::uint32_t> rows[] = {
        {"DE", 0},           {"QRZ", 1},           {"CQ", 2},           {"CQ 000", 3},
        {"CQ 999", 1002},    {"CQ A", 1004},       {"CQ Z", 1029},      {"CQ AA", 1031},
        {"CQ ZZ", 1731},     {"CQ AAA", 1760},     {"CQ ZZZ", 20685},   {"CQ AAAA", 21443},
        {"CQ ZZZZ", 532443},
    };
    for (const auto& [w, v] : rows) {
        INFO(w);
        auto c = decode::wsjt_c28(w);
        REQUIRE(c);
        CHECK(*c == v);
    }
    // Standard calls start at 6257896 (Table 7); 22-bit hashes at 2063592.
    auto lowest = decode::wsjt_c28("0A0");
    REQUIRE(lowest);
    CHECK(*lowest >= 6257896U);
    auto hashed = decode::wsjt_c28("<PJ4/K1ABC>");
    REQUIRE(hashed);
    CHECK(*hashed >= 2063592U);
    CHECK(*hashed < 2063592U + 4194304U);
    // Section 2: 37 * 36 * 10 * 27 * 27 * 27 calls fit in what remains.
    CHECK(37ULL * 36 * 10 * 27 * 27 * 27 == 262177560ULL);
    CHECK((1ULL << 28) - 262177560ULL == 6257896ULL);
}

TEST_CASE("g15 and g25 per grid4_to_g15 and grid6_to_g25", "[ft8][wsjt77]") {
    CHECK(*decode::wsjt_g15("AA00") == 0U);
    CHECK(*decode::wsjt_g15("RR99") == 32399U);
    CHECK(*decode::wsjt_g15("") == 32401U);
    CHECK(*decode::wsjt_g15("RRR") == 32402U);
    CHECK(*decode::wsjt_g15("RR73") == 32403U);
    CHECK(*decode::wsjt_g15("73") == 32404U);
    CHECK(*decode::wsjt_g15("-30") == 32405U);
    CHECK(*decode::wsjt_g15("+99") == 32534U);
    CHECK(32400U + 134U < (1U << 15));
    CHECK(*decode::wsjt_g25("AA00AA") == 0U);
    CHECK(*decode::wsjt_g25("RR99XX") == 18U * 18 * 10 * 10 * 24 * 24 - 1);
    CHECK(!decode::wsjt_g15("SS00"));
}

TEST_CASE("Hash codes take the top bits of a 64-bit product", "[ft8][wsjt77]") {
    // hashcodes.f90: the shorter hashes are prefixes of the longer.
    for (const char* c : {"PJ4/K1ABC", "YW18FIFA", "K1ABC"}) {
        const auto h22 = decode::wsjt_hash(c, 22);
        CHECK(decode::wsjt_hash(c, 12) == h22 >> 10);
        CHECK(decode::wsjt_hash(c, 10) == h22 >> 12);
        CHECK(h22 < (1U << 22));
    }
    CHECK(decode::wsjt_hash("K1ABC", 22) != decode::wsjt_hash("W9XYZ", 22));
}

TEST_CASE("CRC-14 matches a long division by 0x6757", "[ft8][wsjt77]") {
    std::mt19937 rng(7);
    for (int trial = 0; trial < 50; ++trial) {
        std::vector<std::uint8_t> m(77);
        for (auto& b : m) b = static_cast<std::uint8_t>(rng() & 1U);
        // Section 3: the remainder of the 82-bit zero-extended message times
        // x^14, divided by x^14 + 0x6757.
        std::vector<std::uint8_t> d(m);
        d.resize(82 + 14, 0);
        constexpr std::uint32_t poly = 0x6757 | (1U << 14);
        for (std::size_t i = 0; i < 82; ++i) {
            if (d[i] == 0) continue;
            for (int j = 0; j <= 14; ++j) d[i + static_cast<std::size_t>(j)] ^= static_cast<std::uint8_t>((poly >> (14 - j)) & 1U);
        }
        const auto crc = decode::wsjt_crc14(m);
        for (int j = 0; j < 14; ++j) CHECK(crc[static_cast<std::size_t>(j)] == d[82 + static_cast<std::size_t>(j)]);
    }
}

TEST_CASE("gen_crc14's example message", "[ft8][wsjt77]") {
    // gen_crc14.f90's usage line prints this 77-bit message.
    const std::string s = "00000000000000000000000000100000010011011111110011011100100010100001010000001";
    decode::Wsjt77 bits{};
    for (std::size_t i = 0; i < 77; ++i) bits[i] = static_cast<std::uint8_t>(s[i] - '0');
    const auto m = decode::wsjt_unpack(bits);
    WARN("gen_crc14 example unpacks to: " << m.text);
    CHECK(m.i3 == 1);
    CHECK(m.call_to == "CQ");
    CHECK(decode::wsjt_is_standard_call(m.call_de));
    CHECK(m.grid.size() == 4);
    auto repacked = decode::wsjt_pack(m.text);
    REQUIRE(repacked);
    CHECK(*repacked == bits);
}

TEST_CASE("generator.dat and parity.dat describe the same code", "[ft8][ldpc]") {
    const auto& checks = decode::ft_ldpc_checks();
    REQUIRE(checks.size() == 83);
    // parity.dat's header: three ones per column, six or seven per row.
    std::vector<int> col(174, 0);
    for (const auto& row : checks) {
        CHECK((row.size() == 6 || row.size() == 7));
        for (const auto c : row) ++col[c];
    }
    CHECK(std::all_of(col.begin(), col.end(), [](int c) { return c == 3; }));
    auto dec = decode::ft_ldpc_decoder();
    REQUIRE(dec);
    std::mt19937 rng(11);
    for (int trial = 0; trial < 100; ++trial) {
        std::array<std::uint8_t, 91> m{};
        for (auto& b : m) b = static_cast<std::uint8_t>(rng() & 1U);
        std::array<std::uint8_t, 174> cw{};
        decode::ft_ldpc_encode(m, cw);
        CHECK(dec->syndrome_zero(cw));
    }
}

TEST_CASE("FT8 channel symbols carry the Costas arrays and the Gray map", "[ft8]") {
    const auto& p = decode::ft8_protocol();
    REQUIRE(p.layout.size() == 79);
    auto bits = decode::wsjt_pack("CQ K1ABC FN42");
    REQUIRE(bits);
    const auto tones = decode::ft_channel_tones(p, *bits);
    REQUIRE(tones.size() == 79);
    const int costas[7] = {3, 1, 4, 0, 6, 5, 2};
    for (int i = 0; i < 7; ++i) {
        CHECK(tones[static_cast<std::size_t>(i)] == costas[i]);
        CHECK(tones[36 + static_cast<std::size_t>(i)] == costas[i]);
        CHECK(tones[72 + static_cast<std::size_t>(i)] == costas[i]);
    }
    // Table 3: tone 2 is 011, tone 7 is 111.
    CHECK(p.gray[2] == 0b011);
    CHECK(p.gray[7] == 0b111);
    // 12.64 s at 6.25 baud.
    CHECK(79 * p.samples_per_symbol == 151680);
}

TEST_CASE("FT8 decodes several signals in one slot", "[ft8]") {
    const std::vector<std::pair<const char*, double>> sigs = {
        {"CQ K1ABC FN42", 600.0},       {"K1ABC W9XYZ -11", 900.0},
        {"W9XYZ K1ABC R-09", 1250.5},   {"K1ABC W9XYZ RR73", 1600.0},
        {"CQ DX G4ABC IO91", 2100.0},   {"TNX BOB 73 GL", 2600.25},
    };
    std::vector<float> slot(180000, 0.0F);
    double dt = 0.2;
    for (const auto& [text, f] : sigs) {
        siggen::FtModConfig c;
        c.frequency_hz = f;
        c.start_seconds = dt;
        c.amplitude = 0.1;
        dt += 0.25;
        REQUIRE(siggen::ft_add_text(c, text, slot));
    }
    add_noise(slot, 0.1, -12.0, 3);
    decode::FtConfig cfg;
    auto dec = decode::FtDecoder::create(cfg);
    REQUIRE(dec);
    std::vector<decode::FtDecode> out;
    dec->decode_slot(slot, 0.0, 0, out);
    std::vector<std::string> want;
    for (const auto& s : sigs) want.emplace_back(s.first);
    std::sort(want.begin(), want.end());
    CHECK(texts(out) == want);
    for (const auto& d : out) {
        for (const auto& [text, f] : sigs) {
            if (d.message.text == text) {
                CHECK(std::abs(d.frequency_hz - f) < 1.0);
                CHECK(std::abs(d.snr_db + 12.0) < 3.0);
            }
        }
    }
}

TEST_CASE("FT8 stream: slot alignment, 48 kHz input and chunk independence", "[ft8]") {
    // Sample 0 is 7.25 s into a slot, so the first slot boundary is 7.75 s
    // later. Two slots after that.
    const double t0 = 1'699'999'995.0 + 7.25;  // 1699999995 is a multiple of 15
    const int rate = 48000;
    const std::size_t lead = static_cast<std::size_t>(7.75 * rate);
    std::vector<float> audio(lead + 2 * 15 * rate + rate, 0.0F);
    siggen::FtModConfig c;
    c.rate = rate;
    c.amplitude = 0.1;
    c.frequency_hz = 1500.0;
    c.start_seconds = 7.75 + 0.5;
    REQUIRE(siggen::ft_add_text(c, "CQ K1ABC FN42", audio));
    c.frequency_hz = 800.0;
    c.start_seconds = 7.75 + 15.0 + 0.8;
    REQUIRE(siggen::ft_add_text(c, "K1ABC W9XYZ R-15", audio));
    add_noise(audio, 0.1 * 2.0, -10.0, 9);  // 48 kHz: four times the bandwidth

    const auto run = [&](std::uint32_t seed) {
        decode::FtConfig cfg;
        cfg.rate = rate;
        cfg.utc_of_sample = [t0](dsp::SampleIndex) { return t0; };
        auto dec = decode::FtDecoder::create(cfg);
        REQUIRE(dec);
        std::vector<decode::FtDecode> out;
        std::mt19937 rng(seed);
        std::size_t pos = 0;
        while (pos < audio.size()) {
            const std::size_t n = std::min<std::size_t>(audio.size() - pos, 1 + rng() % 30000);
            REQUIRE(dec->process(std::span<const float>(audio.data() + pos, n), out));
            pos += n;
        }
        return out;
    };
    const auto a = run(1);
    const auto b = run(2);
    REQUIRE(a.size() == 2);
    CHECK(a[0].message.text == "CQ K1ABC FN42");
    CHECK(a[1].message.text == "K1ABC W9XYZ R-15");
    CHECK(std::fmod(a[0].slot_utc, 15.0) == 0.0);
    CHECK(a[1].slot_utc == a[0].slot_utc + 15.0);
    CHECK(std::abs(a[0].dt_seconds) < 0.05);
    CHECK(std::abs(a[1].dt_seconds - 0.3) < 0.05);
    CHECK(a[0].slot_start_sample == lead);
    REQUIRE(b.size() == a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].message.text == b[i].message.text);
        CHECK(a[i].frequency_hz == b[i].frequency_hz);
        CHECK(a[i].snr_db == b[i].snr_db);
    }
}

TEST_CASE("FT8 decode threshold in AWGN", "[ft8][threshold]") {
    // Ten signals a slot at one SNR, spread across the passband and in time.
    decode::FtConfig cfg;
    auto dec = decode::FtDecoder::create(cfg);
    REQUIRE(dec);
    const char* msgs[] = {"CQ K1ABC FN42", "K1ABC W9XYZ -11", "W9XYZ K1ABC R-09",
                          "K1ABC W9XYZ RR73", "CQ DX G4ABC IO91", "G4ABC PA9XYZ JO22",
                          "PA9XYZ G4ABC -20", "CQ W9XYZ EN37", "W9XYZ PA9XYZ 73",
                          "K1ABC G4ABC RRR"};
    double threshold = 0.0;
    for (int snr = -14; snr >= -22; --snr) {
        int ok = 0;
        int total = 0;
        for (int trial = 0; trial < 2; ++trial) {
            std::vector<float> slot(180000, 0.0F);
            for (int i = 0; i < 10; ++i) {
                siggen::FtModConfig c;
                c.amplitude = 0.05;
                c.frequency_hz = 300.0 + 240.0 * i + 17.0 * trial;
                c.start_seconds = 0.3 + 0.13 * i;
                REQUIRE(siggen::ft_add_text(c, msgs[i], slot));
            }
            add_noise(slot, 0.05, snr, static_cast<std::uint32_t>(100 + snr * 7 + trial));
            std::vector<decode::FtDecode> out;
            dec->decode_slot(slot, 0.0, 0, out);
            std::set<std::string> got;
            for (const auto& d : out) got.insert(d.message.text);
            for (const char* m : msgs) ok += got.contains(m) ? 1 : 0;
            total += 10;
            // Nothing decoded that was not sent.
            for (const auto& g : got) {
                CHECK(std::find_if(std::begin(msgs), std::end(msgs), [&](const char* m) { return g == m; }) != std::end(msgs));
            }
        }
        const double rate = static_cast<double>(ok) / total;
        WARN("FT8 AWGN SNR " << snr << " dB: " << ok << "/" << total << " decoded");
        if (rate >= 0.5) threshold = snr;
        if (snr >= -16) CHECK(rate >= 0.9);
    }
    WARN("FT8 lowest SNR with at least half decoded: " << threshold << " dB");
    CHECK(threshold <= -18.0);
}
