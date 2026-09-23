#include "core/engine/noise_stage.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <new>
#include <span>
#include <utility>
#include <vector>

#include "core/dsp/vrx_reference.h"
#include "core/engine/record_util.h"
#include "core/gpu/shaders.h"

namespace revenant::engine {
namespace {

constexpr VkBufferUsageFlags kDeviceStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

// The spectral stage's workgroup. Its answer does not depend on this, since
// every bin and every sample is one invocation's ascending sum; 256 finishes
// a frame's two transforms in a quarter of the serial steps 64 would, and a
// device that caps lower gets its own limit.
constexpr std::uint32_t kSpectralLocalSize = 256;

[[nodiscard]] Expected<gpu::Buffer> make_device(const gpu::Context& context, VkDeviceSize bytes,
                                                const char* what) {
    auto buffer = gpu::Buffer::create(context, std::max<VkDeviceSize>(bytes, 4U), kDeviceStorage,
                                      gpu::MemoryKind::DeviceLocal);
    if (!buffer) {
        return std::unexpected(with_context(buffer.error(), what));
    }
    return buffer;
}

void fill_zero(VkCommandBuffer commands, const gpu::Buffer& buffer) {
    // The previous frame's dispatch may still be reading or writing it, and
    // a pipeline barrier's first scope covers every command submitted before
    // this one on the queue.
    record_barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(commands, buffer.handle(), 0, VK_WHOLE_SIZE, 0U);
    record_barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

}  // namespace

Expected<std::unique_ptr<NoiseChain>> NoiseChain::create(const NoiseChainRequest& request,
                                                         const VrxParams& params,
                                                         const dsp::VrxPlan& plan) {
    std::unique_ptr<NoiseChain> chain(new (std::nothrow) NoiseChain());
    if (chain == nullptr) {
        return fail("could not allocate a receiver's noise chain");
    }
    if (auto built = chain->build(request, params, plan); !built) {
        return std::unexpected(with_context(built.error(), "receiver noise chain"));
    }
    return chain;
}

NoiseChain::~NoiseChain() {
    if (descriptors_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptors_, nullptr);
    }
}

Status NoiseChain::build(const NoiseChainRequest& request, const VrxParams& params,
                         const dsp::VrxPlan& plan) {
    if (request.context == nullptr) {
        return fail("no Vulkan context");
    }
    context_ = request.context;
    device_ = context_->device();
    frames_in_flight_ = std::max<std::uint32_t>(1, request.frames_in_flight);
    local_size_x_ =
        request.local_size_x != 0 ? request.local_size_x : gpu::kDefaultLocalSizeX;
    channel_ring_mask_ = request.channel_ring_mask;
    max_audio_ = request.max_audio;
    if (request.audio.size() != frames_in_flight_) {
        return fail(std::format("{} audio buffers for {} frames in flight", request.audio.size(),
                                frames_in_flight_));
    }

    // --- the blanker's figures and its ring ---------------------------------

    blank_config_ = dsp::design_blanker(request.channel_rate);
    const std::uint64_t reach =
        static_cast<std::uint64_t>(dsp::blanker_reach_below(blank_config_)) + blank_config_.lead;

    // Everything the frames in flight can still be reading, plus the
    // blanker's own reach. A ring shorter than that lets this dispatch
    // overwrite samples a fine dispatch submitted earlier is still filtering.
    //
    // Never longer than the channel ring's own channel. The fine stage
    // already runs against that length with the same frames in flight, and
    // the blanked copy is written exactly as the channel is, one lead behind
    // it, so a ring that long is always enough. The bound matters because
    // this estimate is written against the dispatch size and the channel
    // ring's depth is the engine's decision: a receiver must never fail to
    // build over a blanker it may not even be using.
    const std::uint64_t live =
        static_cast<std::uint64_t>(request.max_fine_span) * frames_in_flight_ + reach + 2U;
    const std::uint64_t channel_ring = static_cast<std::uint64_t>(channel_ring_mask_) + 1U;
    const std::uint64_t ring = std::min(std::bit_ceil(live), channel_ring);
    if (ring <= reach + 1U) {
        return fail(std::format("a {} sample channel ring cannot hold the blanker's reach of {}",
                                channel_ring, reach));
    }
    blank_mask_ = static_cast<std::uint32_t>(ring - 1U);

    // The most one dispatch may blank: what the ring holds with the reach
    // taken off, which validate() below insists on.
    max_blank_count_ = static_cast<std::uint32_t>(ring - reach - 1U);

    line_config_ = dsp::LineConfig{};
    spectral_config_ = dsp::spectral_config_for(plan.output_rate);

    // --- pipelines ------------------------------------------------------------

    auto make_pipeline = [&](std::span<const std::uint32_t> spirv, std::uint32_t buffers,
                             std::uint32_t push_bytes, std::uint32_t local_size,
                             std::span<const std::uint32_t> constants,
                             const char* what) -> Expected<gpu::ComputePipeline> {
        gpu::ComputePipeline::Options options;
        options.spirv = spirv;
        options.storage_buffer_count = buffers;
        options.local_size_x = local_size;
        options.push_constant_bytes = push_bytes;
        options.grid_constants = constants;
        auto pipeline = gpu::ComputePipeline::create(*context_, options);
        if (!pipeline) {
            return std::unexpected(with_context(pipeline.error(), what));
        }
        return pipeline;
    };

    {
        const std::uint32_t detect[] = {dsp::kBlankDetect, blank_config_.window,
                                        blank_config_.guard, blank_config_.hang,
                                        blank_config_.lead};
        auto pipeline = make_pipeline(gpu::shaders::noise_blank(), 3, sizeof(dsp::BlankerParams),
                                      local_size_x_, detect, "blanker detect pipeline");
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        detect_pipeline_ = std::move(*pipeline);

        const std::uint32_t apply[] = {dsp::kBlankApply, blank_config_.window,
                                       blank_config_.guard, blank_config_.hang,
                                       blank_config_.lead};
        pipeline = make_pipeline(gpu::shaders::noise_blank(), 3, sizeof(dsp::BlankerParams),
                                 local_size_x_, apply, "blanker apply pipeline");
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        apply_pipeline_ = std::move(*pipeline);
    }
    {
        // The local size IS the lane count, so no scheduling choice can
        // regroup the predictor's sums. core/shaders/noise_line.comp.
        const std::uint32_t constants[] = {line_config_.lanes, line_config_.taps,
                                           line_config_.history};
        auto pipeline = make_pipeline(gpu::shaders::noise_line(), 2, sizeof(dsp::LineParams),
                                      line_config_.lanes, constants, "notch pipeline");
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        line_pipeline_ = std::move(*pipeline);
    }
    {
        const std::uint32_t constants[] = {spectral_config_.frame};
        const std::uint32_t local =
            std::min(kSpectralLocalSize, context_->info().max_workgroup_size_x);
        auto pipeline =
            make_pipeline(gpu::shaders::noise_spectral(), 3, sizeof(dsp::SpectralParams), local,
                          constants, "noise reduction pipeline");
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        spectral_pipeline_ = std::move(*pipeline);
    }

    // --- buffers ----------------------------------------------------------------

    auto flags = make_device(*context_, ring * sizeof(float), "blanker flags");
    if (!flags) {
        return std::unexpected(flags.error());
    }
    flags_ = std::move(*flags);

    auto blanked = make_device(*context_, ring * sizeof(dsp::Complex32), "blanked ring");
    if (!blanked) {
        return std::unexpected(blanked.error());
    }
    blanked_ = std::move(*blanked);

    auto line_state =
        make_device(*context_, dsp::line_state_size(line_config_) * sizeof(float), "notch state");
    if (!line_state) {
        return std::unexpected(line_state.error());
    }
    line_state_ = std::move(*line_state);

    auto spectral_state = make_device(
        *context_, dsp::spectral_state_size(spectral_config_) * sizeof(float),
        "noise reduction state");
    if (!spectral_state) {
        return std::unexpected(spectral_state.error());
    }
    spectral_state_ = std::move(*spectral_state);

    const std::vector<float> tables = dsp::design_spectral_tables(spectral_config_);
    const VkDeviceSize table_bytes = tables.size() * sizeof(float);
    auto spectral_tables = make_device(*context_, table_bytes, "noise reduction tables");
    if (!spectral_tables) {
        return std::unexpected(spectral_tables.error());
    }
    spectral_tables_ = std::move(*spectral_tables);

    {
        auto staging = gpu::Buffer::create(*context_, table_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                           gpu::MemoryKind::Upload);
        if (!staging) {
            return std::unexpected(with_context(staging.error(), "noise reduction staging"));
        }
        if (auto wrote = staging->write(std::as_bytes(std::span<const float>(tables))); !wrote) {
            return std::unexpected(with_context(wrote.error(), "noise reduction staging"));
        }
        auto runner = gpu::CommandRunner::create(*context_);
        if (!runner) {
            return std::unexpected(with_context(runner.error(), "noise chain upload"));
        }
        // Zeroed because zero is each stage's initial state: no flags, no
        // samples blanked, a notch at rest, an empty frame.
        const gpu::CommandRunner::BufferClear clears[] = {
            {flags_.handle(), flags_.size()},
            {blanked_.handle(), blanked_.size()},
            {line_state_.handle(), line_state_.size()},
            {spectral_state_.handle(), spectral_state_.size()},
        };
        if (auto cleared = runner->clear(clears); !cleared) {
            return std::unexpected(with_context(cleared.error(), "noise chain clear"));
        }
        const gpu::CommandRunner::BufferCopy copies[] = {
            {staging->handle(), spectral_tables_.handle(), table_bytes},
        };
        if (auto copied = runner->copy(copies); !copied) {
            return std::unexpected(with_context(copied.error(), "noise chain upload"));
        }
    }

    // --- descriptor sets ------------------------------------------------------

    const std::uint32_t sets = 2U + 2U * frames_in_flight_;
    const std::uint32_t descriptors = 3U + 4U + 2U * frames_in_flight_ + 3U * frames_in_flight_;

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = descriptors;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = sets;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;

    VkResult result = vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptors_);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkCreateDescriptorPool failed for a noise chain ({})",
                                gpu::result_name(result)),
                    result);
    }

    auto allocate = [&](VkDescriptorSetLayout layout, VkDescriptorSet& set,
                        std::span<const VkBuffer> bound, const char* what) -> Status {
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = descriptors_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &layout;
        const VkResult allocated = vkAllocateDescriptorSets(device_, &alloc, &set);
        if (allocated != VK_SUCCESS) {
            return fail(std::format("vkAllocateDescriptorSets failed for the {} ({})", what,
                                    gpu::result_name(allocated)),
                        allocated);
        }
        if (auto wrote = write_storage_set(device_, set, bound); !wrote) {
            return std::unexpected(with_context(wrote.error(), what));
        }
        return {};
    };

    // One set serves both blanker passes: the two pipelines come from one
    // module with the same three bindings, so their set layouts are
    // identically defined and therefore compatible.
    {
        const VkBuffer bound[] = {request.channel_ring, flags_.handle(), blanked_.handle()};
        if (auto made = allocate(detect_pipeline_.descriptor_layout(), blank_set_, bound,
                                 "blanker set");
            !made) {
            return made;
        }
    }
    {
        const VkBuffer bound[] = {blanked_.handle(), request.fine_taps, request.nco,
                                  request.fine_ring};
        if (auto made = allocate(request.fine_layout, fine_set_, bound, "blanked fine set");
            !made) {
            return made;
        }
    }
    line_sets_.assign(frames_in_flight_, VK_NULL_HANDLE);
    spectral_sets_.assign(frames_in_flight_, VK_NULL_HANDLE);
    for (std::uint32_t i = 0; i < frames_in_flight_; ++i) {
        const VkBuffer line_bound[] = {request.audio[i], line_state_.handle()};
        if (auto made =
                allocate(line_pipeline_.descriptor_layout(), line_sets_[i], line_bound, "notch set");
            !made) {
            return made;
        }
        const VkBuffer spectral_bound[] = {request.audio[i], spectral_tables_.handle(),
                                           spectral_state_.handle()};
        if (auto made = allocate(spectral_pipeline_.descriptor_layout(), spectral_sets_[i],
                                 spectral_bound, "noise reduction set");
            !made) {
            return made;
        }
    }

    auto planned = dsp::plan_noise(params, plan, line_config_);
    if (!planned) {
        return std::unexpected(planned.error());
    }
    // Fresh buffers are already zero, so nothing needs resetting.
    plan_ = *planned;
    return {};
}

