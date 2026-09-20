// The engine: the whole chain from a source to audio, and the only thing that
// knows the order the stages run in.
//
// It is headless and has no Qt anywhere. The GUI is one client of the RPC
// surface above this, a script is another, and a remote instance is a third.
// That falls out for free only if it is true from the start, which is why the
// engine is built and tested with no client at all.
//
// The one promise everything else rests on: samples cross the bus once, into
// the device ring, and nothing returns to host memory except audio PCM,
// decoded symbols and detection metadata. Any method here that would break
// that does not exist.
//
// THE SPECTRUM SURFACE IS A DELIBERATE ADDITION TO A FILE TREATED AS FROZEN
//
// Everything above was written before there was any way to see a signal that
// no receiver was tuned to, and docs/detection.md records the consequence:
// the waterfall, the detector and the fine-tuning display are all downstream
// of a full-span spectrum, and there was no method here that returned one.
//
// It is shaped exactly like the audio sink, and that is the argument for it
// rather than for a polling method. A frame is produced by the device once
// per block whether anyone is looking or not, it is already coming back
// through a readback buffer on the completion thread, and a poll would either
// hand back the frame that happened to be latest, which drops most of them
// and tears the one it returns, or grow a queue this layer would then own. A
// callback delivers each frame once, on the thread that read it, with its
// buffer valid for the call and not after.
//
// A spectrum frame is metadata rather than samples, and it is not cheap. An
// earlier version of this comment called it "a fraction of a percent of what
// the stream itself would cost to bring home", which is wrong by about two
// orders of magnitude and was the only argument offered for letting it cross
// a bus this file declares closed.
//
// Measured at the defaults above. M*N/2 at 64 channels and a 2048-point
// transform is 65536 bins of four bytes, so 256 KiB per frame, one frame per
// block. At 20 MS/s a block is 65536 samples, so that is 305 frames a second
// and 80 MB/s coming back, against the 160 MB/s the cf32 stream costs going
// out. Half, not a fraction of a percent.
//
// It is still the right trade and the reason is different from the one that
// was written down. The frame is a REDUCTION: 80 MB/s buys the display and
// the detector everything they need about the whole span, where bringing the
// span itself home would cost twice that and leave both of them to transform
// it on the host. What makes it affordable is that it is bounded by the bin
// count rather than by the sample rate, so it does not grow when the radio
// gets wider. A caller budgeting PCIe or callback latency should use the
// number above and not the adjective. Nothing here returns complex baseband.
//
// AND THE PASSBAND IS A SECOND REDUCTION, WHICH THE PROMISE WAS NEVER
// AMENDED FOR
//
// The paragraphs above were written when the full-span frame was the only
// addition, and they read as though it still is. PassbandFrame is a second
// thing crossing the bus that is neither audio, nor a decoded symbol, nor
// detection metadata, and it arrived without this block saying so. Recorded
// here rather than folded in, because the promise at the top of this file is
// what a reader checks a new surface against, and a surface that is already
// in the tree and not in the promise makes the promise the wrong instrument.
//
// It passes the same test the full-span frame passes, on the same arithmetic
// and more cheaply. A passband frame is passband_transform bins of four
// bytes, one per block per SUBSCRIBED receiver, so at the shipped 4096
// points that is 16 KiB a frame and 5.0 MB/s at the 305 blocks a second a
// 20 MS/s source runs. Against the full-span frame's 80 MB/s it is noise,
// and against the 160 MB/s the cf32 stream costs going out it is three
// percent. It is bounded by the transform size rather than by the
// demodulation rate, so widening a receiver's filter does not widen it.
//
// What keeps it bounded in the other direction is that it is opt in per
// receiver: a rack of fifty receivers with one under examination pays for
// one. Fifty at once would be 250 MB/s and past what the full-span frame
// costs, so the per-receiver sink is load-bearing rather than tidy, and a
// caller that attaches sinks in bulk has left this promise's terms.
//
// The promise is therefore: samples cross once, and what comes back is audio
// PCM, decoded symbols, detection metadata, and bounded spectral reductions
// of the span and of a receiver's own passband. A fourth thing crossing
// amends this block again rather than arriving quietly.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "core/dsp/pfb.h"
#include "core/dsp/types.h"
#include "core/engine/device_ring.h"
#include "core/engine/vrx.h"
#include "core/error.h"
#include "core/gpu/context.h"
#include "core/source/registry.h"
#include "core/source/source.h"

namespace revenant::engine {

struct EngineConfig {
    // -1 chooses, or REVENANT_GPU_INDEX decides. The conformance matrix passes
    // an explicit index.
    int gpu_index = -1;

    // How much capture history the device ring holds. Shrunk to fit the
    // device, and what was achieved is reported rather than assumed.
    double ring_seconds = 8.0;

