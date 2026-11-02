// LPC-10 (FED-STD-1015): the transcribed tables against each other and
// against the constraints the document states, the framing both ways, the
// error control code, frame synchronisation, and the encoder and decoder on
// synthetic voiced and unvoiced signals.
//
// FED-STD-1015 prints no bit-stream test vectors, so there is nothing to hold
// a frame against byte for byte. What it does print twice is enough to check
// a transcription: every table that has both a coding and a decoding column
// must agree with itself, and the two pitch tables must be inverses.
//
// A vocoder does not reproduce the waveform, so the quality figures are
// spectral distortion and level tracking rather than waveform SNR. They are
// printed and the assertions around them are loose, as everywhere in this
// directory.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "core/decode/lpc10.h"

using namespace revenant;
using namespace revenant::decode;

namespace {

std::uint64_t lcg(std::uint64_t& s) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return s >> 33U;
}

// A glottal-ish pulse train with a slow pitch glide through three formant
// resonators, about -12 dBFS.
std::vector<float> voiced_signal(std::size_t n, double f0_start, double f0_end) {
    std::vector<double> src(n, 0.0);
    double phase = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double f0 = f0_start + (f0_end - f0_start) * static_cast<double>(i) / static_cast<double>(n);
        phase += f0 / 8000.0;
        if (phase >= 1.0) {
            phase -= 1.0;
            src[i] = 1.0;
        }
    }
    std::vector<double> y = src;
    for (const auto& [f, bw] : std::array<std::pair<double, double>, 3>{{{600, 80}, {1400, 120}, {2600, 200}}}) {
        const double r = std::exp(-std::numbers::pi * bw / 8000.0);
        const double c = 2.0 * r * std::cos(2.0 * std::numbers::pi * f / 8000.0);
        double y1 = 0.0;
        double y2 = 0.0;
        for (double& v : y) {
            const double out = v + c * y1 - r * r * y2;
            y2 = y1;
            y1 = out;
            v = out;
        }
    }
    double peak = 0.0;
    for (double v : y) peak = std::max(peak, std::abs(v));
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<float>(0.25 * y[i] / peak);
    return out;
}

// White noise through one broad resonance at 3 kHz.
std::vector<float> unvoiced_signal(std::size_t n) {
    std::uint64_t s = 99;
    const double r = std::exp(-std::numbers::pi * 600.0 / 8000.0);
    const double c = 2.0 * r * std::cos(2.0 * std::numbers::pi * 3000.0 / 8000.0);
    double y1 = 0.0;
    double y2 = 0.0;
    std::vector<float> out(n);
    for (auto& v : out) {
        const double x = (static_cast<double>(lcg(s) & 0xFFFF) / 65536.0 - 0.5);
        const double y = x + c * y1 - r * r * y2;
        y2 = y1;
        y1 = y;
        v = static_cast<float>(0.08 * y);
    }
    return out;
}

// LPC log spectrum from reflection coefficients, 64 points 0..4 kHz.
std::vector<double> lpc_log_spectrum(const std::array<double, kLpc10Order>& k) {
    std::array<double, kLpc10Order> a{};
    for (std::size_t i = 0; i < kLpc10Order; ++i) {
        const auto prev = a;
        a[i] = k[i];
        for (std::size_t j = 0; j < i; ++j) a[j] = prev[j] - k[i] * prev[i - 1 - j];
    }
    std::vector<double> out(64);
    for (std::size_t b = 0; b < out.size(); ++b) {
        const double w = std::numbers::pi * (static_cast<double>(b) + 0.5) / 64.0;
        double re = 1.0;
        double im = 0.0;
        for (std::size_t j = 0; j < kLpc10Order; ++j) {
            re -= a[j] * std::cos(w * static_cast<double>(j + 1));
            im += a[j] * std::sin(w * static_cast<double>(j + 1));
        }
        out[b] = -10.0 * std::log10(re * re + im * im);
    }
    return out;
}

double spectral_distortion(const std::array<double, kLpc10Order>& k1,
                           const std::array<double, kLpc10Order>& k2) {
    const auto s1 = lpc_log_spectrum(k1);
    const auto s2 = lpc_log_spectrum(k2);
    double acc = 0.0;
    for (std::size_t i = 0; i < s1.size(); ++i) acc += (s1[i] - s2[i]) * (s1[i] - s2[i]);
    return std::sqrt(acc / static_cast<double>(s1.size()));
}

