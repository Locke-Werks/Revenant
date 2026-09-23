// A receiver's noise mitigation on the device: the impulse blanker ahead of
// its fine stage, and the two audio stages after its demodulator.
//
// Owned by the DemodStage in core/engine/vrx_stage.cpp and called from it at
// three points, which are the whole of the seam: before the fine dispatch,
// to blank the channel and say which ring the fine stage reads; after the
// demodulator's dispatch, to run the notch and the noise reduction over the
// audio in place before it is copied out; and on a retune. The arithmetic is
// core/dsp/noise_reference.h's, and the kernels are proved against its twins
// in tests/reference/test_noise.cpp.
//
// OFF COSTS NOTHING PER BLOCK. Every pipeline and buffer is built with the
// receiver, because a retune runs on the recording thread and must not
// allocate, so turning a stage on is a push constant. A receiver with all
// three off records no dispatch and no barrier for any of them.
//
// THE BLANKER'S RING. The fine stage normally reads the channel ring the
// channelizer writes. With the blanker on it reads this receiver's blanked
// ring instead, through a second descriptor set bound to it, with chan_base
// zero and the blanked ring's own mask. The blanked ring is short, sized to
// what the frames in flight read, so it is not a second copy of the engine's
// ring_seconds per receiver. The blanker trails the channel by its lead,
// kLead channel samples, so the fine stage is held that far behind the
// newest block while it is on.

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <vulkan/vulkan.h>

#include "core/dsp/noise_reference.h"
#include "core/dsp/types.h"
#include "core/engine/vrx.h"
#include "core/error.h"
#include "core/gpu/buffer.h"
#include "core/gpu/context.h"
#include "core/gpu/kernel.h"

namespace revenant::dsp {
struct VrxPlan;
}

namespace revenant::engine {

struct NoiseChainRequest {
    const gpu::Context* context = nullptr;
    std::uint32_t frames_in_flight = 1;
    std::uint32_t local_size_x = 0;

    dsp::SampleRate channel_rate = 0;
    VkBuffer channel_ring = VK_NULL_HANDLE;
    std::uint32_t channel_ring_mask = 0;

    // The most channel samples one fine dispatch reads, its block span plus
    // its filter's reach, which is what sizes the blanked ring.
    std::uint32_t max_fine_span = 0;

    // Everything the fine pipeline binds besides the ring it reads, so the
    // chain can build the second fine set once, at construction.
    VkDescriptorSetLayout fine_layout = VK_NULL_HANDLE;
    VkBuffer fine_taps = VK_NULL_HANDLE;
    VkBuffer nco = VK_NULL_HANDLE;
    VkBuffer fine_ring = VK_NULL_HANDLE;

    // The demodulator's per-frame audio scratch, which the audio stages
    // rewrite in place, and the most frames one dispatch writes there.
    std::span<const VkBuffer> audio;
    std::uint32_t max_audio = 0;
};

class NoiseChain {
public:
    [[nodiscard]] static Expected<std::unique_ptr<NoiseChain>> create(
        const NoiseChainRequest& request, const VrxParams& params, const dsp::VrxPlan& plan);

    ~NoiseChain();

    NoiseChain(const NoiseChain&) = delete;
    NoiseChain& operator=(const NoiseChain&) = delete;
    NoiseChain(NoiseChain&&) = delete;
    NoiseChain& operator=(NoiseChain&&) = delete;

    // A retune's noise fields and plan. Refuses only what plan_noise refuses,
    // which engine::place has already refused for any request that reached
    // here through the engine. `channel_moved` restarts the blanker, whose
    // ring held the previous channel.
    [[nodiscard]] Status retune(const VrxParams& params, const dsp::VrxPlan& plan,
                                bool channel_moved);

    [[nodiscard]] bool blanking() const { return plan_.blank; }

    // How far below its first output the blanker reads. A receiver started
    // with the blanker on is anchored this much later, so its first reference
    // window is channel samples and not the ring's clear value.
    [[nodiscard]] std::uint32_t blanker_reach() const {
        return dsp::blanker_reach_below(blank_config_);
    }

    // What the fine stage reads this block.
    struct FineSource {
        // False when the fine stage reads the channel ring as it always has.
        bool blanked = false;
        VkDescriptorSet set = VK_NULL_HANDLE;
        std::uint32_t chan_mask = 0;

        // The newest index the fine stage may read. Blanked samples trail the
        // channel by the blanker's lead.
        dsp::SampleIndex newest = 0;

        std::uint32_t dispatches = 0;
    };

    // Blanks what the channel has added since the last block, up to the lead
    // behind `newest`, starting from `fine_oldest` when the blanker has only
    // just been switched on. Returns the channel ring unchanged when the
    // blanker is off, and when the ring no longer holds its window, which is
    // the case the fine stage is about to re-anchor on anyway.
    [[nodiscard]] Expected<FineSource> record_blanker(VkCommandBuffer commands,
                                                      std::uint32_t chan_base,
                                                      dsp::SampleIndex newest,
                                                      dsp::SampleIndex fine_oldest);

    // The fine stage re-anchored, so the blanked stream starts again with it.
    void restart_blanker() { blank_running_ = false; }

    // Runs the line stage and the spectral stage over the frame's audio in
    // place, between the demodulator's dispatch and the copy out, and returns
    // how many dispatches it recorded.
    [[nodiscard]] Expected<std::uint32_t> record_audio(VkCommandBuffer commands,
                                                       std::uint32_t frame,
                                                       std::uint32_t count);

private:
    NoiseChain() = default;

    [[nodiscard]] Status build(const NoiseChainRequest& request, const VrxParams& params,
                               const dsp::VrxPlan& plan);
    [[nodiscard]] Status adopt(const dsp::NoisePlan& next, bool channel_moved);

    const gpu::Context* context_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    std::uint32_t frames_in_flight_ = 1;
    std::uint32_t local_size_x_ = gpu::kDefaultLocalSizeX;
    std::uint32_t channel_ring_mask_ = 0;
    std::uint32_t max_audio_ = 0;

    dsp::NoisePlan plan_{};

    dsp::BlankerConfig blank_config_{};
    std::uint32_t blank_mask_ = 0;
    std::uint32_t max_blank_count_ = 0;
    bool blank_running_ = false;
    dsp::SampleIndex detect_next_ = 0;
    dsp::SampleIndex apply_next_ = 0;

    dsp::LineConfig line_config_{};
    bool line_reset_pending_ = false;

    dsp::SpectralConfig spectral_config_{};
    bool spectral_reset_pending_ = false;

    gpu::ComputePipeline detect_pipeline_;
    gpu::ComputePipeline apply_pipeline_;
    gpu::ComputePipeline line_pipeline_;
    gpu::ComputePipeline spectral_pipeline_;

    gpu::Buffer flags_;
    gpu::Buffer blanked_;
    gpu::Buffer line_state_;
    gpu::Buffer spectral_tables_;
    gpu::Buffer spectral_state_;

    VkDescriptorPool descriptors_ = VK_NULL_HANDLE;
    VkDescriptorSet blank_set_ = VK_NULL_HANDLE;
    VkDescriptorSet fine_set_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> line_sets_;
    std::vector<VkDescriptorSet> spectral_sets_;
};

}  // namespace revenant::engine
