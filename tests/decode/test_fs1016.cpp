// FS-1016 CELP: the tables and programs against what FED-STD-1016 prints, the
// framing and its Hamming code, and the encoder and decoder against each
// other on synthetic speech-like signals.
//
// Where the standard prints a result (the first and last 200 code book
// elements, two interpolation weights, the parity result table), the case
// regenerates it from the rule rather than holding the implementation's copy.
// Quality figures are measured and printed; the assertions around them are
// loose, as everywhere in this directory.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <set>
#include <vector>

#include "core/decode/fs1016.h"

using namespace revenant;

namespace {

// 3.3.1: the first 200 elements and the last 200 (elements 882 to 1081) as
// printed.
constexpr std::array<int, 200> kFirst200 = {
    0, 1,  0,  0,  0,  1,  0,  0,  0, 0,  0, 0,  1,  0, 1,  0, 0,  0,  0,  -1,
    0, 0,  -1, 0,  -1, 0,  1,  0,  0, 0,  0, 0,  0,  0, -1, 0, -1, -1, 0,  0,
    -1, 0, -1, 0,  -1, 0,  -1, 0,  0, 0,  0, 0,  0,  0, -1, 0, 0,  0,  1,  0,
    0, 0,  0,  -1, 0,  0,  0,  0,  1, 0,  0, 0,  0,  0, 0,  0, 0,  1,  -1, 0,
    1, 0,  1,  0,  -1, 1,  0,  0,  0, 0,  0, -1, 0,  0, 1,  0, 0,  0,  -1, 0,
    0, 0,  0,  0,  0,  0,  0,  0,  0, 0,  0, 0,  0,  0, 0,  0, 0,  0,  0,  0,
    0, -1, -1, 0,  0,  0,  0,  0,  0, 0,  0, 0,  0,  0, 0,  0, 0,  0,  -1, -1,
    0, 1,  -1, 0,  0,  -1, -1, 0,  0, 0,  0, 1,  0,  0, 0,  0, 0,  0,  0,  0,
    0, 0,  0,  -1, 0,  -1, -1, 0,  0, -1, 0, 1,  0,  0, 0,  1, 0,  0,  0,  0,
    0, -1, 0,  0,  0,  -1, 0,  0,  0, 0,  0, 0,  -1, 0, 1,  0, 0,  0,  0,  0};

constexpr std::array<int, 200> kLast200 = {
    0, 1, 0, 0,  0,  0,  0, 0,  1,  -1, 0, 0,  1,  0, 0,  0, 0, -1,
    0, 0, 0, 0,  0,  0,  0, 0,  0,  0,  -1, 0, 0,  0, 1,  0, 0, 1, 0, 0,
    0, 0, 1, 0,  0,  1,  0, 0,  -1, 0,  0, 0,  1,  0, 0,  0, 0, 0, 0, 0,
    1, 0, 0, -1, 0,  0,  -1, 0, 0,  0,  1, 0,  0,  0, 0,  0, 0, 0, 0, 0,
    0, 0, 0, 1,  0,  0,  0, 0,  0,  1,  0, 0,  -1, 1, -1, 0, 1, 0, 0, 0,
    0, 0, 0, 0,  1,  -1, 0, 0,  0,  0,  0, 0,  0,  0, -1, 1, 0, 0, 0, 0,
    0, 0, 0, 0,  0,  0,  0, 1,  0,  0,  0, 0,  0,  -1, 0, 0, 0, 0, -1, 0,
    0, 0, -1, 0, -1, 0,  0, 0,  0,  0,  0, -1, 0,  0, -1, 0, 0, 0, 0, 0,
    0, 0, 0, 1,  -1, 0,  0, 0,  0,  -1, 0, 1,  0,  0, 1,  0, 0, 0, 0, -1,
    0, 0, 0, 0,  1,  0,  0, 0,  0,  -1, 0, 0,  -1, 0, 0,  0, 0, 0, 0, 1,
    0, 0};

std::uint64_t lcg(std::uint64_t& state) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state >> 33U;
}

decode::Fs1016Params random_params(std::uint64_t& state) {
    decode::Fs1016Params p;
    for (std::size_t i = 0; i < 10; ++i) {
        p.lsp[i] = static_cast<std::uint8_t>(lcg(state) % decode::fs1016_lsp_table(i + 1).size());
    }
    for (std::size_t s = 0; s < 4; ++s) {
        p.pd[s] = static_cast<std::uint8_t>(lcg(state) % (s % 2 == 0 ? 256U : 64U));
        p.pg[s] = static_cast<std::uint8_t>(lcg(state) % 32U);
        p.ci[s] = static_cast<std::uint16_t>(lcg(state) % 512U);
        p.cg[s] = static_cast<std::uint8_t>(lcg(state) % 32U);
    }
    p.sp = 0;
    return p;
}