std::array<double, kLpc10Order> dequantize(const Lpc10Frame& f) {
    std::array<double, kLpc10Order> k{};
    const int order = f.voicing == Lpc10Voicing::Voiced ? 10 : 4;
    for (int i = 1; i <= order; ++i) {
        k[static_cast<std::size_t>(i - 1)] = lpc10_rc_decode(i, f.rc[static_cast<std::size_t>(i - 1)]);
    }
    return k;
}

double rms_db(std::span<const float> x) {
    double e = 0.0;
    for (float v : x) e += static_cast<double>(v) * v;
    return 10.0 * std::log10(e / static_cast<double>(x.size()) + 1e-12);
}

}  // namespace

TEST_CASE("LPC-10 pitch tables of 3.4.1 and 3.4.2 are inverses", "[decode][lpc10]") {
    const auto enc = lpc10_pitch_encode_table();
    const auto dec = lpc10_pitch_decode_table();
    REQUIRE(enc.size() == 60);
    REQUIRE(dec.size() == 128);
    int voiced = 0;
    for (std::size_t code = 0; code < dec.size(); ++code) {
        const int v = dec[code];
        const int weight = std::popcount(static_cast<unsigned>(code));
        if (v > 0) {
            ++voiced;
            const auto it = std::ranges::find(enc, v, &Lpc10PitchCode::period);
            REQUIRE(it != enc.end());
            CHECK(it->code == code);
        }
        // The (U) codes are 0000000 and its single-bit errors, the (T) codes
        // 1111111 and its single-bit errors: 3.4.1's "for error protection".
        CHECK((v == kLpc10PitchU) == (weight <= 1));
        CHECK((v == kLpc10PitchT) == (weight >= 6));
    }
    CHECK(voiced == 60);
    // 3.4.1: 51..400 Hz, i.e. periods 20..156, strictly descending.
    CHECK(enc.front().period == 156);
    CHECK(enc.back().period == 20);
    for (std::size_t i = 1; i < enc.size(); ++i) CHECK(enc[i].period < enc[i - 1].period);
}

TEST_CASE("LPC-10 RMS and reflection coefficient decode values lie in their own coding ranges",
          "[decode][lpc10]") {
    const auto from = lpc10_rms_from_table();
    for (int c = 0; c < 32; ++c) {
        const int hi = c == 31 ? 511 : from[static_cast<std::size_t>(c + 1)] - 1;
        INFO("RMS code " << c);
        CHECK(lpc10_rms_decode(c) >= from[static_cast<std::size_t>(c)]);
        CHECK(lpc10_rms_decode(c) <= hi);
        CHECK(lpc10_rms_encode(lpc10_rms_decode(c)) == c);
    }
    for (int k = 1; k <= 10; ++k) {
        const int lo = lpc10_rc_min_index(k);
        const int hi = lpc10_rc_max_index(k);
        CHECK(hi - lo + 1 == (k <= 2 ? 31 : (1 << lpc10_rc_bits(k))));
        for (int i = lo; i <= hi; ++i) {
            INFO("RC" << k << " index " << i);
            const double d = lpc10_rc_decode(k, i);
            CHECK(d >= lpc10_rc_from(k, i) - 1e-9);
            if (i < hi) {
                CHECK(lpc10_rc_from(k, i + 1) > lpc10_rc_from(k, i));
                CHECK(d < lpc10_rc_from(k, i + 1));
            }
            CHECK(lpc10_rc_encode(k, d) == i);
        }
    }
}

TEST_CASE("LPC-10 error control code of 3.7 corrects one error and detects two",
          "[decode][lpc10]") {
    // Linear, minimum distance 4.
    for (std::uint8_t a = 0; a < 16; ++a) {
        for (std::uint8_t b = 0; b < 16; ++b) {
            CHECK(lpc10_ec_encode(static_cast<std::uint8_t>(a ^ b)) ==
                  (lpc10_ec_encode(a) ^ lpc10_ec_encode(b)));
            if (a != b) {
                const int d = std::popcount(static_cast<unsigned>(a ^ b)) +
                              std::popcount(static_cast<unsigned>(lpc10_ec_encode(a) ^ lpc10_ec_encode(b)));
                CHECK(d >= 4);
            }
        }
    }

    // Every single error in every protected bit of an unvoiced frame.
    Lpc10Frame f;
    f.voicing = Lpc10Voicing::Unvoiced;
    f.rms_code = 22;
    f.rc = {9, -7, 3, -12, 0, 0, 0, 0, 0, 0};
    std::array<std::uint8_t, kLpc10FrameBits> bits{};
    REQUIRE(lpc10_pack(f, 1, bits));
    int corrected = 0;
    for (std::size_t i = 0; i < kLpc10FrameBits - 1; ++i) {
        auto b = bits;
        b[i] ^= 1U;
        const auto u = lpc10_unpack(b);
        REQUIRE(u);
        if (u->ec_corrected == 1) {
            ++corrected;
            CHECK(u->frame.rms_code == f.rms_code);
            for (int k = 0; k < 4; ++k) CHECK(u->frame.rc[static_cast<std::size_t>(k)] == f.rc[static_cast<std::size_t>(k)]);
        }
    }
    // 4 MSBs + 4 check bits for each of 5 fields; the pitch, the data LSBs,
    // sync and the unused bit are not protected.
    CHECK(corrected == 40);

    auto two = bits;
    two[4] ^= 1U;   // R-0 is the LSB, unprotected
    two[9] ^= 1U;   // R-1
    two[13] ^= 1U;  // R-2
    const auto u = lpc10_unpack(two);
    REQUIRE(u);
    CHECK(u->ec_uncorrectable == 1);
}

