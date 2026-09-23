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

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "audio/audio_ring.h"
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
    MixPull pull(std::span<AudioRing* const> rings, const MixControl& control, float* out,
                 std::size_t frames);

    // Times a stream was moved to where the lead puts it, since this mix
    // was made. The first placement of each stream is not counted.
    [[nodiscard]] std::uint64_t alignments() const { return alignments_; }

private:
    struct Stream {
        RingFormat format;
        bool multiplex = false;
        bool level = false;
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

    // Reads `count` output frames of the stream into stream.rendered.
    // Answers the lead's source.
    FrameSource render(Stream& stream, AudioRing& ring, std::size_t count);

    // Moves the stream to `position` and forgets what it had read.
    void place(Stream& stream, double position);

    std::uint32_t out_rate_;
    int out_channels_;
    std::vector<Stream> streams_;
    std::vector<float> sum_;
    SoftLimiter limiter_;
    std::uint64_t alignments_ = 0;
};

}  // namespace revenant::ui
