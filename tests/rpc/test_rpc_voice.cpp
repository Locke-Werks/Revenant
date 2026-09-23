// P25 voice as a receiver's audio: core/rpc/voice_audio.h on its own, then
// subscribeAudio on a p25p1 receiver through the engine and the socket.
//
// WHAT THESE CASES CLAIM
//
// Owner decision, 2026-09-23: a P25 receiver plays the decoded voice in place
// of its analog audio, is silent between calls and plays nothing for an
// encrypted one. subscribeAudio refused every p25p1 receiver before this, and
// the refusal is what the first case in each half replaces.
//
// The voice frames are tests/decode/test_p25p1_voice.cpp's: quantizer values
// arranged into IMBE channel frames by tests/decode/imbe_test_frames.h and
// carried in LDUs by core/dsp/synth/dv_mod.h. The PCM a clear call has to
// produce is the vocoder run straight over those frames, so a case can ask for
// it sample for sample rather than for "something non-zero".

#include <algorithm>
#include <array>
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

#include "core/decode/imbe.h"
#include "core/decode/p25p1.h"
#include "core/dsp/synth/dv_mod.h"
#include "core/dsp/types.h"
#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/decoders.h"
#include "core/rpc/types.h"
#include "core/rpc/voice_audio.h"
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

// test_p25p1_voice.cpp's frames, seed and all, so the two suites carry the
// same call.
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

// A call with a header, in clear or encrypted with ALGID 0x84, which is AES.
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

// Pseudorandom dibits, standing in for whatever the channel carries either
// side of the call. test_p25p1_voice.cpp says why a receiver needs some.
void random_dibits(std::vector<std::uint8_t>& out, std::size_t count, std::uint64_t seed) {
    std::uint64_t state = seed;
    for (std::size_t i = 0; i < count; ++i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        out.push_back(static_cast<std::uint8_t>((state >> 33U) & 0x3U));
    }
}

