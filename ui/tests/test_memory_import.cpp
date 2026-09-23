// Importing SDR#, SDR++ and CHIRP memory files, and exporting CHIRP's.
//
// The sample files are in ui/tests/data/memories and were written for these
// cases from the published descriptions and published files that
// models/memory_import.h cites; no program's source was read for them.
//
// EVERY CASE NAMES THE WRONG IMPORTER IT REJECTS, on the rule the rest of
// ui/tests follows. The wrong importers that matter share one property: they
// produce a list that looks complete. A mode guessed instead of refused plays
// noise; a frequency read through a double is a few hertz off on some entries
// and exact on the rest; an entry skipped without a word is a station the
// operator will assume they never had.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "models/memory_import.h"

using revenant::ui::add_memory;
using revenant::ui::apply_import;
using revenant::ui::decode_text;
using revenant::ui::detect_import_format;
using revenant::ui::edges_for_bandwidth;
using revenant::ui::export_chirp_csv;
using revenant::ui::export_revenant_json;
using revenant::ui::import_chirp;
using revenant::ui::import_memories;
using revenant::ui::import_revenant;
using revenant::ui::import_sdrpp;
using revenant::ui::import_sdrsharp;
using revenant::ui::ImportFormat;
using revenant::ui::ImportResult;
using revenant::ui::map_chirp_mode;
using revenant::ui::map_sdrpp_mode;
using revenant::ui::map_sdrsharp_mode;
using revenant::ui::Memory;
using revenant::ui::MemoryBook;
using revenant::ui::plan_import;
using revenant::ui::read_csv;
using revenant::ui::read_xml;
using revenant::ui::ScanList;
using revenant::ui::SkippedLine;

