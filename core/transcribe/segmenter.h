// One receiver's audio cut into utterances for the recogniser.
//
// WHAT GOES IN AND WHAT COMES OUT. Chunks of a receiver's audio, in stream
// order, each saying whether its gate was open; Utterances at 16000 S/s mono,
// each covering a stretch of the receiver's stream and placed on the
// engine's source clock. The caller fills in which receiver, where it was
// tuned and the call's fields; this file knows none of that, and nothing of
// Qt or core/rpc, so tests/transcribe links it alone.
//
// ONE RULE FOR ALL THREE KINDS, AND A DETECTOR IN FRONT OF ONE. Call and
// Squelch take the gate from the chunk: a digital voice receiver's decoder
// says when a frame carried voice, an analogue receiver's squelch says when
// it was open. Level has no gate to read, so it makes one, a frame at a time,
// from the energy of what it hears against a noise floor it tracks; see
// "Level" below. From there all three are the same machine:
//
//   closed, gate opens        open an utterance at that frame
//   open, gate open           append, and anything held from the gap before
//   open, gate shut           hold the samples, up to the hangover
//   open, shut too long       close at the last open frame, drop what was held
//
// The held samples are what lets an utterance carry the pause between two
// words, or a P25 call carry a data unit lost to a fade, without the trailing
// silence of every utterance going to the recogniser.
//
// TIMING IS KEPT THROUGH EVERYTHING. A chunk that starts later than the last
// one ended means chunks were dropped upstream. A gap no longer than the
// hangover leaves is filled with silence, so what follows is where it was in
// time; a longer one closes the utterance. A chunk that starts EARLIER than
// the last one ended means the stream was restarted, and closes it too.
//
// NEVER ONE UTTERANCE ACROSS TWO TUNINGS. A chunk with a new tuning_epoch
// closes what is open before any of its frames is looked at, which is
// exactly the boundary engine::AudioChunk::tuning_epoch exists to mark, and
// throws away the level detector's floor, which belonged to the old
// frequency.
//
// TWENTY-EIGHT SECONDS AT MOST. Whisper's encoder takes a 30 s window and
// core/transcribe/whisper_runner.h refuses anything longer rather than
// stitching windows. An utterance that reaches max_seconds is closed there
// and a new one opens on the very next frame, so a long transmission comes
// out as consecutive utterances that share no sample and miss none. The
// second and later of those are never dropped for being short: they are the
// end of something that was long.
//
// THE COST. push() runs on a server decode thread for every chunk of every
// transcribed receiver. The resampler is the cost, a dot product of a few
// dozen to a few hundred taps per 16 kHz output sample while an utterance is
// open, and nothing while closed: the level detector is a sum of squares and
// a logarithm per 20 ms. Every buffer is sized when the rate is first seen
// and reused, so a steady stream allocates nothing but the utterance's own
// growing pcm.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "core/transcribe/resample.h"
#include "core/transcribe/utterance.h"

namespace revenant::transcribe {

struct SegmenterConfig {
    SegmentBy by = SegmentBy::Call;

    // An utterance whose voiced stretch is shorter than this is dropped: a
    // key-up, a squelch tail. The pre-roll does not count towards it.
    double min_seconds = 0.6;

    // Cut here and carry on in a new utterance; Whisper takes 30 s at most.
    double max_seconds = 28.0;

    // How long the gate may stay shut inside one utterance.
    double hangover_seconds = 0.5;

