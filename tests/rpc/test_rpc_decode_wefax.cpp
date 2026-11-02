// A radiofax chart from core/dsp/synth/wefax_mod.h, on upper sideband in a
// file, through the channelizer and a usb receiver, decoded by the "wefax"
// adapter and read back off the socket as image events.
//
// tests/decode/test_wefax.cpp scores the decoder sample buffer to rows. What
// this adds is the receiver and the wire.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/decode/wefax.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/modulators.h"
#include "core/dsp/synth/wefax_mod.h"
#include "core/dsp/types.h"
#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/decoders.h"
#include "core/rpc/types.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/decoded_log.h"
#include "tests/rpc/rpc_fixture.h"
#include "tests/support/temp_path.h"

using namespace revenant;
using test::Harness;
using test::HarnessOptions;

namespace {

// test_rpc_decode_audio.cpp's grid, carrier and HF centre, for its reasons.
constexpr std::uint32_t kGridChannels = 4;
constexpr dsp::SampleRate kFileRate = 288'000;
constexpr std::uint32_t kBlockSamples = 16'384;
constexpr dsp::Hertz kCarrierHz = 5'000;
constexpr dsp::Hertz kHfCentre = 14'000'000;
constexpr int kRunTimeoutMs = 300'000;
constexpr int kLpm = 240;
constexpr std::uint32_t kRows = 32;

std::vector<std::uint8_t> chart(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            out[static_cast<std::size_t>(y) * w + x] =
                (x / 150) % 2 == 0 ? static_cast<std::uint8_t>(255 * x / w) : 230;
        }
    }
    return out;
}

class ImageLog {
public:
    void record(const rpc::DecodedImageEvent& event) {
        const std::lock_guard<std::mutex> held(lock_);
        events_.push_back(event);
    }
    [[nodiscard]] std::vector<rpc::DecodedImageEvent> events() const {
        const std::lock_guard<std::mutex> held(lock_);
        return events_;
    }

private:
    mutable std::mutex lock_;
    std::vector<rpc::DecodedImageEvent> events_;
};

// 240/576 with a shortened phasing, by the phasing method onto upper
// sideband, with noise at 30 dB in 2500 Hz.
Expected<std::vector<dsp::Complex32>> chart_on_usb() {
    siggen::WefaxModConfig config;
    config.rate = static_cast<double>(kFileRate);
    config.amplitude = 1.0;
    config.lpm = kLpm;
    config.phasing_seconds = 14.0;
    auto audio = siggen::wefax_render(config, chart(decode::wefax_width(576), kRows));
    if (!audio) {
        return std::unexpected(audio.error());
    }
    siggen::ModulatorConfig common;
    common.rate = kFileRate;
    common.carrier_offset = kCarrierHz;
    common.amplitude = 0.5;
    siggen::SsbParams params;
    params.audio_low_hz = 1'000;
    params.audio_high_hz = 3'000;
    params.hilbert_taps = 511;
    const std::size_t count = audio->size();
    auto made = siggen::generate_ssb(common, params, true, count, std::move(*audio));
    if (!made) {
        return std::unexpected(made.error());
    }
    std::vector<dsp::Complex32> signal = std::move(made->samples);
    auto noise = siggen::awgn_power_for(siggen::NoiseLevel::snr_in_2500_hz_db(30.0),
                                        siggen::mean_power(signal), kFileRate);
    if (!noise) {
        return std::unexpected(noise.error());
    }
    if (auto added = siggen::add_awgn_at_power(signal, *noise, 0xFA4); !added) {
        return std::unexpected(added.error());
    }
    return signal;
}

}  // namespace

