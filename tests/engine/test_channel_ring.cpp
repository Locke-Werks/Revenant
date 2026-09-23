// How deep the channel ring has to be for a receiver's display tap.
//
// WHAT THIS FILE IS THE RECORD OF
//
// Every frame in flight writes its own range of the channel ring, and frames
// overlap on the device, so a reader in the oldest frame in flight has the
// writes of every frame behind it landing above it. core/engine/graph.cpp says
// so in its header and sizes the spectrum's window by it. The display tap, one
// receiver's second fine stage feeding its passband pane, reaches
// dsp::kDisplayTaps channel samples below its own block, and until 2026-09-23
// both the graph's sizing rule and the stage's own check counted one dispatch
// above that reach and not frames_in_flight of them.
//
// With 100 blocks a dispatch and three frames in flight that is a ring of 512
// blocks, from max(4 * 100, 100 + 256), where the tap needs 3 * 100 + 256 =
// 556. A spectrum stage of 256 points or more makes the ring larger for its own
// reasons and hides it, so the configuration below has none.
//
// WHAT THE SECOND CASE DID NOT SEE. Under the old sizing it passed: on the
// RTX 4090, 359 frames of the tone moved at worst 1.583e-07 of the reference
// frame's power and none moved more than 1e-3, so no torn pane was observed on
// this device. The hazard is in the arithmetic rather than in a measurement
// here, which is why the first case is the one that failed before the fix and
// the second is the check that the configuration still runs, and still reads
// clean, with the ring the fix makes.
//
// Two cases. The first builds the stage directly, the way
// tests/engine/test_vrx_reanchor.cpp does, and asks whether it takes a ring
// that frames in flight would overwrite under it. The second runs the engine on
// that configuration with a stationary tone, whose pane should come out the
// same frame after frame.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <mutex>
#include <numbers>
#include <string>
#include <system_error>
#include <vector>

#include <vulkan/vulkan.h>

#include "core/dsp/pfb.h"
#include "core/dsp/vrx_reference.h"
#include "core/engine/engine.h"
#include "core/engine/graph.h"
#include "core/engine/vrx.h"
#include "core/engine/vrx_stage.h"
#include "core/gpu/buffer.h"
#include "core/gpu/context.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/support/temp_path.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kSourceRate = 2'400'000;
constexpr std::uint32_t kChannels = 64;
constexpr std::uint32_t kDecimation = 32;
constexpr dsp::SampleRate kChannelRate = kSourceRate / kDecimation;
constexpr dsp::SampleRate kAudioRate = 48'000;

// 3200 source samples at D = 32 is 100 channel blocks a dispatch, and
// GraphConfig's default is three frames in flight.
constexpr std::size_t kBlockSamples = 3'200;
constexpr std::uint32_t kBlocksPerDispatch = kBlockSamples / kDecimation;
constexpr std::uint32_t kFramesInFlight = 3;
constexpr std::uint32_t kPassbandTransform = 512;

constexpr VkDeviceSize kComplexBytes = 8;

engine::VrxParams nfm_at(dsp::Hertz centre) {
    engine::VrxParams params;
    params.center = centre;
    params.bandwidth = 12'500;
    params.demod = engine::Demod::Nfm;
    params.audio_rate = kAudioRate;
    params.squelch_dbfs = -300.0;
    return params;
}

// Builds one receiver's stage against a channel ring `ring_blocks` deep, the
// way Graph::add_vrx would, and hands back whatever the factory said.
Expected<std::unique_ptr<engine::VrxStage>> build_stage(const gpu::Context& context,
                                                        std::uint32_t ring_blocks,
                                                        const gpu::Buffer& channel_ring) {
    engine::install_default_vrx_stages();
    const engine::VrxStageFactory factory = engine::installed_vrx_stage_factory();
    if (factory == nullptr) {
        return fail("no stage factory is installed");
    }

    dsp::GridParams grid;
    grid.channels = kChannels;
    grid.taps_per_branch = 17;
    grid.decimation = kDecimation;

    const engine::VrxParams params = nfm_at(0);
    auto placement = engine::place(grid, kSourceRate, params);
    if (!placement) {
        return std::unexpected(placement.error());
    }

    engine::VrxStageRequest request;
    request.context = &context;
    request.id = engine::VrxId{1};
    request.params = params;
    request.placement = *placement;
    request.grid = grid;
    request.source_rate = kSourceRate;
    request.channel_rate = kChannelRate;
    request.channel_ring = channel_ring.handle();
    request.channel_ring_bytes = channel_ring.size();
    request.channel_ring_blocks = ring_blocks;
    request.channel_ring_mask = ring_blocks - 1;
    request.max_blocks_per_dispatch = kBlocksPerDispatch;
    request.frames_in_flight = kFramesInFlight;
    request.audio_rate = kAudioRate;
    request.passband_transform = kPassbandTransform;
    return factory(request);
}

}  // namespace

