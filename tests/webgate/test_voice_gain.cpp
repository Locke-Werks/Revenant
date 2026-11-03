// tools/webgate/voice_gain.h: the phone hearing decoded voice at the level the
// desktop plays it. Each case names the wrong implementation it rejects.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "tools/webgate/voice_gain.h"

namespace {

using revenant::webgate::is_digital_voice;
using revenant::webgate::parse_voice_gain_db;
using revenant::webgate::VoiceGainSource;
using revenant::webgate::VoiceGainStage;
namespace rpc = revenant::rpc;
namespace ui = revenant::ui;

[[nodiscard]] std::vector<float> tone(std::size_t n, double amplitude, std::size_t start = 0)
{
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<float>(
            amplitude * std::sin(2.0 * 3.14159265358979323846 * 1000.0 *
                                 static_cast<double>(start + i) / 48000.0));
    }
    return v;
}

[[nodiscard]] float peak_of(std::span<const float> v, std::size_t from = 0)
{
    float p = 0.0F;
    for (std::size_t i = from; i < v.size(); ++i) {
        p = std::max(p, std::abs(v[i]));
    }
    return p;
}

}  // namespace

TEST_CASE("voiceGainDb parses by the desktop's rules", "[webgate][voicegain]")
{
    CHECK(parse_voice_gain_db("30") == 30.0);
    CHECK(parse_voice_gain_db("12") == 12.0);
    // Whole decibels: the control steps in them, and the desktop rounds.
    CHECK(parse_voice_gain_db("12.4") == 12.0);
    CHECK(parse_voice_gain_db("12.6") == 13.0);
    // Clamped to the travel, not wrapped or refused.
    CHECK(parse_voice_gain_db("99") == ui::kVoiceGainMaxDb);
    CHECK(parse_voice_gain_db("-5") == 0.0);
    // Not a number is zero, never silence and never the maximum.
    CHECK(parse_voice_gain_db("nan") == 0.0);
    CHECK(parse_voice_gain_db("loud") == 0.0);
    CHECK(parse_voice_gain_db("") == 0.0);
    CHECK(parse_voice_gain_db("12 dB") == 0.0);
    // Missing is the stream as the engine sent it.
    CHECK(parse_voice_gain_db(std::nullopt) == 0.0);
}

TEST_CASE("the boost is read from audio/voiceGainDb and cached", "[webgate][voicegain]")
{
    using Clock = VoiceGainSource::Clock;
    std::map<std::string, std::string> values{{"voiceGainDb", "20"}};
    int reads = 0;
    Clock::time_point now = Clock::now();
    VoiceGainSource source(
        [&](std::string_view name) -> std::optional<std::string> {
            ++reads;
            const auto at = values.find(std::string(name));
            return at == values.end() ? std::nullopt : std::optional(at->second);
        },
        [&] { return now; }, std::chrono::seconds(2));

    CHECK(source.current() == 20.0);
    values["voiceGainDb"] = "6";
    now += std::chrono::milliseconds(1900);
    // Inside the interval the registry is not touched again.
    CHECK(source.current() == 20.0);
    CHECK(reads == 1);
    now += std::chrono::milliseconds(200);
    CHECK(source.current() == 6.0);

    values.clear();
    now += std::chrono::seconds(3);
    CHECK(source.current() == 0.0);
}

TEST_CASE("only decoded voice modes are digital voice", "[webgate][voicegain]")
{
    CHECK(is_digital_voice(rpc::Demod::P25p1));
    CHECK(is_digital_voice(rpc::Demod::Dmr));
    CHECK(is_digital_voice(rpc::Demod::Dstar));
    CHECK(is_digital_voice(rpc::Demod::Tetra));
    CHECK_FALSE(is_digital_voice(rpc::Demod::Nfm));
    CHECK_FALSE(is_digital_voice(rpc::Demod::Wfm));
    CHECK_FALSE(is_digital_voice(rpc::Demod::Am));
    CHECK_FALSE(is_digital_voice(rpc::Demod::Usb));
    CHECK_FALSE(is_digital_voice(rpc::Demod::Raw));
}

