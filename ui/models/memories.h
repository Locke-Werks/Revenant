// Memories: the frequency manager's model. Places an operator saved or
// imported, the tags and groups that sort them, and scan lists built from
// them.
//
// Qt-free on the rule the rest of ui/models follows, so ui/tests can hold it.
// ui/models/frequency_manager.cpp is the Qt half: where the file lives, the
// list the panel draws, and the reach into EngineLink for a recall.
//
// WHAT A MEMORY IS. A bookmark with more to say about itself. Everything a
// Bookmark carries is here under the same meaning (models/bookmarks.h has the
// arguments, which are not repeated): an ABSOLUTE frequency in integer hertz,
// a mode by the engine's name for it, and filter edges in hertz from the
// memory's own centre, both zero for the mode's default. What a memory adds is
// what makes a thousand of them usable: tags, a free note, a group, an id that
// survives a rename, and when it was made and last used. A memory is recalled
// only when somebody picks it, so it makes no claim about a band at startup
// and settings.h's refusal to remember the receiver still stands.
//
// WHERE IT LIVES. One JSON file, memories.json, in the user's application data
// directory; frequency_manager.cpp names the directory. Not QSettings: the
// bookmark list was one registry string, rewritten whole on every change,
// which is fine for a dozen and not for a thousand, and a registry value is
// not something an operator can copy to a USB stick. The file is the backup.
//
// THE SCHEMA, version 1. Written by memory_book_to_json below and read by
// memory_book_from_json; docs/ui-spectrum.md carries the same description for
// somebody editing the file by hand.
//
//   {
//     "format": "revenant-memories",
//     "version": 1,
//     "memories": [
//       { "id": 7, "name": "KXYZ", "hz": 98500000, "mode": "wfm",
//         "low": -100000, "high": 100000,
//         "tags": ["broadcast", "local"], "notes": "", "group": "FM",
//         "created": "2026-09-23T21:04:11Z", "used": "2026-09-23T21:30:00Z" }
//     ],
//     "scan_lists": [
//       { "name": "tower", "kind": "memories", "members": [7, 9] },
//       { "name": "2 m", "kind": "range", "low": 144000000,
//         "high": 148000000, "step": 12500, "mode": "nfm" }
//     ]
//   }
//
// hz, low, high and step are integers. created and used are UTC in ISO 8601
// to the second, or absent when not known: a bookmark carried across from the
// old list has no creation time, and one that was never recalled has no use.
// A file whose version is higher than this reader's is refused rather than
// read, so an older client never rewrites a newer file with fields missing.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/error.h"
#include "models/bookmarks.h"
#include "models/frequency_entry.h"
#include "models/json_lite.h"
#include "models/mode_choice.h"

namespace revenant::ui {

inline constexpr std::string_view kMemoryFormat = "revenant-memories";
inline constexpr std::int64_t kMemorySchemaVersion = 1;

// The file's name inside the application data directory.
inline constexpr std::string_view kMemoryFileName = "memories.json";

struct Memory {
    // Stable for the memory's life and never reused within a file, so a scan
    // list can name its members and an undo can put one back where it was.
    // Zero until the book assigns one.
    std::uint64_t id = 0;

    std::string name;
    std::int64_t freq_hz = 0;
    std::string mode;
    std::int32_t passband_low = 0;
    std::int32_t passband_high = 0;

    // Lower case is not imposed: a tag is shown as it was first typed. Two
    // tags that differ only in case are one tag, see normalise_tags.
    std::vector<std::string> tags;
    std::string notes;

    // One group per memory, which is what SDR#'s GroupName and an SDR++ list
    // are. Tags are the many-to-many; a group is where it came from.
    std::string group;

    // UTC seconds since the Unix epoch, zero when not known.
    std::int64_t created = 0;
    std::int64_t used = 0;
};

// A scan list: either named memories, in the order given, or a range of
// frequencies with a step. The model only. No scanning engine exists yet and
// nothing here tunes anything; the panel says so where the lists are shown.
struct ScanList {
    enum class Kind : std::uint8_t {
        Memories,
        Range,
    };

    std::string name;
    Kind kind = Kind::Memories;

    // Memory ids, for Kind::Memories.
    std::vector<std::uint64_t> members;

