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
#include "core/engine/open_built_source.h"
#include "core/engine/pacing_window.h"
#include "core/engine/ring_consumer.h"
#include "core/engine/scheduler.h"
#include "core/engine/vrx_stage.h"
#include "core/source/calibration.h"
#include "core/source/corrected_source.h"
#include "core/source/frequency_correction.h"

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

// Where probe receivers' ids start. add_vrx counts up from one and refuses to
// reach this, so the two spaces cannot meet and a probe's id is recognisable
// from the number alone: every public receiver method refuses one.
constexpr std::uint32_t kFirstProbeId = 0x8000'0000U;

[[nodiscard]] bool is_probe_id(VrxId id) { return id.value >= kFirstProbeId; }

// steady_clock in nanoseconds, the one clock the pacing measurement reads.
[[nodiscard]] std::int64_t steady_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] Status refuse_probe_id(VrxId id, const char* where) {
    return fail(std::format(
        "{}: {} is one of the engine's own probe receivers, which core/engine/probe.h keeps "
        "out of vrx_ids and off every public receiver method",
        where, id.value));
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
        //
        // The probe pool goes before the graph, because its worker calls the
        // graph's control plane and its destructor removes its receivers.
        source_.reset();
        probes_.reset();
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
        pace_.store(config.pace, std::memory_order_release);

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
        return adopt_source_locked(std::move(*opened), uri);
    }

    // open_built_source's way in. See core/engine/open_built_source.h.
    [[nodiscard]] Status adopt_source(std::unique_ptr<source::Source> source) {
        const std::scoped_lock lifecycle(lifecycle_lock_);
        if (source == nullptr) {
            return fail("open_built_source with no source");
        }
        if (source_ != nullptr) {
            return fail("this engine already has a source open. Close it first");
        }
        clamp_note_.clear();
        const std::string name = source->capabilities().uri;
        return adopt_source_locked(std::move(source), name);
    }

    // Everything open_source does once a source exists. The caller holds the
    // lifecycle lock and has checked that no source is open.
    [[nodiscard]] Status adopt_source_locked(std::unique_ptr<source::Source> raw_source,
                                             std::string_view uri) {
        // --- the calibration, before anything reads a frequency ---------------
        //
        // Every source is wrapped, a calibrated one or not, so a correction
        // measured later in the session has somewhere to go. The wrapper at
        // zero is exact arithmetic that returns its input.
        const StoredCalibration stored = stored_calibration_for(raw_source->capabilities());
        auto wrapped = std::make_unique<source::CorrectedSource>(
            std::move(raw_source),
            stored.correction_applied ? stored.settings.correction_ppb : 0);
        source::CorrectedSource* corrected = wrapped.get();
        std::unique_ptr<source::Source> source = std::move(wrapped);

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
        //
        // A caller that marked its count as a default gives it up to a source
        // that states the resolution it needs. See
        // EngineConfig::channels_yield_to_source.
        const bool channels_chosen_here =
            grid.channels == 0 || (config_.channels_yield_to_source &&
                                   source->capabilities().resolution.stated());
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

        // Built with the graph and not on first use, so a probe refused for
        // want of a pool is refused at submit rather than half way through a
        // run, and so the worker exists before anything is submitted to it.
        if (config_.probe_receivers > 0) {
            ProbePoolConfig probe_config;
            probe_config.size = std::min(config_.probe_receivers, kMaxProbeReceivers);
            probe_config.grid = grid;
            probe_config.source_rate = rate;
            probe_config.channel_rate = graph_->geometry().channel_rate;
            probe_config.first_id = kFirstProbeId;
            probe_config.cpu_budget = config_.probe_cpu_budget;
            auto pool = ProbePool::create(*graph_, probe_config);
            if (!pool) {
                graph_.reset();
                return std::unexpected(with_context(pool.error(), "Engine::open_source"));
            }
            probes_ = std::move(*pool);
        }

        graph_->set_front_end_correction(stored.settings.dc_removal,
                                         stored.settings.iq_correction);

        source_ = std::move(source);
        capabilities_ = source_->capabilities();
        block_samples_ = block_samples;
        prototype_ = std::move(*prototype);

        corrected_ = corrected;
        calibration_key_ = stored.key;
        calibration_ = stored.settings;
        calibration_persisted_ = stored.persisted;
        calibration_file_unreadable_ = stored.file_unreadable;
        calibration_note_ = stored.note;

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

        // The source's own pace when it has one, a file opened with pace=,
        // and the configured one when it has none.
        pace_.store(source_->own_pace().value_or(config_.pace), std::memory_order_release);

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
        probes_.reset();
        graph_.reset();
        ring_.reset();

        capabilities_ = source::SourceCapabilities{};
        prototype_ = dsp::PrototypeFilter{};
        block_samples_ = 0;
        clamp_note_.clear();

        // A pace set on the closed source was that source's.
        pace_.store(config_.pace, std::memory_order_release);

        corrected_ = nullptr;
        calibration_key_.clear();
        calibration_ = source::DeviceCalibration{};
        calibration_persisted_ = false;
        calibration_file_unreadable_ = false;
        calibration_note_.clear();
        calibration_store_fault_.clear();

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

    [[nodiscard]] Expected<SourceRetune> set_source_center(dsp::Hertz center) override {
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
        // Read BEFORE the tune, because it is what every receiver's absolute
        // frequency is measured against and the line below overwrites it.
        const dsp::Hertz was = info_.source_center;

        auto landed = excusing_pause([&] { return source_->tune(center); });
        if (!landed) {
            return std::unexpected(with_context(landed.error(), "Engine::set_source_center"));
        }

        // The one piece of engine state a retune moves.
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
        // A RECEIVER STAYS ON THE FREQUENCY IT WAS TUNED TO, AND IS REMOVED
        // WHEN THE FRONT END CAN NO LONGER REACH IT.
        //
        // VrxParams::center is a BASEBAND offset, which is the only frame the
        // grid has, so leaving it alone across a retune carried every receiver
        // along with the span: one at +100 kHz was hearing 98.2 MHz at a centre
        // of 98.1 and heard 435.1 MHz at a centre of 435. Reported by an
        // operator on 2026-09-21, who retuned from broadcast FM and found the
        // receiver still making noise at a frequency they had not chosen, and
        // watched its highlight ride along at a fixed position in the span.
        // That is not what a VFO does.
        //
        // So the offset is recomputed to hold the absolute frequency: what was
        // `was + offset` before is the same number after, which means the new
        // offset is the old one less however far the front end moved.
        //
        // OUTSIDE THE SPAN IS REMOVED, NOT CLAMPED AND NOT PARKED. The
        // operator's own words for what they wanted to see were "disappearing
        // VRX", and the test is the receiver's CENTRE rather than its passband:
        // those differ by up to half a receiver's width at the edges, and the
        // centre is the frequency somebody typed or clicked, so it is the one
        // they would say the receiver "is on". A receiver half off the edge
        // keeps running and sounds wrong, which the passband highlight and
        // receiverFitText both already report.
        //
        // A failure to re-place is treated the same as being out of range,
        // because it is: place() refuses a receiver the new grid cannot carry,
        // and leaving it registered on a placement that no longer describes it
        // would be a receiver producing audio from the wrong channel. The same
        // goes for a re-place the graph refuses because the new place needs a
        // different filter; the note on that branch below says how it is
        // reached.
        SourceRetune out;
        out.center = *landed;

        // Probes are not carried along or removed: every one still collecting
        // is ended, because the baseband it has so far came from the old
        // front-end centre and the rest would come from the new one. The
        // detections they were placed on are the old centre's too, and
        // whoever submitted them drops those tracks when this returns.
        if (probes_ != nullptr) {
            probes_->cancel_all();
        }

        if (graph_ != nullptr) {
            const dsp::Hertz moved = *landed - was;
            const dsp::Hertz half_span = static_cast<dsp::Hertz>(info_.source_rate / 2);

            for (const VrxId id : graph_->vrx_ids()) {
                auto status = graph_->vrx_status(id);
                if (!status) {
                    continue;
                }

                VrxParams repinned = status->params;
                repinned.center = status->params.center - moved;

                // Strictly outside. A receiver exactly on the Nyquist edge is
                // reachable, and a half-open test would drop one that had been
                // sitting there happily before anybody retuned.
                const bool reachable =
                    repinned.center >= -half_span && repinned.center <= half_span;

                const dsp::Hertz frequency = was + status->params.center;
                std::string reason;
                RetuneCause cause = RetuneCause::OutsideSpan;
                if (!reachable) {
                    reason = std::format(
                        "the front end was retuned to {} Hz, which leaves this receiver's centre "
                        "at {} Hz outside the span, so the engine removed it",
                        *landed, frequency);
                } else if (auto placement = place(info_.grid, info_.source_rate, repinned);
                           !placement) {
                    cause = RetuneCause::Unplaceable;
                    reason = std::format(
                        "the front end was retuned to {} Hz and the receiver at {} Hz could not "
                        "be placed on the grid from there, so the engine removed it: {}",
                        *landed, frequency, placement.error().message);
                } else if (auto held = graph_->set_vrx_params(id, repinned, *placement); !held) {
                    // THE REFUSAL THAT USED TO BE DISCARDED. Re-queued even
                    // when the offset did not change, which is the epoch bump
                    // the declaration argues for, and the graph refuses a
                    // re-queue that changes the receiver's shape. That is
                    // reachable for every mode place() narrows rather than
                    // refuses: AM, DSB, the sidebands and CW get
                    // channel_rate - 2|residual| of passband, a retune by
                    // anything but a multiple of the channel spacing moves the
                    // residual, and the fine filter is designed from the
                    // narrowed width. Measured on 256 channels over 2 MS/s
                    // (tests/engine/test_engine_retune.cpp): a 10 kHz AM
                    // receiver on a channel centre, retuned by 3500 Hz, is
                    // granted -4312 to 4312 Hz from its new place and its fine
                    // filter goes from 17 taps to 20. Five of the nine
                    // receivers in that case were refused this way; DSB, USB,
                    // CW and an AM receiver whose grant did not move kept
                    // their shape and came along.
                    //
                    // Discarding that left the receiver registered on its OLD
                    // offset, which after the retune is `moved` hertz from
                    // where the operator put it, with nothing in the answer
                    // saying so. That is the ride-along the rebase above
                    // exists to end.
                    //
                    // REMOVED, NOT REBUILT, because the graph cannot rebuild
                    // a receiver under the same id: describe_shape_change says
                    // a new shape is a remove and an add, the audio and
                    // passband sinks live on the recording thread's slot, and
                    // a new slot starts its audio index and display sequence
                    // from zero while the completion thread may still be
                    // delivering the old one's frames. A client that wants the
                    // receiver back adds one at the frequency in this entry.
                    // The reason carries the graph's own sentence, which ends
                    // "a remove and an add", the phrase ui/models/
                    // receiver_link.cpp matches on a refused set_vrx_params.
                    // The wire carries the cause and this sentence in each
                    // RetuneRemoval, so a client can offer that add without
                    // matching on the phrase.
                    cause = RetuneCause::ShapeChanged;
                    reason = std::format(
                        "the front end was retuned to {} Hz, which puts the receiver at {} Hz in "
                        "a different place in its channel and so needs a different filter: {}. "
                        "The engine removed it rather than leave it {} Hz from where it was put",
                        *landed, frequency, held.error().message, moved);
                }

                if (reason.empty()) {
                    continue;
                }

                // Reported in the answer rather than as a failure, for the
                // reason the block above gives: the device has already moved.
                // Only a removal that took is reported. A refusal means the
                // graph no longer held the receiver, so whoever removed it has
                // already accounted for it.
                if (graph_->remove_vrx(id)) {
                    out.removed.push_back(RetuneRemoval{.id = id,
                                                        .cause = cause,
                                                        .frequency = frequency,
                                                        .reason = std::move(reason)});
                }
            }
        }

        return out;
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
        auto landed = excusing_pause([&] { return source_->set_gain(stage, db); });
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
        auto applied = excusing_pause([&] { return source_->set_gain_auto(stage, on); });
        if (!applied) {
            return std::unexpected(with_context(applied.error(), "Engine::set_source_gain_auto"));
        }
        return {};
    }

    [[nodiscard]] Expected<CalibrationState> calibration() const override {
        const std::scoped_lock lifecycle(lifecycle_lock_);
        return calibration_state_locked();
    }

    [[nodiscard]] Expected<CalibrationState> set_calibration(
        const source::DeviceCalibration& settings) override {
        const std::scoped_lock lifecycle(lifecycle_lock_);
        if (source_ == nullptr || corrected_ == nullptr || graph_ == nullptr) {
            return fail("Engine::set_calibration before a source is open: a calibration belongs "
                        "to a device, and there is none");
        }
        if (!source::correction_in_range(settings.correction_ppb)) {
            return fail(std::format(
                "Engine::set_calibration: a correction of {} ppb is past the {} ppb either way "
                "that a crystal error can be. A figure that large is a different radio, or a "
                "carrier that was not the one it was taken for.",
                settings.correction_ppb, source::kMaxCorrectionPpb));
        }

        // The labels move and the device does not. See the declaration.
        if (!capabilities_.device_corrects_frequency) {
            if (auto set = corrected_->set_correction_ppb(settings.correction_ppb); !set) {
                return std::unexpected(with_context(set.error(), "Engine::set_calibration"));
            }
            info_.source_center = source_->center();
        }
        graph_->set_front_end_correction(settings.dc_removal, settings.iq_correction);
        calibration_ = settings;

        store_calibration_locked();
        return calibration_state_locked();
    }

    [[nodiscard]] SourcePacing source_pacing() const override {
        SourcePacing out;
        out.paced_by = pace_.load(std::memory_order_acquire);
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
        const std::int64_t now = ended != 0 ? ended : steady_now_ns();
        if (now <= started) {
            return out;
        }

        out.elapsed_seconds = static_cast<double>(now - started) / 1e9;
        if (info_.source_rate <= 0 || out.elapsed_seconds <= 0.0) {
            return out;
        }

        // Over the last window rather than the whole run, with the samples a
        // control pause cost the source counted in. core/engine/pacing_window.h
        // has why: a lifetime mean charged every retune's 330 ms to the
        // source for the rest of the run.
        const std::scoped_lock pacing(pacing_lock_);
        const PacingReading measured = pacing_window_.read(
            PacingSample{.at_ns = now, .delivered = out.samples_delivered}, info_.source_rate);
        out.realtime_factor = measured.factor;
        out.window_seconds = measured.window_seconds;
        return out;
    }

    [[nodiscard]] Expected<double> set_source_pace(double pace) override {
        const std::scoped_lock lifecycle(lifecycle_lock_);
        if (source_ == nullptr) {
            return fail("Engine::set_source_pace before a source is open: a pace belongs to the "
                        "source it times");
        }
        if (!std::isfinite(pace) || pace < 0.0) {
            return fail(std::format(
                "pace must be zero for unthrottled or a positive multiple of realtime, got {}",
                pace));
        }
        if (capabilities_.flow != source::FlowControl::Demand) {
            return fail(std::format(
                "'{}' runs on its own device's clock, so there is no pace to set: a live radio "
                "delivers at the rate it samples at",
                capabilities_.display_name.empty() ? capabilities_.uri
                                                   : capabilities_.display_name));
        }
        if (auto set = source_->set_pace(pace); !set) {
            return std::unexpected(with_context(set.error(), "Engine::set_source_pace"));
        }
        pace_.store(pace, std::memory_order_release);
        return pace;
    }

    // A control call on the source, with whatever the source counted as lost
    // while it ran excused from the pacing factor, and the factor held where
    // it was while the call runs. An RTL-SDR stops its transfers around every
    // control transfer and counts the samples the device made meanwhile as
    // lost; those are the engine's own doing and not the source falling
    // behind. SourceStats::samples_lost still carries them.
    //
    // Counted whether the call succeeded or not, because the RTL-SDR pauses
    // either way. The source's own counter and not source_stats(), which
    // folds in the graph's losses, and those are never the pause's.
    template <typename Call>
    [[nodiscard]] auto excusing_pause(Call&& call) -> decltype(call()) {
        const std::uint64_t lost_before = source_->stats().samples_lost;
        {
            const std::scoped_lock pacing(pacing_lock_);
            pacing_window_.pause_begun(steady_now_ns());
        }
        auto result = call();
        const std::uint64_t lost_after = source_->stats().samples_lost;
        {
            const std::scoped_lock pacing(pacing_lock_);
            pacing_window_.pause_ended(lost_after > lost_before ? lost_after - lost_before : 0);
        }
        return result;
    }

    // What run()'s control loop sees on each pass. The window keeps the
    // passes that see the delivered count move.
    void offer_pacing(std::int64_t now_ns) {
        const PacingSample sample{.at_ns = now_ns,
                                  .delivered = source_->stats().samples_delivered};
        const std::scoped_lock pacing(pacing_lock_);
        pacing_window_.offer(sample);
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
        if (is_probe_id(id)) {
            return fail("Engine::add_vrx has issued every receiver id below the probe "
                        "receivers' range and will not issue one inside it");
        }
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
        if (is_probe_id(id)) {
            return refuse_probe_id(id, "Engine::remove_vrx");
        }
        return graph_->remove_vrx(id);
    }

    [[nodiscard]] Status set_vrx_params(VrxId id, const VrxParams& params) override {
        if (graph_ == nullptr) {
            return fail("Engine::set_vrx_params before a source is open");
        }
        if (is_probe_id(id)) {
            return refuse_probe_id(id, "Engine::set_vrx_params");
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
        if (is_probe_id(id)) {
            return std::unexpected(refuse_probe_id(id, "Engine::vrx_status").error());
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
        if (is_probe_id(id)) {
            return refuse_probe_id(id, "Engine::set_audio_sink");
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
                        "is set by the receiver's channel and its filter, and there is no "
                        "receiver until the source's rate is known");
        }
        if (config_.passband_transform == 0) {
            return fail("this engine was created with EngineConfig::passband_transform at "
                        "zero, so no passband stage was built. Set it before Engine::create: "
                        "it sizes every receiver's display ring, and a ring cannot be grown "
                        "while a command buffer names it");
        }
        if (is_probe_id(id)) {
            return refuse_probe_id(id, "Engine::set_passband_sink");
        }
        return graph_->set_passband_sink(id, std::move(sink));
    }

    [[nodiscard]] Status submit_probe(const ProbeRequest& request) override {
        if (probes_ == nullptr) {
            return fail(graph_ == nullptr
                            ? "Engine::submit_probe before a source is open: a probe is placed "
                              "on the grid, and there is no grid until the source's rate is known"
                            : "this engine was created with EngineConfig::probe_receivers at "
                              "zero, so it has no probe pool");
        }
        return probes_->submit(request);
    }

    [[nodiscard]] std::size_t take_probe_outcomes(std::span<ProbeOutcome> out) override {
        return probes_ == nullptr ? 0 : probes_->take(out);
    }

    [[nodiscard]] ProbeStats probe_stats() const override {
        return probes_ == nullptr ? ProbeStats{} : probes_->stats();
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
        const std::int64_t start_ns = steady_now_ns();
        {
            const std::scoped_lock pacing(pacing_lock_);
            pacing_window_.start(PacingSample{.at_ns = start_ns,
                                              .delivered = source_->stats().samples_delivered});
        }
        stream_stop_ns_.store(0, std::memory_order_release);
        stream_start_ns_.store(start_ns, std::memory_order_release);

        source::StreamOptions options;
        options.block_samples = block_samples_;

        // Unthrottled by default, and faster than realtime is still not a
        // mode: it is what happens when nothing is holding a stopwatch, and
        // the only thing setting the pace is then the graph's blocking
        // reserve. A caller monitoring a capture on a loudspeaker asks for a
        // stopwatch by setting EngineConfig::pace, or a file's pace=, or
        // set_source_pace, which the source honours and the engine does
        // nothing else with.
        options.pace = pace_.load(std::memory_order_acquire);

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

                // The pacing window sees arrivals from here, because this
                // thread exists for exactly the length of the stream and
                // passes every 2 ms, which is how closely an arrival is timed.
                offer_pacing(steady_now_ns());
            }
        }

        // stop() on a source that has already finished is the join plus
        // whatever error ended the stream, which is what this wants either
        // way.
        Status ended = source_->stop();
        Status flushed = graph_->flush();
        // The last arrival offered at the instant the factor freezes at, so
        // a block delivered after the loop's last pass is in it.
        const std::int64_t stop_ns = steady_now_ns();
        offer_pacing(stop_ns);
        stream_stop_ns_.store(stop_ns, std::memory_order_release);
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

    [[nodiscard]] EngineLoad load() const override {
        EngineLoad out;
        if (graph_ == nullptr || scheduler_ == nullptr) {
            return out;
        }
        const GraphStats graph = graph_->stats();
        const SchedulerStats scheduler = scheduler_->stats();
        out.completions = scheduler.completions_finished;
        out.gpu_wait_ns = scheduler.wait_ns;
        out.handler_ns = scheduler.handler_ns;
        out.handler_max_ns = scheduler.handler_max_ns;
        out.audio_sink_ns = graph.audio_sink_ns;
        out.spectrum_sink_ns = graph.spectrum_sink_ns;
        out.passband_sink_ns = graph.passband_sink_ns;
        out.blocks = graph.blocks_in;
        out.record_ns = graph.record_ns;
        out.frame_stalls = graph.frame_stalls;
        out.frame_wait_ns = graph.frame_wait_ns;
        out.overrun_events = graph.overrun_events;
        out.samples_dropped = graph.samples_dropped;
        out.spectrum_frames = graph.spectrum_frames;
        out.audio_frames = graph.audio_frames;
        return out;
    }

    [[nodiscard]] const Graph* graph() const { return graph_.get(); }
    [[nodiscard]] const Scheduler* scheduler() const { return scheduler_.get(); }
    [[nodiscard]] const dsp::PrototypeFilter& prototype() const { return prototype_; }

private:
    // What the calibration file holds for a source about to be opened.
    struct StoredCalibration {
        std::string key;
        source::DeviceCalibration settings{};
        bool correction_applied = true;
        bool persisted = false;
        bool file_unreadable = false;
        std::string note;
    };

    // Looks the device up in EngineConfig::calibration_path. Never fails an
    // open: a device with no serial, an engine with no path and a file that
    // will not parse all open uncalibrated and say why in the note.
    [[nodiscard]] StoredCalibration stored_calibration_for(
        const source::SourceCapabilities& caps) const {
        StoredCalibration out;
        out.key = source::calibration_key(caps);
        out.correction_applied = !caps.device_corrects_frequency;

        if (out.key.empty()) {
            out.note = "this source carries no serial, so a calibration set on it applies "
                       "until it is closed and is not kept";
        } else if (auto ok = source::validate_calibration_key(out.key); !ok) {
            out.note = ok.error().message;
            out.key.clear();
        } else if (config_.calibration_path.empty()) {
            out.note = "this engine was started without a calibration file, so a calibration "
                       "set here applies until the source is closed and is not kept";
        } else {
            auto table = source::load_calibration_table(config_.calibration_path);
            if (!table) {
                out.file_unreadable = true;
                out.note = std::format(
                    "the calibration file could not be read, so this device opened "
                    "uncalibrated and nothing will be written over the file until it is "
                    "fixed: {}",
                    table.error().message);
            } else {
                out.persisted = true;
                if (const auto found = table->find(out.key); found != table->end()) {
                    out.settings = found->second;
                }
            }
        }

        if (!out.correction_applied) {
            if (!out.note.empty()) {
                out.note += ". ";
            }
            out.note += "The device was opened with a frequency correction of its own (ppm= on "
                        "its URI), so the stored correction is kept but not applied: two "
                        "corrections of one crystal would correct it twice";
        }
        return out;
    }

    // Writes the open device's settings into the file, keeping every other
    // device's line. Caller holds lifecycle_lock_.
    void store_calibration_locked() {
        if (calibration_key_.empty() || config_.calibration_path.empty() ||
            calibration_file_unreadable_) {
            calibration_persisted_ = false;
            return;
        }
        auto table = source::load_calibration_table(config_.calibration_path);
        if (!table) {
            calibration_file_unreadable_ = true;
            calibration_persisted_ = false;
            calibration_store_fault_ = std::format(
                "the calibration is in force and was not kept: the calibration file could not "
                "be read, and nothing is written over a file that cannot be read: {}",
                table.error().message);
            return;
        }
        (*table)[calibration_key_] = calibration_;
        if (auto saved = source::save_calibration_table(config_.calibration_path, *table);
            !saved) {
            calibration_persisted_ = false;
            calibration_store_fault_ = std::format(
                "the calibration is in force and was not kept: {}", saved.error().message);
            return;
        }
        calibration_persisted_ = true;
        calibration_store_fault_.clear();
    }

    [[nodiscard]] CalibrationState calibration_state_locked() const {
        CalibrationState out;
        if (source_ == nullptr || corrected_ == nullptr) {
            return out;
        }
        out.open = true;
        out.key = calibration_key_;
        out.settings = calibration_;
        out.correction_applied = !capabilities_.device_corrects_frequency;
        out.persisted = calibration_persisted_;
        out.note = calibration_note_;
        if (!calibration_store_fault_.empty()) {
            out.note += out.note.empty() ? "" : ". ";
            out.note += calibration_store_fault_;
        }
        out.device_center = corrected_->device_center();
        if (graph_ != nullptr) {
            out.front_end = graph_->front_end_correction();
        }
        return out;
    }

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

    // After graph_, so the default member order also destroys it first; the
    // destructor and close_source reset it explicitly before the graph anyway.
    std::unique_ptr<ProbePool> probes_;
    std::unique_ptr<source::Source> source_;
    source::SourceCapabilities capabilities_{};
    dsp::PrototypeFilter prototype_{};
    std::size_t block_samples_ = 0;

    // The open source's calibration. corrected_ borrows the wrapper source_
    // owns, which every opened source is wrapped in. All of it is written
    // under lifecycle_lock_ and cleared by close_source.
    source::CorrectedSource* corrected_ = nullptr;
    std::string calibration_key_;
    source::DeviceCalibration calibration_{};
    bool calibration_persisted_ = false;

    // Set when the calibration file exists and could not be read. Nothing is
    // written over it then: a hand-edited file with one bad line would
    // otherwise lose every other device in it at the next change.
    bool calibration_file_unreadable_ = false;

    // What the open said, and what the last write said when it failed.
    std::string calibration_note_;
    std::string calibration_store_fault_;

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

    // The pace in force: SourcePacing::paced_by has the rule. Atomic because
    // source_pacing reads it from whatever thread polls, unlocked.
    std::atomic<double> pace_{0.0};
    std::atomic<std::int64_t> stream_stop_ns_{0};

    // The realtime factor's window: see core/engine/pacing_window.h. run()'s
    // control loop offers to it, a control call marks a pause in it and
    // source_pacing() reads it, and the lock is between those alone. The
    // delivery thread never takes it.
    mutable std::mutex pacing_lock_;
    PacingWindow pacing_window_;

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

Status open_built_source(Engine& engine, std::unique_ptr<source::Source> source) {
    auto* impl = dynamic_cast<EngineImpl*>(&engine);
    if (impl == nullptr) {
        return fail("open_built_source needs an engine made by Engine::create; a wrapper "
                    "forwards open_source and has no way to take a source object");
    }
    if (auto adopted = impl->adopt_source(std::move(source)); !adopted) {
        return std::unexpected(with_context(adopted.error(), "open_built_source"));
    }
    return {};
}

}  // namespace revenant::engine
