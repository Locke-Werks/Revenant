// What an operator types into a frequency box, turned into hertz.
//
// WHY THIS IS ITS OWN HEADER AND HOLDS NO Qt
//
// ui/tests links it. The part of a frequency box that can be WRONG is none
// of the widget: it is the unit a bare number means, where the rounding
// lands on a fraction the multiplier cannot carry exactly, and which inputs
// are refused rather than silently turned into a frequency nobody asked
// for. That is integer arithmetic over a short string, which is exactly the
// shape of thing that stays wrong until somebody tunes 14 kHz by accident.
//
// So it lives here, ui/tests asserts it, and the QML does the box.
//
// NO FLOATING POINT ANYWHERE ON THIS PATH, WHICH IS THE POINT OF THE SHAPE
//
// The obvious implementation is strtod and a multiply. 95.1 parses to a
// double that is not 95.1, times 1e6 is 95099999.99999999, and the nearest
// integer is right only because the error happened to be small. It stops
// being small at nine significant digits, which "1234.567891 MHz" already
// is, and a tuner that is one hertz out on a narrow filter is a tuner that
// is audibly out. So the digits are accumulated into a 64-bit mantissa with
// a decimal exponent and scaled by a power of ten at the end, and the only
// rounding in the whole path is the deliberate half-away-from-zero on a
// fraction finer than one hertz.
//
// THE RULE FOR A BARE NUMBER, WHICH IS A GUESS AND IS SHAPED SO ITS WRONG
// ANSWERS ARE LOUD
//
// "95.1" is 95.1 MHz to every operator alive and 95 hertz to a parser that
// takes the unit literally, and there is no reading of the text that settles
// it. So: a number with no suffix is MEGAHERTZ below 1 000 000 and HERTZ at
// or above it.
//
// That covers the four the operator actually typed: 95.1, 98.1 and 462.5625
// are megahertz, 95100000 is hertz. What it gets wrong is a band-plan number
// written bare in kilohertz, "14074" for 14.074 MHz, which this reads as
// 14074 MHz. That is deliberate rather than tolerated: 14 GHz is outside
// every source this engine drives, so the tune is REFUSED and the refusal
// names the range, where reading it as 14 kHz would have been accepted in
// silence on an HF source and left the operator hunting a dead band. A guess
// that has to be wrong is shaped to be wrong somewhere the radio will say
// so.
//
// A suffix always wins over the rule, and ParsedFrequency::unit_inferred
// says the rule was used, so a box can echo which reading it took before
// anything is sent.

