// The transcription queue: Utterances in from any thread, one recogniser
// thread, Transcripts out in order.
//
// ONE WORKER, ONE RECOGNISER, ONE QUEUE FOR EVERY RECEIVER. The owner's switch
// of 2026-10-03 is global, and whisper_full is not reentrant on one context
// (core/transcribe/whisper_runner.h, "ONE CALLER AT A TIME"), so a second
// worker would need a second copy of a 1.6 GB model on the GPU to get any
// parallelism at all. The queue is the serialisation, which is the arrangement
// docs/conventions.md asks for rather than a lock every caller has to know
// about.
//
// THE LEAST URGENT THREAD IN THE PROCESS. The owner's precedence is decoding
// and audio first, the display next, identification last, and text made from
// audio somebody has already heard comes after all of it. The worker runs at
// ThreadClass::SigId, the lowest class core/thread_role.h has; transcriber.cpp
// says why not lower.
//
// LATE TEXT IS WORTH LESS THAN CURRENT TEXT. A full queue evicts its oldest
// utterance rather than refusing the newest, so on a machine that cannot keep
// up the operator reads what is being said now and loses what was said a
// minute ago, instead of the other way round. Every utterance that is not
// transcribed is counted: dropped, rejected as not speech, or failed.
//
// The mutex here is not in the sample path. Producers hold it for a deque push;
// the worker holds it to pop and to publish counters, and never across a
// recognise or a prepare, so status() and submit() do not wait on the GPU.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/error.h"
#include "core/transcribe/utterance.h"

namespace revenant::transcribe {

// One stretch of text and the recogniser's two numbers about it, the same two
// whisper's own transcribe.py judges a decode by.
struct RecognisedSegment {
    std::string text;
    // The recogniser's estimate that the input held no speech, 0 to 1.
    float no_speech_prob = 0.0F;
    // The mean natural log-probability of the segment's text tokens.
    float avg_logprob = 0.0F;
};

// What the transcriber drives. Whisper in production (whisper_prepare below),
// a fake in tests/transcribe.
class Recogniser {
public:
    virtual ~Recogniser() = default;

    // 16000 S/s mono; the transcriber calls it from one thread only.
    virtual Expected<std::vector<RecognisedSegment>> recognise(std::span<const float> pcm16k) = 0;

    // Which device runs it, for TranscriberStatus::backend.
    virtual std::string backend() const = 0;
};

enum class ModelState : std::uint8_t {
    // Nothing has been tried yet: the switch has never been on.
    Absent,
    Downloading,
    Verifying,
    Loading,
    Ready,
    // A prepare failed, or the recogniser failed kFailuresInARow utterances
    // in a row. TranscriberStatus::detail says which and why. Turning the
    // switch off and on again retries.
    Failed,
};

struct TranscriberStatus {
    bool enabled = false;
    ModelState state = ModelState::Absent;
    std::string model_name;
    // The reason for Failed, or while Ready the most recent recogniser error
    // until the next utterance recognises cleanly. Empty otherwise.
    std::string detail;
    // Recogniser::backend() once one is loaded.
    std::string backend;
    std::uint64_t bytes_done = 0;
    std::uint64_t bytes_total = 0;
    std::uint32_t queued = 0;
    std::uint64_t transcribed = 0;
    // Submitted while not ready, evicted by a full queue, emptied out by the
    // switch going off, or abandoned by a recogniser that failed for good.
    std::uint64_t dropped = 0;
    // Recognised, and judged not to be speech by the rules in transcriber.cpp.
    std::uint64_t rejected = 0;
    // closed_ns to text, for the most recent Transcript.
    double last_latency_ms = 0.0;
    // ADDED BEYOND THE AGREED API: utterances the recogniser returned an error
    // for. Every utterance has to land in exactly one counter, and an error is
    // neither a drop (it was attempted) nor a rejection (nothing was judged).
    std::uint64_t errors = 0;
};

// What a Prepare reports as it goes: Downloading with bytes, Verifying,
// Loading. Any other state reported is ignored; the outcome is the return.
using PrepareReport = std::function<void(ModelState state, std::uint64_t done, std::uint64_t total)>;

// Gets a recogniser ready: download the model if absent, verify, load.
// Blocking, on the transcriber's worker thread. `cancel` set means give up
// promptly (the transcriber is being destroyed).
using Prepare = std::function<Expected<std::unique_ptr<Recogniser>>(const PrepareReport& report,
                                                                    const std::atomic<bool>& cancel)>;

struct TranscriberOptions {
    // Beyond this many waiting, the oldest is evicted.
    std::size_t max_queued = 32;
    // For TranscriberStatus::model_name, nothing else.
    std::string model_name;

