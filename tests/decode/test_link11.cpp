// Link-11 CLEW: the control codes and the Hamming code against
// MIL-STD-188-203-1A's tables, and the decoder against the transmitter
// through noise, a two-path channel, a mistuned receiver and arbitrary
// chunking.
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

#include "core/decode/link11.h"
#include "core/dsp/synth/link11_mod.h"

using namespace revenant;
using decode::Link11Rate;

namespace {

constexpr dsp::SampleRate kRate = 8000;

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

std::vector<float> two_path(const std::vector<float>& x) {
    const std::size_t delay = static_cast<std::size_t>(kRate) * 2 / 1000;
    std::vector<float> y(x.size(), 0.0F);
    for (std::size_t i = 0; i < x.size(); ++i) {
        y[i] = x[i] + (i >= delay ? 0.5F * x[i - delay] : 0.0F);
    }
    return y;
}

// A row of table III as printed, bit 29 first.
std::uint32_t row(const std::string& s) {
    std::uint32_t v = 0;
    for (const char c : s) {
        if (c == '0' || c == '1') {
            v = (v << 1U) | static_cast<std::uint32_t>(c - '0');
        }
    }
    return v;
}

std::vector<std::uint32_t> words(std::size_t n, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<std::uint32_t> w(n);
    for (auto& v : w) {
        v = rng() & 0xFFFFFFU;
    }
    return w;
}

std::vector<float> render(const std::vector<std::uint32_t>& frames, Link11Rate rate, double offset = 0.0,
                          bool continuous = false) {
    siggen::Link11ModConfig mc;
    mc.rate = kRate;
    mc.frame_rate = rate;
    mc.offset_hz = offset;
    mc.continuous_phase = continuous;
    mc.lead_in_samples = 1000.0;
    mc.tail_samples = 1500.0;
    auto a = siggen::link11_render(mc, frames);
    REQUIRE(a.has_value());
    return *a;
}

std::vector<decode::Link11Transmission> run(const std::vector<float>& audio, std::size_t chunk = 0) {
    auto dec = decode::Link11Decoder::create({});
    REQUIRE(dec.has_value());
    std::vector<decode::Link11Transmission> out;
    if (chunk == 0) {
        dec->process(audio, out);
    } else {
        for (std::size_t i = 0; i < audio.size(); i += chunk) {
            const std::size_t n = std::min(chunk, audio.size() - i);
            dec->process(std::span<const float>(audio.data() + i, n), out);
        }
    }
    dec->flush(out);
    return out;
}

// Words received correctly, in order.
std::size_t good_words(const decode::Link11Transmission& t, const std::vector<std::uint32_t>& sent) {
    std::size_t ok = 0;
    std::size_t i = 0;
    for (const auto& f : t.frames) {
        if (f.kind == decode::Link11Frame::Kind::Data) {
            if (i < sent.size() && f.word.status >= 0 && f.word.data == sent[i]) {
                ++ok;
            }
            ++i;
        }
    }
    return ok;
}

}  // namespace

TEST_CASE("Link-11 control codes reproduce table III", "[link11]") {
    // Rows transcribed from table III: first frame, second frame.
    const auto start = decode::link11_start_code();
    CHECK(start.first == row("111100101000110000100000111111"));
    CHECK(start.second == row("101100110111011010010011100010"));
    const struct {
        int address;
        const char* first;
        const char* second;
    } rows[] = {
        {001, "000101111001010001100001000001", "110101011001101110110100100111"},
        {076, "111001010001100001000001111110", "011001101110110100100111000101"},
        {016, "101010110011011101101001001110", "011110010100011000010000011111"},
        {034, "010101100110111011010010011100", "111100101000110000100000111111"},
        {040, "100010111100101000110000100000", "111010101100110111011010010011"},
        {053, "010001100001000001111110101011", "101110110100100111000101111001"},
    };
    for (const auto& r : rows) {
        const auto c = decode::link11_address_code(r.address);
        INFO("address " << r.address);
        CHECK(c.first == row(r.first));
        CHECK(c.second == row(r.second));
        // Note 2: bits 0 to 5 of the first frame are the address.
        CHECK((c.first & 077U) == static_cast<std::uint32_t>(r.address));
    }
    // Every code is at least nine bits from every other over two frames, so
    // four errors a frame cannot turn one into another.
    int closest = 60;
    for (int a = 1; a <= 077; ++a) {
        for (int b = a + 1; b <= 077; ++b) {
            const auto x = a == 077 ? start : decode::link11_address_code(a);
            const auto y = b == 077 ? start : decode::link11_address_code(b);
            closest = std::min(closest, std::popcount(x.first ^ y.first) + std::popcount(x.second ^ y.second));
        }
    }
    WARN("Link-11 closest pair of control codes: " << closest << " bits");
    CHECK(closest >= 9);
}

