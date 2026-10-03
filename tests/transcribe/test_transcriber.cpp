// core/transcribe/transcriber.h: the queue between the segmenter and the
// recogniser, driven with a fake Recogniser and a fake Prepare. No model, no
// GPU; whisper_prepare.cpp, the production Prepare, is not exercised here.
//
// WHAT THESE CASES CLAIM
//
// That the model is only touched once the switch is on, and touched once;
// that a failure is retried by the switch and by nothing else; that work which
// cannot be done is counted rather than queued for later or lost; that a full
// queue gives up its oldest; that text comes out in order, numbered, timed from
// when the utterance closed; that each rule in transcriber.cpp's rule block
// rejects what it says and keeps what it says; that the level normalisation
// reaches its target without exceeding its cap or its ceiling; and that the
// transcriber can be destroyed in the middle of a long download.
//
// Each case names, above it, the wrong implementation it exists to fail.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/engine/load_clock.h"
#include "core/transcribe/transcriber.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace revenant::transcribe {
namespace {

using namespace std::chrono_literals;

constexpr auto kPatience = 5s;

// Polls rather than sleeps a fixed time, so a passing case is as fast as the
// machine and a failing one gives up after kPatience instead of hanging ctest.
bool eventually(const std::function<bool()>& condition, std::chrono::milliseconds patience = kPatience)
{
    const auto deadline = std::chrono::steady_clock::now() + patience;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return condition();
}

// Holds whoever calls wait() until release(), and counts them in.
class Gate {
public:
    void wait()
    {
        std::unique_lock lock(mutex_);
        ++entered_;
        changed_.notify_all();
        changed_.wait(lock, [this] { return open_; });
    }

