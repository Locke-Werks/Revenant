// revenant-capture's commands, run against a capture root the writer made.
//
// The root goes through core/capture/writer.h rather than a hand-written
// fixture on purpose: the catalog's own tests check it against format.h, and
// what this file checks is that the tool reads what a recording session
// actually leaves on disk.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "core/capture/format.h"
#include "core/capture/writer.h"
#include "core/source/source.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/support/temp_path.h"
#include "tools/capture/capture_tool.h"

namespace {

namespace fs = std::filesystem;
using namespace revenant;
namespace tool = revenant::capture_tool;

// 2026-10-12T00:00:00Z.
constexpr std::int64_t kAnchor = 1'791'763'200'000'000'000;
constexpr dsp::SampleRate kRate = 1'000'000;
constexpr std::size_t kBlock = 10'000;
constexpr std::uint64_t kEpoch = 0x00c0ffee12345678;
constexpr dsp::Hertz kMarine = 156'800'000;
constexpr dsp::Hertz kWeather = 162'550'000;

class Scratch {
public:
    Scratch() : path_(test::unique_temp_path("revenant-capture-tool"))
    {
        fs::create_directories(path_);
    }
    ~Scratch()
    {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }
    [[nodiscard]] std::string root() const { return (path_ / "capture").generic_string(); }

private:
    fs::path path_;
};

[[nodiscard]] std::string at(double seconds)
{
    return tool::format_instant(kAnchor + static_cast<std::int64_t>(seconds * 1e9));
}

// Four segments in one epoch:
//   0.0 to 0.5 s on the marine channel, closed by size
//   0.5 to 1.0 s on the marine channel, closed by the loss that follows
//   1.1 to 1.5 s on the marine channel after 0.1 s of lost samples, closed by a retune
//   1.5 to 1.8 s on the weather channel, closed by stop
// with a confident burst on the marine channel and a weak one on weather.
void record(const std::string& root)
{
    capture::WriterConfig config;
    config.root = fs::path(root);
    config.max_segment_bytes = 0;
    config.max_segment_seconds = 0.5;
    config.stream_epoch = kEpoch;
    config.hw = "synthetic";
    capture::SegmentWriter writer(std::move(config));
    REQUIRE(writer.start().has_value());

    capture::Annotation burst;
    burst.sample_count = 100'000;
    burst.freq_lower = kMarine + 10'000;
    burst.freq_upper = kMarine + 22'000;
    burst.confidence = 0.92;
    burst.snr_db = 21.0;
    burst.peak_db = -30.0;
    burst.family = "nfm";
    writer.add_annotation(200'000, burst);

    capture::Annotation faint;
    faint.sample_count = 50'000;
    faint.freq_lower = kWeather - 8'000;
    faint.freq_upper = kWeather + 8'000;
    faint.confidence = 0.3;
    faint.snr_db = 4.0;
    faint.peak_db = -70.0;
    writer.add_annotation(1'600'000, faint);

    std::vector<std::byte> bytes(kBlock * 2);
    const auto feed = [&](dsp::SampleIndex start, dsp::Hertz center, std::uint64_t dropped) {
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<std::byte>(127 + static_cast<int>((start + i) % 3) - 1);
        }
        source::SourceBlock block;
        block.stamp = dsp::BlockTimestamp{start, kAnchor, kRate};
        block.format = source::SampleFormat::Cu8;
        block.sample_count = kBlock;
        block.bytes = bytes;
        block.dropped_before = dropped;
        writer.consume(block, center);
    };

    for (dsp::SampleIndex index = 0; index < 1'000'000; index += kBlock) {
        feed(index, kMarine, 0);
    }
    feed(1'100'000, kMarine, 100'000);
    for (dsp::SampleIndex index = 1'110'000; index < 1'500'000; index += kBlock) {
        feed(index, kMarine, 0);
    }
    for (dsp::SampleIndex index = 1'500'000; index < 1'800'000; index += kBlock) {
        feed(index, kWeather, 0);
    }
    writer.stop();
    INFO(writer.stats().last_error);
    REQUIRE(writer.stats().last_error.empty());
}

struct Run {
    bool ok = false;
    std::string error;
    std::string text;
    std::vector<std::string> lines;
};

[[nodiscard]] Run run(std::vector<std::string> args)
{
    Run out;
    tool::Environment env;
    env.now_ns = kAnchor + 2 * 1'000'000'000LL;
    env.out = [&](std::string_view line) {
        out.lines.emplace_back(line);
        out.text += line;
        out.text += '\n';
    };
    auto status = tool::run(args, env);
    out.ok = status.has_value();
    if (!status) {
        out.error = status.error().message;
    }
    return out;
}

[[nodiscard]] bool contains(const std::string& text, std::string_view needle)
{
    return text.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("times, durations, bands and URIs parse the way the help says", "[capture][tools]")
{
    const std::int64_t now = kAnchor;
    CHECK(tool::parse_time("now", "--to", now).value() == now);
    CHECK(tool::parse_time("-10m", "--from", now).value() == now - 600'000'000'000LL);
    CHECK(tool::parse_time("now-1h30m", "--from", now).value() == now - 5'400'000'000'000LL);
    CHECK(tool::parse_time("2026-10-12T00:00:01.5Z", "--from", now).value() ==
          kAnchor + 1'500'000'000LL);
    CHECK(tool::parse_time(std::to_string(kAnchor), "--from", now).value() == kAnchor);

    auto zoneless = tool::parse_time("2026-10-12T00:00:01", "--from", now);
    REQUIRE_FALSE(zoneless.has_value());
    CHECK(contains(zoneless.error().message, "--from"));

    CHECK(tool::parse_duration("2s", "--min-duration").value() == 2'000'000'000LL);
    CHECK(tool::parse_duration("1.5ms", "--min-duration").value() == 1'500'000LL);
    auto unitless = tool::parse_duration("2", "--min-duration");
    REQUIRE_FALSE(unitless.has_value());
    CHECK(contains(unitless.error().message, "write 2s"));

    auto band = tool::parse_band("162.4M:162.56M", "--freq");
    REQUIRE(band.has_value());
    CHECK(band->first == 162'400'000);
    CHECK(band->second == 162'560'000);
    CHECK_FALSE(tool::parse_band("162.56M:162.4M", "--freq").has_value());

    CHECK(tool::format_frequency(162'550'000) == "162.55M");
    CHECK(tool::format_frequency(98'000'000) == "98M");
    CHECK(tool::format_frequency(7'074'000) == "7.074M");

    CHECK(tool::capture_uri("C:/radio captures", kAnchor, kAnchor + 1'000'000'000LL) ==
          "capture:///C:/radio%20captures?from=2026-10-12T00:00:00.000Z"
          "&to=2026-10-12T00:00:01.000Z");
    CHECK(tool::capture_uri("//nas/share/cap", kAnchor, kAnchor).starts_with(
        "capture://nas/share/cap?"));
}

TEST_CASE("list, search and extract read a root the writer made", "[capture][tools]")
{
    Scratch scratch;
    const std::string root = scratch.root();
    record(root);

    SECTION("list shows each segment, the gap and the totals")
    {
        const Run list = run({"list", root});
        INFO(list.error << "\n" << list.text);
        REQUIRE(list.ok);
        CHECK(contains(list.text, "2026-10-12T00:00:00.000Z"));
        CHECK(contains(list.text, "156.8M"));
        CHECK(contains(list.text, "162.55M"));
        CHECK(contains(list.text, "retune"));
        CHECK(contains(list.text, "loss"));
        CHECK(contains(list.text, "4 segments"));
        CHECK(contains(list.text, "gaps:"));
        CHECK(contains(list.text, "100000 samples (0.100s) missing from 2026-10-12T00:00:01.000Z"));
        CHECK_FALSE(contains(list.text, "unfinished:"));

        const Run late = run({"list", root, "--from", at(1.6)});
        REQUIRE(late.ok);
        CHECK(contains(late.text, "1 segment,"));
    }

    SECTION("search finds the burst and says how to replay and hear it")
    {
        const Run search = run({"search", root, "--freq", "156.7M:156.9M"});
        INFO(search.error << "\n" << search.text);
        REQUIRE(search.ok);
        CHECK(contains(search.text, "1 hit in 4 segments"));
        CHECK(contains(search.text, "2026-10-12T00:00:00.200Z"));
        CHECK(contains(search.text, "nfm"));
        CHECK(contains(search.text, "replay  capture:///"));
        CHECK(contains(search.text, "from=2026-10-11T23:59:59.200Z&to=2026-10-12T00:00:01.300Z"));
        CHECK(contains(search.text, "--vrx 156.816M"));

        const Run all = run({"search", root});
        REQUIRE(all.ok);
        CHECK(contains(all.text, "2 hits"));

        const Run confident = run({"search", root, "--min-confidence", "0.5"});
        REQUIRE(confident.ok);
        CHECK(contains(confident.text, "1 hit "));
        CHECK_FALSE(contains(confident.text, "162.542M"));

        const Run json = run({"search", root, "--family=nfm", "--json"});
        INFO(json.text);
        REQUIRE(json.ok);
        CHECK(json.lines.front() == "[");
        CHECK(json.lines.back() == "]");
        CHECK(contains(json.text, "\"family\": \"nfm\""));
        CHECK(contains(json.text, "\"vrx\": \"156.816M\""));

        const Run bad = run({"search", root, "--generator", "mine"});
        REQUIRE_FALSE(bad.ok);
        CHECK(contains(bad.error, "revenant-index"));

        const Run typo = run({"search", root, "--frequency", "156.8M"});
        REQUIRE_FALSE(typo.ok);
        CHECK(contains(typo.error, "does not take --frequency"));
    }

    SECTION("occupancy tells occupied from quiet from not captured")
    {
        const Run chart = run({"occupancy", root, "--freq", "156.7M:156.9M", "--bucket", "100ms",
                               "--from", at(0.0), "--to", at(1.2)});
        INFO(chart.error << "\n" << chart.text);
        REQUIRE(chart.ok);
        CHECK(contains(chart.text, "12 buckets of 0.100s"));
        // 0.2 to 0.3 s holds the burst; 1.0 to 1.1 s is the lost tenth.
        CHECK(contains(chart.text, std::format("{}  |{}|", at(0.2), std::string(40, '#'))));
        CHECK(contains(chart.text, std::format("{}  |{}|", at(0.0), std::string(40, '-'))));
        CHECK(contains(chart.text, std::format("{}  |{}|", at(1.0), std::string(40, ' '))));

        const Run missing = run({"occupancy", root});
        REQUIRE_FALSE(missing.ok);
        CHECK(contains(missing.error, "--freq"));
    }

    SECTION("extract copies whole segments and will not overwrite")
    {
        const std::string out = (scratch.path() / "extract").generic_string();
        const Run extract =
            run({"extract", root, "--from", at(0.6), "--to", at(1.2), "--out", out});
        INFO(extract.error << "\n" << extract.text);
        REQUIRE(extract.ok);
        CHECK(contains(extract.text, "copied 2 segments"));
        CHECK(contains(extract.text, "Whole segments"));

        std::size_t files = 0;
        for (const auto& entry : fs::directory_iterator(out)) {
            (void)entry;
            ++files;
        }
        CHECK(files == 4);

        const Run again =
            run({"extract", root, "--from", at(0.6), "--to", at(1.2), "--out", out});
        REQUIRE_FALSE(again.ok);
        CHECK(contains(again.error, "already exists"));

        const Run empty = run({"extract", root, "--from", "-1h", "--to", "-30m", "--out", out});
        REQUIRE_FALSE(empty.ok);
        CHECK(contains(empty.error, "nothing was captured"));
    }

    SECTION("recover on a clean root finalises nothing")
    {
        const Run recover = run({"recover", root});
        INFO(recover.error);
        REQUIRE(recover.ok);
        CHECK(contains(recover.text, "0 segments finalised"));
    }
}

TEST_CASE("index runs the detector over each segment and writes it back",
          "[gpu][capture][tools]")
{
    REVENANT_NEEDS_GPU();

    Scratch scratch;
    const std::string root = scratch.root();
    record(root);

    const Run index = run({"index", root, "--force"});
    INFO(index.error << "\n" << index.text);
    REQUIRE(index.ok);
    CHECK(contains(index.text, "4 indexed"));
    CHECK(contains(index.text, "x realtime"));
}
