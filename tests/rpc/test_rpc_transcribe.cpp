// Speech to text through the engine and the socket: a P25 call on a p25p1
// receiver comes out as one transcript, placed on the source clock and the
// receiver's frequency, carrying the call's talkgroup.
//
// WHAT THESE CASES CLAIM
//
// The server half of the owner decisions of 2026-10-03: one switch, every
// receiver that makes speech, a digital voice receiver cut into utterances by
// its calls. The recogniser here is a fake, so nothing is downloaded and no
// model runs: what is under test is that the right audio reaches it, cut in
// the right place, and that what it says comes back over the wire with the
// receiver's whereabouts on it. Whisper itself is tests/transcribe's.
//
// The call is tests/rpc/test_rpc_voice.cpp's construction, cut down: a header
// and LDUs carrying the talkgroup, in clear or encrypted.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <numbers>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/decode/p25p1.h"
#include "core/dsp/synth/dv_mod.h"
#include "core/dsp/types.h"
#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/decoders.h"
#include "core/rpc/types.h"
#include "core/rpc/voice_audio.h"
#include "core/transcribe/transcriber.h"
#include "tests/decode/imbe_test_frames.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"
#include "tests/support/temp_path.h"

using namespace revenant;
using test::Harness;
using test::HarnessOptions;

