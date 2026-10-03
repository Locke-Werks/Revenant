// core/transcribe/whisper_runner.h on the real model and the real device.
//
// SKIPPED, SAYING WHY, WHEN THE MODEL IS NOT ON THIS MACHINE. It is 1.6 GB and
// arrives on first use, so a fresh checkout or a runner that has never fetched
// it has nothing to load. revenant_transcribe_tests "[.download]" fetches it.
// REVENANT_REQUIRE_WHISPER=1 turns that skip into a failure, for the same
// reason REVENANT_REQUIRE_GPU exists: a green run that loaded nothing is the
// outcome the variable is there to prevent.
//
// The device is the one the engine would open, through the same fixture every
// GPU case in the tree uses, and its name is the hint. That is the
// integration the hint was designed for, so it is the one tested, and it is
// what keeps Whisper off the integrated Radeon on the development machine.
//
// A CRASH THAT IS NOT THIS CODE'S, RECORDED SO NOBODY CHASES IT HERE. On the
// development machine on 2026-10-03, about one Debug run in five died with an
// access violation inside vkCreateDevice as Whisper loaded, the stack running
// from ggml's createDevice through CheatHappensVulkanLayer_x64, an implicit
// layer a game trainer registers, into VkLayer_khronos_validation, which only
// the fixture's Debug context had enabled, on its own instance. Ten runs with
// every layer on crashed two. Twenty with DISABLE_CH_LAYER=1, that layer's own
// off switch, crashed none, and so did twenty with all five third-party
// layers switched off the same way. A layer forwarding one instance's call
// down another instance's chain is that layer's defect, not this code's.
// Release contexts enable no validation, and the Release runs behind
// tools/loadtest's figures did not crash.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "core/transcribe/model_store.h"
#include "core/transcribe/whisper_runner.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/transcribe/spoken_clip.h"

using namespace revenant;
using namespace revenant::transcribe;

namespace {

[[nodiscard]] bool whisper_required()
{
    const char* value = std::getenv("REVENANT_REQUIRE_WHISPER");
    return value != nullptr && std::string_view(value) == "1";
}

#define REVENANT_NEEDS_WHISPER_MODEL()                                                                    \
    do {                                                                                                  \
        const auto ready = model_ready(kDefaultModel);                                                    \
        if (!ready || !*ready) {                                                                          \
            const std::string why = ready ? "the Whisper model is not at " +                              \
                                                model_path(kDefaultModel).string() +                      \
                                                "; run revenant_transcribe_tests \"[.download]\" to "     \
                                                "fetch it"                                                \
                                          : ready.error().message;                                        \
            if (whisper_required()) {                                                                     \
                FAIL("REVENANT_REQUIRE_WHISPER is set: " << why);                                         \
            }                                                                                             \
            SKIP(why);                                                                                    \
        }                                                                                                 \
    } while (false)

// One loaded model for the binary. A load is seconds and 1.6 GB of device
// memory, and every case here only reads it.
//
// RELEASED BY THE LISTENER BELOW WHEN THE RUN ENDS, NOT AT EXIT. ggml keeps
// its Vulkan instance and devices in function-local statics that come into
// being during the first load, after this pointer did, so static destruction
// runs theirs first and a Whisper freed at exit reaches into a destroyed
// backend. On 2026-10-03 that was a SIGSEGV after the last assertion of a run
// that had passed, on some runs and not others, which Catch reported against
// whichever case ran last.
struct SharedWhisper {
    std::unique_ptr<Whisper> whisper;
    std::string failure;
    bool tried = false;
};

SharedWhisper& shared_state()
{
    static SharedWhisper state;
    return state;
}

Whisper* shared_whisper(std::string& why)
{
    SharedWhisper& state = shared_state();
    if (!state.tried) {
        state.tried = true;
        WhisperOptions options;
        options.model_path = model_path(kDefaultModel);
        options.gpu_hint = test::shared_context().info().name;
        options.word_timestamps = true;
        auto loaded = Whisper::load(options);
        if (loaded) {
            state.whisper = std::move(*loaded);
        } else {
            state.failure = loaded.error().message;
        }
    }
    why = state.failure;
    return state.whisper.get();
}

class ReleaseWhisper final : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;
    void testRunEnded(const Catch::TestRunStats&) override { shared_state().whisper.reset(); }
};

[[nodiscard]] std::string lowered_words(const std::vector<WhisperSegment>& segments)
{
    std::string text;
    for (const WhisperSegment& segment : segments) {
        text += ' ';
        text += segment.text;
    }
    for (char& c : text) {
        const auto byte = static_cast<unsigned char>(c);
        c = std::isalnum(byte) ? static_cast<char>(std::tolower(byte)) : ' ';
    }
    return text + ' ';
}

constexpr std::wstring_view kPangram =
    L"The quick brown fox jumps over the lazy dog. Weather at the field is clear, "
    L"wind two seven zero at ten knots.";

}  // namespace

CATCH_REGISTER_LISTENER(ReleaseWhisper)

