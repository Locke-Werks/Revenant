// Reading other programs' memory files, and writing CHIRP's.
//
// "Refusing to read existing memory files is a self-inflicted adoption
// barrier": an operator arriving from SDR#, SDR++ or a handheld programmed
// with CHIRP has years of frequencies already, and a client that makes them
// retype those has lost them before the first recall. So each format is a
// Qt-free function from the file's text to memories plus a list of what was
// passed over and why, with cases in ui/tests/test_memory_import.cpp against
// sample files in ui/tests/data/memories.
//
// NOTHING IS DROPPED SILENTLY. Every entry that does not become a memory is a
// SkippedLine naming where it was and why, and the panel shows the list
// before anything is added. A mode with no Revenant equivalent is the usual
// reason, and it is refused rather than guessed: a DMR channel imported as
// nfm is a memory that plays noise and looks like it should work.
//
// WHAT EACH READER WAS WRITTEN FROM. Published descriptions and files other
// people published, never another program's source.
//
// SDR#, frequencies.xml. The file is .NET XML serialisation of a list of
// MemoryEntry elements under ArrayOfMemoryEntry. The element names and value
// shapes come from published files, chiefly Frequencies.xml in
// github.com/z1000biker/sdrsharpswldatabase (retrieved 2026-09-23), which
// carries IsScanned, IsFavourite, Name, GroupName, Frequency, DetectorType,
// Shift, FilterBandwidth and CenterFrequency per entry, and the file shared in
// the RadioReference thread "SDR# - Frequency names don't display on
// spectrum" (forums.radioreference.com, thread 391056). Frequency, Shift and
// FilterBandwidth are whole hertz. DetectorType is the demodulator's name as
// SDR#'s own mode buttons label them: NFM, WFM, AM, DSB, LSB, USB, CW, RAW.
// Elements this reader does not know are ignored, which is how IsScanned and
// CenterFrequency, added by later SDR# versions, are handled.
//
// SDR++, frequency_manager_config.json. The SDR++ user guide ("SDR++ USER
// GUIDE for SDR++ up to version 1.1", December 2022, sdrpp.org/manual.pdf,
// "Frequency Manager") describes lists holding bookmarks with a name,
// frequency, bandwidth and mode, and an export of selected bookmarks to a
// .json file. The shapes are from published files: the whole config
// ({"lists": {NAME: {"bookmarks": {NAME: {...}}}}}) in
// github.com/TuxedoHam/sdrpp, and the export ({"bookmarks": {...}}) in
// github.com/jcalado/sdrpp-bookmarks, both retrieved 2026-09-23. frequency and
// bandwidth are hertz written as doubles, "224298.0625" among them. mode is a
// number. 0, 1 and 2 are NFM, WFM and AM by those files: PMR446 channels are
// 0 at 12.5 kHz, broadcast stations 1, airband 2. 3 to 7 are the rest of the
// radio module's mode buttons in the order its menu lays them out, two to a
// row: DSB, USB, CW, LSB, RAW. That second half is the weaker evidence and is
// said so here and in docs/ui-spectrum.md, so a mismatch found later has a
// place to be corrected.
//
// CHIRP, the CSV export. Column meanings are CHIRP's wiki page "Memory Editor
// Columns" (chirpmyradio.com/projects/chirp/wiki/MemoryEditorColumns,
// retrieved 2026-09-23): Frequency in MHz; Duplex one of empty, +, -, split
// or off; Offset in MHz, or the transmit frequency under split; the tone
// modes Tone, TSQL, DTCS and Cross; Mode among FM (5 kHz deviation), NFM
// (2.5 kHz), WFM, AM, DV (D-STAR) and DN (System Fusion). The header row and
// the value formats are from CHIRP's own stock configuration CSVs, "US
// Aviation Frequencies.csv" and "US FRS and GMRS Channels.csv" in the CHIRP
// repository's stock_configs directory, which differ in which columns they
// carry. So columns are found by their header and never by position, and only
// Frequency is required.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "core/error.h"
#include "models/json_lite.h"
#include "models/memories.h"

