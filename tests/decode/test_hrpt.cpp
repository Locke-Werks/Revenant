// NOAA HRPT: the frame sync against KLM Table 4.1.3.1-1's printed words, and
// the decoder against core/dsp/synth/hrpt_mod.h with carrier offset, drift,
// clock error and noise, in any blocking.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/decode/hrpt.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/hrpt_mod.h"

using namespace revenant;

namespace {

siggen::HrptImage test_image(std::size_t rows) {
    siggen::HrptImage img;
    img.rows = rows;
    for (std::size_t ch = 0; ch < decode::kHrptChannels; ++ch) {
        img.channels[ch].resize(rows * decode::kHrptEarthSamples);
        for (std::size_t r = 0; r < rows; ++r) {
            for (std::size_t s = 0; s < decode::kHrptEarthSamples; ++s) {
                img.channels[ch][r * decode::kHrptEarthSamples + s] =
                    static_cast<std::uint16_t>((s / 2 + 97 * ch + 13 * r) % 1024);
            }
        }
    }
    return img;
}

struct Capture {
    std::vector<decode::HrptFrame> frames;
    std::vector<decode::ImageRow> rows;
    std::vector<decode::ImageOpen> opens;
    std::vector<decode::ImageClose> closes;
    double carrier = 0.0;
};

Capture run(const std::vector<dsp::Complex32>& x, double fs, std::size_t chunk) {
    Capture cap;
    decode::HrptEvents ev;
    ev.on_frame = [&](const decode::HrptFrame& f) { cap.frames.push_back(f); };
    ev.image.on_open = [&](const decode::ImageOpen& o) { cap.opens.push_back(o); };
    ev.image.on_row = [&](const decode::ImageRow& r) { cap.rows.push_back(r); };
    ev.image.on_close = [&](const decode::ImageClose& c) { cap.closes.push_back(c); };
    decode::HrptConfig cfg;
    cfg.sample_rate = fs;
    auto dec = decode::HrptDecoder::create(cfg, ev);
    REQUIRE(dec.has_value());
    for (std::size_t i = 0; i < x.size(); i += chunk) {
        dec->push(std::span<const dsp::Complex32>(x.data() + i, std::min(chunk, x.size() - i)));
    }
    cap.carrier = dec->carrier_hz();
    dec->finish();
    return cap;
}

// Word errors over every decoded frame but the first, matched to the sent
// frame by its millisecond field. The first frame is excluded because the
// carrier and timing loops are still settling through it; its errors are
// reported separately by the noise case.
double word_error_rate(const Capture& cap, const std::vector<siggen::HrptFrameWords>& sent) {
    std::size_t bad = 0;
    std::size_t n = 0;
    for (std::size_t i = 1; i < cap.frames.size(); ++i) {
        const auto& f = cap.frames[i];
        for (const auto& s : sent) {
            if (s[decode::kHrptTimeWord + 3] == f.words[decode::kHrptTimeWord + 3] &&
                s[decode::kHrptTimeWord + 2] == f.words[decode::kHrptTimeWord + 2]) {
                for (std::size_t w = 0; w < decode::kHrptFrameWords; ++w) {
                    bad += s[w] != f.words[w] ? 1 : 0;
                    ++n;
                }
                break;
            }
        }
    }
    return n == 0 ? 1.0 : static_cast<double>(bad) / static_cast<double>(n);
}

}  // namespace

TEST_CASE("HRPT frame sync regenerates KLM Table 4.1.3.1-1 words 1 to 6", "[hrpt]") {
    // The six words as printed, bit 1 first.
    constexpr std::uint16_t kPrinted[6] = {0b1010000100, 0b0101101111, 0b1101011100,
                                           0b0110011101, 0b1000001111, 0b0010010101};
    const std::uint64_t sync = decode::hrpt_frame_sync();
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(((sync >> (50 - 10 * i)) & 0x3FFU) == kPrinted[i]);
    }
    // Table 4.1.3.1-1 word counts add to the minor frame.
    CHECK(6 + 2 + 4 + 10 + 30 + 50 + 1 + 520 + 127 + 10240 + 100 == decode::kHrptFrameWords);
    CHECK(decode::kHrptEarthSamples * decode::kHrptChannels == 10240);
    // Table 4.1.2-2: 6 frames of 110,900 bits is 665,400 bit/s.
    CHECK(6.0 * decode::kHrptFrameWords * 10 == decode::kHrptBitRate);
}

