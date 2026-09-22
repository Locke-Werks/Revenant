// The receiver that skips, and the counter that says so.
//
// WHAT THIS FILE IS THE RECORD OF
//
// A receiver whose channel samples are overwritten before its fine stage can
// filter them cannot produce them, so DemodStage::record restarts the receiver
// from the oldest block still in the ring and returns having recorded nothing.
// That is the right thing to do, and for a long time it was also silent: the
// usual cause is a device retune, which declares its gap through
// SourceStats::samples_lost, but a machine that cannot keep up with its own
// channelizer reaches the same branch with no source counter moving at all.
// The audio goes choppy and every number in the engine reads clean.
//
// VrxStatus::reanchors and VrxStatus::reanchor_frames_skipped are what closes
// that, and this is where they are refereed.
//
// WHY THIS DRIVES THE STAGE AND NOT AN ENGINE
//
// Reaching the branch through Engine needs a source that declares a gap, and
// the two that do not need hardware cannot: the synthetic scene has no retune
// and no overrun, and a file source is Demand, so it blocks rather than
// dropping. The only engine-level path to it is a dongle retune, which is in
// tests/engine/test_engine.cpp behind [device] and cannot run on a build
// machine.
//
// The branch itself is arithmetic over three cursors, and the stage is the only
// thing that holds them. So the stage is what is driven here: a real
// DemodStage, built by the installed factory on a real device, handed a run of
// dispatches and then a first_block that has jumped further than the channel
// ring is long. That is exactly the shape a retune's gap presents to it, and it
// needs no radio.
//
// Nothing is submitted. A re-anchor is decided on the host while the command
// buffer is being written, so the counters are answered before any dispatch
// would have run, and a test that submitted would be checking the kernels
// instead of the decision. tests/reference/test_vrx.cpp covers the kernels.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "core/dsp/pfb.h"
#include "core/engine/graph.h"
#include "core/engine/vrx.h"
#include "core/engine/vrx_stage.h"
#include "core/gpu/buffer.h"
#include "core/gpu/context.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kSourceRate = 2'400'000;
constexpr std::uint32_t kChannels = 64;
constexpr std::uint32_t kDecimation = 32;
constexpr dsp::SampleRate kChannelRate = kSourceRate / kDecimation;
constexpr dsp::SampleRate kAudioRate = 48'000;

// Blocks one dispatch presents, and the per-channel depth of the ring behind
// it. Both far smaller than the engine's own, because what matters here is
// only that a jump can clear the ring: a 2048 block ring is 2048 channel
// samples of history, so a jump of eight times that is unambiguously past it
// whatever the fine filter's own reach turns out to be.
constexpr std::uint32_t kBlocksPerDispatch = 64;
constexpr std::uint32_t kRingBlocks = 2048;
constexpr std::uint32_t kFramesInFlight = 2;

// The gap, in channel samples. Eight rings, so the fine filter's tap count is
// a couple of percent of it rather than a term the expected frame count has to
// carry.
constexpr dsp::SampleIndex kGapBlocks = kRingBlocks * 8;

constexpr VkDeviceSize kComplexBytes = 8;

// A command pool and one buffer, because record() writes commands and has
// nowhere to put them otherwise.
//
// Destroys the pool in its destructor and before any buffer the commands name
// goes away, which is the whole reason it is a type and not four calls in the
// case: a command buffer still holding vkCmdCopyBuffer against a freed buffer
// is a validation error on a path that is otherwise about arithmetic.
class Recorder {
public:
    [[nodiscard]] Status open(const gpu::Context& context) {
        device_ = context.device();

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                          VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = context.compute_family();
        if (VkResult result = vkCreateCommandPool(device_, &pool_info, nullptr, &pool_);
            result != VK_SUCCESS) {
            return fail("vkCreateCommandPool failed", result);
        }

        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        if (VkResult result = vkAllocateCommandBuffers(device_, &alloc, &commands_);
            result != VK_SUCCESS) {
            return fail("vkAllocateCommandBuffers failed", result);
        }
        return {};
    }