TEST_CASE("Whisper transcribes synthetic speech on the engine's device", "[transcribe][whisper]")
{
    REVENANT_NEEDS_WHISPER_MODEL();
    REVENANT_NEEDS_GPU();

    auto clip = test::speak_16k(std::wstring(kPangram));
    if (!clip) {
        SKIP("no synthetic speech: " << clip.error().message);
    }

    std::string why;
    Whisper* whisper = shared_whisper(why);
    INFO(why);
    REQUIRE(whisper != nullptr);

    const std::string backend = whisper->backend_description();
    INFO("backend " << backend);
    CHECK(backend.starts_with("Vulkan"));
    CHECK(backend.find(test::shared_context().info().name) != std::string::npos);

    const auto started = std::chrono::steady_clock::now();
    auto segments = whisper->transcribe(*clip);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    REQUIRE(segments);
    REQUIRE_FALSE(segments->empty());

    const std::string text = lowered_words(*segments);
    std::println("whisper: {} on {:.2f} s of speech in {:.3f} s:{}", backend,
                 static_cast<double>(clip->size()) / kWhisperSampleRate, took, text);

    // Words a recogniser cannot miss in clean synthetic speech. Four of five,
    // because a voice that says "lazy" a little oddly is not what this case is
    // about; zero of five is.
    int heard = 0;
    for (const std::string_view word : {" quick ", " brown ", " fox ", " lazy ", " dog "}) {
        heard += text.find(word) != std::string::npos ? 1 : 0;
    }
    INFO("transcript:" << text);
    CHECK(heard >= 4);

    for (const WhisperSegment& segment : *segments) {
        INFO(segment.text);
        CHECK(segment.t1 >= segment.t0);
        CHECK(segment.t1 <= static_cast<float>(clip->size()) / kWhisperSampleRate + 1.0f);
        CHECK(segment.no_speech_prob >= 0.0f);
        CHECK(segment.no_speech_prob < 0.5f);
        CHECK(segment.avg_logprob <= 0.0f);
        CHECK(segment.avg_logprob > -1.0f);
        CHECK_FALSE(segment.words.empty());
        for (const WhisperWord& word : segment.words) {
            CHECK_FALSE(word.text.empty());
            CHECK(word.p > 0.0f);
            CHECK(word.p <= 1.0f);
        }
    }
}

TEST_CASE("Whisper returns nothing for nothing and refuses what it cannot take", "[transcribe][whisper]")
{
    REVENANT_NEEDS_WHISPER_MODEL();
    REVENANT_NEEDS_GPU();
    std::string why;
    Whisper* whisper = shared_whisper(why);
    INFO(why);
    REQUIRE(whisper != nullptr);

    const auto empty = whisper->transcribe({});
    REQUIRE(empty);
    CHECK(empty->empty());

    const std::vector<float> too_long(static_cast<std::size_t>(31 * kWhisperSampleRate), 0.0f);
    const auto refused = whisper->transcribe(too_long);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().message.find("30 s") != std::string::npos);

    std::vector<float> broken(kWhisperSampleRate, 0.0f);
    broken[100] = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(whisper->transcribe(broken));
}

TEST_CASE("Whisper refuses a device hint that names nothing", "[transcribe][whisper]")
{
    REVENANT_NEEDS_WHISPER_MODEL();
    WhisperOptions options;
    options.model_path = model_path(kDefaultModel);
    options.gpu_hint = "no such device anywhere";
    const auto loaded = Whisper::load(options);
    REQUIRE_FALSE(loaded);
    INFO(loaded.error().message);
    CHECK(loaded.error().message.find("no such device anywhere") != std::string::npos);
}

TEST_CASE("Whisper refuses a model that is not there", "[transcribe][whisper]")
{
    WhisperOptions options;
    options.model_path = model_directory() / "no-such-model.bin";
    const auto loaded = Whisper::load(options);
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().message.find("no-such-model.bin") != std::string::npos);
}

// Run by name: revenant_transcribe_tests "[.speed]". Whisper's latency on
// clips of 5 s and 15 s, each the head of one synthetic passage, warm, the
// median of five. Prints and asserts nothing about the numbers.
TEST_CASE("Whisper latency on 5 s and 15 s clips", "[.speed]")
{
    REVENANT_NEEDS_WHISPER_MODEL();
    REVENANT_NEEDS_GPU();
    auto passage = test::speak_16k(
        std::wstring(kPangram) +
        L" Revenant control, this is mobile four, we are on scene at the north gate and holding. "
        L"Requesting a second unit and an ambulance, two patients, both conscious and breathing. "
        L"Traffic is backed up to the interchange, advise inbound units to come in from the east.");
    if (!passage) {
        SKIP("no synthetic speech: " << passage.error().message);
    }
    std::string why;
    Whisper* whisper = shared_whisper(why);
    INFO(why);
    REQUIRE(whisper != nullptr);
    std::println("passage {:.1f} s, backend {}", static_cast<double>(passage->size()) / kWhisperSampleRate,
                 whisper->backend_description());
    REQUIRE(passage->size() >= 15 * kWhisperSampleRate);

    (void)whisper->transcribe(std::span(*passage).first(5 * kWhisperSampleRate));
    for (const std::size_t seconds : {5U, 15U}) {
        const auto clip = std::span<const float>(*passage).first(seconds * kWhisperSampleRate);
        std::vector<double> times;
        std::string text;
        for (int run = 0; run < 5; ++run) {
            const auto started = std::chrono::steady_clock::now();
            auto segments = whisper->transcribe(clip);
            times.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
            REQUIRE(segments);
            text = lowered_words(*segments);
        }
        std::ranges::sort(times);
        std::println("{:2} s clip: median {:.3f} s, min {:.3f} s, max {:.3f} s, {:.1f} s of audio per second;{}",
                     seconds, times[2], times[0], times[4], static_cast<double>(seconds) / times[2], text);
    }
}
