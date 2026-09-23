// What opening a recording will send, and whether it can be sent yet.
//
// WHY THIS IS ITS OWN HEADER AND HOLDS NO Qt
//
// The same reason ui/models/source_choice.h gives for the radios: the part of
// this section that can be wrong is none of the widget. It is which keys a
// file URI carries, when a centre the operator typed is refused because the
// recording states a different one, how a Windows path becomes a URI the
// registry reads back as the same path, and what a filename says about where
// a recording was tuned. Every one of those stays wrong quietly, so they are
// here with cases in ui/tests/test_recording_plan.cpp and the QML binds names.
//
// THE KEYS FOLLOW core/source/registry.cpp's file_config_of AND ITS RULE: a
// value stated in both the URI and the container has to agree, and the engine
// refuses the open naming both. So a container's own rate is never restated,
// its own centre is restated only when it has none, and a disagreement the
// operator typed is refused here with the same reasoning rather than sent and
// refused a round trip later.

#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "models/frequency_entry.h"
#include "models/recording_header.h"

namespace revenant::ui {

// ---------------------------------------------------------------------------
// The URI
// ---------------------------------------------------------------------------

// A filesystem path as the file backend's URI.
//
// THE SHAPE core/source/registry.cpp's split_uri UNDOES. "file:///C:/x" gives
// the body "/C:/x" and the registry drops the leading slash before a drive
// letter; "file://server/share/x" gives a UNC path back. Backslashes become
// forward slashes, which Windows opens either way and which keep a UNC
// prefix recognisable.
//
// ONLY WHAT THE GRAMMAR NEEDS IS ESCAPED. The registry percent-decodes the
// path and splits the query at the first '?', so a '%' or a '?' in a filename
// has to be escaped or the path is cut short or decoded into something else.
// '#' is escaped because every other reader of a URI stops at it. Spaces and
// non-ASCII bytes pass through as they are, which is what docs/recordings.md
// has always typed for "SDR Recordings", and the registry reads them back
// unchanged.
[[nodiscard]] inline std::string file_uri_for_path(std::string_view path)
{
    std::string slashed(path);
    for (char& c : slashed) {
        if (c == '\\') {
            c = '/';
        }
    }

    std::string escaped;
    escaped.reserve(slashed.size() + 8);
    for (const char c : slashed) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '%' || c == '?' || c == '#' || byte < 0x20 || byte == 0x7F) {
            escaped += std::format("%{:02X}", static_cast<unsigned>(byte));
        } else {
            escaped.push_back(c);
        }
    }

    if (escaped.starts_with("//")) {
        return "file:" + escaped;
    }
    if (escaped.starts_with("/")) {
        return "file://" + escaped;
    }
    return "file:///" + escaped;
}

// ---------------------------------------------------------------------------
// The plan
// ---------------------------------------------------------------------------

// What the operator filled in. Each box is its parse and whether anything was
// typed at all, because an empty box and a box of junk are different
// sentences: one asks for a value, the other says the value is not one.
struct RecordingChoice {
    std::optional<std::int64_t> center_hz;
    bool center_typed = false;

    std::optional<std::int64_t> rate;
    bool rate_typed = false;

    // A raw file's format. Unknown takes what the extension named.
    RecordingFormat format = RecordingFormat::Unknown;
};

struct RecordingPlan {
    bool ready = false;

    // What open sends. Composed only when ready, so a half-filled section
    // never shows a URI the engine would refuse.
    std::string uri;

    // Why the open is not offered, as one sentence. Empty when ready.
    std::string blocker;

    // What the plan settled on, for the preview and for the comparison after
    // the open. Zero where nothing is known yet.
    std::int64_t center_hz = 0;
    bool center_from_file = false;
    std::int64_t rate = 0;
    RecordingFormat format = RecordingFormat::Unknown;
    std::uint64_t length_samples = 0;
};