bool same(const decode::Fs1016Params& a, const decode::Fs1016Params& b) {
    return a.lsp == b.lsp && a.pd == b.pd && a.pg == b.pg && a.ci == b.ci && a.cg == b.cg &&
           a.sp == b.sp;
}

// A voiced test signal: an impulse train with a slow pitch glide through
// three formant resonators, and an unvoiced one: noise through two.
std::vector<float> resonate(const std::vector<double>& source,
                            const std::vector<std::pair<double, double>>& formants) {
    std::vector<double> x = source;
    for (const auto& [f, bw] : formants) {
        const double r = std::exp(-std::numbers::pi * bw / 8000.0);
        const double c = 2.0 * r * std::cos(2.0 * std::numbers::pi * f / 8000.0);
        double y1 = 0;
        double y2 = 0;
        for (double& v : x) {
            const double y = v + c * y1 - r * r * y2;
            y2 = y1;
            y1 = y;
            v = y;
        }
    }
    double peak = 0;
    for (double v : x) {
        peak = std::max(peak, std::fabs(v));
    }
    std::vector<float> out(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        out[i] = static_cast<float>(0.4 * x[i] / peak);
    }
    return out;
}

std::vector<float> voiced(std::size_t n) {
    std::vector<double> src(n, 0.0);
    double phase = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double f0 = 110.0 + 30.0 * static_cast<double>(i) / static_cast<double>(n);
        phase += f0 / 8000.0;
        if (phase >= 1.0) {
            phase -= 1.0;
            src[i] = 1.0;
        }
    }
    return resonate(src, {{600, 80}, {1300, 100}, {2500, 150}});
}

std::vector<float> unvoiced(std::size_t n) {
    std::vector<double> src(n);
    std::uint64_t state = 0xF5'1016ULL;
    for (double& v : src) {
        v = static_cast<double>(lcg(state) % 20001U) / 10000.0 - 1.0;
    }
    return resonate(src, {{2500, 600}, {3400, 500}});
}

struct Quality {
    double seg_snr_db;
    double energy_ratio_db;
    double ms_per_frame;
};

// Encoder runs one frame late, so decoded frame k matches input frame k - 1.
Quality code(const std::vector<float>& in) {
    decode::Fs1016Encoder enc;
    decode::Fs1016Decoder dec;
    std::vector<float> out;
    const std::size_t frames = in.size() / decode::kFs1016FrameSamples;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t f = 0; f <= frames; ++f) {
        std::array<float, decode::kFs1016FrameSamples> buf{};
        if (f < frames) {
            std::copy_n(in.begin() + static_cast<std::ptrdiff_t>(f * buf.size()), buf.size(), buf.begin());
        }
        const auto bits = enc.encode(buf);
        const auto pcm = dec.decode(bits);
        out.insert(out.end(), pcm.begin(), pcm.end());
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    double seg = 0;
    std::size_t segs = 0;
    double ein = 0;
    double eout = 0;
    // Skip the first two frames while the filters fill.
    for (std::size_t f = 2; f < frames; ++f) {
        double s = 0;
        double e = 0;
        for (std::size_t i = 0; i < decode::kFs1016FrameSamples; ++i) {
            const double x = in[f * 240 + i];
            const double y = out[(f + 1) * 240 + i];
            s += x * x;
            e += (x - y) * (x - y);
            ein += x * x;
            eout += y * y;
        }
        seg += 10.0 * std::log10((s + 1e-9) / (e + 1e-9));
        ++segs;
    }
    return {seg / static_cast<double>(segs), 10.0 * std::log10(eout / ein),
            ms / static_cast<double>(frames + 1)};
}

}  // namespace

TEST_CASE("FS-1016 code book matches the elements 3.3.1 prints", "[decode][fs1016]") {
    const auto& book = decode::fs1016_stochastic_book();
    for (std::size_t i = 0; i < 200; ++i) {
        INFO("element " << i);
        CHECK(book[i] == kFirst200[i]);
    }
    for (std::size_t i = 0; i < 200; ++i) {
        INFO("element " << 882 + i);
        CHECK(book[882 + i] == kLast200[i]);
    }
}

