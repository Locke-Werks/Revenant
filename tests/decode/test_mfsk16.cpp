// MFSK16 and MFSK8: the printed tables of the ZL1BPU and IZ8BLY documents
// cited in core/decode/mfsk16.h, then the receiver against the transmitter.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/decode/mfsk16.h"
#include "core/decode/viterbi_stream.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/mfsk16_mod.h"
#include "core/dsp/synth/rds_mod.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 8000;

const std::string kText =
    "cq cq de zl1bpu the quick brown fox jumps over the lazy dog 0123456789 "
    "MFSK16 Test, with Capitals & symbols: (ok?) = yes/no; \"quoted\" @ 42% "
    "long path dx on twenty metres sounds like music. ";

double mean_power(const std::vector<float>& x) {
    double sum = 0.0;
    for (const float v : x) {
        sum += static_cast<double>(v) * v;
    }
    return x.empty() ? 0.0 : sum / static_cast<double>(x.size());
}

std::size_t edit_distance(const std::string& a, const std::string& b) {
    std::vector<std::size_t> prev(b.size() + 1);
    std::vector<std::size_t> cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) {
        prev[j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1,
                               prev[j - 1] + (a[i - 1] == b[j - 1] ? 0U : 1U)});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

std::string decode_text(const decode::Mfsk16Config& cfg, const std::vector<float>& audio,
                        std::size_t chunk = 0) {
    auto dec = decode::Mfsk16Decoder::create(cfg);
    REQUIRE(dec.has_value());
    std::vector<decode::Mfsk16Character> out;
    if (chunk == 0) {
        dec->process(audio, out);
    } else {
        for (std::size_t at = 0; at < audio.size(); at += chunk) {
            const std::size_t n = std::min(chunk, audio.size() - at);
            dec->process(std::span<const float>(audio.data() + at, n), out);
        }
    }
    dec->flush(out);
    std::string s;
    for (const auto& c : out) {
        s.push_back(static_cast<char>(c.code));
    }
    return s;
}

double cer(const std::string& got) {
    return static_cast<double>(edit_distance(kText, got)) / static_cast<double>(kText.size());
}

}  // namespace

TEST_CASE("MFSK varicode matches the printed examples and frames every entry", "[mfsk16]") {
    // Varicode page: 'to send the characters "e t", the bits "10001001100"'.
    std::vector<std::uint8_t> bits;
    decode::mfsk_varicode_append("e t", bits);
    std::string s;
    for (const auto b : bits) {
        s.push_back(static_cast<char>('0' + b));
    }
    CHECK(s == "10001001100");

    // Spec C.10: framed by "001", so every code ends in 00 and holds no 001.
    for (const auto code : decode::kMfskVaricode) {
        CHECK(code.front() == '1');
        CHECK(code.ends_with("00"));
        CHECK(code.find("001") == std::string_view::npos);
        CHECK(code.size() <= decode::kMfskVaricodeMaxBits);
    }

    // Every character code through the bit decoder and back.
    std::string all;
    for (int c = 0; c < 256; ++c) {
        all.push_back(static_cast<char>(c));
    }
    bits.clear();
    decode::mfsk_varicode_append(all, bits);
    bits.push_back(1);
    decode::MfskVaricodeDecoder dec;
    std::vector<int> got;
    for (const auto b : bits) {
        if (const int c = dec.push(b); c >= 0) {
            got.push_back(c);
        }
    }
    REQUIRE(got.size() == 256);
    for (int c = 0; c < 256; ++c) {
        CHECK(got[static_cast<std::size_t>(c)] == c);
    }

    // Spec C.11: NUL then a long run of zeros is rejected as invalid, and
    // the next character still prints.
    dec.reset();
    bits.clear();
    decode::mfsk_varicode_append(std::string(1, '\0'), bits);
    bits.insert(bits.end(), 40, 0);
    decode::mfsk_varicode_append("a", bits);
    bits.push_back(1);
    got.clear();
    for (const auto b : bits) {
        if (const int c = dec.push(b); c >= 0) {
            got.push_back(c);
        }
    }
    CHECK(got == std::vector<int>{'a'});
}

TEST_CASE("MFSK16 Gray weighting is spec C.13's table", "[mfsk16]") {
    const int printed[16] = {0b0000, 0b0001, 0b0011, 0b0010, 0b0110, 0b0111, 0b0101, 0b0100,
                             0b1100, 0b1101, 0b1111, 0b1110, 0b1010, 0b1011, 0b1001, 0b1000};
    for (int t = 0; t < 16; ++t) {
        CHECK(decode::mfsk_gray(t) == printed[t]);
        CHECK(decode::mfsk_gray_inverse(printed[t]) == t);
    }
    for (int t = 0; t < 32; ++t) {
        CHECK(decode::mfsk_gray_inverse(decode::mfsk_gray(t)) == t);
    }
}

TEST_CASE("MFSK FEC generators are the NASA K=7 pair", "[mfsk16]") {
    // FEC page: Dibit1 = O1+O3+O4+O6+O7, Dibit2 = O1+O2+O3+O4+O7, O1 as D^0.
    CHECK(decode::kMfskFecGenerators[0] == decode::kCcsdsK7G2);
    CHECK(decode::kMfskFecGenerators[1] == decode::kCcsdsK7G1);
}

