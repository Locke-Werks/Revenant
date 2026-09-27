// Whether this window starts an engine of its own, what it says about that
// engine when it goes, and when it opens the radio used last.
//
// WHY THE WINDOW STARTS ONE AT ALL. v0.1.0's Start Menu entry opens
// revenant-ui.exe, which looks for an engine on 127.0.0.1:17690, and nothing
// started one: a new user got a window saying it was waiting for something
// they had never heard of. The installer puts revenant-engine.exe beside
// revenant-ui.exe (installer.toml, scripts/stage-payload.ps1), so the window
// can start it itself. The owner's decisions of 2026-09-27 are the rules here:
//
//   An engine the window started stops with the window, crash included. That
//   half is models/engine_launcher.cpp's Job object and is not decided here.
//
//   An engine somebody started themselves is never touched. A headless
//   recorder on the default port is somebody's capture; a second engine
//   started beside it would fail to bind at best, and closing or replacing
//   its source would end the capture. So nothing here starts an engine while
//   anything answers on the port, and nothing reopens a radio on an engine
//   this window did not start.
//
//   The first source is the radio used last, and on a first run, or when that
//   radio is gone, nothing opens and the picker is shown.
//
// This header holds no Qt and no Windows; ui/tests links it.

#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace revenant::ui {

// Whether an engine this window starts would be reachable at `address`.
//
// NOT "IS THIS A LOOPBACK ADDRESS", which is the question the owner's rule
// sounds like and the wrong one to answer. The engine binds 127.0.0.1 unless
// told otherwise (core/rpc/server.h), and this window starts it without a
// bind address, so the only addresses that reach it are 127.0.0.1 itself and
// the name that resolves to it. 127.0.0.2 is loopback and reaches nothing
// bound on .1; ::1 is loopback and the engine does not bind IPv6. Starting an
// engine for either would give the person an engine that the window then
// spends the session failing to connect to, which is worse than the state
// this replaces because it looks like the engine is broken.
//
// Anything else is a remote engine or a name somebody chose on purpose, and
// starting a local engine does nothing for either: the owner's rule was
// loopback only, for that reason.
[[nodiscard]] constexpr bool is_launchable_address(std::string_view address)
{
    if (address == "127.0.0.1") {
        return true;
    }
    constexpr std::string_view kLocalhost = "localhost";
    if (address.size() != kLocalhost.size()) {
        return false;
    }
    for (std::size_t i = 0; i < address.size(); ++i) {
        const char c = address[i];
        const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        if (lower != kLocalhost[i]) {
            return false;
        }
    }
    return true;
}

// What main() knows at startup, gathered cheapest first. port_answers costs a
// TCP connect with a wait on it, so main() asks it only when the other two
// already say yes; left false otherwise, and the plan never reads it then.
struct EngineLaunchFacts {
    // is_launchable_address on the address argv or the remembered value named.
    bool launchable_address = false;

    // revenant-engine.exe is in QCoreApplication::applicationDirPath, which is
    // where the installer and the staged payload put it. A development build
    // of ui/ is its own CMake project with its own build tree and has no
    // engine beside it, which is what keeps this whole path out of a dev run
    // and out of CI's smoke run.
    bool engine_beside = false;

    // Something accepted a connection on 127.0.0.1 at the port. An engine
    // somebody started, or anything else holding the port; either way this
    // window does not start a second one, which would fail to bind.
    bool port_answers = false;
};

enum class EngineLaunch : std::uint8_t {
    // Start revenant-engine --no-source --pace 1 --port <port>.
    Start,

    // Each of these is "behave as before": the window waits for an engine and
    // says so. Distinct so a test can say which rule turned it away.
    NotLaunchableAddress,
    NoEngineBeside,
    PortAnswers,
};

// The order is the owner's rule read left to right, and it is also the order
// main() gathers the facts in, so a probe is never taken for an answer that
// would be thrown away.
[[nodiscard]] constexpr EngineLaunch plan_engine_launch(const EngineLaunchFacts& facts)
{
    if (!facts.launchable_address) {
        return EngineLaunch::NotLaunchableAddress;
    }
    if (!facts.engine_beside) {
        return EngineLaunch::NoEngineBeside;
    }
    if (facts.port_answers) {
        return EngineLaunch::PortAnswers;
    }
    return EngineLaunch::Start;
}

