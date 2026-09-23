// The per-receiver passband spectrum, end to end on a real device.
//
// test_engine_spectrum.cpp covers the full-span chain. This is the other
// transform: one receiver's own display stream, which docs/ui-spectrum.md
// calls the fine-tuning display and the thing that makes parking a filter on
// a signal precise rather than approximate.
//
// WHAT THIS FILE USED TO SAY IT TRANSFORMED: "one receiver's own fine
// stream, at the demodulation rate". The fine stream is after the receiver's
// filter, so the pane showed the filter's shape where the neighbourhood
// should have been; the display stream is the same channel mixed the same
// way with no receiver filter in it. core/dsp/vrx_reference.h, "The display
// tap", and the neighbourhood case at the end of this file.
//
// WHAT IS NEW HERE, AND THEREFORE WHAT THESE CASES ARE FOR
//
// The kernel is not new. The passband runs core/shaders/spectrum.comp,
// specialized at two channels instead of sixty-four and dispatched one
// workgroup wide, and tests/reference/test_spectrum.cpp already diffs that
// kernel bit for bit against dsp::reference_spectrum. Nothing below re-proves
// a butterfly.
//
// WHAT THIS PARAGRAPH USED TO SAY
//
// Until 2026-09-20 it put that diff "on both devices in the conformance
// matrix". It has never run on both. Every GPU case in test_spectrum.cpp
// opens with REVENANT_NEEDS_REPRODUCIBLE_SHARED_MEMORY(), and the probe
// behind it in tests/reference/gpu_fixture.cpp returns before ever setting
// reproducible on a device that is not VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
// so the matrix's second leg skips all of them with a reason rather than
// refereeing them. The fixture states the same thing in its own words: of the
// two devices in the matrix, this kernel is refereed on one.
//
// Nothing is missing from the suite. That skip is deliberate, it is measured,
// and docs/fft.md carries the reproduction. What was missing was the accuracy
// of this sentence, and it matters here rather than only there: this is the
// sentence that says why the cases below do not re-prove the transform, so a
// reader deciding what they still owe needs the coverage number to be right.
//
// The specialization count was wrong in the same sentence. core/engine/
// graph.cpp specializes the passband pipeline at TWO channels, because two is
// the floor that core/shaders/spectrum.comp's slot arithmetic and
// dsp::validate(SpectrumParams) both accept, and it dispatches one workgroup
// so the second channel is never entered. The last case in this file already
// said two, so the file contradicted itself.
//
// Three things ARE new, and each one fails quietly:
//
//   The display tap. A second specialization of the fine kernel per
//   receiver, refereed bit for bit in tests/reference/test_vrx.cpp, and a
//   ring the graph transforms. What this file checks is that the pane shows
//   the air around the filter: a carrier outside the receiver's passband
//   reads at the same level as one inside it, and white noise reads flat.
//
//   The frequency axis. It is the display stream's and not the receiver's
//   request: bin zero sits a quarter of the display rate below whatever the
//   fine stage mixed to DC, which for CW is one audio pitch away from
//   VrxParams::center. An axis a fixed offset out draws a correct spectrum
//   under wrong labels, which is worse than no spectrum.
//
//   The per-receiver plumbing. A view, a colour-map scale, a sequence and a
//   readback per receiver rather than one of each for the engine, allocated
//   when a sink is attached rather than when the receiver is added.
//
// So the cases that carry the weight put a generated emitter at a frequency
// this file chose, park several receivers at chosen offsets from it, and
// check each passband puts the energy where the scene's own truth record
// says it is. Two of those offsets put the carrier outside the receiver's
// own filter, where the fine stream would have attenuated it by 80 dB.
//
// WHAT THE FIRST OF THOSE THREE USED TO BE: "The two-pass reconstruction",
// a second pass of the spectrum kernel under a window with its odd taps
// negated, reassembled with three copy regions, because the pane then
// needed every bin of a transform of the fine stream. The display stream is
// oversampled by two on purpose, so one pass and its central half is the
// whole pane.
//
// AND SIGNALS THAT START AND STOP
//
// The synthetic scene's bursty emitters carry a start_sample and an
// end_sample per burst, and PassbandFrame carries the source samples its own
// window covers. That makes "the display went bright when the transmission
// started" a number rather than a look at a picture: every frame is inside a
// burst, outside every burst, or straddling an edge, and the first two
// groups have to be separated by a wide margin in the frame's own decibels.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <limits>
#include <mutex>
#include <numbers>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/dsp/pfb.h"
#include "core/dsp/spectrum_reference.h"
#include "core/dsp/synth/wideband.h"
#include "core/engine/engine.h"
#include "core/engine/spectrum_scale.h"
#include "core/source/device_lock.h"
#include "core/source/rtlsdr_source.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/support/dongle_lock.h"
#include "tests/support/temp_path.h"

using namespace revenant;
using Catch::Approx;

namespace {

constexpr dsp::SampleRate kSourceRate = 2'400'000;
constexpr std::uint32_t kChannels = 64;

// A 512-point transform keeps 256 bins. Both devices in the conformance
// matrix hold a 512-point complex transform in one workgroup's shared memory
// many times over; the engine clamps to what the device offers and reports
// the clamp, and the geometry case checks that no clamp happened, because
// one would mean this number was chosen for a machine that is no longer the
// one being tested.
constexpr std::uint32_t kPassbandTransform = 512;
constexpr std::uint32_t kPassbandBins = kPassbandTransform / 2;

// Small on purpose, as in test_engine_spectrum.cpp: 8192 source samples is
// 256 coarse blocks at D = 32, which is about 164 fine samples at 48 kHz, so
// the window gate is exercised for the first few dispatches instead of being
// satisfied by the very first one.
constexpr std::size_t kBlockSamples = 8'192;

// THE GEOMETRY THE CARRIER CASE PUTS A SIGNAL OUTSIDE THE FILTER WITH
//
// A 6 kHz NFM receiver's further edge is 3 kHz from its centre, so the
// display rule wants a pane at least 12 kHz wide: Fdisp of 24 kHz or more.
// The 75 kS/s channel's ladder is 1, 2, 4, 8, 20, and the largest rung that
// gives that is R = 2, so the display runs at 37.5 kS/s and the pane is
// plus and minus 9375 Hz. An offset of 7 kHz is then 4 kHz outside the
// receiver's own edge and 2.4 kHz inside the pane's.
//
// WHAT THIS PARAGRAPH USED TO WORK OUT: how to reach the OUTER QUARTER of a
// transform of the fine stream, which needed a 16 kHz receiver so that the
// filter passed further out than Fd/4, "a twelfth of the band and nothing
// wider". A signal outside the receiver's filter could not be found at all
// then, which is what this file now checks it can.
//
// A SECOND CONSTRAINT, AND IT IS THE ONE THAT CAUGHT THIS FILE OUT
//
// max_channel_bandwidth is Fc - 2|residual|, which is what fits in the
// channel's Nyquist. It is NOT what fits in the prototype's clean band: the
// prototype's cutoff is half a channel SPACING from the channel centre, so a
// receiver is only looking at real signal while |residual| + B/2 stays under
// spacing/2. Past that the coarse filter is rolling off across the
// receiver's own passband, and a carrier out there comes back attenuated
// enough that a sideband beats it and the measured peak is a kilohertz and a
// half from the carrier. Nothing in the engine is wrong when that happens
// and nothing reports it, so the cases below check it themselves.
constexpr dsp::SampleRate kAudioRate = 8'000;
constexpr dsp::Hertz kWideBandwidth = 16'000;
constexpr dsp::Hertz kNarrowBandwidth = 6'000;
constexpr dsp::SampleRate kExpectedDisplayRate = 37'500;
constexpr dsp::Hertz kOuterOffset = 7'000;

constexpr dsp::Hertz kChannelSpacing = kSourceRate / kChannels;

// A coarse channel centre to hang the scene on, so that receivers placed a
// few kilohertz either side of the emitter all sit near the middle of one
// channel rather than out on its skirt.
constexpr dsp::Hertz kSceneCentre = kChannelSpacing * 5;

// How far the scene may put the emitter from where it was asked for. The
// placement window has to be at least the emitter's occupied bandwidth wide
// or nothing fits in it, and a 3 kHz AM signal is refused anything narrower
// than about 3.2 kHz.
constexpr dsp::Hertz kSceneSlack = 1'600;

engine::EngineConfig passband_config() {
    engine::EngineConfig config;
    config.channels = kChannels;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = kBlockSamples;
    config.audio_rate = kAudioRate;
    config.passband_transform = kPassbandTransform;
    config.gpu_index = -1;  // honours REVENANT_GPU_INDEX
    return config;
}

// How far a receiver tuned here sits from its coarse channel's centre.
//
// Exact rather than approximate: 2.4 MS/s over 64 channels is 37500 Hz
// exactly, so the nearest centre is a rounded multiple and there is no
// rational to carry. A rate that did not divide would need
// dsp::channel_centre and its numerator and denominator.
[[nodiscard]] dsp::Hertz residual_of(dsp::Hertz centre) {
    static_assert(kSourceRate % kChannels == 0,
                  "this file's residual arithmetic assumes a whole-hertz channel spacing");
    const auto channel = static_cast<dsp::Hertz>(
        std::llround(static_cast<double>(centre) / static_cast<double>(kChannelSpacing)));
    return centre - channel * kChannelSpacing;
}

// Whether a receiver tuned here is looking at its coarse channel's clean
// band rather than at the prototype's roll-off. See the note above.
[[nodiscard]] bool inside_clean_band(dsp::Hertz centre, dsp::Hertz bandwidth) {
    return std::abs(residual_of(centre)) + bandwidth / 2 <= kChannelSpacing / 2;
}

// One synthetic scene, described once and built twice: as a URI for the
// engine to open, and as a siggen::Scene here so this file can read the truth
// record the engine never sees.
//
// Every field scene_spec_of() in core/source/registry.cpp reads is set
// explicitly, so a default moving on either side cannot make the two scenes
// differ. If they ever do differ anyway the cases below fail loudly rather
// than passing on a coincidence, because every assertion is against a carrier
// frequency or a burst edge that came from this side.
struct SceneRequest {
    dsp::Hertz span_low = 0;
    dsp::Hertz span_high = 0;
    std::uint64_t seed = 424'242;
    std::size_t emitters = 1;
    std::size_t bursts = 1;
    dsp::SampleIndex samples = 600'000;
    double snr_db = 60.0;
    double noise_dbfs = -120.0;
    double min_burst_seconds = 0.05;
    double max_burst_seconds = 2.0;

