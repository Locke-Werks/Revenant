// tools/webgate/phone_mix.h: the one stream a phone hears, summed from the
// receiver it listens to and every digital voice receiver. Each case names the
// wrong implementation it rejects. The clock is the test's.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "tools/webgate/phone_mix.h"

namespace {

using revenant::webgate::PhoneMixer;
namespace ui = revenant::ui;
using Clock = PhoneMixer::Clock;
using std::chrono::milliseconds;
constexpr std::size_t kB = PhoneMixer::kBlock;
constexpr std::size_t kPrime = PhoneMixer::kPrimeBlocks;

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

[[nodiscard]] std::vector<float> dc(std::size_t n, float level)
{
    return std::vector<float>(n, level);
}

[[nodiscard]] std::vector<float> taken(PhoneMixer& mix, Clock::time_point now)
{
    const auto out = mix.take(now);
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
    const auto t = Clock::now();
    PhoneMixer mix;
    mix.set_lead(1);
    const std::size_t n = (kPrime + 2) * kB;
    const auto lead = tone(n, 0.10);
    const auto dv_a = tone(n, 0.05);
    const auto dv_b = tone(n, 0.02, 7);
    mix.push(2, true, 12.0, dv_a, t);
    mix.push(3, true, 12.0, dv_b, t);
    mix.push(1, false, 12.0, lead, t);
    const auto out = taken(mix, t);
    REQUIRE(out.size() == n);
    const float g = 3.98107F;
    for (std::size_t i = 0; i < out.size(); ++i) {
        // The analogue lead is not boosted: that is the engine's job.
        const float expected = lead[i] + g * dv_a[i] + g * dv_b[i];
        REQUIRE(out[i] == Catch::Approx(expected).margin(2e-6));
    }
}

TEST_CASE("the limiter holds the sum under full scale", "[webgate][mix]")
{
    const auto t = Clock::now();
    PhoneMixer mix;
    mix.set_lead(1);
    const std::size_t n = (kPrime + 2) * kB;
    // Two voice streams at +30 dB are x31.6 each on 0.5: far over.
    mix.push(2, true, 30.0, tone(n, 0.5), t);
    mix.push(3, true, 30.0, tone(n, 0.5, 3), t);
    mix.push(1, false, 30.0, tone(n, 0.8), t);
    const auto out = taken(mix, t);
    REQUIRE(out.size() == n);
    CHECK(peak_of(out) <= static_cast<float>(ui::SoftLimiter::kThreshold) + 1e-6F);
    // Turned down, not off.
    CHECK(peak_of(out) > 0.5F);
}

TEST_CASE("the lead alone passes through the mix untouched", "[webgate][mix]")
{
    const auto t = Clock::now();
    PhoneMixer mix;
    mix.set_lead(1);
    const auto in = tone(kB * 3, 0.25);
    // Odd chunk sizes, the way an engine delivers them.
    std::vector<float> out;
    for (std::size_t at = 0; at < in.size(); at += 700) {
        const std::size_t n = std::min<std::size_t>(700, in.size() - at);
        mix.push(1, false, 30.0, std::span<const float>(in).subspan(at, n), t);
        const auto part = taken(mix, t);
        out.insert(out.end(), part.begin(), part.end());
    }
    // 2880 samples is three whole blocks; the gain setting is for voice only.
    REQUIRE(out.size() == kB * 3);
    for (std::size_t i = 0; i < out.size(); ++i) {
        REQUIRE(out[i] == in[i]);
    }
}

TEST_CASE("a lead one sample short of a block is not skipped", "[webgate][mix]")
{
    const auto t = Clock::now();
    PhoneMixer mix;
    mix.set_lead(1);
    // The voice stream is primed and full, which would tempt a mix that cuts
    // on whoever has a block.
    mix.push(2, true, 0.0, dc(kPrime * 2 * kB, 0.05F), t);
    mix.push(1, false, 0.0, dc(kB - 1, 0.1F), t);
    CHECK(mix.take(t).empty());
    mix.push(1, false, 0.0, dc(1, 0.1F), t);
    const auto out = taken(mix, t);
    REQUIRE(out.size() == kB);
    // The block has the lead whole and the voice with it.
    for (const float s : out) {
        REQUIRE(s == Catch::Approx(0.15F).margin(1e-6));
    }
}

TEST_CASE("an LDU-sized burst of voice loses nothing and puts no hole in the lead",
          "[webgate][mix]")
{
    auto t = Clock::now();
    PhoneMixer mix;
    mix.set_lead(1);
    std::vector<float> all;
    std::size_t lead_pushed = 0;
    for (int step = 0; step < 120; ++step) {
        // The lead is steady, one block every 20 ms. The voice arrives as 180 ms
        // of audio at once, every 180 ms: a decode lane that caught up.
        mix.push(1, false, 0.0, dc(kB, 0.10F), t);
        lead_pushed += kB;
        if (step % 9 == 0) {
            mix.push(2, true, 0.0, dc(9 * kB, 0.05F), t);
        }
        const auto out = taken(mix, t);
        all.insert(all.end(), out.begin(), out.end());
        t += milliseconds(20);
    }
    // Contiguous: one block out for every block of the lead, never fewer.
    CHECK(all.size() == lead_pushed);
    // No hole: every sample carries the lead, and the voice rides on it.
    CHECK(*std::ranges::min_element(all) >= 0.10F - 1e-6F);
    // Nothing trimmed and nothing run dry once it was primed.
    CHECK(mix.dropped() == 0);
    CHECK(mix.padded_blocks() == 0);
    // And the voice is in it.
    CHECK(std::ranges::count_if(all, [](float s) { return s > 0.14F; }) > 60 * kB);
}

TEST_CASE("voice that arrives in uneven chunks leaves no hole in itself", "[webgate][mix]")
{
    auto t = Clock::now();
    PhoneMixer mix;
    mix.set_lead(1);
    std::vector<float> all;
    // A voice chunk of 27 ms at 48 kHz is 1296 samples: not a block.
    for (int step = 0; step < 150; ++step) {
        mix.push(1, false, 0.0, dc(kB, 0.10F), t);
        if (step % 4 == 0) {
            // Four blocks and a little over, as three 27 ms chunks are.
            mix.push(2, true, 0.0, dc(1296 * 3 - 48, 0.05F), t);
        }
        const auto out = taken(mix, t);
        all.insert(all.end(), out.begin(), out.end());
        t += milliseconds(20);
    }
    // Once the voice has primed it is in every block: no sample of the sum is
    // the lead alone.
    const std::size_t from = 40 * kB;
    REQUIRE(all.size() > from);
    for (std::size_t i = from; i < all.size(); ++i) {
        REQUIRE(all[i] == Catch::Approx(0.15F).margin(1e-6));
    }
    CHECK(mix.padded_blocks() == 0);
}

TEST_CASE("a stalled lead does not stall the voice, and the mix keeps real time",
          "[webgate][mix]")
{
    auto t = Clock::now();
    PhoneMixer mix;
    mix.set_lead(1);
    // The lead has a stream, which then says nothing.
    mix.push(1, false, 0.0, dc(100, 0.1F), t);
    std::size_t heard = 0;
    const auto start = t;
    for (int step = 0; step < 50; ++step) {
        // The voice is steady, one block every 20 ms.
        mix.push(2, true, 0.0, dc(kB, 0.2F), t);
        const auto out = taken(mix, t);
        heard += out.size() / kB;
        // Never ahead of the clock: blocks heard by now cannot exceed the
        // blocks of time that have passed, and one more for the first.
        const auto elapsed = std::chrono::duration_cast<milliseconds>(t - start).count();
        REQUIRE(heard <= static_cast<std::size_t>(elapsed / 20 + 1));
        t += milliseconds(20);
    }
    // The voice primed and the lead's 200 ms passed: after that it is heard.
    CHECK(heard >= 25);
}

TEST_CASE("a burst on a stalled mix is not played ahead of real time", "[webgate][mix]")
{
    const auto t = Clock::now();
    PhoneMixer mix;
    // No lead at all: the clock paces.
    mix.push(2, true, 0.0, dc(30 * kB, 0.2F), t);
    CHECK(mix.take(t).size() <= 2 * kB);
    CHECK(taken(mix, t + milliseconds(100)).size() <= 6 * kB);
    // Past the window the oldest went rather than be played late.
    CHECK(mix.dropped() == 10 * kB);
}

TEST_CASE("a stalled voice stream does not stall the lead", "[webgate][mix]")
{
    auto t = Clock::now();
    PhoneMixer mix;
    mix.set_lead(1);
    // The voice stream speaks for a while, then stops.
    mix.push(2, true, 0.0, dc(kPrime * kB + 300, 0.05F), t);
    std::size_t heard = 0;
    std::vector<float> all;
    for (int step = 0; step < 40; ++step) {
        mix.push(1, false, 0.0, dc(kB, 0.10F), t);
        const auto out = taken(mix, t);
        all.insert(all.end(), out.begin(), out.end());
        heard += out.size();
        t += milliseconds(20);
    }
    CHECK(heard == 40 * kB);
    // No hole, whatever the voice did.
    CHECK(*std::ranges::min_element(all) >= 0.10F - 1e-6F);
    // It ran dry and was counted.
    CHECK(mix.padded_blocks() > 0);
}

TEST_CASE("the end of a call is not played at the start of the next", "[webgate][mix]")
{
    auto t = Clock::now();
    PhoneMixer mix;
    mix.push(2, true, 0.0, dc(kPrime * kB + 500, 0.2F), t);
    t += milliseconds(300);
    // A new call: its few blocks do not add to the old tail to reach the prime.
    mix.push(2, true, 0.0, dc(5 * kB, 0.2F), t);
    CHECK(mix.take(t).empty());
}

TEST_CASE("a removed stream holds nothing back", "[webgate][mix]")
{
    const auto t = Clock::now();
    PhoneMixer mix;
    mix.set_lead(5);
    mix.push(5, false, 0.0, dc(10, 0.1F), t);
    mix.remove(5);
    mix.set_lead(std::nullopt);
    mix.push(2, true, 0.0, dc(kPrime * kB, 0.2F), t);
    CHECK(mix.take(t).size() >= kB);
}