#pragma once

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace revenant::ui {

enum class FrequencyUnit : std::uint8_t { Hertz, Kilohertz, Megahertz, Gigahertz };

struct ParsedFrequency {
    std::int64_t hertz = 0;

    // The unit the text was read in, whether it was typed or inferred.
    FrequencyUnit unit = FrequencyUnit::Hertz;

    // No suffix was typed and the magnitude rule above chose the unit. A box
    // echoes the resolved frequency when this is set, because the operator
    // has not stated what they meant and is owed the reading before the
    // radio moves.
    bool unit_inferred = false;
};

// The largest frequency this will produce. Not a statement about any source:
// it is the bound that keeps the scaling below inside a signed 64-bit
// integer with room to spare, and it is four orders of magnitude above
// anything an SDR front end tunes to. A source's own range is what actually
// refuses a tune, and it is asked separately.
inline constexpr std::int64_t kMaxEntryHertz = 1'000'000'000'000;  // 1 THz

// The boundary in the bare-number rule, in the unit the text is read in.
// Below this a bare number is megahertz; at or above it, hertz.
inline constexpr std::int64_t kBareHertzFloor = 1'000'000;

namespace detail {

[[nodiscard]] constexpr int unit_decade(FrequencyUnit unit)
{
    // No default case; see cmake/CompilerFlags.cmake.
    switch (unit) {
        case FrequencyUnit::Hertz:
            return 0;
        case FrequencyUnit::Kilohertz:
            return 3;
        case FrequencyUnit::Megahertz:
            return 6;
        case FrequencyUnit::Gigahertz:
            return 9;
    }
    return 0;
}

[[nodiscard]] constexpr char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// A power of ten as an integer, refusing anything that would not fit. Used
// for both the scale up and the divisor on the scale down.
[[nodiscard]] constexpr std::optional<std::int64_t> pow10(int exponent)
{
    if (exponent < 0 || exponent > 18) {
        return std::nullopt;
    }
    std::int64_t value = 1;
    for (int i = 0; i < exponent; ++i) {
        value *= 10;
    }
    return value;
}

}  // namespace detail

// Hertz from what was typed, or nothing when the text is not a frequency.
//
// ACCEPTED: leading and trailing whitespace; group separators inside the
// digits, which are comma, underscore and space, so "95,100,000" and
// "95 100 000" both work; an optional decimal point with any number of
// digits after it; an optional unit suffix, case insensitive, with or
// without a space before it and with or without a trailing "hz", so "M",
// "MHz", "mhz", " MHZ" are one thing.
//
// REFUSED, and every one of these is refused rather than coerced: an empty
// or whitespace-only string; a sign, because a centre frequency is not
// negative and "-95.1" is a typo rather than a request; two decimal points;
// a suffix with no digits; any character that is not a digit, a separator,
// a point or part of a recognised suffix; a result past kMaxEntryHertz; and
// a mantissa long enough to overflow the scaling, which is about nineteen
// significant digits and is not a frequency.
//
// Zero is accepted as a value and is a legal parse. Whether a source will
// tune to DC is the source's answer, not this function's.
[[nodiscard]] inline std::optional<ParsedFrequency> parse_frequency(std::string_view text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    if (begin == end) {
        return std::nullopt;
    }

    std::string_view body = text.substr(begin, end - begin);

    // The suffix, taken off the back first, so what is left is pure number
    // and the separator handling below never has to know about letters.
    //
    // Longest match first: "hz" alone is hertz, and "khz" must not be read
    // as "hz" with a stray k in front of it.
    std::optional<FrequencyUnit> stated;
    struct Suffix {
        std::string_view text;
        FrequencyUnit unit;
    };
    static constexpr Suffix kSuffixes[] = {
        {"ghz", FrequencyUnit::Gigahertz}, {"mhz", FrequencyUnit::Megahertz},
        {"khz", FrequencyUnit::Kilohertz}, {"hz", FrequencyUnit::Hertz},
        {"g", FrequencyUnit::Gigahertz},   {"m", FrequencyUnit::Megahertz},
        {"k", FrequencyUnit::Kilohertz},
    };
    for (const Suffix& candidate : kSuffixes) {
        if (body.size() <= candidate.text.size()) {
            continue;
        }
        const std::size_t at = body.size() - candidate.text.size();
        bool matched = true;
        for (std::size_t i = 0; i < candidate.text.size(); ++i) {
            if (detail::lower(body[at + i]) != candidate.text[i]) {
                matched = false;
                break;
            }
        }
        if (matched) {
            stated = candidate.unit;
            body = body.substr(0, at);
            break;
        }
    }

    // Whatever space sat between the number and the suffix.
    while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back())) != 0) {
        body.remove_suffix(1);
    }
    if (body.empty()) {
        return std::nullopt;
    }

    // The digits, into a mantissa and a count of how many of them were after
    // the point. Separators are skipped wherever they appear, including
    // inside the fraction, because a person grouping "95.100 000" is doing
    // the same thing they did on the other side of the point.
    std::uint64_t mantissa = 0;
    int fraction_digits = 0;
    bool seen_point = false;
    bool seen_digit = false;
    for (const char c : body) {
        if (c == ',' || c == '_' || std::isspace(static_cast<unsigned char>(c)) != 0) {
            continue;
        }
        if (c == '.') {
            if (seen_point) {
                return std::nullopt;
            }
            seen_point = true;
            continue;
        }
        if (c < '0' || c > '9') {
            return std::nullopt;
        }

        // Nineteen digits is where a uint64 stops holding the next one, and
        // nothing that long is a frequency. Refused rather than wrapped.
        if (mantissa > (UINT64_MAX - 9) / 10) {
            return std::nullopt;
        }
        mantissa = mantissa * 10 + static_cast<std::uint64_t>(c - '0');
        seen_digit = true;
        if (seen_point) {
            ++fraction_digits;
        }
    }
    if (!seen_digit) {
        return std::nullopt;
    }

    // The unit, stated or inferred. The inference needs the value in the
    // unit the text is written in, which is the mantissa scaled down by the
    // fraction alone, so it is computed before any multiplier is applied.
    ParsedFrequency out;
    if (stated.has_value()) {
        out.unit = *stated;
    } else {
        const auto divisor = detail::pow10(fraction_digits);
        if (!divisor.has_value()) {
            return std::nullopt;
        }
        const std::uint64_t written = mantissa / static_cast<std::uint64_t>(*divisor);
        out.unit = written < static_cast<std::uint64_t>(kBareHertzFloor)
                       ? FrequencyUnit::Megahertz
                       : FrequencyUnit::Hertz;
        out.unit_inferred = true;
    }

    const int net = detail::unit_decade(out.unit) - fraction_digits;
    if (net >= 0) {
        const auto scale = detail::pow10(net);
        if (!scale.has_value()) {
            return std::nullopt;
        }
        const auto limit = static_cast<std::uint64_t>(kMaxEntryHertz);
        if (mantissa != 0 && mantissa > limit / static_cast<std::uint64_t>(*scale)) {
            return std::nullopt;
        }
        out.hertz = static_cast<std::int64_t>(mantissa * static_cast<std::uint64_t>(*scale));
    } else {
        // Finer than a hertz, which "95.1234567 MHz" already is. Rounded
        // half away from zero, deliberately and in one place, so the box
        // can echo the hertz it actually took.
        const auto divisor = detail::pow10(-net);
        if (!divisor.has_value()) {
            return std::nullopt;
        }
        const auto d = static_cast<std::uint64_t>(*divisor);
        const std::uint64_t rounded = (mantissa + d / 2) / d;
        if (rounded > static_cast<std::uint64_t>(kMaxEntryHertz)) {
            return std::nullopt;
        }
        out.hertz = static_cast<std::int64_t>(rounded);
    }

    return out;
}

// Hertz back to the shortest text that reads as that frequency, for a box
// seeded from what the radio is currently on and for the "granted" echo.
//
// Always megahertz with six decimals and the trailing zeros kept, because a
// frequency readout that changes width as the digits change is a readout an
// eye cannot track down a column. Six decimals is one hertz at VHF, which is
// the resolution the tuner has.
[[nodiscard]] inline std::string format_mhz(std::int64_t hertz)
{
    const bool negative = hertz < 0;
    const auto magnitude = static_cast<std::uint64_t>(negative ? -hertz : hertz);
    const std::uint64_t whole = magnitude / 1'000'000;
    const std::uint64_t fraction = magnitude % 1'000'000;

    std::string out = negative ? "-" : "";
    out += std::to_string(whole);
    out += '.';
    std::string digits = std::to_string(fraction);
    out.append(6 - digits.size(), '0');
    out += digits;
    return out;
}

}  // namespace revenant::ui
