// The segment writer, its retention ring and its recovery pass.
//
// Every case drives SegmentWriter::consume directly with synthetic cu8 blocks
// rather than through a live source, because what is under test is the file
// layout format.h specifies and the boundaries the writer draws, and a source
// thread would only add timing to that. The one case that does open a source
// is the replay, which is the point of a segment being SigMF at all.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/capture/capture_source.h"
#include "core/capture/format.h"
#include "core/capture/writer.h"
#include "core/source/file_source.h"
#include "core/source/registry.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace revenant;

constexpr dsp::SampleRate kRate = 1'000'000;
// 2026-10-12T12:00:00Z
constexpr std::int64_t kAnchor = 1'791'806'400'000'000'000;
constexpr std::size_t kBlock = 1000;

class Scratch {
public:
    explicit Scratch(std::string_view tag)
    {
        std::random_device entropy;
        root_ = fs::temp_directory_path() /
                std::format("revenant_capture_{}_{:08x}{:08x}", tag, entropy(), entropy());
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

// Each sample's bytes depend on its index, so a replay that lands one sample
// off or a segment that repeats a block is visible.
[[nodiscard]] std::vector<std::byte> block_bytes(dsp::SampleIndex start, std::size_t count)
{
    std::vector<std::byte> out(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        const auto index = start + i;
        out[2 * i] = static_cast<std::byte>(index & 0xFF);
        out[2 * i + 1] = static_cast<std::byte>((index >> 8) & 0xFF);
    }
    return out;
}

struct Feeder {
    capture::SegmentWriter& writer;
    std::vector<std::byte> scratch;

    void feed(dsp::SampleIndex start, dsp::Hertz center, std::uint64_t dropped_before = 0,
              dsp::SampleRate rate = kRate)
    {
        scratch = block_bytes(start, kBlock);
        source::SourceBlock block;
        block.stamp = dsp::BlockTimestamp{start, kAnchor, rate};
        block.format = source::SampleFormat::Cu8;
        block.sample_count = kBlock;
        block.bytes = scratch;
        block.dropped_before = dropped_before;
        writer.consume(block, center);
    }
};

struct Segment {
    fs::path data;
    fs::path meta;
    std::string text;
};

[[nodiscard]] std::vector<Segment> list_segments(const fs::path& root)
{
    std::vector<Segment> out;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const std::string name = entry.path().filename().string();
        if (!name.ends_with(".sigmf-meta")) {
            continue;
        }
        Segment segment;
        segment.meta = entry.path();
        segment.data = entry.path();
        segment.data.replace_extension(".sigmf-data");
        std::ifstream in(entry.path(), std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        segment.text = text.str();
        out.push_back(std::move(segment));
    }
    // By first index rather than by name: a rate change moves the wall clock
    // of the next index, so names need not sort in stream order.
    std::ranges::sort(out, {}, [](const Segment& s) {
        const auto fields = capture::parse_stem(s.meta.stem().string());
        return fields ? fields->first_index : 0;
    });
    return out;
}

[[nodiscard]] bool has(const Segment& segment, std::string_view needle)
{
    return segment.text.find(needle) != std::string::npos;
}

[[nodiscard]] capture::WriterConfig config_for(const fs::path& root)
{
    capture::WriterConfig config;
    config.root = root;
    config.stream_epoch = 0x1234;
    config.hw = "test \"dongle\"\nline two";
    return config;
}

}  // namespace

TEST_CASE("Segments roll on retune, rate, size and loss with the reason recorded",
          "[capture][writer]")
{
    const Scratch scratch("roll");
    auto config = config_for(scratch.root());
    config.max_segment_bytes = 4 * kBlock * 2;  // four blocks
    capture::SegmentWriter writer(config);
    REQUIRE(writer.start().has_value());
    Feeder feeder{writer, {}};

    dsp::SampleIndex at = 0;
    // Five blocks at one centre: four fill a segment and the fifth rolls it.
    for (int i = 0; i < 5; ++i, at += kBlock) {
        feeder.feed(at, 100'000'000);
    }
    // Retune.
    feeder.feed(at, 101'000'000);
    at += kBlock;
    // The device lost 500 samples.
    feeder.feed(at + 500, 101'000'000, 500);
    at += 500 + kBlock;
    // Rate change.
    feeder.feed(at, 101'000'000, 0, 2'000'000);
    writer.stop();

    const auto stats = writer.stats();
    CHECK(stats.last_error.empty());
    CHECK(stats.segments_written == 5);
    CHECK(stats.blocks_dropped == 0);

    const auto segments = list_segments(scratch.root());
    REQUIRE(segments.size() == 5);
    CHECK(has(segments[0], "\"revenant:close_reason\":\"size\""));
    CHECK(has(segments[1], "\"revenant:close_reason\":\"retune\""));
    CHECK(has(segments[2], "\"revenant:close_reason\":\"loss\""));
    CHECK(has(segments[3], "\"revenant:close_reason\":\"rate\""));
    CHECK(has(segments[4], "\"revenant:close_reason\":\"stop\""));
    CHECK(has(segments[3], "\"revenant:dropped_before\":500"));
    CHECK(has(segments[1], "\"revenant:dropped_before\":0"));

    // No open leftovers, no side files.
    for (const auto& entry : fs::recursive_directory_iterator(scratch.root())) {
        const std::string name = entry.path().filename().string();
        CHECK(name.find(".open") == std::string::npos);
    }
}

TEST_CASE("A meta carries every key format.h names and the file source reads it",
          "[capture][writer]")
{
    const Scratch scratch("meta");
    auto config = config_for(scratch.root());
    capture::SegmentWriter writer(config);
    writer.set_gain_db(29.7);
    REQUIRE(writer.start().has_value());
    Feeder feeder{writer, {}};
    for (dsp::SampleIndex at = 0; at < 4 * kBlock; at += kBlock) {
        feeder.feed(at, 145'500'000);
    }
    capture::Annotation annotation;
    annotation.sample_count = 1500;
    annotation.freq_lower = 145'490'000;
    annotation.freq_upper = 145'510'000;
    annotation.confidence = 0.75;
    annotation.peak_db = -20.5;
    annotation.snr_db = 18.0;
    annotation.family = "nbfm";
    writer.add_annotation(2500, annotation, 42);
    writer.stop();

    const auto segments = list_segments(scratch.root());
    REQUIRE(segments.size() == 1);
    const Segment& segment = segments[0];
    for (const std::string_view key :
         {"\"core:datatype\":\"cu8\"", "\"core:sample_rate\":1000000", "\"core:version\":\"1.",
          "\"core:recorder\":\"Revenant ", "\"core:hw\":\"test \\\"dongle\\\"\\nline two\"",
          "\"name\":\"revenant\"", "\"version\":\"1.0.0\"", "\"optional\":true",
          "\"revenant:stream_epoch\":4660", "\"revenant:first_index\":0",
          "\"revenant:epoch_anchor_ns\":1791806400000000000", "\"revenant:dropped_before\":0",
          "\"revenant:close_reason\":\"stop\"", "\"revenant:gain_db\":29.7",
          "\"core:frequency\":145500000", "\"core:datetime\":\"2026-10-12T12:00:00.000000000Z\"",
          "\"core:sample_start\":2500", "\"core:sample_count\":1500",
          "\"core:freq_lower_edge\":145490000", "\"core:label\":\"nbfm\"",
          "\"core:generator\":\"revenant-detect\"", "\"revenant:confidence\":0.75",
          "\"revenant:family\":\"nbfm\"", "\"revenant:track_id\":42"}) {
        INFO(key);
        CHECK(has(segment, key));
    }

    // The stem follows format.h and sits in its UTC day directory.
    CHECK(segment.meta.parent_path().filename() == "2026-10-12");
    CHECK(segment.meta.stem().string() == "rev-20261012T120000.000Z-145500000-0000000000001234-0");

    auto metadata = source::read_sigmf_metadata(segment.meta.string());
    INFO((metadata ? std::string{} : metadata.error().message));
    REQUIRE(metadata.has_value());
    CHECK(metadata->rate == kRate);
    CHECK(metadata->format == source::SampleFormat::Cu8);
    // Extent equals the bytes on disk.
    CHECK(metadata->data_bytes == fs::file_size(segment.data));
    CHECK(fs::file_size(segment.data) == 4 * kBlock * 2);
    REQUIRE(metadata->segments.size() == 1);
    CHECK(metadata->segments[0].center_hz == 145'500'000);
    CHECK(metadata->segments[0].anchor_ns == kAnchor);
}

TEST_CASE("A full queue drops, counts and closes the segment as loss", "[capture][writer]")
{
    const Scratch scratch("drop");
    auto config = config_for(scratch.root());
    config.queue_blocks = 1;
    capture::SegmentWriter writer(config);
    REQUIRE(writer.start().has_value());
    Feeder feeder{writer, {}};
    // As fast as possible: with a one-block queue the writer thread cannot
    // keep up with every block, so some are dropped.
    dsp::SampleIndex at = 0;
    for (int i = 0; i < 2000; ++i, at += kBlock) {
        feeder.feed(at, 50'000'000);
    }
    // One block once the queue has drained, so a loss at the very end is
    // attributed to a segment rather than left pending.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    feeder.feed(at, 50'000'000);
    writer.stop();
    const auto stats = writer.stats();
    INFO(std::format("dropped {}", stats.blocks_dropped));
    if (stats.blocks_dropped == 0) {
        SKIP("the writer kept up with a one-block queue, so nothing was dropped");
    }
    CHECK(stats.samples_dropped == stats.blocks_dropped * kBlock);
    CHECK(stats.bytes_written + stats.samples_dropped * 2 == 2001 * kBlock * 2);

    std::uint64_t dropped_in_meta = 0;
    std::size_t losses = 0;
    for (const Segment& segment : list_segments(scratch.root())) {
        if (has(segment, "\"revenant:close_reason\":\"loss\"")) {
            ++losses;
        }
        const auto key = segment.text.find("\"revenant:dropped_before\":");
        REQUIRE(key != std::string::npos);
        dropped_in_meta += std::stoull(segment.text.substr(key + 26));
        auto metadata = source::read_sigmf_metadata(segment.meta.string());
        REQUIRE(metadata.has_value());
        CHECK(metadata->data_bytes == fs::file_size(segment.data));
    }
    CHECK(losses >= 1);
    CHECK(dropped_in_meta == stats.samples_dropped);
}

TEST_CASE("Retention deletes the oldest closed segments first", "[capture][writer][retention]")
{
    const Scratch scratch("retain");
    auto config = config_for(scratch.root());
    config.max_segment_bytes = kBlock * 2;  // one block per segment
    // Room for about three segments with their metas.
    config.retention_bytes = 3 * (kBlock * 2 + 1500);
    capture::SegmentWriter writer(config);
    REQUIRE(writer.start().has_value());
    Feeder feeder{writer, {}};
    for (dsp::SampleIndex at = 0; at < 10 * kBlock; at += kBlock) {
        feeder.feed(at, 433'920'000);
    }
    writer.stop();

    const auto stats = writer.stats();
    CHECK(stats.segments_written == 10);
    CHECK(stats.segments_deleted >= 6);
    CHECK(stats.retained_bytes <= config.retention_bytes);
    const auto segments = list_segments(scratch.root());
    REQUIRE(!segments.empty());
    CHECK(segments.size() == 10 - stats.segments_deleted);
    // What survives is the newest: the last one ends at sample 10000.
    CHECK(has(segments.back(), "\"revenant:first_index\":9000"));
    CHECK(!has(segments.front(), "\"revenant:first_index\":0,"));
}

TEST_CASE("Retention by age removes old segments and their empty day directory",
          "[capture][writer][retention]")
{
    const Scratch scratch("age");
    // A segment from three days earlier, finalised by an earlier writer.
    {
        auto config = config_for(scratch.root());
        capture::SegmentWriter old_writer(config);
        REQUIRE(old_writer.start().has_value());
        Feeder feeder{old_writer, {}};
        source::SourceBlock block;
        feeder.scratch = block_bytes(0, kBlock);
        block.stamp = dsp::BlockTimestamp{0, kAnchor - 3LL * 86'400'000'000'000, kRate};
        block.format = source::SampleFormat::Cu8;
        block.sample_count = kBlock;
        block.bytes = feeder.scratch;
        old_writer.consume(block, 7'100'000);
        old_writer.stop();
    }
    REQUIRE(fs::exists(scratch.root() / "2026-10-09"));

    auto config = config_for(scratch.root());
    config.stream_epoch = 0x5678;
    config.retention_max_age_ns = 86'400'000'000'000;
    config.now_ns = [] { return kAnchor + 1'000'000'000; };
    capture::SegmentWriter writer(config);
    REQUIRE(writer.start().has_value());
    Feeder feeder{writer, {}};
    feeder.feed(0, 7'100'000);
    writer.stop();

    CHECK(writer.stats().segments_deleted == 1);
    CHECK(!fs::exists(scratch.root() / "2026-10-09"));
    CHECK(list_segments(scratch.root()).size() == 1);
}

TEST_CASE("Recovery finalises a segment a dead writer left open", "[capture][writer][recovery]")
{
    const Scratch scratch("recover");
    capture::SegmentInfo info;
    info.stream_epoch = 0xABCDEF;
    info.first_index = 5000;
    info.epoch_anchor_ns = kAnchor;
    info.rate = kRate;
    info.center = 162'000'000;
    info.format = source::SampleFormat::Cu8;
    const std::string stem = capture::make_stem(info);
    const fs::path dir = scratch.root() / capture::day_directory(info.start_ns());
    fs::create_directories(dir);

    // What the writer leaves on disk when killed mid-block: the side file and
    // a data file ending in half a sample.
    {
        std::ofstream side(dir / (stem + ".open.revenant"), std::ios::binary);
        side << "rate=1000000\nformat=cu8\nanchor_ns=" << kAnchor
             << "\ndropped_before=7\nhw=rtl\n";
        const auto bytes = block_bytes(5000, 3000);
        std::ofstream data(dir / (stem + ".open.sigmf-data"), std::ios::binary);
        data.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        data.put('\x01');
    }
    // And one with no side file, which must be reported rather than guessed.
    {
        info.first_index = 99'000;
        std::ofstream orphan(dir / (capture::make_stem(info) + ".open.sigmf-data"),
                             std::ios::binary);
        orphan << "xx";
    }

    auto report = capture::recover_open_segments(scratch.root());
    REQUIRE(report.has_value());
    CHECK(report->recovered == 1);
    CHECK(report->failures.size() == 1);

    const fs::path data = dir / (stem + ".sigmf-data");
    const fs::path meta = dir / (stem + ".sigmf-meta");
    REQUIRE(fs::exists(data));
    REQUIRE(fs::exists(meta));
    CHECK(!fs::exists(dir / (stem + ".open.revenant")));
    CHECK(fs::file_size(data) == 6000);

    auto metadata = source::read_sigmf_metadata(meta.string());
    REQUIRE(metadata.has_value());
    CHECK(metadata->data_bytes == 6000);
    CHECK(metadata->segments[0].center_hz == 162'000'000);
    // Sample 5000 at 1 MS/s is 5 ms after the anchor.
    CHECK(metadata->segments[0].anchor_ns == kAnchor + 5'000'000);

    std::ifstream in(meta, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    CHECK(text.str().find("\"revenant:close_reason\":\"recovered\"") != std::string::npos);
    CHECK(text.str().find("\"revenant:dropped_before\":7") != std::string::npos);
    CHECK(text.str().find("\"revenant:first_index\":5000") != std::string::npos);
}

TEST_CASE("Stems parse back to what made them", "[capture][writer]")
{
    capture::SegmentInfo info;
    info.stream_epoch = 0x0123456789ABCDEFULL;
    info.first_index = 123'456'789;
    info.epoch_anchor_ns = kAnchor + 250'000'000;
    info.rate = kRate;
    info.center = 7'074'000;
    const auto fields = capture::parse_stem(capture::make_stem(info));
    REQUIRE(fields.has_value());
    CHECK(fields->stream_epoch == info.stream_epoch);
    CHECK(fields->first_index == info.first_index);
    CHECK(fields->center == info.center);
    CHECK(fields->start_ns / 1'000'000 == info.start_ns() / 1'000'000);
    CHECK(!capture::parse_stem("rev-nonsense").has_value());
}

TEST_CASE("A written segment replays through the file source sample for sample",
          "[capture][writer][replay]")
{
    const Scratch scratch("replay");
    auto config = config_for(scratch.root());
    capture::SegmentWriter writer(config);
    REQUIRE(writer.start().has_value());
    Feeder feeder{writer, {}};
    constexpr std::size_t kBlocks = 16;
    std::vector<std::byte> expected;
    for (dsp::SampleIndex at = 0; at < kBlocks * kBlock; at += kBlock) {
        feeder.feed(at, 98'100'000);
        const auto bytes = block_bytes(at, kBlock);
        expected.insert(expected.end(), bytes.begin(), bytes.end());
    }
    writer.stop();

    const auto segments = list_segments(scratch.root());
    REQUIRE(segments.size() == 1);
    const std::string uri = "file:///" + segments[0].data.generic_string();
    auto opened = source::open_source(uri);
    INFO((opened ? std::string{} : opened.error().message));
    REQUIRE(opened.has_value());
    source::Source& replay = **opened;
    CHECK(replay.sample_rate() == kRate);
    CHECK(replay.center() == 98'100'000);
    CHECK(replay.capabilities().native_format == source::SampleFormat::Cu8);
    CHECK(replay.capabilities().length_samples == kBlocks * kBlock);

    std::mutex mutex;
    std::vector<std::byte> got;
    std::int64_t anchor = 0;
    REQUIRE(replay
                .start({}, [&](const source::SourceBlock& block) -> Status {
                    std::lock_guard lock(mutex);
                    if (got.empty()) {
                        anchor = block.stamp.epoch_anchor_ns;
                    }
                    got.insert(got.end(), block.bytes.begin(), block.bytes.end());
                    return {};
                })
                .has_value());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (replay.running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(replay.stop().has_value());

    std::lock_guard lock(mutex);
    REQUIRE(got.size() == expected.size());
    CHECK(got == expected);
    CHECK(anchor == kAnchor);
}

namespace {

// Ages every segment written at kAnchor past a one day limit, so the next
// close deletes all of them.
[[nodiscard]] capture::WriterConfig ageing_config(const fs::path& root, std::uint64_t epoch)
{
    auto config = config_for(root);
    config.stream_epoch = epoch;
    config.max_segment_bytes = kBlock * 2;
    config.retention_max_age_ns = 86'400'000'000'000;
    config.now_ns = [] { return kAnchor + 10 * 86'400'000'000'000; };
    return config;
}

}  // namespace

TEST_CASE("The ring deletes a segment under a replay that keeps reading it to the end",
          "[capture][writer][retention]")
{
    const Scratch scratch("ring_replay");
    {
        auto config = config_for(scratch.root());
        config.max_segment_bytes = kBlock * 2;
        capture::SegmentWriter writer(config);
        REQUIRE(writer.start().has_value());
        Feeder feeder{writer, {}};
        for (dsp::SampleIndex at = 0; at < 3 * kBlock; at += kBlock) {
            feeder.feed(at, 433'920'000);
        }
        writer.stop();
    }
    const auto before = list_segments(scratch.root());
    REQUIRE(before.size() == 3);
    const fs::path oldest = before.front().data;

    capture::CaptureSourceConfig source_config;
    source_config.uri = "capture://ring";
    source_config.root = scratch.root().string();
    auto source = capture::open_capture_source(source_config);
    REQUIRE(source);

    // The ring runs inside the first delivery, when the replay has the oldest
    // segment open and has read only the first block of it.
    capture::WriterStats ring;
    bool ran = false;
    std::vector<std::byte> first_segment;
    source::StreamOptions options;
    options.block_samples = 128;
    auto started = (*source)->start(options, [&](const source::SourceBlock& block) {
        if (!ran) {
            ran = true;
            capture::SegmentWriter writer(ageing_config(scratch.root(), 0x5678));
            REQUIRE(writer.start().has_value());
            Feeder feeder{writer, {}};
            feeder.feed(0, 433'920'000);
            writer.stop();
            ring = writer.stats();
        }
        if (block.stamp.start < kBlock) {
            first_segment.insert(first_segment.end(), block.bytes.begin(), block.bytes.end());
        }
        return Status{};
    });
    REQUIRE(started);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while ((*source)->running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    static_cast<void>((*source)->stop());

    REQUIRE(ran);
    CHECK(ring.delete_failures == 0);
    CHECK(ring.pending_deletes == 0);
    CHECK(ring.segments_deleted >= 3);
    CHECK(!fs::exists(oldest));
    CHECK(first_segment == block_bytes(0, kBlock));
}

namespace {

// Three closed one-block segments a stopped writer left, oldest first.
[[nodiscard]] std::vector<Segment> three_segments(const fs::path& root)
{
    auto config = config_for(root);
    config.max_segment_bytes = kBlock * 2;
    capture::SegmentWriter writer(config);
    REQUIRE(writer.start().has_value());
    Feeder feeder{writer, {}};
    feeder.feed(0, 433'920'000);
    feeder.feed(kBlock, 433'920'000);
    feeder.feed(2 * kBlock, 433'920'000);
    writer.stop();
    auto segments = list_segments(root);
    REQUIRE(segments.size() == 3);
    return segments;
}

void write_bytes(const fs::path& path, std::string_view bytes)
{
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

TEST_CASE("A restart deletes a data file the ring had already given up on, and nothing else",
          "[capture][writer][retention]")
{
    const Scratch scratch("orphan");
    const auto segments = three_segments(scratch.root());
    const fs::path dir = segments.front().data.parent_path();

    // What a refused ring delete leaves after a restart: the meta went, the
    // data did not, and the pending list that remembered it is gone.
    fs::remove(segments.front().meta);
    const fs::path orphan = segments.front().data;

    // A segment still open, which recovery owns. Recovery is off below, so
    // this stays open and adoption must not touch it.
    capture::SegmentInfo info;
    info.stream_epoch = 0x9999;
    info.epoch_anchor_ns = kAnchor;
    info.rate = kRate;
    info.center = 162'000'000;
    info.first_index = 500;
    const std::string open_stem = capture::make_stem(info);
    const fs::path open_data = dir / (open_stem + ".open.sigmf-data");
    write_bytes(open_data, "abcd");
    write_bytes(dir / (open_stem + ".open.revenant"), "rate=1000000\nformat=cu8\n");

    // A closed name with a side file beside it is a close that died part way,
    // which recovery's side of the house still describes, so it is not a
    // leftover either.
    info.first_index = 600;
    const std::string sided_stem = capture::make_stem(info);
    const fs::path sided = dir / (sided_stem + ".sigmf-data");
    write_bytes(sided, "abcd");
    write_bytes(dir / (sided_stem + ".open.revenant"), "rate=1000000\nformat=cu8\n");

    // A meta-less data file newer than every closed segment is a close that
    // died between rename and meta: a recording, not a leftover.
    info.first_index = 50'000'000;
    const fs::path newest = dir / (capture::make_stem(info) + ".sigmf-data");
    write_bytes(newest, "abcd");

    auto config = config_for(scratch.root());
    config.stream_epoch = 0x5678;
    config.recover_on_start = false;
    capture::SegmentWriter writer(config);
    REQUIRE(writer.start().has_value());
    const auto started = writer.stats();
    writer.stop();

    CHECK(!fs::exists(orphan));
    CHECK(started.orphans_deleted == 1);
    CHECK(started.segments_deleted == 1);
    CHECK(started.pending_deletes == 0);
    CHECK(fs::exists(open_data));
    CHECK(fs::exists(sided));
    CHECK(fs::exists(newest));
    CHECK(fs::exists(segments[1].data));
    CHECK(fs::exists(segments[2].data));
}

#if defined(_WIN32)
TEST_CASE("An orphan whose delete is refused at start stays pending until a close retries it",
          "[capture][writer][retention]")
{
    const Scratch scratch("orphan_held");
    const auto segments = three_segments(scratch.root());
    fs::remove(segments.front().meta);
    const fs::path orphan = segments.front().data;

    HANDLE held = CreateFileW(orphan.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);

    auto config = config_for(scratch.root());
    config.stream_epoch = 0x5678;
    config.max_segment_bytes = kBlock * 2;
    capture::SegmentWriter writer(config);
    REQUIRE(writer.start().has_value());
    const auto started = writer.stats();
    CHECK(started.orphans_deleted == 0);
    CHECK(started.delete_failures == 1);
    CHECK(started.pending_deletes == 1);
    // Still on disk, so still counted against the budget.
    CHECK(started.retained_bytes >= fs::file_size(orphan));

    CloseHandle(held);
    Feeder feeder{writer, {}};
    feeder.feed(0, 433'920'000);
    writer.stop();

    const auto retried = writer.stats();
    CHECK(!fs::exists(orphan));
    CHECK(retried.pending_deletes == 0);
    CHECK(retried.segments_deleted == 1);
}

TEST_CASE("A refused ring delete is counted and kept pending until the next close retries it",
          "[capture][writer][retention]")
{
    const Scratch scratch("ring_retry");
    {
        auto config = config_for(scratch.root());
        config.max_segment_bytes = kBlock * 2;
        capture::SegmentWriter writer(config);
        REQUIRE(writer.start().has_value());
        Feeder feeder{writer, {}};
        feeder.feed(0, 433'920'000);
        writer.stop();
    }
    const auto before = list_segments(scratch.root());
    REQUIRE(before.size() == 1);
    const fs::path data = before.front().data;
    const fs::path meta = before.front().meta;

    // What _wfopen does: share read and write, not delete.
    HANDLE held = CreateFileW(data.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);

    capture::SegmentWriter writer(ageing_config(scratch.root(), 0x5678));
    REQUIRE(writer.start().has_value());
    Feeder feeder{writer, {}};
    // A retune closes the first segment, which runs the ring over the held one.
    feeder.feed(0, 433'920'000);
    feeder.feed(kBlock, 144'390'000);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (writer.stats().delete_failures == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto refused = writer.stats();
    CHECK(refused.delete_failures == 1);
    CHECK(refused.pending_deletes == 1);
    // The meta went first, so what is left reads as an orphan, not a problem.
    CHECK(!fs::exists(meta));
    CHECK(fs::exists(data));
    // The refused pair's bytes are not counted as deleted.
    CHECK(refused.retained_bytes >= fs::file_size(data));

    CloseHandle(held);
    feeder.feed(2 * kBlock, 433'920'000);
    writer.stop();

    const auto retried = writer.stats();
    CHECK(retried.delete_failures == 1);
    CHECK(retried.pending_deletes == 0);
    CHECK(!fs::exists(data));
    CHECK(retried.segments_deleted == 4);
}
#endif
