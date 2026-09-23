// The rack's mix, at the sound card's rate: every heard receiver resampled
// to it, placed on one timeline by its sample index, levelled where its mode
// needs it, scaled by its strip, summed and limited.
//
// Qt-free, so ui/tests drives it against AudioRings with no sound card. It is
// run on the sink's pull thread, by RingSource in audio/audio_player.cpp, and
// holds state across pulls; one AudioMix belongs to one open sink.
//
// WHAT THIS REPLACED, and the three decisions the old mix made that the owner
// changed on 2026-09-23 by approving the best-effort mix:
//
//   Format. The sink was opened at the focused receiver's rate and channel
//   count, and a ring at another rate or channel count was left out of the
//   mix. A wfm receiver raised to 171000 S/s for RDS then asked the device
//   for a rate no shared-mode output takes, and a stereo wfm receiver
//   focused left every mono receiver out. Now the sink is opened at the
//   device's own rate and channel count, and every stream is resampled and
//   mapped to it.
//
//   Alignment. Each ring played from its own head, so two receivers on one
//   transmission could be a ring's depth, 200 ms, apart. Now one stream
//   leads, the focused receiver's when it is heard, and every other stream is
//   read at the index that falls at the same instant, found through the
//   arrival anchors AudioRing::write keeps. A stream more than kAlignTolerance
//   off where it should be is moved there, which is counted; one whose audio
//   has not arrived for that instant plays silence for it and its late frames
//   are skipped rather than played behind the others.
//
//   Level. A plain sum that could exceed full scale and clip in the device.
//   Now a soft limiter holds the sum under -1 dBFS; see SoftLimiter.
//
// WHAT THE LEAD DOES DIFFERENTLY. The lead is read the way the single ring
// always was: from its oldest buffered frame, and never past its newest. When
// it has run out the whole mix holds, silence is written for the rest of the
// pull and counted on the lead's ring as starved, and every stream picks up
// from the same instant when the lead's audio arrives. A lead that is instead
// behind a hole in its own ring, its frames evicted before they were played,
// is moved up to what is there.
//
// DRIFT. The engine's clock and the card's are not the same clock, so a
// stream read at exactly its nominal rate fills or drains its ring by the
// difference, until the ring evicts or the lead starves. Every stream is read
// at DriftTrim::ratio() times its nominal step, a few hundred ppm either way
// at most, chosen by a slow loop on the lead's fill; audio/drift_trim.h has
// the loop and why it is inaudible. A stream at the device's rate is copied
// while that ratio is exactly one and read through the resampler's kernel
// otherwise.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "audio/audio_ring.h"
#include "audio/drift_trim.h"
#include "audio/mix_stages.h"
#include "audio/resampler.h"

namespace revenant::ui {

// What the mix is told about one rack slot on a pull.
struct MixSlot {
    // A heard receiver's subscription writes into this slot's ring.
    bool heard = false;

    // The strip's gain, as an amplitude.
    float gain = 1.0F;

    // The receiver's mode hands out audio at the level the signal came in at:
    // am, usb, lsb, dsb, cw. See LevelAgc.
    bool level = false;

    // The receiver is wfm. A wfm stream at or above kMultiplexRateHz is the
    // multiplex rather than programme audio, and is filtered to 15 kHz and
    // de-emphasised on the way in. See Deemphasis.
    bool wfm = false;
};

struct MixControl {
    // The slot the focused receiver's stream is in when it is heard, -1
    // otherwise, in which case the first heard slot with audio leads.
    int lead = -1;
    std::span<const MixSlot> strips;
};

// What one pull did, for the status line.
struct MixPull {
    // The slot that led, -1 when nothing had audio.
    int lead = -1;

    // What the last frame from the lead was. Starved when the pull ran past
    // the lead's audio, which is the one of the causes a mix can add.
    FrameSource lead_source = FrameSource::idle;

    // Frames of this pull the lead's audio covered.
    std::size_t frames_played = 0;

    // The limiter turned something down.
    bool limited = false;

    // The lead's buffered audio after this pull, in seconds of its stream:
    // its ring's newest index less the playhead.
    double fill_seconds = 0.0;

