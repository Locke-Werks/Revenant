// Search over a stored capture, and the index pass that feeds it.
//
// Three kinds of case. The query and occupancy cases run over annotations
// made by hand, so each asserts against arithmetic rather than against a
// detector. The merge cases hold write_annotations to its promise of leaving
// everything it does not own alone. The index case is the only one that needs
// a device: it renders a scene with known carriers and bursts into a SigMF
// segment laid out the way core/capture/format.h says, indexes it through the
// engine, and checks the bursts come back at the right times and
// frequencies.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <sstream>
#include <string>
#include <vector>

#include "core/capture/format.h"
#include "core/capture/index.h"
#include "core/capture/search.h"
#include "core/dsp/synth/modulators.h"
#include "core/dsp/synth/wideband.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/support/temp_path.h"

using namespace revenant;
using capture::Annotation;
using capture::IndexedSegment;
using capture::SegmentInfo;
using capture::StoredAnnotation;

namespace {

constexpr std::int64_t kSecond = 1'000'000'000;

// 2026-10-12T00:00:00Z.
constexpr std::int64_t kAnchorNs = 1'760'227'200LL * kSecond;

template <typename T>
[[nodiscard]] std::string message_of(const T& result) {
    return result.has_value() ? std::string() : result.error().message;
}

[[nodiscard]] SegmentInfo segment_info(dsp::SampleIndex first_index, std::uint64_t count,
                                       dsp::SampleRate rate = 1'000'000) {
    SegmentInfo info;
    info.stream_epoch = 0x1234;
    info.first_index = first_index;
    info.sample_count = count;
    info.epoch_anchor_ns = kAnchorNs;
    info.rate = rate;
    info.center = 146'000'000;
    info.format = source::SampleFormat::Cf32;
    return info;
}

[[nodiscard]] StoredAnnotation stored(std::uint64_t start, std::uint64_t count, dsp::Hertz low,
                                      dsp::Hertz high, double confidence, double snr,
                                      std::string family = {},
                                      std::string generator = "revenant-index") {
    StoredAnnotation out;
    out.annotation.sample_start = start;
    out.annotation.sample_count = count;
    out.annotation.freq_lower = low;
    out.annotation.freq_upper = high;
    out.annotation.confidence = confidence;
    out.annotation.snr_db = snr;
    out.annotation.peak_db = -40.0;
    out.annotation.family = std::move(family);
    out.generator = std::move(generator);
    return out;
}

// Two segments of ten seconds at 1 MS/s, back to back in one epoch, the
// second starting at stream second 10. Every annotation's wall clock is
// therefore the anchor plus its stream second.
[[nodiscard]] std::vector<IndexedSegment> two_segments() {
    std::vector<IndexedSegment> segments(2);
    segments[0].data_path = "a.sigmf-data";
    segments[0].info = segment_info(0, 10'000'000);
    segments[0].annotations = {
        stored(1'000'000, 2'000'000, 146'010'000, 146'030'000, 0.9, 20.0, "NFM"),
        stored(5'000'000, 500'000, 146'500'000, 146'512'500, 0.6, 8.0, "P25 voice"),
        stored(1'000'000, 2'000'000, 146'010'000, 146'030'000, 0.95, 21.0, "NFM",
               "revenant-detect"),
    };
    segments[1].data_path = "b.sigmf-data";
    segments[1].info = segment_info(10'000'000, 10'000'000);
    segments[1].annotations = {
        stored(0, 4'000'000, 146'010'000, 146'030'000, 0.3, 4.0),
        stored(2'000'000, 1'000'000, 145'000'000, 145'100'000, 0.99, 30.0, "DMR"),
    };
    return segments;
}

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return std::move(text).str();
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

struct TempFiles {
    std::vector<std::filesystem::path> paths;
    TempFiles() = default;
    TempFiles(const TempFiles&) = delete;
    TempFiles& operator=(const TempFiles&) = delete;
    ~TempFiles() {
        for (const auto& path : paths) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------

TEST_CASE("an empty query returns every annotation in time order", "[capture][search]") {
    const auto segments = two_segments();
    const auto hits = capture::run_query(segments, {});
    REQUIRE(hits.size() == 5);
    for (std::size_t i = 1; i < hits.size(); ++i) {
        CHECK(hits[i - 1].start_ns <= hits[i].start_ns);
    }
    CHECK(hits.front().start_ns == kAnchorNs + 1 * kSecond);
    CHECK(hits.back().start_ns == kAnchorNs + 12 * kSecond);
    CHECK(hits.back().data_path == "b.sigmf-data");
    CHECK(hits.back().end_ns == kAnchorNs + 13 * kSecond);
    CHECK(hits.back().sample_start == 2'000'000);
    CHECK(hits.back().first_index == 10'000'000);
    CHECK(hits.back().stream_epoch == 0x1234);
}

TEST_CASE("time and frequency are overlap tests", "[capture][search]") {
    const auto segments = two_segments();

    capture::Query query;
    // 2.5 s to 2.6 s catches the NFM pair, which runs 1 s to 3 s, and
    // nothing else.
    query.from_ns = kAnchorNs + 2'500'000'000;
    query.to_ns = kAnchorNs + 2'600'000'000;
    auto hits = capture::run_query(segments, query);
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].family == "NFM");

    // An annotation ending exactly at from_ns is not inside the range, and
    // one starting exactly at to_ns is not either.
    query.from_ns = kAnchorNs + 3 * kSecond;
    query.to_ns = kAnchorNs + 5 * kSecond;
    CHECK(capture::run_query(segments, query).empty());

    // A band that touches only the DMR annotation's upper edge.
    capture::Query band;
    band.freq_low = 145'100'000;
    band.freq_high = 145'200'000;
    hits = capture::run_query(segments, band);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].family == "DMR");
}