TEST_CASE("Diagonal interleaver reproduces the Interleaver page's example", "[mfsk16]") {
    // Label bit b of input symbol k as k*4+b+1; zero is the initial fill.
    decode::MfskDiagonalDelay<std::uint8_t> tx(4, false);
    decode::MfskDiagonalDelay<std::uint8_t> rx(4, true);
    std::vector<std::vector<std::uint8_t>> sent;
    std::vector<std::vector<std::uint8_t>> received;
    for (int k = 0; k < 60; ++k) {
        std::vector<std::uint8_t> in(4);
        for (int b = 0; b < 4; ++b) {
            in[static_cast<std::size_t>(b)] = static_cast<std::uint8_t>(k * 4 + b + 1);
        }
        std::vector<std::uint8_t> out(4);
        std::vector<std::uint8_t> back(4);
        tx.push(in, out);
        rx.push(out, back);
        sent.push_back(out);
        received.push_back(back);
    }
    auto label = [](int sym, int bit) { return static_cast<std::uint8_t>(sym * 4 + bit + 1); };
    // "A E I M ... eB iF ... -fC -jG ... --gD": A first alone, B ten symbols
    // later beside e, C twenty later beside f, D thirty later beside g.
    CHECK(sent[0] == std::vector<std::uint8_t>{label(0, 0), 0, 0, 0});
    CHECK(sent[10][0] == label(10, 0));
    CHECK(sent[10][1] == label(0, 1));
    CHECK(sent[20][1] == label(10, 1));
    CHECK(sent[20][2] == label(0, 2));
    CHECK(sent[30][2] == label(10, 2));
    CHECK(sent[30][3] == label(0, 3));
    // The receiver restores whole symbols 30 symbols late.
    for (int k = 30; k < 60; ++k) {
        for (int b = 0; b < 4; ++b) {
            CHECK(received[static_cast<std::size_t>(k)][static_cast<std::size_t>(b)] ==
                  label(k - 30, b));
        }
    }
}

TEST_CASE("MFSK16 and MFSK8 round trip clean, offset, searched and on LSB", "[mfsk16]") {
    for (const auto mode : {decode::Mfsk16Mode::Mfsk16, decode::Mfsk16Mode::Mfsk8}) {
        for (const bool lsb : {false, true}) {
            siggen::Mfsk16ModConfig mod;
            mod.mode = mode;
            mod.base_hz = 1234.5;
            mod.lower_sideband = lsb;
            mod.lead_in_samples = 3001.7;
            mod.tail_samples = 4000;
            auto audio = siggen::mfsk16_render(mod, kText);
            REQUIRE(audio.has_value());

            decode::Mfsk16Config cfg;
            cfg.mode = mode;
            cfg.lower_sideband = lsb;
            cfg.search = true;
            const std::string got = decode_text(cfg, *audio);
            INFO("mode " << static_cast<int>(mode) << " lsb " << lsb << " got: " << got);
            CHECK(got == kText);
        }
    }
}

TEST_CASE("MFSK16 output does not depend on chunking", "[mfsk16]") {
    siggen::Mfsk16ModConfig mod;
    mod.base_hz = 900.0;
    mod.lead_in_samples = 777.0;
    auto audio = siggen::mfsk16_render(mod, kText);
    REQUIRE(audio.has_value());
    REQUIRE(siggen::add_real_awgn(*audio, mean_power(*audio),
                                  siggen::NoiseLevel::snr_in_2500_hz_db(-6.0), kRate, 7)
                .has_value());
    decode::Mfsk16Config cfg;
    const std::string whole = decode_text(cfg, *audio);
    CHECK(decode_text(cfg, *audio, 1) == whole);
    CHECK(decode_text(cfg, *audio, 333) == whole);
    CHECK(decode_text(cfg, *audio, 4096) == whole);
}

TEST_CASE("MFSK16 and MFSK8 through noise, with drift", "[mfsk16][noise]") {
    struct Point {
        decode::Mfsk16Mode mode;
        double snr;
        double limit;
    };
    // Limits of 1 are measurement points. The 0 dB figure is not zero
    // because noise before the signal can leave one character completed by
    // the first data bits, its bits mostly the preamble's agreeing zeros.
    const Point points[] = {
        {decode::Mfsk16Mode::Mfsk16, 0.0, 0.01},  {decode::Mfsk16Mode::Mfsk16, -9.0, 0.01},
        {decode::Mfsk16Mode::Mfsk16, -12.0, 1.0}, {decode::Mfsk16Mode::Mfsk16, -13.0, 1.0},
        {decode::Mfsk16Mode::Mfsk16, -14.0, 1.0}, {decode::Mfsk16Mode::Mfsk8, -10.0, 0.01},
        {decode::Mfsk16Mode::Mfsk8, -12.0, 1.0},  {decode::Mfsk16Mode::Mfsk8, -13.0, 1.0},
    };
    for (const Point& p : points) {
        double total = 0.0;
        const int runs = 4;
        for (int r = 0; r < runs; ++r) {
            siggen::Mfsk16ModConfig mod;
            mod.mode = p.mode;
            mod.base_hz = 1500.0 + 3.3 * r;
            mod.drift_hz_per_s = 0.1;
            mod.lead_in_samples = 4000.0 + 17.0 * r;
            mod.tail_samples = 4000;
            auto audio = siggen::mfsk16_render(mod, kText);
            REQUIRE(audio.has_value());
            REQUIRE(siggen::add_real_awgn(*audio, mean_power(*audio),
                                          siggen::NoiseLevel::snr_in_2500_hz_db(p.snr), kRate,
                                          100 + static_cast<std::uint64_t>(r))
                        .has_value());
            decode::Mfsk16Config cfg;
            cfg.mode = p.mode;
            total += cer(decode_text(cfg, *audio));
        }
        const double mean = total / runs;
        std::printf("MFSK%s at %.1f dB in 2500 Hz: CER %.4f over %zu characters x %d\n",
                    p.mode == decode::Mfsk16Mode::Mfsk16 ? "16" : "8", p.snr, mean, kText.size(),
                    runs);
        CHECK(mean <= p.limit);
    }
}
