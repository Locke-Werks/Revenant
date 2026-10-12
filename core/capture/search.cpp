#include "core/capture/search.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

namespace revenant::capture {

namespace {

// ---------------------------------------------------------------------------
// JSON, scanned rather than parsed
// ---------------------------------------------------------------------------
//
// The merge has to leave every key it does not own exactly as it found it,
// including ones from extensions this build has never heard of and numbers
// written at a precision a parse and re-print would change. So nothing here
// builds a tree. A value is located as a span of the original text, the
// spans that are kept are copied through verbatim, and only the annotations
// this merge writes are produced fresh.
//
// file_source.cpp has a full parser for reading a sidecar. It is private to
// that file and builds the tree this deliberately avoids, so it is not
// reused.

// Deep enough for any SigMF file, shallow enough that a hostile one cannot
// take the stack.
constexpr int kMaxDepth = 64;

struct Span {
    std::size_t begin = 0;
    std::size_t end = 0;
};

class Scanner {
public:
    explicit Scanner(std::string_view text) : text_(text) {}

    void skip_space(std::size_t& at) const
    {
        while (at < text_.size() && (text_[at] == ' ' || text_[at] == '\t' ||
                                     text_[at] == '\n' || text_[at] == '\r')) {
            ++at;
        }
    }

    // One past the end of the value starting at `at`, after whitespace.
    [[nodiscard]] Expected<Span> value(std::size_t at, int depth = 0) const
    {
        skip_space(at);
        if (at >= text_.size()) {
            return fail("the meta file ends where a value was expected");
        }
        if (depth > kMaxDepth) {
            return fail(std::format("the meta file nests deeper than {} levels", kMaxDepth));
        }
        const char c = text_[at];
        if (c == '"') {
            auto end = string_end(at);
            if (!end) {
                return std::unexpected(end.error());
            }
            return Span{at, *end};
        }
        if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            std::size_t cursor = at + 1;
            skip_space(cursor);
            if (cursor < text_.size() && text_[cursor] == close) {
                return Span{at, cursor + 1};
            }
            while (true) {
                if (c == '{') {
                    auto key = value(cursor, depth + 1);
                    if (!key) {
                        return key;
                    }
                    if (text_[key->begin] != '"') {
                        return fail(std::format("an object key at byte {} is not a string",
                                                key->begin));
                    }
                    cursor = key->end;
                    skip_space(cursor);
                    if (cursor >= text_.size() || text_[cursor] != ':') {
                        return fail(std::format("expected ':' at byte {}", cursor));
                    }
                    ++cursor;
                }
                auto inner = value(cursor, depth + 1);
                if (!inner) {
                    return inner;
                }
                cursor = inner->end;
                skip_space(cursor);
                if (cursor >= text_.size()) {
                    return fail("the meta file ends inside an object or array");
                }
                if (text_[cursor] == ',') {
                    ++cursor;
                    continue;
                }
                if (text_[cursor] == close) {
                    return Span{at, cursor + 1};
                }
                return fail(std::format("unexpected '{}' at byte {}", text_[cursor], cursor));
            }
        }
        // A number or a literal: everything up to the next delimiter.
        std::size_t end = at;
        while (end < text_.size() && text_[end] != ',' && text_[end] != '}' &&
               text_[end] != ']' && text_[end] != ' ' && text_[end] != '\t' &&
               text_[end] != '\n' && text_[end] != '\r') {
            ++end;
        }
        if (end == at) {
            return fail(std::format("unexpected '{}' at byte {}", c, at));
        }
        return Span{at, end};
    }

    // The members of the object at `object`, each as a key span and a value
    // span.
    [[nodiscard]] Expected<std::vector<std::pair<Span, Span>>> members(Span object) const
    {
        std::vector<std::pair<Span, Span>> out;
        if (text_[object.begin] != '{') {
            return fail(std::format("expected an object at byte {}", object.begin));
        }
        std::size_t cursor = object.begin + 1;
        skip_space(cursor);
        if (text_[cursor] == '}') {
            return out;
        }
        while (cursor < object.end) {
            auto key = value(cursor, 1);
            if (!key) {
                return std::unexpected(key.error());
            }
            cursor = key->end;
            skip_space(cursor);
            ++cursor;  // the ':' value() already checked
            auto inner = value(cursor, 1);
            if (!inner) {
                return std::unexpected(inner.error());
            }
            out.emplace_back(*key, *inner);
            cursor = inner->end;
            skip_space(cursor);
            if (text_[cursor] == '}') {
                break;
            }
            ++cursor;
        }
        return out;
    }

