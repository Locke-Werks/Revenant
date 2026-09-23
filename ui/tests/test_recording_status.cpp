// describe_playback and compare_opened_recording: what the strip under the top
// bar says while a recording plays, and what it says when the engine opened
// something other than what the preview read.
//
// EVERY TEST NAMES THE WRONG IMPLEMENTATION IT REJECTS. The ones that matter:
// calling an unthrottled replay "realtime" because the factor is not read; a
// strip that waits for a delivered count the last short block never reaches
// and claims playback forever; and a comparison that is not made, which is
// the one place a remote engine opening a different file would show.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "models/recording_status.h"

using revenant::ui::compare_opened_recording;
using revenant::ui::describe_playback;
using revenant::ui::format_clock;
using revenant::ui::format_recording_length;
using revenant::ui::OpenedRecording;
using revenant::ui::PlaybackSample;

TEST_CASE("a clock carries hours only when there are some", "[recording]")
{
    // Rejects: "0:00:07" on a seven-second file, and minutes past sixty.
    CHECK(format_clock(7) == "0:07");
    CHECK(format_clock(754) == "12:34");
    CHECK(format_clock(3726) == "1:02:06");
    CHECK(format_recording_length(357'739'520u, 96000) == "1:02:06");
    CHECK(format_recording_length(357'739'520u, 0).empty());
    CHECK(format_recording_length(0, 96000).empty());
}

TEST_CASE("position reads against length, and the pace says what paces it", "[recording]")
{
    // Rejects: a pace that ignores sourcePacedBy. An engine started with
    // --pace 0 replays as fast as the GPU goes, and a strip saying "realtime"
    // there is describing a setting nobody made.
    PlaybackSample sample;
    sample.rate = 96000;
    sample.length = 357'739'520u;
    sample.delivered = 96000u * 754u;
    sample.running = true;

    sample.paced_by = 1.0;
    auto line = describe_playback(sample);
    CHECK(line.position == "12:34 / 1:02:06");
    CHECK(line.pace == "realtime");
    CHECK_FALSE(line.ended);
    CHECK(line.fraction > 0.2);
    CHECK(line.fraction < 0.21);

    sample.paced_by = 0.0;
    sample.realtime_factor = 11.84;
    CHECK(describe_playback(sample).pace == "unthrottled, 11.8x");
    sample.realtime_factor = 0.0;
    CHECK(describe_playback(sample).pace == "unthrottled");

    sample.paced_by = 0.5;
    CHECK(describe_playback(sample).pace == "0.5x realtime");
}

TEST_CASE("the end is every sample delivered, or an engine that stopped", "[recording]")
{
    // Rejects: waiting for delivered to equal length exactly. A demand
    // source's last block can land short, the engine stops running, and a
    // strip waiting for the count would claim playback forever.
    PlaybackSample sample;
    sample.rate = 1000;
    sample.length = 10'000;
    sample.running = true;
    sample.delivered = 10'000;
    auto line = describe_playback(sample);
    CHECK(line.ended);
    CHECK(line.position == "ended at 0:10");
    CHECK(line.pace.empty());
    CHECK(line.fraction == 1.0);

    sample.delivered = 9'990;
    sample.running = false;
    CHECK(describe_playback(sample).ended);

    // Not yet started is not ended: an engine between the open and the run.
    sample.delivered = 0;
    CHECK_FALSE(describe_playback(sample).ended);
}

TEST_CASE("no rate or no length says nothing rather than a wrong clock", "[recording]")
{
    // Rejects: a position on a live radio, whose length is zero because it is
    // unbounded, not because it is empty.
    PlaybackSample sample;
    sample.rate = 2'400'000;
    sample.delivered = 5'000'000;
    sample.running = true;
    const auto line = describe_playback(sample);
    CHECK(line.position.empty());
    CHECK_FALSE(line.ended);
}

TEST_CASE("what the engine opened is compared with what the preview read", "[recording]")
{
    // Rejects: assuming they agree. On one machine they always do; through a
    // tunnel the engine opens whatever is at that path on its own disk.
    const OpenedRecording planned{357'739'520u, 96000, "cs24", 7'150'000};
    CHECK(compare_opened_recording(planned, planned).empty());

    const OpenedRecording other{100'000u, 2'400'000, "cs16", 7'150'000};
    const std::string said = compare_opened_recording(planned, other);
    CHECK(said.find("100000 samples") != std::string::npos);
    CHECK(said.find("2400000 S/s") != std::string::npos);
    CHECK(said.find("cs16") != std::string::npos);
    CHECK(said.find("its own machine") != std::string::npos);
}
