// JSON, enough of it for a SigMF sidecar.
//
// ECMA-404 and RFC 8259. Written here rather than pulled in because the
// alternative is a vcpkg dependency and a licence row for a file format whose
// whole grammar is six productions, and because the error messages a sidecar
// needs are about SigMF keys rather than about tokens.
//
// It began inside file_source.cpp, which was its only reader. The rolling
// capture's catalog reads the same sidecars, and the revenant: keys it needs
// are ones the file source has no reason to know about, so the parser moved
// into a unit of its own rather than being copied: two copies of a parser
// drift, and the drift shows up as a meta one of them reads and the other
// refuses.
//
// Integers are kept as integers alongside the double. core:sample_start is a
// sample index, and a 2 MS/s recording passes 2^53 after about a century,
// which is far away, but the same field also carries the byte counts a
// malformed sidecar can put in it and a silently rounded one of those is a
// seek to the wrong place.
//
// Unsigned integers are kept separately as well, because the capture format's
// revenant:stream_epoch is a random u64 and half of those are above the signed
// ceiling. Read through the signed field they would arrive as a double rounded
// to 53 bits, which is a different epoch, and two segments of one stream would
// stop matching each other with nothing to say why.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/error.h"

namespace revenant::source {

struct JsonValue {
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;

    // The literal fitted a signed 64-bit integer exactly.
    bool integral = false;
    std::int64_t integer = 0;

    // The literal fitted an unsigned 64-bit integer exactly. True for every
    // non-negative integral value as well, so a reader of a u64 key need look
    // at nothing else.
    bool unsigned_integral = false;
    std::uint64_t uinteger = 0;

    std::string text;
    std::vector<JsonValue> elements;
    std::vector<std::pair<std::string, JsonValue>> members;

    [[nodiscard]] const JsonValue* member(std::string_view key) const
    {
        for (const auto& entry : members) {
            if (entry.first == key) {
                return &entry.second;
            }
        }
        return nullptr;
    }
};

// One whole document. Trailing text after the top-level value is an error,
// because a sidecar with two objects in it was written by something that
// appended rather than replaced.
[[nodiscard]] Expected<JsonValue> parse_json(std::string_view text);

}  // namespace revenant::source
