// Radiofax: charts through core/dsp/synth/wefax_mod.h and back through
// core/decode/wefax.h, scored as PSNR against what went in.
//
// PSNR is reported in WARN lines and asserted loosely, for the reason
// tests/decode/CMakeLists.txt gives for error rates.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "core/decode/wefax.h"
#include "core/dsp/synth/rds_mod.h"
#include "core/dsp/synth/wefax_mod.h"

using namespace revenant;

namespace {

// A chart-like picture: a white field, a gradient band, black bars and a
// grid, wide enough features that the 1200 Hz front end can carry them.
std::vector<std::uint8_t> chart(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h, 255);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t& v = out[static_cast<std::size_t>(y) * w + x];
            if (y < h / 3) {
                v = static_cast<std::uint8_t>(255 * x / w);
            } else if ((x / (w / 12)) % 3 == 0 && y < 2 * h / 3) {
                v = 0;
            } else if (x % (w / 8) < w / 64 || y % 32 < 2) {
                v = 60;
            }
        }
    }
    return out;
}

struct Collected {
    int opens = 0;
    int closes = 0;
    bool complete = false;
    std::uint32_t width = 0;
    std::string label;
    std::vector<std::vector<std::uint8_t>> rows;

    [[nodiscard]] decode::ImageSink sink() {
        decode::ImageSink s;
        s.on_open = [this](const decode::ImageOpen& o) {
            ++opens;
            width = o.width;
            label = o.label;
        };
        s.on_row = [this](const decode::ImageRow& r) {
            if (r.row == rows.size()) {
                rows.push_back(r.bytes);
            }
        };
        s.on_close = [this](const decode::ImageClose& c) {
            ++closes;
            complete = c.complete;
        };
        return s;
    }
};

Collected decode_audio(std::span<const float> audio, decode::WefaxConfig config,
                       decode::WefaxStats* stats = nullptr, std::size_t max_chunk = 0) {
    auto decoder = decode::WefaxDecoder::create(config);
    REQUIRE(decoder.has_value());
    Collected got;
    const decode::ImageSink sink = got.sink();
    if (max_chunk == 0) {
        decoder->process(audio, sink);
    } else {
        std::mt19937_64 engine(max_chunk);
        std::uniform_int_distribution<std::size_t> size(1, max_chunk);
        for (std::size_t at = 0; at < audio.size();) {
            const std::size_t n = std::min(size(engine), audio.size() - at);
            decoder->process(audio.subspan(at, n), sink);
            at += n;
        }
    }
    decoder->flush(sink);
    if (stats != nullptr) {
        *stats = decoder->stats();
    }
    return got;
}

// Rows the decoder did not deliver count as black.
double psnr(const Collected& got, const std::vector<std::uint8_t>& want, std::uint32_t w) {
    const std::size_t h = want.size() / w;
    double err = 0.0;
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const double g = y < got.rows.size() && got.rows[y].size() == w ? got.rows[y][x] : 0.0;
            const double d = g - want[y * w + x];
            err += d * d;
        }
    }
    const double mse = err / static_cast<double>(want.size());
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

std::vector<float> render(const siggen::WefaxModConfig& config, const std::vector<std::uint8_t>& image) {
    auto audio = siggen::wefax_render(config, image);
    REQUIRE(audio.has_value());
    return std::move(*audio);
}

void add_noise(std::vector<float>& audio, double snr_2500_db, double rate, std::uint64_t seed) {
    REQUIRE(siggen::add_real_awgn(audio, 0.5 * 0.5 / 2.0,
                                  siggen::NoiseLevel::snr_in_2500_hz_db(snr_2500_db),
                                  static_cast<dsp::SampleRate>(rate), seed)
                .has_value());
}

constexpr dsp::SampleRate kRate = 12'000;

}  // namespace

TEST_CASE("IOC x pi gives the handbook's line widths", "[decode][wefax]") {
    CHECK(decode::wefax_width(576) == 1810);
    CHECK(decode::wefax_width(288) == 905);
}

TEST_CASE("every rate and IOC crosses the round trip clean", "[decode][wefax]") {
    struct Case {
        int lpm;
        int ioc;
        bool asymmetric;
    };
    for (const Case c : {Case{120, 576, false}, Case{60, 288, false}, Case{240, 576, true},
                         Case{90, 576, true}, Case{120, 288, true}}) {
        siggen::WefaxModConfig config;
        config.rate = kRate;
        config.lpm = c.lpm;
        config.ioc = c.ioc;
        config.asymmetric_phasing = c.asymmetric;
        const std::uint32_t w = decode::wefax_width(c.ioc);
        const auto image = chart(w, 96);
        decode::WefaxStats stats;
        const Collected got = decode_audio(render(config, image), decode::WefaxConfig{kRate}, &stats);
        const double score = psnr(got, image, w);
        WARN(std::format("{}/{} {}: PSNR {:.1f} dB, {} rows, {} phasing lines, ratio {:.6f}", c.lpm,
                         c.ioc, c.asymmetric ? "5/95" : "50/50", score, got.rows.size(),
                         stats.phasing_lines, stats.last_period_ratio));
        CHECK(got.opens == 1);
        CHECK(got.closes == 1);
        CHECK(got.complete);
        CHECK(got.width == w);
        CHECK(got.rows.size() == 96);
        CHECK(got.label == std::format("WEFAX {}/{}", c.lpm, c.ioc));
        CHECK(stats.last_ioc == c.ioc);
        CHECK(stats.last_lpm == c.lpm);
        CHECK(score > 24.0);
    }
}

