// The rolling capture as the settings window holds it, and the engine command
// line it turns into.
//
// WHY THE CLIENT HOLDS IT AT ALL. The capture is switched on by the engine's
// own flags, --capture <dir> and the three that shape it (docs/capture.md),
// and the window starts its engine with a command line of its own
// (models/engine_launcher.cpp). Before this nothing put those flags on that
// line, so an installed copy could never capture and the capture panel never
// appeared. The settings are the flags in the text forms the engine accepts,
// checked here with the engine's own parser, core/capture/limits.h, so a
// value the window takes is a value the engine takes.
//
// This header holds no Qt and no Windows; ui/tests links it.

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "core/capture/limits.h"

namespace revenant::ui {

struct CaptureChoice {
    bool enabled = false;
    std::wstring folder;
    // Empty means no limit, which is the engine's own default for both.
    std::string retain;
    std::string max_age;
};

// Off by default: at 2.4 MS/s of cu8 the capture writes 4.8 MB a second, about
// 17 GB an hour, and a window that started doing that on a first run would
// fill somebody's disk without their having asked.
inline constexpr bool kCaptureEnabledDefault = false;
inline constexpr std::string_view kCaptureRetainDefault = "50G";

// Bytes a second at the rate the disk-cost line quotes, 2.4 MS/s of two-byte
// complex samples.
inline constexpr double kCaptureBytesPerSecond = 2.4e6 * 2.0;

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

// The whole command line for the engine this window starts.
//
// --pace 1 because the engine's default, 0, runs a synthetic scene as fast as
// the GPU allows, which is right for a benchmark and wrong for a window
// somebody is watching. A live radio sets its own pace and a recording the
// window opens plays at 1 regardless.
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

// What applying a changed capture does.
enum class CaptureApply : std::uint8_t {
    // This window started the engine, so it starts it again with the new
    // flags. The flags are read once, at the engine's start.
    RestartEngine,

    // Somebody else started the engine, and its flags are theirs. The owner's
    // rule from models/engine_start.h: an engine somebody started themselves
    // is never touched, and restarting it would end whatever it was doing.
    LeaveExternal,

    // Nothing changed, or the choice has a problem and is not saved.
    Nothing,
};

[[nodiscard]] constexpr CaptureApply plan_capture_apply(bool engine_ours, bool changed,
                                                        bool valid)
{
    if (!changed || !valid) {
        return CaptureApply::Nothing;
    }
    return engine_ours ? CaptureApply::RestartEngine : CaptureApply::LeaveExternal;
}

// The disk-cost line under the switch. A retain size, when it reads, is turned
// into how many hours it holds at that rate, since an hour count is what the
// operator can weigh against how far back they want to look.
[[nodiscard]] inline std::string capture_cost_sentence(std::string_view retain)
{
    std::string out = "Writes about 17 GB an hour at 2.4 MS/s";
    std::optional<std::uint64_t> bytes;
    if (!retain.empty()) {
        if (auto parsed = capture::parse_capture_size(retain, "retain"); parsed) {
            bytes = *parsed;
        }
    }
    if (retain.empty()) {
        out += ", and with no size limit keeps everything until the disk is full.";
        return out;
    }
    if (!bytes) {
        out += ".";
        return out;
    }
    const double hours = static_cast<double>(*bytes) / (kCaptureBytesPerSecond * 3600.0);
    char tail[96] = {};
    if (hours >= 1.0) {
        std::snprintf(tail, sizeof tail, "; %s keeps about the last %.0f h.",
                      std::string(retain).c_str(), hours);
    } else {
        std::snprintf(tail, sizeof tail, "; %s keeps about the last %.0f min.",
                      std::string(retain).c_str(), hours * 60.0);
    }
    out += tail;
    return out;
}

}  // namespace revenant::ui