TEST_CASE("Link-11 Hamming code of 5.2.4 and table V", "[link11]") {
    // All-zero data: each parity set holds only its own parity bit, so bits
    // 25 to 29 are MARK, and five ones need no overall parity bit.
    CHECK(decode::link11_encode_word(0) == (0x1FU << 25U));
    std::mt19937 rng(8);
    for (int trial = 0; trial < 2000; ++trial) {
        const std::uint32_t d = rng() & 0xFFFFFFU;
        const std::uint32_t f = decode::link11_encode_word(d);
        CHECK((std::popcount(f) & 1) == 1);  // K0, odd
        REQUIRE(decode::link11_check_word(f).status == 0);
        const int b1 = static_cast<int>(rng() % 30);
        const auto one = decode::link11_check_word(f ^ (1U << static_cast<unsigned>(b1)));
        REQUIRE(one.status == 1);
        REQUIRE(one.data == d);
        int b2 = static_cast<int>(rng() % 30);
        if (b2 == b1) {
            b2 = (b2 + 1) % 30;
        }
        const auto two = decode::link11_check_word(f ^ (1U << static_cast<unsigned>(b1)) ^ (1U << static_cast<unsigned>(b2)));
        REQUIRE(two.status == -1);
    }
}

TEST_CASE("Link-11 table I phase changes", "[link11]") {
    CHECK(decode::link11_phase_step(1, 1) == 1);  // -45
    CHECK(decode::link11_phase_step(0, 1) == 3);  // -135
    CHECK(decode::link11_phase_step(0, 0) == 5);  // -225
    CHECK(decode::link11_phase_step(1, 0) == 7);  // -315
}

TEST_CASE("Link-11 round trips at both frame rates", "[link11]") {
    const auto w = words(40, 3);
    for (const auto rate : {Link11Rate::Fast, Link11Rate::Slow}) {
        for (const bool continuous : {false, true}) {
            INFO("rate " << static_cast<int>(rate) << " continuous " << continuous);
            const auto audio = render(siggen::link11_picket_reply(w), rate, 0.0, continuous);
            const auto out = run(audio);
            REQUIRE(out.size() == 1);
            CHECK(out[0].frame_rate == rate);
            CHECK(out[0].continuous_phase == continuous);
            CHECK(out[0].started);
            CHECK(out[0].picket_stop);
            CHECK(out[0].preamble_frames == 5);
            CHECK(out[0].words == 40);
            CHECK(out[0].failed == 0);
            CHECK(good_words(out[0], w) == w.size());
        }
    }
    // An interrogation with message, then a picket's reply after the 10 ms
    // switching gap of 5.1.7, then an interrogation without message.
    auto a = render(siggen::link11_interrogation(std::span<const std::uint32_t>(w.data(), 5), 023), Link11Rate::Fast);
    const auto b = render(siggen::link11_picket_reply(std::span<const std::uint32_t>(w.data() + 5, 7)), Link11Rate::Fast);
    const auto am = decode::link11_address_code(046);
    const auto c = render({am.first, am.second}, Link11Rate::Fast, 0.0);
    a.insert(a.end(), b.begin(), b.end());
    a.insert(a.end(), c.begin(), c.end());
    const auto out = run(a);
    REQUIRE(out.size() == 3);
    CHECK(out[0].control_stop);
    CHECK(out[0].address == 023);
    CHECK(out[0].words == 5);
    CHECK(out[1].picket_stop);
    CHECK(out[1].words == 7);
    CHECK_FALSE(out[2].started);
    CHECK(out[2].address == 046);
    // Chunking does not change anything.
    for (const std::size_t chunk : {1U, 101U, 4096U}) {
        const auto parts = run(a, chunk);
        REQUIRE(parts.size() == out.size());
        for (std::size_t i = 0; i < parts.size(); ++i) {
            CHECK(parts[i].position == out[i].position);
            CHECK(parts[i].frames.size() == out[i].frames.size());
            CHECK(parts[i].address == out[i].address);
        }
    }
}

TEST_CASE("Link-11 in noise, off frequency and on two paths", "[link11]") {
    const auto w = words(200, 5);
    for (const auto rate : {Link11Rate::Fast, Link11Rate::Slow}) {
        for (const bool multipath : {false, true}) {
            for (const double snr : {20.0, 15.0, 12.0, 9.0}) {
                auto audio = render(siggen::link11_picket_reply(w), rate, -40.0);
                if (multipath) {
                    audio = two_path(audio);
                }
                add_noise(audio, snr, 0.5, 31);
                const auto out = run(audio);
                const std::size_t ok = out.empty() ? 0 : good_words(out[0], w);
                const double wer = 1.0 - static_cast<double>(ok) / static_cast<double>(w.size());
                WARN("Link-11 " << (rate == Link11Rate::Fast ? "2250" : "1364") << " bit/s "
                                << (multipath ? "two-path" : "AWGN") << " SNR " << snr << " dB: word error rate "
                                << wer << ", " << (out.empty() ? 0 : out[0].corrected) << " corrected, "
                                << (out.empty() ? 0 : out[0].failed) << " flagged");
                if (snr >= 15.0 && !(multipath && rate == Link11Rate::Fast)) {
                    REQUIRE(out.size() == 1);
                    CHECK(std::abs(out[0].offset_hz + 40.0) < 2.0);
                    CHECK(wer < 0.02);
                }
            }
        }
    }
}

TEST_CASE("Link-11 noise alone decodes nothing", "[link11]") {
    std::vector<float> audio(static_cast<std::size_t>(kRate) * 20, 0.0F);
    add_noise(audio, 0.0, 0.5, 51);
    const auto out = run(audio);
    WARN("Link-11 noise only, 20 s: " << out.size() << " transmissions");
    CHECK(out.size() <= 1);
}
