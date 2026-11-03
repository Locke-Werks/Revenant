// tools/webgate/phone_mix.h: the one stream a phone hears, summed from the
// receiver it listens to and every digital voice receiver. Each case names the
// wrong implementation it rejects.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "tools/webgate/phone_mix.h"

namespace {

using revenant::webgate::PhoneMixer;
namespace ui = revenant::ui;
constexpr std::size_t kB = PhoneMixer::kBlock;

[[nodiscard]] std::vector<float> tone(std::size_t n, double amplitude, std::size_t start = 0)
{
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<float>(amplitude *
                                  std::sin(2.0 * 3.14159265358979323846 * 1000.0 *
                                           static_cast<double>(start + i) / 48000.0));
    }
    return v;
}

[[nodiscard]] std::vector<float> taken(PhoneMixer& mix)
{
    const auto out = mix.take();
    return std::vector<float>(out.begin(), out.end());
}

[[nodiscard]] float peak_of(const std::vector<float>& v)
{
    float p = 0.0F;
    for (const float s : v) {
        p = std::max(p, std::abs(s));
    }
    return p;
}

}  // namespace

TEST_CASE("two voice streams and an analogue lead sum with the boost", "[webgate][mix]")
{
    PhoneMixer mix;
    mix.set_lead(1);
    const auto lead = tone(kB * 4, 0.10);
    const auto dv_a = tone(kB * 4, 0.05);
    const auto dv_b = tone(kB * 4, 0.02, 7);
    // The lead last, the order the engine delivers them in is not promised.
    mix.push(2, true, 12.0, dv_a);
    mix.push(3, true, 12.0, dv_b);
    mix.push(1, false, 12.0, lead);
    const auto out = taken(mix);
    REQUIRE(out.size() == kB * 4);
    const float g = 3.98107F;
    for (std::size_t i = 0; i < out.size(); ++i) {
        // The analogue lead is not boosted: that is the engine's job.
        const float expected = lead[i] + g * dv_a[i] + g * dv_b[i];
        REQUIRE(out[i] == Catch::Approx(expected).margin(2e-6));
    }
}

TEST_CASE("the limiter holds the sum under full scale", "[webgate][mix]")
{
    PhoneMixer mix;
    mix.set_lead(1);
    std::vector<float> all;
    for (int block = 0; block < 10; ++block) {
        const std::size_t at = static_cast<std::size_t>(block) * kB;
        mix.push(1, false, 30.0, tone(kB, 0.8, at));
        // Two voice streams at +30 dB are x31.6 each on 0.5: far over.
        mix.push(2, true, 30.0, tone(kB, 0.5, at));
        mix.push(3, true, 30.0, tone(kB, 0.5, at + 3));
        const auto out = taken(mix);
        all.insert(all.end(), out.begin(), out.end());
    }
    REQUIRE(all.size() == 10 * kB);
    CHECK(peak_of(all) <= static_cast<float>(ui::SoftLimiter::kThreshold) + 1e-6F);
    // Turned down, not off.
    CHECK(peak_of(all) > 0.5F);
}

TEST_CASE("the lead alone passes through the mix untouched", "[webgate][mix]")
{
    PhoneMixer mix;
    mix.set_lead(1);
    const auto in = tone(kB * 3, 0.25);
    // Odd chunk sizes, the way an engine delivers them.
    std::vector<float> out;
    for (std::size_t at = 0; at < in.size(); at += 700) {
        const std::size_t n = std::min<std::size_t>(700, in.size() - at);
        mix.push(1, false, 30.0, std::span<const float>(in).subspan(at, n));
        const auto part = taken(mix);
        out.insert(out.end(), part.begin(), part.end());
    }
    // 2880 samples is three whole blocks; the gain setting is for voice only.
    REQUIRE(out.size() == kB * 3);
    for (std::size_t i = 0; i < out.size(); ++i) {
        REQUIRE(out[i] == in[i]);
    }
}