TEST_CASE("measure filters exclude what does not pass", "[capture][search]") {
    const auto segments = two_segments();

    capture::Query query;
    query.min_confidence = 0.5;
    CHECK(capture::run_query(segments, query).size() == 4);

    query = {};
    query.min_snr_db = 10.0;
    CHECK(capture::run_query(segments, query).size() == 3);

    query = {};
    query.min_duration_ns = 2 * kSecond;
    CHECK(capture::run_query(segments, query).size() == 3);

    query = {};
    query.family = "p25";
    auto hits = capture::run_query(segments, query);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].family == "P25 voice");

    query = {};
    query.generator = "revenant-detect";
    hits = capture::run_query(segments, query);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].confidence == 0.95);
}

TEST_CASE("occupancy counts overlapping presence once and reports coverage",
          "[capture][search]") {
    auto segments = two_segments();

    capture::OccupancyRequest request;
    request.from_ns = kAnchorNs;
    // Past the end of the second segment, so the last bucket is not captured.
    request.to_ns = kAnchorNs + 25 * kSecond;
    request.bucket_ns = 5 * kSecond;
    request.freq_low = 146'000'000;
    request.freq_high = 146'040'000;

    auto buckets = capture::occupancy(segments, request);
    INFO(message_of(buckets));
    REQUIRE(buckets.has_value());
    REQUIRE(buckets->size() == 5);

    // 0-5 s: the NFM pair, 1 s to 3 s, counted once although two generators
    // saw it.
    CHECK((*buckets)[0].occupied == Catch::Approx(0.4));
    CHECK((*buckets)[0].hits == 2);
    CHECK((*buckets)[0].captured == Catch::Approx(1.0));
    // 5-10 s: quiet in this band, and recorded.
    CHECK((*buckets)[1].occupied == 0.0);
    CHECK((*buckets)[1].captured == Catch::Approx(1.0));
    // 10-15 s: the weak one in the second segment, 10 s to 14 s.
    CHECK((*buckets)[2].occupied == Catch::Approx(0.8));
    // 20-25 s: nothing recorded, which is a different answer from quiet.
    CHECK((*buckets)[4].occupied == 0.0);
    CHECK((*buckets)[4].captured == 0.0);

    // The filter applies before anything counts.
    request.filter.min_confidence = 0.5;
    buckets = capture::occupancy(segments, request);
    REQUIRE(buckets.has_value());
    CHECK((*buckets)[2].occupied == 0.0);

    // A bucket that does not divide the range is cut short at the end.
    request.bucket_ns = 10 * kSecond;
    buckets = capture::occupancy(segments, request);
    REQUIRE(buckets.has_value());
    REQUIRE(buckets->size() == 3);
    CHECK(buckets->back().end_ns == request.to_ns);

    request.bucket_ns = 0;
    CHECK_FALSE(capture::occupancy(segments, request).has_value());
}

