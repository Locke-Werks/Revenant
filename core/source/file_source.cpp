// The file backend.
//
// Two things here are not obvious from the header. The first is the delivery
// loop, which is four lines and is the whole flow-control story: fill a block,
// call the sink, stop if it complained. The sink is allowed to take as long as
// it likes and that is the backpressure. Nothing measures elapsed time unless
// StreamOptions::pace asks for it.
//
// The second is the anchor. A raw IQ file carries no header, so the moment
// sample zero was captured is either stated by the caller or guessed from the
// filesystem, and the difference between those two has to be visible
// downstream. An anchor with no stated accuracy is a lie, so the guess travels
// with an accuracy wide enough to actually contain the truth.

#include "core/source/file_source.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "core/source/clock_model.h"
#include "core/thread_role.h"

namespace revenant::source {
namespace {

// std::FILE rather than std::ifstream, and the difference is not small.
//
// Measured on this machine reading a 256 MiB file already in the page cache,
// 2 MiB at a time, three runs each:
//
//   std::ifstream::read   1.37 GB/s
//   std::fread            8.02 GB/s
//   ReadFile              8.17 GB/s
//
// MSVC's basic_filebuf puts a copy through its own buffer even for reads far
// larger than that buffer, and at 20 MS/s cf32 the difference is 8.6x
// realtime against 50x. The file source is supposed to be so far from the
// bottleneck that the GPU chain is the limit; through iostreams it is the
// limit. This is also worth knowing when reading the design's "~1.4 GB/s on
// this box" figure, which was measured through siggen's ofstream writer and
// is the same ceiling rather than the disk's.
struct FileCloser {
    void operator()(std::FILE* file) const noexcept
    {
        if (file != nullptr) {
            static_cast<void>(std::fclose(file));
        }
    }
};

using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

// Percent-decoded octets in a URI are UTF-8. filesystem::path's narrow
// constructor reads a std::string in the active code page instead, so a
// capture under a name with an accent in it would be looked for at a path
// nobody ever wrote. The char8_t overload says UTF-8 explicitly, and on
// Windows converts to the wide form the filesystem actually uses.
[[nodiscard]] std::filesystem::path path_of(const std::string& utf8)
{
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

// Everything after the last separator. Taken off the text rather than through
// filesystem::path::filename().string(), which converts back out of the
// native encoding and can throw on a name it cannot represent.
[[nodiscard]] std::string basename_of(const std::string& path)
{
    const std::size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

[[nodiscard]] Expected<FilePtr> open_binary(const std::string& path)
{
    std::FILE* raw = nullptr;
#if defined(_MSC_VER)
    // The wide form, so a path with a character outside the active code page
    // opens rather than failing with a message about a file that plainly
    // exists.
    const errno_t error = _wfopen_s(&raw, path_of(path).c_str(), L"rb");
    if (error != 0 || raw == nullptr) {
        return fail(std::format("could not open '{}' for reading", path), error);
    }
#else
    raw = std::fopen(path.c_str(), "rb");
    if (raw == nullptr) {
        return fail(std::format("could not open '{}' for reading", path));
    }
#endif
    return FilePtr(raw);
}

// 64-bit positioning. std::fseek takes a long, which is 32 bits on Windows
// and would cap a capture at 2 GiB.
[[nodiscard]] bool seek_absolute(std::FILE* file, std::int64_t offset)
{
#if defined(_MSC_VER)
    return _fseeki64(file, offset, SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

// 128 MiB of cf32. Past this a caller has confused a block with a buffer, and
// the allocation is large enough that failing loudly beats discovering it as a
// bad_alloc on a smaller machine.
constexpr std::size_t kMaxBlockSamples = 1u << 24;

// A quarter of a second at 20 MS/s, which is large enough that the per-block
// overhead disappears against the read and small enough that a blocking
// consumer's latency is still a fraction of a second.
constexpr std::size_t kDefaultBlockSamples = 1u << 18;

// Sentinel in the pending-seek slot. A seek to this index is not expressible
// in any file that fits on any filesystem, so it costs nothing to reserve.
constexpr dsp::SampleIndex kNoPendingSeek = std::numeric_limits<dsp::SampleIndex>::max();

[[nodiscard]] std::int64_t duration_ns_of(dsp::SampleIndex samples, dsp::SampleRate rate)
{
    if (rate <= 0) {
        return 0;
    }
    // Split, for the same overflow reason BlockTimestamp::wall_clock_ns
    // splits: samples * 1e9 leaves 64 bits fifteen minutes into a 20 MS/s
    // capture.
    const auto rate_u = static_cast<dsp::SampleIndex>(rate);
    const auto whole_seconds = static_cast<std::int64_t>(samples / rate_u);
    const auto remainder = static_cast<std::int64_t>(samples % rate_u);
    return whole_seconds * 1'000'000'000 + (remainder * 1'000'000'000) / rate;
}

struct DerivedAnchor {
    std::int64_t anchor_ns = 0;
    std::int64_t accuracy_ns = 0;
};

// The anchor when the caller did not supply one.
//
// The filesystem records when the file was last written, which for a file
// produced by a streaming writer is when the capture ENDED. So the estimate
// for sample zero is that time less the file's own duration. Every part of
// that is a guess: the file may have been copied, in which case the timestamp
// is when the copy ran; it may have been written in one burst from a buffer,
// in which case the duration subtraction is wrong by the whole duration; and
// a FAT volume stores modification times to two seconds.
//
// So the accuracy is the file's duration plus two seconds, which is wide
// enough to contain every one of those cases except an outright copy. It is
// deliberately not a small number. A confident anchor that is wrong by an hour
// is worse than a vague one that says so, because only the vague one stops a
// slotted-mode decoder from trusting it.
[[nodiscard]] Expected<DerivedAnchor> anchor_from_modification_time(const std::string& path,
                                                                    dsp::SampleIndex samples,
                                                                    dsp::SampleRate rate)
{
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(path_of(path), ec);
    if (ec) {
        return fail(std::format("could not read the modification time of '{}': {}", path,
                                ec.message()),
                    ec.value());
    }

    const auto as_system = std::chrono::clock_cast<std::chrono::system_clock>(written);
    const auto since_epoch =
        std::chrono::duration_cast<std::chrono::nanoseconds>(as_system.time_since_epoch());

    const std::int64_t duration_ns = duration_ns_of(samples, rate);

    DerivedAnchor out;
    out.anchor_ns = since_epoch.count() - duration_ns;
    out.accuracy_ns = duration_ns + 2'000'000'000;
    return out;
}

// ---------------------------------------------------------------------------
// Bytes
// ---------------------------------------------------------------------------

[[nodiscard]] Expected<std::uint64_t> file_size_of(const std::string& path)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path_of(path), ec);
    if (ec) {
        return fail(std::format("could not read the size of '{}': {}", path, ec.message()),
                    ec.value());
    }
    return static_cast<std::uint64_t>(size);
}

// Reads up to `count` bytes from `offset`. A short read is not an error here:
// the callers are sniffing a signature or walking chunk headers and each one
// says for itself how much it needed, which produces a better message than a
// generic "unexpected end of file".
[[nodiscard]] Expected<std::vector<std::byte>> read_region(const std::string& path,
                                                           std::uint64_t offset,
                                                           std::size_t count)
{
    auto opened = open_binary(path);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        !seek_absolute(opened->get(), static_cast<std::int64_t>(offset))) {
        return fail(std::format("could not seek '{}' to byte {}", path, offset));
    }

    std::vector<std::byte> out(count);
    const std::size_t got = std::fread(out.data(), 1, count, opened->get());
    out.resize(got);
    return out;
}

[[nodiscard]] std::uint16_t le16(std::span<const std::byte> bytes, std::size_t at)
{
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[at]) |
                                      (static_cast<std::uint16_t>(bytes[at + 1]) << 8));
}

[[nodiscard]] std::uint32_t le32(std::span<const std::byte> bytes, std::size_t at)
{
    std::uint32_t out = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        out |= static_cast<std::uint32_t>(bytes[at + i]) << (8 * i);
    }
    return out;
}

[[nodiscard]] std::uint64_t le64(std::span<const std::byte> bytes, std::size_t at)
{
    std::uint64_t out = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        out |= static_cast<std::uint64_t>(bytes[at + i]) << (8 * i);
    }
    return out;
}

[[nodiscard]] std::string_view four_cc(std::span<const std::byte> bytes, std::size_t at)
{
    return std::string_view(reinterpret_cast<const char*>(bytes.data()) + at, 4);
}

// What the clock behind a recorder's timestamp is actually worth.
//
// Both containers here carry a wall-clock time to a millisecond: SigMF's
// core:datetime is an ISO-8601 string and the auxi chunk's SYSTEMTIME has a
// milliseconds field. Neither field's resolution is its accuracy. The clock
// behind both is the recording PC's, which NTP holds to tens of milliseconds
// when it is running and which drifts through seconds when it is not, and
// nothing in either container says which of those was true.
//
// One second, therefore, and it is deliberately not smaller. FT8 and the
// other slotted modes need UTC to about a second, so this sits exactly on the
// boundary: a decoder reading it can see that the anchor is only just good
// enough rather than assuming it is comfortable. A caller who recorded
// against a disciplined reference overrides it with anchor_accuracy_ns.
constexpr std::int64_t kRecorderClockAccuracyNs = 1'000'000'000;

// Nanoseconds since the Unix epoch for a broken-down UTC time, or nothing
// when the fields do not name a real day.
[[nodiscard]] Expected<std::int64_t> utc_to_epoch_ns(int year, unsigned month, unsigned day,
                                                     int hour, int minute, int second,
                                                     std::int64_t sub_second_ns,
                                                     std::string_view what)
{
    const std::chrono::year_month_day ymd{std::chrono::year{year},
                                          std::chrono::month{month},
                                          std::chrono::day{day}};
    if (!ymd.ok()) {
        return fail(std::format("{} names {:04}-{:02}-{:02}, which is not a real date", what,
                                year, month, day));
    }
    // 60 is a leap second, which no container here is expected to carry and
    // which a reader that clamped would turn into a silent one-second error.
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return fail(std::format("{} names the time {:02}:{:02}:{:02}, which is not a real time "
                                "of day",
                                what, hour, minute, second));
    }

    const auto days = std::chrono::sys_days(ymd).time_since_epoch().count();
    const std::int64_t seconds = static_cast<std::int64_t>(days) * 86'400 +
                                 static_cast<std::int64_t>(hour) * 3'600 +
                                 static_cast<std::int64_t>(minute) * 60 +
                                 static_cast<std::int64_t>(second);
    return seconds * 1'000'000'000 + sub_second_ns;
}

