// The signal graph: the one object that knows the order the stages run in and
// the only place a sample changes hands.
//
// THE CENTRAL CLAIM, AND WHAT MAKES IT TRUE HERE
//
// Samples cross the bus once. That sentence is either an architecture or a
// slogan, and the difference is entirely in this file. What makes it true:
//
//   1. A source block is written once, by the host, into a pinned Upload
//      buffer. That is the last time host code touches a sample.
//   2. The convert kernel reads that pinned buffer directly as a storage
//      buffer and writes Complex32 into the device ring at a masked offset.
//      The bus is crossed by the kernel's reads and by nothing else. A cf32
//      source has no widening to do, so it crosses as a vkCmdCopyBuffer into
//      the ring instead, split in two where the destination straddles the
//      wrap.
//   3. Branch filter, transform and every receiver's stage read and write
//      device-local memory only.
//   4. One vkCmdCopyBuffer at the end brings audio back.
//
// All of that is recorded into ONE command buffer with explicit barriers
// between stages, and submitted once. gpu::CommandRunner submits and waits per
// call, which is right for a reference diff and wrong here: it would make
// every stage boundary a host round trip, which is the exact cost the design
// exists to avoid. So the graph records its own.
//
// The host waits once per block, in the completion thread, for the timeline
// value that says the audio readback has landed. Scheduler::stats().host_waits
// is that count, and it is the number to check when somebody claims a stage
// has started round-tripping.
//
// WHY THE FINE STAGE AND THE DEMODULATOR ARE A SEAM AND NOT CODE HERE
//
// Everything above the channel ring is per receiver, and a receiver's
// arithmetic is a different work package from the plumbing that gets samples
// to it. VrxStage is that seam. The graph allocates each receiver's readback
// region, records its stage into the shared command buffer between the
// transform and the readback, and carries its audio home; what the stage does
// in between is the demodulator package's business.
//
// One stage ships here: the raw tap, Demod::Raw, which is a copy out of the
// channel ring and needs no kernel of its own. It is not a placeholder for a
// demodulator. core/engine/vrx.h names the raw tap as a first-class mode
// precisely so that external tooling and the decoder framework have something
// to attach to before any decoder exists, and it is what proves the whole
// path end to end today.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "core/dsp/pfb.h"
#include "core/dsp/types.h"
#include "core/engine/device_ring.h"
#include "core/engine/engine.h"
#include "core/engine/scheduler.h"
#include "core/engine/vrx.h"
#include "core/error.h"
#include "core/gpu/context.h"
#include "core/source/capabilities.h"
#include "core/source/source.h"

namespace revenant::engine {

// ---------------------------------------------------------------------------
// The per-receiver seam
// ---------------------------------------------------------------------------

// Everything a stage needs to build itself, handed over once at construction.
//
// All of it is fixed for the life of the receiver except params and placement,
// which change through retune(). Nothing here comes from the coarse stage's
// mutable state, which is the structural reason adding a receiver cannot
// disturb the grid.
struct VrxStageRequest {
    const gpu::Context* context = nullptr;

    VrxId id;
    VrxParams params;
    VrxPlacement placement;

    dsp::GridParams grid;
    dsp::SampleRate source_rate = 0;

    // Rate the coarse channel arrives at, source_rate / grid.decimation.
    dsp::SampleRate channel_rate = 0;

    // The channel ring this stage reads: channel-major, so channel k's time
    // series is contiguous at k * blocks + (m & mask), which is the
    // chan_base and chan_mask core/shaders/vrx_fine.comp takes as push
    // constants.
    //
    // The handle is here as well as in StageRecord so a stage can write its
    // descriptor sets once, at construction. Every buffer the coarse chain
    // binds is fixed for the life of the graph and a stage's should be too:
    // rewriting a descriptor set per block is ceremony on the sample path.
    VkBuffer channel_ring = VK_NULL_HANDLE;
    VkDeviceSize channel_ring_bytes = 0;
    std::uint32_t channel_ring_blocks = 0;
    std::uint32_t channel_ring_mask = 0;