// ---------------------------------------------------------------------------
// The meta merge
// ---------------------------------------------------------------------------

namespace {

// A meta with a key from an extension nobody here knows, a number written at
// a precision a re-print would change, and one entry from each generator.
constexpr std::string_view kMeta = R"({
    "global": {
        "core:datatype": "cf32_le",
        "core:sample_rate": 2400000.0,
        "core:version": "1.0.0",
        "core:extensions": [{"name": "revenant", "version": "1.0.0", "optional": true}],
        "revenant:stream_epoch": 4660,
        "someone_else:weird": {"nested": [1, 2.50000, "a \"quoted\" é"]}
    },
    "captures": [{"core:sample_start": 0, "core:frequency": 146000000, "core:datetime": "2026-10-12T00:00:00.000Z"}],
    "annotations": [
        {"core:sample_start": 500, "core:sample_count": 10, "core:freq_lower_edge": 1.0e8, "core:freq_upper_edge": 100010000, "core:generator": "revenant-detect", "revenant:track_id": 7, "revenant:confidence": 0.75},
        {"core:sample_start": 100, "core:sample_count": 10, "core:freq_lower_edge": 1, "core:freq_upper_edge": 2, "core:generator": "revenant-index"},
        {"core:sample_start": 900, "core:comment": "a person wrote this"}
    ]
}
)";

}  // namespace

TEST_CASE("re-indexing replaces only the index's own annotations", "[capture][merge]") {
    Annotation added;
    added.sample_start = 700;
    added.sample_count = 50;
    added.freq_lower = 146'010'000;
    added.freq_upper = 146'030'000;
    added.confidence = 0.875;
    added.peak_db = -31.5;
    added.snr_db = 18.25;
    added.family = "NFM";
    const Annotation one[] = {added};

    auto merged = capture::merge_annotations(kMeta, one, capture::kIndexGenerator);
    INFO(message_of(merged));
    REQUIRE(merged.has_value());

    // Everything before the annotations array, byte for byte.
    const std::size_t head = kMeta.find("\"annotations\"");
    CHECK(merged->substr(0, head) == kMeta.substr(0, head));
    // The kept entries verbatim, including the precision and the unknown key.
    CHECK(merged->find(R"("core:freq_lower_edge": 1.0e8)") != std::string::npos);
    CHECK(merged->find(R"("core:comment": "a person wrote this")") != std::string::npos);

    const std::filesystem::path path = test::unique_temp_path("revenant_search_meta", ".sigmf-meta");
    TempFiles files;
    files.paths.push_back(path);
    write_text(path, *merged);
    auto read = capture::read_annotations(path.string());
    INFO(message_of(read));
    REQUIRE(read.has_value());
    REQUIRE(read->size() == 3);

    // Ordered by start: the detector's at 500, the new one at 700, the
    // person's at 900. The old index entry at 100 is gone.
    CHECK((*read)[0].generator == "revenant-detect");
    CHECK((*read)[0].track_id == 7);
    CHECK((*read)[0].annotation.freq_lower == 100'000'000);
    CHECK((*read)[0].annotation.confidence == 0.75);
    CHECK((*read)[1].generator == "revenant-index");
    CHECK((*read)[1].annotation.sample_start == 700);
    CHECK((*read)[1].annotation.sample_count == 50);
    CHECK((*read)[1].annotation.freq_upper == 146'030'000);
    CHECK((*read)[1].annotation.confidence == 0.875);
    CHECK((*read)[1].annotation.peak_db == -31.5);
    CHECK((*read)[1].annotation.snr_db == 18.25);
    CHECK((*read)[1].annotation.family == "NFM");
    CHECK((*read)[2].generator.empty());
    CHECK((*read)[2].annotation.sample_start == 900);

    // Running the same merge again changes nothing.
    auto again = capture::merge_annotations(*merged, one, capture::kIndexGenerator);
    REQUIRE(again.has_value());
    CHECK(*again == *merged);
}

