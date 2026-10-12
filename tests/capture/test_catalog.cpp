// The capture catalog against segments written by hand to format.h.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/capture/catalog.h"
#include "tests/capture/catalog_fixture.h"

using revenant::capture::Catalog;
using revenant::capture::parse_capture_instant;
using revenant::test::CaptureRoot;
using revenant::test::kCaptureAnchorNs;
using revenant::test::kCaptureEpoch;
using revenant::test::SegmentSpec;
using revenant::test::write_segment;

namespace {

constexpr std::int64_t kMs = 1'000'000;

}  // namespace

TEST_CASE("a scan reads every closed segment's meta into a SegmentInfo", "[capture][catalog]")
{
    CaptureRoot root;
    SegmentSpec a;
    a.sample_count = 500;
    a.annotations =
        "{\"core:sample_start\": 10, \"core:sample_count\": 20, \"core:freq_lower_edge\": "
        "99990000, \"core:freq_upper_edge\": 100010000, \"core:label\": \"FM\", "
        "\"core:generator\": \"revenant-detect\", \"revenant:confidence\": 0.75, "
        "\"revenant:peak_db\": -12.5, \"revenant:snr_db\": 30, \"revenant:family\": \"FM\", "
        "\"revenant:track_id\": 7}";
    write_segment(root.path(), a);

    SegmentSpec b;
    b.first_index = 500;
    b.sample_count = 300;
    b.close_reason = "stop";
    write_segment(root.path(), b);

    auto catalog = Catalog::open(root.utf8());
    REQUIRE(catalog);
    REQUIRE(catalog->problems().empty());
    REQUIRE(catalog->unfinished().empty());
    REQUIRE(catalog->segments().size() == 2);

    const auto& first = catalog->segments()[0];
    CHECK(first.info.stream_epoch == kCaptureEpoch);
    CHECK(first.info.first_index == 0);
    CHECK(first.info.sample_count == 500);
    CHECK(first.info.rate == 1000);
    CHECK(first.info.center == 100'000'000);
    CHECK(first.info.format == revenant::source::SampleFormat::Cu8);
    CHECK(first.info.start_ns() == kCaptureAnchorNs);
    CHECK(first.end_ns() == kCaptureAnchorNs + 500 * kMs);
    CHECK(first.close_reason == "size");
    REQUIRE(first.annotations.size() == 1);
    CHECK(first.annotations[0].sample_start == 10);
    CHECK(first.annotations[0].sample_count == 20);
    CHECK(first.annotations[0].freq_lower == 99'990'000);
    CHECK(first.annotations[0].freq_upper == 100'010'000);
    CHECK(first.annotations[0].confidence == 0.75);
    CHECK(first.annotations[0].peak_db == -12.5);
    CHECK(first.annotations[0].snr_db == 30.0);
    CHECK(first.annotations[0].family == "FM");

    const auto& second = catalog->segments()[1];
    CHECK(second.info.first_index == 500);
    CHECK(second.info.start_ns() == first.end_ns());
    CHECK(second.close_reason == "stop");

    std::uint64_t expected = 0;
    for (const auto& segment : catalog->segments()) {
        expected += std::filesystem::file_size(std::filesystem::path(segment.data_path)) +
                    std::filesystem::file_size(std::filesystem::path(segment.meta_path));
    }
    CHECK(catalog->total_bytes() == expected);
}

TEST_CASE("a malformed meta is reported with its path and reason, and the rest still scan",
          "[capture][catalog]")
{
    CaptureRoot root;
    SegmentSpec good;
    good.sample_count = 100;
    write_segment(root.path(), good);

    SegmentSpec broken;
    broken.first_index = 100;
    broken.sample_count = 100;
    const auto written = write_segment(root.path(), broken);
    {
        std::ofstream meta(written.meta, std::ios::binary | std::ios::trunc);
        meta << "{\"global\": {\"core:datatype\": \"cu8\", \"core:sample_rate\": 1000}, "
                "\"captures\": [{\"core:sample_start\": 0, \"core:frequency\": 1}]}";
    }

    SegmentSpec not_json;
    not_json.first_index = 200;
    not_json.sample_count = 100;
    const auto garbage = write_segment(root.path(), not_json);
    {
        std::ofstream meta(garbage.meta, std::ios::binary | std::ios::trunc);
        meta << "{\"global\": ";
    }

    auto catalog = Catalog::open(root.utf8());
    REQUIRE(catalog);
    REQUIRE(catalog->segments().size() == 1);
    CHECK(catalog->segments()[0].info.first_index == 0);
    REQUIRE(catalog->problems().size() == 2);

    bool saw_missing_epoch = false;
    bool saw_bad_json = false;
    for (const auto& problem : catalog->problems()) {
        if (std::filesystem::path(problem.path) == written.meta) {
            saw_missing_epoch = problem.reason.find("revenant:stream_epoch") != std::string::npos;
        }
        if (std::filesystem::path(problem.path) == garbage.meta) {
            saw_bad_json = problem.reason.find("JSON") != std::string::npos;
        }
    }
    CHECK(saw_missing_epoch);
    CHECK(saw_bad_json);
}

