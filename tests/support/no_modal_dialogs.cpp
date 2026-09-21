// Test binaries must never open a window.
//
// This exists because one did. A test process that could not create a Vulkan
// context called abort(), and the Debug CRT turned that into a modal
// "Debug Error! abort() has been called" dialog on the developer's desktop.
// The process then sat there forever holding the dialog open.
//
// Both consequences are bad and the second is worse. On a desktop it is a
// popup someone has to dismiss. On the CI runner it is a job that hangs until
// its timeout, tens of minutes later, and reports nothing about why: the
// message the process wanted to print is sitting in a dialog nobody will ever
// see. A test suite that can block on user input is not automatable.
//
// Four separate mechanisms can put a dialog on screen from a CRT failure, and
// all four have to be turned off. Disabling one and assuming the rest follow is
// how this comes back.
//
// Linked into every test executable the root CMake project builds, which is
// all six: revenant_reference_tests, revenant_decode_tests,
// revenant_detect_tests, revenant_engine_tests, revenant_rpc_tests and
// revenant_tool_tests. Each one lists this file by path; there is no library
// to link and nothing that adds it automatically.
//
// revenant_ui_tests, the seventh, does not have it and is not going to get it
// from here. ui/ is a separate CMake project with its own CMakeLists.txt, and
// of the seven it is the one where this matters least: that target opens no
// Vulkan context and holds no abort() path of its own.
//
// WHAT THIS PARAGRAPH USED TO SAY: first "Linked into every test executable",
// which revenant_ui_tests made false the day ui/ was added; then, after that
// was corrected, "a seventh target gets it by somebody remembering", which
// left the correction as a note rather than a gate. Both are closed now.
// scripts/check_modal_dialogs.py reads the add_executable source list of
// every *_tests target under tests/ AND under ui/, and fails the build for
// one that does not list this file. CI's guards job runs it and so does
// ctest, as modal_dialogs. revenant_ui_tests is named in the script's exempt
// list with the reason above, so the one known gap is a decision written
// down in the place that checks rather than a claim nobody had checked. An
// eighth target added without the file arrives red, including one whose name
// sits on the line below add_executable(, which the inline awk this replaced
// dropped without saying so.
//
// Costs nothing on a non-Windows build.

#if defined(_WIN32)

#include <crtdbg.h>
#include <cstdio>
#include <cstdlib>
#include <windows.h>

namespace revenant::test {
namespace {

struct DialogSuppressor {
    DialogSuppressor() {
        // 1. abort() itself. Without this the Debug CRT shows the dialog that
        //    started all of this. _CALL_REPORTFAULT additionally stops Windows
        //    Error Reporting from opening its own.
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

        // 2. assert() and the CRT's own invalid-parameter and error reports.
        //    Routed to stderr, which a CI log captures, instead of to a window
        //    nobody is watching.
        //
        //    Debug CRT only. Without _DEBUG these are macros that expand to a
        //    discarded constant, so the loop body vanishes and the induction
        //    variable is left unreferenced, which /W4 reports and /WX turns
        //    into a failed release build. There is nothing to suppress in a
        //    release CRT anyway: it has no report dialogs of its own.
#ifdef _DEBUG
        const int reports[] = {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT};
        for (const int report : reports) {
            _CrtSetReportMode(report, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
        }
#endif

        // 3. The shell's own boxes for a hard fault or a missing device.
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                     SEM_NOOPENFILEERRORBOX);

        // 4. The just-in-time debugger prompt, which is a separate dialog from
        //    the abort one and appears on an unhandled exception.
        SetUnhandledExceptionFilter([](EXCEPTION_POINTERS*) -> LONG {
            std::fputs("\nunhandled exception, terminating\n", stderr);
            std::fflush(stderr);
            return EXCEPTION_EXECUTE_HANDLER;
        });
    }
};

// Namespace-scope, so it runs before main and therefore before any test body.
// Order relative to other translation units is unspecified and does not matter:
// nothing here depends on other global state, and everything that could raise a
// dialog happens inside a test.
const DialogSuppressor g_suppress_dialogs;

}  // namespace
}  // namespace revenant::test

#endif  // _WIN32