    [[nodiscard]] Expected<std::vector<Span>> elements(Span array) const
    {
        std::vector<Span> out;
        if (text_[array.begin] != '[') {
            return fail(std::format("expected an array at byte {}", array.begin));
        }
        std::size_t cursor = array.begin + 1;
        skip_space(cursor);
        if (text_[cursor] == ']') {
            return out;
        }
        while (cursor < array.end) {
            auto inner = value(cursor, 1);
            if (!inner) {
                return std::unexpected(inner.error());
            }
            out.push_back(*inner);
            cursor = inner->end;
            skip_space(cursor);
            if (text_[cursor] == ']') {
                break;
            }
            ++cursor;
        }
        return out;
    }

    // The decoded text of a string value.
    [[nodiscard]] Expected<std::string> string(Span span) const
    {
        if (text_[span.begin] != '"') {
            return fail(std::format("expected a string at byte {}", span.begin));
        }
        std::string out;
        for (std::size_t i = span.begin + 1; i + 1 < span.end; ++i) {
            const char c = text_[i];
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            ++i;
            switch (text_[i]) {
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    auto unit = hex4(i + 1);
                    if (!unit) {
                        return std::unexpected(unit.error());
                    }
                    i += 4;
                    std::uint32_t code = *unit;
                    if (code >= 0xD800 && code < 0xDC00 && i + 6 < span.end &&
                        text_[i + 1] == '\\' && text_[i + 2] == 'u') {
                        auto low = hex4(i + 3);
                        if (low && *low >= 0xDC00 && *low < 0xE000) {
                            code = 0x10000 + ((code - 0xD800) << 10) + (*low - 0xDC00);
                            i += 6;
                        }
                    }
                    append_utf8(out, code);
                    break;
                }
                default: out.push_back(text_[i]); break;
            }
        }
        return out;
    }

    [[nodiscard]] std::string_view raw(Span span) const
    {
        return text_.substr(span.begin, span.end - span.begin);
    }

private:
    [[nodiscard]] Expected<std::size_t> string_end(std::size_t at) const
    {
        for (std::size_t i = at + 1; i < text_.size(); ++i) {
            if (text_[i] == '\\') {
                ++i;
                continue;
            }
            if (text_[i] == '"') {
                return i + 1;
            }
        }
        return fail(std::format("the string at byte {} never ends", at));
    }

    [[nodiscard]] Expected<std::uint32_t> hex4(std::size_t at) const
    {
        if (at + 4 > text_.size()) {
            return fail("a \\u escape is cut short");
        }
        std::uint32_t value = 0;
        const auto parsed = std::from_chars(text_.data() + at, text_.data() + at + 4, value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != text_.data() + at + 4) {
            return fail(std::format("a \\u escape at byte {} is not four hex digits", at));
        }
        return value;
    }

    static void append_utf8(std::string& out, std::uint32_t code)
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

    std::string_view text_;
};

[[nodiscard]] Expected<double> number_of(std::string_view raw, std::string_view key)
{
    double value = 0.0;
    const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size()) {
        return fail(std::format("{} is '{}', which is not a number", key, raw));
    }
    return value;
}

// Sample positions and frequencies are integers, and a double would round a
// position past 2^53. A writer that put "1.0e6" in an integer field is still
// read, through the double, when the value is whole.
template <class T>
[[nodiscard]] Expected<T> integer_of(std::string_view raw, std::string_view key)
{
    T value{};
    const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (parsed.ec == std::errc{} && parsed.ptr == raw.data() + raw.size()) {
        return value;
    }
    auto real = number_of(raw, key);
    if (!real) {
        return std::unexpected(real.error());
    }
    if (std::floor(*real) != *real || std::abs(*real) > 9.0e18) {
        return fail(std::format("{} is '{}', which is not a whole number", key, raw));
    }
    if constexpr (std::is_unsigned_v<T>) {
        if (*real < 0.0) {
            return fail(std::format("{} is '{}', which is negative", key, raw));
        }
    }
    return static_cast<T>(*real);
}