    ~Recorder() {
        if (pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, pool_, nullptr);
        }
    }

    Recorder() = default;
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // A fresh recording each time. The buffer is never submitted, so resetting
    // it is free and keeps every dispatch's commands from piling up.
    [[nodiscard]] Status begin() {
        if (VkResult result = vkResetCommandBuffer(commands_, 0); result != VK_SUCCESS) {
            return fail("vkResetCommandBuffer failed", result);
        }
        VkCommandBufferBeginInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (VkResult result = vkBeginCommandBuffer(commands_, &info); result != VK_SUCCESS) {
            return fail("vkBeginCommandBuffer failed", result);
        }
        return {};
    }

    [[nodiscard]] Status end() {
        if (VkResult result = vkEndCommandBuffer(commands_); result != VK_SUCCESS) {
            return fail("vkEndCommandBuffer failed", result);
        }
        return {};
    }

    [[nodiscard]] VkCommandBuffer handle() const { return commands_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer commands_ = VK_NULL_HANDLE;
};

dsp::GridParams test_grid() {
    dsp::GridParams grid;
    grid.channels = kChannels;
    grid.taps_per_branch = 17;
    grid.decimation = kDecimation;
    return grid;
}

// An NFM receiver on the grid's DC, which is the narrowest thing to set up and
// the one every other engine case uses. The mode does not matter to a cursor.
engine::VrxParams test_params() {
    engine::VrxParams params;
    params.center = 0;
    params.bandwidth = 16'000;
    params.demod = engine::Demod::Nfm;
    params.audio_rate = kAudioRate;
    return params;
}

}  // namespace