[[nodiscard]] Expected<std::vector<dsp::Complex32>> rendered_call(bool encrypted,
                                                                   dsp::SampleRate rate) {
    std::vector<std::uint8_t> dibits;
    random_dibits(dibits, 300, 0x1EAD'0000'0000'0001ULL);
    auto body = siggen::p25_voice_message_dibits(call(encrypted));
    if (!body) {
        return std::unexpected(body.error());
    }
    dibits.insert(dibits.end(), body->begin(), body->end());
    random_dibits(dibits, 200, 0x7A11'0000'0000'0001ULL);

    siggen::P25ModConfig mod;
    mod.filter_taps = rpc::decoders_detail::scaled_taps(mod.filter_taps, mod.rate, rate);
    mod.rate = rate;
    auto rendered = siggen::p25_render_dibits(mod, dibits);
    if (!rendered) {
        return rendered;
    }

    // Half a second of nothing after the call. The last LDU's voice is played
    // out behind a pre-roll, so it leaves the stream after the air has gone
    // quiet, and a capture that stopped with the call would cut it off.
    rendered->insert(rendered->end(), static_cast<std::size_t>(rate / 2), dsp::Complex32{});
    return rendered;
}

// The vocoder run straight over the frames, with nothing in between.
[[nodiscard]] std::vector<float> reference_pcm(std::span<const VoiceFrame> frames) {
    decode::ImbeDecoder decoder;
    std::vector<float> pcm;
    std::array<float, decode::ImbeDecoder::kPcmFrames> block{};
    for (const VoiceFrame& frame : frames) {
        REQUIRE(decoder.decode(frame, block).has_value());
        pcm.insert(pcm.end(), block.begin(), block.end());
    }
    return pcm;
}

// Exact zeros off both ends. The voice sits between the pre-roll and the
// silence after the call, and both are written as 0.0F, so what is left is
// the voice and whatever gap an underrun put in the middle of it.
[[nodiscard]] std::vector<float> trimmed(std::span<const float> samples) {
    auto first = std::find_if(samples.begin(), samples.end(), [](float v) { return v != 0.0F; });
    auto last = std::find_if(samples.rbegin(), samples.rend(), [](float v) { return v != 0.0F; });
    if (first == samples.end()) {
        return {};
    }
    return std::vector<float>(first, last.base());
}

// The whole stream through P25AudioStream a block at a time, as the server's
// sink hands it over, gathered into one timeline.
struct Streamed {
    std::vector<float> pcm;
    std::size_t voiced_chunks = 0;
    bool contiguous = true;
};

[[nodiscard]] Streamed stream(std::span<const dsp::Complex32> iq, dsp::SampleRate rate,
                              std::size_t block) {
    auto made = rpc::P25AudioStream::create(rate);
    REQUIRE(made.has_value());
    std::vector<float> interleaved;
    Streamed out;
    rpc::VoiceChunk chunk;
    std::uint64_t next = 0;
    for (std::size_t at = 0; at < iq.size(); at += block) {
        const std::size_t count = std::min(block, iq.size() - at);
        interleaved.resize(count * 2);
        for (std::size_t i = 0; i < count; ++i) {
            interleaved[2 * i] = iq[at + i].real();
            interleaved[2 * i + 1] = iq[at + i].imag();
        }
        const rpc::DecoderChunk in{
            .samples = interleaved, .channels = 2, .rate = rate, .start = at};
        const auto processed = made->process(in, false, chunk);
        INFO(test::message_of(processed));
        REQUIRE(processed.has_value());
        out.contiguous = out.contiguous && chunk.start == next;
        next = chunk.start + chunk.samples.size();
        out.voiced_chunks += chunk.voiced ? 1U : 0U;
        out.pcm.insert(out.pcm.end(), chunk.samples.begin(), chunk.samples.end());
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The stream on its own
// ---------------------------------------------------------------------------

TEST_CASE("a clear call's voice comes out whole at 8000 S/s however the input is blocked",
          "[rpc][voice]") {
    constexpr dsp::SampleRate kRate = 48'000;
    auto iq = rendered_call(false, kRate);
    INFO(test::message_of(iq));
    REQUIRE(iq.has_value());

    const std::vector<float> reference = reference_pcm(voice_frames(kCallFrames));

    // 328 is the fine stage's chunk at the engine's 16384-sample blocks on a
    // 2.4 MS/s source, 2731 the 288000 S/s capture the wire case plays, and
    // 16384 a block longer than a whole LDU.
    for (const std::size_t block : {std::size_t{328}, std::size_t{2731}, std::size_t{16384}}) {
        INFO("block " << block);
        const Streamed out = stream(*iq, kRate, block);

        // One index for the whole stream at the voice rate, derived from the
        // receiver's own, with nothing skipped and nothing counted twice.
        CHECK(out.contiguous);
        CHECK(out.pcm.size() ==
              static_cast<std::size_t>(iq->size() * rpc::kP25VoiceRateHz / kRate));

        // The voice is the vocoder's PCM for the frames that went in, to the
        // bit, with no gap in the middle of it. A fixed 50 ms pre-roll, which
        // is shorter than a 16384-sample chunk, underran at that blocking and
        // put a run of zeros inside the call; at 328 and 2731 the LDUs happened
        // to land early enough in their chunks that it did not.
        CHECK(trimmed(out.pcm) == trimmed(reference));
        CHECK(out.voiced_chunks > 0);
    }
}

TEST_CASE("an encrypted call is silence at the full rate", "[rpc][voice]") {
    constexpr dsp::SampleRate kRate = 48'000;
    auto iq = rendered_call(true, kRate);
    INFO(test::message_of(iq));
    REQUIRE(iq.has_value());

    const Streamed out = stream(*iq, kRate, 328);
    CHECK(out.contiguous);
    CHECK(out.pcm.size() == static_cast<std::size_t>(iq->size() * rpc::kP25VoiceRateHz / kRate));
    CHECK(out.voiced_chunks == 0);
    CHECK(std::ranges::all_of(out.pcm, [](float v) { return v == 0.0F; }));
}

TEST_CASE("the voice stream refuses what is not complex baseband at its rate",
          "[rpc][voice]") {
    auto made = rpc::P25AudioStream::create(48'000);
    REQUIRE(made.has_value());
    const std::vector<float> mono(480, 0.0F);
    rpc::VoiceChunk out;

    const auto audio = made->process(
        rpc::DecoderChunk{.samples = mono, .channels = 1, .rate = 48'000, .start = 0}, false, out);
    REQUIRE_FALSE(audio.has_value());
    CHECK(audio.error().message.find("complex baseband") != std::string::npos);

    const auto rate = made->process(
        rpc::DecoderChunk{.samples = mono, .channels = 2, .rate = 72'000, .start = 0}, false, out);
    REQUIRE_FALSE(rate.has_value());
    CHECK(rate.error().message.find("72000") != std::string::npos);
}

TEST_CASE("a fenced chunk is silence and is not decoded", "[rpc][voice]") {
    constexpr dsp::SampleRate kRate = 48'000;
    auto iq = rendered_call(false, kRate);
    REQUIRE(iq.has_value());

    auto made = rpc::P25AudioStream::create(kRate);
    REQUIRE(made.has_value());
    std::vector<float> interleaved(iq->size() * 2);
    for (std::size_t i = 0; i < iq->size(); ++i) {
        interleaved[2 * i] = (*iq)[i].real();
        interleaved[2 * i + 1] = (*iq)[i].imag();
    }
    rpc::VoiceChunk out;
    const auto processed = made->process(
        rpc::DecoderChunk{.samples = interleaved, .channels = 2, .rate = kRate, .start = 0}, true,
        out);
    REQUIRE(processed.has_value());
    CHECK(out.samples.size() == iq->size() * rpc::kP25VoiceRateHz / kRate);
    CHECK_FALSE(out.voiced);
    CHECK(std::ranges::all_of(out.samples, [](float v) { return v == 0.0F; }));
    CHECK(made->call().frames_decoded == 0);
}

// ---------------------------------------------------------------------------
// Through the engine and the socket
// ---------------------------------------------------------------------------

namespace {

// The grid tests/rpc/test_rpc_decode.cpp plays its P25 capture through: 288000
// S/s over four channels, the carrier 5 kHz above channel 0's centre.
constexpr std::uint32_t kGridChannels = 4;
constexpr dsp::SampleRate kFileRate = 288'000;
constexpr dsp::Hertz kCarrierHz = 5'000;
constexpr std::uint32_t kBlockSamples = 16'384;
constexpr double kQuietSeconds = 0.5;

class VoiceCapture {
public:
    explicit VoiceCapture(std::string_view tag)
        : path_(test::unique_temp_path(std::format("revenant-voice-{}", tag), ".cf32")) {}

    VoiceCapture(const VoiceCapture&) = delete;
    VoiceCapture& operator=(const VoiceCapture&) = delete;

    ~VoiceCapture() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] Status write(std::span<const dsp::Complex32> signal) {
        const auto quiet =
            static_cast<std::size_t>(kQuietSeconds * static_cast<double>(kFileRate));
        std::vector<dsp::Complex32> all(quiet, dsp::Complex32{});
        for (std::size_t n = 0; n < signal.size(); ++n) {
            const std::int64_t turns =
                (kCarrierHz * static_cast<std::int64_t>(n)) % kFileRate;
            const double angle = 2.0 * std::numbers::pi * static_cast<double>(turns) /
                                 static_cast<double>(kFileRate);
            const std::complex<double> moved =
                std::complex<double>(signal[n]) * std::polar(1.0, angle);
            all.emplace_back(static_cast<float>(moved.real()), static_cast<float>(moved.imag()));
        }
        all.insert(all.end(), quiet, dsp::Complex32{});
        samples_ = all.size();

        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        if (!out) {
            return fail(std::format("could not write {}", path_.string()));
        }
        out.write(reinterpret_cast<const char*>(all.data()),
                  static_cast<std::streamsize>(all.size() * sizeof(dsp::Complex32)));
        if (!out) {
            return fail(std::format("writing {} failed part way through", path_.string()));
        }
        return {};
    }

    [[nodiscard]] std::string uri() const {
        return std::format("file:///{}?rate={}&format=cf32&center=420000000",
                           path_.generic_string(), kFileRate);
    }
    [[nodiscard]] dsp::SampleIndex samples() const { return samples_; }

private:
    std::filesystem::path path_;
    dsp::SampleIndex samples_ = 0;
};

struct Heard {
    std::uint64_t start = 0;
    std::uint32_t rate = 0;
    std::uint16_t channels = 0;
    bool open = false;
    std::vector<float> samples;
};

class Listener {
public:
    void record(const rpc::AudioChunk& chunk) {
        const std::lock_guard<std::mutex> held(lock_);
        heard_.push_back(Heard{chunk.sample_index, chunk.sample_rate, chunk.channel_count,
                               chunk.squelch_open, chunk.samples});
    }
    void end(const std::string& reason) {
        const std::lock_guard<std::mutex> held(lock_);
        ended_ = reason;
    }
    [[nodiscard]] std::vector<Heard> heard() const {
        const std::lock_guard<std::mutex> held(lock_);
        return heard_;
    }
    [[nodiscard]] std::uint64_t frames() const {
        const std::lock_guard<std::mutex> held(lock_);
        std::uint64_t total = 0;
        for (const Heard& one : heard_) {
            total += one.samples.size();
        }
        return total;
    }
    [[nodiscard]] std::string ended() const {
        const std::lock_guard<std::mutex> held(lock_);
        return ended_;
    }

private:
    mutable std::mutex lock_;
    std::vector<Heard> heard_;
    std::string ended_;
};

// Runs a call through a p25p1 receiver with subscribeAudio on it and hands
// back what crossed. The subscription asks for the deepest queue the server
// grants, five seconds, because the file runs unthrottled and is shorter
// than that: nothing is evicted, so every frame the route made arrives.
[[nodiscard]] std::vector<Heard> listen_to(bool encrypted) {
    auto iq = rendered_call(encrypted, kFileRate);
    INFO(test::message_of(iq));
    REQUIRE(iq.has_value());

    VoiceCapture file(encrypted ? "encrypted" : "clear");
    const auto wrote = file.write(*iq);
    INFO(test::message_of(wrote));
    REQUIRE(wrote.has_value());

    Harness harness;
    HarnessOptions options;
    options.source_uri = file.uri();
    options.channels = kGridChannels;
    options.block_samples = kBlockSamples;
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    auto vrx = harness.client().add_vrx(
        rpc::VrxParams{.center = kCarrierHz, .bandwidth = 0, .demod = rpc::Demod::P25p1});
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto listener = std::make_shared<Listener>();
    auto granted = harness.client().subscribe_audio(
        *vrx, 5'000, [listener](const rpc::AudioChunk& chunk) { listener->record(chunk); },
        [listener](const std::string& why) { listener->end(why); });
    INFO(test::message_of(granted));
    REQUIRE(granted.has_value());

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());
    const std::uint64_t blocks = (file.samples() + kBlockSamples - 1) / kBlockSamples;
    CHECK(harness.wait_for_blocks(blocks, 60'000) >= blocks);

    // Everything the engine delivered at 48000 S/s, at 8000, less the part of
    // the last block the fine stage was still holding.
    const auto expected = static_cast<std::uint64_t>(
        static_cast<double>(file.samples()) * rpc::kP25VoiceRateHz / kFileRate * 0.95);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (listener->frames() < expected && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto stopped = harness.stop_engine();
    INFO(test::message_of(stopped));
    CHECK(stopped.has_value());
    CHECK(listener->ended().empty());

    const std::vector<Heard> heard = listener->heard();
    INFO(std::format("{} frames crossed of the {} expected", listener->frames(), expected));
    REQUIRE(listener->frames() >= expected);
    harness.client().unsubscribe_audio(*vrx);
    return heard;
}

}  // namespace

TEST_CASE("subscribeAudio on a p25p1 receiver plays a clear call's voice",
          "[gpu][rpc][audio][voice]") {
    REVENANT_NEEDS_GPU();

    const std::vector<Heard> heard = listen_to(false);

    std::vector<float> timeline;
    std::uint64_t next = heard.front().start;
    bool contiguous = true;
    std::size_t voiced = 0;
    for (const Heard& chunk : heard) {
        CHECK(chunk.rate == rpc::kP25VoiceRateHz);
        CHECK(chunk.channels == 1);
        contiguous = contiguous && chunk.start == next;
        next = chunk.start + chunk.samples.size();
        voiced += chunk.open ? 1U : 0U;
        timeline.insert(timeline.end(), chunk.samples.begin(), chunk.samples.end());
    }

    // Silence either side of the call at the full rate, the voice in the
    // middle with the gate open, and the voice itself the vocoder's PCM for
    // the frames the transmitter sent, sample for sample.
    CHECK(contiguous);
    CHECK(voiced > 0);
    CHECK_FALSE(heard.front().open);
    CHECK_FALSE(heard.back().open);
    CHECK(trimmed(timeline) == trimmed(reference_pcm(voice_frames(kCallFrames))));
}

TEST_CASE("subscribeAudio on a p25p1 receiver plays nothing of an encrypted call",
          "[gpu][rpc][audio][voice]") {
    REVENANT_NEEDS_GPU();

    const std::vector<Heard> heard = listen_to(true);

    // Chunks keep coming at the full rate, which is what tells a listener the
    // receiver is still there, and every one of them is silence with the gate
    // shut.
    for (const Heard& chunk : heard) {
        CHECK(chunk.rate == rpc::kP25VoiceRateHz);
        CHECK_FALSE(chunk.open);
        CHECK(std::ranges::all_of(chunk.samples, [](float v) { return v == 0.0F; }));
    }
}