[[nodiscard]] std::string escaped(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
    return out;
}

// JSON has no NaN or infinity, and a meta that carried one would be refused
// by every other SigMF reader. A measurement that came out non-finite is
// left out rather than written as a number it is not.
void append_number(std::string& out, std::string_view key, double value)
{
    if (std::isfinite(value)) {
        out += std::format(",\n            \"{}\": {}", key, value);
    }
}

[[nodiscard]] std::string serialise(const Annotation& annotation, std::string_view generator)
{
    std::string out = "{\n";
    out += std::format("            \"core:sample_start\": {}", annotation.sample_start);
    out += std::format(",\n            \"core:sample_count\": {}", annotation.sample_count);
    out += std::format(",\n            \"core:freq_lower_edge\": {}", annotation.freq_lower);
    out += std::format(",\n            \"core:freq_upper_edge\": {}", annotation.freq_upper);
    out += std::format(",\n            \"core:label\": {}",
                       escaped(annotation.family.empty() ? std::string_view("signal")
                                                         : std::string_view(annotation.family)));
    out += std::format(",\n            \"core:generator\": {}", escaped(generator));
    append_number(out, "revenant:confidence", annotation.confidence);
    append_number(out, "revenant:peak_db", annotation.peak_db);
    append_number(out, "revenant:snr_db", annotation.snr_db);
    if (!annotation.family.empty()) {
        out += std::format(",\n            \"revenant:family\": {}", escaped(annotation.family));
    }
    out += "\n        }";
    return out;
}

[[nodiscard]] Expected<std::string> read_file(const std::string& path)
{
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) {
        return fail(std::format("could not open '{}'", path));
    }
    std::ostringstream text;
    text << in.rdbuf();
    if (in.bad()) {
        return fail(std::format("could not read '{}'", path));
    }
    return std::move(text).str();
}

// The top-level object of a meta file and its members.
struct Document {
    Span object;
    std::vector<std::pair<Span, Span>> members;
};

[[nodiscard]] Expected<Document> top_level(const Scanner& scanner, std::string_view text)
{
    std::size_t at = 0;
    // A UTF-8 byte order mark is not JSON, and some editors write one.
    if (text.starts_with("\xEF\xBB\xBF")) {
        at = 3;
    }
    auto object = scanner.value(at);
    if (!object) {
        return std::unexpected(object.error());
    }
    if (text[object->begin] != '{') {
        return fail("the meta file is not a JSON object");
    }
    std::size_t after = object->end;
    scanner.skip_space(after);
    if (after != text.size()) {
        return fail(std::format("the meta file carries text after its object, at byte {}",
                                after));
    }
    auto members = scanner.members(*object);
    if (!members) {
        return std::unexpected(members.error());
    }
    return Document{*object, std::move(*members)};
}

[[nodiscard]] Expected<std::optional<Span>> annotations_of(const Scanner& scanner,
                                                           const Document& document)
{
    for (const auto& [key, value] : document.members) {
        auto name = scanner.string(key);
        if (!name) {
            return std::unexpected(name.error());
        }
        if (*name == "annotations") {
            if (scanner.raw(value).front() != '[') {
                return fail("annotations is not an array");
            }
            return std::optional<Span>(value);
        }
    }
    return std::optional<Span>();
}

[[nodiscard]] Expected<std::optional<Span>> member_named(
    const Scanner& scanner, std::span<const std::pair<Span, Span>> members, std::string_view name)
{
    for (const auto& [key, value] : members) {
        auto text = scanner.string(key);
        if (!text) {
            return std::unexpected(text.error());
        }
        if (*text == name) {
            return std::optional<Span>(value);
        }
    }
    return std::optional<Span>();
}