namespace revenant::ui {

enum class ImportFormat : std::uint8_t {
    Revenant,
    SdrSharp,
    SdrPlusPlus,
    Chirp,
};

[[nodiscard]] inline std::string_view import_format_name(ImportFormat format)
{
    switch (format) {
        case ImportFormat::Revenant: return "Revenant";
        case ImportFormat::SdrSharp: return "SDR#";
        case ImportFormat::SdrPlusPlus: return "SDR++";
        case ImportFormat::Chirp: return "CHIRP";
    }
    return {};
}

struct ImportResult {
    ImportFormat format = ImportFormat::Revenant;
    std::vector<Memory> entries;
    std::vector<SkippedLine> skipped;
};

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

namespace import_detail {

inline void append_utf8(std::string& out, std::uint32_t code)
{
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

[[nodiscard]] inline bool valid_utf8(std::string_view text)
{
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        if (c < 0x80) {
            extra = 0;
        } else if ((c & 0xE0) == 0xC0 && c >= 0xC2) {
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((c & 0xF8) == 0xF0 && c <= 0xF4) {
            extra = 3;
        } else {
            return false;
        }
        if (i + extra >= text.size() && extra > 0) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

[[nodiscard]] inline std::string trim(std::string_view text)
{
    return memory_detail::trimmed(text);
}

[[nodiscard]] inline bool equal_nocase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && fold_case(a) == fold_case(b);
}

}  // namespace import_detail

// A file's bytes as UTF-8, whatever it was saved as.
//
// A byte order mark decides it when there is one: UTF-8's is dropped, and
// UTF-16 in either byte order is converted, since that is what "Unicode" means
// in a Windows save dialog and a CSV round-tripped through Excel comes back in
// it. Without a mark the bytes are UTF-8 if they read as UTF-8, and otherwise
// Latin-1, which is what an older CHIRP on Windows wrote a station name with
// an accent in.
[[nodiscard]] inline std::string decode_text(std::string_view bytes)
{
    if (bytes.starts_with("\xEF\xBB\xBF")) {
        bytes.remove_prefix(3);
        return std::string(bytes);
    }
    const bool le = bytes.starts_with("\xFF\xFE");
    const bool be = bytes.starts_with("\xFE\xFF");
    if (le || be) {
        std::string out;
        std::size_t i = 2;
        const auto unit = [&](std::size_t at) -> std::uint32_t {
            const auto a = static_cast<unsigned char>(bytes[at]);
            const auto b = static_cast<unsigned char>(bytes[at + 1]);
            return le ? static_cast<std::uint32_t>(a | (b << 8))
                      : static_cast<std::uint32_t>((a << 8) | b);
        };
        while (i + 1 < bytes.size()) {
            std::uint32_t code = unit(i);
            i += 2;
            if (code >= 0xD800 && code <= 0xDBFF && i + 1 < bytes.size()) {
                const std::uint32_t low = unit(i);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    i += 2;
                } else {
                    code = 0xFFFD;
                }
            } else if (code >= 0xD800 && code <= 0xDFFF) {
                code = 0xFFFD;
            }
            import_detail::append_utf8(out, code);
        }
        return out;
    }
    if (import_detail::valid_utf8(bytes)) {
        return std::string(bytes);
    }
    std::string out;
    for (const char c : bytes) {
        import_detail::append_utf8(out, static_cast<unsigned char>(c));
    }
    return out;
}

// ---------------------------------------------------------------------------
// CSV
// ---------------------------------------------------------------------------

struct CsvRecord {
    // The line the record starts on, counted from one.
    std::size_t line = 0;
    std::vector<std::string> fields;
};

// RFC 4180 with the tolerance real files need: CRLF or LF, a quoted field may
// hold commas, quotes written twice and line breaks, and a blank line is not
// a record. Never fails; a quote left open runs to the end of the file, which
// the reader downstream then reports as one malformed record.
[[nodiscard]] inline std::vector<CsvRecord> read_csv(std::string_view text)
{
    std::vector<CsvRecord> out;
    CsvRecord record;
    std::string field;
    bool quoted = false;
    bool field_started = false;
    std::size_t line = 1;
    record.line = 1;

    const auto end_field = [&] {
        record.fields.push_back(std::move(field));
        field.clear();
        field_started = false;
    };
    const auto end_record = [&] {
        end_field();
        const bool blank = record.fields.size() == 1 && record.fields[0].empty();
        if (!blank) {
            out.push_back(std::move(record));
        }
        record = CsvRecord{};
        record.line = line;
    };

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                if (c == '\n') {
                    ++line;
                }
                field.push_back(c);
            }
            continue;
        }
        if (c == '"' && !field_started) {
            quoted = true;
            field_started = true;
            continue;
        }
        if (c == ',') {
            end_field();
            continue;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            ++line;
            end_record();
            continue;
        }
        field.push_back(c);
        field_started = true;
    }
    if (field_started || !field.empty() || !record.fields.empty()) {
        end_record();
    }
    return out;
}