TEST_CASE("a receiver whose inputs were overwritten counts the restart and the frames it lost",
          "[gpu][engine][vrx][m2]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    const gpu::Context& context = test::shared_context();

    // The same factory Engine::create installs, asked for a stage directly.
    // Building a DemodStage by hand is not possible from here and should not
    // be: the seam is the factory, and a stage reached any other way would be
    // a different object from the one the engine runs.
    engine::install_default_vrx_stages();
    engine::VrxStageFactory factory = engine::installed_vrx_stage_factory();
    REQUIRE(factory != nullptr);

    const dsp::GridParams grid = test_grid();
    const engine::VrxParams params = test_params();

    auto placement = engine::place(grid, kSourceRate, params);
    INFO(test::message_of(placement));
    REQUIRE(placement.has_value());

    auto channel_ring = gpu::Buffer::create(
        context, static_cast<VkDeviceSize>(kChannels) * kRingBlocks * kComplexBytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        gpu::MemoryKind::DeviceLocal);
    INFO(test::message_of(channel_ring));
    REQUIRE(channel_ring.has_value());

    engine::VrxStageRequest request;
    request.context = &context;
    request.id = engine::VrxId{1};
    request.params = params;
    request.placement = *placement;
    request.grid = grid;
    request.source_rate = kSourceRate;
    request.channel_rate = kChannelRate;
    request.channel_ring = channel_ring->handle();
    request.channel_ring_bytes = channel_ring->size();
    request.channel_ring_blocks = kRingBlocks;
    request.channel_ring_mask = kRingBlocks - 1;
    request.max_blocks_per_dispatch = kBlocksPerDispatch;
    request.frames_in_flight = kFramesInFlight;
    request.audio_rate = kAudioRate;

    auto built = factory(request);
    INFO(test::message_of(built));
    REQUIRE(built.has_value());
    REQUIRE(*built != nullptr);
    engine::VrxStage& stage = **built;

    // Sized the way the graph sizes it, from the stage's own answer, so a
    // dispatch that produced more frames than this would be the stage's bug
    // and not the harness's.
    auto audio = gpu::Buffer::create(context, stage.audio_bytes_for(kBlocksPerDispatch),
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     gpu::MemoryKind::Readback);
    INFO(test::message_of(audio));
    REQUIRE(audio.has_value());

    Recorder recorder;
    REQUIRE(recorder.open(context).has_value());

    std::uint32_t frame = 0;
    auto dispatch = [&](dsp::SampleIndex first_block) -> Expected<engine::StageOutput> {
        if (auto opened = recorder.begin(); !opened) {
            return std::unexpected(opened.error());
        }
        engine::StageRecord record;
        record.commands = recorder.handle();
        record.frame_index = frame;
        record.channel_ring = channel_ring->handle();
        record.channel_ring_bytes = channel_ring->size();
        record.first_block = first_block;
        record.block_count = kBlocksPerDispatch;
        record.audio_destination = audio->handle();
        record.audio_bytes = audio->size();
        frame = (frame + 1) % kFramesInFlight;

        auto recorded = stage.record(record);
        if (auto ended = recorder.end(); !ended) {
            return std::unexpected(ended.error());
        }
        return recorded;
    };

    // THE RUN BEFORE THE GAP. Four dispatches, contiguous, which is enough for
    // the stage to anchor on the first and be producing audio by the last: a
    // receiver that had produced nothing would have no cursor to skip and the
    // case would pass without meaning anything.
    constexpr int kSettleDispatches = 4;
    std::uint64_t frames_before = 0;
    dsp::SampleIndex next_block = 0;
    for (int i = 0; i < kSettleDispatches; ++i) {
        auto recorded = dispatch(next_block);
        INFO("dispatch " << i << " at block " << next_block << ": "
                         << test::message_of(recorded));
        REQUIRE(recorded.has_value());
        CHECK(recorded->reanchors == 0);
        CHECK(recorded->reanchor_frames_skipped == 0);
        frames_before += recorded->frames;
        next_block += kBlocksPerDispatch;
    }
    INFO("frames produced before the gap: " << frames_before);
    REQUIRE(frames_before > 0);

    // THE GAP. first_block jumps eight rings forward, which is what the stage
    // sees when a device stopped its transfers for a third of a second and the
    // channelizer resumed further up the stream.
    const dsp::SampleIndex resumed_at = next_block + kGapBlocks;
    auto skipped = dispatch(resumed_at);
    INFO("the dispatch across the gap: " << test::message_of(skipped));
    REQUIRE(skipped.has_value());

    // ONE EVENT, AND NO AUDIO OUT OF THE DISPATCH THAT SKIPPED. The stage
    // restarts and returns, so a dispatch reporting both a re-anchor and
    // frames would mean it had filtered samples it had just declared gone.
    CHECK(skipped->reanchors == 1);
    CHECK(skipped->frames == 0);
    CHECK(skipped->dispatches == 0);

    // AND THE FRAMES IT SKIPPED, measured against the gap's own length rather
    // than against anything the stage reported. kGapBlocks channel samples at
    // kChannelRate is a span of time, the receiver runs at kAudioRate, so the
    // frames nobody will ever hear are that span times that rate: 10485.8
    // here, and the stage answers 10518.
    //
    // A BAND AND NOT AN EQUALITY. The remaining 32 frames are the fine
    // filter's reach and the block the restart anchored into, which move the
    // resumed cursor by a couple of hundred channel samples out of the 16384
    // in the gap. Computing them exactly here would be a second copy of the
    // stage's own arithmetic, which would agree with whatever that arithmetic
    // did and check nothing. Two percent is wide enough for the filter and far
    // too narrow for a count that had lost or doubled the gap, which is the
    // failure worth catching.
    const double expected_frames = static_cast<double>(kGapBlocks) *
                                   static_cast<double>(kAudioRate) /
                                   static_cast<double>(kChannelRate);
    INFO("skipped " << skipped->reanchor_frames_skipped << " frames, expected about "
                    << expected_frames);
    CHECK(static_cast<double>(skipped->reanchor_frames_skipped) > expected_frames * 0.98);
    CHECK(static_cast<double>(skipped->reanchor_frames_skipped) < expected_frames * 1.02);

    // AND THE RECEIVER IS RUNNING AGAIN, which is the other half of why the
    // branch re-anchors instead of refusing. A restart that left the receiver
    // silent would report the same two numbers and be a dead receiver.
    std::uint64_t frames_after = 0;
    dsp::SampleIndex block = resumed_at + kBlocksPerDispatch;
    for (int i = 0; i < kSettleDispatches; ++i) {
        auto recorded = dispatch(block);
        INFO("dispatch " << i << " after the gap at block " << block << ": "
                         << test::message_of(recorded));
        REQUIRE(recorded.has_value());

        // AND IT DOES NOT SKIP AGAIN. Contiguous blocks after a restart are
        // inside the ring by construction, so a second re-anchor here would
        // mean the restart had anchored somewhere the ring does not hold.
        CHECK(recorded->reanchors == 0);
        frames_after += recorded->frames;
        block += kBlocksPerDispatch;
    }
    INFO("frames produced after the gap: " << frames_after);
    CHECK(frames_after > 0);
}
