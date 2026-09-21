// Clicking a signal, and getting a receiver that can hear it.
//
// THE DEFECT THIS FILE IS THE RECORD OF
//
// Clicking a 145 kHz broadcast FM station opened an NFM receiver with a
// plus and minus 8 kHz filter. Nothing was broken: rpc::VrxParams::demod
// defaults to Nfm, click-to-tune sent the struct as it found it, and the
// engine built exactly what it was asked for. The operator heard a
// distorted narrow slice of a strong clean station on the first thing
// anybody does with this program.
//
// Fixing the mode alone would have moved the failure rather than removed
// it. A WFM receiver wants 200 kHz of passband and a 64-channel grid over
// a 2.4 MS/s dongle guarantees 37.5 kHz, so the engine used to narrow the
// request and report the narrowing on a status field. The audio arrives
// before anybody reads a status field, and a discriminator fed a truncated
// signal produces the wrong audio at full strength rather than a quiet
// version of the right one. So the two halves are one defect and this file
// covers both:
//
//   engine::demod_for_signal    picks the mode from the measurement
//   engine::place               refuses the placement that cannot work
//   engine::channel_count_for   names the grid that can
//
// Most of what follows needs no device. The last two cases do, because
// "can actually hear it" is a claim about a receiver that is running.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "core/dsp/vrx_reference.h"
#include "core/engine/engine.h"
#include "core/engine/vrx.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;

namespace {

// The dongle the operator was on.
constexpr dsp::SampleRate kSourceRate = 2'400'000;

// What the detector measured on the station that started this. Wide enough
// that no narrowband allocation holds it and narrow enough that it is not
// the 200 kHz the band plan allocates, which is the point: the rule has to
// work on what was measured rather than on the channel it belongs to.
constexpr dsp::Hertz kBroadcastWidthHz = 150'000;

// A scene with one continuously keyed carrier in it, so a receiver placed
// on it produces audio rather than silence. The content is not what these
// cases are about; that a receiver 200 kHz wide runs and delivers is.
std::string scene_uri(dsp::Hertz offset_hz, dsp::SampleIndex samples) {
    return "synthetic:wideband?rate=" + std::to_string(kSourceRate) +
           "&emitters=1&modes=nfm&seed=717171&noise_dbfs=-120&snr_min=50&snr_max=50" +
           "&samples=" + std::to_string(samples) +
           "&span_low=" + std::to_string(offset_hz - 8000) +
           "&span_high=" + std::to_string(offset_hz + 8000);
}

engine::EngineConfig config_with_channels(std::uint32_t channels) {
    engine::EngineConfig config;
    config.channels = channels;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = 32'768;
    config.gpu_index = -1;  // honours REVENANT_GPU_INDEX
    return config;
}

// The request click-to-tune builds, written once so the cases below cannot
// drift from each other or from ui/models/receiver_link.cpp.
//
// The passband is left UNSTATED, which is what the client sends: the
// measured width is not a passband, because on the sideband modes the
// occupied band sits entirely to one side of the carrier. The engine
// answers with the mode's own entry in dsp::default_passband and the
// operator drags from there.
//
// UNSTATED MEANS BOTH EDGES ZERO AND bandwidth ZERO. VrxParams::bandwidth
// defaults to 12000 rather than to nothing, so a caller that zeroes only
// the edges gets a 12 kHz shorthand instead of the mode's own plan, which
// on wfm is the difference between a 200 kHz request and a 12 kHz one.
// ui/models/receiver_link.cpp clears it on every tune for this reason.
engine::VrxParams click_to_tune(dsp::Hertz baseband_center, dsp::Hertz measured_width) {
    engine::VrxParams params;
    params.center = baseband_center;
    params.bandwidth = 0;
    params.demod = engine::demod_for_signal({.occupied_hz = measured_width});
    return params;
}

}  // namespace

// ---------------------------------------------------------------------------
// The decision rule
// ---------------------------------------------------------------------------