namespace {

using VoiceFrame = std::array<std::uint8_t, decode::kP25VoiceFrameBits>;

constexpr std::uint16_t kNac = 0x293;
constexpr std::uint16_t kTalkgroup = 0x02A7;
constexpr std::size_t kCallFrames = 36;

constexpr std::uint32_t kGridChannels = 4;
constexpr dsp::SampleRate kFileRate = 288'000;
constexpr dsp::Hertz kCarrierHz = 5'000;
constexpr std::uint32_t kBlockSamples = 16'384;
constexpr std::int64_t kFileCenterHz = 420'000'000;

// Silence either side of the call in the file, long enough for the
// segmenter's hangover to close the utterance before the file ends.
constexpr double kQuietSeconds = 1.0;

[[nodiscard]] std::vector<VoiceFrame> voice_frames(std::size_t count) {
    std::mt19937_64 rng(0x9E37'79B9'0025'0001ULL);
    std::vector<VoiceFrame> out;
    out.reserve(count);
    for (std::size_t f = 0; f < count; ++f) {
        decode::imbe_test::Quantizers q = decode::imbe_test::loud_frame(
            static_cast<std::uint32_t>((f * 23U + 11U) % 208U), (f / 3U) % 2U == 0U,
            static_cast<std::uint32_t>(30U + rng() % 30U));
        for (std::uint32_t& value : q.spectral) {
            value = static_cast<std::uint32_t>(rng() & 0xFFFFU);
        }
        q.sync = static_cast<std::uint32_t>(f & 1U);
        const std::vector<std::uint8_t> bits =
            decode::imbe_test::channel_frame(decode::imbe_test::prioritize(q));
        VoiceFrame frame{};
        std::copy(bits.begin(), bits.end(), frame.begin());
        out.push_back(frame);
    }
    return out;
}

[[nodiscard]] siggen::P25VoiceMessage call(bool encrypted) {
    siggen::P25VoiceMessage message;
    message.network_access_code = kNac;
    const std::uint8_t algid = encrypted ? std::uint8_t{0x84} : decode::kP25AlgidUnencrypted;
    decode::P25Header header;
    header.algorithm_id = algid;
    header.key_id = encrypted ? std::uint16_t{0x1234} : std::uint16_t{0};
    header.talkgroup_id = kTalkgroup;
    if (encrypted) {
        for (std::size_t i = 0; i < header.message_indicator.size(); ++i) {
            header.message_indicator[i] = static_cast<std::uint8_t>(0x11 * (i + 1));
            message.message_indicator[i] = static_cast<std::uint8_t>(0xA0 + i);
        }
    }
    message.header = header;
    message.link_control = {0x00, 0x00, 0x00, 0x00, static_cast<std::uint8_t>(kTalkgroup >> 8U),
                            static_cast<std::uint8_t>(kTalkgroup & 0xFFU), 0x12, 0xD6, 0x87};
    message.algorithm_id = algid;
    message.key_id = header.key_id;
    message.voice = voice_frames(kCallFrames);
    return message;
}

void random_dibits(std::vector<std::uint8_t>& out, std::size_t count, std::uint64_t seed) {
    std::uint64_t state = seed;
    for (std::size_t i = 0; i < count; ++i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        out.push_back(static_cast<std::uint8_t>((state >> 33U) & 0x3U));
    }
}

// The call at kFileRate, moved kCarrierHz up, with kQuietSeconds of nothing
// either side. `call_start` and `call_end` are where the modulated call sits in
// the file, which is the source clock.
struct Rendered {
    std::vector<dsp::Complex32> iq;
    std::uint64_t call_start = 0;
    std::uint64_t call_end = 0;
};

[[nodiscard]] Expected<Rendered> rendered_call(bool encrypted) {
    std::vector<std::uint8_t> dibits;
    random_dibits(dibits, 300, 0x1EAD'0000'0000'0001ULL);
    auto body = siggen::p25_voice_message_dibits(call(encrypted));
    if (!body) {
        return std::unexpected(body.error());
    }
    dibits.insert(dibits.end(), body->begin(), body->end());
    random_dibits(dibits, 200, 0x7A11'0000'0000'0001ULL);

    siggen::P25ModConfig mod;
    mod.filter_taps = rpc::decoders_detail::scaled_taps(mod.filter_taps, mod.rate, kFileRate);
    mod.rate = kFileRate;
    auto signal = siggen::p25_render_dibits(mod, dibits);
    if (!signal) {
        return std::unexpected(signal.error());
    }

    Rendered out;
    const auto quiet = static_cast<std::size_t>(kQuietSeconds * static_cast<double>(kFileRate));
    out.iq.assign(quiet, dsp::Complex32{});
    out.call_start = out.iq.size();
    for (std::size_t n = 0; n < signal->size(); ++n) {
        const std::int64_t turns = (kCarrierHz * static_cast<std::int64_t>(n)) % kFileRate;
        const double angle = 2.0 * std::numbers::pi * static_cast<double>(turns) /
                             static_cast<double>(kFileRate);
        const std::complex<double> moved =
            std::complex<double>((*signal)[n]) * std::polar(1.0, angle);
        out.iq.emplace_back(static_cast<float>(moved.real()), static_cast<float>(moved.imag()));
    }
    out.call_end = out.iq.size();
    out.iq.insert(out.iq.end(), quiet, dsp::Complex32{});
    return out;
}

class Capture {
public:
    Capture() : path_(test::unique_temp_path("revenant-transcribe", ".cf32")) {}
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    ~Capture() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] Status write(std::span<const dsp::Complex32> iq) {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        if (!out) {
            return fail(std::format("could not write {}", path_.string()));
        }
        out.write(reinterpret_cast<const char*>(iq.data()),
                  static_cast<std::streamsize>(iq.size() * sizeof(dsp::Complex32)));
        if (!out) {
            return fail(std::format("writing {} failed part way through", path_.string()));
        }
        samples_ = iq.size();
        return {};
    }

    [[nodiscard]] std::string uri() const {
        return std::format("file:///{}?rate={}&format=cf32&center={}", path_.generic_string(),
                           kFileRate, kFileCenterHz);
    }
    [[nodiscard]] std::uint64_t samples() const { return samples_; }

private:
    std::filesystem::path path_;
    std::uint64_t samples_ = 0;
};

// Says how much audio it was given and how loud, so a case can tell a call
// from silence without a model.
class FakeRecogniser final : public transcribe::Recogniser {
public:
    explicit FakeRecogniser(std::shared_ptr<std::atomic<int>> calls) : calls_(std::move(calls)) {}

    Expected<std::vector<transcribe::RecognisedSegment>> recognise(
        std::span<const float> pcm16k) override {
        calls_->fetch_add(1);
        double energy = 0.0;
        for (const float v : pcm16k) {
            energy += static_cast<double>(v) * v;
        }
        transcribe::RecognisedSegment segment;
        segment.text = std::format("heard {} ms", pcm16k.size() / 16);
        segment.no_speech_prob = energy > 0.0 ? 0.01F : 0.99F;
        segment.avg_logprob = energy > 0.0 ? -0.1F : -3.0F;
        return std::vector<transcribe::RecognisedSegment>{segment};
    }

    [[nodiscard]] std::string backend() const override { return "fake"; }

private:
    std::shared_ptr<std::atomic<int>> calls_;
};

[[nodiscard]] transcribe::Prepare fake_prepare(std::shared_ptr<std::atomic<int>> calls) {
    return [calls](const transcribe::PrepareReport& report,
                   const std::atomic<bool>&) -> Expected<std::unique_ptr<transcribe::Recogniser>> {
        report(transcribe::ModelState::Loading, 0, 0);
        return std::unique_ptr<transcribe::Recogniser>(std::make_unique<FakeRecogniser>(calls));
    };
}

class Transcripts {
public:
    void record(const rpc::Transcript& transcript) {
        const std::lock_guard<std::mutex> held(lock_);
        seen_.push_back(transcript);
    }
    [[nodiscard]] std::vector<rpc::Transcript> seen() const {
        const std::lock_guard<std::mutex> held(lock_);
        return seen_;
    }

private:
    mutable std::mutex lock_;
    std::vector<rpc::Transcript> seen_;
};

struct Outcome {
    std::vector<rpc::Transcript> transcripts;
    Rendered rendered;
    std::uint64_t vrx = 0;
    bool transcribing_before = false;
    int recognised = 0;
};

// Runs one call through a p25p1 receiver with the switch as asked and the
// receiver's own choice as asked, and hands back what crossed.
[[nodiscard]] Outcome run_call(bool encrypted, bool switch_on, rpc::TranscribeChoice choice) {
    auto rendered = rendered_call(encrypted);
    INFO(test::message_of(rendered));
    REQUIRE(rendered.has_value());
    Capture file;
    const auto wrote = file.write(rendered->iq);
    INFO(test::message_of(wrote));
    REQUIRE(wrote.has_value());

    auto calls = std::make_shared<std::atomic<int>>(0);
    Harness harness;
    HarnessOptions options;
    options.source_uri = file.uri();
    options.channels = kGridChannels;
    options.block_samples = kBlockSamples;
    options.transcribe_prepare = fake_prepare(calls);
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    auto vrx = harness.client().add_vrx(
        rpc::VrxParams{.center = kCarrierHz, .bandwidth = 0, .demod = rpc::Demod::P25p1});
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto heard = std::make_shared<Transcripts>();
    const auto subscribed = harness.client().subscribe_transcripts(
        [heard](const rpc::Transcript& t) { heard->record(t); }, {});
    INFO(test::message_of(subscribed));
    REQUIRE(subscribed.has_value());

    // The switch, and then the recogniser coming ready on the transcriber's
    // thread. The taps go on at the next reconcile, which the per-receiver
    // call below forces, so the file cannot run past them before they exist.
    if (switch_on) {
        const auto set = harness.client().set_transcription(true);
        INFO(test::message_of(set));
        REQUIRE(set.has_value());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            auto status = harness.client().transcription_status();
            REQUIRE(status.has_value());
            if (status->model_state == rpc::TranscriptionModelState::Ready) {
                break;
            }
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    const auto chose = harness.client().set_vrx_transcribe(*vrx, choice);
    INFO(test::message_of(chose));
    REQUIRE(chose.has_value());

    Outcome out;
    out.vrx = *vrx;
    auto status = harness.client().vrx_status(*vrx);
    REQUIRE(status.has_value());
    out.transcribing_before = status->transcribing;

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());
    const std::uint64_t blocks = (file.samples() + kBlockSamples - 1) / kBlockSamples;
    CHECK(harness.wait_for_blocks(blocks, 60'000) >= blocks);

    // The utterance closes a hangover after the call and is then recognised
    // on another thread and sent; wait for it rather than for a fixed time.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (switch_on && choice != rpc::TranscribeChoice::Off && !encrypted &&
           heard->seen().empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // And a little longer either way, so a case expecting nothing would see
    // something that arrived late.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const auto stopped = harness.stop_engine();
    INFO(test::message_of(stopped));
    CHECK(stopped.has_value());
    harness.client().unsubscribe_transcripts();

    out.transcripts = heard->seen();
    out.rendered = std::move(*rendered);
    out.recognised = calls->load();
    return out;
}

}  // namespace

TEST_CASE("a clear P25 call is one transcript, on the call's rows and frequency",
          "[gpu][rpc][transcribe]") {
    REVENANT_NEEDS_GPU();

    const Outcome out = run_call(false, true, rpc::TranscribeChoice::Auto);
    CHECK(out.transcribing_before);
    REQUIRE(out.transcripts.size() == 1);
    const rpc::Transcript& t = out.transcripts.front();

    CHECK(t.vrx == out.vrx);
    CHECK(t.mode == "p25p1");
    CHECK(t.text.starts_with("heard "));
    CHECK(t.center_hz == kFileCenterHz + kCarrierHz);
    CHECK(t.low_hz < t.center_hz);
    CHECK(t.high_hz > t.center_hz);

    // On the source clock, inside the call give or take the voice stream's
    // pre-roll and the segmenter's frame: rejects a transcript stamped with
    // the receiver's own stream index, which at 8000 S/s against 288000 would
    // land a thirty-sixth of the way into the file.
    const auto slack = static_cast<std::uint64_t>(0.5 * kFileRate);
    INFO(std::format("transcript [{}, {}) against the call [{}, {})", t.source_start,
                     t.source_end, out.rendered.call_start, out.rendered.call_end));
    CHECK(t.source_start + slack >= out.rendered.call_start);
    CHECK(t.source_end <= out.rendered.call_end + slack);
    CHECK(t.source_end > t.source_start);

    // The call's own description of itself.
    const rpc::DecodedField* talkgroup = nullptr;
    for (const rpc::DecodedField& field : t.fields) {
        if (field.key == "talkgroup") {
            talkgroup = &field;
        }
    }
    REQUIRE(talkgroup != nullptr);
    REQUIRE(talkgroup->integer() != nullptr);
    CHECK(*talkgroup->integer() == kTalkgroup);
}

TEST_CASE("an encrypted call reaches the recogniser not at all", "[gpu][rpc][transcribe]") {
    REVENANT_NEEDS_GPU();

    // Rejects a tap that cuts by the receiver's carrier rather than by its
    // voice: an encrypted call is on the air and has no voice to transcribe,
    // and docs/modes.md's line is that nothing is attempted on it.
    const Outcome out = run_call(true, true, rpc::TranscribeChoice::Auto);
    CHECK(out.transcripts.empty());
    CHECK(out.recognised == 0);
}

TEST_CASE("nothing is transcribed with the switch off or the receiver chosen out",
          "[gpu][rpc][transcribe]") {
    REVENANT_NEEDS_GPU();

    SECTION("switch off") {
        const Outcome out = run_call(false, false, rpc::TranscribeChoice::Auto);
        CHECK_FALSE(out.transcribing_before);
        CHECK(out.transcripts.empty());
    }
    SECTION("receiver off") {
        const Outcome out = run_call(false, true, rpc::TranscribeChoice::Off);
        CHECK_FALSE(out.transcribing_before);
        CHECK(out.transcripts.empty());
    }
}

TEST_CASE("an engine without a recogniser says so in words", "[rpc][transcribe]") {
    Harness harness;
    HarnessOptions options;
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    const auto set = harness.client().set_transcription(true);
    REQUIRE_FALSE(set.has_value());
    CHECK(set.error().message.find("without speech to text") != std::string::npos);

    const auto status = harness.client().transcription_status();
    REQUIRE(status.has_value());
    CHECK_FALSE(status->enabled);
}