    // Coarse grid. channels must be a power of two and decimation must divide
    // it; the project uses 2x oversampling, so decimation is channels/2.
    std::uint32_t channels = 64;
    std::uint32_t taps_per_branch = 17;

    // Audio rate every receiver resamples to unless it asks for another.
    dsp::SampleRate audio_rate = 48'000;

    // Samples the upload stages at a time. Larger is fewer submissions and
    // more latency.
    std::size_t block_samples = 65'536;

    // How fast a Demand source is asked to deliver, as a multiple of
    // realtime. Zero, the default, is unthrottled, and unthrottled is still
    // what happens when nothing is holding a stopwatch.
    //
    // This is not a throttle on the engine and there is still no code path
    // for faster than realtime. It is the source being asked to deliver on a
    // clock, which is the one thing a live monitor needs and a radio supplies
    // for free: a capture replayed as fast as the GPU retires it produces
    // audio a loudspeaker cannot accept, and what the listener hears is the
    // monitor's backlog being trimmed rather than the recording. A Paced
    // source ignores this, because its own hardware already sets the rate.
    double pace = 0.0;

    // Points in the full-span spectrum's per-channel transform. Zero, the
    // default, builds no spectrum stage at all.
    //
    // Off by default because it is not free: a pipeline, a twiddle circle, a
    // window, and one readback region per frame in flight, for a frame nobody
    // has asked to see. A headless recording pays none of it.
    //
    // The stage is not one transform across the whole span. The span is
    // covered by transforming each coarse channel the channelizer has already
    // produced and keeping the central half of each one, which is what fits
    // the shared-memory budget docs/fft.md scopes the FFT kernel to. The
    // number here is therefore per channel, so a frame is
    // grid.channels * spectrum_transform / 2 bins wide. It is clamped to what
    // this device's shared memory holds, and EngineInfo::spectrum reports
    // what was settled on.
    std::uint32_t spectrum_transform = 0;

    // Holds one or both ends of the spectrum's colour map still, in dBFS.
    //
    // Empty is automatic, which docs/ui-spectrum.md makes the default because
    // it is right almost always. The exception it names is comparing two
    // captures, where a scale that moves is a scale that lies about which
    // signal was stronger, and that is what these are for.
    //
    // They sit on the config rather than behind a setter because that is
    // where the rest of the spectrum stage's shape sits, and because a pin
    // taken for a comparison is decided before the run rather than during it.
    // A display that wants to pin interactively has everything it needs on
    // the frame already: SpectrumFrame carries the raw percentiles beside the
    // smoothed ends.
    std::optional<float> spectrum_floor_db;
    std::optional<float> spectrum_ceiling_db;

    // Points in a receiver's passband transform. Zero, the default, builds no
    // passband stage at all, and then set_passband_sink is refused the same
    // way set_spectrum_sink is at spectrum_transform zero.
    //
    // Two levels of opt-in, and they are answering different costs. This one
    // decides whether the machinery exists: a pipeline, two window tables, and
    // room in every receiver's fine ring for a window this long. A headless
    // recording leaves it zero and pays none of that. Attaching a sink to one
    // receiver is the second: until a receiver has one, it allocates no
    // buffers and dispatches nothing, so a rack of fifty receivers with one
    // under examination pays for one transform.
    //
    // Unlike spectrum_transform this is not per channel. A passband frame is
    // this many bins wide and covers one receiver's whole demodulation rate,
    // because the fine stream is one stream rather than a bank.
    std::uint32_t passband_transform = 0;
};

// The frequency axis of a spectrum frame, and how wide one is.
//
// Both frequencies are exact rationals rather than integer hertz, for the
// reason dsp::ChannelCentre gives: a bin is source_rate / (D * N) hertz wide
// and a coarse channel centre is k * source_rate / M, and neither is a whole
// number of hertz in general. Rounding either here would put an unsourceable
// offset into every frequency a person clicks on, which is what the
// integer-hertz convention exists to prevent. A display converts once, at the
// point it draws a label.
//
// Both are relative to the source's baseband DC, the same frame VrxParams is
// in. Add EngineInfo::source_center for absolute radio frequency.
struct SpectrumGeometry {
    // Points in one coarse channel's transform, and the central half of that
    // which is kept.
    std::uint32_t transform = 0;
    std::uint32_t bins_per_channel = 0;

    // Coarse channels the frame tiles, and the total bins across the span.
    std::uint32_t channels = 0;
    std::uint32_t bins = 0;

    std::int64_t bin_width_numerator = 0;
    std::int64_t bin_width_denominator = 1;
    std::int64_t bin_zero_numerator = 0;
    std::int64_t bin_zero_denominator = 1;

    [[nodiscard]] constexpr bool enabled() const { return bins != 0; }

