// How deep the channel ring has to be for a receiver's display tap and for its
// fine filter.
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
//
// THE FINE FILTER HAD THE SAME DEFECT AND A WIDER ONE. It reaches up to
// dsp::kMaxFineTaps channel samples below its block, the stage checked
// `blocks + taps` against the ring, and the graph did not count the filter at
// all: with no spectrum or passband stage the ring was bit_ceil((F + 1) * B),
// which covers the reach only when a dispatch is at least 256 blocks. At 128
// blocks a dispatch and three frames that is 512 blocks where the 252 tap
// filter of a 3 kHz AM receiver needs 636, and the old check took it. Every shipped grid runs 2048 blocks or
// more a dispatch, so this is a small-block configuration, which the engine
// allows down to one block and revenant-engine down to 256 source samples.
//
// Three cases for it. The first builds the stage directly on the 512 block
// ring and is the one that failed before the fix. The second is the kernel's
// host twin, core/dsp/vrx_reference.h's reference_vrx_fine, run over that ring
// with the two later frames' channel samples written before the oldest frame's
// fine stage reads, which is an order frames in flight permit on the device:
// it shows what the masked read returns then. The third runs the engine on the
// small-block grid with no spectrum or passband stage, which after the stage's
// fix alone refused the receiver outright because the graph still gave it 512.
//
// WHAT THE TWIN SHOWED, 2026-09-23. With the later frames written, 79 of the
// dispatch's 82 fine outputs changed on the 512 block ring and the error was
// 5.574e-03 of the output's power, -22.5 dB, from white noise that fills the
// channel; on 1024 blocks none changed. Whether the device ever runs the
// frames in that order was not measured, and the display tap's engine case
// above never caught it doing so. The first case failed before the fix and the
// third passed, since the old graph and the old check agreed with each other
// on 512.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
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

// The fine filter's cases. 4096 source samples at D = 32 is 128 channel
// blocks a dispatch, where the old graph rule gives bit_ceil(4 * 128) = 512
// and the old stage check wants 128 + 256 = 384, so it took the ring.
constexpr std::size_t kFineBlockSamples = 4'096;
constexpr std::uint32_t kFineBlocksPerDispatch = kFineBlockSamples / kDecimation;
constexpr std::uint32_t kFineOldRing = 512;

// A 3 kHz AM receiver on a 75 kS/s channel, whose fine filter plans at 252
// taps: more than the 128 that 512 - 3 * 128 leaves and within the 384 the old
// check allowed. The cases check both rather than assuming the length.
engine::VrxParams narrow_am_at(dsp::Hertz centre) {
    engine::VrxParams params;
    params.center = centre;
    params.bandwidth = 3'000;
    params.demod = engine::Demod::Am;
    params.audio_rate = kAudioRate;
    params.squelch_dbfs = -300.0;
    return params;
}

dsp::GridParams test_grid() {
    dsp::GridParams grid;
    grid.channels = kChannels;
    grid.taps_per_branch = 17;
    grid.decimation = kDecimation;
    return grid;
}

// Builds one receiver's stage against a channel ring `ring_blocks` deep, the
// way Graph::add_vrx would, and hands back whatever the factory said.
Expected<std::unique_ptr<engine::VrxStage>> build_stage(
    const gpu::Context& context, std::uint32_t ring_blocks, const gpu::Buffer& channel_ring,
    const engine::VrxParams& params = nfm_at(0),
    std::uint32_t blocks_per_dispatch = kBlocksPerDispatch,
    std::uint32_t passband_transform = kPassbandTransform) {
    engine::install_default_vrx_stages();
    const engine::VrxStageFactory factory = engine::installed_vrx_stage_factory();
    if (factory == nullptr) {
        return fail("no stage factory is installed");
    }

    const dsp::GridParams grid = test_grid();
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
    request.max_blocks_per_dispatch = blocks_per_dispatch;
    request.frames_in_flight = kFramesInFlight;
    request.audio_rate = kAudioRate;
    request.passband_transform = passband_transform;
    return factory(request);
}

