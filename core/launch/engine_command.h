// The command line for an engine a client starts for itself, and the capture
// choice that shapes it.
//
// Shared by the Qt client (ui/) and webgate, which both start the engine
// beside themselves under core/launch/engine_launcher. The capture flags are
// the engine's own, --capture <dir> and the three that shape it
// (docs/capture.md), in the text forms the engine accepts, checked here with
// the engine's own parser, core/capture/limits.h, so a value a client takes is
// a value the engine takes.
//
// This header holds no Qt and no Windows.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "core/capture/limits.h"

namespace revenant::launch {

struct CaptureChoice {
    bool enabled = false;
    std::wstring folder;
    // Empty means no limit, which is the engine's own default for both.
    std::string retain;
    std::string max_age;
};

struct CaptureProblems {
    std::string folder;
    std::string retain;
    std::string max_age;

    [[nodiscard]] bool any() const
    {
        return !folder.empty() || !retain.empty() || !max_age.empty();
    }
};

// What is wrong with each field, by the engine's rules. Checked whether or not
// the capture is on, so a field shows its problem while it is being typed.
//
// THE FOLDER MUST BE ABSOLUTE. The engine runs with its own directory as its
// working directory, so a relative folder would land under Program Files,
// which it cannot write and the operator would not look in.
[[nodiscard]] inline CaptureProblems capture_problems(const CaptureChoice& choice)
{
    CaptureProblems problems;
    if (choice.folder.empty()) {
        problems.folder = "name a folder for the capture";
    } else if (!std::filesystem::path(choice.folder).is_absolute()) {
        problems.folder = "the folder must be a full path, such as C:\\Revenant\\capture";
    }
    if (!choice.retain.empty()) {
        if (auto bytes = capture::parse_capture_size(choice.retain, "the size kept"); !bytes) {
            problems.retain = bytes.error().message;
        }
    }
    if (!choice.max_age.empty()) {
        if (auto seconds = capture::parse_capture_duration(choice.max_age, "the age kept");
            !seconds) {
            problems.max_age = seconds.error().message;
        }
    }
    return problems;
}

// One argument quoted the way CommandLineToArgvW and the CRT read it back:
// backslashes are literal except before a quote, where they escape in pairs.
// The install directory is under Program Files and the default capture folder
// can sit under a user name with a space in it, so this is the ordinary case
// and not a corner.
[[nodiscard]] inline std::wstring quote_argument(const std::wstring& argument)
{
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring out = L"\"";
    for (auto it = argument.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != argument.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == argument.end()) {
            // Doubled, so the closing quote below is not escaped by them.
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out.push_back(*it);
    }
    out.push_back(L'"');
    return out;
}

[[nodiscard]] inline std::wstring widen_ascii(std::string_view text)
{
    return {text.begin(), text.end()};
}

// The whole command line for the engine a client starts.
//
// --pace 1 because the engine's default, 0, runs a synthetic scene as fast as
// the GPU allows, which is right for a benchmark and wrong for a window
// somebody is watching. A live radio sets its own pace and a recording the
// client opens plays at 1 regardless.
//
// A CHOICE WITH A PROBLEM ADDS NO CAPTURE FLAGS AT ALL, rather than the ones
// that pass. The engine refuses to start on a flag it cannot read, so passing
// a bad one would trade a missing capture for a missing engine; and a capture
// with half its limits is one that fills a disk the operator thought was
// bounded. The settings window refuses to save such a choice in any case, so
// this only meets one written to the registry by hand.
[[nodiscard]] inline std::wstring engine_command_line(const std::filesystem::path& exe,
                                                      std::uint16_t port,
                                                      const std::filesystem::path& token_file,
                                                      const CaptureChoice& capture)
{
    std::wstring command = quote_argument(exe.wstring());
    command += L" --no-source --pace 1 --port ";
    command += std::to_wstring(port);
    if (!token_file.empty()) {
        command += L" --token-file ";
        command += quote_argument(token_file.wstring());
    }
    if (capture.enabled && !capture_problems(capture).any()) {
        command += L" --capture ";
        command += quote_argument(capture.folder);
        if (!capture.retain.empty()) {
            command += L" --capture-retain ";
            command += quote_argument(widen_ascii(capture.retain));
        }
        if (!capture.max_age.empty()) {
            command += L" --capture-max-age ";
            command += quote_argument(widen_ascii(capture.max_age));
        }
    }
    return command;
}

}  // namespace revenant::launch