constexpr std::string_view kIndexedKey = "revenant:indexed";

// Where global and its revenant:indexed array sit, and what the array holds,
// each entry beside its own text so a merge can keep it verbatim.
struct StampLayout {
    Span global;
    std::vector<std::pair<Span, Span>> global_members;
    std::optional<Span> array;
    std::vector<std::pair<IndexStamp, Span>> entries;
};

[[nodiscard]] Expected<StampLayout> stamps_of(const Scanner& scanner, const Document& document)
{
    auto global = member_named(scanner, document.members, "global");
    if (!global) {
        return std::unexpected(global.error());
    }
    if (!global->has_value() || scanner.raw(**global).front() != '{') {
        return fail("the meta file has no global object to record the index pass in");
    }
    StampLayout out;
    out.global = **global;
    auto members = scanner.members(out.global);
    if (!members) {
        return std::unexpected(members.error());
    }
    out.global_members = std::move(*members);
    auto array = member_named(scanner, out.global_members, kIndexedKey);
    if (!array) {
        return std::unexpected(array.error());
    }
    if (!array->has_value()) {
        return out;
    }
    if (scanner.raw(**array).front() != '[') {
        return fail(std::format("{} is not an array", kIndexedKey));
    }
    out.array = **array;
    auto elements = scanner.elements(**array);
    if (!elements) {
        return std::unexpected(elements.error());
    }
    for (std::size_t i = 0; i < elements->size(); ++i) {
        const Span entry = (*elements)[i];
        auto fields = scanner.members(entry);
        if (!fields) {
            return std::unexpected(
                with_context(fields.error(), std::format("{} entry {}", kIndexedKey, i)));
        }
        IndexStamp stamp;
        bool have_generator = false;
        for (const auto& [key_span, value_span] : *fields) {
            auto key = scanner.string(key_span);
            if (!key) {
                return std::unexpected(key.error());
            }
            std::string* into = nullptr;
            if (*key == "generator") {
                into = &stamp.generator;
                have_generator = true;
            } else if (*key == "at") {
                into = &stamp.at;
            } else if (*key == "settings") {
                into = &stamp.settings;
            }
            if (into == nullptr) {
                continue;
            }
            auto text = scanner.string(value_span);
            if (!text) {
                return std::unexpected(with_context(
                    text.error(), std::format("{} entry {} {}", kIndexedKey, i, *key)));
            }
            *into = std::move(*text);
        }
        if (!have_generator) {
            return fail(std::format("{} entry {} has no generator", kIndexedKey, i));
        }
        out.entries.emplace_back(std::move(stamp), entry);
    }
    return out;
}

[[nodiscard]] std::string serialise(const IndexStamp& stamp)
{
    return std::format("{{\"generator\": {}, \"at\": {}, \"settings\": {}}}",
                       escaped(stamp.generator), escaped(stamp.at), escaped(stamp.settings));
}

[[nodiscard]] std::string iso_now()
{
    using namespace std::chrono;
    const auto now = floor<milliseconds>(system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%S}Z", now);
}

// A span of the original text and what replaces it. Insertions are edits
// whose span covers only the whitespace they displace.
struct Edit {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string text;
};

// Where a new last member of the object goes: after its last member, before
// the whitespace that precedes the close, so nothing already there moves.
[[nodiscard]] std::size_t insertion_point(std::string_view text, Span object)
{
    std::size_t at = object.end - 1;
    while (at > object.begin && (text[at - 1] == ' ' || text[at - 1] == '\t' ||
                                 text[at - 1] == '\n' || text[at - 1] == '\r')) {
        --at;
    }
    return at;
}