// ---------------------------------------------------------------------------
// JSON, enough of it for a SigMF sidecar
// ---------------------------------------------------------------------------
//
// ECMA-404 and RFC 8259. Written here rather than pulled in because the
// alternative is a vcpkg dependency and a licence row for a file format whose
// whole grammar is six productions, and because the error messages a sidecar
// needs are about SigMF keys rather than about tokens.
//
// Integers are kept as integers alongside the double. core:sample_start is a
// sample index, and a 2 MS/s recording passes 2^53 after about a century,
// which is far away, but the same field also carries the byte counts a
// malformed sidecar can put in it and a silently rounded one of those is a
// seek to the wrong place.

struct JsonValue {
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    bool integral = false;
    std::int64_t integer = 0;
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

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    [[nodiscard]] Expected<JsonValue> parse_document()
    {
        skip_space();
        auto value = parse_value(0);
        if (!value) {
            return value;
        }
        skip_space();
        if (at_ != text_.size()) {
            return error("trailing text after the end of the document");
        }
        return value;
    }

private:
    // A sidecar is two levels of nesting. Anything past this is either
    // corrupt or hostile, and either way a recursive descent parser that
    // followed it would take the stack with it.
    static constexpr int kMaxDepth = 64;

    [[nodiscard]] std::unexpected<Error> error(std::string_view what) const
    {
        return fail(std::format("malformed JSON at byte {}: {}", at_, what));
    }

    void skip_space()
    {
        while (at_ < text_.size()) {
            const char c = text_[at_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++at_;
            } else {
                break;
            }
        }
    }

    [[nodiscard]] bool consume(char c)
    {
        if (at_ < text_.size() && text_[at_] == c) {
            ++at_;
            return true;
        }
        return false;
    }

    [[nodiscard]] Expected<JsonValue> parse_value(int depth);
    [[nodiscard]] Expected<std::string> parse_string();
    [[nodiscard]] Expected<JsonValue> parse_number();

    std::string_view text_;
    std::size_t at_ = 0;
};

Expected<std::string> JsonParser::parse_string()
{
    if (!consume('"')) {
        return error("expected a string");
    }

    std::string out;
    while (true) {
        if (at_ >= text_.size()) {
            return error("a string is not closed before the end of the document");
        }
        const char c = text_[at_++];
        if (c == '"') {
            return out;
        }
        if (static_cast<unsigned char>(c) < 0x20) {
            return error("a raw control character inside a string");
        }
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (at_ >= text_.size()) {
            return error("a string ends with a dangling escape");
        }
        const char esc = text_[at_++];
        switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (at_ + 4 > text_.size()) {
                    return error("a \\u escape is cut short");
                }
                std::uint32_t code = 0;
                for (int i = 0; i < 4; ++i) {
                    const char h = text_[at_ + static_cast<std::size_t>(i)];
                    int digit = -1;
                    if (h >= '0' && h <= '9') {
                        digit = h - '0';
                    } else if (h >= 'a' && h <= 'f') {
                        digit = h - 'a' + 10;
                    } else if (h >= 'A' && h <= 'F') {
                        digit = h - 'A' + 10;
                    }
                    if (digit < 0) {
                        return error("a \\u escape has a character that is not hexadecimal");
                    }
                    code = (code << 4) | static_cast<std::uint32_t>(digit);
                }
                at_ += 4;

                // A leading surrogate must be followed by its trailing half.
                // Emitting the halves separately would produce a string that
                // is not valid UTF-8, which then reaches a filesystem call as
                // a path nobody can open.
                if (code >= 0xD800 && code <= 0xDBFF) {
                    if (at_ + 6 > text_.size() || text_[at_] != '\\' || text_[at_ + 1] != 'u') {
                        return error("a leading surrogate is not followed by a trailing one");
                    }
                    std::uint32_t low = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = text_[at_ + 2 + static_cast<std::size_t>(i)];
                        int digit = -1;
                        if (h >= '0' && h <= '9') {
                            digit = h - '0';
                        } else if (h >= 'a' && h <= 'f') {
                            digit = h - 'a' + 10;
                        } else if (h >= 'A' && h <= 'F') {
                            digit = h - 'A' + 10;
                        }
                        if (digit < 0) {
                            return error("a trailing surrogate is not hexadecimal");
                        }
                        low = (low << 4) | static_cast<std::uint32_t>(digit);
                    }
                    if (low < 0xDC00 || low > 0xDFFF) {
                        return error("a leading surrogate is followed by something that is not "
                                     "a trailing surrogate");
                    }
                    at_ += 6;
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                } else if (code >= 0xDC00 && code <= 0xDFFF) {
                    return error("a trailing surrogate with no leading one before it");
                }

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
                break;
            }
            default:
                return error("an unknown escape sequence");
        }
    }
}

Expected<JsonValue> JsonParser::parse_number()
{
    const std::size_t start = at_;
    if (at_ < text_.size() && (text_[at_] == '-' || text_[at_] == '+')) {
        ++at_;
    }
    bool has_fraction = false;
    while (at_ < text_.size()) {
        const char c = text_[at_];
        if ((c >= '0' && c <= '9')) {
            ++at_;
        } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
            has_fraction = true;
            ++at_;
        } else {
            break;
        }
    }
    if (at_ == start) {
        return error("expected a number");
    }

    const std::string_view token = text_.substr(start, at_ - start);
    JsonValue value;
    value.kind = JsonValue::Kind::Number;

    const char* first = token.data();
    const char* last = token.data() + token.size();
    const auto parsed = std::from_chars(first, last, value.number);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return fail(std::format("malformed JSON at byte {}: '{}' is not a number", start, token));
    }

    if (!has_fraction) {
        std::int64_t as_integer = 0;
        const auto exact = std::from_chars(first, last, as_integer);
        if (exact.ec == std::errc{} && exact.ptr == last) {
            value.integral = true;
            value.integer = as_integer;
        }
    }
    return value;
}

Expected<JsonValue> JsonParser::parse_value(int depth)
{
    if (depth > kMaxDepth) {
        return error(std::format("nesting deeper than {} levels", kMaxDepth));
    }
    if (at_ >= text_.size()) {
        return error("the document ends where a value was expected");
    }

    const char c = text_[at_];
    if (c == '{') {
        ++at_;
        JsonValue object;
        object.kind = JsonValue::Kind::Object;
        skip_space();
        if (consume('}')) {
            return object;
        }
        while (true) {
            skip_space();
            auto key = parse_string();
            if (!key) {
                return std::unexpected(key.error());
            }
            skip_space();
            if (!consume(':')) {
                return error("expected ':' after an object key");
            }
            skip_space();
            auto value = parse_value(depth + 1);
            if (!value) {
                return value;
            }
            object.members.emplace_back(std::move(*key), std::move(*value));
            skip_space();
            if (consume(',')) {
                continue;
            }
            if (consume('}')) {
                return object;
            }
            return error("expected ',' or '}' in an object");
        }
    }

    if (c == '[') {
        ++at_;
        JsonValue array;
        array.kind = JsonValue::Kind::Array;
        skip_space();
        if (consume(']')) {
            return array;
        }
        while (true) {
            skip_space();
            auto value = parse_value(depth + 1);
            if (!value) {
                return value;
            }
            array.elements.push_back(std::move(*value));
            skip_space();
            if (consume(',')) {
                continue;
            }
            if (consume(']')) {
                return array;
            }
            return error("expected ',' or ']' in an array");
        }
    }

    if (c == '"') {
        auto text = parse_string();
        if (!text) {
            return std::unexpected(text.error());
        }
        JsonValue value;
        value.kind = JsonValue::Kind::String;
        value.text = std::move(*text);
        return value;
    }

    if (text_.compare(at_, 4, "true") == 0) {
        at_ += 4;
        JsonValue value;
        value.kind = JsonValue::Kind::Bool;
        value.boolean = true;
        return value;
    }
    if (text_.compare(at_, 5, "false") == 0) {
        at_ += 5;
        JsonValue value;
        value.kind = JsonValue::Kind::Bool;
        value.boolean = false;
        return value;
    }
    if (text_.compare(at_, 4, "null") == 0) {
        at_ += 4;
        return JsonValue{};
    }

    return parse_number();
}

// ---------------------------------------------------------------------------
// SigMF
// ---------------------------------------------------------------------------

// A sidecar is metadata. Anything this large is a data file someone renamed,
// and reading it whole to find that out would be the cost of the mistake
// rather than the report of it.
constexpr std::uint64_t kMaxSidecarBytes = 64ull * 1024 * 1024;

[[nodiscard]] Expected<std::string> read_text_file(const std::string& path)
{
    auto size = file_size_of(path);
    if (!size) {
        return std::unexpected(size.error());
    }
    if (*size > kMaxSidecarBytes) {
        return fail(std::format(
            "'{}' is {} bytes, past the {} byte ceiling for a SigMF sidecar. A sidecar is a few "
            "kilobytes of JSON; a file this size is the dataset, so check that meta= and the "
            "data path are not the same file.",
            path, *size, kMaxSidecarBytes));
    }

    auto bytes = read_region(path, 0, static_cast<std::size_t>(*size));
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    if (bytes->size() != static_cast<std::size_t>(*size)) {
        return fail(std::format("'{}' returned {} of its {} bytes", path, bytes->size(), *size));
    }

    std::string out(reinterpret_cast<const char*>(bytes->data()), bytes->size());

    // A BOM at the head of a UTF-8 JSON document is legal on disk and is not
    // whitespace to the parser, so it would be reported as a stray character
    // at byte zero rather than as what it is.
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return out;
}