[[nodiscard]] inline RecordingPlan plan_recording_open(const RecordingHeader& header,
                                                       const RecordingChoice& choice)
{
    RecordingPlan plan;

    if (header.path.empty()) {
        plan.blocker = "choose a recording first";
        return plan;
    }
    if (!header.readable()) {
        plan.blocker = "the engine will not open this: " + header.refusal;
        return plan;
    }

    // THE FORMAT. A container's own, or a raw file's chosen or named one.
    plan.format = header.format;
    if (header.kind == RecordingKind::Raw && choice.format != RecordingFormat::Unknown) {
        plan.format = choice.format;
    }
    if (plan.format == RecordingFormat::Unknown) {
        plan.blocker = "a raw file's format is not in its name: choose cu8, cs8, cs16, cs24 or "
                       "cf32";
        return plan;
    }

    // THE RATE. A container's own is final, and restating a different one is
    // the disagreement the engine refuses.
    if (choice.rate_typed && !choice.rate.has_value()) {
        plan.blocker = "the rate box does not hold a number of samples per second";
        return plan;
    }
    if (header.has_rate) {
        plan.rate = header.rate;
        if (choice.rate.has_value() && *choice.rate != header.rate) {
            plan.blocker = std::format(
                "the recording states {} S/s and the engine refuses a rate that disagrees with it",
                header.rate);
            return plan;
        }
    } else if (choice.rate.has_value() && *choice.rate > 0) {
        plan.rate = *choice.rate;
    } else {
        plan.blocker = "nothing in this file says what rate it was recorded at: type it";
        return plan;
    }

    // THE LENGTH, and the two refusals the engine makes on it. Before the
    // centre, so a recording waiting only for its centre already shows how
    // long it is.
    const std::size_t bps = recording_bytes_per_sample(plan.format);
    if (header.data_bytes % bps != 0) {
        plan.blocker = std::format(
            "{} bytes is not a whole number of {} samples, so either the format is wrong or the "
            "file was cut short",
            header.data_bytes, recording_format_name(plan.format));
        return plan;
    }
    plan.length_samples = header.data_bytes / bps;
    if (plan.length_samples == 0) {
        plan.blocker = "the file holds no samples";
        return plan;
    }

    // THE CENTRE. Required when the file states none, which the owner asked for
    // on 2026-09-23 and which is the difference between a spectrum at the right
    // frequencies and one presented at 0 Hz with every absolute label wrong.
    if (choice.center_typed && !choice.center_hz.has_value()) {
        plan.blocker = "the centre box does not hold a frequency";
        return plan;
    }
    if (header.has_center) {
        plan.center_hz = header.center_hz;
        plan.center_from_file = true;
        if (choice.center_hz.has_value() && *choice.center_hz != header.center_hz) {
            plan.blocker = std::format(
                "the recording states a centre of {} MHz and the engine refuses one that "
                "disagrees with it",
                format_mhz(header.center_hz));
            return plan;
        }
    } else if (choice.center_hz.has_value() && *choice.center_hz > 0) {
        plan.center_hz = *choice.center_hz;
    } else {
        plan.blocker = "this recording states no centre frequency: type the one it was tuned to";
        return plan;
    }

    std::string uri = file_uri_for_path(header.path);
    char separator = '?';
    const auto append = [&uri, &separator](std::string_view key, const std::string& value) {
        uri.push_back(separator);
        separator = '&';
        uri.append(key);
        uri.push_back('=');
        uri.append(value);
    };
    if (header.kind == RecordingKind::Raw) {
        // All three, format included even when the extension named it. The
        // engine's own inference is case-sensitive where this one is not, so
        // stating it means a .CS16 opens as what the preview said.
        append("rate", std::to_string(plan.rate));
        append("format", recording_format_name(plan.format));
    }
    if (!header.has_center) {
        append("center", std::to_string(plan.center_hz));
    }

    plan.uri = std::move(uri);
    plan.ready = true;
    return plan;
}

// ---------------------------------------------------------------------------
// What a filename says, as a guess
// ---------------------------------------------------------------------------

// A centre read off a filename, and the reason in words. Offered and never
// applied: the box stays empty until the operator takes it, because a guess
// that fills itself in stops being read as one.
struct CenterGuess {
    std::int64_t hz = 0;
    std::string why;
};

