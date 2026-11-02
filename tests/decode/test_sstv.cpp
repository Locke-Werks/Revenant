// SSTV: every Dayton paper mode through core/dsp/synth/sstv_mod.h and back
// through core/decode/sstv.h, scored as PSNR against the image that went in.
//
// PSNR is reported in INFO lines and asserted loosely, for the reason
// tests/decode/CMakeLists.txt gives for error rates.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <tuple>
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

struct Collected {
    int opens = 0;
    int closes = 0;
    bool complete = false;
    std::uint32_t width = 0;
    std::uint32_t rows_closed = 0;
    decode::ImageRow::Model pixels = decode::ImageRow::Rgb888;
    std::string mode;
    std::vector<std::vector<std::uint8_t>> rows;
    std::vector<bool> seen;

    [[nodiscard]] decode::ImageSink sink() {
        decode::ImageSink s;
        s.on_open = [this](const decode::ImageOpen& o) {
            ++opens;
            width = o.width;
            pixels = o.model;
            mode = o.label;
        };
        s.on_row = [this](const decode::ImageRow& r) {
            if (rows.size() <= r.row) {
                rows.resize(r.row + 1);
                seen.resize(r.row + 1, false);
            }
            rows[r.row] = r.bytes;
            seen[r.row] = true;
        };
        s.on_close = [this](const decode::ImageClose& c) {
            ++closes;
            complete = c.complete;
            rows_closed = c.rows;
        };
        return s;
    }
};

Collected decode_audio(std::span<const float> audio, dsp::SampleRate rate,
                       decode::SstvStats* stats = nullptr, std::size_t max_chunk = 0,
                       std::uint64_t seed = 1) {
    auto decoder = decode::SstvDecoder::create(decode::SstvConfig{rate});
    REQUIRE(decoder.has_value());
    Collected collected;
    const decode::ImageSink out = collected.sink();
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
    return collected;
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
        const auto audio = render(mode, kRate);
        decode::SstvStats stats;
        const Collected got = decode_audio(audio, kRate, &stats);
        const double score = psnr(got, mode, test_image(mode.width, mode.height));
        WARN(std::format("{}: PSNR {:.1f} dB, syncs {} found {} missed", mode.name, score,
                         stats.syncs_found, stats.syncs_missed));
        CHECK(got.opens == 1);
        CHECK(got.closes == 1);
        CHECK(got.complete);
        CHECK(got.rows_closed == mode.height);
        CHECK(got.mode == mode.name);
        if (!mode.fax_start) {
            CHECK(stats.last_vis == mode.vis);
        }
        CHECK(score > 24.0);
    }
}