    // A silent scene, for the cost case at the end of this file. The
    // generator is single threaded and a populated scene at 20 MS/s costs
    // about four hundred times a silent one, which is enough to make a
    // timing run report the generator's rate rather than the engine's.
    // tools/bench/throughput.h has the measured pair.
    dsp::SampleRate rate = kSourceRate;
    bool noise = true;
};

std::string scene_uri(const SceneRequest& request) {
    return std::format(
        "synthetic:wideband?rate={}&emitters={}&bursts={}&seed={}&samples={}&modes=am"
        "&noise={}&noise_dbfs={}&snr_min={}&snr_max={}&span_low={}&span_high={}&min_burst={}"
        "&max_burst={}",
        request.rate, request.emitters, request.bursts, request.seed, request.samples,
        request.noise ? "on" : "off", request.noise_dbfs, request.snr_db, request.snr_db,
        request.span_low, request.span_high, request.min_burst_seconds,
        request.max_burst_seconds);
}

Expected<siggen::Scene> scene_of(const SceneRequest& request) {
    siggen::SceneSpec spec;
    spec.rate = request.rate;
    spec.center_hz = 0;
    spec.duration_samples = request.samples;
    spec.epoch_anchor_ns = 0;
    spec.seed = request.seed;
    spec.add_noise = request.noise;
    spec.noise_power_full_band_dbfs = request.noise_dbfs;
    spec.worker_threads = 1;
    spec.random.emitter_count = request.emitters;
    spec.random.bursts_per_emitter = request.bursts;
    spec.random.span_low_hz = request.span_low;
    spec.random.span_high_hz = request.span_high;
    spec.random.snr_in_occupied_bandwidth_db_min = request.snr_db;
    spec.random.snr_in_occupied_bandwidth_db_max = request.snr_db;
    spec.random.min_burst_seconds = request.min_burst_seconds;
    spec.random.max_burst_seconds = request.max_burst_seconds;
    spec.random.palette = {siggen::Modulation::Am};
    return siggen::Scene::create(spec);
}

// One frame, reduced. PassbandFrame::power_db is valid for the call and not
// after, which is a contract these cases also check by obeying it.
struct CapturedFrame {
    engine::VrxId vrx;
    engine::PassbandGeometry geometry;
    dsp::SampleIndex start = 0;
    dsp::SampleIndex count = 0;
    std::uint64_t sequence = 0;
    float floor_db = 0.0F;
    float ceiling_db = 0.0F;
    float percentile_low_db = 0.0F;
    float percentile_high_db = 0.0F;

    std::size_t peak_bin = 0;
    float peak_db = 0.0F;
    std::size_t non_finite = 0;
    std::size_t bins_seen = 0;

    [[nodiscard]] double peak_hz() const {
        return geometry.bin_zero_hz() +
               static_cast<double>(peak_bin) * geometry.bin_width_hz();
    }
};

CapturedFrame reduce(const engine::PassbandFrame& frame) {
    CapturedFrame out;
    out.vrx = frame.vrx;
    out.geometry = frame.geometry;
    out.start = frame.start;
    out.count = frame.count;
    out.sequence = frame.sequence;
    out.floor_db = frame.floor_db;
    out.ceiling_db = frame.ceiling_db;
    out.percentile_low_db = frame.percentile_low_db;
    out.percentile_high_db = frame.percentile_high_db;
    out.bins_seen = frame.power_db.size();

    float best = -std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < frame.power_db.size(); ++i) {
        const float value = frame.power_db[i];
        if (!std::isfinite(value)) {
            ++out.non_finite;
            continue;
        }
        if (value > best) {
            best = value;
            out.peak_bin = i;
        }
    }
    out.peak_db = best;
    return out;
}

// A receiver to park, as an offset from the emitter's carrier. A positive
// offset puts the RECEIVER above the carrier, so the carrier lands at minus
// that offset in the receiver's own passband.
struct Parked {
    dsp::Hertz receiver_above_carrier = 0;
    engine::Demod demod = engine::Demod::Nfm;
    dsp::Hertz bandwidth = kWideBandwidth;
    dsp::Hertz cw_pitch = 700;
};

struct Capture {
    Status run_status;
    engine::EngineInfo info{};
    std::vector<engine::VrxId> ids;
    std::vector<engine::VrxStatus> settled;
    std::vector<CapturedFrame> frames;
};

// Runs one capture with a receiver per entry in `parked`, each with its own
// passband sink, and hands back every frame any of them delivered.
//
// The mutex is declared before the engine so that it is destroyed after it.
// Locals are destroyed in reverse order and the sinks hold a reference to
// this one, so the other order leaves a window where the engine is still
// being torn down with a dead mutex behind its callbacks.
Capture capture_passbands(const engine::EngineConfig& config, const std::string& uri,
                          dsp::Hertz carrier_hz, const std::vector<Parked>& parked) {
    Capture out;
    std::mutex lock;

    auto created = engine::Engine::create(config);
    if (!created) {
        out.run_status = std::unexpected(created.error());
        return out;
    }
    auto& eng = **created;

    if (auto opened = eng.open_source(uri); !opened) {
        out.run_status = opened;
        return out;
    }
    out.info = eng.info();

    for (const Parked& want : parked) {
        engine::VrxParams params;
        params.center = carrier_hz + want.receiver_above_carrier;
        params.bandwidth = want.bandwidth;
        params.demod = want.demod;
        params.cw_pitch = want.cw_pitch;

        // Off, because a squelched receiver still has a passband and this
        // file would rather not depend on that being true to see a frame.
        // The case that checks it is the burst one, where the signal really
        // does go away.
        params.squelch_dbfs = -300.0;

        auto added = eng.add_vrx(params);
        if (!added) {
            out.run_status = std::unexpected(added.error());
            return out;
        }
        out.ids.push_back(*added);

        if (auto attached =
                eng.set_passband_sink(*added, [&](const engine::PassbandFrame& frame) -> Status {
                    const std::lock_guard<std::mutex> guard(lock);
                    out.frames.push_back(reduce(frame));
                    return {};
                });
            !attached) {
            out.run_status = attached;
            return out;
        }
    }

    out.run_status = eng.run();

    for (const engine::VrxId id : out.ids) {
        if (auto status = eng.vrx_status(id)) {
            out.settled.push_back(*status);
        }
    }
    return out;
}

