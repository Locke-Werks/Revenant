// The engine, assembled.
//
// This file is deliberately thin. Everything hard is in graph.cpp, which owns
// the submission model, and in scheduler.cpp, which owns the threads. What is
// left here is the part the frozen core/engine/engine.h actually describes:
// opening a source, sizing the grid and the ring against it, reporting what
// was settled on, and driving the whole thing from run() to stop().
//
// TWO DECISIONS THAT SHOW UP IN EngineInfo AND ARE NOT OBVIOUS
//
// The grid is sized against the DEVICE before it is sized against the source.
// A 4096-channel grid is arithmetic that fits comfortably and shared memory
// that does not: the FFT holds a whole transform resident, the AMD integrated
// part in the conformance matrix caps maxComputeSharedMemorySize at 32768
// where the RTX 4090 offers 49152, and a grid that exceeds it cannot be
// launched at all. So the channel count is clamped to what the device holds
// and EngineInfo::grid says what happened.
//
// EngineInfo::channel_rate and channel_spacing are integer hertz, and
// rate / D and rate / M are frequently not whole numbers: 2,500,000 over 64
// channels is 39062.5 Hz of spacing. These two fields therefore truncate, and
// they are for display. Nothing in the engine computes from them. The exact
// value is dsp::ChannelCentre, carried as the rational it is, and
// VrxPlacement::channel_centre is what a receiver actually tunes against. See
// the comment on ChannelCentre in core/dsp/pfb.h for why rounding it anywhere
// else is the unsourceable tuning offset the conventions exist to prevent.
//
// WHAT STOP() COSTS
//
// stop() ends the stream for good. Waking a source thread parked in the
// ring's blocking reserve means stopping the cursor table, and a stopped
// cursor table does not restart. Running again means opening the source
// again, which costs a file handle and a grid design and nothing else. The
// alternative is a restart path that exists to be tested and never used, and
// a teardown that can hang when the GPU does.

#include "core/engine/engine.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <format>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "core/dsp/pfb_fft_reference.h"
#include "core/dsp/spectrum_reference.h"
#include "core/engine/graph.h"
#include "core/engine/ring_consumer.h"
#include "core/engine/scheduler.h"
#include "core/engine/vrx_stage.h"

namespace revenant::engine {
namespace {

// How often run() looks to see whether the source thread has finished.
//
// A poll and not a signal, because core/source/source.h offers no completion
// callback: Source::running() goes false when the delivery loop exits and
// nothing notifies. Two milliseconds is below any latency a person can
// perceive at the end of a stream and costs five hundred atomic loads a
// second on one thread. The integrator note asks for an end-of-stream
// callback on Source, which would delete this.
constexpr std::chrono::milliseconds kRunPollInterval{2};

// How long close_source waits for a running stream to stop before reporting
// that it would not.
//
// What it is waiting for is a device stop plus a GPU flush: on an RTL-SDR that
// is rtlsdr_cancel_async through libusb and the worker joining, and on the
// device side it is the scheduler retiring whatever is already submitted. Both
// are milliseconds on the hardware this has run on, so five seconds is not a
// budget, it is the point past which the honest answer is that something is
// wedged and a caller should hear about it rather than block.
//
// Bounded at all because the caller in the deployment that needs this is an RPC
// loop thread. A wait with no deadline there is a client that hangs, and a
// client that hangs is diagnosed as a network fault.
constexpr std::chrono::milliseconds kCloseStopTimeout{5000};

// The 2x-oversampled design. A critically sampled bank splits a signal sitting
// on a channel edge across two channels and neither one is usable, which is
// the whole reason core/dsp/pfb.h calls D = M/2 the project's choice rather
// than a tuning parameter.
[[nodiscard]] std::uint32_t oversampled_decimation(std::uint32_t channels) {
    return channels > 1 ? channels / 2 : 1;
}

class EngineImpl final : public Engine {
public:
    EngineImpl() = default;

    ~EngineImpl() override {
        (void)stop();
        // Order matters. The source's thread calls into the graph, the graph's
        // completion thread calls into the receivers' sinks, and the ring
        // outlives both. Destroying them in declaration order would take the
        // graph out from under a source thread that has not joined.
        source_.reset();
        graph_.reset();
        scheduler_.reset();
    }

    [[nodiscard]] Status configure(const EngineConfig& config) {
        if (!std::isfinite(config.pace) || config.pace < 0.0) {
            return fail(std::format(
                "pace must be zero for unthrottled or a positive multiple of realtime, got {}",
                config.pace));
        }
        config_ = config;

        gpu::Context::Options options;
        options.device_index = config.gpu_index;
        auto context = gpu::Context::create(options);
        if (!context) {
            return std::unexpected(with_context(context.error(), "Engine::create"));
        }
        context_ = std::move(*context);
        info_.device = context_.info();

        SchedulerConfig scheduler_config;
        auto scheduler = Scheduler::create(context_, scheduler_config);
        if (!scheduler) {
            return std::unexpected(with_context(scheduler.error(), "Engine::create"));
        }
        scheduler_ = std::move(*scheduler);
        return {};
    }

