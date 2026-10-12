// The text grammar for the rolling capture's limits, shared by revenant-cli
// and revenant-engine so --capture-segment 256M means the same thing typed
// into either.
//
// SIZES ARE BINARY. 256M is 256 MiB, because WriterConfig's default is
// written that way and a segment of "256M" should be the default segment.
// K, M, G and T, either case, with an optional trailing B or iB; a bare
// number is bytes.
//
// DURATIONS take s, m, h or d; a bare number is seconds. --capture-segment
// takes either a size or a duration, and there only a trailing s makes it a
// duration, because 256M would otherwise be 256 minutes.

#pragma once

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <variant>

#include "core/error.h"

namespace revenant::capture {

namespace limits_detail {

[[nodiscard]] inline bool parse_number(std::string_view text, double& value)
{
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end && std::isfinite(value) && value >= 0.0;
}

[[nodiscard]] inline char lower(char c)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

}  // namespace limits_detail

[[nodiscard]] inline Expected<std::uint64_t> parse_capture_size(std::string_view text,
                                                                 std::string_view what)
{
    std::string_view number = text;
    if (number.size() >= 2 && limits_detail::lower(number.back()) == 'b') {
        number.remove_suffix(1);
        if (!number.empty() && limits_detail::lower(number.back()) == 'i') {
            number.remove_suffix(1);
        }
    }
    double scale = 1.0;
    if (!number.empty()) {
        switch (limits_detail::lower(number.back())) {
        case 'k': scale = 1024.0; break;
        case 'm': scale = 1024.0 * 1024.0; break;
        case 'g': scale = 1024.0 * 1024.0 * 1024.0; break;
        case 't': scale = 1024.0 * 1024.0 * 1024.0 * 1024.0; break;
        default: break;
        }
        if (scale != 1.0) {
            number.remove_suffix(1);
        }
    }
    double value = 0.0;
    if (!limits_detail::parse_number(number, value)) {
        return fail(std::format("{} '{}' is not a size. Give bytes with an optional K, M, G or T, "
                                "such as 256M or 50G",
                                what, text));
    }
    const double bytes = std::floor(value * scale);
    if (bytes < 1.0 || bytes > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return fail(std::format("{} '{}' is out of range: it must be at least one byte", what,
                                text));
    }
    return static_cast<std::uint64_t>(bytes);
}

// Seconds.
[[nodiscard]] inline Expected<double> parse_capture_duration(std::string_view text,
                                                             std::string_view what)
{
    std::string_view number = text;
    double scale = 1.0;
    if (!number.empty()) {
        switch (limits_detail::lower(number.back())) {
        case 's': scale = 1.0; number.remove_suffix(1); break;
        case 'm': scale = 60.0; number.remove_suffix(1); break;
        case 'h': scale = 3600.0; number.remove_suffix(1); break;
        case 'd': scale = 86400.0; number.remove_suffix(1); break;
        default: break;
        }
    }
    double value = 0.0;
    if (!limits_detail::parse_number(number, value) || value <= 0.0) {
        return fail(std::format("{} '{}' is not a positive duration. Give a number with s, m, h "
                                "or d, such as 60s or 24h",
                                what, text));
    }
    return value * scale;
}

// --capture-segment: a size, or with a trailing s a duration in seconds.
struct SegmentLimit {
    std::uint64_t bytes = 0;
    double seconds = 0.0;
};

[[nodiscard]] inline Expected<SegmentLimit> parse_segment_limit(std::string_view text,
                                                                std::string_view what)
{
    if (!text.empty() && limits_detail::lower(text.back()) == 's') {
        auto seconds = parse_capture_duration(text, what);
        if (!seconds) {
            return std::unexpected(seconds.error());
        }
        return SegmentLimit{.bytes = 0, .seconds = *seconds};
    }
    auto bytes = parse_capture_size(text, what);
    if (!bytes) {
        return fail(std::format("{} '{}' is neither a size such as 256M nor a duration ending in "
                                "s such as 60s",
                                what, text));
    }
    return SegmentLimit{.bytes = *bytes, .seconds = 0.0};
}

}  // namespace revenant::capture
