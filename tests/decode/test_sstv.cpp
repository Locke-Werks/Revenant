// SSTV: every Dayton paper mode through core/dsp/synth/sstv_mod.h and back
// through core/decode/sstv.h, scored as PSNR against the image that went in.
//
// PSNR is reported in INFO lines and asserted loosely, for the reason
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

#include "core/decode/sstv.h"
#include "core/dsp/synth/rds_mod.h"
#include "core/dsp/synth/sstv_mod.h"

using namespace revenant;

namespace {

// A picture with gradients and edges in every channel, the same for every mode.
std::vector<std::uint8_t> test_image(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 3);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 3;
            const double fx = static_cast<double>(x) / w;
            const double fy = static_cast<double>(y) / h;
            out[i] = static_cast<std::uint8_t>(255.0 * fx);
            out[i + 1] = static_cast<std::uint8_t>(255.0 * fy);
            const double dx = fx - 0.5;
            const double dy = fy - 0.5;
            out[i + 2] = (dx * dx + dy * dy) < 0.08 ? 220 : 40;
        }
    }
    return out;
}

struct Collected : decode::ImageSink {
    int opens = 0;
    int closes = 0;
    bool complete = false;
    std::uint32_t width = 0;
    decode::ImagePixels pixels = decode::ImagePixels::Rgb;
    std::string mode;
    std::vector<std::vector<std::uint8_t>> rows;
    std::vector<bool> seen;

    void open(std::uint64_t, std::uint32_t w, decode::ImagePixels p, double,
              std::string_view m) override {
        ++opens;
        width = w;
        pixels = p;
        mode = std::string(m);
    }
    void row(std::uint32_t index, std::span<const std::uint8_t> data) override {
        if (rows.size() <= index) {
            rows.resize(index + 1);
            seen.resize(index + 1, false);
        }
        rows[index].assign(data.begin(), data.end());
        seen[index] = true;
    }
    void close(bool c) override {
        ++closes;
        complete = c;
    }
};

Collected decode_audio(std::span<const float> audio, dsp::SampleRate rate,
                       decode::SstvStats* stats = nullptr, std::size_t max_chunk = 0,
                       std::uint64_t seed = 1) {
    auto decoder = decode::SstvDecoder::create(decode::SstvConfig{rate});
    REQUIRE(decoder.has_value());
    Collected out;
    if (max_chunk == 0) {
        decoder->process(audio, out);
    } else {
        std::mt19937_64 engine(seed);
        std::uniform_int_distribution<std::size_t> size(1, max_chunk);
        std::size_t at = 0;
        while (at < audio.size()) {
            const std::size_t n = std::min(size(engine), audio.size() - at);
            decoder->process(audio.subspan(at, n), out);
            at += n;
        }
    }
    decoder->flush(out);
    if (stats != nullptr) {
        *stats = decoder->stats();
    }
    return out;
}

// Over every row the decoder delivered and every row it did not, which count
// as black.
double psnr(const Collected& got, const decode::SstvMode& mode,
            const std::vector<std::uint8_t>& want) {
    double err = 0.0;
    std::size_t count = 0;
    for (std::uint32_t y = 0; y < mode.height; ++y) {
        const bool have = y < got.rows.size() && got.seen[y];
        for (std::uint32_t x = 0; x < mode.width; ++x) {
            for (int c = 0; c < 3; ++c) {
                const std::size_t wi = (static_cast<std::size_t>(y) * mode.width + x) * 3 + c;
                double g = 0.0;
                double expect = want[wi];
                if (mode.colour == decode::SstvColour::Gray) {
                    expect = want[(static_cast<std::size_t>(y) * mode.width + x) * 3 + 1];
                    g = have ? got.rows[y][x] : 0.0;
                } else {
                    g = have ? got.rows[y][3 * x + c] : 0.0;
                }
                err += (g - expect) * (g - expect);
                ++count;
            }
        }
    }
    const double mse = err / static_cast<double>(count);
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

double mean_power(std::span<const float> x) {
    double s = 0.0;
    for (const float v : x) {
        s += static_cast<double>(v) * v;
    }
    return x.empty() ? 0.0 : s / static_cast<double>(x.size());
}

std::vector<float> render(const decode::SstvMode& mode, double rate, double offset_hz = 0.0) {
    siggen::SstvModConfig config;
    config.rate = rate;
    config.offset_hz = offset_hz;
    const auto image = test_image(mode.width, mode.height);
    auto audio = siggen::sstv_render(config, mode, image);
    REQUIRE(audio.has_value());
    return std::move(*audio);
}

void add_noise(std::vector<float>& audio, double snr_2500_db, dsp::SampleRate rate,
               std::uint64_t seed) {
    const auto pad = static_cast<std::size_t>(siggen::SstvModConfig{}.pad_seconds * rate);
    const double power =
        mean_power(std::span<const float>(audio).subspan(pad, audio.size() - 2 * pad));
    REQUIRE(siggen::add_real_awgn(audio, power, siggen::NoiseLevel::snr_in_2500_hz_db(snr_2500_db),
                                  rate, seed)
                .has_value());
}

}  // namespace