TEST_CASE("HRPT round trip with carrier offset, drift and clock error", "[hrpt]") {
    const auto sent = siggen::hrpt_format(test_image(6));
    siggen::HrptBasebandSpec spec;
    spec.carrier_hz = 35000.0;
    spec.drift_hz_per_s = -800.0;
    spec.clock_ppm = 25.0;
    const auto x = siggen::hrpt_baseband(sent, spec);
    const auto cap = run(x, spec.sample_rate, 65536);
    INFO("frames " << cap.frames.size() << " carrier " << cap.carrier);
    REQUIRE(cap.frames.size() >= sent.size() - 1);
    CHECK(word_error_rate(cap, sent) == 0.0);
    const auto& f = cap.frames.back();
    CHECK(f.spacecraft == 7);
    CHECK(f.day == 123);
    CHECK(f.channel_3a);
    CHECK(f.minor_frame >= 1);
    CHECK(cap.opens.size() == 5);
    CHECK(cap.opens[0].width == 2048);
    CHECK(cap.opens[0].line_rate_hz == 6.0);
    CHECK(cap.closes.size() == 5);
    CHECK(cap.closes[0].complete);
    CHECK(cap.rows.size() == 5 * cap.frames.size());
    CHECK(std::abs(cap.carrier - (35000.0 - 800.0 * static_cast<double>(x.size()) /
                                               spec.sample_rate)) < 300.0);
}

TEST_CASE("HRPT at the minimum rate of two samples a chip", "[hrpt]") {
    const auto sent = siggen::hrpt_format(test_image(4));
    siggen::HrptBasebandSpec spec;
    spec.sample_rate = 2661600.0;
    spec.carrier_hz = -12000.0;
    const auto cap = run(siggen::hrpt_baseband(sent, spec), spec.sample_rate, 65536);
    REQUIRE(cap.frames.size() >= sent.size() - 1);
    CHECK(word_error_rate(cap, sent) == 0.0);
}

TEST_CASE("HRPT in noise", "[hrpt]") {
    const auto sent = siggen::hrpt_format(test_image(5));
    siggen::HrptBasebandSpec spec;
    spec.carrier_hz = 20000.0;
    const auto clean = siggen::hrpt_baseband(sent, spec);
    for (double ebn0 : {12.0, 9.0, 7.0}) {
        auto x = clean;
        REQUIRE(siggen::add_awgn(x, siggen::NoiseLevel::eb_over_n0_db(ebn0, decode::kHrptBitRate),
                                 static_cast<dsp::SampleRate>(spec.sample_rate),
                                 0x4890'0000ULL + static_cast<std::uint64_t>(ebn0))
                    .has_value());
        const auto cap = run(x, spec.sample_rate, 65536);
        const double wer = word_error_rate(cap, sent);
        WARN("Eb/N0 " << ebn0 << " dB (total power): frames " << cap.frames.size()
                      << " of " << sent.size() << ", word error rate " << wer);
        CHECK(cap.frames.size() >= sent.size() - 1);
        if (ebn0 >= 12.0) {
            CHECK(wer < 0.01);
        } else if (ebn0 >= 9.0) {
            CHECK(wer < 0.1);
        }
    }
}

TEST_CASE("HRPT decodes the same in any blocking", "[hrpt]") {
    const auto sent = siggen::hrpt_format(test_image(3));
    siggen::HrptBasebandSpec spec;
    spec.carrier_hz = 5000.0;
    const auto x = siggen::hrpt_baseband(sent, spec);
    const auto whole = run(x, spec.sample_rate, x.size());
    REQUIRE(whole.frames.size() >= 2);
    for (std::size_t chunk : {std::size_t{1}, std::size_t{999}, std::size_t{65536}}) {
        const auto part = run(x, spec.sample_rate, chunk);
        INFO("chunk " << chunk);
        REQUIRE(part.frames.size() == whole.frames.size());
        for (std::size_t i = 0; i < whole.frames.size(); ++i) {
            REQUIRE(part.frames[i].words == whole.frames[i].words);
            REQUIRE(part.frames[i].sample == whole.frames[i].sample);
        }
    }
}
