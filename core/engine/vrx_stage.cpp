// The fine stage and the seven demodulators, recorded into the graph's
// command buffer.
//
// Two dispatches per receiver per block. core/shaders/vrx_fine.comp mixes the
// residual to DC, filters to the requested bandwidth and resamples the coarse
// channel to the demodulation rate, writing a per-receiver ring.
// core/shaders/vrx_demod.comp detects out of that ring and writes real audio,
// which is then copied into the graph's readback buffer. Both kernels are
// proved bit-exact against their twins in tests/reference/test_vrx.cpp; this
// file is the plumbing that gets them the right push constants.
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

// How far below its own sample the detector reads. Named rather than folded
// into one worst case because AM's DC-removal window is hundreds of samples
// and everything else is one or none, and sizing every receiver for AM would
// multiply every fine ring in the machine.
[[nodiscard]] std::uint32_t detector_history(std::uint32_t mode, std::uint32_t dc_taps) {
    if (mode == dsp::kDemodAm) {
        return dc_taps - 1U;
    }
    if (mode == dsp::kDemodNfm || mode == dsp::kDemodWfm) {
        return 1U;
    }
    return 0U;
}

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

[[nodiscard]] bool same_shape(const dsp::VrxFineConfig& a, const dsp::VrxFineConfig& b) {
    return a.taps == b.taps && a.phases == b.phases && a.nco_log2 == b.nco_log2;
}

[[nodiscard]] bool same_shape(const dsp::VrxDemodConfig& a, const dsp::VrxDemodConfig& b) {
    return a.mode == b.mode && a.decimation == b.decimation && a.audio_taps == b.audio_taps &&
           a.dc_taps == b.dc_taps;
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
        return static_cast<VkDeviceSize>(audio_for(outputs_for(blocks))) * sizeof(float);
    }

    [[nodiscard]] Expected<StageOutput> record(const StageRecord& record) override;

    [[nodiscard]] Status retune(const VrxParams& params,
                                const VrxPlacement& placement) override;

private:
    DemodStage() = default;

    [[nodiscard]] Status build(const VrxStageRequest& request);
    [[nodiscard]] Status build_pipelines();
    [[nodiscard]] Status build_buffers(VkBuffer channel_ring);
    [[nodiscard]] Status build_descriptors(VkBuffer channel_ring);

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

    bool started_ = false;
    bool taps_pending_ = false;
    dsp::SampleIndex first_output_ = 0;
    dsp::SampleIndex next_output_ = 0;
    dsp::SampleIndex next_audio_ = 0;
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
    // and a dispatch advances the input by a whole block. Both have to be
    // live in the channel ring at once or the kernel's masked read returns
    // the far end of the ring and quietly filters the wrong samples.
    const std::uint64_t channel_span = blocks + plan_.fine.taps;
    if (channel_span > channel_ring_blocks_) {
        return fail(std::format(
            "a {} tap fine filter over a {} block dispatch needs {} channel samples live and "
            "the channel ring holds {}. Either the receiver is too narrow for this grid or the "
            "engine's ring_seconds is too short",
            plan_.fine.taps, blocks, channel_span, channel_ring_blocks_));
    }

    max_outputs_ = static_cast<std::uint32_t>(outputs_for(blocks));
    max_audio_ = static_cast<std::uint32_t>(audio_for(max_outputs_));
    fine_history_ =
        (plan_.demod.audio_taps - 1U) + detector_history(plan_.demod.mode, plan_.demod.dc_taps);

    // Room for every dispatch that can be in flight at once, plus the
    // detector's history, plus the one the recording thread is filling. A
    // ring exactly one dispatch long would have the detector reading samples
    // the next fine dispatch had already overwritten, which is silent.
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

    if (auto built = build_pipelines(); !built) {
        return built;
    }
    if (auto built = build_buffers(request.channel_ring); !built) {
        return built;
    }
    return build_descriptors(request.channel_ring);
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
        const std::uint32_t constants[] = {plan_.demod.mode, plan_.demod.decimation,
                                           plan_.demod.audio_taps, plan_.demod.dc_taps};
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

    taps_bytes_ = plan_.fine_taps.size() * sizeof(dsp::Complex32);
    const VkDeviceSize weight_bytes = plan_.demod_weights.size() * sizeof(float);
    const VkDeviceSize fine_bytes = static_cast<VkDeviceSize>(fine_capacity_) * kComplexBytes;
    const VkDeviceSize audio_bytes = static_cast<VkDeviceSize>(max_audio_) * sizeof(float);

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
    clears.reserve(1 + audio_.size());
    clears.push_back({fine_ring_.handle(), fine_ring_.size()});
    for (auto& buffer : audio_) {
        clears.push_back({buffer.handle(), buffer.size()});
    }
    if (auto cleared = runner->clear(clears); !cleared) {
        return std::unexpected(with_context(cleared.error(), "receiver clear"));
    }

    const gpu::CommandRunner::BufferCopy copies[] = {
        {taps_staging_[0].handle(), taps_.handle(), taps_bytes_},
        {weight_staging->handle(), weights_.handle(), weight_bytes},
    };
    if (auto copied = runner->copy(copies); !copied) {
        return std::unexpected(with_context(copied.error(), "receiver upload"));
    }
    return {};
}

