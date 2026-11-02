// Analogue television, ITU-R BT.470-6: the timing table against the
// document, then pictures through composite video, FM and negative AM
// against core/dsp/synth/atv_mod.h, with noise and an echo, and the same
// signal decoded in several blockings.
//
// Picture error is the mean absolute difference in 8-bit grey levels over
// every pixel of every completed frame, printed in a WARN line; the
// assertions around it are loose.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <numbers>
#include <string>
#include <vector>

#include "core/decode/atv.h"
#include "core/dsp/synth/atv_mod.h"
#include "core/dsp/synth/channel.h"

using namespace revenant;
using decode::AtvStandard;

namespace {

siggen::AtvPicture test_picture(AtvStandard standard, std::uint32_t width, int variant) {
    const auto t = decode::atv_timing(standard);
    siggen::AtvPicture p;
    p.width = width;
    p.height = 2 * t.rows_per_field;
    p.gray.resize(static_cast<std::size_t>(p.width) * p.height);
    for (std::uint32_t y = 0; y < p.height; ++y) {
        for (std::uint32_t x = 0; x < p.width; ++x) {
            const double fx = static_cast<double>(x) / p.width;
            const double fy = static_cast<double>(y) / p.height;
            double g = 128.0 + 90.0 * std::sin(2.0 * std::numbers::pi * (2.0 * fx + 0.5 * variant)) *
                                   std::cos(2.0 * std::numbers::pi * 1.5 * fy);
            if (fx > 0.4 && fx < 0.6 && fy > 0.4 && fy < 0.6) {
                g = variant % 2 == 0 ? 250.0 : 5.0;  // a box, to see the frames apart
            }
            p.gray[static_cast<std::size_t>(y) * p.width + x] = static_cast<std::uint8_t>(std::lround(g));
        }
    }
    return p;
}

struct Decoded {
    std::map<std::uint32_t, std::vector<std::vector<std::uint8_t>>> images;
    std::map<std::uint32_t, std::uint32_t> closed;  // id -> rows
    std::vector<decode::ImageRow> rows;
    decode::AtvStats stats;
    double seconds = 0.0;
};

Decoded run(const decode::AtvConfig& config, std::span<const float> video, std::size_t block) {
    Decoded d;
    decode::AtvCallbacks cb;
    cb.on_open = [&](const decode::ImageOpen& o) {
        d.images[o.image_id].assign(o.height, std::vector<std::uint8_t>(o.width, 0));
    };
    cb.on_row = [&](const decode::ImageRow& r) {
        d.images[r.image_id].at(r.row) = r.bytes;
        d.rows.push_back(r);
    };
    cb.on_close = [&](const decode::ImageClose& c) { d.closed[c.image_id] = c.rows; };
    auto rx = decode::AtvReceiver::create(config, cb);
    REQUIRE(rx.has_value());
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t at = 0; at < video.size(); at += block) {
        rx->push(video.subspan(at, std::min(block, video.size() - at)));
    }
    d.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    d.stats = rx->stats();
    return d;
}

// Mean absolute error of every completed frame against the picture it should
// be, and how many frames completed.
double frame_error(const Decoded& d, const std::vector<siggen::AtvPicture>& pictures,
                   std::size_t& complete) {
    complete = 0;
    double sum = 0.0;
    std::size_t count = 0;
    std::size_t index = 0;
    for (const auto& [id, rows] : d.images) {
        const auto closed = d.closed.find(id);
        if (closed == d.closed.end() || closed->second != rows.size() || index >= pictures.size()) {
            ++index;
            continue;
        }
        const auto& p = pictures[index++];
        ++complete;
        for (std::size_t y = 0; y < rows.size(); ++y) {
            for (std::size_t x = 0; x < rows[y].size(); ++x) {
                sum += std::abs(static_cast<double>(rows[y][x]) - p.gray[y * p.width + x]);
                ++count;
            }
        }
    }
    return count > 0 ? sum / static_cast<double>(count) : 255.0;
}

}  // namespace

TEST_CASE("ATV timing follows BT.470 tables 1, 1-1 and 1-2", "[decode][atv]") {
    const auto m = decode::atv_timing(AtvStandard::M525);
    const auto bg = decode::atv_timing(AtvStandard::BG625);
    // Table 1 items 1 and 2: lines per picture, two fields of it.
    CHECK(m.lines_per_field * 2 == 525.0);
    CHECK(bg.lines_per_field * 2 == 625.0);
    // Table 1 item 3: 15 734.264 Hz (M/NTSC) and 15 625 Hz line frequency.
    CHECK(std::abs(1e6 / m.line_us - 15734.264) < 0.1);
    CHECK(1e6 / bg.line_us == 15625.0);
    // Table 1-2: field-sync pulse shorter than half a line, so a serration
    // remains; l + m + n inside the field-blanking interval j.
    for (const auto& t : {m, bg}) {
        CHECK(t.field_sync_us < t.line_us / 2);
        CHECK(t.pre_equalising_lines + t.field_sync_lines + t.post_equalising_lines <
              t.field_blanking_lines);
        CHECK(t.active_start_us + t.front_porch_us < t.line_us);
    }
    // The first line of each field sits on the line grid: whole line periods
    // from OV in a first field, a half more in a second.
    for (const auto s : {AtvStandard::M525, AtvStandard::BG625}) {
        const double f1 = decode::atv_first_line_offset(s, false);
        const double f2 = decode::atv_first_line_offset(s, true);
        CHECK(f1 == std::floor(f1));
        CHECK(f2 - std::floor(f2) == 0.5);
        const auto t = decode::atv_timing(s);
        CHECK(f1 + t.rows_per_field + t.pre_equalising_lines <= t.lines_per_field + 0.5);
    }
}

