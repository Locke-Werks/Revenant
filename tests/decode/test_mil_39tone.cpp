// MIL-STD-188-110 39-tone parallel mode: the code and tables against
// MIL-STD-188-110B Appendix B, and the decoder against the transmitter
// through noise, a two-path channel, a mistuned receiver and arbitrary
// chunking.
//
// Error rates are measured and reported in WARN lines with loose assertions,
// for the reason tests/decode/CMakeLists.txt gives.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include "core/decode/mil_39tone.h"
#include "core/dsp/synth/mil_39tone_mod.h"

using namespace revenant;
using decode::Tone39Mode;

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

std::vector<std::uint8_t> random_bits(std::size_t n, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<std::uint8_t> b(n);
    for (auto& v : b) {
        v = static_cast<std::uint8_t>(rng() & 1U);
    }
    return b;
}

std::vector<float> render(const Tone39Mode& mode, const std::vector<std::uint8_t>& data, double offset = 0.0,
                          bool extended = false) {
    siggen::Tone39ModConfig mc;
    mc.rate = kRate;
    mc.mode = mode;
    mc.offset_hz = offset;
    mc.extended_preamble = extended;
    mc.lead_in_samples = 3000.0;
    mc.tail_samples = 4000.0;
    auto a = siggen::tone39_render(mc, data);
    REQUIRE(a.has_value());
    return *a;
}

std::vector<decode::Tone39Transmission> run(const std::vector<float>& audio, decode::Tone39Config cfg = {},
                                            std::size_t chunk = 0) {
    auto dec = decode::Tone39Decoder::create(cfg);
    REQUIRE(dec.has_value());
    std::vector<decode::Tone39Transmission> out;
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

// Bit errors over the data sent, counting missing bits as errors.
std::size_t errors(const decode::Tone39Transmission& t, const std::vector<std::uint8_t>& sent) {
    std::size_t e = 0;
    for (std::size_t i = 0; i < sent.size(); ++i) {
        if (i >= t.data.size() || t.data[i] != sent[i]) {
            ++e;
        }
    }
    return e;
}

}  // namespace

TEST_CASE("39-tone framing sequence ends nine MARKs then a SPACE", "[tone39]") {
    // B.5.4.3: the final bit is the first SPACE after nine contiguous MARKs.
    for (const int len : {252, 256, 264, 512}) {
        const auto s = decode::tone39_framing_sequence(len);
        REQUIRE(s.size() == static_cast<std::size_t>(len));
        CHECK(s.back() == 0);
        for (int i = 2; i <= 10; ++i) {
            CHECK(s[s.size() - static_cast<std::size_t>(i)] == 1);
        }
        CHECK(s[s.size() - 11] == 0);
    }
    // Maximal length: the 511-bit period holds 256 MARKs.
    const auto p = decode::tone39_framing_sequence(511);
    CHECK(std::count(p.begin(), p.end(), 1) == 256);
    // A shorter sequence is the tail of a longer one.
    const auto a = decode::tone39_framing_sequence(252);
    CHECK(std::equal(a.begin(), a.end(), p.end() - 252));
}

TEST_CASE("39-tone table B-III intervals are whole super blocks", "[tone39]") {
    for (const auto& f : decode::tone39_framing_table()) {
        const Tone39Mode m{f.rate, f.degree};
        CHECK(f.interval_bits == f.superblocks * decode::tone39_superblock_bits(m));
    }
    CHECK(decode::tone39_framing_table().size() == 28);
}

TEST_CASE("39-tone Reed-Solomon generator and correction", "[tone39]") {
    // B.5.2: g(x) = x^4 + a^13 x^3 + a^6 x^2 + a^3 x + a^10, a = 0010 in
    // GF(16) mod x^4 + x + 1: a^3 = 1000, a^6 = 1100, a^10 = 0111, a^13 = 1101.
    const auto g = decode::tone39_rs_generator();
    CHECK(g[0] == 1);
    CHECK(g[1] == 0b1101);
    CHECK(g[2] == 0b1100);
    CHECK(g[3] == 0b1000);
    CHECK(g[4] == 0b0111);
    std::mt19937 rng(3);
    for (const int k : {3, 10}) {
        for (int trial = 0; trial < 500; ++trial) {
            std::vector<std::uint8_t> d(static_cast<std::size_t>(k));
            for (auto& v : d) {
                v = static_cast<std::uint8_t>(rng() & 15U);
            }
            auto w = decode::tone39_rs_encode(d);
            const auto clean = w;
            const int ne = trial % 3;
            for (int e = 0; e < ne; ++e) {
                w[(static_cast<std::size_t>(rng()) % w.size())] ^= static_cast<std::uint8_t>(1 + rng() % 15);
            }
            const int r = decode::tone39_rs_decode(w);
            REQUIRE(r >= 0);
            REQUIRE(w == clean);
        }
    }
}