    [[nodiscard]] Status open_source(std::string_view uri) override {
        const std::scoped_lock lifecycle(lifecycle_lock_);

        if (source_ != nullptr) {
            return fail("this engine already has a source open. Close it first: this is two "
                        "calls rather than a replace, because a replace that failed on the new "
                        "URI would have destroyed the working source already");
        }

        // A CLAMP NOTE FROM A PREVIOUS SOURCE IS NOT THIS SOURCE'S. It is
        // appended to rather than assigned, so without this the second open on
        // an engine reports the first one's ring clamp beside its own, and the
        // third reports both.
        clamp_note_.clear();

        auto opened = source::open_source(uri);
        if (!opened) {
            return std::unexpected(with_context(opened.error(), "Engine::open_source"));
        }
        auto source = std::move(*opened);

        const dsp::SampleRate rate = source->sample_rate();
        if (rate <= 0) {
            return fail(std::format("'{}' reports a sample rate of {}", uri, rate));
        }

        // --- the grid -------------------------------------------------------

        dsp::GridParams grid;
        grid.channels = config_.channels;
        grid.taps_per_branch = config_.taps_per_branch;

        // The device's shared memory, not a guess. See the note at the top of
        // the file and max_fft_transform_size in core/dsp/pfb_fft_reference.h.
        //
        // Hoisted above the channel count rather than left below it, because
        // the resolution request can WIDEN the grid and needs the same ceiling
        // the clamp below applies. One number, computed once, so a widening
        // cannot reach past what the clamp would take back.
        const std::uint32_t transform_ceiling =
            dsp::max_fft_transform_size(info_.device.max_workgroup_shared_memory);
        if (transform_ceiling == 0) {
            return fail(std::format("'{}' reports {} bytes of workgroup shared memory, which "
                                    "holds no transform at all",
                                    info_.device.name, info_.device.max_workgroup_shared_memory));
        }

        // Zero is "work it out from the source", which is the only answer
        // that can be right on both a 2.4 MS/s dongle and a 20 MS/s capture.
        // See default_channel_count. A caller that names a count gets
        // exactly that count, clamped only by the device.
        const bool channels_chosen_here = grid.channels == 0;
        if (channels_chosen_here) {
            grid.channels = default_channel_count(rate);
        }

        // --- how fine a spectrum this source's content needs ------------------
        //
        // THE HALF OF source::ResolutionRequest THAT CHOOSES, and this is the
        // only place that holds every number it needs: the source's request,
        // the rate, and the device's ceiling.
        //
        // WHAT THE DEFAULT GETS WRONG. Every grid constant here was tuned for
        // an RTL-SDR watching broadcast FM: 2.4 MS/s over 64 channels with a
        // 2048-point transform is 65536 bins at 36.6 Hz, and the narrowest
        // thing on that band is 12.5 kHz, so it is met by a factor of 85. Point
        // the same geometry at HF and FT8 is 1.4 bins wide and PSK31 is under
        // one. The detector still fires; what it publishes is a centre it
        // cannot place inside the signal and a width that is the bin's rather
        // than the signal's, which is worse than a miss because it reads as a
        // working detector.
        //
        // THE CHANNEL COUNT IS THE LEVER AND THE TRANSFORM IS NOT, which is the
        // opposite of what SourceCapabilities::resolution used to suggest. Bin
        // width is rate / (D * N) and D is M/2, so both M and N narrow it, but
        // N is capped at dsp::kMaxSpectrumTransform, which is 2048 and is a
        // twiddle-table limit shared with the channelizer rather than a device
        // one. At the shipped 2048 the transform is already at that cap, so it
        // has no room left to give. M has plenty: 2 MS/s of HF needs 7.75 Hz
        // bins, which is M = 256 at N = 2048.
        //
        // WHAT IT COSTS, BECAUSE IT IS NOT FREE. More channels is a finer
        // waterfall and a NARROWER WIDEST RECEIVER, and the two trade directly:
        // M = 256 over 2 MS/s is 7.8 kHz of channel spacing, so no receiver
        // wider than that can be placed. That is right for HF, where the
        // widest thing in the band plan is a few kilohertz, and would be wrong
        // on VHF, where it would refuse a 12.5 kHz NFM channel. The request is
        // only stated by sources that know their own span, and
        // source::resolution_for_span only asks for the fine grid below 30 MHz.
        //
        // ONLY WHEN THE CALLER NAMED NO COUNT. The contract above is that a
        // caller who names one gets exactly that, and overriding it here would
        // take away the only lever they have. One who named a count too coarse
        // for the band is told, below, rather than corrected.
        const source::ResolutionRequest& wanted = source->capabilities().resolution;
        const auto met_at = [&](std::uint32_t channels) {
            // rate / (D * N) hertz per bin, exactly, which is the rational
            // spectrum_geometry_for builds. A rounded integer would put the
            // decision one bin either side of the truth.
            const std::uint32_t transform = config_.spectrum_transform == 0
                                                ? dsp::kMaxSpectrumTransform
                                                : config_.spectrum_transform;
            return wanted.met_by(rate, static_cast<std::int64_t>(
                                           oversampled_decimation(channels)) *
                                           static_cast<std::int64_t>(transform));
        };

        if (wanted.stated() && channels_chosen_here && !met_at(grid.channels)) {
            std::uint32_t finer = grid.channels;
            while (finer < transform_ceiling && !met_at(finer)) {
                finer *= 2;
            }
            if (finer != grid.channels) {
                clamp_note_ = std::format(
                    "this source asks for {} bins across a {} Hz signal ({}), which {} channels "
                    "do not give at this rate, so the grid was widened to {}. That is {} Hz of "
                    "channel spacing, and no receiver wider than that can be placed",
                    wanted.bins_across_narrowest, wanted.narrowest_signal_hz,
                    wanted.basis.empty() ? "no reason given" : wanted.basis, grid.channels,
                    finer, rate / static_cast<dsp::SampleRate>(finer));
                grid.channels = finer;
            }
        }

        if (!std::has_single_bit(grid.channels)) {
            return fail(std::format("channels is {} and the channelizer needs a power of two",
                                    grid.channels));
        }

        if (grid.channels > transform_ceiling) {
            clamp_note_ = std::format(
                "{} channels need {} bytes of shared memory and '{}' offers {}, so the grid was "
                "reduced to {} channels",
                grid.channels, dsp::fft_shared_bytes(grid.channels), info_.device.name,
                info_.device.max_workgroup_shared_memory, transform_ceiling);
            grid.channels = transform_ceiling;
        }
        grid.decimation = oversampled_decimation(grid.channels);

        if (auto ok = dsp::validate(grid); !ok) {
            return std::unexpected(with_context(ok.error(), "Engine::open_source"));
        }

        auto prototype = dsp::design_prototype(grid);
        if (!prototype) {
            return std::unexpected(with_context(prototype.error(), "Engine::open_source"));
        }
        auto twiddles = dsp::build_twiddles(grid.channels);
        if (!twiddles) {
            return std::unexpected(with_context(twiddles.error(), "Engine::open_source"));
        }

        // --- the block size --------------------------------------------------

        std::size_t block_samples = config_.block_samples;
        if (block_samples == 0) {
            block_samples = source->capabilities().preferred_block_samples;
        }
        if (block_samples == 0) {
            block_samples = 65'536;
        }
        if (block_samples < grid.decimation) {
            block_samples = grid.decimation;
        }

        // --- the ring ---------------------------------------------------------

        // One dispatch reads the filter's whole support plus a block, so a
        // ring smaller than that can never assemble a window. Four times that
        // is the floor rather than exactly that, so the writer is not
        // permanently pressed against the retirement floor.
        const auto span = static_cast<dsp::SampleIndex>(grid.prototype_length()) +
                          static_cast<dsp::SampleIndex>(block_samples);
        const dsp::SampleIndex floor_capacity = std::bit_ceil(span * 4);

        RingConfig ring_config;
        ring_config.rate = rate;
        ring_config.seconds_wanted = config_.ring_seconds;
        const auto from_seconds =
            static_cast<dsp::SampleIndex>(config_.ring_seconds * static_cast<double>(rate));
        if (from_seconds < floor_capacity) {
            ring_config.capacity_samples = floor_capacity;
        }

        auto ring = DeviceRing::create(context_, ring_config);
        if (!ring) {
            return std::unexpected(with_context(ring.error(), "Engine::open_source"));
        }
        ring_ = std::make_unique<DeviceRing>(std::move(*ring));

        // A ring clamped by the device can end up smaller than one dispatch
        // needs, and the honest answer then is a smaller block rather than a
        // failure: the whole sizing contract is that the engine shrinks to fit
        // and says so.
        const dsp::SampleIndex capacity = ring_->geometry().capacity_samples;
        if (static_cast<dsp::SampleIndex>(grid.prototype_length()) + block_samples > capacity / 2) {
            const auto room = capacity / 2;
            if (room <= grid.prototype_length()) {
                return fail(std::format(
                    "the device ring holds {} samples and the prototype alone spans {}. The grid "
                    "is too large for the memory this device will allocate",
                    capacity, grid.prototype_length()));
            }
            const std::size_t reduced =
                static_cast<std::size_t>(room - grid.prototype_length());
            if (!clamp_note_.empty()) {
                clamp_note_.append("; then ");
            }
            clamp_note_.append(std::format(
                "a {}-sample block plus {} of filter support would not fit half a {}-sample "
                "ring, so the block was reduced to {}",
                block_samples, grid.prototype_length(), capacity, reduced));
            block_samples = reduced;
        }

        // --- the graph ---------------------------------------------------------

        GraphConfig graph_config;
        graph_config.grid = grid;
        graph_config.source_rate = rate;
        graph_config.format = source->capabilities().native_format;
        graph_config.flow = source->capabilities().flow;
        graph_config.block_samples = block_samples;
        graph_config.audio_rate = config_.audio_rate;
        graph_config.spectrum_transform = config_.spectrum_transform;
        graph_config.spectrum_floor_db = config_.spectrum_floor_db;
        graph_config.spectrum_ceiling_db = config_.spectrum_ceiling_db;
        graph_config.passband_transform = config_.passband_transform;

        auto graph = Graph::create(context_, *ring_, *scheduler_, graph_config);
        if (!graph) {
            return std::unexpected(with_context(graph.error(), "Engine::open_source"));
        }
        graph_ = std::move(*graph);

        if (auto prepared = graph_->prepare(*prototype, *twiddles); !prepared) {
            graph_.reset();
            return std::unexpected(with_context(prepared.error(), "Engine::open_source"));
        }

        source_ = std::move(source);
        capabilities_ = source_->capabilities();
        block_samples_ = block_samples;
        prototype_ = std::move(*prototype);

        info_.ring = ring_->geometry();
        info_.grid = grid;
        info_.source_rate = rate;
        info_.spectrum = graph_->geometry().spectrum;
        info_.passband_transform = graph_->geometry().passband_transform;

        // HERE AND NOT IN close_source, SO THE NUMBER ONLY EVER NAMES A STREAM
        // THAT EXISTS. Incrementing on the way out would leave an engine with
        // no source carrying the epoch of a stream that has been torn down, and
        // a consumer comparing against it would find its stale correlation
        // still current. An engine between sources keeps the epoch of the last
        // one it served, which is the truthful answer to "which stream were
        // those indices in": that one, and it has ended.
        //
        // Past the last failure point in this function, so a refused open does
        // not consume an epoch. Nothing depends on them being consecutive, but
        // a gap would be a stream nobody could ever produce a sample for.
        ++info_.source_epoch;

        // The starting point, and set_source_center keeps it current from
        // here on.
        //
        // WHAT THIS COMMENT USED TO SAY: "read once here rather than
        // forwarded live, because this engine has no tune call: the centre
        // is fixed by the URI the source was opened with, so a snapshot is
        // the whole truth for the life of the engine. When a retunable
        // device arrives this becomes stale and has to follow the tune,
        // which is the change to make then and not now." The change is made:
        // the engine has a tune call, and this field follows it.
        info_.source_center = source_->center();

        // Truncating division, deliberately and only for display. See the note
        // at the top of this file.
        info_.channel_rate = rate / static_cast<dsp::SampleRate>(grid.decimation);
        info_.channel_spacing =
            static_cast<dsp::Hertz>(rate / static_cast<dsp::SampleRate>(grid.channels));

        if (!graph_->geometry().clamp_reason.empty()) {
            if (!clamp_note_.empty()) {
                clamp_note_.append("; then ");
            }
            clamp_note_.append(graph_->geometry().clamp_reason);
        }
        if (!clamp_note_.empty() && !info_.ring.clamped) {
            // The ring is the only field in the frozen EngineInfo that can
            // carry a sentence, so a grid or block clamp rides along in it
            // rather than going unreported. The alternative is a caller who
            // asked for 4096 channels, got 2048, and finds out when a
            // frequency lands in the wrong channel.
            info_.ring.clamped = true;
            info_.ring.clamp_reason = clamp_note_;
        } else if (!clamp_note_.empty()) {
            info_.ring.clamp_reason.append("; also ");
            info_.ring.clamp_reason.append(clamp_note_);
        }

        return {};
    }