Status NoiseChain::retune(const VrxParams& params, const dsp::VrxPlan& plan,
                          bool channel_moved) {
    auto planned = dsp::plan_noise(params, plan, line_config_);
    if (!planned) {
        return std::unexpected(planned.error());
    }
    if (dsp::spectral_config_for(plan.output_rate) != spectral_config_) {
        // The audio rate is part of the receiver's shape, so a retune cannot
        // move it; this is the stage saying so rather than reading a table
        // built for another frame.
        return fail("a retune moved the audio rate under the noise reduction's frame");
    }
    return adopt(*planned, channel_moved);
}

Status NoiseChain::adopt(const dsp::NoisePlan& next, bool channel_moved) {
    // A stage switching on starts from rest rather than from whatever it
    // held when it was last switched off, which is audio from another time.
    // The predictor starts again too when its stride or delay moves, because
    // its weights were learned against a different reference.
    const bool ale_before = plan_.line && (plan_.line_params.flags & dsp::kLineAle) != 0U;
    const bool ale_after = next.line && (next.line_params.flags & dsp::kLineAle) != 0U;
    const bool ale_moved = ale_before && ale_after &&
                           (plan_.line_params.stride != next.line_params.stride ||
                            plan_.line_params.delay != next.line_params.delay);
    if ((next.line && !plan_.line) || (ale_after && !ale_before) || ale_moved) {
        line_reset_pending_ = true;
    }
    if (next.spectral && !plan_.spectral) {
        spectral_reset_pending_ = true;
    }
    if (!next.blank || channel_moved) {
        blank_running_ = false;
    }
    plan_ = next;
    return {};
}