TEST_CASE("FS-1016 adaptive code table is a permutation in delay order", "[decode][fs1016]") {
    std::set<std::uint8_t> seen;
    for (std::size_t i = 0; i < decode::kFs1016AdaptiveCodes; ++i) {
        seen.insert(decode::fs1016_adaptive_hex(i));
        if (i > 0) {
            CHECK(decode::fs1016_adaptive_delay(i) > decode::fs1016_adaptive_delay(i - 1));
        }
    }
    CHECK(seen.size() == 256);
    // 3.5.4 spot values.
    CHECK(decode::fs1016_adaptive_hex(0) == 0x42);
    CHECK(std::fabs(decode::fs1016_adaptive_delay(255) - 147.0) < 1e-9);
    CHECK(decode::fs1016_adaptive_hex(255) == 0xAF);
}

TEST_CASE("FS-1016 interpolation weights match 3.5.3's worked example", "[decode][fs1016]") {
    // Code 66.67: weight 0 is .41245, weight -20 is -.0011766, -19 is
    // .0014386, 19 is -.0011302.
    CHECK(std::fabs(decode::fs1016_interp_weight(0, 0.66666667) - 0.41245) < 5e-5);
    CHECK(std::fabs(decode::fs1016_interp_weight(-20, 0.66666667) - -0.0011766) < 5e-7);
    CHECK(std::fabs(decode::fs1016_interp_weight(-19, 0.66666667) - 0.0014386) < 5e-7);
    CHECK(std::fabs(decode::fs1016_interp_weight(19, 0.66666667) - -0.0011302) < 5e-7);
}

TEST_CASE("FS-1016 Hamming code corrects every single error", "[decode][fs1016]") {
    std::uint64_t state = 1016;
    const auto p = random_params(state);
    const auto clean = decode::fs1016_pack(p, false);
    // The 15 bits of the code: the 11 protected bits and HP-0..3, found by
    // which flips change the syndrome.
    std::size_t covered = 0;
    for (std::size_t bit = 0; bit < decode::kFs1016FrameBits; ++bit) {
        auto bits = clean;
        bits[bit] ^= 1U;
        const auto u = decode::fs1016_unpack(bits);
        if (u.syndrome != 0) {
            ++covered;
            INFO("bit " << bit + 1);
            CHECK(same(u.params, p));
        }
    }
    CHECK(covered == 15);
    const auto u = decode::fs1016_unpack(clean);
    CHECK(u.syndrome == 0);
    CHECK_FALSE(u.corrected);
}

TEST_CASE("FS-1016 frame packing round-trips", "[decode][fs1016]") {
    std::uint64_t state = 77;
    for (int i = 0; i < 500; ++i) {
        const auto p = random_params(state);
        const bool sync = (i & 1) != 0;
        const auto u = decode::fs1016_unpack(decode::fs1016_pack(p, sync));
        REQUIRE(same(u.params, p));
        CHECK(u.sync == sync);
        CHECK(u.syndrome == 0);
    }
}

TEST_CASE("FS-1016 LSP conversion round-trips", "[decode][fs1016]") {
    const std::array<double, 10> lsp = {300, 550, 900, 1300, 1700, 2100, 2500, 2900, 3250, 3600};
    const auto back = decode::fs1016_lpc_to_lsp(decode::fs1016_lsp_to_lpc(lsp));
    for (std::size_t i = 0; i < 10; ++i) {
        CHECK(std::fabs(back[i] - lsp[i]) < 0.5);
    }
}

TEST_CASE("FS-1016 encoder and decoder on synthetic speech", "[decode][fs1016]") {
    const std::size_t n = 240 * 50;
    const Quality v = code(voiced(n));
    const Quality u = code(unvoiced(n));
    std::printf("FS-1016 voiced: segSNR %.2f dB, energy %.2f dB, %.2f ms/frame\n", v.seg_snr_db,
                v.energy_ratio_db, v.ms_per_frame);
    std::printf("FS-1016 unvoiced: segSNR %.2f dB, energy %.2f dB, %.2f ms/frame\n",
                u.seg_snr_db, u.energy_ratio_db, u.ms_per_frame);
    CHECK(v.seg_snr_db > 2.0);
    CHECK(std::fabs(v.energy_ratio_db) < 6.0);
    CHECK(std::fabs(u.energy_ratio_db) < 6.0);
}

TEST_CASE("FS-1016 decoder reports sync alternation", "[decode][fs1016]") {
    decode::Fs1016Encoder enc;
    decode::Fs1016Decoder dec;
    std::array<float, 240> silence{};
    for (int i = 0; i < 4; ++i) {
        (void)dec.decode(enc.encode(silence));
        CHECK(dec.last_frame().sync == ((i & 1) != 0));
        CHECK(dec.last_frame().sync_as_expected);
    }
    auto bits = enc.encode(silence);
    bits[143] ^= 1U;
    (void)dec.decode(bits);
    CHECK_FALSE(dec.last_frame().sync_as_expected);
}