    [[nodiscard]] Status close_source() override {
        // HELD ACROSS THE WHOLE TEARDOWN, WHICH IS WHAT MAKES has_source AN
        // ANSWER RATHER THAN A GLIMPSE. See the note on lifecycle_lock_: a host
        // that reads has_source() between run() returning and this function
        // finishing sees a source that is open and a stream that has ended, and
        // concludes the source ran out.
        const std::scoped_lock lifecycle(lifecycle_lock_);

        // Idempotent on an engine with nothing open. The caller wanted no
        // source and there is none, and answering with a refusal would make a
        // client that closes before every open have to know which state it was
        // in to interpret the answer.
        if (source_ == nullptr && graph_ == nullptr && ring_ == nullptr) {
            return {};
        }

        // THE WAIT IS THE WHOLE OF THE DIFFICULTY AND NONE OF IT IS OPTIONAL
        //
        // run() captures `Graph* graph = graph_.get()` into the source callback
        // and calls graph_->flush() after the stream ends. running_ is stored
        // false only after that flush has returned, which makes it the exact
        // marker for "run() is finished with the graph and the source". Tearing
        // either down before it is a use-after-free on the source's own thread,
        // which is the hardest kind to see: it reads as a driver fault or a
        // corrupt frame rather than as a lifetime bug here.
        if (running_.load(std::memory_order_acquire)) {
            if (auto asked = stop(); !asked) {
                return std::unexpected(with_context(asked.error(), "Engine::close_source"));
            }

            // BOUNDED, BECAUSE A SOURCE THAT WILL NOT STOP MUST NOT TAKE THE
            // CALLER WITH IT. This runs on an RPC loop thread in the deployment
            // that needs it, and a wait with no deadline there is a client that
            // hangs rather than one that is told what happened.
            //
            // Generous, because what it is waiting for is a device stop plus a
            // GPU flush: rtlsdr_cancel_async through libusb plus the
            // scheduler's outstanding submissions. Five seconds is far past
            // either on the hardware this has been measured on and short enough
            // that an operator does not conclude the program has died.
            const auto deadline = std::chrono::steady_clock::now() + kCloseStopTimeout;
            {
                std::unique_lock lock(run_lock_);
                while (running_.load(std::memory_order_acquire)) {
                    if (std::chrono::steady_clock::now() >= deadline) {
                        break;
                    }
                    run_signal_.wait_for(lock, kRunPollInterval);
                }
            }

            if (running_.load(std::memory_order_acquire)) {
                // Nothing is torn down. An engine still streaming is a working
                // engine, and a close that gave up half way would leave one
                // that is neither.
                return fail(std::format(
                    "Engine::close_source asked the stream to stop and it was still running "
                    "{} ms later, so nothing has been torn down and this engine is still "
                    "serving its old source. A source that will not stop is the fault to "
                    "chase; core/source/rtlsdr_source.cpp's cancel path is the usual one",
                    std::chrono::duration_cast<std::chrono::milliseconds>(kCloseStopTimeout)
                        .count()));
            }
        }

        // Before the graph, because a fan-out's entry names a receiver the
        // graph is about to destroy. See Engine::drop_audio_fanouts for why
        // attach_audio_sink's on-demand pruning does not cover this.
        drop_audio_fanouts();

        // The destructor's order, for the destructor's reasons: the source's
        // thread calls into the graph, the graph's completion thread calls into
        // the receivers' sinks, and the ring outlives both. ring_ is reset here
        // and not in the destructor because there the member order does it.
        source_.reset();
        graph_.reset();
        ring_.reset();

        capabilities_ = source::SourceCapabilities{};
        prototype_ = dsp::PrototypeFilter{};
        block_samples_ = 0;
        clamp_note_.clear();

        // The device survives and everything describing a source does not.
        // Assigning a fresh EngineInfo and putting the device back is one line
        // per field that stays rather than one per field that goes, which is
        // the direction that does not rot: a field added to EngineInfo for a
        // future source property is cleared here by default instead of being
        // left behind by an omission nobody notices.
        //
        // source_epoch is carried over for the reason open_source gives.
        const gpu::DeviceInfo device = info_.device;
        const std::uint64_t epoch = info_.source_epoch;
        info_ = EngineInfo{};
        info_.device = device;
        info_.source_epoch = epoch;

        // AND THE GRID IS ZEROED ON TOP OF THAT, BECAUSE ITS DEFAULT IS NOT
        // EMPTY. dsp::GridParams default-constructs to the design's canonical
        // values, M=64 D=32 and 17 taps per branch, which is right for a
        // caller who did not name a grid and wrong for this: a fresh
        // EngineInfo would report a 64-channel grid on an engine with no
        // source, and a client reading it would believe a measurement that had
        // never been taken. source_rate of zero is the marker for no source,
        // but nothing stops a client reading grid.channels without checking it,
        // and a zero there is unmistakable where a 64 is not.
        info_.grid.channels = 0;
        info_.grid.decimation = 0;
        info_.grid.taps_per_branch = 0;

        // Not reset, and each for its own reason.
        //
        // next_id_ keeps counting. Receiver ids are monotonic for the life of
        // the engine so that a stale id is detectable; restarting them here
        // would make a client holding a receiver from the previous source
        // address a live one on this one.
        //
        // stream_start_ns_ and stream_stop_ns_ are left to run(), which writes
        // both at the top of every run. Clearing them here would report a
        // realtime factor of zero, which SourcePacing defines as "not
        // measured", and that is already what an engine between sources is.
        return {};
    }

