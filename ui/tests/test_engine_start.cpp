// plan_engine_launch, engine_exit_sentence, remembers_as_last_radio and
// plan_last_source: when the window starts an engine of its own, what it says
// when that engine goes, and when it reopens the radio used last.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows. Nothing here starts a process: the Job object and the
// pipe are models/engine_launcher.cpp and are verified on a staged payload.

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "models/engine_start.h"
#include "models/status_summary.h"

using revenant::ui::engine_exit_reason_line;
using revenant::ui::engine_exit_sentence;
using revenant::ui::EngineLaunch;
using revenant::ui::EngineLaunchFacts;
using revenant::ui::is_launchable_address;
using revenant::ui::LastSourceAction;
using revenant::ui::LastSourceFacts;
using revenant::ui::plan_engine_launch;
using revenant::ui::plan_last_source;
using revenant::ui::remembers_as_last_radio;
using revenant::ui::StatusInputs;
using revenant::ui::StatusLevel;
using revenant::ui::summarise_status;

namespace {

EngineLaunchFacts installed_and_idle()
{
    EngineLaunchFacts facts;
    facts.launchable_address = true;
    facts.engine_beside = true;
    facts.port_answers = false;
    return facts;
}

}  // namespace

// Rejects "is this loopback" as the test. The engine binds 127.0.0.1 and is
// started with no bind address, so 127.0.0.2 and ::1 are loopback addresses
// that reach nothing it serves; starting one for them gives the person an
// engine the window cannot connect to.
TEST_CASE("an engine is started only for an address it would be reached at", "[engine-start]")
{
    CHECK(is_launchable_address("127.0.0.1"));
    CHECK(is_launchable_address("localhost"));
    CHECK(is_launchable_address("LocalHost"));

    CHECK_FALSE(is_launchable_address("127.0.0.2"));
    CHECK_FALSE(is_launchable_address("::1"));
    CHECK_FALSE(is_launchable_address("[::1]"));
    CHECK_FALSE(is_launchable_address("192.168.1.20"));
    CHECK_FALSE(is_launchable_address("radio.example"));
    CHECK_FALSE(is_launchable_address("localhost."));
    CHECK_FALSE(is_launchable_address(""));
}

// The installed case: loopback, the engine beside the window, nothing on the
// port.
TEST_CASE("the installed layout with nothing running starts an engine", "[engine-start]")
{
    CHECK(plan_engine_launch(installed_and_idle()) == EngineLaunch::Start);
}

// Rejects starting a second engine beside one somebody started, a headless
// recorder for instance. The second would fail to bind at best, and the
// owner's rule is that an engine somebody started is never touched.
TEST_CASE("an engine already answering is used and not joined by another", "[engine-start]")
{
    auto facts = installed_and_idle();
    facts.port_answers = true;
    CHECK(plan_engine_launch(facts) == EngineLaunch::PortAnswers);
}

// Rejects a development build trying to start what is not there, which is
// also what keeps CI's smoke run, run out of ui/'s own build tree, from
// starting anything.
TEST_CASE("no engine beside the window starts nothing", "[engine-start]")
{
    auto facts = installed_and_idle();
    facts.engine_beside = false;
    CHECK(plan_engine_launch(facts) == EngineLaunch::NoEngineBeside);
}

// Rejects starting a local engine for a remote address, which the window would
// then never reach, and rejects any order that lets a later fact override the
// address: main() does not take the probe for a remote address at all, so the
// port answer it passes then is a default and must not be read.
TEST_CASE("a remote address starts nothing whatever else is true", "[engine-start]")
{
    auto facts = installed_and_idle();
    facts.launchable_address = false;
    CHECK(plan_engine_launch(facts) == EngineLaunch::NotLaunchableAddress);

    facts.engine_beside = false;
    facts.port_answers = true;
    CHECK(plan_engine_launch(facts) == EngineLaunch::NotLaunchableAddress);
}

// Rejects "the last line of stderr". tools/engined/main.cpp ends a failed
// serve() with the reason and then "  code <n>", so the last line alone would
// tell the person a number.
TEST_CASE("the exit sentence carries the engine's reason, not its last line", "[engine-start]")
{
    const std::vector<std::string> tail = {
        "",
        "revenant-engine: no Vulkan device supports the compute queue this engine needs",
        "  code 7",
    };
    CHECK(engine_exit_reason_line(tail) ==
          "no Vulkan device supports the compute queue this engine needs");
    CHECK(engine_exit_sentence(1, tail) ==
          "the engine this window started exited with code 1: no Vulkan device supports the "
          "compute queue this engine needs");
}

// The latest reason wins when the engine wrote two, which is an option refused
// and then the help hint that follows it.
TEST_CASE("the latest line naming the engine is the reason", "[engine-start]")
{
    const std::vector<std::string> tail = {
        "revenant-engine: unknown option --no-source",
        "Try revenant-engine --help.",
    };
    CHECK(engine_exit_reason_line(tail) == "unknown option --no-source");
}

