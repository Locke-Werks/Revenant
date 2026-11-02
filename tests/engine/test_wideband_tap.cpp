// The wideband tap on its own, with no GPU and no engine.
//
// WidebandTap is host code end to end: a ring, a conversion, a mix and a
// decimating FIR on its own thread. Engine::attach_wideband_tap only builds
// one for the open source's rate and format and puts it on the delivery
// chain, so the properties a wideband decoder depends on are checked here
// against blocks built by hand.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <numbers>
#include <thread>
#include <vector>

#include "core/engine/wideband_tap.h"
#include "core/error.h"

using namespace revenant;
using engine::WidebandChunk;
using engine::WidebandTap;
using engine::WidebandTapConfig;

namespace {

struct Collected {
    std::mutex lock;
    std::vector<std::complex<float>> samples;
    std::vector<dsp::SampleIndex> starts;
    std::uint64_t dropped = 0;
    dsp::SampleRate rate = 0;
    dsp::Hertz center = 0;
};

engine::WidebandSink collect_into(Collected& out) {
    return [&out](const WidebandChunk& chunk) -> Status {
        const std::scoped_lock held(out.lock);
        out.starts.push_back(chunk.start);
        out.dropped += chunk.dropped_before;
        out.rate = chunk.rate;
        out.center = chunk.center;
        for (std::size_t k = 0; k + 1 < chunk.iq.size(); k += 2) {
            out.samples.emplace_back(chunk.iq[k], chunk.iq[k + 1]);
        }
        return {};
    };
}

// A complex tone at `freq` Hz, cf32, as the bytes a SourceBlock carries.
std::vector<std::byte> tone_cf32(std::size_t count, double freq, double rate,
                                 std::size_t first, float amplitude = 0.5F) {
    std::vector<std::byte> bytes(count * 8);
    for (std::size_t n = 0; n < count; ++n) {
        const double phase = 2.0 * std::numbers::pi * freq *
                             static_cast<double>(first + n) / rate;
        const float pair[2] = {amplitude * static_cast<float>(std::cos(phase)),
                               amplitude * static_cast<float>(std::sin(phase))};
        std::memcpy(bytes.data() + n * 8, pair, 8);
    }
    return bytes;
}

source::SourceBlock block_of(const std::vector<std::byte>& bytes, std::size_t count,
                             source::SampleFormat format, std::uint64_t sequence) {
    source::SourceBlock block;
    block.format = format;
    block.sample_count = count;
    block.bytes = bytes;
    block.sequence = sequence;
    return block;
}

void wait_for(Collected& out, std::size_t frames) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            const std::scoped_lock held(out.lock);
            if (out.samples.size() >= frames) {
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Power of a complex sequence at `freq` Hz, by direct correlation.
double power_at(const std::vector<std::complex<float>>& x, std::size_t from, double freq,
                double rate) {
    std::complex<double> acc{};
    for (std::size_t n = from; n < x.size(); ++n) {
        const double phase = -2.0 * std::numbers::pi * freq * static_cast<double>(n) / rate;
        acc += std::complex<double>(x[n]) * std::polar(1.0, phase);
    }
    const double len = static_cast<double>(x.size() - from);
    return std::norm(acc / len);
}

}  // namespace

TEST_CASE("a wideband tap refuses rates it cannot deliver", "[engine][wideband]") {
    Collected out;
    WidebandTapConfig config;
    config.output_rate = 700'000;  // does not divide 2.4 MS/s
    auto made = WidebandTap::create(config, 2'400'000, source::SampleFormat::Cu8,
                                    collect_into(out));
    REQUIRE_FALSE(made.has_value());

    config.output_rate = 4'800'000;  // above the source
    REQUIRE_FALSE(WidebandTap::create(config, 2'400'000, source::SampleFormat::Cu8,
                                      collect_into(out))
                      .has_value());

    config.output_rate = 0;
    config.offset = 1'300'000;  // outside the span
    REQUIRE_FALSE(WidebandTap::create(config, 2'400'000, source::SampleFormat::Cu8,
                                      collect_into(out))
                      .has_value());

    config.offset = 0;
    REQUIRE_FALSE(
        WidebandTap::create(config, 2'400'000, source::SampleFormat::Cu8, {}).has_value());
}

TEST_CASE("a wideband tap at the source rate passes cf32 through unchanged",
          "[engine][wideband]") {
    Collected out;
    auto made = WidebandTap::create({}, 2'048'000, source::SampleFormat::Cf32,
                                    collect_into(out));
    REQUIRE(made.has_value());
    auto& tap = **made;
    REQUIRE(tap.decimation() == 1);
    REQUIRE(tap.filter_taps() == 0);

    constexpr std::size_t kBlock = 16384;
    std::vector<std::vector<std::byte>> blocks;
    for (std::size_t b = 0; b < 4; ++b) {
        blocks.push_back(tone_cf32(kBlock, 100'000.0, 2'048'000.0, b * kBlock));
        tap.push(block_of(blocks.back(), kBlock, source::SampleFormat::Cf32, b), 100'000'000);
    }
    wait_for(out, 4 * kBlock);
    REQUIRE(tap.stop().has_value());

    REQUIRE(out.samples.size() == 4 * kBlock);
    REQUIRE(out.rate == 2'048'000);
    REQUIRE(out.center == 100'000'000);
    const auto expected = tone_cf32(4 * kBlock, 100'000.0, 2'048'000.0, 0);
    for (std::size_t n = 0; n < out.samples.size(); n += 997) {
        float pair[2];
        std::memcpy(pair, expected.data() + n * 8, 8);
        REQUIRE(out.samples[n].real() == pair[0]);
        REQUIRE(out.samples[n].imag() == pair[1]);
    }
}

TEST_CASE("a wideband tap mixes the offset to DC and decimates", "[engine][wideband]") {
    // 2.4 MS/s source, wanted signal 300 kHz up, delivered at 240 kS/s. A tone
    // 10 kHz above the wanted centre must come out at +10 kHz, and a second
    // tone 500 kHz away from it, far past the 108 kHz passband edge, must be
    // gone by the filter's stopband.
    Collected out;
    WidebandTapConfig config;
    config.offset = 300'000;
    config.output_rate = 240'000;
    auto made = WidebandTap::create(config, 2'400'000, source::SampleFormat::Cf32,
                                    collect_into(out));
    REQUIRE(made.has_value());
    auto& tap = **made;
    REQUIRE(tap.decimation() == 10);

    constexpr std::size_t kBlock = 24000;
    constexpr std::size_t kBlocks = 10;
    std::vector<std::vector<std::byte>> blocks;
    for (std::size_t b = 0; b < kBlocks; ++b) {
        auto wanted = tone_cf32(kBlock, 310'000.0, 2'400'000.0, b * kBlock, 0.4F);
        const auto unwanted = tone_cf32(kBlock, -200'000.0, 2'400'000.0, b * kBlock, 0.4F);
        for (std::size_t k = 0; k < wanted.size(); k += 4) {
            float a = 0.0F;
            float u = 0.0F;
            std::memcpy(&a, wanted.data() + k, 4);
            std::memcpy(&u, unwanted.data() + k, 4);
            a += u;
            std::memcpy(wanted.data() + k, &a, 4);
        }
        blocks.push_back(std::move(wanted));
        tap.push(block_of(blocks.back(), kBlock, source::SampleFormat::Cf32, b), 0);
    }
    const std::size_t frames = kBlocks * kBlock / 10;
    wait_for(out, frames);
    REQUIRE(tap.stop().has_value());
    REQUIRE(out.samples.size() == frames);

    // Skip the filter's fill.
    const std::size_t settle = tap.filter_taps();
    const double wanted = power_at(out.samples, settle, 10'000.0, 240'000.0);
    const double aliased = power_at(out.samples, settle, -500'000.0 + 240'000.0 * 2, 240'000.0);
    INFO("wanted " << 10.0 * std::log10(wanted) << " dB, alias " << 10.0 * std::log10(aliased)
                   << " dB");
    REQUIRE(wanted == Catch::Approx(0.16).epsilon(0.02));
    REQUIRE(10.0 * std::log10(aliased / wanted) < -70.0);
}

TEST_CASE("a wideband tap widens cu8 with the kernels' numbers", "[engine][wideband]") {
    Collected out;
    auto made = WidebandTap::create({}, 1'024'000, source::SampleFormat::Cu8, collect_into(out));
    REQUIRE(made.has_value());
    std::vector<std::byte> bytes = {std::byte{0}, std::byte{255}, std::byte{128},
                                    std::byte{127}};
    (*made)->push(block_of(bytes, 2, source::SampleFormat::Cu8, 0), 0);
    wait_for(out, 2);
    REQUIRE((*made)->stop().has_value());
    REQUIRE(out.samples.size() == 2);
    REQUIRE(out.samples[0].real() == -1.0F);
    REQUIRE(out.samples[0].imag() == 1.0F);
    REQUIRE(out.samples[1].real() > 0.0F);
    REQUIRE(out.samples[1].imag() < 0.0F);
}

TEST_CASE("a full ring costs the tap its own samples and is counted", "[engine][wideband]") {
    // A sink that blocks until released, so the ring fills behind it.
    std::mutex gate;
    gate.lock();
    std::vector<dsp::SampleIndex> starts;
    std::uint64_t dropped = 0;
    std::mutex seen;
    WidebandTapConfig config;
    config.ring_seconds = 0.01;  // ~10 ms at 1.024 MS/s
    auto made = WidebandTap::create(
        config, 1'024'000, source::SampleFormat::Cf32,
        [&](const WidebandChunk& chunk) -> Status {
            const std::scoped_lock wait(gate);
            const std::scoped_lock held(seen);
            starts.push_back(chunk.start);
            dropped += chunk.dropped_before;
            return {};
        });
    REQUIRE(made.has_value());
    auto& tap = **made;

    constexpr std::size_t kBlock = 4096;
    const auto bytes = tone_cf32(kBlock, 0.0, 1'024'000.0, 0);
    for (std::uint64_t b = 0; b < 64; ++b) {
        tap.push(block_of(bytes, kBlock, source::SampleFormat::Cf32, b), 0);
    }
    REQUIRE(tap.dropped_source_samples() > 0);
    gate.unlock();
    // One more after the release, so the drop is reported on a chunk.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tap.push(block_of(bytes, kBlock, source::SampleFormat::Cf32, 64), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(tap.stop().has_value());

    const std::scoped_lock held(seen);
    REQUIRE(dropped == tap.dropped_source_samples());
    // Starts are strictly increasing and account for the gap.
    for (std::size_t k = 1; k < starts.size(); ++k) {
        REQUIRE(starts[k] > starts[k - 1]);
    }
}

TEST_CASE("a format change stops the tap and says why", "[engine][wideband]") {
    Collected out;
    auto made = WidebandTap::create({}, 1'024'000, source::SampleFormat::Cf32,
                                    collect_into(out));
    REQUIRE(made.has_value());
    std::vector<std::byte> bytes(4);
    (*made)->push(block_of(bytes, 2, source::SampleFormat::Cu8, 0), 0);
    REQUIRE((*made)->failed());
    REQUIRE_FALSE((*made)->stop().has_value());
}
