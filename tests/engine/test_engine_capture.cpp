// The rolling capture wired into the engine: a stream retuned half way through
// is two segments, at the two centres, whose indices meet, and a detection
// handed to the engine lands in the meta of the segment it was heard in.
//
// tests/capture/test_writer.cpp covers the writer on blocks it is fed by hand.
// What it cannot cover is the part this file is about: that the engine's sink
// attributes each block to the centre it was really taken at, from its own
// record of the retune rather than by asking the source, and that the last
// segment stays open after run() so tracks alive at the end of the stream can
// still be written onto it.
//
// The front end is tests/engine/movable_centre.h around a synthetic scene, so
// the retune is the engine's real set_source_center on a source with no radio
// behind it.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <mutex>
#include <fstream>
#include <memory>
#include <cstdlib>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/capture/format.h"
#include "core/capture/track_annotations.h"
#include "core/capture/writer.h"
#include "core/detect/detector.h"
#include "core/engine/engine.h"
#include "core/engine/open_built_source.h"
#include "core/source/capabilities.h"
#include "core/source/registry.h"
#include "tests/capture/catalog_fixture.h"
#include "tests/engine/movable_centre.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;
namespace fs = std::filesystem;

namespace {

constexpr dsp::SampleRate kRate = 2'000'000;
constexpr dsp::Hertz kOpenedAt = 100'000'000;
constexpr dsp::Hertz kRetunedTo = 101'500'000;

class Scratch {
public:
    Scratch()
    {
        std::random_device entropy;
        root_ = fs::temp_directory_path() /
                std::format("revenant_engine_capture_{:08x}{:08x}", entropy(), entropy());
        fs::create_directories(root_);
    }
    ~Scratch()
    {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    [[nodiscard]] const fs::path& root() const { return root_; }

private:
    fs::path root_;
};

struct Segment {
    capture::StemFields stem;
    std::uint64_t bytes = 0;
    std::string meta;
};

[[nodiscard]] std::vector<Segment> list_segments(const fs::path& root)
{
    std::vector<Segment> out;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const fs::path& path = entry.path();
        if (!path.filename().string().ends_with(capture::kMetaExtension)) {
            continue;
        }
        auto stem = capture::parse_stem(path.stem().string());
        REQUIRE(stem.has_value());
        fs::path data = path;
        data.replace_extension(capture::kDataExtension);
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        out.push_back(Segment{*stem, fs::file_size(data), text.str()});
    }
    std::ranges::sort(out, {}, [](const Segment& s) { return s.stem.first_index; });
    return out;
}

[[nodiscard]] bool has(const Segment& segment, std::string_view needle)
{
    return segment.meta.find(needle) != std::string::npos;
}

// A track as the detector would leave it, at an absolute frequency, heard from
// `first` to `last`.
[[nodiscard]] detect::Track track_at(std::uint64_t id, dsp::Hertz center, dsp::SampleIndex first,
                                     dsp::SampleIndex last)
{
    detect::Track track;
    track.id = id;
    track.state = detect::TrackState::Live;
    track.center = center;
    track.bandwidth = 20'000;
    track.snr_2500_db = 18.0;
    track.confidence = 0.9;
    track.first_seen = first;
    track.last_seen = last;
    return track;
}

}  // namespace

