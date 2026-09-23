// DriftTrim, on its own against a model of the ring and inside AudioMix
// against a simulated engine whose clock is off the card's.
//
// Each case names the wrong implementation it rejects. The one the loop
// replaces read every stream at exactly its nominal step, so a producer
// 100 ppm fast filled the lead's ring 6 ms a minute towards eviction, and one
// 100 ppm slow drained it until the lead starved.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

#include "audio/audio_mix.h"
#include "audio/audio_ring.h"
#include "audio/drift_trim.h"

using revenant::rpc::AudioChunk;
using revenant::ui::AudioMix;
using revenant::ui::AudioRing;
using revenant::ui::DriftTrim;
using revenant::ui::MixControl;
using revenant::ui::MixPull;
using revenant::ui::MixSlot;

namespace {

// A card at 48000 S/s pulling 10 ms at a time.
constexpr std::uint32_t kCard = 48'000;
constexpr std::size_t kPull = 480;

// The shipped shape: a 48000 S/s receiver handed out a chunk every 27 ms,
// which audio/audio_ring.h gives for the shipped 16384-sample source block,
// and the 200 ms depth this client asks the engine for.
constexpr std::uint32_t kLeadRate = 48'000;
constexpr std::size_t kLeadChunk = 1'296;
constexpr std::uint32_t kVoiceRate = 8'000;
constexpr std::size_t kVoiceChunk = 216;
constexpr std::uint32_t kDepthMs = 200;

constexpr double kMinutes = 10.0;

// The same deterministic jitter every run: up to 4 ms of network and
// scheduler delay on each chunk's arrival, never reordering them.
struct Jitter {
    std::uint32_t state = 12'345;
    double next_s()
    {
        state = state * 1'664'525U + 1'013'904'223U;
        return 0.004 * static_cast<double>(state >> 8) / static_cast<double>(1U << 24);
    }
};

[[nodiscard]] AudioChunk chunk_of(std::uint64_t index, std::size_t frames, std::uint32_t rate,
                                  double hz, double amplitude)
{
    AudioChunk chunk;
    chunk.sample_rate = rate;
    chunk.channel_count = 1;
    chunk.sample_index = index;
    chunk.squelch_open = true;
    chunk.samples.resize(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        chunk.samples[n] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(index + n) /
                                 static_cast<double>(rate)));
    }
    return chunk;
}

struct Run {
    // What the loop did, after it first locked.
    double worst_error_s = 0.0;
    double least_fill_s = 1.0;
    double most_fill_s = 0.0;
    double final_trim = 0.0;
    double trim_wander = 0.0;
    double largest_trim = 0.0;
    double largest_trim_step = 0.0;
    double first_locked_s = -1.0;
    double target_s = 0.0;

    // What the card heard.
    double largest_output_step = 0.0;
    std::uint64_t starved_after_lock = 0;
    std::uint64_t starved_at_start = 0;

    // What the rings counted.
    std::uint64_t overrun = 0;
    std::uint64_t skipped = 0;
    std::uint64_t alignments = 0;
    int rebases_after_lock = 0;
};