TEST_CASE("a display tap refuses a channel ring that later frames would overwrite under it",
          "[gpu][engine][passband][channel-ring]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());
    const gpu::Context& context = test::shared_context();

    // What the old rule gave this configuration, and what the tap needs.
    constexpr std::uint32_t kOldRing = 512;
    constexpr std::uint32_t kNeeded = kBlocksPerDispatch * kFramesInFlight + dsp::kDisplayTaps;
    static_assert(kBlocksPerDispatch + dsp::kDisplayTaps <= kOldRing,
                  "the old check has to accept this ring for the case to mean anything");
    static_assert(kNeeded > kOldRing);

    const auto ring_of = [&](std::uint32_t blocks) {
        return gpu::Buffer::create(
            context, static_cast<VkDeviceSize>(kChannels) * blocks * kComplexBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            gpu::MemoryKind::DeviceLocal);
    };

    auto small = ring_of(kOldRing);
    INFO(test::message_of(small));
    REQUIRE(small.has_value());
    auto refused = build_stage(context, kOldRing, *small);
    INFO("a " << kOldRing << " block ring: " << test::message_of(refused));
    CHECK_FALSE(refused.has_value());
    if (!refused) {
        CHECK(refused.error().message.find("frames in flight") != std::string::npos);
    }

    // And the control: the power of two above what it needs is taken.
    constexpr std::uint32_t kEnough = 1024;
    static_assert(kEnough >= kNeeded);
    auto large = ring_of(kEnough);
    INFO(test::message_of(large));
    REQUIRE(large.has_value());
    auto taken = build_stage(context, kEnough, *large);
    INFO("a " << kEnough << " block ring: " << test::message_of(taken));
    CHECK(taken.has_value());
}

TEST_CASE("a stationary tone's passband pane is the same frame after frame with no spectrum stage",
          "[gpu][engine][passband][channel-ring]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // One tone and nothing else, 2 kHz above a receiver on channel 3's centre.
    // A complex tone's windowed power spectrum does not depend on its phase,
    // so every frame of it is the same frame; one built from history a later
    // frame had already overwritten splices two stretches of the tone and
    // leaks energy across the pane.
    constexpr dsp::Hertz kReceiver = 3 * (kSourceRate / static_cast<dsp::Hertz>(kChannels));
    constexpr dsp::Hertz kTone = kReceiver + 2'000;
    constexpr double kAmplitude = 0.1;
    constexpr std::size_t kSamples = 1'200'000;

    const std::filesystem::path path = test::unique_temp_path("revenant_test_channel_ring", ".cf32");
    {
        std::FILE* file = std::fopen(path.string().c_str(), "wb");
        REQUIRE(file != nullptr);
        std::vector<dsp::Complex32> chunk(65'536);
        for (std::size_t done = 0; done < kSamples; done += chunk.size()) {
            const std::size_t count = std::min(chunk.size(), kSamples - done);
            for (std::size_t i = 0; i < count; ++i) {
                const auto n = static_cast<double>(done + i);
                const double turns = std::fmod(
                    static_cast<double>(kTone) * n / static_cast<double>(kSourceRate), 1.0);
                chunk[i] = dsp::Complex32{
                    static_cast<float>(kAmplitude * std::cos(2.0 * std::numbers::pi * turns)),
                    static_cast<float>(kAmplitude * std::sin(2.0 * std::numbers::pi * turns))};
            }
            REQUIRE(std::fwrite(chunk.data(), sizeof(dsp::Complex32), count, file) == count);
        }
        std::fclose(file);
    }
    struct Remove {
        std::filesystem::path path;
        ~Remove() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } remove{path};

    // Declared before the engine so it outlives the sinks that hold it.
    std::mutex lock;
    std::vector<std::vector<double>> frames;

    engine::EngineConfig config;
    config.channels = kChannels;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = kBlockSamples;
    config.audio_rate = kAudioRate;
    config.passband_transform = kPassbandTransform;
    config.spectrum_transform = 0;
    config.gpu_index = -1;

    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;

    const std::string uri = "file:///" + path.generic_string() +
                            "?rate=" + std::to_string(kSourceRate) + "&format=cf32";
    const auto opened = eng.open_source(uri);
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    auto added = eng.add_vrx(nfm_at(kReceiver));
    INFO(test::message_of(added));
    REQUIRE(added.has_value());

    REQUIRE(eng.set_passband_sink(*added,
                                  [&](const engine::PassbandFrame& frame) -> Status {
                                      std::vector<double> linear(frame.power_db.size());
                                      for (std::size_t bin = 0; bin < linear.size(); ++bin) {
                                          linear[bin] = std::pow(
                                              10.0,
                                              static_cast<double>(frame.power_db[bin]) / 10.0);
                                      }
                                      const std::lock_guard<std::mutex> guard(lock);
                                      frames.push_back(std::move(linear));
                                      return {};
                                  })
                .has_value());

    const Status ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    // The first few frames are the filters filling, so the reference is a
    // frame from the middle and the comparison starts past the warm-up.
    constexpr std::size_t kWarmUp = 8;
    REQUIRE(frames.size() > 4 * kWarmUp);
    const std::vector<double>& reference = frames[frames.size() / 2];
    double total = 0.0;
    for (const double value : reference) {
        total += value;
    }
    REQUIRE(total > 0.0);

    // Each frame's departure from the reference, as the power it moved
    // against the power there is. Float rounding in a transform of a tone
    // whose phase differs frame to frame is parts in a million of this.
    double worst = 0.0;
    std::size_t worst_frame = 0;
    std::size_t torn = 0;
    constexpr double kTornAbove = 1e-3;
    for (std::size_t index = kWarmUp; index < frames.size(); ++index) {
        const std::vector<double>& frame = frames[index];
        REQUIRE(frame.size() == reference.size());
        double moved = 0.0;
        for (std::size_t bin = 0; bin < frame.size(); ++bin) {
            moved += std::abs(frame[bin] - reference[bin]);
        }
        const double share = moved / total;
        if (share > worst) {
            worst = share;
            worst_frame = index;
        }
        if (share > kTornAbove) {
            ++torn;
        }
    }
    WARN(std::format("{} frames compared; the worst, frame {}, moved {:.3e} of the reference's "
                     "power; {} moved more than {:.0e}",
                     frames.size() - kWarmUp, worst_frame, worst, torn, kTornAbove));
    CHECK(torn == 0);
}