namespace recording_detail {

[[nodiscard]] inline bool all_digits(std::string_view text)
{
    if (text.empty()) {
        return false;
    }
    bool point = false;
    for (const char c : text) {
        if (c == '.') {
            if (point) {
                return false;
            }
            point = true;
            continue;
        }
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return text.front() != '.' && text.back() != '.';
}

// The unit suffix on a token, "hz", "khz", "mhz" or "ghz" in any case, and
// the digits before it. Empty suffix when the token is not a number with one.
struct UnitToken {
    std::string number;
    std::string suffix;
};

[[nodiscard]] inline UnitToken unit_token(std::string_view token)
{
    std::string lower(token);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    for (const std::string_view suffix : {"khz", "mhz", "ghz", "hz"}) {
        if (lower.size() > suffix.size() && lower.ends_with(suffix)) {
            const std::string number = lower.substr(0, lower.size() - suffix.size());
            if (all_digits(number)) {
                return UnitToken{number, std::string(token.substr(token.size() - suffix.size()))};
            }
        }
    }
    return {};
}

}  // namespace recording_detail

// The centre a recording's name suggests.
//
// TWO SHAPES, WHICH ARE THE TWO THIS MACHINE'S RECORDINGS AND THE COMMON
// RECORDERS WRITE:
//
//   A band, "7000_7300kHz", as the KF4FIC wideband WAVs name theirs. The
//   guess is the midpoint, which is the assumption docs/recordings.md made
//   for its runs (7150000 and 14175000) and says in as many words is an
//   assumption: the name is the band being listened to and the file carries
//   96 kHz of it, so the true centre could be anywhere a 96 kHz span fits.
//
//   One frequency with a unit, "7100kHz" or "7100000Hz", as HDSDR and SDR#
//   name theirs, which usually is the tuning.
//
// A bare number is never read as a frequency. A date, a time and a sample
// rate are all bare numbers in the names recorders write, and a guess taken
// from the wrong one is a plausible frequency with nothing to say it is not.
[[nodiscard]] inline std::optional<CenterGuess> guess_center_from_name(std::string_view path)
{
    std::string name = recording_detail::base_name(path);

    // Only an extension this section opens comes off, so a name ending in
    // "14.074MHz" keeps its decimal point.
    const std::string extension = recording_extension(name);
    for (const std::string_view known : {"wav", "rf64", "bw64", "sigmf-meta", "sigmf-data", "cu8",
                                         "cs8", "cs16", "cs24", "cf32", "raw", "iq", "bin",
                                         "dat"}) {
        if (extension == known) {
            name.erase(name.size() - extension.size() - 1);
            break;
        }
    }

    std::vector<std::string> tokens;
    std::string current;
    for (const char c : name) {
        if (c == '_' || c == '-' || c == ' ' || c == '(' || c == ')' || c == ',' || c == '[' ||
            c == ']') {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }

    constexpr std::int64_t kCeilingHz = 100'000'000'000;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const recording_detail::UnitToken high = recording_detail::unit_token(tokens[i]);
        if (high.suffix.empty()) {
            continue;
        }
        const auto high_hz = parse_frequency(tokens[i]);
        if (!high_hz || high_hz->hertz <= 0 || high_hz->hertz > kCeilingHz) {
            continue;
        }

        if (i > 0 && recording_detail::all_digits(tokens[i - 1])) {
            const auto low_hz = parse_frequency(tokens[i - 1] + high.suffix);
            if (low_hz && low_hz->hertz > 0 && low_hz->hertz < high_hz->hertz) {
                const std::int64_t mid = low_hz->hertz + (high_hz->hertz - low_hz->hertz) / 2;
                return CenterGuess{
                    mid, std::format("the midpoint of {} to {} {} in the name, which is the band "
                                     "and not necessarily the tuning",
                                     tokens[i - 1], high.number, high.suffix)};
            }
        }
        return CenterGuess{high_hz->hertz,
                           std::format("{} {} in the name", high.number, high.suffix)};
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// --open-recording PATH[:CENTER]
// ---------------------------------------------------------------------------

struct RecordingArgument {
    std::string path;

    // As typed, parsed by the same frequency box rule as the section's field.
    // Empty when none was given.
    std::string center_text;
};

// Split at the LAST colon, and only when what follows it is not part of a
// path. "C:\x.wav" has a colon and no centre: a colon at index one is a drive
// letter's. "C:\x.wav:7150000" and "x.wav:7.15M" carry one. A suffix holding
// a slash is a path that happened to contain a colon, which Windows does not
// allow in a filename, so it is taken as a path and the engine says so.
[[nodiscard]] inline RecordingArgument split_recording_argument(std::string_view text)
{
    const std::size_t colon = text.find_last_of(':');
    if (colon == std::string_view::npos || colon <= 1) {
        return RecordingArgument{std::string(text), {}};
    }
    const std::string_view suffix = text.substr(colon + 1);
    if (suffix.find_first_of("/\\") != std::string_view::npos) {
        return RecordingArgument{std::string(text), {}};
    }
    return RecordingArgument{std::string(text.substr(0, colon)), std::string(suffix)};
}

// ---------------------------------------------------------------------------
// Whose disk the path is on
// ---------------------------------------------------------------------------

// Whether an engine address is this machine, so the section can say when the
// preview it shows was read from a disk the engine will not open. The
// loopback spellings core/rpc/server.h and tools/engined accept, and nothing
// cleverer: a LAN address that happens to be this machine's own is still
// shown the caveat, which costs a sentence, where the other mistake would
// cost the operator a preview of the wrong file presented as the right one.
[[nodiscard]] inline bool engine_is_local(std::string_view address)
{
    if (address.starts_with('[') && address.ends_with(']')) {
        address = address.substr(1, address.size() - 2);
    }
    return address == "localhost" || address == "::1" || address.starts_with("127.");
}

// ---------------------------------------------------------------------------
// The file dialog
// ---------------------------------------------------------------------------

// The dialog's filters, in the Qt name-filter form. The first is every type
// the engine opens, so the ordinary case needs no choice; the rest narrow it;
// the last lets a raw file with some other extension through, whose format
// the section then asks for.
[[nodiscard]] inline std::vector<std::string> recording_file_filters()
{
    return {
        "Recordings (*.wav *.rf64 *.bw64 *.sigmf-meta *.sigmf-data *.cu8 *.cs8 *.cs16 *.cs24 "
        "*.cf32)",
        "WAV, RF64 and BW64 (*.wav *.rf64 *.bw64)",
        "SigMF (*.sigmf-meta *.sigmf-data)",
        "Raw IQ (*.cu8 *.cs8 *.cs16 *.cs24 *.cf32)",
        "All files (*)",
    };
}

}  // namespace revenant::ui