    // Level only: audio kept from before the detector opened.
    double pre_roll_seconds = 0.2;
};

// LEVEL: THE DETECTOR'S NUMBERS.
//
// Frames of 20 ms. Speech runs at about four syllables a second, so a frame
// is a twelfth of a syllable and the envelope is resolved; much longer and a
// short word fits inside one frame, much shorter and a frame of noise is too
// few samples to measure. The level is the frame's mean square in dB.
inline constexpr double kLevelFrameSeconds = 0.020;

// Open when a frame is this far above the floor: ten times the noise power.
// In steady noise the floor settles on the noise's mean level in dB, and a
// 20 ms frame's level wanders about it by the spread of a mean square over
// its samples (a relative variance of 2/N for N independent ones). Measured
// on 2026-10-03 over ten minutes of white noise at -40 dBFS, from seed
// 20261003: the floor within 0.06 dB of the mean, and no frame more than
// 1.95 dB over it at 8000 S/s (160 samples a frame) or 0.71 dB at 48000. A
// stationary signal stepping up by six dB therefore peaks near 8 dB over
// the floor and does not open. Speech at any useful signal to noise ratio is
// 15 dB and more over the noise at its syllable peaks.
inline constexpr double kLevelOpenMarginDb = 10.0;

// The floor follows a quieter frame down with this time constant, five
// frames: the gaps between syllables and words are 50 to a few hundred ms,
// and in each the floor finds the noise again, so speech cannot carry the
// floor up with it while it lasts.
inline constexpr double kLevelFloorFallSeconds = 0.1;

// And a louder frame up at no more than this. Slow enough that a speaker
// talking without a gap for three seconds at 20 dB over the noise is still
// heard; fast enough that a stationary signal which did open the detector,
// a carrier keyed up 20 dB over the noise, lets it go after about
// (20 - 10) / 3 seconds plus the hangover.
inline constexpr double kLevelFloorRiseDbPerSecond = 3.0;

// A frame quieter than this is digital silence, a gate's or a gap's zeros,
// and says nothing about the noise: it neither opens nor moves the floor.
// Without it a stretch of zeros drags the floor to nothing and the noise
// returning afterwards opens the detector for half a minute.
inline constexpr double kLevelSilenceDb = -100.0;

// A sensible config per kind.
[[nodiscard]] inline SegmenterConfig segmenter_config_for(SegmentBy by)
{
    SegmenterConfig config;
    config.by = by;
    switch (by) {
    case SegmentBy::Call:
        // P25 voice arrives one Logical Link Data Unit at a time, nine 20 ms
        // IMBE frames, 180 ms, and a data unit lost to a fade leaves a gap
        // that long inside a call. 0.5 s bridges two lost in a row and is
        // still well under the pause between one party's unkey and the
        // other's reply.
        config.hangover_seconds = 0.5;
        config.pre_roll_seconds = 0.0;
        break;
    case SegmentBy::Squelch:
        // A weak signal flutters the squelch shut for a few hundred ms at a
        // time inside one transmission.
        config.hangover_seconds = 0.6;
        config.pre_roll_seconds = 0.0;
        break;
    case SegmentBy::Level:
        // The pauses between a speaker's sentences, which should stay one
        // utterance; and a fifth of a second before the open, because the
        // detector opens on the first loud frame and a word's onset is
        // quieter than its vowel.
        config.hangover_seconds = 0.6;
        config.pre_roll_seconds = 0.2;
        break;
    }
    return config;
}

struct SegmenterChunk {
    // Interleaved if channels is 2.
    std::span<const float> samples;
    std::uint32_t channels = 1;

    // The chunk's own rate: 8000 for decoded voice, the receiver's audio
    // rate otherwise (anything from 8000 to 192000).
    std::uint32_t rate = 0;

    // Receiver stream index of the first frame.
    std::uint64_t start = 0;

    // Call: the chunk carried voice; Squelch: squelch open; ignored for
    // Level.
    bool gate = true;

    // One past the last source sample of the engine block that produced
    // this chunk.
    std::uint64_t source_end = 0;

    // Source samples per second, to place the utterance on the source clock.
    double source_rate = 0.0;

    // A change closes any open utterance first.
    std::uint64_t tuning_epoch = 0;
};

class Segmenter {
public:
    explicit Segmenter(SegmenterConfig config) : config_(config) {}

    // Appends every utterance this chunk closed to `out`. Fills pcm
    // (16000 S/s mono), start_sample, end_sample, sample_rate (receiver
    // stream), source_start, source_end and cut_by. Leaves vrx, the tuning,
    // mode, fields and closed_ns for the caller. A chunk with no frames, no
    // channels or no rate is ignored.
    void push(const SegmenterChunk& chunk, std::vector<Utterance>& out)
    {
        if (chunk.channels == 0 || chunk.rate == 0) {
            return;
        }
        const std::size_t frames = chunk.samples.size() / chunk.channels;
        if (frames == 0) {
            return;
        }

        if (have_stream_) {
            if (chunk.tuning_epoch != epoch_ || chunk.rate != rate_) {
                flush(out);
                forget_history();
                have_stream_ = false;
            } else if (chunk.start < next_) {
                flush(out);
                forget_history();
            } else if (chunk.start > next_) {
                bridge(chunk.start - next_, out);
            }
        }
        if (chunk.rate != rate_) {
            configure(chunk.rate);
        }
        have_stream_ = true;
        epoch_ = chunk.tuning_epoch;

        anchor_end_ = chunk.start + frames;
        anchor_source_end_ = chunk.source_end;
        anchor_source_rate_ = chunk.source_rate;

        const std::span<const float> mono = mix(chunk, frames);
        if (config_.by == SegmentBy::Level) {
            detect(mono, chunk.start, out);
        } else {
            run(mono, chunk.start, chunk.gate, out);
        }
        next_ = chunk.start + frames;
    }