// The bin most of one receiver's frames agree on, and how many agreed.
//
// A vote rather than one frame's argmax, for the reason
// test_engine_spectrum.cpp states: docs/fft.md has the integrated Radeon
// corrupting isolated spectrum dispatches with correct answers either side,
// so a case that reads one frame fails for a reason that is written down and
// is not the reason the case exists.
struct Vote {
    std::size_t bin = 0;
    std::size_t agreed = 0;
    std::size_t total = 0;

    [[nodiscard]] double agreement() const {
        return total == 0 ? 0.0 : static_cast<double>(agreed) / static_cast<double>(total);
    }
};

Vote vote_for(const std::vector<CapturedFrame>& frames, engine::VrxId id) {
    std::unordered_map<std::size_t, std::size_t> votes;
    Vote out;
    for (const CapturedFrame& frame : frames) {
        if (frame.vrx != id) {
            continue;
        }
        ++out.total;
        ++votes[frame.peak_bin];
    }
    for (const auto& [bin, count] : votes) {
        if (count > out.agreed) {
            out.agreed = count;
            out.bin = bin;
        }
    }
    return out;
}

}  // namespace

TEST_CASE("an engine built with no passband stage refuses a sink and says why",
          "[gpu][engine][passband][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The stage sizes every receiver's fine ring, and a ring cannot be grown
    // while a command buffer names it, so "I forgot to set
    // passband_transform" is a mistake a caller makes once, at a point where
    // nothing has gone wrong yet. The refusal has to name the field rather
    // than the symptom.
    auto config = passband_config();
    config.passband_transform = 0;

    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;

    const auto early =
        eng.set_passband_sink(engine::VrxId{1}, [](const engine::PassbandFrame&) -> Status {
            return {};
        });
    REQUIRE_FALSE(early.has_value());
    INFO(early.error().message);
    CHECK(early.error().message.find("source") != std::string::npos);

    const SceneRequest scene{.span_low = kSceneCentre - kSceneSlack,
                             .span_high = kSceneCentre + kSceneSlack,
                             .samples = 100'000};
    REQUIRE(eng.open_source(scene_uri(scene)).has_value());
    CHECK(eng.info().passband_transform == 0);

    engine::VrxParams params;
    params.center = 0;
    params.bandwidth = kWideBandwidth;
    params.demod = engine::Demod::Nfm;
    auto added = eng.add_vrx(params);
    INFO(test::message_of(added));
    REQUIRE(added.has_value());

    const auto refused =
        eng.set_passband_sink(*added, [](const engine::PassbandFrame&) -> Status { return {}; });
    REQUIRE_FALSE(refused.has_value());
    INFO(refused.error().message);
    CHECK(refused.error().message.find("passband_transform") != std::string::npos);
}

TEST_CASE("a raw tap has no fine stream and the refusal says so",
          "[gpu][engine][passband][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The graph's raw tap is a vkCmdCopyBuffer out of the coarse channel
    // ring: no mixer, no filter, no per-receiver baseband on the device to
    // transform. Refusing is the honest answer and the message has to say
    // which of the two reasons it is, because "no passband for this
    // receiver" and "no passband on this engine" are different mistakes.
    auto created = engine::Engine::create(passband_config());
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;

    const SceneRequest scene{.span_low = kSceneCentre - kSceneSlack,
                             .span_high = kSceneCentre + kSceneSlack,
                             .samples = 100'000};
    REQUIRE(eng.open_source(scene_uri(scene)).has_value());
    CHECK(eng.info().passband_transform == kPassbandTransform);

    engine::VrxParams params;
    params.center = 0;
    params.bandwidth = kWideBandwidth;
    params.demod = engine::Demod::Raw;
    auto added = eng.add_vrx(params);
    INFO(test::message_of(added));
    REQUIRE(added.has_value());

    const auto refused =
        eng.set_passband_sink(*added, [](const engine::PassbandFrame&) -> Status { return {}; });
    REQUIRE_FALSE(refused.has_value());
    INFO(refused.error().message);
    CHECK(refused.error().message.find("fine stream") != std::string::npos);

    // And an unknown receiver is a different refusal again.
    const auto unknown =
        eng.set_passband_sink(engine::VrxId{9999},
                              [](const engine::PassbandFrame&) -> Status { return {}; });
    REQUIRE_FALSE(unknown.has_value());
    INFO(unknown.error().message);
    CHECK(unknown.error().message.find("9999") != std::string::npos);
}

TEST_CASE("a carrier lands where the passband's own axis says, inside and outside the filter",
          "[gpu][engine][passband][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The case the rest of the file is scaffolding for.
    //
    // Four 6 kHz receivers on one emitter, at four offsets:
    //
    //   +7 kHz  the receiver sits above the carrier, so the carrier is at
    //           -7 kHz, 4 kHz OUTSIDE the receiver's lower edge.
    //   +2 kHz  carrier at -2 kHz, inside the passband.
    //   -2 kHz  carrier at +2 kHz, inside the passband.
    //   -7 kHz  carrier at +7 kHz, 4 kHz outside the upper edge.
    //
    // The two outside offsets are where the fine stage's filter is 80 dB
    // down. A pane that still transformed the fine stream would report the
    // peak somewhere else in the frame, or report it tens of decibels below
    // the inside ones; the display stream reports it at the same level.
    //
    // WHAT THIS CASE USED TO BE: four 16 kHz receivers at the same offsets,
    // all four carriers inside their filters, chosen so the carrier landed
    // in each of the three regions a two-pass reassembly built the frame
    // from.
    const SceneRequest scene{.span_low = kSceneCentre - kSceneSlack,
                             .span_high = kSceneCentre + kSceneSlack,
                             .samples = 700'000};

    auto built = scene_of(scene);
    INFO(test::message_of(built));
    REQUIRE(built.has_value());
    REQUIRE(built->truth().size() == 1);
    const dsp::Hertz carrier = built->truth().front().carrier_offset_hz;
    INFO(std::format("the scene put its carrier at {} Hz, {} Hz from the channel centre it was "
                     "aimed at",
                     carrier, carrier - kSceneCentre));

    const std::vector<Parked> parked{
        {.receiver_above_carrier = kOuterOffset, .bandwidth = kNarrowBandwidth},
        {.receiver_above_carrier = 2'000, .bandwidth = kNarrowBandwidth},
        {.receiver_above_carrier = -2'000, .bandwidth = kNarrowBandwidth},
        {.receiver_above_carrier = -kOuterOffset, .bandwidth = kNarrowBandwidth},
    };

    // Every one of them has to be looking at its channel's clean band, or
    // what comes back is the coarse prototype's roll-off and the peak is not
    // the carrier. This is a statement about the scene and the offsets above
    // rather than about the engine, so it fails here rather than downstream.
    for (const Parked& want : parked) {
        const dsp::Hertz centre = carrier + want.receiver_above_carrier;
        INFO(std::format("a receiver at {} Hz sits {} Hz from its channel centre with a {} Hz "
                         "bandwidth, against a half spacing of {} Hz",
                         centre, residual_of(centre), want.bandwidth, kChannelSpacing / 2));
        REQUIRE(inside_clean_band(centre, want.bandwidth));
    }

    const auto captured =
        capture_passbands(passband_config(), scene_uri(scene), carrier, parked);
    INFO(test::message_of(captured.run_status));
    REQUIRE(captured.run_status.has_value());
    REQUIRE(captured.ids.size() == parked.size());
    REQUIRE(captured.frames.size() > 4 * 8);
    REQUIRE(captured.settled.size() == parked.size());

    // Nobody's bandwidth was cut down to fit its channel. A clamp would move
    // the edge the two outside offsets are measured against.
    for (const engine::VrxStatus& status : captured.settled) {
        INFO(std::format("receiver {} asked for {} Hz on channel {}", status.id.value,
                         status.params.bandwidth, status.placement.channel));
        CHECK_FALSE(status.placement.bandwidth_clamped);
    }

    std::size_t total_non_finite = 0;
    for (const CapturedFrame& frame : captured.frames) {
        total_non_finite += frame.non_finite;
    }
    INFO(std::format("{} non-finite bins across {} frames", total_non_finite,
                     captured.frames.size()));
    CHECK(total_non_finite == 0);

    // The mean peak level each receiver saw, for the comparison after the
    // loop between carriers inside and outside the filter.
    std::vector<double> mean_peak_db(parked.size(), 0.0);

    for (std::size_t i = 0; i < parked.size(); ++i) {
        const engine::VrxId id = captured.ids[i];
        const dsp::Hertz offset = parked[i].receiver_above_carrier;
        const Vote vote = vote_for(captured.frames, id);

        INFO(std::format("receiver {} parked {} Hz above the carrier", id.value, offset));
        REQUIRE(vote.total > 8);

        // Every frame this receiver delivered carries the same axis, so any
        // one of them will do for the geometry.
        engine::PassbandGeometry geometry{};
        double peak_sum = 0.0;
        std::size_t peak_count = 0;
        for (const CapturedFrame& frame : captured.frames) {
            if (frame.vrx == id) {
                geometry = frame.geometry;
                peak_sum += static_cast<double>(frame.peak_db);
                ++peak_count;
            }
        }
        mean_peak_db[i] = peak_sum / static_cast<double>(peak_count);

        INFO(std::format("{} bins of {} Hz from {} Hz, at a display rate of {}",
                         geometry.bins, geometry.bin_width_hz(), geometry.bin_zero_hz(),
                         geometry.rate));

        CHECK(geometry.enabled());
        CHECK(geometry.transform == kPassbandTransform);
        CHECK(geometry.bins == kPassbandBins);
        CHECK(geometry.rate == kExpectedDisplayRate);

        // The frame is half the display rate wide: the central half of the
        // transform, which is the part of the display stream that is flat
        // and free of aliases.
        const double span = static_cast<double>(geometry.bins) * geometry.bin_width_hz();
        CHECK(span == Approx(0.5 * static_cast<double>(kExpectedDisplayRate)));

        // And the axis is centred on the receiver, which for every mode but
        // CW is where the fine stage mixed to DC.
        const double centre = geometry.bin_zero_hz() + 0.5 * span;
        CHECK(centre == Approx(static_cast<double>(carrier + offset)).margin(1.0));

        // Where the carrier actually landed, read off the frame's own axis.
        const double peak_hz = geometry.bin_zero_hz() +
                               static_cast<double>(vote.bin) * geometry.bin_width_hz();

        INFO(std::format(
            "{} of {} frames peaked in bin {} of {}, which the axis puts at {} Hz against a "
            "carrier at {} Hz",
            vote.agreed, vote.total, vote.bin, geometry.bins, peak_hz, carrier));

        // Three bins. The carrier is exactly where the truth record says and
        // the receiver is tuned in whole hertz, so the only slack is half a
        // bin of quantisation plus the window's own spread. The failures
        // this is aimed at are not close: a mirrored axis misses by twice
        // the offset, and an axis centred on the channel rather than on the
        // receiver by up to half a channel spacing.
        CHECK(std::abs(peak_hz - static_cast<double>(carrier)) <
              3.0 * geometry.bin_width_hz());

        // Most frames agree. Deliberately not all of them; see Vote.
        CHECK(vote.agreement() > 0.5);
    }

    // The same carrier, seen from inside two receivers' filters and from
    // outside two others'. With no receiver filter in the display stream the
    // four read the same level to within the window's scalloping, which for
    // a carrier landing anywhere in a bin is under a decibel and a half. A
    // pane of the fine stream put the two outside ones 80 dB down.
    const double inside_db = 0.5 * (mean_peak_db[1] + mean_peak_db[2]);
    const double outside_db = 0.5 * (mean_peak_db[0] + mean_peak_db[3]);
    INFO(std::format("the carrier reads {:.2f} dB from inside the filter and {:.2f} dB from 4 kHz "
                     "outside it",
                     inside_db, outside_db));
    CHECK(std::abs(inside_db - outside_db) < 1.5);
}

TEST_CASE("a passband frame's window and sequence place it in the stream",
          "[gpu][engine][passband][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    const SceneRequest scene{.span_low = kSceneCentre - kSceneSlack,
                             .span_high = kSceneCentre + kSceneSlack,
                             .samples = 500'000};

    auto built = scene_of(scene);
    INFO(test::message_of(built));
    REQUIRE(built.has_value());
    REQUIRE(built->truth().size() == 1);

    const auto captured = capture_passbands(passband_config(), scene_uri(scene),
                                            built->truth().front().carrier_offset_hz,
                                            {{.receiver_above_carrier = 0}});
    INFO(test::message_of(captured.run_status));
    REQUIRE(captured.run_status.has_value());
    REQUIRE(captured.frames.size() > 8);

    // The window is N display samples, which for this 16 kHz receiver at a
    // display rate of 75 kS/s against a source at 2.4 MS/s is N * 32 source
    // samples. Checked against the frame's own rate rather than against 32,
    // so a receiver on a different rung is covered by the same relation.
    const auto& first = captured.frames.front();
    const dsp::SampleIndex expected_count =
        static_cast<dsp::SampleIndex>(kPassbandTransform) *
        static_cast<dsp::SampleIndex>(kSourceRate) /
        static_cast<dsp::SampleIndex>(first.geometry.rate);

    std::uint64_t expected_sequence = 0;
    dsp::SampleIndex previous_start = 0;
    bool seen = false;
    for (const CapturedFrame& frame : captured.frames) {
        INFO(std::format("frame {} covers [{}, {})", frame.sequence, frame.start,
                         frame.start + frame.count));

        // Counts from zero for this receiver and never skips. A receiver
        // attached an hour into a run has missed nothing, so its first frame
        // is sequence zero rather than the engine's block count.
        CHECK(frame.sequence == expected_sequence);
        ++expected_sequence;

        CHECK(frame.bins_seen == kPassbandBins);
        CHECK(frame.count == expected_count);

        // Inside the stream. The start is the window's first fine sample
        // mapped back through both group delays, and that subtraction is in
        // an unsigned type, so a window allowed to begin below the delay
        // would report a start near 2^64.
        CHECK(frame.start < scene.samples);

        // And the window ENDS inside the stream, which is the half that says
        // anything: the frame covers samples the source produced rather than
        // reaching past the last one.
        //
        // WHAT THIS LINE USED TO BE: `frame.start + frame.count <
        // scene.samples + expected_count`. Take the two assertions above it,
        // start < scene.samples and count == expected_count, and that is
        // their sum. It could not fail while they passed, so it certified the
        // two lines above it and nothing about the frame.
        CHECK(frame.start + frame.count <= scene.samples);

        // Monotonic. Each dispatch transforms the newest window, so the
        // windows overlap and advance rather than repeating.
        if (seen) {
            CHECK(frame.start > previous_start);
        }
        previous_start = frame.start;
        seen = true;

        // The colour map's ends bracket the frame and are at least the
        // minimum span apart, which every consumer relies on.
        CHECK(frame.ceiling_db > frame.floor_db);
        CHECK(frame.ceiling_db - frame.floor_db >=
              Approx(engine::kSpectrumMinimumSpanDb).margin(0.01));
        CHECK(frame.percentile_high_db >= frame.percentile_low_db);
    }
}

TEST_CASE("detaching and reattaching a passband keeps the row count going",
          "[gpu][engine][passband][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // An empty sink is the detach, and it frees the transform buffers. What
    // it must not free is where the display had got to: frames recorded
    // against the old buffers can still be in flight when the new ones
    // arrive, so a count that restarted with them would run backwards over
    // the handover and a waterfall would draw rows it had already drawn.
    const SceneRequest scene{.span_low = kSceneCentre - kSceneSlack,
                             .span_high = kSceneCentre + kSceneSlack,
                             .samples = 400'000};

    auto built = scene_of(scene);
    INFO(test::message_of(built));
    REQUIRE(built.has_value());
    REQUIRE(built->truth().size() == 1);

    std::mutex lock;
    std::vector<std::uint64_t> sequences;

    auto created = engine::Engine::create(passband_config());
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;
    REQUIRE(eng.open_source(scene_uri(scene)).has_value());

    engine::VrxParams params;
    params.center = built->truth().front().carrier_offset_hz;
    params.bandwidth = kWideBandwidth;
    params.demod = engine::Demod::Nfm;
    params.squelch_dbfs = -300.0;
    auto added = eng.add_vrx(params);
    INFO(test::message_of(added));
    REQUIRE(added.has_value());

    // Attached, detached and attached again before the run, so the second
    // view is the one that records and the first is torn down with nothing
    // in flight against it. The point being checked is the count, which the
    // slot owns and the view does not.
    const auto attach = [&] {
        return eng.set_passband_sink(*added, [&](const engine::PassbandFrame& frame) -> Status {
            const std::lock_guard<std::mutex> guard(lock);
            sequences.push_back(frame.sequence);
            return {};
        });
    };

    REQUIRE(attach().has_value());
    REQUIRE(eng.set_passband_sink(*added, {}).has_value());
    REQUIRE(attach().has_value());

    const Status ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    const std::lock_guard<std::mutex> guard(lock);
    INFO(std::format("{} frames delivered", sequences.size()));
    REQUIRE(sequences.size() > 4);
    for (std::size_t i = 0; i < sequences.size(); ++i) {
        CHECK(sequences[i] == i);
    }
}

TEST_CASE("a passband follows an emitter that starts and stops",
          "[gpu][engine][passband][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The whole point of scoring against a generated scene rather than
    // against a radio: the emitter's start and end samples are known
    // exactly, so "the display responded to the transmission" is a margin in
    // decibels and not an impression.
    //
    // Three bursts laid out one per third of the scene, each 0.1 to 0.15
    // seconds inside its own third, so there is dead air either side of
    // every one of them. Every frame is then inside a burst, outside all of
    // them, or straddling an edge, and the straddlers are thrown away
    // because what they contain is a genuine mixture rather than a
    // disagreement.
    const SceneRequest scene{
        .span_low = kSceneCentre - kSceneSlack,
        .span_high = kSceneCentre + kSceneSlack,
        .bursts = 3,
        .samples = 1'800'000,  // 0.75 s at 2.4 MS/s, so 0.25 s per burst window
        .min_burst_seconds = 0.10,
        .max_burst_seconds = 0.15,
    };

    auto built = scene_of(scene);
    INFO(test::message_of(built));
    REQUIRE(built.has_value());

    const auto& truth = built->truth();
    INFO(std::format("{} bursts in the truth record", truth.size()));
    REQUIRE(truth.size() == scene.bursts);

    const dsp::Hertz carrier = truth.front().carrier_offset_hz;
    for (const siggen::EmitterTruth& burst : truth) {
        INFO(std::format("burst {} runs [{}, {}) at {} Hz", burst.id, burst.start_sample,
                         burst.end_sample, burst.carrier_offset_hz));
        REQUIRE_FALSE(burst.open_ended());
        REQUIRE(burst.end_sample > burst.start_sample);

        // One frequency slot, so every burst is the same emitter keying up
        // again and one receiver hears all three.
        REQUIRE(burst.carrier_offset_hz == carrier);
    }

    const auto captured = capture_passbands(passband_config(), scene_uri(scene), carrier,
                                            {{.receiver_above_carrier = 0}});
    INFO(test::message_of(captured.run_status));
    REQUIRE(captured.run_status.has_value());
    REQUIRE(captured.frames.size() > 32);

    std::vector<float> inside;
    std::vector<float> outside;
    std::size_t straddling = 0;

    for (const CapturedFrame& frame : captured.frames) {
        const dsp::SampleIndex window_end = frame.start + frame.count;

        bool wholly_in = false;
        bool touches = false;
        for (const siggen::EmitterTruth& burst : truth) {
            const bool disjoint =
                window_end <= burst.start_sample || frame.start >= burst.end_sample;
            if (!disjoint) {
                touches = true;
            }
            if (frame.start >= burst.start_sample && window_end <= burst.end_sample) {
                wholly_in = true;
            }
        }

        if (wholly_in) {
            inside.push_back(frame.peak_db);
        } else if (!touches) {
            outside.push_back(frame.peak_db);
        } else {
            ++straddling;
        }
    }

    INFO(std::format("{} frames inside a burst, {} outside every burst, {} straddling an edge",
                     inside.size(), outside.size(), straddling));

    // Both groups have to be populated or the assertion below proves
    // nothing. A window is N*32 source samples, which is 16384 at this
    // geometry, so a 0.1 second burst holds several of them and so does the
    // gap after it. (N*50 and 25600, this used to say, while the window was
    // N samples of a 48 kS/s fine stream.)
    REQUIRE(inside.size() > 4);
    REQUIRE(outside.size() > 4);

    const auto mean_of = [](const std::vector<float>& values) {
        double sum = 0.0;
        for (const float value : values) {
            sum += static_cast<double>(value);
        }
        return sum / static_cast<double>(values.size());
    };

    const double loud = mean_of(inside);
    const double quiet = mean_of(outside);
    const float weakest_on = *std::min_element(inside.begin(), inside.end());
    const float loudest_off = *std::max_element(outside.begin(), outside.end());

    INFO(std::format(
        "on air the peak bin averages {} dB and off air {} dB; the weakest on-air frame is {} "
        "dB and the loudest off-air frame is {} dB",
        loud, quiet, weakest_on, loudest_off));

    // A 60 dB emitter against a -120 dBFS floor. Thirty decibels of
    // separation between the two means is a long way inside that and a long
    // way outside anything a stale readback, a frame of the ring's
    // leftovers, or a window straddling a skip would produce: all three of
    // those show the same thing whether the transmitter is on or off.
    CHECK(loud - quiet > 30.0);

    // And the separation is per frame rather than only on average, which is
    // what a squelch or a burst detector would need from this.
    CHECK(weakest_on > loudest_off);
}

TEST_CASE("one pass's central half puts a tone in the bin the passband axis names",
          "[engine][passband][m1]") {
    // No device. The identity the graph's frame rests on, refereed against
    // dsp::reference_spectrum, which is the bit-exact twin of the kernel the
    // graph dispatches: the pass keeps transform bins -N/4 to N/4, the graph
    // copies them to the frame in ascending order, and frame bin b holds
    // signed transform bin b - N/4, which is what passband_geometry_for's
    // bin zero of DC minus a quarter of the display rate says.
    //
    // WHAT THIS CASE USED TO BE: "two central halves under the two windows
    // reconstruct the whole band", the identity behind a second pass under
    // a window with its odd taps negated. The frame no longer needs the
    // outer quarters, because the display stream puts its own anti-alias
    // skirt there.
    constexpr std::uint32_t kTransform = 256;
    constexpr std::uint32_t kRingBlocks = 1024;

    auto twiddles = dsp::build_twiddles(kTransform);
    INFO(test::message_of(twiddles));
    REQUIRE(twiddles.has_value());

    // Built the way core/engine/graph.cpp builds it: the shipped window
    // followed by a correction half of unity, because a passband has no
    // channel shape to divide out.
    auto base = dsp::build_spectrum_window(kTransform);
    INFO(test::message_of(base));
    REQUIRE(base.has_value());

    std::vector<float> window = *base;
    for (std::uint32_t bin = 0; bin < kTransform / 2; ++bin) {
        window[kTransform + bin] = 1.0F;
    }

    // The kernel is specialized at two channels because that is the smallest
    // its own validate() accepts, and the graph dispatches only the first.
    // Channel zero therefore writes at slot one, which is the upper half of
    // the frame the twin fills, and the second channel is left at zero here
    // exactly as it is left unwritten there.
    dsp::SpectrumParams params;
    params.channels = 2;
    params.transform = kTransform;
    params.stages = dsp::fft_stages(kTransform);
    params.chan_blocks = kRingBlocks;
    params.chan_mask = kRingBlocks - 1;
    params.in_offset = 0;

    const std::size_t half = kTransform / 2;
    const std::size_t quarter = kTransform / 4;

    // Both ends of the kept half and points between. Integer bins, so the
    // answer is a single bin rather than a spread.
    for (const int tone_bin : {-64, -30, 0, 17, 63}) {
        INFO(std::format("a tone at transform bin {}", tone_bin));

        std::vector<dsp::Complex32> ring(2 * kRingBlocks, dsp::Complex32{});
        for (std::uint32_t n = 0; n < kRingBlocks; ++n) {
            const double phase = 2.0 * std::numbers::pi * static_cast<double>(tone_bin) *
                                 static_cast<double>(n) / static_cast<double>(kTransform);
            ring[n] = dsp::Complex32{static_cast<float>(std::cos(phase)),
                                     static_cast<float>(std::sin(phase))};
        }

        std::vector<float> pass(2 * half, 0.0F);
        REQUIRE(dsp::reference_spectrum(params, ring, *twiddles, window, pass).has_value());

        // The graph's one copy region, with the offsets record_passbands()
        // records it with.
        std::vector<float> frame(half, 0.0F);
        for (std::size_t j = 0; j < half; ++j) {
            frame[j] = pass[half + j];
        }

        // Ascending frame bin b holds signed transform bin b - N/4.
        const auto expected = static_cast<std::size_t>(tone_bin + static_cast<int>(quarter));

        std::size_t peak = 0;
        float best = -std::numeric_limits<float>::infinity();
        for (std::size_t b = 0; b < frame.size(); ++b) {
            REQUIRE(std::isfinite(frame[b]));
            if (frame[b] > best) {
                best = frame[b];
                peak = b;
            }
        }

        INFO(std::format("peak at bin {} of {}, expected {}, at {} dB", peak, frame.size(),
                         expected, best));
        CHECK(peak == expected);

        // A unit-amplitude tone on a bin centre reads 0 dB, because
        // build_spectrum_window normalises to a coherent gain of one and the
        // passband applies no correction on top of it. That is the whole
        // claim of the unity second half: anything else would show here as
        // an offset.
        CHECK(static_cast<double>(best) == Approx(0.0).margin(0.1));
    }
}

TEST_CASE("a passband over a real radio shows the station above the corners",
          "[.][gpu][engine][passband][rtlsdr][dongle]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // Hidden, because it needs a dongle and a band with something on it, and
    // because it is a confirmation rather than a measurement. Run it by
    // name.
    //
    //   revenant_engine_tests.exe "a passband over a real radio*" -s
    //
    // The generated scenes above are what proves the passband correct: they
    // carry a truth record and this does not. What a radio adds is that the
    // samples came off hardware through the convert kernel and the
    // channelizer rather than out of an arithmetic expression.
    //
    // WHAT THIS CASE USED TO ASSERT, AND WHY IT CANNOT NOW. That the bins
    // beyond the receiver's B/2 were "that filter's stopband and have to be
    // far below the ones inside it", so that "the display is showing where
    // the filter's edges are". That was the fault the display tap removed:
    // the pane no longer carries the receiver's filter at all, so what sits
    // beyond B/2 is whatever is on the air there. A broadcast station is
    // 200 kHz wide and this receiver is 120, so the corners of the pane are
    // the station's own outer sidebands and the old margin is not a
    // property of anything.
    //
    // What it reports instead, with no threshold, is the level inside the
    // receiver's band against the level in the corners, which on a strong
    // station is the station's own spectral shape.
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    // Sixteen channels rather than sixty-four, because at 2.4 MS/s a
    // 64-channel grid gives a 37.5 kHz spacing and the widest receiver that
    // stays inside one channel's clean band is narrower than FM broadcast.
    // At sixteen the spacing is 150 kHz and a 120 kHz receiver fits with
    // 15 kHz of room for the residual.
    constexpr dsp::Hertz kWfmBandwidth = 120'000;
    constexpr std::uint32_t kRadioTransform = 2'048;

    engine::EngineConfig config;
    config.channels = 16;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = 65'536;
    config.audio_rate = 48'000;
    config.passband_transform = kRadioTransform;
    config.gpu_index = -1;

    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;

    if (auto opened = eng.open_source("rtlsdr://0?freq=98.1M&rate=2400000&gain=20");
        !opened) {
        SKIP("the dongle could not be opened: " + opened.error().message);
    }

    // Baseband DC, which is where the dongle was tuned. The R820T puts a DC
    // spike there, so the bin at the exact centre is the receiver's own
    // artefact rather than the station; the margin below is measured over
    // the whole passband and is not sensitive to one bin.
    engine::VrxParams params;
    params.center = 0;
    params.bandwidth = kWfmBandwidth;
    params.demod = engine::Demod::Wfm;
    params.squelch_dbfs = -300.0;

    auto added = eng.add_vrx(params);
    INFO(test::message_of(added));
    REQUIRE(added.has_value());

    std::mutex lock;
    std::vector<double> in_band;
    std::vector<double> out_of_band;
    std::vector<CapturedFrame> kept;
    std::atomic<std::uint64_t> seen{0};

    REQUIRE(eng.set_passband_sink(
                   *added,
                   [&](const engine::PassbandFrame& frame) -> Status {
                       const std::lock_guard<std::mutex> guard(lock);

                       // Split at the fine filter's passband edge, read off
                       // the frame's own axis rather than assumed.
                       double inside = 0.0;
                       double outside = 0.0;
                       std::size_t inside_bins = 0;
                       std::size_t outside_bins = 0;
                       for (std::size_t bin = 0; bin < frame.power_db.size(); ++bin) {
                           const double hz = frame.geometry.bin_zero_hz() +
                                             static_cast<double>(bin) *
                                                 frame.geometry.bin_width_hz();
                           if (std::abs(hz) <= 0.5 * static_cast<double>(kWfmBandwidth)) {
                               inside += static_cast<double>(frame.power_db[bin]);
                               ++inside_bins;
                           } else if (std::abs(hz) >=
                                      0.85 * 0.25 * static_cast<double>(frame.geometry.rate)) {
                               // The far corners of the pane, which is a
                               // quarter of the display rate either side.
                               outside += static_cast<double>(frame.power_db[bin]);
                               ++outside_bins;
                           }
                       }
                       if (inside_bins > 0 && outside_bins > 0) {
                           in_band.push_back(inside / static_cast<double>(inside_bins));
                           out_of_band.push_back(outside / static_cast<double>(outside_bins));
                           kept.push_back(reduce(frame));
                       }
                       seen.fetch_add(1, std::memory_order_release);
                       return {};
                   })
                .has_value());

    // A radio never ends, so something has to stop the run. From another
    // thread and not from the sink: the sink is on the completion thread and
    // stop() waits on it.
    constexpr std::uint64_t kWantedFrames = 60;
    std::thread stopper([&] {
        while (seen.load(std::memory_order_acquire) < kWantedFrames) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        (void)eng.stop();
    });

    const Status ran = eng.run();
    stopper.join();
    INFO(test::message_of(ran));

    const std::lock_guard<std::mutex> guard(lock);
    INFO(std::format("{} frames delivered, {} of them measured", seen.load(), kept.size()));
    REQUIRE(kept.size() > 8);

    const auto mean_of = [](const std::vector<double>& values) {
        double sum = 0.0;
        for (const double value : values) {
            sum += value;
        }
        return sum / static_cast<double>(values.size());
    };

    const double inside = mean_of(in_band);
    const double outside = mean_of(out_of_band);

    INFO(std::format("{} bins across {} Hz at {} Hz each; inside the {} Hz passband the mean "
                     "bin is {:.1f} dB and out in the corners it is {:.1f} dB",
                     kept.front().geometry.bins,
                     static_cast<double>(kept.front().geometry.bins) *
                         kept.front().geometry.bin_width_hz(),
                     kept.front().geometry.bin_width_hz(), kWfmBandwidth, inside, outside));

    CHECK(kept.front().geometry.bins == kRadioTransform / 2);
    CHECK(kept.front().geometry.rate > kWfmBandwidth);

    WARN(std::format("98.1 MHz: inside the passband {:.1f} dB, in the corners {:.1f} dB, across "
                     "{} frames",
                     inside, outside, kept.size()));
}

namespace {

// The owner's question, as a measurement: white noise, one tone inside the
// passband and one just outside it, and what the pane makes of both.
constexpr dsp::SampleRate kHoodRate = 2'400'000;
constexpr dsp::Hertz kHoodCentre = 188'500;
constexpr dsp::Hertz kHoodEdge = 3'000;
constexpr dsp::Hertz kHoodInsideTone = -1'000;
constexpr dsp::Hertz kHoodOutsideTone = 5'000;
constexpr double kHoodToneAmplitude = 0.01;
constexpr double kHoodNoiseDbfs = -40.0;
constexpr std::size_t kHoodSamples = 2'400'000;
constexpr std::uint64_t kHoodSeed = 20'260'922;

struct Neighbourhood {
    engine::PassbandGeometry geometry{};
    std::size_t frames = 0;
    std::vector<double> power;  // summed linear power per bin
    std::vector<double> offset_hz;

    [[nodiscard]] double mean_db(double from_hz, double to_hz) const {
        double sum = 0.0;
        std::size_t used = 0;
        for (std::size_t bin = 0; bin < power.size(); ++bin) {
            const double hz = offset_hz[bin];
            const double magnitude = std::abs(hz);
            if (magnitude < from_hz || magnitude > to_hz) {
                continue;
            }
            if (std::abs(hz - kHoodInsideTone) < 400.0 || std::abs(hz - kHoodOutsideTone) < 400.0) {
                continue;
            }
            sum += power[bin] / static_cast<double>(frames);
            ++used;
        }
        return used == 0 ? std::numeric_limits<double>::quiet_NaN()
                         : 10.0 * std::log10(sum / static_cast<double>(used));
    }

    [[nodiscard]] double tone_db(double at_hz) const {
        double best = 0.0;
        for (std::size_t bin = 0; bin < power.size(); ++bin) {
            if (std::abs(offset_hz[bin] - at_hz) <= 200.0) {
                best = std::max(best, power[bin] / static_cast<double>(frames));
            }
        }
        return best > 0.0 ? 10.0 * std::log10(best) : -std::numeric_limits<double>::infinity();
    }

    // The tone's power summed across its main lobe, which unlike the peak
    // bin does not depend on where in a bin the tone happens to fall. Two
    // tones of one amplitude read the same here whatever their frequencies;
    // their peaks differ by the window's scalloping.
    [[nodiscard]] double tone_energy_db(double at_hz) const {
        double sum = 0.0;
        for (std::size_t bin = 0; bin < power.size(); ++bin) {
            if (std::abs(offset_hz[bin] - at_hz) <= 300.0) {
                sum += power[bin] / static_cast<double>(frames);
            }
        }
        return sum > 0.0 ? 10.0 * std::log10(sum) : -std::numeric_limits<double>::infinity();
    }

    // Mean noise in each of `bands` equal slices of the pane, tones excluded.
    [[nodiscard]] std::vector<double> slices_db(std::size_t bands) const {
        std::vector<double> out;
        const std::size_t per = power.size() / bands;
        for (std::size_t band = 0; band < bands; ++band) {
            double sum = 0.0;
            std::size_t used = 0;
            for (std::size_t bin = band * per; bin < (band + 1) * per; ++bin) {
                const double hz = offset_hz[bin];
                if (std::abs(hz - kHoodInsideTone) < 400.0 ||
                    std::abs(hz - kHoodOutsideTone) < 400.0) {
                    continue;
                }
                sum += power[bin] / static_cast<double>(frames);
                ++used;
            }
            out.push_back(used == 0 ? std::numeric_limits<double>::quiet_NaN()
                                    : 10.0 * std::log10(sum / static_cast<double>(used)));
        }
        return out;
    }
};

[[nodiscard]] Neighbourhood measure_neighbourhood() {
    const std::filesystem::path path =
        test::unique_temp_path("revenant_test_passband_hood", ".cf32");

    {
        std::mt19937_64 generator(kHoodSeed);
        const double sigma = std::sqrt(0.5 * std::pow(10.0, kHoodNoiseDbfs / 10.0));
        std::normal_distribution<double> normal(0.0, sigma);

        std::FILE* file = std::fopen(path.string().c_str(), "wb");
        REQUIRE(file != nullptr);
        std::vector<dsp::Complex32> chunk(65'536);
        for (std::size_t done = 0; done < kHoodSamples; done += chunk.size()) {
            const std::size_t count = std::min(chunk.size(), kHoodSamples - done);
            for (std::size_t i = 0; i < count; ++i) {
                const auto n = static_cast<double>(done + i);
                double re = normal(generator);
                double im = normal(generator);
                for (const dsp::Hertz offset : {kHoodInsideTone, kHoodOutsideTone}) {
                    const double turns = std::fmod(
                        static_cast<double>(kHoodCentre + offset) * n /
                            static_cast<double>(kHoodRate),
                        1.0);
                    re += kHoodToneAmplitude * std::cos(2.0 * std::numbers::pi * turns);
                    im += kHoodToneAmplitude * std::sin(2.0 * std::numbers::pi * turns);
                }
                chunk[i] = dsp::Complex32{static_cast<float>(re), static_cast<float>(im)};
            }
            REQUIRE(std::fwrite(chunk.data(), sizeof(dsp::Complex32), count, file) == count);
        }
        std::fclose(file);
    }
    struct Remove {
        std::filesystem::path path;
        ~Remove() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } remove{path};

    Neighbourhood out;
    std::mutex lock;

    engine::EngineConfig config;
    config.channels = kChannels;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = kBlockSamples;
    config.audio_rate = 48'000;
    config.passband_transform = 1'024;
    config.gpu_index = -1;

    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;

    const std::string uri = "file:///" + path.generic_string() +
                            "?rate=" + std::to_string(kHoodRate) + "&format=cf32";
    const auto opened = eng.open_source(uri);
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    engine::VrxParams params;
    params.center = kHoodCentre;
    params.passband_low = -kHoodEdge;
    params.passband_high = kHoodEdge;
    params.demod = engine::Demod::Nfm;
    params.squelch_dbfs = -300.0;
    auto added = eng.add_vrx(params);
    INFO(test::message_of(added));
    REQUIRE(added.has_value());

    REQUIRE(eng.set_passband_sink(*added,
                                  [&](const engine::PassbandFrame& frame) -> Status {
                                      const std::lock_guard<std::mutex> guard(lock);
                                      if (out.power.size() != frame.power_db.size()) {
                                          out.power.assign(frame.power_db.size(), 0.0);
                                          out.frames = 0;
                                      }
                                      out.geometry = frame.geometry;
                                      for (std::size_t bin = 0; bin < frame.power_db.size();
                                           ++bin) {
                                          out.power[bin] += std::pow(
                                              10.0,
                                              static_cast<double>(frame.power_db[bin]) / 10.0);
                                      }
                                      ++out.frames;
                                      return {};
                                  })
                .has_value());

    const Status ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    out.offset_hz.resize(out.power.size());
    for (std::size_t bin = 0; bin < out.power.size(); ++bin) {
        out.offset_hz[bin] = out.geometry.bin_zero_hz() +
                             static_cast<double>(bin) * out.geometry.bin_width_hz() -
                             static_cast<double>(kHoodCentre);
    }
    return out;
}

}  // namespace

TEST_CASE("the passband pane shows the neighbourhood rather than the filter",
          "[gpu][engine][passband][neighbourhood][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The owner's question, "why does the detail spectrum have a massive hump
    // that moves with the filter", as a measurement. White noise at
    // -40 dBFS across the whole source, one tone 1 kHz inside a 6 kHz NFM
    // receiver's passband and one 2 kHz outside it, both at -40 dBFS.
    //
    // Before the display tap, with the pane a transform of the fine stream,
    // this printed a floor of -84.18 dB inside the passband and -120.41 dB
    // averaged outside it, eighths of the pane from -88.47 to -175.43 dB, an
    // 88.69 dB spread, and the outside tone at -123.98 dB against the inside
    // one's -40.36: the filter's own shape, with the neighbour 83.6 dB under
    // where it was. docs/ui-spectrum.md has both sets of figures.
    const Neighbourhood hood = measure_neighbourhood();
    REQUIRE(hood.frames > 16);

    const double span = static_cast<double>(hood.geometry.bins) * hood.geometry.bin_width_hz();
    const double inside = hood.mean_db(0.0, static_cast<double>(kHoodEdge) - 500.0);
    const double outside = hood.mean_db(static_cast<double>(kHoodEdge) + 500.0, 0.5 * span);
    const double tone_in = hood.tone_db(kHoodInsideTone);
    const double tone_out = hood.tone_db(kHoodOutsideTone);
    const double energy_in = hood.tone_energy_db(kHoodInsideTone);
    const double energy_out = hood.tone_energy_db(kHoodOutsideTone);
    const auto slices = hood.slices_db(8);
    const auto [low, high] = std::minmax_element(slices.begin(), slices.end());

    std::string sliced;
    for (const double value : slices) {
        sliced += std::format(" {:.2f}", value);
    }
    WARN(std::format(
        "{} frames of {} bins at {:.2f} Hz, {:.0f} Hz across at a stream rate of {}. Noise "
        "inside the passband {:.2f} dB, outside it {:.2f} dB. Tone inside at {} Hz {:.2f} dB "
        "peak and {:.2f} dB across its lobe, tone outside at {} Hz {:.2f} dB peak and {:.2f} dB "
        "across its lobe. Noise by eighths of the pane:{} (spread {:.2f} dB)",
        hood.frames, hood.geometry.bins, hood.geometry.bin_width_hz(), span, hood.geometry.rate,
        inside, outside, kHoodInsideTone, tone_in, energy_in, kHoodOutsideTone, tone_out,
        energy_out, sliced, *high - *low));

    // The pane is wide enough to show the neighbourhood at all: at least
    // four times the passband's reach, which here is its 3 kHz edge.
    CHECK(span >= 4.0 * static_cast<double>(kHoodEdge));
    CHECK(static_cast<double>(kHoodOutsideTone) < 0.5 * span);

    // The floor is the same inside and outside the passband. The display
    // filter's own ripple is under 0.01 dB (tests/reference/test_vrx.cpp);
    // what this tolerance allows for is the estimate itself, a mean of a
    // few hundred overlapping frames of noise.
    CHECK(std::abs(inside - outside) < 0.2);

    // Flat across the whole pane: no eighth of it is more than half a
    // decibel from any other, where the fine stream's pane spread 88.69 dB.
    CHECK(*high - *low < 0.5);

    // And the tone outside the passband is where it is on the air, at the
    // same level as the one inside, rather than 83.6 dB down.
    CHECK(std::abs(energy_in - energy_out) < 0.2);
}

TEST_CASE("what a passband costs per block", "[.][gpu][engine][passband][cost]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // Hidden, so it is not part of a ctest run: it is a measurement and it
    // has no threshold. Run it by name.
    //
    //   revenant_engine_tests.exe "what a passband costs per block" -s
    //
    // Wall clock around Engine::run() with and without sinks attached, at
    // the geometry tools/bench/throughput.h measures the rest of the engine
    // at: 20 MS/s, 64 channels, 65536-sample blocks, a silent scene so the
    // generator is not the thing being timed. The delta divided by the block
    // count and the receiver count is what one passband costs.
    //
    // It is a delta of two end-to-end runs and not a GPU timestamp, so it
    // includes the host side: the extra readback, the percentile, the
    // colour-map recurrence and the sink call. That is the number a caller
    // deciding whether to open a second display actually pays. The rig in
    // tools/bench splits stages on the device and would give a tighter
    // figure for the dispatches alone.
    constexpr dsp::SampleRate kBenchRate = 20'000'000;
    constexpr std::size_t kBenchBlock = 65'536;
    constexpr std::uint32_t kBenchTransform = 2'048;
    constexpr std::size_t kBenchBlocks = 600;

    // The minimum of several runs rather than the mean. Timing noise on a
    // desktop is one-sided: nothing makes a run faster than it really is, so
    // the fastest run is the closest to the cost and an average is the cost
    // plus whatever else the machine was doing. A first pass at 240 blocks
    // and one run per point had the per-receiver figure at 45, 14 and 39
    // microseconds for one, eight and thirty-two receivers, which is noise
    // rather than a curve.
    //
    // Five runs and 600 blocks is enough to make the eight-receiver point
    // repeat within a microsecond or two. One receiver and thirty-two still
    // do not, and the note in core/engine/graph.cpp's record_passbands says
    // so rather than quoting a figure this case cannot produce twice.
    constexpr int kBenchRepeats = 5;

    const SceneRequest scene{.span_low = -1'000'000,
                             .span_high = 1'000'000,
                             .emitters = 0,
                             .samples = kBenchBlock * kBenchBlocks,
                             .rate = kBenchRate,
                             .noise = false};
    const std::string uri = scene_uri(scene);

    // Returns seconds of wall clock for one run of `receivers` receivers,
    // `attached` of which have a passband sink.
    const auto one_run = [&](std::uint32_t transform, std::size_t receivers,
                             std::size_t attached) -> double {
        std::mutex lock;
        std::uint64_t frames = 0;

        auto config = passband_config();
        config.block_samples = kBenchBlock;
        config.ring_seconds = 0.25;
        config.passband_transform = transform;

        auto created = engine::Engine::create(config);
        REQUIRE(created.has_value());
        auto& eng = **created;
        REQUIRE(eng.open_source(uri).has_value());

        for (std::size_t i = 0; i < receivers; ++i) {
            engine::VrxParams params;

            // Spread across the span so they do not all share one coarse
            // channel, which is how a rack of receivers is actually laid
            // out and what the channelizer exists for.
            params.center = static_cast<dsp::Hertz>(i) * 37'500 - 900'000;
            params.bandwidth = kWideBandwidth;
            params.demod = engine::Demod::Nfm;
            params.squelch_dbfs = -300.0;

            auto added = eng.add_vrx(params);
            REQUIRE(added.has_value());
            if (i < attached) {
                REQUIRE(eng.set_passband_sink(
                               *added,
                               [&](const engine::PassbandFrame&) -> Status {
                                   const std::lock_guard<std::mutex> guard(lock);
                                   ++frames;
                                   return {};
                               })
                            .has_value());
            }
        }

        const auto started = std::chrono::steady_clock::now();
        const Status ran = eng.run();
        const auto finished = std::chrono::steady_clock::now();
        REQUIRE(ran.has_value());

        if (attached > 0) {
            CHECK(frames > 0);
        }
        return std::chrono::duration<double>(finished - started).count();
    };

    const auto measure = [&](std::uint32_t transform, std::size_t receivers,
                             std::size_t attached) -> double {
        double best = std::numeric_limits<double>::infinity();
        for (int repeat = 0; repeat < kBenchRepeats; ++repeat) {
            best = std::min(best, one_run(transform, receivers, attached));
        }
        return best;
    };

    const auto blocks = static_cast<double>(kBenchBlocks);
    const double source_seconds =
        static_cast<double>(scene.samples) / static_cast<double>(kBenchRate);

    // Three runs per point: no passband stage at all, the stage built with
    // every sink detached, and every sink attached. The first delta is what
    // a receiver nobody is looking at pays for the display tap existing,
    // which should be nothing but memory; the second is what looking costs.
    const auto report = [&](std::uint32_t transform, std::size_t receivers) {
        const double none = measure(0, receivers, 0);
        const double bare = measure(transform, receivers, 0);
        const double with = measure(transform, receivers, receivers);
        const double detached_us = 1e6 * (bare - none) / blocks;
        const double per_block_us = 1e6 * (with - bare) / blocks;

        WARN(std::format(
            "N {}, {} receivers: {:.3f} s with no passband stage, {:.3f} s built and detached, "
            "{:.3f} s with every passband on. Detached costs {:.2f} us per block for all of them "
            "and {:.2f} us each; attached costs {:.2f} us per block for all of them and {:.2f} "
            "us each. Realtime multiple {:.2f}x detached, {:.2f}x with.",
            transform, receivers, none, bare, with, detached_us,
            detached_us / static_cast<double>(receivers), per_block_us,
            per_block_us / static_cast<double>(receivers), source_seconds / bare,
            source_seconds / with));
    };

    // One warm run that nothing reads, so the first point does not carry
    // pipeline creation and first-touch page faults for all the others.
    (void)one_run(kBenchTransform, 1, 0);

    for (const std::size_t receivers : {std::size_t{1}, std::size_t{8}, std::size_t{32}}) {
        report(kBenchTransform, receivers);
    }

    // The same receiver count at a quarter of the transform, which is what
    // says whether the cost is the arithmetic or the dispatch. Two
    // transforms of one workgroup each on a card with a hundred and
    // twenty-eight multiprocessors is almost entirely idle silicon, so the
    // prediction is that it barely moves.
    report(kBenchTransform / 4, 8);
}
