// The fine stage, the seven demodulators and the three digital voice taps,
// recorded into the graph's command buffer, and the display tap beside them.
//
// Two dispatches per receiver per block. core/shaders/vrx_fine.comp mixes the
// residual to DC, filters to the requested bandwidth and resamples the coarse
// channel to the demodulation rate, writing a per-receiver ring.
// core/shaders/vrx_demod.comp detects out of that ring and writes real audio,
// or for P25p1, Dstar, Tetra and Dmr hands the ring's complex baseband out
// unchanged at the rate their decoder wants, and either is then copied into
// the graph's readback buffer. Both kernels are
// proved bit-exact against their twins in tests/reference/test_vrx.cpp; this
// file is the plumbing that gets them the right push constants.
//
// A third dispatch on the blocks where the graph asks for the display, and
// only on those: vrx_fine.comp again, specialized with the display tap's one
// 256-tap branch, reading the same channel and writing the ring the passband
// transform reads. It carries the fine stage's mix and none of its filter,
// so the pane shows what is around the receiver rather than the receiver's
// own filter shape. core/dsp/vrx_reference.h, "The display tap", has the
// design and the rate rule.
//
// WHAT THIS FILE OWNS AND WHY.
//
// Everything here is per receiver except the NCO circle, which is shared: it
// depends only on its own length, every receiver reads the same table, and at
// the default 16-bit index that is 512 KiB nobody should pay for fifty times.
// The tap table is not shareable, because it carries the receiver's own
// filter centre folded into the coefficients, which is the whole trick that
// makes the mixer one rotation per output instead of one per tap.
//
// THE THREE INDICES, WHICH ARE THE PART THAT GOES WRONG.
//
// The graph hands over an absolute channel-block index and a count. A channel
// block is one sample of every channel, so the channel stream's index IS the
// block index. From that this file derives two more streams:
//
//   the fine stream, at the demodulation rate Fd, where output j is taken at
//   channel instant j*Fc/Fd and needs taps-1 channel samples of history below
//   it;
//
//   the audio stream, at Fa = Fd/R, where sample i detects at fine index i*R
//   and needs the decimation filter's support and the detector's own history
//   below that.
//
// Both are pure functions of the absolute index, computed in exact integer
// arithmetic by core/dsp/vrx_reference.cpp's fine_block and demod_block. The
// only state carried between blocks is how far each stream has got, and that
// is a position rather than an accumulator: a stage re-entering the stream at
// an arbitrary index would compute the same push constants and the kernels
// would produce the same bits. That property is what makes retroactive decode
// and faster-than-realtime replay possible, and it is asserted directly in
// tests/reference/test_vrx.cpp.
//
// WHAT THIS COSTS, MEASURED ON 2026-09-18 AND NOT WHAT WAS PREDICTED.
//
// This comment used to say that the seam's one call per receiver meant 2N
// global pipeline flushes per block, that no two receivers would overlap, and
// that the fix when fifty receivers were measured would be to hand the seam
// phases instead of one record() call so the barriers amortise.
//
// The barriers cost 0.1 us per block at every receiver count from one to a
// hundred. They are not the problem and batching them would buy nothing. The
// prediction is left here in its corrected form because a wrong number that
// somebody has already written down sends the next person optimising the
// wrong thing, and deleting it quietly would let the same guess be made
// again.
//
// What a receiver costs is the dispatch work: about 18 us per receiver per
// block at M = 64 and 20 MS/s, essentially linear, 17.6 us at one receiver
// and 20.7 us at a hundred. The coarse chain beside it is flat at about
// 10 us whether there is one receiver or a hundred, which is the half of
// "two hundred receivers cost what two cost" that turned out to be true.
//
// So fifty receivers run at 3.28 times realtime on the discrete card with
// every overrun counter at zero, a hundred at 1.79x, and the thing to
// optimise if that is ever not enough is the fine stage itself rather than
// the seam around it. docs/fft.md carries the full table.

#include "core/engine/vrx_stage.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "core/dsp/types.h"
#include "core/dsp/vrx_reference.h"
#include "core/engine/graph.h"
#include "core/engine/noise_stage.h"
#include "core/engine/record_util.h"
#include "core/engine/vrx.h"
#include "core/gpu/buffer.h"
#include "core/gpu/context.h"
#include "core/gpu/kernel.h"
#include "core/gpu/shaders.h"

