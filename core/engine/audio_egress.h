// Audio egress: where a receiver's audio leaves the engine.
//
// The shape is fixed by two facts that pull in opposite directions. Audio is
// produced by whichever thread reads back the mapped Vulkan buffer, in blocks
// sized by whatever the GPU chain found convenient. Audio is consumed either
// by a sound device, which asks for an exact number of frames on its own
// clock and punishes a late answer with a click, or by a file, which will
// stall for a hundred milliseconds when the filesystem feels like it. Neither
// side can wait for the other, so between them sits one lock-free SPSC ring
// per receiver, core/engine/spsc_ring.h, and this file is the accounting and
// the threading around it.
//
// THREE THREADS, and only two of them are realtime. Every method below says
// which one calls it, because a structure like this is unreviewable
// otherwise.
//
//   The producer thread. The GPU readback thread, one of them, calling
//   publish(). It never blocks, never locks, never allocates and never waits
//   on a fence. When a ring is full the samples are gone, and they are
//   counted rather than logged, because a dropout is something the operator
//   hears and a log line produces a recording that sounds broken with nothing
//   able to say why. The single exception to the no-allocation rule is the
//   error return, which is reached only when the caller has published to a
//   receiver that does not exist or with the wrong channel count. That is a
//   wiring mistake, not a steady state.
//
//   The device callback thread. Owned by the operating system, and for M1 it
//   does not exist: it arrives with the first Pull backend. It reads the ring
//   directly through an AudioTap and is under the same prohibitions as the
//   producer. read_or_fill() is what it calls, so a shortfall becomes counted
//   silence rather than a stale buffer.
//
//   The egress drain thread. Owned by AudioEgress, and the one that is not
//   realtime. It exists for Push backends, the file case, and it blocks on
//   the filesystem by design: a slow disk turns into backlog in the ring and
//   then into counted drops on the producer, which is the correct place for
//   that cost to land. It allocates nothing on the path that moves samples,
//   which is why a fault message is copied into a fixed buffer in the slot
//   rather than into a std::string.
//
//   THAT SENTENCE USED TO READ "it does not allocate at all, including on its
//   fault path", and the fault path has always allocated: Error carries a
//   std::string and the backend builds one to fail with. The promise that is
//   true, and the one a reader needs, is about the steady state. A fault
//   allocates at most once per receiver, because the slot stops draining the
//   instant one is recorded.
//
//   IT IS ALSO THE THREAD AN EXCEPTION KILLS THE PROCESS FROM. It is a
//   std::thread, so anything thrown out of a backend's write() is
//   std::terminate. core/engine/audio_egress.cpp catches at the call for the
//   reason core/engine/engine.h gives for call_sink, and a backend is
//   therefore allowed to throw even though returning a Status is what this
//   interface asks for.
//
// PUSH AND PULL, which is the seam a device backend arrives through.
//
// A file wants to be written to; a sound device wants to read from you. That
// difference is the reason a naive "AudioSink::write()" interface has to be
// rewritten the day WASAPI shows up, so it is in the interface from the
// start. A Push backend, the WAV writer here, is driven by the drain thread
// calling write(). A Pull backend, WASAPI or ALSA or CoreAudio, is handed an
// AudioTap at attach() and reads from it on its own callback thread; the
// drain thread does not touch that receiver at all. Everything else, the
// ring, the drop accounting, publish(), the slot lifetime, is identical. So
// adding a device backend is one class implementing AudioBackend, and nothing
// in this file changes.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "core/dsp/types.h"
#include "core/engine/engine.h"
#include "core/engine/spsc_ring.h"
#include "core/engine/vrx.h"
#include "core/engine/wav_writer.h"
#include "core/error.h"

namespace revenant::engine {

// The slot table is a fixed array because publish() resolves a receiver on
// the producer thread and must not chase a container that another thread can
// reallocate. 64 matches kMaxRingConsumers in core/engine/ring_consumer.h and
// leaves room above the fifty receiver figure the M1 acceptance test uses.
inline constexpr std::uint32_t kMaxAudioReceivers = 64;

// Mono or stereo. Stereo exists for WFM; nothing in the engine produces more,
// and the WAV writer refuses more for its own reasons.
inline constexpr std::uint32_t kMaxAudioChannels = 2;

// What a receiver's audio stream is, fixed for the life of the registration.
//
// A change to the rate, the channel count or the demodulator is a new
// registration and, for a file backend, a new file. Changing them underneath
// an open recording would produce a file whose header describes the first
// second of it.
struct AudioStreamInfo {
    VrxId vrx;