    [[nodiscard]] bool has_source() const override {
        // UNDER THE LOCK, AND THAT IS THE WHOLE POINT OF THE FUNCTION. A plain
        // read of the pointer is one instruction and would be wrong exactly
        // when it matters: a host loops on this to tell "a client closed the
        // source" from "the source ran out", and the two are indistinguishable
        // in the window between run() returning and close_source finishing its
        // teardown. Taking the lock makes the answer a completed transition.
        //
        // Measured rather than reasoned about. revenant-engine exited the first
        // time a client changed radios through the picker, reporting the
        // cancellation message from the stream it had just been asked to end.
        const std::scoped_lock lifecycle(lifecycle_lock_);
        return source_ != nullptr;
    }

    [[nodiscard]] const source::SourceCapabilities& source_capabilities() const override {
        return capabilities_;
    }

    [[nodiscard]] const EngineInfo& info() const override { return info_; }

    [[nodiscard]] SourceTuning source_tuning() const override {
        SourceTuning out;
        for (const source::TuneRange& range : capabilities_.tune_ranges) {
            // A backend that could not describe its tuner leaves the list
            // empty rather than guessing, which reads here as a source that
            // cannot be retuned. That is the right answer: librtlsdr has no
            // driver for a tuner it did not recognise, so nothing on such a
            // dongle can be tuned at all.
            if (range.high < range.low) {
                continue;
            }
            if (!out.can_retune) {
                out.can_retune = true;
                out.low = range.low;
                out.high = range.high;
                continue;
            }
            out.low = std::min(out.low, range.low);
            out.high = std::max(out.high, range.high);
        }
        return out;
    }