    [[nodiscard]] constexpr double bin_width_hz() const {
        return bin_width_denominator == 0 ? 0.0
                                          : static_cast<double>(bin_width_numerator) /
                                                static_cast<double>(bin_width_denominator);
    }

    // Centre frequency of bin zero.
    //
    // Half a coarse channel below the negative edge of the band, because the
    // lowest channel is centred on the Nyquist frequency and owns the half
    // channel either side of it. The part below -rate/2 is the same physical
    // spectrum as the part below +rate/2, since a sampled band is periodic,
    // and on any real receiver both are the front end's roll-off. A display
    // labels it as written and the axis stays continuous.
    [[nodiscard]] constexpr double bin_zero_hz() const {
        return bin_zero_denominator == 0 ? 0.0
                                         : static_cast<double>(bin_zero_numerator) /
                                               static_cast<double>(bin_zero_denominator);
    }
};

// The frequency axis of one receiver's passband frame.
//
// A separate type from SpectrumGeometry rather than a reuse of it, because
// two of that struct's fields would have to mean something else here:
// `channels` is a coarse channel count and a passband has no bank behind it,
// and `bins_per_channel` is the central half of a transform where a passband
// keeps every bin. Both are load-bearing in the full-span frame's layout, so
// a passband borrowing the struct would be a frame whose fields a consumer
// has to know not to believe.
//
// Rationals for the same reason SpectrumGeometry carries them: Fd / N is
// rarely a whole hertz, and neither is the fine stream's DC, which is a
// coarse channel centre plus the residual the fine stage mixed out. Rounding
// either here puts an unsourceable offset into every frequency a person
// clicks on.
//
// Both frequencies are relative to the source's baseband DC, the same frame
// VrxParams::center is in. Add EngineInfo::source_center for absolute radio
// frequency.
struct PassbandGeometry {
    // Points in the transform, which is also the bin count: the whole band is
    // kept, edge to edge.
    std::uint32_t transform = 0;
    std::uint32_t bins = 0;

    // The fine stream's rate, which is the width of the frame. plan_vrx
    // rounds it up to a whole multiple of the audio rate, so it is somewhat
    // wider than the receiver's bandwidth rather than equal to it, and the
    // margin is where the filter's skirts are drawn.
    dsp::SampleRate rate = 0;

    std::int64_t bin_width_numerator = 0;
    std::int64_t bin_width_denominator = 1;
    std::int64_t bin_zero_numerator = 0;
    std::int64_t bin_zero_denominator = 1;

    [[nodiscard]] constexpr bool enabled() const { return bins != 0; }

    [[nodiscard]] constexpr double bin_width_hz() const {
        return bin_width_denominator == 0 ? 0.0
                                          : static_cast<double>(bin_width_numerator) /
                                                static_cast<double>(bin_width_denominator);
    }

    // Centre frequency of bin zero, which is half the demodulation rate below
    // whatever the fine stage mixed to DC.
    //
    // That is NOT always the receiver's centre. For CW the fine stage
    // translates the carrier to the operator's pitch rather than to DC, so
    // this sits a pitch below VrxParams::center. Carried rather than derived
    // for exactly that reason: a display that computed the axis from
    // params.center would be one pitch out on one mode and right on the other
    // seven, which is the shape of a bug nobody finds.
    [[nodiscard]] constexpr double bin_zero_hz() const {
        return bin_zero_denominator == 0 ? 0.0
                                         : static_cast<double>(bin_zero_numerator) /
                                               static_cast<double>(bin_zero_denominator);
    }
};

// What the engine settled on, which is frequently not what was asked for.
struct EngineInfo {
    gpu::DeviceInfo device;
    RingGeometry ring;
    dsp::GridParams grid;
    dsp::SampleRate source_rate = 0;
    dsp::SampleRate channel_rate = 0;
    dsp::Hertz channel_spacing = 0;

    // Left empty when EngineConfig::spectrum_transform was zero.
    SpectrumGeometry spectrum{};

    // Points in a passband transform, after the same clamp against this
    // device's shared memory the full-span transform takes. Zero when
    // EngineConfig::passband_transform was zero.
    //
    // The transform size is all that is engine-wide. The rest of a passband's
    // axis is the receiver's, because the rate is the receiver's, so the
    // geometry arrives on each frame rather than here.
    std::uint32_t passband_transform = 0;

