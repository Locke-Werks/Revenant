// The rolling capture as the settings window holds it, and the engine command
// line it turns into.
//
// WHY THE CLIENT HOLDS IT AT ALL. The capture is switched on by the engine's
// own flags, --capture <dir> and the three that shape it (docs/capture.md),
// and the window starts its engine with a command line of its own
// (core/launch/engine_launcher.cpp). Before this nothing put those flags on that
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
#include "core/launch/engine_command.h"

namespace revenant::ui {

// The command-line half moved to core/launch so webgate builds the same line.
using launch::CaptureChoice;
using launch::CaptureProblems;
using launch::capture_problems;
using launch::engine_command_line;
using launch::quote_argument;
using launch::widen_ascii;

// Off by default: at 2.4 MS/s of cu8 the capture writes 4.8 MB a second, about
// 17 GB an hour, and a window that started doing that on a first run would
// fill somebody's disk without their having asked.
inline constexpr bool kCaptureEnabledDefault = false;
inline constexpr std::string_view kCaptureRetainDefault = "50G";

// Bytes a second at the rate the disk-cost line quotes, 2.4 MS/s of two-byte
// complex samples.
inline constexpr double kCaptureBytesPerSecond = 2.4e6 * 2.0;

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