TEST_CASE("Appendix B round trips through both of its matrices", "[decode][sstv]") {
    for (int r = 0; r <= 255; r += 51) {
        for (int g = 0; g <= 255; g += 51) {
            for (int b = 0; b <= 255; b += 51) {
                const auto ycc = decode::sstv_rgb_to_ycc(r, g, b);
                const auto rgb = decode::sstv_ycc_to_rgb(ycc.y, ycc.ry, ycc.by);
                CHECK(std::abs(rgb.r - r) < 1.5);
                CHECK(std::abs(rgb.g - g) < 1.5);
                CHECK(std::abs(rgb.b - b) < 1.5);
            }
        }
    }
}

TEST_CASE("the mode table reproduces the paper's transmission times", "[decode][sstv]") {
    // Total times the paper prints, "not including cal. header/VIS".
    const std::pair<std::string_view, double> printed[] = {
        {"Scottie 1", 109.6}, {"Scottie 2", 71.1},  {"Scottie DX", 268.9}, {"Martin 1", 114.3},
        {"Martin 2", 58.06},  {"Robot 36", 36.0},   {"Robot 72", 72.0},    {"Wraase SC2-180", 182.0},
        {"Pasokon P3", 203.0}, {"Pasokon P5", 304.6}, {"Pasokon P7", 406.1}, {"PD50", 49.7},
        {"PD90", 90.0},       {"PD120", 126.1},     {"PD160", 160.9},      {"PD180", 187.1},
        {"PD240", 248.0},     {"PD290", 288.7}};
    for (const auto& [name, seconds] : printed) {
        const decode::SstvMode* mode = decode::sstv_mode_by_name(name);
        REQUIRE(mode != nullptr);
        const double total = (mode->lead_ms() + mode->period_ms() * mode->periods()) / 1000.0;
        INFO(std::format("{}: {:.2f} s against {} s", name, total, seconds));
        CHECK(std::abs(total - seconds) / seconds < 0.01);
    }
}

TEST_CASE("every mode crosses the round trip clean, VIS code included", "[decode][sstv]") {
    constexpr dsp::SampleRate kRate = 12'000;
    for (const decode::SstvMode& mode : decode::sstv_modes()) {
        if (mode.vis < 0) {
            continue;
        }
        const auto audio = render(mode, kRate);
        decode::SstvStats stats;
        const Collected got = decode_audio(audio, kRate, &stats);
        const double score = psnr(got, mode, test_image(mode.width, mode.height));
        WARN(std::format("{}: PSNR {:.1f} dB, syncs {} found {} missed", mode.name, score,
                         stats.syncs_found, stats.syncs_missed));
        CHECK(got.opens == 1);
        CHECK(got.closes == 1);
        CHECK(got.complete);
        CHECK(got.mode == mode.name);
        CHECK(stats.last_vis == mode.vis);
        CHECK(score > 24.0);
    }
}