    dsp::SampleRate rate = 0;
    std::uint32_t channels = 1;

    // Carried so a recording can be traced back to what made it. See the
    // metadata argument in core/engine/wav_writer.h.
    dsp::Hertz center = 0;
    Demod demod = Demod::Nfm;

    // Index of the first audio sample expected, at `rate`. Time is a sample
    // index everywhere in this engine, and this is where a recording attaches
    // to that timeline.
    dsp::SampleIndex start = 0;

    // Unix epoch nanosecond of audio sample 0 of the stream. Zero when the
    // source cannot supply one, which is honest and is not the same as the
    // epoch.
    std::int64_t epoch_anchor_ns = 0;

    std::string label;
    std::string source_uri;

    // ISO 8601. Supplied by the caller because nothing in the DSP path reads
    // a clock: a capture replayed faster than realtime has to record the
    // capture's time, not the replay's.
    std::string created;
};

// The reader's end of one receiver's ring.
//
// Handed to a Pull backend so a device callback can read without going
// through AudioEgress at all. Holds a borrowed pointer: it is valid from
// AudioBackend::attach() until AudioBackend::close() returns, and the
// contract on close() below is what makes that bound real.
//
// THREAD SAFETY. One reader thread, like the ring underneath it. Nothing here
// allocates, locks or blocks.
class AudioTap {
public:
    AudioTap() = default;

    // Constructed by the egress layer. A backend receives one and does not
    // build one.
    AudioTap(SpscRing<float>* ring, dsp::SampleRate rate, std::uint32_t channels)
        : ring_(ring), rate_(rate), channels_(channels)
    {
    }

    [[nodiscard]] bool valid() const { return ring_ != nullptr; }
    [[nodiscard]] dsp::SampleRate rate() const { return rate_; }
    [[nodiscard]] std::uint32_t channels() const { return channels_; }

    // Interleaved samples, not frames. Returns how many were real.
    std::size_t read(std::span<float> out) { return ring_ == nullptr ? 0 : ring_->read(out); }

    // What a device callback calls. Fills the shortfall with silence and
    // counts the underrun, because the listener has already heard it and
    // counting is the only useful response.
    std::size_t read_or_fill(std::span<float> out)
    {
        if (ring_ == nullptr) {
            std::fill(out.begin(), out.end(), 0.0F);
            return 0;
        }
        return ring_->read_or_fill(out, 0.0F);
    }

    [[nodiscard]] std::size_t readable_frames() const
    {
        return ring_ == nullptr || channels_ == 0 ? 0 : ring_->readable() / channels_;
    }

    // Caps the backlog by discarding the oldest, from the side that owns the
    // read cursor. A device that has been starved and then over-filled is
    // holding audio the listener will hear late; see the note on trim_to() in
    // core/engine/spsc_ring.h for why this cannot be done from the writer.
    std::size_t trim_to_frames(std::size_t max_backlog_frames)
    {
        return ring_ == nullptr ? 0 : ring_->trim_to(max_backlog_frames * channels_);
    }

    [[nodiscard]] std::uint64_t underrun_samples() const
    {
        return ring_ == nullptr ? 0 : ring_->underrun_samples();
    }
    [[nodiscard]] std::uint64_t underrun_events() const
    {
        return ring_ == nullptr ? 0 : ring_->underrun_events();
    }

private:
    SpscRing<float>* ring_ = nullptr;
    dsp::SampleRate rate_ = 0;
    std::uint32_t channels_ = 1;
};

// Which way the audio moves at the far end.
enum class AudioBackendKind : std::uint8_t {
    // The drain thread calls write(). Files, network sinks, anything that is
    // told when to accept data.
    Push,