    // For Kind::Range: both ends inclusive, in hertz, and the mode a scan
    // would listen in.
    std::int64_t low_hz = 0;
    std::int64_t high_hz = 0;
    std::int64_t step_hz = 0;
    std::string mode;
};

struct MemoryBook {
    std::vector<Memory> memories;
    std::vector<ScanList> scan_lists;

    // The next id add_memory hands out. Always above every id in the book.
    std::uint64_t next_id = 1;
};

// Something a reader passed over, and why. line is the source line, or zero
// where the source has no lines worth naming.
struct SkippedLine {
    std::size_t line = 0;
    std::string what;
    std::string reason;
};

// ---------------------------------------------------------------------------
// Small text rules
// ---------------------------------------------------------------------------

namespace memory_detail {

[[nodiscard]] inline char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] inline std::string trimmed(std::string_view text)
{
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && space(text[begin])) {
        ++begin;
    }
    while (end > begin && space(text[end - 1])) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

}  // namespace memory_detail

// ASCII lower case. Names from other programs arrive in any script, and
// folding only ASCII leaves the rest compared exactly rather than wrongly.
[[nodiscard]] inline std::string fold_case(std::string_view text)
{
    std::string out(text);
    for (char& c : out) {
        c = memory_detail::lower(c);
    }
    return out;
}

// Tags as a set: trimmed, inner whitespace collapsed, commas dropped (a comma
// is what separates tags when they are typed), empty ones dropped, and a
// second spelling of a tag already held dropped in favour of the first.
[[nodiscard]] inline std::vector<std::string> normalise_tags(const std::vector<std::string>& tags)
{
    std::vector<std::string> out;
    std::vector<std::string> folded;
    for (const std::string& raw : tags) {
        std::string tag;
        bool space = false;
        for (const char c : raw) {
            if (c == ',') {
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                space = !tag.empty();
                continue;
            }
            if (space) {
                tag.push_back(' ');
                space = false;
            }
            tag.push_back(c);
        }
        if (tag.empty()) {
            continue;
        }
        std::string key = fold_case(tag);
        if (std::find(folded.begin(), folded.end(), key) != folded.end()) {
            continue;
        }
        folded.push_back(std::move(key));
        out.push_back(std::move(tag));
    }
    return out;
}

// "a, b ,c" as typed into the tag field.
[[nodiscard]] inline std::vector<std::string> split_tags(std::string_view text)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
        parts.emplace_back(text.substr(start, end - start));
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return normalise_tags(parts);
}

[[nodiscard]] inline std::string join_tags(const std::vector<std::string>& tags)
{
    std::string out;
    for (const std::string& tag : tags) {
        if (!out.empty()) {
            out += ", ";
        }
        out += tag;
    }
    return out;
}

// The mode is one the engine plans a receiver for.
[[nodiscard]] inline bool memory_mode_known(std::string_view mode)
{
    return std::find(kDemodNames.begin(), kDemodNames.end(), mode) != kDemodNames.end();
}

// ---------------------------------------------------------------------------
// Time, as the file writes it
// ---------------------------------------------------------------------------

// "2026-09-23T21:04:11Z". The caller passes the time in: nothing in this file
// reads a clock, so a test is the same test tomorrow.
[[nodiscard]] inline std::string format_utc(std::int64_t utc_seconds)
{
    using namespace std::chrono;
    const sys_seconds at{std::chrono::seconds{utc_seconds}};
    const sys_days midnight = floor<days>(at);
    const year_month_day date{midnight};
    const hh_mm_ss time_of_day{at - midnight};
    char out[32];
    std::snprintf(out, sizeof out, "%04d-%02u-%02uT%02d:%02d:%02dZ", static_cast<int>(date.year()),
                  static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                  static_cast<int>(time_of_day.hours().count()),
                  static_cast<int>(time_of_day.minutes().count()),
                  static_cast<int>(time_of_day.seconds().count()));
    return out;
}

