// The command palette's search: what it lists, how a query matches an entry,
// and the order the matches come back in.
//
// Qt-free so ui/tests can hold it. models/key_map.h turns the answer into
// what ui/qml/CommandPalette.qml draws.
//
// WHAT IS LISTED. Every window action in models/key_actions.h, in the table's
// order, then every band in models/band_plan.h. The filter display's keys and
// the overlay's own are not listed: they act on a display that has focus, and
// the palette is what has focus while it is open.
//
// HOW A QUERY MATCHES. Each word of the query has to appear in the entry, its
// letters in order and not necessarily together, so "wid fil" finds "widen
// the filter" and "20m" finds "20 m". A word is looked for in the label first
// and then in the entry's group and keywords, which is how "mode usb" finds
// "switch the receiver to usb": neither word is where the other is. A word
// found only in the group or keywords scores below the same word found in the
// label, so what the palette says is what ranks.
//
// HOW MATCHES RANK. A letter scores more at the start of a word, and more
// again straight after the letter before it, and a gap costs a little per
// letter skipped. So a query typed as the start of a word beats the same
// letters scattered through a longer one: "fl" puts "floor" above "fill
// level". The best-scoring placement of the letters is the one taken, not the
// first one found, which is the difference between "20m" scoring "20 m" at
// its start and "SW 120 m" at a digit halfway through a number.
//
// Enabled entries come before disabled ones whatever their score, because
// Return runs the entry at the top and a disabled one would do nothing; the
// disabled ones stay in the list, dimmed and saying why, so a search for
// something unavailable answers rather than coming back empty. Ties keep the
// table's order.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "models/band_plan.h"
#include "models/key_actions.h"

namespace revenant::ui {

// ---------------------------------------------------------------------------
// One word against one text
// ---------------------------------------------------------------------------

struct FuzzyMatch {
    int score = 0;

