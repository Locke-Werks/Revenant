// BurstTimeline: transmissions merged into bursts, attachments pinned by
// overlap, history kept within bounds, and the CSV a burst exports as.
//
// EVERY TEST HERE NAMES THE WRONG IMPLEMENTATION IT REJECTS: a held detector
// track drawn as one bar over every key-up, a transcript pinned to the wrong
// channel because it was nearest in time, history that grows for a week, and
// a transcript with a comma that splits into two CSV columns.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "models/burst_timeline.h"

using revenant::ui::Burst;
using revenant::ui::burst_csv;
using revenant::ui::burst_csv_field;
using revenant::ui::burst_replay_uri;
using revenant::ui::burst_utc_text;
using revenant::ui::BurstAttachment;
using revenant::ui::BurstAttachmentKind;
using revenant::ui::BurstConfig;
using revenant::ui::BurstObservation;
using revenant::ui::BurstTimeline;
using revenant::ui::kBurstSecondNs;

namespace {

constexpr std::int64_t kT0 = 1'760'000'000LL * kBurstSecondNs;

BurstObservation seen(std::uint64_t source, std::int64_t hz, double from_s, double to_s,
                      std::string mode = {})
{
    BurstObservation obs;
    obs.source = source;
    obs.center_hz = hz;
    obs.bandwidth_hz = 12'500;
    obs.start_ns = kT0 + static_cast<std::int64_t>(from_s * 1e9);
    obs.end_ns = kT0 + static_cast<std::int64_t>(to_s * 1e9);
    obs.mode = std::move(mode);
    return obs;
}

std::int64_t at(double s) { return kT0 + static_cast<std::int64_t>(s * 1e9); }

BurstAttachment said(std::int64_t hz, double from_s, double to_s, std::string text,
                     BurstAttachmentKind kind = BurstAttachmentKind::Transcript)
{
    BurstAttachment a;
    a.kind = kind;
    a.hz = hz;
    a.start_ns = at(from_s);
    a.end_ns = at(to_s);
    a.text = std::move(text);
    return a;
}

}  // namespace

TEST_CASE("observations of one source within the hang time extend one burst", "[burst]")
{
    BurstTimeline t;
    t.observe(seen(1, 146'520'000, 0.0, 1.0, "nfm"));
    t.observe(seen(1, 146'520'200, 0.0, 2.5));
    t.tick(at(3.0));
    REQUIRE(t.bursts().size() == 1);
    CHECK(t.bursts()[0].open);
    CHECK(t.bursts()[0].end_ns == at(2.5));
    CHECK(t.bursts()[0].mode == "nfm");
}

TEST_CASE("a lane quiet past the hang time closes, and later activity opens a new burst", "[burst]")
{
    BurstTimeline t;
    t.observe(seen(7, 460'000'000, 0.0, 1.0));
    t.tick(at(4.0));
    REQUIRE_FALSE(t.bursts()[0].open);

    // The detector held the track, so its first sighting is still 0 s. The
    // new burst must begin after the old one ended, not at 0 s.
    t.observe(seen(7, 460'000'000, 0.0, 6.0));
    REQUIRE(t.bursts().size() == 2);
    CHECK(t.bursts()[1].start_ns >= t.bursts()[0].end_ns);
    CHECK(t.bursts()[1].open);
    CHECK(t.lanes().size() == 1);
}

TEST_CASE("centres inside the lane tolerance share a lane, others get their own", "[burst]")
{
    BurstTimeline t;
    t.observe(seen(1, 100'000'000, 0.0, 1.0));
    t.observe(seen(2, 100'003'000, 10.0, 11.0));
    t.observe(seen(3, 100'025'000, 10.0, 11.0));
    const auto lanes = t.lanes();
    REQUIRE(lanes.size() == 2);
    CHECK(lanes[0] == 100'000'000);
    CHECK(lanes[1] == 100'025'000);
}

TEST_CASE("an attachment goes to the burst it overlaps in frequency, not the nearest in time",
          "[burst]")
{
    BurstTimeline t;
    t.observe(seen(1, 155'000'000, 0.0, 5.0));
    t.observe(seen(2, 156'000'000, 4.0, 5.0));
    t.attach(said(155'000'000, 4.0, 5.0, "unit 12 en route"), at(5.0));
    CHECK(t.bursts()[0].transcript() == "unit 12 en route");
    CHECK(t.bursts()[1].transcript().empty());
}