    [[nodiscard]] Expected<dsp::Hertz> set_source_center(dsp::Hertz center) override {
        if (source_ == nullptr) {
            return fail("Engine::set_source_center before a source is open: there is no front "
                        "end to point anywhere");
        }

        // The source's own refusal, not one composed here. A file says a
        // recording's centre is a property of the samples already on disk
        // and tells the caller to reopen the URI; a synthetic scene says to
        // move the emitters instead; a dongle says which ranges it reaches.
        // Three different things to do about it, and a message of this
        // layer's own would replace all three with a category.
        auto landed = source_->tune(center);
        if (!landed) {
            return std::unexpected(with_context(landed.error(), "Engine::set_source_center"));
        }

        // The one piece of engine state a retune moves. Everything else in
        // the chain works in the source's baseband frame, which has not
        // changed; see the note on Engine::set_source_center.
        info_.source_center = *landed;

        // And the marker on every receiver's stream, so a consumer that
        // accumulates state about the transmitter it is hearing has the same
        // boundary a per-receiver retune gives it. The argument for
        // re-queueing rather than inventing a second epoch is on the
        // declaration.
        //
        // A failure here is not the caller's business and is deliberately
        // discarded. The device has already moved: reporting a failed epoch
        // bump as a failed retune would tell a client the front end is where
        // it was, which is the one thing that is certainly untrue. A
        // receiver that went away between the tune and this pass is the
        // ordinary case and is exactly what fails.
        if (graph_ != nullptr) {
            for (const VrxId id : graph_->vrx_ids()) {
                auto status = graph_->vrx_status(id);
                if (!status) {
                    continue;
                }
                auto placement = place(info_.grid, info_.source_rate, status->params);
                if (!placement) {
                    continue;
                }
                static_cast<void>(graph_->set_vrx_params(id, status->params, *placement));
            }
        }

        return *landed;
    }

