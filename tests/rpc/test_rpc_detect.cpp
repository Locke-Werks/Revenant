// The detector's track list, over a real socket, against a running engine.
//
// The detector itself is tested in tests/detect/test_detector.cpp, over
// synthetic frames, with no engine and no wire. Nothing here re-tests
// detection: every case below is about the seam, which is a different set of
// failures. A centre that arrives as a baseband offset rather than an
// absolute frequency. A state that renumbers so a held track draws as live,
// or a merged one that arrives pointing at no parent. A confidence bar that
// is read from the wrong end of the comparison. A detector that never gets
// built, or one that gets built and never fed because the frames it needs
// were dropped by the subscription filter.
//
// ONE CASE BRINGS ITS OWN SCENE
//
// Everything here runs on tests/rpc/rpc_fixture.h except the merge case,
// which stands up its own engine, server and client on a scene of its own.
// The fixture builds one scene shape and offers only its length and its
// centre, and that shape cannot merge: see the note on merging_scene_uri
// below for what a merge needs and why a static scene never supplies it.
//
// WHY THE ENGINE IS STOPPED BEFORE THE LISTS ARE COMPARED
//
// The detector decides ten times a second, so two calls a millisecond apart
// can legitimately see different lists and a case that compared them would
// fail for the wrong reason at a rate nobody could reproduce. Stopping the
// engine stops the frames, which freezes the detector at its last decision
// and makes every later call return the same answer. The comparisons below
// are then exact rather than approximate, and DetectionList::lastDecision is
// asserted equal across the pair so that a future change which un-freezes it
// fails here rather than becoming flake.
//
// A DETECTION IS NOT CHECKED AGAINST THE SCENE'S OWN TRUTH RECORD
//
// siggen keeps one, and reaching it would mean opening the scene a second
// time in the test process and rebuilding it. That is a detector case rather
// than an RPC one: whether the detector finds the right signals is scored in
// tests/detect, and what this file can say cheaply and exactly is that every
// number which crossed the wire is consistent with the geometry the engine
// reports through the same wire. A centre outside the span, a bandwidth of
// zero, a last_detected after a last_seen, a channel index outside the grid:
// all of those are conversion faults and all of them are caught here.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/detect/detector.h"
#include "core/dsp/synth/wideband.h"
#include "core/dsp/types.h"
#include "core/engine/engine.h"
#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/decoders.h"
#include "core/rpc/server.h"
#include "core/rpc/types.h"
#include "ui/models/label_tune.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"
#include "tests/support/temp_path.h"

using namespace revenant;
using test::Harness;
using test::HarnessOptions;
using test::kChannels;
using test::kSpectrumTransform;

namespace {

// A centre nothing else in the suite uses, and deliberately not a round
// number of channel widths away from anything.
//
// Its only job is to be non-zero. A detection carries absolute frequency with
// EngineInfo::source_center added, and at a centre of zero that claim is
// unfalsifiable because absolute and baseband are then the same number. The
// scene's emitters are placed relative to it, so every track's centre should
// land within a span of it and none within a span of zero.
constexpr dsp::Hertz kSceneCenter = 145'137'000;

// Long enough for every phase of the threshold case: about a second for the
// detector's average to fill, a few decisions to birth tracks, then three
// seconds of hold to run out after the threshold is raised, then another
// birth once it comes back down. Thirty seconds at pace 1.
//
// Nothing generates the tail: every case stops the engine as soon as its
// waits are satisfied, and a synthetic source produces samples on demand. The
// number is a ceiling that keeps a slow machine from running out of scene
// mid-wait, not a runtime.
constexpr dsp::SampleIndex kRunSamples = 72'000'960;
constexpr std::uint32_t kBlockSamples = 16'384;

// Well above anything in the scene, which is placed at 30 to 40 dB of SNR in
// each emitter's own occupied bandwidth. In the 2500 Hz reference bandwidth a
// wider emitter reads higher still, so the margin is larger than it looks.
// Inside the -60 to 120 dB the server accepts.
constexpr double kSilencingThresholdDb = 90.0;

// Between the two groups of SNR this scene produces, with more than fifteen
// decibels of clearance either way.
//
// The nine tracks born at the default threshold measured 14.5, 15.6, 17.6,
// 17.7, 19.0, 36.5, 38.8, 39.1 and 44.7 dB in the 2500 Hz reference
// bandwidth, read off the wire on 2026-09-19. A threshold here keeps the four
// strong ones detected and stops every candidate the other five are built
// from, which is what the confidence case needs: one list holding tracks that
// are still rising and tracks that have started to decay.
constexpr double kPartitioningThresholdDb = 25.0;

[[nodiscard]] HarnessOptions detecting_options() {
    HarnessOptions options;
    options.spectrum_transform = kSpectrumTransform;
    options.samples = kRunSamples;
    options.pace = 1.0;
    options.block_samples = kBlockSamples;
    options.center_hz = kSceneCenter;
    return options;
}

void bring_up(Harness& harness, const HarnessOptions& options) {
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());
}