    // Where each letter of the pattern landed in the text, ascending.
    std::vector<std::size_t> positions;
};

namespace fuzzy_detail {

inline constexpr int kLetter = 16;
inline constexpr int kTextStart = 32;
inline constexpr int kWordStart = 24;
inline constexpr int kDigitRunStart = 8;
// A letter straight after the one before it is worth as much as a word
// start, so "flo" run together at the start of "floor" beats three word
// starts scattered through "fill level old".
inline constexpr int kConsecutive = 24;
inline constexpr int kWholeWord = 12;
inline constexpr int kGapPerLetter = 1;
inline constexpr int kLeadingGapCap = 12;

[[nodiscard]] constexpr char fold(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] constexpr bool is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

[[nodiscard]] constexpr bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

// What landing a letter at this position is worth, before any run or gap.
[[nodiscard]] constexpr int letter_score(std::string_view text, std::size_t j)
{
    int score = kLetter;
    if (j == 0) {
        score += kTextStart;
    } else if (!is_alnum(text[j - 1])) {
        score += kWordStart;
    } else if (is_digit(text[j]) != is_digit(text[j - 1])) {
        score += kDigitRunStart;
    }
    return score;
}

}  // namespace fuzzy_detail

// The best placement of pattern's letters in text, in order, ignoring case,
// or nothing when they are not all there. An empty pattern matches anything
// with a score of zero.
[[nodiscard]] inline std::optional<FuzzyMatch> fuzzy_match(std::string_view pattern,
                                                           std::string_view text)
{
    using namespace fuzzy_detail;
    FuzzyMatch out;
    const std::size_t m = pattern.size();
    const std::size_t n = text.size();
    if (m == 0) {
        return out;
    }
    if (m > n) {
        return std::nullopt;
    }

    // best[i * n + j]: the best score with pattern[0..i] placed and pattern[i]
    // on text[j]. from[]: where pattern[i - 1] was, to walk back the
    // positions. A linear gap cost lets the best predecessor be carried along
    // the row rather than searched for, so this is m * n and not m * n * n.
    constexpr int kNone = -(1 << 28);
    std::vector<int> best(m * n, kNone);
    std::vector<std::size_t> from(m * n, 0);

    for (std::size_t j = 0; j < n; ++j) {
        if (fold(text[j]) == fold(pattern[0])) {
            const int leading = static_cast<int>(std::min<std::size_t>(j, kLeadingGapCap));
            best[j] = letter_score(text, j) - leading * kGapPerLetter;
        }
    }

    for (std::size_t i = 1; i < m; ++i) {
        // The best of best[i-1][k] + k * gap for k < j - 1, carried as j moves.
        int carried = kNone;
        std::size_t carried_at = 0;
        for (std::size_t j = i; j < n; ++j) {
            if (j >= 2) {
                const std::size_t k = j - 2;
                const int prior = best[(i - 1) * n + k];
                if (prior != kNone) {
                    const int lifted = prior + static_cast<int>(k) * kGapPerLetter;
                    if (lifted > carried) {
                        carried = lifted;
                        carried_at = k;
                    }
                }
            }
            if (fold(text[j]) != fold(pattern[i])) {
                continue;
            }
            int score = kNone;
            std::size_t at = 0;
            const int adjacent = best[(i - 1) * n + (j - 1)];
            if (adjacent != kNone) {
                score = adjacent + kConsecutive;
                at = j - 1;
            }
            if (carried != kNone) {
                const int gapped = carried - static_cast<int>(j - 1) * kGapPerLetter;
                if (gapped > score) {
                    score = gapped;
                    at = carried_at;
                }
            }
            if (score != kNone) {
                best[i * n + j] = score + letter_score(text, j);
                from[i * n + j] = at;
            }
        }
    }

    int top = kNone;
    std::size_t end = 0;
    for (std::size_t j = m - 1; j < n; ++j) {
        if (best[(m - 1) * n + j] > top) {
            top = best[(m - 1) * n + j];
            end = j;
        }
    }
    if (top == kNone) {
        return std::nullopt;
    }

    out.score = top;
    out.positions.resize(m);
    std::size_t j = end;
    for (std::size_t i = m; i-- > 0;) {
        out.positions[i] = j;
        j = from[i * n + j];
    }

    // The pattern is a whole word of the text, which "2" is in "2 m" and is
    // not in "20 m". Without this the two differ by one letter of gap.
    const std::size_t first = out.positions.front();
    const std::size_t last = out.positions.back();
    const bool whole = last - first + 1 == m && (first == 0 || !is_alnum(text[first - 1])) &&
                       (last + 1 == n || !is_alnum(text[last + 1]));
    if (whole) {
        out.score += kWholeWord;
    }
    return out;
}

// ---------------------------------------------------------------------------
// What the palette lists
// ---------------------------------------------------------------------------

struct PaletteEntry {
    std::string id;
    std::string label;
    std::string group;
    std::string keywords;

    // "Ctrl+K or Ctrl+Shift+P", empty for an entry no key reaches.
    std::string keys;
    std::uint32_t needs = kNeedsNothing;

    std::string handler;
    std::string argument;

    // A band's index in kBands, or -1 for an action.
    int band = -1;
};

// Word-for-word what a band is found by: its name, its group and the word
// "band", so "band" lists them all and "airband" finds one.
inline constexpr std::string_view kBandKeywords = "band";

[[nodiscard]] inline std::vector<PaletteEntry> palette_entries()
{
    std::vector<PaletteEntry> out;
    for (const KeyAction& action : kKeyActions) {
        if (action.context != KeyContext::Window) {
            continue;
        }
        PaletteEntry entry;
        entry.id = action.id;
        entry.label = action.label;
        entry.group = action.group;
        entry.keywords = action.keywords;
        entry.keys = key_text(action);
        entry.needs = action.needs;
        entry.handler = action.handler;
        entry.argument = action.argument;
        out.push_back(std::move(entry));
    }
    for (std::size_t i = 0; i < kBands.size(); ++i) {
        const Band& band = kBands[i];
        PaletteEntry entry;
        entry.id = "band." + std::to_string(i);
        entry.label = band.name;
        entry.group = band.group;
        entry.keywords = std::string(kBandKeywords) + " " + std::string(band.mode);
        entry.needs = kNeedsRetune;
        entry.handler = "tune.band";
        entry.argument = std::to_string(i);
        entry.band = static_cast<int>(i);
        out.push_back(std::move(entry));
    }
    return out;
}

// ---------------------------------------------------------------------------
// A query against the list
// ---------------------------------------------------------------------------

enum class PaletteScope {
    Everything,
    Bands,
};

// What decides whether an entry can run now: the bits models/key_actions.h
// derives, and the source's tuning limits for a band.
struct PaletteState {
    std::uint32_t have = kNeedsNothing;
    std::int64_t tune_low_hz = 0;
    std::int64_t tune_high_hz = 0;
};

struct PaletteHit {
    std::size_t entry = 0;
    int score = 0;
    bool enabled = false;