    // What the source's baseband DC corresponds to in real radio frequency,
    // read once when the source was opened.
    //
    // Every frequency in VrxParams is an offset from baseband DC, because
    // that is the only frame the grid has. A person tunes in absolute hertz,
    // so something has to hold the constant that relates the two, and a
    // caller that had to parse it back out of the source URI would be
    // reimplementing each backend's grammar to do it. Zero for a source whose
    // baseband is not a translation of anything, which is what a synthetic
    // scene with no declared centre is.
    dsp::Hertz source_center = 0;
};

// One receiver's audio, handed to the caller on the host.
//
// WHAT THIS COMMENT USED TO SAY. Until 2026-09-20 it read "this and the
// spectrum frame are the only things that come back across the bus".
// PassbandFrame below is a third and it was added without this sentence
// being touched. The count was right once, and counting is the wrong shape
// for it, so the rule is stated instead: what comes back is audio PCM,
// decoded symbols, detection metadata and the two bounded spectral
// reductions, with the arithmetic for all of it at the top of this file.
// Everything else the engine computes stays on the device.
struct AudioChunk {
    VrxId vrx;
    dsp::SampleIndex start = 0;
    dsp::SampleRate rate = 0;

    // Interleaved if stereo, mono otherwise. Real, not complex: a demodulator
    // that hands out complex baseband is the raw tap, and that goes to a
    // decoder rather than here.
    std::span<const float> samples;
    std::uint32_t channels = 1;

    // The squelch gate at the moment these frames were produced.
    //
    // False means the samples above are ZEROS THE GRAPH WROTE, not a quiet
    // band. core/engine/graph.cpp mutes a closed receiver in the readback
    // buffer before any sink is called, so every consumer of this chunk sees
    // the same silence, and without this flag none of them can tell the gate
    // shutting from the transmitter stopping. A recording is identical either
    // way; a subscriber's indicator is not.
    //
    // Defaulted true so a producer that does not gate says nothing about a
    // gate. It is not a claim that the signal was strong.
    bool squelch_open = true;
};

using AudioSink = std::function<Status(const AudioChunk&)>;

// One consumer's place in a receiver's audio.
//
// Issued by Engine::attach_audio_sink, unique for the life of one Engine and
// never reused. A token that has already been detached therefore detaches
// nothing, rather than detaching whichever consumer arrived next.
using AudioSinkId = std::uint64_t;

// More than one consumer on one receiver's audio.
//
// WHAT THIS EXISTS TO STOP. set_audio_sink below holds one sink per receiver
// and the second call replaces the first, silently and with no way to ask
// what was there. That is survivable while the only caller is the process
// that built the engine; it stops being survivable the moment a second one
// can ask for audio over the wire, because the first subscribeAudio on a
// host that is also recording or playing locally takes the sound card and
// nothing anywhere says so. tools/cli/main.cpp already hand-rolls a two-way
// version of this in a lambda, which is the shape of the problem rather than
// a solution to it.
//
// ORDERING. Sinks are called in attach order, in one pass, on the producer
// thread, and every one of them is called: a sink that fails does not stop
// the sinks behind it. Attach order is stable and is not a priority; nothing
// here promises a recording sees a chunk before a subscriber does, only that
// each of them sees every chunk exactly once and in stream order.
//
// FAILURE. The first error is remembered and returned after the pass, so the
// caller learns that something refused while every other consumer still got
// its samples. core/engine/graph.cpp turns that error into a failing
// dispatch and ends the run, which is right: core/engine/audio_egress.h
// reserves a sink error for a wiring mistake, and a consumer that merely
// cannot keep up counts a drop of its own and returns success.
//
// WHAT IT DOES NOT DO, WHICH IS THE PART THAT MATTERS. A slow sink is NOT
// isolated. The pass is synchronous on the thread that retired the GPU
// readback, so a sink that blocks for ten milliseconds delays every sink
// behind it and the completion thread with them. No fan-out can fix that,
// because the buffer that absorbs a slow consumer has to be sized in that
// consumer's own units: a recording wants seconds of disk hiccup
// (AudioEgressConfig::ring_seconds), a loudspeaker wants one device period,
// and a wire subscription wants the depth its client asked for. Every sink
// attached here must therefore copy and return. The ones in this tree do:
// AudioEgress::publish writes into a lock-free ring, and the RPC server's
// copies into that subscription's own queue.
//
// THREAD SAFETY. attach, detach and empty are the control thread and take
// lock_. deliver is the producer thread: it allocates nothing and copies
// nothing, loading one shared_ptr to an immutable list that a concurrent
// attach replaces rather than mutates.
//
// THIS PARAGRAPH USED TO SAY deliver "takes no lock". It takes one, and the
// correction matters because the sentence was being read as a promise about
// the thread retiring GPU readbacks.
//
// std::atomic<std::shared_ptr<T>> is not lock free anywhere. MSVC's
// is_always_lock_free is a hardcoded false and load() calls
// _Repptr._Lock_and_load(), which spins on a lock bit stashed in the
// control-block pointer, copies the pointer pair, increments the refcount
// and unlocks. So the producer takes a spin lock, an atomic increment and a
// release on every chunk.
//
// THE SHAPE STANDS, and the reason is the rate rather than the cost. The
// lock bit lives in this object, not in a process-wide table, so the only
// thing that can contend for it is an attach, a detach or another deliver on
// THIS fan-out. attach and detach are control plane, meaning an operator
// action or a client subscribing, and they hold it for the length of a
// pointer swap. deliver runs once per chunk per receiver, which at
// 16384-sample blocks on a 2.4 MS/s source is 146 times a second: an
// uncontended lock bit against an interval of 6.8 milliseconds. Neither
// alternative is better. A std::mutex here puts a real lock on the same
// thread and blocks it behind an attach; giving the producer its own
// unsynchronised copy needs a reclamation scheme for the list the control
// thread just replaced, which is the problem shared_ptr is already solving.
//
// What would change the answer is a per-sample or per-frame fan-out rather
// than a per-chunk one. It is not that, and AudioChunk is the only thing
// this class carries.
class AudioFanout {
public:
    AudioFanout() = default;