TEST_CASE("voice is boosted by the configured decibels", "[webgate][voicegain]")
{
    VoiceGainStage stage;
    const auto in = tone(4800, 0.05);
    const auto processed = stage.process(12.0, in);
    const std::vector<float> out(processed.begin(), processed.end());
    // 12 dB is x3.981; 0.05 stays well under the limiter's threshold.
    REQUIRE(out.size() == in.size());
    CHECK(peak_of(out) == Catch::Approx(0.05 * 3.98107).epsilon(0.01));
    for (std::size_t i = 0; i < out.size(); ++i) {
        CHECK(out[i] == Catch::Approx(in[i] * 3.98107F).margin(1e-6));
    }
}

TEST_CASE("zero decibels below the threshold passes voice untouched", "[webgate][voicegain]")
{
    VoiceGainStage stage;
    const auto in = tone(4800, 0.3);
    const auto out = stage.process(0.0, in);
    REQUIRE(out.size() == in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        CHECK(out[i] == in[i]);
    }
}

TEST_CASE("the limiter holds a hot input under full scale", "[webgate][voicegain]")
{
    VoiceGainStage stage;
    // 0.5 at +30 dB is x15.8, nearly sixteen times full scale unlimited.
    for (int chunk = 0; chunk < 10; ++chunk) {
        const auto in = tone(960, 0.5, static_cast<std::size_t>(chunk) * 960);
        const auto out = stage.process(30.0, in);
        REQUIRE(out.size() == in.size());
        for (const float s : out) {
            REQUIRE(std::abs(s) <= static_cast<float>(ui::SoftLimiter::kThreshold) + 1e-6F);
        }
    }
    // And it is still loud: the limiter turned the burst down, not off.
    const auto last = stage.process(30.0, tone(960, 0.5, 9600));
    CHECK(peak_of(last) > 0.5F);
}

TEST_CASE("a change in gain ramps over about 20 ms", "[webgate][voicegain]")
{
    VoiceGainStage stage;
    // A constant input shows the gain itself, sample by sample.
    const std::vector<float> dc(4800, 0.01F);
    {
        const auto settled = stage.process(0.0, dc);
        CHECK(settled[0] == Catch::Approx(0.01F));
    }
    const auto out = stage.process(12.0, dc);
    const float target = 0.01F * 3.98107F;
    REQUIRE(out.size() == dc.size());

    // Starts from where the gain was, not at the target: a step would click.
    CHECK(out[0] < 0.0101F);
    // Rises monotonically in even steps, none larger than the ramp's share.
    const float step = (target - 0.01F) / static_cast<float>(VoiceGainStage::kRampSamples);
    for (std::size_t i = 1; i < VoiceGainStage::kRampSamples; ++i) {
        CHECK(out[i] >= out[i - 1]);
        CHECK(out[i] - out[i - 1] <= step * 1.01F);
    }
    // Arrives at the target when the ramp is done and stays there.
    CHECK(out[VoiceGainStage::kRampSamples] == Catch::Approx(target).epsilon(1e-4));
    CHECK(out.back() == Catch::Approx(target).epsilon(1e-4));
    CHECK(VoiceGainStage::kRampSamples == 960);
}

TEST_CASE("a ramp split across chunks matches one unbroken chunk", "[webgate][voicegain]")
{
    // The ramp is counted in samples, so chunk boundaries must not restart it.
    const std::vector<float> dc(1920, 0.02F);
    VoiceGainStage whole;
    static_cast<void>(whole.process(0.0, std::span<const float>(dc).first(10)));
    const auto unbroken = whole.process(18.0, dc);
    const std::vector<float> a(unbroken.begin(), unbroken.end());

    VoiceGainStage split;
    static_cast<void>(split.process(0.0, std::span<const float>(dc).first(10)));
    std::vector<float> b;
    for (std::size_t at = 0; at < dc.size(); at += 160) {
        const auto piece = split.process(18.0, std::span<const float>(dc).subspan(at, 160));
        b.insert(b.end(), piece.begin(), piece.end());
    }
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        REQUIRE(a[i] == b[i]);
    }
    CHECK(b[0] < 0.0203F);
    CHECK(b.back() == Catch::Approx(0.02F * 7.94328F).epsilon(1e-3));
}