    // Closes what is open, for a receiver going away or the switch going
    // off. The stream carries on from where it was if more chunks follow.
    void flush(std::vector<Utterance>& out)
    {
        if (config_.by == SegmentBy::Level && !frame_.empty()) {
            const bool voiced = level_.decide(frame_, false);
            run(frame_, frame_start_, voiced, out);
            frame_.clear();
        }
        if (open_) {
            close(out);
        }
    }

    // Forgets what is open without emitting it, and everything learned about
    // the stream: the next chunk is treated as the first.
    void reset()
    {
        open_ = false;
        continuation_ = false;
        current_ = Utterance{};
        resampler_.reset();
        forget_history();
        have_stream_ = false;
        next_ = 0;
    }

    [[nodiscard]] bool open() const { return open_; }

    [[nodiscard]] const SegmenterConfig& config() const { return config_; }

private:
    // The Level detector: a noise floor in dB and a decision per frame.
    struct LevelDetector {
        bool have_floor = false;
        double floor_db = 0.0;
        double fall = 0.0;
        double rise_per_frame = 0.0;

        void configure(double frame_seconds)
        {
            fall = 1.0 - std::exp(-frame_seconds / kLevelFloorFallSeconds);
            rise_per_frame = kLevelFloorRiseDbPerSecond * frame_seconds;
            have_floor = false;
        }

        // Whether the frame is above the floor by the margin, decided against
        // the floor before this frame moves it. A partial frame, at a flush
        // or a gap, is decided but does not move the floor: its estimate is
        // from fewer samples than the margin was set for.
        bool decide(std::span<const float> frame, bool full)
        {
            double sum = 0.0;
            for (const float x : frame) {
                sum += static_cast<double>(x) * x;
            }
            const double mean_square = sum / static_cast<double>(frame.size());
            if (!(mean_square > 0.0)) {
                return false;
            }
            const double level = 10.0 * std::log10(mean_square);
            if (level < kLevelSilenceDb) {
                return false;
            }
            if (!have_floor) {
                if (full) {
                    floor_db = level;
                    have_floor = true;
                }
                return false;
            }
            const bool voiced = level > floor_db + kLevelOpenMarginDb;
            if (full) {
                if (level < floor_db) {
                    floor_db += fall * (level - floor_db);
                } else {
                    floor_db += std::min(level - floor_db, rise_per_frame);
                }
            }
            return voiced;
        }
    };

    void configure(std::uint32_t rate)
    {
        rate_ = rate;
        resampler_.configure(rate, kRecogniserRateHz);
        hangover_frames_ = frames_for(config_.hangover_seconds);
        pending_.clear();
        pending_.reserve(hangover_frames_);
        max_frames_ = std::max<std::size_t>(1, frames_for(config_.max_seconds));
        if (config_.by == SegmentBy::Level) {
            frame_length_ = std::max<std::size_t>(1, frames_for(kLevelFrameSeconds));
            frame_.clear();
            frame_.reserve(frame_length_);
            ring_.assign(frames_for(config_.pre_roll_seconds), 0.0F);
            level_.configure(static_cast<double>(frame_length_) / rate);
        }
        ring_head_ = 0;
        ring_count_ = 0;
    }

    [[nodiscard]] std::size_t frames_for(double seconds) const
    {
        if (!(seconds > 0.0)) {
            return 0;
        }
        return static_cast<std::size_t>(std::llround(seconds * rate_));
    }

    // Everything that describes the stream so far rather than the open
    // utterance: the held gap, the pre-roll, the partial frame and the
    // floor. Called when the stream is no longer the same stream.
    void forget_history()
    {
        pending_.clear();
        frame_.clear();
        ring_head_ = 0;
        ring_count_ = 0;
        level_.have_floor = false;
    }