    AudioFanout(const AudioFanout&) = delete;
    AudioFanout& operator=(const AudioFanout&) = delete;
    AudioFanout(AudioFanout&&) = delete;
    AudioFanout& operator=(AudioFanout&&) = delete;

    // Control thread. Returns the token that detaches this one consumer.
    [[nodiscard]] AudioSinkId attach(AudioSink sink) {
        const std::scoped_lock held(lock_);
        const AudioSinkId token = next_++;

        // Copy, append, publish. The list the producer thread may be walking
        // right now is never written to, so it needs no lock to read one.
        auto next = std::make_shared<Entries>(*live_.load());
        next->push_back(Entry{token, std::move(sink)});
        live_.store(std::move(next));
        return token;
    }

    // Control thread. False when that token is not attached, which is an
    // ordinary answer rather than an error: a client can cancel and then drop
    // the capability.
    bool detach(AudioSinkId token) {
        const std::scoped_lock held(lock_);
        auto current = live_.load();
        auto next = std::make_shared<Entries>();
        next->reserve(current->size());
        bool found = false;
        for (const Entry& entry : *current) {
            if (entry.id == token) {
                found = true;
                continue;
            }
            next->push_back(entry);
        }
        if (!found) {
            return false;
        }
        live_.store(std::move(next));
        return true;
    }

    [[nodiscard]] bool empty() const { return live_.load()->empty(); }
    [[nodiscard]] std::size_t size() const { return live_.load()->size(); }

    // The producer thread, through the one AudioSink this fan-out installs.
    [[nodiscard]] Status deliver(const AudioChunk& chunk) const {
        const std::shared_ptr<const Entries> entries = live_.load();
        Status outcome;
        for (const Entry& entry : *entries) {
            // Unreachable through Engine::attach_audio_sink, which refuses an
            // empty sink before it gets here, and reachable through this
            // class, which has no error channel on attach and so takes what
            // it is given. A caller that lost its callable gets a token that
            // detaches and a fan-out that skips it, rather than a crash on
            // the thread retiring GPU readbacks.
            // tests/engine/test_audio_fanout.cpp drives it through the class.
            if (!entry.sink) {
                continue;
            }
            if (auto handed = entry.sink(chunk); !handed && outcome) {
                // The first failure, kept and returned after the pass. Later
                // ones are lost on purpose: the run is ending either way and
                // the first message names the consumer that started it.
                outcome = std::unexpected(handed.error());
            }
        }
        return outcome;
    }

private:
    struct Entry {
        AudioSinkId id = 0;
        AudioSink sink;
    };
    using Entries = std::vector<Entry>;

    // Serialises attach against detach. The producer never takes it.
    std::mutex lock_;

    // Replaced whole rather than mutated, which is what lets deliver() read
    // it with one atomic load and no lock. Never null.
    std::atomic<std::shared_ptr<const Entries>> live_{std::make_shared<const Entries>()};

    AudioSinkId next_ = 1;
};

// One full-span spectrum frame, handed to the caller on the host.
//
// One frame per dispatch, which is one per source block, so the frame rate is
// the source rate over EngineConfig::block_samples and is not something a
// consumer chooses. A consumer that wants fewer decimates in time; a consumer
// that wants more asks for smaller blocks, which costs submissions.
struct SpectrumFrame {
    // Power per bin in decibels relative to full scale, ascending in
    // frequency across the whole span with no gaps and nothing counted twice.
    //
    // Valid for the duration of the call and not after. A consumer that keeps
    // it copies it, the same as AudioChunk::samples.
    std::span<const float> power_db;

    SpectrumGeometry geometry;

    // The source samples this frame's window covers, as a half-open range
    // [start, start + count). Absolute from the start of the stream, so it
    // lines up with an AudioChunk's start and with anything else indexed the
    // way docs/conventions.md says time is indexed. The window is N coarse
    // channel samples, so it spans N * grid.decimation source samples, and
    // the filter's group delay has already been taken off.
    dsp::SampleIndex start = 0;
    dsp::SampleIndex count = 0;