TEST_CASE("Martin 1 against noise at 48 kHz, measured", "[decode][sstv]") {
    constexpr dsp::SampleRate kRate = 48'000;
    const decode::SstvMode& mode = *decode::sstv_mode_by_name("Martin 1");
    const auto clean = render(mode, kRate);
    const auto want = test_image(mode.width, mode.height);
    // Loose floors, a few dB under what was measured.
    const std::pair<double, double> points[] = {{30.0, 28.0}, {15.0, 20.0}, {8.0, 13.0}, {3.0, 0.0}};
    for (const auto& [snr, floor] : points) {
        auto audio = clean;
        add_noise(audio, snr, kRate, 0x55717);
        decode::SstvStats stats;
        const Collected got = decode_audio(audio, kRate, &stats);
        const double score = psnr(got, mode, want);
        WARN(std::format("Martin 1 at {} dB/2500 Hz: PSNR {:.1f} dB, syncs {} found {} missed",
                         snr, score, stats.syncs_found, stats.syncs_missed));
        CHECK(score >= floor);
    }
}

TEST_CASE("a mistuned receiver is corrected from the leader", "[decode][sstv]") {
    constexpr dsp::SampleRate kRate = 12'000;
    const decode::SstvMode& mode = *decode::sstv_mode_by_name("Scottie 2");
    for (const double offset : {-100.0, 60.0, 120.0}) {
        decode::SstvStats stats;
        const Collected got = decode_audio(render(mode, kRate, offset), kRate, &stats);
        const double score = psnr(got, mode, test_image(mode.width, mode.height));
        INFO(std::format("offset {} Hz: measured {:.1f} Hz, PSNR {:.1f} dB", offset,
                         stats.last_offset_hz, score));
        CHECK(std::abs(stats.last_offset_hz - offset) < 10.0);
        CHECK(score > 24.0);
    }
}

TEST_CASE("a sample clock that differs from the transmitter's is fitted out", "[decode][sstv]") {
    constexpr dsp::SampleRate kRate = 12'000;
    const decode::SstvMode& mode = *decode::sstv_mode_by_name("Martin 2");
    for (const double ppm : {-500.0, 300.0}) {
        // Rendered fast or slow, read at the nominal rate.
        const auto audio = render(mode, kRate * (1.0 + ppm * 1.0e-6));
        decode::SstvStats stats;
        const Collected got = decode_audio(audio, kRate, &stats);
        const double score = psnr(got, mode, test_image(mode.width, mode.height));
        INFO(std::format("{} ppm: fitted period ratio {:.6f}, PSNR {:.1f} dB", ppm,
                         stats.last_period_ratio, score));
        CHECK(std::abs(stats.last_period_ratio - (1.0 + ppm * 1.0e-6)) < 1.0e-4);
        CHECK(score > 24.0);
    }
}

TEST_CASE("the rows do not depend on how the audio is cut into chunks", "[decode][sstv]") {
    constexpr dsp::SampleRate kRate = 12'000;
    const decode::SstvMode& mode = *decode::sstv_mode_by_name("Robot 36");
    auto audio = render(mode, kRate);
    add_noise(audio, 12.0, kRate, 0xC4C4);
    const Collected whole = decode_audio(audio, kRate);
    for (const std::size_t max_chunk : {std::size_t{1}, std::size_t{97}, std::size_t{4096}}) {
        const Collected cut = decode_audio(audio, kRate, nullptr, max_chunk, max_chunk);
        INFO("chunks up to " << max_chunk);
        const bool same = cut.rows == whole.rows;
        CHECK(same);
        CHECK(cut.complete == whole.complete);
    }
}

TEST_CASE("a stream that stops mid-image closes it incomplete", "[decode][sstv]") {
    constexpr dsp::SampleRate kRate = 12'000;
    const decode::SstvMode& mode = *decode::sstv_mode_by_name("Robot 36");
    const auto audio = render(mode, kRate);
    const Collected got = decode_audio(std::span<const float>(audio).first(audio.size() / 2), kRate);
    CHECK(got.opens == 1);
    CHECK(got.closes == 1);
    CHECK_FALSE(got.complete);
    CHECK(got.rows.size() > 80);
}