// One field for a CSV row, quoted when it has to be.
[[nodiscard]] inline std::string csv_field(std::string_view text)
{
    if (text.find_first_of(",\"\r\n") == std::string_view::npos) {
        return std::string(text);
    }
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"') {
            out += "\"\"";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

// ---------------------------------------------------------------------------
// XML, as much of it as frequencies.xml uses
// ---------------------------------------------------------------------------

struct XmlElement {
    std::string name;
    std::string text;
    std::size_t line = 0;
    std::vector<XmlElement> children;

    [[nodiscard]] const XmlElement* child(std::string_view wanted) const
    {
        for (const XmlElement& one : children) {
            if (one.name == wanted) {
                return &one;
            }
        }
        return nullptr;
    }
};

namespace import_detail {

// Elements and their text, attributes read past and ignored, the five
// predefined entities and numeric ones decoded, comments, processing
// instructions, CDATA and a DOCTYPE skipped. No entity a document declares
// for itself is ever expanded, so a file built to expand into gigabytes reads
// as its literal text. Depth is bounded for the reason json_lite.h gives.
class XmlReader {
public:
    explicit XmlReader(std::string_view text) : text_(text) {}

    Expected<XmlElement> document()
    {
        skip_misc();
        if (at_ >= text_.size() || text_[at_] != '<') {
            return failure("no root element");
        }
        auto root = element(0);
        if (!root) {
            return root;
        }
        skip_misc();
        if (at_ < text_.size()) {
            return failure("text after the root element");
        }
        return root;
    }

private:
    std::string_view text_;
    std::size_t at_ = 0;
    std::size_t line_ = 1;

    [[nodiscard]] std::unexpected<Error> failure(std::string_view what) const
    {
        return fail("line " + std::to_string(line_) + ": " + std::string(what));
    }

    void advance(std::size_t count)
    {
        for (std::size_t i = 0; i < count && at_ < text_.size(); ++i) {
            if (text_[at_] == '\n') {
                ++line_;
            }
            ++at_;
        }
    }

    [[nodiscard]] bool at(std::string_view word) const { return text_.substr(at_).starts_with(word); }

    bool skip_past(std::string_view end)
    {
        const std::size_t found = text_.find(end, at_);
        if (found == std::string_view::npos) {
            return false;
        }
        advance(found + end.size() - at_);
        return true;
    }

    void skip_space()
    {
        while (at_ < text_.size() &&
               (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\r' || text_[at_] == '\n')) {
            advance(1);
        }
    }

    // Whitespace, comments, processing instructions and a DOCTYPE, which is
    // everything that may sit around the root element.
    void skip_misc()
    {
        while (true) {
            skip_space();
            if (at("<?")) {
                if (!skip_past("?>")) {
                    at_ = text_.size();
                }
            } else if (at("<!--")) {
                if (!skip_past("-->")) {
                    at_ = text_.size();
                }
            } else if (at("<!DOCTYPE")) {
                // An internal subset is in brackets and may hold '>'.
                int depth = 0;
                while (at_ < text_.size()) {
                    const char c = text_[at_];
                    advance(1);
                    if (c == '[') {
                        ++depth;
                    } else if (c == ']') {
                        --depth;
                    } else if (c == '>' && depth <= 0) {
                        break;
                    }
                }
            } else {
                return;
            }
        }
    }

    static void decode_entities(std::string_view raw, std::string& out)
    {
        std::size_t i = 0;
        while (i < raw.size()) {
            if (raw[i] != '&') {
                out.push_back(raw[i++]);
                continue;
            }
            const std::size_t semi = raw.find(';', i);
            if (semi == std::string_view::npos || semi - i > 10) {
                out.push_back(raw[i++]);
                continue;
            }
            const std::string_view name = raw.substr(i + 1, semi - i - 1);
            if (name == "amp") {
                out.push_back('&');
            } else if (name == "lt") {
                out.push_back('<');
            } else if (name == "gt") {
                out.push_back('>');
            } else if (name == "quot") {
                out.push_back('"');
            } else if (name == "apos") {
                out.push_back('\'');
            } else if (name.starts_with("#")) {
                const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
                std::uint32_t code = 0;
                bool ok = name.size() > (hex ? 2U : 1U);
                for (std::size_t k = hex ? 2 : 1; ok && k < name.size(); ++k) {
                    const char d = name[k];
                    std::uint32_t digit = 0;
                    if (d >= '0' && d <= '9') {
                        digit = static_cast<std::uint32_t>(d - '0');
                    } else if (hex && d >= 'a' && d <= 'f') {
                        digit = static_cast<std::uint32_t>(d - 'a' + 10);
                    } else if (hex && d >= 'A' && d <= 'F') {
                        digit = static_cast<std::uint32_t>(d - 'A' + 10);
                    } else {
                        ok = false;
                        break;
                    }
                    code = code * (hex ? 16U : 10U) + digit;
                    if (code > 0x10FFFF) {
                        ok = false;
                    }
                }
                if (!ok) {
                    out.append(raw.substr(i, semi - i + 1));
                } else {
                    append_utf8(out, code);
                }
            } else {
                // An entity the document declared, or a typo: kept literally.
                out.append(raw.substr(i, semi - i + 1));
            }
            i = semi + 1;
        }
    }

    Expected<XmlElement> element(int depth)
    {
        if (depth > 64) {
            return failure("nested too deeply");
        }
        XmlElement out;
        out.line = line_;
        advance(1);  // '<'
        const std::size_t name_start = at_;
        while (at_ < text_.size() && text_[at_] != '>' && text_[at_] != '/' &&
               text_[at_] != ' ' && text_[at_] != '\t' && text_[at_] != '\r' && text_[at_] != '\n') {
            advance(1);
        }
        out.name = std::string(text_.substr(name_start, at_ - name_start));
        if (out.name.empty()) {
            return failure("an element with no name");
        }

        // Attributes, read past. A quoted value may hold '>' or '/'.
        while (true) {
            skip_space();
            if (at_ >= text_.size()) {
                return failure("the file ends inside <" + out.name + ">");
            }
            if (at("/>")) {
                advance(2);
                return out;
            }
            if (text_[at_] == '>') {
                advance(1);
                break;
            }
            if (text_[at_] == '"' || text_[at_] == '\'') {
                const char quote = text_[at_];
                advance(1);
                while (at_ < text_.size() && text_[at_] != quote) {
                    advance(1);
                }
                advance(1);
                continue;
            }
            advance(1);
        }

        // Content.
        while (true) {
            if (at_ >= text_.size()) {
                return failure("<" + out.name + "> is never closed");
            }
            if (at("</")) {
                advance(2);
                const std::size_t close_start = at_;
                while (at_ < text_.size() && text_[at_] != '>') {
                    advance(1);
                }
                const std::string close = trim(text_.substr(close_start, at_ - close_start));
                advance(1);
                if (close != out.name) {
                    return failure("<" + out.name + "> is closed by </" + close + ">");
                }
                return out;
            }
            if (at("<!--")) {
                if (!skip_past("-->")) {
                    return failure("a comment that never closes");
                }
                continue;
            }
            if (at("<![CDATA[")) {
                advance(9);
                const std::size_t end = text_.find("]]>", at_);
                if (end == std::string_view::npos) {
                    return failure("a CDATA section that never closes");
                }
                out.text.append(text_.substr(at_, end - at_));
                advance(end + 3 - at_);
                continue;
            }
            if (at("<?")) {
                if (!skip_past("?>")) {
                    return failure("a processing instruction that never closes");
                }
                continue;
            }
            if (text_[at_] == '<') {
                auto child = element(depth + 1);
                if (!child) {
                    return child;
                }
                out.children.push_back(std::move(*child));
                continue;
            }
            const std::size_t text_start = at_;
            while (at_ < text_.size() && text_[at_] != '<') {
                advance(1);
            }
            decode_entities(text_.substr(text_start, at_ - text_start), out.text);
        }
    }
};

}  // namespace import_detail

[[nodiscard]] inline Expected<XmlElement> read_xml(std::string_view text)
{
    return import_detail::XmlReader(text).document();
}

// ---------------------------------------------------------------------------
// Modes and filters
// ---------------------------------------------------------------------------

// The answer to "what is this program's mode called here", or why there is
// none. Both halves are shown to the operator: the name when it maps, the
// reason when it does not.
struct ModeMapping {
    std::string mode;
    std::string refusal;
};

// SDR#'s DetectorType. Its eight are the engine's first eight under upper-case
// names.
[[nodiscard]] inline ModeMapping map_sdrsharp_mode(std::string_view detector)
{
    const std::string name = fold_case(import_detail::trim(detector));
    for (const std::string_view ours : {"nfm", "wfm", "am", "dsb", "lsb", "usb", "cw", "raw"}) {
        if (name == ours) {
            return {std::string(ours), {}};
        }
    }
    if (name.empty()) {
        return {{}, "no DetectorType"};
    }
    return {{}, "SDR# mode \"" + import_detail::trim(detector) + "\" has no Revenant equivalent"};
}

// SDR++'s numbered modes. See the file header for the evidence behind each.
inline constexpr std::array<std::string_view, 8> kSdrPlusPlusModes = {"nfm", "wfm", "am",  "dsb",
                                                                      "usb", "cw",  "lsb", "raw"};

[[nodiscard]] inline ModeMapping map_sdrpp_mode(std::int64_t mode)
{
    if (mode >= 0 && mode < static_cast<std::int64_t>(kSdrPlusPlusModes.size())) {
        return {std::string(kSdrPlusPlusModes[static_cast<std::size_t>(mode)]), {}};
    }
    return {{}, "SDR++ mode number " + std::to_string(mode) + " is not one SDR++ documents"};
}

// CHIRP's Mode column. FM and NFM are both narrowband FM to Revenant, whose
// nfm filter is wide enough for 5 kHz deviation and so for 2.5 kHz too; NAM is
// narrow AM, which is AM with a narrower filter; DV is D-STAR and P25 is
// P25 phase 1, the two CHIRP digital modes the engine demodulates. An empty
// Mode is CHIRP's own default for a new memory, FM.
[[nodiscard]] inline ModeMapping map_chirp_mode(std::string_view mode)
{
    const std::string raw = import_detail::trim(mode);
    const std::string name = fold_case(raw);
    struct Pair {
        std::string_view chirp;
        std::string_view ours;
    };
    constexpr Pair kPairs[] = {{"fm", "nfm"},  {"nfm", "nfm"}, {"wfm", "wfm"},   {"am", "am"},
                               {"nam", "am"},  {"usb", "usb"}, {"lsb", "lsb"},   {"cw", "cw"},
                               {"dv", "dstar"}, {"p25", "p25p1"}, {"", "nfm"}};
    for (const Pair& pair : kPairs) {
        if (name == pair.chirp) {
            return {std::string(pair.ours), {}};
        }
    }
    // Named reasons for the ones an operator is likely to have, so the skip
    // list says what the channel is rather than only that it was refused.
    if (name == "dmr") {
        return {{}, "DMR is not a mode the engine demodulates"};
    }
    if (name == "dn") {
        return {{}, "DN (System Fusion) is not a mode the engine demodulates"};
    }
    if (name == "cwr" || name == "ncwr") {
        return {{}, "CHIRP mode \"" + raw + "\" is CW on the reversed sideband, which Revenant's cw has no setting for"};
    }
    if (name == "auto") {
        return {{}, "CHIRP mode Auto leaves the mode to the radio, and a memory needs one"};
    }
    return {{}, "CHIRP mode \"" + raw + "\" has no Revenant equivalent"};
}

// Filter edges for a bandwidth another program saved, in Revenant's terms.
//
// Symmetric about the memory's frequency for every mode but the sidebands.
// usb and lsb put one edge on the suppressed carrier and the other a
// bandwidth away, which is what "a 2700 Hz USB filter" means in the programs
// these come from. Revenant's own SSB default starts at 300 Hz rather than at
// the carrier (core/dsp/vrx_reference.cpp, default_passband); the imported
// width is kept as it was saved rather than moved onto that convention. Zero
// or a negative width answers zero edges, the mode's default.
[[nodiscard]] inline std::pair<std::int32_t, std::int32_t> edges_for_bandwidth(std::string_view mode,
                                                                              std::int64_t bandwidth_hz)
{
    if (bandwidth_hz <= 0) {
        return {0, 0};
    }
    const auto width = static_cast<std::int32_t>(std::min<std::int64_t>(bandwidth_hz, 20'000'000));
    if (mode == "usb") {
        return {0, width};
    }
    if (mode == "lsb") {
        return {-width, 0};
    }
    const std::int32_t low = -(width / 2);
    return {low, low + width};
}

// ---------------------------------------------------------------------------
// The readers
// ---------------------------------------------------------------------------

// SDR#'s frequencies.xml.
[[nodiscard]] inline Expected<ImportResult> import_sdrsharp(std::string_view text)
{
    auto root = read_xml(text);
    if (!root) {
        return std::unexpected(with_context(root.error(), "not readable as XML"));
    }
    if (root->name != "ArrayOfMemoryEntry") {
        return fail("the root element is <" + root->name +
                    ">, and an SDR# frequencies.xml has <ArrayOfMemoryEntry>");
    }

    ImportResult out;
    out.format = ImportFormat::SdrSharp;
    std::size_t position = 0;
    for (const XmlElement& entry : root->children) {
        if (entry.name != "MemoryEntry") {
            continue;
        }
        ++position;
        const auto field = [&](std::string_view name) {
            const XmlElement* found = entry.child(name);
            return found != nullptr ? import_detail::trim(found->text) : std::string();
        };
        const std::string name = field("Name");
        const std::string where = name.empty() ? "entry " + std::to_string(position) : name;

        const std::string frequency_text = field("Frequency");
        const auto frequency = json::literal_to_integer(frequency_text);
        if (!frequency || *frequency <= 0) {
            out.skipped.push_back({entry.line, where,
                                   frequency_text.empty()
                                       ? "no Frequency"
                                       : "Frequency \"" + frequency_text + "\" is not a whole number of hertz above zero"});
            continue;
        }
        const ModeMapping mode = map_sdrsharp_mode(field("DetectorType"));
        if (mode.mode.empty()) {
            out.skipped.push_back({entry.line, where, mode.refusal});
            continue;
        }

        Memory memory;
        memory.name = name;
        memory.freq_hz = *frequency;
        memory.mode = mode.mode;
        const auto bandwidth = json::literal_to_integer(field("FilterBandwidth"), true);
        std::tie(memory.passband_low, memory.passband_high) =
            edges_for_bandwidth(memory.mode, bandwidth.value_or(0));
        memory.group = field("GroupName");
        if (import_detail::equal_nocase(field("IsFavourite"), "true")) {
            memory.tags.push_back("favourite");
        }
        // A converter's shift, in force when the entry was saved. Recorded
        // and not applied: Revenant has no converter setting, and whether the
        // saved frequency already includes the shift is SDR#'s business.
        const auto shift = json::literal_to_integer(field("Shift"));
        if (shift && *shift != 0) {
            memory.notes = "SDR# shift " + std::to_string(*shift) + " Hz";
        }
        out.entries.push_back(std::move(memory));
    }
    return out;
}

// SDR++'s frequency_manager_config.json, or a bookmark export from it.
[[nodiscard]] inline Expected<ImportResult> import_sdrpp(std::string_view text)
{
    auto root = json::parse(text);
    if (!root) {
        return std::unexpected(with_context(root.error(), "not readable as JSON"));
    }
    if (!root->is_object()) {
        return fail("an SDR++ file is a JSON object");
    }

    ImportResult out;
    out.format = ImportFormat::SdrPlusPlus;

    const auto read_bookmarks = [&](const json::Value& bookmarks, const std::string& group) {
        for (const json::Member& member : bookmarks.members) {
            const json::Value& mark = member.value;
            const std::string where =
                group.empty() ? member.key : group + ": " + member.key;
            if (!mark.is_object()) {
                out.skipped.push_back({mark.line, where, "not an object"});
                continue;
            }
            const json::Value* frequency_value = mark.find("frequency");
            const auto frequency = json::as_integer(frequency_value, true);
            if (!frequency || *frequency <= 0) {
                out.skipped.push_back({mark.line, where,
                                       frequency_value == nullptr
                                           ? "no frequency"
                                           : "frequency is not a number of hertz above zero"});
                continue;
            }
            const auto number = json::as_integer(mark.find("mode"));
            if (!number) {
                out.skipped.push_back({mark.line, where, "no mode number"});
                continue;
            }
            const ModeMapping mode = map_sdrpp_mode(*number);
            if (mode.mode.empty()) {
                out.skipped.push_back({mark.line, where, mode.refusal});
                continue;
            }
            Memory memory;
            // SDR++ allows names that differ only by trailing spaces, which
            // its guide suggests as the way round its no-duplicates rule;
            // the spaces are the only thing trimmed here.
            memory.name = import_detail::trim(member.key);
            memory.freq_hz = *frequency;
            memory.mode = mode.mode;
            std::tie(memory.passband_low, memory.passband_high) = edges_for_bandwidth(
                memory.mode, json::as_integer(mark.find("bandwidth"), true).value_or(0));
            memory.group = group;
            out.entries.push_back(std::move(memory));
        }
    };

    const json::Value* lists = root->find("lists");
    const json::Value* bookmarks = root->find("bookmarks");
    if (lists != nullptr && lists->is_object()) {
        for (const json::Member& list : lists->members) {
            const json::Value* inner =
                list.value.is_object() ? list.value.find("bookmarks") : nullptr;
            if (inner == nullptr || !inner->is_object()) {
                out.skipped.push_back({list.value.line, list.key, "a list with no bookmarks object"});
                continue;
            }
            read_bookmarks(*inner, list.key);
        }
    } else if (bookmarks != nullptr && bookmarks->is_object()) {
        read_bookmarks(*bookmarks, {});
    } else {
        return fail("no \"lists\" or \"bookmarks\" object, which an SDR++ frequency manager file has");
    }
    return out;
}

// CHIRP's CSV export.
[[nodiscard]] inline Expected<ImportResult> import_chirp(std::string_view text)
{
    const std::vector<CsvRecord> records = read_csv(text);
    if (records.empty()) {
        return fail("the file is empty");
    }

    const CsvRecord& header = records.front();
    const auto column = [&](std::string_view name) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < header.fields.size(); ++i) {
            if (import_detail::equal_nocase(import_detail::trim(header.fields[i]), name)) {
                return i;
            }
        }
        return std::nullopt;
    };
    const auto frequency_column = column("Frequency");
    if (!frequency_column) {
        return fail("the first row has no Frequency column, which a CHIRP CSV export has");
    }
    const auto location_column = column("Location");
    const auto name_column = column("Name");
    const auto duplex_column = column("Duplex");
    const auto offset_column = column("Offset");
    const auto tone_column = column("Tone");
    const auto rtone_column = column("rToneFreq");
    const auto ctone_column = column("cToneFreq");
    const auto dtcs_column = column("DtcsCode");
    const auto polarity_column = column("DtcsPolarity");
    const auto rx_dtcs_column = column("RxDtcsCode");
    const auto cross_column = column("CrossMode");
    const auto mode_column = column("Mode");
    const auto skip_column = column("Skip");
    const auto comment_column = column("Comment");
    const auto urcall_column = column("URCALL");
    const auto rpt1_column = column("RPT1CALL");
    const auto rpt2_column = column("RPT2CALL");

    ImportResult out;
    out.format = ImportFormat::Chirp;
    for (std::size_t r = 1; r < records.size(); ++r) {
        const CsvRecord& row = records[r];
        const auto get = [&](const std::optional<std::size_t>& index) {
            return index && *index < row.fields.size() ? import_detail::trim(row.fields[*index])
                                                       : std::string();
        };
        const std::string name = get(name_column);
        const std::string location = get(location_column);
        const std::string where = !name.empty() ? name
                                  : !location.empty() ? "location " + location
                                                      : "row " + std::to_string(r + 1);

        // Megahertz as written, read exactly: "146.520000" is 146520000.
        const std::string frequency_text = get(frequency_column);
        const auto frequency = frequency_text.empty()
                                   ? std::nullopt
                                   : json::literal_to_integer(frequency_text + "e6");
        if (!frequency || *frequency <= 0) {
            out.skipped.push_back({row.line, where,
                                   frequency_text.empty()
                                       ? "no Frequency"
                                       : "Frequency \"" + frequency_text + "\" is not megahertz to the hertz"});
            continue;
        }
        const ModeMapping mode = map_chirp_mode(get(mode_column));
        if (mode.mode.empty()) {
            out.skipped.push_back({row.line, where, mode.refusal});
            continue;
        }

        // Everything about transmitting and squelch goes in the note, in
        // CHIRP's own terms. Revenant only listens, so none of it changes a
        // recall, and an operator programming a radio later wants it back.
        std::vector<std::string> facts;
        const std::string duplex = get(duplex_column);
        const std::string offset = get(offset_column);
        if (duplex == "+" || duplex == "-") {
            facts.push_back("duplex " + duplex + offset + " MHz");
        } else if (import_detail::equal_nocase(duplex, "split")) {
            facts.push_back("transmit on " + offset + " MHz");
        } else if (import_detail::equal_nocase(duplex, "off")) {
            facts.push_back("transmit off");
        }
        const std::string tone = get(tone_column);
        const std::string dtcs = get(dtcs_column) + " " + get(polarity_column);
        if (tone == "Tone") {
            facts.push_back("tone " + get(rtone_column) + " Hz");
        } else if (tone == "TSQL") {
            facts.push_back("tone squelch " + get(ctone_column) + " Hz");
        } else if (tone == "TSQL-R") {
            facts.push_back("reverse tone squelch " + get(ctone_column) + " Hz");
        } else if (tone == "DTCS") {
            facts.push_back("DCS " + dtcs);
        } else if (tone == "DTCS-R") {
            facts.push_back("reverse DCS " + dtcs);
        } else if (tone == "Cross") {
            facts.push_back("cross " + get(cross_column) + ", tone " + get(rtone_column) +
                            " Hz, tone squelch " + get(ctone_column) + " Hz, DCS " + dtcs +
                            ", receive DCS " + get(rx_dtcs_column));
        } else if (!tone.empty()) {
            facts.push_back("tone mode " + tone);
        }
        const std::string skip = get(skip_column);
        if (skip == "S") {
            facts.push_back("scan: skip");
        } else if (skip == "P") {
            facts.push_back("scan: priority");
        }
        if (mode.mode == "dstar") {
            const std::string ur = get(urcall_column);
            const std::string r1 = get(rpt1_column);
            const std::string r2 = get(rpt2_column);
            if (!ur.empty() || !r1.empty() || !r2.empty()) {
                facts.push_back("D-STAR UR " + ur + ", RPT1 " + r1 + ", RPT2 " + r2);
            }
        }
        if (!location.empty()) {
            facts.push_back("location " + location);
        }

        Memory memory;
        memory.name = name;
        memory.freq_hz = *frequency;
        memory.mode = mode.mode;
        memory.notes = get(comment_column);
        std::string chirp_line;
        for (const std::string& fact : facts) {
            chirp_line += chirp_line.empty() ? "CHIRP: " : "; ";
            chirp_line += fact;
        }
        if (!chirp_line.empty()) {
            memory.notes += memory.notes.empty() ? "" : "\n";
            memory.notes += chirp_line;
        }
        out.entries.push_back(std::move(memory));
    }
    return out;
}

