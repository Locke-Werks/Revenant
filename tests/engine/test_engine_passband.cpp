// The per-receiver passband spectrum, end to end on a real device.
//
// test_engine_spectrum.cpp covers the full-span chain. This is the other
// transform: one receiver's own fine stream, at the demodulation rate, which
// docs/ui-spectrum.md calls the fine-tuning display and the thing that makes
// parking a filter on a signal precise rather than approximate.
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
//   The two-pass reconstruction. spectrum.comp keeps the central half of its
//   transform, because that is what tiles a channel bank. A passband needs
//   every bin: the demodulation rate is only 1.5 times the receiver's
//   bandwidth at plan_vrx's shared floor, so the central half is 0.75 of the
//   bandwidth and cuts into the signal rather than into the guard. The other
//   half comes back from the same kernel under a window whose odd taps are
//   negated, which shifts the transform by exactly N/2 bins, and the graph
//   reassembles the two central halves into one ascending frame with three
//   copy regions. Get the window wrong and half the frame mirrors the other
//   half; get a copy region wrong and the frame is a plausible spectrum with
//   a quarter of it in the wrong place.
//
//   The frequency axis. It is the fine stream's and not the receiver's
//   request: bin zero sits half a demodulation rate below whatever the fine
//   stage mixed to DC, which for CW is one audio pitch away from
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
// says it is. One of those offsets is deliberately in the outer quarter of
// the passband, where a single pass of the kernel writes no bin at all.
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
#include <format>
#include <limits>
#include <mutex>
#include <numbers>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/dsp/pfb.h"
#include "core/dsp/spectrum_reference.h"
#include "core/dsp/synth/wideband.h"
#include "core/engine/engine.h"
#include "core/engine/spectrum_scale.h"
#include "core/source/rtlsdr_source.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;
using Catch::Approx;