// Rejects calling a line that does not name the engine its reason. Measured on
// the staged payload: an engine killed from outside had last written
// librtlsdr's tuner line, and the first version put it after a colon as though
// the tuner had ended the engine. It is quoted as the last thing said, trimmed
// of the indent the engine gives continuation lines.
TEST_CASE("an unnamed last line is quoted and not called a reason", "[engine-start]")
{
    const std::vector<std::string> tail = {"warning: something", "   the rest of it", "   "};
    CHECK(engine_exit_reason_line(tail).empty());
    CHECK(engine_exit_sentence(3, tail) ==
          "the engine this window started exited with code 3; the last thing it wrote was "
          "\"the rest of it\"");
}

// Rejects a sentence that ends in a colon and nothing, rejects printing a crash
// as a four-billion exit code, and rejects calling every high code a crash:
// 0xC0000005 is an access violation, and 0xFFFFFFFF is what Stop-Process and
// Task Manager end a process with, which the first version called an exception.
TEST_CASE("a crash is said in hex and silence is said as silence", "[engine-start]")
{
    const std::vector<std::string> nothing;
    CHECK(engine_exit_sentence(0xC0000005U, nothing) ==
          "the engine this window started crashed with exception 0xC0000005, and said "
          "nothing on stderr");
    CHECK(engine_exit_sentence(0xFFFFFFFFU, nothing) ==
          "the engine this window started exited with code 0xFFFFFFFF, and said nothing on "
          "stderr");
    CHECK(engine_exit_sentence(2, nothing) ==
          "the engine this window started exited with code 2, and said nothing on stderr");
}

// Rejects remembering a recording as the radio used last, which would start
// playing last night's file at launch, and rejects matching the scheme by case.
TEST_CASE("a recording is not remembered as the radio used last", "[engine-start]")
{
    CHECK(remembers_as_last_radio("rtlsdr://0?freq=98100000&rate=2400000"));
    CHECK(remembers_as_last_radio("synthetic:wideband?rate=2400000"));
    CHECK_FALSE(remembers_as_last_radio("file:///C:/captures/night.wav"));
    CHECK_FALSE(remembers_as_last_radio("FILE:///C:/captures/night.wav"));
    CHECK_FALSE(remembers_as_last_radio(""));
}

// The first run and the ordinary run on an engine this window started.
TEST_CASE("an engine of ours with no source gets the radio used last or the picker",
          "[engine-start]")
{
    LastSourceFacts facts;
    facts.engine_ours = true;
    facts.have_remembered = true;
    CHECK(plan_last_source(facts) == LastSourceAction::Reopen);

    facts.have_remembered = false;
    CHECK(plan_last_source(facts) == LastSourceAction::ShowPicker);
}

// Rejects the wider rule, any sourceless engine. An engine somebody else
// started may be sourceless on purpose, and a remembered URI names a device on
// this machine that is a different radio on another.
TEST_CASE("an engine somebody else started is never given a source", "[engine-start]")
{
    LastSourceFacts facts;
    facts.engine_ours = false;
    facts.have_remembered = true;
    CHECK(plan_last_source(facts) == LastSourceAction::Nothing);

    // Not the picker either: the person did not ask this window to set that
    // engine up, and opening a panel over it would read as though they had.
    facts.have_remembered = false;
    CHECK(plan_last_source(facts) == LastSourceAction::Nothing);
}

// Rejects replacing a source that is open, which the owner ruled out, and
// replacing a source the operator asked for on the command line, which is
// newer than anything remembered.
TEST_CASE("an open source and an open already asked for are left alone", "[engine-start]")
{
    LastSourceFacts facts;
    facts.engine_ours = true;
    facts.have_remembered = true;
    facts.source_open = true;
    CHECK(plan_last_source(facts) == LastSourceAction::Nothing);

    facts.source_open = false;
    facts.open_already_asked = true;
    CHECK(plan_last_source(facts) == LastSourceAction::Nothing);
}

// Rejects "no engine" as the pill's word for an engine the window is bringing
// up. Nothing is wrong yet, so it is a warning, and an exited engine is "no
// engine" again with its reason behind it.
TEST_CASE("an engine being started is a warning and not an absence", "[engine-start]")
{
    StatusInputs in;
    in.connected = false;
    in.engine_starting = true;
    auto summary = summarise_status(in);
    CHECK(summary.headline == "engine starting");
    CHECK(summary.level == StatusLevel::Warn);

    in.engine_starting = false;
    summary = summarise_status(in);
    CHECK(summary.headline == "no engine");
    CHECK(summary.level == StatusLevel::Bad);

    // Connected is connected, whatever the start flag says.
    in.connected = true;
    in.engine_running = true;
    in.source_open = true;
    in.engine_starting = true;
    summary = summarise_status(in);
    CHECK(summary.level == StatusLevel::Quiet);
}