    // Why it cannot run, when it cannot.
    std::string reason;

    // The label's letters the query landed on, for the palette to mark.
    std::vector<std::size_t> label_positions;
};

// A word found only in the group or keywords scores this much less than it
// would have in the label.
inline constexpr int kOffLabelPenalty = 40;

[[nodiscard]] inline std::vector<std::string_view> query_words(std::string_view query)
{
    std::vector<std::string_view> words;
    std::size_t i = 0;
    while (i < query.size()) {
        while (i < query.size() && query[i] == ' ') {
            ++i;
        }
        const std::size_t start = i;
        while (i < query.size() && query[i] != ' ') {
            ++i;
        }
        if (i > start) {
            words.push_back(query.substr(start, i - start));
        }
    }
    return words;
}

[[nodiscard]] inline std::vector<PaletteHit> rank_palette(const std::vector<PaletteEntry>& entries,
                                                          std::string_view query,
                                                          PaletteScope scope,
                                                          const PaletteState& state)
{
    const std::vector<std::string_view> words = query_words(query);
    std::vector<PaletteHit> hits;
    for (std::size_t e = 0; e < entries.size(); ++e) {
        const PaletteEntry& entry = entries[e];
        if (scope == PaletteScope::Bands && entry.band < 0) {
            continue;
        }

        PaletteHit hit;
        hit.entry = e;
        bool all = true;
        for (const std::string_view word : words) {
            const std::optional<FuzzyMatch> in_label = fuzzy_match(word, entry.label);
            std::optional<FuzzyMatch> off_label = fuzzy_match(word, entry.group);
            const std::optional<FuzzyMatch> in_keywords = fuzzy_match(word, entry.keywords);
            if (in_keywords && (!off_label || in_keywords->score > off_label->score)) {
                off_label = in_keywords;
            }
            const int label_score = in_label ? in_label->score : -(1 << 28);
            const int other_score = off_label ? off_label->score - kOffLabelPenalty : -(1 << 28);
            if (!in_label && !off_label) {
                all = false;
                break;
            }
            if (label_score >= other_score) {
                hit.score += label_score;
                hit.label_positions.insert(hit.label_positions.end(),
                                           in_label->positions.begin(),
                                           in_label->positions.end());
            } else {
                hit.score += other_score;
            }
        }
        if (!all) {
            continue;
        }
        std::sort(hit.label_positions.begin(), hit.label_positions.end());
        hit.label_positions.erase(
            std::unique(hit.label_positions.begin(), hit.label_positions.end()),
            hit.label_positions.end());

        const std::uint32_t missing = key_missing(entry.needs, state.have);
        hit.enabled = missing == 0;
        hit.reason = std::string(key_needs_text(missing));
        if (hit.enabled && entry.band >= 0 &&
            !band_reachable(kBands[static_cast<std::size_t>(entry.band)], state.tune_low_hz,
                            state.tune_high_hz)) {
            hit.enabled = false;
            hit.reason = "out of this radio's range";
        }
        hits.push_back(std::move(hit));
    }

    std::stable_sort(hits.begin(), hits.end(), [](const PaletteHit& a, const PaletteHit& b) {
        if (a.enabled != b.enabled) {
            return a.enabled;
        }
        return a.score > b.score;
    });
    return hits;
}

}  // namespace revenant::ui