    // Upper bound on the channel blocks one dispatch will present, which is
    // what the stage sizes its own buffers against.
    std::uint32_t max_blocks_per_dispatch = 0;

    // How many command buffers are in flight, so a stage that needs per-frame
    // scratch knows how many copies to make.
    std::uint32_t frames_in_flight = 0;

    std::uint32_t local_size_x = 0;

    // The engine's audio rate, already resolved from VrxParams::audio_rate or
    // the engine default.
    dsp::SampleRate audio_rate = 0;

    // Points in a passband transform, or 0 when the graph has no passband
    // stage. See GraphConfig::passband_transform.
    //
    // A stage that keeps a fine ring has to hold a whole window of this
    // length below everything the frames in flight are writing, or the
    // transform reads slots a later dispatch has already overwritten. That
    // is a sizing decision made once at construction, which is why it is
    // here rather than on StageRecord: growing a ring mid-stream would mean
    // freeing a buffer an in-flight command buffer still names.
    std::uint32_t passband_transform = 0;
};

// What one recorded dispatch will produce on the host side.
struct StageOutput {
    // Audio frames, not floats. A stereo frame is two floats.
    std::uint32_t frames = 0;

    // What this call actually put in the command buffer, which is not a
    // function of the receiver existing. A stage returns early with none of
    // these recorded when it has no whole audio sample to make this time
    // round, and GraphStats counts commands rather than receivers, so the
    // stage is the only thing that can report them.
    std::uint32_t dispatches = 0;

    // Device-to-host copies, which is vkCmdCopyBuffer calls into a mapped
    // readback buffer and not the regions inside one: a copy split in two by
    // the ring's wrap is one crossing of the bus.
    std::uint32_t readbacks = 0;

    // 1 for mono audio, 2 for a stereo pair or for a raw complex tap
    // presented as interleaved I/Q. Two different things, and this field
    // cannot tell them apart; complex_iq below is what does.
    std::uint32_t channels = 1;

    // Whether a pair of floats is one complex sample rather than two audio
    // channels. Only the graph's own raw tap sets it.
    //
    // A SEPARATE FIELD BECAUSE THE COUNT WAS OVERLOADED AND SOMETHING READ
    // IT. The signal meter branched on channels == 2 and metered a stereo
    // WFM receiver on magnitude, so its level and its squelch comparison
    // both read 3.01 dB high from the day FM stereo landed. A stage that
    // leaves this false and sets channels to 2 is saying "two real audio
    // channels", which is what every demodulator means.
    bool complex_iq = false;

    dsp::SampleRate rate = 0;

    // How far the stage's own complex baseband has got after this dispatch,
    // as a half-open range of absolute fine-stream indices: [fine_from,
    // fine_next) are the receiver's samples and are live in the ring
    // fine_output() names.
    //
    // Both, and not just the end, because the passband transform's window
    // reaches back over several dispatches and the bottom of the ring is not
    // the stream. Below fine_from is whatever the stage cleared the ring to,
    // and a window straddling that boundary transforms a step that was never
    // on the air. It is the same gate the full-span spectrum applies against
    // blocks_contiguous_from, one stream down.
    //
    // Left zero by a stage with no fine ring, which is what an empty
    // fine_output() already says.
    dsp::SampleIndex fine_from = 0;
    dsp::SampleIndex fine_next = 0;