    // Frames delivered before this one. A waterfall that skipped a row knows
    // it skipped a row, rather than drawing a gap as though the band went
    // quiet.
    std::uint64_t sequence = 0;

    // The colour map's two ends for this frame, in the same dBFS as power_db.
    //
    // A SECOND DELIBERATE ADDITION TO THIS FILE, AND WHY IT IS HERE RATHER
    // THAN IN EACH CONSUMER
    //
    // docs/ui-spectrum.md, "Auto-scaling, both ends": the floor and the
    // ceiling both track the signal over about thirty seconds, from
    // percentiles rather than extremes, expanding in a frame or two and
    // contracting over the thirty, and measured on the device. Four numbers
    // is what that costs on the wire.
    //
    // They are on the frame because otherwise every consumer computes them,
    // and then the waterfall, the detector's threshold and a screenshot's
    // legend disagree about where the noise floor is while all three are
    // looking at the same frame. That disagreement is invisible: each one is
    // internally consistent and nothing crosses to compare them.
    //
    // The two that actually come out of the device are the percentiles below.
    // These two are the smoothed ends, carried alongside rather than instead,
    // because a consumer with its own time constant, a detector that wants
    // this frame's noise floor and not a thirty-second average among them,
    // needs the measurement rather than the display's version of it.
    float floor_db = 0.0F;
    float ceiling_db = 0.0F;

    // What the device measured of THIS frame, before any smoothing: the low
    // and high percentiles named in core/dsp/spectrum_levels_reference.h.
    //
    // These are the honest instantaneous figure. They move several decibels
    // frame to frame even on a static band, which is why nothing draws
    // against them directly.
    float percentile_low_db = 0.0F;
    float percentile_high_db = 0.0F;
};

using SpectrumSink = std::function<Status(const SpectrumFrame&)>;

// One receiver's passband, handed to the caller on the host.
//
// docs/ui-spectrum.md, "The fine-tuning display": a second spectrum over the
// receiver's own passband rather than the wide span, which is what makes
// parking a filter on a signal precise rather than approximate. It is a
// transform of the fine stream and not a zoom of the wide one, and the
// difference is resolution: at the shipped geometry a 4096-point transform of
// a 48 kS/s fine ring resolves 11.7 Hz where a 2^18-point transform of a
// 20 MHz span resolves 76 Hz.
//
// NO CHANNEL-SHAPE CORRECTION IS APPLIED, AND THAT IS THE OPPOSITE OF THE
// FULL-SPAN FRAME
//
// SpectrumFrame's bins are divided by the channelizer prototype's droop
// across each channel's kept band, because that band's edges sit exactly on
// the prototype's cutoff and the span tiles the droop once per channel. None
// of that reasoning reaches here. A passband frame is one piece with no seam
// to hide, its edges are at plus and minus half the demodulation rate rather
// than on any filter's cutoff, and the response shaping it is the receiver's
// own fine filter.
//
// Dividing that out would erase the thing the display exists to show. The
// skirts ARE the feature: a filter parked on a signal is judged by where its
// edges fall against the signal's. And the stopband is 60 to 120 dB down, so
// inverting the filter would multiply the noise out there by up to 1e12 and
// replace a clean floor with a wall.
//
// The deeper reason is that a passband frame is a view of a stream something
// downstream consumes. Whatever the fine stage's filter did to these samples,
// the demodulator is demodulating it, so a corrected picture would disagree
// with the audio. The full-span frame has no such consumer: it is a
// measurement of the band, the seams are in the measurement, and correcting
// them makes it more true rather than less.
//
// What that leaves in is the coarse prototype's own tilt across the
// receiver's slice of its channel, which IS a measurement artefact of the
// same class. It is left in for the same reason: the fine stage filtered a
// channel that already had that tilt, so it is in the audio too.
struct PassbandFrame {
    VrxId vrx;

    // Power per bin in decibels relative to full scale, ascending in
    // frequency across the whole demodulation rate with no gaps.
    //
    // Valid for the duration of the call and not after, the same as
    // AudioChunk::samples and SpectrumFrame::power_db.
    std::span<const float> power_db;

    PassbandGeometry geometry;

    // The source samples this frame's window covers, as a half-open range
    // [start, start + count). Absolute from the start of the stream, so it
    // lines up with an AudioChunk's start and with a SpectrumFrame's. Both
    // the channelizer prototype's group delay and the fine filter's have
    // already been taken off, so this is when the energy was on the air and
    // not when the samples reached this stage.
    dsp::SampleIndex start = 0;
    dsp::SampleIndex count = 0;