// A Revenant memory file, which is what an export writes and a backup is.
// Memories only: their ids are this file's and mean nothing in the book they
// are going into, so scan lists are not carried across.
[[nodiscard]] inline Expected<ImportResult> import_revenant(std::string_view text)
{
    auto loaded = memory_book_from_json(text);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    ImportResult out;
    out.format = ImportFormat::Revenant;
    out.skipped = std::move(loaded->skipped);
    for (Memory& memory : loaded->book.memories) {
        memory.id = 0;
        out.entries.push_back(std::move(memory));
    }
    return out;
}

// Which reader a file wants, from its contents and, failing that, its name.
// The contents first, because a frequencies.xml renamed by somebody is still
// SDR#'s and a CSV called memories.json is still a CSV.
[[nodiscard]] inline std::optional<ImportFormat> detect_import_format(std::string_view text,
                                                                     std::string_view file_name)
{
    std::size_t at = 0;
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' ||
                                text[at] == '\n')) {
        ++at;
    }
    const std::string_view start = text.substr(at);
    if (start.starts_with("<")) {
        return ImportFormat::SdrSharp;
    }
    if (start.starts_with("{")) {
        return start.find(kMemoryFormat) != std::string_view::npos ? ImportFormat::Revenant
                                                                   : ImportFormat::SdrPlusPlus;
    }
    const std::size_t newline = start.find('\n');
    const std::string first = fold_case(start.substr(0, newline));
    if (first.find("frequency") != std::string::npos && first.find(',') != std::string::npos) {
        return ImportFormat::Chirp;
    }
    const std::string name = fold_case(file_name);
    if (name.ends_with(".csv")) {
        return ImportFormat::Chirp;
    }
    if (name.ends_with(".xml")) {
        return ImportFormat::SdrSharp;
    }
    return std::nullopt;
}