// core:datatype, from the SigMF specification's datatype grammar:
// an optional 'r' or 'c' for real or complex, then 'f', 'i' or 'u', then the
// bit width, then '_le' or '_be'.
//
// This backend takes the four the engine's upload kernels convert, and every
// refusal names the string it found. Two of them matter more than the rest.
//
// A real datatype is not an error in the recording. It is a recording of
// something else: one stream of samples rather than an I and a Q, so reading
// it as interleaved IQ halves the rate and mirrors the spectrum, and the
// result is a plausible picture of a signal that was never transmitted.
//
// A big-endian datatype has every sample's bytes the other way round. Read as
// little-endian, a cs16 sample of 0x0102 becomes 0x0201, which is a different
// number in the same range, so there is no value anywhere in the file that
// looks wrong.
[[nodiscard]] Expected<SampleFormat> sigmf_datatype_to_format(std::string_view datatype)
{
    std::string_view rest = datatype;
    if (rest.starts_with("r")) {
        return fail(std::format(
            "SigMF core:datatype '{}' is a real datatype, and this backend reads complex IQ. "
            "Reading real samples as interleaved I and Q halves the sample rate and mirrors "
            "the spectrum, so it would produce a picture rather than a failure.",
            datatype));
    }
    if (rest.starts_with("c")) {
        rest.remove_prefix(1);
    } else {
        return fail(std::format(
            "SigMF core:datatype '{}' does not begin with 'c' or 'r', so it is not a datatype "
            "string the specification defines",
            datatype));
    }

    bool big_endian = false;
    if (rest.ends_with("_le")) {
        rest.remove_suffix(3);
    } else if (rest.ends_with("_be")) {
        rest.remove_suffix(3);
        big_endian = true;
    }

    if (big_endian && rest != "u8" && rest != "i8") {
        return fail(std::format(
            "SigMF core:datatype '{}' is big-endian and this backend reads little-endian only. "
            "Byte-swapped samples are still in range, so nothing downstream would notice: "
            "convert the dataset, or record it as c{}_le.",
            datatype, rest));
    }

    if (rest == "u8") {
        return SampleFormat::Cu8;
    }
    if (rest == "i8") {
        return SampleFormat::Cs8;
    }
    if (rest == "i16") {
        return SampleFormat::Cs16;
    }
    if (rest == "f32") {
        return SampleFormat::Cf32;
    }
    return fail(std::format(
        "SigMF core:datatype '{}' is not one this backend reads. It takes cu8, ci8, ci16_le and "
        "cf32_le, which are the formats the upload kernels convert.",
        datatype));
}

// core:datetime, which the specification defines as an ISO-8601 string in
// UTC. Parsed by hand rather than by a locale-sensitive time parser, and the
// trailing Z is required.
//
// A string with a local offset on it, or with no zone at all, read as UTC is
// an anchor wrong by whole hours, and an anchor wrong by whole hours places
// every slotted-mode decode in the wrong minute while looking exactly like a
// correct one.
[[nodiscard]] Expected<std::int64_t> parse_sigmf_datetime(std::string_view text)
{
    const auto digits = [&](std::size_t at, std::size_t count, int& out) -> bool {
        if (at + count > text.size()) {
            return false;
        }
        int value = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const char c = text[at + i];
            if (c < '0' || c > '9') {
                return false;
            }
            value = value * 10 + (c - '0');
        }
        out = value;
        return true;
    };

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    const bool shape = text.size() >= 20 && digits(0, 4, year) && text[4] == '-' &&
                       digits(5, 2, month) && text[7] == '-' && digits(8, 2, day) &&
                       text[10] == 'T' && digits(11, 2, hour) && text[13] == ':' &&
                       digits(14, 2, minute) && text[16] == ':' && digits(17, 2, second);
    if (!shape) {
        return fail(std::format(
            "SigMF core:datetime '{}' is not an ISO-8601 UTC timestamp. The shape the "
            "specification asks for is 2026-09-21T14:03:07.250Z.",
            text));
    }

    std::size_t at = 19;
    std::int64_t sub_second_ns = 0;
    if (at < text.size() && text[at] == '.') {
        ++at;
        std::int64_t scale = 100'000'000;
        std::size_t seen = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            if (scale > 0) {
                sub_second_ns += static_cast<std::int64_t>(text[at] - '0') * scale;
                scale /= 10;
            }
            ++at;
            ++seen;
        }
        if (seen == 0) {
            return fail(std::format(
                "SigMF core:datetime '{}' has a decimal point with no fractional seconds after "
                "it",
                text));
        }
    }

    if (at + 1 != text.size() || text[at] != 'Z') {
        return fail(std::format(
            "SigMF core:datetime '{}' does not end in 'Z'. The specification requires UTC, and "
            "a local time read as UTC is an anchor wrong by whole hours with nothing "
            "downstream able to tell.",
            text));
    }

    return utc_to_epoch_ns(year, static_cast<unsigned>(month), static_cast<unsigned>(day), hour,
                           minute, second, sub_second_ns,
                           std::format("SigMF core:datetime '{}'", text));
}

// The capability description and the anchor come out of one pass over the
// filesystem, so describe_sources() and open_source() cannot disagree about
// how long the file is or when it started.
struct ResolvedFile {
    SourceCapabilities caps;
    std::int64_t anchor_ns = 0;
    dsp::SampleIndex length_samples = 0;

    // The file the samples are in, which for SigMF is the dataset rather
    // than the path in the URI, and where in it sample zero of this stream
    // sits. A WAV's chunk headers and a chosen SigMF segment both land here,
    // so the delivery loop stays one multiply and an add.
    std::string data_path;
    std::uint64_t data_offset = 0;

    // Settled from the container and the URI together, which is why the
    // source reads them from here rather than from the config.
    dsp::SampleRate rate = 0;
    SampleFormat format = SampleFormat::Cf32;
    dsp::Hertz center_hz = 0;
};

[[nodiscard]] Expected<ResolvedFile> resolve_file(const FileSourceConfig& config);

class FileSource final : public Source {
public:
    FileSource() = default;
    ~FileSource() override;

    [[nodiscard]] Status open(const FileSourceConfig& config, ResolvedFile resolved);

    [[nodiscard]] const SourceCapabilities& capabilities() const override { return caps_; }

    [[nodiscard]] Expected<dsp::Hertz> tune(dsp::Hertz center) override;
    [[nodiscard]] dsp::Hertz center() const override { return center_hz_; }

    [[nodiscard]] Expected<dsp::SampleRate> set_sample_rate(dsp::SampleRate rate) override;
    [[nodiscard]] dsp::SampleRate sample_rate() const override { return rate_; }

    [[nodiscard]] Expected<double> set_gain(std::string_view stage, double db) override;
    [[nodiscard]] Status set_gain_auto(std::string_view stage, bool on) override;

    [[nodiscard]] Status start(const StreamOptions& options, BlockSink sink) override;
    [[nodiscard]] Status stop() override;
    [[nodiscard]] bool running() const override { return running_.load(std::memory_order_acquire); }

    [[nodiscard]] Status seek(dsp::SampleIndex index) override;

    [[nodiscard]] SourceStats stats() const override;
    [[nodiscard]] ClockQuality clock() const override;

    [[nodiscard]] std::optional<double> own_pace() const override;
    [[nodiscard]] Status set_pace(double pace) override;

private:
    void run();
    void join_locked();

    // Sleeps until `target` or until the pace changes or a stop is asked for,
    // whichever is first, and says whether the pace changed. In slices, so a
    // block held for a slow pace does not hold a faster one back for its whole
    // length.
    [[nodiscard]] bool sleep_for_pace(std::chrono::steady_clock::time_point target,
                                      double pace_in_force);

    SourceCapabilities caps_{};
    std::string path_{};
    dsp::SampleRate rate_ = 0;
    dsp::Hertz center_hz_ = 0;
    SampleFormat format_ = SampleFormat::Cf32;
    std::size_t bytes_per_sample_ = 0;
    dsp::SampleIndex length_samples_ = 0;

    // Where sample zero of this stream begins in the file. Nonzero for a WAV,
    // whose samples sit after the chunk headers, and for a SigMF capture
    // segment that does not start at the beginning of its dataset.
    std::uint64_t data_offset_ = 0;
    std::int64_t anchor_ns_ = 0;

    // Written once at open, read-only afterwards, so clock() needs no
    // synchronisation with the source thread. The one value that does move,
    // how far the stream has run, is read from write_index_ below instead of
    // being pushed into the model from the delivery loop.
    ClockModel clock_model_{};

    // Control plane only: start, stop, seek and the setters. The delivery loop
    // never takes it, which is what keeps the sample path free of a mutex.
    mutable std::mutex control_{};
    std::thread thread_{};
    BlockSink sink_{};
    FilePtr file_{};
    std::vector<std::byte> buffer_{};
    std::size_t block_samples_ = 0;

    // The URI's pace=, or the last set_pace. Under control_.
    std::optional<double> own_pace_{};

    // The pace in force. Atomic because set_pace moves it while the delivery
    // loop reads it on every block.
    std::atomic<double> pace_{0.0};

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<dsp::SampleIndex> pending_seek_{kNoPendingSeek};

    // Owned by the source thread while it runs and by the control plane when
    // it does not; the join is the handoff in both directions.
    dsp::SampleIndex position_ = 0;
    Error stop_error_{};
    bool has_stop_error_ = false;

    std::atomic<std::uint64_t> blocks_delivered_{0};
    std::atomic<std::uint64_t> samples_delivered_{0};
    std::atomic<dsp::SampleIndex> write_index_{0};
};

FileSource::~FileSource()
{
    std::scoped_lock lock(control_);
    join_locked();
}