[[nodiscard]] Expected<StoredAnnotation> annotation_of(const Scanner& scanner, Span entry,
                                                       std::size_t index)
{
    auto members = scanner.members(entry);
    if (!members) {
        return std::unexpected(
            with_context(members.error(), std::format("annotation {}", index)));
    }
    StoredAnnotation out;
    bool have_start = false;
    for (const auto& [key_span, value_span] : *members) {
        auto key = scanner.string(key_span);
        if (!key) {
            return std::unexpected(key.error());
        }
        const std::string_view raw = scanner.raw(value_span);
        const auto as_string = [&]() { return scanner.string(value_span); };

        Status read;
        const auto take = [&read](auto&& got, auto& into) {
            if (got) {
                into = *got;
            } else {
                read = std::unexpected(got.error());
            }
        };

        if (*key == "core:sample_start") {
            take(integer_of<std::uint64_t>(raw, *key), out.annotation.sample_start);
            have_start = true;
        } else if (*key == "core:sample_count") {
            take(integer_of<std::uint64_t>(raw, *key), out.annotation.sample_count);
        } else if (*key == "core:freq_lower_edge") {
            take(integer_of<dsp::Hertz>(raw, *key), out.annotation.freq_lower);
        } else if (*key == "core:freq_upper_edge") {
            take(integer_of<dsp::Hertz>(raw, *key), out.annotation.freq_upper);
        } else if (*key == "core:generator") {
            take(as_string(), out.generator);
        } else if (*key == "revenant:confidence") {
            take(number_of(raw, *key), out.annotation.confidence);
        } else if (*key == "revenant:peak_db") {
            take(number_of(raw, *key), out.annotation.peak_db);
        } else if (*key == "revenant:snr_db") {
            take(number_of(raw, *key), out.annotation.snr_db);
        } else if (*key == "revenant:family") {
            take(as_string(), out.annotation.family);
        } else if (*key == "revenant:track_id") {
            take(integer_of<std::uint64_t>(raw, *key), out.track_id);
        }
        if (!read) {
            return std::unexpected(with_context(read.error(), std::format("annotation {}", index)));
        }
    }
    if (!have_start) {
        return fail(std::format("annotation {} has no core:sample_start", index));
    }
    return out;
}

[[nodiscard]] std::string lowered(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

[[nodiscard]] std::int64_t wall_clock_at(const SegmentInfo& info, std::uint64_t offset)
{
    return dsp::BlockTimestamp{info.first_index + offset, info.epoch_anchor_ns, info.rate}
        .wall_clock_ns();
}

// Everything in the query except time and frequency, which occupancy states
// separately.
[[nodiscard]] bool passes_measures(const StoredAnnotation& stored, const Query& query,
                                   std::int64_t duration_ns, const std::string& family_lowered)
{
    const Annotation& a = stored.annotation;
    if (a.confidence < query.min_confidence) {
        return false;
    }
    if (query.min_snr_db.has_value() && a.snr_db < *query.min_snr_db) {
        return false;
    }
    if (duration_ns < query.min_duration_ns) {
        return false;
    }
    if (!query.generator.empty() && stored.generator != query.generator) {
        return false;
    }
    if (!family_lowered.empty() &&
        lowered(a.family).find(family_lowered) == std::string::npos) {
        return false;
    }
    return true;
}

[[nodiscard]] bool overlaps_band(const Annotation& a, std::optional<dsp::Hertz> low,
                                 std::optional<dsp::Hertz> high)
{
    if (low.has_value() && a.freq_upper < *low) {
        return false;
    }
    if (high.has_value() && a.freq_lower > *high) {
        return false;
    }
    return true;
}

// Total length of the union of intervals, each already clipped to a bucket.
[[nodiscard]] std::int64_t union_length(std::vector<std::pair<std::int64_t, std::int64_t>>& spans)
{
    std::ranges::sort(spans);
    std::int64_t total = 0;
    std::int64_t open_start = 0;
    std::int64_t open_end = 0;
    bool open = false;
    for (const auto& [start, end] : spans) {
        if (!open || start > open_end) {
            if (open) {
                total += open_end - open_start;
            }
            open_start = start;
            open_end = end;
            open = true;
        } else {
            open_end = std::max(open_end, end);
        }
    }
    if (open) {
        total += open_end - open_start;
    }
    return total;
}

}  // namespace