    // What DriftTrim was handed: the lead's lag behind its arrivals when the
    // pull was timed and the lead anchored, and fill_seconds otherwise.
    //
    // WHY NOT THE FILL ALONE. Audio arrives a chunk at a time, 27 ms of it
    // at the shipped block size, and the card takes it 10 ms at a time, so
    // the fill read at a pull is a sawtooth that deep with the arrivals'
    // jitter on its phase, and what is left of it after DriftTrim's average
    // reaches the trim. The lag is the pull's clock reading less the arrival
    // anchor, less the playhead's stream time: the same quantity measured
    // against the anchor's line through the earliest arrivals rather than
    // against the last chunk that happened to land, and it has no sawtooth.
    // Measured on ten simulated minutes of an engine 100 ppm slow in
    // ui/tests/test_drift_trim.cpp: holding the fill, the trim wandered
    // 28 ppm either side of the drift after three minutes and the averaged
    // level 1.5 ms; holding the lag, 5 ppm and 1.3 ms.
    double lag_seconds = 0.0;
    bool timed = false;
};

class AudioMix {
public:
    // A wfm receiver at or above this rate is handing out the multiplex.
    // core/engine/vrx.h's kCompositeAudioRateHz, copied because this process
    // links no part of the engine.
    static constexpr std::uint32_t kMultiplexRateHz = 114'000;

    // The programme band of a multiplex, and where its stopband has to be:
    // 15 kHz of audio and the 19 kHz pilot.
    static constexpr double kProgrammeTopHz = 15'000.0;
    static constexpr double kPilotHz = 19'000.0;

    // How far a stream may sit from where the lead puts it before it is moved,
    // in seconds. The anchors carry a millisecond or so of jitter between
    // them, and moving a stream on that would click every few seconds.
    static constexpr double kAlignTolerance = 0.005;

    AudioMix(std::uint32_t out_rate, int out_channels);

    [[nodiscard]] std::uint32_t out_rate() const { return out_rate_; }
    [[nodiscard]] int out_channels() const { return out_channels_; }

    // Fills `frames` frames of `out`, interleaved at out_channels(), from
    // `rings`, one per slot and as many as control.strips has.
    //
    // now_ns is the pull's own reading of the clock AudioRing::write's
    // arrivals were taken on, or AudioRing::kNoArrival. With it, and an
    // arrival anchor on the lead, the drift loop holds the lead's lag behind
    // its arrivals rather than its ring's fill; see lag_seconds.
    MixPull pull(std::span<AudioRing* const> rings, const MixControl& control, float* out,
                 std::size_t frames, std::int64_t now_ns = AudioRing::kNoArrival);

    // Times a stream was moved to where the lead puts it, since this mix
    // was made. The first placement of each stream is not counted.
    [[nodiscard]] std::uint64_t alignments() const { return alignments_; }

    // The loop holding the lead's fill against the two clocks' drift. Every
    // stream is read at drift().ratio() times its nominal step, the lead
    // and the streams aligned to it alike, since they all come off the
    // engine's one clock. See audio/drift_trim.h.
    [[nodiscard]] const DriftTrim& drift() const { return drift_; }

    // Off reads every stream at its nominal step, as the mix did before the
    // loop, and is there so a test can show what the loop is holding back.
    void set_drift_correction(bool on) { correct_drift_ = on; }

private:
    struct Stream {
        RingFormat format;
        bool multiplex = false;
        bool level = false;

        // At the device's rate with nothing asked of the filter, so it is
        // copied while the trim is exactly zero and read through the kernel
        // otherwise. See copying().
        bool copyable = false;
        Resampler resampler;
        LevelAgc agc;
        Deemphasis deemphasis;

        // Where the next output frame reads, as an absolute index into the
        // stream, fractional between input frames.
        double position = 0.0;
        bool placed = false;

        // The input frames the resampler reads, the first of them at
        // absolute index history_start. Empty for a passthrough, which reads
        // straight into `rendered`.
        std::vector<float> history;
        std::uint64_t history_start = 0;

        // This pull's frames at the stream's own channel count.
        std::vector<float> rendered;
    };

    // Rebuilds the stream's state when its format or treatment changed.
    void prepare(Stream& stream, RingFormat format, bool multiplex, bool level);

    // Reads `count` output frames of the stream into stream.rendered, at
    // `ratio` times its nominal step. Answers the lead's source.
    FrameSource render(Stream& stream, AudioRing& ring, std::size_t count, double ratio);

    // Moves the stream to `position` and forgets what it had read.
    void place(Stream& stream, double position, double ratio);

    // Whether this pull copies the stream frame for frame: at the device's
    // rate, untrimmed, on a whole frame.
    [[nodiscard]] static bool copying(const Stream& stream, double ratio);

    // Input frames per output frame this pull.
    [[nodiscard]] static double step_of(const Stream& stream, double ratio);

    std::uint32_t out_rate_;
    int out_channels_;
    std::vector<Stream> streams_;
    std::vector<float> sum_;
    SoftLimiter limiter_;
    std::uint64_t alignments_ = 0;

    DriftTrim drift_;
    bool correct_drift_ = true;
    int last_lead_ = -1;
    bool last_timed_ = false;
};

}  // namespace revenant::ui
