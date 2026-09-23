// The frequency manager's model: memories, their file, the bookmarks they grew
// out of, search and sort, delete and undo, and scan lists.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows. Most of the wrong ones here lose something quietly: a
// migration that drops the one bookmark with an odd field, an undo that puts
// a memory back outside the scan list it was in, a file reader that rewrites
// a newer file with half its fields. None of them crashes, and each is found
// by an operator months later as a station that is simply not there.
//
// WHAT IS NOT HERE. Where the file is on disk and the panel that draws the
// list: both are Qt, in ui/models/frequency_manager.cpp and
// ui/qml/FrequencyManager.qml.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "models/memories.h"

using revenant::ui::add_memory;
using revenant::ui::delete_memory;
using revenant::ui::duplicate_of;
using revenant::ui::format_mhz;
using revenant::ui::format_utc;
using revenant::ui::join_tags;
using revenant::ui::list_memories;
using revenant::ui::Memory;
using revenant::ui::memory_as_bookmark;
using revenant::ui::memory_book_from_json;
using revenant::ui::memory_book_to_json;
using revenant::ui::memory_label;
using revenant::ui::memory_matches;
using revenant::ui::memory_near;
using revenant::ui::MemoryBook;
using revenant::ui::MemorySort;
using revenant::ui::migrate_bookmarks;
using revenant::ui::normalise_tags;
using revenant::ui::parse_utc;
using revenant::ui::restore_memory;
using revenant::ui::scan_list_frequencies;
using revenant::ui::scan_list_problem;
using revenant::ui::scan_list_size;
using revenant::ui::ScanList;
using revenant::ui::split_tags;
using revenant::ui::tag_counts;