// Ten minutes of an engine `drift` fast of the card, one receiver at the
// card's rate and, with `voice`, a P25 receiver's 8000 S/s beside it on the
// same engine blocks. Card time is the reference, and the steady clock the
// arrivals and, with `timed`, the pulls are read on is taken to be the card's.
// A steady clock off the card's moves the anchor and the pull times alike, so
// the lag still moves at the engine's rate against the card's; this does not
// model that.
[[nodiscard]] Run simulate(double drift, bool correct, bool voice, bool timed = true)
{
    std::array<AudioRing, 2> rings;
    for (AudioRing& ring : rings) {
        ring.set_depth_millis(kDepthMs);
    }
    std::array<AudioRing*, 2> pointers{&rings[0], &rings[1]};
    std::array<MixSlot, 2> slots{};
    slots[0].heard = true;
    slots[1].heard = voice;
    const MixControl control{0, slots};

    AudioMix mix(kCard, 1);
    mix.set_drift_correction(correct);

    Jitter jitter;
    const double chunk_s = static_cast<double>(kLeadChunk) / static_cast<double>(kLeadRate);
    const auto chunk_arrival = [&](std::uint64_t j) {
        return static_cast<double>(j + 1) * chunk_s / (1.0 + drift);
    };

    std::uint64_t next_chunk = 0;
    double next_arrival = chunk_arrival(0) + jitter.next_s();

    // The sink opens a little after the first chunk, as AudioPlayer's 50 ms
    // timer has it.
    double card_s = chunk_arrival(0) + 0.030;
    const double end_s = kMinutes * 60.0;

    Run run;
    std::vector<float> out(kPull);
    float previous = 0.0F;
    bool have_previous = false;
    double previous_trim = 0.0;
    bool was_locked = false;

    while (card_s < end_s) {
        while (next_arrival <= card_s) {
            const auto arrival_ns = static_cast<std::int64_t>(5e9 + next_arrival * 1e9);
            rings[0].write(chunk_of(next_chunk * kLeadChunk, kLeadChunk, kLeadRate, 1'000.0, 0.5),
                           arrival_ns);
            if (voice) {
                rings[1].write(chunk_of(40'000 + next_chunk * kVoiceChunk, kVoiceChunk, kVoiceRate,
                                        300.0, 0.2),
                               arrival_ns);
            }
            ++next_chunk;
            next_arrival = std::max(next_arrival, chunk_arrival(next_chunk) + jitter.next_s());
        }

        const bool locked_before = mix.drift().locked();
        const auto now_ns = static_cast<std::int64_t>(5e9 + card_s * 1e9);
        const MixPull pulled =
            mix.pull(pointers, control, out.data(), kPull, timed ? now_ns : AudioRing::kNoArrival);
        card_s += static_cast<double>(kPull) / static_cast<double>(kCard);

        for (const float sample : out) {
            if (have_previous) {
                run.largest_output_step = std::max(
                    run.largest_output_step, std::abs(static_cast<double>(sample - previous)));
            }
            previous = sample;
            have_previous = true;
        }

        const DriftTrim& loop = mix.drift();
        run.largest_trim = std::max(run.largest_trim, std::abs(loop.trim()));
        if (card_s > 180.0) {
            run.trim_wander = std::max(run.trim_wander, std::abs(loop.trim() - drift));
        }
        run.largest_trim_step =
            std::max(run.largest_trim_step, std::abs(loop.trim() - previous_trim));
        previous_trim = loop.trim();

        if (was_locked && locked_before && !loop.locked()) {
            ++run.rebases_after_lock;
        }
        if (loop.locked() && !was_locked && run.first_locked_s < 0.0) {
            run.first_locked_s = card_s;
            run.target_s = loop.target();
        }
        was_locked = was_locked || loop.locked();

        const std::size_t starved = kPull - pulled.frames_played;
        if (run.first_locked_s < 0.0) {
            run.starved_at_start += starved;
        } else {
            run.starved_after_lock += starved;
        }

        if (run.first_locked_s >= 0.0) {
            if (loop.locked()) {
                run.worst_error_s =
                    std::max(run.worst_error_s, std::abs(loop.smoothed() - loop.target()));
            }
            run.least_fill_s = std::min(run.least_fill_s, pulled.fill_seconds);
            run.most_fill_s = std::max(run.most_fill_s, pulled.fill_seconds);
        }
    }

    run.final_trim = mix.drift().trim();
    run.overrun = rings[0].counts().frames_overrun + rings[1].counts().frames_overrun;
    run.skipped = rings[0].counts().frames_skipped + rings[1].counts().frames_skipped;
    run.alignments = mix.alignments();
    return run;
}

[[nodiscard]] std::string describe(const char* name, const Run& run)
{
    std::ostringstream text;
    text << name << ": locked at " << run.first_locked_s << " s on " << run.target_s * 1e3
         << " ms; averaged level worst " << run.worst_error_s * 1e3 << " ms off; fill "
         << run.least_fill_s * 1e3 << " to " << run.most_fill_s * 1e3 << " ms; trim "
         << run.final_trim * 1e6 << " ppm at the end, within " << run.trim_wander * 1e6
         << " of the drift after three minutes, " << run.largest_trim * 1e6 << " at most, stepping "
         << run.largest_trim_step * 1e6 << " ppm at most; output step " << run.largest_output_step
         << "; starved " << run.starved_at_start << " before lock and " << run.starved_after_lock
         << " after; overrun " << run.overrun << ", skipped " << run.skipped << ", aligned "
         << run.alignments << ", rebased " << run.rebases_after_lock;
    return text.str();
}

// The largest step between two output samples a 1 kHz sine at 0.5 and a
// 300 Hz one at 0.2 can make at 48000 S/s, with 1 percent for the resampler
// and the trim. A starve drops the sum to zero from wherever it was, and a
// kernel starting against frames it does not have is a click; either is
// several times this.
const double kSmoothStep =
    1.01 * 2.0 * std::numbers::pi * (0.5 * 1'000.0 + 0.2 * 300.0) / static_cast<double>(kCard);

// The band the averaged level holds within, either side of the one the loop
// locked on: 2 ms. The loop's own figure for a 100 ppm step is 1.1 ms; this
// is that with the two seconds of settling before the lock, the five-second
// average and the slew limit on top. Measured: 1.3 ms.
constexpr double kBandS = 0.002;

// How far the trim may wander from the drift once the loop has found it,
// three minutes in: 12 ppm, 0.02 cent. Measured: 9 ppm.
constexpr double kWander = 12e-6;

void check_held(const Run& run, double drift)
{
    CHECK(run.first_locked_s > 0.0);
    CHECK(run.first_locked_s < 5.0);
    CHECK(run.worst_error_s < kBandS);
    CHECK(run.rebases_after_lock == 0);

    // No skips of any kind: nothing evicted, nothing played late, no stream
    // moved, and the lead never ran dry once the loop had it.
    CHECK(run.overrun == 0);
    CHECK(run.skipped == 0);
    CHECK(run.alignments == 0);
    CHECK(run.starved_after_lock == 0);
    CHECK(run.least_fill_s > 0.0);

    // The drift is found, and nothing was asked of the clamp or the slew
    // limit beyond their stated bounds.
    CHECK(run.trim_wander < kWander);
    CHECK(std::abs(run.final_trim - drift) < kWander);
    CHECK(run.largest_trim <= DriftTrim::kLimit);
    CHECK(run.largest_trim_step <= DriftTrim::kMaxStep * (1.0 + 1e-9));

    CHECK(run.largest_output_step < kSmoothStep);
}

// A model of what the loop is handed, for the loop on its own: the lead's
// lag behind its arrivals moves at drift - trim per second of card time, and
// each reading carries up to a millisecond of the pull thread's scheduling
// either way.
struct Plant {
    double drift = 0.0;
    double base_s = 0.030;
    double t = 0.0;
    Jitter jitter;

    [[nodiscard]] double lag() { return base_s + 0.5 * (jitter.next_s() - 0.002); }
};

}  // namespace

TEST_CASE("the loop finds the drift and holds the level", "[audio][drift]")
{
    // REJECTS: a trim that follows the level with no integral, which leaves a
    // standing error of drift / Kp, 1.5 ms per 100 ppm, and one that jumps to
    // its answer, which is a pitch step.
    //
    // 100 ppm either way, and the 300 ppm the engine measured against the
    // card on 2026-09-20, which needs the slew limit's 15 seconds to reach.
    // Measured: the level at most 1.3 ms off at 100 ppm and 0.24 ms once two
    // minutes in; 3.8 ms and 0.64 ms at 300 ppm.
    struct Case {
        double drift;
        double worst_s;
        double settled_s;
    };
    for (const Case one : {Case{100e-6, 0.002, 0.0005}, Case{-100e-6, 0.002, 0.0005},
                           Case{300e-6, 0.006, 0.0015}, Case{-300e-6, 0.006, 0.0015}}) {
        DriftTrim loop;
        Plant plant{.drift = one.drift};
        const double dt = 0.010;
        double worst = 0.0;
        double settled = 0.0;
        double largest_step = 0.0;
        double previous = 0.0;
        for (int i = 0; i < 60'000; ++i) {
            loop.observe(plant.lag(), dt);
            plant.base_s += (plant.drift - loop.trim()) * dt;
            plant.t += dt;
            largest_step = std::max(largest_step, std::abs(loop.trim() - previous));
            previous = loop.trim();
            if (loop.locked()) {
                const double error = std::abs(loop.smoothed() - loop.target());
                worst = std::max(worst, error);
                if (plant.t > 120.0) {
                    settled = std::max(settled, error);
                }
            }
        }
        INFO("drift " << one.drift * 1e6 << " ppm: trim " << loop.trim() * 1e6
                      << " ppm after ten minutes, worst error " << worst * 1e3
                      << " ms, after two minutes " << settled * 1e3 << " ms");
        CHECK(std::abs(loop.trim() - one.drift) < 1e-6);
        CHECK(worst < one.worst_s);
        CHECK(settled < one.settled_s);
        CHECK(largest_step <= DriftTrim::kMaxStep * (1.0 + 1e-9));
    }
}

TEST_CASE("the trim stops at its clamp and comes back without winding up", "[audio][drift]")
{
    // REJECTS: an integral that keeps integrating while the trim is held at
    // the clamp. A 700 ppm drift is past what the loop may correct, so the
    // level runs away; when the drift goes, an integral wound up behind the
    // clamp would hold the trim at 500 ppm for minutes and drain the ring.
    DriftTrim loop;
    Plant plant{.drift = 700e-6};
    const double dt = 0.010;
    double largest = 0.0;
    for (int i = 0; i < 30'000; ++i) {
        loop.observe(plant.lag(), dt);
        plant.base_s += (plant.drift - loop.trim()) * dt;
        plant.t += dt;
        largest = std::max(largest, std::abs(loop.trim()));
    }
    CHECK(largest <= DriftTrim::kLimit);
    CHECK(loop.trim() > DriftTrim::kLimit - 1e-12);

    // The drift goes. The level is about 60 ms over by now and the loop
    // brings it back at the clamp; the trim then has to come off it as the
    // level arrives rather than carry on draining past it. With the integral
    // wound up to the clamp the level fell 8.5 ms below where it locked, in a
    // model of this loop run both ways; held back, 1.9 ms.
    plant.drift = 0.0;
    double released_at = -1.0;
    double undershoot = 0.0;
    for (int i = 0; i < 60'000; ++i) {
        loop.observe(plant.lag(), dt);
        plant.base_s += (plant.drift - loop.trim()) * dt;
        plant.t += dt;
        if (released_at < 0.0 && loop.trim() < 0.5 * DriftTrim::kLimit) {
            released_at = plant.t;
        }
        undershoot = std::min(undershoot, loop.smoothed() - loop.target());
    }
    INFO("trim " << loop.trim() * 1e6 << " ppm at the end, level off by "
                 << (loop.smoothed() - loop.target()) * 1e3 << " ms, at most "
                 << undershoot * 1e3 << " ms under on the way back");
    CHECK(released_at > 0.0);
    CHECK(undershoot > -0.003);
    CHECK(std::abs(loop.trim()) < 2e-6);
    CHECK(std::abs(loop.smoothed() - loop.target()) < 0.0005);
}

TEST_CASE("a rebase keeps the drift and measures the level again", "[audio][drift]")
{
    // REJECTS: forgetting the drift with the level, which would drop the
    // trim to zero, a 100 ppm step, every time the operator focused another
    // receiver.
    DriftTrim loop;
    Plant plant{.drift = 100e-6};
    const double dt = 0.010;
    for (int i = 0; i < 60'000; ++i) {
        loop.observe(plant.lag(), dt);
        plant.base_s += (plant.drift - loop.trim()) * dt;
        plant.t += dt;
    }
    const double before = loop.trim();
    loop.rebase();
    CHECK_FALSE(loop.locked());
    plant.base_s += 0.040;
    double furthest = 0.0;
    for (int i = 0; i < 250; ++i) {
        loop.observe(plant.lag(), dt);
        plant.base_s += (plant.drift - loop.trim()) * dt;
        plant.t += dt;
        furthest = std::max(furthest, std::abs(loop.trim() - before));
    }
    INFO("the trim moved " << furthest * 1e6 << " ppm across the rebase");
    CHECK(furthest < 3e-6);
    REQUIRE(loop.locked());
    // The 40 ms raise is the new level, rather than something to drain.
    CHECK(loop.target() > 0.065);
}

TEST_CASE("an engine 100 ppm fast is held for ten minutes with no skips", "[audio][drift][mix]")
{
    // REJECTS: reading every stream at its nominal step, below, and a trim
    // applied to the lead alone, which would walk the P25 receiver off the
    // lead's instant at the trim's 100 ppm and move it back, a click, every
    // 50 seconds.
    const Run run = simulate(100e-6, true, true);
    INFO(describe("100 ppm fast", run));
    check_held(run, 100e-6);
}

TEST_CASE("an engine 100 ppm slow is held for ten minutes with no skips", "[audio][drift][mix]")
{
    const Run run = simulate(-100e-6, true, true);
    INFO(describe("100 ppm slow", run));
    check_held(run, -100e-6);
}

TEST_CASE("without the trim the same engines fill and drain the ring", "[audio][drift][mix]")
{
    // What the loop is holding back, on the same simulation: 100 ppm over
    // ten minutes is 60 ms of audio one way or the other. Measured: the
    // fast engine's level 59.4 ms over where it locked by the end, and the
    // slow one's lead starved 2064 frames once it had drained.
    const Run fast = simulate(100e-6, false, false);
    INFO(describe("100 ppm fast, uncorrected", fast));
    CHECK(fast.worst_error_s > 0.050);

    const Run slow = simulate(-100e-6, false, false);
    INFO(describe("100 ppm slow, uncorrected", slow));
    CHECK(slow.starved_after_lock > 0);
}

TEST_CASE("with no arrival times the loop holds the ring's fill instead", "[audio][drift][mix]")
{
    // REJECTS: a loop that does nothing when the pull is untimed or the lead
    // has no anchor, which is a chunk written without an arrival. It falls
    // back to the fill, whose 27 ms sawtooth makes the trim wander further,
    // and still has to hold the band with no skips.
    const Run run = simulate(-100e-6, true, false, false);
    INFO(describe("100 ppm slow, untimed", run));
    CHECK(run.worst_error_s < kBandS);
    CHECK(run.starved_after_lock == 0);
    CHECK(run.overrun == 0);
    CHECK(run.rebases_after_lock == 0);
    CHECK(std::abs(run.final_trim + 100e-6) < 50e-6);
    CHECK(run.largest_trim_step <= DriftTrim::kMaxStep * (1.0 + 1e-9));
    CHECK(run.largest_output_step < kSmoothStep);
}
