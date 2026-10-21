// The trunking panel's text fields: the talkgroup alias table and the allow,
// deny and priority lists, as the operator types them and as the settings
// store keeps them.
//
// Plain text on purpose, one entry per line or comma: a scanner owner pastes
// a talkgroup list from a reference site, and a grid editor would make that
// a row at a time. Anything that does not parse is skipped and counted, so
// the panel can say how many lines it ignored rather than refusing them all.
//
// No Qt, so ui/tests/test_trunk_aliases.cpp links without it.

#pragma once

#include <charconv>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace revenant::ui {

namespace trunk_text_detail {

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

template <typename T>
bool number(std::string_view text, T& out) {
    text = trim(text);
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc() && end == text.data() + text.size() && !text.empty();
}

// Splits on newlines and commas.
inline std::vector<std::string_view> entries(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n' || text[i] == ',') {
            const std::string_view piece = trim(text.substr(start, i - start));
            if (!piece.empty() && piece.front() != '#') {
                out.push_back(piece);
            }
            start = i + 1;
        }
    }
    return out;
}

}  // namespace trunk_text_detail

template <typename V>
struct TrunkParsed {
    V value;
    int skipped = 0;
};

// "100 = Fire Dispatch", one per line. A comma is part of a label, so only
// newlines separate here.
inline TrunkParsed<std::map<std::uint32_t, std::string>> parse_trunk_aliases(
    std::string_view text) {
    using namespace trunk_text_detail;
    TrunkParsed<std::map<std::uint32_t, std::string>> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i != text.size() && text[i] != '\n') {
            continue;
        }
        const std::string_view line = trim(text.substr(start, i - start));
        start = i + 1;
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto eq = line.find('=');
        std::uint32_t tg = 0;
        const std::string_view label = eq == std::string_view::npos ? "" : trim(line.substr(eq + 1));
        if (eq == std::string_view::npos || !number(line.substr(0, eq), tg) || label.empty()) {
            ++out.skipped;
            continue;
        }
        out.value[tg] = std::string(label);
    }
    return out;
}

inline std::string format_trunk_aliases(const std::map<std::uint32_t, std::string>& aliases) {
    std::string out;
    for (const auto& [tg, label] : aliases) {
        out += std::to_string(tg) + " = " + label + "\n";
    }
    return out;
}

// "100, 200, 3501" or one per line.
inline TrunkParsed<std::vector<std::uint32_t>> parse_trunk_groups(std::string_view text) {
    using namespace trunk_text_detail;
    TrunkParsed<std::vector<std::uint32_t>> out;
    for (const std::string_view piece : entries(text)) {
        std::uint32_t tg = 0;
        if (number(piece, tg)) {
            out.value.push_back(tg);
        } else {
            ++out.skipped;
        }
    }
    return out;
}

// "500=5, 600=3": talkgroup = priority, higher wins.
inline TrunkParsed<std::vector<std::pair<std::uint32_t, std::int32_t>>> parse_trunk_priorities(
    std::string_view text) {
    using namespace trunk_text_detail;
    TrunkParsed<std::vector<std::pair<std::uint32_t, std::int32_t>>> out;
    for (const std::string_view piece : entries(text)) {
        const auto eq = piece.find('=');
        std::uint32_t tg = 0;
        std::int32_t priority = 0;
        if (eq == std::string_view::npos || !number(piece.substr(0, eq), tg) ||
            !number(piece.substr(eq + 1), priority)) {
            ++out.skipped;
            continue;
        }
        out.value.emplace_back(tg, priority);
    }
    return out;
}

// What a call row shows for its talkgroup: the alias where there is one.
inline std::string trunk_group_label(const std::map<std::uint32_t, std::string>& aliases,
                                     std::uint32_t talkgroup) {
    const auto it = aliases.find(talkgroup);
    return it == aliases.end() ? "TG " + std::to_string(talkgroup) : it->second;
}

}  // namespace revenant::ui