namespace revenant::engine {
namespace {

constexpr VkBufferUsageFlags kDeviceStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

constexpr std::uint32_t kComplexBytes = 8;

// A fine ring beyond this is a sizing mistake rather than a wide receiver: at
// the largest demodulation rate the planner will choose it is minutes of
// audio, and it would mean the graph asked for a dispatch far larger than the
// one it sized its own buffers for.
constexpr std::uint32_t kMaxFineCapacity = 1U << 22;

constexpr std::uint32_t kMinFineCapacity = 1U << 10;

// The one claim dsp::VrxShape rests on that nothing else measures: the fine
// tap table is exactly dsp::fine_tap_table_size(fine) entries long, which is
// why the shape carries the config and not the length.
//
// It is checked rather than assumed because the consequence of it being
// false is silent. build_buffers sizes the device and staging buffers from
// the config, and a retune whose shape compares equal copies that many bytes
// out of whatever table the new plan brought, so a table that did not match
// its config would be copied short or read past its end with no diagnostic
// anywhere. Two lines here is what lets the shape stay one list.
[[nodiscard]] Status fine_taps_match_config(const dsp::VrxPlan& plan) {
    const std::size_t wanted = dsp::fine_tap_table_size(plan.fine);
    if (plan.fine_taps.size() == wanted) {
        return {};
    }
    return fail(std::format(
        "a receiver's fine tap table holds {} entries where its {} phases and {} taps imply "
        "{}. dsp::VrxShape leaves the length out because it is a function of the config, so "
        "the two cannot differ and one of the planner or fine_tap_table_size has moved",
        plan.fine_taps.size(), plan.fine.phases, plan.fine.taps, wanted));
}

// WHERE detector_history WENT. It used to live here and answer how far below
// its own sample the detector reads, and this file added the audio filter's
// reach to it by hand. dsp::demod_fine_history in core/dsp/vrx_reference.h
// is that sum, and dsp::validate refuses a dispatch against the same
// function, so the ring sized here and the span refused there can no longer
// come out different. They could before, and FM stereo is what would have
// separated them: its pilot filter reaches below the audio window's centre
// and neither expression here knew about it.

// The largest fine output whose integer input instant is at or below `newest`,
// which is floor((newest + 1) * Fd / Fc) - 1 rounded the way the resampler
// rounds. Split over Fc first so no product leaves 64 bits at any index this
// engine can reach.
[[nodiscard]] dsp::SampleIndex highest_output_for(dsp::SampleIndex newest,
                                                  dsp::SampleRate channel_rate,
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

// The smallest fine output whose integer input instant is at or above
// `needed`, which is ceil(needed * Fd / Fc), split the same way.
[[nodiscard]] dsp::SampleIndex lowest_output_for(dsp::SampleIndex needed,
                                                 dsp::SampleRate channel_rate,
                                                 dsp::SampleRate demod_rate) {
    const auto fc = static_cast<dsp::SampleIndex>(channel_rate);
    const auto fd = static_cast<dsp::SampleIndex>(demod_rate);
    const dsp::SampleIndex whole = needed / fc;
    const dsp::SampleIndex part = needed % fc;
    return whole * fd + (part * fd + fc - 1U) / fc;
}

// ---------------------------------------------------------------------------
// The shared NCO circle
// ---------------------------------------------------------------------------

// One table per device per length, held weakly so it goes away with the last
// receiver that wanted it. Weakly rather than forever because a table is half
// a megabyte and an engine that opened a source, closed it and opened another
// at a different rate would otherwise keep both.
struct NcoCache {
    struct Entry {
        VkDevice device = VK_NULL_HANDLE;
        std::uint32_t log2 = 0;
        std::weak_ptr<gpu::Buffer> table;
    };

    std::mutex lock;
    std::vector<Entry> entries;
};

[[nodiscard]] NcoCache& nco_cache() {
    static NcoCache cache;
    return cache;
}

[[nodiscard]] Expected<std::shared_ptr<gpu::Buffer>> shared_nco_table(
    const gpu::Context& context, std::uint32_t log2) {
    auto& cache = nco_cache();
    std::scoped_lock guard(cache.lock);

    for (auto& entry : cache.entries) {
        if (entry.device == context.device() && entry.log2 == log2) {
            if (auto held = entry.table.lock()) {
                return held;
            }
        }
    }

    auto built = dsp::build_nco_table(log2);
    if (!built) {
        return std::unexpected(with_context(built.error(), "vrx stage NCO table"));
    }
    const VkDeviceSize bytes = built->size() * sizeof(dsp::Complex32);

    auto device_buffer =
        gpu::Buffer::create(context, bytes, kDeviceStorage, gpu::MemoryKind::DeviceLocal);
    if (!device_buffer) {
        return std::unexpected(with_context(device_buffer.error(), "vrx stage NCO table"));
    }

    auto staging = gpu::Buffer::create(context, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       gpu::MemoryKind::Upload);
    if (!staging) {
        return std::unexpected(with_context(staging.error(), "vrx stage NCO staging"));
    }
    if (auto wrote = staging->write(std::as_bytes(std::span<const dsp::Complex32>(*built)));
        !wrote) {
        return std::unexpected(with_context(wrote.error(), "vrx stage NCO staging"));
    }

    auto runner = gpu::CommandRunner::create(context);
    if (!runner) {
        return std::unexpected(with_context(runner.error(), "vrx stage NCO upload"));
    }
    const gpu::CommandRunner::BufferCopy copies[] = {
        {staging->handle(), device_buffer->handle(), bytes},
    };
    if (auto copied = runner->copy(copies); !copied) {
        return std::unexpected(with_context(copied.error(), "vrx stage NCO upload"));
    }

    auto shared = std::make_shared<gpu::Buffer>(std::move(*device_buffer));

    auto dead = std::find_if(cache.entries.begin(), cache.entries.end(),
                             [](const NcoCache::Entry& e) { return e.table.expired(); });
    if (dead != cache.entries.end()) {
        *dead = NcoCache::Entry{context.device(), log2, shared};
    } else {
        cache.entries.push_back(NcoCache::Entry{context.device(), log2, shared});
    }
    return shared;
}

// ---------------------------------------------------------------------------
// The stage
// ---------------------------------------------------------------------------

class DemodStage final : public VrxStage {
public:
    [[nodiscard]] static Expected<std::unique_ptr<VrxStage>> create(
        const VrxStageRequest& request);

    ~DemodStage() override {
        if (descriptors_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, descriptors_, nullptr);
        }
    }

    [[nodiscard]] VkDeviceSize audio_bytes_for(std::uint32_t blocks) const override {
        // Frames times floats per frame. The second factor was 1 until FM
        // stereo, which is a channel count and not a mode: a stereo WFM
        // receiver writes L and R where a mono one wrote one sample, and a
        // buffer sized without it is one the kernel writes past.
        return static_cast<VkDeviceSize>(audio_for(outputs_for(blocks))) *
               plan_.demod.channels * sizeof(float);
    }

    [[nodiscard]] Expected<StageOutput> record(const StageRecord& record) override;

    [[nodiscard]] Status retune(const VrxParams& params,
                                const VrxPlacement& placement) override;

    // Every field of this is written in build() and never again, which is
    // what lets the graph read it from the control plane while the recording
    // thread is inside record(). The ring and its size are fixed, and so is
    // the group delay, because the display filter's length does not depend
    // on the display rate.
    //
    // Where the stream's DC sits and the rate it runs at DO move on a
    // retune, so they are not here. They ride back on StageOutput, which
    // only the recording thread reads.
    [[nodiscard]] StageDisplayOutput display_output() const override { return display_output_; }

private:
    DemodStage() = default;

    [[nodiscard]] Status build(const VrxStageRequest& request);
    [[nodiscard]] Status build_display(const VrxStageRequest& request, std::uint64_t blocks);
    [[nodiscard]] Status build_pipelines();
    [[nodiscard]] Status build_buffers(VkBuffer channel_ring);
    [[nodiscard]] Status build_descriptors(VkBuffer channel_ring);
    [[nodiscard]] Status record_display(const StageRecord& record, StageOutput& out);

    [[nodiscard]] std::uint64_t outputs_for(std::uint64_t blocks) const {
        // Each channel sample advances the output instant by Fd/Fc outputs,
        // and the fractional part can straddle one more.
        const auto fc = static_cast<std::uint64_t>(plan_.channel_rate);
        const auto fd = static_cast<std::uint64_t>(plan_.demod_rate);
        return (blocks * fd) / fc + 1U;
    }

    [[nodiscard]] std::uint64_t audio_for(std::uint64_t outputs) const {
        return outputs / plan_.demod.decimation + 1U;
    }

    const gpu::Context* context_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;

    dsp::VrxPlan plan_;
    dsp::GridParams grid_{};
    dsp::SampleRate source_rate_ = 0;

    std::uint32_t chan_base_ = 0;
    std::uint32_t channel_ring_blocks_ = 0;
    std::uint32_t channel_ring_mask_ = 0;
    std::uint32_t frames_in_flight_ = 1;
    std::uint32_t local_size_x_ = gpu::kDefaultLocalSizeX;

    std::uint32_t fine_capacity_ = 0;
    std::uint32_t fine_mask_ = 0;
    std::uint32_t fine_history_ = 0;
    std::uint32_t max_outputs_ = 0;
    std::uint32_t max_audio_ = 0;
    VkDeviceSize taps_bytes_ = 0;

    gpu::ComputePipeline fine_pipeline_;
    gpu::ComputePipeline demod_pipeline_;

    std::shared_ptr<gpu::Buffer> nco_;
    gpu::Buffer taps_;
    std::vector<gpu::Buffer> taps_staging_;
    gpu::Buffer weights_;
    gpu::Buffer fine_ring_;
    std::vector<gpu::Buffer> audio_;

    VkDescriptorPool descriptors_ = VK_NULL_HANDLE;
    VkDescriptorSet fine_set_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> demod_sets_;

    // The display tap. Built when the graph has a passband stage at all,
    // dispatched only on blocks whose StageRecord::display is set.
    bool display_built_ = false;
    dsp::VrxDisplayPlan display_;
    StageDisplayOutput display_output_{};
    std::uint32_t display_capacity_ = 0;
    std::uint32_t display_mask_ = 0;
    std::uint32_t max_display_outputs_ = 0;
    VkDeviceSize display_taps_bytes_ = 0;
    gpu::ComputePipeline display_pipeline_;
    gpu::Buffer display_taps_;
    std::vector<gpu::Buffer> display_taps_staging_;
    gpu::Buffer display_ring_;
    VkDescriptorSet display_set_ = VK_NULL_HANDLE;

    // Where the display stream starts again, given the oldest channel block
    // the graph is offering. Called when the display is switched on, when a
    // retune moves the display rate, and when a discontinuity has put the
    // inputs it wanted out of the ring. The ring below display_from_ is then
    // somebody else's samples, and StageOutput::display_from is what tells
    // the graph not to transform them.
    void anchor_display(dsp::SampleIndex first_block) {
        const dsp::SampleIndex needed =
            first_block + static_cast<dsp::SampleIndex>(display_.fine.taps - 1U);
        display_from_ =
            lowest_output_for(needed, display_.channel_rate, display_.display_rate);
        next_display_ = display_from_;
    }

    bool display_running_ = false;
    dsp::SampleIndex display_from_ = 0;
    dsp::SampleIndex next_display_ = 0;

    // Where this receiver starts filtering, given the oldest channel block the
    // graph is offering. Called once when the stage first records, and again
    // whenever a discontinuity has put the inputs it wanted out of the ring.
    //
    // All three cursors move together, which is why this is one function rather
    // than three assignments at each site: first_output_ is the oldest fine
    // sample this receiver produced, next_output_ is the fine stage's read
    // cursor, and next_audio_ is the demodulator's. Moving one without the
    // others is a demodulator reading samples the fine stage never wrote.
    //
    // WHAT THIS USED TO SAY: that first_output_ "is what the passband window
    // is drawn from" and that moving one cursor alone left "a receiver whose
    // display and whose audio disagree". The passband reads the display ring
    // now, which keeps cursors of its own; see anchor_display.
    void anchor_to(dsp::SampleIndex first_block) {
        // The first output whose filter support is entirely inside samples this
        // graph has actually channelized. Starting lower would filter zeros and
        // produce a transient nobody asked for, and starting from the ring's
        // contents would filter whatever the allocator left.
        const dsp::SampleIndex needed =
            first_block + static_cast<dsp::SampleIndex>(plan_.fine.taps - 1U);
        first_output_ = lowest_output_for(needed, plan_.channel_rate, plan_.demod_rate);
        next_output_ = first_output_;

        const auto decimation = static_cast<dsp::SampleIndex>(plan_.demod.decimation);
        next_audio_ = (first_output_ + fine_history_ + decimation - 1U) / decimation;
    }

    bool started_ = false;
    bool taps_pending_ = false;
    dsp::SampleIndex first_output_ = 0;
    dsp::SampleIndex next_output_ = 0;
    dsp::SampleIndex next_audio_ = 0;

    // The noise blanker, notch and noise reduction, for a receiver that
    // produces audio; null for the complex taps. core/engine/noise_stage.h
    // has the three points it is called from.
    std::unique_ptr<NoiseChain> noise_;
};

Expected<std::unique_ptr<VrxStage>> DemodStage::create(const VrxStageRequest& request) {
    std::unique_ptr<DemodStage> stage(new (std::nothrow) DemodStage());
    if (stage == nullptr) {
        return fail("could not allocate a receiver stage");
    }
    if (auto built = stage->build(request); !built) {
        return std::unexpected(built.error());
    }
    return std::unique_ptr<VrxStage>(std::move(stage));
}

Status DemodStage::build(const VrxStageRequest& request) {
    if (request.context == nullptr) {
        return fail("a receiver stage was requested with no Vulkan context");
    }
    context_ = request.context;
    device_ = context_->device();
    grid_ = request.grid;
    source_rate_ = request.source_rate;
    channel_ring_blocks_ = request.channel_ring_blocks;
    channel_ring_mask_ = request.channel_ring_mask;
    frames_in_flight_ = std::max<std::uint32_t>(1, request.frames_in_flight);
    local_size_x_ =
        request.local_size_x != 0 ? request.local_size_x : gpu::kDefaultLocalSizeX;
    chan_base_ = request.placement.channel * channel_ring_blocks_;

    // The graph resolved the audio rate against its own default already, and
    // the planner would otherwise resolve it against a different one.
    VrxParams params = request.params;
    params.audio_rate = request.audio_rate;

    auto planned = dsp::plan_vrx(grid_, source_rate_, params, request.placement);
    if (!planned) {
        return std::unexpected(with_context(planned.error(), "receiver plan"));
    }
    plan_ = std::move(*planned);

    if (plan_.channel_rate != request.channel_rate) {
        return fail(std::format(
            "the graph channelizes at {} S/s and the placement says {}; the two describe the "
            "same grid and must agree",
            request.channel_rate, plan_.channel_rate));
    }

    const auto blocks = static_cast<std::uint64_t>(request.max_blocks_per_dispatch);
    if (blocks == 0) {
        return fail("a receiver stage cannot be built against a zero-block dispatch");
    }

    // The fine filter reaches taps-1 channel samples below its first input,
    // and every frame in flight behind this one writes its own dispatch above
    // it. Frames overlap on the device, so all of that has to be live in the
    // channel ring at once or a later frame's channelizer overwrites history
    // this frame's fine stage is still reading, the kernel's masked read
    // returns the new samples, and the receiver quietly filters the wrong
    // ones. The display tap below has the same rule.
    //
    // WHAT THIS USED TO COUNT: `blocks + taps`, one dispatch above the reach.
    // At 128 blocks and three frames in flight it accepted a 512 block ring
    // for the 252 tap filter of a 3 kHz AM receiver, which needs 636, and the
    // graph gave exactly that ring to a grid with no spectrum or passband
    // stage. Run through the kernel's host twin with the two later frames
    // written first, 79 of that dispatch's 82 outputs changed.
    // tests/engine/test_channel_ring.cpp.
    const std::uint64_t channel_span = blocks * frames_in_flight_ + plan_.fine.taps;
    if (channel_span > channel_ring_blocks_) {
        return fail(std::format(
            "a {} tap fine filter with {} frames in flight of {} blocks each needs {} channel "
            "samples live and the channel ring holds {}. Either the receiver is too narrow for "
            "this grid or the engine's ring_seconds is too short",
            plan_.fine.taps, frames_in_flight_, blocks, channel_span, channel_ring_blocks_));
    }

    max_outputs_ = static_cast<std::uint32_t>(outputs_for(blocks));
    max_audio_ = static_cast<std::uint32_t>(audio_for(max_outputs_));
    fine_history_ = dsp::demod_fine_history(plan_.demod);

    // Room for every dispatch that can be in flight at once, plus the
    // detector's history, plus the one the recording thread is filling. A
    // ring exactly one dispatch long would have the detector reading samples
    // the next fine dispatch had already overwritten, which is silent.
    //
    // WHAT THIS USED TO ADD: room for a passband window, because the
    // passband transformed this ring. It transforms the display ring now,
    // which build_display sizes, so this ring is the demodulator's alone.
    const std::uint64_t wanted =
        static_cast<std::uint64_t>(max_outputs_) * (frames_in_flight_ + 1U) + fine_history_ + 2U;
    if (wanted > kMaxFineCapacity) {
        return fail(std::format(
            "this receiver would need a fine ring of {} samples and the limit is {}",
            wanted, kMaxFineCapacity));
    }
    fine_capacity_ = std::bit_ceil(std::max<std::uint64_t>(wanted, kMinFineCapacity)) &
                     0xFFFF'FFFFU;
    fine_mask_ = fine_capacity_ - 1U;

    // Both kernels' 32-bit arithmetic, checked here rather than at the first
    // block. Every one of these is a sizing mistake in this function and none
    // of them is visible on the device: the recurrence simply addresses the
    // wrong sample.
    {
        dsp::VrxFineParams probe;
        probe.chan_base = chan_base_;
        probe.chan_mask = channel_ring_mask_;
        probe.out_mask = fine_mask_;
        probe.count = max_outputs_;
        probe.out_rate = static_cast<std::uint32_t>(plan_.demod_rate);
        probe.step_whole =
            static_cast<std::uint32_t>(plan_.channel_rate / plan_.demod_rate);
        probe.step_rem = static_cast<std::uint32_t>(plan_.channel_rate % plan_.demod_rate);
        probe.frac0 = probe.out_rate - 1U;
        probe.inv_out_rate = 1.0F / static_cast<float>(plan_.demod_rate);
        if (auto valid = dsp::validate(plan_.fine, probe); !valid) {
            return std::unexpected(with_context(valid.error(), "receiver fine stage"));
        }
    }
    {
        dsp::VrxDemodParams probe;
        probe.in_mask = fine_mask_;
        probe.count = max_audio_;
        probe.gain = plan_.demod_gain;
        if (auto valid = dsp::validate(plan_.demod, probe); !valid) {
            return std::unexpected(with_context(valid.error(), "receiver demodulator"));
        }
    }

    if (request.passband_transform != 0) {
        if (auto built = build_display(request, blocks); !built) {
            return built;
        }
    }

    if (auto built = build_pipelines(); !built) {
        return built;
    }
    if (auto built = build_buffers(request.channel_ring); !built) {
        return built;
    }

    if (display_built_) {
        display_output_.ring = display_ring_.handle();
        display_output_.capacity = display_capacity_;
        display_output_.mask = display_mask_;
        display_output_.group_delay_channel_samples = display_.group_delay_channel_samples;
    }

    if (auto built = build_descriptors(request.channel_ring); !built) {
        return built;
    }

    if (!produces_audio(request.params.demod)) {
        return {};
    }
    std::vector<VkBuffer> audio(audio_.size());
    for (std::size_t i = 0; i < audio_.size(); ++i) {
        audio[i] = audio_[i].handle();
    }
    NoiseChainRequest chain;
    chain.context = context_;
    chain.frames_in_flight = frames_in_flight_;
    chain.local_size_x = local_size_x_;
    chain.channel_rate = plan_.channel_rate;
    chain.channel_ring = request.channel_ring;
    chain.channel_ring_mask = channel_ring_mask_;
    chain.max_fine_span = static_cast<std::uint32_t>(blocks) + plan_.fine.taps + 1U;
    chain.fine_layout = fine_pipeline_.descriptor_layout();
    chain.fine_taps = taps_.handle();
    chain.nco = nco_->handle();
    chain.fine_ring = fine_ring_.handle();
    chain.audio = audio;
    chain.max_audio = max_audio_;
    auto noise = NoiseChain::create(chain, params, plan_);
    if (!noise) {
        return std::unexpected(noise.error());
    }
    noise_ = std::move(*noise);
    return {};
}

Status DemodStage::build_display(const VrxStageRequest& request, std::uint64_t blocks) {
    auto planned = dsp::plan_vrx_display(plan_);
    if (!planned) {
        return std::unexpected(with_context(planned.error(), "receiver display tap"));
    }
    display_ = std::move(*planned);

    // The display filter reaches back further than most fine filters do,
    // kDisplayTaps channel samples, and all of it has to stay live while
    // every frame in flight behind this one writes its own dispatch above
    // it. Frames overlap on the device, so a ring that holds the reach and
    // one dispatch lets the channelizer in a later frame overwrite history
    // this frame's tap is still reading, and the pane shows a splice.
    //
    // WHAT THIS USED TO COUNT: `blocks + taps`, one dispatch above the reach.
    // At 100 blocks and three frames in flight that accepted a 512 block
    // ring where the 256 tap filter needs 556.
    // tests/engine/test_channel_ring.cpp.
    const std::uint64_t channel_span = blocks * frames_in_flight_ + display_.fine.taps;
    if (channel_span > channel_ring_blocks_) {
        return fail(std::format(
            "the {} tap display filter with {} frames in flight of {} blocks each needs {} "
            "channel samples live and the channel ring holds {}. The engine's ring_seconds is "
            "too short for a passband stage",
            display_.fine.taps, frames_in_flight_, blocks, channel_span, channel_ring_blocks_));
    }

    // Sized for R = 1, one display output per channel sample, which is the
    // most any rung produces. A retune can move R and cannot resize a ring a
    // command buffer names, so the ring is built for the widest rate the
    // stage could ever be retuned to rather than the one it starts at.
    max_display_outputs_ = static_cast<std::uint32_t>(blocks + 1U);

    // The transform's window is the last N display samples of the dispatch
    // that recorded it, and every frame submitted behind that one writes
    // above it. So the ring holds the window plus a frame's worth for each
    // of them, or the transform reads slots a later dispatch has already
    // overwritten and the display shows a splice of two eras.
    const std::uint64_t wanted = static_cast<std::uint64_t>(request.passband_transform) +
                                 static_cast<std::uint64_t>(max_display_outputs_) *
                                     frames_in_flight_ +
                                 2U;
    if (wanted > kMaxFineCapacity) {
        return fail(std::format(
            "this receiver would need a display ring of {} samples and the limit is {}", wanted,
            kMaxFineCapacity));
    }
    display_capacity_ =
        std::bit_ceil(std::max<std::uint64_t>(wanted, kMinFineCapacity)) & 0xFFFF'FFFFU;
    display_mask_ = display_capacity_ - 1U;

    // The kernel's 32-bit arithmetic at the worst case, R = 1: a step of one
    // whole channel sample, no remainder, and the largest output rate.
    dsp::VrxFineParams probe;
    probe.chan_base = chan_base_;
    probe.chan_mask = channel_ring_mask_;
    probe.out_mask = display_mask_;
    probe.count = max_display_outputs_;
    probe.out_rate = static_cast<std::uint32_t>(plan_.channel_rate);
    probe.step_whole = 1U;
    probe.step_rem = 0U;
    probe.frac0 = 0U;
    probe.inv_out_rate = 1.0F / static_cast<float>(plan_.channel_rate);
    if (auto valid = dsp::validate(display_.fine, probe); !valid) {
        return std::unexpected(with_context(valid.error(), "receiver display tap"));
    }
    if (display_.taps.size() != dsp::fine_tap_table_size(display_.fine)) {
        return fail(std::format("the display tap table holds {} entries and its config implies "
                                "{}",
                                display_.taps.size(), dsp::fine_tap_table_size(display_.fine)));
    }
    display_taps_bytes_ = dsp::fine_tap_table_size(display_.fine) * sizeof(dsp::Complex32);
    display_built_ = true;
    return {};
}

// Where the fine stream's DC sits in the source's baseband frame: the coarse
// channel's centre plus what the mixer translates to DC, both exact
// rationals and both already reduced by their own producers.
//
// mix is the residual for five of the modes, the residual for USB and LSB
// too (their asymmetry is in the FILTER centre, not the mixer's), and the
// residual less the operator's pitch for CW. Taking it from the plan rather
// than re-deriving it from the mode is the point: one table of that, in
// plan_vrx, and nothing here to fall out of step with it.
[[nodiscard]] std::pair<std::int64_t, std::int64_t> fine_dc_of(const dsp::VrxPlan& plan) {
    const std::int64_t centre_num = plan.placement.channel_centre.numerator;
    const std::int64_t centre_den = plan.placement.channel_centre.denominator;
    if (centre_den == 0 || plan.mix_denominator == 0) {
        return {0, 1};
    }
    return {centre_num * plan.mix_denominator + plan.mix_numerator * centre_den,
            centre_den * plan.mix_denominator};
}

Status DemodStage::build_pipelines() {
    {
        const std::uint32_t constants[] = {plan_.fine.taps, plan_.fine.phases,
                                           plan_.fine.nco_log2};
        gpu::ComputePipeline::Options options;
        options.spirv = gpu::shaders::vrx_fine();
        options.storage_buffer_count = 4;
        options.local_size_x = local_size_x_;
        options.push_constant_bytes = sizeof(dsp::VrxFineParams);
        options.grid_constants = constants;

        auto pipeline = gpu::ComputePipeline::create(*context_, options);
        if (!pipeline) {
            return std::unexpected(with_context(pipeline.error(), "receiver fine pipeline"));
        }
        fine_pipeline_ = std::move(*pipeline);
    }
    {
        const std::uint32_t constants[] = {plan_.demod.mode,      plan_.demod.decimation,
                                           plan_.demod.audio_taps, plan_.demod.dc_taps,
                                           plan_.demod.channels,   plan_.demod.pilot_taps};
        gpu::ComputePipeline::Options options;
        options.spirv = gpu::shaders::vrx_demod();
        options.storage_buffer_count = 3;
        options.local_size_x = local_size_x_;
        options.push_constant_bytes = sizeof(dsp::VrxDemodParams);
        options.grid_constants = constants;

        auto pipeline = gpu::ComputePipeline::create(*context_, options);
        if (!pipeline) {
            return std::unexpected(with_context(pipeline.error(), "receiver demod pipeline"));
        }
        demod_pipeline_ = std::move(*pipeline);
    }
    if (display_built_) {
        // The fine kernel again. One branch and no fractional phase, because
        // the display rate divides the channel rate; the tap count is fixed
        // so that a retune which moves the rate is a table and not a
        // pipeline.
        const std::uint32_t constants[] = {display_.fine.taps, display_.fine.phases,
                                           display_.fine.nco_log2};
        gpu::ComputePipeline::Options options;
        options.spirv = gpu::shaders::vrx_fine();
        options.storage_buffer_count = 4;
        options.local_size_x = local_size_x_;
        options.push_constant_bytes = sizeof(dsp::VrxFineParams);
        options.grid_constants = constants;

        auto pipeline = gpu::ComputePipeline::create(*context_, options);
        if (!pipeline) {
            return std::unexpected(with_context(pipeline.error(), "receiver display pipeline"));
        }
        display_pipeline_ = std::move(*pipeline);
    }
    return {};
}

Status DemodStage::build_buffers(VkBuffer channel_ring) {
    if (channel_ring == VK_NULL_HANDLE) {
        return fail("a receiver stage was requested before the channel ring existed");
    }

    auto nco = shared_nco_table(*context_, plan_.fine.nco_log2);
    if (!nco) {
        return std::unexpected(nco.error());
    }
    nco_ = std::move(*nco);

    // Sized from the config, not from the vector that happens to be in the
    // plan. dsp::VrxShape leaves the tap table's length out on the grounds
    // that it is a function of VrxFineConfig, and every retune past the
    // shape comparison copies taps_bytes_ bytes out of whatever table the
    // new plan brought. Taking the length off the vector here would make
    // that reasoning circular: a plan whose table did not match its config
    // would size the buffer to itself and the mismatch would first be felt
    // as a short or overrunning copy at some later retune.
    if (const Status sized = fine_taps_match_config(plan_); !sized) {
        return std::unexpected(with_context(sized.error(), "receiver tap table"));
    }
    taps_bytes_ = dsp::fine_tap_table_size(plan_.fine) * sizeof(dsp::Complex32);
    const VkDeviceSize weight_bytes = plan_.demod_weights.size() * sizeof(float);
    const VkDeviceSize fine_bytes = static_cast<VkDeviceSize>(fine_capacity_) * kComplexBytes;
    const VkDeviceSize audio_bytes =
        static_cast<VkDeviceSize>(max_audio_) * plan_.demod.channels * sizeof(float);

    auto make_device = [&](VkDeviceSize bytes, const char* what) -> Expected<gpu::Buffer> {
        auto buffer =
            gpu::Buffer::create(*context_, bytes, kDeviceStorage, gpu::MemoryKind::DeviceLocal);
        if (!buffer) {
            return std::unexpected(with_context(buffer.error(), what));
        }
        return buffer;
    };

    auto taps = make_device(taps_bytes_, "receiver tap table");
    if (!taps) {
        return std::unexpected(taps.error());
    }
    taps_ = std::move(*taps);

    auto weights = make_device(weight_bytes, "receiver detector weights");
    if (!weights) {
        return std::unexpected(weights.error());
    }
    weights_ = std::move(*weights);

    auto fine = make_device(fine_bytes, "receiver fine ring");
    if (!fine) {
        return std::unexpected(fine.error());
    }
    fine_ring_ = std::move(*fine);

    if (display_built_) {
        auto display_taps = make_device(display_taps_bytes_, "receiver display taps");
        if (!display_taps) {
            return std::unexpected(display_taps.error());
        }
        display_taps_ = std::move(*display_taps);

        auto display_ring = make_device(
            static_cast<VkDeviceSize>(display_capacity_) * kComplexBytes, "receiver display ring");
        if (!display_ring) {
            return std::unexpected(display_ring.error());
        }
        display_ring_ = std::move(*display_ring);

        display_taps_staging_.reserve(frames_in_flight_);
        for (std::uint32_t i = 0; i < frames_in_flight_; ++i) {
            auto staging = gpu::Buffer::create(*context_, display_taps_bytes_,
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                               gpu::MemoryKind::Upload);
            if (!staging) {
                return std::unexpected(
                    with_context(staging.error(), "receiver display tap staging"));
            }
            display_taps_staging_.push_back(std::move(*staging));
        }
        if (auto wrote = display_taps_staging_[0].write(
                std::as_bytes(std::span<const dsp::Complex32>(display_.taps)));
            !wrote) {
            return std::unexpected(with_context(wrote.error(), "receiver display taps"));
        }
    }

    // One staging buffer per frame in flight, and it is the frame index that
    // makes writing it on the recording thread safe: the graph does not reuse
    // a frame slot until that slot's previous submission has completed, so
    // the copy recorded out of this buffer last time round is finished before
    // the host writes it again. One shared staging buffer would be torn by a
    // second retune landing while the first copy was still in flight.
    audio_.reserve(frames_in_flight_);
    taps_staging_.reserve(frames_in_flight_);
    for (std::uint32_t i = 0; i < frames_in_flight_; ++i) {
        auto buffer = make_device(audio_bytes, "receiver audio scratch");
        if (!buffer) {
            return std::unexpected(buffer.error());
        }
        audio_.push_back(std::move(*buffer));

        auto staging = gpu::Buffer::create(*context_, taps_bytes_,
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                           gpu::MemoryKind::Upload);
        if (!staging) {
            return std::unexpected(with_context(staging.error(), "receiver tap staging"));
        }
        taps_staging_.push_back(std::move(*staging));
    }

    auto weight_staging = gpu::Buffer::create(*context_, weight_bytes,
                                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                              gpu::MemoryKind::Upload);
    if (!weight_staging) {
        return std::unexpected(with_context(weight_staging.error(), "receiver weight staging"));
    }
    if (auto wrote = weight_staging->write(
            std::as_bytes(std::span<const float>(plan_.demod_weights)));
        !wrote) {
        return std::unexpected(with_context(wrote.error(), "receiver weights"));
    }
    if (auto wrote = taps_staging_[0].write(
            std::as_bytes(std::span<const dsp::Complex32>(plan_.fine_taps)));
        !wrote) {
        return std::unexpected(with_context(wrote.error(), "receiver taps"));
    }

    auto runner = gpu::CommandRunner::create(*context_);
    if (!runner) {
        return std::unexpected(with_context(runner.error(), "receiver upload"));
    }

    // Zeroed for the same reason Graph::prepare zeroes the channel ring: the
    // detector reads history the first fine dispatch has not written, and
    // whatever the allocator last had in that memory would otherwise be
    // demodulated.
    std::vector<gpu::CommandRunner::BufferClear> clears;
    clears.reserve(2 + audio_.size());
    clears.push_back({fine_ring_.handle(), fine_ring_.size()});
    if (display_built_) {
        clears.push_back({display_ring_.handle(), display_ring_.size()});
    }
    for (auto& buffer : audio_) {
        clears.push_back({buffer.handle(), buffer.size()});
    }
    if (auto cleared = runner->clear(clears); !cleared) {
        return std::unexpected(with_context(cleared.error(), "receiver clear"));
    }

    std::vector<gpu::CommandRunner::BufferCopy> copies{
        {taps_staging_[0].handle(), taps_.handle(), taps_bytes_},
        {weight_staging->handle(), weights_.handle(), weight_bytes},
    };
    if (display_built_) {
        copies.push_back(
            {display_taps_staging_[0].handle(), display_taps_.handle(), display_taps_bytes_});
    }
    if (auto copied = runner->copy(copies); !copied) {
        return std::unexpected(with_context(copied.error(), "receiver upload"));
    }
    return {};
}

Status DemodStage::build_descriptors(VkBuffer channel_ring) {
    // The fine set binds nothing that varies per frame, so there is one of
    // it, and the display set is the same shape. The detector writes into a
    // per-frame scratch buffer, so there is one of those per frame in flight.
    const std::uint32_t display_sets = display_built_ ? 1U : 0U;
    const std::uint32_t sets = 1U + display_sets + frames_in_flight_;
    const std::uint32_t buffers = 4U + 4U * display_sets + 3U * frames_in_flight_;

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = buffers;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = sets;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;

    VkResult result = vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptors_);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkCreateDescriptorPool failed for a receiver ({})",
                                gpu::result_name(result)),
                    result);
    }

    {
        VkDescriptorSetLayout layout = fine_pipeline_.descriptor_layout();
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = descriptors_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &layout;
        result = vkAllocateDescriptorSets(device_, &alloc, &fine_set_);
        if (result != VK_SUCCESS) {
            return fail(std::format("vkAllocateDescriptorSets failed for a receiver ({})",
                                    gpu::result_name(result)),
                        result);
        }
        const VkBuffer bound[] = {channel_ring, taps_.handle(), nco_->handle(),
                                  fine_ring_.handle()};
        if (auto wrote = write_storage_set(device_, fine_set_, bound); !wrote) {
            return std::unexpected(with_context(wrote.error(), "receiver fine set"));
        }
    }

    if (display_built_) {
        VkDescriptorSetLayout layout = display_pipeline_.descriptor_layout();
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = descriptors_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &layout;
        result = vkAllocateDescriptorSets(device_, &alloc, &display_set_);
        if (result != VK_SUCCESS) {
            return fail(std::format("vkAllocateDescriptorSets failed for a receiver display ({})",
                                    gpu::result_name(result)),
                        result);
        }
        const VkBuffer bound[] = {channel_ring, display_taps_.handle(), nco_->handle(),
                                  display_ring_.handle()};
        if (auto wrote = write_storage_set(device_, display_set_, bound); !wrote) {
            return std::unexpected(with_context(wrote.error(), "receiver display set"));
        }
    }

    std::vector<VkDescriptorSetLayout> layouts(frames_in_flight_,
                                               demod_pipeline_.descriptor_layout());
    demod_sets_.assign(frames_in_flight_, VK_NULL_HANDLE);

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = descriptors_;
    alloc.descriptorSetCount = frames_in_flight_;
    alloc.pSetLayouts = layouts.data();
    result = vkAllocateDescriptorSets(device_, &alloc, demod_sets_.data());
    if (result != VK_SUCCESS) {
        return fail(std::format("vkAllocateDescriptorSets failed for a receiver detector ({})",
                                gpu::result_name(result)),
                    result);
    }

    for (std::uint32_t i = 0; i < frames_in_flight_; ++i) {
        const VkBuffer bound[] = {fine_ring_.handle(), weights_.handle(), audio_[i].handle()};
        if (auto wrote = write_storage_set(device_, demod_sets_[i], bound); !wrote) {
            return std::unexpected(with_context(wrote.error(), "receiver demod set"));
        }
    }
    return {};
}