TEST_CASE("ATV composite round trips for both line standards", "[decode][atv]") {
    for (const auto standard : {AtvStandard::M525, AtvStandard::BG625}) {
        constexpr dsp::SampleRate kRate = 13'500'000;
        std::vector<siggen::AtvPicture> pictures{test_picture(standard, 720, 0),
                                                 test_picture(standard, 720, 1),
                                                 test_picture(standard, 720, 2)};
        // Arbitrary gain and offset: the decoder measures its own levels.
        auto video = siggen::atv_composite(standard, kRate, pictures);
        for (auto& v : video) {
            v = 0.013F * v + 0.4F;
        }
        decode::AtvConfig config;
        config.standard = standard;
        config.sample_rate = kRate;
        const auto d = run(config, video, 100'000);
        std::size_t complete = 0;
        const double mae = frame_error(d, pictures, complete);
        WARN((standard == AtvStandard::M525 ? "525" : "625")
             << " composite: " << complete << " frames, mean error " << mae << " grey levels, "
             << d.stats.fields << " fields, line period " << d.stats.line_period_us << " us; "
             << d.seconds << " s for " << static_cast<double>(video.size()) / kRate << " s");
        CHECK(complete == pictures.size());
        CHECK(mae < 3.0);
    }
}

TEST_CASE("ATV over FM with noise", "[decode][atv]") {
    constexpr dsp::SampleRate kRate = 20'000'000;
    const auto standard = AtvStandard::BG625;
    std::vector<siggen::AtvPicture> pictures{test_picture(standard, 720, 0),
                                             test_picture(standard, 720, 1)};
    const auto video = siggen::atv_composite(standard, kRate, pictures);
    auto iq = siggen::atv_fm_modulate(video, standard, kRate, 3.0e6);
    // Carrier to noise 16 dB in the full 20 MHz.
    REQUIRE(siggen::add_awgn_at_power(iq, std::pow(10.0, -16.0 / 10.0), 77).has_value());
    decode::AtvFmDemodulator fm(kRate);
    std::vector<float> demod;
    fm.push(iq, demod);
    decode::AtvConfig config;
    config.standard = standard;
    config.sample_rate = kRate;
    const auto d = run(config, demod, 1 << 18);
    std::size_t complete = 0;
    const double mae = frame_error(d, pictures, complete);
    WARN("FM, CNR 16 dB in 20 MHz: " << complete << " frames, mean error " << mae
                                     << " grey levels; " << d.seconds << " s for "
                                     << static_cast<double>(demod.size()) / kRate << " s");
    CHECK(complete == pictures.size());
    CHECK(mae < 16.0);
}

TEST_CASE("ATV over negative AM with an echo and noise", "[decode][atv]") {
    constexpr dsp::SampleRate kRate = 10'000'000;
    const auto standard = AtvStandard::M525;
    std::vector<siggen::AtvPicture> pictures{test_picture(standard, 720, 0),
                                             test_picture(standard, 720, 1)};
    const auto video = siggen::atv_composite(standard, kRate, pictures);
    auto iq = siggen::atv_am_modulate(video, standard);
    siggen::MultipathProfile profile;
    profile.taps = {{0.0, 0.0, 0.0}, {1.3e-6, -26.0, 0.7}};
    auto echoed = siggen::apply_multipath(iq, profile, kRate);
    REQUIRE(echoed.has_value());
    REQUIRE(siggen::add_awgn_at_power(*echoed, std::pow(10.0, -30.0 / 10.0), 5).has_value());
    std::vector<float> envelope;
    decode::atv_am_envelope(*echoed, envelope);
    decode::AtvConfig config;
    config.standard = standard;
    config.sample_rate = kRate;
    config.inverted = true;
    const auto d = run(config, envelope, 50'000);
    std::size_t complete = 0;
    const double mae = frame_error(d, pictures, complete);
    WARN("AM, echo -26 dB at 1.3 us, SNR 30 dB: " << complete << " frames, mean error " << mae
                                                  << " grey levels");
    CHECK(complete == pictures.size());
    CHECK(mae < 16.0);
}

TEST_CASE("ATV decodes the same in any blocking", "[decode][atv]") {
    constexpr dsp::SampleRate kRate = 8'000'000;
    const auto standard = AtvStandard::BG625;
    std::vector<siggen::AtvPicture> pictures{test_picture(standard, 360, 0),
                                             test_picture(standard, 360, 3)};
    auto video = siggen::atv_composite(standard, kRate, pictures);
    std::vector<dsp::Complex32> noisy(video.begin(), video.end());
    REQUIRE(siggen::add_awgn_at_power(noisy, 25.0, 3).has_value());
    for (std::size_t i = 0; i < video.size(); ++i) {
        video[i] = noisy[i].real();
    }
    decode::AtvConfig config;
    config.standard = standard;
    config.sample_rate = kRate;
    config.width = 360;
    const auto whole = run(config, video, video.size());
    REQUIRE(!whole.rows.empty());
    for (const std::size_t block : {std::size_t{1}, std::size_t{777}, std::size_t{65536}}) {
        INFO("block " << block);
        const auto split = run(config, video, block);
        REQUIRE(split.rows.size() == whole.rows.size());
        bool same = true;
        for (std::size_t i = 0; i < split.rows.size(); ++i) {
            same = same && split.rows[i].bytes == whole.rows[i].bytes &&
                   split.rows[i].row == whole.rows[i].row &&
                   split.rows[i].image_id == whole.rows[i].image_id;
        }
        CHECK(same);
    }
}