void FileSource::join_locked()
{
    stop_requested_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

Status FileSource::open(const FileSourceConfig& config, ResolvedFile resolved)
{
    caps_ = std::move(resolved.caps);

    // The dataset rather than the URI's path, and the settled rate, format
    // and centre rather than the ones the URI happened to carry. A SigMF
    // sidecar may name a dataset with another name entirely, and may be the
    // only thing that said what rate the samples were taken at.
    path_ = resolved.data_path;
    rate_ = resolved.rate;
    center_hz_ = resolved.center_hz;
    format_ = resolved.format;
    bytes_per_sample_ = bytes_per_sample(resolved.format);
    length_samples_ = resolved.length_samples;
    data_offset_ = resolved.data_offset;
    anchor_ns_ = resolved.anchor_ns;

    if (config.pace_given) {
        own_pace_ = config.pace;
        pace_.store(config.pace, std::memory_order_release);
    }

    ClockModelConfig clock_config;
    clock_config.source = ClockSource::Internal;
    clock_config.rate = rate_;
    clock_config.anchor_accuracy_ns = caps_.timestamp_accuracy_ns;
    clock_config.oscillator_tolerance_ppm = config.ppm_uncertainty;

    auto model = ClockModel::create(clock_config);
    if (!model) {
        return std::unexpected(with_context(model.error(), "file source clock"));
    }
    clock_model_ = std::move(*model);
    if (auto anchored = clock_model_.set_anchor(anchor_ns_); !anchored) {
        return std::unexpected(with_context(anchored.error(), "file source clock"));
    }

    auto opened = open_binary(path_);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    file_ = std::move(*opened);
    return {};
}

Expected<dsp::Hertz> FileSource::tune(dsp::Hertz center)
{
    return fail(std::format(
        "the file source cannot tune: a recording's centre frequency is a property of the "
        "samples already on disk, not a setting. It declares no tune range, so "
        "SourceCapabilities::can_tune({}) is false. Reopen the URI with a different "
        "center= if the recorded value was wrong.",
        center));
}

Expected<dsp::SampleRate> FileSource::set_sample_rate(dsp::SampleRate rate)
{
    if (rate == rate_) {
        return rate_;
    }
    return fail(std::format(
        "the file source is fixed at {} S/s, which is what its URI declared the recording "
        "to be; {} S/s would reinterpret the same bytes as a different signal. Reopen with "
        "rate= if the declared value was wrong.",
        rate_, rate));
}

Expected<double> FileSource::set_gain(std::string_view stage, double)
{
    return fail(std::format(
        "the file source has no gain stage '{}': it declares none at all, because the gain "
        "that produced these samples was applied before they were written.",
        stage));
}

Status FileSource::set_gain_auto(std::string_view stage, bool)
{
    return fail(std::format("the file source has no gain stage '{}' to put into automatic mode",
                            stage));
}

Status FileSource::seek(dsp::SampleIndex index)
{
    std::scoped_lock lock(control_);

    if (length_samples_ > 0 && index > length_samples_) {
        return fail(std::format("cannot seek to sample {}: '{}' holds {} samples", index, path_,
                                length_samples_));
    }

    if (running_.load(std::memory_order_acquire)) {
        // Taken up by the delivery loop before it fills the next block, so a
        // seek never tears a block in half. The block that lands after it
        // carries the new index in its stamp, which is how a consumer sees
        // exactly where the seek went.
        pending_seek_.store(index, std::memory_order_release);
        return {};
    }

    position_ = index;
    write_index_.store(index, std::memory_order_relaxed);
    return {};
}

Status FileSource::start(const StreamOptions& options, BlockSink sink)
{
    std::scoped_lock lock(control_);

    if (running_.load(std::memory_order_acquire) || thread_.joinable()) {
        return fail(std::format("'{}' is already streaming", path_));
    }
    if (!sink) {
        return fail("a source cannot be started without a sink");
    }
    if (!std::isfinite(options.pace) || options.pace < 0.0) {
        return fail(std::format("pace must be zero for unthrottled or a positive multiple of "
                                "realtime, got {}",
                                options.pace));
    }
    if (length_samples_ > 0 && options.start_index > length_samples_) {
        return fail(std::format("cannot start at sample {}: '{}' holds {} samples",
                                options.start_index, path_, length_samples_));
    }

    std::size_t block = options.block_samples;
    if (block == 0) {
        block = caps_.preferred_block_samples;
    }
    if (block == 0) {
        block = kDefaultBlockSamples;
    }
    if (block > kMaxBlockSamples) {
        return fail(std::format("a block of {} samples is past the {} sample ceiling; that is a "
                                "buffer, not a block",
                                block, kMaxBlockSamples));
    }

    buffer_.assign(block * bytes_per_sample_, std::byte{0});
    block_samples_ = block;
    pace_.store(own_pace_.value_or(options.pace), std::memory_order_release);
    sink_ = std::move(sink);

    position_ = options.start_index;
    pending_seek_.store(kNoPendingSeek, std::memory_order_relaxed);
    stop_error_ = Error{};
    has_stop_error_ = false;

    blocks_delivered_.store(0, std::memory_order_relaxed);
    samples_delivered_.store(0, std::memory_order_relaxed);
    write_index_.store(position_, std::memory_order_relaxed);

    stop_requested_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);

    try {
        thread_ = std::thread([this] { run(); });
    } catch (const std::system_error& error) {
        running_.store(false, std::memory_order_release);
        return fail(std::format("could not start the source thread for '{}': {}", path_,
                                error.what()),
                    error.code().value());
    }
    return {};
}

Status FileSource::stop()
{
    std::scoped_lock lock(control_);
    join_locked();
    if (has_stop_error_) {
        return std::unexpected(stop_error_);
    }
    return {};
}

SourceStats FileSource::stats() const
{
    SourceStats out;
    out.blocks_delivered = blocks_delivered_.load(std::memory_order_relaxed);
    out.samples_delivered = samples_delivered_.load(std::memory_order_relaxed);

    // A Demand source cannot overrun. There is nowhere for a sample to be lost:
    // the sink either takes the block or stops the stream. These three stay
    // zero for the life of the source and the acceptance test asserts it.
    out.overrun_events = 0;
    out.samples_lost = 0;
    out.last_loss_index = 0;

    out.write_index = write_index_.load(std::memory_order_relaxed);
    return out;
}

std::optional<double> FileSource::own_pace() const
{
    std::scoped_lock lock(control_);
    return own_pace_;
}

Status FileSource::set_pace(double pace)
{
    if (!std::isfinite(pace) || pace < 0.0) {
        return fail(std::format("pace must be zero for unthrottled or a positive multiple of "
                                "realtime, got {}",
                                pace));
    }
    if (caps_.flow == FlowControl::Paced && pace == 0.0) {
        return fail("this file was opened with flow=paced, which plays it as a radio on its own "
                    "clock, so it cannot be unthrottled: reopen it without flow=paced");
    }
    std::scoped_lock lock(control_);
    own_pace_ = pace;
    pace_.store(pace, std::memory_order_release);
    return {};
}

bool FileSource::sleep_for_pace(std::chrono::steady_clock::time_point target,
                                double pace_in_force)
{
    using Clock = std::chrono::steady_clock;

    // Short enough that a change from a crawl to realtime is heard at once,
    // long enough that a realtime stream wakes a few times a block at most.
    constexpr auto kSlice = std::chrono::milliseconds(20);

    for (auto now = Clock::now(); now < target; now = Clock::now()) {
        if (stop_requested_.load(std::memory_order_acquire)) {
            return false;
        }
        if (pace_.load(std::memory_order_acquire) != pace_in_force) {
            return true;
        }
        std::this_thread::sleep_until(std::min(target, now + kSlice));
    }
    return false;
}

ClockQuality FileSource::clock() const
{
    // The model is immutable after open, so this reads it without a lock. The
    // only moving part is how far the stream has run, which the model would
    // otherwise have to be told from the delivery loop; taking it from the
    // published write index instead keeps the loop out of the model entirely.
    ClockQuality quality = clock_model_.quality();
    quality.accuracy_ns =
        clock_model_.accuracy_ns_at(write_index_.load(std::memory_order_relaxed));
    return quality;
}

void FileSource::run()
{
    name_this_thread(L"revenant source file");
    using Clock = std::chrono::steady_clock;

    std::uint64_t sequence = 0;
    bool seeked = true;  // forces the initial positioning of the stream

    // Wall clock at the edge, and only when pace was asked for. An unthrottled
    // Demand source never reads a clock, which is the reason it can outrun
    // realtime without anything in the code saying so.
    Clock::time_point pace_origin{};
    dsp::SampleIndex pace_origin_index = 0;
    double pace = pace_.load(std::memory_order_acquire);

    while (!stop_requested_.load(std::memory_order_acquire)) {
        const dsp::SampleIndex requested = pending_seek_.exchange(kNoPendingSeek,
                                                                  std::memory_order_acq_rel);
        if (requested != kNoPendingSeek) {
            position_ = requested;
            seeked = true;
        }

        if (seeked) {
            const auto offset = static_cast<std::int64_t>(
                data_offset_ + position_ * static_cast<dsp::SampleIndex>(bytes_per_sample_));
            if (!seek_absolute(file_.get(), offset)) {
                stop_error_ = Error{std::format("could not seek '{}' to sample {}", path_,
                                                position_)};
                has_stop_error_ = true;
                break;
            }
            seeked = false;

            // A seek resets the pacing origin so the throttle does not try to
            // catch up to where an unseeked stream would have been by now.
            pace_origin = Clock::now();
            pace_origin_index = position_;
        }

        if (length_samples_ > 0 && position_ >= length_samples_) {
            break;  // end of the recording, which is not an error
        }

        std::size_t want = block_samples_;
        if (length_samples_ > 0) {
            const dsp::SampleIndex remaining = length_samples_ - position_;
            if (remaining < static_cast<dsp::SampleIndex>(want)) {
                want = static_cast<std::size_t>(remaining);
            }
        }

        const std::size_t want_bytes = want * bytes_per_sample_;
        const std::size_t got_bytes = std::fread(buffer_.data(), 1, want_bytes, file_.get());

        if (got_bytes != want_bytes) {
            // The size was read at open, so a short read means the file
            // shrank underneath us. Reporting it beats delivering a truncated
            // block that looks like a clean end of stream.
            stop_error_ = Error{std::format(
                "'{}' returned {} of the {} bytes wanted at sample {}: the file changed size "
                "after it was opened",
                path_, got_bytes, want_bytes, position_)};
            has_stop_error_ = true;
            break;
        }

        SourceBlock block;
        block.stamp = dsp::BlockTimestamp{position_, anchor_ns_, rate_};
        block.format = format_;
        block.sample_count = want;
        block.bytes = std::span<const std::byte>(buffer_.data(), want_bytes);
        block.dropped_before = 0;
        block.sequence = sequence;

        // A CHANGE OF PACE RESTARTS THE STOPWATCH HERE, for the reason a seek
        // does: timed from the old origin at the new pace, a stream sped up
        // from realtime would find itself minutes behind and deliver them in
        // a burst, and one slowed down would go silent until the clock caught
        // up with where it already was.
        for (bool changed = true; changed;) {
            changed = false;
            if (const double now_pace = pace_.load(std::memory_order_acquire); now_pace != pace) {
                pace = now_pace;
                pace_origin = Clock::now();
                pace_origin_index = position_;
            }
            if (pace > 0.0) {
                const dsp::SampleIndex since = position_ - pace_origin_index;
                const double nominal_ns =
                    static_cast<double>(duration_ns_of(since, rate_)) / pace;
                const auto target =
                    pace_origin + std::chrono::nanoseconds(static_cast<std::int64_t>(nominal_ns));
                changed = sleep_for_pace(target, pace);
            }
        }
        if (stop_requested_.load(std::memory_order_acquire)) {
            break;
        }

        // Allowed to block for as long as it likes. That is the backpressure,
        // and it is the entire mechanism by which this source runs at exactly
        // the rate its consumer retires work.
        if (Status delivered = sink_(block); !delivered) {
            stop_error_ = delivered.error();
            has_stop_error_ = true;
            break;
        }

        position_ += want;
        ++sequence;
        blocks_delivered_.fetch_add(1, std::memory_order_relaxed);
        samples_delivered_.fetch_add(want, std::memory_order_relaxed);
        write_index_.store(position_, std::memory_order_relaxed);
    }

    running_.store(false, std::memory_order_release);
}

}  // namespace

Expected<SampleFormat> sample_format_from_name(std::string_view name)
{
    if (name == "cu8") {
        return SampleFormat::Cu8;
    }
    if (name == "cs8") {
        return SampleFormat::Cs8;
    }
    if (name == "cs16") {
        return SampleFormat::Cs16;
    }
    if (name == "cf32") {
        return SampleFormat::Cf32;
    }
    if (name == "cs24") {
        return SampleFormat::Cs24;
    }
    return fail(std::format("unknown sample format '{}', expected cu8, cs8, cs16, cs24 or cf32",
                            name));
}

