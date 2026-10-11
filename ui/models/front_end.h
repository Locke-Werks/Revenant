// The RTL-SDR's front-end switches the radio panel offers: the bias tee, the
// tuner's IF bandwidth, the RTL2832U's digital AGC and offset tuning, plus an
// operator's name for each dongle.
//
// EVERY ONE OF THEM IS A URI KEY AND A REOPEN. bias=, bw=, agc= and offset=
// are written at open by core/source/rtlsdr_source.cpp, and none of them has
// a live call on the wire, so changing one reopens the source with that key
// changed, the way direct sampling does. The current state is read back off
// the URI the engine reports, so the panel shows what is in force and not
// what was last clicked.
//
// REMEMBERED PER RADIO, EXCEPT THE BIAS TEE. Bandwidth, digital AGC and offset
// tuning go under source/radios/<radio>/ beside directSampling and come back
// when the radio is picked. The bias tee does not: it puts 4.5 V on the
// antenna port, the antenna on the end of the coax changes between sessions,
// and core/source/rtlsdr_source.h argues that it has to be asked for every
// time. Remembering it would undo that.
//
// This header holds no Qt; ui/tests links it.

#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "models/source_choice.h"

namespace revenant::ui {

// The value of `key` in `uri`'s query, or nothing when the key is absent.
[[nodiscard]] inline std::optional<std::string> query_value(std::string_view uri,
                                                            std::string_view key)
{
    const std::size_t mark = uri.find('?');
    if (mark == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view query = uri.substr(mark + 1);
    while (!query.empty()) {
        const std::size_t amp = query.find('&');
        const std::string_view part = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
        const std::size_t eq = part.find('=');
        if (part.substr(0, eq) == key) {
            return std::string(eq == std::string_view::npos ? std::string_view{}
                                                            : part.substr(eq + 1));
        }
    }
    return std::nullopt;
}

// The spellings the engine's Query::boolean takes as true.
[[nodiscard]] inline bool query_flag(std::string_view uri, std::string_view key)
{
    const auto value = query_value(uri, key);
    if (!value) {
        return false;
    }
    return *value == "1" || *value == "true" || *value == "on" || *value == "yes";
}

[[nodiscard]] inline std::uint32_t query_hertz(std::string_view uri, std::string_view key)
{
    const auto value = query_value(uri, key);
    std::uint32_t out = 0;
    if (value && !value->empty()) {
        std::from_chars(value->data(), value->data() + value->size(), out);
    }
    return out;
}

struct FrontEnd {
    bool bias_tee = false;
    bool digital_agc = false;
    bool offset_tuning = false;
    std::uint32_t bandwidth_hz = 0;  // zero: librtlsdr follows the rate
};

[[nodiscard]] inline FrontEnd front_end_of(std::string_view uri)
{
    FrontEnd out;
    out.bias_tee = query_flag(uri, "bias");
    out.digital_agc = query_flag(uri, "agc");
    out.offset_tuning = query_flag(uri, "offset");
    out.bandwidth_hz = query_hertz(uri, "bw");
    return out;
}

// `uri` with every front-end key set to `wanted`, a key left off where the
// value is the engine's default so an untouched radio keeps the URI it always
// had. offset= is written only where the tuner takes it: the R820T refuses the
// call and the open would fail.
[[nodiscard]] inline std::string with_front_end(std::string_view uri, const FrontEnd& wanted,
                                                bool offset_available)
{
    const auto flag = [](bool on) {
        return on ? std::optional<std::string_view>("1") : std::nullopt;
    };
    std::string out = with_query_value(uri, "bias", flag(wanted.bias_tee));
    out = with_query_value(out, "agc", flag(wanted.digital_agc));
    out = with_query_value(out, "offset", flag(wanted.offset_tuning && offset_available));
    const std::string bw = std::to_string(wanted.bandwidth_hz);
    out = with_query_value(out, "bw",
                           wanted.bandwidth_hz == 0 ? std::nullopt
                                                    : std::optional<std::string_view>(bw));
    return out;
}

// The bandwidths offered, in hertz, zero first for automatic. The R820T2's
// IF filter has a fixed set of widths and librtlsdr picks the nearest one at
// or above the request, so these are the ones that land somewhere distinct
// at the rates a dongle is run at.
inline constexpr std::array<std::uint32_t, 8> kTunerBandwidths{
    0, 300'000, 600'000, 1'000'000, 1'500'000, 2'000'000, 2'400'000, 3'000'000};

// Where each remembered switch is kept for one radio, beside directSampling.
[[nodiscard]] inline std::string front_end_key(std::string_view radio_id, std::string_view field)
{
    std::string key(kSourceGroup);
    key += "/radios/";
    key += radio_id;
    key += '/';
    key += field;
    return key;
}

// An operator's name for a dongle, kept by serial so it follows the dongle
// between USB ports. Empty serial, empty key: a dongle nobody could read the
// serial of cannot be told from the next one.
[[nodiscard]] inline std::string radio_label_key(std::string_view serial)
{
    if (serial.empty()) {
        return {};
    }
    std::string key(kSourceGroup);
    key += "/labels/";
    for (const char c : serial) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_';
        key.push_back(keep ? c : '_');
    }
    return key;
}

// What is wrong with a serial the operator typed for writing to a dongle's
// EEPROM, or empty when it is fine. The rule is tools/rtl-serial's own: one to
// 32 letters, digits, hyphens and underscores.
[[nodiscard]] inline std::string serial_problem(std::string_view serial)
{
    if (serial.empty()) {
        return "type a serial";
    }
    if (serial.size() > 32) {
        return "32 characters at most";
    }
    for (const char c : serial) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            return "letters, digits, - and _ only";
        }
    }
    return {};
}

}  // namespace revenant::ui
