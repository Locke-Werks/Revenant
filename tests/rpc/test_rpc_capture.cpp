// The rolling capture over the wire: captureCatalog, captureSearch and
// captureOccupancy against a root a real engine wrote, the refusal on an
// engine keeping none, a hit's replay URI opened on the engine that is
// recording, and a retune a replay follows reaching the server with nothing
// subscribed to ring its loop.
//
// The annotations are handed to the engine's writer directly rather than
// left to the detector. What is under test is the wire's reading of a root,
// and a detector that found a different number of things on a different GPU
// would make every count here a guess.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/capture/format.h"
#include "core/dsp/types.h"
#include "core/rpc/client.h"
#include "core/rpc/types.h"
#include "tests/capture/catalog_fixture.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"

using namespace revenant;
using test::Harness;
using test::HarnessOptions;

namespace {

constexpr std::uint32_t kBlock = 16'384;

// Two thirds of a second of the scene, in segments of a tenth: enough that a
// search and an occupancy have several segments to cross, few enough that the
// writer's queue never has to drop at pace 2.
constexpr dsp::SampleIndex kSamples = test::kSourceRate * 2 / 3;

capture::Annotation annotation(std::uint64_t count, dsp::Hertz low, dsp::Hertz high,
                               const char* family) {
    capture::Annotation out;
    out.sample_count = count;
    out.freq_lower = low;
    out.freq_upper = high;
    out.confidence = 0.9;
    out.peak_db = -20.0;
    out.snr_db = 18.0;
    out.family = family;
    return out;
}

// Runs the scene to its end and leaves the writer's last segment open, which
// is where run() leaves it.
void record(Harness& harness) {
    REQUIRE(harness.start_engine().has_value());
    REQUIRE(harness.wait_for_blocks((kSamples + kBlock - 1) / kBlock, 30'000) >= (kSamples + kBlock - 1) / kBlock);
    REQUIRE(harness.stop_engine().has_value());
}

}  // namespace

TEST_CASE("a capture the engine wrote is listed, searched and measured over the wire",
          "[gpu][rpc][capture]") {
    REVENANT_NEEDS_GPU();

    test::CaptureRoot root;
    HarnessOptions options;
    options.samples = kSamples;
    options.block_samples = kBlock;
    options.pace = 2.0;
    options.capture = engine::EngineConfig::Capture{.root = root.path(),
                                                    .max_segment_bytes = 0,
                                                    .max_segment_seconds = 0.1};
    Harness harness;
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    // Asked before anything ran: the writer has not made the root's first day
    // yet, which is an empty capture and not a refusal.
    {
        const auto empty = harness.client().capture_catalog(std::nullopt, std::nullopt);
        INFO(test::message_of(empty));
        REQUIRE(empty.has_value());
        CHECK(empty->segments.empty());
    }

    record(harness);

    // Three detections, one early, one in the middle and one late, so they
    // land in different segments, two of them under one family and one off
    // on its own frequency. Filed after run() returns, as the server's
    // annotator files the tracks still alive at the end of a stream, and
    // closed onto the metas by closing the source.
    constexpr std::uint64_t kLength = 20'000;
    harness.engine().add_capture_annotation(1'000, annotation(kLength, 100'000, 110'000, "alpha"),
                                            7);
    harness.engine().add_capture_annotation(kSamples / 2,
                                            annotation(kLength, 300'000, 310'000, "beta"), 8);
    harness.engine().add_capture_annotation(kSamples - 3 * kLength,
                                            annotation(kLength, 100'000, 110'000, "alpha"), 9);
    REQUIRE(harness.client().close_source().has_value());

    const auto stats = harness.engine().capture_stats();
    REQUIRE(stats.has_value());
    INFO("blocks dropped by the writer " << stats->blocks_dropped);
    REQUIRE(stats->blocks_dropped == 0);

    // THE CATALOG. Every sample the scene produced, in segments of one epoch
    // that meet, with the three annotations counted where they landed.
    const auto catalog = harness.client().capture_catalog(std::nullopt, std::nullopt);
    INFO(test::message_of(catalog));
    REQUIRE(catalog.has_value());
    REQUIRE(catalog->segments.size() >= 3);
    CHECK(catalog->segments.size() == stats->segments_written);
    std::uint64_t samples = 0;
    std::uint32_t annotations = 0;
    std::uint64_t bytes = 0;
    for (const rpc::CaptureSegment& segment : catalog->segments) {
        samples += segment.sample_count;
        annotations += segment.annotations;
        bytes += segment.bytes;
        CHECK(segment.stream_epoch == catalog->segments.front().stream_epoch);
        CHECK(segment.sample_rate == test::kSourceRate);
        CHECK(segment.duration_ns > 0);
        CHECK_FALSE(segment.format.empty());
    }
    CHECK(samples == kSamples);
    CHECK(annotations == 3);
    CHECK(catalog->bytes == bytes);
    CHECK(catalog->root_bytes >= bytes);
    CHECK(catalog->gaps.empty());
    CHECK(catalog->in_progress == 0);
    CHECK(catalog->problems.empty());
    for (std::size_t i = 1; i < catalog->segments.size(); ++i) {
        CHECK(catalog->segments[i].first_index ==
              catalog->segments[i - 1].first_index + catalog->segments[i - 1].sample_count);
    }

    // A range that covers only the first segment lists only it.
    const rpc::CaptureSegment& first = catalog->segments.front();
    const auto opening =
        harness.client().capture_catalog(first.start_ns, first.start_ns + first.duration_ns);
    REQUIRE(opening.has_value());
    REQUIRE(opening->segments.size() == 1);
    CHECK(opening->segments.front().first_index == first.first_index);

    // THE SEARCH.
    const auto all = harness.client().capture_search(rpc::CaptureQuery{});
    INFO(test::message_of(all));
    REQUIRE(all.has_value());
    REQUIRE(all->hits.size() == 3);
    CHECK(all->matched == 3);
    CHECK(all->skipped.empty());
    CHECK(all->hits[0].track_id == 7);
    CHECK(all->hits[1].track_id == 8);
    CHECK(all->hits[2].track_id == 9);
    for (const rpc::CaptureHit& hit : all->hits) {
        CHECK(hit.end_ns > hit.start_ns);
        CHECK(hit.generator == "revenant-detect");
        CHECK(hit.replay_uri.starts_with("capture:"));
        CHECK(hit.replay_uri.find("pace=1") != std::string::npos);
    }
    // One nanosecond either way: both sides round the same exact instant.
    const std::int64_t expected_start =
        first.start_ns + 1'000LL * 1'000'000'000LL / test::kSourceRate;
    CHECK(std::abs(all->hits[0].start_ns - expected_start) <= 1);

    rpc::CaptureQuery family;
    family.family = "ALPHA";
    const auto alphas = harness.client().capture_search(family);
    REQUIRE(alphas.has_value());
    CHECK(alphas->hits.size() == 2);

    rpc::CaptureQuery band;
    band.freq_low_hz = 305'000;
    band.freq_high_hz = 400'000;
    const auto betas = harness.client().capture_search(band);
    REQUIRE(betas.has_value());
    REQUIRE(betas->hits.size() == 1);
    CHECK(betas->hits.front().family == "beta");

    rpc::CaptureQuery limited;
    limited.limit = 1;
    const auto one = harness.client().capture_search(limited);
    REQUIRE(one.has_value());
    CHECK(one->hits.size() == 1);
    CHECK(one->matched == 3);

    rpc::CaptureQuery other;
    other.generator = "revenant-index";
    const auto none = harness.client().capture_search(other);
    REQUIRE(none.has_value());
    CHECK(none->hits.empty());

    // OCCUPANCY, in ten buckets over the whole capture and the alpha band.
    const std::int64_t from = first.start_ns;
    const std::int64_t to = catalog->segments.back().start_ns + catalog->segments.back().duration_ns;
    rpc::CaptureOccupancyRequest request;
    request.from_ns = from;
    request.to_ns = to;
    request.bucket_ns = (to - from + 9) / 10;
    request.freq_low_hz = 100'000;
    request.freq_high_hz = 110'000;
    const auto buckets = harness.client().capture_occupancy(request);
    INFO(test::message_of(buckets));
    REQUIRE(buckets.has_value());
    REQUIRE(buckets->size() == 10);
    CHECK(buckets->front().start_ns == from);
    CHECK(buckets->back().end_ns == to);
    std::uint32_t hits = 0;
    for (const rpc::CaptureBucket& bucket : *buckets) {
        CHECK(bucket.captured > 0.99);
        hits += bucket.hits;
    }
    CHECK(buckets->front().occupied > 0.0);
    CHECK(buckets->front().hits == 1);
    CHECK(buckets->back().hits == 1);
    CHECK(hits == 2);

    // Refused in words, not computed.
    rpc::CaptureOccupancyRequest backwards = request;
    backwards.to_ns = from;
    CHECK_FALSE(harness.client().capture_occupancy(backwards).has_value());
    rpc::CaptureOccupancyRequest huge = request;
    huge.bucket_ns = 1;
    CHECK_FALSE(harness.client().capture_occupancy(huge).has_value());

    // THE REPLAY, opened from the hit on the engine that wrote the root. It
    // plays, and it is not recorded: the catalog afterwards is the catalog
    // before, segment for segment.
    REQUIRE(harness.client().open_source(all->hits[1].replay_uri).has_value());
    REQUIRE(harness.start_engine().has_value());
    const std::uint64_t replay_blocks = harness.wait_for_blocks(1, 30'000);
    CHECK(replay_blocks >= 1);
    REQUIRE(harness.stop_engine().has_value());
    REQUIRE(harness.client().close_source().has_value());

    const auto after = harness.client().capture_catalog(std::nullopt, std::nullopt);
    REQUIRE(after.has_value());
    CHECK(after->segments.size() == catalog->segments.size());
    CHECK(after->bytes == catalog->bytes);
    const auto after_stats = harness.engine().capture_stats();
    REQUIRE(after_stats.has_value());
    CHECK(after_stats->segments_written == stats->segments_written);
}

TEST_CASE("an engine keeping no capture refuses the capture calls in words",
          "[gpu][rpc][capture]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    const auto ready = harness.open(HarnessOptions{});
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    const auto catalog = harness.client().capture_catalog(std::nullopt, std::nullopt);
    REQUIRE_FALSE(catalog.has_value());
    CHECK(catalog.error().message.find("--capture") != std::string::npos);

    const auto search = harness.client().capture_search(rpc::CaptureQuery{});
    REQUIRE_FALSE(search.has_value());
    CHECK(search.error().message.find("--capture") != std::string::npos);

    rpc::CaptureOccupancyRequest request;
    request.from_ns = 0;
    request.to_ns = 1'000'000'000;
    request.bucket_ns = 100'000'000;
    const auto buckets = harness.client().capture_occupancy(request);
    REQUIRE_FALSE(buckets.has_value());
    CHECK(buckets.error().message.find("--capture") != std::string::npos);
}

TEST_CASE("a retune a replay follows is drained with nothing subscribed",
          "[gpu][rpc][capture]") {
    REVENANT_NEEDS_GPU();

    // The server drained take_followed_retunes on its pump, and only spectrum,
    // audio and decoded traffic rang the pump. With none of it the retune sat
    // in the engine for the life of the stream: nothing told a client that
    // receivers had gone. The engine now rings the pump itself, so the list
    // is empty here because the server took it, not because the engine never
    // filled it, which the centre having moved says.
    constexpr dsp::Hertz kMarine = 156'800'000;
    constexpr dsp::Hertz kWeather = 162'550'000;
    constexpr dsp::SampleRate kReplayRate = 2'048'000;
    constexpr std::uint64_t kSegment = 1u << 19;

    test::CaptureRoot root;
    test::SegmentSpec first;
    first.rate = kReplayRate;
    first.center = kMarine;
    first.sample_count = kSegment;
    test::write_segment(root.path(), first);
    test::SegmentSpec second = first;
    second.first_index = kSegment;
    second.center = kWeather;
    test::write_segment(root.path(), second);

    HarnessOptions options;
    options.source_uri = "capture:///" + root.utf8();
    options.channels = test::kChannels;
    Harness harness;
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    REQUIRE(harness.start_engine().has_value());
    REQUIRE(harness.wait_for_blocks(2 * kSegment / options.block_samples, 30'000) >= 2 * kSegment / options.block_samples);
    REQUIRE(harness.stop_engine().has_value());
    REQUIRE(harness.engine().info().source_center == kWeather);

    // Long enough for a woken pump many times over, and the list is looked
    // at only after it.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    CHECK(harness.engine().take_followed_retunes().empty());
}