Expected<StageOutput> DemodStage::record(const StageRecord& record) {
    StageOutput out;
    out.channels = plan_.demod.channels;
    out.rate = plan_.output_rate;

    // A digital voice receiver's pair is one complex sample, not a stereo
    // frame, and the signal meter divides by a different count for each.
    // Left false it would average |z|^2 over two floats instead of one
    // sample and read 3.01 dB low, the mirror of the error
    // StageOutput::complex_iq was added to end for stereo WFM.
    out.complex_iq = dsp::is_complex_output(plan_.demod.mode);

    if (record.block_count == 0) {
        return out;
    }
    if (record.frame_index >= frames_in_flight_) {
        return fail(std::format("a receiver was recorded into frame {} of {}",
                                record.frame_index, frames_in_flight_));
    }
    const std::uint32_t frame = record.frame_index;

    if (!started_) {
        // With the blanker on from the start, the first reference window has
        // to be channel samples rather than the ring's clear value, or the
        // first samples of the stream all stand out against it and are
        // blanked. Starting that much later costs a fraction of a
        // millisecond once.
        const std::uint32_t settle =
            (noise_ != nullptr && noise_->blanking()) ? noise_->blanker_reach() : 0U;
        anchor_to(record.first_block + settle);
        started_ = true;
    }

    if (taps_pending_) {
        // A retune's tap table. The first barrier is the write-after-read
        // against the fine dispatch of the frame still in flight, which reads
        // this same buffer: a pipeline barrier's first scope covers every
        // command submitted earlier on this queue, so one barrier here is the
        // dependency and no semaphore is needed.
        if (auto wrote = taps_staging_[frame].write(
                std::as_bytes(std::span<const dsp::Complex32>(plan_.fine_taps)));
            !wrote) {
            return std::unexpected(with_context(wrote.error(), "receiver retune"));
        }
        record_barrier(record.commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT);

        VkBufferCopy region{};
        region.srcOffset = 0;
        region.dstOffset = 0;
        region.size = taps_bytes_;
        vkCmdCopyBuffer(record.commands, taps_staging_[frame].handle(), taps_.handle(), 1,
                        &region);

        // The display table moves with the mix, so it rides the same pair of
        // barriers rather than paying for its own.
        if (display_built_) {
            if (auto wrote = display_taps_staging_[frame].write(
                    std::as_bytes(std::span<const dsp::Complex32>(display_.taps)));
                !wrote) {
                return std::unexpected(with_context(wrote.error(), "receiver display retune"));
            }
            VkBufferCopy display_region{};
            display_region.size = display_taps_bytes_;
            vkCmdCopyBuffer(record.commands, display_taps_staging_[frame].handle(),
                            display_taps_.handle(), 1, &display_region);
        }

        record_barrier(record.commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_READ_BIT);
        taps_pending_ = false;
    }

    // --- the fine stage -----------------------------------------------------

    const dsp::SampleIndex newest = record.first_block + record.block_count - 1U;

    // What the fine stage reads: the channel ring, or with the blanker on
    // this receiver's blanked copy of its channel, which trails the channel
    // by the blanker's lead. core/engine/noise_stage.h.
    VkDescriptorSet fine_set = fine_set_;
    std::uint32_t fine_base = chan_base_;
    std::uint32_t fine_chan_mask = channel_ring_mask_;
    dsp::SampleIndex fine_newest = newest;
    if (noise_ != nullptr && noise_->blanking()) {
        auto next = dsp::fine_block(plan_, 0U, channel_ring_mask_, fine_mask_, next_output_, 1U);
        if (!next) {
            return std::unexpected(with_context(next.error(), "receiver blanker"));
        }
        auto source =
            noise_->record_blanker(record.commands, chan_base_, newest, next->oldest_input);
        if (!source) {
            return std::unexpected(with_context(source.error(), "receiver blanker"));
        }
        out.dispatches += source->dispatches;
        if (source->blanked) {
            fine_set = source->set;
            fine_base = 0U;
            fine_chan_mask = source->chan_mask;
            fine_newest = source->newest;
        }
    }

    const dsp::SampleIndex limit =
        highest_output_for(fine_newest, plan_.channel_rate, plan_.demod_rate);

    std::uint32_t count = 0;
    if (limit >= next_output_) {
        const dsp::SampleIndex available = limit + 1U - next_output_;
        count = static_cast<std::uint32_t>(
            std::min<dsp::SampleIndex>(available, max_outputs_));
    }

    if (count > 0) {
        auto block = dsp::fine_block(plan_, fine_base, fine_chan_mask, fine_mask_,
                                     next_output_, count);
        if (!block) {
            return std::unexpected(with_context(block.error(), "receiver fine block"));
        }
        if (newest - block->oldest_input >= channel_ring_blocks_) {
            // RE-ANCHORED, NOT REFUSED. The inputs this dispatch wants have
            // been overwritten and no answer exists for them, so the only
            // question is what to do next, and refusing was the wrong answer:
            // record() failing propagates out through Graph::on_block, the
            // source's delivery loop treats a refusing sink as the stream
            // ending, and the whole engine stops. An operator saw that as the
            // engine disappearing when they typed a frequency.
            //
            // A receiver whose inputs are gone is in the same position as one
            // that has just been created: the oldest thing it can honestly
            // filter is the oldest thing still in the ring. So it starts again
            // from here, which is exactly what the !started_ branch above
            // computes, and the samples in between stay missing because they
            // are missing.
            //
            // WHAT PUTS A RECEIVER HERE. Chiefly a device retune, which stops
            // the transfers for about a third of a second and declares the gap
            // through the source's overrun counters: the stream index jumps
            // that far, the channelizer jumps with it, and this stage's cursor
            // does not. That case is already reported, in
            // SourceStats::samples_lost and on the block's dropped_before, so
            // the skip here is the consequence of a loss somebody has been told
            // about rather than a second one.
            //
            // THE CASE NO SOURCE COUNTER REPORTS, which is what the two
            // numbers below are for: a receiver also arrives here because
            // this device could not keep up with the channelizer, with no
            // source-side overrun to go with it. One doing that repeatedly
            // skips repeatedly and sounds choppy. out.reanchors and
            // out.reanchor_frames_skipped go to the graph, which adds both to
            // this receiver's own totals, and VrxStatus::reanchors is where a
            // client reads them. Beside SourceStats::samples_lost they tell
            // the retune above apart from this: both move after a retune,
            // only these move when the machine loses the race.
            //
            // WHAT THIS PARAGRAPH USED TO SAY. Until 2026-09-21 it read
            // "Nothing counts that yet", and ended "Giving VrxStatus a
            // re-anchor count is the fix and it wants a wire field, so it is
            // written down rather than done here". The field exists now, on
            // VrxStatus and on the wire, so a receiver skipping for this
            // second reason is no longer silent.
            //
            // THE SKIPPED AMOUNT IS MEASURED AND NOT ESTIMATED. next_audio_
            // is where the demodulator would have written next, anchor_to
            // moves it, and the frames between the two are exactly the frame
            // indices nothing will ever produce. Saturating, because a
            // restart that did not move the cursor forward would otherwise
            // subtract unsigned into about 1.8e19 frames of reported loss,
            // and no loss is the honest answer to a restart that skipped
            // nothing.
            const dsp::SampleIndex resumed_from = next_audio_;
            anchor_to(record.first_block);
            if (noise_ != nullptr) {
                noise_->restart_blanker();
            }
            out.reanchors = 1;
            out.reanchor_frames_skipped =
                next_audio_ > resumed_from ? next_audio_ - resumed_from : 0;
            return out;
        }

        record_dispatch(record.commands, fine_pipeline_, fine_set,
                        std::as_bytes(std::span<const dsp::VrxFineParams>(&block->params, 1)),
                        group_count(count, local_size_x_));
        next_output_ += count;
        out.dispatches += 1;
    }

    // Recorded between the fine dispatch and the barrier that fences it, so
    // the two overlap: both read the channel ring and each writes its own.
    // The display ring's reader is the graph's transform, which records its
    // own barrier ahead of it.
    if (auto displayed = record_display(record, out); !displayed) {
        return std::unexpected(displayed.error());
    }

    if (count > 0) {
        record_barrier(record.commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_READ_BIT);
    }

    const auto dc = fine_dc_of(plan_);
    out.fine_dc_numerator = dc.first;
    out.fine_dc_denominator = dc.second;

    // --- the detector -------------------------------------------------------

    if (next_output_ == first_output_) {
        return out;
    }

    const auto decimation = static_cast<dsp::SampleIndex>(plan_.demod.decimation);
    const dsp::SampleIndex newest_fine = next_output_ - 1U;
    const dsp::SampleIndex audio_limit = newest_fine / decimation;

    std::uint32_t audio_count = 0;
    if (audio_limit >= next_audio_) {
        const dsp::SampleIndex available = audio_limit + 1U - next_audio_;
        audio_count =
            static_cast<std::uint32_t>(std::min<dsp::SampleIndex>(available, max_audio_));
    }
    if (audio_count == 0) {
        return out;
    }

    auto block = dsp::demod_block(plan_, fine_mask_, next_audio_, audio_count);
    if (!block) {
        return std::unexpected(with_context(block.error(), "receiver demod block"));
    }
    if (newest_fine - block->oldest_fine >= fine_capacity_) {
        return fail(std::format(
            "this dispatch would detect over fine samples [{}, {}] and the fine ring holds {}",
            block->oldest_fine, newest_fine, fine_capacity_));
    }

    const VkDeviceSize bytes =
        static_cast<VkDeviceSize>(audio_count) * plan_.demod.channels * sizeof(float);
    if (bytes > record.audio_bytes) {
        return fail(std::format(
            "this receiver produced {} audio frames of {} channels and the graph sized its "
            "readback for {} values",
            audio_count, plan_.demod.channels, record.audio_bytes / sizeof(float)));
    }

    record_dispatch(record.commands, demod_pipeline_, demod_sets_[frame],
                    std::as_bytes(std::span<const dsp::VrxDemodParams>(&block->params, 1)),
                    group_count(audio_count, local_size_x_));

    // The notch and the noise reduction rewrite the audio in place, between
    // the demodulator's write and the copy out, so the barrier below fences
    // whichever wrote last.
    if (noise_ != nullptr) {
        auto post = noise_->record_audio(record.commands, frame, audio_count);
        if (!post) {
            return std::unexpected(with_context(post.error(), "receiver audio stages"));
        }
        out.dispatches += *post;
    }

    record_barrier(record.commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT);

    VkBufferCopy region{};
    region.srcOffset = 0;
    region.dstOffset = 0;
    region.size = bytes;
    vkCmdCopyBuffer(record.commands, audio_[frame].handle(), record.audio_destination, 1,
                    &region);

    next_audio_ += audio_count;
    out.frames = audio_count;
    out.dispatches += 1;
    out.readbacks += 1;
    return out;
}