Expected<SampleFormat> sample_format_from_extension(std::string_view path)
{
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 >= path.size()) {
        return fail(std::format(
            "'{}' has no extension to infer a sample format from, so the URI has to say "
            "format=cu8, cs8, cs16, cs24 or cf32",
            path));
    }

    const std::string_view extension = path.substr(dot + 1);
    auto known = sample_format_from_name(extension);
    if (known) {
        return known;
    }
    return fail(std::format(
        "'{}' does not name a sample format, so the URI has to say format=cu8, cs8, cs16, cs24 "
        "or cf32 rather than leaving it to be guessed from the extension",
        extension));
}

const char* container_name(Container container)
{
    switch (container) {
        case Container::Auto: return "auto";
        case Container::Raw: return "raw";
        case Container::Sigmf: return "sigmf";
        case Container::Wav: return "wav";
    }
    return "unknown";
}

Expected<Container> container_from_name(std::string_view name)
{
    if (name == "auto") {
        return Container::Auto;
    }
    if (name == "raw") {
        return Container::Raw;
    }
    if (name == "sigmf") {
        return Container::Sigmf;
    }
    if (name == "wav") {
        return Container::Wav;
    }
    return fail(std::format("unknown container '{}', expected auto, raw, sigmf or wav", name));
}

std::string sigmf_meta_path_for(std::string_view data_path)
{
    constexpr std::string_view kDataSuffix = ".sigmf-data";
    if (data_path.ends_with(kDataSuffix)) {
        std::string out(data_path.substr(0, data_path.size() - kDataSuffix.size()));
        out += ".sigmf-meta";
        return out;
    }
    // The specification's non-conforming dataset case: the sidecar keeps its
    // own name and the dataset is named inside it, so the sidecar for an
    // arbitrary file is that file's name with the suffix appended.
    return std::string(data_path) + ".sigmf-meta";
}

Expected<RecordingMetadata> read_sigmf_metadata(const std::string& meta_path)
{
    auto text = read_text_file(meta_path);
    if (!text) {
        return std::unexpected(with_context(text.error(), "SigMF sidecar"));
    }

    JsonParser parser(*text);
    auto document = parser.parse_document();
    if (!document) {
        return std::unexpected(
            with_context(document.error(), std::format("SigMF sidecar '{}'", meta_path)));
    }
    if (document->kind != JsonValue::Kind::Object) {
        return fail(std::format("SigMF sidecar '{}' is not a JSON object", meta_path));
    }

    const JsonValue* global = document->member("global");
    if (global == nullptr || global->kind != JsonValue::Kind::Object) {
        return fail(std::format(
            "SigMF sidecar '{}' has no 'global' object. The specification makes global, "
            "captures and annotations the three top-level keys, and global is where the "
            "datatype and the sample rate live.",
            meta_path));
    }

    // core:version. Accepting anything is how a reader ends up applying a
    // version 1 grammar to a version 2 document, so the major is checked and
    // named.
    const JsonValue* version = global->member("core:version");
    if (version == nullptr || version->kind != JsonValue::Kind::String) {
        return fail(std::format(
            "SigMF sidecar '{}' has no global core:version string, which the specification "
            "requires",
            meta_path));
    }
    if (!version->text.starts_with("1.")) {
        return fail(std::format(
            "SigMF sidecar '{}' declares core:version '{}'. This reader implements the "
            "version 1 metadata format, and a different major version is a different grammar "
            "rather than a superset of this one.",
            meta_path, version->text));
    }

    const JsonValue* metadata_only = global->member("core:metadata_only");
    if (metadata_only != nullptr && metadata_only->kind == JsonValue::Kind::Bool &&
        metadata_only->boolean) {
        return fail(std::format(
            "SigMF sidecar '{}' sets core:metadata_only, which declares that there is no "
            "dataset to play",
            meta_path));
    }

    const JsonValue* datatype = global->member("core:datatype");
    if (datatype == nullptr || datatype->kind != JsonValue::Kind::String) {
        return fail(std::format(
            "SigMF sidecar '{}' has no global core:datatype string, which the specification "
            "requires and which is the only thing that says how wide a sample is",
            meta_path));
    }
    auto format = sigmf_datatype_to_format(datatype->text);
    if (!format) {
        return std::unexpected(
            with_context(format.error(), std::format("SigMF sidecar '{}'", meta_path)));
    }

    const JsonValue* channels = global->member("core:num_channels");
    if (channels != nullptr && channels->kind == JsonValue::Kind::Number &&
        !(channels->integral && channels->integer == 1)) {
        return fail(std::format(
            "SigMF sidecar '{}' declares core:num_channels {}. A multi-channel dataset "
            "interleaves the channels sample by sample, so reading it as one stream would mix "
            "them together; this backend reads single-channel datasets.",
            meta_path, channels->number));
    }

    RecordingMetadata out;
    out.container = Container::Sigmf;
    out.has_format = true;
    out.format = *format;

    const JsonValue* rate = global->member("core:sample_rate");
    if (rate != nullptr && rate->kind == JsonValue::Kind::Number) {
        // Integer hertz, and a fractional rate is refused rather than
        // rounded. See docs/conventions.md: rounding a rate once puts every
        // frequency derived from it off by the same ratio, and the error is
        // not reproducible across machines because nothing says which way it
        // rounded.
        const double value = rate->number;
        if (!(value > 0.0) || value != std::floor(value) ||
            value > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            return fail(std::format(
                "SigMF sidecar '{}' declares core:sample_rate {}, which is not a positive "
                "whole number of samples per second. Frequency and rate are integers "
                "throughout this engine so that no stage has to be trusted to round the same "
                "way twice: state rate= in the URI to say which integer was meant.",
                meta_path, value));
        }
        out.has_rate = true;
        out.rate = static_cast<dsp::SampleRate>(value);
    }

    std::uint64_t trailing_bytes = 0;
    const JsonValue* trailing = global->member("core:trailing_bytes");
    if (trailing != nullptr && trailing->kind == JsonValue::Kind::Number) {
        if (!trailing->integral || trailing->integer < 0) {
            return fail(std::format("SigMF sidecar '{}' declares a core:trailing_bytes that is "
                                    "not a non-negative integer",
                                    meta_path));
        }
        trailing_bytes = static_cast<std::uint64_t>(trailing->integer);
    }

    std::int64_t dataset_first_index = 0;
    const JsonValue* offset = global->member("core:offset");
    if (offset != nullptr && offset->kind == JsonValue::Kind::Number) {
        if (!offset->integral || offset->integer < 0) {
            return fail(std::format(
                "SigMF sidecar '{}' declares a core:offset that is not a non-negative integer",
                meta_path));
        }
        dataset_first_index = offset->integer;
    }

    // The dataset. core:dataset names one whose name does not follow the
    // convention, and the specification resolves it relative to the sidecar's
    // own directory rather than to the process's working directory.
    const std::filesystem::path meta_dir = path_of(meta_path).parent_path();
    const JsonValue* dataset = global->member("core:dataset");
    if (dataset != nullptr && dataset->kind == JsonValue::Kind::String) {
        if (dataset->text.empty()) {
            return fail(std::format("SigMF sidecar '{}' declares an empty core:dataset",
                                    meta_path));
        }
        const std::filesystem::path named = path_of(dataset->text);
        const std::filesystem::path resolved =
            named.is_absolute() ? named : (meta_dir / named);
        const std::u8string utf8 = resolved.generic_u8string();
        out.data_path.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    } else {
        constexpr std::string_view kMetaSuffix = ".sigmf-meta";
        if (!std::string_view(meta_path).ends_with(kMetaSuffix)) {
            return fail(std::format(
                "SigMF sidecar '{}' does not end in .sigmf-meta and declares no core:dataset, "
                "so nothing says which file holds the samples",
                meta_path));
        }
        out.data_path = meta_path.substr(0, meta_path.size() - kMetaSuffix.size());
        out.data_path += ".sigmf-data";
    }

    auto data_size = file_size_of(out.data_path);
    if (!data_size) {
        return std::unexpected(with_context(
            data_size.error(),
            std::format("the dataset named by SigMF sidecar '{}'", meta_path)));
    }
    if (trailing_bytes > *data_size) {
        return fail(std::format(
            "SigMF sidecar '{}' declares {} trailing bytes but its dataset '{}' holds only {} "
            "bytes in total",
            meta_path, trailing_bytes, out.data_path, *data_size));
    }
    out.data_offset = 0;
    out.data_bytes = *data_size - trailing_bytes;

    const std::size_t bps = bytes_per_sample(*format);
    const std::uint64_t total_samples = bps == 0 ? 0 : out.data_bytes / bps;

    // The captures array.
    const JsonValue* captures = document->member("captures");
    if (captures == nullptr || captures->kind != JsonValue::Kind::Array) {
        return fail(std::format(
            "SigMF sidecar '{}' has no 'captures' array. That array is where the centre "
            "frequency and the start time live, and a recording with none has said nothing "
            "about where its receiver was pointed.",
            meta_path));
    }

    std::vector<CaptureSegment> segments;
    for (std::size_t i = 0; i < captures->elements.size(); ++i) {
        const JsonValue& entry = captures->elements[i];
        if (entry.kind != JsonValue::Kind::Object) {
            return fail(std::format("SigMF sidecar '{}': captures[{}] is not an object",
                                    meta_path, i));
        }

        const JsonValue* start = entry.member("core:sample_start");
        if (start == nullptr || start->kind != JsonValue::Kind::Number || !start->integral ||
            start->integer < 0) {
            return fail(std::format(
                "SigMF sidecar '{}': captures[{}] has no core:sample_start integer, which the "
                "specification makes the one required field of a capture segment",
                meta_path, i));
        }
        if (start->integer < dataset_first_index) {
            return fail(std::format(
                "SigMF sidecar '{}': captures[{}] starts at sample {}, before the dataset's "
                "own core:offset of {}",
                meta_path, i, start->integer, dataset_first_index));
        }

        const JsonValue* header_bytes = entry.member("core:header_bytes");
        if (header_bytes != nullptr && header_bytes->kind == JsonValue::Kind::Number &&
            !(header_bytes->integral && header_bytes->integer == 0)) {
            return fail(std::format(
                "SigMF sidecar '{}': captures[{}] declares core:header_bytes {}. This reader "
                "computes a sample's position as its index times the sample width, which a "
                "per-segment header displaces; supporting it means a byte map rather than a "
                "multiply, and nothing has needed one yet.",
                meta_path, i, header_bytes->number));
        }

        CaptureSegment segment;
        segment.sample_start = static_cast<dsp::SampleIndex>(start->integer - dataset_first_index);

        const JsonValue* frequency = entry.member("core:frequency");
        if (frequency != nullptr && frequency->kind == JsonValue::Kind::Number) {
            const double value = frequency->number;
            if (!std::isfinite(value) ||
                std::abs(value) > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
                return fail(std::format(
                    "SigMF sidecar '{}': captures[{}] declares core:frequency {}, which is not "
                    "a frequency",
                    meta_path, i, value));
            }
            if (value != std::floor(value)) {
                return fail(std::format(
                    "SigMF sidecar '{}': captures[{}] declares core:frequency {}, which is not "
                    "a whole number of hertz. This engine counts frequency in integer hertz so "
                    "that a tuning offset is exact and traceable: state center= in the URI to "
                    "say which hertz was meant.",
                    meta_path, i, value));
            }
            segment.has_center = true;
            segment.center_hz = static_cast<dsp::Hertz>(value);
        }

        const JsonValue* datetime = entry.member("core:datetime");
        if (datetime != nullptr && datetime->kind == JsonValue::Kind::String) {
            auto anchor = parse_sigmf_datetime(datetime->text);
            if (!anchor) {
                return std::unexpected(with_context(
                    anchor.error(), std::format("SigMF sidecar '{}': captures[{}]", meta_path, i)));
            }
            segment.has_anchor = true;
            segment.anchor_ns = *anchor;
        }

        if (!segments.empty() && segment.sample_start <= segments.back().sample_start) {
            return fail(std::format(
                "SigMF sidecar '{}': captures[{}] starts at sample {}, which is not after "
                "captures[{}] at sample {}. The specification requires the array to be sorted "
                "by core:sample_start with no duplicates, and an unsorted one gives a segment "
                "a negative length.",
                meta_path, i, segment.sample_start, i - 1, segments.back().sample_start));
        }
        segments.push_back(segment);
    }

    if (segments.empty()) {
        // Legal, and it means the recording never said where its receiver was
        // pointed. One segment covering the whole dataset keeps everything
        // below this uniform, and the note is what reaches the operator.
        CaptureSegment whole;
        whole.sample_start = 0;
        segments.push_back(whole);
        out.notes.emplace_back(std::format(
            "'{}' has an empty captures array, so the recording states no centre frequency and "
            "no start time. center= in the URI is the only thing that can supply one.",
            meta_path));
    }

    if (segments.back().sample_start >= total_samples) {
        return fail(std::format(
            "SigMF sidecar '{}' describes a capture starting at sample {}, but its dataset "
            "'{}' holds {} bytes, which is {} samples of {}. The metadata and the data "
            "disagree: either the recording was cut short or the sidecar belongs to another "
            "file.",
            meta_path, segments.back().sample_start, out.data_path, out.data_bytes,
            total_samples, format_name(*format)));
    }

    for (std::size_t i = 0; i < segments.size(); ++i) {
        const dsp::SampleIndex end =
            (i + 1 < segments.size()) ? segments[i + 1].sample_start : total_samples;
        segments[i].sample_count = end - segments[i].sample_start;
    }

    out.segments = std::move(segments);
    out.has_center = out.segments.front().has_center;
    out.center_hz = out.segments.front().center_hz;
    out.has_anchor = out.segments.front().has_anchor;
    out.anchor_ns = out.segments.front().anchor_ns;
    out.anchor_accuracy_ns = kRecorderClockAccuracyNs;
    return out;
}