// The reverse, for exactly the form format_utc writes. Anything else is not a
// time this file wrote and reads as unknown rather than as a guess.
[[nodiscard]] inline std::optional<std::int64_t> parse_utc(std::string_view text)
{
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != 'Z') {
        return std::nullopt;
    }
    const auto number = [&](std::size_t at, std::size_t width) -> std::optional<int> {
        int out = 0;
        for (std::size_t i = at; i < at + width; ++i) {
            if (text[i] < '0' || text[i] > '9') {
                return std::nullopt;
            }
            out = out * 10 + (text[i] - '0');
        }
        return out;
    };
    const auto y = number(0, 4);
    const auto mo = number(5, 2);
    const auto d = number(8, 2);
    const auto h = number(11, 2);
    const auto mi = number(14, 2);
    const auto s = number(17, 2);
    if (!y || !mo || !d || !h || !mi || !s || *h > 23 || *mi > 59 || *s > 59) {
        return std::nullopt;
    }
    using namespace std::chrono;
    const year_month_day date{year{*y}, month{static_cast<unsigned>(*mo)},
                              day{static_cast<unsigned>(*d)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    const sys_seconds at = sys_days{date} + hours{*h} + minutes{*mi} + std::chrono::seconds{*s};
    return at.time_since_epoch().count();
}

// ---------------------------------------------------------------------------
// The book
// ---------------------------------------------------------------------------

[[nodiscard]] inline const Memory* find_memory(const MemoryBook& book, std::uint64_t id)
{
    for (const Memory& memory : book.memories) {
        if (memory.id == id) {
            return &memory;
        }
    }
    return nullptr;
}

[[nodiscard]] inline Memory* find_memory(MemoryBook& book, std::uint64_t id)
{
    return const_cast<Memory*>(find_memory(static_cast<const MemoryBook&>(book), id));
}

// Why this memory cannot be kept, or empty when it can. The same two refusals
// the bookmark list made, for the same reasons: no frequency is no place, and
// an empty mode means "keep the current one" to tuneReceiver, which makes a
// recall that does something different each time.
[[nodiscard]] inline std::string memory_problem(const Memory& memory)
{
    if (memory.freq_hz <= 0) {
        return "no frequency";
    }
    if (memory.mode.empty()) {
        return "no mode";
    }
    return {};
}

// Adds a memory, handing it an id and, if it has none, a creation time.
// Answers the id.
inline std::uint64_t add_memory(MemoryBook& book, Memory memory, std::int64_t now)
{
    memory.id = book.next_id++;
    memory.tags = normalise_tags(memory.tags);
    if (memory.created == 0) {
        memory.created = now;
    }
    book.memories.push_back(std::move(memory));
    return book.memories.back().id;
}

// The same place under the same name, already in the book: the de-duplication
// rule for an import and for saving the receiver twice. Names compare without
// case and without surrounding space, and the frequency exactly: two stations
// a channel apart are two memories, and a list that merged them would lose one.
[[nodiscard]] inline const Memory* duplicate_of(const MemoryBook& book, std::int64_t freq_hz,
                                                std::string_view name)
{
    const std::string key = fold_case(memory_detail::trimmed(name));
    for (const Memory& memory : book.memories) {
        if (memory.freq_hz == freq_hz && fold_case(memory_detail::trimmed(memory.name)) == key) {
            return &memory;
        }
    }
    return nullptr;
}

// The memory within tolerance_hz of this frequency, nearest first, or
// nullptr. What the panel's "already saved" reads, on bookmark_at's argument
// that two frequencies inside one passband are one station.
[[nodiscard]] inline const Memory* memory_near(const MemoryBook& book, std::int64_t freq_hz,
                                               std::int64_t tolerance_hz)
{
    const Memory* best = nullptr;
    std::int64_t best_distance = 0;
    for (const Memory& memory : book.memories) {
        const std::int64_t distance =
            memory.freq_hz > freq_hz ? memory.freq_hz - freq_hz : freq_hz - memory.freq_hz;
        if (distance > tolerance_hz) {
            continue;
        }
        if (best == nullptr || distance < best_distance) {
            best = &memory;
            best_distance = distance;
        }
    }
    return best;
}

// A memory as the recall rule in models/bookmarks.h wants it.
[[nodiscard]] inline Bookmark memory_as_bookmark(const Memory& memory)
{
    Bookmark mark;
    mark.name = memory.name;
    mark.freq_hz = memory.freq_hz;
    mark.demod = memory.mode;
    mark.passband_low = memory.passband_low;
    mark.passband_high = memory.passband_high;
    return mark;
}

// What a list shows for a memory with no name, which is bookmark_label's
// answer so the two lists agree.
[[nodiscard]] inline std::string memory_label(const Memory& memory)
{
    return bookmark_label(memory_as_bookmark(memory));
}

// ---------------------------------------------------------------------------
// Delete, and the undo that puts it back
// ---------------------------------------------------------------------------

// Everything a delete took away, which is the memory, its place in the list
// and its place in every scan list that named it. Restoring any less would
// put back a memory that had quietly left the scan lists it was in.
struct DeletedMemory {
    Memory memory;
    std::size_t index = 0;

    struct Membership {
        std::size_t list = 0;
        std::size_t position = 0;
    };
    std::vector<Membership> memberships;
};

inline std::optional<DeletedMemory> delete_memory(MemoryBook& book, std::uint64_t id)
{
    for (std::size_t i = 0; i < book.memories.size(); ++i) {
        if (book.memories[i].id != id) {
            continue;
        }
        DeletedMemory gone;
        gone.memory = std::move(book.memories[i]);
        gone.index = i;
        book.memories.erase(book.memories.begin() + static_cast<std::ptrdiff_t>(i));
        for (std::size_t l = 0; l < book.scan_lists.size(); ++l) {
            auto& members = book.scan_lists[l].members;
            // Back to front, so each recorded position is the one to insert
            // at when the list is rebuilt front to back.
            for (std::size_t p = members.size(); p-- > 0;) {
                if (members[p] == id) {
                    members.erase(members.begin() + static_cast<std::ptrdiff_t>(p));
                    gone.memberships.push_back({l, p});
                }
            }
        }
        std::reverse(gone.memberships.begin(), gone.memberships.end());
        return gone;
    }
    return std::nullopt;
}

// Puts a deleted memory back where it was, under its own id. Clamped where
// the book has shrunk since, so an undo after two deletes in a row still
// lands every memory rather than refusing the one whose index moved.
inline void restore_memory(MemoryBook& book, DeletedMemory gone)
{
    const std::size_t at = std::min(gone.index, book.memories.size());
    const std::uint64_t id = gone.memory.id;
    book.memories.insert(book.memories.begin() + static_cast<std::ptrdiff_t>(at),
                         std::move(gone.memory));
    for (const auto& member : gone.memberships) {
        if (member.list >= book.scan_lists.size()) {
            continue;
        }
        auto& members = book.scan_lists[member.list].members;
        const std::size_t position = std::min(member.position, members.size());
        members.insert(members.begin() + static_cast<std::ptrdiff_t>(position), id);
    }
    book.next_id = std::max(book.next_id, id + 1);
}

// ---------------------------------------------------------------------------
// Search, tags and sort
// ---------------------------------------------------------------------------

enum class MemorySort : std::uint8_t {
    Name,
    Frequency,
    Mode,
    Tags,
    Used,
};

// Whether a memory matches a typed query. Every word has to appear, without
// case, somewhere in the name, the group, the notes, a tag, the mode, or the
// frequency written in megahertz or in hertz. So "146.52" finds 146520000,
// "air tower" finds a memory tagged air with tower in its name, and a query
// of nothing matches everything.
[[nodiscard]] inline bool memory_matches(const Memory& memory, std::string_view query)
{
    std::string hay = fold_case(memory.name);
    hay += '\n';
    hay += fold_case(memory.group);
    hay += '\n';
    hay += fold_case(memory.notes);
    hay += '\n';
    hay += fold_case(join_tags(memory.tags));
    hay += '\n';
    hay += memory.mode;
    hay += '\n';
    hay += format_mhz(memory.freq_hz);
    hay += '\n';
    hay += std::to_string(memory.freq_hz);

    const std::string folded = fold_case(query);
    std::size_t at = 0;
    while (at < folded.size()) {
        while (at < folded.size() && (folded[at] == ' ' || folded[at] == '\t')) {
            ++at;
        }
        std::size_t end = at;
        while (end < folded.size() && folded[end] != ' ' && folded[end] != '\t') {
            ++end;
        }
        if (end > at && hay.find(folded.substr(at, end - at)) == std::string::npos) {
            return false;
        }
        at = end;
    }
    return true;
}

// Whether a memory carries every tag in wanted, without case. Every and not
// any: choosing a second tag narrows the list, which is what choosing more
// of anything in a filter does everywhere else.
[[nodiscard]] inline bool memory_has_tags(const Memory& memory,
                                          const std::vector<std::string>& wanted)
{
    for (const std::string& tag : wanted) {
        const std::string key = fold_case(tag);
        const bool has = std::any_of(memory.tags.begin(), memory.tags.end(),
                                     [&](const std::string& own) { return fold_case(own) == key; });
        if (!has) {
            return false;
        }
    }
    return true;
}

// The indices of the memories the panel lists, in the order it lists them.
//
// Ties fall back to frequency and then to id, so two memories that compare
// equal on the chosen column keep one order across every refresh instead of
// swapping rows under the pointer. Descending reverses the chosen column
// only, not the tie-break. Used sorts the most recent first when ascending,
// because "recently used" is the only reading of that column anybody wants,
// and never-used memories go last either way.
[[nodiscard]] inline std::vector<std::size_t> list_memories(const MemoryBook& book,
                                                            std::string_view query,
                                                            const std::vector<std::string>& tags,
                                                            MemorySort sort, bool descending)
{
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < book.memories.size(); ++i) {
        const Memory& memory = book.memories[i];
        if (memory_matches(memory, query) && memory_has_tags(memory, tags)) {
            out.push_back(i);
        }
    }

    const auto column = [&](const Memory& a, const Memory& b) -> int {
        const auto three_way = [](const auto& x, const auto& y) { return x < y ? -1 : y < x ? 1 : 0; };
        switch (sort) {
            case MemorySort::Name:
                return three_way(fold_case(memory_label(a)), fold_case(memory_label(b)));
            case MemorySort::Frequency:
                return 0;
            case MemorySort::Mode:
                return three_way(a.mode, b.mode);
            case MemorySort::Tags: {
                // Untagged last, whichever way the column runs.
                if (a.tags.empty() != b.tags.empty()) {
                    return a.tags.empty() != descending ? 1 : -1;
                }
                return three_way(fold_case(join_tags(a.tags)), fold_case(join_tags(b.tags)));
            }
            case MemorySort::Used: {
                if ((a.used == 0) != (b.used == 0)) {
                    return (a.used == 0) != descending ? 1 : -1;
                }
                return three_way(b.used, a.used);
            }
        }
        return 0;
    };

    std::sort(out.begin(), out.end(), [&](std::size_t left, std::size_t right) {
        const Memory& a = book.memories[left];
        const Memory& b = book.memories[right];
        int order = column(a, b);
        if (sort == MemorySort::Frequency) {
            order = a.freq_hz < b.freq_hz ? -1 : b.freq_hz < a.freq_hz ? 1 : 0;
        }
        if (order != 0) {
            return descending ? order > 0 : order < 0;
        }
        if (a.freq_hz != b.freq_hz) {
            return a.freq_hz < b.freq_hz;
        }
        return a.id < b.id;
    });
    return out;
}

