// The signal graph, recorded.
//
// FIVE THINGS TO READ BEFORE THE CODE, because each is a place where the
// obvious implementation is wrong in a way that does not show up until
// something is already wrong in the audio.
//
//
// 1. ONE COMMAND BUFFER, ONE SUBMISSION, ONE WAIT
//
// gpu::CommandRunner submits and waits inside every call. That is right for
// the reference diff, which wants "run this kernel and give me the answer",
// and wrong here: it would put a host round trip between the convert kernel
// and the branch filter, and another between the branch filter and the
// transform, on every block. So the graph records the whole chain itself,
// with vkCmdPipelineBarrier between stages, and submits once. The host waits
// exactly once per block, in the completion thread, for the timeline value
// that says the audio readback has landed.
//
// Not vkCmdPipelineBarrier2. core/gpu/context.cpp enables timelineSemaphore
// and maintenance4 and does not enable synchronization2, so the 1.3 barrier
// structures are not available on the device this engine creates. The 1.0
// forms express the same dependencies here, and every one of them is a
// full memory barrier rather than a buffer barrier because the ring, the
// branch scratch and the channel ring are whole-buffer dependencies and
// splitting them per range would buy nothing measurable.
//
//
// 2. WHY FRAMES MAY OVERLAP ON THE DEVICE, WHICH IS NOT OBVIOUS
//
// Frame k+1 is recorded and submitted while frame k is still executing, with
// no semaphore between them. That is safe, and the reason is arithmetic
// rather than synchronisation:
//
//   the ring          frame k's branch filter reads [m0*D - N + 1, mL*D].
//                     Frame k+1's convert writes [published_k, ...), and
//                     published_k > mL*D by construction, because a block is
//                     only dispatched once every input sample it needs has
//                     been published. The two ranges are disjoint. The
//                     retirement floor is what keeps them disjoint after the
//                     ring wraps.
//   the channel ring  frame k writes blocks [m0, m0+count) and frame k+1
//                     writes the range above it. Disjoint as long as the
//                     channel ring holds frames_in_flight dispatches, which
//                     is how it is sized.
//   everything else   staging, branch scratch, command buffer, descriptor
//                     sets and readback regions are per frame.
//
// So the only ordering needed is host side: do not reuse a frame slot until
// its timeline value has completed. That is a counter and a wait, and it is
// what Scheduler's completion thread drives.
//
//
// 3. THE GRAPH IS THE RING'S PRODUCER, AND THE ORDER OF ITS CURSOR MOVES IS
//    THE CONTRACT
//
// Four moves per block, and each is where it is for a reason:
//
//   reserve   before a byte is copied. Taking the window is what laps a Lossy
//             consumer that has fallen behind, and what parks a Demand source
//             against the retirement floor. On a Paced source a short grant
//             is an overrun and is counted where it happens.
//   publish   after the command buffer is recorded, before it is submitted.
//             It is a release store over host memory and nothing else: the
//             device-side hazard is the barriers inside the command buffer
//             plus the timeline, and the two are different problems.
//   claim     after the publish, because the ring refuses a claim past what
//             has been published and the block count was derived from the
//             published end.
//   retire    on the completion thread, once the timeline value says the
//             dispatch has finished reading. Retiring at record time hands
//             the writer permission to overwrite samples an in-flight
//             dispatch is still reading, and the symptom is noise in one
//             demodulated channel with nothing in any log.
//
// Those cursors are the ring's own, reached through DeviceRing's producer
// methods. They lived in a private ConsumerTable here for a while, because
// the header declared no producer: the arithmetic was right and it was in the
// wrong object, so no other consumer of the ring could see it and
// DeviceRing::write_index() answered zero for the ring's whole life. A
// producer-side cursor that is not the ring's is that bug again.
//
//
// 4. THE FILTER WARM-UP IS NOT AN EDGE CASE
//
// Output block m reads input samples down to m*D - (N-1), where N is the
// prototype length. For the first blocks of a stream that index is below
// zero, and the kernel's masked arithmetic would happily read the far end of
// the ring and filter whatever the allocator left there. So the first output
// block is ceil((N-1)/D), which at M=64, 17 taps per branch and D=32 is block
// 34: about a millisecond at 2 MS/s. The same bound is re-checked against
// first_available() on every dispatch, because a Lossy consumer that has been
// lapped is in exactly the same position as a stream that has just started.
//
//
// 5. WHAT ADD_VRX TOUCHES, AND WHAT IT MUST NOT
//
// Adding a receiver allocates: one VrxStage, one readback buffer per frame in
// flight, and one host scratch vector. It touches no pipeline, no
// specialization constant, no prototype, no twiddle table, no ring and no
// channel ring. GraphStats::coarse_builds counts pipeline construction and
// stays at one for the life of an open source; the probe asserts it across a
// mid-stream add. That is the claim core/engine/vrx.h makes in prose, kept
// honest by a counter.

#include "core/engine/graph.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <utility>

#include "core/dsp/convert.h"
#include "core/dsp/pfb_branch_reference.h"
#include "core/dsp/pfb_fft_reference.h"
#include "core/dsp/spectrum_levels_reference.h"
#include "core/dsp/spectrum_reference.h"
#include "core/dsp/vrx_reference.h"
#include "core/engine/record_util.h"
#include "core/engine/signal_meter.h"
#include "core/engine/spectrum_scale.h"
#include "core/engine/ring_consumer.h"
#include "core/gpu/buffer.h"
#include "core/gpu/kernel.h"
#include "core/gpu/shaders.h"

namespace revenant::engine {
namespace {

// The channelizer's FFT push-constant block, exactly as
// core/shaders/pfb_fft.comp declares it. dsp::PfbFftParams carries the three
// specialization constants alongside these four, so it cannot be handed to a
// dispatch as bytes; it is still the thing validate() checks, which is why
// both exist.
struct FftPushConstants {
    std::uint32_t block_base = 0;
    std::uint32_t block_count = 0;
    std::uint32_t out_ring_blocks = 0;
    std::uint32_t out_ring_mask = 0;
};

static_assert(sizeof(FftPushConstants) == 4 * sizeof(std::uint32_t),
              "the FFT push block is four packed uint32 in core/shaders/pfb_fft.comp");

// The request with its audio rate filled in, which is what the graph plans
// against.
//
// VrxParams::audio_rate of zero means "the engine's default", and the engine
// default is GraphConfig::audio_rate. The planner does not know that: given
// a zero it falls back to dsp's own kDefaultAudioRate of 48000, which is a
// different number the moment a graph is built with anything else.
//
// WHAT THAT COST BEFORE THIS FUNCTION EXISTED, BECAUSE IT WAS SILENT AND THE
// QT CLIENT ALWAYS TOOK THE PATH. add_vrx resolved the rate for the STAGE,
// through VrxStageRequest::audio_rate, and then planned with the caller's
// raw params. So a graph at 16000 built a stage running at 16000 and cached
// a demodulation rate derived from 48000. Every later retune was then
// compared against the wrong basis: a widen that moves the rate at 16000 and
// not at 48000 looked like no change at all, set_vrx_params returned success
// and stored the request, DemodStage::retune refused it on the recording
// thread where nobody was left to hear, and the caller got its own request
// echoed back over a stage still running the old filter.
[[nodiscard]] VrxParams with_audio_rate(const VrxParams& params, dsp::SampleRate fallback) {
    VrxParams resolved = params;
    if (resolved.audio_rate == 0) {
        resolved.audio_rate = fallback;
    }
    return resolved;
}

// The spectrum kernel's push-constant block, exactly as
// core/shaders/spectrum.comp declares it. dsp::SpectrumParams carries the
// three specialization constants alongside these three, which is why both
// exist here as they do for the FFT.
struct SpectrumPushConstants {
    std::uint32_t chan_blocks = 0;
    std::uint32_t chan_mask = 0;
    std::uint32_t in_offset = 0;
};

static_assert(sizeof(SpectrumPushConstants) == 3 * sizeof(std::uint32_t),
              "the spectrum push block is three packed uint32 in core/shaders/spectrum.comp");

// The levels kernel's push-constant block, exactly as
// core/shaders/spectrum_levels.comp declares it. That kernel has no
// specialization constants beyond the workgroup size, so unlike the two above
// this is the whole of its parameter set and dsp::SpectrumLevelsParams is the
// same three fields; they are still separate types, because one is what
// validate() checks and the other is what goes on the wire.
struct SpectrumLevelsPushConstants {
    std::uint32_t bins = 0;
    std::uint32_t low_permille = 0;
    std::uint32_t high_permille = 0;
};

static_assert(sizeof(SpectrumLevelsPushConstants) == 3 * sizeof(std::uint32_t),
              "the levels push block is three packed uint32 in "
              "core/shaders/spectrum_levels.comp");

constexpr VkBufferUsageFlags kDeviceStorage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

// The staging buffer is bound as a storage buffer, not only as a copy source.
// That is what makes the widening conversion cross the bus exactly once: the
// convert kernel reads the pinned host allocation directly and writes
// Complex32 into device memory, where a copy-then-convert would move the
// native bytes to the device and then read them again.
constexpr VkBufferUsageFlags kStagingUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

constexpr VkBufferUsageFlags kReadbackUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

constexpr std::uint32_t kComplexBytes = 8;

[[nodiscard]] std::span<const std::uint32_t> convert_shader_for(source::SampleFormat format) {
    switch (format) {
        case source::SampleFormat::Cu8: return gpu::shaders::convert_cu8_cf32();
        case source::SampleFormat::Cs8: return gpu::shaders::convert_cs8_cf32();
        case source::SampleFormat::Cs16: return gpu::shaders::convert_cs16_cf32();
        case source::SampleFormat::Cs24: return gpu::shaders::convert_cs24_cf32();
        case source::SampleFormat::Cf32: break;
    }
    return {};
}

// Words the convert kernels load for a run of samples.
//
// The 8-bit formats pack two samples per 32-bit word, so an odd sample count
// leaves half a word of padding that the kernel still loads. A staging buffer
// sized to exactly the sample bytes is two bytes short of that read.
// core/dsp/convert.h states the same rule for the CPU twins.
[[nodiscard]] std::uint64_t staging_bytes_for(source::SampleFormat format,
                                              std::uint64_t samples) {
    const auto bytes = samples * static_cast<std::uint64_t>(source::bytes_per_sample(format));
    return ((bytes + 3) / 4) * 4;
}

// The frequency axis of a spectrum frame, worked out once from the grid.
//
// The central-half selection tiles the span exactly once only on a
// 2x-oversampled grid. At any other decimation a channel's central half is
// not one channel spacing wide, so the pieces either overlap or leave holes
// and every frequency label in the frame is wrong by a growing amount. The
// engine builds D = M/2 and nothing offers another, so this is refused rather
// than approximated: a spectrum with a quietly wrong axis is worse than no
// spectrum, because a person acts on it.
[[nodiscard]] Expected<SpectrumGeometry> spectrum_geometry_for(const dsp::GridParams& grid,
                                                               dsp::SampleRate rate,
                                                               std::uint32_t transform) {
    dsp::SpectrumParams probe;
    probe.channels = grid.channels;
    probe.transform = transform;
    probe.stages = dsp::fft_stages(transform);
    // Stands in for the real ring, which is sized after this runs. Only the
    // three fields above reach the geometry.
    probe.chan_blocks = transform;
    probe.chan_mask = transform - 1;
    if (auto ok = dsp::validate(probe); !ok) {
        return std::unexpected(with_context(ok.error(), "the spectrum stage"));
    }

    if (grid.decimation * 2 != grid.channels) {
        return fail(std::format(
            "the spectrum stage keeps the central half of each channel's bins, which tiles the "
            "span only on a 2x-oversampled grid. This grid is M {} with D {}, so a central half "
            "is {} of a channel spacing wide",
            grid.channels, grid.decimation,
            static_cast<double>(grid.channels) / (2.0 * static_cast<double>(grid.decimation))));
    }
    if (rate <= 0) {
        return fail("the spectrum stage needs the source's sample rate");
    }

    SpectrumGeometry geometry;
    geometry.transform = transform;
    geometry.bins_per_channel = dsp::spectrum_bins_per_channel(transform);
    geometry.channels = grid.channels;
    geometry.bins = dsp::spectrum_bin_count(grid.channels, transform);

    // rate / (D * N) hertz per bin, exactly.
    geometry.bin_width_numerator = rate;
    geometry.bin_width_denominator =
        static_cast<std::int64_t>(grid.decimation) * static_cast<std::int64_t>(transform);

    // Bin zero is a quarter of a channel stream below the centre of channel
    // M/2, whose centre is -rate/2. At D = M/2 a quarter of a channel stream
    // is half a channel spacing, so that is -rate*(M+1)/(2M).
    geometry.bin_zero_numerator = -rate * (static_cast<std::int64_t>(grid.channels) + 1);
    geometry.bin_zero_denominator = 2 * static_cast<std::int64_t>(grid.channels);
    return geometry;
}

// The frequency axis of one receiver's passband frame.
//
// The frame is the central half of an N-point transform of the display
// stream, so bin b is at dc + (b - N/4) * Fdisp / N hertz and bin zero is a
// quarter of the display rate below whatever the fine stage mixed to DC.
// Both ends are exact rationals for the reason dsp::ChannelCentre gives.
//
// WHAT THIS USED TO BE: the whole of a transform of the fine stream, bin zero
// "half the demodulation rate below" DC and N bins. See passband_window for
// why the second half went.
[[nodiscard]] PassbandGeometry passband_geometry_for(dsp::SampleRate rate,
                                                     std::uint32_t transform,
                                                     std::int64_t dc_numerator,
                                                     std::int64_t dc_denominator) {
    PassbandGeometry geometry;
    geometry.transform = transform;
    geometry.bins = transform / 2;
    geometry.rate = rate;
    geometry.bin_width_numerator = rate;
    geometry.bin_width_denominator = static_cast<std::int64_t>(transform);

    // dc - rate/4, over a common denominator so nothing rounds.
    geometry.bin_zero_numerator = 4 * dc_numerator - rate * dc_denominator;
    geometry.bin_zero_denominator = 4 * dc_denominator;
    return geometry;
}

// Bins the passband transform keeps: the kernel's central half.
[[nodiscard]] constexpr std::uint32_t passband_pass_bins(std::uint32_t transform) {
    return transform / 2;
}

// The passband stage's analysis window.
//
// ONE PASS AND THE CENTRAL HALF, WHICH IS WHAT THE KERNEL DOES ANYWAY
//
// core/shaders/spectrum.comp keeps the CENTRAL HALF of each transform,
// because that is what tiles an oversampled channel bank exactly once. The
// display stream is oversampled by two on purpose: its anti-alias filter is
// flat to Fdisp/4 and in stopband by 3*Fdisp/4, so the central half is
// exactly the part of the transform that is flat and alias-free, and the
// outer quarters are the filter's own skirt, which is the one thing the pane
// must not show. core/dsp/vrx_reference.h, "The display tap", has the rule.
//
// WHAT THIS USED TO BE. Two windows and two passes. The pane transformed the
// fine stream, whose rate is only 1.5 times the receiver's bandwidth at
// minimum_demod_rate's floor, so the central half cut into the signal and a
// second pass under a window with its odd taps negated fetched the outer
// quarters by the modulation theorem. That trick was correct and is no
// longer needed: the display rate is chosen here rather than inherited, so
// it is chosen with the margin one pass needs.
//
// THE SECOND HALF OF THE BUFFER IS ONES, AND THAT IS A DECISION
//
// dsp::build_spectrum_window returns the window followed by the channelizer
// prototype's per-bin droop correction. That correction belongs to the
// full-span frame, whose kept band has its edges on the prototype's cutoff
// once per channel. A passband frame has no seam to hide and its edges are
// not on any cutoff, so the correction half is overwritten with unity, and
// the design parameters that built it are irrelevant.
//
// WHAT THAT PARAGRAPH USED TO SAY: that "the response across it is the
// receiver's own fine filter, which is exactly what the display exists to
// show". It was the receiver's filter, and showing it was the fault the
// display tap removed.
//
// Built by the shipped builder rather than by arithmetic of its own so that
// the window taps are bit-identical to the full-span stage's, which is what
// lets dsp::reference_spectrum referee this kernel with nothing new.
[[nodiscard]] Expected<std::vector<float>> passband_window(std::uint32_t transform) {
    auto built = dsp::build_spectrum_window(transform);
    if (!built) {
        return std::unexpected(with_context(built.error(), "the passband window"));
    }
    std::vector<float> taps = std::move(*built);

    for (std::uint32_t bin = 0; bin < passband_pass_bins(transform); ++bin) {
        taps[transform + bin] = 1.0F;
    }
    return taps;
}

// The process-wide stage factory. Control plane, read once per graph.
//
// A function-local static rather than a namespace-scope object so that a
// translation unit registering from a static initialiser cannot race the
// registry's own construction.
struct FactoryRegistry {
    std::mutex lock;
    VrxStageFactory factory;
};

[[nodiscard]] FactoryRegistry& factory_registry() {
    static FactoryRegistry registry;
    return registry;
}

// ---------------------------------------------------------------------------
// The raw tap
// ---------------------------------------------------------------------------

// Demod::Raw: one coarse channel's time series, handed back as interleaved
// float I/Q.
//
// This needs no kernel. The channel ring is channel-major precisely so that
// one channel's time series is contiguous, so the tap is a vkCmdCopyBuffer of
// one run, or of two where the run straddles the ring's wrap. The fine mixing
// a narrower receiver needs is a stage the demodulator package supplies; the
// raw tap deliberately does not do it, because a tap that resampled would no
// longer be the thing a decoder can attach to and compare against the
// channelizer's own output.
class RawTapStage final : public VrxStage {
public:
    explicit RawTapStage(const VrxStageRequest& request)
        : channel_(request.placement.channel),
          ring_blocks_(request.channel_ring_blocks),
          ring_mask_(request.channel_ring_mask),
          channel_rate_(request.channel_rate) {}

    [[nodiscard]] VkDeviceSize audio_bytes_for(std::uint32_t blocks) const override {
        return static_cast<VkDeviceSize>(blocks) * kComplexBytes;
    }

    [[nodiscard]] Expected<StageOutput> record(const StageRecord& record) override {
        StageOutput out;
        out.frames = record.block_count;
        out.channels = 2;
        out.complex_iq = true;
        out.rate = channel_rate_;
        if (record.block_count == 0) {
            return out;
        }

        const auto first_slot = static_cast<std::uint32_t>(record.first_block & ring_mask_);
        const std::uint32_t to_end = ring_blocks_ - first_slot;
        const std::uint32_t contiguous = std::min(to_end, record.block_count);

        const VkDeviceSize channel_origin =
            static_cast<VkDeviceSize>(channel_) * ring_blocks_ * kComplexBytes;

        VkBufferCopy regions[2]{};
        std::uint32_t region_count = 1;

        regions[0].srcOffset = channel_origin +
                               static_cast<VkDeviceSize>(first_slot) * kComplexBytes;
        regions[0].dstOffset = 0;
        regions[0].size = static_cast<VkDeviceSize>(contiguous) * kComplexBytes;

        if (contiguous < record.block_count) {
            // The wrap. Two regions in one command, exactly as the ring
            // writer splits an upload, and for the same reason:
            // vkCmdCopyBuffer takes contiguous ranges and a ring does not
            // promise them.
            regions[1].srcOffset = channel_origin;
            regions[1].dstOffset = static_cast<VkDeviceSize>(contiguous) * kComplexBytes;
            regions[1].size =
                static_cast<VkDeviceSize>(record.block_count - contiguous) * kComplexBytes;
            region_count = 2;
        }

        const VkDeviceSize total =
            static_cast<VkDeviceSize>(record.block_count) * kComplexBytes;
        if (total > record.audio_bytes) {
            return fail(std::format("the raw tap needs {} bytes for {} blocks and was given {}",
                                    total, record.block_count, record.audio_bytes));
        }

        // One crossing of the bus whether the wrap split it into two regions
        // or not, which is what GraphStats::readbacks counts.
        vkCmdCopyBuffer(record.commands, record.channel_ring, record.audio_destination,
                        region_count, regions);
        out.readbacks = 1;
        return out;
    }