// ---------------------------------------------------------------------------
// RIFF WAV, and the RF64 and BW64 extensions
// ---------------------------------------------------------------------------
//
// The container is Microsoft and IBM's "Multimedia Programming Interface and
// Data Specifications 1.0", August 1991: a four-character chunk id, a 32-bit
// little-endian size, the payload, and a pad byte when the size is odd. The
// fmt chunk is WAVEFORMATEX and WAVEFORMATEXTENSIBLE, documented in the Win32
// audio reference.
//
// RF64 is EBU Tech 3306, "MBWF/RF64: An extended File Format for Audio". BW64
// is ITU-R BS.2088, the same construction under another name. Both replace
// the RIFF signature, set the 32-bit sizes to the all-ones sentinel, and put
// the real 64-bit sizes in a ds64 chunk that comes first. That is the whole
// mechanism for going past 4 GB, which at 2 MS/s cs16 is eight and a half
// minutes.
//
// THE auxi CHUNK HAS NO SPECIFICATION AND THIS READER SAYS SO RATHER THAN
// PRETENDING OTHERWISE. It is the de-facto extension the SDR# and HDSDR
// family of recorders write, carrying a start time, a stop time and a centre
// frequency. There is no standards body behind it and no vendor document to
// cite a clause of, so the field offsets below are a layout this reader
// believes only as far as it can corroborate: the sample rate it carries is
// cross-checked against the fmt chunk and a disagreement is refused, the
// times are rejected unless they name real dates, and a centre frequency of
// zero is treated as absent rather than as DC. The two SYSTEMTIME blocks are
// the Win32 structure of that name, which is eight 16-bit fields in the order
// year, month, day of week, day, hour, minute, second, millisecond.

namespace {

struct WavFormat {
    std::uint16_t tag = 0;
    std::uint16_t channels = 0;
    std::uint32_t rate = 0;
    std::uint16_t block_align = 0;
    std::uint16_t bits = 0;
};

}  // namespace