struct TagCount {
    std::string tag;
    std::size_t count = 0;
};

// Every tag in the book with how many memories carry it, alphabetical without
// case, each shown as its first spelling.
[[nodiscard]] inline std::vector<TagCount> tag_counts(const MemoryBook& book)
{
    std::vector<TagCount> out;
    for (const Memory& memory : book.memories) {
        for (const std::string& tag : memory.tags) {
            const std::string key = fold_case(tag);
            auto found = std::find_if(out.begin(), out.end(), [&](const TagCount& known) {
                return fold_case(known.tag) == key;
            });
            if (found == out.end()) {
                out.push_back({tag, 1});
            } else {
                ++found->count;
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const TagCount& a, const TagCount& b) {
        return fold_case(a.tag) < fold_case(b.tag);
    });
    return out;
}

// ---------------------------------------------------------------------------
// Scan lists
// ---------------------------------------------------------------------------

// Past this a range is refused: a scan over it would take longer than the
// radio will stay on one band, and a typo of a step in hertz for one in
// kilohertz is the usual way to get there.
inline constexpr std::int64_t kMaxScanChannels = 100'000;

// Why a scan list is not usable, or empty. Memory ids that are no longer in
// the book are not a problem: a delete takes them out, and an undo puts them
// back.
[[nodiscard]] inline std::string scan_list_problem(const ScanList& list)
{
    if (list.name.empty()) {
        return "a scan list needs a name";
    }
    if (list.kind == ScanList::Kind::Memories) {
        return {};
    }
    if (list.low_hz <= 0 || list.high_hz <= 0) {
        return "a range needs both ends above zero";
    }
    if (list.high_hz < list.low_hz) {
        return "the range ends below where it starts";
    }
    if (list.step_hz <= 0) {
        return "a range needs a step above zero";
    }
    if ((list.high_hz - list.low_hz) / list.step_hz + 1 > kMaxScanChannels) {
        return "that range and step make more than " + std::to_string(kMaxScanChannels) +
               " channels";
    }
    if (!list.mode.empty() && !memory_mode_known(list.mode)) {
        return "the mode " + list.mode + " is not one the engine plans";
    }
    return {};
}

// How many places a scan of this list would visit.
[[nodiscard]] inline std::int64_t scan_list_size(const MemoryBook& book, const ScanList& list)
{
    if (list.kind == ScanList::Kind::Range) {
        if (!scan_list_problem(list).empty()) {
            return 0;
        }
        return (list.high_hz - list.low_hz) / list.step_hz + 1;
    }
    std::int64_t count = 0;
    for (const std::uint64_t id : list.members) {
        if (find_memory(book, id) != nullptr) {
            ++count;
        }
    }
    return count;
}

// The frequencies a scan would visit, in order. A range starts on its low end
// and stops at the last step at or below its high end.
[[nodiscard]] inline std::vector<std::int64_t> scan_list_frequencies(const MemoryBook& book,
                                                                     const ScanList& list)
{
    std::vector<std::int64_t> out;
    if (list.kind == ScanList::Kind::Range) {
        const std::int64_t count = scan_list_size(book, list);
        out.reserve(static_cast<std::size_t>(count));
        for (std::int64_t i = 0; i < count; ++i) {
            out.push_back(list.low_hz + i * list.step_hz);
        }
        return out;
    }
    for (const std::uint64_t id : list.members) {
        if (const Memory* memory = find_memory(book, id)) {
            out.push_back(memory->freq_hz);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// The file
// ---------------------------------------------------------------------------

[[nodiscard]] inline json::Value memory_to_json(const Memory& memory)
{
    json::Value out = json::object();
    out.set("id", json::integer(static_cast<std::int64_t>(memory.id)));
    out.set("name", json::string(memory.name));
    out.set("hz", json::integer(memory.freq_hz));
    out.set("mode", json::string(memory.mode));
    out.set("low", json::integer(memory.passband_low));
    out.set("high", json::integer(memory.passband_high));
    json::Value tags = json::array();
    for (const std::string& tag : memory.tags) {
        tags.items.push_back(json::string(tag));
    }
    out.set("tags", std::move(tags));
    out.set("notes", json::string(memory.notes));
    out.set("group", json::string(memory.group));
    if (memory.created != 0) {
        out.set("created", json::string(format_utc(memory.created)));
    }
    if (memory.used != 0) {
        out.set("used", json::string(format_utc(memory.used)));
    }
    return out;
}

[[nodiscard]] inline json::Value scan_list_to_json(const ScanList& list)
{
    json::Value out = json::object();
    out.set("name", json::string(list.name));
    if (list.kind == ScanList::Kind::Memories) {
        out.set("kind", json::string("memories"));
        json::Value members = json::array();
        for (const std::uint64_t id : list.members) {
            members.items.push_back(json::integer(static_cast<std::int64_t>(id)));
        }
        out.set("members", std::move(members));
    } else {
        out.set("kind", json::string("range"));
        out.set("low", json::integer(list.low_hz));
        out.set("high", json::integer(list.high_hz));
        out.set("step", json::integer(list.step_hz));
        out.set("mode", json::string(list.mode));
    }
    return out;
}

[[nodiscard]] inline std::string memory_book_to_json(const MemoryBook& book)
{
    json::Value root = json::object();
    root.set("format", json::string(std::string(kMemoryFormat)));
    root.set("version", json::integer(kMemorySchemaVersion));
    json::Value memories = json::array();
    for (const Memory& memory : book.memories) {
        memories.items.push_back(memory_to_json(memory));
    }
    root.set("memories", std::move(memories));
    json::Value lists = json::array();
    for (const ScanList& list : book.scan_lists) {
        lists.items.push_back(scan_list_to_json(list));
    }
    root.set("scan_lists", std::move(lists));
    return json::write(root);
}

struct LoadedBook {
    MemoryBook book;

    // Entries that were in the file and are not in the book, with why. Kept
    // so the panel can say that the file held more than it shows, which is
    // the difference between a list that lost something and a file that was
    // edited by hand.
    std::vector<SkippedLine> skipped;
};

namespace memory_detail {

[[nodiscard]] inline std::string string_member(const json::Value& object, std::string_view key)
{
    const json::Value* value = object.find(key);
    return value != nullptr && value->is_string() ? value->text : std::string();
}

[[nodiscard]] inline std::optional<std::int32_t> edge_member(const json::Value& object,
                                                             std::string_view key)
{
    const json::Value* value = object.find(key);
    if (value == nullptr) {
        return 0;
    }
    const auto edge = json::as_integer(value);
    if (!edge || *edge < -100'000'000 || *edge > 100'000'000) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(*edge);
}

}  // namespace memory_detail

// One memory from the file, or why it was passed over.
[[nodiscard]] inline std::expected<Memory, std::string> memory_from_json(const json::Value& value)
{
    if (!value.is_object()) {
        return std::unexpected("not an object");
    }
    Memory memory;
    if (const auto id = json::as_integer(value.find("id")); id && *id > 0) {
        memory.id = static_cast<std::uint64_t>(*id);
    }
    memory.name = memory_detail::string_member(value, "name");
    const json::Value* hz = value.find("hz");
    const auto freq = json::as_integer(hz);
    if (!freq) {
        return std::unexpected(hz == nullptr ? "no hz" : "hz is not a whole number of hertz");
    }
    memory.freq_hz = *freq;
    memory.mode = memory_detail::string_member(value, "mode");
    const auto low = memory_detail::edge_member(value, "low");
    const auto high = memory_detail::edge_member(value, "high");
    if (!low || !high) {
        return std::unexpected("a filter edge is not a whole number of hertz in range");
    }
    memory.passband_low = *low;
    memory.passband_high = *high;
    if (const json::Value* tags = value.find("tags"); tags != nullptr && tags->is_array()) {
        for (const json::Value& tag : tags->items) {
            if (tag.is_string()) {
                memory.tags.push_back(tag.text);
            }
        }
        memory.tags = normalise_tags(memory.tags);
    }
    memory.notes = memory_detail::string_member(value, "notes");
    memory.group = memory_detail::string_member(value, "group");
    memory.created = parse_utc(memory_detail::string_member(value, "created")).value_or(0);
    memory.used = parse_utc(memory_detail::string_member(value, "used")).value_or(0);

    if (std::string problem = memory_problem(memory); !problem.empty()) {
        return std::unexpected(std::move(problem));
    }
    return memory;
}

// The book in a file, or an error when the file is not one this reader can
// trust with a rewrite. A single bad entry is skipped and reported; a file
// that is not the format, or is a newer version of it, is refused whole.
[[nodiscard]] inline Expected<LoadedBook> memory_book_from_json(std::string_view text)
{
    auto root = json::parse(text);
    if (!root) {
        return std::unexpected(with_context(root.error(), "not readable as JSON"));
    }
    if (!root->is_object() ||
        memory_detail::string_member(*root, "format") != kMemoryFormat) {
        return fail("not a Revenant memory file: there is no \"format\": \"" +
                    std::string(kMemoryFormat) + "\" at the top");
    }
    const auto version = json::as_integer(root->find("version"));
    if (!version || *version < 1) {
        return fail("the file has no version this reader understands");
    }
    if (*version > kMemorySchemaVersion) {
        return fail("the file is schema version " + std::to_string(*version) +
                    ", written by a newer Revenant; this one reads version " +
                    std::to_string(kMemorySchemaVersion) + " and will not rewrite it");
    }

    LoadedBook out;
    std::vector<std::uint64_t> seen;
    std::vector<Memory> needs_id;
    if (const json::Value* list = root->find("memories"); list != nullptr && list->is_array()) {
        for (const json::Value& item : list->items) {
            auto memory = memory_from_json(item);
            if (!memory) {
                out.skipped.push_back(
                    {item.line,
                     item.is_object() ? memory_detail::string_member(item, "name") : std::string(),
                     memory.error()});
                continue;
            }
            // A repeated or missing id is a hand edit, not a reason to lose
            // the memory: it gets a new one once every good id is known.
            if (memory->id == 0 ||
                std::find(seen.begin(), seen.end(), memory->id) != seen.end()) {
                needs_id.push_back(std::move(*memory));
                continue;
            }
            seen.push_back(memory->id);
            out.book.next_id = std::max(out.book.next_id, memory->id + 1);
            out.book.memories.push_back(std::move(*memory));
        }
    }
    for (Memory& memory : needs_id) {
        memory.id = out.book.next_id++;
        out.book.memories.push_back(std::move(memory));
    }

    if (const json::Value* lists = root->find("scan_lists"); lists != nullptr && lists->is_array()) {
        for (const json::Value& item : lists->items) {
            if (!item.is_object()) {
                out.skipped.push_back({item.line, {}, "a scan list that is not an object"});
                continue;
            }
            ScanList list;
            list.name = memory_detail::string_member(item, "name");
            const std::string kind = memory_detail::string_member(item, "kind");
            if (kind == "memories") {
                list.kind = ScanList::Kind::Memories;
                if (const json::Value* members = item.find("members");
                    members != nullptr && members->is_array()) {
                    for (const json::Value& member : members->items) {
                        if (const auto id = json::as_integer(&member); id && *id > 0) {
                            list.members.push_back(static_cast<std::uint64_t>(*id));
                        }
                    }
                }
            } else if (kind == "range") {
                list.kind = ScanList::Kind::Range;
                list.low_hz = json::as_integer(item.find("low")).value_or(0);
                list.high_hz = json::as_integer(item.find("high")).value_or(0);
                list.step_hz = json::as_integer(item.find("step")).value_or(0);
                list.mode = memory_detail::string_member(item, "mode");
            } else {
                out.skipped.push_back({item.line, list.name, "a scan list of unknown kind \"" + kind + "\""});
                continue;
            }
            if (std::string problem = scan_list_problem(list); !problem.empty()) {
                out.skipped.push_back({item.line, list.name, std::move(problem)});
                continue;
            }
            out.book.scan_lists.push_back(std::move(list));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Bringing the old bookmark list across
// ---------------------------------------------------------------------------

// The bookmark list as ui/models/bookmark_link.cpp stored it: one JSON array
// in the registry value bookmarks/list, each entry {name, hz, demod, low,
// high}, hz written by QJsonDocument from a double.
//
// WITHOUT LOSS means every bookmark the old list showed arrives as a memory
// with the same name, frequency, mode and edges, in the same order. The rule
// for what counts as a bookmark is the old reader's, Bookmark::empty, so an
// entry the old list never showed is not invented here; it is reported
// instead, which the old reader never did. The registry value itself is left
// where it is, so an older client run afterwards still finds its list.
struct Migration {
    std::vector<Memory> memories;
    std::vector<SkippedLine> skipped;
};

[[nodiscard]] inline Migration migrate_bookmarks(std::string_view stored)
{
    Migration out;
    if (memory_detail::trimmed(stored).empty()) {
        return out;
    }
    auto root = json::parse(stored);
    if (!root || !root->is_array()) {
        out.skipped.push_back({0, "bookmarks/list", "the stored list is not a JSON array"});
        return out;
    }
    std::size_t position = 0;
    for (const json::Value& item : root->items) {
        ++position;
        const std::string where = "bookmark " + std::to_string(position);
        if (!item.is_object()) {
            out.skipped.push_back({0, where, "not an object"});
            continue;
        }
        Bookmark mark;
        mark.name = memory_detail::string_member(item, "name");
        // QJsonDocument writes a double, so "9.85e+07" is as possible as
        // "98500000"; either is read exactly. A fraction of a hertz cannot
        // come from the old writer, which stored an integer, and is rounded
        // rather than refused because the old reader truncated it.
        mark.freq_hz = json::as_integer(item.find("hz"), true).value_or(0);
        mark.demod = memory_detail::string_member(item, "demod");
        mark.passband_low =
            static_cast<std::int32_t>(json::as_integer(item.find("low"), true).value_or(0));
        mark.passband_high =
            static_cast<std::int32_t>(json::as_integer(item.find("high"), true).value_or(0));
        if (mark.empty()) {
            out.skipped.push_back({0, mark.name.empty() ? where : mark.name,
                                   mark.freq_hz == 0 ? "no frequency, which the old list never showed"
                                                     : "no mode, which the old list never showed"});
            continue;
        }
        Memory memory;
        memory.name = mark.name;
        memory.freq_hz = mark.freq_hz;
        memory.mode = mark.demod;
        memory.passband_low = mark.passband_low;
        memory.passband_high = mark.passband_high;
        out.memories.push_back(std::move(memory));
    }
    return out;
}

}  // namespace revenant::ui