TEST_CASE("39-tone interleaver follows figure B-2", "[tone39]") {
    // Degree 4 at 75 bit/s: (7,3) words. Mark one data bit at a time and
    // find where it leaves the interleaver. Figure B-2: D0..D7 are the first
    // eight bits out (row 0 of codewords 1 and 2), D24..D31 the next eight
    // (row 0 of codewords 3 and 4), then D8.
    const Tone39Mode m{75, 4};
    const auto where = [&](int bit) {
        std::vector<std::uint8_t> d(static_cast<std::size_t>(decode::tone39_superblock_data_bits(m)), 0);
        d[static_cast<std::size_t>(bit)] = 1;
        const auto out = decode::tone39_encode_superblock(m, d);
        // The first data row is systematic: the bit's own position is the
        // first 1 in the output.
        return static_cast<int>(std::find(out.begin(), out.end(), 1) - out.begin());
    };
    CHECK(where(0) == 0);
    CHECK(where(7) == 7);
    CHECK(where(24) == 8);
    CHECK(where(31) == 15);
    CHECK(where(8) == 16);
    CHECK(where(32) == 24);
    // Round trip with errors spread by the interleaver.
    const auto data = random_bits(static_cast<std::size_t>(decode::tone39_superblock_data_bits(m)), 9);
    const auto coded = decode::tone39_encode_superblock(m, data);
    std::vector<float> soft(coded.size());
    for (std::size_t i = 0; i < coded.size(); ++i) {
        soft[i] = coded[i] != 0 ? 1.0F : -1.0F;
    }
    // A burst of 16 bits hits each of the four codewords in one symbol.
    for (std::size_t i = 40; i < 56; ++i) {
        soft[i] = -soft[i];
    }
    const auto dec = decode::tone39_decode_superblock(m, soft);
    CHECK(dec.failed == 0);
    CHECK(dec.data == data);
}

TEST_CASE("39-tone table B-V phase changes", "[tone39]") {
    CHECK(decode::tone39_phase_step(0, 1) == 1);  // later MARK, earlier SPACE: +45
    CHECK(decode::tone39_phase_step(0, 0) == 3);  // +135
    CHECK(decode::tone39_phase_step(1, 0) == 5);  // +225
    CHECK(decode::tone39_phase_step(1, 1) == 7);  // +315
}

TEST_CASE("39-tone round trip at every rate", "[tone39]") {
    struct Case {
        Tone39Mode mode;
        bool extended;
    };
    const Case cases[] = {
        {{2400, 9}, false},
        {{1200, 1}, false},
        {{600, 33}, true},
        {{300, 1, decode::Tone39Diversity::Frequency}, false},
        {{150, 9}, false},
        {{75, 4}, false},
    };
    for (const auto& c : cases) {
        const auto data = random_bits(static_cast<std::size_t>(decode::tone39_superblock_data_bits(c.mode)) * 2, 11);
        const auto audio = render(c.mode, data, 0.0, c.extended);
        const auto out = run(audio);
        REQUIRE(out.size() == 1);
        INFO("rate " << c.mode.rate << " degree " << c.mode.degree);
        CHECK(out[0].mode.rate == c.mode.rate);
        CHECK(out[0].mode.degree == c.mode.degree);
        CHECK(out[0].mode.diversity == c.mode.diversity);
        CHECK(out[0].extended_preamble == c.extended);
        CHECK(out[0].failed == 0);
        CHECK(errors(out[0], data) == 0);
    }
}

TEST_CASE("39-tone survives offset, chunking, noise and two paths", "[tone39]") {
    const Tone39Mode mode{2400, 1};
    const auto data = random_bits(8000, 5);
    const auto clean = render(mode, data, 37.5);
    const auto whole = run(clean);
    REQUIRE(whole.size() == 1);
    CHECK(std::abs(whole[0].offset_hz - 37.5) < 1.0);
    CHECK(errors(whole[0], data) == 0);
    for (const std::size_t chunk : {1U, 97U, 4096U}) {
        const auto parts = run(clean, {}, chunk);
        REQUIRE(parts.size() == 1);
        CHECK(parts[0].data == whole[0].data);
        CHECK(parts[0].position == whole[0].position);
    }
    for (const bool multipath : {false, true}) {
        for (const double snr : {20.0, 15.0, 12.0, 9.0}) {
            auto audio = render(mode, data, -20.0);
            if (multipath) {
                audio = two_path(audio);
            }
            add_noise(audio, snr, 0.5, 17);
            const auto out = run(audio);
            REQUIRE(out.size() == 1);
            const double ber = static_cast<double>(errors(out[0], data)) / static_cast<double>(data.size());
            WARN("39-tone 2400 bit/s " << (multipath ? "two-path" : "AWGN") << " SNR " << snr
                                       << " dB: BER " << ber << ", " << out[0].failed << " of "
                                       << out[0].codewords << " codewords failed");
            if (snr >= 15.0) {
                CHECK(ber < 1e-2);
            }
        }
    }
    // 75 bit/s with time diversity holds up far lower.
    const Tone39Mode slow{75, 1};
    const auto little = random_bits(300, 6);
    for (const double snr : {6.0, 3.0, 0.0}) {
        auto audio = render(slow, little, 10.0);
        add_noise(audio, snr, 0.5, 21);
        const auto out = run(audio);
        REQUIRE(out.size() == 1);
        const double ber = static_cast<double>(errors(out[0], little)) / static_cast<double>(little.size());
        WARN("39-tone 75 bit/s AWGN SNR " << snr << " dB: BER " << ber);
        if (snr >= 3.0) {
            CHECK(ber < 1e-2);
        }
    }
}

TEST_CASE("39-tone noise alone decodes nothing", "[tone39]") {
    std::vector<float> audio(static_cast<std::size_t>(kRate) * 30, 0.0F);
    add_noise(audio, 0.0, 0.5, 99);
    const auto out = run(audio);
    WARN("39-tone noise only, 30 s: " << out.size() << " transmissions");
    CHECK(out.size() <= 1);
}