TEST_CASE("an open segment is skipped and reported, and so is a data file with no meta",
          "[capture][catalog]")
{
    CaptureRoot root;
    SegmentSpec closed;
    closed.sample_count = 100;
    write_segment(root.path(), closed);

    SegmentSpec open;
    open.first_index = 100;
    open.sample_count = 50;
    open.open = true;
    const auto open_written = write_segment(root.path(), open);

    SegmentSpec orphan;
    orphan.epoch = 0x1234;
    orphan.sample_count = 70;
    orphan.orphan = true;
    const auto orphan_written = write_segment(root.path(), orphan);

    auto catalog = Catalog::open(root.utf8());
    REQUIRE(catalog);
    REQUIRE(catalog->segments().size() == 1);
    CHECK(catalog->problems().empty());
    REQUIRE(catalog->unfinished().size() == 2);

    bool saw_open = false;
    bool saw_orphan = false;
    for (const auto& data : catalog->unfinished()) {
        const std::filesystem::path path(data.data_path);
        if (path == open_written.data) {
            saw_open = data.in_progress && data.data_bytes == 100;
        }
        if (path == orphan_written.data) {
            saw_orphan = !data.in_progress && data.data_bytes == 140;
        }
    }
    CHECK(saw_open);
    CHECK(saw_orphan);
    CHECK(catalog->total_bytes() == catalog->segments()[0].data_bytes +
                                        catalog->segments()[0].meta_bytes + 100 + 140);
}

TEST_CASE("lookups by time and by index find the right segment and nothing in a gap",
          "[capture][catalog]")
{
    CaptureRoot root;
    SegmentSpec a;
    a.sample_count = 500;  // 0 to 500 ms
    write_segment(root.path(), a);
    SegmentSpec b;
    b.first_index = 500;
    b.sample_count = 500;  // 500 to 1000 ms
    write_segment(root.path(), b);
    SegmentSpec c;
    c.first_index = 1200;
    c.sample_count = 300;  // 1200 to 1500 ms, after a 200 ms gap
    c.dropped_before = 200;
    c.close_reason = "loss";
    write_segment(root.path(), c);

    auto catalog = Catalog::open(root.utf8());
    REQUIRE(catalog);
    REQUIRE(catalog->segments().size() == 3);

    const auto in_range = catalog->overlapping(kCaptureAnchorNs + 400 * kMs,
                                               kCaptureAnchorNs + 1100 * kMs);
    REQUIRE(in_range.size() == 2);
    CHECK(in_range[0].info.first_index == 0);
    CHECK(in_range[1].info.first_index == 500);

    // Half open: a range ending exactly where a segment starts does not hold it.
    CHECK(catalog->overlapping(kCaptureAnchorNs, kCaptureAnchorNs + 500 * kMs).size() == 1);
    CHECK(catalog->overlapping(kCaptureAnchorNs + 2000 * kMs, kCaptureAnchorNs + 3000 * kMs)
              .empty());

    auto at = catalog->at_time(kCaptureAnchorNs + 500 * kMs);
    REQUIRE(at);
    CHECK(at->info.first_index == 500);
    CHECK_FALSE(catalog->at_time(kCaptureAnchorNs + 1100 * kMs));
    CHECK_FALSE(catalog->at_time(kCaptureAnchorNs - 1));

    auto held = catalog->containing(kCaptureEpoch, 1299);
    REQUIRE(held);
    CHECK(held->info.first_index == 1200);
    CHECK(held->info.dropped_before == 200);
    CHECK_FALSE(catalog->containing(kCaptureEpoch, 1100));
    CHECK_FALSE(catalog->containing(kCaptureEpoch, 1500));
    CHECK_FALSE(catalog->containing(kCaptureEpoch + 1, 10));
}

TEST_CASE("a refresh picks up a segment that closed and drops one the ring removed",
          "[capture][catalog]")
{
    CaptureRoot root;
    SegmentSpec a;
    a.sample_count = 100;
    const auto first = write_segment(root.path(), a);

    auto catalog = Catalog::open(root.utf8());
    REQUIRE(catalog);
    REQUIRE(catalog->segments().size() == 1);

    SegmentSpec b;
    b.first_index = 100;
    b.sample_count = 100;
    write_segment(root.path(), b);
    std::filesystem::remove(first.data);
    std::filesystem::remove(first.meta);

    REQUIRE(catalog->refresh());
    REQUIRE(catalog->segments().size() == 1);
    CHECK(catalog->segments()[0].info.first_index == 100);
}

TEST_CASE("a capture instant is ISO 8601 UTC or epoch nanoseconds and nothing else",
          "[capture][catalog]")
{
    CHECK(parse_capture_instant("2026-10-12T00:00:00Z") == kCaptureAnchorNs);
    CHECK(parse_capture_instant("2026-10-12T00:00:01.250Z") == kCaptureAnchorNs + 1250 * kMs);
    CHECK(parse_capture_instant("1791763200000000000") == kCaptureAnchorNs);
    CHECK_FALSE(parse_capture_instant("2026-10-12T00:00:00"));
    CHECK_FALSE(parse_capture_instant("2026-10-12T00:00:00+02:00"));
    CHECK_FALSE(parse_capture_instant("2026-02-30T00:00:00Z"));
    CHECK_FALSE(parse_capture_instant("yesterday"));
}
