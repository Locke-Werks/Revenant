// The digital voice modes, through the engine, into their decoders.
//
// WHAT THIS FILE IS THE RECORD OF
//
// Until 2026-09-22 a P25p1, Dstar or Tetra receiver was the graph's raw tap:
// one coarse channel copied out of the channel ring, at the channel rate,
// with the carrier wherever the grid's residual left it and nothing filtered
// out. core/decode's three decoders assume the opposite of all three things,
// a carrier at DC, a channel filter and the rate their own filters were
// designed at, and had only ever been fed by their own transmitters at that
// rate in tests/decode. Nothing measured what they made of the tap.
//
// These modes now get the fine stage: the residual mixed to DC, the mode's
// channel filtered, and the result resampled to 48000 S/s for P25 and D-STAR
// and 72000 for TETRA, which is dsp::complex_tap_rate_step and is each
// decoder's own default. This file checks that the engine builds that.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/dsp/pfb.h"
#include "core/engine/engine.h"
#include "core/engine/vrx.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;

namespace {

// ---------------------------------------------------------------------------
// The engine these cases run
// ---------------------------------------------------------------------------

// The canonical test engine, for the cases that only need a receiver built.
constexpr dsp::SampleRate kSceneRate = 2'400'000;

engine::EngineConfig scene_config() {
    engine::EngineConfig config;
    config.channels = 64;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = 32'768;
    config.gpu_index = -1;  // honours REVENANT_GPU_INDEX
    return config;
}

// One NFM emitter in a synthetic scene. What it carries is irrelevant to the
// structural case; it exists so the run has samples to move.
std::string scene_uri(dsp::Hertz offset_hz, dsp::SampleIndex samples) {
    return "synthetic:wideband?rate=" + std::to_string(kSceneRate) +
           "&emitters=1&modes=nfm&seed=616161&noise_dbfs=-120&snr_min=40&snr_max=40" +
           "&samples=" + std::to_string(samples) +
           "&span_low=" + std::to_string(offset_hz - 8000) +
           "&span_high=" + std::to_string(offset_hz + 8000);
}

struct ModeRate {
    engine::Demod mode;
    dsp::SampleRate rate;
};

constexpr ModeRate kDigitalModes[] = {
    {engine::Demod::P25p1, 48'000},
    {engine::Demod::Dstar, 48'000},
    {engine::Demod::Tetra, 72'000},
};

}  // namespace

// ---------------------------------------------------------------------------
// The receiver the engine builds
// ---------------------------------------------------------------------------

TEST_CASE("a digital voice receiver is a fine stage at its decoder's rate",
          "[gpu][engine][dv]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    auto created = engine::Engine::create(scene_config());
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;

    // Five channel spacings and a bit, so every receiver has a residual for
    // its mixer to take out.
    constexpr dsp::Hertz kCentre = 37'500 * 5 + 4'000;
    REQUIRE(eng.open_source(scene_uri(kCentre, 400'000)).has_value());

    struct Seen {
        std::mutex lock;
        std::uint64_t frames = 0;
        dsp::SampleRate rate = 0;
        std::uint32_t channels = 0;
    };
    std::vector<std::unique_ptr<Seen>> seen;
    std::vector<engine::VrxId> ids;

    for (const ModeRate& want : kDigitalModes) {
        INFO("mode " << engine::demod_name(want.mode));
        engine::VrxParams params;
        params.center = kCentre;
        params.demod = want.mode;
        params.bandwidth = 0;  // the mode's own channel

        const auto added = eng.add_vrx(params);
        INFO(test::message_of(added));
        REQUIRE(added.has_value());
        ids.push_back(*added);

        // The status names the rate the stream will arrive at. Before the
        // planner accepted these modes this was zero: vrx_shape_for refused
        // them, so the graph never learned a shape for the receiver at all.
        const auto status = eng.vrx_status(*added);
        REQUIRE(status.has_value());
        CHECK(status->demod_rate == want.rate);

        // Moving the dial is a push constant and a new tap table, the same
        // as for any receiver. It was refused on every one of these modes,
        // because Graph::set_vrx_params asks the planner whether the retune
        // changes the pipeline's shape and the planner refused the mode.
        engine::VrxParams nudged = params;
        nudged.center = kCentre + 250;
        const auto retuned = eng.set_vrx_params(*added, nudged);
        INFO(test::message_of(retuned));
        CHECK(retuned.has_value());

        auto record = std::make_unique<Seen>();
        Seen* target = record.get();
        seen.push_back(std::move(record));
        REQUIRE(eng.set_audio_sink(*added, [target](const engine::AudioChunk& chunk) -> Status {
                       const std::lock_guard<std::mutex> guard(target->lock);
                       target->rate = chunk.rate;
                       target->channels = chunk.channels;
                       target->frames += chunk.samples.size() / 2U;
                       return {};
                   }).has_value());
    }

    const auto ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    for (std::size_t i = 0; i < ids.size(); ++i) {
        const ModeRate& want = kDigitalModes[i];
        INFO("mode " << engine::demod_name(want.mode));
        const std::lock_guard<std::mutex> guard(seen[i]->lock);

        // Complex pairs at the decoder's rate, and not the channel rate the
        // raw tap would have delivered, which on this grid is 75000.
        CHECK(seen[i]->channels == 2U);
        CHECK(seen[i]->rate == want.rate);

        // 400000 samples at 2.4 MS/s is a sixth of a second, so a sixth of
        // the rate in frames, less the filters' support at the front.
        const double expected = static_cast<double>(want.rate) * 400'000.0 /
                                static_cast<double>(kSceneRate);
        INFO(seen[i]->frames << " frames, " << expected << " expected at most");
        CHECK(static_cast<double>(seen[i]->frames) > 0.8 * expected);
        CHECK(static_cast<double>(seen[i]->frames) <= expected + 1.0);
    }
}

TEST_CASE("a digital voice receiver cut below its own channel is refused", "[engine][dv]") {
    // The three modes joined engine::clamp_breaks_demodulator when they got a
    // fine stage, because a passband that is applied can now take part of
    // the signal away, and a decoder handed a truncated channel reads it as
    // symbol errors at full strength. 256 channels on 2.4 MS/s guarantee
    // 9375 Hz to a receiver placed anywhere, which carries D-STAR's 6 kHz and
    // neither P25's 12.5 nor TETRA's 25.
    constexpr dsp::GridParams kFine{
        .channels = 256,
        .taps_per_branch = 17,
        .decimation = 128,
    };

    struct Case {
        engine::Demod mode;
        bool fits;
    };
    const Case cases[] = {
        {engine::Demod::P25p1, false},
        {engine::Demod::Dstar, true},
        {engine::Demod::Tetra, false},
        // The raw tap is still not a demodulator and is narrowed rather
        // than refused.
        {engine::Demod::Raw, true},
    };

    for (const Case& want : cases) {
        INFO("mode " << engine::demod_name(want.mode));
        engine::VrxParams params;
        // Half a channel spacing off a centre, where one channel carries the
        // least.
        params.center = 9'375 * 7 + 4'687;
        params.demod = want.mode;
        params.bandwidth = 0;
        if (want.mode == engine::Demod::Raw) {
            params.bandwidth = 25'000;
        }

        const auto placed = engine::place(kFine, kSceneRate, params);
        INFO(test::message_of(placed));
        CHECK(placed.has_value() == want.fits);
        if (!want.fits && !placed.has_value()) {
            // Named as a decoder problem rather than as audio, since none of
            // these three ends in audio.
            CHECK(placed.error().message.find("symbols smeared") != std::string::npos);
            CHECK(placed.error().message.find("--channels") != std::string::npos);
        }
    }
}