    [[nodiscard]] Status retune(const VrxParams&, const VrxPlacement& placement) override {
        // The channel index is the whole of this stage's tuning state, which
        // is why retuning a raw tap rebuilds nothing.
        channel_ = placement.channel;
        return {};
    }

private:
    std::uint32_t channel_ = 0;
    std::uint32_t ring_blocks_ = 0;
    std::uint32_t ring_mask_ = 0;
    dsp::SampleRate channel_rate_ = 0;
};

}  // namespace

void install_vrx_stage_factory(VrxStageFactory factory) {
    auto& registry = factory_registry();
    std::scoped_lock lock(registry.lock);
    registry.factory = std::move(factory);
}

VrxStageFactory installed_vrx_stage_factory() {
    auto& registry = factory_registry();
    std::scoped_lock lock(registry.lock);
    return registry.factory;
}

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

struct Graph::Impl {
    // One receiver's passband transform: everything that exists only because
    // somebody is looking closely at that receiver.
    //
    // Held behind a shared_ptr and referenced by every frame that recorded
    // against it, for the same reason VrxSlot is: detaching a sink while a
    // frame is in flight must not free a buffer the device is still writing.
    //
    // ALLOCATED ON ATTACH, NOT ON ADD, WHICH IS THE OPT-IN
    //
    // Three device buffers and a readback per frame in flight, plus a
    // descriptor pool, is not a rounding error at fifty receivers. A receiver
    // nobody has attached a sink to holds none of it and records no dispatch,
    // so the rack pays for the receivers under examination and not for the
    // ones merely running.
    //
    // What a receiver DOES pay for unconditionally is its display ring and
    // display tap table, because a ring cannot be grown while a command
    // buffer names it. That is memory rather than work, and it is why
    // GraphConfig::passband_transform is a second switch above this one.
    struct PassbandView {
        VkDevice device = VK_NULL_HANDLE;
        VkDescriptorPool descriptors = VK_NULL_HANDLE;

        std::uint32_t transform = 0;

        // Copied from the stage at attach. Every field is fixed for the life
        // of the stage, which is what makes reading it from the control
        // plane safe while a block is being recorded.
        StageDisplayOutput display{};

        // Per frame in flight, because frames overlap on the device.
        //
        // pass is a whole transform's worth although the pass writes only
        // its upper half: core/shaders/spectrum.comp places a channel's
        // output at slot (k + M/2) mod M, and the smallest M its own
        // validate() accepts is two, so channel zero lands at slot one. The
        // lower half is never written and never read. Half a transform of
        // scratch is cheaper than a second kernel.
        std::vector<gpu::Buffer> pass;
        std::vector<gpu::Buffer> frame;
        std::vector<gpu::Buffer> readback;
        std::vector<gpu::Buffer> levels_output;
        std::vector<gpu::Buffer> levels_readback;

        std::vector<VkDescriptorSet> pass_set;
        std::vector<VkDescriptorSet> levels_set;

        // Completion thread only, which is single and in order.
        //
        // Buffers and nothing else. What a receiver's passband display is
        // up to, the row count and the colour map, lives on the slot rather
        // than here, because it has to outlive a sink being swapped.
        std::vector<float> scratch;

        ~PassbandView() {
            if (descriptors != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device, descriptors, nullptr);
            }
        }

        PassbandView() = default;
        PassbandView(const PassbandView&) = delete;
        PassbandView& operator=(const PassbandView&) = delete;
        PassbandView(PassbandView&&) = delete;
        PassbandView& operator=(PassbandView&&) = delete;
    };

    // One receiver. Kept behind a shared_ptr so that removing it while a frame
    // is in flight is safe: the frame's snapshot holds a reference and the
    // slot outlives the dispatch that was reading it.
    struct VrxSlot {
        VrxId id;

        // Fixed at add_vrx. A Probe is dispatched like any receiver and left
        // out of vrx_ids; see VrxRole.
        VrxRole role = VrxRole::Receiver;

        // Control plane only, under control_lock. These are what vrx_status
        // reports, and no engine thread reads them.
        VrxParams params;
        VrxPlacement placement;

        // What this receiver's pipeline was built as, recomputed whenever
        // params or placement change. Control plane only, like the two
        // above, and the thing set_vrx_params compares a request against to
        // tell a push constant from a rebuild.
        //
        // Cached rather than asked of the stage, because the graph's own raw
        // tap has no plan to ask, and computed by dsp::vrx_shape_for, which
        // is plan_vrx's arithmetic without the three filter tables.
        //
        // WHAT THIS USED TO BE, BECAUSE THE SHORTFALL WAS REACHABLE. Two
        // fields, a demodulation rate and a tap count, compared against two
        // fields of a plan. DemodStage::retune compared six. A retune that
        // moved only the audio rate could leave both of these exactly where
        // they were while moving the decimation and the output rate, so the
        // graph accepted it and the stage refused it where no caller was
        // left. dsp::VrxShape is now the whole question and both ask it.
        dsp::VrxShape shape{};

        // False when the request could not be planned at all, in which case
        // set_vrx_params has nothing to compare against and lets the
        // planner's own refusal answer.
        //
        // Only the raw tap reaches that state. Every other stage is built
        // by a factory that plans, so an unplannable request fails in
        // add_vrx before a slot exists; RawTapStage is a buffer copy of a
        // channel and never asks the planner, so a raw receiver whose
        // passband the planner would refuse is still added.
        bool shape_known = false;

        // What with_audio_rate resolved this receiver's audio rate to, which
        // is params.audio_rate unless that was zero and the engine's default
        // answered. Kept beside the params rather than derived in
        // vrx_status, because shape.output_rate is only there for a receiver
        // the planner could plan and the raw tap is not one.
        dsp::SampleRate resolved_audio_rate = 0;

        // The recording thread's own copy, so that a retune landing while a
        // block is being recorded cannot tear a field out from under it. The
        // two are kept in step by the control queue, which is the only path
        // between them.
        VrxParams recording_params;

        // How many retunes have been QUEUED for this receiver, control plane
        // only, under control_lock. Stamped onto the retune op as it is
        // queued and reported by vrx_status, so a consumer knows the number
        // the chunks are heading for before any chunk carries it.
        std::uint64_t queued_epoch = 0;

        // The same number, as far as the recording thread has got: the
        // queued_epoch of the last retune op it applied. Recording thread
        // only, snapshotted into every frame and delivered on every
        // AudioChunk. See AudioChunk::tuning_epoch for what a consumer does
        // with it and why the boundary cannot be drawn on the control plane.
        //
        // ASSIGNED FROM THE OP AND NOT INCREMENTED HERE, which is what makes
        // the two sides one number rather than two counts that agree until
        // they do not. Incrementing meant the control plane knew how many
        // retunes it had asked for and the sample path knew how many it had
        // applied, and nothing anywhere held the value that says whether a
        // given chunk is before or after a given request.
        std::uint64_t recording_epoch = 0;

        std::unique_ptr<VrxStage> stage;

        // One per frame in flight, so a frame never waits on another frame's
        // readback and adding a receiver never resizes a buffer an in-flight
        // frame is writing.
        std::vector<gpu::Buffer> readback;
        VkDeviceSize audio_bytes = 0;

        // Touched by the completion thread only, which is single and
        // in-order, so no synchronisation and no per-frame copies.
        std::vector<float> scratch;
        dsp::SampleIndex audio_index = 0;

        // What this receiver's passband display is up to: how many rows it
        // has drawn, where the last one was, and where its colour map has
        // got to.
        //
        // Here and not on the PassbandView, so that detaching a sink and
        // attaching another does not restart the row count or reset the
        // colour map. Frames recorded against the old view can still be in
        // flight when the new one arrives, and a counter living on the view
        // would have those frames numbered from a different origin than the
        // ones behind them. Completion thread only, like the two above.
        std::optional<SpectrumScale> passband_scale;
        std::uint64_t passband_sequence = 0;
        dsp::SampleIndex passband_last_start = 0;
        bool passband_have_last = false;

        // Recording thread only. The completion thread gets its own copy of
        // the pointer in the frame snapshot, so replacing a sink never races
        // a delivery that is already under way.
        std::shared_ptr<AudioSink> sink;

        // Both null until a passband sink is attached, and both dropped
        // together when it is detached. Recording thread only, like the
        // audio sink above and for the same reason.
        std::shared_ptr<PassbandView> passband;
        std::shared_ptr<PassbandSink> passband_sink;

        std::atomic<std::uint64_t> audio_samples{0};
        std::atomic<std::uint64_t> audio_dropped{0};
        std::atomic<std::uint64_t> level_bits{0};
        std::atomic<bool> squelch_open{false};

        // WRITTEN ON THE RECORDING THREAD, unlike the two counters above,
        // which move on the completion thread as frames are delivered. A
        // re-anchor is decided while the command buffer is being written and
        // produces no frames at all, so there is no delivery to attribute it
        // to and nothing for the completion thread to see. Atomic for the
        // same reason the rest are: vrx_status reads them from the control
        // plane. See VrxStatus::reanchors for what they mean.
        std::atomic<std::uint64_t> reanchors{0};
        std::atomic<std::uint64_t> reanchor_frames_skipped{0};

        void store_level(double value) {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            level_bits.store(bits, std::memory_order_relaxed);
        }

        [[nodiscard]] double load_level() const {
            const std::uint64_t bits = level_bits.load(std::memory_order_relaxed);
            double value = kSilenceFloorDbfs;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }
    };

    // A control-plane change, applied by the recording thread at a block
    // boundary. A queue rather than a lock because the recording thread must
    // not take one, and a Treiber stack rather than a ring because the control
    // plane is rare and unbounded and the sample path only ever drains.
    struct ControlOp {
        enum class Kind : std::uint8_t { Add, Remove, Retune, Sink, Spectrum, Passband };

        Kind kind = Kind::Add;
        VrxId id;
        VrxParams params;
        VrxPlacement placement;

        // Retune only: the receiver's queued_epoch at the moment this op was
        // pushed, which the recording thread copies onto the slot when it
        // applies it. Carried on the op rather than recomputed, so that the
        // number vrx_status handed a caller before the drain is the number
        // the chunks after the drain carry.
        std::uint64_t tuning_epoch = 0;
        std::shared_ptr<VrxSlot> slot;
        std::shared_ptr<AudioSink> sink;
        std::shared_ptr<SpectrumSink> spectrum_sink;

        // Both null on a detach, which is how one operation says either
        // thing: a view with no sink would record a transform nobody reads
        // and a sink with no view would have nothing to read.
        std::shared_ptr<PassbandView> passband;
        std::shared_ptr<PassbandSink> passband_sink;
        ControlOp* next = nullptr;
    };

    // What the completion thread is allowed to see. Everything mutable was
    // snapshotted by the recording thread when the frame was recorded, so the
    // two threads share only the slot's atomics and the buffers the slot's
    // lifetime keeps alive.
    struct FrameVrx {
        std::shared_ptr<VrxSlot> slot;
        std::shared_ptr<AudioSink> sink;
        VkBuffer readback = VK_NULL_HANDLE;
        std::size_t readback_index = 0;
        StageOutput output{};
        double squelch_dbfs = kSilenceFloorDbfs;

        // Snapshotted with the sink and for the same reason: a retune
        // applied while this frame is in flight must not restamp samples
        // the old tuning produced.
        std::uint64_t tuning_epoch = 0;

        // The passband this frame recorded for this receiver, if any. The
        // view is held by value in the snapshot so that a detach landing
        // while this frame is in flight cannot free the buffers under it.
        std::shared_ptr<PassbandView> passband;
        std::shared_ptr<PassbandSink> passband_sink;
        PassbandGeometry passband_geometry{};
        dsp::SampleIndex passband_start = 0;
        dsp::SampleIndex passband_count = 0;

        // Ring slot of the oldest display sample in this frame's window,
        // which is what the kernel takes as its in_offset. Worked out in the
        // planning phase and used one phase later, because the phases are
        // what keep the barriers off the per-receiver path.
        std::uint32_t passband_window_offset = 0;
        bool passband_recorded = false;
    };

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        gpu::Buffer staging;
        gpu::Buffer branch_output;
        VkDescriptorSet convert_set = VK_NULL_HANDLE;
        VkDescriptorSet branch_set = VK_NULL_HANDLE;
        VkDescriptorSet fft_set = VK_NULL_HANDLE;

        std::vector<FrameVrx> vrxs;
        dsp::SampleIndex retire_through = 0;
        std::uint64_t timeline_value = 0;

        // The spectrum stage's own scratch and destination, per frame for the
        // same reason a receiver's are: frames overlap on the device, so a
        // shared buffer would have the next dispatch writing over the copy
        // the last one is still reading.
        gpu::Buffer spectrum_output;
        gpu::Buffer spectrum_readback;
        VkDescriptorSet spectrum_set = VK_NULL_HANDLE;

        // Two floats, measured from the frame above by a second dispatch
        // before either leaves the device. Per frame for the same reason the
        // frame itself is.
        gpu::Buffer spectrum_levels_output;
        gpu::Buffer spectrum_levels_readback;
        VkDescriptorSet spectrum_levels_set = VK_NULL_HANDLE;