    void release()
    {
        {
            const std::lock_guard lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }

    bool entered(int count)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, kPatience, [&] { return entered_ >= count; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool open_ = false;
    int entered_ = 0;
};

using Respond = std::function<Expected<std::vector<RecognisedSegment>>(std::span<const float>)>;

std::vector<RecognisedSegment> confident(std::string text)
{
    return {RecognisedSegment{std::move(text), 0.01F, -0.1F}};
}

// What the fake recogniser saw, owned by the test and outliving the
// transcriber, which is declared after it.
struct Heard {
    std::mutex mutex;
    std::vector<std::vector<float>> pcm;
    int worker_priority = 0;
    Respond respond = [](std::span<const float>) -> Expected<std::vector<RecognisedSegment>> {
        return confident(" Engine four en route.");
    };
};

class FakeRecogniser final : public Recogniser {
public:
    explicit FakeRecogniser(Heard& heard) : heard_(heard) {}

    Expected<std::vector<RecognisedSegment>> recognise(std::span<const float> pcm16k) override
    {
        Respond respond;
        {
            const std::lock_guard lock(heard_.mutex);
            heard_.pcm.emplace_back(pcm16k.begin(), pcm16k.end());
#if defined(_WIN32)
            heard_.worker_priority = GetThreadPriority(GetCurrentThread());
#endif
            respond = heard_.respond;
        }
        return respond(pcm16k);
    }

    std::string backend() const override { return "fake backend"; }

private:
    Heard& heard_;
};

// A Prepare that counts its calls and hands back a FakeRecogniser.
struct CountingPrepare {
    Heard* heard = nullptr;
    std::atomic<int> calls{0};
    // Fails the calls whose number, from 1, is listed here.
    std::vector<int> fail_on;

    Prepare make()
    {
        return [this](const PrepareReport&, const std::atomic<bool>&) -> Expected<std::unique_ptr<Recogniser>> {
            const int call = ++calls;
            if (std::ranges::find(fail_on, call) != fail_on.end()) {
                return fail("the fake model would not load");
            }
            return std::make_unique<FakeRecogniser>(*heard);
        };
    }
};

struct Collected {
    std::mutex mutex;
    std::vector<Transcript> transcripts;

    Transcriber::Sink sink()
    {
        return [this](Transcript transcript) {
            const std::lock_guard lock(mutex);
            transcripts.push_back(std::move(transcript));
        };
    }

    std::size_t count()
    {
        const std::lock_guard lock(mutex);
        return transcripts.size();
    }

    std::vector<Transcript> copy()
    {
        const std::lock_guard lock(mutex);
        return transcripts;
    }
};

std::unique_ptr<Transcriber> make(TranscriberOptions options, Prepare prepare, Transcriber::Sink sink)
{
    auto made = Transcriber::create(std::move(options), std::move(prepare), std::move(sink));
    REQUIRE(made.has_value());
    return std::move(*made);
}

// A 440 Hz tone at kRecogniserRateHz, `seconds` long.
Utterance tone(std::uint64_t vrx, float amplitude, double seconds = 0.5, float dc = 0.0F)
{
    Utterance utterance;
    utterance.vrx = vrx;
    const auto samples = static_cast<std::size_t>(seconds * kRecogniserRateHz);
    utterance.pcm.resize(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        const double phase = 2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / kRecogniserRateHz;
        utterance.pcm[i] = dc + amplitude * static_cast<float>(std::sin(phase));
    }
    utterance.sample_rate = kRecogniserRateHz;
    utterance.closed_ns = engine::load_clock_ns();
    return utterance;
}

void enable_and_wait_ready(Transcriber& transcriber)
{
    transcriber.set_enabled(true);
    REQUIRE(eventually([&] { return transcriber.ready(); }));
}

double rms(std::span<const float> pcm)
{
    double energy = 0.0;
    for (const float sample : pcm) {
        energy += static_cast<double>(sample) * sample;
    }
    return std::sqrt(energy / static_cast<double>(pcm.size()));
}

float peak(std::span<const float> pcm)
{
    float most = 0.0F;
    for (const float sample : pcm) {
        most = std::max(most, std::abs(sample));
    }
    return most;
}

// Runs one utterance through a transcriber whose recogniser answers with
// `segments`, and returns the transcript, or nothing when it was rejected.
std::optional<Transcript> judge_one(std::vector<RecognisedSegment> segments)
{
    Heard heard;
    heard.respond = [segments](std::span<const float>) -> Expected<std::vector<RecognisedSegment>> {
        return segments;
    };
    CountingPrepare prepare{&heard};
    Collected collected;
    auto transcriber = make({}, prepare.make(), collected.sink());
    enable_and_wait_ready(*transcriber);
    transcriber->submit(tone(1, 0.1F));
    REQUIRE(eventually([&] {
        const TranscriberStatus status = transcriber->status();
        return status.transcribed + status.rejected == 1;
    }));
    const TranscriberStatus status = transcriber->status();
    if (status.rejected == 1) {
        return std::nullopt;
    }
    REQUIRE(eventually([&] { return collected.count() == 1; }));
    return collected.copy().front();
}

bool rejected(std::vector<RecognisedSegment> segments)
{
    return !judge_one(std::move(segments)).has_value();
}

std::string kept_text(std::vector<RecognisedSegment> segments)
{
    const auto transcript = judge_one(std::move(segments));
    REQUIRE(transcript.has_value());
    return transcript->text;
}

}  // namespace

// REJECTS: a transcriber that prepares at create, which would download or load
// 1.6 GB on every engine start with the switch off, and one that prepares on
// every switch-on, which reloads the model each time somebody toggles it.
TEST_CASE("prepare runs once, on the first switch-on and not at create", "[transcribe][transcriber]") {
    Heard heard;
    CountingPrepare prepare{&heard};
    Collected collected;
    auto transcriber = make({}, prepare.make(), collected.sink());

    std::this_thread::sleep_for(50ms);
    CHECK(prepare.calls == 0);
    CHECK(transcriber->status().state == ModelState::Absent);
    CHECK_FALSE(transcriber->ready());

    enable_and_wait_ready(*transcriber);
    CHECK(prepare.calls == 1);

    transcriber->set_enabled(false);
    CHECK_FALSE(transcriber->ready());
    CHECK(transcriber->status().state == ModelState::Ready);
    enable_and_wait_ready(*transcriber);
    std::this_thread::sleep_for(50ms);
    CHECK(prepare.calls == 1);
}

// REJECTS: a Failed state that holds for the life of the process, and a
// transcriber that retries a failed prepare on its own in a loop, which for a
// download is hammering a server and for a load is a GPU allocation storm.
TEST_CASE("a failed prepare is retried by turning the switch off and on, and only then",
          "[transcribe][transcriber]") {
    Heard heard;
    CountingPrepare prepare{&heard};
    prepare.fail_on = {1};
    Collected collected;
    auto transcriber = make({}, prepare.make(), collected.sink());

    transcriber->set_enabled(true);
    REQUIRE(eventually([&] { return transcriber->status().state == ModelState::Failed; }));
    CHECK(transcriber->status().detail.find("would not load") != std::string::npos);
    std::this_thread::sleep_for(100ms);
    CHECK(prepare.calls == 1);
    CHECK_FALSE(transcriber->ready());

    transcriber->set_enabled(false);
    enable_and_wait_ready(*transcriber);
    CHECK(prepare.calls == 2);
    CHECK(transcriber->status().detail.empty());
}

// REJECTS: an utterance held until the recogniser is ready and transcribed
// minutes late, and one discarded without being counted.
TEST_CASE("an utterance submitted before the recogniser is ready is dropped and counted",
          "[transcribe][transcriber]") {
    Heard heard;
    Gate loading;
    std::atomic<int> calls{0};
    const Prepare prepare = [&](const PrepareReport& report,
                                const std::atomic<bool>&) -> Expected<std::unique_ptr<Recogniser>> {
        ++calls;
        report(ModelState::Loading, 0, 0);
        loading.wait();
        return std::make_unique<FakeRecogniser>(heard);
    };
    Collected collected;
    auto transcriber = make({}, prepare, collected.sink());

    transcriber->submit(tone(1, 0.1F));
    CHECK(transcriber->status().dropped == 1);

    transcriber->set_enabled(true);
    REQUIRE(loading.entered(1));
    transcriber->submit(tone(2, 0.1F));
    CHECK(transcriber->status().dropped == 2);
    CHECK(transcriber->status().queued == 0);

    loading.release();
    REQUIRE(eventually([&] { return transcriber->ready(); }));
    std::this_thread::sleep_for(50ms);
    const std::lock_guard lock(heard.mutex);
    CHECK(heard.pcm.empty());
    CHECK(collected.count() == 0);
}

// REJECTS: a full queue that refuses the newest utterance, which on a machine
// that cannot keep up shows the operator text from further and further back.
TEST_CASE("a full queue evicts its oldest utterance and counts it", "[transcribe][transcriber]") {
    Heard heard;
    Gate recognising;
    heard.respond = [&](std::span<const float>) -> Expected<std::vector<RecognisedSegment>> {
        recognising.wait();
        return confident("Engine four en route.");
    };
    CountingPrepare prepare{&heard};
    Collected collected;
    TranscriberOptions options;
    options.max_queued = 2;
    auto transcriber = make(options, prepare.make(), collected.sink());
    enable_and_wait_ready(*transcriber);

    transcriber->submit(tone(0, 0.1F));
    REQUIRE(recognising.entered(1));
    transcriber->submit(tone(1, 0.1F));
    transcriber->submit(tone(2, 0.1F));
    transcriber->submit(tone(3, 0.1F));
    CHECK(transcriber->status().queued == 2);
    CHECK(transcriber->status().dropped == 1);

    recognising.release();
    REQUIRE(eventually([&] { return collected.count() == 3; }));
    const auto transcripts = collected.copy();
    CHECK(transcripts[0].heard.vrx == 0);
    CHECK(transcripts[1].heard.vrx == 2);
    CHECK(transcripts[2].heard.vrx == 3);
}

// REJECTS: transcripts reordered, a sequence number spent on a rejected
// utterance so the client sees a gap that is not a loss, and a latency
// measured from when the worker picked the utterance up rather than from when
// it closed, which hides exactly the queueing delay it exists to show. Also
// a Transcript that carries the audio back out, which the wire does not want.
TEST_CASE("transcripts come out in order, numbered, timed from the utterance's close",
          "[transcribe][transcriber]") {
    Heard heard;
    // The utterance 0.25 s long is the one judged not to be speech.
    heard.respond = [](std::span<const float> pcm) -> Expected<std::vector<RecognisedSegment>> {
        if (pcm.size() == kRecogniserRateHz / 4) {
            return confident("[BLANK_AUDIO]");
        }
        return confident("Engine four en route.");
    };
    CountingPrepare prepare{&heard};
    Collected collected;
    auto transcriber = make({}, prepare.make(), collected.sink());
    enable_and_wait_ready(*transcriber);

    for (std::uint64_t vrx = 0; vrx < 10; ++vrx) {
        Utterance utterance = tone(vrx, 0.1F, vrx == 5 ? 0.25 : 0.5);
        utterance.closed_ns = engine::load_clock_ns() - 50'000'000;
        transcriber->submit(std::move(utterance));
    }
    REQUIRE(eventually([&] { return transcriber->status().transcribed + transcriber->status().rejected == 10; }));
    REQUIRE(eventually([&] { return collected.count() == 9; }));

    const auto transcripts = collected.copy();
    std::uint64_t vrx = 0;
    for (std::uint64_t sequence = 0; sequence < transcripts.size(); ++sequence, ++vrx) {
        if (vrx == 5) {
            ++vrx;
        }
        INFO("sequence " << sequence);
        CHECK(transcripts[sequence].sequence == sequence);
        CHECK(transcripts[sequence].heard.vrx == vrx);
        CHECK(transcripts[sequence].heard.pcm.empty());
        CHECK(transcripts[sequence].latency_ms >= 50.0);
        CHECK(transcripts[sequence].latency_ms < 50.0 + 5000.0);
    }
    CHECK(transcriber->status().rejected == 1);
    CHECK(transcriber->status().last_latency_ms >= 50.0);
}

// REJECTS: whisper's rule applied with OR, which throws away quiet but clearly
// decoded speech whenever either number dips, and not applied at all.
TEST_CASE("rule 1: whisper's own no-speech rule needs both of its numbers", "[transcribe][transcriber][rules]") {
    CHECK(rejected({{"Engine four en route.", 0.7F, -1.2F}}));
    CHECK_FALSE(rejected({{"Engine four en route.", 0.7F, -0.5F}}));
    CHECK_FALSE(rejected({{"Engine four en route.", 0.3F, -1.5F}}));

    TranscriberOptions strict;
    strict.reject_no_speech = 0.2F;
    strict.reject_logprob = -0.2F;
    Heard heard;
    heard.respond = [](std::span<const float>) -> Expected<std::vector<RecognisedSegment>> {
        return std::vector<RecognisedSegment>{{"Engine four en route.", 0.3F, -0.3F}};
    };
    CountingPrepare prepare{&heard};
    Collected collected;
    auto transcriber = make(strict, prepare.make(), collected.sink());
    enable_and_wait_ready(*transcriber);
    transcriber->submit(tone(1, 0.1F));
    REQUIRE(eventually([&] { return transcriber->status().rejected == 1; }));
}

// REJECTS: a loop passed through as text, a loop deleted whole so its first
// copy is lost, and a rule so eager it cuts what a person really says three
// times over on the air.
TEST_CASE("rule 2: a repetition loop is cut to one copy", "[transcribe][transcriber][rules]") {
    CHECK(kept_text(confident("Copy that. Copy that. Copy that. Copy that. Copy that.")) == "Copy that.");
    CHECK(kept_text(confident("Engine 4 responding. Ten four ten four ten four ten four")) ==
          "Engine 4 responding. Ten four");
    CHECK(kept_text(confident("Mayday mayday mayday, vessel taking on water")) ==
          "Mayday mayday mayday, vessel taking on water");

    // A loop Whisper wrote one copy per segment is only visible joined.
    std::vector<RecognisedSegment> segments(6, RecognisedSegment{" Copy that.", 0.01F, -0.1F});
    CHECK(kept_text(segments) == "Copy that.");

    // A loop cut to one copy then meets rule 4.
    CHECK(rejected({{"Thank you. Thank you. Thank you. Thank you. Thank you.", 0.5F, -0.4F}}));
}

// REJECTS: Whisper's verdict that there was no speech, written as text,
// published as though it were speech; and the marks stripped out of a real
// sentence, which loses the operator the fact that a word was unclear.
TEST_CASE("rule 3: empty text, punctuation and sound marks alone are rejected", "[transcribe][transcriber][rules]") {
    CHECK(rejected(confident("")));
    CHECK(rejected(confident("   \t ")));
    CHECK(rejected(confident(" [BLANK_AUDIO]")));
    CHECK(rejected(confident("(music)")));
    CHECK(rejected(confident("[MUSIC] (applause)")));
    CHECK(rejected(confident("*sigh*")));
    CHECK(rejected(confident("\xE2\x99\xAA\xE2\x99\xAA")));
    CHECK(rejected(confident("...")));
    CHECK(rejected(confident(" - ")));
    CHECK(rejected(confident("[BLANK_AUD")));
    CHECK(rejected({}));

    CHECK(kept_text(confident(" Unit 7 [inaudible] on scene.")) == "Unit 7 [inaudible] on scene.");
}

// REJECTS: the stock phrases published from noise, and the opposite failure:
// a list applied whatever the recogniser's confidence, which deletes every
// "thank you" and "okay" a person says, or applied to a sentence that merely
// contains one.
TEST_CASE("rule 4: Whisper's known hallucinations are rejected unless it was confident",
          "[transcribe][transcriber][rules]") {
    // Not confident: no_speech_prob too high, avg_logprob fine.
    CHECK(rejected({{" Thank you.", 0.5F, -0.2F}}));
    CHECK(rejected({{" Thanks for watching!", 0.3F, -0.1F}}));
    CHECK(rejected({{" Thank you for watching.", 0.3F, -0.1F}}));
    CHECK(rejected({{" you", 0.4F, -0.2F}}));
    CHECK(rejected({{" Subtitles by the Amara.org community", 0.3F, -0.2F}}));
    CHECK(rejected({{" Subtitles by somebody else entirely", 0.3F, -0.2F}}));
    CHECK(rejected({{" I'm sorry.", 0.3F, -0.2F}}));
    // Not confident: no_speech_prob fine, avg_logprob low. Either is enough.
    CHECK(rejected({{" Okay.", 0.05F, -0.9F}}));
    CHECK(rejected({{" Bye.", 0.05F, -0.7F}}));

    // Confident on both counts: a person said it.
    CHECK(kept_text({{" Thank you.", 0.05F, -0.1F}}) == "Thank you.");
    CHECK(kept_text({{" Okay.", 0.1F, -0.3F}}) == "Okay.");
    // A listed phrase inside a sentence is speech however doubtful.
    CHECK(kept_text({{" Thank you, Engine 4.", 0.5F, -0.8F}}) == "Thank you, Engine 4.");

    // A trailing hallucination in a segment of its own goes, the speech stays.
    CHECK(kept_text({{" Engine 4 on scene.", 0.05F, -0.2F}, {" Thanks for watching!", 0.4F, -0.6F}}) ==
          "Engine 4 on scene.");
}

// REJECTS: segments concatenated with Whisper's leading spaces doubled, a
// confidence reported as the raw log-probability or as 1 + logprob, and a
// no_speech_prob that takes the most confident segment's word for the whole.
TEST_CASE("segments are joined and their confidence is exp of the mean log-probability",
          "[transcribe][transcriber][rules]") {
    const auto one = judge_one({{" Engine four", 0.02F, std::log(0.8F)}});
    REQUIRE(one.has_value());
    CHECK(one->confidence == Catch::Approx(0.8F).margin(1e-4));
    CHECK(one->no_speech_prob == Catch::Approx(0.02F));

    // Nine characters each once trimmed, so an even weighting:
    // exp((ln 0.9 + ln 0.4) / 2) = sqrt(0.36) = 0.6.
    const auto two = judge_one({{" Unit four ", 0.02F, std::log(0.9F)}, {" en route.  ", 0.1F, std::log(0.4F)}});
    REQUIRE(two.has_value());
    CHECK(two->text == "Unit four en route.");
    CHECK(two->confidence == Catch::Approx(0.6F).margin(1e-4));
    CHECK(two->no_speech_prob == Catch::Approx(0.1F));
}

// REJECTS: audio handed to the recogniser at whatever level it arrived, a gain
// with no cap that lifts a squelch opening on nothing to speech level, a gain
// that clips, and an RMS taken with an AM carrier's DC still in it.
TEST_CASE("each utterance is brought to one level, within a capped gain and a peak ceiling",
          "[transcribe][transcriber][level]") {
    Heard heard;
    CountingPrepare prepare{&heard};
    Collected collected;
    auto transcriber = make({}, prepare.make(), collected.sink());
    enable_and_wait_ready(*transcriber);

    // In the order heard.pcm will hold them.
    transcriber->submit(tone(0, 0.01F));         // -43 dBFS RMS: lifted to the target
    transcriber->submit(tone(1, 0.001F));        // -63 dBFS RMS: lifted by the cap and no more
    transcriber->submit(tone(2, 0.9F));          // loud: brought down to the target
    transcriber->submit(tone(3, 0.01F, 0.5, 0.3F));  // the same tone on a DC offset
    Utterance spike = tone(4, 0.01F);
    spike.pcm[100] = 0.5F;
    transcriber->submit(std::move(spike));       // the target would put the spike far above full scale
    Utterance silent;
    silent.vrx = 5;
    silent.pcm.assign(kRecogniserRateHz / 2, 0.0F);
    transcriber->submit(std::move(silent));

    REQUIRE(eventually([&] {
        const std::lock_guard lock(heard.mutex);
        return heard.pcm.size() == 6;
    }));
    const std::lock_guard lock(heard.mutex);
    const double target = 0.1;
    const double cap = std::pow(10.0, 30.0 / 20.0);

    CHECK(rms(heard.pcm[0]) == Catch::Approx(target).epsilon(0.01));
    CHECK(rms(heard.pcm[1]) == Catch::Approx(0.001 / std::numbers::sqrt2 * cap).epsilon(0.01));
    CHECK(rms(heard.pcm[1]) < target / 2.0);
    CHECK(rms(heard.pcm[2]) == Catch::Approx(target).epsilon(0.01));

    double mean = 0.0;
    for (const float sample : heard.pcm[3]) {
        mean += sample;
    }
    mean /= static_cast<double>(heard.pcm[3].size());
    CHECK(std::abs(mean) < 1e-3);
    CHECK(rms(heard.pcm[3]) == Catch::Approx(target).epsilon(0.01));

    CHECK(peak(heard.pcm[4]) == Catch::Approx(0.9F).epsilon(1e-4));
    CHECK(peak(heard.pcm[4]) <= 0.9F + 1e-6F);

    CHECK(peak(heard.pcm[5]) == 0.0F);
    for (const auto& pcm : heard.pcm) {
        CHECK(std::ranges::all_of(pcm, [](float sample) { return std::isfinite(sample); }));
    }
}

// REJECTS: a destructor that waits out a download it could have cancelled,
// which turns closing the engine into a wait of minutes, and a prepare that is
// never told to stop.
TEST_CASE("destroying the transcriber during a slow prepare returns promptly", "[transcribe][transcriber]") {
    std::atomic<bool> saw_cancel{false};
    std::atomic<bool> started{false};
    const Prepare slow = [&](const PrepareReport& report,
                             const std::atomic<bool>& cancel) -> Expected<std::unique_ptr<Recogniser>> {
        started = true;
        const auto give_up = std::chrono::steady_clock::now() + 60s;
        std::uint64_t done = 0;
        while (std::chrono::steady_clock::now() < give_up) {
            if (cancel.load()) {
                saw_cancel = true;
                return fail("cancelled");
            }
            report(ModelState::Downloading, done += 1000, 1'000'000'000);
            std::this_thread::sleep_for(5ms);
        }
        return fail("a minute passed and nobody cancelled");
    };
    Collected collected;
    auto transcriber = make({}, slow, collected.sink());
    transcriber->set_enabled(true);
    REQUIRE(eventually([&] { return started.load(); }));
    REQUIRE(eventually([&] { return transcriber->status().state == ModelState::Downloading; }));

    const auto began = std::chrono::steady_clock::now();
    transcriber.reset();
    const auto took = std::chrono::steady_clock::now() - began;
    CHECK(took < 2s);
    CHECK(saw_cancel);
}

// REJECTS: a status() that holds the lock the worker recognises under, so the
// RPC loop asking for it stalls for the length of a Whisper call on a busy GPU.
TEST_CASE("status does not wait on a recognise in progress", "[transcribe][transcriber]") {
    Heard heard;
    Gate recognising;
    heard.respond = [&](std::span<const float>) -> Expected<std::vector<RecognisedSegment>> {
        recognising.wait();
        return confident("Engine four en route.");
    };
    CountingPrepare prepare{&heard};
    Collected collected;
    auto transcriber = make({}, prepare.make(), collected.sink());
    enable_and_wait_ready(*transcriber);
    transcriber->submit(tone(0, 0.1F));
    REQUIRE(recognising.entered(1));

    const auto began = std::chrono::steady_clock::now();
    const TranscriberStatus status = transcriber->status();
    transcriber->submit(tone(1, 0.1F));
    CHECK(transcriber->ready());
    CHECK(std::chrono::steady_clock::now() - began < 500ms);
    CHECK(status.state == ModelState::Ready);
    recognising.release();
    REQUIRE(eventually([&] { return collected.count() == 2; }));
}

// REJECTS: an off switch that unloads the model, so the next on reloads it,
// and one that leaves queued work to surface as text after the operator
// turned transcription off.
TEST_CASE("switching off empties the queue as dropped and keeps the recogniser loaded",
          "[transcribe][transcriber]") {
    Heard heard;
    Gate recognising;
    heard.respond = [&](std::span<const float>) -> Expected<std::vector<RecognisedSegment>> {
        recognising.wait();
        return confident("Engine four en route.");
    };
    CountingPrepare prepare{&heard};
    Collected collected;
    auto transcriber = make({}, prepare.make(), collected.sink());
    enable_and_wait_ready(*transcriber);

    transcriber->submit(tone(0, 0.1F));
    REQUIRE(recognising.entered(1));
    transcriber->submit(tone(1, 0.1F));
    transcriber->submit(tone(2, 0.1F));
    transcriber->submit(tone(3, 0.1F));
    transcriber->set_enabled(false);
    CHECK(transcriber->status().queued == 0);
    CHECK(transcriber->status().dropped == 3);

    // The one already being recognised finishes and is dropped as well.
    recognising.release();
    REQUIRE(eventually([&] { return transcriber->status().dropped == 4; }));
    CHECK(collected.count() == 0);
    CHECK(transcriber->status().transcribed == 0);

    enable_and_wait_ready(*transcriber);
    CHECK(prepare.calls == 1);
}

// REJECTS: one bad utterance taking transcription down for good, three in a
// row leaving a dead recogniser to fail every utterance after them, and a
// failure that is not counted anywhere.
TEST_CASE("recogniser errors are counted, and three in a row fail it until the switch is cycled",
          "[transcribe][transcriber]") {
    Heard heard;
    std::atomic<int> failures_left{1};
    heard.respond = [&](std::span<const float>) -> Expected<std::vector<RecognisedSegment>> {
        if (failures_left > 0) {
            --failures_left;
            return fail("the fake device was lost");
        }
        return confident("Engine four en route.");
    };
    CountingPrepare prepare{&heard};
    Collected collected;
    auto transcriber = make({}, prepare.make(), collected.sink());
    enable_and_wait_ready(*transcriber);

    transcriber->submit(tone(0, 0.1F));
    REQUIRE(eventually([&] { return transcriber->status().errors == 1; }));
    CHECK(transcriber->status().state == ModelState::Ready);
    CHECK(transcriber->status().detail.find("device was lost") != std::string::npos);

    transcriber->submit(tone(1, 0.1F));
    REQUIRE(eventually([&] { return collected.count() == 1; }));
    CHECK(transcriber->status().detail.empty());

    // Two, then a success, then three: only the run of three fails it.
    failures_left = 2;
    transcriber->submit(tone(2, 0.1F));
    transcriber->submit(tone(3, 0.1F));
    transcriber->submit(tone(4, 0.1F));
    REQUIRE(eventually([&] { return collected.count() == 2; }));
    CHECK(transcriber->status().state == ModelState::Ready);

    failures_left = 3;
    transcriber->submit(tone(5, 0.1F));
    transcriber->submit(tone(6, 0.1F));
    transcriber->submit(tone(7, 0.1F));
    REQUIRE(eventually([&] { return transcriber->status().state == ModelState::Failed; }));
    const TranscriberStatus failed = transcriber->status();
    CHECK(failed.errors == 6);
    CHECK(failed.detail.find("3 utterances in a row") != std::string::npos);
    CHECK_FALSE(transcriber->ready());

    transcriber->submit(tone(8, 0.1F));
    CHECK(transcriber->status().dropped == failed.dropped + 1);

    transcriber->set_enabled(false);
    enable_and_wait_ready(*transcriber);
    CHECK(prepare.calls == 2);
    transcriber->submit(tone(9, 0.1F));
    REQUIRE(eventually([&] { return collected.count() == 3; }));
}

// REJECTS: a status that reports nothing while a 1.6 GB download runs, a
// model name or backend that never reaches it, and counters that do not add
// up to what was submitted.
TEST_CASE("status reports the prepare's progress and every counter", "[transcribe][transcriber]") {
    Heard heard;
    heard.respond = [](std::span<const float> pcm) -> Expected<std::vector<RecognisedSegment>> {
        if (pcm.size() == kRecogniserRateHz / 4) {
            return confident("(music)");
        }
        return confident("Engine four en route.");
    };
    Gate downloading;
    Gate loading;
    const Prepare prepare = [&](const PrepareReport& report,
                                const std::atomic<bool>&) -> Expected<std::unique_ptr<Recogniser>> {
        report(ModelState::Downloading, 500, 1000);
        downloading.wait();
        report(ModelState::Loading, 1000, 1000);
        // Ignored: the outcome is the return value, not a report.
        report(ModelState::Ready, 0, 0);
        loading.wait();
        return std::make_unique<FakeRecogniser>(heard);
    };
    Collected collected;
    TranscriberOptions options;
    options.model_name = "ggml-fake.bin";
    auto transcriber = make(options, prepare, collected.sink());

    TranscriberStatus status = transcriber->status();
    CHECK_FALSE(status.enabled);
    CHECK(status.state == ModelState::Absent);
    CHECK(status.model_name == "ggml-fake.bin");

    transcriber->set_enabled(true);
    REQUIRE(downloading.entered(1));
    status = transcriber->status();
    CHECK(status.enabled);
    CHECK(status.state == ModelState::Downloading);
    CHECK(status.bytes_done == 500);
    CHECK(status.bytes_total == 1000);

    downloading.release();
    REQUIRE(loading.entered(1));
    CHECK(transcriber->status().state == ModelState::Loading);
    CHECK(transcriber->status().bytes_done == 1000);
    loading.release();
    REQUIRE(eventually([&] { return transcriber->ready(); }));
    CHECK(transcriber->status().backend == "fake backend");

    transcriber->submit(tone(0, 0.1F));
    transcriber->submit(tone(1, 0.1F, 0.25));
    transcriber->submit(tone(2, 0.1F));
    REQUIRE(eventually([&] {
        const TranscriberStatus now = transcriber->status();
        return now.transcribed + now.rejected == 3;
    }));
    status = transcriber->status();
    CHECK(status.transcribed == 2);
    CHECK(status.rejected == 1);
    CHECK(status.dropped == 0);
    CHECK(status.errors == 0);
    CHECK(status.queued == 0);
    CHECK(status.last_latency_ms > 0.0);
}

// REJECTS: a worker left at normal priority, where it competes on equal terms
// with the RPC loop that fans out the display, and above the owner's order of
// precedence for the least urgent work in the process.
TEST_CASE("the recogniser runs on a thread below normal priority", "[transcribe][transcriber]") {
#if defined(_WIN32)
    Heard heard;
    CountingPrepare prepare{&heard};
    Collected collected;
    auto transcriber = make({}, prepare.make(), collected.sink());
    enable_and_wait_ready(*transcriber);
    transcriber->submit(tone(0, 0.1F));
    REQUIRE(eventually([&] { return collected.count() == 1; }));
    const std::lock_guard lock(heard.mutex);
    CHECK(heard.worker_priority == THREAD_PRIORITY_BELOW_NORMAL);
#else
    SKIP("thread priorities are only set on Windows");
#endif
}

// REJECTS: a transcriber that accepts a queue with no room, which would evict
// every utterance as it arrived, or that starts without a prepare or a sink and
// fails only when the switch is first turned on.
TEST_CASE("create refuses options it cannot run with", "[transcribe][transcriber]") {
    Heard heard;
    CountingPrepare prepare{&heard};
    Collected collected;

    TranscriberOptions no_room;
    no_room.max_queued = 0;
    CHECK_FALSE(Transcriber::create(no_room, prepare.make(), collected.sink()).has_value());
    CHECK_FALSE(Transcriber::create({}, Prepare{}, collected.sink()).has_value());
    CHECK_FALSE(Transcriber::create({}, prepare.make(), Transcriber::Sink{}).has_value());
    CHECK(prepare.calls == 0);
}

}  // namespace revenant::transcribe