    [[nodiscard]] Expected<double> set_source_gain(std::string_view stage, double db) override {
        if (source_ == nullptr) {
            return fail("Engine::set_source_gain before a source is open: there is no front end "
                        "to set a gain on");
        }

        // The source's own refusal for the same reason set_source_center takes
        // the source's: a file says its samples were digitised at whatever gain
        // the recorder used, and a dongle names the stage it does have. A
        // message composed here would replace both with a category.
        //
        // Nothing in EngineInfo moves. Gain changes what the samples look like
        // and not what any index or frequency means, so unlike a retune there
        // is no epoch to bump and no receiver to re-place: a consumer
        // accumulating state about a transmitter is still hearing the same
        // transmitter, louder or quieter.
        auto landed = source_->set_gain(stage, db);
        if (!landed) {
            return std::unexpected(with_context(landed.error(), "Engine::set_source_gain"));
        }
        return *landed;
    }

    [[nodiscard]] Status set_source_gain_auto(std::string_view stage, bool on) override {
        if (source_ == nullptr) {
            return fail("Engine::set_source_gain_auto before a source is open: there is no front "
                        "end to hand to an AGC");
        }
        auto applied = source_->set_gain_auto(stage, on);
        if (!applied) {
            return std::unexpected(with_context(applied.error(), "Engine::set_source_gain_auto"));
        }
        return {};
    }

    [[nodiscard]] SourcePacing source_pacing() const override {
        SourcePacing out;
        out.paced_by = config_.pace;
        out.demand = capabilities_.flow == source::FlowControl::Demand;
        if (source_ == nullptr) {
            return out;
        }

        out.samples_delivered = source_->stats().samples_delivered;

        const std::int64_t started = stream_start_ns_.load(std::memory_order_acquire);
        if (started == 0) {
            // Nothing has been measured, which SourcePacing::realtime_factor
            // documents as a third state rather than as a stalled source.
            return out;
        }

        // Frozen at the end of the run rather than left to decay against a
        // clock that keeps going. A finished replay's factor is what it
        // achieved, and letting it fall towards zero afterwards would make
        // a completed file look like a source that died.
        const std::int64_t ended = stream_stop_ns_.load(std::memory_order_acquire);
        const std::int64_t now =
            ended != 0 ? ended
                       : std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        if (now <= started) {
            return out;
        }

        out.elapsed_seconds = static_cast<double>(now - started) / 1e9;
        if (info_.source_rate <= 0 || out.elapsed_seconds <= 0.0) {
            return out;
        }
        const double capture_seconds = static_cast<double>(out.samples_delivered) /
                                       static_cast<double>(info_.source_rate);
        out.realtime_factor = capture_seconds / out.elapsed_seconds;
        return out;
    }

    [[nodiscard]] Expected<VrxId> add_vrx(const VrxParams& params) override {
        if (graph_ == nullptr) {
            return fail("Engine::add_vrx before a source is open: a receiver is placed on the "
                        "grid, and there is no grid until the source's rate is known");
        }

        // Pure: the grid, the rate and the request, and nothing else. This is
        // the whole of what adding a receiver computes against the coarse
        // stage, and it reads it rather than changing it.
        auto placement = place(info_.grid, info_.source_rate, params);
        if (!placement) {
            return std::unexpected(with_context(placement.error(), "Engine::add_vrx"));
        }

        const VrxId id{next_id_.fetch_add(1, std::memory_order_relaxed)};
        auto added = graph_->add_vrx(id, params, *placement);
        if (!added) {
            return std::unexpected(added.error());
        }
        return id;
    }

    [[nodiscard]] Status remove_vrx(VrxId id) override {
        if (graph_ == nullptr) {
            return fail("Engine::remove_vrx before a source is open");
        }
        return graph_->remove_vrx(id);
    }

    [[nodiscard]] Status set_vrx_params(VrxId id, const VrxParams& params) override {
        if (graph_ == nullptr) {
            return fail("Engine::set_vrx_params before a source is open");
        }
        auto placement = place(info_.grid, info_.source_rate, params);
        if (!placement) {
            return std::unexpected(with_context(placement.error(), "Engine::set_vrx_params"));
        }
        return graph_->set_vrx_params(id, params, *placement);
    }

    [[nodiscard]] Expected<VrxStatus> vrx_status(VrxId id) const override {
        if (graph_ == nullptr) {
            return fail("Engine::vrx_status before a source is open");
        }
        return graph_->vrx_status(id);
    }

    [[nodiscard]] std::vector<VrxId> vrx_ids() const override {
        return graph_ == nullptr ? std::vector<VrxId>{} : graph_->vrx_ids();
    }

    [[nodiscard]] Status set_audio_sink(VrxId id, AudioSink sink) override {
        if (graph_ == nullptr) {
            return fail("Engine::set_audio_sink before a source is open");
        }
        return graph_->set_audio_sink(id, std::move(sink));
    }

    [[nodiscard]] Status set_spectrum_sink(SpectrumSink sink) override {
        if (graph_ == nullptr) {
            return fail("Engine::set_spectrum_sink before a source is open: the spectrum stage "
                        "is sized against the grid, and there is no grid until the source's "
                        "rate is known");
        }
        if (config_.spectrum_transform == 0) {
            return fail("this engine was created with EngineConfig::spectrum_transform at zero, "
                        "so no spectrum stage was built. Set it before Engine::create, because "
                        "the stage belongs to the coarse chain and adding one to a running "
                        "graph would mean rebuilding that");
        }
        return graph_->set_spectrum_sink(std::move(sink));
    }