    // Whisper's own no-speech rule: a segment is skipped when no_speech_prob is
    // above reject_no_speech AND avg_logprob is below reject_logprob. The
    // defaults are openai/whisper's transcribe.py, no_speech_threshold 0.6 and
    // logprob_threshold -1.0. transcriber.cpp has the rest of the rules.
    float reject_no_speech = 0.6F;
    float reject_logprob = -1.0F;
};

class Transcriber {
public:
    // Called on the worker thread, once per transcript, in order.
    using Sink = std::function<void(Transcript)>;

    // The recogniser a run of errors this long is taken to be broken rather
    // than unlucky.
    static constexpr int kFailuresInARow = 3;

    // Starts the worker and nothing else: prepare is not called until the
    // switch first goes on, so an engine with transcription off never touches
    // the model.
    [[nodiscard]] static Expected<std::unique_ptr<Transcriber>> create(TranscriberOptions options,
                                                                        Prepare prepare, Sink sink);

    // Cancels a prepare in progress, finishes nothing more, joins.
    ~Transcriber();

    Transcriber(const Transcriber&) = delete;
    Transcriber& operator=(const Transcriber&) = delete;
    Transcriber(Transcriber&&) = delete;
    Transcriber& operator=(Transcriber&&) = delete;

    // Any thread. The first time it goes on, the worker runs prepare. Off then
    // on again retries a prepare that failed. Off empties the queue (counted
    // as dropped) but keeps a loaded recogniser loaded.
    void set_enabled(bool on);

    // Enabled and the recogniser is loaded.
    [[nodiscard]] bool ready() const;

    // Any thread, cheap. Not ready: dropped and counted. Queue full: the
    // OLDEST is evicted and counted, because late text is worth less than
    // current text.
    void submit(Utterance utterance);

    // Any thread. A snapshot under a short lock; never waits on a recognise.
    [[nodiscard]] TranscriberStatus status() const;

private:
    Transcriber(TranscriberOptions options, Prepare prepare, Sink sink);

    void run();
    void run_prepare(std::unique_lock<std::mutex>& lock);
    void process(Utterance utterance);

    const TranscriberOptions options_;
    const Prepare prepare_;
    const Sink sink_;

    // Everything below is guarded by mutex_ except recogniser_, which only the
    // worker touches, and cancel_, which a running prepare polls.
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Utterance> queue_;
    TranscriberStatus counters_;
    bool stop_ = false;
    bool prepare_pending_ = false;
    bool preparing_ = false;
    int failures_in_a_row_ = 0;
    std::uint64_t sequence_ = 0;

    std::unique_ptr<Recogniser> recogniser_;
    std::atomic<bool> cancel_{false};
    std::thread worker_;
};

// The production Prepare, in whisper_prepare.cpp: model_ready(kDefaultModel)
// or download_model, reported as Verifying and Downloading, then Whisper::load
// with `options` and its model_path filled in, reported as Loading. Declared
// against an incomplete WhisperOptions so this header, and the tests that use
// it with a fake, do not need whisper_runner.h.
struct WhisperOptions;
[[nodiscard]] Prepare whisper_prepare(WhisperOptions options);

}  // namespace revenant::transcribe