TEST_CASE("120/576 against noise, measured", "[decode][wefax]") {
    siggen::WefaxModConfig config;
    config.rate = kRate;
    const std::uint32_t w = decode::wefax_width(576);
    const auto image = chart(w, 96);
    const auto clean = render(config, image);
    // Loose floors, a few dB under what was measured.
    const std::pair<double, double> points[] = {{30.0, 28.0}, {15.0, 19.0}, {8.0, 13.0}};
    for (const auto& [snr, floor] : points) {
        auto audio = clean;
        add_noise(audio, snr, kRate, 0xFA7);
        decode::WefaxStats stats;
        const Collected got = decode_audio(audio, decode::WefaxConfig{kRate}, &stats);
        const double score = psnr(got, image, w);
        WARN(std::format("120/576 at {} dB/2500 Hz: PSNR {:.1f} dB, {} rows, complete {}", snr,
                         score, got.rows.size(), got.complete));
        CHECK(got.opens == 1);
        CHECK(score >= floor);
    }
}

TEST_CASE("a mistuned receiver is corrected from the start tone", "[decode][wefax]") {
    const std::uint32_t w = decode::wefax_width(576);
    const auto image = chart(w, 48);
    for (const double offset : {-80.0, 50.0}) {
        siggen::WefaxModConfig config;
        config.rate = kRate;
        config.offset_hz = offset;
        decode::WefaxStats stats;
        const Collected got = decode_audio(render(config, image), decode::WefaxConfig{kRate}, &stats);
        const double score = psnr(got, image, w);
        INFO(std::format("offset {} Hz: measured {:.1f} Hz, PSNR {:.1f} dB", offset,
                         stats.last_offset_hz, score));
        CHECK(std::abs(stats.last_offset_hz - offset) < 5.0);
        CHECK(score > 24.0);
    }
}

TEST_CASE("a sample clock off the transmitter's is fitted from phasing", "[decode][wefax]") {
    const std::uint32_t w = decode::wefax_width(576);
    const auto image = chart(w, 96);
    for (const double ppm : {-300.0, 200.0}) {
        siggen::WefaxModConfig config;
        config.rate = kRate * (1.0 + ppm * 1.0e-6);
        decode::WefaxStats stats;
        const Collected got = decode_audio(render(config, image), decode::WefaxConfig{kRate}, &stats);
        const double score = psnr(got, image, w);
        WARN(std::format("{} ppm: fitted ratio {:.6f}, PSNR {:.1f} dB", ppm,
                         stats.last_period_ratio, score));
        CHECK(std::abs(stats.last_period_ratio - (1.0 + ppm * 1.0e-6)) < 1.0e-5);
        CHECK(score > 24.0);
    }
}

TEST_CASE("polarity is a setting, and a fixed rate skips the search", "[decode][wefax]") {
    const std::uint32_t w = decode::wefax_width(288);
    const auto image = chart(w, 48);
    siggen::WefaxModConfig config;
    config.rate = kRate;
    config.ioc = 288;
    config.lpm = 90;
    config.white_high = false;
    const auto audio = render(config, image);
    decode::WefaxConfig rx{kRate};
    rx.white_high = false;
    rx.lpm = 90;
    const Collected right = decode_audio(audio, rx);
    CHECK(psnr(right, image, w) > 24.0);
    rx.white_high = true;
    // The other way up the phasing's white is black, so the leading edge is
    // the wrong one and the picture a negative: nowhere near.
    const Collected wrong = decode_audio(audio, rx);
    CHECK(psnr(wrong, image, w) < 12.0);
    CHECK_FALSE(decode::WefaxDecoder::create(decode::WefaxConfig{kRate, true, 100}).has_value());
}

TEST_CASE("the rows do not depend on how the audio is cut into chunks", "[decode][wefax]") {
    siggen::WefaxModConfig config;
    config.rate = kRate;
    config.lpm = 240;
    const std::uint32_t w = decode::wefax_width(576);
    const auto image = chart(w, 64);
    auto audio = render(config, image);
    add_noise(audio, 12.0, kRate, 0xC0C0);
    const Collected whole = decode_audio(audio, decode::WefaxConfig{kRate});
    CHECK(whole.opens == 1);
    for (const std::size_t max_chunk : {std::size_t{1}, std::size_t{97}, std::size_t{4096}}) {
        const Collected cut = decode_audio(audio, decode::WefaxConfig{kRate}, nullptr, max_chunk);
        INFO("chunks up to " << max_chunk);
        const bool same = cut.rows == whole.rows;
        CHECK(same);
        CHECK(cut.complete == whole.complete);
    }
}

TEST_CASE("a stream that stops mid-chart closes it incomplete", "[decode][wefax]") {
    siggen::WefaxModConfig config;
    config.rate = kRate;
    const auto image = chart(decode::wefax_width(576), 96);
    const auto audio = render(config, image);
    // Start 5 s, phasing 30 s, then half of the 48 s of picture.
    const auto cut = static_cast<std::size_t>((0.5 + 35.0 + 24.0) * kRate);
    const Collected got = decode_audio(std::span<const float>(audio).first(cut), decode::WefaxConfig{kRate});
    CHECK(got.opens == 1);
    CHECK(got.closes == 1);
    CHECK_FALSE(got.complete);
    CHECK(got.rows.size() > 40);
    CHECK(got.rows.size() < 50);
}