Expected<NoiseChain::FineSource> NoiseChain::record_blanker(VkCommandBuffer commands,
                                                            std::uint32_t chan_base,
                                                            dsp::SampleIndex newest,
                                                            dsp::SampleIndex fine_oldest) {
    FineSource source;
    if (!plan_.blank) {
        blank_running_ = false;
        return source;
    }
    const std::uint32_t hang = blank_config_.hang;
    const std::uint32_t lead = blank_config_.lead;

    if (!blank_running_) {
        // Start where the fine stage next reads, and detect far enough below
        // that for the apply pass's hang, so no flag it reads is left over
        // from before.
        if (fine_oldest < hang) {
            return source;
        }
        apply_next_ = fine_oldest;
        detect_next_ = fine_oldest - hang;
        blank_running_ = true;
    }

    // The detect pass reads a window below its first sample. If the channel
    // ring no longer holds it the samples it wanted are gone, which is the
    // case the fine stage is about to re-anchor on; stand aside for this
    // block and start again from wherever the fine stage lands.
    const std::uint64_t channel_ring = static_cast<std::uint64_t>(channel_ring_mask_) + 1U;
    if (detect_next_ < blank_config_.guard + blank_config_.window ||
        newest - (detect_next_ - blank_config_.guard - blank_config_.window) >= channel_ring ||
        detect_next_ > newest + 1U) {
        blank_running_ = false;
        return source;
    }

    const std::uint64_t pending = newest + 1U - detect_next_;
    const auto detect_count =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(pending, max_blank_count_));

    dsp::BlankerParams params;
    params.chan_base = chan_base;
    params.chan_mask = channel_ring_mask_;
    params.out_mask = blank_mask_;
    params.threshold = plan_.blank_threshold;

    if (detect_count > 0) {
        params.chan_first = static_cast<std::uint32_t>(detect_next_ & channel_ring_mask_);
        params.out_first = static_cast<std::uint32_t>(detect_next_ & blank_mask_);
        params.count = detect_count;
        record_dispatch(commands, detect_pipeline_, blank_set_,
                        std::as_bytes(std::span<const dsp::BlankerParams>(&params, 1)),
                        group_count(detect_count, local_size_x_));
        detect_next_ += detect_count;
        source.dispatches += 1;
    }

    // Everything flagged so far lets the apply pass run to the lead below it.
    const dsp::SampleIndex apply_end = detect_next_ > lead ? detect_next_ - lead : 0U;
    std::uint32_t apply_count = 0;
    if (apply_end > apply_next_) {
        apply_count = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(apply_end - apply_next_, max_blank_count_));
    }
    if (apply_count > 0) {
        record_barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        params.chan_first = static_cast<std::uint32_t>(apply_next_ & channel_ring_mask_);
        params.out_first = static_cast<std::uint32_t>(apply_next_ & blank_mask_);
        params.count = apply_count;
        record_dispatch(commands, apply_pipeline_, blank_set_,
                        std::as_bytes(std::span<const dsp::BlankerParams>(&params, 1)),
                        group_count(apply_count, local_size_x_));
        apply_next_ += apply_count;
        source.dispatches += 1;
        record_barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    source.blanked = true;
    source.set = fine_set_;
    source.chan_mask = blank_mask_;
    // Nothing at or above apply_next_ has been blanked, so the fine stage
    // stops below it. apply_next_ starts at the fine stage's oldest input,
    // which is above zero once the receiver is anchored.
    source.newest = apply_next_ > 0U ? apply_next_ - 1U : 0U;
    return source;
}