        // Snapshotted by the recording thread when the frame was recorded, so
        // replacing the sink never races a delivery already under way.
        std::shared_ptr<SpectrumSink> spectrum_sink;
        dsp::SampleIndex spectrum_start = 0;
        dsp::SampleIndex spectrum_count = 0;
        bool spectrum_recorded = false;
    };

    const gpu::Context* context = nullptr;
    DeviceRing* ring = nullptr;
    Scheduler* scheduler = nullptr;

    GraphConfig config{};
    GraphGeometry geometry{};
    dsp::GridParams grid{};
    std::uint32_t prototype_length = 0;
    bool prepared = false;

    gpu::Buffer prototype_buffer;
    gpu::Buffer twiddle_buffer;
    gpu::Buffer channel_ring;

    // The spectrum stage's own tables. A second twiddle circle because its
    // transform is N points and the channelizer's is M, and a window because
    // an unwindowed transform's 13 dB sidelobes smear one broadcast carrier
    // across the whole display.
    gpu::Buffer spectrum_twiddles;
    gpu::Buffer spectrum_window;

    // The passband stage's tables, shared by every receiver that attaches a
    // sink. A third twiddle circle because its transform is its own size,
    // and one window; passband_window() has the argument, and says why
    // there used to be two.
    gpu::Buffer passband_twiddles;
    gpu::Buffer passband_window_table;

    bool has_convert = false;
    gpu::ComputePipeline convert_pipeline;
    gpu::ComputePipeline branch_pipeline;
    gpu::ComputePipeline fft_pipeline;
    gpu::ComputePipeline spectrum_pipeline;
    gpu::ComputePipeline spectrum_levels_pipeline;

    // A second specialization of core/shaders/spectrum.comp, at one channel
    // and the passband's own transform size.
    //
    // docs/fft.md records an integrated-device fault that tracks how many
    // pipelines one module is specialized into, so a second one is worth
    // naming rather than adding quietly. It buys the whole stage: the same
    // refereed butterfly graph, no new kernel, no new CPU twin.
    gpu::ComputePipeline passband_pipeline;

    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    VkSemaphore timeline = VK_NULL_HANDLE;

    // The graph's registration on the ring. Note 3 at the top of the file has
    // the order its cursors move in.
    RingConsumer consumer;

    std::vector<Frame> frames;

    // Recording thread only.
    std::vector<std::shared_ptr<VrxSlot>> active;
    dsp::SampleIndex next_block = 0;

    // The oldest sample index whose contents are the stream's rather than
    // whatever the ring held before.
    //
    // A hole gets into the ring two ways, both of them Paced-only: the source
    // reported samples it dropped, or the graph had no frame free and could
    // not place a block it had already reserved. Either way the cursor has to
    // advance so the ring's index keeps tracking the stream's index, and the
    // samples in that range are not the stream's. A block whose filter support
    // overlaps such a range would be filtered from stale memory and would look
    // like signal, so the first output block after a hole is the first one
    // whose whole support clears it.
    dsp::SampleIndex trusted_from = 0;
    bool first_delivery_seen = false;

    // Recording thread. The spectrum's window is N channel blocks long and
    // reaches back over dispatches, so it needs to know where the channel
    // ring stopped being a continuous record of the stream. That is the first
    // block after the filter warm-up, and it moves forward again every time a
    // dispatch skips blocks: a window straddling a skip would transform a
    // splice of two eras and paint a band of energy that was never on the
    // air.
    dsp::SampleIndex blocks_contiguous_from = 0;
    std::shared_ptr<SpectrumSink> spectrum_sink;

    // Completion thread only, which is single and in-order, so no
    // synchronisation and no per-frame copies.
    std::vector<float> spectrum_scratch;
    std::uint64_t spectrum_sequence = 0;
    dsp::SampleIndex prototype_group_delay = 0;

    // The colour map's ends and the recurrence that moves them. Completion
    // thread only, and that is the whole of its thread safety: the recurrence
    // is sequential, so it belongs on the one thread that sees every frame
    // exactly once and in order.
    std::optional<SpectrumScale> spectrum_scale;
    dsp::SampleIndex spectrum_last_start = 0;
    bool spectrum_have_last = false;

    std::atomic<ControlOp*> control_head{nullptr};

    // Control plane view, so vrx_status and vrx_ids answer without touching
    // anything the recording thread owns.
    mutable std::mutex control_lock;
    std::vector<std::shared_ptr<VrxSlot>> known;

    VrxStageFactory factory;

    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> completed{0};
    std::atomic<bool> cancelled{false};

    // What a recording thread waiting for a frame slot parks on, bumped by
    // both things that can make waiting the wrong answer: a frame retiring
    // and cancel().
    //
    // Separate from `completed` because `completed` is the slot arithmetic
    // itself. cancel() cannot move it to wake anybody, since a cancel that
    // added one would tell the recording thread a frame in flight had
    // finished and hand out its slot. Until 2026-09-20 cancel() called
    // completed.notify_all() and changed nothing, and std::atomic::wait
    // re-reads the value on every notify and parks again when it has not
    // moved, so that call woke nothing: a cancel arriving while the
    // recording thread was parked on a frame slot left it parked until the
    // GPU happened to retire something, and on a wedged device that is for
    // ever.
    std::atomic<std::uint64_t> frame_epoch{0};

    std::atomic<std::uint64_t> blocks_in{0};
    std::atomic<std::uint64_t> samples_in{0};
    std::atomic<std::uint64_t> submissions{0};
    std::atomic<std::uint64_t> readbacks{0};
    std::atomic<std::uint64_t> dispatches{0};
    std::atomic<std::uint64_t> channel_blocks{0};
    std::atomic<std::uint64_t> overrun_events{0};
    std::atomic<std::uint64_t> samples_dropped{0};
    std::atomic<std::uint64_t> frame_stalls{0};
    std::atomic<std::uint64_t> coarse_builds{0};
    std::atomic<std::uint64_t> audio_frames{0};
    std::atomic<std::uint64_t> audio_dropped{0};
    std::atomic<std::uint64_t> spectrum_frames{0};
    std::atomic<std::uint64_t> spectrum_skipped{0};
    std::atomic<std::uint64_t> passband_frames{0};
    std::atomic<std::uint64_t> passband_skipped{0};
    std::atomic<std::uint64_t> vrx_retune_refusals{0};

    ~Impl() { destroy(); }

    void destroy() noexcept {
        // Deregistering rather than stopping the ring. Both wake a producer
        // parked on this consumer's retirement, because release_slot bumps
        // the epoch, and only this one leaves the ring usable: a graph that
        // failed halfway through create() has no business poisoning a ring it
        // was handed. Discarded because a destructor has nobody to tell, and
        // because the only failure is a consumer that is already gone.
        if (ring != nullptr && consumer.valid()) {
            (void)ring->remove_consumer(consumer);
        }
        ControlOp* head = control_head.exchange(nullptr, std::memory_order_acq_rel);
        while (head != nullptr) {
            ControlOp* next = head->next;
            delete head;
            head = next;
        }
        if (context == nullptr || context->device() == VK_NULL_HANDLE) {
            return;
        }
        const VkDevice device = context->device();
        for (auto& frame : frames) {
            if (frame.pool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device, frame.pool, nullptr);
                frame.pool = VK_NULL_HANDLE;
            }
        }
        frames.clear();
        if (descriptors != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptors, nullptr);
            descriptors = VK_NULL_HANDLE;
        }
        if (timeline != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, timeline, nullptr);
            timeline = VK_NULL_HANDLE;
        }
    }

    // Publishes a whole reservation.
    //
    // The ring lets a producer publish one reservation in pieces, so a short
    // return is legal there and a bug here: this graph reserves and publishes
    // one block at a time. The two cursors coming apart means the ring's index
    // has stopped being the stream's index, and every timestamp downstream is
    // derived from that equality.
    [[nodiscard]] Status publish_all(std::uint64_t granted) {
        auto published = ring->publish(granted);
        if (!published) {
            return std::unexpected(published.error());
        }
        if (*published != granted) {
            return fail(std::format(
                "the graph reserved {} samples of ring and published {}: the producer's cursors "
                "have come apart, so the ring's index is no longer the stream's",
                granted, *published));
        }
        return {};
    }

    void push_control(ControlOp* op) {
        op->next = control_head.load(std::memory_order_relaxed);
        while (!control_head.compare_exchange_weak(op->next, op, std::memory_order_release,
                                                   std::memory_order_relaxed)) {
        }
    }

    // Recording thread. Pops the whole stack, restores submission order, and
    // applies each change.
    void drain_control() {
        ControlOp* head = control_head.exchange(nullptr, std::memory_order_acq_rel);
        ControlOp* ordered = nullptr;
        while (head != nullptr) {
            ControlOp* next = head->next;
            head->next = ordered;
            ordered = head;
            head = next;
        }

        while (ordered != nullptr) {
            ControlOp* op = ordered;
            ordered = op->next;
            apply_control(*op);
            delete op;
        }
    }

    void apply_control(ControlOp& op) {
        switch (op.kind) {
            case ControlOp::Kind::Add:
                active.push_back(std::move(op.slot));
                return;
            case ControlOp::Kind::Remove:
                std::erase_if(active, [&](const std::shared_ptr<VrxSlot>& slot) {
                    return slot->id == op.id;
                });
                return;
            case ControlOp::Kind::Retune:
                for (auto& slot : active) {
                    if (slot->id == op.id) {
                        slot->recording_params = op.params;

                        // BEFORE the stage is asked, and moved whatever it
                        // answers. This is the boundary every chunk from
                        // here on is stamped with, and the frames already
                        // recorded keep the old number, which is the whole
                        // of what AudioChunk::tuning_epoch is for. Moving
                        // it on the refusal below as well costs a consumer
                        // one reacquisition in a case the counter under it
                        // says must never happen; not moving it would make
                        // the epoch mean "the tuning changed", which the
                        // recording thread is not in a position to know
                        // without asking the stage what it did with six
                        // fields.
                        //
                        // ASSIGNED AND NOT INCREMENTED. Two retunes drained
                        // in one pass both land here before a single frame
                        // is recorded, so the epoch arrives at the second
                        // op's number either way; what assignment adds is
                        // that VrxStatus::tuning_epoch already held that
                        // number, so a consumer can compare rather than
                        // count the boundaries it sees. Counting is what
                        // broke: one drain of two ops produces one stamped
                        // change, so a consumer waiting for two waited for
                        // ever.
                        slot->recording_epoch = op.tuning_epoch;

                        // COUNTED, NOT DISCARDED. There is no caller left
                        // to return this to: the op is on the recording
                        // thread and set_vrx_params returned success a
                        // round trip ago. But "no caller" is not "no
                        // reader", and dropping it on the floor is what let
                        // the graph's idea of a shape change differ from
                        // the stage's for as long as it did.
                        //
                        // set_vrx_params asks dsp::vrx_shape_for the same
                        // question before queueing, so this counter is
                        // required to stay at zero and a nonzero value
                        // means the two have come apart. GraphStats::
                        // vrx_retune_refusals says what it means to read.
                        //
                        // A stage that cannot retune keeps its old tuning,
                        // which is wrong but audible, where dropping the
                        // receiver would be silent.
                        //
                        // WHAT THIS COMMENT USED TO CLAIM. It said the
                        // error surfaced on the next status read through
                        // the params not matching. It did not: this op is
                        // queued only after set_vrx_params has already
                        // stored the new params on the slot, so vrx_status
                        // reported exactly what was asked for while the
                        // stage ran something else. A caller dragging a
                        // filter edge past a rate boundary therefore saw
                        // the request echoed back, heard no change, and had
                        // nothing anywhere to tell it why. The refusal in
                        // set_vrx_params is what closes that.
                        if (!slot->stage->retune(op.params, op.placement)) {
                            vrx_retune_refusals.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                return;
            case ControlOp::Kind::Sink:
                for (auto& slot : active) {
                    if (slot->id == op.id) {
                        slot->sink = op.sink;
                    }
                }
                return;
            case ControlOp::Kind::Spectrum:
                spectrum_sink = op.spectrum_sink;
                return;
            case ControlOp::Kind::Passband:
                for (auto& slot : active) {
                    if (slot->id == op.id) {
                        // Both together. On a detach both are null and the
                        // old view's last reference here goes away, which
                        // frees it as soon as the frames holding one retire.
                        slot->passband = op.passband;
                        slot->passband_sink = op.passband_sink;
                    }
                }
                return;
        }
    }

    // Control plane. Everything one receiver needs to have its passband
    // transformed, allocated in one place so that attaching a sink is the
    // only moment any of it is created.
    [[nodiscard]] Expected<std::shared_ptr<PassbandView>> build_passband_view(
        const StageDisplayOutput& display) {
        const std::uint32_t points = geometry.passband_transform;
        if (display.capacity < points) {
            return fail(std::format(
                "a {}-point window does not fit a display ring of {} samples, so the "
                "transform would read the same samples twice. The ring is sized against "
                "VrxStageRequest::passband_transform at construction, so this receiver was "
                "built before the passband stage was",
                points, display.capacity));
        }
        if (display.mask != display.capacity - 1U || !std::has_single_bit(display.capacity)) {
            return fail(std::format(
                "a display ring of {} samples with mask {} is not a power-of-two ring, and the "
                "kernel masks its own index",
                display.capacity, display.mask));
        }

        auto view = std::make_shared<PassbandView>();
        view->device = context->device();
        view->transform = points;
        view->display = display;
        view->scratch.assign(passband_pass_bins(points), 0.0F);

        // The pass buffer is the kernel's whole two-channel frame, of which
        // it writes the upper half; the frame and its readback are that half.
        const auto pass_bytes = static_cast<VkDeviceSize>(points) * sizeof(float);
        const auto frame_bytes =
            static_cast<VkDeviceSize>(passband_pass_bins(points)) * sizeof(float);
        constexpr VkDeviceSize kLevelsBytes =
            static_cast<VkDeviceSize>(dsp::kSpectrumLevelsOutputs) * sizeof(float);

        auto make = [&](std::vector<gpu::Buffer>& into, VkDeviceSize bytes,
                        gpu::MemoryKind kind, VkBufferUsageFlags usage,
                        const char* what) -> Status {
            into.reserve(geometry.frames_in_flight);
            for (std::uint32_t i = 0; i < geometry.frames_in_flight; ++i) {
                auto buffer = gpu::Buffer::create(*context, bytes, usage, kind);
                if (!buffer) {
                    return std::unexpected(with_context(buffer.error(), what));
                }
                into.push_back(std::move(*buffer));
            }
            return {};
        };

        if (auto ok = make(view->pass, pass_bytes, gpu::MemoryKind::DeviceLocal,
                           kDeviceStorage, "passband pass scratch");
            !ok) {
            return std::unexpected(ok.error());
        }
        if (auto ok = make(view->frame, frame_bytes, gpu::MemoryKind::DeviceLocal,
                           kDeviceStorage, "passband frame");
            !ok) {
            return std::unexpected(ok.error());
        }
        if (auto ok = make(view->readback, frame_bytes, gpu::MemoryKind::Readback,
                           kReadbackUsage, "passband readback");
            !ok) {
            return std::unexpected(ok.error());
        }
        if (auto ok = make(view->levels_output, kLevelsBytes, gpu::MemoryKind::DeviceLocal,
                           kDeviceStorage, "passband levels scratch");
            !ok) {
            return std::unexpected(ok.error());
        }
        if (auto ok = make(view->levels_readback, kLevelsBytes, gpu::MemoryKind::Readback,
                           kReadbackUsage, "passband levels readback");
            !ok) {
            return std::unexpected(ok.error());
        }

        // Two sets per frame in flight: the transform and the percentile.
        const std::uint32_t sets = 2U * geometry.frames_in_flight;
        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_size.descriptorCount = (4U + 2U) * geometry.frames_in_flight;

        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = sets;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;

        VkResult result =
            vkCreateDescriptorPool(view->device, &pool_info, nullptr, &view->descriptors);
        if (result != VK_SUCCESS) {
            return fail(std::format("vkCreateDescriptorPool failed for a passband ({})",
                                    gpu::result_name(result)),
                        result);
        }

        auto allocate = [&](std::vector<VkDescriptorSet>& into,
                            VkDescriptorSetLayout layout) -> Status {
            std::vector<VkDescriptorSetLayout> layouts(geometry.frames_in_flight, layout);
            into.assign(geometry.frames_in_flight, VK_NULL_HANDLE);

            VkDescriptorSetAllocateInfo alloc{};
            alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc.descriptorPool = view->descriptors;
            alloc.descriptorSetCount = geometry.frames_in_flight;
            alloc.pSetLayouts = layouts.data();
            const VkResult allocated =
                vkAllocateDescriptorSets(view->device, &alloc, into.data());
            if (allocated != VK_SUCCESS) {
                return fail(std::format("vkAllocateDescriptorSets failed for a passband ({})",
                                        gpu::result_name(allocated)),
                            allocated);
            }
            return {};
        };

        if (auto ok = allocate(view->pass_set, passband_pipeline.descriptor_layout()); !ok) {
            return std::unexpected(ok.error());
        }
        if (auto ok = allocate(view->levels_set, spectrum_levels_pipeline.descriptor_layout());
            !ok) {
            return std::unexpected(ok.error());
        }

        for (std::uint32_t i = 0; i < geometry.frames_in_flight; ++i) {
            const VkBuffer pass[] = {display.ring, passband_twiddles.handle(),
                                     passband_window_table.handle(), view->pass[i].handle()};
            if (auto wrote = write_storage_set(view->device, view->pass_set[i], pass); !wrote) {
                return std::unexpected(with_context(wrote.error(), "passband pass set"));
            }
            const VkBuffer levels[] = {view->frame[i].handle(),
                                       view->levels_output[i].handle()};
            if (auto wrote = write_storage_set(view->device, view->levels_set[i], levels);
                !wrote) {
                return std::unexpected(with_context(wrote.error(), "passband levels set"));
            }
        }
        return view;
    }

    [[nodiscard]] std::shared_ptr<VrxSlot> find_known(VrxId id) const {
        for (const auto& slot : known) {
            if (slot->id == id) {
                return slot;
            }
        }
        return nullptr;
    }

    // Recording thread. Where a display sample sits in the SOURCE's index,
    // which is the one index everything downstream shares.
    //
    // Display output j is taken at channel instant j*Fc/Fdisp, the display
    // filter delays it by its own group delay in channel samples, channel
    // sample m is taken at input m*D, and the channelizer prototype delays
    // that by its group delay in input samples. Composed and with Fs = Fc*D:
    //
    //     source(j) = j*Fs/Fdisp - (display_delay*D + prototype_delay)
    //
    // Split over Fdisp before multiplying by Fs so the product cannot leave
    // 64 bits at any index this engine can reach, which is the same split
    // core/engine/vrx_stage.cpp uses on the same ratio.
    [[nodiscard]] dsp::SampleIndex source_index_of_display(const StageDisplayOutput& display,
                                                           dsp::SampleRate rate,
                                                           dsp::SampleIndex j) const {
        const auto fd = static_cast<dsp::SampleIndex>(rate);
        const auto fs = static_cast<dsp::SampleIndex>(config.source_rate);
        if (fd == 0) {
            return 0;
        }
        const dsp::SampleIndex scaled = (j / fd) * fs + ((j % fd) * fs) / fd;

        const auto delay =
            static_cast<dsp::SampleIndex>(std::llround(
                display.group_delay_channel_samples * static_cast<double>(grid.decimation))) +
            prototype_group_delay;

        // Clamped rather than wrapped. The first window of a stream sits a
        // few thousand samples above zero and the delay is a few hundred, so
        // this does not bite in practice; an unsigned index that wrapped
        // would put a frame at 1.8e19 on a timeline, which is not a number
        // anything downstream recovers from.
        return scaled > delay ? scaled - delay : 0;
    }

    // Recording thread. Every enabled receiver's passband, recorded once the
    // stages have all run.
    //
    // ONE PASS OF THE SPECTRUM KERNEL OVER THE DISPLAY RING
    //
    // core/shaders/spectrum.comp keeps the central half of its transform,
    // and the display stream is built so that the central half is exactly
    // the flat, alias-free part; passband_window() has the argument. The pass
    // writes at N/2 within its own buffer rather than at zero, because the
    // kernel is specialized at two channels and places channel zero at slot
    // one, so one copy region moves pass[N/2 .. N) to frame[0 .. N/2).
    //
    // WHAT THIS USED TO DO: two passes over the fine ring, the second under
    // a window with its odd taps negated, and three copy regions that
    // reassembled all N bins. That was needed while the pane was the fine
    // stream and its rate had no margin to spare; see passband_window.
    //
    // WHY THIS IS FIVE PHASES OVER EVERY RECEIVER RATHER THAN ONE FUNCTION
    //    PER RECEIVER, MEASURED
    //
    // A passband is two dispatches of one workgroup each. On a card with a
    // hundred and twenty-eight multiprocessors that is almost entirely idle
    // silicon, so what it costs is not the arithmetic but the serialisation
    // around it: every vkCmdPipelineBarrier is a full stop, and receivers
    // fenced off from each other cannot overlap however little work each one
    // has.
    //
    // Recorded per receiver, with its own four barriers, this cost 32.7 us
    // per receiver per block at eight receivers against 10.8 us at one, and
    // quartering the transform from 2048 points to 512 changed it by 2.6
    // percent. Both numbers say the same thing: the transform is free and
    // the fences are not.
    //
    // Phased, the barrier count is four whatever the receiver count is, and
    // every receiver's two transforms are in the same dependency region as
    // every other's, so they overlap. The phases are the dependency chain
    // written out: transform, assemble, measure, carry home. That took the
    // eight-receiver figure to about 15 us each, against a coarse chain of
    // about 10 us and a fine stage of about 18.
    //
    // WHAT THE MEASUREMENT WILL AND WILL NOT SUPPORT
    //
    // Eight receivers is the only point that repeats: 32.7 and 35.4 us each
    // before the phases, 14.6, 15.1, 16.2 and 16.2 after, over four runs of
    // each. That comparison is what justifies the shape of this function.
    //
    // One receiver and thirty-two do not repeat. Across four runs the same
    // case gave 1.5 to 13.9 us each at one and 4.7 to 14.7 at thirty-two, so
    // the honest figure at those counts is "somewhere between five and
    // fifteen microseconds" and nothing finer. At one receiver the whole
    // delta is a few milliseconds over a seventy-millisecond run, which is
    // near the resolution of a wall clock around a whole engine.
    //
    // Transform size does not show up at all: 512 points against 2048 at
    // eight receivers gave 11.3 to 18.9 against 14.6 to 16.2, which is the
    // same number. Two single-workgroup transforms are idle silicon either
    // way.
    //
    // All of it measured 2026-09-19 on the RTX 4090 at 20 MS/s in
    // 65536-sample blocks by the hidden cost case in
    // tests/engine/test_engine_passband.cpp, as a wall-clock delta of two
    // end-to-end runs, each the minimum of five. That includes the host
    // side, so it is what a caller opening a second display pays rather than
    // what the dispatches cost. A per-stage figure would want GPU
    // timestamps, which is what the rig in tools/bench is for.
    //
    // Those figures are for the two-pass stage over the fine ring. The stage
    // is now one transform here plus the display tap the receiver's own
    // stage records; docs/ui-spectrum.md carries what that measured.
    void record_passbands(Frame& frame) {
        VkCommandBuffer commands = frame.commands;

        std::size_t planned = 0;
        for (FrameVrx& entry : frame.vrxs) {
            if (entry.passband == nullptr) {
                continue;
            }
            PassbandView& view = *entry.passband;
            const auto points = static_cast<dsp::SampleIndex>(view.transform);

            // The window is the last N display samples. It has to be
            // entirely one run of the stream: below display_from the ring
            // holds the stage's clear, samples from before the sink was
            // attached, or samples at a rate a retune has since left, and a
            // window straddling that transforms a step that was never on the
            // air. Expected for the first blocks after a sink is attached or
            // the display rate moves, which is why it is counted rather than
            // logged. A stage that was not asked for the display reports a
            // rate of zero, which lands here too.
            if (entry.output.display_rate <= 0 || entry.output.display_next < points ||
                entry.output.display_next - points < entry.output.display_from) {
                passband_skipped.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            const dsp::SampleIndex first = entry.output.display_next - points;
            const dsp::SampleRate rate = entry.output.display_rate;
            entry.passband_window_offset =
                static_cast<std::uint32_t>(first & view.display.mask);
            entry.passband_geometry =
                passband_geometry_for(rate, view.transform, entry.output.fine_dc_numerator,
                                      entry.output.fine_dc_denominator);
            entry.passband_start = source_index_of_display(view.display, rate, first);
            entry.passband_count =
                source_index_of_display(view.display, rate, entry.output.display_next) -
                entry.passband_start;
            entry.passband_recorded = true;
            ++planned;
        }

        if (planned == 0) {
            return;
        }

        // The display dispatches wrote the windows; the transforms read them.
        // Load-bearing: a stage records its display dispatch beside its fine
        // dispatch and fences only what its own demodulator reads.
        //
        // WHAT THIS USED TO SAY: "strictly redundant today and is recorded
        // anyway", which was true while the windows were in the fine ring
        // and each stage's own barrier covered them.
        record_barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_READ_BIT);

        for (FrameVrx& entry : frame.vrxs) {
            if (!entry.passband_recorded) {
                continue;
            }
            PassbandView& view = *entry.passband;
            const std::size_t slot = entry.readback_index;

            SpectrumPushConstants params;
            params.chan_blocks = view.display.capacity;
            params.chan_mask = view.display.mask;
            params.in_offset = entry.passband_window_offset;
            const auto push = std::as_bytes(std::span<const SpectrumPushConstants>(&params, 1));

            // One workgroup each, and nothing fences them apart: every
            // receiver reads its own ring and writes its own buffer.
            record_dispatch(commands, passband_pipeline, view.pass_set[slot], push, 1);
        }

        record_barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT);

        constexpr VkDeviceSize kFloat = sizeof(float);
        for (FrameVrx& entry : frame.vrxs) {
            if (!entry.passband_recorded) {
                continue;
            }
            PassbandView& view = *entry.passband;
            const std::size_t slot = entry.readback_index;
            const auto half = static_cast<VkDeviceSize>(passband_pass_bins(view.transform));

            VkBufferCopy central{};
            central.srcOffset = half * kFloat;
            central.dstOffset = 0;
            central.size = half * kFloat;
            vkCmdCopyBuffer(commands, view.pass[slot].handle(), view.frame[slot].handle(), 1,
                            &central);
        }

        // Two readers of each frame: the percentile that measures it and the
        // copy that carries it home. The measurement runs on the device for
        // the reason docs/ui-spectrum.md gives about the span, and on the
        // frame the operator sees rather than on the pass buffer, whose lower
        // half is never written.
        record_barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);

        for (FrameVrx& entry : frame.vrxs) {
            if (!entry.passband_recorded) {
                continue;
            }
            PassbandView& view = *entry.passband;
            const std::size_t slot = entry.readback_index;

            SpectrumLevelsPushConstants levels_params;
            levels_params.bins = passband_pass_bins(view.transform);
            levels_params.low_permille = dsp::kSpectrumLowPermille;
            levels_params.high_permille = dsp::kSpectrumHighPermille;

            record_dispatch(
                commands, spectrum_levels_pipeline, view.levels_set[slot],
                std::as_bytes(std::span<const SpectrumLevelsPushConstants>(&levels_params, 1)),
                1);

            VkBufferCopy home{};
            home.srcOffset = 0;
            home.dstOffset = 0;
            home.size = static_cast<VkDeviceSize>(passband_pass_bins(view.transform)) * kFloat;
            vkCmdCopyBuffer(commands, view.frame[slot].handle(), view.readback[slot].handle(),
                            1, &home);
        }

        record_barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT);

        for (FrameVrx& entry : frame.vrxs) {
            if (!entry.passband_recorded) {
                continue;
            }
            PassbandView& view = *entry.passband;
            const std::size_t slot = entry.readback_index;

            VkBufferCopy levels_home{};
            levels_home.srcOffset = 0;
            levels_home.dstOffset = 0;
            levels_home.size = static_cast<VkDeviceSize>(dsp::kSpectrumLevelsOutputs) * kFloat;
            vkCmdCopyBuffer(commands, view.levels_output[slot].handle(),
                            view.levels_readback[slot].handle(), 1, &levels_home);
        }

        // The transform and the percentile. The display tap is the stage's
        // and the stage counts it.
        dispatches.fetch_add(2 * planned, std::memory_order_relaxed);

        // Two per receiver: the frame and its percentiles. The
        // device-to-device copy out of the pass buffer is not a readback and
        // is not counted here.
        readbacks.fetch_add(2 * planned, std::memory_order_relaxed);
    }

    // The completion thread, once per submitted frame, in submission order.
    //
    // This is the whole of what comes back across the bus: one read out of
    // each receiver's mapped readback region, and the ring retirement that
    // tells the writer those samples are finished with. Nothing here touches
    // the device.
    Status complete(std::uint32_t ticket) {
        Status outcome;
        if (ticket >= frames.size()) {
            return fail(std::format("the completion thread was handed frame {} of {}", ticket,
                                    frames.size()));
        }
        Frame& frame = frames[ticket];

        for (auto& entry : frame.vrxs) {
            VrxSlot& slot = *entry.slot;
            const std::size_t floats = static_cast<std::size_t>(entry.output.frames) *
                                       entry.output.channels;
            if (floats == 0) {
                continue;
            }
            // THE TWO FAILURE PATHS BELOW MOVE THE STREAM INDEX ON, and that
            // is what `lost` is for. The frames were produced on the device
            // and this host pass could not carry them, so they are a hole in
            // the receiver's audio. Leaving slot.audio_index where it was
            // would give the next chunk the index these frames should have
            // had, and every consumer downstream reads AudioChunk::start as
            // the stream position: a hole nothing declares reads as
            // continuous audio, which is the one reading that cannot be
            // recovered from later. Counting them in audio_dropped is the
            // same treatment the sink refusal below gets, for the same
            // reason.
            //
            // audio_samples and the graph-wide audio_frames are deliberately
            // not moved here. Those count frames that reached a consumer and
            // these did not.
            const auto lose_frames = [&] {
                slot.audio_index += entry.output.frames;
                slot.audio_dropped.fetch_add(entry.output.frames, std::memory_order_relaxed);
                audio_dropped.fetch_add(entry.output.frames, std::memory_order_relaxed);
            };

            if (floats > slot.scratch.size()) {
                outcome = fail(std::format(
                    "receiver {} produced {} floats and its scratch holds {}: audio_bytes_for "
                    "returned less than the stage went on to write",
                    slot.id.value, floats, slot.scratch.size()));
                lose_frames();
                continue;
            }

            const std::span<float> audio(slot.scratch.data(), floats);
            if (auto read = slot.readback[entry.readback_index].read(std::as_writable_bytes(audio));
                !read) {
                outcome = std::unexpected(with_context(
                    read.error(), std::format("receiver {} readback", slot.id.value)));
                lose_frames();
                continue;
            }

            // The signal meter. Complex output is metered on magnitude, which
            // is what makes a raw tap's level comparable with a demodulator's
            // rather than reading 3 dB low for being two real channels.
            //
            // WHAT THIS BRANCHED ON UNTIL 2026-09-21: entry.output.channels
            // == 2, which read a stereo WFM receiver's L and R pair as one
            // complex sample and divided the sum of squares by frames rather
            // than by samples. Every stereo receiver's level, and therefore
            // its squelch comparison, read 3.01 dB high. StageOutput::
            // complex_iq is the field that says which of the two a pair is,
            // because the channel count cannot.
            const double level =
                meter_dbfs(audio, entry.output.frames, entry.output.complex_iq);
            slot.store_level(level);

            // Squelched means muted, not stopped. The level is stored above
            // this and the passband loop below runs whatever the gate did, so
            // a squelched receiver keeps feeding its meter and its display.
            //
            // THE MUTE IS WRITTEN INTO THE SHARED READBACK BUFFER, so every
            // consumer of this chunk sees the same silence and none of them
            // can opt out. core/engine/vrx.h said a squelched receiver still
            // feeds its decoder chain, and this line has always contradicted
            // it. The header was corrected rather than this line: every sink
            // reachable from here is an audio output, muting an output is what
            // the gate is for, and there is no decoder framework in
            // core/engine for the exemption to have been written for.
            // AudioChunk::squelch_open is what tells a consumer these zeros
            // are the gate rather than the band.
            //
            // WHAT THIS BLOCK USED TO DO. Until 2026-09-20 it also added every
            // muted frame to slot.audio_dropped and to the graph-wide
            // audio_dropped, which core/engine/vrx.h and
            // core/rpc/revenant.capnp both describe as a dropout the operator
            // heard. A closed squelch is not one. The chunk is delivered at
            // the full rate with the sample index unbroken, so nothing is
            // lost and nothing has to be filled; what the listener hears is
            // the threshold they set. Counting it as loss made the field climb
            // for the whole of every quiet channel, which is the one reading
            // that makes it useless for what it is for.
            const bool open = level >= entry.squelch_dbfs;
            slot.squelch_open.store(open, std::memory_order_relaxed);
            if (!open) {
                std::fill(audio.begin(), audio.end(), 0.0F);
            }

            AudioChunk chunk;
            chunk.vrx = slot.id;
            chunk.start = slot.audio_index;
            chunk.rate = entry.output.rate;
            chunk.samples = std::span<const float>(audio.data(), audio.size());
            chunk.channels = entry.output.channels;
            chunk.squelch_open = open;
            chunk.tuning_epoch = entry.tuning_epoch;

            if (entry.sink != nullptr && *entry.sink) {
                if (auto delivered = call_sink(*entry.sink, chunk); !delivered) {
                    // Frames this receiver produced that reached nothing,
                    // which is what audio_dropped counts now that a squelch
                    // mute does not. See the note on VrxStatus::audio_dropped:
                    // a sink refusing a chunk also fails this dispatch and
                    // ends the run, so the counter moves once on the way out
                    // rather than accumulating.
                    slot.audio_dropped.fetch_add(entry.output.frames,
                                                 std::memory_order_relaxed);
                    audio_dropped.fetch_add(entry.output.frames, std::memory_order_relaxed);
                    outcome = std::unexpected(with_context(
                        delivered.error(), std::format("receiver {} sink", slot.id.value)));
                }
            }

            slot.audio_index += entry.output.frames;
            slot.audio_samples.fetch_add(entry.output.frames, std::memory_order_relaxed);
            audio_frames.fetch_add(entry.output.frames, std::memory_order_relaxed);
        }

        // Separate from the audio loop above, and after it, because a
        // squelched receiver still has a passband: the audio is muted and
        // the display is not, for the same reason the level meter keeps
        // reading. `continue` in that loop skips a receiver that produced no
        // audio this dispatch, and one of those can still have produced a
        // frame.
        for (auto& entry : frame.vrxs) {
            if (!entry.passband_recorded) {
                continue;
            }
            if (auto delivered = deliver_passband(entry); !delivered) {
                outcome = std::unexpected(delivered.error());
            }
        }

        if (frame.spectrum_recorded) {
            if (auto delivered = deliver_spectrum(frame); !delivered) {
                outcome = std::unexpected(delivered.error());
            }
            frame.spectrum_recorded = false;
        }
        frame.spectrum_sink.reset();

        // Releases the receivers this frame referenced, so a receiver removed
        // while it was in flight is destroyed here rather than at the next
        // reuse of the slot.
        frame.vrxs.clear();

        if (frame.retire_through > 0) {
            if (auto retired = ring->retire(consumer, frame.retire_through); !retired) {
                outcome = std::unexpected(with_context(retired.error(), "Graph frame retire"));
            }
        }

        // Last, and after everything the frame owns has been read: this is
        // what lets the recording thread reuse the slot.
        completed.fetch_add(1, std::memory_order_release);

        // Bumped after `completed`, so a thread woken by it and reading
        // `completed` sees the retirement this frame just published.
        frame_epoch.fetch_add(1, std::memory_order_release);
        frame_epoch.notify_all();
        return outcome;
    }

    // The completion thread, for one frame that recorded a spectrum.
    //
    // The whole frame is metadata: M*N/2 decibel values for a twenty-megahertz
    // span, plus the two floats the percentile stage measured from it, which
    // is what the display and the detector both want. It is not cheap and the
    // note at the top of core/engine/engine.h has the measured figure, which
    // is about half of what the stream itself costs going out. Nothing
    // complex crosses here.
    Status deliver_spectrum(Frame& frame) {
        const auto bins = static_cast<std::size_t>(geometry.spectrum.bins);
        if (bins == 0 || bins > spectrum_scratch.size()) {
            return fail(std::format(
                "a frame recorded a spectrum of {} bins and the host scratch holds {}", bins,
                spectrum_scratch.size()));
        }
        if (!spectrum_scale.has_value()) {
            return fail("a frame recorded a spectrum and the graph has no colour-map scale, "
                        "which Graph::prepare builds with the stage");
        }

        const std::span<float> values(spectrum_scratch.data(), bins);
        if (auto read = frame.spectrum_readback.read(std::as_writable_bytes(values)); !read) {
            return std::unexpected(with_context(read.error(), "spectrum readback"));
        }

        // Eight bytes, measured on the device from the frame above before
        // either of them moved. The host never scans the frame; see
        // docs/ui-spectrum.md on why that matters and
        // core/shaders/spectrum_levels.comp on how the measurement is made.
        std::array<float, dsp::kSpectrumLevelsOutputs> measured{};
        if (auto read =
                frame.spectrum_levels_readback.read(std::as_writable_bytes(std::span(measured)));
            !read) {
            return std::unexpected(with_context(read.error(), "spectrum levels readback"));
        }

        // Source time since the last frame, from the sample indices rather
        // than from a clock, so a capture replayed at forty times realtime
        // scales the way it did live. A dispatch whose window was not
        // contiguous skips a frame and leaves a gap here, which the
        // exponential crosses in one step.
        double elapsed_seconds = 0.0;
        if (spectrum_have_last && frame.spectrum_start > spectrum_last_start &&
            config.source_rate > 0) {
            elapsed_seconds = static_cast<double>(frame.spectrum_start - spectrum_last_start) /
                              static_cast<double>(config.source_rate);
        }
        spectrum_last_start = frame.spectrum_start;
        spectrum_have_last = true;

        const SpectrumScaleLevels ends =
            spectrum_scale->update(measured[0], measured[1], elapsed_seconds);

        // Counted after the sink, not before, because engine.h promises this
        // is "frames delivered before this one" and a consumer uses it to
        // tell a dropped row from a quiet band. Incremented ahead of the
        // null check, a sink attached after a thousand blocks had run would
        // receive its first frame carrying sequence 1000 and draw a thousand
        // rows it never missed.
        //
        // spectrum_frames is the other number and deliberately counts
        // something else: frames the device produced, whether or not anyone
        // was listening, which is what a stats reader wants.
        spectrum_frames.fetch_add(1, std::memory_order_relaxed);

        if (frame.spectrum_sink == nullptr || !*frame.spectrum_sink) {
            return {};
        }

        SpectrumFrame out;
        out.power_db = std::span<const float>(values.data(), values.size());
        out.geometry = geometry.spectrum;
        out.start = frame.spectrum_start;
        out.count = frame.spectrum_count;
        out.sequence = spectrum_sequence;
        ++spectrum_sequence;
        out.floor_db = ends.floor_db;
        out.ceiling_db = ends.ceiling_db;
        out.percentile_low_db = measured[0];
        out.percentile_high_db = measured[1];

        if (auto delivered = call_sink(*frame.spectrum_sink, out); !delivered) {
            return std::unexpected(with_context(delivered.error(), "spectrum sink"));
        }
        return {};
    }

    // The completion thread, for one receiver that recorded a passband.
    //
    // Shaped exactly like deliver_spectrum above, against per-receiver state
    // instead of the graph's: the frame, the two percentiles measured from it
    // on the device, and a scale of this receiver's own. The one difference
    // is that the sequence counts frames for this receiver rather than for
    // the engine, because a receiver attached an hour into a run has not
    // missed anything.
    Status deliver_passband(FrameVrx& entry) {
        entry.passband_recorded = false;

        PassbandView& view = *entry.passband;
        VrxSlot& slot = *entry.slot;
        const auto bins = static_cast<std::size_t>(entry.passband_geometry.bins);
        if (bins == 0 || bins > view.scratch.size()) {
            return fail(std::format(
                "a frame recorded a passband of {} bins for receiver {} and its host scratch "
                "holds {}",
                bins, slot.id.value, view.scratch.size()));
        }
        if (!slot.passband_scale.has_value()) {
            return fail(std::format(
                "receiver {} recorded a passband and has no colour-map scale, which "
                "Graph::add_vrx builds with the receiver",
                slot.id.value));
        }

        const std::span<float> values(view.scratch.data(), bins);
        if (auto read = view.readback[entry.readback_index].read(std::as_writable_bytes(values));
            !read) {
            return std::unexpected(with_context(
                read.error(), std::format("receiver {} passband readback", slot.id.value)));
        }

        std::array<float, dsp::kSpectrumLevelsOutputs> measured{};
        if (auto read = view.levels_readback[entry.readback_index].read(
                std::as_writable_bytes(std::span(measured)));
            !read) {
            return std::unexpected(with_context(
                read.error(),
                std::format("receiver {} passband levels readback", slot.id.value)));
        }

        // Source time since this receiver's last frame, from the sample
        // indices rather than from a clock, so a capture replayed at forty
        // times realtime scales the way it did live.
        double elapsed_seconds = 0.0;
        if (slot.passband_have_last && entry.passband_start > slot.passband_last_start &&
            config.source_rate > 0) {
            elapsed_seconds =
                static_cast<double>(entry.passband_start - slot.passband_last_start) /
                static_cast<double>(config.source_rate);
        }
        slot.passband_last_start = entry.passband_start;
        slot.passband_have_last = true;

        const SpectrumScaleLevels ends =
            slot.passband_scale->update(measured[0], measured[1], elapsed_seconds);

        // Counted before the null check and after the measurement, for the
        // reason deliver_spectrum gives at length: this counts what the
        // device produced, and the sequence below counts what was delivered.
        passband_frames.fetch_add(1, std::memory_order_relaxed);

        if (entry.passband_sink == nullptr || !*entry.passband_sink) {
            return {};
        }

        PassbandFrame out;
        out.vrx = slot.id;
        out.power_db = std::span<const float>(values.data(), values.size());
        out.geometry = entry.passband_geometry;
        out.start = entry.passband_start;
        out.count = entry.passband_count;
        out.sequence = slot.passband_sequence;
        ++slot.passband_sequence;
        out.floor_db = ends.floor_db;
        out.ceiling_db = ends.ceiling_db;
        out.percentile_low_db = measured[0];
        out.percentile_high_db = measured[1];

        if (auto delivered = call_sink(*entry.passband_sink, out); !delivered) {
            return std::unexpected(with_context(
                delivered.error(), std::format("receiver {} passband sink", slot.id.value)));
        }
        return {};
    }
};

