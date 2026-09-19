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
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "core/engine/engine.h"
#include "core/engine/vrx.h"
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