Expected<std::vector<StoredAnnotation>> read_annotations(const std::string& meta_path)
{
    auto text = read_file(meta_path);
    if (!text) {
        return std::unexpected(text.error());
    }
    const Scanner scanner(*text);
    auto document = top_level(scanner, *text);
    if (!document) {
        return std::unexpected(with_context(document.error(), meta_path));
    }
    auto array = annotations_of(scanner, *document);
    if (!array) {
        return std::unexpected(with_context(array.error(), meta_path));
    }
    std::vector<StoredAnnotation> out;
    if (!array->has_value()) {
        return out;
    }
    auto entries = scanner.elements(**array);
    if (!entries) {
        return std::unexpected(with_context(entries.error(), meta_path));
    }
    out.reserve(entries->size());
    for (std::size_t i = 0; i < entries->size(); ++i) {
        auto one = annotation_of(scanner, (*entries)[i], i);
        if (!one) {
            return std::unexpected(with_context(one.error(), meta_path));
        }
        out.push_back(std::move(*one));
    }
    return out;
}

Expected<std::string> merge_annotations(std::string_view meta_text,
                                        std::span<const Annotation> annotations,
                                        std::string_view generator,
                                        const std::optional<IndexStamp>& stamp)
{
    const Scanner scanner(meta_text);
    auto document = top_level(scanner, meta_text);
    if (!document) {
        return std::unexpected(document.error());
    }
    auto array = annotations_of(scanner, *document);
    if (!array) {
        return std::unexpected(array.error());
    }

    // Kept entries stay as their original text, keyed by their start so the
    // merged array can be ordered without re-printing them.
    std::vector<std::pair<std::uint64_t, std::string>> merged;
    if (array->has_value()) {
        auto entries = scanner.elements(**array);
        if (!entries) {
            return std::unexpected(entries.error());
        }
        for (std::size_t i = 0; i < entries->size(); ++i) {
            auto one = annotation_of(scanner, (*entries)[i], i);
            if (!one) {
                return std::unexpected(one.error());
            }
            if (one->generator == generator) {
                continue;
            }
            merged.emplace_back(one->annotation.sample_start,
                                std::string(scanner.raw((*entries)[i])));
        }
    }
    for (const Annotation& annotation : annotations) {
        merged.emplace_back(annotation.sample_start, serialise(annotation, generator));
    }
    // Stable, so entries that start together keep the order they were in.
    std::ranges::stable_sort(merged, {}, &std::pair<std::uint64_t, std::string>::first);

    std::string body = "[";
    for (std::size_t i = 0; i < merged.size(); ++i) {
        body += i == 0 ? "\n        " : ",\n        ";
        body += merged[i].second;
    }
    body += merged.empty() ? "]" : "\n    ]";

    std::vector<Edit> edits;
    if (array->has_value()) {
        edits.push_back({(*array)->begin, (*array)->end, std::move(body)});
    } else {
        // No annotations key yet: one goes in as the last member.
        const std::size_t close = document->object.end - 1;
        edits.push_back({insertion_point(meta_text, document->object), close,
                         std::string(document->members.empty() ? "\n    " : ",\n    ") +
                             "\"annotations\": " + body + "\n"});
    }

    if (stamp.has_value()) {
        auto layout = stamps_of(scanner, *document);
        if (!layout) {
            return std::unexpected(layout.error());
        }
        std::string list = "[";
        bool first = true;
        const auto add = [&](std::string_view entry) {
            list += first ? "\n            " : ",\n            ";
            list += entry;
            first = false;
        };
        for (const auto& [kept, span] : layout->entries) {
            if (kept.generator != stamp->generator) {
                add(scanner.raw(span));
            }
        }
        add(serialise(*stamp));
        list += "\n        ]";
        if (layout->array.has_value()) {
            edits.push_back({layout->array->begin, layout->array->end, std::move(list)});
        } else {
            edits.push_back({insertion_point(meta_text, layout->global), layout->global.end - 1,
                             std::string(layout->global_members.empty() ? "\n        "
                                                                        : ",\n        ") +
                                 std::format("\"{}\": ", kIndexedKey) + list + "\n    "});
        }
    }

    // Global and annotations are separate top-level members, so the edits
    // never overlap and apply in order of position.
    std::ranges::sort(edits, {}, &Edit::begin);
    std::string out;
    out.reserve(meta_text.size() + 1024);
    std::size_t cursor = 0;
    for (const Edit& edit : edits) {
        out.append(meta_text.substr(cursor, edit.begin - cursor));
        out += edit.text;
        cursor = edit.end;
    }
    out.append(meta_text.substr(cursor));
    return out;
}