Status DemodStage::record_display(const StageRecord& record, StageOutput& out) {
    if (!display_built_ || !record.display) {
        // Off costs nothing, and the next time it is on the ring below the
        // cursor is whatever was there, so it starts again.
        display_running_ = false;
        return {};
    }
    if (!display_running_) {
        anchor_display(record.first_block);
        display_running_ = true;
    }

    const dsp::SampleIndex newest = record.first_block + record.block_count - 1U;
    const dsp::SampleIndex limit =
        highest_output_for(newest, display_.channel_rate, display_.display_rate);

    std::uint32_t count = 0;
    if (limit >= next_display_) {
        const dsp::SampleIndex available = limit + 1U - next_display_;
        count = static_cast<std::uint32_t>(
            std::min<dsp::SampleIndex>(available, max_display_outputs_));
    }

    if (count > 0) {
        auto block = dsp::display_block(display_, chan_base_, channel_ring_mask_, display_mask_,
                                        next_display_, count);
        if (!block) {
            return std::unexpected(with_context(block.error(), "receiver display block"));
        }
        if (newest - block->oldest_input >= channel_ring_blocks_) {
            // The inputs are gone, for the same reasons the fine stage's can
            // be. Nothing audible is lost here, so this is not counted as a
            // re-anchor: the display restarts from what the ring still holds
            // and the graph counts the frames it skips while the window
            // refills.
            anchor_display(record.first_block);
        } else {
            record_dispatch(record.commands, display_pipeline_, display_set_,
                            std::as_bytes(std::span<const dsp::VrxFineParams>(&block->params, 1)),
                            group_count(count, local_size_x_));
            next_display_ += count;
            out.dispatches += 1;
        }
    }

    out.display_from = display_from_;
    out.display_next = next_display_;
    out.display_rate = display_.display_rate;
    return {};
}

