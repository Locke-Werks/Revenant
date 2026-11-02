// 2G ALE: the code against MIL-STD-188-141D Appendix A's printed examples,
// and the decoder against the transmitter through noise, a two-path channel,
// a mistuned receiver and arbitrary chunking.
//
// Error rates are measured and reported in WARN lines with loose assertions,
// for the reason tests/decode/CMakeLists.txt gives.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "core/decode/ale_2g.h"
#include "core/dsp/synth/ale_2g_mod.h"

using namespace revenant;
using decode::AlePreamble;

namespace {

constexpr dsp::SampleRate kRate = 8000;

// Signal-to-noise ratio in a 3 kHz bandwidth, white noise across the
// 4 kHz Nyquist band.
void add_noise(std::vector<float>& x, double snr_db, double amplitude, std::uint32_t seed) {
    const double signal = amplitude * amplitude / 2.0;
    const double noise_3k = signal / std::pow(10.0, snr_db / 10.0);
    const double sigma = std::sqrt(noise_3k * (static_cast<double>(kRate) / 2.0) / 3000.0);
    std::mt19937 rng(seed);
    std::normal_distribution<double> n(0.0, sigma);
    for (auto& v : x) {
        v = static_cast<float>(v + n(rng));
    }
}

// Fixed two-tap channel: a second path 6 dB down and 2 ms late.
std::vector<float> two_path(const std::vector<float>& x) {
    const std::size_t delay = static_cast<std::size_t>(kRate) * 2 / 1000;
    std::vector<float> y(x.size(), 0.0F);
    for (std::size_t i = 0; i < x.size(); ++i) {
        y[i] = x[i] + (i >= delay ? 0.5F * x[i - delay] : 0.0F);
    }
    return y;
}

std::vector<std::uint32_t> sample_call() {
    // A scan call: the first called word repeated, then the whole address,
    // then the conclusion (A.5.2.3.2, A.5.5).
    std::vector<std::uint32_t> words;
    const auto to = decode::ale_address_words(AlePreamble::To, "MIAMI");
    for (int i = 0; i < 4; ++i) {
        words.push_back(to[0]);
    }
    words.insert(words.end(), to.begin(), to.end());
    const auto tis = decode::ale_address_words(AlePreamble::Tis, "K4XYZ");
    words.insert(words.end(), tis.begin(), tis.end());
    return words;
}

struct Result {
    std::vector<decode::AleWord> words;
    std::vector<decode::AleFrame> frames;
};

Result run(const std::vector<float>& audio, std::size_t chunk = 0) {
    auto dec = decode::Ale2gDecoder::create({});
    REQUIRE(dec.has_value());
    Result r;
    if (chunk == 0) {
        dec->process(audio, r.words, r.frames);
    } else {
        for (std::size_t i = 0; i < audio.size(); i += chunk) {
            const std::size_t n = std::min(chunk, audio.size() - i);
            dec->process(std::span<const float>(audio.data() + i, n), r.words, r.frames);
        }
    }
    dec->flush(r.frames);
    return r;
}

std::vector<float> render(const std::vector<std::uint32_t>& words, double offset_hz = 0.0,
                          double lead = 2000.0) {
    siggen::Ale2gModConfig mc;
    mc.rate = kRate;
    mc.offset_hz = offset_hz;
    mc.lead_in_samples = lead;
    mc.tail_samples = 4000.0;
    auto audio = siggen::ale_2g_render(mc, words);
    REQUIRE(audio.has_value());
    return *audio;
}

bool frame_ok(const Result& r) {
    for (const auto& f : r.frames) {
        if (f.kind == decode::AleFrame::Kind::Call && f.from == "K4XYZ" && f.accepting &&
            f.to.size() == 1 && f.to[0] == "MIAMI") {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("ALE Golay matches figure A-8's encoding example", "[ale2g]") {
    // Figure A-8: data 1101 0001 0101 encodes to check bits 0101 0110 0110.
    CHECK(decode::ale_golay_parity(0b110100010101) == 0b010101100110);
    // The identity rows of figure A-6 return the P rows.
    CHECK(decode::ale_golay_parity(0b100000000000) == 0b101011100011);
    CHECK(decode::ale_golay_parity(0b000000000001) == 0b010111000111);
}

TEST_CASE("ALE Golay H of figure A-7 annihilates every code word", "[ale2g]") {
    // Figure A-7, the P-transpose half, row by row: check bit i is the parity
    // of the data bits marked in row i.
    const std::uint16_t pt[12] = {
        0b111110010010, 0b011111001001, 0b110001110110, 0b011000111011,
        0b110010001111, 0b100111010101, 0b101101111000, 0b010110111100,
        0b001011011110, 0b000101101111, 0b111100100101, 0b101011100011,
    };
    for (std::uint32_t d = 0; d < 4096; ++d) {
        const std::uint16_t p = decode::ale_golay_parity(static_cast<std::uint16_t>(d));
        for (int i = 0; i < 12; ++i) {
            const int bit = std::popcount(static_cast<unsigned>(pt[i] & d)) & 1;
            REQUIRE(bit == ((p >> (11 - i)) & 1));
        }
    }
}

TEST_CASE("ALE Golay corrects three errors and flags four", "[ale2g]") {
    std::mt19937 rng(7);
    for (int trial = 0; trial < 2000; ++trial) {
        const auto d = static_cast<std::uint16_t>(rng() & 0xFFFU);
        const std::uint32_t cw = (static_cast<std::uint32_t>(d) << 12U) | decode::ale_golay_parity(d);
        const int nerr = trial % 5;
        std::uint32_t e = 0;
        while (std::popcount(e) < nerr) {
            e |= 1U << (rng() % 24);
        }
        const std::uint32_t y = cw ^ e;
        const auto r = decode::ale_golay_decode(static_cast<std::uint16_t>(y >> 12U),
                                                static_cast<std::uint16_t>(y & 0xFFFU));
        if (nerr <= 3) {
            REQUIRE(r.errors == nerr);
            REQUIRE(r.data == d);
        } else {
            REQUIRE(r.errors == -1);
        }
    }
}

TEST_CASE("ALE address stuffing follows A.5.2.4.3's MIAMI example", "[ale2g]") {
    const auto w = decode::ale_address_words(AlePreamble::To, "MIAMI");
    REQUIRE(w.size() == 2);
    CHECK(w[0] == decode::ale_word(AlePreamble::To, "MIA"));
    CHECK(w[1] == decode::ale_word(AlePreamble::Data, "MI@"));
    // Figure A-12: preamble in W1..W3.
    CHECK((w[0] >> 21U) == 0b010U);
    const auto long_addr = decode::ale_address_words(AlePreamble::Tis, "ABCDEFGHIJ");
    REQUIRE(long_addr.size() == 4);
    CHECK(decode::ale_word_preamble(long_addr[2]) == AlePreamble::Rep);
    CHECK(decode::ale_word_preamble(long_addr[3]) == AlePreamble::Data);
}

TEST_CASE("ALE word bits follow figure A-10", "[ale2g]") {
    const std::uint32_t word = decode::ale_word(AlePreamble::Tis, "ABC");
    const auto bits = decode::ale_word_bits(word);
    // A1 is W1, B1 is W13, A13 is G1, B13 is G13 inverted, S49 is zero.
    CHECK(bits[0] == ((word >> 23U) & 1U));
    CHECK(bits[1] == ((word >> 11U) & 1U));
    const auto ga = decode::ale_golay_parity(static_cast<std::uint16_t>(word >> 12U));
    const auto gb = decode::ale_golay_parity(static_cast<std::uint16_t>(word & 0xFFFU));
    CHECK(bits[24] == ((ga >> 11U) & 1U));
    CHECK(bits[25] == (((gb >> 11U) & 1U) ^ 1U));
    CHECK(bits[48] == 0);
    // 49 symbols per triple word (A.5.1.3).
    const std::vector<std::uint32_t> one = {word};
    CHECK(siggen::ale_2g_tones(one).size() == 49);
}

TEST_CASE("ALE decodes a clean scan call", "[ale2g]") {
    const auto words = sample_call();
    const auto r = run(render(words));
    CHECK(frame_ok(r));
    REQUIRE(r.frames.size() == 1);
    CHECK(r.frames[0].to == std::vector<std::string>{"MIAMI"});
}

TEST_CASE("ALE decodes a sound, and a TWAS refuses", "[ale2g]") {
    const auto words = decode::ale_address_words(AlePreamble::Twas, "NET1");
    const auto r = run(render(words));
    REQUIRE(r.frames.size() == 1);
    CHECK(r.frames[0].kind == decode::AleFrame::Kind::Sound);
    CHECK(r.frames[0].from == "NET1");
    CHECK_FALSE(r.frames[0].accepting);
}

TEST_CASE("ALE output does not depend on chunking", "[ale2g]") {
    auto audio = render(sample_call(), 0.0, 1234.5);
    add_noise(audio, 6.0, 0.5, 11);
    const auto whole = run(audio);
    for (const std::size_t chunk : {1U, 97U, 1000U}) {
        const auto parts = run(audio, chunk);
        REQUIRE(parts.words.size() == whole.words.size());
        for (std::size_t i = 0; i < whole.words.size(); ++i) {
            CHECK(parts.words[i].word == whole.words[i].word);
            CHECK(parts.words[i].position == whole.words[i].position);
        }
    }
}

TEST_CASE("ALE survives a 60 Hz mistuning", "[ale2g]") {
    for (const double off : {-60.0, 60.0}) {
        auto audio = render(sample_call(), off);
        add_noise(audio, 10.0, 0.5, 3);
        CHECK(frame_ok(run(audio)));
    }
}

TEST_CASE("ALE frame success against SNR, AWGN and two-path", "[ale2g]") {
    const auto words = sample_call();
    for (const bool multipath : {false, true}) {
        for (const double snr : {6.0, 3.0, 0.0, -3.0}) {
            int ok = 0;
            int word_ok = 0;
            int word_total = 0;
            constexpr int kTrials = 20;
            for (int t = 0; t < kTrials; ++t) {
                auto audio = render(words, 0.0, 1000.0 + 37.0 * t);
                if (multipath) {
                    audio = two_path(audio);
                }
                add_noise(audio, snr, multipath ? 0.75 : 0.5, static_cast<std::uint32_t>(100 + t));
                const auto r = run(audio);
                ok += frame_ok(r) ? 1 : 0;
                // Distinct sent words (the scan call's repeats count once).
                for (const std::uint32_t w : {words[4], words[5], words[6], words[7]}) {
                    ++word_total;
                    for (const auto& got : r.words) {
                        if (got.word == w) {
                            ++word_ok;
                            break;
                        }
                    }
                }
            }
            WARN("2G ALE " << (multipath ? "two-path" : "AWGN") << " SNR " << snr
                           << " dB/3 kHz: frames " << ok << "/" << kTrials << ", words "
                           << word_ok << "/" << word_total);
            if (snr >= 6.0) {
                CHECK(ok >= kTrials - 2);
            }
        }
    }
}

TEST_CASE("ALE noise alone decodes nothing", "[ale2g]") {
    std::vector<float> audio(static_cast<std::size_t>(kRate) * 60, 0.0F);
    add_noise(audio, 0.0, 0.5, 99);
    const auto r = run(audio);
    WARN("2G ALE noise only, 60 s: " << r.words.size() << " words");
    CHECK(r.words.size() <= 1);
}