TEST_CASE("LPC-10 frames survive pack and unpack", "[decode][lpc10]") {
    std::uint64_t s = 7;
    const auto pitches = lpc10_pitch_encode_table();
    for (int trial = 0; trial < 2000; ++trial) {
        Lpc10Frame f;
        const auto v = lcg(s) % 3;
        f.voicing = v == 0 ? Lpc10Voicing::Unvoiced : v == 1 ? Lpc10Voicing::Transition : Lpc10Voicing::Voiced;
        if (f.voicing == Lpc10Voicing::Voiced) f.pitch_period = pitches[lcg(s) % pitches.size()].period;
        f.rms_code = static_cast<int>(lcg(s) % 32);
        const int order = f.voicing == Lpc10Voicing::Voiced ? 10 : 4;
        for (int k = 1; k <= order; ++k) {
            const int span = lpc10_rc_max_index(k) - lpc10_rc_min_index(k) + 1;
            f.rc[static_cast<std::size_t>(k - 1)] =
                lpc10_rc_min_index(k) + static_cast<int>(lcg(s) % static_cast<std::uint64_t>(span));
        }
        const std::uint8_t sync = static_cast<std::uint8_t>(trial & 1);
        std::array<std::uint8_t, kLpc10FrameBits> bits{};
        REQUIRE(lpc10_pack(f, sync, bits));
        CHECK(bits[53] == sync);
        const auto u = lpc10_unpack(bits);
        REQUIRE(u);
        CHECK(u->sync == sync);
        CHECK(u->frame.voicing == f.voicing);
        CHECK(u->frame.pitch_period == f.pitch_period);
        CHECK(u->frame.rms_code == f.rms_code);
        CHECK(u->frame.rc == f.rc);
        CHECK(u->ec_corrected == 0);
        CHECK(u->ec_uncorrectable == 0);
    }
}

TEST_CASE("LPC-10 frame sync locks on the alternating bit and follows it", "[decode][lpc10]") {
    Lpc10Encoder enc;
    const auto speech = voiced_signal(kLpc10FrameSamples * 80, 110.0, 140.0);
    std::vector<std::array<std::uint8_t, kLpc10FrameBits>> sent;
    std::vector<std::uint8_t> stream;
    std::uint64_t s = 3;
    for (int i = 0; i < 23; ++i) stream.push_back(static_cast<std::uint8_t>(lcg(s) & 1U));
    for (std::size_t f = 0; f < 80; ++f) {
        std::array<std::uint8_t, kLpc10FrameBits> bits{};
        REQUIRE(enc.encode(std::span(speech).subspan(f * kLpc10FrameSamples, kLpc10FrameSamples), bits));
        sent.push_back(bits);
        stream.insert(stream.end(), bits.begin(), bits.end());
    }
    // A sprinkling of bit errors, about 1 in 1000.
    int flipped = 0;
    for (auto& b : stream) {
        if (lcg(s) % 1000 == 0) {
            b ^= 1U;
            ++flipped;
        }
    }

    Lpc10FrameSync sync;
    std::vector<std::array<std::uint8_t, kLpc10FrameBits>> got;
    sync.push(stream, got);
    INFO("flipped " << flipped << " bits, recovered " << got.size() << " frames");
    REQUIRE(sync.locked());
    REQUIRE(got.size() >= 60);
    // Every recovered frame is a sent frame in order, at the right alignment.
    const std::size_t first = 80 - got.size();
    std::size_t exact = 0;
    for (std::size_t i = 0; i < got.size(); ++i) {
        int diff = 0;
        for (std::size_t b = 0; b < kLpc10FrameBits; ++b) diff += got[i][b] != sent[first + i][b];
        CHECK(diff <= 2);
        exact += diff == 0;
    }
    CHECK(exact + static_cast<std::size_t>(flipped) >= got.size());
}