TEST_CASE("nothing is cut until the lead holds a whole block", "[webgate][mix]")
{
    PhoneMixer mix;
    mix.set_lead(1);
    mix.push(1, false, 0.0, tone(kB - 1, 0.1));
    CHECK(mix.take().empty());
    mix.push(1, false, 0.0, tone(1, 0.1));
    CHECK(mix.take().size() == kB);
}

TEST_CASE("a stalled lead does not stall the voice", "[webgate][mix]")
{
    PhoneMixer mix;
    mix.set_lead(1);
    // The lead has a stream, which then says nothing.
    mix.push(1, false, 0.0, tone(100, 0.1));
    std::size_t heard = 0;
    for (int block = 0; block < 12; ++block) {
        mix.push(2, true, 0.0, tone(kB, 0.2, static_cast<std::size_t>(block) * kB));
        heard += mix.take().size();
    }
    // Held back by kStallBlocks, then flowing: nearly all twelve blocks.
    CHECK(heard >= 9 * kB);
    CHECK(heard <= 12 * kB);
}

TEST_CASE("a stalled voice stream does not stall the lead", "[webgate][mix]")
{
    PhoneMixer mix;
    mix.set_lead(1);
    // The voice stream speaks once, then stops with part of a block over.
    mix.push(2, true, 0.0, tone(kB + 300, 0.2));
    std::size_t heard = 0;
    for (int block = 0; block < 10; ++block) {
        mix.push(1, false, 0.0, tone(kB, 0.1, static_cast<std::size_t>(block) * kB));
        const auto out = taken(mix);
        heard += out.size();
        for (std::size_t i = 0; i < out.size(); ++i) {
            REQUIRE(std::isfinite(out[i]));
        }
    }
    CHECK(heard == 10 * kB);
    // The leftover piece of a block was neither played nor lost: it waits.
    CHECK(mix.padded_blocks() > 0);
}

TEST_CASE("a stream that arrives in a late burst has the oldest dropped", "[webgate][mix]")
{
    PhoneMixer mix;
    mix.set_lead(1);
    mix.push(2, true, 0.0, tone(kB * 30, 0.2));
    mix.push(1, false, 0.0, tone(kB * 10, 0.1));
    const auto out = taken(mix);
    // Only the newest kMaxQueued of the burst is kept, so the voice is never
    // played seconds behind the lead, and the mix is not run ahead of real
    // time to get through it: the lead's ten blocks are all that is cut.
    CHECK(out.size() == kB * 10);
    CHECK(mix.dropped() == kB * 24);
}

TEST_CASE("with no lead the first stream with a block paces the mix", "[webgate][mix]")
{
    PhoneMixer mix;
    mix.push(2, true, 0.0, tone(kB, 0.2));
    CHECK(mix.take().size() == kB);
    // A stream that is removed no longer holds anything back.
    mix.set_lead(5);
    mix.push(5, false, 0.0, tone(10, 0.1));
    mix.remove(5);
    mix.set_lead(std::nullopt);
    mix.push(2, true, 0.0, tone(kB, 0.2));
    CHECK(mix.take().size() == kB);
}

TEST_CASE("a change in the voice boost ramps inside the mix", "[webgate][mix]")
{
    PhoneMixer mix;
    mix.set_lead(1);
    const std::vector<float> dc(kB * 3, 0.01F);
    mix.push(1, false, 0.0, std::vector<float>(kB, 0.0F));
    mix.push(2, true, 0.0, std::span<const float>(dc).first(kB));
    static_cast<void>(taken(mix));
    mix.push(1, false, 0.0, std::vector<float>(kB * 2, 0.0F));
    mix.push(2, true, 12.0, std::span<const float>(dc).last(kB * 2));
    const auto out = taken(mix);
    REQUIRE(out.size() == kB * 2);
    // No step: the first sample is within one ramp step of the old level.
    CHECK(out[0] < 0.0101F);
    CHECK(out[1500] == Catch::Approx(0.01F * 3.98107F).epsilon(1e-4));
}
