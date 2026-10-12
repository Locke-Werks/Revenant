// Replaying a capture root as a source.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/capture/capture_source.h"
#include "core/source/registry.h"
#include "core/source/source.h"
#include "tests/capture/catalog_fixture.h"

using revenant::capture::CaptureSourceConfig;
using revenant::capture::open_capture_source;
using revenant::test::capture_bytes;
using revenant::test::CaptureRoot;
using revenant::test::kCaptureAnchorNs;
using revenant::test::SegmentSpec;
using revenant::test::write_segment;

namespace {

constexpr std::int64_t kMs = 1'000'000;

struct Delivered {
    std::uint64_t start = 0;
    std::size_t count = 0;
    std::uint64_t dropped = 0;
    std::int64_t wall_ns = 0;
    std::int64_t center = 0;
    std::vector<std::byte> bytes;
};

// Starts, waits for the end of the run, stops, and hands back every block.
std::vector<Delivered> play(revenant::source::Source& source,
                            revenant::source::StreamOptions options = {})
{
    std::vector<Delivered> blocks;
    if (options.block_samples == 0) {
        options.block_samples = 128;
    }
    auto started = source.start(options, [&](const revenant::source::SourceBlock& block) {
        Delivered d;
        d.start = block.stamp.start;
        d.count = block.sample_count;
        d.dropped = block.dropped_before;
        d.wall_ns = block.stamp.wall_clock_ns();
        d.center = block.center;
        d.bytes.assign(block.bytes.begin(), block.bytes.end());
        blocks.push_back(std::move(d));
        return revenant::Status{};
    });
    REQUIRE(started);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (source.running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(source.stop());
    return blocks;
}

CaptureSourceConfig config_for(const CaptureRoot& root)
{
    CaptureSourceConfig config;
    config.uri = "capture://test";
    config.root = root.utf8();
    return config;
}

}  // namespace

TEST_CASE("replay across two contiguous segments is sample-identical", "[capture][replay]")
{
    CaptureRoot root;
    SegmentSpec a;
    a.sample_count = 500;
    write_segment(root.path(), a);
    SegmentSpec b;
    b.first_index = 500;
    b.sample_count = 500;
    write_segment(root.path(), b);

    auto source = open_capture_source(config_for(root));
    REQUIRE(source);
    CHECK((*source)->center() == 100'000'000);
    CHECK((*source)->sample_rate() == 1000);
    CHECK((*source)->capabilities().flow == revenant::source::FlowControl::Demand);
    CHECK((*source)->capabilities().length_samples == 1000);

    const auto blocks = play(**source);
    std::vector<std::byte> joined;
    std::uint64_t expect = 0;
    for (const Delivered& d : blocks) {
        CHECK(d.start == expect);
        CHECK(d.dropped == 0);
        CHECK(d.wall_ns == kCaptureAnchorNs + static_cast<std::int64_t>(d.start) * kMs);
        expect += d.count;
        joined.insert(joined.end(), d.bytes.begin(), d.bytes.end());
    }
    CHECK(expect == 1000);
    CHECK(joined == capture_bytes(0, 1000));
}

TEST_CASE("a gap between segments arrives as dropped_before on the block after it",
          "[capture][replay]")
{
    CaptureRoot root;
    SegmentSpec a;
    a.sample_count = 300;
    write_segment(root.path(), a);
    SegmentSpec b;
    b.first_index = 450;
    b.sample_count = 200;
    b.dropped_before = 150;
    write_segment(root.path(), b);

    auto source = open_capture_source(config_for(root));
    REQUIRE(source);
    const auto blocks = play(**source);

    std::uint64_t samples = 0;
    std::uint64_t dropped = 0;
    for (const Delivered& d : blocks) {
        if (d.dropped != 0) {
            CHECK(d.start == 450);
            CHECK(d.dropped == 150);
            CHECK(d.bytes == capture_bytes(450, d.count));
        }
        samples += d.count;
        dropped += d.dropped;
    }
    CHECK(samples == 500);
    CHECK(dropped == 150);
}

TEST_CASE("a seek lands on the sample asked for, and one into a gap on the next that exists",
          "[capture][replay]")
{
    CaptureRoot root;
    SegmentSpec a;
    a.sample_count = 300;
    write_segment(root.path(), a);
    SegmentSpec b;
    b.first_index = 300;
    b.sample_count = 300;
    write_segment(root.path(), b);
    SegmentSpec c;
    c.first_index = 700;
    c.sample_count = 100;
    write_segment(root.path(), c);

    auto source = open_capture_source(config_for(root));
    REQUIRE(source);

    REQUIRE((*source)->seek(420));
    auto blocks = play(**source);
    REQUIRE_FALSE(blocks.empty());
    CHECK(blocks.front().start == 420);
    CHECK(blocks.front().dropped == 0);
    CHECK(blocks.front().bytes == capture_bytes(420, blocks.front().count));

    REQUIRE((*source)->seek(650));
    blocks = play(**source);
    REQUIRE_FALSE(blocks.empty());
    CHECK(blocks.front().start == 700);
    CHECK(blocks.front().dropped == 0);

    CHECK_FALSE((*source)->seek(900));
}

TEST_CASE("from= and to= select the samples between them", "[capture][replay]")
{
    CaptureRoot root;
    SegmentSpec a;
    a.sample_count = 500;
    write_segment(root.path(), a);
    SegmentSpec b;
    b.first_index = 500;
    b.sample_count = 500;
    write_segment(root.path(), b);

    CaptureSourceConfig config = config_for(root);
    config.from_given = true;
    config.from_ns = kCaptureAnchorNs + 250 * kMs + 1;  // rounds up to sample 251
    config.to_given = true;
    config.to_ns = kCaptureAnchorNs + 750 * kMs;

    auto source = open_capture_source(config);
    REQUIRE(source);
    const auto blocks = play(**source);
    REQUIRE_FALSE(blocks.empty());
    CHECK(blocks.front().start == 251);
    std::uint64_t samples = 0;
    for (const Delivered& d : blocks) {
        samples += d.count;
    }
    CHECK(samples == 750 - 251);
}

TEST_CASE("replay carries a retune as the centre of the block after it", "[capture][replay]")
{
    CaptureRoot root;
    SegmentSpec a;
    a.sample_count = 200;
    write_segment(root.path(), a);
    SegmentSpec b;
    b.first_index = 200;
    b.sample_count = 200;
    b.center = 101'000'000;
    write_segment(root.path(), b);

    auto source = open_capture_source(config_for(root));
    REQUIRE(source);
    CHECK((*source)->capabilities().length_samples == 400);
    CHECK((*source)->center() == a.center);
    bool noted = false;
    for (const std::string& note : (*source)->capabilities().notes) {
        noted = noted || note.find("retunes 1 time") != std::string::npos;
    }
    CHECK(noted);

    const auto blocks = play(**source);
    std::uint64_t samples = 0;
    for (const Delivered& d : blocks) {
        samples += d.count;
        // Blocks never straddle a segment, so each is wholly at one centre.
        CHECK(d.center == (d.start < 200 ? a.center : b.center));
        CHECK(d.dropped == 0);
    }
    CHECK(samples == 400);
    CHECK((*source)->center() == b.center);
}

TEST_CASE("replay stops at a rate change and says so", "[capture][replay]")
{
    CaptureRoot root;
    SegmentSpec a;
    a.sample_count = 200;
    write_segment(root.path(), a);
    SegmentSpec b;
    b.first_index = 200;
    b.sample_count = 200;
    b.rate = a.rate * 2;
    write_segment(root.path(), b);

    auto source = open_capture_source(config_for(root));
    REQUIRE(source);
    CHECK((*source)->capabilities().length_samples == 200);
    bool noted = false;
    for (const std::string& note : (*source)->capabilities().notes) {
        noted = noted || note.find("sample rate changes") != std::string::npos;
    }
    CHECK(noted);
}

TEST_CASE("a capture URI opens through the registry and refuses an unknown key",
          "[capture][replay]")
{
    CaptureRoot root;
    SegmentSpec a;
    a.sample_count = 100;
    write_segment(root.path(), a);

    const std::string base = "capture:///" + root.utf8();
    auto opened = revenant::source::open_source(base + "?from=2026-10-12T00:00:00.050Z&pace=max");
    REQUIRE(opened);
    CHECK((*opened)->capabilities().backend == "capture");
    const auto blocks = play(**opened);
    REQUIRE_FALSE(blocks.empty());
    CHECK(blocks.front().start == 50);

    CHECK_FALSE(revenant::source::open_source(base + "?form=2026-10-12T00:00:00Z"));
    CHECK_FALSE(revenant::source::open_source(base + "?from=2026-10-12T00:00:00"));
}