TEST_CASE("a decoded message stamped inside the hang time still attaches", "[burst]")
{
    BurstTimeline t;
    t.observe(seen(1, 851'000'000, 0.0, 3.0));
    BurstAttachment d = said(851'000'000, 4.0, 4.0, "tg 101", BurstAttachmentKind::Decoded);
    d.ids = {"talkgroup=101", "source=4401"};
    t.attach(d, at(4.0));
    REQUIRE(t.bursts()[0].ids().size() == 2);
    CHECK(t.bursts()[0].decoded() == "tg 101");
}

TEST_CASE("an attachment that arrives before its burst waits and is placed later", "[burst]")
{
    BurstTimeline t;
    t.attach(said(144'390'000, 1.0, 2.0, "aprs"), at(2.0));
    CHECK(t.pending() == 1);
    t.observe(seen(9, 144'390'000, 0.5, 2.5));
    // Retried on the tick, not per observation.
    t.tick(at(2.6));
    CHECK(t.pending() == 0);
    CHECK(t.bursts()[0].transcript() == "aprs");
}

TEST_CASE("a lane goes when the last burst on it is trimmed", "[burst]")
{
    BurstConfig config;
    config.max_bursts = 1;
    BurstTimeline t(config);
    t.observe(seen(1, 851'000'000, 0.0, 1.0));
    t.tick(at(10.0));
    t.observe(seen(2, 852'000'000, 11.0, 12.0));
    t.tick(at(12.5));
    REQUIRE(t.lanes().size() == 1);
    CHECK(t.lanes().front() == 852'000'000);
}

TEST_CASE("pending attachments that never find a burst expire", "[burst]")
{
    BurstTimeline t;
    t.attach(said(1, 0.0, 1.0, "lost"), at(1.0));
    t.tick(at(100.0));
    CHECK(t.pending() == 0);
}

TEST_CASE("history is bounded by count and by age", "[burst]")
{
    BurstConfig config;
    config.max_bursts = 5;
    config.max_age_ns = 60 * kBurstSecondNs;
    BurstTimeline t(config);
    for (int i = 0; i < 10; ++i) {
        t.observe(seen(static_cast<std::uint64_t>(i + 1), 100'000'000 + i * 100'000, i, i + 0.5));
    }
    t.tick(at(20.0));
    REQUIRE(t.bursts().size() == 5);
    CHECK(t.bursts().front().start_ns == at(5.0));

    t.tick(at(200.0));
    CHECK(t.bursts().empty());
}

TEST_CASE("visible returns only bursts overlapping the window", "[burst]")
{
    BurstTimeline t;
    t.observe(seen(1, 1, 0.0, 1.0));
    t.observe(seen(2, 900'000, 10.0, 11.0));
    CHECK(t.visible(at(5.0), at(20.0)).size() == 1);
    CHECK(t.visible(at(0.5), at(10.5)).size() == 2);
}

TEST_CASE("CSV quotes fields that hold separators and doubles their quotes", "[burst]")
{
    CHECK(burst_csv_field("plain") == "plain");
    CHECK(burst_csv_field("a, b") == "\"a, b\"");
    CHECK(burst_csv_field("say \"hi\"") == "\"say \"\"hi\"\"\"");

    BurstTimeline t;
    t.observe(seen(1, 146'520'000, 0.0, 1.5, "nfm"));
    t.attach(said(146'520'000, 0.0, 1.5, "copy, over"), at(1.5));
    const std::string csv = burst_csv(t.visible(at(-1.0), at(10.0)));
    const auto first_break = csv.find("\r\n");
    REQUIRE(csv.substr(0, first_break).starts_with("start_utc,end_utc,duration_s"));
    const std::string row = csv.substr(first_break + 2);
    CHECK(row.find("146520000,12500,nfm,") != std::string::npos);
    CHECK(row.find("1.500,") != std::string::npos);
    CHECK(row.find("\"copy, over\"") != std::string::npos);
    CHECK(row.ends_with("\r\n"));
}

TEST_CASE("UTC text is ISO 8601 to the millisecond", "[burst]")
{
    CHECK(burst_utc_text(0) == "1970-01-01T00:00:00.000Z");
    CHECK(burst_utc_text(1'250'000'000LL) == "1970-01-01T00:00:01.250Z");
}

TEST_CASE("a replay URI keeps the hit's root and takes the burst's span", "[burst]")
{
    const auto uri = burst_replay_uri("capture:///D:/cap?from=1&to=2&pace=1", 10, 20);
    REQUIRE(uri.has_value());
    CHECK(*uri == "capture:///D:/cap?from=10&to=20&pace=1");
    CHECK_FALSE(burst_replay_uri("rtlsdr://0", 1, 2).has_value());
}