TEST_CASE("the demodulator comes from the measured width", "[engine][tune][m1]") {
    using engine::Demod;
    using engine::SignalEvidence;

    // The case on air. 145 kHz measured, 150 kHz here so the constant is
    // not sitting on the boundary of anything.
    CHECK(engine::demod_for_signal({.occupied_hz = kBroadcastWidthHz}) == Demod::Wfm);

    // A land mobile channel stays narrowband. 16 kHz is the Carson figure
    // dsp::default_passband(Nfm) is derived from and 25 kHz is the
    // allocation it lives in, so both are Nfm and the first hertz past the
    // allocation is not.
    CHECK(engine::demod_for_signal({.occupied_hz = 16'000}) == Demod::Nfm);
    CHECK(engine::demod_for_signal({.occupied_hz = 25'000}) == Demod::Nfm);
    CHECK(engine::demod_for_signal({.occupied_hz = 25'001}) == Demod::Wfm);

    // No measurement is not a measurement of zero. A caller with nothing to
    // go on gets what rpc::VrxParams::demod would have given it anyway,
    // which is the one case where the struct default is the right answer.
    CHECK(engine::demod_for_signal({}) == Demod::Nfm);
    CHECK(engine::demod_for_signal({.occupied_hz = -1}) == Demod::Nfm);

    // A family, when one is known, is a measurement of the modulation and
    // the width is a measurement of the occupancy, so the family wins.
    // Nothing populates it today; the cases exist so that the day something
    // does, the rule is already pinned.
    const SignalEvidence carrier{.occupied_hz = 2'000,
                                 .family = characterise::ModulationFamily::Unmodulated};
    CHECK(engine::demod_for_signal(carrier) == Demod::Cw);

    const SignalEvidence linear{.occupied_hz = kBroadcastWidthHz,
                                .family = characterise::ModulationFamily::Psk};
    CHECK(engine::demod_for_signal(linear) == Demod::Raw);

    const SignalEvidence multicarrier{.occupied_hz = kBroadcastWidthHz,
                                      .family = characterise::ModulationFamily::Ofdm};
    CHECK(engine::demod_for_signal(multicarrier) == Demod::Raw);

    // FSK falls through to the width on purpose: a discriminator is the
    // front end of every FSK voice mode this project will decode, and the
    // width is what says which one.
    const SignalEvidence keyed{.occupied_hz = 12'000,
                               .family = characterise::ModulationFamily::Fsk};
    CHECK(engine::demod_for_signal(keyed) == Demod::Nfm);
}

// ---------------------------------------------------------------------------
// The grid that carries it
// ---------------------------------------------------------------------------

TEST_CASE("the channel count follows the widest receiver", "[engine][tune][m1]") {
    // The grid is 2x oversampled, so a receiver landing anywhere is
    // guaranteed rate/M and the count is the largest power of two that
    // keeps that at or above the width.
    CHECK(engine::channel_count_for(kSourceRate, 200'000) == 8);
    CHECK(static_cast<dsp::Hertz>(kSourceRate) / 8 >= 200'000);

    // A narrower receiver buys back resolution, which is the trade the
    // whole rule exists to make explicit.
    CHECK(engine::channel_count_for(kSourceRate, 16'000) == 128);

    // Two is the floor rather than one: a source that cannot carry the
    // width at any count is not helped by a channelizer that channelizes
    // nothing.
    CHECK(engine::channel_count_for(250'000, 200'000) == 2);
    CHECK(engine::channel_count_for(0, 200'000) == 2);
    CHECK(engine::channel_count_for(kSourceRate, 0) == 2);

    // default_channel_count is this function at kWidestReceiverHz and not a
    // second copy of the arithmetic.
    CHECK(engine::default_channel_count(kSourceRate) ==
          engine::channel_count_for(kSourceRate, engine::kWidestReceiverHz));
}

TEST_CASE("a WFM placement one channel cannot carry is refused by name",
          "[engine][tune][m1]") {
    dsp::GridParams coarse;
    coarse.channels = 64;
    coarse.decimation = 32;
    coarse.taps_per_branch = 17;

    const engine::VrxParams params = click_to_tune(0, kBroadcastWidthHz);
    REQUIRE(params.demod == engine::Demod::Wfm);

    auto refused = engine::place(coarse, kSourceRate, params);
    REQUIRE_FALSE(refused.has_value());

    const std::string& message = refused.error().message;
    INFO(message);

    // What the mode needs, which is the table's own figure and not the
    // measured width.
    CHECK(message.find("wfm receiver needs 200000 Hz") != std::string::npos);

    // Why it is refused rather than narrowed. This sentence is the whole
    // difference between a report and a fix: an operator who is told the
    // number still has a receiver making noise.
    CHECK(message.find("wrong audio") != std::string::npos);

    // And the one argument that changes it, plus the reason this session
    // cannot.
    CHECK(message.find("--channels 8") != std::string::npos);
    CHECK(message.find("cannot be changed while it is running") != std::string::npos);

    // The count the refusal names is the count that works, checked rather
    // than trusted: the same request on that grid is granted in full.
    dsp::GridParams wide;
    wide.channels = 8;
    wide.decimation = 4;
    wide.taps_per_branch = 17;

    auto placed = engine::place(wide, kSourceRate, params);
    INFO(test::message_of(placed));
    REQUIRE(placed.has_value());
    CHECK_FALSE(placed->bandwidth_clamped);
    CHECK(placed->granted_high - placed->granted_low ==
          dsp::default_passband(engine::Demod::Wfm).width());
}

TEST_CASE("a clamp that only narrows a linear mode is still granted", "[engine][tune][m1]") {
    // The other side of the rule, and the reason it is not "refuse every
    // clamp". An envelope detector is linear in its passband, so an AM
    // receiver given less bandwidth has less audio bandwidth and is not
    // broken. Refusing it would train an operator to ignore the refusal on
    // the mode where it matters.
    dsp::GridParams coarse;
    coarse.channels = 64;
    coarse.decimation = 32;
    coarse.taps_per_branch = 17;

    engine::VrxParams wide_am;
    wide_am.center = 0;
    wide_am.demod = engine::Demod::Am;
    wide_am.passband_low = -90'000;
    wide_am.passband_high = 90'000;

    auto placed = engine::place(coarse, kSourceRate, wide_am);
    INFO(test::message_of(placed));
    REQUIRE(placed.has_value());
    CHECK(placed->bandwidth_clamped);
    CHECK(placed->granted_high - placed->granted_low < 180'000);

    // And a narrow WFM receiver somebody asked for on purpose is not a
    // clamp at all, so it is not refused either. tests/rpc/test_rpc_rds.cpp
    // opens exactly this to prove a decoder faults on a passband too narrow
    // for the FM composite.
    engine::VrxParams narrow_wfm;
    narrow_wfm.center = 0;
    narrow_wfm.demod = engine::Demod::Wfm;
    narrow_wfm.passband_low = -20'000;
    narrow_wfm.passband_high = 20'000;

    auto narrow = engine::place(coarse, kSourceRate, narrow_wfm);
    INFO(test::message_of(narrow));
    REQUIRE(narrow.has_value());
    CHECK_FALSE(narrow->bandwidth_clamped);
}

// ---------------------------------------------------------------------------
// The whole path, on a device
// ---------------------------------------------------------------------------

TEST_CASE("clicking a 150 kHz signal opens a receiver that can hear it",
          "[gpu][engine][tune][m1]") {
    REVENANT_NEEDS_GPU();

    // Zero is "size the grid from the source", which is what a tool with an
    // operator behind it passes. At 2.4 MS/s that is eight channels, and
    // eight channels is what makes the rest of this case possible.
    auto created = engine::Engine::create(config_with_channels(0));
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;

    constexpr dsp::Hertz kOffset = 300'000;
    REQUIRE(eng.open_source(scene_uri(kOffset, 1'200'000)).has_value());
    CHECK(eng.info().grid.channels == 8);

    const engine::VrxParams params = click_to_tune(kOffset, kBroadcastWidthHz);
    CHECK(params.demod == engine::Demod::Wfm);

    auto added = eng.add_vrx(params);
    INFO(test::message_of(added));
    REQUIRE(added.has_value());

    auto status = eng.vrx_status(*added);
    INFO(test::message_of(status));
    REQUIRE(status.has_value());

    // The whole broadcast channel, not a fragment of it. This is the
    // assertion the defect would fail: before the grid followed the
    // receiver this came back at about a fifth of the width with
    // bandwidth_clamped set and the audio already wrong.
    CHECK_FALSE(status->placement.bandwidth_clamped);
    CHECK(status->placement.granted_low == -100'000);
    CHECK(status->placement.granted_high == 100'000);
    CHECK(status->placement.granted_high - status->placement.granted_low >
          kBroadcastWidthHz);

    // And the fine stage runs fast enough to represent it. A passband wider
    // than the rate it is demodulated at is a filter that cannot exist, and
    // dsp::minimum_demod_rate is 1.5 times the width.
    CHECK(static_cast<dsp::Hertz>(status->demod_rate) >= 300'000);

    std::mutex lock;
    std::atomic<std::uint64_t> chunks{0};
    dsp::SampleRate audio_rate = 0;
    std::size_t frames = 0;

    REQUIRE(eng.set_audio_sink(*added, [&](const engine::AudioChunk& chunk) -> Status {
                 chunks.fetch_add(1, std::memory_order_relaxed);
                 const std::lock_guard<std::mutex> guard(lock);
                 audio_rate = chunk.rate;
                 frames += chunk.samples.size() / (chunk.channels == 0 ? 1 : chunk.channels);
                 return {};
             }).has_value());

    const auto ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    // Running and delivering. The content is not this case's business: what
    // it establishes is that the receiver a click builds is one the engine
    // will actually carry, which is the thing that was not true.
    INFO(chunks.load() << " chunks, " << frames << " frames at " << audio_rate << " S/s");
    CHECK(chunks.load() > 0);
    CHECK(frames > 4096);
    CHECK(audio_rate == 48'000);
}

TEST_CASE("the same click on a grid that cannot carry it is refused, not narrowed",
          "[gpu][engine][tune][m1]") {
    REVENANT_NEEDS_GPU();

    // A caller that names a channel count gets exactly that count, which is
    // the library contract and is why this path still exists after
    // default_channel_count. What it no longer gets is a receiver that
    // sounds broken.
    auto created = engine::Engine::create(config_with_channels(64));
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;

    constexpr dsp::Hertz kOffset = 300'000;
    REQUIRE(eng.open_source(scene_uri(kOffset, 400'000)).has_value());

    auto refused = eng.add_vrx(click_to_tune(kOffset, kBroadcastWidthHz));
    REQUIRE_FALSE(refused.has_value());
    INFO(refused.error().message);
    CHECK(refused.error().message.find("--channels 8") != std::string::npos);

    // Nothing was left behind. A refusal that created a receiver anyway
    // would be worse than the clamp it replaced.
    CHECK(eng.vrx_ids().empty());
}
