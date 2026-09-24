// summarise_status and meter_fraction: how loudly the main window speaks
// about the engine, and how far along a receiver strip's meter a level is.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows. For the status, the wrong implementations are a fault
// demoted to a note, which an operator finds only when they wonder why the
// band is empty, and an ordinary condition promoted to a fault, which is a
// banner that is always up and soon ignored.

#include <catch2/catch_test_macros.hpp>

#include "models/level_meter.h"
#include "models/status_summary.h"

using revenant::ui::meter_fraction;
using revenant::ui::meter_has_reading;
using revenant::ui::StatusInputs;
using revenant::ui::StatusLevel;
using revenant::ui::summarise_status;

namespace {

StatusInputs healthy()
{
    StatusInputs in;
    in.connected = true;
    in.engine_running = true;
    in.source_open = true;
    in.frame_rate = 36.6;
    return in;
}

}  // namespace

TEST_CASE("a healthy engine is quiet and the pill says the rate", "[status]")
{
    const auto summary = summarise_status(healthy());
    CHECK(summary.level == StatusLevel::Quiet);
    CHECK(summary.headline == "36.6 rows/s");
}

// Rejects the old window's habit of treating a clamp as news. It is the
// ordinary case, a ring rounded to a power of two, and a banner for it would
// be up on every engine there is.
TEST_CASE("a clamp, dropped frames and other receivers are notes", "[status]")
{
    auto in = healthy();
    in.clamped = true;
    CHECK(summarise_status(in).level == StatusLevel::Note);
    CHECK(summarise_status(in).headline == "36.6 rows/s");

    in = healthy();
    in.frames_dropped_by_engine = 3;
    CHECK(summarise_status(in).level == StatusLevel::Note);

    in = healthy();
    in.stranded = true;
    CHECK(summarise_status(in).level == StatusLevel::Note);
}

// Rejects a pill that names the first condition in whatever order the fields
// were declared. No engine outranks everything, because nothing else on
// screen is live without one.
TEST_CASE("no engine outranks every other fault", "[status]")
{
    StatusInputs in;
    in.connected = false;
    in.front_end_fault = true;
    in.detection_fault = true;
    const auto summary = summarise_status(in);
    CHECK(summary.level == StatusLevel::Bad);
    CHECK(summary.headline == "no engine");
}

TEST_CASE("the picture not being the radio's is bad", "[status]")
{
    auto in = healthy();
    in.front_end_fault = true;
    CHECK(summarise_status(in).level == StatusLevel::Bad);
    CHECK(summarise_status(in).headline == "front end overloaded");

    in = healthy();
    in.source_behind = true;
    CHECK(summarise_status(in).headline == "source behind");

    in = healthy();
    in.detection_fault = true;
    CHECK(summarise_status(in).headline == "detector refused");
}

// Rejects promoting what the operator did not get to a fault. A refused tune
// is worth saying and nothing on screen is wrong because of it.
TEST_CASE("what the operator asked for and did not get is a warning", "[status]")
{
    auto in = healthy();
    in.engine_running = false;
    CHECK(summarise_status(in).level == StatusLevel::Warn);
    CHECK(summarise_status(in).headline == "engine stopped");

    in = healthy();
    in.source_open = false;
    CHECK(summarise_status(in).headline == "no source open");

    in = healthy();
    in.tune_fault = true;
    CHECK(summarise_status(in).headline == "tune refused");

    in = healthy();
    in.source_fault = true;
    CHECK(summarise_status(in).headline == "radio refused");

    in = healthy();
    in.receiver_gone = true;
    CHECK(summarise_status(in).headline == "receiver let go");
}

// A fault and a note together are the fault. The note is still in the
// drawer; the pill names the one that matters first.
TEST_CASE("a fault hides a note in the pill but not its level", "[status]")
{
    auto in = healthy();
    in.clamped = true;
    in.tune_fault = true;
    CHECK(summarise_status(in).level == StatusLevel::Warn);
    CHECK(summarise_status(in).headline == "tune refused");
}

// Rejects the duplication the owner found on 2026-09-23: "receiver let go"
// beside the receivers button in the pill and again as a chip in a strip
// under the bar. The pill names one condition and the chips are the rest, so
// no label is ever on screen twice.
TEST_CASE("the pill's headline is never also a chip", "[status]")
{
    auto in = healthy();
    in.receiver_gone = true;
    auto summary = summarise_status(in);
    CHECK(summary.headline == "receiver let go");
    CHECK(summary.chips.empty());

    in.tune_fault = true;
    in.source_fault = true;
    summary = summarise_status(in);
    CHECK(summary.headline == "radio refused");
    REQUIRE(summary.chips.size() == 2);
    CHECK(summary.chips[0] == "tune refused");
    CHECK(summary.chips[1] == "receiver let go");
}

// Rejects dropping a notice because a worse one holds the pill. Every
// condition that was a chip in the old strip is still on screen once: the
// strip showed the three refusals and the let-go receiver with no engine too.
TEST_CASE("every true chip condition is on screen exactly once", "[status]")
{
    StatusInputs in;
    in.connected = false;
    in.tune_fault = true;
    in.receiver_gone = true;
    auto summary = summarise_status(in);
    CHECK(summary.headline == "no engine");
    CHECK(summary.level == StatusLevel::Bad);
    REQUIRE(summary.chips.size() == 2);
    CHECK(summary.chips[0] == "tune refused");
    CHECK(summary.chips[1] == "receiver let go");

    in = healthy();
    in.front_end_fault = true;
    in.source_behind = true;
    in.detection_fault = true;
    summary = summarise_status(in);
    CHECK(summary.headline == "front end overloaded");
    REQUIRE(summary.chips.size() == 2);
    CHECK(summary.chips[0] == "source behind");
    CHECK(summary.chips[1] == "detector refused");
}

// Rejects inventing chips for the two states the pill names and the drawer
// explains. The old strip had no chip for either, and a chip reading "engine
// stopped" beside a pill reading "detector refused" would be a new notice
// nobody asked for.
TEST_CASE("a stopped engine and a closed source are the pill's alone", "[status]")
{
    auto in = healthy();
    in.detection_fault = true;
    in.engine_running = false;
    in.source_open = false;
    auto summary = summarise_status(in);
    CHECK(summary.headline == "detector refused");
    CHECK(summary.chips.empty());

    in = healthy();
    in.engine_running = false;
    in.receiver_gone = true;
    summary = summarise_status(in);
    CHECK(summary.headline == "engine stopped");
    REQUIRE(summary.chips.size() == 1);
    CHECK(summary.chips[0] == "receiver let go");
}

// Rejects drawing the engine's placeholder as a level. -200 dBFS is "not
// measured yet", and an empty bar for it reads as a receiver hearing nothing.
TEST_CASE("the meter has no reading at the engine's placeholder", "[meter]")
{
    CHECK_FALSE(meter_has_reading(-200.0));
    CHECK(meter_fraction(-200.0) == 0.0);
    CHECK(meter_has_reading(-120.0));
}

TEST_CASE("the meter is a fixed scale, pinned at its ends", "[meter]")
{
    CHECK(meter_fraction(-80.0) == 0.0);
    CHECK(meter_fraction(-20.0) == 1.0);
    CHECK(meter_fraction(-50.0) == 0.5);
    CHECK(meter_fraction(-120.0) == 0.0);
    CHECK(meter_fraction(5.4) == 1.0);
}