Expected<RecordingMetadata> read_wav_metadata(const std::string& path)
{
    auto total_size = file_size_of(path);
    if (!total_size) {
        return std::unexpected(total_size.error());
    }

    auto header = read_region(path, 0, 12);
    if (!header) {
        return std::unexpected(header.error());
    }
    if (header->size() < 12) {
        return fail(std::format(
            "'{}' holds {} bytes, which is not even the twelve a RIFF header needs", path,
            header->size()));
    }

    const std::string_view signature = four_cc(*header, 0);
    if (signature == "RIFX") {
        return fail(std::format(
            "'{}' is RIFX, the big-endian variant of RIFF. Every multi-byte field in it is "
            "byte-swapped, samples included, so reading it as RIFF would produce numbers that "
            "are all in range and all wrong. Convert it to RIFF, or to a raw file with "
            "format= stated.",
            path));
    }
    const bool is_rf64 = signature == "RF64" || signature == "BW64";
    if (signature != "RIFF" && !is_rf64) {
        return fail(std::format(
            "'{}' begins with '{}' rather than RIFF, RF64 or BW64, so it is not a WAV file",
            path, signature));
    }
    if (four_cc(*header, 8) != "WAVE") {
        return fail(std::format("'{}' is a {} file but its form type is '{}' rather than WAVE",
                                path, signature, four_cc(*header, 8)));
    }

    constexpr std::uint32_t kSizeSentinel = 0xFFFFFFFFu;
    if (!is_rf64 && *total_size > kSizeSentinel) {
        return fail(std::format(
            "'{}' is {} bytes and begins with RIFF, whose size field is 32 bits and cannot "
            "count past {}. A recording this long is written as RF64 or BW64, so this file's "
            "declared sizes have already wrapped and nothing in it can be trusted.",
            path, *total_size, kSizeSentinel));
    }

    bool have_format = false;
    WavFormat format{};
    bool have_data = false;
    std::uint64_t data_offset = 0;
    std::uint64_t data_bytes = 0;
    bool have_ds64 = false;
    std::uint64_t ds64_data_bytes = 0;

    bool have_auxi = false;
    std::uint32_t auxi_center = 0;
    std::uint32_t auxi_rate = 0;
    bool auxi_has_start = false;
    std::int64_t auxi_start_ns = 0;

    std::vector<std::string> notes;

    std::uint64_t at = 12;
    while (at + 8 <= *total_size) {
        auto chunk = read_region(path, at, 8);
        if (!chunk) {
            return std::unexpected(chunk.error());
        }
        if (chunk->size() < 8) {
            return fail(std::format(
                "'{}' ends in the middle of a chunk header at byte {}", path, at));
        }

        const std::string id(four_cc(*chunk, 0));
        const std::uint32_t declared = le32(*chunk, 4);
        const std::uint64_t body = at + 8;

        std::uint64_t size = declared;
        if (declared == kSizeSentinel && have_ds64 && id == "data") {
            size = ds64_data_bytes;
        }

        if (id == "ds64") {
            if (!is_rf64) {
                notes.emplace_back(std::format(
                    "'{}' carries a ds64 chunk but its signature is RIFF rather than RF64 or "
                    "BW64, so the 64-bit sizes in it are advisory",
                    path));
            }
            if (size < 28) {
                return fail(std::format(
                    "'{}' has a ds64 chunk of {} bytes, and EBU Tech 3306 puts the riff size, "
                    "the data size, the sample count and the table length in the first 28",
                    path, size));
            }
            auto payload = read_region(path, body, 28);
            if (!payload) {
                return std::unexpected(payload.error());
            }
            if (payload->size() < 28) {
                return fail(std::format("'{}' ends inside its ds64 chunk", path));
            }
            have_ds64 = true;
            ds64_data_bytes = le64(*payload, 8);
        } else if (id == "fmt ") {
            if (size < 16) {
                return fail(std::format(
                    "'{}' has a fmt chunk of {} bytes, and WAVEFORMATEX is 16 before any "
                    "extension",
                    path, size));
            }
            const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(size, 40));
            auto payload = read_region(path, body, want);
            if (!payload) {
                return std::unexpected(payload.error());
            }
            if (payload->size() < 16) {
                return fail(std::format("'{}' ends inside its fmt chunk", path));
            }
            have_format = true;
            format.tag = le16(*payload, 0);
            format.channels = le16(*payload, 2);
            format.rate = le32(*payload, 4);
            format.block_align = le16(*payload, 12);
            format.bits = le16(*payload, 14);

            // WAVE_FORMAT_EXTENSIBLE moves the real format into the first two
            // bytes of the SubFormat GUID, which begins at offset 24 of the
            // chunk. Reading the tag alone would see 0xFFFE and refuse a file
            // that is ordinary 16-bit PCM.
            if (format.tag == 0xFFFE) {
                if (payload->size() < 26) {
                    return fail(std::format(
                        "'{}' declares WAVE_FORMAT_EXTENSIBLE but its fmt chunk is {} bytes, "
                        "too short to hold the SubFormat GUID that says what the samples "
                        "actually are",
                        path, payload->size()));
                }
                format.tag = le16(*payload, 24);
            }
        } else if (id == "auxi") {
            // 60 bytes: two SYSTEMTIME blocks of sixteen, then seven 32-bit
            // fields. Writers append more and this reader reads none of it.
            constexpr std::size_t kAuxiCore = 60;
            if (size < kAuxiCore) {
                notes.emplace_back(std::format(
                    "'{}' has an auxi chunk of {} bytes, short of the {} the SDR recorders "
                    "write, so its centre frequency and start time were not read",
                    path, size, kAuxiCore));
            } else {
                auto payload = read_region(path, body, kAuxiCore);
                if (!payload) {
                    return std::unexpected(payload.error());
                }
                if (payload->size() < kAuxiCore) {
                    return fail(std::format("'{}' ends inside its auxi chunk", path));
                }
                have_auxi = true;
                auxi_center = le32(*payload, 32);
                auxi_rate = le32(*payload, 36);

                const std::uint16_t year = le16(*payload, 0);
                if (year != 0) {
                    auto anchor = utc_to_epoch_ns(
                        year, le16(*payload, 2), le16(*payload, 6), le16(*payload, 8),
                        le16(*payload, 10), le16(*payload, 12),
                        static_cast<std::int64_t>(le16(*payload, 14)) * 1'000'000,
                        std::format("the auxi start time in '{}'", path));
                    if (!anchor) {
                        return std::unexpected(anchor.error());
                    }
                    auxi_has_start = true;
                    auxi_start_ns = *anchor;
                }
            }
        } else if (id == "data") {
            have_data = true;
            data_offset = body;
            data_bytes = size;
        }

        // Chunks are padded to an even boundary, and the pad byte is not
        // counted in the size. A reader that forgets it walks one byte into
        // every following chunk id.
        const std::uint64_t advance = size + (size % 2);
        if (advance > *total_size - body) {
            break;
        }
        at = body + advance;
    }

    if (!have_format) {
        return fail(std::format("'{}' has no fmt chunk, so nothing in it says what a sample is",
                                path));
    }
    if (!have_data) {
        return fail(std::format("'{}' has no data chunk", path));
    }

    // The refusal the task of reading an untrusted container turns on. A data
    // chunk declaring more bytes than the file holds is either truncated or
    // not the file the header describes, and reading to the end of what is
    // there would deliver whatever follows as samples.
    if (data_bytes > *total_size - data_offset) {
        return fail(std::format(
            "'{}' has a data chunk at byte {} declaring {} bytes, but the file holds only {} "
            "bytes in total, which leaves {}. The recording was cut short or the header "
            "belongs to another file; reading to the end of what is there would deliver the "
            "difference as samples.",
            path, data_offset, data_bytes, *total_size, *total_size - data_offset));
    }

    if (format.channels != 2) {
        return fail(std::format(
            "'{}' declares {} channels in its fmt chunk. An IQ recording is two, I and Q "
            "interleaved; one is an audio recording and more is a multi-receiver file whose "
            "channels this reader would mix together.",
            path, format.channels));
    }

    SampleFormat sample_format = SampleFormat::Cf32;
    constexpr std::uint16_t kFormatPcm = 1;
    constexpr std::uint16_t kFormatFloat = 3;
    if (format.tag == kFormatPcm && format.bits == 8) {
        // The RIFF specification makes 8-bit PCM unsigned offset binary and
        // everything wider signed two's complement, which is exactly the
        // split between this engine's cu8 and cs16.
        sample_format = SampleFormat::Cu8;
    } else if (format.tag == kFormatPcm && format.bits == 16) {
        sample_format = SampleFormat::Cs16;
    } else if (format.tag == kFormatPcm && format.bits == 24) {
        // What HF recorders write. Three bytes a component, packed, which is
        // cs24 exactly; core/shaders/convert_cs24_cf32.comp widens it on the
        // device like the other three, so this is no host pass.
        sample_format = SampleFormat::Cs24;
    } else if (format.tag == kFormatFloat && format.bits == 32) {
        sample_format = SampleFormat::Cf32;
    } else {
        return fail(std::format(
            "'{}' declares format tag {} at {} bits per sample, which is not one this backend "
            "reads. It takes 8-bit, 16-bit and 24-bit PCM and 32-bit IEEE float, which are the "
            "widths the upload kernels convert.",
            path, format.tag, format.bits));
    }

    const std::uint16_t expected_align =
        static_cast<std::uint16_t>(format.channels * (format.bits / 8));
    if (format.block_align != 0 && format.block_align != expected_align) {
        return fail(std::format(
            "'{}' declares {} bits across {} channels, which is {} bytes per frame, but its "
            "block align is {}. A frame laid out differently from its own arithmetic would be "
            "read at the wrong stride.",
            path, format.bits, format.channels, expected_align, format.block_align));
    }

    RecordingMetadata out;
    out.container = Container::Wav;
    out.data_path = path;
    out.data_offset = data_offset;
    out.data_bytes = data_bytes;
    out.has_format = true;
    out.format = sample_format;
    out.has_rate = format.rate > 0;
    out.rate = static_cast<dsp::SampleRate>(format.rate);
    out.notes = std::move(notes);

    if (have_auxi) {
        if (auxi_rate != 0 && format.rate != 0 && auxi_rate != format.rate) {
            return fail(std::format(
                "'{}' declares {} samples per second in its fmt chunk and {} in its auxi "
                "chunk. One of the two is the rate the samples were taken at and nothing here "
                "can tell which, so every frequency derived from the wrong one would be off by "
                "the ratio: state rate= in the URI to settle it.",
                path, format.rate, auxi_rate));
        }
        if (auxi_center != 0) {
            out.has_center = true;
            out.center_hz = static_cast<dsp::Hertz>(auxi_center);
        } else {
            out.notes.emplace_back(std::format(
                "'{}' has an auxi chunk whose centre frequency is zero, which the recorders "
                "write when they had nothing to put there. center= in the URI is the only "
                "thing that can supply one.",
                path));
        }
        if (auxi_has_start) {
            out.has_anchor = true;
            out.anchor_ns = auxi_start_ns;
            out.anchor_accuracy_ns = kRecorderClockAccuracyNs;
        }
    } else {
        // Perseus and the other recorders that write a private chunk land
        // here. The samples are read correctly and the centre frequency is
        // not in anything this reader understands, which is a thing to say
        // rather than a zero to pass off as DC.
        out.notes.emplace_back(std::format(
            "'{}' is a WAV with no auxi chunk, so it carries no centre frequency this reader "
            "understands. The rate and the sample format came from its fmt chunk; center= in "
            "the URI is the only thing that can supply the frequency.",
            path));
    }

    return out;
}

namespace {

// Which container the bytes are in, when the URI did not say.
//
// The signature is read from the file rather than inferred from the
// extension. A .dat written by a WAV recorder is still a WAV, a .wav holding
// raw IQ is not one, and an extension is the part of a filename most likely
// to have been changed by whoever copied it.
[[nodiscard]] Expected<Container> sniff_container(const std::string& path,
                                                  const std::string& explicit_meta)
{
    if (!explicit_meta.empty()) {
        return Container::Sigmf;
    }

    const std::string_view text = path;
    if (text.ends_with(".sigmf-meta") || text.ends_with(".sigmf-data")) {
        // Named as SigMF, so a missing sidecar is a fault to report rather
        // than a reason to fall back to reading the dataset raw. Falling back
        // would need format= and rate= that the sidecar was carrying, so the
        // failure would arrive as a complaint about a missing query parameter
        // instead of about the file that is actually absent.
        return Container::Sigmf;
    }

    std::error_code ec;
    if (std::filesystem::exists(path_of(sigmf_meta_path_for(path)), ec) && !ec) {
        return Container::Sigmf;
    }

    auto head = read_region(path, 0, 12);
    if (!head) {
        return std::unexpected(head.error());
    }
    if (head->size() >= 12 && four_cc(*head, 8) == "WAVE") {
        const std::string_view signature = four_cc(*head, 0);
        if (signature == "RIFF" || signature == "RF64" || signature == "BW64" ||
            signature == "RIFX") {
            return Container::Wav;
        }
    }
    return Container::Raw;
}

Expected<ResolvedFile> resolve_file(const FileSourceConfig& config)
{
    if (config.path.empty()) {
        return fail("a file source needs a path");
    }
    if (!std::isfinite(config.ppm_uncertainty) || config.ppm_uncertainty < 0.0) {
        return fail("ppm_uncertainty must be a finite number of parts per million, zero or above");
    }
    if (config.anchor_accuracy_given && config.anchor_accuracy_ns < 0) {
        return fail("anchor_accuracy_ns must not be negative");
    }
    if (config.rate_given && config.rate <= 0) {
        return fail(std::format("rate= must be a positive number of samples per second, got {}",
                                config.rate));
    }

    const std::filesystem::path path = path_of(config.path);
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec) {
        return fail(std::format("could not stat '{}': {}", config.path, ec.message()), ec.value());
    }
    if (!std::filesystem::is_regular_file(status)) {
        return fail(std::format("'{}' is not a regular file", config.path));
    }

    Container container = config.container;
    if (container == Container::Auto) {
        auto sniffed = sniff_container(config.path, config.meta_path);
        if (!sniffed) {
            return std::unexpected(sniffed.error());
        }
        container = *sniffed;
    }
    if (config.segment_given && container != Container::Sigmf) {
        return fail(std::format(
            "segment= names a SigMF capture segment and '{}' is being read as {}. Only SigMF "
            "expresses a recording that retunes.",
            config.path, container_name(container)));
    }

    RecordingMetadata meta;
    switch (container) {
        case Container::Auto:
            return fail("the container should have been settled before this point");
        case Container::Raw: {
            auto size = file_size_of(config.path);
            if (!size) {
                return std::unexpected(size.error());
            }
            meta.container = Container::Raw;
            meta.data_path = config.path;
            meta.data_offset = 0;
            meta.data_bytes = *size;
            break;
        }
        case Container::Sigmf: {
            std::string meta_path = config.meta_path;
            if (meta_path.empty()) {
                meta_path = std::string_view(config.path).ends_with(".sigmf-meta")
                                ? config.path
                                : sigmf_meta_path_for(config.path);
            }
            std::error_code exists_ec;
            if (!std::filesystem::exists(path_of(meta_path), exists_ec) || exists_ec) {
                return fail(std::format(
                    "'{}' is being read as SigMF and its sidecar '{}' is not there. SigMF keeps "
                    "the rate, the datatype and the centre frequency in that file, so without "
                    "it nothing says what the samples are. Put the sidecar beside the dataset, "
                    "name another one with meta=, or read the file as raw with "
                    "container=raw&rate=&format=.",
                    config.path, meta_path));
            }
            auto read = read_sigmf_metadata(meta_path);
            if (!read) {
                return std::unexpected(read.error());
            }
            meta = std::move(*read);
            break;
        }
        case Container::Wav: {
            auto read = read_wav_metadata(config.path);
            if (!read) {
                return std::unexpected(read.error());
            }
            meta = std::move(*read);
            break;
        }
    }