Status DemodStage::build_descriptors(VkBuffer channel_ring) {
    // The fine set binds nothing that varies per frame, so there is one of
    // it. The detector writes into a per-frame scratch buffer, so there is
    // one of those per frame in flight.
    const std::uint32_t sets = 1U + frames_in_flight_;
    const std::uint32_t buffers = 4U + 3U * frames_in_flight_;

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
    out.channels = 1;
    out.rate = plan_.output_rate;

    if (record.block_count == 0) {
        return out;
    }
    if (record.frame_index >= frames_in_flight_) {
        return fail(std::format("a receiver was recorded into frame {} of {}",
                                record.frame_index, frames_in_flight_));
    }
    const std::uint32_t frame = record.frame_index;

    if (!started_) {
        // The first output whose filter support is entirely inside samples
        // this graph has actually channelized. Starting lower would filter
        // zeros and produce a transient nobody asked for, and starting from
        // the ring's contents would filter whatever the allocator left.
        const dsp::SampleIndex needed =
            record.first_block + static_cast<dsp::SampleIndex>(plan_.fine.taps - 1U);
        first_output_ = lowest_output_for(needed, plan_.channel_rate, plan_.demod_rate);
        next_output_ = first_output_;

        const auto decimation = static_cast<dsp::SampleIndex>(plan_.demod.decimation);
        next_audio_ = (first_output_ + fine_history_ + decimation - 1U) / decimation;
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

        record_barrier(record.commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_READ_BIT);
        taps_pending_ = false;
    }

    // --- the fine stage -----------------------------------------------------

    const dsp::SampleIndex newest = record.first_block + record.block_count - 1U;
    const dsp::SampleIndex limit =
        highest_output_for(newest, plan_.channel_rate, plan_.demod_rate);

    std::uint32_t count = 0;
    if (limit >= next_output_) {
        const dsp::SampleIndex available = limit + 1U - next_output_;
        count = static_cast<std::uint32_t>(
            std::min<dsp::SampleIndex>(available, max_outputs_));
    }

    if (count > 0) {
        auto block = dsp::fine_block(plan_, chan_base_, channel_ring_mask_, fine_mask_,
                                     next_output_, count);
        if (!block) {
            return std::unexpected(with_context(block.error(), "receiver fine block"));
        }
        if (newest - block->oldest_input >= channel_ring_blocks_) {
            return fail(std::format(
                "this dispatch would filter channel samples [{}, {}] and the channel ring only "
                "holds the last {}. The receiver fell behind the channelizer",
                block->oldest_input, newest, channel_ring_blocks_));
        }

        record_dispatch(record.commands, fine_pipeline_, fine_set_,
                        std::as_bytes(std::span<const dsp::VrxFineParams>(&block->params, 1)),
                        group_count(count, local_size_x_));
        record_barrier(record.commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_READ_BIT);
        next_output_ += count;
    }

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

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(audio_count) * sizeof(float);
    if (bytes > record.audio_bytes) {
        return fail(std::format(
            "this receiver produced {} audio samples and the graph sized its readback for {}",
            audio_count, record.audio_bytes / sizeof(float)));
    }

    record_dispatch(record.commands, demod_pipeline_, demod_sets_[frame],
                    std::as_bytes(std::span<const dsp::VrxDemodParams>(&block->params, 1)),
                    group_count(audio_count, local_size_x_));
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
    return out;
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
    if (!same_shape(next.fine, plan_.fine) || !same_shape(next.demod, plan_.demod) ||
        next.channel_rate != plan_.channel_rate || next.demod_rate != plan_.demod_rate ||
        next.output_rate != plan_.output_rate ||
        next.fine_taps.size() != plan_.fine_taps.size()) {
        return fail(std::format(
            "this retune changes the receiver's filter shape, not just where it is pointed: "
            "{} taps at {} S/s becomes {} taps at {} S/s. Moving the dial is a push constant "
            "and a new tap table, which is free; changing the bandwidth or the audio rate is a "
            "remove and an add",
            plan_.fine.taps, plan_.demod_rate, next.fine.taps, next.demod_rate));
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
            if (request.params.demod == Demod::Raw) {
                // Declined, not failed. The graph's own raw tap is a buffer
                // copy of a channel that is already contiguous, and a null
                // stage is how the seam says so.
                return std::unique_ptr<VrxStage>{};
            }
            return DemodStage::create(request);
        });
}

}  // namespace revenant::engine