    std::span<const float> mix(const SegmenterChunk& chunk, std::size_t frames)
    {
        if (chunk.channels == 1) {
            return chunk.samples.first(frames);
        }
        if (mono_.size() < frames) {
            mono_.resize(frames);
        }
        const std::size_t channels = chunk.channels;
        const float scale = 1.0F / static_cast<float>(channels);
        for (std::size_t f = 0; f < frames; ++f) {
            float sum = 0.0F;
            for (std::size_t c = 0; c < channels; ++c) {
                sum += chunk.samples[f * channels + c];
            }
            mono_[f] = sum * scale;
        }
        return std::span<const float>(mono_).first(frames);
    }

    // Dropped chunks between the last one and this: silence if the open
    // utterance can hold it, the end of the utterance if it cannot.
    void bridge(std::uint64_t gap, std::vector<Utterance>& out)
    {
        if (config_.by == SegmentBy::Level && !frame_.empty()) {
            const bool voiced = level_.decide(frame_, false);
            run(frame_, frame_start_, voiced, out);
            frame_.clear();
        }
        if (open_) {
            if (pending_.size() + gap <= hangover_frames_) {
                if (pending_.empty()) {
                    pending_start_ = next_;
                }
                pending_.insert(pending_.end(), static_cast<std::size_t>(gap), 0.0F);
                return;
            }
            ring_write(pending_);
            close(out);
        }
        if (!ring_.empty()) {
            const std::size_t zeros = static_cast<std::size_t>(std::min<std::uint64_t>(gap, ring_.size()));
            for (std::size_t i = 0; i < zeros; ++i) {
                ring_put(0.0F);
            }
        }
    }

    // Level: gathers whole frames, decides each, and runs it.
    void detect(std::span<const float> mono, std::uint64_t start, std::vector<Utterance>& out)
    {
        std::size_t at = 0;
        while (at < mono.size()) {
            if (frame_.empty()) {
                frame_start_ = start + at;
            }
            const std::size_t take = std::min(frame_length_ - frame_.size(), mono.size() - at);
            frame_.insert(frame_.end(), mono.begin() + static_cast<std::ptrdiff_t>(at),
                          mono.begin() + static_cast<std::ptrdiff_t>(at + take));
            at += take;
            if (frame_.size() == frame_length_) {
                const bool voiced = level_.decide(frame_, true);
                run(frame_, frame_start_, voiced, out);
                frame_.clear();
            }
        }
    }

    // The state machine, over a run of frames that share one gate.
    void run(std::span<const float> samples, std::uint64_t start, bool gate, std::vector<Utterance>& out)
    {
        if (samples.empty()) {
            return;
        }
        if (gate) {
            if (!open_) {
                open_at(start);
            } else if (!pending_.empty()) {
                commit(pending_, pending_start_, out);
                pending_.clear();
            }
            commit(samples, start, out);
            return;
        }
        if (open_) {
            if (pending_.empty()) {
                pending_start_ = start;
            }
            if (pending_.size() + samples.size() <= hangover_frames_) {
                pending_.insert(pending_.end(), samples.begin(), samples.end());
                return;
            }
            // The hangover ran out inside this run. What was held goes to
            // the pre-roll, which is where audio before an open belongs.
            ring_write(pending_);
            close(out);
        }
        ring_write(samples);
    }

    void open_at(std::uint64_t start)
    {
        open_ = true;
        continuation_ = false;
        current_ = Utterance{};
        resampler_.reset();
        committed_ = 0;
        voiced_start_ = start;

        const std::uint64_t pre_roll = std::min<std::uint64_t>(ring_count_, start);
        utterance_start_ = start - pre_roll;
        committed_end_ = utterance_start_;
        current_.source_start = place(utterance_start_);
        current_.source_end = current_.source_start;

        // The pre-roll, oldest first. It is at most a fraction of a second,
        // so it cannot reach max_seconds on its own and goes straight in.
        const std::size_t size = ring_.size();
        auto left = static_cast<std::size_t>(pre_roll);
        const auto skip = static_cast<std::size_t>(ring_count_ - pre_roll);
        std::size_t first = size == 0 ? 0 : (ring_head_ + size - ring_count_ + skip) % size;
        while (left > 0) {
            const std::size_t piece = std::min(left, size - first);
            feed(std::span<const float>(ring_).subspan(first, piece));
            left -= piece;
            first = (first + piece) % size;
        }
        ring_head_ = 0;
        ring_count_ = 0;
    }