// The fine output the stage would take last for a dispatch whose newest
// channel sample is `newest`: floor((newest + 1) * Fd / Fc) - 1, the same
// arithmetic as highest_output_for in core/engine/vrx_stage.cpp, which is
// file-local there.
dsp::SampleIndex highest_output_for(dsp::SampleIndex newest, dsp::SampleRate channel_rate,
                                    dsp::SampleRate demod_rate) {
    const auto fc = static_cast<dsp::SampleIndex>(channel_rate);
    const auto fd = static_cast<dsp::SampleIndex>(demod_rate);
    const dsp::SampleIndex bound = newest + 1U;
    const dsp::SampleIndex whole = bound / fc;
    const dsp::SampleIndex part = bound % fc;
    if (part == 0) {
        return whole * fd - 1U;
    }
    return whole * fd + (part * fd - 1U) / fc;
}

// White complex noise that is a pure function of the channel sample's
// absolute index, so the samples a later frame writes differ from the history
// they land on and any read of one in place of the other shows.
dsp::Complex32 noise_at(dsp::SampleIndex n) {
    std::uint64_t z = n + 0x9E37'79B9'7F4A'7C15ULL;
    z = (z ^ (z >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D0'49BB'1331'11EBULL;
    z ^= z >> 31U;
    const double re = static_cast<double>(z >> 40U) / 16'777'216.0 - 0.5;
    const double im = static_cast<double>(z & 0xFF'FFFFU) / 16'777'216.0 - 0.5;
    return {static_cast<float>(re), static_cast<float>(im)};
}

// One dispatch of the fine stage through its host twin, over one channel's
// ring `ring_blocks` deep, for the frame whose first channel block is
// `first_block`. The ring holds everything up to that frame's newest sample.
// With `later_frames` it also holds what the next frames_in_flight - 1 frames'
// channelizers write, which is what the device may have done by the time this
// frame's fine stage runs.
Expected<std::vector<dsp::Complex32>> twin_fine(const dsp::VrxPlan& plan,
                                                const std::vector<dsp::Complex32>& nco,
                                                std::uint32_t ring_blocks,
                                                dsp::SampleIndex first_block,
                                                bool later_frames) {
    const dsp::SampleIndex newest = first_block + kFineBlocksPerDispatch - 1U;
    const dsp::SampleIndex first_output =
        highest_output_for(first_block - 1U, plan.channel_rate, plan.demod_rate) + 1U;
    const dsp::SampleIndex last_output =
        highest_output_for(newest, plan.channel_rate, plan.demod_rate);
    const auto count = static_cast<std::uint32_t>(last_output + 1U - first_output);
    const std::uint32_t fine_capacity = std::bit_ceil(count);

    auto block = dsp::fine_block(plan, 0, ring_blocks - 1U, fine_capacity - 1U, first_output,
                                 count);
    if (!block) {
        return std::unexpected(block.error());
    }
    if (block->newest_input > newest) {
        return fail(std::format("the dispatch reads channel sample {} and the frame ends at {}",
                                block->newest_input, newest));
    }

    const dsp::SampleIndex mask = ring_blocks - 1U;
    std::vector<dsp::Complex32> ring(ring_blocks);
    for (dsp::SampleIndex n = newest + 1U - ring_blocks; n <= newest; ++n) {
        ring[n & mask] = noise_at(n);
    }
    if (later_frames) {
        const dsp::SampleIndex end =
            newest + 1U + static_cast<dsp::SampleIndex>(kFineBlocksPerDispatch) *
                              (kFramesInFlight - 1U);
        for (dsp::SampleIndex n = newest + 1U; n < end; ++n) {
            ring[n & mask] = noise_at(n);
        }
    }

    std::vector<dsp::Complex32> fine(fine_capacity);
    if (auto ran = dsp::reference_vrx_fine(plan.fine, block->params, ring, plan.fine_taps, nco,
                                           fine);
        !ran) {
        return std::unexpected(ran.error());
    }

    std::vector<dsp::Complex32> out(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        out[i] = fine[(block->params.out_offset + i) & (fine_capacity - 1U)];
    }
    return out;
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

TEST_CASE("a fine filter refuses a channel ring that later frames would overwrite under it",
          "[gpu][engine][channel-ring]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());
    const gpu::Context& context = test::shared_context();

    const engine::VrxParams params = narrow_am_at(0);
    auto placement = engine::place(test_grid(), kSourceRate, params);
    INFO(test::message_of(placement));
    REQUIRE(placement.has_value());
    auto plan = dsp::plan_vrx(test_grid(), kSourceRate, params, *placement);
    INFO(test::message_of(plan));
    REQUIRE(plan.has_value());
    const std::uint32_t taps = plan->fine.taps;
    INFO("a " << taps << " tap fine filter");

    // What the old rules gave and took, and what the filter needs.
    static_assert(std::bit_ceil(kFineBlocksPerDispatch * (kFramesInFlight + 1U)) == kFineOldRing,
                  "the old graph rule has to give this ring with no spectrum or passband stage");
    REQUIRE(kFineBlocksPerDispatch + taps <= kFineOldRing);
    REQUIRE(kFineBlocksPerDispatch * kFramesInFlight + taps > kFineOldRing);

    const auto ring_of = [&](std::uint32_t blocks) {
        return gpu::Buffer::create(
            context, static_cast<VkDeviceSize>(kChannels) * blocks * kComplexBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            gpu::MemoryKind::DeviceLocal);
    };

    // No passband stage, so the only thing that can refuse is the fine
    // filter's own check.
    auto small = ring_of(kFineOldRing);
    INFO(test::message_of(small));
    REQUIRE(small.has_value());
    auto refused = build_stage(context, kFineOldRing, *small, params, kFineBlocksPerDispatch, 0);
    INFO("a " << kFineOldRing << " block ring: " << test::message_of(refused));
    CHECK_FALSE(refused.has_value());
    if (!refused) {
        CHECK(refused.error().message.find("fine filter") != std::string::npos);
        CHECK(refused.error().message.find("frames in flight") != std::string::npos);
    }

    // The ring the graph now gives this grid, bit_ceil(3 * 128 + 256), is
    // taken.
    constexpr std::uint32_t kEnough =
        std::bit_ceil(kFineBlocksPerDispatch * kFramesInFlight + dsp::kMaxFineTaps);
    static_assert(kEnough == 1024);
    auto large = ring_of(kEnough);
    INFO(test::message_of(large));
    REQUIRE(large.has_value());
    auto taken = build_stage(context, kEnough, *large, params, kFineBlocksPerDispatch, 0);
    INFO("a " << kEnough << " block ring: " << test::message_of(taken));
    CHECK(taken.has_value());
}

TEST_CASE("the fine filter's twin reads a later frame's samples on the ring the old rule gave",
          "[engine][channel-ring]") {
    const engine::VrxParams params = narrow_am_at(0);
    auto placement = engine::place(test_grid(), kSourceRate, params);
    INFO(test::message_of(placement));
    REQUIRE(placement.has_value());
    auto plan = dsp::plan_vrx(test_grid(), kSourceRate, params, *placement);
    INFO(test::message_of(plan));
    REQUIRE(plan.has_value());
    REQUIRE(kFineBlocksPerDispatch + plan->fine.taps <= kFineOldRing);
    REQUIRE(kFineBlocksPerDispatch * kFramesInFlight + plan->fine.taps > kFineOldRing);

    auto nco = dsp::build_nco_table(plan->fine.nco_log2);
    INFO(test::message_of(nco));
    REQUIRE(nco.has_value());

    // Far enough in that every index the dispatch or the fill touches is
    // positive, and otherwise arbitrary.
    constexpr dsp::SampleIndex kFirstBlock = dsp::SampleIndex{1'000} * kFineBlocksPerDispatch;

    const auto run = [&](std::uint32_t ring_blocks, bool later_frames) {
        auto out = twin_fine(*plan, *nco, ring_blocks, kFirstBlock, later_frames);
        INFO("a " << ring_blocks << " block ring: " << test::message_of(out));
        REQUIRE(out.has_value());
        return std::move(*out);
    };

    // What the filter should produce: nothing written above the frame yet.
    const std::vector<dsp::Complex32> clean = run(1024, false);
    REQUIRE(!clean.empty());
    double power = 0.0;
    for (const dsp::Complex32 value : clean) {
        power += std::norm(value);
    }
    REQUIRE(power > 0.0);

    const auto departure = [&](const std::vector<dsp::Complex32>& other, std::size_t& moved) {
        REQUIRE(other.size() == clean.size());
        double error = 0.0;
        moved = 0;
        for (std::size_t i = 0; i < clean.size(); ++i) {
            error += std::norm(other[i] - clean[i]);
            if (other[i] != clean[i]) {
                ++moved;
            }
        }
        return error / power;
    };

    // On the old ring the two later frames' samples wrap onto the oldest
    // history this frame's filter is still reading.
    std::size_t old_moved = 0;
    const double old_error = departure(run(kFineOldRing, true), old_moved);
    // And the same ring with nothing written above: identical, so the
    // difference above is the later frames and nothing else.
    std::size_t quiet_moved = 0;
    const double quiet_error = departure(run(kFineOldRing, false), quiet_moved);
    // On the ring the fix gives, the later frames land below everything read.
    std::size_t new_moved = 0;
    const double new_error = departure(run(1024, true), new_moved);

    WARN(std::format("{} fine outputs from a {} tap filter over {} blocks. On {} blocks with the "
                     "later frames written, {} of them moved and the error was {:.3e} of the "
                     "output's power ({:.1f} dB); on {} blocks, {} moved",
                     clean.size(), plan->fine.taps, kFineBlocksPerDispatch, kFineOldRing,
                     old_moved, old_error, 10.0 * std::log10(old_error), 1024, new_moved));
    CHECK(quiet_moved == 0);
    CHECK(quiet_error == 0.0);
    CHECK(old_moved > 0);
    CHECK(old_error > 1e-3);
    CHECK(new_moved == 0);
    CHECK(new_error == 0.0);
}

TEST_CASE("a small-block grid with no spectrum or passband stage takes a narrow receiver",
          "[gpu][engine][channel-ring]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // Channel 3's centre, and an AM emitter the synthetic scene places inside
    // a 3.2 kHz span around it.
    constexpr dsp::Hertz kReceiver = 3 * (kSourceRate / static_cast<dsp::Hertz>(kChannels));
    constexpr dsp::SampleIndex kSamples = 600'000;

    engine::EngineConfig config;
    config.channels = kChannels;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = kFineBlockSamples;
    config.audio_rate = kAudioRate;
    config.spectrum_transform = 0;
    config.passband_transform = 0;
    config.gpu_index = -1;

    std::mutex lock;
    std::size_t audio_samples = 0;

    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;

    const auto opened = eng.open_source(
        "synthetic:wideband?rate=" + std::to_string(kSourceRate) +
        "&emitters=1&modes=am&seed=424242&noise_dbfs=-120&snr_min=60&snr_max=60" +
        "&samples=" + std::to_string(kSamples) + "&span_low=" + std::to_string(kReceiver - 1'600) +
        "&span_high=" + std::to_string(kReceiver + 1'600));
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    // Refused with "frames in flight" in the message when the graph sized the
    // ring without counting the fine filter, because it gave 512 blocks.
    auto added = eng.add_vrx(narrow_am_at(kReceiver));
    INFO(test::message_of(added));
    REQUIRE(added.has_value());

    REQUIRE(eng.set_audio_sink(*added,
                               [&](const engine::AudioChunk& chunk) -> Status {
                                   const std::lock_guard<std::mutex> guard(lock);
                                   audio_samples += chunk.samples.size();
                                   return {};
                               })
                .has_value());

    const Status ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    // 600000 samples at 2.4 MS/s is 250 ms, 12000 samples of 48 kHz audio
    // less the filters' fill.
    const std::lock_guard<std::mutex> guard(lock);
    INFO(audio_samples << " audio samples");
    CHECK(audio_samples > 6'000);
}