namespace {

// 2026-09-23T21:04:11Z, the time every case here pretends it is.
constexpr std::int64_t kNow = 1'790'197'451;

[[nodiscard]] std::string read_file(const std::string& name)
{
    std::ifstream file(std::string(REVENANT_UI_TEST_DATA_DIR) + "/memories/" + name,
                       std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

[[nodiscard]] Memory make(std::string name, std::int64_t hz, std::string mode,
                          std::vector<std::string> tags = {})
{
    Memory memory;
    memory.name = std::move(name);
    memory.freq_hz = hz;
    memory.mode = std::move(mode);
    memory.tags = std::move(tags);
    return memory;
}

[[nodiscard]] std::vector<std::string> names(const MemoryBook& book,
                                             const std::vector<std::size_t>& order)
{
    std::vector<std::string> out;
    for (const std::size_t i : order) {
        out.push_back(book.memories[i].name);
    }
    return out;
}

}  // namespace

// Rejects a label built from a double, which prints 146.52 as 146.519999 on
// some inputs and makes a frequency search miss the station it names.
TEST_CASE("frequencies are written to the hertz in megahertz", "[memories]")
{
    CHECK(format_mhz(146'520'000) == "146.520000");
    CHECK(format_mhz(14'074'000) == "14.074000");
    CHECK(format_mhz(446'006'250) == "446.006250");
    CHECK(format_mhz(999) == "0.000999");
    CHECK(format_mhz(-1'500'000) == "-1.500000");
}

// Rejects a time format that drifts through a local time zone or reads any
// string at all as a time.
TEST_CASE("times are UTC to the second and read back exactly", "[memories]")
{
    CHECK(format_utc(kNow) == "2026-09-23T21:04:11Z");
    CHECK(parse_utc("2026-09-23T21:04:11Z") == kNow);
    CHECK(parse_utc(format_utc(0)) == 0);
    CHECK(parse_utc(format_utc(4'102'444'800)) == 4'102'444'800);
    CHECK_FALSE(parse_utc("2026-02-30T00:00:00Z").has_value());
    CHECK_FALSE(parse_utc("2026-09-23 21:04:11").has_value());
    CHECK_FALSE(parse_utc("").has_value());
}

// Rejects tags compared as typed, which lets "Air" and "air" become two chips
// that each filter to half the list.
TEST_CASE("tags are a set without case, kept in their first spelling", "[memories]")
{
    CHECK(split_tags(" Air, tower ,air,, local  net ") ==
          std::vector<std::string>{"Air", "tower", "local net"});
    CHECK(normalise_tags({"a,b", "  ", "A"}) == std::vector<std::string>{"ab", "A"});
    CHECK(join_tags({"air", "tower"}) == "air, tower");
}

// Rejects a de-duplication by frequency alone, which would merge two named
// services sharing a channel, and one by exact name, which lets a re-import
// with different capitals double the list.
TEST_CASE("a duplicate is the same frequency under the same name", "[memories]")
{
    MemoryBook book;
    add_memory(book, make("Tower", 118'300'000, "am"), kNow);
    CHECK(duplicate_of(book, 118'300'000, "  tower ") != nullptr);
    CHECK(duplicate_of(book, 118'300'000, "Ground") == nullptr);
    CHECK(duplicate_of(book, 118'325'000, "Tower") == nullptr);

    // Near a memory is a different question, asked with a receiver's width.
    CHECK(memory_near(book, 118'302'000, 5'000) != nullptr);
    CHECK(memory_near(book, 118'310'000, 5'000) == nullptr);
}

// Rejects ids that restart, or that reuse a deleted memory's, which would make
// a scan list silently name a different station after an undo.
TEST_CASE("ids are handed out once and creation time is stamped", "[memories]")
{
    MemoryBook book;
    const auto a = add_memory(book, make("A", 1'000'000, "am"), kNow);
    const auto b = add_memory(book, make("B", 2'000'000, "am"), kNow);
    CHECK(a == 1);
    CHECK(b == 2);
    CHECK(book.memories[0].created == kNow);
    REQUIRE(delete_memory(book, b).has_value());
    CHECK(add_memory(book, make("C", 3'000'000, "am"), kNow) == 3);
}

// Rejects an undo that restores the memory and not its place: at the end of
// the list instead of where it was, or outside the scan lists that named it.
TEST_CASE("undoing a delete puts the memory back where it was, scan lists included",
          "[memories]")
{
    MemoryBook book;
    const auto a = add_memory(book, make("A", 1'000'000, "am"), kNow);
    const auto b = add_memory(book, make("B", 2'000'000, "am"), kNow);
    const auto c = add_memory(book, make("C", 3'000'000, "am"), kNow);
    ScanList list;
    list.name = "all";
    list.members = {c, b, a, b};
    book.scan_lists.push_back(list);

    auto gone = delete_memory(book, b);
    REQUIRE(gone.has_value());
    CHECK(book.memories.size() == 2);
    CHECK(book.scan_lists[0].members == std::vector<std::uint64_t>{c, a});

    restore_memory(book, std::move(*gone));
    REQUIRE(book.memories.size() == 3);
    CHECK(book.memories[1].name == "B");
    CHECK(book.memories[1].id == b);
    CHECK(book.scan_lists[0].members == std::vector<std::uint64_t>{c, b, a, b});

    CHECK_FALSE(delete_memory(book, 99).has_value());
}

// Rejects a search that only looks at names, and one that needs a frequency
// typed the way it is stored: "146.52" is how a person writes 146520000.
TEST_CASE("search finds a memory by any word in it, frequency included", "[memories]")
{
    Memory memory = make("Repeater", 146'940'000, "nfm", {"club"});
    memory.notes = "Tuesday net";
    memory.group = "Amateur";
    CHECK(memory_matches(memory, ""));
    CHECK(memory_matches(memory, "146.94"));
    CHECK(memory_matches(memory, "146940"));
    CHECK(memory_matches(memory, "CLUB tuesday"));
    CHECK(memory_matches(memory, "amateur nfm"));
    CHECK_FALSE(memory_matches(memory, "club wednesday"));
}

// Rejects a tag filter that widens with each chip, and a sort whose ties come
// out in whatever order the sort left them, which moves rows under the
// pointer every time the list refreshes.
TEST_CASE("the list filters by every chosen tag and sorts with a stable tie-break",
          "[memories]")
{
    MemoryBook book;
    add_memory(book, make("bravo", 146'000'000, "nfm", {"club"}), kNow);
    add_memory(book, make("Alpha", 145'000'000, "nfm", {"club", "local"}), kNow);
    add_memory(book, make("charlie", 7'000'000, "lsb"), kNow);
    add_memory(book, make("alpha", 144'000'000, "am", {"local"}), kNow);

    CHECK(names(book, list_memories(book, "", {}, MemorySort::Name, false)) ==
          std::vector<std::string>{"alpha", "Alpha", "bravo", "charlie"});
    CHECK(names(book, list_memories(book, "", {}, MemorySort::Name, true)) ==
          std::vector<std::string>{"charlie", "bravo", "alpha", "Alpha"});
    CHECK(names(book, list_memories(book, "", {}, MemorySort::Frequency, false)) ==
          std::vector<std::string>{"charlie", "alpha", "Alpha", "bravo"});
    CHECK(names(book, list_memories(book, "", {}, MemorySort::Mode, false)) ==
          std::vector<std::string>{"alpha", "charlie", "Alpha", "bravo"});

    // Untagged last whichever way the tags column runs.
    CHECK(names(book, list_memories(book, "", {}, MemorySort::Tags, false)).back() == "charlie");
    CHECK(names(book, list_memories(book, "", {}, MemorySort::Tags, true)).back() == "charlie");

    CHECK(names(book, list_memories(book, "", {"CLUB"}, MemorySort::Name, false)) ==
          std::vector<std::string>{"Alpha", "bravo"});
    CHECK(names(book, list_memories(book, "", {"club", "local"}, MemorySort::Name, false)) ==
          std::vector<std::string>{"Alpha"});

    const auto counts = tag_counts(book);
    REQUIRE(counts.size() == 2);
    CHECK(counts[0].tag == "club");
    CHECK(counts[0].count == 2);
    CHECK(counts[1].count == 2);
}

// Rejects a "recently used" sort that puts never-used memories first because
// zero is the smallest time.
TEST_CASE("recently used sorts newest first and never used last", "[memories]")
{
    MemoryBook book;
    add_memory(book, make("never", 1'000'000, "am"), kNow);
    add_memory(book, make("old", 2'000'000, "am"), kNow);
    add_memory(book, make("new", 3'000'000, "am"), kNow);
    book.memories[1].used = kNow - 100;
    book.memories[2].used = kNow;
    CHECK(names(book, list_memories(book, "", {}, MemorySort::Used, false)) ==
          std::vector<std::string>{"new", "old", "never"});
    CHECK(names(book, list_memories(book, "", {}, MemorySort::Used, true)) ==
          std::vector<std::string>{"old", "new", "never"});
}

// Rejects a file writer and reader that disagree about any field, which is the
// failure that loses something on every save.
TEST_CASE("a book written to its file reads back field for field", "[memories]")
{
    MemoryBook book;
    Memory memory = make("KXYZ", 98'500'000, "wfm", {"broadcast", "local"});
    memory.passband_low = -100'000;
    memory.passband_high = 100'000;
    memory.notes = "line one\nline \"two\"";
    memory.group = "FM";
    memory.used = kNow + 60;
    const auto id = add_memory(book, memory, kNow);
    ScanList members;
    members.name = "mine";
    members.members = {id};
    ScanList range;
    range.name = "2 m";
    range.kind = ScanList::Kind::Range;
    range.low_hz = 144'000'000;
    range.high_hz = 148'000'000;
    range.step_hz = 12'500;
    range.mode = "nfm";
    book.scan_lists = {members, range};

    const auto loaded = memory_book_from_json(memory_book_to_json(book));
    REQUIRE(loaded.has_value());
    CHECK(loaded->skipped.empty());
    REQUIRE(loaded->book.memories.size() == 1);
    const Memory& back = loaded->book.memories[0];
    CHECK(back.id == id);
    CHECK(back.name == "KXYZ");
    CHECK(back.freq_hz == 98'500'000);
    CHECK(back.mode == "wfm");
    CHECK(back.passband_low == -100'000);
    CHECK(back.passband_high == 100'000);
    CHECK(back.tags == std::vector<std::string>{"broadcast", "local"});
    CHECK(back.notes == "line one\nline \"two\"");
    CHECK(back.group == "FM");
    CHECK(back.created == kNow);
    CHECK(back.used == kNow + 60);
    CHECK(loaded->book.next_id == id + 1);
    REQUIRE(loaded->book.scan_lists.size() == 2);
    CHECK(loaded->book.scan_lists[0].members == std::vector<std::uint64_t>{id});
    CHECK(loaded->book.scan_lists[1].kind == ScanList::Kind::Range);
    CHECK(loaded->book.scan_lists[1].step_hz == 12'500);
}

// Rejects a reader that drops the whole file over one bad entry, or keeps the
// bad entry with a default filled in. The sample carries a memory with no
// mode, one at a fraction of a hertz and a range that runs backwards.
TEST_CASE("the sample file loads with its bad entries reported, not kept", "[memories]")
{
    const auto loaded = memory_book_from_json(read_file("revenant_memories.json"));
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->book.memories.size() == 2);
    CHECK(loaded->book.memories[0].id == 7);
    CHECK(loaded->book.memories[1].name == "Tower");
    CHECK(loaded->book.memories[1].used == 0);
    CHECK(loaded->book.next_id == 10);
    REQUIRE(loaded->skipped.size() == 3);
    CHECK(loaded->skipped[0].what == "No mode");
    CHECK(loaded->skipped[0].reason == "no mode");
    CHECK(loaded->skipped[1].what == "Fractional");
    CHECK(loaded->skipped[1].reason == "hz is not a whole number of hertz");
    CHECK(loaded->skipped[1].line > loaded->skipped[0].line);
    CHECK(loaded->skipped[2].what == "backwards");
    CHECK(loaded->book.scan_lists.size() == 2);
}

// Rejects a reader that rewrites a file a newer client wrote, losing whatever
// the newer schema added, and one that treats any JSON as a memory file.
TEST_CASE("a newer or foreign file is refused whole", "[memories]")
{
    const auto newer = memory_book_from_json(
        R"({"format": "revenant-memories", "version": 2, "memories": []})");
    REQUIRE_FALSE(newer.has_value());
    CHECK(newer.error().message.find("newer Revenant") != std::string::npos);

    CHECK_FALSE(memory_book_from_json(R"({"bookmarks": {}})").has_value());
    CHECK_FALSE(memory_book_from_json("not json").has_value());
}

// Rejects a reader that loses a memory whose id was edited into a duplicate:
// it gets a new id rather than being dropped or overwriting the first.
TEST_CASE("a repeated id is given a new one rather than lost", "[memories]")
{
    const auto loaded = memory_book_from_json(
        R"({"format": "revenant-memories", "version": 1, "memories": [
            {"id": 3, "name": "a", "hz": 1000000, "mode": "am"},
            {"id": 3, "name": "b", "hz": 2000000, "mode": "am"},
            {"name": "c", "hz": 3000000, "mode": "am"}]})");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->book.memories.size() == 3);
    CHECK(loaded->book.memories[0].id == 3);
    CHECK(loaded->book.memories[1].id == 4);
    CHECK(loaded->book.memories[2].id == 5);
    CHECK(loaded->book.next_id == 6);
}

// THE MIGRATION. Rejects one that loses anything the old list showed: a
// field, the order, or a bookmark whose frequency QJsonDocument happened to
// write in exponent form. The string is what bookmark_link.cpp stores in the
// registry value bookmarks/list, byte for byte in shape.
TEST_CASE("every bookmark the old list showed becomes a memory without loss", "[memories]")
{
    const std::string stored =
        R"([{"demod":"wfm","high":80000,"hz":98500000,"low":-80000,"name":"KXYZ"},)"
        R"({"demod":"usb","high":2700,"hz":1.4074e+07,"low":300,"name":""},)"
        R"({"demod":"nfm","high":0,"hz":146520000,"low":0,"name":"Calling"},)"
        R"({"demod":"","high":0,"hz":145500000,"low":0,"name":"no mode"},)"
        R"({"demod":"am","high":0,"hz":0,"low":0,"name":"no frequency"}])";
    const auto migration = migrate_bookmarks(stored);

    REQUIRE(migration.memories.size() == 3);
    CHECK(migration.memories[0].name == "KXYZ");
    CHECK(migration.memories[0].freq_hz == 98'500'000);
    CHECK(migration.memories[0].mode == "wfm");
    CHECK(migration.memories[0].passband_low == -80'000);
    CHECK(migration.memories[0].passband_high == 80'000);
    CHECK(migration.memories[1].freq_hz == 14'074'000);
    CHECK(migration.memories[1].name.empty());
    CHECK(memory_label(migration.memories[1]) == "14.074 MHz");
    CHECK(migration.memories[1].passband_low == 300);
    CHECK(migration.memories[2].name == "Calling");

    // What the old reader dropped without a word is reported now.
    REQUIRE(migration.skipped.size() == 2);
    CHECK(migration.skipped[0].what == "no mode");
    CHECK(migration.skipped[1].what == "no frequency");

    // And a migrated memory recalls exactly as the bookmark did.
    const auto mark = memory_as_bookmark(migration.memories[0]);
    CHECK(mark.freq_hz == 98'500'000);
    CHECK(mark.demod == "wfm");
    CHECK(mark.passband_low == -80'000);

    CHECK(migrate_bookmarks("").memories.empty());
    CHECK(migrate_bookmarks("").skipped.empty());
    CHECK(migrate_bookmarks("{}").skipped.size() == 1);
}

// Rejects a range that counts its channels off by one, accepts a step of zero
// (a scan that never moves), or accepts a typo that makes millions of them.
TEST_CASE("a scan list is sized and checked before anything scans it", "[memories]")
{
    MemoryBook book;
    const auto a = add_memory(book, make("A", 1'000'000, "am"), kNow);
    ScanList list;
    list.name = "range";
    list.kind = ScanList::Kind::Range;
    list.low_hz = 144'000'000;
    list.high_hz = 144'100'000;
    list.step_hz = 25'000;
    list.mode = "nfm";
    CHECK(scan_list_problem(list).empty());
    CHECK(scan_list_size(book, list) == 5);
    CHECK(scan_list_frequencies(book, list) ==
          std::vector<std::int64_t>{144'000'000, 144'025'000, 144'050'000, 144'075'000,
                                    144'100'000});

    list.step_hz = 0;
    CHECK(scan_list_problem(list) == "a range needs a step above zero");
    list.step_hz = 1;
    list.high_hz = 1'000'000'000;
    CHECK_FALSE(scan_list_problem(list).empty());
    list.step_hz = 12'500;
    list.mode = "fm";
    CHECK_FALSE(scan_list_problem(list).empty());

    ScanList members;
    members.name = "mine";
    members.members = {a, 42};
    CHECK(scan_list_size(book, members) == 1);
    CHECK(scan_list_frequencies(book, members) == std::vector<std::int64_t>{1'000'000});
    members.name.clear();
    CHECK_FALSE(scan_list_problem(members).empty());
}