TEST_CASE("a meta with no annotations gets an array and keeps the rest", "[capture][merge]") {
    constexpr std::string_view kBare =
        "{\n    \"global\": {\"core:datatype\": \"cu8\", \"x:y\": 1.50},\n    \"captures\": []\n}\n";

    Annotation added;
    added.sample_start = 3;
    added.sample_count = 4;
    added.freq_lower = 5;
    added.freq_upper = 6;
    added.confidence = 0.5;
    added.snr_db = std::nan("");
    const Annotation one[] = {added};

    const std::filesystem::path path = test::unique_temp_path("revenant_search_bare", ".sigmf-meta");
    TempFiles files;
    files.paths.push_back(path);
    write_text(path, kBare);

    const auto written = capture::write_annotations(path.string(), one);
    INFO(message_of(written));
    REQUIRE(written.has_value());
    CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));

    const std::string text = read_text(path);
    CHECK(text.starts_with("{\n    \"global\": {\"core:datatype\": \"cu8\", \"x:y\": 1.50},\n"
                           "    \"captures\": []"));
    // Not a number, so not written.
    CHECK(text.find("revenant:snr_db") == std::string::npos);
    CHECK(text.find("\"core:label\": \"signal\"") != std::string::npos);

    auto read = capture::read_annotations(path.string());
    INFO(message_of(read));
    REQUIRE(read.has_value());
    REQUIRE(read->size() == 1);
    CHECK((*read)[0].annotation.sample_count == 4);

    // Replacing with nothing leaves an empty array, still valid.
    REQUIRE(capture::write_annotations(path.string(), {}).has_value());
    read = capture::read_annotations(path.string());
    REQUIRE(read.has_value());
    CHECK(read->empty());
}

TEST_CASE("a malformed meta is refused and left untouched", "[capture][merge]") {
    const std::filesystem::path path = test::unique_temp_path("revenant_search_bad", ".sigmf-meta");
    TempFiles files;
    files.paths.push_back(path);
    constexpr std::string_view kBad = "{\"global\": {\"core:datatype\": \"cu8\"}, \"annotations\": [";
    write_text(path, kBad);

    const auto written = capture::write_annotations(path.string(), {});
    CHECK_FALSE(written.has_value());
    CHECK(read_text(path) == kBad);
    CHECK_FALSE(capture::read_annotations(path.string()).has_value());
}

// ---------------------------------------------------------------------------
// The index pass, end to end
// ---------------------------------------------------------------------------

namespace {

constexpr dsp::SampleRate kRate = 2'400'000;
constexpr dsp::Hertz kCentre = 146'000'000;
constexpr double kSceneSeconds = 12.0;

// Stream index of the segment's first sample. Not zero, so a pass that
// confused the stream's index with the segment's would be off by this much.
constexpr dsp::SampleIndex kFirstIndex = 3 * kRate;

[[nodiscard]] siggen::ModulatorSpec qpsk(double symbol_rate, dsp::Hertz offset,
                                         std::uint64_t seed) {
    siggen::ModulatorSpec spec;
    spec.kind = siggen::Modulation::Qpsk;
    spec.common.rate = kRate;
    spec.common.carrier_offset = offset;
    spec.common.seed = seed;
    spec.psk.symbol_rate = symbol_rate;
    spec.psk.rolloff = 0.35;
    spec.psk.symbol_count = 4096;
    return spec;
}

struct Burst {
    dsp::Hertz offset;
    double start_s;
    double end_s;
};

// A carrier on for the whole scene, one emitter keyed twice, and a wider one
// keyed once. The keyed emitter's gap is four seconds, longer than the
// detector's hold plus the time its average takes to fall, so the two
// transmissions are two spells and not one.
constexpr dsp::Hertz kCarrierOffset = -700'000;
constexpr Burst kBursts[] = {
    {300'000, 2.0, 4.0},
    {300'000, 8.0, 9.5},
    {-150'000, 5.0, 7.0},
};

[[nodiscard]] std::int64_t iso_seconds_ns(dsp::SampleIndex index) {
    return dsp::BlockTimestamp{index, kAnchorNs, kRate}.wall_clock_ns();
}

[[nodiscard]] std::string iso8601(std::int64_t ns) {
    const auto time = std::chrono::sys_time<std::chrono::nanoseconds>(std::chrono::nanoseconds(ns));
    return std::format("{:%Y-%m-%dT%H:%M:%S}Z",
                       std::chrono::floor<std::chrono::milliseconds>(time));
}

// The segment as format.h lays it out, minus the writer: the data file and a
// meta carrying the core keys and the revenant global keys. The writer is not
// in this tree yet, so this is the contract's own description of the file.
[[nodiscard]] std::string segment_meta(const SegmentInfo& info) {
    return std::format(
        R"({{
    "global": {{
        "core:datatype": "cf32_le",
        "core:sample_rate": {},
        "core:version": "1.2.0",
        "core:recorder": "Revenant test",
        "core:hw": "siggen scene",
        "core:extensions": [{{"name": "revenant", "version": "1.0.0", "optional": true}}],
        "revenant:stream_epoch": {},
        "revenant:first_index": {},
        "revenant:epoch_anchor_ns": {},
        "revenant:dropped_before": 0,
        "revenant:close_reason": "stop"
    }},
    "captures": [
        {{"core:sample_start": 0, "core:frequency": {}, "core:datetime": "{}"}}
    ],
    "annotations": []
}}
)",
        info.rate, info.stream_epoch, info.first_index, info.epoch_anchor_ns, info.center,
        iso8601(info.start_ns()));
}

}  // namespace