// A file's bytes as memories, whichever of the four formats it is.
[[nodiscard]] inline Expected<ImportResult> import_memories(std::string_view bytes,
                                                            std::string_view file_name)
{
    const std::string text = decode_text(bytes);
    const auto format = detect_import_format(text, file_name);
    if (!format) {
        return fail("not a format this reads: SDR#'s frequencies.xml, SDR++'s frequency "
                    "manager JSON, a CHIRP CSV export or a Revenant memory file");
    }
    switch (*format) {
        case ImportFormat::Revenant: return import_revenant(text);
        case ImportFormat::SdrSharp: return import_sdrsharp(text);
        case ImportFormat::SdrPlusPlus: return import_sdrpp(text);
        case ImportFormat::Chirp: return import_chirp(text);
    }
    return fail("not a format this reads");
}

// ---------------------------------------------------------------------------
// Preview and de-duplication
// ---------------------------------------------------------------------------

// What an import would do to a book, shown before it does it.
struct ImportPlan {
    std::vector<Memory> to_add;

    // Entries already in the book, or earlier in the same file, under
    // duplicate_of's rule. Reported with the skipped lines, since each is
    // an entry the file had and the book will not gain.
    std::vector<SkippedLine> duplicates;
};

[[nodiscard]] inline ImportPlan plan_import(const MemoryBook& book, const ImportResult& result)
{
    ImportPlan plan;
    MemoryBook seen;
    for (const Memory& entry : result.entries) {
        const std::string label = memory_label(entry);
        if (duplicate_of(book, entry.freq_hz, entry.name) != nullptr) {
            plan.duplicates.push_back({0, label, "already in the list at " + format_mhz(entry.freq_hz) + " MHz"});
            continue;
        }
        if (duplicate_of(seen, entry.freq_hz, entry.name) != nullptr) {
            plan.duplicates.push_back({0, label, "appears twice in the file"});
            continue;
        }
        Memory copy = entry;
        copy.id = 0;
        seen.memories.push_back(copy);
        plan.to_add.push_back(std::move(copy));
    }
    return plan;
}