    // Frames delivered to this receiver before this one, so a waterfall that
    // skipped a row knows it skipped a row.
    //
    // The receiver's count and not the engine's, and it survives a sink
    // being replaced: the buffers behind a passband are rebuilt on every
    // attach, but frames recorded against the old set can still be in flight
    // when the new one arrives, and a count that restarted with the buffers
    // would run backwards across that handover.
    std::uint64_t sequence = 0;

    // This receiver's own colour map, smoothed by its own SpectrumScale with
    // the same time constants the full-span one uses.
    //
    // Its own and not the span's, because docs/ui-spectrum.md is explicit
    // that the fine display scales on the receiver's passband: a display
    // scaled by something it is not showing is a display that lies, and a
    // passband holding one 30 dB signal has nothing in common with a 20 MHz
    // span whose percentiles are mostly noise.
    float floor_db = 0.0F;
    float ceiling_db = 0.0F;

    // What the device measured of THIS frame, before any smoothing. A
    // consumer with its own time constant wants these rather than the two
    // above; there are no pins on the passband scale for that reason.
    float percentile_low_db = 0.0F;
    float percentile_high_db = 0.0F;
};

using PassbandSink = std::function<Status(const PassbandFrame&)>;

class Engine {
public:
    [[nodiscard]] static Expected<std::unique_ptr<Engine>> create(const EngineConfig& config);

    virtual ~Engine() = default;

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Opens a source by URI and sizes the grid and the ring against it. The
    // engine takes ownership: a source outliving the graph that reads it is a
    // use-after-free waiting for a scheduling accident.
    [[nodiscard]] virtual Status open_source(std::string_view uri) = 0;
    [[nodiscard]] virtual const source::SourceCapabilities& source_capabilities() const = 0;

    [[nodiscard]] virtual const EngineInfo& info() const = 0;

    // Adding a receiver must not rebuild the coarse stage, and nothing in
    // VrxParams appears in GridParams, so it cannot.
    //
    // params.center is a baseband offset and nothing in here rebases it: it
    // reaches engine::place as given and is stored as given. A caller that
    // knows a frequency in absolute hertz, which is every caller with an
    // operator or a detection behind it, subtracts info().source_center
    // first. The note on VrxParams::center has the argument for that being
    // the caller's job.
    [[nodiscard]] virtual Expected<VrxId> add_vrx(const VrxParams& params) = 0;
    [[nodiscard]] virtual Status remove_vrx(VrxId id) = 0;

    // The same frame in both directions: vrx_status hands back the params it
    // was given, so reading one out and passing it straight back in leaves
    // the receiver where it was.
    [[nodiscard]] virtual Status set_vrx_params(VrxId id, const VrxParams& params) = 0;
    [[nodiscard]] virtual Expected<VrxStatus> vrx_status(VrxId id) const = 0;
    [[nodiscard]] virtual std::vector<VrxId> vrx_ids() const = 0;

    // Routes a receiver's audio to a callback. One sink per receiver; setting
    // a second replaces the first, and an empty one detaches.
    //
    // THIS IS THE SLOT AND NOT THE SEAM. It is the low-level call, kept
    // because the graph has exactly one place to put a sink and something has
    // to fill it. A caller that wants audio alongside whatever else is
    // already listening uses attach_audio_sink below; a caller that reaches
    // this directly displaces every consumer the fan-out was holding and is
    // not told it did.
    [[nodiscard]] virtual Status set_audio_sink(VrxId id, AudioSink sink) = 0;

    // Adds a consumer to this receiver's audio without displacing the ones
    // already there, and hands back the token that removes it again.
    //
    // Non-virtual and written once, here, on top of set_audio_sink: the first
    // attach on a receiver installs an AudioFanout in the slot and every
    // attach after it joins that fan-out. The last detach takes the slot off
    // again, so a receiver nobody is listening to costs one std::function
    // call per dispatch and no more. AudioFanout above has the ordering and
    // the failure semantics, including what a slow consumer does to the
    // others, which is the part a caller has to design around.
    //
    // Refused in the engine's own words for a receiver that does not exist
    // and before a source is open, because both refusals come back out of
    // set_audio_sink unchanged.
    //
    // WHAT IT CANNOT DO. It cannot stop a caller from reaching
    // set_audio_sink directly and throwing the fan-out away; nothing can,
    // short of removing that method, and it is the only way to fill the slot.
    // tools/cli/main.cpp is the remaining direct caller in this tree.
    [[nodiscard]] Expected<AudioSinkId> attach_audio_sink(VrxId id, AudioSink sink);

    // Removes one consumer. The token is the one attach_audio_sink returned.
    // Detaching a token that is not attached is an error and not a silent
    // success, because the two things it means, a double detach and a token
    // from another receiver, are both mistakes a caller wants to hear about.
    [[nodiscard]] Status detach_audio_sink(VrxId id, AudioSinkId sink);

