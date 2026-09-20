// The one place an audio chunk crosses from the Cap'n Proto event loop onto
// the thread that feeds the sound card.
//
// It holds no Qt type and no QAudioSink. That is what lets ui/tests drive it
// with no audio device present, and it is the only part of the audio path
// that has a test behind it: everything above this is QAudioSink talking to
// WASAPI, which cannot be asserted on from a test binary.
//
// THE THREE THREADS, AND WHICH TWO TOUCH THIS
//
// core/rpc/client.h is explicit that the audio callback is invoked ON the
// event loop thread, must not call back into the same Client, and must
// return quickly, because everything else that connection does is waiting
// behind it. write() is what that callback calls and it does one thing: it
// works out where the chunk belongs in the timeline and memcpys it in.
//
// read() is called from whatever thread QAudioSink pulls on, which on the
// Windows backend is a thread Qt owns and runs at a raised priority. The Qt
// GUI thread touches neither, and reads counts() for the display.
//
// WHY A MUTEX AND NOT A LOCK-FREE RING, AND WHAT THAT COSTS
//
// One std::mutex guards the whole object. The consequence, stated rather
// than implied: THE RPC EVENT LOOP THREAD CAN BLOCK HERE, for as long as
// the audio thread holds the lock, and the audio thread can block for as
// long as the event loop thread holds it. Nothing else this connection does
// makes progress while the first of those is happening, and the sound card
// underruns if the second runs long.
//
// It is bounded by what is done under the lock, and what is done under the
// lock is a memcpy of a few kilobytes and some integer arithmetic. No
// allocation on the ordinary path, no call into Qt, no call into the
// Client, no system call. At the shipped shape, a 16384-sample source block
// against a 48 kHz receiver is a chunk every 27 ms or so and the sink pulls
// a few hundred frames every few milliseconds, so the two threads meet a few
// hundred times a second and hold the lock for microseconds each time.
//
// A single-producer single-consumer lock-free ring would remove even that,
// and it was refused. The write is not a fixed-length append: a chunk that
// arrives with a gap in front of it writes the gap's silence and then the
// chunk, as one placement that either fits or triggers a resync, and it
// writes a second parallel ring describing what each frame is. That is
// three wrapped copies and a branch that reads the buffered count, and
// making it correct without a lock means acquire/release reasoning over
// four indices for the sake of a lock held across a memcpy. The cost of
// being wrong there is a fault that appears as an occasional click on one
// machine.
//
// THE ONE RULE FOR CALLERS. Never hold this lock and then call QAudioSink,
// the Client, or anything that can block. Both of the threads above are
// threads something is waiting on.
//
// WHAT IS ALLOCATED, AND WHEN
//
// Nothing, except when the format changes. The buffers are sized when the
// first chunk arrives, because the depth was granted in milliseconds and
// this object cannot turn milliseconds into frames before it knows the
// receiver's audio rate, which is the same reason AudioStats::bufferFrames
// is zero until then. So one allocation lands on the event loop thread on
// the first chunk of a stream and on any later chunk that changes the rate
// or the channel count. A receiver whose rate changed is a remove and an
// add on the engine's side, so that is once per gesture and not once per
// chunk.

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "core/rpc/types.h"

namespace revenant::ui {

// What a frame handed to the sound card actually is. Zeros reach the card
// from three different causes and they are three different faults; a level
// meter cannot tell them apart and this can.
//
// Switched over in ui/models/audio_link.cpp with no default label, so a new
// enumerator raises C4062 there. See cmake/CompilerFlags.cmake.
enum class FrameSource : std::uint8_t {
    // Nothing has been played yet. Not silence: the stream has not started.
    idle,

    // The demodulator produced these and the squelch was open.
    audio,

    // Zeros the ENGINE wrote because the gate was shut. AudioChunk carries
    // squelch_open per chunk for exactly this, and the chunk still crosses
    // at the full rate, so the timeline is whole and the band is simply
    // being held closed.
    gated,

    // Zeros THIS object wrote to keep the timeline honest across a gap. The
    // audio either side of them is real and the instant between them was
    // lost, upstream or on the wire.
    gap_fill,

    // Zeros the READER invented because the ring was empty when the card
    // asked. Nothing was lost: the audio has not arrived yet. This is the
    // one of the three that means the network or the engine is behind, and
    // the other two mean it is not.
    starved,
};

// The stream's shape, which is on every chunk rather than cached from
// VrxStatus for the reason core/rpc/types.h gives: a pane outlives the
// receiver behind it and the next receiver can be at another rate.
struct RingFormat {
    std::uint32_t sample_rate = 0;
    std::uint16_t channel_count = 0;

    [[nodiscard]] bool valid() const { return sample_rate > 0 && channel_count > 0; }