TEST_CASE("a retune mid-stream closes the capture's segment at the sample it took effect",
          "[gpu][engine][capture]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    Scratch scratch;

    engine::EngineConfig config;
    config.channels = 64;
    config.ring_seconds = 0.5;
    config.gpu_index = -1;
    // Two seconds of source in about one of wall clock, so the retune below
    // lands in the middle of a stream that is still running.
    config.pace = 2.0;
    engine::EngineConfig::Capture capture_config;
    capture_config.root = scratch.root();
    config.capture = capture_config;

    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;

    auto scene = source::open_source(std::format(
        "synthetic:wideband?rate={}&center={}&emitters=2&samples={}&seed=20261012", kRate,
        kOpenedAt, 2 * kRate));
    INFO(test::message_of(scene));
    REQUIRE(scene.has_value());
    REQUIRE(engine::open_built_source(eng, std::make_unique<test::MovableCentre>(
                                               std::move(*scene)))
                .has_value());

    // Fed the way revenant-cli and the RPC server feed it, through the
    // annotator, so the conversion from a track is what is under test.
    capture::TrackAnnotator annotator(
        [&eng](dsp::SampleIndex start, const capture::Annotation& annotation,
               std::uint64_t track_id) { eng.add_capture_annotation(start, annotation, track_id); });

    Status ran;
    std::thread runner([&] { ran = eng.run(); });

    // A track heard early on, ended before the retune so its segment is still
    // open when the annotation arrives.
    while (eng.source_stats().samples_delivered < kRate / 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const detect::Track early = track_at(7, kOpenedAt + 250'000, 1000, 1000 + kRate / 4);
    annotator.observe(std::span(&early, 1), 0.5);
    annotator.observe({}, 0.5);
    REQUIRE(annotator.sent() == 1);

    auto retuned = eng.set_source_center(kRetunedTo);
    INFO(test::message_of(retuned));
    REQUIRE(retuned.has_value());
    const dsp::SampleIndex retuned_near = eng.source_stats().samples_delivered;

    runner.join();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    // Still alive at the end of the stream: finish() hands it over after
    // run() has returned, which only lands because run() left the segment
    // open.
    const dsp::SampleIndex end = eng.source_stats().samples_delivered;
    const detect::Track late = track_at(9, kRetunedTo - 400'000, end - kRate / 8, end - 1);
    annotator.observe(std::span(&late, 1), 0.5);
    annotator.finish();
    REQUIRE(annotator.sent() == 2);

    eng.close_capture();
    const auto stats = eng.capture_stats();
    REQUIRE(stats.has_value());
    INFO("last error: " << stats->last_error);
    CHECK(stats->last_error.empty());
    CHECK(stats->blocks_dropped == 0);
    CHECK(stats->annotations_discarded == 0);
    CHECK(stats->segments_written == 2);

    const std::vector<Segment> segments = list_segments(scratch.root());
    REQUIRE(segments.size() == 2);

    const std::size_t sample_bytes = source::bytes_per_sample(eng.source_capabilities().native_format);
    REQUIRE(sample_bytes > 0);

    const Segment& before = segments[0];
    const Segment& after = segments[1];
    CHECK(before.stem.center == kOpenedAt);
    CHECK(after.stem.center == kRetunedTo);
    CHECK(before.stem.first_index == 0);
    CHECK(before.stem.stream_epoch == after.stem.stream_epoch);

    // Contiguous: the second starts on the sample after the first ends, and
    // together they are the whole stream.
    const std::uint64_t before_samples = before.bytes / sample_bytes;
    const std::uint64_t after_samples = after.bytes / sample_bytes;
    CHECK(after.stem.first_index == before_samples);
    CHECK(before_samples + after_samples == end);
    CHECK(has(before, "\"revenant:close_reason\":\"retune\""));
    CHECK(has(after, "\"revenant:close_reason\":\"stop\""));
    CHECK(has(after, "\"revenant:dropped_before\":0"));

    // The boundary is where the retune was made, to within the blocks that
    // were already past the sink when set_source_center returned.
    const auto boundary = static_cast<std::int64_t>(after.stem.first_index);
    CHECK(std::abs(boundary - static_cast<std::int64_t>(retuned_near)) <=
          static_cast<std::int64_t>(kRate / 4));

    // Each detection in the meta of the segment it was heard in, with its
    // edges, its track id and a start relative to that segment.
    CHECK(has(before, "\"revenant:track_id\":7"));
    CHECK(has(before, "\"core:sample_start\":1000"));
    CHECK(has(before, std::format("\"core:freq_lower_edge\":{}", kOpenedAt + 240'000)));
    CHECK(has(before, std::format("\"core:freq_upper_edge\":{}", kOpenedAt + 260'000)));
    CHECK(!has(before, "\"revenant:track_id\":9"));

    CHECK(has(after, "\"revenant:track_id\":9"));
    CHECK(has(after, std::format("\"core:sample_start\":{}",
                                 (end - kRate / 8) - after.stem.first_index)));
    CHECK(!has(after, "\"revenant:track_id\":7"));
}

TEST_CASE("an engine without a capture reports none and drops annotations",
          "[gpu][engine][capture]") {
    REVENANT_NEEDS_GPU();

    engine::EngineConfig config;
    config.channels = 64;
    config.ring_seconds = 0.5;
    config.gpu_index = -1;
    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;
    REQUIRE(eng.open_source(std::format("synthetic:wideband?rate={}&emitters=0&samples={}", kRate,
                                        kRate / 4))
                .has_value());

    eng.add_capture_annotation(0, capture::Annotation{}, 1);
    REQUIRE(eng.run().has_value());
    eng.close_capture();
    CHECK_FALSE(eng.capture_stats().has_value());
}

TEST_CASE("a capture replay follows a retune between segments at the sample it was made",
          "[gpu][engine][capture]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // A marine channel, then NOAA weather: further apart than the span, so a
    // receiver on the first cannot come along and is removed, which is the
    // retune's work visibly landing.
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

    engine::EngineConfig config;
    config.channels = 64;
    config.ring_seconds = 0.5;
    config.gpu_index = -1;
    config.spectrum_transform = 2048;
    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;

    auto opened = eng.open_source("capture:///" + root.utf8() + "?pace=max");
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());
    REQUIRE(eng.info().source_center == kMarine);
    REQUIRE(eng.source_capabilities().length_samples == 2 * kSegment);

    engine::VrxParams params;
    params.center = 100'000;
    params.demod = engine::Demod::Nfm;
    params.squelch_dbfs = -300.0;
    auto added = eng.add_vrx(params);
    INFO(test::message_of(added));
    REQUIRE(added.has_value());

    std::mutex lock;
    dsp::SampleIndex last_frame_end = 0;
    REQUIRE(eng.set_spectrum_sink([&](const engine::SpectrumFrame& frame) -> Status {
                   const std::scoped_lock guard(lock);
                   last_frame_end = std::max(last_frame_end, frame.start + frame.count);
                   return {};
               })
                .has_value());

    const Status ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    // The whole of both segments, not the first alone.
    CHECK(eng.source_stats().samples_delivered == 2 * kSegment);
    {
        const std::scoped_lock guard(lock);
        CHECK(last_frame_end > kSegment);
    }

    CHECK(eng.info().source_center == kWeather);
    const std::vector<engine::SourceRetune> followed = eng.take_followed_retunes();
    REQUIRE(followed.size() == 1);
    CHECK(followed[0].center == kWeather);
    CHECK(followed[0].at == kSegment);
    REQUIRE(followed[0].removed.size() == 1);
    CHECK(followed[0].removed[0].id == *added);
    CHECK(followed[0].removed[0].cause == engine::RetuneCause::OutsideSpan);
    CHECK(followed[0].removed[0].frequency == kMarine + 100'000);
    CHECK_FALSE(eng.vrx_status(*added).has_value());

    // Taken once.
    CHECK(eng.take_followed_retunes().empty());
}

TEST_CASE("a capture replay that starts far into its stream plays from the graph's own zero",
          "[gpu][engine][capture]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // Every hit the client's capture panel replays is like this: written an
    // hour into a stream, so its first index is billions of samples in. The
    // ring places sample i at slot i, and until the engine rebased a replay
    // the first block was refused as "further than one ring from the origin"
    // and the engine stopped. 64 rings in is far enough to say the same.
    constexpr dsp::Hertz kMarine = 156'800'000;
    constexpr dsp::Hertz kWeather = 162'550'000;
    constexpr dsp::SampleRate kReplayRate = 2'048'000;
    constexpr std::uint64_t kSegment = 1u << 19;
    constexpr std::uint64_t kFirst = std::uint64_t{1} << 26;

    test::CaptureRoot root;
    test::SegmentSpec first;
    first.rate = kReplayRate;
    first.center = kMarine;
    first.first_index = kFirst;
    first.sample_count = kSegment;
    test::write_segment(root.path(), first);
    test::SegmentSpec second = first;
    second.first_index = kFirst + kSegment;
    second.center = kWeather;
    test::write_segment(root.path(), second);

    engine::EngineConfig config;
    config.channels = 64;
    config.ring_seconds = 0.5;
    config.gpu_index = -1;
    config.spectrum_transform = 2048;
    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;

    auto opened = eng.open_source("capture:///" + root.utf8() + "?pace=max");
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    const Status ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());
    CHECK(eng.source_stats().samples_delivered == 2 * kSegment);

    // The retune is reported on the graph's scale, where the replay began at
    // zero, because that is the scale receivers and plugins count on.
    const std::vector<engine::SourceRetune> followed = eng.take_followed_retunes();
    REQUIRE(followed.size() == 1);
    CHECK(followed[0].center == kWeather);
    CHECK(followed[0].at == kSegment);
}