    // Into the open utterance, cutting at max_seconds.
    void commit(std::span<const float> samples, std::uint64_t start, std::vector<Utterance>& out)
    {
        while (!samples.empty()) {
            const std::size_t room = max_frames_ - committed_;
            const std::size_t take = std::min(room, samples.size());
            feed(samples.first(take));
            samples = samples.subspan(take);
            start += take;
            if (committed_ == max_frames_) {
                close(out);
                open_ = true;
                continuation_ = true;
                current_ = Utterance{};
                committed_ = 0;
                utterance_start_ = start;
                voiced_start_ = start;
                committed_end_ = start;
                current_.source_start = place(start);
                current_.source_end = current_.source_start;
            }
        }
    }

    void feed(std::span<const float> samples)
    {
        resampler_.process(samples, current_.pcm);
        committed_ += samples.size();
        committed_end_ += samples.size();
        current_.source_end = place(committed_end_);
    }

    // Emits the open utterance unless it is empty or, as a first part, too
    // short; either way nothing is open afterwards.
    void close(std::vector<Utterance>& out)
    {
        const double voiced = static_cast<double>(committed_end_ - voiced_start_) / rate_;
        const bool keep = committed_ > 0 && (continuation_ || voiced >= config_.min_seconds);
        if (keep) {
            resampler_.finish(current_.pcm);
            current_.start_sample = utterance_start_;
            current_.end_sample = committed_end_;
            current_.sample_rate = rate_;
            current_.cut_by = config_.by;
            out.push_back(std::move(current_));
        } else {
            resampler_.reset();
        }
        current_ = Utterance{};
        open_ = false;
        continuation_ = false;
        committed_ = 0;
        pending_.clear();
    }

    // Stream index to source index, through the latest chunk: its frame i
    // sits at source_end - (end - i) * source_rate / rate. The relation is
    // a straight line within a tuning, so it holds for the pre-roll and the
    // held gap, which arrived in earlier chunks, as well. Zero when the
    // chunk gave no source rate.
    [[nodiscard]] std::uint64_t place(std::uint64_t index) const
    {
        if (!(anchor_source_rate_ > 0.0)) {
            return 0;
        }
        const double back = (static_cast<double>(anchor_end_) - static_cast<double>(index)) *
                            anchor_source_rate_ / static_cast<double>(rate_);
        const auto offset = static_cast<std::int64_t>(std::llround(back));
        const auto placed = static_cast<std::int64_t>(anchor_source_end_) - offset;
        return placed > 0 ? static_cast<std::uint64_t>(placed) : 0;
    }

    void ring_write(std::span<const float> samples)
    {
        if (ring_.empty()) {
            return;
        }
        if (samples.size() >= ring_.size()) {
            samples = samples.last(ring_.size());
        }
        for (const float x : samples) {
            ring_put(x);
        }
    }

    void ring_put(float x)
    {
        ring_[ring_head_] = x;
        ring_head_ = (ring_head_ + 1) % ring_.size();
        ring_count_ = std::min(ring_count_ + 1, ring_.size());
    }

    SegmenterConfig config_;

    // The stream.
    bool have_stream_ = false;
    std::uint32_t rate_ = 0;
    std::uint64_t epoch_ = 0;
    std::uint64_t next_ = 0;
    std::size_t hangover_frames_ = 0;
    std::size_t max_frames_ = 1;
    std::vector<float> mono_;

    // The latest chunk, which places everything on the source clock.
    std::uint64_t anchor_end_ = 0;
    std::uint64_t anchor_source_end_ = 0;
    double anchor_source_rate_ = 0.0;

    // The open utterance. committed_ counts frames since it opened, which
    // is what max_seconds limits.
    bool open_ = false;
    bool continuation_ = false;
    Utterance current_;
    Resampler resampler_;
    std::size_t committed_ = 0;
    std::uint64_t utterance_start_ = 0;
    std::uint64_t voiced_start_ = 0;
    std::uint64_t committed_end_ = 0;

    // Gate-shut frames held while the hangover runs.
    std::vector<float> pending_;
    std::uint64_t pending_start_ = 0;

    // Level: the frame being gathered, the pre-roll and the detector.
    std::vector<float> frame_;
    std::uint64_t frame_start_ = 0;
    std::size_t frame_length_ = 1;
    std::vector<float> ring_;
    std::size_t ring_head_ = 0;
    std::size_t ring_count_ = 0;
    LevelDetector level_;
};

}  // namespace revenant::transcribe