    // Where the fine stream's DC sits in the SOURCE's baseband frame, as an
    // exact rational in hertz: the coarse channel's centre plus the residual
    // the stage mixes out.
    //
    // Not the receiver's centre. CW translates the carrier to the operator's
    // pitch instead of to DC, so the two differ by the pitch there and agree
    // on the other seven modes. The stage is the only thing that knows which,
    // so the stage is what reports it.
    //
    // Here rather than on StageFineOutput because a retune moves it, and
    // this is the one surface the graph reads on the recording thread only.
    // Everything on StageFineOutput is fixed at construction, which is what
    // lets the control plane read that struct while a block is being
    // recorded.
    std::int64_t fine_dc_numerator = 0;
    std::int64_t fine_dc_denominator = 1;
};

// A stage's own complex baseband on the device, for a second transform over
// the receiver's passband.
//
// The fine stage already writes exactly this: mixed to DC, limited to the
// requested bandwidth, at the demodulation rate. docs/ui-spectrum.md calls
// that the reason the fine-tuning display is cheap, since the expensive part
// is already paid for by the demodulator. This is the seam that lets the
// graph reach it without owning the demodulator's arithmetic.
//
// Everything here is fixed for the life of the stage. A retune moves where
// the receiver points and changes no field below, which is what lets the
// graph write a descriptor set once.
struct StageFineOutput {
    // VK_NULL_HANDLE when the stage keeps no such ring, which is how a stage
    // declines a passband rather than by failing one.
    VkBuffer ring = VK_NULL_HANDLE;

    // Complex samples, a power of two, and its mask.
    std::uint32_t capacity = 0;
    std::uint32_t mask = 0;

    // The fine stream's rate, which is the width of a passband frame.
    dsp::SampleRate rate = 0;

    // Delay from the channel stream to the fine stream, in channel samples,
    // so the graph can say when a window's energy was on the air rather than
    // when its samples reached this stage. Fractional because an
    // interpolating prototype's centre is.
    //
    // Fixed like the rest: a retune that would change the filter's shape is
    // refused outright, so the delay a shape implies cannot move either.
    double group_delay_channel_samples = 0.0;
};

// What a stage is handed at record time, once per block per receiver.
struct StageRecord {
    VkCommandBuffer commands = VK_NULL_HANDLE;

    // Which of the frames_in_flight slots this is, for a stage keeping
    // per-frame scratch.
    std::uint32_t frame_index = 0;

    VkBuffer channel_ring = VK_NULL_HANDLE;
    VkDeviceSize channel_ring_bytes = 0;

    // Absolute output-block index of the first block this dispatch produced,
    // and how many. Absolute, uint64, because time is a sample index and a
    // stage that needs a 32-bit value truncates it itself with the same
    // reasoning core/shaders/pfb_fft.comp states for block_base.
    dsp::SampleIndex first_block = 0;
    std::uint32_t block_count = 0;

    // Where the audio goes. Host-visible, owned by the graph, and exactly the
    // size audio_bytes_for(max_blocks_per_dispatch) asked for.
    VkBuffer audio_destination = VK_NULL_HANDLE;
    VkDeviceSize audio_bytes = 0;
};

// One receiver's fine stage and demodulator, recorded into the graph's command
// buffer.
//
// Thread safety: record() is called on the recording thread only, and never
// concurrently with retune(), which the graph applies at a block boundary on
// the same thread. A stage needs no synchronisation of its own.
class VrxStage {
public:
    virtual ~VrxStage() = default;

    VrxStage(const VrxStage&) = delete;
    VrxStage& operator=(const VrxStage&) = delete;
    VrxStage(VrxStage&&) = delete;
    VrxStage& operator=(VrxStage&&) = delete;

    // Bytes this stage will write for `blocks` channel blocks. Called once
    // with max_blocks_per_dispatch to size the readback region, so it must be
    // an upper bound and not an estimate.
    [[nodiscard]] virtual VkDeviceSize audio_bytes_for(std::uint32_t blocks) const = 0;