namespace {

constexpr dsp::SampleRate kSourceRate = 2'400'000;
constexpr std::uint32_t kChannels = 64;

// 512 points of a 48 kHz fine stream is 93.75 Hz per bin. Both devices in the
// conformance matrix hold a 512-point complex transform in one workgroup's
// shared memory many times over; the engine clamps to what the device offers
// and reports the clamp, and the geometry case checks that no clamp happened,
// because one would mean this number was chosen for a machine that is no
// longer the one being tested.
constexpr std::uint32_t kPassbandTransform = 512;

// Small on purpose, as in test_engine_spectrum.cpp: 8192 source samples is
// 256 coarse blocks at D = 32, which is about 164 fine samples at 48 kHz, so
// the window gate is exercised for the first few dispatches instead of being
// satisfied by the very first one.
constexpr std::size_t kBlockSamples = 8'192;

// THE GEOMETRY THAT LETS A SIGNAL REACH THE OUTER QUARTER AT ALL
//
// A signal outside the receiver's own filter is not in the passband to be
// found, so putting one where a single pass of the kernel writes no bin
// needs the filter to pass further out than Fd/4. plan_vrx sets
// Fd = ceil(minimum_demod_rate / Fa) * Fa and the NFM minimum is the shared
// floor of 1.5B, so B/Fd can be at most 2/3 and B/2 at most Fd/3. The window
// that is both inside the filter and outside the central half is therefore
// Fd/4 to Fd/3, which is a twelfth of the band and nothing wider.
//
// At Fa = 8 kHz and B = 16 kHz the minimum is 24 kHz, which is three audio
// rates exactly, so Fd = 24 kHz, the filter passes to 8 kHz and the outer
// quarter starts at 6 kHz. An offset of 7 kHz sits in the middle of that
// window with a kilohertz of margin either side.
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
constexpr dsp::SampleRate kExpectedDemodRate = 24'000;
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

TEST_CASE("a carrier lands where the passband's own axis says, in every quarter of the band",
          "[gpu][engine][passband][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The case the rest of the file is scaffolding for.
    //
    // Four receivers on one emitter, at four offsets chosen so that the
    // carrier lands in each of the three regions the frame is assembled
    // from. Pass A supplies the central half; pass B supplies both outer
    // quarters, and its output is split across two copy regions, so the two
    // wide offsets below exercise different regions of the same pass.
    //
    //   +7 kHz  the receiver sits above the carrier, so the carrier is at
    //           -7 kHz: the NEGATIVE outer quarter, the upper half of pass
    //           B's output copied to the bottom of the frame.
    //   +2 kHz  carrier at -2 kHz, central half, pass A.
    //   -2 kHz  carrier at +2 kHz, central half, pass A.
    //   -7 kHz  carrier at +7 kHz: the POSITIVE outer quarter, the lower
    //           half of pass B's output copied to the top of the frame.
    //
    // A single-pass implementation writes no bin at all at plus or minus
    // 7 kHz, so it cannot pass this by being off by a little; it reports the
    // peak somewhere in the central half and misses by seven kilohertz.
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
        {.receiver_above_carrier = kOuterOffset},
        {.receiver_above_carrier = 2'000},
        {.receiver_above_carrier = -2'000},
        {.receiver_above_carrier = -kOuterOffset},
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

    // Nobody's bandwidth was cut down to fit its channel. A clamp here would
    // pull the filter's edge inside the offsets chosen above and the outer
    // quarter would hold nothing but stopband, which reads as a broken
    // reassembly rather than as a receiver that did not get what it asked
    // for.
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

    for (std::size_t i = 0; i < parked.size(); ++i) {
        const engine::VrxId id = captured.ids[i];
        const dsp::Hertz offset = parked[i].receiver_above_carrier;
        const Vote vote = vote_for(captured.frames, id);

        INFO(std::format("receiver {} parked {} Hz above the carrier", id.value, offset));
        REQUIRE(vote.total > 8);

        // Every frame this receiver delivered carries the same axis, so any
        // one of them will do for the geometry.
        engine::PassbandGeometry geometry{};
        for (const CapturedFrame& frame : captured.frames) {
            if (frame.vrx == id) {
                geometry = frame.geometry;
                break;
            }
        }

        INFO(std::format("{} bins of {} Hz from {} Hz, at a demodulation rate of {}",
                         geometry.bins, geometry.bin_width_hz(), geometry.bin_zero_hz(),
                         geometry.rate));

        CHECK(geometry.enabled());
        CHECK(geometry.transform == kPassbandTransform);
        CHECK(geometry.bins == kPassbandTransform);
        CHECK(geometry.rate == kExpectedDemodRate);

        // The frame is the whole demodulation rate wide, edge to edge. Half
        // of it would be the central-half selection left in place.
        const double span = static_cast<double>(geometry.bins) * geometry.bin_width_hz();
        CHECK(span == Approx(static_cast<double>(kExpectedDemodRate)));

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
        // this is aimed at are not close: a missing second pass misses by
        // 14 kHz, a mirrored assembly by twice the offset, and an axis
        // centred on the channel rather than on the receiver by up to half a
        // channel spacing.
        CHECK(std::abs(peak_hz - static_cast<double>(carrier)) <
              3.0 * geometry.bin_width_hz());

        // Most frames agree. Deliberately not all of them; see Vote.
        CHECK(vote.agreement() > 0.5);
    }
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

    // The window is N fine samples, which at Fd = 48 kHz against a source at
    // 2.4 MS/s is N * 50 source samples. Checked against the frame's own
    // rate rather than against 50, so a receiver whose plan chose a
    // different decimation is covered by the same relation.
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

        CHECK(frame.bins_seen == kPassbandTransform);
        CHECK(frame.count == expected_count);

        // Inside the stream. The start is the window's first fine sample
        // mapped back through both group delays, and that subtraction is in
        // an unsigned type, so a window allowed to begin below the delay
        // would report a start near 2^64.
        CHECK(frame.start < scene.samples);
        CHECK(frame.start + frame.count < scene.samples + expected_count);

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
    // nothing. A window is N*50 source samples, which is 25600 at this
    // geometry, so a 0.1 second burst holds several of them and so does the
    // gap after it.
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

TEST_CASE("two central halves under the two windows reconstruct the whole band",
          "[engine][passband][m1]") {
    // No device. This is the identity the graph's reassembly rests on,
    // refereed against dsp::reference_spectrum, which is the bit-exact twin
    // of the kernel the graph dispatches.
    //
    //     sum_n w[n] x[n] (-1)^n e^(-j2*pi*k*n/N) = X_w((k + N/2) mod N)
    //
    // The kernel keeps the central half, so the ordinary window hands back
    // bins -N/4 to N/4 and the negated-odd window hands back the two outer
    // quarters. Three copy regions put them in ascending order. If either
    // the window or a region is wrong, a tone in the outer quarter of the
    // band lands in the wrong bin or in no bin at all, and that is what this
    // asserts against tones placed in each of the three regions.
    constexpr std::uint32_t kTransform = 256;
    constexpr std::uint32_t kRingBlocks = 1024;

    auto twiddles = dsp::build_twiddles(kTransform);
    INFO(test::message_of(twiddles));
    REQUIRE(twiddles.has_value());

    // Built the way core/engine/graph.cpp builds them: the shipped window
    // followed by a correction half of unity, because a passband has no
    // channel shape to divide out. The second table is the first one's bits
    // with the odd taps' signs flipped, which is exact.
    auto base = dsp::build_spectrum_window(kTransform);
    INFO(test::message_of(base));
    REQUIRE(base.has_value());

    std::vector<float> window_a = *base;
    for (std::uint32_t bin = 0; bin < kTransform / 2; ++bin) {
        window_a[kTransform + bin] = 1.0F;
    }
    std::vector<float> window_b = window_a;
    for (std::uint32_t n = 1; n < kTransform; n += 2) {
        window_b[n] = -window_b[n];
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

    // One tone per region of the assembled frame: the negative outer
    // quarter, the central half, and the positive outer quarter. Integer
    // bins, so the answer is a single bin rather than a spread.
    for (const int tone_bin : {-100, -30, 17, 100}) {
        INFO(std::format("a tone at transform bin {}", tone_bin));

        std::vector<dsp::Complex32> ring(2 * kRingBlocks, dsp::Complex32{});
        for (std::uint32_t n = 0; n < kRingBlocks; ++n) {
            const double phase = 2.0 * std::numbers::pi * static_cast<double>(tone_bin) *
                                 static_cast<double>(n) / static_cast<double>(kTransform);
            ring[n] = dsp::Complex32{static_cast<float>(std::cos(phase)),
                                     static_cast<float>(std::sin(phase))};
        }

        std::vector<float> pass_a(2 * half, 0.0F);
        std::vector<float> pass_b(2 * half, 0.0F);
        REQUIRE(dsp::reference_spectrum(params, ring, *twiddles, window_a, pass_a).has_value());
        REQUIRE(dsp::reference_spectrum(params, ring, *twiddles, window_b, pass_b).has_value());

        // The graph's three copy regions, in the same order and with the
        // same offsets record_passband() records them with.
        std::vector<float> frame(kTransform, 0.0F);
        for (std::size_t j = 0; j < half; ++j) {
            frame[quarter + j] = pass_a[half + j];
        }
        for (std::size_t j = 0; j < quarter; ++j) {
            frame[3 * quarter + j] = pass_b[half + j];
            frame[j] = pass_b[half + quarter + j];
        }

        // Ascending frame bin b holds signed frequency b - N/2.
        const auto expected = static_cast<std::size_t>(tone_bin + static_cast<int>(half));

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

TEST_CASE("a passband over a real radio shows the filter's own edges",
          "[.][gpu][engine][passband][rtlsdr]") {
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
    // The assertion is one that holds whatever is on the air. The receiver's
    // fine filter passes to B/2 and the frame reaches Fd/2 either side, so
    // the bins beyond B/2 are that filter's stopband and have to be far below
    // the ones inside it. A dead band makes both of them noise and the margin
    // is the filter's attenuation; a live band makes the inside a signal and
    // widens it. Either way the display is showing where the filter's edges
    // are, which is what docs/ui-spectrum.md says the fine-tuning display is
    // for.
    //
    // That sentence said "the frame is Fd/2 wide" until 2026-09-20, which
    // this file's own assertion contradicts: the case above CHECKs that the
    // span equals kExpectedDemodRate, so the frame is Fd wide edge to edge.
    // The old figure also made the assertion below impossible. Fd is 1.5
    // times B at plan_vrx's floor, so a frame Fd/2 wide would reach only
    // 0.375 B either side and hold no stopband bins at all.
    auto attached = source::enumerate_rtlsdr_devices();
    if (!attached.has_value() || attached->empty()) {
        SKIP("no RTL-SDR is attached to this machine");
    }

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

    if (auto opened = eng.open_source("rtlsdr://0?freq=98.1M&rate=2400000&gain=auto");
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
                                      0.85 * 0.5 * static_cast<double>(frame.geometry.rate)) {
                               // The far corners only. Between the passband
                               // edge and here is the transition, which is
                               // neither one thing nor the other.
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

    CHECK(kept.front().geometry.bins == kRadioTransform);
    CHECK(kept.front().geometry.rate > kWfmBandwidth);

    // The filter is there and the display shows it. Twenty decibels is well
    // under what a Kaiser design at this transition reaches and well over
    // anything a frame of stale memory or a mirrored assembly would produce.
    CHECK(inside - outside > 20.0);

    WARN(std::format("98.1 MHz: inside the passband {:.1f} dB, in the corners {:.1f} dB, so "
                     "{:.1f} dB of filter edge across {} frames",
                     inside, outside, inside - outside, kept.size()));
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

    const auto report = [&](std::uint32_t transform, std::size_t receivers) {
        const double bare = measure(transform, receivers, 0);
        const double with = measure(transform, receivers, receivers);
        const double per_block_us = 1e6 * (with - bare) / blocks;

        WARN(std::format(
            "N {}, {} receivers: {:.3f} s bare, {:.3f} s with every passband on, so {:.2f} us "
            "per block for all of them and {:.2f} us each. Realtime multiple {:.2f}x bare, "
            "{:.2f}x with.",
            transform, receivers, bare, with, per_block_us,
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
