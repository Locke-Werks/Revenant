// DecodeAnnotator: decoded identifiers turned into coalesced capture spans.
//
// Every case drives it with plain stream indices, since it has no clock of
// its own, and collects what it emits.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/capture/decode_annotations.h"
#include "core/capture/format.h"

using namespace revenant;
using capture::Annotation;
using capture::DecodeAnnotator;

namespace {

constexpr std::uint64_t kGap = 10'000;

struct Emitted {
    std::uint64_t start = 0;
    Annotation annotation;
};

struct Harness {
    std::vector<Emitted> out;
    DecodeAnnotator annotator{kGap, [this](std::uint64_t start, const Annotation& a) {
                                  out.push_back({start, a});
                              }};
};

[[nodiscard]] DecodeAnnotator::Sighting sighting(
    std::uint32_t vrx, std::string decoder,
    std::vector<std::pair<std::string, std::string>> fields, std::uint64_t end,
    std::uint64_t span = 100)
{
    DecodeAnnotator::Sighting s;
    s.vrx = vrx;
    s.decoder = std::move(decoder);
    s.fields = std::move(fields);
    s.end_index = end;
    s.span_samples = span;
    s.freq_lower_hz = 852'000'000.0;
    s.freq_upper_hz = 852'012'500.0;
    return s;
}

}  // namespace

TEST_CASE("messages within the gap coalesce into one span", "[capture][decode]")
{
    Harness h;
    h.annotator.observe(sighting(1, "p25p1", {{"talkgroup", "101"}}, 1'000));
    h.annotator.observe(sighting(1, "p25p1", {{"talkgroup", "101"}}, 5'000));
    h.annotator.observe(sighting(1, "p25p1", {{"talkgroup", "101"}}, 14'000));
    CHECK(h.out.empty());
    CHECK(h.annotator.open_spans() == 1);

    h.annotator.finish();
    REQUIRE(h.out.size() == 1);
    const Emitted& e = h.out.front();
    CHECK(e.start == 900);
    CHECK(e.annotation.sample_count == 13'100);
    CHECK(e.annotation.messages == 3);
    CHECK(e.annotation.decoder == "p25p1");
    REQUIRE(e.annotation.decoded.size() == 1);
    CHECK(e.annotation.decoded[0].first == "talkgroup");
    CHECK(e.annotation.decoded[0].second == "101");
    CHECK(e.annotation.confidence == 1.0);
    CHECK(e.annotation.freq_lower == 852'000'000);
    CHECK(e.annotation.freq_upper == 852'012'500);
    CHECK(capture::annotation_label(e.annotation) == "p25p1 talkgroup 101");
}

TEST_CASE("a quiet stretch longer than the gap splits the span", "[capture][decode]")
{
    Harness h;
    h.annotator.observe(sighting(1, "ais", {{"mmsi", "366123456"}}, 1'000));
    // 1'000 + kGap is not yet past the gap.
    h.annotator.observe(sighting(1, "ais", {{"mmsi", "366123456"}}, 11'000));
    CHECK(h.out.empty());
    h.annotator.observe(sighting(1, "ais", {{"mmsi", "366123456"}}, 21'001));
    REQUIRE(h.out.size() == 1);
    CHECK(h.out[0].start == 900);
    CHECK(h.out[0].annotation.sample_count == 10'100);
    CHECK(h.out[0].annotation.messages == 2);

    h.annotator.advance(21'001 + kGap);
    CHECK(h.out.size() == 1);
    h.annotator.advance(21'002 + kGap);
    REQUIRE(h.out.size() == 2);
    CHECK(h.out[1].start == 20'901);
    CHECK(h.out[1].annotation.messages == 1);
}

TEST_CASE("close_vrx ends only that receiver's spans", "[capture][decode]")
{
    Harness h;
    h.annotator.observe(sighting(1, "dmr", {{"talkgroup", "9"}, {"source", "3100123"}}, 1'000));
    h.annotator.observe(sighting(2, "dmr", {{"talkgroup", "9"}}, 1'000));
    CHECK(h.annotator.open_spans() == 3);

    h.annotator.close_vrx(1);
    REQUIRE(h.out.size() == 2);
    for (const Emitted& e : h.out) {
        CHECK(e.annotation.decoder == "dmr");
    }
    CHECK(h.annotator.open_spans() == 1);

    // The same identifier on the retuned receiver opens a new span.
    h.annotator.observe(sighting(1, "dmr", {{"talkgroup", "9"}}, 2'000));
    CHECK(h.annotator.open_spans() == 2);
    h.annotator.finish();
    CHECK(h.out.size() == 4);
    CHECK(h.annotator.open_spans() == 0);
}

TEST_CASE("only allowlisted identifier keys are indexed", "[capture][decode]")
{
    Harness h;
    h.annotator.observe(sighting(
        1, "p25p1", {{"nac", "3344"}, {"duid", "7"}, {"talkgroup", "101"}, {"source", ""}}, 1'000));
    h.annotator.observe(sighting(1, "rtty", {{"text", "CQ CQ"}}, 1'000));
    h.annotator.finish();
    REQUIRE(h.out.size() == 1);
    CHECK(h.out[0].annotation.decoded[0].first == "talkgroup");

    CHECK(DecodeAnnotator::indexed("ais", "callsign"));
    CHECK(DecodeAnnotator::indexed("pocsag", "address"));
    CHECK(DecodeAnnotator::indexed("ax25", "source"));
    CHECK(DecodeAnnotator::indexed("m17", "source"));
    CHECK(DecodeAnnotator::indexed("dstar", "my"));
    CHECK_FALSE(DecodeAnnotator::indexed("ax25", "comment"));
    CHECK_FALSE(DecodeAnnotator::indexed("pocsag", "message"));
    CHECK_FALSE(DecodeAnnotator::indexed("cw", "source"));
}

TEST_CASE("a span longer than the stream so far starts at zero", "[capture][decode]")
{
    Harness h;
    h.annotator.observe(sighting(1, "m17", {{"source", "N0CALL"}}, 50, 4'000));
    h.annotator.finish();
    REQUIRE(h.out.size() == 1);
    CHECK(h.out[0].start == 0);
    CHECK(h.out[0].annotation.sample_count == 50);
}
