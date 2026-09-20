// The detector's track list, over a real socket, against a running engine.
//
// The detector itself is tested in tests/detect/test_detector.cpp, over
// synthetic frames, with no engine and no wire. Nothing here re-tests
// detection: every case below is about the seam, which is a different set of
// failures. A centre that arrives as a baseband offset rather than an
// absolute frequency. A state that renumbers so a held track draws as live.
// A confidence bar that is read from the wrong end of the comparison. A
// detector that never gets built, or one that gets built and never fed
// because the frames it needs were dropped by the subscription filter.
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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/dsp/types.h"
#include "core/engine/engine.h"
#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/types.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"

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
        auto answered = client.detections(0.0);
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
        auto answered = client.detections(0.0);
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
    auto first = harness.client().detections(0.0);
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

    auto settled = harness.client().detections(0.0);
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
    auto again = harness.client().detections(0.0);
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
    REQUIRE(!found.detections.empty());

    const auto stopped = harness.stop_engine();
    INFO(test::message_of(stopped));
    REQUIRE(stopped.has_value());

    auto unfiltered = harness.client().detections(0.0);
    INFO(test::message_of(unfiltered));
    REQUIRE(unfiltered.has_value());
    REQUIRE(!unfiltered->detections.empty());

    // A bar of zero excludes nothing, so the list and the count the detector
    // holds are the same number. A filter reading the comparison the wrong
    // way round fails here before any of the arithmetic below.
    CHECK(unfiltered->detections.size() == unfiltered->total);

    double lowest = 1.0;
    double highest = 0.0;
    for (const rpc::Detection& detection : unfiltered->detections) {
        lowest = std::min(lowest, detection.confidence);
        highest = std::max(highest, detection.confidence);
    }
    INFO(std::format("confidence runs {:.6f} to {:.6f} across {} detections", lowest, highest,
                     unfiltered->detections.size()));

    // The comparison is inclusive, so a bar exactly at the least confident
    // track keeps that track.
    //
    // This pair, with the one below it, is what pins the filter to the exact
    // boundary rather than to "somewhere around here", and the pinning is
    // what makes the case worth running: every track in this scene is born at
    // the same decision and they all carry an identical confidence for as
    // long as they are all detected, so a bar picked anywhere else would only
    // ever be testing all-or-nothing.
    auto at_bar = harness.client().detections(lowest);
    INFO(test::message_of(at_bar));
    REQUIRE(at_bar.has_value());
    REQUIRE(at_bar->last_decision == unfiltered->last_decision);
    CHECK(at_bar->detections.size() == unfiltered->detections.size());

    // One representable step above it, so that same track is excluded.
    const double bar = std::nextafter(lowest, 1.0);
    REQUIRE(bar < 1.0);

    auto filtered = harness.client().detections(bar);
    INFO(test::message_of(filtered));
    REQUIRE(filtered.has_value());

    // Same decision, so the two lists describe the same instant and the
    // comparison is exact.
    REQUIRE(filtered->last_decision == unfiltered->last_decision);

    CHECK(filtered->detections.size() < unfiltered->detections.size());

    // The bar is the caller's and is not stored: the detector still holds
    // everything it held.
    CHECK(filtered->total == unfiltered->total);

    for (const rpc::Detection& detection : filtered->detections) {
        CHECK(detection.confidence >= bar);
    }

    // Everything in the filtered list was in the unfiltered one, by id.
    for (const rpc::Detection& detection : filtered->detections) {
        const bool present = std::ranges::any_of(
            unfiltered->detections,
            [&](const rpc::Detection& other) { return other.id == detection.id; });
        INFO(std::format("detection {} is not in the unfiltered list", detection.id));
        CHECK(present);
    }

    // And the bar did not leak into the server: a second caller asking for
    // everything still gets everything.
    auto after = harness.client().detections(0.0);
    REQUIRE(after.has_value());
    CHECK(after->detections.size() == unfiltered->detections.size());
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
    auto acknowledged = harness.client().detections(0.0);
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
    auto at_one = harness.client().detections(1.0);
    CHECK_FALSE(at_one.has_value());

    auto above_one = harness.client().detections(1.5);
    CHECK_FALSE(above_one.has_value());

    auto below_zero = harness.client().detections(-0.25);
    CHECK_FALSE(below_zero.has_value());

    auto not_a_number =
        harness.client().detections(std::numeric_limits<double>::quiet_NaN());
    CHECK_FALSE(not_a_number.has_value());

    // The open interval's own edge is fine.
    auto just_under = harness.client().detections(std::nextafter(1.0, 0.0));
    INFO(test::message_of(just_under));
    CHECK(just_under.has_value());

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
    auto unchanged = harness.client().detections(0.0);
    INFO(test::message_of(unchanged));
    REQUIRE(unchanged.has_value());
    CHECK(unchanged->detection_threshold_db == 6.0);

    const auto accepted = harness.client().set_detection_threshold(11.5);
    INFO(test::message_of(accepted));
    REQUIRE(accepted.has_value());

    auto applied = harness.client().detections(0.0);
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

    auto refused = harness.client().detections(0.0);
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