    // Routes the full-span spectrum to a callback. One sink for the engine,
    // because there is one span; setting a second replaces the first and an
    // empty one stops delivery.
    //
    // Refused when EngineConfig::spectrum_transform was zero, because the
    // stage is built with the rest of the coarse chain when the source is
    // opened and adding one later would mean rebuilding it, which is the one
    // thing the graph promises never to do while a stream is running.
    [[nodiscard]] virtual Status set_spectrum_sink(SpectrumSink sink) = 0;

    // Routes one receiver's passband to a callback, and is the per-receiver
    // opt-in: until a sink is attached that receiver records no transform and
    // holds no buffers for one. An empty sink detaches and frees them again.
    //
    // One sink per receiver; setting a second replaces the first. Refused
    // when EngineConfig::passband_transform was zero, and refused for a raw
    // tap, which has no fine stage to transform: the graph's raw tap is a
    // copy out of the coarse channel ring and never mixes or filters, so
    // there is no per-receiver baseband on the device to look at. The
    // full-span spectrum already covers that channel.
    [[nodiscard]] virtual Status set_passband_sink(VrxId id, PassbandSink sink) = 0;

    // Runs the graph until the source ends or stop() is called. The source's
    // own thread drives it; this returns once the stream is finished and
    // hands back whatever ended it.
    [[nodiscard]] virtual Status run() = 0;
    [[nodiscard]] virtual Status stop() = 0;
    [[nodiscard]] virtual bool running() const = 0;

    // Counters, not logs. An overrun is a correctness event: a recording that
    // looks continuous and is not, with nothing downstream able to tell, is
    // the failure mode a log line produces.
    [[nodiscard]] virtual source::SourceStats source_stats() const = 0;

protected:
    Engine() = default;

private:
    // The fan-outs this engine has installed, one per receiver that has at
    // least one attached consumer. Control-plane state on an otherwise pure
    // interface, which is deliberate: it has to sit above set_audio_sink to
    // be shared by every caller, and there is nowhere above Engine that every
    // caller passes through.
    //
    // Keyed by VrxId::value rather than by VrxId, because VrxId has equality
    // and no ordering and giving it one would be a comparison nobody means.
    std::mutex audio_fanout_lock_;
    std::map<std::uint32_t, std::shared_ptr<AudioFanout>> audio_fanouts_;
};

inline Expected<AudioSinkId> Engine::attach_audio_sink(VrxId id, AudioSink sink) {
    if (!sink) {
        return fail("Engine::attach_audio_sink was given an empty sink. Detaching is "
                    "detach_audio_sink with the token attach handed back, so an empty one "
                    "here is a caller that lost its callable rather than one asking to stop");
    }

    const std::scoped_lock held(audio_fanout_lock_);

    auto found = audio_fanouts_.find(id.value);
    const bool fresh = found == audio_fanouts_.end();
    std::shared_ptr<AudioFanout> fanout =
        fresh ? std::make_shared<AudioFanout>() : found->second;

    const AudioSinkId token = fanout->attach(std::move(sink));
    if (!fresh) {
        return token;
    }

    // The slot is filled before the map records it, so a refusal leaves this
    // engine exactly as it was: the local fan-out dies here with the sink
    // that was just attached to it, and the receiver keeps whatever it had.
    if (auto installed = set_audio_sink(id, [fanout](const AudioChunk& chunk) -> Status {
            return fanout->deliver(chunk);
        });
        !installed) {
        return std::unexpected(installed.error());
    }

    audio_fanouts_.emplace(id.value, std::move(fanout));
    return token;
}

inline Status Engine::detach_audio_sink(VrxId id, AudioSinkId sink) {
    const std::scoped_lock held(audio_fanout_lock_);

    auto found = audio_fanouts_.find(id.value);
    if (found == audio_fanouts_.end()) {
        return fail(std::format(
            "receiver {} has no attached audio consumers, so sink {} cannot be detached from "
            "it",
            id.value, sink));
    }
    if (!found->second->detach(sink)) {
        return fail(std::format(
            "sink {} is not attached to receiver {}. A token is unique for the life of an "
            "engine, so this is either a second detach of the same one or a token issued for "
            "another receiver",
            sink, id.value));
    }
    if (!found->second->empty()) {
        return {};
    }

    audio_fanouts_.erase(found);

    // Discarded, and the one place in this pair where a refusal is not the
    // caller's business. A receiver removed while a consumer was still
    // attached is the ordinary teardown order and set_audio_sink refuses an
    // id the graph no longer knows; the fan-out has already been dropped
    // above, so there is nothing left on the receiver either way.
    static_cast<void>(set_audio_sink(id, {}));
    return {};
}

}  // namespace revenant::engine