    [[nodiscard]] bool operator==(const RingFormat&) const = default;
};

// Everything this object has counted since the last reset. Split by cause,
// because the fixes are different: a wire gap is a client that cannot keep
// up, an upstream gap is the engine dropping before it reached the wire, an
// overrun is this ring being handed audio faster than the card takes it, and
// a starve is the opposite.
struct RingCounts {
    // Frames taken from chunks, which is the real audio plus the engine's
    // own squelched zeros.
    std::uint64_t frames_written = 0;

    // Silence written here to keep the timeline whole across a gap.
    std::uint64_t frames_filled = 0;

    // The gap, split the way the schema asks for it to be split.
    // AudioChunk::frames_dropped_before is what the SERVER's queue evicted,
    // so a sample_index gap of exactly that much was lost on the wire and
    // anything beyond it was lost upstream in the engine.
    std::uint64_t frames_gap_wire = 0;
    std::uint64_t frames_gap_upstream = 0;

    // Gap frames that were NOT filled, because the gap was longer than the
    // whole ring. See the resync note on write().
    std::uint64_t frames_gap_discarded = 0;

    // Frames evicted from the FRONT because a write did not fit. Front and
    // not back, matching the engine's own queue: late audio is worse than
    // no audio when the point is to hear what the radio is doing now.
    std::uint64_t frames_overrun = 0;

    // Frames of silence the reader invented because the ring was empty.
    std::uint64_t frames_starved = 0;

    // Times a gap was seen at all, and times one was too long to fill. One
    // two-second stall and four hundred scattered hitches lose the same
    // frames and sound nothing alike, which is the argument AudioStats
    // makes for drop_events and it holds here.
    std::uint64_t gap_events = 0;
    std::uint64_t resyncs = 0;

    // Times a chunk arrived with a sample index BEHIND where the stream
    // was. The engine's invariant says this cannot happen inside one
    // receiver's stream; it does happen when the id is reused, and the
    // baseline is restarted rather than the chunk being spliced.
    std::uint64_t restarts = 0;

    // Chunks refused because sample_rate or channel_count was zero, which
    // would make frames() a divide by zero or a rate the card cannot be
    // opened at.
    std::uint64_t malformed_chunks = 0;
};

// What one pull off the ring did.
struct ReadResult {
    // Frames copied out of the ring, and frames of silence written past
    // them because the ring ran out. They sum to the frames asked for: the
    // reader always fills the whole request, because a short read is what
    // QAudioSink treats as the end of the stream and it stops the sink.
    std::size_t frames_from_ring = 0;
    std::size_t frames_starved = 0;

    // The format the ring held UNDER THE LOCK THIS READ TOOK, which is the
    // only report of it a reader can act on. See THE FORMAT AND THE
    // SAMPLES COME BACK TOGETHER on read().
    RingFormat format;

    // The ring's format is not the one the caller asked to read at, so
    // nothing was copied and the caller's buffer was not touched. The
    // frames are counted as starved because that is what the card plays.
    bool format_moved = false;

    // What the LAST frame handed over was, which is what is about to come
    // out of the speaker rather than what most recently arrived. That is
    // the whole reason the sources are carried through the ring: an
    // indicator driven off the newest chunk leads the sound by the ring's
    // whole depth.
    FrameSource last_source = FrameSource::idle;
};

class AudioRing {
public:
    AudioRing() = default;

    AudioRing(const AudioRing&) = delete;
    AudioRing& operator=(const AudioRing&) = delete;
    AudioRing(AudioRing&&) = delete;
    AudioRing& operator=(AudioRing&&) = delete;

    // The depth to size the ring at, in milliseconds, which is the value
    // Client::subscribe_audio GRANTED and never the value that was asked
    // for. See the note above the definition for why the two rings are the
    // same depth rather than stacked.
    //
    // Takes effect at the next format change, because that is when the ring
    // is allocated and it is the first moment milliseconds can be turned
    // into frames. Call it before subscribing.
    void set_depth_millis(std::uint32_t millis);

    // Producer. Called on the Cap'n Proto event loop thread, from the
    // callback Client::subscribe_audio was given.
    //
    // THE GAP DECISION, WHICH IS THE WHOLE OF WHAT THIS FUNCTION DECIDES
    //
    // AudioChunk::sample_index is absolute, so the frames missing between
    // the previous chunk and this one are
    //
    //     sample_index - (previous index + previous frame count)
    //
    // and exactly that many frames of silence are written before the chunk.
    // The alternative, splicing the chunk on directly, was refused: it
    // shortens the timeline by every frame ever lost, and a receiver losing
    // 300 ms a minute then plays a second early after three minutes with
    // nothing on screen saying so. Drift is inaudible and cumulative. A
    // click is audible once and costs nothing afterwards, and the counters
    // beside it say how much was lost and where.
    //
    // THE ONE GAP THAT IS NOT FILLED. A gap longer than the ring's whole
    // capacity cannot be written without evicting live audio to make room
    // for silence, which is silence winning an argument against sound. That
    // case drops everything buffered, writes the chunk alone, counts the
    // gap in frames_gap_discarded and raises resyncs. The timeline jumps
    // there, and the resync counter is the only thing that says so, which
    // is why it is on screen rather than only in here.
    void write(const rpc::AudioChunk& chunk);