Graph::Graph() : impl_(std::make_unique<Impl>()) {}

Graph::~Graph() {
    if (impl_ == nullptr) {
        return;
    }

    // The completion thread runs a handler that holds a pointer to this
    // object's Impl, so it has to be joined before anything here is freed.
    // prepare() is what started it, so the destructor is what stops it: a
    // caller who had to remember would eventually not.
    if (impl_->scheduler != nullptr) {
        impl_->scheduler->stop();
    }

    // Everything the frames own is referenced by submissions that may still be
    // executing. There is nowhere to report a failure from a destructor, so
    // the wait is unconditional and its result discarded.
    if (impl_->context != nullptr && impl_->context->device() != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(impl_->context->device());
    }
}

Expected<std::unique_ptr<Graph>> Graph::create(const gpu::Context& context, DeviceRing& ring,
                                               Scheduler& scheduler, const GraphConfig& config) {
    if (!context.valid()) {
        return fail("Graph::create called with an invalid gpu::Context");
    }
    if (config.source_rate <= 0) {
        return fail("Graph::create needs the source's sample rate");
    }
    if (auto ok = dsp::validate(config.grid); !ok) {
        return std::unexpected(with_context(ok.error(), "Graph::create"));
    }
    if (config.frames_in_flight == 0 || config.frames_in_flight > 8) {
        return fail(std::format("frames_in_flight is {}; one is a stall and more than eight is a "
                                "latency budget nobody asked for",
                                config.frames_in_flight));
    }
    if (config.block_samples == 0) {
        return fail("Graph::create needs a non-zero block size");
    }
    if (ring.geometry().capacity_samples == 0) {
        return fail("Graph::create was given a ring that was never created");
    }

    const auto& device = context.info();
    const std::uint32_t fft_bytes = dsp::fft_shared_bytes(config.grid.channels);
    if (fft_bytes > device.max_workgroup_shared_memory) {
        return fail(std::format(
            "a {}-channel transform needs {} bytes of shared memory and '{}' offers {}. The "
            "largest power-of-two grid this device holds in one pass is {} channels",
            config.grid.channels, fft_bytes, device.name, device.max_workgroup_shared_memory,
            dsp::max_fft_transform_size(device.max_workgroup_shared_memory)));
    }

    std::unique_ptr<Graph> graph(new (std::nothrow) Graph());
    if (graph == nullptr || graph->impl_ == nullptr) {
        return fail("Graph::create could not allocate the graph");
    }
    auto& impl = *graph->impl_;
    impl.context = &context;
    impl.ring = &ring;
    impl.scheduler = &scheduler;
    impl.config = config;
    impl.grid = config.grid;
    impl.prototype_length = config.grid.prototype_length();
    impl.factory = installed_vrx_stage_factory();

    // Workgroup sizes. Never hardcoded: the conformance matrix runs the same
    // modules on a device with a smaller limit, and a kernel that bakes one in
    // cannot be launched on the device that would have caught the bug.
    const std::uint32_t ceiling =
        std::min(device.max_workgroup_size_x, device.max_workgroup_invocations);
    if (ceiling == 0) {
        return fail(std::format("'{}' reports a maximum workgroup size of zero", device.name));
    }
    std::uint32_t local_size = config.local_size_x != 0 ? config.local_size_x
                                                        : gpu::kDefaultLocalSizeX;
    local_size = std::min(local_size, ceiling);

    // The transform is one workgroup per block with the threads striding, so
    // half the channel count is the natural width: every thread owns one
    // butterfly per stage with none left over.
    std::uint32_t fft_local = std::max(1U, config.grid.channels / 2);
    fft_local = std::min(fft_local, ceiling);
    fft_local = std::min(fft_local, 256U);

    const auto decimation = static_cast<std::uint64_t>(config.grid.decimation);

    // Blocks one dispatch produces at most: a full source block's worth,
    // rounded down, and never zero.
    auto max_blocks = static_cast<std::uint32_t>(
        std::max<std::uint64_t>(1, static_cast<std::uint64_t>(config.block_samples) / decimation));

    // --- the spectrum stage, sized before the ring is sized around it -------

    SpectrumGeometry spectrum;
    std::string stage_note;
    if (config.spectrum_transform != 0) {
        const std::uint32_t transform_ceiling =
            dsp::max_spectrum_transform_size(device.max_workgroup_shared_memory);
        if (transform_ceiling == 0) {
            return fail(std::format(
                "'{}' reports {} bytes of workgroup shared memory, which holds no spectrum "
                "transform at all",
                device.name, device.max_workgroup_shared_memory));
        }

        std::uint32_t transform = config.spectrum_transform;
        if (transform > transform_ceiling) {
            // Which of the two ceilings bound is a different fact about the
            // world, and this sentence is the only field EngineInfo has to
            // say it in. max_spectrum_transform_size returns the lesser of
            // what this device's shared memory holds and
            // dsp::kMaxSpectrumTransform, the largest circle build_twiddles
            // will build, so naming shared memory unconditionally tells a
            // client on a large card that its transform needed fewer bytes
            // than the card offers and was cut anyway. Read across the wire
            // that is a reason to go buy a bigger GPU, which cannot lift a
            // cap the GPU did not set.
            const std::uint32_t device_ceiling =
                dsp::max_fft_transform_size(device.max_workgroup_shared_memory);
            stage_note =
                device_ceiling < dsp::kMaxSpectrumTransform
                    ? std::format("a {}-point spectrum transform needs {} bytes of shared memory "
                                  "and '{}' offers {}, so it was reduced to {} points",
                                  transform, dsp::fft_shared_bytes(transform), device.name,
                                  device.max_workgroup_shared_memory, transform_ceiling)
                    : std::format("a {}-point spectrum transform is above the {}-point ceiling "
                                  "core/dsp/pfb_design.cpp's twiddle table sets, so it was "
                                  "reduced to {} points. That ceiling is not this device's: '{}' "
                                  "has shared memory for {} points, and a larger card does not "
                                  "raise it",
                                  transform, dsp::kMaxSpectrumTransform, transform_ceiling,
                                  device.name, device_ceiling);
            transform = transform_ceiling;
        }

        auto made = spectrum_geometry_for(config.grid, config.source_rate, transform);
        if (!made) {
            return std::unexpected(with_context(made.error(), "Graph::create"));
        }
        spectrum = *made;

        // Refused here rather than at the first frame, because a pinned pair
        // with no range in it is a mistake in a command line and the person
        // who made it is still watching this output.
        SpectrumScaleConfig scale;
        scale.pinned_floor_db = config.spectrum_floor_db;
        scale.pinned_ceiling_db = config.spectrum_ceiling_db;
        if (auto ok = SpectrumScale::create(scale); !ok) {
            return std::unexpected(with_context(ok.error(), "Graph::create"));
        }
    }

    // One workgroup per coarse channel with the threads striding, so half the
    // transform is the natural width: every thread owns one butterfly per
    // stage with none left over. The same reasoning as the channelizer's
    // transform above, against a different size.
    std::uint32_t spectrum_local = 0;

    // The levels kernel is one workgroup reading the whole frame, so its
    // width is the only parallelism it has and wider is strictly better up to
    // the point where the histogram's atomics start colliding. 256 is the
    // same cap the transforms take and is what a subgroup-friendly width
    // looks like on both devices in the matrix.
    std::uint32_t spectrum_levels_local = 0;
    if (spectrum.enabled()) {
        spectrum_local = std::max(1U, spectrum.transform / 2);
        spectrum_local = std::min(spectrum_local, ceiling);
        spectrum_local = std::min(spectrum_local, 256U);
    }

    // --- the passband stage, which shares the spectrum kernel ---------------

    std::uint32_t passband_transform = 0;
    std::uint32_t passband_local = 0;
    if (config.passband_transform != 0) {
        const std::uint32_t transform_ceiling =
            dsp::max_spectrum_transform_size(device.max_workgroup_shared_memory);
        if (transform_ceiling == 0) {
            return fail(std::format(
                "'{}' reports {} bytes of workgroup shared memory, which holds no passband "
                "transform at all",
                device.name, device.max_workgroup_shared_memory));
        }

        // Four points is the smallest core/shaders/spectrum.comp's own
        // validate() accepts, and below it there is no central half to place.
        if (config.passband_transform < 4 || !std::has_single_bit(config.passband_transform)) {
            return fail(std::format(
                "passband_transform is {}; it is a transform size, so a power of two of at "
                "least four",
                config.passband_transform));
        }

        passband_transform = std::min(config.passband_transform, transform_ceiling);
        if (passband_transform < config.passband_transform) {
            const std::uint32_t device_ceiling =
                dsp::max_fft_transform_size(device.max_workgroup_shared_memory);
            const std::string note =
                device_ceiling < dsp::kMaxSpectrumTransform
                    ? std::format("a {}-point passband transform needs {} bytes of shared "
                                  "memory and '{}' offers {}, so it was reduced to {} points",
                                  config.passband_transform,
                                  dsp::fft_shared_bytes(config.passband_transform), device.name,
                                  device.max_workgroup_shared_memory, passband_transform)
                    : std::format("a {}-point passband transform is above the {}-point ceiling "
                                  "core/dsp/pfb_design.cpp's twiddle table sets, so it was "
                                  "reduced to {} points. That ceiling is not this device's: "
                                  "'{}' has shared memory for {} points",
                                  config.passband_transform, dsp::kMaxSpectrumTransform,
                                  passband_transform, device.name, device_ceiling);
            if (!stage_note.empty()) {
                stage_note.append("; also ");
            }
            stage_note.append(note);
        }

        passband_local = std::max(1U, passband_transform / 2);
        passband_local = std::min(passband_local, ceiling);
        passband_local = std::min(passband_local, 256U);
    }

    // One width serves both, because one kernel does. Built whenever either
    // stage is, since the passband measures its own two percentiles with the
    // same module the span does.
    if (spectrum.enabled() || passband_transform != 0) {
        spectrum_levels_local = std::min(ceiling, 256U);
    }

    std::uint32_t ring_blocks = config.channel_ring_blocks;
    if (ring_blocks == 0) {
        // Every frame in flight owns a disjoint range, which is what lets
        // frames overlap on the device at all. One spare dispatch of headroom
        // so a receiver reading the oldest frame is never adjacent to the
        // newest write.
        std::uint64_t wanted =
            static_cast<std::uint64_t>(max_blocks) * (config.frames_in_flight + 1);

        // The spectrum reaches further back than anything else does. Its
        // window is the last N blocks of every channel, and the frames
        // submitted behind it are writing above that, so the ring has to hold
        // both at once or a transform reads slots the next dispatch has
        // already overwritten.
        if (spectrum.enabled()) {
            wanted = std::max(wanted, static_cast<std::uint64_t>(spectrum.transform) +
                                          static_cast<std::uint64_t>(max_blocks) *
                                              config.frames_in_flight);
        }

        // Every receiver's display tap reads dsp::kDisplayTaps channel
        // samples below the block it is filtering, and the frames submitted
        // behind it are writing above that, for the spectrum's reason above.
        // The stage refuses a ring that cannot hold the reach and every frame
        // in flight at once.
        //
        // WHAT THIS USED TO COUNT: `max_blocks + kDisplayTaps`, one dispatch
        // above the reach and not frames_in_flight of them. At 100 blocks a
        // dispatch and three frames that is 512 blocks where the 256 tap
        // display filter needs 556, and a spectrum stage of 256 points or more
        // hid it by making the ring larger for its own reasons.
        if (config.passband_transform != 0) {
            wanted = std::max(wanted, static_cast<std::uint64_t>(max_blocks) *
                                              config.frames_in_flight +
                                          dsp::kDisplayTaps);
        }
        ring_blocks = static_cast<std::uint32_t>(std::bit_ceil(wanted));
    }
    if (!std::has_single_bit(ring_blocks)) {
        return fail(std::format("channel_ring_blocks is {}, which is not a power of two; the "
                                "kernel masks the block index",
                                ring_blocks));
    }

    const std::uint64_t channel_ring_bytes = static_cast<std::uint64_t>(config.grid.channels) *
                                             ring_blocks * kComplexBytes;
    if (channel_ring_bytes > device.max_storage_buffer_range) {
        return fail(std::format(
            "a {}-channel ring of {} blocks needs {} bytes and '{}' binds at most {}",
            config.grid.channels, ring_blocks, channel_ring_bytes, device.name,
            device.max_storage_buffer_range));
    }

    impl.geometry.channel_rate = config.source_rate / static_cast<dsp::SampleRate>(decimation);
    impl.geometry.channel_ring_blocks = ring_blocks;
    impl.geometry.channel_ring_mask = ring_blocks - 1;
    impl.geometry.max_blocks_per_dispatch = std::min(max_blocks, ring_blocks);
    impl.geometry.frames_in_flight = config.frames_in_flight;
    impl.geometry.local_size_x = local_size;
    impl.geometry.fft_local_size_x = fft_local;
    impl.geometry.spectrum_local_size_x = spectrum_local;
    impl.geometry.spectrum_levels_local_size_x = spectrum_levels_local;
    impl.geometry.spectrum = spectrum;
    impl.geometry.passband_transform = passband_transform;
    impl.geometry.passband_local_size_x = passband_local;
    impl.geometry.block_samples = config.block_samples;
    impl.geometry.channel_ring_bytes = channel_ring_bytes;
    impl.geometry.staging_bytes = staging_bytes_for(config.format, config.block_samples);

    if (impl.geometry.max_blocks_per_dispatch < max_blocks) {
        impl.geometry.clamped = true;
        impl.geometry.clamp_reason =
            std::format("a block of {} samples would produce {} output blocks and the channel "
                        "ring holds {}, so each dispatch produces at most {}",
                        config.block_samples, max_blocks, ring_blocks,
                        impl.geometry.max_blocks_per_dispatch);
    }
    if (!stage_note.empty()) {
        impl.geometry.clamped = true;
        if (!impl.geometry.clamp_reason.empty()) {
            impl.geometry.clamp_reason.append("; also ");
        }
        impl.geometry.clamp_reason.append(stage_note);
    }

    // A caller who pinned channel_ring_blocks can pin one too small for the
    // spectrum's window, and the failure on the device is a frame stitched
    // out of two eras that looks like a band full of signal. Checked here,
    // where the numbers are, rather than at the first dispatch.
    if (spectrum.enabled()) {
        const std::uint64_t needed =
            static_cast<std::uint64_t>(spectrum.transform) +
            static_cast<std::uint64_t>(impl.geometry.max_blocks_per_dispatch) *
                config.frames_in_flight;
        if (needed > ring_blocks) {
            return fail(std::format(
                "a {}-point spectrum window plus {} frames of {} blocks each needs a channel "
                "ring of {} blocks and this one holds {}",
                spectrum.transform, config.frames_in_flight,
                impl.geometry.max_blocks_per_dispatch, needed, ring_blocks));
        }
    }

    // The ring must hold at least the filter support plus a whole block, or a
    // dispatch can never assemble its own window.
    const auto needed = static_cast<dsp::SampleIndex>(impl.prototype_length) +
                        static_cast<dsp::SampleIndex>(config.block_samples);
    if (ring.geometry().capacity_samples < needed) {
        return fail(std::format(
            "the device ring holds {} samples and one dispatch needs {}: {} of filter support "
            "plus a {}-sample block",
            ring.geometry().capacity_samples, needed, impl.prototype_length,
            config.block_samples));
    }

    // Blocking against a Demand source is the backpressure that makes offline
    // replay run at the speed the GPU allows. Lossy against a Paced one,
    // because a radio's clock does not wait and stalling the writer would turn
    // this consumer being slow into lost radio samples.
    //
    // Through register_consumer rather than DeviceRing::add_consumer so the
    // pairing is checked against the source's flow control instead of being
    // trusted to the line above. RingConfig carries no flow control, so the
    // ring cannot make that check on its own.
    const ConsumerKind kind = config.flow == source::FlowControl::Demand ? ConsumerKind::Blocking
                                                                         : ConsumerKind::Lossy;
    auto registered = register_consumer(ring, config.flow, kind);
    if (!registered) {
        return std::unexpected(with_context(registered.error(), "Graph::create"));
    }
    impl.consumer = *registered;

    VkSemaphoreTypeCreateInfo type{};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = 0;

    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphore_info.pNext = &type;

    VkResult result =
        vkCreateSemaphore(context.device(), &semaphore_info, nullptr, &impl.timeline);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkCreateSemaphore (timeline) failed ({})",
                                gpu::result_name(result)),
                    result);
    }

    // Up to five sets per frame, written once and never rewritten: every
    // buffer the coarse chain binds is fixed for the life of the graph or
    // fixed per frame, so a per-block descriptor update would be pure
    // ceremony. The last two are the spectrum's and its percentile's,
    // allocated whether or not the stage is built, because a pool is cheap
    // and sizing it two ways is one more thing to get wrong.
    //
    // The buffer count is the sum of the five kernels' bindings: convert 2,
    // branch 3, transform 3, spectrum 4, levels 2.
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = config.frames_in_flight * 14;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = config.frames_in_flight * 5;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;

    result = vkCreateDescriptorPool(context.device(), &pool_info, nullptr, &impl.descriptors);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkCreateDescriptorPool failed ({})", gpu::result_name(result)),
                    result);
    }

    return graph;
}

