// What the speech-to-text pieces hand each other: an utterance cut from one
// receiver's audio, and the text the recogniser made of it.
//
// OWNER DECISIONS OF 2026-10-03. One global switch transcribes every receiver
// that makes speech. A digital voice receiver is cut into utterances by its
// calls, an analogue one by its squelch, and one whose squelch is open by the
// level of what it hears. WFM is left out unless a receiver is chosen in. The
// recogniser is whisper.cpp on the engine's GPU through Vulkan, with its model
// downloaded the first time the switch goes on. core/rpc/revenant.capnp, under
// "Speech to text", is the wire; docs/rpc.md has the design.
//
// The pieces, in the order audio passes through them:
//
//   segmenter.h    one receiver's chunks in, Utterances out, at 16000 S/s
//   transcriber.h  a queue of Utterances, one recogniser thread, Transcripts out
//   whisper_runner.h, model_store.h  the recogniser and its model on disk
//
// Qt-free, and free of core/rpc, so tests/transcribe links these alone.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace revenant::transcribe {

// The rate Whisper takes, and so the rate every Utterance carries.
inline constexpr std::uint32_t kRecogniserRateHz = 16'000;

// How an utterance's ends are found, which depends on what the receiver is.
enum class SegmentBy : std::uint8_t {
    // A digital voice receiver's decoded voice: silent between calls, so a
    // call is the stretch the stream says is voiced.
    Call,

    // An analogue receiver with its squelch closing between transmissions: an
    // utterance is the squelch opening to its closing.
    Squelch,

    // An analogue receiver whose squelch never closes: an energy detector
    // against the noise floor it measures, on the lines of the owner's OracleX
    // voice activity detector.
    Level,
};

// One thing a receiver's protocol said about the call, for a digital voice
// utterance: "talkgroup", "source" and the like, under the keys its decoder
// publishes in core/rpc/decoders.h. Integers only, because every call field
// carried today is one.
struct CallField {
    std::string key;
    std::int64_t value = 0;
};

struct Utterance {
    std::uint64_t vrx = 0;

    // The audio, mono at kRecogniserRateHz, no longer than the segmenter's
    // SegmenterConfig::max_seconds, which keeps it inside Whisper's 30 s.
    std::vector<float> pcm;

    // [start_sample, end_sample) in the receiver's own stream at sample_rate,
    // the clock engine::AudioChunk::start counts in.
    std::uint64_t start_sample = 0;
    std::uint64_t end_sample = 0;
    std::uint32_t sample_rate = 0;

    // The same stretch in the engine's source sample index, the clock a
    // spectrum frame and a detection count in, so a client can put the text
    // on the waterfall rows the speech was on. See engine::AudioChunk::
    // source_end for how a chunk is placed on it.
    std::uint64_t source_start = 0;
    std::uint64_t source_end = 0;

    // Where the receiver was when it heard this, absolute hertz, and its mode
    // by Demod's lower case name. Filled by whoever owns the receiver.
    std::int64_t center_hz = 0;
    std::int64_t low_hz = 0;
    std::int64_t high_hz = 0;
    std::string mode;

    SegmentBy cut_by = SegmentBy::Call;

    std::vector<CallField> fields;

    // core/engine/load_clock.h's clock when the utterance was closed, which is
    // when its last sample reached the engine's host side: the start of the
    // latency a Transcript reports.
    std::uint64_t closed_ns = 0;
};

// What the recogniser made of one Utterance that it judged to be speech.
struct Transcript {
    // The utterance it came from, without its audio.
    Utterance heard;

    std::string text;

    // From zero to one: the mean probability of the tokens chosen, and the
    // recogniser's own estimate that there was no speech.
    float confidence = 0.0F;
    float no_speech_prob = 0.0F;

    // closed_ns to the text being ready.
    double latency_ms = 0.0;

    // Transcripts produced before this one by this transcriber.
    std::uint64_t sequence = 0;
};

}  // namespace revenant::transcribe