TEST_CASE("a radiofax chart from a usb receiver crosses the wire row by row",
          "[gpu][rpc][decode][image][wefax]") {
    REVENANT_NEEDS_GPU();

    auto signal = chart_on_usb();
    INFO(test::message_of(signal));
    REQUIRE(signal.has_value());

    const std::filesystem::path path = test::unique_temp_path("revenant-decode-wefax", ".cf32");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(signal->data()),
                  static_cast<std::streamsize>(signal->size() * sizeof(dsp::Complex32)));
        REQUIRE(out.good());
    }
    struct Remove {
        std::filesystem::path p;
        ~Remove() {
            std::error_code ignored;
            std::filesystem::remove(p, ignored);
        }
    } remove{path};

    Harness harness;
    HarnessOptions options;
    options.source_uri = std::format("file:///{}?rate={}&format=cf32&center={}",
                                     path.generic_string(), kFileRate, kHfCentre);
    options.channels = kGridChannels;
    options.block_samples = kBlockSamples;
    options.samples = signal->size();
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    auto vrx = harness.client().add_vrx(
        rpc::VrxParams{.center = kCarrierHz, .bandwidth = 0, .demod = rpc::Demod::Usb});
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto messages = std::make_shared<test::MessageLog>();
    auto images = std::make_shared<ImageLog>();
    auto resolved = harness.client().subscribe_decoded(
        *vrx, "wefax", test::into(messages), test::ending(messages),
        [images](const rpc::DecodedImageEvent& event) { images->record(event); });
    INFO(test::message_of(resolved));
    REQUIRE(resolved.has_value());

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());
    const std::uint64_t blocks = (signal->size() + kBlockSamples - 1) / kBlockSamples;
    const std::uint64_t seen_blocks = harness.wait_for_blocks(blocks, kRunTimeoutMs);
    INFO(std::format("{} blocks delivered of {}", seen_blocks, blocks));
    CHECK(seen_blocks >= blocks);

    const auto closed = [](const std::vector<rpc::DecodedImageEvent>& seen) {
        return std::ranges::any_of(seen, [](const rpc::DecodedImageEvent& event) {
            return event.kind == rpc::DecodedImageEvent::Kind::Close;
        });
    };
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(test::kDecodedWaitMs);
    std::vector<rpc::DecodedImageEvent> seen = images->events();
    while (!closed(seen) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        seen = images->events();
    }
    static_cast<void>(harness.stop_engine());

    const std::uint32_t width = decode::wefax_width(576);
    REQUIRE(seen.size() >= 2);
    const rpc::DecodedImageEvent& open = seen.front();
    CHECK(open.kind == rpc::DecodedImageEvent::Kind::Open);
    CHECK(open.decoder == "wefax");
    CHECK(open.mode == "WEFAX 240/576");
    CHECK(open.width == width);
    CHECK(open.colour == rpc::ImageColour::Gray8);
    CHECK(std::abs(open.line_rate_hz - kLpm / 60.0) < 1.0e-9);

    const auto want = chart(width, kRows);
    double err = 0.0;
    std::size_t count = 0;
    std::uint32_t next = 0;
    bool in_order = true;
    for (const rpc::DecodedImageEvent& event : seen) {
        if (event.kind != rpc::DecodedImageEvent::Kind::Row) {
            continue;
        }
        in_order = in_order && event.row == next;
        ++next;
        REQUIRE(event.pixels.size() == width);
        for (std::size_t i = 0; i < width; ++i) {
            const double d = static_cast<double>(event.pixels[i]) -
                             want[static_cast<std::size_t>(event.row) * width + i];
            err += d * d;
            ++count;
        }
    }
    CHECK(in_order);
    CHECK(next == kRows);
    REQUIRE(count > 0);
    const double psnr = 10.0 * std::log10(255.0 * 255.0 * static_cast<double>(count) / err);
    WARN(std::format("240/576 over usb at 30 dB/2500 Hz: {} rows, PSNR {:.1f} dB", next, psnr));
    CHECK(psnr > 20.0);

    const rpc::DecodedImageEvent& close = seen.back();
    CHECK(close.kind == rpc::DecodedImageEvent::Kind::Close);
    CHECK(close.complete);
    CHECK(messages->messages().empty());
}