Status Graph::prepare(const dsp::PrototypeFilter& prototype,
                      std::span<const dsp::Complex32> twiddles) {
    auto& impl = *impl_;
    if (impl.prepared) {
        return fail("Graph::prepare called twice: the coarse stage is built once per open "
                    "source and adding a receiver must not rebuild it");
    }
    if (prototype.taps.size() != impl.prototype_length) {
        return fail(std::format("the prototype has {} taps and the grid needs {}",
                                prototype.taps.size(), impl.prototype_length));
    }
    if (twiddles.size() != impl.grid.channels) {
        return fail(std::format("the twiddle table has {} entries and the grid needs {}",
                                twiddles.size(), impl.grid.channels));
    }

    const gpu::Context& context = *impl.context;
    const VkDevice device = context.device();

    // --- the tables, uploaded once ------------------------------------------

    auto make_device_buffer = [&](VkDeviceSize bytes) -> Expected<gpu::Buffer> {
        return gpu::Buffer::create(context, bytes, kDeviceStorage, gpu::MemoryKind::DeviceLocal);
    };

    const VkDeviceSize prototype_bytes = prototype.taps.size() * sizeof(float);
    auto prototype_buffer = make_device_buffer(prototype_bytes);
    if (!prototype_buffer) {
        return std::unexpected(with_context(prototype_buffer.error(), "Graph::prepare prototype"));
    }
    const VkDeviceSize twiddle_bytes = twiddles.size() * sizeof(dsp::Complex32);
    auto twiddle_buffer = make_device_buffer(twiddle_bytes);
    if (!twiddle_buffer) {
        return std::unexpected(with_context(twiddle_buffer.error(), "Graph::prepare twiddles"));
    }
    auto channel_buffer = make_device_buffer(impl.geometry.channel_ring_bytes);
    if (!channel_buffer) {
        return std::unexpected(with_context(channel_buffer.error(), "Graph::prepare channel ring"));
    }

    impl.prototype_buffer = std::move(*prototype_buffer);
    impl.twiddle_buffer = std::move(*twiddle_buffer);
    impl.channel_ring = std::move(*channel_buffer);

    // The spectrum stage's own tables, built here rather than handed in: both
    // are a function of the transform size this graph settled on, and the
    // caller does not know that until create() has clamped it against the
    // device.
    std::vector<dsp::Complex32> spectrum_twiddle_table;
    std::vector<float> spectrum_window_taps;
    VkDeviceSize spectrum_twiddle_bytes = 0;
    VkDeviceSize spectrum_window_bytes = 0;
    if (impl.geometry.spectrum.enabled()) {
        const std::uint32_t points = impl.geometry.spectrum.transform;

        auto circle = dsp::build_twiddles(points);
        if (!circle) {
            return std::unexpected(with_context(circle.error(), "Graph::prepare spectrum"));
        }
        spectrum_twiddle_table = std::move(*circle);

        // The tap count matters and the attenuation is the default.
        //
        // The window's second half is a correction for the prototype filter's
        // own droop across each channel's kept band, so it has to be built
        // for the prototype this grid actually has. engine.cpp designs that
        // one with design_prototype(grid), which takes the default 120 dB,
        // so only the tap count needs carrying. Leaving it defaulted was
        // correct at --taps 17 and silently wrong anywhere else: measured,
        // 9 taps against 17 is 0.93 dB of residual droop near every seam and
        // 33 taps is 1.75 dB, which lands straight in a reported SNR.
        auto window = dsp::build_spectrum_window(points, impl.grid.taps_per_branch);
        if (!window) {
            return std::unexpected(with_context(window.error(), "Graph::prepare spectrum"));
        }
        spectrum_window_taps = std::move(*window);

        spectrum_twiddle_bytes = spectrum_twiddle_table.size() * sizeof(dsp::Complex32);
        spectrum_window_bytes = spectrum_window_taps.size() * sizeof(float);

        auto device_twiddles = make_device_buffer(spectrum_twiddle_bytes);
        if (!device_twiddles) {
            return std::unexpected(
                with_context(device_twiddles.error(), "Graph::prepare spectrum twiddles"));
        }
        impl.spectrum_twiddles = std::move(*device_twiddles);

        auto device_window = make_device_buffer(spectrum_window_bytes);
        if (!device_window) {
            return std::unexpected(
                with_context(device_window.error(), "Graph::prepare spectrum window"));
        }
        impl.spectrum_window = std::move(*device_window);
    }

    // The passband stage's two tables. Shared by every receiver, because
    // neither depends on the receiver: the twiddle circle and the window are
    // functions of the transform size alone.
    std::vector<dsp::Complex32> passband_twiddle_table;
    std::vector<float> passband_window_taps;
    VkDeviceSize passband_twiddle_bytes = 0;
    VkDeviceSize passband_window_bytes = 0;
    if (impl.geometry.passband_transform != 0) {
        const std::uint32_t points = impl.geometry.passband_transform;

        auto circle = dsp::build_twiddles(points);
        if (!circle) {
            return std::unexpected(with_context(circle.error(), "Graph::prepare passband"));
        }
        passband_twiddle_table = std::move(*circle);

        auto window = passband_window(points);
        if (!window) {
            return std::unexpected(with_context(window.error(), "Graph::prepare passband"));
        }
        passband_window_taps = std::move(*window);

        passband_twiddle_bytes = passband_twiddle_table.size() * sizeof(dsp::Complex32);
        passband_window_bytes = passband_window_taps.size() * sizeof(float);

        auto device_twiddles = make_device_buffer(passband_twiddle_bytes);
        if (!device_twiddles) {
            return std::unexpected(
                with_context(device_twiddles.error(), "Graph::prepare passband twiddles"));
        }
        impl.passband_twiddles = std::move(*device_twiddles);

        auto device_window = make_device_buffer(passband_window_bytes);
        if (!device_window) {
            return std::unexpected(
                with_context(device_window.error(), "Graph::prepare passband window"));
        }
        impl.passband_window_table = std::move(*device_window);
    }

    {
        // A CommandRunner here and nowhere else in this file. Uploading the
        // filter tables is not the sample path, it happens once, and it wants
        // exactly the submit-and-wait shape the runner provides.
        auto runner = gpu::CommandRunner::create(context);
        if (!runner) {
            return std::unexpected(with_context(runner.error(), "Graph::prepare"));
        }

        auto prototype_staging = gpu::Buffer::create(context, prototype_bytes,
                                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                     gpu::MemoryKind::Upload);
        if (!prototype_staging) {
            return std::unexpected(
                with_context(prototype_staging.error(), "Graph::prepare prototype staging"));
        }
        if (auto wrote = prototype_staging->write(std::as_bytes(std::span(prototype.taps)));
            !wrote) {
            return std::unexpected(with_context(wrote.error(), "Graph::prepare prototype"));
        }

        auto twiddle_staging = gpu::Buffer::create(context, twiddle_bytes,
                                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                   gpu::MemoryKind::Upload);
        if (!twiddle_staging) {
            return std::unexpected(
                with_context(twiddle_staging.error(), "Graph::prepare twiddle staging"));
        }
        if (auto wrote = twiddle_staging->write(std::as_bytes(twiddles)); !wrote) {
            return std::unexpected(with_context(wrote.error(), "Graph::prepare twiddles"));
        }

        // Zeroed, not left as the allocator handed it back. A dispatch writes
        // only the blocks it produced, and a receiver reading a slot no
        // dispatch has touched would otherwise demodulate whatever was last in
        // that memory. The reference suite learned this the same way; see the
        // note on CommandRunner::clear.
        const gpu::CommandRunner::BufferClear clears[] = {
            {impl.channel_ring.handle(), impl.channel_ring.size()},
        };
        if (auto cleared = runner->clear(clears); !cleared) {
            return std::unexpected(with_context(cleared.error(), "Graph::prepare"));
        }

        std::vector<gpu::CommandRunner::BufferCopy> copies{
            {prototype_staging->handle(), impl.prototype_buffer.handle(), prototype_bytes},
            {twiddle_staging->handle(), impl.twiddle_buffer.handle(), twiddle_bytes},
        };

        // Declared out here so they outlive the copy, which is recorded and
        // submitted below rather than as each one is added.
        gpu::Buffer spectrum_twiddle_staging;
        gpu::Buffer spectrum_window_staging;
        if (impl.geometry.spectrum.enabled()) {
            auto staged = gpu::Buffer::create(context, spectrum_twiddle_bytes,
                                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                              gpu::MemoryKind::Upload);
            if (!staged) {
                return std::unexpected(
                    with_context(staged.error(), "Graph::prepare spectrum twiddle staging"));
            }
            spectrum_twiddle_staging = std::move(*staged);
            if (auto wrote = spectrum_twiddle_staging.write(
                    std::as_bytes(std::span(spectrum_twiddle_table)));
                !wrote) {
                return std::unexpected(
                    with_context(wrote.error(), "Graph::prepare spectrum twiddles"));
            }

            auto window_staged = gpu::Buffer::create(context, spectrum_window_bytes,
                                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                     gpu::MemoryKind::Upload);
            if (!window_staged) {
                return std::unexpected(with_context(window_staged.error(),
                                                    "Graph::prepare spectrum window staging"));
            }
            spectrum_window_staging = std::move(*window_staged);
            if (auto wrote = spectrum_window_staging.write(
                    std::as_bytes(std::span(spectrum_window_taps)));
                !wrote) {
                return std::unexpected(
                    with_context(wrote.error(), "Graph::prepare spectrum window"));
            }

            copies.push_back({spectrum_twiddle_staging.handle(), impl.spectrum_twiddles.handle(),
                              spectrum_twiddle_bytes});
            copies.push_back({spectrum_window_staging.handle(), impl.spectrum_window.handle(),
                              spectrum_window_bytes});
        }

        gpu::Buffer passband_twiddle_staging;
        gpu::Buffer passband_window_staging;
        if (impl.geometry.passband_transform != 0) {
            auto stage_upload = [&](VkDeviceSize bytes, std::span<const std::byte> source,
                                    const char* what) -> Expected<gpu::Buffer> {
                auto staged = gpu::Buffer::create(context, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                  gpu::MemoryKind::Upload);
                if (!staged) {
                    return std::unexpected(with_context(staged.error(), what));
                }
                if (auto wrote = staged->write(source); !wrote) {
                    return std::unexpected(with_context(wrote.error(), what));
                }
                return staged;
            };

            auto twiddles_staged =
                stage_upload(passband_twiddle_bytes,
                             std::as_bytes(std::span(passband_twiddle_table)),
                             "Graph::prepare passband twiddle staging");
            if (!twiddles_staged) {
                return std::unexpected(twiddles_staged.error());
            }
            passband_twiddle_staging = std::move(*twiddles_staged);

            auto window_staged = stage_upload(passband_window_bytes,
                                              std::as_bytes(std::span(passband_window_taps)),
                                              "Graph::prepare passband window staging");
            if (!window_staged) {
                return std::unexpected(window_staged.error());
            }
            passband_window_staging = std::move(*window_staged);

            copies.push_back({passband_twiddle_staging.handle(), impl.passband_twiddles.handle(),
                              passband_twiddle_bytes});
            copies.push_back({passband_window_staging.handle(),
                              impl.passband_window_table.handle(), passband_window_bytes});
        }

        if (auto copied = runner->copy(copies); !copied) {
            return std::unexpected(with_context(copied.error(), "Graph::prepare"));
        }
    }

    // --- the three coarse pipelines, built once ------------------------------

    const std::uint32_t branch_constants[] = {impl.grid.channels, impl.grid.taps_per_branch,
                                              impl.grid.decimation};
    const std::uint32_t fft_constants[] = {impl.grid.channels, impl.grid.decimation,
                                           dsp::fft_stages(impl.grid.channels)};

    impl.has_convert = impl.config.format != source::SampleFormat::Cf32;
    if (impl.has_convert) {
        gpu::ComputePipeline::Options options;
        options.spirv = convert_shader_for(impl.config.format);
        options.storage_buffer_count = 2;
        options.local_size_x = impl.geometry.local_size_x;
        options.push_constant_bytes = sizeof(dsp::ConvertParams);
        auto pipeline = gpu::ComputePipeline::create(context, options);
        if (!pipeline) {
            return std::unexpected(with_context(pipeline.error(), "Graph::prepare convert"));
        }
        impl.convert_pipeline = std::move(*pipeline);
    }

    {
        gpu::ComputePipeline::Options options;
        options.spirv = gpu::shaders::pfb_branch();
        options.storage_buffer_count = 3;
        options.local_size_x = impl.geometry.local_size_x;
        options.push_constant_bytes = sizeof(dsp::PfbBranchParams);
        options.grid_constants = branch_constants;
        auto pipeline = gpu::ComputePipeline::create(context, options);
        if (!pipeline) {
            return std::unexpected(with_context(pipeline.error(), "Graph::prepare branch"));
        }
        impl.branch_pipeline = std::move(*pipeline);
    }

    {
        gpu::ComputePipeline::Options options;
        options.spirv = gpu::shaders::pfb_fft();
        options.storage_buffer_count = 3;
        options.local_size_x = impl.geometry.fft_local_size_x;
        options.push_constant_bytes = sizeof(FftPushConstants);
        options.grid_constants = fft_constants;
        auto pipeline = gpu::ComputePipeline::create(context, options);
        if (!pipeline) {
            return std::unexpected(with_context(pipeline.error(), "Graph::prepare fft"));
        }
        impl.fft_pipeline = std::move(*pipeline);
    }

    if (impl.geometry.spectrum.enabled()) {
        const std::uint32_t spectrum_constants[] = {
            impl.geometry.spectrum.channels, impl.geometry.spectrum.transform,
            dsp::fft_stages(impl.geometry.spectrum.transform)};

        gpu::ComputePipeline::Options options;
        options.spirv = gpu::shaders::spectrum();
        options.storage_buffer_count = 4;
        options.local_size_x = impl.geometry.spectrum_local_size_x;
        options.push_constant_bytes = sizeof(SpectrumPushConstants);
        options.grid_constants = spectrum_constants;
        auto pipeline = gpu::ComputePipeline::create(context, options);
        if (!pipeline) {
            return std::unexpected(with_context(pipeline.error(), "Graph::prepare spectrum"));
        }
        impl.spectrum_pipeline = std::move(*pipeline);
    }

    if (impl.geometry.passband_transform != 0) {
        // One channel, because a display stream is one stream. Two is what
        // core/shaders/spectrum.comp's slot arithmetic and
        // dsp::validate(SpectrumParams) both take as the floor, and the
        // graph dispatches exactly one workgroup against it, so the second
        // channel is never entered and its origin is never computed. The
        // cost of saying two is that channel zero writes at slot one, which
        // is why a pass buffer is a whole transform wide and only its upper
        // half is used.
        const std::uint32_t passband_constants[] = {
            2U, impl.geometry.passband_transform,
            dsp::fft_stages(impl.geometry.passband_transform)};

        gpu::ComputePipeline::Options options;
        options.spirv = gpu::shaders::spectrum();
        options.storage_buffer_count = 4;
        options.local_size_x = impl.geometry.passband_local_size_x;
        options.push_constant_bytes = sizeof(SpectrumPushConstants);
        options.grid_constants = passband_constants;
        auto pipeline = gpu::ComputePipeline::create(context, options);
        if (!pipeline) {
            return std::unexpected(with_context(pipeline.error(), "Graph::prepare passband"));
        }
        impl.passband_pipeline = std::move(*pipeline);
    }

    // The percentile, measured on the device from a frame before it moves.
    //
    // No grid constants: this kernel's shape is entirely in its push
    // constants, so one pipeline serves every geometry, and that is what lets
    // one of them serve both the span and every receiver's passband. The
    // kernel beside it does the opposite, and docs/fft.md records an
    // integrated-device fault that tracks exactly how many pipelines one
    // module is specialized into, so this one adds none.
    if (impl.geometry.spectrum.enabled() || impl.geometry.passband_transform != 0) {
        gpu::ComputePipeline::Options levels;
        levels.spirv = gpu::shaders::spectrum_levels();
        levels.storage_buffer_count = 2;
        levels.local_size_x = impl.geometry.spectrum_levels_local_size_x;
        levels.push_constant_bytes = sizeof(SpectrumLevelsPushConstants);
        auto levels_pipeline = gpu::ComputePipeline::create(context, levels);
        if (!levels_pipeline) {
            return std::unexpected(
                with_context(levels_pipeline.error(), "Graph::prepare spectrum levels"));
        }
        impl.spectrum_levels_pipeline = std::move(*levels_pipeline);
    }

    impl.coarse_builds.fetch_add(1, std::memory_order_relaxed);

    // --- one frame's worth of everything, times frames_in_flight ------------

    const VkDeviceSize branch_bytes = static_cast<VkDeviceSize>(
        impl.geometry.max_blocks_per_dispatch * impl.grid.channels * kComplexBytes);

    impl.frames.resize(impl.config.frames_in_flight);
    for (std::uint32_t i = 0; i < impl.config.frames_in_flight; ++i) {
        auto& frame = impl.frames[i];

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = context.compute_family();

        VkResult result = vkCreateCommandPool(device, &pool_info, nullptr, &frame.pool);
        if (result != VK_SUCCESS) {
            return fail(std::format("vkCreateCommandPool failed for frame {} ({})", i,
                                    gpu::result_name(result)),
                        result);
        }

        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = frame.pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;

        result = vkAllocateCommandBuffers(device, &alloc, &frame.commands);
        if (result != VK_SUCCESS) {
            return fail(std::format("vkAllocateCommandBuffers failed for frame {} ({})", i,
                                    gpu::result_name(result)),
                        result);
        }

        auto staging = gpu::Buffer::create(context, impl.geometry.staging_bytes, kStagingUsage,
                                           gpu::MemoryKind::Upload);
        if (!staging) {
            return std::unexpected(with_context(staging.error(), "Graph::prepare staging"));
        }
        frame.staging = std::move(*staging);

        auto branch = gpu::Buffer::create(context, branch_bytes, kDeviceStorage,
                                          gpu::MemoryKind::DeviceLocal);
        if (!branch) {
            return std::unexpected(with_context(branch.error(), "Graph::prepare branch scratch"));
        }
        frame.branch_output = std::move(*branch);

        if (impl.geometry.spectrum.enabled()) {
            const VkDeviceSize frame_bytes =
                static_cast<VkDeviceSize>(impl.geometry.spectrum.bins) * sizeof(float);

            auto output = gpu::Buffer::create(context, frame_bytes, kDeviceStorage,
                                              gpu::MemoryKind::DeviceLocal);
            if (!output) {
                return std::unexpected(
                    with_context(output.error(), "Graph::prepare spectrum scratch"));
            }
            frame.spectrum_output = std::move(*output);

            auto readback = gpu::Buffer::create(context, frame_bytes, kReadbackUsage,
                                                gpu::MemoryKind::Readback);
            if (!readback) {
                return std::unexpected(
                    with_context(readback.error(), "Graph::prepare spectrum readback"));
            }
            frame.spectrum_readback = std::move(*readback);

            constexpr VkDeviceSize kLevelsBytes =
                static_cast<VkDeviceSize>(dsp::kSpectrumLevelsOutputs) * sizeof(float);

            auto levels_output = gpu::Buffer::create(context, kLevelsBytes, kDeviceStorage,
                                                     gpu::MemoryKind::DeviceLocal);
            if (!levels_output) {
                return std::unexpected(
                    with_context(levels_output.error(), "Graph::prepare spectrum levels scratch"));
            }
            frame.spectrum_levels_output = std::move(*levels_output);

            auto levels_readback = gpu::Buffer::create(context, kLevelsBytes, kReadbackUsage,
                                                       gpu::MemoryKind::Readback);
            if (!levels_readback) {
                return std::unexpected(with_context(levels_readback.error(),
                                                    "Graph::prepare spectrum levels readback"));
            }
            frame.spectrum_levels_readback = std::move(*levels_readback);
        }

        VkDescriptorSetLayout layouts[5]{};
        std::uint32_t set_count = 0;
        if (impl.has_convert) {
            layouts[set_count++] = impl.convert_pipeline.descriptor_layout();
        }
        layouts[set_count++] = impl.branch_pipeline.descriptor_layout();
        layouts[set_count++] = impl.fft_pipeline.descriptor_layout();
        if (impl.geometry.spectrum.enabled()) {
            layouts[set_count++] = impl.spectrum_pipeline.descriptor_layout();
            layouts[set_count++] = impl.spectrum_levels_pipeline.descriptor_layout();
        }

        VkDescriptorSetAllocateInfo set_alloc{};
        set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        set_alloc.descriptorPool = impl.descriptors;
        set_alloc.descriptorSetCount = set_count;
        set_alloc.pSetLayouts = layouts;

        VkDescriptorSet sets[5]{};
        result = vkAllocateDescriptorSets(device, &set_alloc, sets);
        if (result != VK_SUCCESS) {
            return fail(std::format("vkAllocateDescriptorSets failed for frame {} ({})", i,
                                    gpu::result_name(result)),
                        result);
        }

        std::uint32_t next = 0;
        if (impl.has_convert) {
            frame.convert_set = sets[next++];
            const VkBuffer bound[] = {frame.staging.handle(), impl.ring->buffer()};
            if (auto wrote = write_storage_set(device, frame.convert_set, bound); !wrote) {
                return std::unexpected(with_context(wrote.error(), "Graph::prepare convert set"));
            }
        }
        frame.branch_set = sets[next++];
        {
            const VkBuffer bound[] = {impl.ring->buffer(), impl.prototype_buffer.handle(),
                                      frame.branch_output.handle()};
            if (auto wrote = write_storage_set(device, frame.branch_set, bound); !wrote) {
                return std::unexpected(with_context(wrote.error(), "Graph::prepare branch set"));
            }
        }
        frame.fft_set = sets[next++];
        {
            const VkBuffer bound[] = {frame.branch_output.handle(), impl.twiddle_buffer.handle(),
                                      impl.channel_ring.handle()};
            if (auto wrote = write_storage_set(device, frame.fft_set, bound); !wrote) {
                return std::unexpected(with_context(wrote.error(), "Graph::prepare fft set"));
            }
        }
        if (impl.geometry.spectrum.enabled()) {
            frame.spectrum_set = sets[next++];
            const VkBuffer bound[] = {impl.channel_ring.handle(), impl.spectrum_twiddles.handle(),
                                      impl.spectrum_window.handle(),
                                      frame.spectrum_output.handle()};
            if (auto wrote = write_storage_set(device, frame.spectrum_set, bound); !wrote) {
                return std::unexpected(with_context(wrote.error(), "Graph::prepare spectrum set"));
            }

            frame.spectrum_levels_set = sets[next];
            const VkBuffer levels_bound[] = {frame.spectrum_output.handle(),
                                             frame.spectrum_levels_output.handle()};
            if (auto wrote = write_storage_set(device, frame.spectrum_levels_set, levels_bound);
                !wrote) {
                return std::unexpected(
                    with_context(wrote.error(), "Graph::prepare spectrum levels set"));
            }
        }
    }

    // The first output block. See note 4 at the top of the file: below this
    // the filter's support runs off the front of the stream and the kernel's
    // masked read would filter the far end of the ring.
    const auto support = impl.prototype_length > 0 ? impl.prototype_length - 1 : 0;
    impl.next_block =
        (static_cast<dsp::SampleIndex>(support) + impl.grid.decimation - 1) / impl.grid.decimation;

    // Nothing below the first output block is the stream's, so that is where
    // the channel ring starts being a continuous record and where the
    // spectrum's window is allowed to begin.
    impl.blocks_contiguous_from = impl.next_block;
    impl.prototype_group_delay = prototype.group_delay_samples;
    if (impl.geometry.spectrum.enabled()) {
        impl.spectrum_scratch.assign(impl.geometry.spectrum.bins, 0.0F);

        SpectrumScaleConfig scale;
        scale.pinned_floor_db = impl.config.spectrum_floor_db;
        scale.pinned_ceiling_db = impl.config.spectrum_ceiling_db;
        auto made = SpectrumScale::create(scale);
        if (!made) {
            return std::unexpected(with_context(made.error(), "Graph::prepare spectrum scale"));
        }
        impl.spectrum_scale = std::move(*made);
    }

    // The graph owns the timeline semaphore, so the graph is what starts the
    // completion thread against it. Doing it here rather than making the
    // caller do it keeps the one thing that must happen exactly once next to
    // the object whose lifetime it belongs to.
    if (auto started = impl.scheduler->start(
            impl.timeline,
            [state = &impl](std::uint64_t, std::uint32_t ticket) { return state->complete(ticket); });
        !started) {
        return std::unexpected(with_context(started.error(), "Graph::prepare"));
    }

    impl.prepared = true;
    return {};
}