// Adds a plan's memories to the book, stamped with the import time, and
// answers how many.
inline std::size_t apply_import(MemoryBook& book, ImportPlan plan, std::int64_t now)
{
    for (Memory& memory : plan.to_add) {
        memory.created = now;
        add_memory(book, std::move(memory), now);
    }
    return plan.to_add.size();
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

// The header CHIRP writes today, the longer of the two in its stock
// configurations.
inline constexpr std::string_view kChirpHeader =
    "Location,Name,Frequency,Duplex,Offset,Tone,rToneFreq,cToneFreq,DtcsCode,DtcsPolarity,"
    "RxDtcsCode,CrossMode,Mode,TStep,Skip,Power,Comment,URCALL,RPT1CALL,RPT2CALL,DVCODE";

// Revenant's mode in CHIRP's Mode column, or why it has none. nfm goes out as
// FM, which is CHIRP's default and what a handheld on a 25 or 12.5 kHz
// channel is set to; it reads back in as nfm.
[[nodiscard]] inline ModeMapping chirp_mode_for(std::string_view mode)
{
    struct Pair {
        std::string_view ours;
        std::string_view chirp;
    };
    constexpr Pair kPairs[] = {{"nfm", "FM"},  {"wfm", "WFM"}, {"am", "AM"},  {"usb", "USB"},
                               {"lsb", "LSB"}, {"cw", "CW"},   {"dstar", "DV"}, {"p25p1", "P25"}};
    for (const Pair& pair : kPairs) {
        if (mode == pair.ours) {
            return {std::string(pair.chirp), {}};
        }
    }
    return {{}, "CHIRP has no mode for " + std::string(mode)};
}

struct ChirpExport {
    std::string text;
    std::vector<SkippedLine> skipped;
};

// Memories as a CHIRP CSV, numbered from 1 in the order given. The tone and
// duplex columns are CHIRP's neutral values, since a memory here holds only
// what a receiver needs; an import from CHIRP kept its tones in the note, and
// the note goes out as the Comment. Line breaks in a note become "; ", because
// a CHIRP memory's comment is one line.
[[nodiscard]] inline ChirpExport export_chirp_csv(const std::vector<Memory>& memories)
{
    ChirpExport out;
    out.text = std::string(kChirpHeader) + "\r\n";
    std::size_t location = 1;
    for (const Memory& memory : memories) {
        const ModeMapping mode = chirp_mode_for(memory.mode);
        if (mode.mode.empty()) {
            out.skipped.push_back({0, memory_label(memory), mode.refusal});
            continue;
        }
        std::string comment;
        for (const char c : memory.notes) {
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                comment += "; ";
            } else {
                comment.push_back(c);
            }
        }
        out.text += std::to_string(location++) + "," + csv_field(memory.name) + "," +
                    format_mhz(memory.freq_hz) + ",,0.000000,,88.5,88.5,023,NN,023,Tone->Tone," +
                    mode.mode + ",5.00,,," + csv_field(comment) + ",,,,\r\n";
    }
    return out;
}

// Memories as a Revenant memory file, with the scan lists that name only
// memories being exported, trimmed to them. Ranges go with every export,
// since they name no memory.
[[nodiscard]] inline std::string export_revenant_json(const MemoryBook& book,
                                                      const std::vector<std::size_t>& indices)
{
    MemoryBook out;
    std::vector<std::uint64_t> ids;
    for (const std::size_t index : indices) {
        if (index < book.memories.size()) {
            out.memories.push_back(book.memories[index]);
            ids.push_back(book.memories[index].id);
        }
    }
    for (const ScanList& list : book.scan_lists) {
        ScanList copy = list;
        if (copy.kind == ScanList::Kind::Memories) {
            std::erase_if(copy.members, [&](std::uint64_t id) {
                return std::find(ids.begin(), ids.end(), id) == ids.end();
            });
            if (copy.members.empty()) {
                continue;
            }
        }
        out.scan_lists.push_back(std::move(copy));
    }
    return memory_book_to_json(out);
}

}  // namespace revenant::ui