namespace {

constexpr std::int64_t kNow = 1'790'197'451;

[[nodiscard]] std::string read_file(const std::string& name)
{
    std::ifstream file(std::string(REVENANT_UI_TEST_DATA_DIR) + "/memories/" + name,
                       std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

[[nodiscard]] const Memory* named(const ImportResult& result, std::string_view name)
{
    for (const Memory& memory : result.entries) {
        if (memory.name == name) {
            return &memory;
        }
    }
    return nullptr;
}

[[nodiscard]] const SkippedLine* skipped(const ImportResult& result, std::string_view what)
{
    for (const SkippedLine& line : result.skipped) {
        if (line.what == what) {
            return &line;
        }
    }
    return nullptr;
}

}  // namespace

// Rejects a mode table that maps by position, or maps an unknown name to a
// default instead of refusing it.
TEST_CASE("each format's modes map onto Revenant's or are refused with a reason", "[import]")
{
    CHECK(map_sdrsharp_mode("WFM").mode == "wfm");
    CHECK(map_sdrsharp_mode("CW").mode == "cw");
    CHECK(map_sdrsharp_mode("RAW").mode == "raw");
    CHECK(map_sdrsharp_mode("DRM").mode.empty());
    CHECK(map_sdrsharp_mode("DRM").refusal.find("DRM") != std::string::npos);

    CHECK(map_sdrpp_mode(0).mode == "nfm");
    CHECK(map_sdrpp_mode(1).mode == "wfm");
    CHECK(map_sdrpp_mode(2).mode == "am");
    CHECK(map_sdrpp_mode(3).mode == "dsb");
    CHECK(map_sdrpp_mode(4).mode == "usb");
    CHECK(map_sdrpp_mode(5).mode == "cw");
    CHECK(map_sdrpp_mode(6).mode == "lsb");
    CHECK(map_sdrpp_mode(7).mode == "raw");
    CHECK(map_sdrpp_mode(8).mode.empty());
    CHECK(map_sdrpp_mode(-1).mode.empty());

    CHECK(map_chirp_mode("FM").mode == "nfm");
    CHECK(map_chirp_mode("NFM").mode == "nfm");
    CHECK(map_chirp_mode("NAM").mode == "am");
    CHECK(map_chirp_mode("DV").mode == "dstar");
    CHECK(map_chirp_mode("P25").mode == "p25p1");
    CHECK(map_chirp_mode("").mode == "nfm");
    CHECK(map_chirp_mode("DMR").refusal == "DMR is not a mode the engine demodulates");
    CHECK(map_chirp_mode("RTTY").mode.empty());
}

// Rejects edges centred on a sideband's carrier, which would put half of a USB
// filter below the carrier where there is no signal.
TEST_CASE("a saved bandwidth becomes edges the way the mode uses them", "[import]")
{
    CHECK(edges_for_bandwidth("usb", 2'700) == std::pair<std::int32_t, std::int32_t>{0, 2'700});
    CHECK(edges_for_bandwidth("lsb", 2'400) == std::pair<std::int32_t, std::int32_t>{-2'400, 0});
    CHECK(edges_for_bandwidth("am", 8'333) == std::pair<std::int32_t, std::int32_t>{-4'166, 4'167});
    CHECK(edges_for_bandwidth("wfm", 0) == std::pair<std::int32_t, std::int32_t>{0, 0});
}

// THE SDR# CASE. Rejects a reader that stops at an element it does not know
// (IsScanned and CenterFrequency, from later versions), leaves &amp; in a
// name, applies a converter shift it cannot know the sense of, or drops the
// DRM entry and the one with no frequency without saying so.
TEST_CASE("an SDR# frequencies.xml is read entry by entry", "[import]")
{
    const auto result = import_memories(read_file("sdrsharp_frequencies.xml"), "frequencies.xml");
    REQUIRE(result.has_value());
    CHECK(result->format == ImportFormat::SdrSharp);
    CHECK(result->entries.size() == 5);

    const Memory* fm = named(*result, "Local FM");
    REQUIRE(fm != nullptr);
    CHECK(fm->freq_hz == 98'500'000);
    CHECK(fm->mode == "wfm");
    CHECK(fm->group == "Broadcast");
    CHECK(fm->tags == std::vector<std::string>{"favourite"});
    CHECK(fm->passband_low == -90'000);
    CHECK(fm->passband_high == 90'000);

    const Memory* tower = named(*result, "Tower & Ground");
    REQUIRE(tower != nullptr);
    CHECK(tower->mode == "am");
    CHECK(tower->tags.empty());

    const Memory* ft8 = named(*result, "20 m FT8");
    REQUIRE(ft8 != nullptr);
    CHECK(ft8->group.empty());
    CHECK(ft8->passband_low == 0);
    CHECK(ft8->passband_high == 2'700);

    const Memory* net = named(*result, "40 m net");
    REQUIRE(net != nullptr);
    CHECK(net->freq_hz == 7'150'000);
    CHECK(net->notes == "SDR# shift -125000000 Hz");
    CHECK(net->passband_low == -2'400);

    REQUIRE(result->skipped.size() == 2);
    const SkippedLine* drm = skipped(*result, "Digital broadcast");
    REQUIRE(drm != nullptr);
    CHECK(drm->reason == "SDR# mode \"DRM\" has no Revenant equivalent");
    CHECK(drm->line == 43);
    const SkippedLine* none = skipped(*result, "No frequency");
    REQUIRE(none != nullptr);
    CHECK(none->reason == "no Frequency");
}

// Rejects an XML reader that expands a document's own entities, which is the
// "billion laughs" file that turns a few hundred bytes into gigabytes.
TEST_CASE("the XML reader never expands a declared entity", "[import]")
{
    const std::string hostile =
        "<?xml version=\"1.0\"?>\n<!DOCTYPE lolz [<!ENTITY lol \"lol\"><!ENTITY lol2 "
        "\"&lol;&lol;&lol;&lol;\">]>\n<ArrayOfMemoryEntry><MemoryEntry><Name>&lol2;</Name>"
        "<Frequency>1000000</Frequency><DetectorType>AM</DetectorType></MemoryEntry>"
        "</ArrayOfMemoryEntry>";
    const auto result = import_sdrsharp(hostile);
    REQUIRE(result.has_value());
    REQUIRE(result->entries.size() == 1);
    CHECK(result->entries[0].name == "&lol2;");

    CHECK_FALSE(read_xml("<a><b></a>").has_value());
    CHECK_FALSE(read_xml("<a>").has_value());
    CHECK(read_xml("<a><![CDATA[x < y]]>&#65;&#x42;</a>")->text == "x < yAB");
    CHECK_FALSE(import_sdrsharp("<Other/>").has_value());
}

// THE SDR++ CASE. Rejects a reader that takes the frequency through a double,
// refuses SDR++'s fractional bandwidths, loses the list a bookmark was in, or
// keeps the trailing space SDR++'s guide suggests for telling two names apart.
TEST_CASE("an SDR++ frequency manager config is read list by list", "[import]")
{
    const auto result = import_memories(read_file("sdrpp_frequency_manager_config.json"),
                                        "frequency_manager_config.json");
    REQUIRE(result.has_value());
    CHECK(result->format == ImportFormat::SdrPlusPlus);
    CHECK(result->entries.size() == 9);

    const Memory* atis = named(*result, "ATIS");
    REQUIRE(atis != nullptr);
    CHECK(atis->group == "Air Band");
    CHECK(atis->mode == "am");
    CHECK(atis->freq_hz == 134'500'000);

    const Memory* community = named(*result, "Community radio");
    REQUIRE(community != nullptr);
    CHECK(community->mode == "wfm");
    CHECK(community->passband_low == -112'149);
    CHECK(community->passband_high == 112'149);

    CHECK(named(*result, "Time signal")->mode == "dsb");
    CHECK(named(*result, "20 m SSB")->mode == "usb");
    CHECK(named(*result, "Beacon")->mode == "cw");
    CHECK(named(*result, "80 m SSB")->mode == "lsb");
    CHECK(named(*result, "80 m SSB")->passband_low == -2'800);
    CHECK(named(*result, "IQ")->mode == "raw");
    CHECK(named(*result, "PMR 01")->freq_hz == 446'006'250);
    CHECK(named(*result, "PMR 01")->mode == "nfm");

    REQUIRE(result->skipped.size() == 2);
    const SkippedLine* unknown = skipped(*result, "HF: Unknown mode");
    REQUIRE(unknown != nullptr);
    CHECK(unknown->reason == "SDR++ mode number 9 is not one SDR++ documents");
    CHECK(unknown->line == 56);
    CHECK(skipped(*result, "HF: No frequency")->reason == "no frequency");

    const auto exported = import_memories(read_file("sdrpp_export.json"), "pmr.json");
    REQUIRE(exported.has_value());
    CHECK(exported->entries.size() == 2);
    CHECK(exported->entries[1].freq_hz == 446'018'750);
    CHECK(exported->entries[1].group.empty());

    CHECK_FALSE(import_sdrpp(R"({"selectedList": "x"})").has_value());
}

// THE CHIRP CASE. Rejects a reader that finds columns by position (the two
// headers CHIRP writes differ), reads megahertz through a double, splits a
// quoted name at its comma, maps DMR, System Fusion or CW-reverse to
// something that will not decode them, or forgets the tones an operator
// needs to program a radio from this list later.
TEST_CASE("a CHIRP CSV export is read row by row, tones kept in the note", "[import]")
{
    const auto result = import_memories(read_file("chirp_export.csv"), "radio.csv");
    REQUIRE(result.has_value());
    CHECK(result->format == ImportFormat::Chirp);
    CHECK(result->entries.size() == 9);

    const Memory* call = named(*result, "CALL");
    REQUIRE(call != nullptr);
    CHECK(call->freq_hz == 146'520'000);
    CHECK(call->mode == "nfm");
    CHECK(call->notes == "National simplex\nCHIRP: location 0");

    CHECK(named(*result, "RPT1")->notes ==
          "CHIRP: duplex -0.600000 MHz; tone 100.0 Hz; location 1");
    CHECK(named(*result, "RPT2")->notes ==
          "CHIRP: duplex +5.000000 MHz; tone squelch 127.3 Hz; scan: skip; location 2");
    CHECK(named(*result, "DCS RPT")->notes ==
          "CHIRP: duplex +5.000000 MHz; DCS 125 NR; location 3");
    CHECK(named(*result, "SPLIT")->notes ==
          "Satellite uplink\nCHIRP: transmit on 437.800000 MHz; location 4");
    CHECK(named(*result, "DSTAR")->mode == "dstar");
    CHECK(named(*result, "DSTAR")->notes.find("D-STAR UR CQCQCQ, RPT1 W1ABC  B") !=
          std::string::npos);
    CHECK(named(*result, "Guard")->mode == "am");
    CHECK(named(*result, "Guard")->notes.starts_with("Distress, emergency\n"));
    CHECK(named(*result, "Wx, local")->notes.starts_with("He said \"listen\"\n"));
    CHECK(named(*result, "CROSS")->notes.find("cross Tone->DTCS") != std::string::npos);

    REQUIRE(result->skipped.size() == 4);
    CHECK(skipped(*result, "DMR TG")->reason == "DMR is not a mode the engine demodulates");
    CHECK(skipped(*result, "DMR TG")->line == 8);
    CHECK(skipped(*result, "C4FM")->reason.starts_with("DN"));
    CHECK(skipped(*result, "EMPTY")->reason == "no Frequency");
    CHECK(skipped(*result, "CWREV")->reason.find("reversed sideband") != std::string::npos);
}

// Rejects a reader that needs the long header, and one that breaks on the
// CRLF a Windows CHIRP writes.
TEST_CASE("CHIRP's shorter header and CRLF line ends read the same", "[import]")
{
    const auto short_header = import_chirp(read_file("chirp_short_header.csv"));
    REQUIRE(short_header.has_value());
    REQUIRE(short_header->entries.size() == 2);
    CHECK(short_header->entries[0].freq_hz == 462'562'500);
    CHECK(short_header->entries[0].name == "FRS 1");

    std::string crlf;
    for (const char c : read_file("chirp_short_header.csv")) {
        if (c == '\n') {
            crlf += '\r';
        }
        crlf += c;
    }
    const auto windows = import_chirp(crlf);
    REQUIRE(windows.has_value());
    CHECK(windows->entries.size() == 2);
    CHECK(windows->entries[1].name == "FRS 2");

    CHECK_FALSE(import_chirp("Name,Mode\nx,FM\n").has_value());

    const auto records = read_csv("a,\"b,\nc\",d\n\n1,2,3");
    REQUIRE(records.size() == 2);
    CHECK(records[0].fields[1] == "b,\nc");
    CHECK(records[1].line == 4);
}

// Rejects an importer that reads a file saved as "Unicode" from a Windows
// dialog as a string of NULs, or refuses Latin-1 from an older CHIRP.
TEST_CASE("text arrives as UTF-8 whatever it was saved as", "[import]")
{
    const std::string utf16le("\xFF\xFE" "A\0\xE9\0", 6);
    CHECK(decode_text(utf16le) == "A\xC3\xA9");
    const std::string utf16be("\xFE\xFF\0A\0\xE9", 6);
    CHECK(decode_text(utf16be) == "A\xC3\xA9");
    CHECK(decode_text("caf\xE9") == "caf\xC3\xA9");
    CHECK(decode_text("\xEF\xBB\xBFok") == "ok");
    CHECK(decode_text("caf\xC3\xA9") == "caf\xC3\xA9");
}

// Rejects detection by file name alone, which misreads a renamed file.
TEST_CASE("the format is told from the contents first", "[import]")
{
    CHECK(detect_import_format("<?xml?><ArrayOfMemoryEntry/>", "list.csv") == ImportFormat::SdrSharp);
    CHECK(detect_import_format("{\"lists\": {}}", "x.json") == ImportFormat::SdrPlusPlus);
    CHECK(detect_import_format("{\"format\": \"revenant-memories\"}", "x") == ImportFormat::Revenant);
    CHECK(detect_import_format("Location,Name,Frequency\n", "x.txt") == ImportFormat::Chirp);
    CHECK_FALSE(detect_import_format("hello", "notes.txt").has_value());
}

// THE PREVIEW. Rejects an import that adds what the book already holds, or
// the same entry twice from one file, and one that does not say so.
TEST_CASE("an import is previewed and de-duplicated before it is applied", "[import]")
{
    MemoryBook book;
    Memory existing;
    existing.name = "call";
    existing.freq_hz = 146'520'000;
    existing.mode = "nfm";
    add_memory(book, existing, kNow);

    auto result = import_chirp(read_file("chirp_export.csv"));
    REQUIRE(result.has_value());
    result->entries.push_back(result->entries[1]);

    const auto plan = plan_import(book, *result);
    CHECK(plan.to_add.size() == result->entries.size() - 2);
    REQUIRE(plan.duplicates.size() == 2);
    CHECK(plan.duplicates[0].what == "CALL");
    CHECK(plan.duplicates[0].reason == "already in the list at 146.520000 MHz");
    CHECK(plan.duplicates[1].reason == "appears twice in the file");

    // Nothing changed until it is applied.
    CHECK(book.memories.size() == 1);
    CHECK(apply_import(book, plan, kNow) == plan.to_add.size());
    CHECK(book.memories.size() == 1 + plan.to_add.size());
    CHECK(book.memories.back().created == kNow);
    CHECK(book.memories.back().id == book.memories.size());
}

// Rejects an export that writes megahertz through a double, a mode CHIRP
// cannot load, or a note whose line break splits the row; and one that
// cannot be read back by the importer beside it.
TEST_CASE("an export to CHIRP reads back as the memories it came from", "[import]")
{
    std::vector<Memory> memories;
    const auto add = [&](std::string name, std::int64_t hz, std::string mode, std::string notes) {
        Memory memory;
        memory.name = std::move(name);
        memory.freq_hz = hz;
        memory.mode = std::move(mode);
        memory.notes = std::move(notes);
        memories.push_back(memory);
    };
    add("PMR 01", 446'006'250, "nfm", "line one\nline, two");
    add("Airband", 118'300'000, "am", "");
    add("Quote \"q\"", 14'074'000, "usb", "");
    add("Composite", 98'500'000, "raw", "");
    add("TETRA", 390'012'500, "tetra", "");

    const auto exported = export_chirp_csv(memories);
    REQUIRE(exported.skipped.size() == 2);
    CHECK(exported.skipped[0].reason == "CHIRP has no mode for raw");
    CHECK(exported.text.starts_with("Location,Name,Frequency,Duplex,Offset,Tone,"));
    CHECK(exported.text.find("1,PMR 01,446.006250,,0.000000,") != std::string::npos);

    const auto back = import_chirp(exported.text);
    REQUIRE(back.has_value());
    CHECK(back->skipped.empty());
    REQUIRE(back->entries.size() == 3);
    CHECK(back->entries[0].freq_hz == 446'006'250);
    CHECK(back->entries[0].mode == "nfm");
    CHECK(back->entries[0].notes.starts_with("line one; line, two\n"));
    CHECK(back->entries[2].name == "Quote \"q\"");
    CHECK(back->entries[2].mode == "usb");
}

// Rejects a Revenant export that carries scan list members it did not export,
// which on import elsewhere would name memories that are not there.
TEST_CASE("an export to Revenant's format carries only what it exports", "[import]")
{
    MemoryBook book;
    Memory a;
    a.name = "a";
    a.freq_hz = 1'000'000;
    a.mode = "am";
    Memory b = a;
    b.name = "b";
    b.freq_hz = 2'000'000;
    const auto id_a = add_memory(book, a, kNow);
    const auto id_b = add_memory(book, b, kNow);
    ScanList both;
    both.name = "both";
    both.members = {id_a, id_b};
    ScanList only_b;
    only_b.name = "only b";
    only_b.members = {id_b};
    book.scan_lists = {both, only_b};

    const std::string text = export_revenant_json(book, {0});
    const auto loaded = revenant::ui::memory_book_from_json(text);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->book.memories.size() == 1);
    REQUIRE(loaded->book.scan_lists.size() == 1);
    CHECK(loaded->book.scan_lists[0].members == std::vector<std::uint64_t>{id_a});

    const auto imported = import_revenant(text);
    REQUIRE(imported.has_value());
    REQUIRE(imported->entries.size() == 1);
    CHECK(imported->entries[0].id == 0);
    CHECK(imported->entries[0].name == "a");
}