const GraphGeometry& Graph::geometry() const { return impl_->geometry; }

// ---------------------------------------------------------------------------
// Control plane
// ---------------------------------------------------------------------------

Expected<VrxId> Graph::add_vrx(VrxId id, const VrxParams& params, const VrxPlacement& placement,
                               VrxRole role) {
    auto& impl = *impl_;
    if (!impl.prepared) {
        return fail("Graph::add_vrx before prepare");
    }
    if (!id.valid()) {
        return fail("Graph::add_vrx with a zero id");
    }

    // Resolved once, here, so the stage and the plan cannot be built against
    // two different audio rates. See with_audio_rate.
    const VrxParams resolved = with_audio_rate(params, impl.config.audio_rate);

    VrxStageRequest request;
    request.context = impl.context;
    request.id = id;
    request.params = params;
    request.placement = placement;
    request.grid = impl.grid;
    request.source_rate = impl.config.source_rate;
    request.channel_rate = impl.geometry.channel_rate;
    request.channel_ring = impl.channel_ring.handle();
    request.channel_ring_bytes = impl.channel_ring.size();
    request.channel_ring_blocks = impl.geometry.channel_ring_blocks;
    request.channel_ring_mask = impl.geometry.channel_ring_mask;
    request.max_blocks_per_dispatch = impl.geometry.max_blocks_per_dispatch;
    request.frames_in_flight = impl.geometry.frames_in_flight;
    request.local_size_x = impl.geometry.local_size_x;
    request.audio_rate = resolved.audio_rate;
    request.passband_transform = impl.geometry.passband_transform;
    request.fine_stage_complex_tap = role == VrxRole::Probe;

    std::unique_ptr<VrxStage> stage;
    if (impl.factory) {
        auto built = impl.factory(request);
        if (!built) {
            return std::unexpected(with_context(built.error(), "Graph::add_vrx"));
        }
        stage = std::move(*built);
    }
    if (stage == nullptr && role == VrxRole::Probe) {
        // A probe exists to hand core/characterise an extract mixed to DC and
        // filtered near the signal. The raw tap is neither, and a probe that
        // quietly became one would put the classifier back the 20 dB down
        // docs/detection.md measured the raw tap at.
        return fail(std::format(
            "a probe receiver needs the fine stage and no stage factory built one for '{}'. "
            "engine::install_default_vrx_stages installs the factory that does",
            demod_name(params.demod)));
    }
    if (stage == nullptr) {
        // Demod::Raw alone falls back to the graph's own copy. The digital
        // voice modes used to share it, and handed a decoder one unmixed,
        // unfiltered coarse channel at the channel rate. They need the fine
        // stage, which arrives with the factory, so without one they are
        // refused like every other demodulator rather than given a stream
        // no decoder here was built for.
        if (params.demod != Demod::Raw) {
            return fail(std::format(
                "no stage factory is installed, so '{}' cannot be built. The graph ships one "
                "stage of its own, the raw complex tap, which needs no kernel. Every other "
                "mode, the digital voice taps included, needs the fine stage, which arrives "
                "with the demodulator package and installs itself through "
                "engine::install_vrx_stage_factory",
                demod_name(params.demod)));
        }
        stage = std::make_unique<RawTapStage>(request);
    }

    auto slot = std::make_shared<Impl::VrxSlot>();
    slot->id = id;
    slot->role = role;
    slot->params = params;
    slot->placement = placement;
    slot->recording_params = params;
    slot->resolved_audio_rate = resolved.audio_rate;
    if (auto shape = dsp::vrx_shape_for(impl.grid, impl.config.source_rate, resolved,
                                        placement)) {
        slot->shape = *shape;
        slot->shape_known = true;
    }
    slot->stage = std::move(stage);
    slot->audio_bytes = slot->stage->audio_bytes_for(impl.geometry.max_blocks_per_dispatch);
    if (slot->audio_bytes == 0) {
        return fail("a stage that returns zero audio bytes would never deliver anything");
    }
    slot->store_level(kSilenceFloorDbfs);

    // One readback buffer per frame in flight. Per frame and not shared,
    // because a shared buffer would have to be resized when a receiver is
    // added and a frame already in flight is writing into it.
    slot->readback.reserve(impl.geometry.frames_in_flight);
    for (std::uint32_t i = 0; i < impl.geometry.frames_in_flight; ++i) {
        auto buffer = gpu::Buffer::create(*impl.context, slot->audio_bytes, kReadbackUsage,
                                          gpu::MemoryKind::Readback);
        if (!buffer) {
            return std::unexpected(with_context(buffer.error(), "Graph::add_vrx readback"));
        }
        slot->readback.push_back(std::move(*buffer));
    }
    slot->scratch.assign(slot->audio_bytes / sizeof(float), 0.0F);

    // The receiver's passband colour map, built here rather than when a sink
    // is attached, so that it survives one being swapped for another. No
    // pins: nothing in VrxParams sets them, and PassbandFrame carries the
    // raw percentiles for a consumer that wants its own.
    //
    // Built even when the graph has no passband stage. It is two floats and
    // a little history, against the alternative of a second place where the
    // stage's absence has to be remembered.
    if (auto made = SpectrumScale::create(SpectrumScaleConfig{})) {
        slot->passband_scale = std::move(*made);
    } else {
        return std::unexpected(with_context(made.error(), "Graph::add_vrx passband scale"));
    }

    {
        std::scoped_lock lock(impl.control_lock);
        if (impl.find_known(id) != nullptr) {
            return fail(std::format("receiver {} is already registered", id.value));
        }
        impl.known.push_back(slot);
    }

    auto* op = new (std::nothrow) Impl::ControlOp();
    if (op == nullptr) {
        return fail("Graph::add_vrx could not allocate the control operation");
    }
    op->kind = Impl::ControlOp::Kind::Add;
    op->id = id;
    op->slot = std::move(slot);
    impl.push_control(op);
    return id;
}