    [[nodiscard]] Status set_passband_sink(VrxId id, PassbandSink sink) override {
        if (graph_ == nullptr) {
            return fail("Engine::set_passband_sink before a source is open: a passband's width "
                        "is the receiver's demodulation rate, and there is no receiver until "
                        "the source's rate is known");
        }
        if (config_.passband_transform == 0) {
            return fail("this engine was created with EngineConfig::passband_transform at "
                        "zero, so no passband stage was built. Set it before Engine::create: "
                        "it sizes every receiver's fine ring, and a ring cannot be grown while "
                        "a command buffer names it");
        }
        return graph_->set_passband_sink(id, std::move(sink));
    }

    [[nodiscard]] Status run() override {
        if (source_ == nullptr || graph_ == nullptr) {
            // A HOST THAT LOOPS HAS TO TELL THIS FROM A FAULT, AND has_source
            // IS HOW. Since close_source landed this refusal has two causes: a
            // host that called run() before opening anything, which is a bug,
            // and a source closed between the host's check and this call, which
            // is an operator changing radios and is not. Neither this call nor
            // the other can distinguish them, because by the time either is
            // asked the state is the same. The host asks has_source() after a
            // failed run: false means it raced a close and should go back to
            // waiting. tools/engined/main.cpp is the worked example.
            return fail("Engine::run before a source is open");
        }
        if (running_.load(std::memory_order_acquire)) {
            return fail("this engine is already running");
        }

        // THE START SECTION IS UNDER THE LIFECYCLE LOCK AND THE WAIT IS NOT.
        //
        // Without it a run() that arrived while close_source was tearing the
        // graph down would read source_ and graph_ as this function's opening
        // lines just did, find them non-null, and start a stream on objects
        // about to be destroyed. The guards above are re-checked inside, which
        // is not belt and braces: they were true a moment ago and the lock is
        // the first thing that makes them still true.
        //
        // It is released before the wait below, so close_source can take it
        // while a stream is running: that is the ordinary case, and a lock held
        // for the length of a stream would make every close wait for the source
        // to end on its own, which is the opposite of what it is for.
        {
            const std::scoped_lock lifecycle(lifecycle_lock_);
            if (source_ == nullptr || graph_ == nullptr) {
                return fail("Engine::run before a source is open");
            }
            if (running_.load(std::memory_order_acquire)) {
                return fail("this engine is already running");
            }
            stop_requested_.store(false, std::memory_order_release);
            running_.store(true, std::memory_order_release);
        }

        // Before the source starts, so the elapsed time includes whatever
        // the first block cost to produce. Measuring from the first block
        // instead would hide exactly the startup a slow source spends.
        stream_stop_ns_.store(0, std::memory_order_release);
        stream_start_ns_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count(),
                               std::memory_order_release);

        source::StreamOptions options;
        options.block_samples = block_samples_;

        // Unthrottled by default, and faster than realtime is still not a
        // mode: it is what happens when nothing is holding a stopwatch, and
        // the only thing setting the pace is then the graph's blocking
        // reserve. A caller monitoring a capture on a loudspeaker asks for a
        // stopwatch by setting EngineConfig::pace, which the source honours
        // and the engine does nothing else with.
        options.pace = config_.pace;

        Graph* graph = graph_.get();
        auto started = source_->start(options, [graph](const source::SourceBlock& block) {
            return graph->on_block(block);
        });
        if (!started) {
            running_.store(false, std::memory_order_release);
            return std::unexpected(with_context(started.error(), "Engine::run"));
        }

        {
            std::unique_lock lock(run_lock_);
            while (!stop_requested_.load(std::memory_order_acquire) && source_->running()) {
                run_signal_.wait_for(lock, kRunPollInterval);
            }
        }

        // stop() on a source that has already finished is the join plus
        // whatever error ended the stream, which is what this wants either
        // way.
        Status ended = source_->stop();
        Status flushed = graph_->flush();
        stream_stop_ns_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count(),
                              std::memory_order_release);
        running_.store(false, std::memory_order_release);

        // AFTER the store and not before it, because close_source is parked on
        // this waiting for exactly that store: running_ going false is what says
        // run() has finished with the graph and the source, so a notify ahead of
        // it would wake a waiter that finds nothing changed.
        //
        // Not under run_lock_. A missed wakeup here is a close_source that waits
        // out one more kRunPollInterval rather than one that hangs, because that
        // loop re-tests the atomic on a timeout. Taking the lock would mean
        // taking it on the path that ends every stream, for two milliseconds of
        // latency in the one case where somebody is waiting.
        run_signal_.notify_all();

        if (!ended) {
            return ended;
        }
        if (!flushed) {
            return flushed;
        }
        if (scheduler_ != nullptr && scheduler_->failed()) {
            return std::unexpected(scheduler_->error());
        }
        return {};
    }

    [[nodiscard]] Status stop() override {
        stop_requested_.store(true, std::memory_order_release);
        if (graph_ != nullptr) {
            graph_->cancel();
        }
        run_signal_.notify_all();
        return {};
    }

