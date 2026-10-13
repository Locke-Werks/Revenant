// engine_command_line, quote_argument, capture_problems, plan_capture_apply and
// capture_cost_sentence: the rolling capture the settings window turns into
// flags on the engine this window starts.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows. Nothing here starts a process; the launch itself is
// verified on a staged payload.

#include <string>

#include <catch2/catch_test_macros.hpp>

#include "models/capture_choice.h"

using revenant::ui::capture_cost_sentence;
using revenant::ui::capture_problems;
using revenant::ui::CaptureApply;
using revenant::ui::CaptureChoice;
using revenant::ui::engine_command_line;
using revenant::ui::plan_capture_apply;
using revenant::ui::quote_argument;

namespace {

const std::filesystem::path kExe = L"C:\\Program Files\\Revenant\\revenant-engine.exe";

CaptureChoice capture_on()
{
    CaptureChoice choice;
    choice.enabled = true;
    choice.folder = L"D:\\capture";
    choice.retain = "50G";
    return choice;
}

}  // namespace

TEST_CASE("an engine started with the capture off gets none of its flags")
{
    // Rejects an implementation that passes --capture whenever a folder is
    // set, which is always: the folder has a default.
    CaptureChoice off = capture_on();
    off.enabled = false;
    const std::wstring line = engine_command_line(kExe, 17690, {}, off);
    CHECK(line ==
          L"\"C:\\Program Files\\Revenant\\revenant-engine.exe\" --no-source --pace 1 --port 17690");
    CHECK(line.find(L"--capture") == std::wstring::npos);
}

TEST_CASE("an engine started with the capture on gets the folder and its limits")
{
    CaptureChoice on = capture_on();
    on.max_age = "7d";
    const std::wstring line = engine_command_line(kExe, 17690, {}, on);
    CHECK(line.ends_with(L" --port 17690 --capture D:\\capture --capture-retain 50G "
                         L"--capture-max-age 7d"));
}

TEST_CASE("an empty limit is left off the line rather than passed empty")
{
    // Rejects passing --capture-max-age "", which the engine refuses, so an
    // unbounded age would stop the engine starting at all.
    CaptureChoice on = capture_on();
    on.retain.clear();
    const std::wstring line = engine_command_line(kExe, 17690, {}, on);
    CHECK(line.ends_with(L"--capture D:\\capture"));
    CHECK(line.find(L"--capture-retain") == std::wstring::npos);
    CHECK(line.find(L"--capture-max-age") == std::wstring::npos);
}

TEST_CASE("a capture folder with a space in it is quoted for CreateProcess")
{
    // Rejects appending the folder bare: a user name with a space splits it
    // into two arguments and the engine captures to "C:\Users\Jo".
    CaptureChoice on = capture_on();
    on.folder = L"C:\\Users\\Jo Smith\\AppData\\Local\\Locke Werks\\Revenant\\capture";
    const std::wstring line = engine_command_line(kExe, 17690, L"C:\\tok en.txt", on);
    CHECK(line.find(L" --token-file \"C:\\tok en.txt\"") != std::wstring::npos);
    CHECK(line.find(L" --capture \"C:\\Users\\Jo Smith\\AppData\\Local\\Locke Werks\\Revenant"
                    L"\\capture\" ") != std::wstring::npos);
}

TEST_CASE("quote_argument escapes the backslashes CommandLineToArgvW would read as escapes")
{
    CHECK(quote_argument(L"plain") == L"plain");
    CHECK(quote_argument(L"") == L"\"\"");
    // A trailing backslash inside quotes would escape the closing quote and
    // swallow the next argument, so it is doubled.
    CHECK(quote_argument(L"D:\\my capture\\") == L"\"D:\\my capture\\\\\"");
    // Backslashes before an embedded quote are doubled and the quote escaped.
    CHECK(quote_argument(L"a\\\"b c") == L"\"a\\\\\\\"b c\"");
    // Backslashes elsewhere are literal and left alone.
    CHECK(quote_argument(L"C:\\a b\\c") == L"\"C:\\a b\\c\"");
}

TEST_CASE("sizes and ages are checked with the engine's own grammar")
{
    // Rejects a client-side grammar that drifts from core/capture/limits.h:
    // each of these is what the engine takes or refuses.
    CaptureChoice choice = capture_on();
    for (const char* good : {"50G", "256M", "1T", "512MiB", "1024", "2.5g"}) {
        choice.retain = good;
        CHECK(capture_problems(choice).retain.empty());
    }
    for (const char* bad : {"50 GB", "fifty", "-1G", "0", "G"}) {
        choice.retain = bad;
        CHECK_FALSE(capture_problems(choice).retain.empty());
    }
    choice.retain = "50G";
    for (const char* good : {"24h", "7d", "90m", "3600", "30s"}) {
        choice.max_age = good;
        CHECK(capture_problems(choice).max_age.empty());
    }
    for (const char* bad : {"0", "0h", "a week", "7w", "-1d"}) {
        choice.max_age = bad;
        CHECK_FALSE(capture_problems(choice).max_age.empty());
    }
    choice.max_age.clear();
    CHECK_FALSE(capture_problems(choice).any());
}

TEST_CASE("the capture folder must be a full path")
{
    // Rejects accepting "capture": the engine runs in its own directory, so
    // that would be a folder under Program Files.
    CaptureChoice choice = capture_on();
    choice.folder = L"capture";
    CHECK_FALSE(capture_problems(choice).folder.empty());
    choice.folder.clear();
    CHECK_FALSE(capture_problems(choice).folder.empty());
    choice.folder = L"\\\\nas\\share\\capture";
    CHECK(capture_problems(choice).folder.empty());
}

TEST_CASE("a choice with a problem adds no capture flags at all")
{
    // Rejects passing the flags that parse and dropping the one that does
    // not, which turns a bounded capture into an unbounded one.
    CaptureChoice choice = capture_on();
    choice.max_age = "a week";
    const std::wstring line = engine_command_line(kExe, 17690, {}, choice);
    CHECK(line.find(L"--capture") == std::wstring::npos);
}

TEST_CASE("applying restarts only an engine this window started")
{
    // Rejects restarting whatever engine is connected: one somebody else
    // started is theirs, and restarting it would end their capture.
    CHECK(plan_capture_apply(true, true, true) == CaptureApply::RestartEngine);
    CHECK(plan_capture_apply(false, true, true) == CaptureApply::LeaveExternal);
    CHECK(plan_capture_apply(true, false, true) == CaptureApply::Nothing);
    CHECK(plan_capture_apply(true, true, false) == CaptureApply::Nothing);
    CHECK(plan_capture_apply(false, true, false) == CaptureApply::Nothing);
}

TEST_CASE("the disk cost says how long the size kept lasts")
{
    CHECK(capture_cost_sentence("50G") ==
          "Writes about 17 GB an hour at 2.4 MS/s; 50G keeps about the last 3 h.");
    CHECK(capture_cost_sentence("4G") ==
          "Writes about 17 GB an hour at 2.4 MS/s; 4G keeps about the last 15 min.");
    CHECK(capture_cost_sentence("").find("until the disk is full") != std::string::npos);
    CHECK(capture_cost_sentence("lots") == "Writes about 17 GB an hour at 2.4 MS/s.");
}