TEST_CASE("FAX480 opens on its start tone and phasing, against noise and mistuning",
          "[decode][sstv][fax480]") {
    constexpr dsp::SampleRate kRate = 12'000;
    const decode::SstvMode& mode = *decode::sstv_mode_by_name("FAX480");
    const auto want = test_image(mode.width, mode.height);
    const std::pair<double, double> points[] = {{30.0, 28.0}, {15.0, 18.0}, {8.0, 12.0}};
    for (const auto& [snr, floor] : points) {
        auto audio = render(mode, kRate, 40.0);
        add_noise(audio, snr, kRate, 0xFA4);
        decode::SstvStats stats;
        const Collected got = decode_audio(audio, kRate, &stats);
        const double score = psnr(got, mode, want);
        WARN(std::format("FAX480 at {} dB/2500 Hz, 40 Hz high: PSNR {:.1f} dB, syncs {} found {} "
                         "missed",
                         snr, score, stats.syncs_found, stats.syncs_missed));
        CHECK(got.opens == 1);
        CHECK(got.pixels == decode::ImageRow::Gray8);
        CHECK(got.opens <= 1);
        CHECK(score >= floor);
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
        CHECK(got.opens <= 1);
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

// Bruchanov's handbook from here on.

TEST_CASE("the handbook's modes reproduce its transfer times and line rates",
          "[decode][sstv][handbook]") {
    // Table 4.6 and chapter 5. AVT's include the three VIS codes and the
    // digital header, about eight seconds; the handbook's own totals are
    // rounded to the second, so the tolerance is 2 %.
    const double vis_ms = 2.0 * decode::kVisLeaderMs + decode::kVisBreakMs + 10.0 * decode::kVisBitMs;
    const double avt_lead_ms = 3.0 * vis_ms + decode::kAvtFrames * decode::kAvtFrameMs;
    const std::tuple<std::string_view, double, double> printed[] = {
        {"AVT 24", 31.0, 960.000 / 3.0}, {"AVT 90", 98.0, 480.000 / 3.0},
        {"AVT 94", 102.0, 384.000 / 3.0}, {"AVT 188", 196.0, 384.000 / 3.0},
        {"AVT 125 BW", 133.0, 192.000},  {"MP73", 73.0, 210.526},
        {"MP115", 115.0, 133.038},       {"MP140", 140.0, 110.092},
        {"MP175", 175.0, 87.591},        {"MR73", 73.0, 419.141},
        {"MR90", 90.0, 340.619},         {"MR115", 115.0, 266.489},
        {"MR140", 140.0, 218.858},       {"MR175", 175.0, 175.362},
        {"ML180", 180.0, 330.306},       {"ML240", 240.0, 248.293},
        {"ML280", 280.0, 212.277},       {"ML320", 320.0, 185.960}};
    for (const auto& [name, seconds, lpm] : printed) {
        const decode::SstvMode* mode = decode::sstv_mode_by_name(name);
        REQUIRE(mode != nullptr);
        const double lead = mode->avt_header ? avt_lead_ms : 0.0;
        const double total = (lead + mode->period_ms() * mode->periods()) / 1000.0;
        // The handbook counts MMSSTV lines as luminance scans: two per PD-style
        // period for MP, and twice the period rate for MR and ML.
        const double rate = 60'000.0 / mode->period_ms() *
                            (mode->name.starts_with("MR") || mode->name.starts_with("ML") ? 2.0 : 1.0) *
                            mode->lines_per_period;
        INFO(std::format("{}: {:.2f} s against {} s, {:.3f} lpm against {}", name, total, seconds,
                         rate, lpm));
        CHECK(std::abs(total - seconds) / seconds < 0.02);
        CHECK(std::abs(rate - lpm) / lpm < 0.001);
    }
}

TEST_CASE("each AVT variant and each 16-bit code names its own mode", "[decode][sstv][handbook]") {
    for (const char* name : {"AVT 90", "AVT 90 Narrow", "AVT 90 QRM", "AVT 90 Narrow QRM"}) {
        const decode::SstvMode& mode = *decode::sstv_mode_by_name(name);
        CHECK(decode::sstv_mode_by_vis(mode.vis) == &mode);
    }
    // Chapter 5 note b: AVT 188 shares 76 with Scottie DX, so the table lookup
    // answers Scottie DX and the decoder tells them apart by the AVT header.
    CHECK(decode::sstv_mode_by_vis(76)->name == "Scottie DX");
    for (const decode::SstvMode& mode : decode::sstv_modes()) {
        if (mode.vis16()) {
            CHECK((mode.vis & 0xFF) == decode::kVis16Low);
            CHECK(std::popcount(static_cast<unsigned>(mode.vis >> 8)) % 2 == 1);
        }
    }
}

TEST_CASE("AVT 90 against noise, mistuning and a clock error, measured",
          "[decode][sstv][handbook][avt]") {
    constexpr dsp::SampleRate kRate = 12'000;
    for (const char* name : {"AVT 90", "AVT 90 Narrow QRM"}) {
        const decode::SstvMode& mode = *decode::sstv_mode_by_name(name);
        const auto want = test_image(mode.width, mode.height);
        // Loose floors, a few dB under what was measured.
        const std::pair<double, double> points[] = {{30.0, 28.0}, {15.0, 16.0}, {8.0, 10.0}};
        for (const auto& [snr, floor] : points) {
            auto audio = render(mode, kRate, 35.0);
            add_noise(audio, snr, kRate, 0xA417);
            decode::SstvStats stats;
            const Collected got = decode_audio(audio, kRate, &stats);
            const double score = psnr(got, mode, want);
            WARN(std::format("{} at {} dB/2500 Hz, 35 Hz high: PSNR {:.1f} dB, offset {:.1f} Hz",
                             name, snr, score, stats.last_offset_hz));
            CHECK(got.opens == 1);
            CHECK(got.mode == mode.name);
            CHECK(score >= floor);
        }
        for (const double ppm : {-400.0, 250.0}) {
            const auto audio = render(mode, kRate * (1.0 + ppm * 1.0e-6));
            decode::SstvStats stats;
            const Collected got = decode_audio(audio, kRate, &stats);
            const double score = psnr(got, mode, want);
            WARN(std::format("{} at {} ppm: header ratio {:.6f}, PSNR {:.1f} dB", name, ppm,
                             stats.last_period_ratio, score));
            CHECK(std::abs(stats.last_period_ratio - (1.0 + ppm * 1.0e-6)) < 1.0e-5);
            CHECK(score > 24.0);
        }
    }
}

TEST_CASE("interlaced modes hand the sink their rows in order", "[decode][sstv][handbook]") {
    constexpr dsp::SampleRate kRate = 12'000;
    for (const char* name : {"MSCAN TV-1", "AVT 24 QRM"}) {
        const decode::SstvMode& mode = *decode::sstv_mode_by_name(name);
        const auto audio = render(mode, kRate);
        auto decoder = decode::SstvDecoder::create(decode::SstvConfig{kRate});
        REQUIRE(decoder.has_value());
        std::vector<std::uint32_t> order;
        decode::ImageSink sink;
        sink.on_row = [&](const decode::ImageRow& r) { order.push_back(r.row); };
        // Cut short in the last field, so held rows go out at the close.
        decoder->process(std::span<const float>(audio).first(audio.size() * 9 / 10), sink);
        decoder->flush(sink);
        INFO(name);
        CHECK(std::ranges::is_sorted(order));
        CHECK(std::ranges::adjacent_find(order) == order.end());
        CHECK(order.size() > mode.height / 2);
    }
}

TEST_CASE("MP115 and AVT 24 rows do not depend on how the audio is cut",
          "[decode][sstv][handbook]") {
    constexpr dsp::SampleRate kRate = 12'000;
    for (const char* name : {"MP115", "AVT 24 QRM"}) {
        const decode::SstvMode& mode = *decode::sstv_mode_by_name(name);
        auto audio = render(mode, kRate);
        add_noise(audio, 12.0, kRate, 0xC4C5);
        const Collected whole = decode_audio(audio, kRate);
        CHECK(whole.opens == 1);
        for (const std::size_t max_chunk : {std::size_t{1}, std::size_t{97}, std::size_t{4096}}) {
            const Collected cut = decode_audio(audio, kRate, nullptr, max_chunk, max_chunk);
            INFO(name << ", chunks up to " << max_chunk);
            const bool same = cut.rows == whole.rows;
            CHECK(same);
            CHECK(cut.complete == whole.complete);
        }
    }
}