Status Graph::remove_vrx(VrxId id) {
    auto& impl = *impl_;
    {
        std::scoped_lock lock(impl.control_lock);
        if (impl.find_known(id) == nullptr) {
            return fail(std::format("no receiver {} is registered", id.value));
        }
        std::erase_if(impl.known, [&](const std::shared_ptr<Impl::VrxSlot>& slot) {
            return slot->id == id;
        });
    }

    auto* op = new (std::nothrow) Impl::ControlOp();
    if (op == nullptr) {
        return fail("Graph::remove_vrx could not allocate the control operation");
    }
    op->kind = Impl::ControlOp::Kind::Remove;
    op->id = id;
    impl.push_control(op);
    return {};
}

Status Graph::set_vrx_params(VrxId id, const VrxParams& params, const VrxPlacement& placement) {
    auto& impl = *impl_;

    // ALLOCATED BEFORE THE LOCK IS TAKEN, which is the opposite order from
    // add_vrx and is required rather than tidier. The locked section below
    // moves the receiver's queued_epoch, and that number is what a consumer
    // fences against: an allocation failing after it moved would leave a
    // receiver whose status promises a retune that will never be applied,
    // and every consumer comparing against it discards for the rest of the
    // run. Allocating first means the only two outcomes are a refusal that
    // moved nothing and an op that carries the number it moved to.
    auto op = std::unique_ptr<Impl::ControlOp>(new (std::nothrow) Impl::ControlOp());
    if (op == nullptr) {
        return fail("Graph::set_vrx_params could not allocate the control operation");
    }

    {
        std::scoped_lock lock(impl.control_lock);
        auto slot = impl.find_known(id);
        if (slot == nullptr) {
            return fail(std::format("no receiver {} is registered", id.value));
        }
        if (slot->params.demod != params.demod) {
            return fail(std::format(
                "receiver {} is a '{}' and cannot become a '{}' in place: the demodulator is the "
                "stage, so changing it is a remove and an add",
                id.value, demod_name(slot->params.demod), demod_name(params.demod)));
        }
        // Refused before anything is stored, because a change to the
        // pipeline's shape is a different pipeline, a different ring and a
        // different tap table, and rebuilding those under command buffers
        // already in flight is the one thing the graph promises never to
        // do. DemodStage::retune refuses it too, in these same words, and
        // that refusal reaches nobody: it is discarded on the recording
        // thread by the control op this method queues. So the question has
        // to be asked here, where there is still a caller to answer.
        //
        // Asked OF THE PLANNER rather than of a rule written out again
        // here, and asked as ONE PREDICATE the stage also asks. Two places
        // deciding what counts as a shape change is how one of them comes
        // to differ from the other, and the difference is a retune this
        // method accepts and the stage silently drops, which is the failure
        // this check exists to end. dsp::VrxShape carries the whole
        // question; see its header.
        //
        // Through vrx_shape_for and not plan_vrx, so the control plane pays
        // for two Kaiser order estimates rather than for three designed
        // filter tables. A passband drag asks this once per gesture and the
        // keyboard once per keystroke.
        //
        // The audio rate is resolved first, for the reason with_audio_rate
        // gives: a graph whose default is not 48000 would otherwise compare
        // a request against a shape derived from a rate the stage never ran
        // at.
        //
        // Moving the dial is not this. A retune that keeps the shape is a
        // push constant and a new tap table, which is free, and that is
        // exactly what panning a filter of a fixed width is. Changing its
        // WIDTH moves the tap count, because Kaiser sets the length from
        // the transition and the transition is half a width, so a widen is
        // a remove and an add however small it is.
        const VrxParams resolved = with_audio_rate(params, impl.config.audio_rate);
        auto planned =
            dsp::vrx_shape_for(impl.grid, impl.config.source_rate, resolved, placement);
        if (!planned) {
            return std::unexpected(with_context(planned.error(), "Graph::set_vrx_params"));
        }
        if (slot->shape_known && *planned != slot->shape) {
            return fail(dsp::describe_shape_change(slot->shape, *planned));
        }

        slot->params = params;
        slot->placement = placement;
        slot->shape = *planned;
        slot->shape_known = true;
        slot->resolved_audio_rate = resolved.audio_rate;

        // MOVED HERE, UNDER THE LOCK AND AFTER THE LAST REFUSAL, so that
        // vrx_status called the instant this returns already reports the
        // number the chunks are heading for. A refused retune queues no op
        // and must not move it, or a consumer fences against an epoch no
        // chunk will ever carry and discards for ever.
        op->tuning_epoch = ++slot->queued_epoch;
    }

    op->kind = Impl::ControlOp::Kind::Retune;
    op->id = id;
    op->params = params;
    op->placement = placement;
    impl.push_control(op.release());
    return {};
}

Status Graph::set_audio_sink(VrxId id, AudioSink sink) {
    auto& impl = *impl_;
    {
        std::scoped_lock lock(impl.control_lock);
        if (impl.find_known(id) == nullptr) {
            return fail(std::format("no receiver {} is registered", id.value));
        }
    }

    auto* op = new (std::nothrow) Impl::ControlOp();
    if (op == nullptr) {
        return fail("Graph::set_audio_sink could not allocate the control operation");
    }
    op->kind = Impl::ControlOp::Kind::Sink;
    op->id = id;
    op->sink = std::make_shared<AudioSink>(std::move(sink));
    impl.push_control(op);
    return {};
}

Status Graph::set_spectrum_sink(SpectrumSink sink) {
    auto& impl = *impl_;
    if (!impl.geometry.spectrum.enabled()) {
        return fail("this graph was built with no spectrum stage. The stage is part of the "
                    "coarse chain and is built once when the source is opened, so it is chosen "
                    "through GraphConfig::spectrum_transform rather than by attaching a sink");
    }

    auto* op = new (std::nothrow) Impl::ControlOp();
    if (op == nullptr) {
        return fail("Graph::set_spectrum_sink could not allocate the control operation");
    }
    op->kind = Impl::ControlOp::Kind::Spectrum;
    op->spectrum_sink = std::make_shared<SpectrumSink>(std::move(sink));
    impl.push_control(op);
    return {};
}

Status Graph::set_passband_sink(VrxId id, PassbandSink sink) {
    auto& impl = *impl_;
    if (impl.geometry.passband_transform == 0) {
        return fail("this graph was built with no passband stage. It costs a pipeline, two "
                    "window tables and a window's worth of room in every receiver's fine ring, "
                    "and a ring cannot be grown while a command buffer names it, so it is "
                    "chosen through GraphConfig::passband_transform before the source is "
                    "opened rather than by attaching a sink");
    }

    std::shared_ptr<Impl::VrxSlot> slot;
    {
        std::scoped_lock lock(impl.control_lock);
        slot = impl.find_known(id);
        if (slot == nullptr) {
            return fail(std::format("no receiver {} is registered", id.value));
        }
    }

    auto* op = new (std::nothrow) Impl::ControlOp();
    if (op == nullptr) {
        return fail("Graph::set_passband_sink could not allocate the control operation");
    }
    op->kind = Impl::ControlOp::Kind::Passband;
    op->id = id;

    // An empty sink is a detach, and a detach allocates nothing. The
    // recording thread drops both the view and the sink together, and the
    // frames still in flight keep the buffers alive through their own
    // references until they retire.
    if (!sink) {
        impl.push_control(op);
        return {};
    }

    // Read once, here, on the control plane. Every field of
    // StageDisplayOutput is written at construction and never again, which
    // is the reason this is safe while the recording thread is inside
    // record() on the same stage. The two things a retune does move, the
    // frequency the stream's DC sits at and the rate it runs at, come back
    // on StageOutput instead and are read on the recording thread.
    const StageDisplayOutput display = slot->stage->display_output();
    if (display.ring == VK_NULL_HANDLE) {
        delete op;
        return fail(std::format(
            "receiver {} keeps no fine stream and so no display stream, so there is nothing "
            "to transform. The graph's raw tap is a copy out of the coarse channel ring with "
            "no mixing and no filtering, and the full-span spectrum already covers that "
            "channel",
            id.value));
    }

    auto built = impl.build_passband_view(display);
    if (!built) {
        delete op;
        return std::unexpected(with_context(
            built.error(), std::format("receiver {} passband", id.value)));
    }

    op->passband = std::move(*built);
    op->passband_sink = std::make_shared<PassbandSink>(std::move(sink));
    impl.push_control(op);
    return {};
}

Expected<VrxStatus> Graph::vrx_status(VrxId id) const {
    auto& impl = *impl_;
    std::scoped_lock lock(impl.control_lock);
    auto slot = impl.find_known(id);
    if (slot == nullptr) {
        return fail(std::format("no receiver {} is registered", id.value));
    }

    VrxStatus status;
    status.id = slot->id;
    status.params = slot->params;
    status.placement = slot->placement;
    status.demod_rate = slot->shape.demod_rate;
    status.resolved_audio_rate = slot->resolved_audio_rate;
    status.tuning_epoch = slot->queued_epoch;
    status.level_dbfs = slot->load_level();
    status.squelch_open = slot->squelch_open.load(std::memory_order_relaxed);
    status.audio_samples = slot->audio_samples.load(std::memory_order_relaxed);
    status.audio_dropped = slot->audio_dropped.load(std::memory_order_relaxed);

    // EVENTS FIRST AND ACQUIRING, FRAMES SECOND. control_lock keeps other
    // control-plane callers out and locks nothing out of the sample path, so
    // these two loads can straddle a skip. The acquire pairs with the release
    // on the recording thread, which adds the frames before the event: an
    // event this load can see has its frames already visible, so the frame
    // count is never short of the events beside it. The other direction,
    // frames whose event has not landed yet, understates by one event for one
    // poll and corrects itself on the next.
    status.reanchors = slot->reanchors.load(std::memory_order_acquire);
    status.reanchor_frames_skipped =
        slot->reanchor_frames_skipped.load(std::memory_order_relaxed);
    return status;
}

std::vector<VrxId> Graph::vrx_ids() const {
    auto& impl = *impl_;
    std::scoped_lock lock(impl.control_lock);
    std::vector<VrxId> out;
    out.reserve(impl.known.size());
    for (const auto& slot : impl.known) {
        if (slot->role == VrxRole::Probe) {
            continue;
        }
        out.push_back(slot->id);
    }
    return out;
}

std::size_t Graph::probe_count() const {
    auto& impl = *impl_;
    std::scoped_lock lock(impl.control_lock);
    return static_cast<std::size_t>(
        std::count_if(impl.known.begin(), impl.known.end(),
                      [](const std::shared_ptr<Impl::VrxSlot>& slot) {
                          return slot->role == VrxRole::Probe;
                      }));
}

// ---------------------------------------------------------------------------
// The sample path
// ---------------------------------------------------------------------------

