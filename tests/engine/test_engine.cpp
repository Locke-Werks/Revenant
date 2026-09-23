// The whole chain, end to end, on a real device.
//
// This is the case that makes the architecture's central claim testable
// instead of aspirational: a source's native bytes cross the bus once, are
// widened by a compute kernel into the device ring, channelized on the device,
// tapped per receiver, and only the receiver's own output comes back. Every
// other test in the suite checks one stage against its twin. This one checks
// that the stages, wired together, still carry a signal from one end to the
// other and put it where it belongs.
//
// A tone at a known frequency is the input because it is the one signal whose
// correct answer is unarguable: it must arrive in the channel whose passband
// contains it, at unit amplitude, and at the right frequency within that
// channel. A chain that is subtly wrong about the grid, the branch reversal,
// the transform's ordering or the channel-major layout gets one of those three
// wrong, and none of them is visible in a spectrum that looks plausible.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/engine/engine.h"
#include "core/engine/vrx.h"
#include "core/source/registry.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/support/tone_measure.h"

using namespace revenant;
using Catch::Approx;

namespace {

constexpr dsp::SampleRate kSourceRate = 2'400'000;

// A synthetic scene holding exactly one unmodulated carrier, so the expected
// output is a single complex exponential and nothing else.
std::string tone_uri(dsp::Hertz offset_hz, dsp::SampleIndex samples) {
    // The noise floor stays on but is pushed far down, and the emitter sits
    // well above it. Turning the noise off entirely is refused, and rightly:
    // an emitter's level is an SNR, a ratio against the floor, so with no
    // floor there is nothing to place it against.
    // The span is sized to the emitter rather than to the frequency. A scene
    // places its emitters randomly inside the span and refuses a span an
    // emitter will not fit in, so a 3 kHz AM signal needs at least 3 kHz of
    // room. Giving it 3.2 kHz leaves the carrier within 100 Hz of where this
    // test wants it, which against a 37.5 kHz channel spacing is nothing.
    // AM rather than CW. CW is keyed, so it is silent between characters and
    // a level measured over an arbitrary window lands wherever the keying
    // happened to be. AM carries its carrier continuously, which makes the
    // level meaningful without making the frequency test any weaker.
    return "synthetic:wideband?rate=" + std::to_string(kSourceRate) +
           "&emitters=1&modes=am&seed=424242&noise_dbfs=-120&snr_min=60&snr_max=60" +
           "&samples=" + std::to_string(samples) +
           "&span_low=" + std::to_string(offset_hz - 1600) +
           "&span_high=" + std::to_string(offset_hz + 1600);
}

// One continuously-keyed narrowband FM transmission, for the cases that want
// something a demodulator can actually recover.
//
// The span is 16 kHz wide because the scene picks the emitter's deviation
// between 2.5 and 5 kHz and its modulating tone up to 1.5 kHz, so by Carson's
// rule the widest it builds is 13 kHz and a narrower span would be refused.
// The emitter then sits somewhere inside that span rather than exactly at the
// offset, which is why the receiver in these cases is wider than the signal.
std::string nfm_uri(dsp::Hertz offset_hz, dsp::SampleIndex samples) {
    return "synthetic:wideband?rate=" + std::to_string(kSourceRate) +
           "&emitters=1&modes=nfm&seed=515151&noise_dbfs=-120&snr_min=50&snr_max=50" +
           "&samples=" + std::to_string(samples) +
           "&span_low=" + std::to_string(offset_hz - 8000) +
           "&span_high=" + std::to_string(offset_hz + 8000);
}

engine::EngineConfig default_config() {
    engine::EngineConfig config;
    config.channels = 64;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = 32'768;
    config.gpu_index = -1;  // honours REVENANT_GPU_INDEX
    return config;
}

}  // namespace