    // The backend's own thread reads an AudioTap on the device's clock.
    // WASAPI, ALSA, PipeWire, CoreAudio.
    Pull,
};

// Where one receiver's audio goes.
//
// THREAD SAFETY, per method:
//
//   open, attach, close   the control thread, one at a time, never
//                         concurrently with each other. May allocate and may
//                         block.
//   write                 the egress drain thread only, Push backends only.
//                         Must not be called after close(). May block; must
//                         not allocate if the backend wants the drain
//                         thread's no-allocation property to hold.
//
// close() carries the one obligation that is easy to miss and impossible to
// debug afterwards: it must not return until no thread belonging to the
// backend can touch its AudioTap again. The egress layer frees the ring
// immediately afterwards, and a device callback that fires once more reads
// freed memory. Stopping the device stream is part of closing it.
class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    AudioBackend(const AudioBackend&) = delete;
    AudioBackend& operator=(const AudioBackend&) = delete;
    AudioBackend(AudioBackend&&) = delete;
    AudioBackend& operator=(AudioBackend&&) = delete;

    [[nodiscard]] virtual AudioBackendKind kind() const = 0;

    // Called once, before anything else, with the stream this backend is
    // about to carry. A backend that cannot serve that rate or channel count
    // fails here rather than degrading quietly.
    [[nodiscard]] virtual Status open(const AudioStreamInfo& info) = 0;

    [[nodiscard]] virtual Status close() = 0;

    // Push only. Interleaved, always a whole number of frames.
    //
    // Returning an error is how this says no, and a throw is caught rather
    // than forbidden. The caller is the drain thread, which is a std::thread,
    // so an escaping exception would be std::terminate; the catch at the call
    // site turns one into the same recorded fault a returned Error produces,
    // and the what() string is carried through. An implementation should
    // still return the error, because the message it builds that way is the
    // one it chose.
    [[nodiscard]] virtual Status write(std::span<const float>)
    {
        return fail("this audio backend is not a Push backend and has no write()");
    }

    // Push only, and the control thread rather than the drain thread. Makes
    // what has been written durable and complete: for the WAV backend it is
    // what refreshes the header, so the file on disk is playable without the
    // writer having been closed.
    [[nodiscard]] virtual Status flush() { return {}; }

    // Pull only. Called once, after open(), with the tap the backend's own
    // thread reads.
    [[nodiscard]] virtual Status attach(const AudioTap&)
    {
        return fail("this audio backend is not a Pull backend and has no attach()");
    }

protected:
    AudioBackend() = default;
};

// The file backend, which is the whole of M1's egress.
//
// Takes the path and the WAV options; the rate, the channel count and every
// metadata field come from the AudioStreamInfo at open().
[[nodiscard]] Expected<std::unique_ptr<AudioBackend>> make_wav_file_backend(
    std::string path, const WavWriteOptions& options = {});

struct AudioEgressConfig {
    // Ring capacity per receiver, in seconds of audio. Rounded up to a power
    // of two of samples by SpscRing. This is how long the drain thread may be
    // stalled on the filesystem before samples start being dropped, so it is
    // sized against a disk hiccup rather than against latency: nothing waits
    // for this buffer to fill.
    double ring_seconds = 2.0;

    // Frames the drain thread moves in one pass per receiver. The scratch
    // buffer for this is allocated by start(), on the control thread.
    std::size_t drain_frames = 4096;

    // Caps the backlog a Push backend is allowed to accumulate, discarding
    // the oldest beyond it. Zero, the default, never trims: for a recording,
    // late audio is still audio and nothing is improved by throwing it away.
    // A live monitor sets this, because audio that is four seconds behind is
    // not worth hearing.
    double max_backlog_seconds = 0.0;

    // How long the drain thread sleeps when every ring is empty. It is not on
    // a device clock and nothing waits for it, so this trades wakeups against
    // how long audio sits in a ring before reaching the disk.
    std::chrono::microseconds idle_poll{1000};

    // A hole in the sample index up to this long is filled with silence so
    // the recording stays aligned with the stream's sample index, which is
    // the authority everything else is correlated against. A larger jump is
    // treated as a discontinuity, a seek or a retune rather than a loss, and
    // the writer simply resynchronises. Zero disables filling.
    double max_gap_fill_seconds = 2.0;
};

// Per receiver, and all of it in frames rather than interleaved samples: a
// dropout is a duration the operator heard, and "4800 samples" of a stereo
// stream is half of what it sounds like.
struct AudioEgressStats {
    VrxId vrx;
    bool active = false;
    dsp::SampleRate rate = 0;
    std::uint32_t channels = 1;

    // Accepted into the ring by publish().
    std::uint64_t frames_published = 0;