    // Records the fine stage, the demodulator and the write into
    // record.audio_destination, and returns what the host will find there.
    //
    // The return is how many audio frames this dispatch produced, not how
    // many it could have. A fractional resampler running Fc/Fd produces a
    // count that depends on an accumulator the stage carries between blocks,
    // so it is the stage that knows, and it knows only once it has decided.
    // Asking separately would mean either recomputing the recurrence or
    // trusting a stale answer.
    //
    // Must not submit, must not wait, and must not allocate on this path.
    [[nodiscard]] virtual Expected<StageOutput> record(const StageRecord& record) = 0;

    // A retune that stays on the same coarse channel is a push constant. One
    // that moves to another channel is still only this stage's business,
    // because the channel it reads is an index it holds and not anything the
    // grid knows about.
    [[nodiscard]] virtual Status retune(const VrxParams& params,
                                        const VrxPlacement& placement) = 0;

    // The stage's own complex baseband, or an empty record when it keeps
    // none. Called on the control plane, once, when a passband sink is
    // attached, and never on the sample path.
    //
    // Defaulted rather than pure so that a stage written before the passband
    // existed still compiles and simply declines one. The graph's raw tap is
    // that case and stays that case: it copies a coarse channel out of the
    // channel ring without mixing or filtering, so it has no per-receiver
    // baseband of its own to transform.
    [[nodiscard]] virtual StageFineOutput fine_output() const { return {}; }

protected:
    VrxStage() = default;
};

using VrxStageFactory =
    std::function<Expected<std::unique_ptr<VrxStage>>(const VrxStageRequest& request)>;

// Installs the factory the graph asks for a stage it cannot build itself.
//
// A free function rather than a field on EngineConfig because EngineConfig is
// frozen, and a registration hook rather than a link-time dependency because
// the demodulator package and this one are written separately and neither
// should have to include the other's headers to be linked.
//
// Thread safety: control plane. Call it once during start-up, before any
// Engine is created. The graph reads it at construction and holds its own
// copy, so replacing it later does not disturb a running engine.
void install_vrx_stage_factory(VrxStageFactory factory);

[[nodiscard]] VrxStageFactory installed_vrx_stage_factory();

// ---------------------------------------------------------------------------
// The graph
// ---------------------------------------------------------------------------

struct GraphConfig {
    dsp::GridParams grid;
    dsp::SampleRate source_rate = 0;
    source::SampleFormat format = source::SampleFormat::Cf32;
    source::FlowControl flow = source::FlowControl::Demand;

    // Samples the source delivers at a time. The staging buffer is sized from
    // it, so a block larger than this is rejected rather than truncated.
    std::size_t block_samples = 65'536;

    dsp::SampleRate audio_rate = 48'000;

    // Command buffers, staging buffers and readback regions in flight. Three
    // is enough to keep a queue fed while the host records the next block and
    // the completion thread drains the last.
    std::uint32_t frames_in_flight = 3;

    // Per-channel capacity of the channel ring, in output blocks. 0 derives a
    // power of two large enough that every frame in flight owns a disjoint
    // range, which is what lets frames overlap on the device.
    std::uint32_t channel_ring_blocks = 0;

    // 0 takes a size the device allows. Never hardcoded: the conformance
    // matrix runs the same kernels on a device with a smaller limit.
    std::uint32_t local_size_x = 0;

    // Points in the full-span spectrum's per-channel transform, or 0 to build
    // no spectrum stage. See EngineConfig::spectrum_transform, which is where
    // this comes from and where the reasoning is.
    //
    // A non-zero value enlarges the channel ring: the stage transforms the
    // last N blocks of every channel, so the ring has to hold that window
    // plus everything the frames in flight are writing above it.
    std::uint32_t spectrum_transform = 0;

    // Holds one or both ends of the colour map still. See
    // EngineConfig::spectrum_floor_db, which is where these come from and
    // where the reasoning is.
    std::optional<float> spectrum_floor_db;
    std::optional<float> spectrum_ceiling_db;