    // Consumer. Called on whatever thread QAudioSink pulls on.
    //
    // THE FORMAT AND THE SAMPLES COME BACK TOGETHER, WHICH IS WHY expect IS
    // AN ARGUMENT
    //
    // expect is the format the CALLER's buffer is sized for, and it is
    // compared against the ring's inside the one lock this call takes. The
    // reader is QAudioSink's own pull thread and the writer is the Cap'n
    // Proto event loop, so a rate or channel-count change genuinely lands
    // between any two calls a reader makes. Asking format() and then
    // read() is two locked calls that can disagree, and the disagreement
    // is a buffer sized for one channel count filled at another.
    //
    // WHAT THE READER DOES WHEN IT FINDS THE FORMAT MOVED. Nothing is
    // copied, nothing is consumed, THE CALLER'S BUFFER IS LEFT UNTOUCHED,
    // and the result carries format_moved with the ring's new format in
    // format. The buffer is left alone rather than zeroed for the same
    // reason the no-stream case leaves it alone: only the caller knows how
    // many floats the sink asked for, because the sink's channel count is
    // not always the ring's. So the caller fills its own silence at its
    // own width and waits for its owner to reopen at the new format. The
    // whole request is counted as starved, because starved is what the
    // card is about to play.
    //
    // ALWAYS FILLS THE WHOLE REQUEST OTHERWISE, padding with zeros when
    // the ring is short. A QIODevice that returns fewer bytes than asked
    // for is a QAudioSink that goes Idle and stops, so a network hiccup
    // would end the stream rather than dip it. The padding is counted and
    // reported as FrameSource::starved, so a dip is visible instead of
    // merely inaudible.
    //
    // out holds frames * expect.channel_count floats.
    //
    // THE OTHER CASE THAT LEAVES THE BUFFER ALONE: with no format
    // established there is no channel count, so this object does not know
    // how long the caller's buffer is. A caller takes its format from a
    // previous read or from format() and has nothing to open a sink with
    // at that point, so it does not reach here; RingSource checks anyway.
    [[nodiscard]] ReadResult read(float* out, std::size_t frames,
                                  const RingFormat& expect);

    // Forgets the stream and everything buffered, and leaves the counters
    // where they are: they describe one subscription and reset() is called
    // when one ends. Use reset_counts() for a new subscription.
    void reset();

    // Both, which is what starting a subscription wants.
    void reset_counts();

    [[nodiscard]] RingFormat format() const;

    // Bumped on every format change, so a consumer holding an open sink can
    // tell that it has to reopen without comparing structs under its own
    // lock. Zero before the first chunk.
    [[nodiscard]] std::uint64_t format_generation() const;

    [[nodiscard]] RingCounts counts() const;
    [[nodiscard]] std::size_t frames_buffered() const;
    [[nodiscard]] std::size_t capacity_frames() const;

private:
    // All five of these want the lock held.
    void establish(RingFormat incoming);
    void grow_to(std::size_t frames);
    void push(const float* samples, std::size_t frames, FrameSource source);
    void push_silence(std::size_t frames, FrameSource source);
    void evict_front(std::size_t frames);

    mutable std::mutex mutex_;

    // Interleaved, capacity_ * channels floats, and sources_ is one byte per
    // FRAME rather than per sample. A byte a frame is 9.6 KiB at 48 kHz and
    // a 200 ms depth, which buys an indicator that follows what the speaker
    // is playing instead of what the wire last delivered.
    std::vector<float> samples_;
    std::vector<std::uint8_t> sources_;

    std::size_t capacity_ = 0;  // frames
    std::size_t head_ = 0;      // frame index of the oldest buffered frame
    std::size_t buffered_ = 0;  // frames

    RingFormat format_;
    std::uint64_t format_generation_ = 0;
    std::uint32_t depth_millis_ = 0;

    // Where the stream is up to: the sample index the next contiguous chunk
    // would carry. have_stream_ is false before the first chunk of a
    // format, because there is no previous index to measure a gap against
    // and the first chunk's own index is not a gap. The schema says so
    // outright: a demodulator produces nothing until its filter support is
    // inside real samples, so the stream opens with a silent interval that
    // is not a loss.
    bool have_stream_ = false;
    std::uint64_t next_index_ = 0;

    FrameSource last_source_ = FrameSource::idle;
    RingCounts counts_;
};

}  // namespace revenant::ui
