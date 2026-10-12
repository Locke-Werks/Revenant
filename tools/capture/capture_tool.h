// revenant-capture: the rolling capture from a terminal.
//
// Everything the executable does lives behind run(), so a test drives the
// same code a person does and reads back the same text. main.cpp is argv in,
// lines to stdout, an exit code out, and nothing else.
//
// THE CLOCK IS AN ARGUMENT. "-10m" and "now" mean something different every
// time they are typed, so the instant they are measured from is handed in
// rather than read here, and a test pins it.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::capture_tool {

struct Environment {
    // Unix epoch nanoseconds that "now" and "-10m" are measured from.
    std::int64_t now_ns = 0;

    // One line of standard output, without the newline.
    std::function<void(std::string_view)> out;
};

// Runs one command line, program name excluded: {"list", "D:/capture"}.
// Output goes through env.out as it is produced, so a long index pass
// reports each segment as it finishes. An error is the message main prints.
[[nodiscard]] Status run(std::span<const std::string> args, const Environment& env);

// The usage text, for --help and for a command line with no command.
[[nodiscard]] std::string usage();

// The pieces, exposed for their tests.

// What parse_capture_instant accepts, plus "now" and a duration before now
// written with a leading minus, such as -10m or -1h30m.
[[nodiscard]] Expected<std::int64_t> parse_time(std::string_view text, std::string_view what,
                                                std::int64_t now_ns);

// 2s, 500ms, 10m, 1h30m, 1d. A unit is required: a bare number could be
// seconds or samples, and guessing either is a wrong answer that looks right.
[[nodiscard]] Expected<std::int64_t> parse_duration(std::string_view text,
                                                    std::string_view what);

// LOW:HIGH in revenant-cli's frequency grammar, or one frequency, which is a
// band of zero width and finds whatever covers it.
[[nodiscard]] Expected<std::pair<dsp::Hertz, dsp::Hertz>> parse_band(std::string_view text,
                                                                     std::string_view what);

// 2026-10-12T14:03:07.250Z, milliseconds, what parse_capture_instant reads.
[[nodiscard]] std::string format_instant(std::int64_t ns);

// 162.55M, 98.1M, 7.074M: always megahertz with trailing zeros trimmed, and
// exact, so the text pastes back into --vrx and means the same integer hertz.
[[nodiscard]] std::string format_frequency(dsp::Hertz hz);

// capture:///C:/root?from=...&to=... with the root percent-encoded.
[[nodiscard]] std::string capture_uri(std::string_view root, std::int64_t from_ns,
                                      std::int64_t to_ns);

}  // namespace revenant::capture_tool