    [[nodiscard]] bool running() const override {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] source::SourceStats source_stats() const override {
        source::SourceStats out;
        if (source_ != nullptr) {
            out = source_->stats();
        }
        if (graph_ == nullptr) {
            return out;
        }

        // The ring's losses are folded in on purpose.
        //
        // The frozen Engine has exactly one place to report a counted loss,
        // and a sample the graph could not place is gone in precisely the way
        // a sample the device dropped is gone. Reporting only the source's
        // view would produce a run that looks continuous and is not, with
        // nothing downstream able to tell, which is the failure this counter
        // exists to prevent. The two are distinguishable: a Demand source
        // reports zero of its own, by construction, so anything here came
        // from the graph.
        const GraphStats graph_stats = graph_->stats();
        out.overrun_events += graph_stats.overrun_events;
        out.samples_lost += graph_stats.samples_dropped;
        return out;
    }

    [[nodiscard]] GraphConditions graph_conditions() const override {
        GraphConditions out;
        if (graph_ == nullptr) {
            // Zero on an engine between sources, which is the truth: no graph
            // has refused anything and no thread has waited for a slot. The
            // counters of the graph that just went are not carried forward,
            // because they were about a stream that has ended and a client
            // comparing them against the next one would read a step as
            // activity.
            return out;
        }
        const GraphStats stats = graph_->stats();
        out.vrx_retune_refusals = stats.vrx_retune_refusals;
        out.frame_stalls = stats.frame_stalls;
        return out;
    }

    [[nodiscard]] const Graph* graph() const { return graph_.get(); }
    [[nodiscard]] const Scheduler* scheduler() const { return scheduler_.get(); }
    [[nodiscard]] const dsp::PrototypeFilter& prototype() const { return prototype_; }

private:
    EngineConfig config_{};
    EngineInfo info_{};
    std::string clamp_note_;

    gpu::Context context_;
    std::unique_ptr<Scheduler> scheduler_;

    // Behind a unique_ptr because DeviceRing's State is an incomplete type in
    // the frozen header and its defaulted default constructor cannot be
    // instantiated outside device_ring.cpp. The out-of-line move constructor
    // and destructor are declared, so holding one by pointer works where
    // holding one by value does not. Declared before graph_, so the graph is
    // destroyed first: the graph's frames reference the ring's buffer.
    std::unique_ptr<DeviceRing> ring_;
    std::unique_ptr<Graph> graph_;
    std::unique_ptr<source::Source> source_;
    source::SourceCapabilities capabilities_{};
    dsp::PrototypeFilter prototype_{};
    std::size_t block_samples_ = 0;

    // Ids start at one so that a default-constructed VrxId is never a live
    // receiver; see VrxId::valid().
    std::atomic<std::uint32_t> next_id_{1};

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    // steady_clock nanoseconds, so the pair can be read from any thread
    // without a lock. Zero in stream_start_ns_ means the stream has not
    // begun; zero in stream_stop_ns_ means it has not ended, and a non-zero
    // one freezes the realtime factor at what the run achieved.
    //
    // A wall clock is deliberately not used and neither is the source's own
    // timestamp. The question is how long the host took, which is a
    // steady_clock question, and docs/conventions.md keeps the DSP path off
    // any clock at all.
    std::atomic<std::int64_t> stream_start_ns_{0};
    std::atomic<std::int64_t> stream_stop_ns_{0};

    // Control plane only. run() parks on it and stop() wakes it; the sample
    // path never touches either.
    mutable std::mutex run_lock_;
    std::condition_variable run_signal_;

    // Serialises open_source, close_source and the START of run(), and makes
    // has_source() an answer rather than a glimpse.
    //
    // WHAT IT IS NOT: a lock on the sample path. It is taken three times per
    // source and never per block, and run() releases it before parking for the
    // length of the stream, so a close does not wait for a source to end.
    //
    // WHAT IT IS FOR. A source can now be closed while a stream is running,
    // which puts two threads on the same objects: the host's, inside run(),
    // and whoever called close_source, usually an RPC loop. Two things go
    // wrong without this and both did.
    //
    // A run() that arrived mid-teardown found source_ and graph_ non-null and
    // started a stream on objects about to be destroyed.
    //
    // A host that read has_source() in the window between run() returning and
    // close_source finishing saw a source that was open and a stream that had
    // ended, which is indistinguishable from a source that ran out.
    // revenant-engine exited the first time a client changed radios in the
    // picker, reporting the cancellation message from the stream it had just
    // been asked to end.
    //
    // mutable because has_source() is const and the lock is what the
    // constness is about: the answer is only meaningful when no transition is
    // half-applied.
    mutable std::mutex lifecycle_lock_;
};

}  // namespace

std::uint32_t default_channel_count(dsp::SampleRate rate) {
    // The arithmetic moved to engine::channel_count_for so that place() can
    // name the count that would have carried a receiver it had to refuse.
    // Its own refusal is about one mode's channel plan rather than about
    // kWidestReceiverHz, so it needs the width as an argument.
    return channel_count_for(rate, kWidestReceiverHz);
}

Expected<std::unique_ptr<Engine>> Engine::create(const EngineConfig& config) {
    // The graph ships the raw tap and asks a factory for every other
    // demodulator. This is the one place that knows a graph is about to
    // exist, so it is where the demodulator package gets registered; see the
    // note in core/engine/vrx_stage.h on why this is not a static
    // initialiser. Once per process, because installing is a lock and a
    // std::function assignment and there is no reason to do it per engine.
    static std::once_flag stages_installed;
    std::call_once(stages_installed, install_default_vrx_stages);

    auto engine = std::make_unique<EngineImpl>();
    if (auto configured = engine->configure(config); !configured) {
        return std::unexpected(configured.error());
    }
    return std::unique_ptr<Engine>(std::move(engine));
}

}  // namespace revenant::engine