Status DemodStage::retune(const VrxParams& params, const VrxPlacement& placement) {
    VrxParams resolved = params;
    if (resolved.audio_rate == 0) {
        resolved.audio_rate = plan_.audio_rate;
    }

    auto planned = dsp::plan_vrx(grid_, source_rate_, resolved, placement);
    if (!planned) {
        return std::unexpected(with_context(planned.error(), "receiver retune"));
    }
    const dsp::VrxPlan& next = *planned;

    // A retune that changes the pipeline's shape is a different pipeline, a
    // different tap count and a different ring. Rebuilding those here means
    // destroying buffers that command buffers already in flight still name,
    // and this stage cannot see when those complete. So it is refused for the
    // same reason and in the same words the graph refuses a mode change.
    //
    // Graph::set_vrx_params asks THE SAME PREDICATE before it queues the
    // control op, so a caller normally gets this refusal on its own call
    // with the numbers in it. Kept because it is this stage's own invariant
    // and the graph's check is not allowed to be the only thing holding it:
    // a second entry point, or a graph edited to ask a looser question,
    // would find this.
    //
    // WHAT THIS PARAGRAPH USED TO SAY: that the refusal "now reaches
    // nobody". It reached nobody and it was not unreachable, because the
    // two checks were two hand-kept lists of fields and the graph's was
    // four entries shorter. dsp::VrxShape is now the one list and both
    // sides ask it, so arriving here means the two have come apart. The
    // graph counts it as GraphStats::vrx_retune_refusals rather than
    // dropping it on the floor.
    const dsp::VrxShape want = dsp::shape_of(next);
    const dsp::VrxShape have = dsp::shape_of(plan_);
    if (want != have) {
        return fail(dsp::describe_shape_change(have, want));
    }

    // The shape says the fine config is unchanged, and the tap table's
    // length is a function of that config, so the new table is exactly as
    // long as the buffers built for the old one. That is the sentence
    // VrxShape gives for not carrying the length, and this is the only
    // place it is load-bearing: the copy below runs on the recording thread
    // with taps_bytes_ fixed at build time, so a table that had come out a
    // different length would be copied short or read past its end and the
    // audio would carry the damage rather than a refusal.
    if (const Status sized = fine_taps_match_config(next); !sized) {
        return std::unexpected(with_context(sized.error(), "receiver retune"));
    }

    // The noise fields ride the same retune, as push constants the chain
    // builds from them. Asked before the display below commits anything, so
    // a refusal leaves the receiver exactly as it was.
    if (noise_ != nullptr) {
        const bool channel_moved = placement.channel != plan_.placement.channel;
        if (auto noise = noise_->retune(resolved, next, channel_moved); !noise) {
            return std::unexpected(with_context(noise.error(), "receiver retune"));
        }
    }

    // The display tap follows the tuning. Its pipeline, its table's length
    // and its ring are fixed whatever the rate, so this never needs a
    // rebuild; a new rate does need a new stream, because a transform window
    // cannot straddle two rates.
    if (display_built_) {
        auto display = dsp::plan_vrx_display(next);
        if (!display) {
            return std::unexpected(with_context(display.error(), "receiver display retune"));
        }
        if (display->fine != display_.fine || display->taps.size() != display_.taps.size()) {
            return fail(std::format(
                "a retune changed the display tap's shape from {} taps of {} phases to {} of "
                "{}, which the display pipeline was not built for",
                display_.fine.taps, display_.fine.phases, display->fine.taps,
                display->fine.phases));
        }
        if (display->display_rate != display_.display_rate) {
            display_running_ = false;
        }
        display_ = std::move(*display);
    }

    // Everything that survives is the tuning: the tap table's modulation, the
    // mixer's increment, the channel the receiver reads and the detector's
    // gain. The first is a copy the next recorded block makes; the rest are
    // push constants this stage builds from the plan every block.
    plan_ = std::move(*planned);
    chan_base_ = placement.channel * channel_ring_blocks_;
    taps_pending_ = true;
    return {};
}

}  // namespace