Expected<std::vector<IndexStamp>> read_index_stamps(const std::string& meta_path)
{
    auto text = read_file(meta_path);
    if (!text) {
        return std::unexpected(text.error());
    }
    const Scanner scanner(*text);
    auto document = top_level(scanner, *text);
    if (!document) {
        return std::unexpected(with_context(document.error(), meta_path));
    }
    auto layout = stamps_of(scanner, *document);
    if (!layout) {
        return std::unexpected(with_context(layout.error(), meta_path));
    }
    std::vector<IndexStamp> out;
    out.reserve(layout->entries.size());
    for (auto& [stamp, span] : layout->entries) {
        out.push_back(std::move(stamp));
    }
    return out;
}

bool indexed_by(std::span<const IndexStamp> stamps, std::span<const StoredAnnotation> annotations,
                std::string_view generator)
{
    return std::ranges::any_of(stamps,
                               [&](const IndexStamp& s) { return s.generator == generator; }) ||
           std::ranges::any_of(annotations, [&](const StoredAnnotation& a) {
               return a.generator == generator;
           });
}

Status write_annotations(const std::string& meta_path, std::span<const Annotation> annotations,
                         std::string_view generator, std::string_view settings)
{
    auto text = read_file(meta_path);
    if (!text) {
        return std::unexpected(text.error());
    }
    const IndexStamp stamp{std::string(generator), iso_now(), std::string(settings)};
    auto merged = merge_annotations(*text, annotations, generator, stamp);
    if (!merged) {
        return std::unexpected(with_context(merged.error(), meta_path));
    }

    const std::filesystem::path target(meta_path);
    std::filesystem::path temporary = target;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return fail(std::format("could not create '{}'", temporary.string()));
        }
        out.write(merged->data(), static_cast<std::streamsize>(merged->size()));
        out.flush();
        if (!out) {
            out.close();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return fail(std::format("could not write '{}'", temporary.string()));
        }
    }
    // Replaces the existing file in one step on Windows as well: MSVC's
    // rename is MoveFileExW with MOVEFILE_REPLACE_EXISTING.
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return fail(std::format("could not replace '{}': {}", meta_path, error.message()),
                    error.value());
    }
    return {};
}

std::vector<Hit> run_query(std::span<const IndexedSegment> segments, const Query& query)
{
    const std::string family = lowered(query.family);
    std::vector<Hit> hits;
    for (const IndexedSegment& segment : segments) {
        for (const StoredAnnotation& stored : segment.annotations) {
            const Annotation& a = stored.annotation;
            const std::int64_t start = wall_clock_at(segment.info, a.sample_start);
            const std::int64_t end = wall_clock_at(segment.info, a.sample_start + a.sample_count);
            if (!passes_measures(stored, query, end - start, family)) {
                continue;
            }
            if (!overlaps_band(a, query.freq_low, query.freq_high)) {
                continue;
            }
            // Half open, except that an instantaneous annotation at from_ns
            // is still inside the range.
            if (query.from_ns.has_value() && end < *query.from_ns) {
                continue;
            }
            if (query.from_ns.has_value() && end == *query.from_ns && end != start) {
                continue;
            }
            if (query.to_ns.has_value() && start >= *query.to_ns) {
                continue;
            }

            Hit hit;
            hit.start_ns = start;
            hit.end_ns = end;
            hit.freq_lower = a.freq_lower;
            hit.freq_upper = a.freq_upper;
            hit.data_path = segment.data_path;
            hit.sample_start = a.sample_start;
            hit.sample_count = a.sample_count;
            hit.stream_epoch = segment.info.stream_epoch;
            hit.first_index = segment.info.first_index;
            hit.confidence = a.confidence;
            hit.peak_db = a.peak_db;
            hit.snr_db = a.snr_db;
            hit.family = a.family;
            hit.generator = stored.generator;
            hits.push_back(std::move(hit));
        }
    }
    std::ranges::sort(hits, [](const Hit& x, const Hit& y) {
        return std::tie(x.start_ns, x.freq_lower, x.data_path, x.sample_start) <
               std::tie(y.start_ns, y.freq_lower, y.data_path, y.sample_start);
    });
    return hits;
}