TEST_CASE("LPC-10 encoder and decoder on synthetic voiced and unvoiced speech",
          "[decode][lpc10]") {
    constexpr std::size_t kFrames = 120;
    const auto voiced = voiced_signal(kLpc10FrameSamples * kFrames, 100.0, 160.0);
    const auto noise = unvoiced_signal(kLpc10FrameSamples * kFrames);

    for (int which = 0; which < 2; ++which) {
        const auto& in = which == 0 ? voiced : noise;
        Lpc10Encoder enc;
        Lpc10Decoder dec;
        std::vector<float> out(in.size());
        int voiced_frames = 0;
        int pitch_ok = 0;
        double sd_quant = 0.0;
        double level_err = 0.0;
        double sd_output = 0.0;
        int measured = 0;
        Lpc10Encoder reanalysis;
        for (std::size_t f = 0; f < kFrames; ++f) {
            const auto frame_in = std::span(in).subspan(f * kLpc10FrameSamples, kLpc10FrameSamples);
            const auto params = enc.analyse(frame_in);
            REQUIRE(params);
            const auto quantization_sd = spectral_distortion(enc.last_rc(), dequantize(*params));
            std::array<std::uint8_t, kLpc10FrameBits> bits{};
            REQUIRE(lpc10_pack(*params, static_cast<std::uint8_t>(f & 1U), bits));
            auto frame_out = std::span(out).subspan(f * kLpc10FrameSamples, kLpc10FrameSamples);
            REQUIRE(dec.decode(bits, frame_out));
            for (float v : frame_out) REQUIRE(std::isfinite(v));
            REQUIRE(reanalysis.analyse(frame_out));

            if (f < 4) continue;  // windows filling
            ++measured;
            if (params->voicing == Lpc10Voicing::Voiced) {
                ++voiced_frames;
                const double t = (static_cast<double>(f) + 0.5) / static_cast<double>(kFrames);
                const double true_period = 8000.0 / (100.0 + 60.0 * t);
                if (std::abs(params->pitch_period - true_period) <= 0.06 * true_period) ++pitch_ok;
            }
            sd_quant += quantization_sd;
            level_err += rms_db(frame_out) - rms_db(frame_in);
            sd_output += spectral_distortion(enc.last_rc(), reanalysis.last_rc());
        }
        sd_quant /= measured;
        level_err /= measured;
        sd_output /= measured;
        const char* name = which == 0 ? "voiced" : "unvoiced";
        WARN("LPC-10 " << name << ": voiced frames " << voiced_frames << "/" << measured
                       << ", pitch within 6% " << pitch_ok << ", quantization SD " << sd_quant
                       << " dB, output level error " << level_err
                       << " dB, re-analysed output SD " << sd_output << " dB");
        // Unvoiced frames carry only RC(1)..RC(4) (3.3.2), so their figure
        // includes the truncation from 10th to 4th order as well.
        CHECK(sd_quant < (which == 0 ? 2.0 : 4.5));
        CHECK(std::abs(level_err) < 6.0);
        CHECK(sd_output < 9.0);
        if (which == 0) {
            CHECK(voiced_frames >= measured * 8 / 10);
            CHECK(pitch_ok >= voiced_frames * 8 / 10);
        } else {
            CHECK(voiced_frames <= measured / 10);
        }
    }
}

TEST_CASE("LPC-10 decoder repeats the last frame on an invalid pitch code", "[decode][lpc10]") {
    Lpc10Decoder dec;
    Lpc10Frame f;
    f.voicing = Lpc10Voicing::Voiced;
    f.pitch_period = 80;
    f.rms_code = 20;
    std::array<std::uint8_t, kLpc10FrameBits> bits{};
    REQUIRE(lpc10_pack(f, 0, bits));
    std::array<float, kLpc10FrameSamples> out{};
    REQUIRE(dec.decode(bits, out));
    CHECK_FALSE(dec.last_frame().repeated);
    CHECK(dec.last_frame().pitch_period == 80);

    // Code 3 is (I) in 3.4.2. Pitch bits sit at frame bits 4, 9, 15, 27, 31,
    // 42 and 47 (3.3.3); set P-0 and P-1 only.
    std::array<std::uint8_t, kLpc10FrameBits> bad{};
    bad[3] = 1;
    bad[8] = 1;
    REQUIRE(dec.decode(bad, out));
    CHECK(dec.last_frame().repeated);
    CHECK(dec.last_frame().pitch_period == 80);

    std::array<std::uint8_t, 10> short_frame{};
    CHECK_FALSE(dec.decode(short_frame, out));
}