Expected<std::uint32_t> NoiseChain::record_audio(VkCommandBuffer commands, std::uint32_t frame,
                                                 std::uint32_t count) {
    if ((!plan_.line && !plan_.spectral) || count == 0) {
        return 0U;
    }
    if (frame >= frames_in_flight_) {
        return fail(std::format("the noise chain was recorded into frame {} of {}", frame,
                                frames_in_flight_));
    }
    if (count > max_audio_) {
        return fail(std::format("the noise chain was handed {} audio frames and was built for "
                                "{}",
                                count, max_audio_));
    }

    std::uint32_t dispatches = 0;

    // The demodulator's write of this frame's audio, before either stage
    // reads it.
    record_barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    if (plan_.line) {
        if (line_reset_pending_) {
            fill_zero(commands, line_state_);
            line_reset_pending_ = false;
        }
        dsp::LineParams params = plan_.line_params;
        params.count = count;
        record_dispatch(commands, line_pipeline_, line_sets_[frame],
                        std::as_bytes(std::span<const dsp::LineParams>(&params, 1)), 1U);
        dispatches += 1;
    }

    if (plan_.spectral) {
        if (plan_.line) {
            record_barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        }
        if (spectral_reset_pending_) {
            fill_zero(commands, spectral_state_);
            spectral_reset_pending_ = false;
        }
        dsp::SpectralParams params = plan_.spectral_params;
        params.count = count;
        record_dispatch(commands, spectral_pipeline_, spectral_sets_[frame],
                        std::as_bytes(std::span<const dsp::SpectralParams>(&params, 1)), 1U);
        dispatches += 1;
    }
    return dispatches;
}

}  // namespace revenant::engine