TEST_CASE("the engine opens a source and reports what it settled on", "[gpu][engine][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    auto created = engine::Engine::create(default_config());
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;

    const auto opened = eng.open_source(tone_uri(0, 400'000));
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    const auto& info = eng.info();
    INFO("ring " << info.ring.capacity_samples << " samples, " << info.ring.seconds_retained
                 << " s; grid M=" << info.grid.channels << " D=" << info.grid.decimation);

    CHECK(info.source_rate == kSourceRate);
    CHECK(info.grid.channels == 64);

    // 2x oversampled, which is the project's choice and not a default worth
    // drifting from: a critically sampled bank splits a signal on a channel
    // edge across two channels and neither is usable.
    CHECK(info.grid.decimation == info.grid.channels / 2);
    CHECK(info.channel_rate == kSourceRate / info.grid.decimation);

    // The ring reports what it achieved rather than what was asked for.
    CHECK(info.ring.capacity_samples > 0);
    CHECK(info.ring.seconds_retained > 0.0);
}

TEST_CASE("a receiver cannot be placed before a source is open", "[gpu][engine][m1]") {
    REVENANT_NEEDS_GPU();

    auto created = engine::Engine::create(default_config());
    REQUIRE(created.has_value());

    engine::VrxParams params;
    params.demod = engine::Demod::Raw;
    const auto added = (*created)->add_vrx(params);

    // Placement depends on the grid and the grid depends on the source's rate,
    // so this is refused rather than being answered with a guess.
    REQUIRE_FALSE(added.has_value());
    INFO(added.error().message);
    CHECK(added.error().message.find("source") != std::string::npos);
}

TEST_CASE("a tone travels the whole chain and arrives in the right channel",
          "[gpu][engine][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // Put the carrier squarely inside one channel rather than on an edge, so
    // the test is about the chain being correct and not about a filter's
    // rolloff.
    constexpr dsp::Hertz kChannelSpacing = kSourceRate / 64;
    constexpr dsp::Hertz kToneOffset = kChannelSpacing * 5;

    auto created = engine::Engine::create(default_config());
    REQUIRE(created.has_value());
    auto& eng = **created;

    REQUIRE(eng.open_source(tone_uri(kToneOffset, 600'000)).has_value());

    engine::VrxParams params;
    params.center = kToneOffset;
    params.bandwidth = kChannelSpacing;
    params.demod = engine::Demod::Raw;

    const auto added = eng.add_vrx(params);
    INFO(test::message_of(added));
    REQUIRE(added.has_value());

    const auto status = eng.vrx_status(*added);
    REQUIRE(status.has_value());
    INFO("placed on channel " << status->placement.channel << " centred at "
                              << status->placement.channel_centre.hertz() << " Hz");

    // Channel 5 is where a carrier at five channel spacings belongs. Getting
    // this wrong by a mirror, which is what dropping the branch reversal
    // would do, would place it on channel 59.
    CHECK(status->placement.channel == 5);

    std::mutex lock;
    std::vector<dsp::Complex32> collected;
    std::atomic<std::uint64_t> chunks{0};

    REQUIRE(eng.set_audio_sink(*added, [&](const engine::AudioChunk& chunk) -> Status {
                 chunks.fetch_add(1, std::memory_order_relaxed);
                 // The raw tap hands back interleaved complex.
                 if (chunk.channels != 2) {
                     return fail("the raw tap should produce interleaved I/Q");
                 }
                 const std::lock_guard<std::mutex> guard(lock);
                 if (collected.size() < 65536) {
                     for (std::size_t i = 0; i + 1 < chunk.samples.size(); i += 2) {
                         collected.push_back(
                             dsp::Complex32{chunk.samples[i], chunk.samples[i + 1]});
                     }
                 }
                 return {};
             }).has_value());

    const auto ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    INFO(chunks.load() << " audio chunks, " << collected.size() << " complex samples");
    REQUIRE(chunks.load() > 0);
    REQUIRE(collected.size() > 8192);

    // No samples were lost anywhere. A Demand source cannot overrun by
    // construction, so anything other than zero here is a defect rather than
    // a load problem.
    const auto stats = eng.source_stats();
    CHECK(stats.overrun_events == 0);
    CHECK(stats.samples_lost == 0);

    // Skip the filter's settling transient before measuring.
    const std::size_t skip = collected.size() / 4;
    const std::size_t count = 4096;
    REQUIRE(collected.size() > skip + count);

    // The carrier sits at the channel centre, so inside the channel it should
    // be at or very near DC. Estimate its frequency from the mean phase
    // advance between consecutive samples, which is exact for a pure tone and
    // needs no transform.
    std::complex<double> product{0.0, 0.0};
    double power = 0.0;
    for (std::size_t i = skip + 1; i < skip + count; ++i) {
        const std::complex<double> a(collected[i]);
        const std::complex<double> b(collected[i - 1]);
        product += a * std::conj(b);
        power += std::norm(a);
    }
    const double mean_power = power / static_cast<double>(count - 1);
    const double advance = std::arg(product);
    const double channel_rate = static_cast<double>(eng.info().channel_rate);
    const double measured_hz = advance * channel_rate / (2.0 * 3.14159265358979323846);

    INFO("mean power " << mean_power << ", residual frequency " << measured_hz << " Hz");

    // There is a signal, and it is well clear of the scene's noise floor.
    // The floor is at -120 dBFS, which is 1e-12 in power, and the emitter
    // sits 60 dB above it. A threshold of 1e-9 is far enough above the floor
    // to mean something and far enough below the carrier to survive the
    // filter's passband ripple and AM's power being split with its sidebands.
    INFO("noise floor is 1e-12 in power; measured " << mean_power);
    CHECK(mean_power > 1.0e-9);

    // And it is at the channel centre, within a small fraction of the channel
    // spacing. A chain that mixed by the wrong residual, or placed the
    // receiver on the wrong channel, lands this far away from zero.
    CHECK(std::abs(measured_hz) < static_cast<double>(kChannelSpacing) * 0.05);
}

TEST_CASE("an NFM receiver demodulates a synthetic transmission", "[gpu][engine][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The whole chain with a real demodulator on the end of it. Everything
    // below this is already proved elsewhere: the channelizer against its
    // twins in tests/reference/test_pfb.cpp, the fine stage and the detector
    // against theirs in tests/reference/test_vrx.cpp. What only this case
    // covers is the wiring between them, which is where the indices are, and
    // an index that is wrong by the filter's support produces audio that
    // sounds like audio.
    constexpr dsp::Hertz kOffset = 37'500 * 5;

    auto created = engine::Engine::create(default_config());
    REQUIRE(created.has_value());
    auto& eng = **created;

    REQUIRE(eng.open_source(nfm_uri(kOffset, 1'200'000)).has_value());

    engine::VrxParams params;
    params.center = kOffset;
    // Wide enough that the emitter is inside the passband wherever in the
    // span the scene put it, and wide enough that its deviation, which the
    // scene picks between 2.5 and 5 kHz, does not overrun the detector's
    // full-scale point.
    params.bandwidth = 25'000;
    params.demod = engine::Demod::Nfm;

    const auto added = eng.add_vrx(params);
    INFO(test::message_of(added));
    REQUIRE(added.has_value());

    std::mutex lock;
    std::vector<float> audio;
    dsp::SampleRate audio_rate = 0;
    std::uint32_t channels = 0;

    REQUIRE(eng.set_audio_sink(*added, [&](const engine::AudioChunk& chunk) -> Status {
                 const std::lock_guard<std::mutex> guard(lock);
                 audio_rate = chunk.rate;
                 channels = chunk.channels;
                 audio.insert(audio.end(), chunk.samples.begin(), chunk.samples.end());
                 return {};
             }).has_value());

    const auto ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    INFO(audio.size() << " audio samples at " << audio_rate << " S/s, " << channels
                      << " channel(s)");
    REQUIRE(audio.size() > 4096);

    // Real audio, not a complex tap: one float per frame at the engine's
    // audio rate rather than two at the channel rate.
    CHECK(channels == 1);
    CHECK(audio_rate == 48'000);

    // 1.2 million samples at 2.4 MS/s is half a second, which at 48 kHz is
    // 24000 audio samples. The chain loses the filter's support at the front
    // and whatever the last partial block could not complete at the back, so
    // this is a range and not an equality.
    CHECK(audio.size() > 20'000);
    CHECK(audio.size() < 24'100);

    const auto share = test::dominant_tone(audio, static_cast<double>(audio_rate),
                                           {400.0, 1000.0, 1500.0});
    INFO("dominant tone " << share.frequency_hz << " Hz holding " << share.share
                          << " of the AC power");

    // The scene modulates an NFM emitter with a single tone chosen from these
    // three. A discriminator fed the right samples recovers that tone and
    // very little else; one fed samples from the wrong point in the stream
    // recovers noise, and noise through a discriminator is full scale and
    // spread across the band.
    CHECK(share.share > 0.5);
}

TEST_CASE("a paced engine delivers on a clock rather than as fast as it can",
          "[gpu][engine][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // Unthrottled is the default and is what every other case here uses,
    // because the exit criterion is faster than realtime and there is no code
    // path for it. A loudspeaker is the one consumer that cannot accept that:
    // a capture replayed as fast as the GPU retires it fills the monitor's
    // ring, and what the listener hears is the backlog being trimmed. So a
    // source can be asked to hold a stopwatch, and this is the case that
    // proves the request reaches it.
    constexpr dsp::SampleIndex kSamples = 480'000;  // 0.2 s at 2.4 MS/s
    constexpr double kPace = 2.0;                   // so the run takes ~0.1 s

    auto config = default_config();
    config.pace = kPace;

    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;

    REQUIRE(eng.open_source(tone_uri(0, kSamples)).has_value());

    const auto start = std::chrono::steady_clock::now();
    const auto ran = eng.run();
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    const double capture_seconds =
        static_cast<double>(kSamples) / static_cast<double>(kSourceRate);
    const double floor_seconds = capture_seconds / kPace;
    INFO(capture_seconds << " s of capture at " << kPace << "x took " << elapsed << " s");

    // A lower bound only. A machine under load takes longer and that is not a
    // fault; a machine that finished early was not pacing at all, which is
    // the failure this exists to catch. 0.8 of the floor leaves room for the
    // source rounding its own block boundaries.
    CHECK(elapsed > floor_seconds * 0.8);
}

TEST_CASE("a receiver keeps producing audio across a retune", "[gpu][engine][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // Moving the dial is the one retune a stage handles in place: the tap
    // table is re-modulated to the new centre and copied into the buffer the
    // kernel reads, while a dispatch from an earlier frame is still reading
    // it. That copy is ordered against the earlier read by one barrier and
    // nothing else, and if the ordering is wrong the symptom is a block of
    // audio filtered through half of each tap table, which is a click.
    constexpr dsp::Hertz kOffset = 37'500 * 4;

    auto created = engine::Engine::create(default_config());
    REQUIRE(created.has_value());
    auto& eng = **created;

    REQUIRE(eng.open_source(nfm_uri(kOffset, 1'200'000)).has_value());

    engine::VrxParams params;
    params.center = kOffset;
    params.bandwidth = 25'000;
    params.demod = engine::Demod::Nfm;

    const auto added = eng.add_vrx(params);
    INFO(test::message_of(added));
    REQUIRE(added.has_value());

    std::mutex lock;
    std::size_t before = 0;
    std::size_t after = 0;
    std::atomic<bool> retuned{false};
    Status retune_status;

    REQUIRE(eng.set_audio_sink(*added, [&](const engine::AudioChunk& chunk) -> Status {
                 const std::lock_guard<std::mutex> guard(lock);
                 if (retuned.load(std::memory_order_relaxed)) {
                     after += chunk.samples.size();
                     return {};
                 }
                 before += chunk.samples.size();
                 if (before > 4096) {
                     // A few hundred hertz, which stays on the same coarse
                     // channel and leaves every rate and every tap count
                     // exactly where they were. That is the retune a stage
                     // must take in place.
                     engine::VrxParams moved = params;
                     moved.center = kOffset + 300;
                     retune_status = eng.set_vrx_params(*added, moved);
                     retuned.store(true, std::memory_order_relaxed);
                 }
                 return {};
             }).has_value());

    const auto ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    INFO(test::message_of(retune_status));
    CHECK(retune_status.has_value());

    INFO(before << " audio samples before the retune, " << after << " after");
    CHECK(retuned.load());
    CHECK(after > 4096);

    // The stream did not restart. A stage that rebuilt itself on a retune
    // would drop its position and the two halves would not add up.
    CHECK(before + after > 20'000);

    const auto status = eng.vrx_status(*added);
    REQUIRE(status.has_value());
    CHECK(status->params.center == kOffset + 300);
}

TEST_CASE("two retunes drained together move the tuning epoch by two and stamp one number",
          "[gpu][engine][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // WHAT A CONSUMER OF AudioChunk::tuning_epoch MUST NOT DO, pinned so it
    // cannot come back. Graph::drain_control applies the whole queued stack
    // in one pass before a single frame is recorded, so two retunes queued
    // inside one block period move the epoch twice and produce exactly ONE
    // number on the wire. A consumer that fenced by counting the changes it
    // observed therefore waited for two and saw one, and waited for ever;
    // core/rpc/server.cpp's RDS decoder did exactly that and answered with
    // zeros and no fault for the rest of the run, which reads as a quiet
    // band.
    //
    // Both retunes are queued BEFORE the engine runs, so the pass that
    // applies them is the first drain and there is no scheduling in the
    // assertion: every chunk this test sees carries the same epoch, and
    // that epoch is two.
    constexpr dsp::Hertz kOffset = 37'500 * 4;

    auto created = engine::Engine::create(default_config());
    REQUIRE(created.has_value());
    auto& eng = **created;

    REQUIRE(eng.open_source(nfm_uri(kOffset, 1'200'000)).has_value());

    engine::VrxParams params;
    params.center = kOffset;
    params.bandwidth = 25'000;
    params.demod = engine::Demod::Nfm;

    const auto added = eng.add_vrx(params);
    INFO(test::message_of(added));
    REQUIRE(added.has_value());

    const auto fresh = eng.vrx_status(*added);
    REQUIRE(fresh.has_value());
    CHECK(fresh->tuning_epoch == 0);

    // Two in-place moves: the centre shifts a few hundred hertz and every
    // rate and tap count stays where it was, which is the retune a stage
    // takes without a rebuild.
    engine::VrxParams first = params;
    first.center = kOffset + 300;
    const auto moved_once = eng.set_vrx_params(*added, first);
    INFO(test::message_of(moved_once));
    REQUIRE(moved_once.has_value());

    engine::VrxParams second = params;
    second.center = kOffset + 600;
    const auto moved_twice = eng.set_vrx_params(*added, second);
    INFO(test::message_of(moved_twice));
    REQUIRE(moved_twice.has_value());

    // THE TARGET IS KNOWABLE BEFORE ANY CHUNK CARRIES IT. This is the whole
    // of what makes a fence a comparison rather than a count: the number
    // the chunks are heading for is on the receiver the instant the retune
    // is accepted.
    const auto queued = eng.vrx_status(*added);
    REQUIRE(queued.has_value());
    CHECK(queued->tuning_epoch == 2);

    // A retune the graph REFUSES must not move it. A fence set against an
    // epoch no chunk will ever carry is the same permanent discard by
    // another route, so this is a correctness assertion and not tidiness.
    engine::VrxParams wider = params;
    wider.bandwidth = 50'000;
    const auto refused = eng.set_vrx_params(*added, wider);
    CHECK_FALSE(refused.has_value());
    const auto unmoved = eng.vrx_status(*added);
    REQUIRE(unmoved.has_value());
    CHECK(unmoved->tuning_epoch == 2);

    std::mutex lock;
    std::vector<std::uint64_t> epochs;
    std::size_t chunks = 0;

    REQUIRE(eng.set_audio_sink(*added, [&](const engine::AudioChunk& chunk) -> Status {
                 const std::lock_guard<std::mutex> guard(lock);
                 ++chunks;
                 if (epochs.empty() || epochs.back() != chunk.tuning_epoch) {
                     epochs.push_back(chunk.tuning_epoch);
                 }
                 return {};
             }).has_value());

    const auto ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    const std::lock_guard<std::mutex> guard(lock);
    INFO(chunks << " chunks carrying " << epochs.size() << " distinct epochs");
    REQUIRE(chunks > 0);

    // ONE VALUE, WHICH IS TWO. Not "the last one is two": the point of the
    // case is that the run contains no change of epoch at all, so a
    // consumer watching for changes has nothing to watch and a consumer
    // comparing against the target it read is already satisfied.
    REQUIRE(epochs.size() == 1);
    CHECK(epochs.front() == 2);
    CHECK(epochs.front() == queued->tuning_epoch);
}

TEST_CASE("a receiver that names no audio rate is planned at the engine's, not at 48 kHz",
          "[gpu][engine][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // VrxParams::audio_rate of zero means "the engine's default". The Qt
    // client always sends zero, because the whole point of the field is
    // that a client does not have to know what the engine was started
    // with. So this is not a corner: it is the only path one of the two
    // clients ever takes.
    //
    // The graph resolved it for the STAGE and then planned with the raw
    // request, where the planner falls back to its own 48000. On a graph
    // built with anything else the two disagreed, and the disagreement was
    // invisible until a retune was measured against the wrong basis.
    constexpr dsp::SampleRate kAudioRate = 16'000;
    constexpr dsp::Hertz kOffset = 37'500 * 4;

    engine::EngineConfig config = default_config();
    config.audio_rate = kAudioRate;

    auto created = engine::Engine::create(config);
    REQUIRE(created.has_value());
    auto& eng = **created;

    REQUIRE(eng.open_source(nfm_uri(kOffset, 400'000)).has_value());

    engine::VrxParams params;
    params.center = kOffset;
    params.demod = engine::Demod::Nfm;
    params.passband_low = -4'000;
    params.passband_high = 4'000;
    params.audio_rate = 0;

    const auto added = eng.add_vrx(params);
    INFO(test::message_of(added));
    REQUIRE(added.has_value());

    // An 8 kHz NFM band needs 12000 by the shared floor, which rounds up to
    // one whole audio rate at 16000 and to one at 48000 as well. The two
    // bases therefore differ by the whole engine rate, and this is the
    // number the passband display's axis spans.
    const auto status = eng.vrx_status(*added);
    REQUIRE(status.has_value());
    CHECK(status->demod_rate == kAudioRate);

    // And the retune is measured against that basis. Sliding the same 8 kHz
    // width up to +1000..+9000 leaves the width alone and moves the reach to
    // 9000, so the requirement goes to 18000 and the demodulation rate from
    // 16000 to 32000. That is a rebuild, and the caller has to be told on
    // its own call.
    //
    // On the 48000 basis the same slide changes nothing at all: 18000 still
    // rounds up to 48000. So this refusal is exactly the one the wrong basis
    // swallowed, and a success here means the graph is planning against a
    // rate its stage never ran at.
    engine::VrxParams slid = params;
    slid.passband_low = 1'000;
    slid.passband_high = 9'000;

    const Status refused = eng.set_vrx_params(*added, slid);
    INFO(test::message_of(refused));
    CHECK_FALSE(refused.has_value());
    if (!refused) {
        CHECK(refused.error().message.find("remove and an add") != std::string::npos);
    }

    // Refused before anything was stored, so the status still describes the
    // receiver that is actually running.
    const auto after = eng.vrx_status(*added);
    REQUIRE(after.has_value());
    CHECK(after->params.passband_low == -4'000);
    CHECK(after->demod_rate == kAudioRate);
}

TEST_CASE("an audio rate change that leaves the demodulation rate alone is still a rebuild",
          "[gpu][engine][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The shape a two-field comparison could not see. At the 48000 default
    // an NFM receiver at -10000..+10000 needs 30000, so Fd is 48000 and the
    // decimation is 1. Ask for 24000 and Fd is 48000 again, because 30000
    // still rounds up to two of them, and the tap count is identical
    // because the transition depends only on Fd, the edges and the channel
    // rate. What moves is the decimation, 1 to 2, and the output rate,
    // 48000 to 24000: a different pipeline and a different audio rate at
    // the sink.
    //
    // The graph used to accept this, store 24000, echo it back, and let
    // DemodStage::retune refuse it on the recording thread. The sink went on
    // emitting 48 kS/s while the client was told the rate had changed.
    constexpr dsp::Hertz kOffset = 37'500 * 4;

    auto created = engine::Engine::create(default_config());
    REQUIRE(created.has_value());
    auto& eng = **created;

    REQUIRE(eng.open_source(nfm_uri(kOffset, 400'000)).has_value());

    engine::VrxParams params;
    params.center = kOffset;
    params.demod = engine::Demod::Nfm;
    params.passband_low = -10'000;
    params.passband_high = 10'000;

    const auto added = eng.add_vrx(params);
    INFO(test::message_of(added));
    REQUIRE(added.has_value());

    const auto before = eng.vrx_status(*added);
    REQUIRE(before.has_value());
    CHECK(before->demod_rate == 48'000);

    engine::VrxParams halved = params;
    halved.audio_rate = 24'000;

    const Status refused = eng.set_vrx_params(*added, halved);
    INFO(test::message_of(refused));
    CHECK_FALSE(refused.has_value());
    if (!refused) {
        CHECK(refused.error().message.find("remove and an add") != std::string::npos);
    }

    // Nothing was stored, so the echo cannot claim a rate the sink is not
    // delivering.
    const auto after = eng.vrx_status(*added);
    REQUIRE(after.has_value());
    CHECK(after->params.audio_rate == 0);
    CHECK(after->demod_rate == 48'000);

    // A pan of the same width across the same rate is still free, which is
    // the property the refusal above must not have cost.
    engine::VrxParams panned = params;
    panned.passband_low = -9'500;
    panned.passband_high = 10'500;

    const Status accepted = eng.set_vrx_params(*added, panned);
    INFO(test::message_of(accepted));
    CHECK(accepted.has_value());
}

TEST_CASE("receivers on several modes run together", "[gpu][engine][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // Each receiver records its own two dispatches and its own barriers into
    // the shared command buffer, so more than one of them is a different code
    // path from one of them, and it is the path the engine will always be on.
    auto created = engine::Engine::create(default_config());
    REQUIRE(created.has_value());
    auto& eng = **created;

    REQUIRE(eng.open_source("synthetic:wideband?rate=" + std::to_string(kSourceRate) +
                            "&emitters=12&seed=90210&samples=600000")
                .has_value());

    struct Receiver {
        engine::Demod demod;
        dsp::Hertz bandwidth;
    };

    const Receiver wanted[] = {
        {engine::Demod::Raw, 12'000}, {engine::Demod::Am, 10'000},
        {engine::Demod::Nfm, 16'000}, {engine::Demod::Usb, 3'000},
        {engine::Demod::Lsb, 3'000},  {engine::Demod::Dsb, 6'000},
        {engine::Demod::Cw, 500},
    };

    std::mutex lock;
    std::vector<std::size_t> received(std::size(wanted), 0);
    std::vector<std::uint32_t> reported_channels(std::size(wanted), 0);

    for (std::size_t i = 0; i < std::size(wanted); ++i) {
        engine::VrxParams params;
        params.center = 37'500 * static_cast<dsp::Hertz>(i + 1);
        params.bandwidth = wanted[i].bandwidth;
        params.demod = wanted[i].demod;

        const auto added = eng.add_vrx(params);
        INFO("receiver " << i << " (" << engine::demod_name(wanted[i].demod) << "): "
                         << test::message_of(added));
        REQUIRE(added.has_value());

        REQUIRE(eng.set_audio_sink(*added, [&, i](const engine::AudioChunk& chunk) -> Status {
                     const std::lock_guard<std::mutex> guard(lock);
                     received[i] += chunk.samples.size();
                     reported_channels[i] = chunk.channels;
                     return {};
                 }).has_value());
    }

    const auto ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    for (std::size_t i = 0; i < std::size(wanted); ++i) {
        INFO("receiver " << i << " (" << engine::demod_name(wanted[i].demod) << ") delivered "
                         << received[i] << " floats on " << reported_channels[i]
                         << " channel(s)");
        CHECK(received[i] > 1024);

        // The raw tap hands back interleaved complex at the channel rate.
        // Everything else is real audio.
        CHECK(reported_channels[i] == (wanted[i].demod == engine::Demod::Raw ? 2U : 1U));
    }

    const auto stats = eng.source_stats();
    CHECK(stats.overrun_events == 0);
    CHECK(stats.samples_lost == 0);
}

// ---------------------------------------------------------------------------
// The grid the source implies
// ---------------------------------------------------------------------------

TEST_CASE("the default channel count carries the widest receiver at every rate",
          "[engine][m1]") {
    // No GPU. This is the arithmetic that decides whether an operator
    // clicking a broadcast FM station gets a working receiver, and it has
    // one answer per source rate rather than the constant 64 it used to
    // have.
    //
    // The guaranteed width is rate/M, per dsp::max_channel_bandwidth: a
    // receiver landing halfway between two channel centres keeps only that
    // much of the 2*rate/M its channel stream carries. So the bar each case
    // below checks is rate/M against 200 kHz.
    struct Case {
        dsp::SampleRate rate;
        std::uint32_t expect;
    };

    // 2.4 MS/s is the dongle the operator was on. 64 channels there is
    // 37.5 kHz guaranteed, which is a fifth of a broadcast FM passband; 8
    // is 300 kHz and is what revenant-engine's own --help example has been
    // telling the reader to pass by hand.
    //
    // 20 MS/s answers 64, which is why nothing about a wideband capture
    // changes.
    const Case cases[] = {
        {250'000, 2},      // too narrow at any M, so the floor
        {400'000, 2},      // exactly two 200 kHz channels
        {2'400'000, 8},    // bit_floor(12)
        {2'400'032, 8},    // the RPC fixture's rate, for the same reason
        {10'000'000, 32},  // bit_floor(50)
        {20'000'000, 64},  // bit_floor(100), the figure that used to be the constant
    };

    for (const Case& one : cases) {
        const std::uint32_t chosen = engine::default_channel_count(one.rate);
        INFO(one.rate << " S/s chose " << chosen << " channels");
        CHECK(chosen == one.expect);

        // Every answer has to be a grid the channelizer will take, which is
        // a power of two, and has to carry the widest receiver unless the
        // source is too narrow to at any count.
        CHECK(std::has_single_bit(chosen));
        const auto guaranteed = static_cast<dsp::Hertz>(one.rate) /
                                static_cast<dsp::Hertz>(chosen);
        if (one.rate >= 2 * engine::kWidestReceiverHz) {
            CHECK(guaranteed >= engine::kWidestReceiverHz);
        }
    }

    // A rate nobody should be able to reach, answered rather than divided
    // by. open_source refuses a non-positive rate on its own, and this
    // function is exported, so it answers the floor instead of dividing by
    // zero somewhere a caller cannot see.
    CHECK(engine::default_channel_count(0) == 2);
}

TEST_CASE("an engine given no channel count sizes the grid against the source",
          "[gpu][engine][m1]") {
    REVENANT_NEEDS_GPU();

    engine::EngineConfig config = default_config();
    config.channels = 0;

    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;

    const auto opened = eng.open_source(tone_uri(300'000, 200'000));
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    const engine::EngineInfo& info = eng.info();
    CHECK(info.grid.channels == engine::default_channel_count(kSourceRate));

    // The point of the whole change: a broadcast FM receiver placed
    // anywhere on this grid is not clamped. At the old constant 64 it was,
    // by about three to one.
    engine::VrxParams wide;
    wide.center = 300'000;
    wide.demod = engine::Demod::Wfm;
    wide.passband_low = -100'000;
    wide.passband_high = 100'000;

    auto placement = engine::place(info.grid, info.source_rate, wide);
    INFO(test::message_of(placement));
    REQUIRE(placement.has_value());
    CHECK_FALSE(placement->bandwidth_clamped);
    CHECK(placement->granted_high - placement->granted_low == 200'000);
}

// ---------------------------------------------------------------------------
// Retuning the front end, and the pacing measurement
// ---------------------------------------------------------------------------

TEST_CASE("a source that cannot retune refuses in its own words", "[gpu][engine][m1]") {
    REVENANT_NEEDS_GPU();

    engine::EngineConfig config = default_config();

    auto created = engine::Engine::create(config);
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;

    // Before a source, there is no front end to point anywhere and the
    // engine says that rather than dereferencing nothing.
    auto early = eng.set_source_center(100'000'000);
    REQUIRE_FALSE(early.has_value());
    CHECK(early.error().message.find("before a source is open") != std::string::npos);
    CHECK_FALSE(eng.source_tuning().can_retune);

    REQUIRE(eng.open_source(tone_uri(300'000, 200'000)).has_value());

    const engine::SourceTuning tuning = eng.source_tuning();
    CHECK_FALSE(tuning.can_retune);
    CHECK(tuning.low == 0);
    CHECK(tuning.high == 0);

    auto refused = eng.set_source_center(100'000'000);
    REQUIRE_FALSE(refused.has_value());
    INFO(refused.error().message);

    // The synthetic source's own sentence, carried through rather than
    // replaced. It names what to do instead, and that instruction differs
    // from the file source's.
    CHECK(refused.error().message.find("synthetic source cannot tune") != std::string::npos);

    // And nothing moved.
    CHECK(eng.info().source_center == 0);
}

TEST_CASE("the realtime factor is measured over the run and frozen when it ends",
          "[gpu][engine][m1]") {
    REVENANT_NEEDS_GPU();

    engine::EngineConfig config = default_config();
    config.pace = 0.0;

    auto created = engine::Engine::create(config);
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;
    REQUIRE(eng.open_source(tone_uri(300'000, 240'000)).has_value());

    {
        const engine::SourcePacing idle = eng.source_pacing();

        // NOT MEASURED, which is a third state. There is no elapsed time to
        // divide by before run() and a client reading zero as a stalled
        // source would fault every engine that has not started.
        CHECK(idle.realtime_factor == 0.0);
        CHECK(idle.elapsed_seconds == 0.0);
        CHECK(idle.paced_by == 0.0);

        // A synthetic scene is a demand source, which is what makes an
        // unthrottled run legitimate rather than a fault. A client needs
        // this to tell "nobody is holding a stopwatch" from "the radio's
        // own clock", where a factor below one means something else
        // entirely.
        CHECK(idle.demand);
    }

    REQUIRE(eng.run().has_value());

    const engine::SourcePacing done = eng.source_pacing();
    INFO("factor " << done.realtime_factor << " over " << done.elapsed_seconds << " s");
    CHECK(done.realtime_factor > 0.0);
    CHECK(done.elapsed_seconds > 0.0);
    CHECK(done.samples_delivered >= 240'000);

    // Frozen at what the run achieved rather than decaying against a clock
    // that keeps going. A finished replay reading as a dying source is the
    // failure this pins.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const engine::SourcePacing later = eng.source_pacing();
    CHECK(later.realtime_factor == done.realtime_factor);
    CHECK(later.elapsed_seconds == done.elapsed_seconds);
}

TEST_CASE("a source closes and another opens on the same engine", "[gpu][engine][m2]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    auto created = engine::Engine::create(default_config());
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;

    // Before anything, and this is the one place the epoch is zero. A
    // consumer that correlated against zero would be correlating against a
    // stream that has never existed, which is why the first open makes it one
    // rather than leaving the first stream at zero.
    CHECK_FALSE(eng.has_source());
    CHECK(eng.info().source_epoch == 0);

    // Closing an engine with nothing open is a success and not a refusal. A
    // client that closes before every open should not have to know which
    // state it was in to read the answer.
    CHECK(eng.close_source().has_value());

    REQUIRE(eng.open_source(tone_uri(0, 200'000)).has_value());
    CHECK(eng.has_source());
    CHECK(eng.info().source_epoch == 1);
    CHECK(eng.info().source_rate == kSourceRate);

    // A SECOND OPEN IS REFUSED RATHER THAN REPLACING THE FIRST, and the
    // refusal names the call that gets from one to the other. A replace that
    // failed on the new URI would have destroyed the working source already.
    auto second = eng.open_source(tone_uri(0, 200'000));
    REQUIRE_FALSE(second.has_value());
    INFO(second.error().message);
    CHECK(second.error().message.find("Close it first") != std::string::npos);

    // A receiver, so the close has something to tear down that a caller can
    // see the absence of afterwards.
    engine::VrxParams params;
    params.demod = engine::Demod::Raw;
    params.center = 0;
    params.bandwidth = 12'000;
    auto added = eng.add_vrx(params);
    INFO(test::message_of(added));
    REQUIRE(added.has_value());
    REQUIRE(eng.vrx_ids().size() == 1);

    const auto closed = eng.close_source();
    INFO(test::message_of(closed));
    REQUIRE(closed.has_value());

    CHECK_FALSE(eng.has_source());

    // EVERY RECEIVER WENT WITH THE GRAPH. Their placement was computed
    // against a grid that no longer exists and their centre is an offset from
    // a baseband whose meaning was the closed source's.
    CHECK(eng.vrx_ids().empty());

    // And the receiver that was there is refused by id rather than answered
    // with a zeroed status, which is what a client polling one across a close
    // has to be told.
    auto stale = eng.vrx_status(*added);
    CHECK_FALSE(stale.has_value());

    // Everything describing a source is back to nothing, and the device is
    // not: it belongs to the engine rather than to the stream.
    CHECK(eng.info().source_rate == 0);
    CHECK(eng.info().grid.channels == 0);
    CHECK(eng.info().source_center == 0);
    CHECK_FALSE(eng.info().device.name.empty());

    // THE EPOCH IS KEPT AND NOT CLEARED. An engine between sources still
    // answers truthfully about which stream the indices a client is holding
    // belonged to: that one, and it has ended.
    CHECK(eng.info().source_epoch == 1);

    // A second source, at a different rate, so the grid and the ring have to
    // be rebuilt rather than reused. This is the case a replace could not do
    // safely and the whole reason the lifecycle was opened up.
    const std::string other = "synthetic:wideband?rate=1200000&emitters=1&modes=am&seed=99"
                              "&noise_dbfs=-120&snr_min=60&snr_max=60&samples=200000";
    const auto reopened = eng.open_source(other);
    INFO(test::message_of(reopened));
    REQUIRE(reopened.has_value());

    CHECK(eng.has_source());
    CHECK(eng.info().source_rate == 1'200'000);
    CHECK(eng.info().source_epoch == 2);

    // The grid was sized against the new rate rather than carried over.
    CHECK(eng.info().grid.channels > 0);
    CHECK(eng.info().grid.decimation > 0);
    CHECK(eng.info().channel_rate ==
          1'200'000 / static_cast<dsp::SampleRate>(eng.info().grid.decimation));

    // Receiver ids do NOT restart. They are monotonic for the life of the
    // engine so that a client holding one from the previous source cannot
    // address a live one on this one.
    auto after = eng.add_vrx(params);
    INFO(test::message_of(after));
    REQUIRE(after.has_value());
    CHECK(after->value > added->value);
}

TEST_CASE("closing a running source stops the stream first", "[gpu][engine][m2]") {
    REVENANT_NEEDS_GPU();

    auto created = engine::Engine::create(default_config());
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;

    // Long enough that the run cannot finish on its own while this case is
    // closing it. The point is the close racing a LIVE stream, which is the
    // arrangement that would be a use-after-free without the wait: run()
    // holds a raw Graph* in the source callback and flushes the graph after
    // the stream ends.
    REQUIRE(eng.open_source(tone_uri(0, 40'000'000)).has_value());

    std::thread runner([&eng] { static_cast<void>(eng.run()); });

    // Wait for the stream to actually be running rather than assuming the
    // thread got there, so the close below is measured against a stream and
    // not against a race this case happened to win.
    for (int i = 0; i < 500 && !eng.running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(eng.running());

    const auto closed = eng.close_source();
    INFO(test::message_of(closed));
    REQUIRE(closed.has_value());

    // close_source waited for run() to be finished with the graph, so by the
    // time it returned the engine was no longer running. Asserting it here
    // rather than after the join is the point: the join would make this true
    // whether or not the wait existed.
    CHECK_FALSE(eng.running());
    CHECK_FALSE(eng.has_source());

    runner.join();

    // And the engine is usable again rather than merely not crashed.
    REQUIRE(eng.open_source(tone_uri(0, 200'000)).has_value());
    CHECK(eng.info().source_epoch == 2);

    // THE SECOND STREAM HAS TO ACTUALLY RUN, which is the assertion this case
    // was missing and the one that matters. Opening a source proves the grid
    // and the ring were rebuilt; it says nothing about whether the cancel that
    // ended the first stream left anything behind that stops the second.
    //
    // It did. Graph::cancel poisons the ring as well as the graph, on purpose
    // and permanently, because the thread it has to reach is the source's and
    // it is parked inside the ring's reserve_blocking. close_source replaces
    // the ring, so that much is clean; what this case caught is the whole
    // second run returning immediately instead of delivering a block.
    const auto second = eng.run();
    INFO(test::message_of(second));
    CHECK(second.has_value());

    const source::SourceStats after = eng.source_stats();
    INFO("second stream delivered " << after.blocks_delivered << " blocks, "
                                    << after.samples_delivered << " samples");
    CHECK(after.blocks_delivered > 0);
    CHECK(after.samples_delivered > 0);
}

TEST_CASE("a retune holds each receiver's frequency and drops the ones it leaves behind",
          "[gpu][engine][m2]") {
    REVENANT_NEEDS_GPU();

    // A SYNTHETIC SCENE CANNOT RETUNE, so this drives the rebase through the
    // arithmetic rather than through a device: what the engine does to a
    // receiver's offset is the same whichever backend moved the oscillator, and
    // the case that needs a dongle is in the device test further down.
    //
    // Two receivers, placed either side of the centre, so the near one survives
    // a small move and the far one does not. Before 2026-09-21 both survived
    // every move, because a receiver kept its BASEBAND offset and was carried
    // along with the span: an operator retuning from broadcast FM found their
    // receiver still making noise at a frequency they had not chosen.
    auto created = engine::Engine::create(default_config());
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;

    REQUIRE(eng.open_source(tone_uri(0, 400'000)).has_value());

    const dsp::Hertz half_span = static_cast<dsp::Hertz>(kSourceRate / 2);

    engine::VrxParams near_centre;
    near_centre.center = 100'000;
    near_centre.bandwidth = 16'000;
    near_centre.demod = engine::Demod::Nfm;
    const auto near_id = eng.add_vrx(near_centre);
    INFO(test::message_of(near_id));
    REQUIRE(near_id.has_value());

    engine::VrxParams near_edge;
    near_edge.center = half_span - 50'000;
    near_edge.bandwidth = 16'000;
    near_edge.demod = engine::Demod::Nfm;
    const auto edge_id = eng.add_vrx(near_edge);
    INFO(test::message_of(edge_id));
    REQUIRE(edge_id.has_value());

    REQUIRE(eng.vrx_ids().size() == 2);

    // The synthetic source refuses, which is the point: a refused retune must
    // not touch a receiver at all. A rebase applied before the device answered
    // would move every receiver on a call that changed nothing.
    const auto refused = eng.set_source_center(462'000'000);
    REQUIRE_FALSE(refused.has_value());

    CHECK(eng.vrx_ids().size() == 2);
    auto near_after = eng.vrx_status(*near_id);
    REQUIRE(near_after.has_value());
    CHECK(near_after->params.center == 100'000);
    auto edge_after = eng.vrx_status(*edge_id);
    REQUIRE(edge_after.has_value());
    CHECK(edge_after->params.center == half_span - 50'000);
}

TEST_CASE("an HF recording gets the finer grid its content needs", "[gpu][engine][m2]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // THE HALF OF ResolutionRequest THAT CHOOSES, against the half that states.
    //
    // A file source calls source::resolution_for_span on its own centre and
    // rate, and below 30 MHz that asks for four bins across PSK31's 31 Hz,
    // which is a ceiling of 7.75 Hz per bin. The shipped geometry is 36.6 Hz.
    // Nothing read the request until Engine::open_source did, and what a
    // detector does on a grid that coarse is worse than missing the signal: it
    // fires, and publishes a centre it cannot place inside the signal and a
    // width that is the bin's rather than the signal's.
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "revenant_test_hf_grid.cf32";

    // Enough samples to open and size a ring against, and no more. Nothing
    // here runs the stream; the assertion is about the geometry the open
    // settled on.
    {
        const std::vector<dsp::Complex32> data(200'000, dsp::Complex32{0.0F, 0.0F});
        std::FILE* file = std::fopen(path.string().c_str(), "wb");
        REQUIRE(file != nullptr);
        std::fwrite(data.data(), sizeof(dsp::Complex32), data.size(), file);
        std::fclose(file);
    }

    struct Remove {
        std::filesystem::path path;
        ~Remove() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } remove{path};

    // 7.1 MHz is the 40 m band, well inside HF, and 2 MS/s is an ordinary
    // width for a wideband HF capture.
    const std::string hf = "file:///" + path.generic_string() +
                           "?rate=2000000&format=cf32&center=7100000";

    engine::EngineConfig config = default_config();
    config.spectrum_transform = 2048;  // the shipped default, and its ceiling

    // ZERO, WHICH IS THE WHOLE CONDITION. The contract is that a caller who
    // names a channel count gets exactly that count, so the request can only
    // move a count the engine chose. default_config names 64, which is what
    // every other case here wants and is what this one must not have.
    config.channels = 0;

    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;

    const auto opened = eng.open_source(hf);
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    const engine::EngineInfo& info = eng.info();
    INFO("transform " << info.spectrum.transform << ", "
                      << info.spectrum.bin_width_hz() << " Hz per bin");

    // THE PRECONDITION FIRST, so a failure says which half is wrong. If the
    // source states no request there is nothing for the engine to meet, and
    // asserting the consequence first would report the choosing half as broken
    // when it was the stating half that said nothing.
    const auto& wanted = eng.source_capabilities().resolution;
    INFO("asked for " << wanted.bins_across_narrowest << " bins across "
                      << wanted.narrowest_signal_hz << " Hz: " << wanted.basis);
    REQUIRE(wanted.stated());

    // And the count this rate would have chosen on its own does NOT meet it, or
    // the assertions below prove nothing: a request already met needs no
    // widening, and the choosing half would be dead code that still passed.
    const std::uint32_t unaided = engine::default_channel_count(info.source_rate);
    INFO("unaided this rate would have chosen " << unaided << " channels");
    REQUIRE_FALSE(wanted.met_by(info.source_rate,
                                static_cast<std::int64_t>(unaided / 2) * 2048));

    // WIDENED, AND THE ASSERTION IS THE REQUEST RATHER THAN A NUMBER. Checking
    // for 256 specifically would pin an answer that depends on the rate, the
    // transform and the device's shared memory at once, and would have to be
    // re-derived by hand every time one of them moved. The request is what this
    // exists to meet, so the request is what is asserted.
    //
    // THE CHANNEL COUNT AND NOT THE TRANSFORM. Bin width is rate / (D * N) and
    // both narrow it, but dsp::kMaxSpectrumTransform is 2048 and the shipped
    // default is already at it, so N has nothing left to give. That cap is a
    // twiddle-table limit shared with the channelizer rather than a device one.
    CHECK(info.grid.channels > unaided);
    CHECK(info.spectrum.transform == 2048);
    CHECK(wanted.met_by(info.spectrum.bin_width_numerator,
                        info.spectrum.bin_width_denominator));

    // SAID, NOT SILENTLY SUBSTITUTED. The ring's clamp sentence is the one
    // field in EngineInfo that can carry prose, which is how a reduced channel
    // count is already reported, and a geometry the caller did not ask for has
    // to arrive the same way.
    INFO("clamp reason: " << info.ring.clamp_reason);
    CHECK(info.ring.clamped);
    CHECK(info.ring.clamp_reason.find("bins across") != std::string::npos);

    // And it names what the widening COST, which is the part an operator has to
    // know before they try to place a receiver: more channels is a narrower
    // widest receiver, and the two trade directly.
    CHECK(info.ring.clamp_reason.find("channel spacing") != std::string::npos);
}

TEST_CASE("a VHF source keeps the transform the caller chose", "[gpu][engine][m2]") {
    REVENANT_NEEDS_GPU();

    // THE CONTROL ARM, and without it the case above is not evidence that
    // anything is conditional. A synthetic scene states no resolution request
    // at all, so nothing should move: an unstated request is met by anything,
    // and a function that raised the transform regardless would pass every
    // assertion in the HF case and quietly double the cost of every VHF
    // session.
    engine::EngineConfig config = default_config();
    config.spectrum_transform = 2048;
    config.channels = 0;

    auto created = engine::Engine::create(config);
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;

    REQUIRE(eng.open_source(tone_uri(0, 200'000)).has_value());

    CHECK_FALSE(eng.source_capabilities().resolution.stated());
    CHECK(eng.info().spectrum.transform == 2048);
    CHECK(eng.info().grid.channels == engine::default_channel_count(kSourceRate));
    CHECK_FALSE(eng.info().ring.clamp_reason.find("bins across") != std::string::npos);
}

TEST_CASE("a dongle opened after a close can be retuned while the graph runs",
          "[gpu][engine][m2][device]") {
    REVENANT_NEEDS_GPU();

    // THE PICKER'S EXACT PATH, minus the RPC and the window: an engine already
    // streaming something else, a close, an open onto a live dongle, the graph
    // running, and a retune.
    //
    // Observed by hand on 2026-09-21 against a live R820T. Samples flowed and
    // frames reached the client, so the open and the stream were fine, and every
    // retune failed inside the tuner with "r82xx_set_freq: failed=-9", which is
    // LIBUSB_ERROR_PIPE on the i2c write. That message proves the request
    // reached librtlsdr, so the client was not refusing locally.
    //
    // Four narrower shapes were written first and all four pass on this
    // hardware, in tests/engine/test_rtlsdr_source.cpp: tuning a stopped dongle,
    // tuning a streaming one, describing every device before opening one, and
    // describing every device while one streams. What none of them has is the
    // engine, the graph, and a close of a different backend first.
    // describe_sources rather than enumerate_sources, because that is what the
    // picker calls: Session.listSources answers from the describing form, and
    // on this backend describing means opening every device index to ask the
    // tuner what it is. So by the time the engine opens the dongle, this
    // process has already opened and closed it once.
    std::string dongle;
    {
        auto described = source::describe_sources();
        if (!described) {
            SKIP("sources could not be described: " + described.error().message);
        }
        for (const source::SourceCapabilities& caps : *described) {
            if (caps.backend == "rtlsdr" && caps.available()) {
                dongle = caps.uri;
                break;
            }
        }
    }
    if (dongle.empty()) {
        SKIP("no RTL-SDR described itself as available");
    }

    auto created = engine::Engine::create(default_config());
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;

    // A synthetic scene first, running, exactly as the window is when somebody
    // opens the picker. Long enough that it cannot end on its own.
    REQUIRE(eng.open_source(tone_uri(0, 40'000'000)).has_value());
    std::thread first([&eng] { static_cast<void>(eng.run()); });
    for (int i = 0; i < 500 && !eng.running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(eng.running());

    REQUIRE(eng.close_source().has_value());
    first.join();

    // The URI the picker composes with a centre typed and the rate and gain
    // boxes left empty, rather than a fuller one written here: the backend's
    // defaults are the same either way, and spelling them out would be testing
    // a different string from the one that failed by hand.
    const auto opened = eng.open_source(dongle + "?freq=98100000");
    if (!opened) {
        SKIP("the dongle could not be opened after the close: " + opened.error().message);
    }

    std::thread second([&eng] { static_cast<void>(eng.run()); });
    for (int i = 0; i < 500 && !eng.running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(eng.running());

    // Streaming for real before the tune, so the retune lands against transfers
    // in flight and a GPU graph consuming them.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline &&
           eng.source_stats().samples_delivered < 600'000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    INFO("samples before the tune: " << eng.source_stats().samples_delivered);
    REQUIRE(eng.source_stats().samples_delivered >= 600'000);

    const engine::SourceTuning tuning = eng.source_tuning();
    INFO("can retune " << tuning.can_retune << ", " << tuning.low << " to " << tuning.high);
    REQUIRE(tuning.can_retune);

    // A RECEIVER, LIVE, BEFORE THE RETUNE. Graph::on_block is documented "the
    // source thread only", and retune_streaming_locked has to stop the
    // transfers to move the tuner, which used to take the delivery thread down
    // with them and hand on_block to a freshly spawned one. With no receiver in
    // the graph that survived three retunes in a row by hand, VHF and UHF
    // alike, because the recording a bare spectrum pass does is not the
    // recording that minds. With a receiver it is a crash, which is what
    // reached the operator: type a frequency, press enter, engine gone.
    // nfm and not wfm: default_config pins a 64 channel grid, which guarantees
    // a receiver 75 kHz anywhere and refuses a 200 kHz broadcast receiver in as
    // many words. What this case needs is a receiver being recorded per block,
    // and the mode does not matter to that.
    engine::VrxParams listening;
    listening.center = 0;
    listening.bandwidth = 16'000;
    listening.demod = engine::Demod::Nfm;
    const auto receiver = eng.add_vrx(listening);
    INFO(test::message_of(receiver));
    REQUIRE(receiver.has_value());

    // WITH AUDIO RUNNING, because that is what the operator had and because it
    // is a second consumer on the sample path rather than a detail of the
    // display. A receiver nobody is listening to records less per block than
    // one that is feeding a sink.
    std::atomic<std::uint64_t> audio_chunks{0};
    REQUIRE(eng.set_audio_sink(*receiver, [&audio_chunks](const engine::AudioChunk&) -> Status {
                   audio_chunks.fetch_add(1, std::memory_order_relaxed);
                   return {};
               })
                .has_value());

    // Long enough that the receiver is genuinely being recorded per block
    // rather than merely registered.
    const auto settled = eng.source_stats().samples_delivered + 600'000;
    const auto vrx_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < vrx_deadline &&
           eng.source_stats().samples_delivered < settled) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(eng.vrx_ids().size() == 1);

    // THE CONTROL. How fast audio arrives with nothing being retuned, measured
    // over the same interval the check after the retunes uses, so "audio
    // stopped" can be told from "audio was never flowing".
    const std::uint64_t audio_a = audio_chunks.load(std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const std::uint64_t audio_b = audio_chunks.load(std::memory_order_relaxed);
    INFO("audio chunks with no retune: " << audio_a << " then " << audio_b << " one second later");
    REQUIRE(audio_b > audio_a);

    // A NUDGE FIRST, AND THE RECEIVER STAYS ON ITS OWN FREQUENCY.
    //
    // 100 kHz and not the 3 MHz this case used to move, because the order now
    // matters: the receiver was added at a baseband offset of 0, so at a centre
    // of 98.1 MHz it is on 98.1 MHz, and a 3 MHz move puts it outside a 2.4 MHz
    // span and removes it. Moving 100 kHz has to leave it on 98.1 MHz, which
    // means its offset becomes -100 kHz. Before 2026-09-21 the offset stayed at
    // 0 and the receiver was dragged to 98.0 MHz with nothing said, which is
    // the behaviour an operator reported from broadcast FM.
    constexpr dsp::Hertz kOpenedAt = 98'100'000;
    constexpr dsp::Hertz kWanted = 98'000'000;
    auto landed = eng.set_source_center(kWanted);
    INFO(test::message_of(landed));
    REQUIRE(landed.has_value());

    const dsp::Hertz offset = landed->center - kWanted;
    INFO("asked " << kWanted << " Hz, landed " << landed->center << " Hz, offset " << offset);
    CHECK(std::abs(offset) <= kWanted / 10'000);

    // EngineInfo follows the tune, which is what every axis label and every
    // detection is derived from.
    CHECK(eng.info().source_center == landed->center);

    REQUIRE(eng.vrx_ids().size() == 1);
    CHECK(landed->removed.empty());
    auto held = eng.vrx_status(*receiver);
    REQUIRE(held.has_value());
    const dsp::Hertz absolute = eng.info().source_center + held->params.center;
    INFO("receiver offset " << held->params.center << " at centre "
                            << eng.info().source_center << ", so absolute " << absolute);
    CHECK(absolute == kOpenedAt);

    // WHAT THE RETUNE COST THE RECEIVER, which is the one thing only a radio
    // can show: retune_streaming_locked stops the transfers to move the tuner,
    // the stream index jumps by the gap, and a receiver whose channel samples
    // went with it restarts rather than filtering samples that no longer
    // exist. How long that takes is the dongle's business, so whether the gap
    // clears this engine's channel ring is not something to assert on.
    //
    // What is asserted is the one combination that cannot be honest: frames
    // reported lost with no event to account for them. The reverse is legal, a
    // restart that resumed on the same audio frame it left, so it is not
    // checked. tests/engine/test_vrx_reanchor.cpp provokes the branch
    // deliberately and pins the arithmetic.
    INFO("re-anchors after the nudge: " << held->reanchors << ", skipping "
                                        << held->reanchor_frames_skipped << " frames; source lost "
                                        << eng.source_stats().samples_lost << " samples");
    CHECK((held->reanchor_frames_skipped == 0 || held->reanchors > 0));

    // AND IT IS STILL AUDIBLE, which a surviving receiver id does not prove: a
    // receiver that is registered and silent is a receiver the operator has
    // lost, and that is exactly the shape the retune bug took before the
    // delivery thread was kept alive across a pause.
    const std::uint64_t audio_before_jump = audio_chunks.load(std::memory_order_relaxed);
    const auto samples_before_jump = eng.source_stats().samples_delivered;
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Samples first, because it says which half broke. Samples still arriving
    // with no audio is the graph or the receiver; samples stopped is the source.
    INFO("samples after the nudge: " << samples_before_jump << ", then "
                                     << eng.source_stats().samples_delivered);
    CHECK(eng.source_stats().samples_delivered > samples_before_jump);
    INFO("audio chunks after the nudge: " << audio_before_jump << ", then "
                                          << audio_chunks.load(std::memory_order_relaxed));
    CHECK(audio_chunks.load(std::memory_order_relaxed) > audio_before_jump);

    // AND A JUMP IT DOES NOT SURVIVE. 435 MHz is 337 MHz away and the span is
    // 2.4 MHz wide, so the receiver's centre is nowhere near reachable and it
    // is removed rather than carried along or clamped to the edge. The
    // operator's words for what they wanted were "disappearing VRX".
    auto uhf = eng.set_source_center(435'000'000);
    INFO(test::message_of(uhf));
    REQUIRE(uhf.has_value());
    CHECK(eng.info().source_center == uhf->center);
    CHECK(eng.vrx_ids().empty());
    CHECK_FALSE(eng.vrx_status(*receiver).has_value());

    // AND THE ANSWER SAYS SO, which is what lets the RPC server end the
    // receiver's subscribers with a reason instead of leaving them quiet. The
    // frequency is where the receiver was, not where the front end went.
    REQUIRE(uhf->removed.size() == 1);
    CHECK(uhf->removed.front().id == *receiver);
    CHECK(uhf->removed.front().frequency == kOpenedAt);

    // THE RETUNE ITSELF STILL SUCCEEDED AND THE ENGINE IS STILL SERVING. A
    // receiver that could not come along is not a failed retune, and an engine
    // that stopped is the failure this case was written for in the first place.
    auto back = eng.set_source_center(98'100'000);
    INFO(test::message_of(back));
    REQUIRE(back.has_value());
    CHECK(eng.info().source_center == back->center);
    CHECK(eng.running());

    // AND THE SOURCE IS STILL DELIVERING with no receiver left on it, which is
    // the state a removal leaves behind and the one the engine has to keep
    // serving: the spectrum, the waterfall and the detector all run without a
    // receiver, so an engine that stopped when its last receiver went would
    // take the operator's whole display with it.
    const auto samples_at_end = eng.source_stats().samples_delivered;
    std::this_thread::sleep_for(std::chrono::seconds(1));
    INFO("samples after the last retune: " << samples_at_end << ", then "
                                           << eng.source_stats().samples_delivered);
    CHECK(eng.source_stats().samples_delivered > samples_at_end);
    CHECK(eng.vrx_ids().empty());

    REQUIRE(eng.stop().has_value());
    second.join();
}