    // Points in a receiver's passband transform, or 0 to build no passband
    // stage. See EngineConfig::passband_transform, which is where this comes
    // from and where the reasoning is.
    //
    // A non-zero value enlarges every receiver's fine ring by a window's
    // worth, whether or not that receiver ever attaches a sink, because the
    // ring is sized once when the stage is built.
    std::uint32_t passband_transform = 0;
};

// What the graph settled on, which the caller needs to see rather than infer.
struct GraphGeometry {
    dsp::SampleRate channel_rate = 0;
    std::uint32_t channel_ring_blocks = 0;
    std::uint32_t channel_ring_mask = 0;
    std::uint32_t max_blocks_per_dispatch = 0;
    std::uint32_t frames_in_flight = 0;
    std::uint32_t local_size_x = 0;
    std::uint32_t fft_local_size_x = 0;
    std::uint32_t spectrum_local_size_x = 0;
    std::uint32_t spectrum_levels_local_size_x = 0;

    // Empty when GraphConfig::spectrum_transform was zero.
    SpectrumGeometry spectrum{};

    // Zero when GraphConfig::passband_transform was, and otherwise what it
    // was clamped to against this device's shared memory.
    std::uint32_t passband_transform = 0;
    std::uint32_t passband_local_size_x = 0;

    std::size_t block_samples = 0;
    std::uint64_t channel_ring_bytes = 0;
    std::uint64_t staging_bytes = 0;

    // True when block_samples was reduced, with the reason, for the same
    // reason RingGeometry reports its clamp: a caller who asked for one thing
    // and got another is told.
    bool clamped = false;
    std::string clamp_reason;
};

struct GraphStats {
    std::uint64_t blocks_in = 0;
    std::uint64_t samples_in = 0;

    // Queue submissions. One per block is the contract; anything more means a
    // stage grew its own submission.
    std::uint64_t submissions = 0;

    // Device-to-host copies recorded: a receiver's audio, a passband frame
    // and its two percentiles, the spectrum frame and its two percentiles.
    // All of them sit inside the one command buffer, so they cost no extra
    // submission and no extra wait.
    //
    // WHAT THIS USED TO SAY, AND WHAT IT USED TO COUNT. Until 2026-09-20 it
    // read "one per block per receiver" and the graph added the number of
    // active receivers once per block, which was wrong in both directions at
    // once. A receiver whose stage had no whole audio sample this dispatch
    // records no copy and was counted anyway; the spectrum's two copies and
    // each passband's two were not counted at all. The number now comes back
    // from whatever recorded the command, which is the only place that knows
    // whether it did.
    std::uint64_t readbacks = 0;

    // Compute dispatches recorded, by the same rule and for the same reason.
    // Until 2026-09-20 this counted the convert, the channelizer's two, the
    // spectrum's two and the passband's three per receiver, and silently
    // omitted every VrxStage: the fine filter and the demodulator, which are
    // the two dispatches per receiver per block that the whole per-receiver
    // path consists of.
    std::uint64_t dispatches = 0;
    std::uint64_t channel_blocks = 0;

    // Blocks the graph could not place. Only a Paced source can produce these:
    // a Demand source blocks instead, which is the backpressure.
    std::uint64_t overrun_events = 0;
    std::uint64_t samples_dropped = 0;

    // Times the recording thread had to wait for a frame slot. Expected and
    // healthy on a Demand source, a warning sign on a Paced one.
    std::uint64_t frame_stalls = 0;

    // Times the coarse stage was built. Must be exactly one for the life of
    // an open source, whatever receivers come and go. add_vrx asserting on
    // this is how the claim in core/engine/vrx.h is kept honest.
    std::uint64_t coarse_builds = 0;

    std::uint64_t audio_frames = 0;
    std::uint64_t audio_dropped = 0;

    // Spectrum frames delivered, and dispatches that produced none because
    // the window's history was not yet contiguous. The second is expected
    // exactly once at the start of a stream and after every overrun, and is a
    // counter rather than a log line for the same reason every other loss
    // here is.
    std::uint64_t spectrum_frames = 0;
    std::uint64_t spectrum_skipped = 0;