    // Where the container and the URI both spoke, they have to agree. Picking
    // a winner would mean one of the two is silently ignored, and the whole
    // reason an unknown query key is an error here is that a capture whose
    // declared metadata is wrong looks exactly like one whose metadata is
    // right.
    if (config.rate_given && meta.has_rate && config.rate != meta.rate) {
        return fail(std::format(
            "'{}' declares {} samples per second in its {} metadata and the URI says {}. One "
            "of the two is the rate the samples were taken at, and every frequency derived "
            "from the wrong one is off by the ratio of the two. Drop rate= to take the "
            "recording's own figure, or fix whichever is wrong.",
            config.path, meta.rate, container_name(container), config.rate));
    }
    const dsp::SampleRate rate = config.rate_given ? config.rate : meta.rate;
    if (rate <= 0) {
        return fail(std::format(
            "'{}' needs rate= in its URI: it is being read as {} and nothing in it says what "
            "rate the samples were taken at",
            config.path, container_name(container)));
    }

    if (config.format_given && meta.has_format && config.format != meta.format) {
        return fail(std::format(
            "'{}' declares {} samples in its {} metadata and the URI says {}. Reading one as "
            "the other reinterprets every byte in the file.",
            config.path, format_name(meta.format), container_name(container),
            format_name(config.format)));
    }
    SampleFormat format = SampleFormat::Cf32;
    if (config.format_given) {
        format = config.format;
    } else if (meta.has_format) {
        format = meta.format;
    } else {
        auto inferred = sample_format_from_extension(config.path);
        if (!inferred) {
            return std::unexpected(inferred.error());
        }
        format = *inferred;
    }

    if (config.center_given && meta.has_center && config.center_hz != meta.center_hz) {
        return fail(std::format(
            "'{}' declares a centre frequency of {} Hz in its {} metadata and the URI says {} "
            "Hz. Every frequency read off the display would be wrong by the difference of {} "
            "Hz, which is a plausible-looking spectrum of the wrong part of the band.",
            config.path, meta.center_hz, container_name(container), config.center_hz,
            config.center_hz - meta.center_hz));
    }

    const std::size_t bps = bytes_per_sample(format);
    if (bps == 0) {
        return fail("unsupported sample format");
    }
    if (meta.data_bytes % bps != 0) {
        // A trailing partial sample means either the wrong format was declared
        // or the writer was interrupted. Both are worth stopping for: the
        // second is recoverable by truncating the file, and the first produces
        // a capture whose every sample is wrong.
        return fail(std::format(
            "'{}' holds {} bytes of samples, which is not a whole number of {} samples ({} "
            "bytes each). Either the declared format is wrong or the file is truncated.",
            config.path, meta.data_bytes, format_name(format), bps));
    }

    std::vector<std::string> notes = std::move(meta.notes);

    // The segment, which is the part a naive SigMF reader gets wrong.
    dsp::SampleIndex first_sample = 0;
    auto length_samples = static_cast<dsp::SampleIndex>(meta.data_bytes / bps);
    dsp::Hertz meta_center = meta.center_hz;
    bool meta_has_center = meta.has_center;
    std::int64_t meta_anchor_ns = meta.anchor_ns;
    bool meta_has_anchor = meta.has_anchor;

    if (container == Container::Sigmf) {
        const auto& segments = meta.segments;
        if (config.segment_given) {
            if (config.segment >= segments.size()) {
                return fail(std::format(
                    "'{}' asks for capture segment {} and the recording has {}, numbered 0 to "
                    "{}",
                    config.path, config.segment, segments.size(), segments.size() - 1));
            }
            const CaptureSegment& chosen = segments[config.segment];
            first_sample = chosen.sample_start;
            length_samples = chosen.sample_count;
            meta_has_center = chosen.has_center;
            meta_center = chosen.center_hz;
            meta_has_anchor = chosen.has_anchor;
            meta_anchor_ns = chosen.anchor_ns;

            if (config.center_given && chosen.has_center && config.center_hz != chosen.center_hz) {
                return fail(std::format(
                    "'{}' capture segment {} declares a centre frequency of {} Hz and the URI "
                    "says {} Hz",
                    config.path, config.segment, chosen.center_hz, config.center_hz));
            }
            if (segments.size() > 1) {
                notes.emplace_back(std::format(
                    "playing capture segment {} of {}, samples {} to {} of the dataset",
                    config.segment, segments.size(), chosen.sample_start,
                    chosen.sample_start + chosen.sample_count));
            }
        } else if (segments.size() > 1) {
            const bool uniform = std::all_of(
                segments.begin(), segments.end(), [&](const CaptureSegment& segment) {
                    return segment.has_center == segments.front().has_center &&
                           segment.center_hz == segments.front().center_hz;
                });
            if (!uniform) {
                // The silent failure this whole branch exists to prevent: a
                // reader that takes captures[0] and plays the file through
                // puts everything after the retune at a frequency nobody
                // transmitted on, and the spectrum still looks like a
                // spectrum.
                std::string listing;
                for (std::size_t i = 0; i < segments.size(); ++i) {
                    listing += std::format("\n  segment={} starts at sample {} ({} samples)",
                                           i, segments[i].sample_start,
                                           segments[i].sample_count);
                    listing += segments[i].has_center
                                   ? std::format(" at {} Hz", segments[i].center_hz)
                                   : std::string(" with no frequency stated");
                }
                return fail(std::format(
                    "'{}' retunes: its {} capture segments do not all sit at one centre "
                    "frequency, and this source reports one centre for the whole stream. "
                    "Playing across the retune would put every sample after it at a frequency "
                    "nobody transmitted on. Add segment=N to play one of them:{}",
                    config.path, segments.size(), listing));
            }
            notes.emplace_back(std::format(
                "'{}' has {} capture segments and they all sit at one centre frequency, so the "
                "whole dataset is played as one stream",
                config.path, segments.size()));
        }
    }

    if (length_samples == 0) {
        return fail(std::format("'{}' holds {} bytes of samples, which is less than one {} "
                                "sample",
                                config.path, meta.data_bytes, format_name(format)));
    }

    const dsp::Hertz center_hz =
        config.center_given ? config.center_hz : (meta_has_center ? meta_center : 0);
    if (!config.center_given && !meta_has_center) {
        notes.emplace_back(std::format(
            "'{}' states no centre frequency, so the stream is presented at 0 Hz and every "
            "absolute frequency derived from it will be baseband. Add center= to place it.",
            config.path));
    }

    // The anchor, and the one place the URI is allowed to override the
    // container rather than having to agree with it. Rate, format and centre
    // change what the samples mean; the anchor is a correction an operator is
    // entitled to make, because the recorder's clock is exactly the thing
    // they may know was wrong, and anchor_accuracy_ns is how they say how
    // good the correction is.
    std::int64_t accuracy_ns = 0;
    std::int64_t anchor_ns = 0;
    if (config.anchor_given) {
        anchor_ns = config.anchor_ns;
        // One sample period is the floor: a stated anchor still cannot place
        // sample zero finer than the grid the samples sit on.
        accuracy_ns = std::max<std::int64_t>(1, 1'000'000'000 / rate);
        if (meta_has_anchor && meta_anchor_ns != config.anchor_ns) {
            notes.emplace_back(std::format(
                "anchor_ns in the URI overrides the {} ns the {} metadata carries, a "
                "difference of {} ns",
                meta_anchor_ns, container_name(container), config.anchor_ns - meta_anchor_ns));
        }
    } else if (meta_has_anchor) {
        anchor_ns = meta_anchor_ns;
        accuracy_ns = meta.anchor_accuracy_ns > 0 ? meta.anchor_accuracy_ns
                                                  : kRecorderClockAccuracyNs;
    } else {
        auto derived = anchor_from_modification_time(meta.data_path, length_samples, rate);
        if (!derived) {
            return std::unexpected(derived.error());
        }
        anchor_ns = derived->anchor_ns;
        accuracy_ns = derived->accuracy_ns;
    }

    if (config.anchor_accuracy_given) {
        accuracy_ns = config.anchor_accuracy_ns;
    }

    if (container != Container::Raw) {
        notes.emplace_back(std::format(
            "read as {}: rate {} S/s from {}, format {} from {}, centre {} Hz from {}",
            container_name(container), rate, config.rate_given ? "the URI" : "the recording",
            format_name(format), config.format_given ? "the URI" : "the recording", center_hz,
            config.center_given ? "the URI"
                                : (meta_has_center ? "the recording" : "nowhere")));
    }

    SourceCapabilities caps;
    caps.uri = config.uri;
    caps.backend = "file";
    caps.display_name =
        config.display_name.empty() ? basename_of(config.path) : config.display_name;

    // No tune ranges at all, deliberately. A recording's centre frequency is a
    // fact about bytes already on disk, so can_tune() answers false for every
    // frequency including the one the file was recorded at, and tune() fails
    // with a message naming the capability rather than the symptom.
    caps.tune_ranges.clear();

    caps.sample_rates = {rate};
    caps.min_rate = rate;
    caps.max_rate = rate;

    caps.native_format = format;
    caps.bits_per_component = static_cast<std::uint8_t>((bps / 2) * 8);

    // flow=paced plays the file as a radio: FileSourceConfig::paced_flow has
    // what that changes and what it is for.
    caps.flow = config.paced_flow ? FlowControl::Paced : FlowControl::Demand;
    caps.seekable = true;
    caps.length_samples = length_samples;
    caps.preferred_block_samples = kDefaultBlockSamples;
    caps.timestamp_accuracy_ns = accuracy_ns;
    caps.clock_sources = {ClockSource::Internal};
    caps.notes = std::move(notes);

    // The source side of the grid question. A recording's span is fixed, so
    // unlike a dongle's this cannot go stale under a retune. Nothing reads it
    // yet; see SourceCapabilities::resolution for what should.
    caps.resolution = resolution_for_span(center_hz, rate);

    ResolvedFile out;
    out.caps = std::move(caps);
    out.anchor_ns = anchor_ns;
    out.length_samples = length_samples;
    out.data_path = meta.data_path;
    out.data_offset = meta.data_offset + first_sample * static_cast<std::uint64_t>(bps);
    out.rate = rate;
    out.format = format;
    out.center_hz = center_hz;
    return out;
}

}  // namespace

Expected<SourceCapabilities> describe_file_source(const FileSourceConfig& config)
{
    auto resolved = resolve_file(config);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    return std::move(resolved->caps);
}

Expected<std::unique_ptr<Source>> open_file_source(const FileSourceConfig& config)
{
    auto resolved = resolve_file(config);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }

    auto source = std::make_unique<FileSource>();
    if (auto opened = source->open(config, std::move(*resolved)); !opened) {
        return std::unexpected(opened.error());
    }
    return std::unique_ptr<Source>(std::move(source));
}

}  // namespace revenant::source