// The sentence the window shows for an engine it started that has exited.
//
// WHICH LINE OF STDERR. The engine's last line is not always its reason.
// tools/engined/main.cpp ends a failed serve() with "revenant-engine: <why>"
// and then, when the error carries one, "  code <n>", so "the last line" of a
// failed start is a bare number. The fatal paths there all begin with the
// program's name, so the last line that does is the reason; the prefix is
// dropped because the sentence already names the engine.
//
// A LINE THAT DOES NOT NAME THE ENGINE IS QUOTED AND NOT CALLED A REASON.
// Measured on the staged payload, 2026-09-27: an engine ended from outside had
// last written librtlsdr's "Found Rafael Micro R820T tuner", and the first
// version of this put that after a colon as though the tuner had killed it.
// Whatever a crash or a kill last wrote is still worth showing, so it goes in
// as what it is, the last thing the engine said.
//
// THE EXIT CODE IS SAID IN HEX FROM 0x80000000 UP, and called a crash only
// from 0xC0000000 to 0xCFFFFFFF. Those are NTSTATUS error values, an exception
// and not a return from main, and 3221225477 means nothing to anybody while
// 0xC0000005 is an access violation to anyone who has seen one.
// models/engine_launcher.cpp sets JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION
// so that is what a crash ends with, rather than a Windows Error Reporting
// dialog nobody can see because the engine has no window. Stop-Process and
// Task Manager end a process with 0xFFFFFFFF, which is not an exception, and
// the first version called it one.
[[nodiscard]] inline std::string engine_exit_reason_line(std::span<const std::string> tail)
{
    constexpr std::string_view kPrefix = "revenant-engine: ";
    for (auto it = tail.rbegin(); it != tail.rend(); ++it) {
        const std::string_view line = *it;
        if (line.starts_with(kPrefix)) {
            return std::string(line.substr(kPrefix.size()));
        }
    }
    return {};
}

// The last non-blank line, trimmed of the indent the engine gives continuation
// lines. What the sentence quotes when no line names the engine.
[[nodiscard]] inline std::string engine_last_line(std::span<const std::string> tail)
{
    for (auto it = tail.rbegin(); it != tail.rend(); ++it) {
        std::string_view line = *it;
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())) != 0) {
            line.remove_prefix(1);
        }
        if (!line.empty()) {
            return std::string(line);
        }
    }
    return {};
}

[[nodiscard]] inline std::string engine_exit_sentence(std::uint32_t exit_code,
                                                      std::span<const std::string> tail)
{
    char code[48] = {};
    if (exit_code >= 0xC0000000U && exit_code < 0xD0000000U) {
        std::snprintf(code, sizeof code, "crashed with exception 0x%08X",
                      static_cast<unsigned>(exit_code));
    } else if (exit_code >= 0x80000000U) {
        std::snprintf(code, sizeof code, "exited with code 0x%08X",
                      static_cast<unsigned>(exit_code));
    } else {
        std::snprintf(code, sizeof code, "exited with code %u", static_cast<unsigned>(exit_code));
    }

    std::string out = "the engine this window started ";
    out += code;
    if (const std::string reason = engine_exit_reason_line(tail); !reason.empty()) {
        out += ": ";
        out += reason;
        return out;
    }
    if (const std::string last = engine_last_line(tail); !last.empty()) {
        out += "; the last thing it wrote was \"";
        out += last;
        out += "\"";
        return out;
    }
    out += ", and said nothing on stderr";
    return out;
}

// Whether a URI is remembered as the radio used last.
//
// A RECORDING IS NOT, deliberately. It goes through the same openSource as a
// radio, which is why the question arises, but the owner's rule was the last
// RADIO: a window that reopened last night's recording would start playing a
// file at launch nobody asked to hear, and the recording section keeps its own
// recent list (models/recent_recordings.h) for the file somebody does want
// back. So opening a recording leaves the remembered radio where it was, and
// the next launch comes back on the radio used before it.
//
// The scheme is compared the way core/source/registry.cpp splits it, up to the
// first colon and without regard to case.
[[nodiscard]] constexpr bool remembers_as_last_radio(std::string_view uri)
{
    if (uri.empty()) {
        return false;
    }
    constexpr std::string_view kFile = "file:";
    if (uri.size() < kFile.size()) {
        return true;
    }
    for (std::size_t i = 0; i < kFile.size(); ++i) {
        const char c = uri[i];
        const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        if (lower != kFile[i]) {
            return true;
        }
    }
    return false;
}

// What the window does about a source on its first connection to an engine.
struct LastSourceFacts {
    // This window started the engine it has just connected to, and that
    // process has not exited. See EngineLink::setEngineStarted for why a
    // sourceless engine somebody else started is left alone.
    bool engine_ours = false;

    // The engine already has a source open. Never replaced: the owner's rule,
    // and also what an engine started with a URI rather than --no-source
    // looks like, or one a second window raced this one to.
    bool source_open = false;

    // This window has already asked for a source before the connection
    // landed, which --open-recording does. That request is the operator's and
    // is newer than anything remembered.
    bool open_already_asked = false;

    // A radio is remembered from an earlier session.
    bool have_remembered = false;
};

enum class LastSourceAction : std::uint8_t {
    Nothing,
    Reopen,
    ShowPicker,
};

[[nodiscard]] constexpr LastSourceAction plan_last_source(const LastSourceFacts& facts)
{
    if (!facts.engine_ours || facts.source_open || facts.open_already_asked) {
        return LastSourceAction::Nothing;
    }
    return facts.have_remembered ? LastSourceAction::Reopen : LastSourceAction::ShowPicker;
}

}  // namespace revenant::ui