    // Passband frames produced across every receiver that has a sink, and
    // dispatches that produced none because the fine stream did not yet hold
    // a whole contiguous window. The second is expected for the first few
    // blocks after a receiver is added and after any skip, for the same
    // reason spectrum_skipped is.
    std::uint64_t passband_frames = 0;
    std::uint64_t passband_skipped = 0;

    // Retunes a stage refused after set_vrx_params had already accepted
    // them. MUST BE ZERO. Both sides ask dsp::vrx_shape_for the same
    // question, so a count above zero means the graph and the stage have
    // come apart about what a rebuild is, and the symptom on the far side
    // is a client told its change took effect over a receiver still running
    // the old filter.
    //
    // A counter rather than a log line, for the reason every other loss
    // here is one, and a counter rather than nothing because the refusal
    // arrives on the recording thread with no caller left to return it to.
    // Discarding it silently is what let the two lists differ unnoticed.
    std::uint64_t vrx_retune_refusals = 0;

    dsp::SampleIndex write_index = 0;
    dsp::SampleIndex retired_index = 0;
    dsp::SampleIndex next_output_block = 0;
};

class Graph {
public:
    [[nodiscard]] static Expected<std::unique_ptr<Graph>> create(const gpu::Context& context,
                                                                  DeviceRing& ring,
                                                                  Scheduler& scheduler,
                                                                  const GraphConfig& config);

    ~Graph();

    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;
    Graph(Graph&&) = delete;
    Graph& operator=(Graph&&) = delete;

    // Uploads the prototype and the twiddle table and builds the three coarse
    // pipelines. Once, before the first block. This is the "coarse stage" that
    // add_vrx is forbidden to rebuild.
    [[nodiscard]] Status prepare(const dsp::PrototypeFilter& prototype,
                                 std::span<const dsp::Complex32> twiddles);

    [[nodiscard]] const GraphGeometry& geometry() const;

    // --- control plane, any thread but the recording thread -----------------

    [[nodiscard]] Expected<VrxId> add_vrx(VrxId id, const VrxParams& params,
                                          const VrxPlacement& placement);
    [[nodiscard]] Status remove_vrx(VrxId id);
    [[nodiscard]] Status set_vrx_params(VrxId id, const VrxParams& params,
                                        const VrxPlacement& placement);
    [[nodiscard]] Status set_audio_sink(VrxId id, AudioSink sink);
    [[nodiscard]] Status set_spectrum_sink(SpectrumSink sink);

    // Attaches or detaches one receiver's passband. Allocating here rather
    // than in add_vrx is the per-receiver opt-in: a receiver nobody is
    // examining holds no transform buffers and records no dispatch.
    //
    // The allocation happens on this thread, before the control operation is
    // queued, so the recording thread never allocates. Detaching hands the
    // buffers to the recording thread to drop, and a frame already in flight
    // holds its own reference, so the memory outlives the dispatch reading
    // it.
    [[nodiscard]] Status set_passband_sink(VrxId id, PassbandSink sink);
    [[nodiscard]] Expected<VrxStatus> vrx_status(VrxId id) const;
    [[nodiscard]] std::vector<VrxId> vrx_ids() const;

    // --- the sample path, the source thread only ----------------------------

    // The source's BlockSink. Blocks as backpressure against a Demand source
    // and never blocks against a Paced one, where a block that cannot be
    // placed is a counted overrun instead.
    [[nodiscard]] Status on_block(const source::SourceBlock& block);

    // Waits for every in-flight frame and delivers its audio. Called when the
    // stream ends, never per block.
    [[nodiscard]] Status flush();

    // Wakes anything parked in on_block so a stop does not have to wait for
    // the ring to drain.
    void cancel();

    [[nodiscard]] GraphStats stats() const;

private:
    Graph();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace revenant::engine