// Polls until the detector has taken at least this many decisions and found
// something, or the deadline passes. Returns the last answer either way, so a
// case that did not get there can say what it did get.
//
// A poll rather than a wait on frame arrivals: the RPC surface is a poll, and
// a case that waited on something else would be asserting about a path the
// caller does not use.
//
// The decision count matters and is not decoration. Every track is born at
// the decision the detector's warm-up gate releases, so a list taken at the
// first one has every field at its initial value: one hit each, no smoothing
// applied to any centre, and an age of zero. Waiting for a few more means the
// tracker has actually run over these tracks before anything is asserted
// about what came off the wire.
[[nodiscard]] rpc::DetectionList wait_for_detections(rpc::Client& client,
                                                     std::uint64_t min_decisions,
                                                     int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    rpc::DetectionList last;
    for (;;) {
        auto answered = client.detections(0.0, 0.0);
        if (answered) {
            last = std::move(*answered);
            if (last.decisions >= min_decisions && !last.detections.empty()) {
                return last;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return last;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// Polls until the list is empty at a decision that has actually been taken,
// which is what a raised threshold eventually produces once every held track
// has run out its hold.
[[nodiscard]] rpc::DetectionList wait_for_quiet(rpc::Client& client, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    rpc::DetectionList last;
    for (;;) {
        auto answered = client.detections(0.0, 0.0);
        if (answered) {
            last = std::move(*answered);
            if (last.decisions > 0 && last.detections.empty()) {
                return last;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return last;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// The last list that still held tracks, and the empty one that followed it.
//
// wait_for_quiet answers with the empty list alone, which is the wrong end
// for a case about the hold. How far a track's silence got before the
// detector dropped it is only visible in the last list that still carried
// the track, and that list is gone by the time an empty one arrives.
//
// Both members default to an empty list, so a wait that gave up hands back
// something a case can assert is missing rather than a plausible pair.
struct FadeOut {
    rpc::DetectionList last_populated;
    rpc::DetectionList quiet;
};

[[nodiscard]] FadeOut wait_for_fade_out(rpc::Client& client, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    FadeOut out;
    for (;;) {
        auto answered = client.detections(0.0, 0.0);
        if (answered) {
            if (!answered->detections.empty()) {
                out.last_populated = std::move(*answered);
            } else if (answered->decisions > 0) {
                out.quiet = std::move(*answered);
                return out;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return out;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// The two ends of a list's confidence column, which is where a bar worth
// testing comes from. Defaults are the empty list's, so a range taken from
// nothing is empty rather than a pair of plausible numbers.
struct ConfidenceRange {
    double lowest = 1.0;
    double highest = 0.0;
};

[[nodiscard]] ConfidenceRange confidence_range(const rpc::DetectionList& list) {
    ConfidenceRange out;
    for (const rpc::Detection& detection : list.detections) {
        out.lowest = std::min(out.lowest, detection.confidence);
        out.highest = std::max(out.highest, detection.confidence);
    }
    return out;
}

// Polls until two detections in one list disagree about confidence, or the
// deadline passes. Returns the last answer either way.
//
// A list where every confidence is identical cannot be partitioned by a bar,
// and a case that tried would be asserting about an empty result. See the
// note in the confidence case for what produces the disagreement.
[[nodiscard]] rpc::DetectionList wait_for_confidence_spread(rpc::Client& client,
                                                            int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    rpc::DetectionList last;
    for (;;) {
        auto answered = client.detections(0.0, 0.0);
        if (answered) {
            last = std::move(*answered);
            const ConfidenceRange range = confidence_range(last);
            if (last.detections.size() > 1 && range.lowest < range.highest) {
                return last;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return last;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// Polls until some track in the list has been swallowed by another, or the
// deadline passes. Returns the last answer either way.
[[nodiscard]] rpc::DetectionList wait_for_merge(rpc::Client& client, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    rpc::DetectionList last;
    for (;;) {
        auto answered = client.detections(0.0, 0.0);
        if (answered) {
            last = std::move(*answered);
            const bool merged = std::ranges::any_of(last.detections, [](const rpc::Detection& d) {
                return d.state == rpc::TrackState::Merged;
            });
            if (merged) {
                return last;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return last;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// The band the engine says its spectrum covers, in absolute hertz. Read off
// the wire rather than recomputed from kSourceRate, so a detection is checked
// against the same geometry the engine reports and not against this file's
// idea of it.
struct AbsoluteSpan {
    double low = 0.0;
    double high = 0.0;
};

[[nodiscard]] AbsoluteSpan span_of(const rpc::EngineInfo& info) {
    const double width = info.spectrum.bin_width.hertz();
    const double zero = info.spectrum.bin_zero.hertz();
    const double base = static_cast<double>(info.source_center);

    AbsoluteSpan out;
    out.low = base + zero - 0.5 * width;
    out.high = base + zero + (static_cast<double>(info.spectrum.bins) - 0.5) * width;
    return out;
}

// A scene that merges, which the fixture's cannot.
//
// Five emitters inside sixty kilohertz, each transmitting in six bursts
// rather than continuously. Crowding them puts several tracks inside the
// reach of one candidate, and making them intermittent is what lets a wide
// one appear on top of two that are already tracked. Neither alone does it.
//
// A static scene cannot merge however it is arranged, which is why the
// fixture's never does. The detector takes the strongest deflection first and
// grows it onto its own shoulders, so a window covering two signals always
// clashes with the narrower windows that already took those bins and is
// skipped. It takes the scene changing under a tracker that is already
// holding ids. Measured on 2026-09-19 over the fixture's scene, 220 decisions
// at each of 6, 8, 12, 16 and 20 dB gave 0 merges and 0 splits; this scene
// gave a merged track in 75 of 84 status samples across one 18 second run.
//
// Seed 5 was chosen by running six of them and keeping the one that merged
// most. There is nothing else special about it, and a change to the scene
// generator or to the detector can take the merge away, which is why the case
// below says what it was waiting for when it gives up.
[[nodiscard]] std::string merging_scene_uri(dsp::SampleIndex samples) {
    return std::format(
        "synthetic:wideband?rate={}&emitters=5&seed=5&noise_dbfs=-100&snr_min=25&snr_max=40"
        "&span_low=-30000&span_high=30000&bursts=6&min_burst=0.15&max_burst=0.6"
        "&samples={}&center={}",
        test::kSourceRate, samples, kSceneCenter);
}

// Engine, server and client over a socket, on a scene this file names.
//
// test::Harness would be the place for this and cannot be: it builds its URI
// from one fixed scene shape and exposes only the sample count and the
// centre. Everything below is that fixture's sequence, with two differences.
// The URI is the caller's, and open() also starts the engine, because the one
// case using this has nothing to do before the frames arrive.
//
// The teardown order is the fixture's header's and is copied rather than
// re-derived: the engine outlives the server because the server holds a sink
// on it, and the client goes first because it holds a connection to the
// server.
class SceneRig {
public:
    SceneRig() = default;
    ~SceneRig() { shutdown(); }

    SceneRig(const SceneRig&) = delete;
    SceneRig& operator=(const SceneRig&) = delete;
    SceneRig(SceneRig&&) = delete;
    SceneRig& operator=(SceneRig&&) = delete;

    // Opens everything and starts the engine on its own thread, because a
    // case that polls the wire has to be driven while the engine runs.
    [[nodiscard]] Status open(const std::string& uri, std::uint32_t probes = 0,
                              double pace = 1.0) {
        engine::EngineConfig config;
        config.gpu_index = -1;  // honours REVENANT_GPU_INDEX, like every other binary here
        config.channels = kChannels;
        config.taps_per_branch = 17;
        config.ring_seconds = 0.5;
        config.block_samples = kBlockSamples;
        config.pace = pace;
        config.spectrum_transform = kSpectrumTransform;
        config.probe_receivers = probes;

        auto created = engine::Engine::create(config);
        if (!created) {
            return std::unexpected(with_context(created.error(), "building the engine"));
        }
        engine_ = std::move(*created);

        if (auto opened = engine_->open_source(uri); !opened) {
            return std::unexpected(with_context(opened.error(), "opening the scene"));
        }

        rpc::ServerOptions server_options;
        const rpc::Token token = test::test_token();
        server_options.token.assign(token.begin(), token.end());
        auto served = rpc::Server::create(*engine_, server_options);
        if (!served) {
            return std::unexpected(with_context(served.error(), "starting the server"));
        }
        server_ = std::move(*served);

        auto connected = rpc::Client::connect("127.0.0.1", server_->port(), token);
        if (!connected) {
            return std::unexpected(with_context(connected.error(), "connecting the client"));
        }
        client_ = std::move(*connected);

        running_ = true;
        runner_ = std::thread([this] { run_outcome_ = engine_->run(); });
        return {};
    }

    [[nodiscard]] rpc::Client& client() { return *client_; }

    // Stops the engine, joins its thread and hands back what run() returned,
    // with the cancellation this asked for treated as the clean finish it is.
    // The same distinction test::Harness draws, and for the same reason: a
    // device lost mid-run must not hide behind "we asked it to stop".
    [[nodiscard]] Status stop() {
        if (!running_) {
            return run_outcome_;
        }
        static_cast<void>(engine_->stop());
        if (runner_.joinable()) {
            runner_.join();
        }
        running_ = false;

        if (!run_outcome_ &&
            run_outcome_.error().message.find("the engine was stopped") != std::string::npos) {
            run_outcome_ = Status{};
        }
        return run_outcome_;
    }

private:
    void shutdown() {
        static_cast<void>(stop());
        client_.reset();
        server_.reset();
        engine_.reset();
    }

    std::unique_ptr<engine::Engine> engine_;
    std::unique_ptr<rpc::Server> server_;
    std::unique_ptr<rpc::Client> client_;

    std::thread runner_;
    Status run_outcome_;
    bool running_ = false;
};

}  // namespace

TEST_CASE("detections cross the wire and agree with the geometry the engine reports",
          "[gpu][rpc][detect][m2]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    Harness harness;
    bring_up(harness, detecting_options());

    // Before a single frame. This is the call that builds the detector, and
    // the answer it can give is the one that says so: zero decisions and
    // nothing found. It would read identically if the detector were built at
    // startup and fed nothing, which is why the run below is what separates
    // the two.
    auto first = harness.client().detections(0.0, 0.0);
    INFO(test::message_of(first));
    REQUIRE(first.has_value());
    CHECK(first->decisions == 0);
    CHECK(first->detections.empty());
    CHECK(first->total == 0);

    // The detector's default, which the CLI prints and docs/detection.md
    // calls a starting point rather than a right answer. Asserted because it
    // is what the SNR bound below is measured against.
    CHECK(first->detection_threshold_db == 6.0);

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());

    const rpc::DetectionList live = wait_for_detections(harness.client(), 8, 20000);
    INFO(std::format("{} decisions, {} of {} detections", live.decisions,
                     live.detections.size(), live.total));
    REQUIRE(live.decisions > 0);
    REQUIRE(!live.detections.empty());

    // Frozen from here, so every number below belongs to one decision.
    const auto stopped = harness.stop_engine();
    INFO(test::message_of(stopped));
    REQUIRE(stopped.has_value());

    auto settled = harness.client().detections(0.0, 0.0);
    INFO(test::message_of(settled));
    REQUIRE(settled.has_value());

    auto engine_info = harness.client().info();
    REQUIRE(engine_info.has_value());
    REQUIRE(engine_info->spectrum.enabled());
    REQUIRE(engine_info->source_center == kSceneCenter);

    const AbsoluteSpan span = span_of(*engine_info);
    const double sample_rate = static_cast<double>(engine_info->source_rate);
    REQUIRE(sample_rate > 0.0);

    const rpc::DetectionList& list = *settled;
    CHECK(list.detections.size() == list.total);
    CHECK(list.last_decision > 0);

    std::vector<std::uint64_t> ids;
    ids.reserve(list.detections.size());
    std::int64_t previous_centre = std::numeric_limits<std::int64_t>::min();
    std::size_t live_count = 0;

    for (const rpc::Detection& detection : list.detections) {
        INFO(std::format("detection {} at {} Hz, {} Hz wide, {:.1f} dB, confidence {:.3f}, {}",
                         detection.id, detection.center_hz, detection.bandwidth_hz,
                         detection.snr_2500_db, detection.confidence,
                         rpc::track_state_name(detection.state)));

        // Ids are issued from one and never reused, so zero is not one.
        CHECK(detection.id != 0);
        ids.push_back(detection.id);

        // Pending is never published. A reader seeing one means the state
        // enum renumbered or the detector's own contract changed.
        CHECK(detection.state != rpc::TrackState::Pending);

        // Absolute, with source_center added. A centre that arrived as a
        // baseband offset would be within a megahertz or so of zero and
        // nowhere near the span, which is the whole reason this scene has a
        // centre at all.
        CHECK(static_cast<double>(detection.center_hz) >= span.low);
        CHECK(static_cast<double>(detection.center_hz) <= span.high);

        // At least one bin and no wider than the span it was measured in.
        CHECK(detection.bandwidth_hz >= 1);
        CHECK(static_cast<double>(detection.bandwidth_hz) <= span.high - span.low);

        // Every candidate a track is built from cleared the threshold, and a
        // track's SNR is a weighted average of those, so it cannot be below
        // it. The tolerance is for the decibel round trip and nothing else.
        CHECK(detection.snr_2500_db >= list.detection_threshold_db - 1.0e-6);

        // Rises toward one without reaching it.
        CHECK(detection.confidence > 0.0);
        CHECK(detection.confidence < 1.0);

        // THE CALIBRATED NUMBER REACHES A CLIENT, which is the half of a new
        // wire field that goes missing quietly: docs/rpc.md records a case
        // where the schema carried a pair and read_engine_info dropped it, and
        // says a field the schema carries and the client silently drops is
        // exactly the shape of gap a reader assumes is still open.
        //
        // Every published detection cleared the detection threshold, and the
        // margin map is a half AT the threshold and rises above it, so a
        // client that never read the field would arrive here holding the
        // default zero and fail this line rather than the next one.
        CHECK(detection.margin_confidence >= 0.5);
        CHECK(detection.margin_confidence < 1.0);

        // And it is the margin it claims to be, which is checkable here
        // because every term is on this same wire: the map is
        // 1 - 0.5*exp(-(snr - threshold)/6) and both inputs arrived with it.
        // A field written from the wrong source, or from the stopwatch, does
        // not satisfy its own definition against two numbers it did not
        // choose. The tolerance is the two decibel round trips and nothing
        // else.
        const double expected =
            1.0 - 0.5 * std::exp(-(detection.snr_2500_db - list.detection_threshold_db) / 6.0);
        CHECK(std::abs(detection.margin_confidence - expected) < 1.0e-6);

        // THE SHAPE REACHES A CLIENT TOO, and it is the same half that goes
        // missing quietly. Both fields default to zero and false, so a client
        // that never read them arrives holding exactly the reading that means
        // "we could not tell", which is the failure the flag exists to stop a
        // display drawing as "certainly junk".
        //
        // Every published detection was built from a candidate with positive
        // excess in it, so there was always something to measure.
        CHECK(detection.shape_measured);

        // A fraction of the band's own excess, so it cannot be negative and
        // cannot exceed the whole. Three bins out of three is the degenerate
        // upper end and is reachable, so the bound is inclusive.
        CHECK(detection.concentration > 0.0);
        CHECK(detection.concentration <= 1.0);

        // Sample indices, and their order is the tracker's invariant: a track
        // was first seen before it was last detected, and last detected no
        // later than it was last seen.
        CHECK(detection.first_seen <= detection.last_detected);
        CHECK(detection.last_detected <= detection.last_seen);
        CHECK(detection.last_seen <= list.last_decision);

        // Which follows from the three above and is what a display actually
        // draws. A live track has been silent for no time at all; a held one
        // has been silent for some, which is what it is decaying over. The
        // pair is what makes the state enum's mapping load-bearing rather
        // than a field nothing reads: a conversion that put every track in
        // one state fails one of these two.
        if (detection.state == rpc::TrackState::Live) {
            ++live_count;
            CHECK(detection.silent_samples() == 0);
        }
        if (detection.state == rpc::TrackState::Held) {
            CHECK(detection.silent_samples() > 0);
        }

        // Seconds are the client's division, and the only clock here.
        const double age_seconds = static_cast<double>(detection.age_samples()) / sample_rate;
        CHECK(age_seconds >= 0.0);
        CHECK(age_seconds <= static_cast<double>(list.last_decision) / sample_rate);

        // grid_channels was configured, so the hysteresis is on and every
        // track has a channel.
        CHECK(detection.channel_valid);
        CHECK(detection.channel < kChannels);

        // Zero unless this track is inside another's band.
        if (detection.state != rpc::TrackState::Merged) {
            CHECK(detection.merged_into == 0);
        }

        // Ascending in frequency, which is what the detector publishes and
        // what a display draws without sorting again.
        CHECK(detection.center_hz >= previous_centre);
        previous_centre = detection.center_hz;
    }

    std::ranges::sort(ids);
    CHECK(std::ranges::adjacent_find(ids) == ids.end());

    // The scene's emitters are continuous, so at the decision this list was
    // frozen at, something was being detected rather than merely remembered.
    INFO(std::format("{} of {} detections were live", live_count, list.detections.size()));
    CHECK(live_count > 0);

    // The same call twice against a stopped engine is the same answer. A
    // detector still being fed, or a list rebuilt per call from something
    // that moves, fails here.
    auto again = harness.client().detections(0.0, 0.0);
    REQUIRE(again.has_value());
    CHECK(again->last_decision == list.last_decision);
    CHECK(again->detections.size() == list.detections.size());
}

TEST_CASE("the confidence bar belongs to the caller and filters the list",
          "[gpu][rpc][detect][m2]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    Harness harness;
    bring_up(harness, detecting_options());

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());

    const rpc::DetectionList found = wait_for_detections(harness.client(), 8, 20000);
    INFO(std::format("{} decisions, {} detections", found.decisions, found.detections.size()));
    REQUIRE(found.detections.size() > 1);

    // WHY THE THRESHOLD IS MOVED BEFORE THE BAR IS EXERCISED
    //
    // Every emitter in this scene is continuous, so every track is born at
    // the decision the detector's warm-up gate releases and detected at every
    // decision after it. Confidence rises by a fixed fraction of its
    // remaining distance to one, so tracks with the same history arrive at
    // the same number to the last bit: measured on 2026-09-19 this list read
    // 0.968136 to 0.968136 across nine detections.
    //
    // A bar picked out of a column like that can only be all or nothing, and
    // the version of this case that picked one was asserting nothing. At
    // nextafter(lowest, 1.0) every detection was excluded, the filtered list
    // came back empty, and both of the per-row loops below ran zero times, so
    // the exclusive side had no rows to be wrong about.
    //
    // kPartitioningThresholdDb is what puts two ends on the column. The four
    // strong tracks keep being detected and keep rising; the five weak ones
    // stop being detected, hold, and decay. The detector's bootstrap hold is
    // three seconds, so both groups are in one list for that long, which is
    // all this case wants from the detector. Measured on 2026-09-19 the frozen
    // list read 0.901802 across the five held tracks and 0.979288 across the
    // four live ones, and the bar below kept four and dropped five.
    const auto raised = harness.client().set_detection_threshold(kPartitioningThresholdDb);
    INFO(test::message_of(raised));
    REQUIRE(raised.has_value());

    const rpc::DetectionList spreading = wait_for_confidence_spread(harness.client(), 20000);
    {
        const ConfidenceRange range = confidence_range(spreading);
        INFO(std::format("confidence runs {:.6f} to {:.6f} across {} detections at decision {}",
                         range.lowest, range.highest, spreading.detections.size(),
                         spreading.last_decision));
        REQUIRE(spreading.detections.size() > 1);
        REQUIRE(range.lowest < range.highest);
    }

    // Frozen from here, so every list below belongs to one decision.
    const auto stopped = harness.stop_engine();
    INFO(test::message_of(stopped));
    REQUIRE(stopped.has_value());

    auto unfiltered = harness.client().detections(0.0, 0.0);
    INFO(test::message_of(unfiltered));
    REQUIRE(unfiltered.has_value());
    REQUIRE(unfiltered->detections.size() > 1);

    // A bar of zero excludes nothing, so the list and the count the detector
    // holds are the same number. A filter reading the comparison the wrong
    // way round fails here before any of the arithmetic below.
    CHECK(unfiltered->detections.size() == unfiltered->total);

    const ConfidenceRange range = confidence_range(*unfiltered);
    INFO(std::format("confidence runs {:.6f} to {:.6f} across {} detections", range.lowest,
                     range.highest, unfiltered->detections.size()));

    // The decisions taken between the poll above and the stop did not undo
    // the spread. Without this the list could be uniform again and every row
    // assertion below would pass by having no rows, which is exactly how this
    // case used to pass.
    REQUIRE(range.lowest < range.highest);

    // The comparison is inclusive, so a bar exactly at the least confident
    // track keeps that track and everything above it.
    //
    // This pair, with the one below it, is what pins the filter to the exact
    // boundary rather than to "somewhere around here".
    auto at_bar = harness.client().detections(range.lowest, 0.0);
    INFO(test::message_of(at_bar));
    REQUIRE(at_bar.has_value());
    REQUIRE(at_bar->last_decision == unfiltered->last_decision);
    CHECK(at_bar->detections.size() == unfiltered->detections.size());

    // One representable step above it, so that same track is excluded and the
    // rising ones at the other end of the spread are not.
    const double bar = std::nextafter(range.lowest, 1.0);
    REQUIRE(bar < 1.0);
    REQUIRE(bar <= range.highest);

    auto filtered = harness.client().detections(bar, 0.0);
    INFO(test::message_of(filtered));
    REQUIRE(filtered.has_value());

    // Same decision, so the two lists describe the same instant and the
    // comparison is exact.
    REQUIRE(filtered->last_decision == unfiltered->last_decision);

    // Both sides of the bar have rows in them, which is what the two loops
    // below need before they can say anything.
    REQUIRE(!filtered->detections.empty());
    REQUIRE(filtered->detections.size() < unfiltered->detections.size());

    // The bar is the caller's and is not stored: the detector still holds
    // everything it held.
    CHECK(filtered->total == unfiltered->total);

    // Kept: at or above the bar, and known to the unfiltered list by id.
    for (const rpc::Detection& detection : filtered->detections) {
        INFO(std::format("detection {} was kept at confidence {:.17g} against a bar of {:.17g}",
                         detection.id, detection.confidence, bar));
        CHECK(detection.confidence >= bar);

        const bool present = std::ranges::any_of(
            unfiltered->detections,
            [&](const rpc::Detection& other) { return other.id == detection.id; });
        CHECK(present);
    }

    // Excluded: below the bar, every one of them. A filter that dropped a row
    // for any other reason, or that kept the wrong end of the comparison and
    // happened to return the right count, fails here.
    std::size_t excluded = 0;
    for (const rpc::Detection& detection : unfiltered->detections) {
        const bool kept = std::ranges::any_of(
            filtered->detections,
            [&](const rpc::Detection& other) { return other.id == detection.id; });
        if (kept) {
            continue;
        }
        ++excluded;
        INFO(std::format("detection {} was excluded at confidence {:.17g} against a bar of {:.17g}",
                         detection.id, detection.confidence, bar));
        CHECK(detection.confidence < bar);
    }
    CHECK(excluded > 0);
    CHECK(excluded == unfiltered->detections.size() - filtered->detections.size());

    // And the bar did not leak into the server: a second caller asking for
    // everything still gets everything.
    auto after = harness.client().detections(0.0, 0.0);
    REQUIRE(after.has_value());
    CHECK(after->detections.size() == unfiltered->detections.size());
}

TEST_CASE("a merged track crosses the wire carrying the id it was merged into",
          "[gpu][rpc][detect][m2]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The one state and the one field nothing else on this wire exercises.
    //
    // detect::TrackState::Merged and Track::merged_into are covered in
    // tests/detect/test_detector.cpp, which never touches this layer. Three
    // pieces of conversion sit between that coverage and a display:
    // write_detection's setMergedInto, to_schema(Merged) and the client's
    // read_track_state. Every other case in this file passes with all three
    // wrong. The fixture's scene never merges, and the one other place any of
    // them names Merged is a check that skips a row when it sees one.
    //
    // What a merge means, since the numbers below depend on it: a track that
    // is not detected at this decision because another track's candidate
    // swallowed its band. It is not the same thing as a held track. A held
    // one has no evidence and is decaying; a merged one has evidence that is
    // being counted against a different id, so it stops decaying and points
    // at the id that took it.
    SceneRig rig;
    const auto ready = rig.open(merging_scene_uri(kRunSamples));
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    const rpc::DetectionList list = wait_for_merge(rig.client(), 25000);

    // Worded for the failure this case has to be able to report, which is a
    // wait that gave up: the list it hands back is then whatever the last
    // poll saw, and its size is the difference between a scene that stopped
    // merging and a detector that found nothing at all.
    INFO(std::format("the merge wait came back at decision {} with {} detections",
                     list.last_decision, list.detections.size()));

    std::size_t merged_rows = 0;
    std::size_t resolved_parents = 0;
    for (const rpc::Detection& detection : list.detections) {
        if (detection.state != rpc::TrackState::Merged) {
            continue;
        }
        ++merged_rows;

        INFO(std::format("detection {} at {} Hz, {} Hz wide, merged into {}", detection.id,
                         detection.center_hz, detection.bandwidth_hz, detection.merged_into));

        // The ordinal survived both conversions. A to_schema that mapped
        // Merged to some other state, or a read_track_state that rejected
        // ordinal three, never reaches this line.
        CHECK(rpc::track_state_name(detection.state) == std::string("merged"));

        // The field the state exists to carry. Zero here is what a dropped
        // setMergedInto looks like, and it is also what every non-merged row
        // in this file is checked to be, so the two checks are each other's
        // control.
        CHECK(detection.merged_into != 0);
        CHECK(detection.merged_into != detection.id);

        // Not detected at this decision, which is what a merge and a hold
        // have in common and is what separates both from a live track.
        CHECK(detection.silent_samples() > 0);

        const auto parent = std::ranges::find_if(list.detections, [&](const rpc::Detection& other) {
            return other.id == detection.merged_into;
        });

        // A parent the list no longer names is legal and is not an error to
        // report. The detector suspends a child's confidence decay while it
        // is merged and does not suspend the parent's, so a parent that was
        // young when it took the candidate can be dropped for low confidence
        // while its child is still inside its own hold. It did not happen in
        // any run of this scene on 2026-09-19, and the count below is what
        // keeps a case where it happened to every row from passing empty.
        if (parent == list.detections.end()) {
            continue;
        }
        ++resolved_parents;

        // Ordering, which is the part of a merge that cannot be got right by
        // accident. The candidate goes to the elder of the gated tracks, so
        // the parent is never the younger of the pair, and the child's
        // evidence is being counted against the parent, so the parent was
        // detected no earlier than the child was. A merged_into read out of
        // the wrong field, or truncated on the wire, would have to land on
        // another track's id and then satisfy both of these.
        CHECK(parent->first_seen <= detection.first_seen);
        CHECK(parent->last_detected >= detection.last_detected);
    }

    // Outside the loop, so a run that never merged fails here with the INFO
    // above rather than passing on an empty list. That is the whole defect
    // this case exists to close, so it must not be able to repeat it.
    CHECK(merged_rows > 0);
    CHECK(resolved_parents > 0);

    const auto stopped = rig.stop();
    INFO(test::message_of(stopped));
    CHECK(stopped.has_value());
}

TEST_CASE("the detection threshold is the operator's and changes what the detector finds",
          "[gpu][rpc][detect][m2]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    Harness harness;
    bring_up(harness, detecting_options());

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());

    const rpc::DetectionList found = wait_for_detections(harness.client(), 8, 20000);
    INFO(std::format("{} decisions, {} detections at {:.1f} dB", found.decisions,
                     found.detections.size(), found.detection_threshold_db));
    REQUIRE(!found.detections.empty());

    const auto raised = harness.client().set_detection_threshold(kSilencingThresholdDb);
    INFO(test::message_of(raised));
    REQUIRE(raised.has_value());

    // Read back through the list rather than trusted. This is engine-wide
    // state and the reply is the only place a client can see what it really
    // is.
    auto acknowledged = harness.client().detections(0.0, 0.0);
    REQUIRE(acknowledged.has_value());
    CHECK(acknowledged->detection_threshold_db == kSilencingThresholdDb);

    // Nothing in the scene clears 90 dB, so no candidate is emitted, nothing
    // is reassociated, and every track runs out the detector's bootstrap hold
    // and is dropped. That takes a few seconds of source time at pace 1; the
    // deadline is generous because a slow machine paces the same way but
    // polls less often.
    const rpc::DetectionList quiet = wait_for_quiet(harness.client(), 20000);
    INFO(std::format("{} detections survived the raised threshold, {} decisions taken",
                     quiet.detections.size(), quiet.decisions));
    CHECK(quiet.detections.empty());
    CHECK(quiet.total == 0);

    // Decisions kept being taken while nothing was found, which is what makes
    // the empty list above a measurement rather than a stalled detector.
    CHECK(quiet.decisions > found.decisions);

    // Back down, and the same scene comes back. A one-way kill would pass
    // every assertion above.
    const auto lowered = harness.client().set_detection_threshold(6.0);
    INFO(test::message_of(lowered));
    REQUIRE(lowered.has_value());

    const rpc::DetectionList returned = wait_for_detections(harness.client(), 1, 20000);
    INFO(std::format("{} detections after the threshold came back down",
                     returned.detections.size()));
    CHECK(!returned.detections.empty());
    CHECK(returned.detection_threshold_db == 6.0);

    // New identities. Ids are issued from one and never reused, so a track
    // that was dropped and found again is a different track, and every id
    // here is above every id from before the silence.
    std::uint64_t highest_before = 0;
    for (const rpc::Detection& detection : found.detections) {
        highest_before = std::max(highest_before, detection.id);
    }
    for (const rpc::Detection& detection : returned.detections) {
        INFO(std::format("detection {} against a high water mark of {}", detection.id,
                         highest_before));
        CHECK(detection.id > highest_before);
    }

    const auto stopped = harness.stop_engine();
    INFO(test::message_of(stopped));
    REQUIRE(stopped.has_value());
}

TEST_CASE("the detector's hold crosses the wire and bounds what the detector does",
          "[gpu][rpc][detect][m2]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // DetectionList::detectorHoldSeconds exists because a display cannot
    // derive it. Before it was on the wire, ui/render/spectrum_item.h
    // carried a compiled-in copy of DetectorConfig::bootstrap_hold_seconds
    // and its own comment called that the display's assumption about the
    // engine's configuration.
    //
    // A case that only compared the number against the detector's default
    // would pass against a server that wrote the literal 3.0 and never read
    // the detector at all. So this asserts two things that can disagree:
    // that the number on the wire is the one the engine's own header
    // declares, and that the detector's observed behaviour is bounded by
    // the number that arrived.
    Harness harness;
    bring_up(harness, detecting_options());

    const double configured = detect::DetectorConfig{}.bootstrap_hold_seconds;
    REQUIRE(configured > 0.0);

    // Stated on the call that BUILDS the detector, before a frame exists. A
    // display sizes its drawing from this and the first list it has rows to
    // draw is later than this, so a field that only appeared once something
    // was found would be a field arriving after it was needed.
    auto first = harness.client().detections(0.0, 0.0);
    INFO(test::message_of(first));
    REQUIRE(first.has_value());
    CHECK(first->decisions == 0);
    CHECK(first->detector_hold_seconds == configured);

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());

    const rpc::DetectionList live = wait_for_detections(harness.client(), 8, 20000);
    INFO(std::format("{} decisions, {} detections, hold {} s", live.decisions,
                     live.detections.size(), live.detector_hold_seconds));
    REQUIRE(!live.detections.empty());
    CHECK(live.detector_hold_seconds == configured);

    auto engine_info = harness.client().info();
    REQUIRE(engine_info.has_value());
    const double sample_rate = static_cast<double>(engine_info->source_rate);
    REQUIRE(sample_rate > 0.0);

    // Silence the scene, which is the only way to make the hold observable:
    // a track that keeps being detected never spends any of it. Nothing here
    // clears 90 dB, so every track stops being detected at the next decision
    // and runs its hold out from there.
    const auto raised = harness.client().set_detection_threshold(kSilencingThresholdDb);
    INFO(test::message_of(raised));
    REQUIRE(raised.has_value());

    const FadeOut faded = wait_for_fade_out(harness.client(), 20000);
    INFO(std::format("last populated list at decision {} with {} detections, quiet at {}",
                     faded.last_populated.last_decision, faded.last_populated.detections.size(),
                     faded.quiet.last_decision));
    REQUIRE(!faded.last_populated.detections.empty());
    REQUIRE(faded.quiet.decisions > 0);
    REQUIRE(faded.quiet.detections.empty());

    // The threshold moved and the hold did not. A server that wrote the
    // wrong member of DetectorConfig, or aliased the two fields, fails here
    // as well as against the default above.
    CHECK(faded.last_populated.detector_hold_seconds == configured);
    CHECK(faded.quiet.detector_hold_seconds == configured);
    CHECK(faded.quiet.detection_threshold_db == kSilencingThresholdDb);

    const double hold = faded.last_populated.detector_hold_seconds;
    double longest_silence = 0.0;
    for (const rpc::Detection& detection : faded.last_populated.detections) {
        const double silence = static_cast<double>(detection.silent_samples()) / sample_rate;
        longest_silence = std::max(longest_silence, silence);

        INFO(std::format("detection {} had been silent {:.3f} s against a hold of {:.3f} s",
                         detection.id, silence, hold));

        // The detector drops a track at the decision its silence exceeds
        // the hold, so a track that is still being published has not
        // exceeded it. The bound is exact rather than approximate, and a
        // hold reported smaller than the one the detector runs fails it.
        CHECK(silence <= hold);
    }

    // The other side of the same claim, which the bound above cannot make:
    // a hold reported LARGER than the one the detector runs satisfies every
    // check in the loop and leaves every observed silence far short of it.
    // Half the hold is a wide margin on purpose. Measured on 2026-09-19 on
    // GPU 0, the longest silence in the last populated list reached 2.970 s
    // of the 3.0 s hold, which is one decision short of it at the
    // detector's default interval. A loaded machine polls less often and
    // lands further short, and this case must not turn into a timing
    // measurement, so the bound is set where that cannot matter. Tripling
    // the reported hold in core/rpc/server.cpp on the same day failed this
    // line at 2.970 against 4.5 while every other check in the loop still
    // passed, which is what says the bound has teeth of its own.
    INFO(std::format("the longest silence reached {:.3f} s of a {:.3f} s hold", longest_silence,
                     hold));
    CHECK(longest_silence > 0.5 * hold);

    const auto stopped = harness.stop_engine();
    INFO(test::message_of(stopped));
    REQUIRE(stopped.has_value());
}

TEST_CASE("the detector refuses the arguments that would fail silently",
          "[gpu][rpc][detect][m2]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    Harness harness;
    bring_up(harness, detecting_options());

    // A bar of one lists nothing however strong the signal is, because a
    // track's confidence approaches one without arriving. An empty list is
    // also what a dead band looks like, so this has to fail rather than
    // answer.
    auto at_one = harness.client().detections(1.0, 0.0);
    CHECK_FALSE(at_one.has_value());

    auto above_one = harness.client().detections(1.5, 0.0);
    CHECK_FALSE(above_one.has_value());

    auto below_zero = harness.client().detections(-0.25, 0.0);
    CHECK_FALSE(below_zero.has_value());

    auto not_a_number =
        harness.client().detections(std::numeric_limits<double>::quiet_NaN(), 0.0);
    CHECK_FALSE(not_a_number.has_value());

    // The open interval's own edge is fine, and it is the value a clamped
    // control has to be able to send.
    //
    // ui/models/engine_link.h clamps its writable confidenceBar property to
    // kMaxConfidenceBar, which is this number written as a constexpr
    // expression because the property needs one. That clamp was [0, 1]
    // INCLUSIVE until 2026-09-19, which put the one value rejected above
    // inside the range the property accepted: writing 1.0 made every later
    // poll fail, and poll_detections discards a failed poll by design,
    // because that is how a dead engine is normally found, so the overlay
    // stopped updating with nothing on screen saying why.
    //
    // Both forms are asserted equal rather than one of them being trusted.
    // epsilon is 2^-52 and the spacing of doubles just below one is 2^-53,
    // so the subtraction is exact and the two are the same double.
    constexpr double kTopOfRange = 1.0 - std::numeric_limits<double>::epsilon() / 2.0;
    CHECK(kTopOfRange == std::nextafter(1.0, 0.0));

    auto just_under = harness.client().detections(kTopOfRange, 0.0);
    INFO(test::message_of(just_under));
    CHECK(just_under.has_value());

    // And nothing sits between the top of the accepted range and the value
    // that is refused, which is what makes clamping to kTopOfRange the
    // right fix rather than a guess at a safe margin below one. A server
    // that moved its guard to reject anything above one, or to reject a
    // little below it, fails one of this pair.
    const double one_step_higher = std::nextafter(kTopOfRange, 2.0);
    CHECK(one_step_higher == 1.0);
    auto refused_at_one = harness.client().detections(one_step_higher, 0.0);
    CHECK_FALSE(refused_at_one.has_value());

    // And the threshold's bounds, which are the command line's rather than
    // the arithmetic's.
    auto far_too_high = harness.client().set_detection_threshold(500.0);
    CHECK_FALSE(far_too_high.has_value());

    auto far_too_low = harness.client().set_detection_threshold(-500.0);
    CHECK_FALSE(far_too_low.has_value());

    auto threshold_nan =
        harness.client().set_detection_threshold(std::numeric_limits<double>::quiet_NaN());
    CHECK_FALSE(threshold_nan.has_value());

    // A refused threshold left the detector where it was, rather than half
    // applying.
    auto unchanged = harness.client().detections(0.0, 0.0);
    INFO(test::message_of(unchanged));
    REQUIRE(unchanged.has_value());
    CHECK(unchanged->detection_threshold_db == 6.0);

    const auto accepted = harness.client().set_detection_threshold(11.5);
    INFO(test::message_of(accepted));
    REQUIRE(accepted.has_value());

    auto applied = harness.client().detections(0.0, 0.0);
    REQUIRE(applied.has_value());
    CHECK(applied->detection_threshold_db == 11.5);
}

TEST_CASE("a detection tunes a receiver, which is the whole point of putting it on the wire",
          "[gpu][rpc][detect][m2]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    Harness harness;
    bring_up(harness, detecting_options());

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());

    const rpc::DetectionList found = wait_for_detections(harness.client(), 8, 20000);
    REQUIRE(!found.detections.empty());

    auto engine_info = harness.client().info();
    REQUIRE(engine_info.has_value());
    REQUIRE(engine_info->source_center == kSceneCenter);

    // The widest, rather than the strongest. A click lands on whatever the
    // operator points at, and the widest is the one least likely to be a bare
    // carrier whose measured bandwidth is a single bin, which would make this
    // a case about narrow filter design rather than about tuning.
    const rpc::Detection* target = &found.detections.front();
    for (const rpc::Detection& detection : found.detections) {
        if (detection.bandwidth_hz > target->bandwidth_hz) {
            target = &detection;
        }
    }
    INFO(std::format("tuning detection {} at {} Hz, {} Hz wide", target->id, target->center_hz,
                     target->bandwidth_hz));

    // The conversion the schema documents on VrxParams::center, done here
    // because a caller has to do it: a detection is absolute and the grid
    // works in baseband.
    const std::int64_t baseband = target->center_hz - engine_info->source_center;

    rpc::VrxParams params;
    params.center = baseband;
    params.bandwidth = target->bandwidth_hz;
    params.demod = rpc::Demod::Nfm;

    auto added = harness.client().add_vrx(params);
    INFO(test::message_of(added));
    REQUIRE(added.has_value());

    auto status = harness.client().vrx_status(*added);
    INFO(test::message_of(status));
    REQUIRE(status.has_value());

    // The placement's two exact rationals put back together are the baseband
    // centre that was asked for. residual is defined as the request minus the
    // channel centre, so this is the whole placement round trip in one line,
    // and it is exact rather than approximate because neither term was ever
    // rounded.
    const double placed = status->placement.channel_centre.hertz() +
                          status->placement.residual.hertz();
    CHECK(placed == static_cast<double>(baseband));

    CHECK(status->params.center == baseband);
    CHECK(status->placement.channel < kChannels);

    // Whether the clamp fired is not this case's business, but a clamped
    // receiver that did not report it is: docs/detection.md names silent
    // clamping as the failure click-to-tune has to surface, and the pair has
    // to agree.
    const std::int64_t channel_spacing = engine_info->channel_spacing;
    REQUIRE(channel_spacing > 0);
    if (target->bandwidth_hz > channel_spacing) {
        CHECK(status->placement.bandwidth_clamped);
    }

    // And the mistake the schema warns about, pinned rather than described.
    // Passing the absolute centre straight through puts the receiver far
    // outside the source's own span, and place() refuses it. If the engine
    // ever starts rebasing, this fails here and the schema's note about
    // VrxParams::center is what has to change with it.
    rpc::VrxParams unconverted = params;
    unconverted.center = target->center_hz;
    auto refused = harness.client().add_vrx(unconverted);
    CHECK_FALSE(refused.has_value());

    const auto stopped = harness.stop_engine();
    INFO(test::message_of(stopped));
    REQUIRE(stopped.has_value());
}

TEST_CASE("a decimated spectrum subscription and the detector run off the same sink",
          "[gpu][rpc][detect][m2]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    Harness harness;
    bring_up(harness, detecting_options());

    // One sink feeds both, and they want different things from it. A
    // subscriber asking for every sixteenth frame must not decide what the
    // detector sees: the detector integrates over about a second and a frame
    // withheld from it is energy it never gets back, so it is fed before the
    // subscription filter runs rather than after.
    //
    // This case cannot see which frames the detector was given. What it can
    // see is that both halves still work with the other one running, which is
    // the combination nothing else here exercises: the detector takes a lock
    // on the engine's completion thread and the fan-out runs on the event
    // loop thread, and an ordering mistake between them shows up as one side
    // or the other going quiet.
    auto log = std::make_shared<test::FrameLog>();
    const auto subscribed = harness.client().subscribe_spectrum(
        16, [log](const rpc::SpectrumFrame& frame) { log->record(frame); });
    INFO(test::message_of(subscribed));
    REQUIRE(subscribed.has_value());

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());

    const rpc::DetectionList found = wait_for_detections(harness.client(), 8, 20000);
    INFO(std::format("{} decisions and {} detections beside {} delivered frames",
                     found.decisions, found.detections.size(), log->size()));
    CHECK(found.decisions >= 8);
    CHECK(!found.detections.empty());

    // The subscriber got frames too, so the detector did not starve it.
    CHECK(log->size() > 0);

    const auto stopped = harness.stop_engine();
    INFO(test::message_of(stopped));
    REQUIRE(stopped.has_value());
}

TEST_CASE("an engine with no spectrum stage refuses detections in the engine's own words",
          "[gpu][rpc][detect][m2]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // spectrum_transform at zero, which is the default and what a headless
    // recording runs. The detector works on spectrum frames and there are
    // none, which is a different answer from a quiet band and has to read
    // like one.
    Harness harness;
    HarnessOptions options;
    options.center_hz = kSceneCenter;
    bring_up(harness, options);

    auto refused = harness.client().detections(0.0, 0.0);
    REQUIRE_FALSE(refused.has_value());

    const std::string message = refused.error().message;
    INFO(message);

    // The engine's sentence, not the detector's. A detector built against an
    // empty geometry would refuse in bin counts, which names no field anybody
    // can change.
    CHECK(message.find("spectrum") != std::string::npos);
    CHECK(message.find("spectrum_transform") != std::string::npos);

    // The other entry point takes the same route and has to give the same
    // answer, rather than appearing to succeed against a detector that does
    // not exist.
    auto also_refused = harness.client().set_detection_threshold(20.0);
    INFO(test::message_of(also_refused));
    CHECK_FALSE(also_refused.has_value());
}
// The margin bar, which is the half of this pair that can actually partition a
// real list.
//
// WHY THIS CASE IS SHORTER THAN THE CONFIDENCE ONE ABOVE. That one has to
// manufacture a spread before it can test a filter, by raising the DETECTION
// threshold until half the tracks stop being detected and start decaying,
// because a column of confidence is otherwise constant across everything that
// has been up for a second. The margin column has a spread by construction:
// it is a monotone function of each track's own SNR, and this scene places
// emitters from 14 to 45 dB.
TEST_CASE("the margin bar filters the list and belongs to the caller",
          "[rpc][detect]") {
    Harness harness;
    bring_up(harness, detecting_options());

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());

    const rpc::DetectionList live = wait_for_detections(harness.client(), 8, 20000);
    INFO(std::format("{} decisions, {} detections", live.decisions, live.detections.size()));
    REQUIRE(live.detections.size() > 1);

    // Frozen, so every list below describes the same instant and the
    // comparisons are exact rather than approximate.
    const auto stopped = harness.stop_engine();
    INFO(test::message_of(stopped));
    REQUIRE(stopped.has_value());

    auto unfiltered = harness.client().detections(0.0, 0.0);
    INFO(test::message_of(unfiltered));
    REQUIRE(unfiltered.has_value());
    REQUIRE(unfiltered->detections.size() > 1);

    double lowest = 1.0;
    double highest = 0.0;
    for (const rpc::Detection& detection : unfiltered->detections) {
        lowest = std::min(lowest, detection.margin_confidence);
        highest = std::max(highest, detection.margin_confidence);
    }
    INFO(std::format("margin runs {:.6f} to {:.6f} across {} detections", lowest, highest,
                     unfiltered->detections.size()));

    // THE SPREAD IS THE POINT AND IS ASSERTED. A column that reads the same
    // for everything cannot be partitioned by any bar, which is the defect
    // this number was added to fix, so a run where it were constant would
    // make everything below pass without meaning anything.
    REQUIRE(lowest < highest);

    // Inclusive at the bar, so the weakest track and everything above it stay.
    auto at_bar = harness.client().detections(0.0, lowest);
    INFO(test::message_of(at_bar));
    REQUIRE(at_bar.has_value());
    REQUIRE(at_bar->last_decision == unfiltered->last_decision);
    CHECK(at_bar->detections.size() == unfiltered->detections.size());

    // One representable step above, so that track goes and the stronger ones
    // do not.
    const double bar = std::nextafter(lowest, 1.0);
    auto filtered = harness.client().detections(0.0, bar);
    INFO(test::message_of(filtered));
    REQUIRE(filtered.has_value());
    REQUIRE(filtered->last_decision == unfiltered->last_decision);
    REQUIRE(!filtered->detections.empty());
    CHECK(filtered->detections.size() < unfiltered->detections.size());

    // The bar is the caller's and is not stored: the detector still holds
    // everything it held.
    CHECK(filtered->total == unfiltered->total);

    for (const rpc::Detection& detection : filtered->detections) {
        INFO(std::format("detection {} kept at margin {:.17g} against a bar of {:.17g}",
                         detection.id, detection.margin_confidence, bar));
        CHECK(detection.margin_confidence >= bar);
    }

    // THE TWO BARS ARE INDEPENDENT AND BOTH APPLY. A list asked for things
    // that are both strong and settled is the intersection, which is what
    // makes the pair worth having over a single knob.
    auto both = harness.client().detections(0.5, bar);
    INFO(test::message_of(both));
    REQUIRE(both.has_value());
    CHECK(both->detections.size() <= filtered->detections.size());
    for (const rpc::Detection& detection : both->detections) {
        CHECK(detection.margin_confidence >= bar);
        CHECK(detection.confidence >= 0.5);
    }
}

// Refused rather than answered emptily, on exactly the grounds the confidence
// bar is: the map approaches one without arriving, so a bar of one lists
// nothing however strong the signal is, and an empty list is also what a dead
// band looks like.
TEST_CASE("a margin bar of one is refused rather than answered empty",
          "[rpc][detect]") {
    Harness harness;
    bring_up(harness, detecting_options());

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());

    static_cast<void>(wait_for_detections(harness.client(), 4, 20000));

    for (const double bad : {1.0, 1.5, -0.25, std::numeric_limits<double>::quiet_NaN()}) {
        auto refused = harness.client().detections(0.0, bad);
        INFO(std::format("margin bar {}", bad));
        CHECK_FALSE(refused.has_value());
    }

    // And the largest value below one is accepted, so the refusal is about
    // the unreachable bar rather than about the top of the range.
    auto accepted = harness.client().detections(0.0, std::nextafter(1.0, 0.0));
    INFO(test::message_of(accepted));
    CHECK(accepted.has_value());
}

// THE LABEL ON THE WIRE, the owner's decision of 2026-09-23. A server whose
// engine has probe receivers runs tier two on its own detector and every
// detection carries what detect::label_track makes of its track. Carriers at
// 30 dB, because the probe survey in tests/engine/test_engine_probe.cpp names
// those at that level every time, so a label that never arrives is the wire
// and not the characteriser.
//
// REJECTS: a server that labels nothing, a label that crosses without its
// kind, and an unlabelled row that says it may drive a receiver.
TEST_CASE("a probed detection crosses the wire with its label", "[gpu][rpc][detect][m2]") {
    REVENANT_NEEDS_GPU();

    const std::string uri = std::format(
        "synthetic:wideband?rate={}&emitters=3&seed=17&noise_dbfs=-100&snr_min=30&snr_max=30"
        "&span_low=-150000&span_high=150000&modes=cw&center={}",
        test::kSourceRate, kSceneCenter);

    SceneRig rig;
    const auto ready = rig.open(uri, 2, 4.0);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    rpc::DetectionList last;
    const rpc::Detection* labelled = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (labelled == nullptr && std::chrono::steady_clock::now() < deadline) {
        auto answered = rig.client().detections(0.0, 0.0);
        REQUIRE(answered.has_value());
        last = std::move(*answered);
        for (const rpc::Detection& detection : last.detections) {
            if (detection.label.kind != rpc::LabelKind::Unknown) {
                labelled = &detection;
                break;
            }
        }
        if (labelled == nullptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    INFO(std::format("{} detections at decision {}", last.detections.size(), last.last_decision));
    REQUIRE(labelled != nullptr);

    INFO(std::format("detection {} labelled {} '{}' at {:.2f} after {} probes", labelled->id,
                     static_cast<int>(labelled->label.kind), labelled->label.name,
                     labelled->label.confidence, labelled->label.probes));
    CHECK(labelled->label.kind == rpc::LabelKind::AnalogModulation);
    CHECK((labelled->label.name == "CW" || labelled->label.name == "AM"));
    CHECK(labelled->label.may_drive);
    CHECK(labelled->label.probes > 0);
    CHECK(labelled->label.confidence > 0.0);

    for (const rpc::Detection& detection : last.detections) {
        if (detection.label.kind == rpc::LabelKind::Unknown) {
            CHECK(detection.label.name.empty());
            CHECK_FALSE(detection.label.may_drive);
        }
    }

    const auto stopped = rig.stop();
    INFO(test::message_of(stopped));
    REQUIRE(stopped.has_value());
}

// A RECORDING, OPENED OVER THE WIRE THE WAY A WINDOW OPENS ONE, PLAYED AT
// REALTIME, AND LABELLED.
//
// The owner reported after the playtest of 2026-09-23 that signal
// identification did nothing on a recording. This is that path end to end on
// an engine configured as revenant-engine configures itself: no grid chosen,
// 65536-sample blocks, a 2048-point transform, 30 rows a second asked for and
// four probe receivers. The engine starts with no source, a client opens a
// 96 kS/s cf32 file with no pace in its URI, which Session.openSource plays at
// realtime, and polls the detections at revenant-ui's four a second until one
// comes back labelled. Four AM carriers at 30 dB, the level
// tests/engine/test_engine_probe.cpp's survey names every time, so a label
// that never arrives is the path and not the characteriser.
//
// WHAT IT FOUND, recorded in docs/rpc.md under Threading: the path was not
// broken at the engine or on the wire. It runs at the 30 rows a second
// revenant-engine now asks for and at one row per block, which is what the
// engine did before, and a label arrives either way; revenant-loadtest
// labelled seven tracks of eight on the KF4FIC 7 MHz excerpt through the
// same close-and-open.
//
// REJECTS: a recording opened over the wire that is not played at realtime,
// a detector that is not rebuilt for the recording's geometry, and labels
// that never arrive for a file.
TEST_CASE("a recording opened over the wire at realtime is detected and labelled",
          "[gpu][rpc][detect][m2]") {
    REVENANT_NEEDS_GPU();
    const double rows = GENERATE(30.0, 0.0);
    INFO("rows a second asked for: " << rows);

    constexpr dsp::SampleRate kRate = 96'000;
    constexpr double kSeconds = 14.0;
    const auto samples = static_cast<dsp::SampleIndex>(kSeconds * kRate);

    siggen::SceneSpec spec;
    spec.rate = kRate;
    spec.duration_samples = samples;
    spec.seed = 20260923;
    spec.noise_power_full_band_dbfs = -60.0;
    spec.worker_threads = 1;
    spec.random.emitter_count = 4;
    spec.random.span_low_hz = -40'000;
    spec.random.span_high_hz = 40'000;
    spec.random.snr_in_occupied_bandwidth_db_min = 30.0;
    spec.random.snr_in_occupied_bandwidth_db_max = 30.0;
    spec.random.palette = {siggen::Modulation::Am};
    auto scene = siggen::Scene::create(spec);
    INFO(test::message_of(scene));
    REQUIRE(scene.has_value());

    const std::filesystem::path path = test::unique_temp_path("revenant-recording", ".cf32");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        const auto wrote = siggen::stream_scene(
            *scene, 0, samples, 65'536,
            [&](const dsp::BlockTimestamp&, dsp::ConstComplexSpan block) -> Status {
                out.write(reinterpret_cast<const char*>(block.data()),
                          static_cast<std::streamsize>(block.size_bytes()));
                return out.good() ? Status{} : fail("writing the recording failed");
            });
        INFO(test::message_of(wrote));
        REQUIRE(wrote.has_value());
    }
    struct Remove {
        std::filesystem::path path;
        ~Remove() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } const remove{path};

    engine::EngineConfig config;
    config.gpu_index = -1;
    config.channels = 0;
    config.block_samples = 65'536;
    config.ring_seconds = 2.0;
    config.spectrum_transform = 2048;
    config.spectrum_rows_per_second = rows;
    config.probe_receivers = 4;
    auto created = engine::Engine::create(config);
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;

    rpc::ServerOptions server_options;
    const rpc::Token token = test::test_token();
    server_options.token.assign(token.begin(), token.end());
    auto served = rpc::Server::create(eng, server_options);
    REQUIRE(served.has_value());
    auto connected = rpc::Client::connect("127.0.0.1", (*served)->port(), token);
    REQUIRE(connected.has_value());
    rpc::Client& client = **connected;

    // revenant-engine's own loop: run while there is a source.
    std::atomic<bool> serving{true};
    std::thread runner([&] {
        while (serving.load()) {
            if (!eng.has_source()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            const std::uint64_t epoch = eng.info().source_epoch;
            static_cast<void>(eng.run());
            while (serving.load() && eng.has_source() && eng.info().source_epoch == epoch) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    });

    const std::string uri = std::format("file:///{}?rate={}&format=cf32&center=14100000",
                                        path.generic_string(), kRate);
    const auto opened = client.open_source(uri);
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    rpc::DetectionList last;
    const rpc::Detection* labelled = nullptr;
    const auto began = std::chrono::steady_clock::now();
    const auto deadline = began + std::chrono::seconds(static_cast<int>(kSeconds) - 1);
    while (labelled == nullptr && std::chrono::steady_clock::now() < deadline) {
        auto answered = client.detections(0.0, 0.0);
        REQUIRE(answered.has_value());
        last = std::move(*answered);
        for (const rpc::Detection& detection : last.detections) {
            if (detection.label.kind != rpc::LabelKind::Unknown) {
                labelled = &detection;
                break;
            }
        }
        if (labelled == nullptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
    const double waited =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    const engine::SourcePacing pacing = eng.source_pacing();
    const engine::EngineInfo info = eng.info();
    INFO(std::format("{} detections at decision {} after {:.1f} s; paced by {}, block {}",
                     last.detections.size(), last.last_decision, waited, pacing.paced_by,
                     info.block_samples));

    serving.store(false);
    static_cast<void>(eng.stop());
    runner.join();

    // Played at realtime, as a window's open asks, and at the row rate asked.
    CHECK(pacing.paced_by == 1.0);
    CHECK(info.source_rate == kRate);
    CHECK((info.block_samples < 65'536) == (rows > 0.0));

    REQUIRE(labelled != nullptr);
    CHECK(labelled->label.kind == rpc::LabelKind::AnalogModulation);
    CHECK(labelled->label.probes > 0);
    CHECK(labelled->center_hz > 14'100'000 - 40'000);
    CHECK(labelled->center_hz < 14'100'000 + 40'000);
}

// The client's label table against the server's decoder registry, here
// because ui/tests links no decoder and this binary links all of them.
//
// REJECTS: a decoder name in ui/models/label_tune.h that the registry does not
// hold, which would attach nothing and say nothing about it, and a decoder
// paired with a mode whose output it does not read, which subscribeDecoded
// refuses in words the operator would then see instead of a decode.
TEST_CASE("every labelled protocol's decoder reads the mode the label sets", "[rpc][detect]") {
    for (const auto& row : ui::label_tune_detail::kProtocols) {
        INFO(std::string(row.name));
        CHECK(engine::demod_from_name(row.mode).has_value());
        if (row.decoder.empty()) {
            continue;
        }
        const rpc::DecoderSpec* found = nullptr;
        for (const rpc::DecoderSpec& spec : rpc::decoder_registry()) {
            if (spec.name == row.decoder) {
                found = &spec;
            }
        }
        REQUIRE(found != nullptr);
        CHECK(rpc::decoder_accepts(*found, row.mode));
    }
    for (const auto& row : ui::label_tune_detail::kAnalogue) {
        INFO(std::string(row.name));
        CHECK(engine::demod_from_name(row.mode).has_value());
    }
}
