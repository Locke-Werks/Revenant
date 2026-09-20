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
#include "core/engine/record_util.h"
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

// Below this a sample is silence for every purpose in this engine, and taking
// its logarithm produces -inf, which propagates into a meter and a squelch
// comparison as a NaN nobody can source.
constexpr double kSilenceFloorDbfs = -200.0;

[[nodiscard]] std::span<const std::uint32_t> convert_shader_for(source::SampleFormat format) {
    switch (format) {
        case source::SampleFormat::Cu8: return gpu::shaders::convert_cu8_cf32();
        case source::SampleFormat::Cs8: return gpu::shaders::convert_cs8_cf32();
        case source::SampleFormat::Cs16: return gpu::shaders::convert_cs16_cf32();
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

[[nodiscard]] double dbfs_of(double rms) {
    if (!(rms > 0.0)) {
        return kSilenceFloorDbfs;
    }
    const double value = 20.0 * std::log10(rms);
    return value < kSilenceFloorDbfs ? kSilenceFloorDbfs : value;
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

        vkCmdCopyBuffer(record.commands, record.channel_ring, record.audio_destination,
                        region_count, regions);
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
    // One receiver. Kept behind a shared_ptr so that removing it while a frame
    // is in flight is safe: the frame's snapshot holds a reference and the
    // slot outlives the dispatch that was reading it.
    struct VrxSlot {
        VrxId id;

        // Control plane only, under control_lock. These are what vrx_status
        // reports, and no engine thread reads them.
        VrxParams params;
        VrxPlacement placement;

        // The recording thread's own copy, so that a retune landing while a
        // block is being recorded cannot tear a field out from under it. The
        // two are kept in step by the control queue, which is the only path
        // between them.
        VrxParams recording_params;

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

        // Recording thread only. The completion thread gets its own copy of
        // the pointer in the frame snapshot, so replacing a sink never races
        // a delivery that is already under way.
        std::shared_ptr<AudioSink> sink;

        std::atomic<std::uint64_t> audio_samples{0};
        std::atomic<std::uint64_t> audio_dropped{0};
        std::atomic<std::uint64_t> level_bits{0};
        std::atomic<bool> squelch_open{false};

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
        enum class Kind : std::uint8_t { Add, Remove, Retune, Sink, Spectrum };

        Kind kind = Kind::Add;
        VrxId id;
        VrxParams params;
        VrxPlacement placement;
        std::shared_ptr<VrxSlot> slot;
        std::shared_ptr<AudioSink> sink;
        std::shared_ptr<SpectrumSink> spectrum_sink;
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

    bool has_convert = false;
    gpu::ComputePipeline convert_pipeline;
    gpu::ComputePipeline branch_pipeline;
    gpu::ComputePipeline fft_pipeline;
    gpu::ComputePipeline spectrum_pipeline;
    gpu::ComputePipeline spectrum_levels_pipeline;

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
                        // Discarded deliberately: a stage that cannot retune
                        // keeps its old tuning, which is wrong but audible,
                        // where dropping the receiver would be silent. The
                        // error surfaces on the next status read through the
                        // params not matching.
                        (void)slot->stage->retune(op.params, op.placement);
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
        }
    }

    [[nodiscard]] std::shared_ptr<VrxSlot> find_known(VrxId id) const {
        for (const auto& slot : known) {
            if (slot->id == id) {
                return slot;
            }
        }
        return nullptr;
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
            if (floats > slot.scratch.size()) {
                outcome = fail(std::format(
                    "receiver {} produced {} floats and its scratch holds {}: audio_bytes_for "
                    "returned less than the stage went on to write",
                    slot.id.value, floats, slot.scratch.size()));
                continue;
            }

            const std::span<float> audio(slot.scratch.data(), floats);
            if (auto read = slot.readback[entry.readback_index].read(std::as_writable_bytes(audio));
                !read) {
                outcome = std::unexpected(with_context(
                    read.error(), std::format("receiver {} readback", slot.id.value)));
                continue;
            }

            // The signal meter. Complex output is metered on magnitude, which
            // is what makes a raw tap's level comparable with a demodulator's
            // rather than reading 3 dB low for being two real channels.
            double sum = 0.0;
            if (entry.output.channels == 2) {
                for (std::size_t i = 0; i + 1 < floats; i += 2) {
                    const double re = static_cast<double>(audio[i]);
                    const double im = static_cast<double>(audio[i + 1]);
                    sum += re * re + im * im;
                }
                sum /= static_cast<double>(entry.output.frames);
            } else {
                for (const float value : audio) {
                    const double v = static_cast<double>(value);
                    sum += v * v;
                }
                sum /= static_cast<double>(floats);
            }
            const double level = dbfs_of(std::sqrt(sum));
            slot.store_level(level);

            // Squelched means muted, not stopped. A squelched receiver still
            // feeds its decoder chain and its meter, which is why the level is
            // stored above this and not below it.
            const bool open = level >= entry.squelch_dbfs;
            slot.squelch_open.store(open, std::memory_order_relaxed);
            if (!open) {
                std::fill(audio.begin(), audio.end(), 0.0F);
                slot.audio_dropped.fetch_add(entry.output.frames, std::memory_order_relaxed);
                audio_dropped.fetch_add(entry.output.frames, std::memory_order_relaxed);
            }

            AudioChunk chunk;
            chunk.vrx = slot.id;
            chunk.start = slot.audio_index;
            chunk.rate = entry.output.rate;
            chunk.samples = std::span<const float>(audio.data(), audio.size());
            chunk.channels = entry.output.channels;

            if (entry.sink != nullptr && *entry.sink) {
                if (auto delivered = (*entry.sink)(chunk); !delivered) {
                    outcome = std::unexpected(with_context(
                        delivered.error(), std::format("receiver {} sink", slot.id.value)));
                }
            }

            slot.audio_index += entry.output.frames;
            slot.audio_samples.fetch_add(entry.output.frames, std::memory_order_relaxed);
            audio_frames.fetch_add(entry.output.frames, std::memory_order_relaxed);
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
        completed.notify_all();
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

        if (auto delivered = (*frame.spectrum_sink)(out); !delivered) {
            return std::unexpected(with_context(delivered.error(), "spectrum sink"));
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
    std::string spectrum_note;
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
            spectrum_note =
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
    if (!spectrum_note.empty()) {
        impl.geometry.clamped = true;
        if (!impl.geometry.clamp_reason.empty()) {
            impl.geometry.clamp_reason.append("; also ");
        }
        impl.geometry.clamp_reason.append(spectrum_note);
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

        auto window = dsp::build_spectrum_window(points);
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

        // The percentile, measured on the device from the frame above. No
        // grid constants: this kernel's shape is entirely in its push
        // constants, so one pipeline serves every geometry. The kernel beside
        // it does the opposite, and docs/fft.md records an integrated-device
        // fault that tracks exactly how many pipelines one module is
        // specialized into, so this one adds none.
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

Expected<VrxId> Graph::add_vrx(VrxId id, const VrxParams& params, const VrxPlacement& placement) {
    auto& impl = *impl_;
    if (!impl.prepared) {
        return fail("Graph::add_vrx before prepare");
    }
    if (!id.valid()) {
        return fail("Graph::add_vrx with a zero id");
    }

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
    request.audio_rate = params.audio_rate != 0 ? params.audio_rate : impl.config.audio_rate;

    std::unique_ptr<VrxStage> stage;
    if (impl.factory) {
        auto built = impl.factory(request);
        if (!built) {
            return std::unexpected(with_context(built.error(), "Graph::add_vrx"));
        }
        stage = std::move(*built);
    }
    if (stage == nullptr) {
        if (params.demod != Demod::Raw) {
            return fail(std::format(
                "no stage factory is installed, so '{}' cannot be built. The graph ships one "
                "stage of its own, the raw complex tap (Demod::Raw), which needs no kernel. "
                "Every other demodulator arrives with the fine-stage package and installs "
                "itself through engine::install_vrx_stage_factory",
                demod_name(params.demod)));
        }
        stage = std::make_unique<RawTapStage>(request);
    }

    auto slot = std::make_shared<Impl::VrxSlot>();
    slot->id = id;
    slot->params = params;
    slot->placement = placement;
    slot->recording_params = params;
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
        slot->params = params;
        slot->placement = placement;
    }

    auto* op = new (std::nothrow) Impl::ControlOp();
    if (op == nullptr) {
        return fail("Graph::set_vrx_params could not allocate the control operation");
    }
    op->kind = Impl::ControlOp::Kind::Retune;
    op->id = id;
    op->params = params;
    op->placement = placement;
    impl.push_control(op);
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
    status.level_dbfs = slot->load_level();
    status.squelch_open = slot->squelch_open.load(std::memory_order_relaxed);
    status.audio_samples = slot->audio_samples.load(std::memory_order_relaxed);
    status.audio_dropped = slot->audio_dropped.load(std::memory_order_relaxed);
    return status;
}

std::vector<VrxId> Graph::vrx_ids() const {
    auto& impl = *impl_;
    std::scoped_lock lock(impl.control_lock);
    std::vector<VrxId> out;
    out.reserve(impl.known.size());
    for (const auto& slot : impl.known) {
        out.push_back(slot->id);
    }
    return out;
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
    if (target_end <= reserved) {
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
        impl.completed.wait(done, std::memory_order_acquire);
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

            auto recorded = slot->stage->record(record);
            if (!recorded) {
                (void)vkEndCommandBuffer(frame.commands);
                impl.ring->release_reservation(granted);
                return std::unexpected(with_context(
                    recorded.error(), std::format("receiver {} stage", slot->id.value)));
            }

            Impl::FrameVrx entry;
            entry.slot = slot;
            entry.sink = slot->sink;
            entry.readback = slot->readback[frame_index].handle();
            entry.readback_index = frame_index;
            entry.output = *recorded;
            entry.squelch_dbfs = slot->recording_params.squelch_dbfs;
            frame.vrxs.push_back(std::move(entry));
        }
        impl.readbacks.fetch_add(frame.vrxs.size(), std::memory_order_relaxed);
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
    impl.completed.notify_all();
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
    if (impl.ring != nullptr) {
        out.write_index = impl.ring->write_index();
        const auto cursor = impl.ring->cursor(impl.consumer);
        out.retired_index = cursor ? cursor->retired : 0;
    }
    out.next_output_block = impl.next_block;
    return out;
}

}  // namespace revenant::engine