void install_default_vrx_stages() {
    install_vrx_stage_factory(
        [](const VrxStageRequest& request) -> Expected<std::unique_ptr<VrxStage>> {
            // A probe's Raw is the exception, and the reason it is decided
            // here: core/engine/graph.h, VrxStageRequest::
            // fine_stage_complex_tap. It gets the fine stage and the kernel's
            // Raw passthrough, the same path the digital voice modes take.
            if (request.params.demod == Demod::Raw && !request.fine_stage_complex_tap) {
                // Declined, not failed. The graph's own raw tap is a buffer
                // copy of a channel that is already contiguous, and a null
                // stage is how the seam says so.
                //
                // WHAT THIS BRANCH USED TO SAY. Until 2026-09-22 it declined
                // every complex tap and read "The digital voice modes decline
                // here too: what they want is that same contiguous complex
                // baseband, and core/decode does the rest on the host." It
                // was not what they want. The raw tap hands a decoder one
                // whole coarse channel at the channel rate with the carrier
                // wherever the residual left it, and every decoder in
                // core/decode assumes a carrier at DC, a channel filter and
                // its own sample rate. They are built here now, as a fine
                // stage and the kernel's complex passthrough, and deliver
                // exactly that.
                return std::unique_ptr<VrxStage>{};
            }
            return DemodStage::create(request);
        });
}

}  // namespace revenant::engine