Status Graph::on_block(const source::SourceBlock& block) {
    auto& impl = *impl_;
    if (!impl.prepared) {
        return fail("Graph::on_block before prepare");
    }
    if (impl.cancelled.load(std::memory_order_acquire)) {
        return fail("the engine was stopped");
    }
    if (block.sample_count == 0) {
        return {};
    }
    if (block.format != impl.config.format) {
        return fail(std::format(
            "the graph was built for '{}' and the source delivered '{}'. The format is fixed at "
            "open, because it selects the convert pipeline",
            source::format_name(impl.config.format), source::format_name(block.format)));
    }
    if (block.sample_count > impl.geometry.block_samples) {
        return fail(std::format("a block of {} samples is larger than the {} the staging buffer "
                                "holds",
                                block.sample_count, impl.geometry.block_samples));
    }
    const auto expected_bytes =
        block.sample_count * source::bytes_per_sample(block.format);
    if (block.bytes.size() < expected_bytes) {
        return fail(std::format("a block claiming {} '{}' samples carried {} bytes, which is {} "
                                "short",
                                block.sample_count, source::format_name(block.format),
                                block.bytes.size(), expected_bytes - block.bytes.size()));
    }

    // Control changes land between blocks, never inside one. A receiver added
    // mid-dispatch would be recorded against a command buffer that had already
    // been submitted.
    impl.drain_control();

    const dsp::SampleIndex target_end =
        block.stamp.start + static_cast<dsp::SampleIndex>(block.sample_count);
    const dsp::SampleIndex reserved = impl.ring->reserved_index();

    // The test is on the block's FIRST sample, not its last. A block that
    // starts below the reservation and ends above it overlaps samples the
    // ring has already placed, and it is backwards in exactly the way this
    // refuses: the ring's index is the stream's index, so the same index
    // cannot mean two different samples.
    //
    // WHAT THIS USED TO SAY. Until 2026-09-20 the test was `target_end <=
    // reserved`, which let an overlapping block straight through, and the
    // two subtractions below then ran on a start that was less than
    // `reserved`. `gap` is unsigned, so it wrapped to about 1.8e19 and was
    // added to samples_dropped as reported loss, while `need` stayed small
    // enough that the capacity check waved it past. The counter went from a
    // number an operator reads to a number nothing can mean.
    if (block.stamp.start < reserved) {
        return fail(std::format(
            "the source delivered samples [{}, {}) and the ring has already reserved through {}. "
            "A source that moves backwards has to be reopened, because the ring's index is the "
            "stream's index and there is no second meaning for it",
            block.stamp.start, target_end, reserved));
    }

    // The gap when a Paced source dropped samples. It is reserved as well as
    // the block, so the ring's index never drifts from the stream's index. The
    // gap region keeps whatever was there, and the loss is counted: see the
    // integrator note about filling it, which is a vkCmdFillBuffer nobody has
    // needed yet.
    const std::uint64_t gap = block.stamp.start - reserved;
    const std::uint64_t need = target_end - reserved;

    // The first delivery is not a gap even when it starts above zero. A Demand
    // source opened with StreamOptions::start_index begins the stream there,
    // and the ring's index is the stream's index, so the slots below it are
    // simply never written rather than lost.
    const bool first_delivery = !impl.first_delivery_seen;
    impl.first_delivery_seen = true;

    const auto ring_capacity = impl.ring->geometry().capacity_samples;
    if (need > ring_capacity) {
        return fail(std::format(
            "this block needs {} samples of ring and the ring holds {}. {} of that is the {} "
            "between the last sample placed and this block's first, so the engine either fell "
            "more than a whole ring behind or was started at a sample index further than one "
            "ring from the origin",
            need, ring_capacity, gap,
            first_delivery ? "stream's start index" : "gap the source reported"));
    }

    std::uint64_t granted = 0;
    if (impl.config.flow == source::FlowControl::Demand) {
        // The backpressure. This is what makes an offline replay run at
        // exactly the rate the GPU retires work, and it is the whole of
        // "faster than realtime": there is no throttle anywhere, only this
        // parking until a dispatch has finished with the samples it read.
        if (auto taken = impl.ring->reserve_blocking(need); !taken) {
            // cancel() stops the ring to wake exactly this wait, so a refusal
            // after it is the stop that was asked for rather than a fault in
            // the ring. Reported in the words run()'s callers already treat as
            // a clean finish, the way the frame-slot wait below reports its own.
            if (impl.cancelled.load(std::memory_order_acquire)) {
                return fail("the engine was stopped while the producer waited for ring room");
            }
            return std::unexpected(with_context(taken.error(), "Graph::on_block"));
        }
        granted = need;
    } else {
        auto taken = impl.ring->reserve(need);
        if (!taken) {
            return std::unexpected(with_context(taken.error(), "Graph::on_block"));
        }
        granted = *taken;
        if (granted < need) {
            const std::uint64_t lost = need - granted;
            impl.ring->note_dropped(lost);
            impl.overrun_events.fetch_add(1, std::memory_order_relaxed);
            impl.samples_dropped.fetch_add(lost, std::memory_order_relaxed);
        }
        if (granted == 0) {
            return {};
        }
    }

    if (gap > 0) {
        impl.trusted_from = std::max(impl.trusted_from, block.stamp.start);
        if (!first_delivery) {
            impl.overrun_events.fetch_add(1, std::memory_order_relaxed);
            impl.samples_dropped.fetch_add(gap, std::memory_order_relaxed);
        }
    }

    // A frame slot. On a Demand source waiting here is ordinary backpressure.
    // On a Paced one it is forbidden, because the device's clock does not
    // wait, so an unavailable slot loses the block and says so.
    const std::uint64_t ticket = impl.submitted.load(std::memory_order_relaxed);
    const auto in_flight = static_cast<std::uint64_t>(impl.geometry.frames_in_flight);
    bool counted_stall = false;
    for (;;) {
        // The epoch snapshot comes first, before anything this loop tests,
        // for the reason written on Impl::frame_epoch and on the completion
        // thread's own park in scheduler.cpp: a waker that bumps the counter
        // after the state it published cannot be missed by a reader that
        // snapshotted the counter before reading that state.
        const std::uint64_t epoch = impl.frame_epoch.load(std::memory_order_acquire);
        const std::uint64_t done = impl.completed.load(std::memory_order_acquire);
        if (ticket - done < in_flight) {
            break;
        }
        if (!counted_stall) {
            impl.frame_stalls.fetch_add(1, std::memory_order_relaxed);
            counted_stall = true;
        }
        if (impl.config.flow == source::FlowControl::Paced) {
            // Publish anyway so the ring's index tracks the stream's. The
            // samples are not in the ring and are counted as lost, which is
            // what an overrun is.
            if (auto published = impl.publish_all(granted); !published) {
                return std::unexpected(with_context(published.error(), "Graph::on_block"));
            }
            impl.ring->note_dropped(block.sample_count);
            impl.overrun_events.fetch_add(1, std::memory_order_relaxed);
            impl.samples_dropped.fetch_add(block.sample_count, std::memory_order_relaxed);
            impl.trusted_from = std::max(impl.trusted_from, target_end);
            return {};
        }
        if (impl.cancelled.load(std::memory_order_acquire)) {
            impl.ring->release_reservation(granted);
            return fail("the engine was stopped while waiting for a frame slot");
        }
        impl.frame_epoch.wait(epoch, std::memory_order_acquire);
    }

    const auto frame_index = static_cast<std::uint32_t>(ticket % in_flight);
    auto& frame = impl.frames[frame_index];

    // The one host pass over the samples, into pinned memory the convert
    // kernel reads directly. Partitioned across the pool because at 20 MS/s
    // this is 160 MB/s of memcpy and one core is not the right amount of
    // machine to spend on it.
    const auto copy_bytes = static_cast<std::size_t>(expected_bytes);
    {
        const auto* source_bytes = block.bytes.data();
        auto& pool = impl.scheduler->pool();
        constexpr std::size_t kMemcpyChunk = 64 * 1024;
        const std::size_t chunks = (copy_bytes + kMemcpyChunk - 1) / kMemcpyChunk;
        Status copy_status;
        if (chunks <= 1 || pool.worker_count() <= 1) {
            copy_status = frame.staging.write(std::span(source_bytes, copy_bytes));
        } else {
            std::atomic<bool> failed{false};
            pool.parallel_for(chunks, 1, [&](std::size_t begin, std::size_t end) {
                const std::size_t offset = begin * kMemcpyChunk;
                const std::size_t limit = std::min(end * kMemcpyChunk, copy_bytes);
                if (limit <= offset) {
                    return;
                }
                if (!frame.staging.write(std::span(source_bytes + offset, limit - offset),
                                         static_cast<VkDeviceSize>(offset))) {
                    failed.store(true, std::memory_order_relaxed);
                }
            });
            if (failed.load(std::memory_order_relaxed)) {
                copy_status = fail("a staging write failed while partitioning the upload");
            }
        }
        if (!copy_status) {
            impl.ring->release_reservation(granted);
            return std::unexpected(with_context(copy_status.error(), "Graph::on_block staging"));
        }
    }

    // How many output blocks this dispatch can produce. Block m needs input up
    // to m*D and down to m*D - (N - 1); both bounds are checked rather than
    // assumed, because the lower one is exactly what a lapped Lossy consumer
    // violates.
    const auto decimation = static_cast<dsp::SampleIndex>(impl.grid.decimation);
    const auto support = static_cast<dsp::SampleIndex>(
        impl.prototype_length > 0 ? impl.prototype_length - 1 : 0);
    const dsp::SampleIndex published_end = reserved + granted;

    // Two floors, and they are counted differently. Skipping past a hole or
    // past the start of the stream loses nothing that was not already
    // reported where it happened; skipping because the writer lapped this
    // consumer is a loss discovered here and nowhere else.
    dsp::SampleIndex first_block = impl.next_block;
    while (first_block * decimation < support ||
           first_block * decimation - support < impl.trusted_from) {
        ++first_block;
    }
    const dsp::SampleIndex after_trusted = first_block;

    const dsp::SampleIndex oldest = impl.ring->first_available();
    while (first_block * decimation < support ||
           first_block * decimation - support < oldest) {
        ++first_block;
    }
    if (first_block > after_trusted) {
        const auto skipped = (first_block - after_trusted) * decimation;
        impl.overrun_events.fetch_add(1, std::memory_order_relaxed);
        impl.samples_dropped.fetch_add(skipped, std::memory_order_relaxed);
    }

    std::uint32_t block_count = 0;
    if (published_end > first_block * decimation) {
        const dsp::SampleIndex last = (published_end - 1) / decimation;
        if (last >= first_block) {
            const dsp::SampleIndex available = last - first_block + 1;
            block_count = static_cast<std::uint32_t>(
                std::min<dsp::SampleIndex>(available, impl.geometry.max_blocks_per_dispatch));
        }
    }

    // Anything the two loops above skipped leaves stale slots in the channel
    // ring. A receiver does not care, because it reads forward from where it
    // is; the spectrum does, because its window reaches back over several
    // dispatches and one straddling a skip would transform a splice of two
    // eras, which paints a band of energy that was never on the air.
    if (first_block != impl.next_block) {
        impl.blocks_contiguous_from = first_block;
    }

    // Whether this dispatch has a continuous window behind it to transform.
    // False for the first N blocks of a stream and for the first N after any
    // skip, which is a counted gap in the waterfall rather than a row of
    // invented signal.
    //
    // Not computed at all when nobody is listening. The stage is built with
    // the coarse chain because it cannot be added to a running graph, but a
    // dispatch and a readback per block for a frame that goes nowhere is
    // still work, and a caller that has not attached a sink has said it does
    // not want it yet.
    bool record_spectrum = false;
    dsp::SampleIndex spectrum_first_block = 0;
    const bool spectrum_wanted = impl.geometry.spectrum.enabled() &&
                                 impl.spectrum_sink != nullptr && *impl.spectrum_sink;
    if (block_count > 0 && spectrum_wanted) {
        const auto points = static_cast<dsp::SampleIndex>(impl.geometry.spectrum.transform);
        const dsp::SampleIndex one_past = first_block + block_count;
        if (one_past >= points && one_past - points >= impl.blocks_contiguous_from) {
            record_spectrum = true;
            spectrum_first_block = one_past - points;
        } else {
            impl.spectrum_skipped.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // --- record ------------------------------------------------------------

    VkResult result = vkResetCommandBuffer(frame.commands, 0);
    if (result != VK_SUCCESS) {
        impl.ring->release_reservation(granted);
        return fail(std::format("vkResetCommandBuffer failed ({})", gpu::result_name(result)),
                    result);
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    result = vkBeginCommandBuffer(frame.commands, &begin);
    if (result != VK_SUCCESS) {
        impl.ring->release_reservation(granted);
        return fail(std::format("vkBeginCommandBuffer failed ({})", gpu::result_name(result)),
                    result);
    }

    const std::uint64_t capacity_mask = impl.ring->geometry().capacity_mask;
    const auto destination = static_cast<std::uint32_t>(block.stamp.start & capacity_mask);

    if (impl.has_convert) {
        dsp::ConvertParams params;
        params.ring_mask = static_cast<std::uint32_t>(capacity_mask);
        params.dst_offset = destination;
        params.src_offset = 0;
        params.count = static_cast<std::uint32_t>(block.sample_count);

        record_dispatch(frame.commands, impl.convert_pipeline, frame.convert_set,
                        std::as_bytes(std::span<const dsp::ConvertParams>(&params, 1)),
                        group_count(block.sample_count, impl.geometry.local_size_x));
        impl.dispatches.fetch_add(1, std::memory_order_relaxed);
    } else {
        // cf32 needs no widening, so the bus crossing is the copy itself. The
        // destination is a ring and vkCmdCopyBuffer wants a contiguous range,
        // so a block straddling the wrap becomes two regions. This split lives
        // here and nowhere else: a consumer kernel masks its own index and
        // never sees a wrap at all.
        const auto capacity = impl.ring->geometry().capacity_samples;
        const std::uint64_t to_end = capacity - destination;
        const std::uint64_t first = std::min<std::uint64_t>(block.sample_count, to_end);

        VkBufferCopy regions[2]{};
        std::uint32_t region_count = 1;
        regions[0].srcOffset = 0;
        regions[0].dstOffset = static_cast<VkDeviceSize>(destination) * kComplexBytes;
        regions[0].size = static_cast<VkDeviceSize>(first) * kComplexBytes;
        if (first < block.sample_count) {
            regions[1].srcOffset = static_cast<VkDeviceSize>(first) * kComplexBytes;
            regions[1].dstOffset = 0;
            regions[1].size =
                static_cast<VkDeviceSize>(block.sample_count - first) * kComplexBytes;
            region_count = 2;
        }
        vkCmdCopyBuffer(frame.commands, frame.staging.handle(), impl.ring->buffer(), region_count,
                        regions);
    }

    if (block_count > 0) {
        record_barrier(frame.commands,
                       VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

        dsp::PfbBranchParams branch_params;
        branch_params.ring_mask = static_cast<std::uint32_t>(capacity_mask);
        branch_params.base_offset =
            static_cast<std::uint32_t>((first_block * decimation) & capacity_mask);
        branch_params.block_count = block_count;

        record_dispatch(
            frame.commands, impl.branch_pipeline, frame.branch_set,
            std::as_bytes(std::span<const dsp::PfbBranchParams>(&branch_params, 1)),
            group_count(static_cast<std::uint64_t>(block_count) * impl.grid.channels,
                        impl.geometry.local_size_x));

        record_barrier(frame.commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_READ_BIT);

        FftPushConstants fft_params;
        fft_params.block_base = static_cast<std::uint32_t>(first_block);
        fft_params.block_count = block_count;
        fft_params.out_ring_blocks = impl.geometry.channel_ring_blocks;
        fft_params.out_ring_mask = impl.geometry.channel_ring_mask;

        // One workgroup per transform. The group count is the block count and
        // has nothing to do with the thread count inside one.
        record_dispatch(frame.commands, impl.fft_pipeline, frame.fft_set,
                        std::as_bytes(std::span<const FftPushConstants>(&fft_params, 1)),
                        block_count);

        impl.dispatches.fetch_add(2, std::memory_order_relaxed);
        impl.channel_blocks.fetch_add(block_count, std::memory_order_relaxed);
    }

    // --- the spectrum and the receivers -------------------------------------

    frame.vrxs.clear();
    frame.spectrum_recorded = false;
    frame.spectrum_sink = impl.spectrum_sink;

    // One barrier for every reader of the channel ring, the spectrum and the
    // receivers alike, because they all wait on the same transform.
    if (block_count > 0 && (record_spectrum || !impl.active.empty())) {
        record_barrier(frame.commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    }

    if (record_spectrum) {
        SpectrumPushConstants spectrum_params;
        spectrum_params.chan_blocks = impl.geometry.channel_ring_blocks;
        spectrum_params.chan_mask = impl.geometry.channel_ring_mask;
        spectrum_params.in_offset = static_cast<std::uint32_t>(
            spectrum_first_block & impl.geometry.channel_ring_mask);

        // One workgroup per coarse channel. The group count is the channel
        // count and has nothing to do with the thread count inside one, the
        // same as the transform above.
        record_dispatch(
            frame.commands, impl.spectrum_pipeline, frame.spectrum_set,
            std::as_bytes(std::span<const SpectrumPushConstants>(&spectrum_params, 1)),
            impl.geometry.spectrum.channels);

        // Two readers of the frame the kernel just wrote: the copy that
        // carries it home and the percentile that measures it. The
        // measurement runs here, on the device, and not on the host side of
        // that copy: docs/ui-spectrum.md is explicit that reading a frame
        // back to measure it puts the one thing this architecture exists to
        // avoid back into the path, and a consumer that is not on this
        // machine could not do it at all.
        record_barrier(frame.commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);

        VkBufferCopy region{};
        region.srcOffset = 0;
        region.dstOffset = 0;
        region.size = static_cast<VkDeviceSize>(impl.geometry.spectrum.bins) * sizeof(float);
        vkCmdCopyBuffer(frame.commands, frame.spectrum_output.handle(),
                        frame.spectrum_readback.handle(), 1, &region);

        SpectrumLevelsPushConstants levels_params;
        levels_params.bins = impl.geometry.spectrum.bins;
        levels_params.low_permille = dsp::kSpectrumLowPermille;
        levels_params.high_permille = dsp::kSpectrumHighPermille;

        // One workgroup, which is the whole of this kernel's shape: it holds
        // a histogram of the frame in shared memory and a second workgroup
        // would count the same bins into its own copy.
        record_dispatch(
            frame.commands, impl.spectrum_levels_pipeline, frame.spectrum_levels_set,
            std::as_bytes(std::span<const SpectrumLevelsPushConstants>(&levels_params, 1)), 1);

        record_barrier(frame.commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT);

        VkBufferCopy levels_region{};
        levels_region.srcOffset = 0;
        levels_region.dstOffset = 0;
        levels_region.size =
            static_cast<VkDeviceSize>(dsp::kSpectrumLevelsOutputs) * sizeof(float);
        vkCmdCopyBuffer(frame.commands, frame.spectrum_levels_output.handle(),
                        frame.spectrum_levels_readback.handle(), 1, &levels_region);

        // What the window covers in the source's own index, which is the one
        // index everything downstream shares. This is
        // dsp::channel_sample_to_input written out, because the graph keeps
        // the prototype's group delay rather than the whole filter: channel
        // block m is taken at input m*D and the linear-phase prototype delays
        // the signal by exactly its group delay, which the extra zero tap in
        // taps_per_branch makes a whole number of input samples.
        frame.spectrum_start =
            spectrum_first_block * decimation - impl.prototype_group_delay;
        frame.spectrum_count =
            static_cast<dsp::SampleIndex>(impl.geometry.spectrum.transform) * decimation;
        frame.spectrum_recorded = true;
        impl.dispatches.fetch_add(2, std::memory_order_relaxed);

        // The frame and its two percentiles, which are two crossings of the
        // bus and were counted as none until 2026-09-20.
        impl.readbacks.fetch_add(2, std::memory_order_relaxed);
    }

    if (block_count > 0 && !impl.active.empty()) {
        frame.vrxs.reserve(impl.active.size());
        for (const auto& slot : impl.active) {
            StageRecord record;
            record.commands = frame.commands;
            record.frame_index = frame_index;
            record.channel_ring = impl.channel_ring.handle();
            record.channel_ring_bytes = impl.channel_ring.size();
            record.first_block = first_block;
            record.block_count = block_count;
            record.audio_destination = slot->readback[frame_index].handle();
            record.audio_bytes = slot->audio_bytes;
            record.display = slot->passband != nullptr;

            auto recorded = slot->stage->record(record);
            if (!recorded) {
                (void)vkEndCommandBuffer(frame.commands);
                impl.ring->release_reservation(granted);
                return std::unexpected(with_context(
                    recorded.error(), std::format("receiver {} stage", slot->id.value)));
            }

            // Added here and not carried on the frame, because a re-anchor is
            // not something a delivery reports: the dispatch that skipped
            // recorded no audio, so the completion thread never looks at it.
            // A receiver whose inputs were overwritten has lost frames
            // whether or not anything was listening.
            //
            // Frames first and relaxed, then the event and RELEASING, which is
            // the one place a counter in this file is not relaxed. Two relaxed
            // increments of two locations can be observed in either order, so
            // a reader could see the event and the old frame count and report
            // a skip that lost nothing. The release here pairs with the
            // acquire in Graph::vrx_status: a reader that sees this event sees
            // the frames that went with it.
            if (recorded->reanchors != 0) {
                slot->reanchor_frames_skipped.fetch_add(recorded->reanchor_frames_skipped,
                                                        std::memory_order_relaxed);
                slot->reanchors.fetch_add(recorded->reanchors, std::memory_order_release);
            }

            Impl::FrameVrx entry;
            entry.slot = slot;
            entry.sink = slot->sink;
            entry.readback = slot->readback[frame_index].handle();
            entry.readback_index = frame_index;
            entry.output = *recorded;
            entry.squelch_dbfs = slot->recording_params.squelch_dbfs;
            entry.tuning_epoch = slot->recording_epoch;

            // Snapshotted the same way the audio sink is, so detaching never
            // races a delivery already under way. The transform itself is
            // recorded after this loop, not inside it: see record_passbands.
            entry.passband = slot->passband;
            entry.passband_sink = slot->passband_sink;
            frame.vrxs.push_back(std::move(entry));
        }

        // What the stages recorded, asked of the stages. A receiver with no
        // whole audio sample this dispatch records neither its demodulator
        // nor its copy, so counting receivers here counted intentions.
        std::uint64_t stage_dispatches = 0;
        std::uint64_t stage_readbacks = 0;
        for (const Impl::FrameVrx& entry : frame.vrxs) {
            stage_dispatches += entry.output.dispatches;
            stage_readbacks += entry.output.readbacks;
        }
        impl.dispatches.fetch_add(stage_dispatches, std::memory_order_relaxed);
        impl.readbacks.fetch_add(stage_readbacks, std::memory_order_relaxed);

        // After every fine stage, so that all of them sit in one dependency
        // region and their transforms can overlap.
        impl.record_passbands(frame);
    }

    // Everything above this line stayed on the device. What crosses back is
    // audio PCM and a frame of decibels, which is the whole of what
    // core/engine/engine.h permits.
    if (frame.spectrum_recorded || !frame.vrxs.empty()) {
        record_barrier(frame.commands,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    }

    result = vkEndCommandBuffer(frame.commands);
    if (result != VK_SUCCESS) {
        impl.ring->release_reservation(granted);
        return fail(std::format("vkEndCommandBuffer failed ({})", gpu::result_name(result)),
                    result);
    }

    // The samples are in the command buffer; make them visible to any host
    // thread that reads the write cursor. The device-side hazard is the
    // barriers above and the timeline below, and the two must not be
    // conflated: a release store says nothing about a copy in flight.
    if (auto published = impl.publish_all(granted); !published) {
        return std::unexpected(with_context(published.error(), "Graph::on_block"));
    }

    // After the publish, not before: the ring refuses a claim past what has
    // been published, and this window's end was derived from the published
    // end a few lines up.
    if (block_count > 0) {
        if (auto claimed = impl.ring->claim(impl.consumer,
                                            (first_block + block_count - 1) * decimation + 1);
            !claimed) {
            return std::unexpected(with_context(claimed.error(), "Graph::on_block claim"));
        }
    }

    // What the NEXT dispatch's lowest read will be. Retiring through it in the
    // completion handler is what lets the writer overwrite everything older,
    // and doing it there rather than here is the whole point of the two
    // cursors: retire at record time and the writer is told samples are free
    // while an in-flight dispatch is still reading them.
    const dsp::SampleIndex next_first = first_block + block_count;
    frame.retire_through =
        block_count > 0 && next_first * decimation > support ? next_first * decimation - support
                                                             : 0;
    frame.timeline_value = ticket + 1;

    VkTimelineSemaphoreSubmitInfo timeline_info{};
    timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline_info.signalSemaphoreValueCount = 1;
    timeline_info.pSignalSemaphoreValues = &frame.timeline_value;

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.pNext = &timeline_info;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.commands;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &impl.timeline;

    result = impl.context->submit(submit, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkQueueSubmit failed ({})", gpu::result_name(result)), result);
    }

    impl.submissions.fetch_add(1, std::memory_order_relaxed);
    impl.blocks_in.fetch_add(1, std::memory_order_relaxed);
    impl.samples_in.fetch_add(block.sample_count, std::memory_order_relaxed);
    impl.next_block = first_block + block_count;
    impl.submitted.store(ticket + 1, std::memory_order_release);

    if (auto posted = impl.scheduler->post(frame.timeline_value, frame_index); !posted) {
        return std::unexpected(with_context(posted.error(), "Graph::on_block"));
    }
    return {};
}

Status Graph::flush() {
    auto& impl = *impl_;
    if (impl.scheduler == nullptr) {
        return {};
    }
    return impl.scheduler->drain();
}

void Graph::cancel() {
    auto& impl = *impl_;
    impl.cancelled.store(true, std::memory_order_release);

    // Permanent, and on the ring rather than on this graph, because the thread
    // this has to reach is the source's and it is parked inside the ring's
    // reserve_blocking. Cancel is the one place that poisons the ring: the
    // destructor deregisters instead, so a graph that never ran leaves the
    // ring alone.
    if (impl.ring != nullptr) {
        impl.ring->stop();
    }

    // Bumped, not merely notified. See Impl::frame_epoch: a notify that
    // leaves the value alone wakes a parked thread only to have it park
    // again, which is why this call did nothing until 2026-09-20.
    impl.frame_epoch.fetch_add(1, std::memory_order_release);
    impl.frame_epoch.notify_all();
}

GraphStats Graph::stats() const {
    const auto& impl = *impl_;
    GraphStats out;
    out.blocks_in = impl.blocks_in.load(std::memory_order_relaxed);
    out.samples_in = impl.samples_in.load(std::memory_order_relaxed);
    out.submissions = impl.submissions.load(std::memory_order_relaxed);
    out.readbacks = impl.readbacks.load(std::memory_order_relaxed);
    out.dispatches = impl.dispatches.load(std::memory_order_relaxed);
    out.channel_blocks = impl.channel_blocks.load(std::memory_order_relaxed);
    out.overrun_events = impl.overrun_events.load(std::memory_order_relaxed);
    out.samples_dropped = impl.samples_dropped.load(std::memory_order_relaxed);
    out.frame_stalls = impl.frame_stalls.load(std::memory_order_relaxed);
    out.coarse_builds = impl.coarse_builds.load(std::memory_order_relaxed);
    out.audio_frames = impl.audio_frames.load(std::memory_order_relaxed);
    out.audio_dropped = impl.audio_dropped.load(std::memory_order_relaxed);
    out.spectrum_frames = impl.spectrum_frames.load(std::memory_order_relaxed);
    out.spectrum_skipped = impl.spectrum_skipped.load(std::memory_order_relaxed);
    out.passband_frames = impl.passband_frames.load(std::memory_order_relaxed);
    out.passband_skipped = impl.passband_skipped.load(std::memory_order_relaxed);
    out.vrx_retune_refusals = impl.vrx_retune_refusals.load(std::memory_order_relaxed);
    if (impl.ring != nullptr) {
        out.write_index = impl.ring->write_index();
        const auto cursor = impl.ring->cursor(impl.consumer);
        out.retired_index = cursor ? cursor->retired : 0;
    }
    out.next_output_block = impl.next_block;
    return out;
}

}  // namespace revenant::engine