TEST_CASE("indexing a segment finds its bursts at the right times and frequencies",
          "[gpu][capture][index]") {
    REVENANT_NEEDS_GPU();

    siggen::SceneSpec spec;
    spec.rate = kRate;
    spec.center_hz = kCentre;
    spec.duration_samples = static_cast<dsp::SampleIndex>(kSceneSeconds * kRate);
    spec.seed = 20261012;
    spec.noise_power_full_band_dbfs = -60.0;
    {
        siggen::EmitterPlacement carrier;
        carrier.modulator = qpsk(20'000.0, kCarrierOffset, 1);
        carrier.snr_in_occupied_bandwidth_db = 20.0;
        spec.emitters.push_back(carrier);
    }
    for (std::size_t i = 0; i < std::size(kBursts); ++i) {
        siggen::EmitterPlacement burst;
        burst.modulator = qpsk(i == 2 ? 56'000.0 : 20'000.0, kBursts[i].offset, 2 + i);
        burst.snr_in_occupied_bandwidth_db = 20.0;
        burst.start_sample = static_cast<dsp::SampleIndex>(kBursts[i].start_s * kRate);
        burst.end_sample = static_cast<dsp::SampleIndex>(kBursts[i].end_s * kRate);
        spec.emitters.push_back(burst);
    }
    auto scene = siggen::Scene::create(spec);
    INFO(message_of(scene));
    REQUIRE(scene.has_value());

    SegmentInfo info;
    info.stream_epoch = 0x00c0ffee00000001;
    info.first_index = kFirstIndex;
    info.sample_count = spec.duration_samples;
    info.epoch_anchor_ns = kAnchorNs;
    info.rate = kRate;
    info.center = kCentre;
    info.format = source::SampleFormat::Cf32;

    TempFiles files;
    const std::filesystem::path stem = test::unique_temp_path("rev-index-test");
    const std::filesystem::path data = stem.string() + std::string(capture::kDataExtension);
    const std::filesystem::path meta = stem.string() + std::string(capture::kMetaExtension);
    files.paths = {data, meta};
    {
        std::ofstream out(data, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        std::vector<std::complex<float>> block(1 << 20);
        for (dsp::SampleIndex at = 0; at < spec.duration_samples; at += block.size()) {
            const auto count = std::min<dsp::SampleIndex>(block.size(), spec.duration_samples - at);
            scene->render(at, dsp::ComplexSpan(block.data(), static_cast<std::size_t>(count)));
            out.write(reinterpret_cast<const char*>(block.data()),
                      static_cast<std::streamsize>(count * sizeof(std::complex<float>)));
        }
        REQUIRE(out.good());
    }
    write_text(meta, segment_meta(info));

    auto indexed = capture::index_segment(data.string(), info);
    INFO(message_of(indexed));
    REQUIRE(indexed.has_value());

    std::println("index: {:.2f} s of capture in {:.2f} s of wall, {:.1f}x realtime, "
                 "{} annotations",
                 indexed->capture_seconds, indexed->wall_seconds,
                 indexed->capture_seconds / indexed->wall_seconds, indexed->annotations.size());
    for (const Annotation& a : indexed->annotations) {
        std::println("  {:7.3f} s to {:7.3f} s  {:+9} to {:+9} Hz  conf {:.2f} snr {:5.1f} "
                     "peak {:6.1f} {}",
                     static_cast<double>(a.sample_start) / kRate,
                     static_cast<double>(a.sample_start + a.sample_count) / kRate,
                     a.freq_lower - kCentre, a.freq_upper - kCentre, a.confidence, a.snr_db,
                     a.peak_db, a.family);
    }
    CHECK(indexed->capture_seconds == Catch::Approx(kSceneSeconds).margin(0.1));

    // Through the meta and back, as a catalog would hand it to a query.
    REQUIRE(capture::write_annotations(meta.string(), indexed->annotations).has_value());
    auto read = capture::read_annotations(meta.string());
    INFO(message_of(read));
    REQUIRE(read.has_value());
    REQUIRE(read->size() == indexed->annotations.size());
    std::vector<IndexedSegment> segments(1);
    segments[0].data_path = data.string();
    segments[0].info = info;
    segments[0].annotations = std::move(*read);

    // Each burst is one hit in its own band, starting within a second of when
    // it keyed (the detector integrates over one) and ending no earlier than
    // it unkeyed less a decision, and no later than the average's fall allows.
    for (const Burst& burst : kBursts) {
        const dsp::Hertz frequency = kCentre + burst.offset;
        capture::Query query;
        query.freq_low = frequency;
        query.freq_high = frequency;
        query.from_ns = iso_seconds_ns(kFirstIndex + static_cast<dsp::SampleIndex>(burst.start_s * kRate));
        query.to_ns = iso_seconds_ns(kFirstIndex + static_cast<dsp::SampleIndex>(burst.end_s * kRate));
        const auto hits = capture::run_query(segments, query);
        INFO(std::format("burst at {:+} Hz, {} s to {} s", burst.offset, burst.start_s,
                         burst.end_s));
        REQUIRE(hits.size() == 1);
        const capture::Hit& hit = hits.front();
        const double start_s = static_cast<double>(hit.start_ns - info.start_ns()) / kSecond;
        const double end_s = static_cast<double>(hit.end_ns - info.start_ns()) / kSecond;
        INFO(std::format("found {:.3f} s to {:.3f} s", start_s, end_s));
        CHECK(start_s >= burst.start_s);
        CHECK(start_s <= burst.start_s + 1.0);
        CHECK(end_s >= burst.end_s - 0.2);
        CHECK(end_s <= burst.end_s + 3.0);
        CHECK(hit.freq_lower <= frequency);
        CHECK(hit.freq_upper >= frequency);
        CHECK(hit.freq_upper - hit.freq_lower < 120'000);
        CHECK(hit.snr_db > 10.0);
        CHECK(hit.generator == capture::kIndexGenerator);
    }

    // The carrier is one hit spanning nearly the whole segment.
    capture::Query carrier;
    carrier.freq_low = kCentre + kCarrierOffset;
    carrier.freq_high = kCentre + kCarrierOffset;
    const auto carrier_hits = capture::run_query(segments, carrier);
    REQUIRE(carrier_hits.size() == 1);
    CHECK(carrier_hits[0].end_ns - carrier_hits[0].start_ns > 10 * kSecond);

    // Nothing anywhere the scene put nothing.
    capture::Query empty;
    empty.freq_low = kCentre + 500'000;
    empty.freq_high = kCentre + 600'000;
    CHECK(capture::run_query(segments, empty).empty());

    // The keyed emitter's band, bucketed by second: busy while it was keyed,
    // and idle through the middle of its gap.
    capture::OccupancyRequest request;
    request.from_ns = info.start_ns();
    request.to_ns = info.start_ns() + static_cast<std::int64_t>(kSceneSeconds) * kSecond;
    request.bucket_ns = kSecond;
    request.freq_low = kCentre + 290'000;
    request.freq_high = kCentre + 310'000;
    auto buckets = capture::occupancy(segments, request);
    REQUIRE(buckets.has_value());
    REQUIRE(buckets->size() == 12);
    CHECK((*buckets)[3].occupied == Catch::Approx(1.0));
    CHECK((*buckets)[0].occupied == 0.0);
    CHECK((*buckets)[7].occupied == 0.0);
    CHECK((*buckets)[8].occupied > 0.5);
    for (const capture::OccupancyBucket& bucket : *buckets) {
        CHECK(bucket.captured == Catch::Approx(1.0));
    }

    // Indexing again replaces, rather than adds to, what the first pass wrote.
    REQUIRE(capture::write_annotations(meta.string(), indexed->annotations).has_value());
    auto reread = capture::read_annotations(meta.string());
    REQUIRE(reread.has_value());
    CHECK(reread->size() == indexed->annotations.size());
}