Expected<std::vector<OccupancyBucket>> occupancy(std::span<const IndexedSegment> segments,
                                                 const OccupancyRequest& request)
{
    if (request.bucket_ns <= 0) {
        return fail("an occupancy bucket has to be longer than zero");
    }
    if (request.to_ns <= request.from_ns) {
        return fail("an occupancy range has to end after it starts");
    }
    if (request.freq_high < request.freq_low) {
        return fail("an occupancy band has to have its high edge above its low edge");
    }
    const std::int64_t span = request.to_ns - request.from_ns;
    const std::int64_t count = (span + request.bucket_ns - 1) / request.bucket_ns;
    // A bucket list is something a person reads or a display draws; a
    // request for millions of buckets is a unit mistake, not a query.
    constexpr std::int64_t kMaxBuckets = 1'000'000;
    if (count > kMaxBuckets) {
        return fail(std::format("{} buckets were asked for, more than the {} allowed; is the "
                                "bucket in nanoseconds?",
                                count, kMaxBuckets));
    }

    // Every annotation and every segment that passes, as wall-clock spans,
    // collected once and clipped per bucket.
    struct Interval {
        std::int64_t start;
        std::int64_t end;
    };
    std::vector<Interval> present;
    std::vector<Interval> recorded;
    const std::string family = lowered(request.filter.family);
    for (const IndexedSegment& segment : segments) {
        const std::int64_t segment_start = segment.info.start_ns();
        const std::int64_t segment_end = wall_clock_at(segment.info, segment.info.sample_count);
        if (segment_end > request.from_ns && segment_start < request.to_ns) {
            recorded.push_back({segment_start, segment_end});
        }
        for (const StoredAnnotation& stored : segment.annotations) {
            const Annotation& a = stored.annotation;
            const std::int64_t start = wall_clock_at(segment.info, a.sample_start);
            const std::int64_t end = wall_clock_at(segment.info, a.sample_start + a.sample_count);
            if (end <= request.from_ns || start >= request.to_ns) {
                continue;
            }
            if (!overlaps_band(a, request.freq_low, request.freq_high)) {
                continue;
            }
            if (!passes_measures(stored, request.filter, end - start, family)) {
                continue;
            }
            present.push_back({start, end});
        }
    }

    std::vector<OccupancyBucket> buckets(static_cast<std::size_t>(count));
    std::vector<std::pair<std::int64_t, std::int64_t>> clipped;
    for (std::int64_t i = 0; i < count; ++i) {
        OccupancyBucket& bucket = buckets[static_cast<std::size_t>(i)];
        bucket.start_ns = request.from_ns + i * request.bucket_ns;
        bucket.end_ns = std::min(bucket.start_ns + request.bucket_ns, request.to_ns);
        const auto length = static_cast<double>(bucket.end_ns - bucket.start_ns);

        const auto fraction = [&](const std::vector<Interval>& intervals,
                                  std::uint32_t* touched) {
            clipped.clear();
            for (const Interval& interval : intervals) {
                const std::int64_t start = std::max(interval.start, bucket.start_ns);
                const std::int64_t end = std::min(interval.end, bucket.end_ns);
                if (end > start) {
                    clipped.emplace_back(start, end);
                }
            }
            if (touched != nullptr) {
                *touched = static_cast<std::uint32_t>(clipped.size());
            }
            return static_cast<double>(union_length(clipped)) / length;
        };
        bucket.occupied = fraction(present, &bucket.hits);
        bucket.captured = fraction(recorded, nullptr);
    }
    return buckets;
}

}  // namespace revenant::capture