    // Offered to publish() and refused because the ring was full. This
    // receiver's loss, on this egress, and nowhere else.
    //
    // WHAT THIS COMMENT USED TO SAY: "this is the number that belongs in
    // VrxStatus::audio_dropped". It does not, on two counts. Nothing carries
    // it there and nothing can: AudioEgress is built above Engine and has no
    // route back into a receiver's slot, and adding one would mean the graph
    // taking a number from a layer it does not know about. And it would not
    // fit if it did. A receiver can have more than one consumer now, per the
    // composition seam in core/engine/engine.h, so one receiver produces one
    // of these per egress plus one per wire subscription, and there is a
    // single field over there to hold them. VrxStatus::audio_dropped has been
    // narrowed to the engine's own loss for that reason and says so.
    std::uint64_t frames_dropped = 0;
    std::uint64_t drop_events = 0;

    // Silence inserted to cover a hole in the sample index. Not a drop made
    // here: it is a drop made upstream, made visible and made harmless to the
    // recording's timeline.
    std::uint64_t frames_filled = 0;
    std::uint64_t gap_events = 0;

    // Jumps too large to fill, so a seek or a retune rather than a loss.
    std::uint64_t discontinuities = 0;

    // Discarded from the oldest end by the backlog cap.
    std::uint64_t frames_trimmed = 0;

    // Handed to the backend and accepted by it.
    std::uint64_t frames_written = 0;

    // Sitting in the ring right now.
    std::uint64_t backlog_frames = 0;

    // Pull backends only: what the device callback found missing.
    std::uint64_t underrun_frames = 0;
    std::uint64_t underrun_events = 0;

    // Set when the backend returned an error. The receiver keeps running and
    // keeps counting drops; one receiver's disk filling up does not stop the
    // other forty-nine.
    bool faulted = false;
    std::string fault;
    long long fault_code = 0;
};

class AudioEgress {
public:
    [[nodiscard]] static Expected<std::unique_ptr<AudioEgress>> create(
        const AudioEgressConfig& config = {});

    virtual ~AudioEgress() = default;

    AudioEgress(const AudioEgress&) = delete;
    AudioEgress& operator=(const AudioEgress&) = delete;
    AudioEgress(AudioEgress&&) = delete;
    AudioEgress& operator=(AudioEgress&&) = delete;

    // --- the control thread -------------------------------------------------
    //
    // All of these allocate, some of them block, and they serialise against
    // each other. None of them may be called from the producer thread or from
    // a backend.

    // Allocates the ring, opens the backend and makes the receiver
    // publishable. Legal while running.
    [[nodiscard]] virtual Status add_receiver(const AudioStreamInfo& info,
                                              std::unique_ptr<AudioBackend> backend) = 0;

    // Stops accepting for this receiver, waits for any producer that is
    // inside publish() to leave, drains what is left into the backend and
    // closes it. Blocks for as long as that takes, which for a file is a
    // flush.
    [[nodiscard]] virtual Status remove_receiver(VrxId id) = 0;

    // Spawns the drain thread. Adding receivers before or after is equally
    // fine.
    [[nodiscard]] virtual Status start() = 0;

    // Joins the drain thread, then drains and flushes every backend. The
    // files on disk are complete and readable when this returns; they are not
    // closed, so a later start() resumes into the same recordings.
    [[nodiscard]] virtual Status stop() = 0;

    [[nodiscard]] virtual bool running() const = 0;

    // A sink to hand to Engine::set_audio_sink. Calling it is exactly
    // publish(), so everything said about that applies.
    [[nodiscard]] virtual Expected<AudioSink> sink_for(VrxId id) = 0;

    [[nodiscard]] virtual Expected<AudioEgressStats> stats(VrxId id) const = 0;
    [[nodiscard]] virtual std::vector<AudioEgressStats> all_stats() const = 0;

    // --- the producer thread ------------------------------------------------

    // Copies a chunk into the receiver's ring.
    //
    // Never blocks, never allocates on the success path, and never returns an
    // error for a full ring: a drop is counted, not reported, because the
    // caller cannot do anything about it and stopping the stream over one
    // would be worse than the dropout. An error here means the chunk was
    // addressed to a receiver that is not registered, or its channel count or
    // rate disagrees with the registration, both of which are wiring
    // mistakes.
    [[nodiscard]] virtual Status publish(const AudioChunk& chunk) = 0;

protected:
    AudioEgress() = default;
};

}  // namespace revenant::engine
