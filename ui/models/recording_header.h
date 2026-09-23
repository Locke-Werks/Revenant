// What a recording says about itself, read before the engine is asked to
// open it.
//
// WHY THE CLIENT READS THE HEADER AND NOT THE ENGINE
//
// The picker has to show format, rate, channels, length and centre before
// anything is opened, because the centre is often not in the file and the
// operator has to supply it first. Two places could read it: an engine call
// that describes a URI without opening it, or this header. This one, for two
// reasons that are about this tree rather than about taste.
//
//   The wire has no such call. listSources describes the devices the engine
//   enumerates and a file is named rather than enumerated, sourceDescriptor
//   describes the source that is already open, and SourceDescriptor carries
//   no centre frequency for either. A describe call would be a schema change,
//   a server method and a client method, and ui/models/wire_seam.h records
//   that core/rpc is read-only from this project.
//
//   The file dialog is the client's. It browses the filesystem of the machine
//   the window runs on, so the path it hands back is a path HERE.
//
// THE PATH IS RESOLVED BY THE ENGINE, AND THAT IS THE LIMIT OF THIS CHOICE.
// openSource takes a URI and core/source/registry.cpp opens it on the
// engine's machine. revenant-engine binds loopback only, so today the two are
// the same machine and the same filesystem and the preview reads the file the
// engine will open. An engine reached through a tunnel would open whatever is
// at that path on ITS disk, which this preview has never seen; the section
// says so rather than presenting a preview of one file as a promise about
// another. After the open, ui/models/recording_link.cpp compares what the
// engine reports against what this read, so a disagreement is shown rather
// than assumed away.
//
// THE PREVIEW HAS TO AGREE WITH core/source/file_source.cpp, so this follows
// its rules rather than improving on them: the same container sniff in the
// same order, the same sample formats, the same refusals for the files it
// refuses. Where the engine would refuse, `refusal` says so in words, and the
// open button stays dead rather than sending a request the engine answers
// with the same sentence a round trip later. The tests in
// ui/tests/test_recording_header.cpp build each container byte by byte.
//
// Qt-free, so ui/tests can link it. File access goes through RecordingFiles,
// so the tests hand in bytes and the window hands in the disk.

#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace revenant::ui {

enum class RecordingKind : std::uint8_t {
    // Headerless interleaved IQ. The rate, the format and the centre are not
    // in the bytes.
    Raw,

    // RIFF, RF64 or BW64 WAVE.
    Wav,

    // A .sigmf-meta sidecar and its dataset.
    Sigmf,
};

// The engine's sample formats, by the names its URI grammar takes.
enum class RecordingFormat : std::uint8_t {
    Unknown,
    Cu8,
    Cs8,
    Cs16,
    Cs24,
    Cf32,
};

[[nodiscard]] inline const char* recording_format_name(RecordingFormat format)
{
    switch (format) {
        case RecordingFormat::Unknown: return "";
        case RecordingFormat::Cu8: return "cu8";
        case RecordingFormat::Cs8: return "cs8";
        case RecordingFormat::Cs16: return "cs16";
        case RecordingFormat::Cs24: return "cs24";
        case RecordingFormat::Cf32: return "cf32";
    }
    return "";
}

// Bytes per COMPLEX sample, which is what a length is counted in.
[[nodiscard]] inline std::size_t recording_bytes_per_sample(RecordingFormat format)
{
    switch (format) {
        case RecordingFormat::Unknown: return 0;
        case RecordingFormat::Cu8: return 2;
        case RecordingFormat::Cs8: return 2;
        case RecordingFormat::Cs16: return 4;
        case RecordingFormat::Cs24: return 6;
        case RecordingFormat::Cf32: return 8;
    }
    return 0;
}

// Bits in one I or Q component.
[[nodiscard]] inline int recording_bits_per_component(RecordingFormat format)
{
    return static_cast<int>(recording_bytes_per_sample(format) / 2 * 8);
}

[[nodiscard]] inline RecordingFormat recording_format_from_name(std::string_view name)
{
    for (const RecordingFormat format : {RecordingFormat::Cu8, RecordingFormat::Cs8,
                                         RecordingFormat::Cs16, RecordingFormat::Cs24,
                                         RecordingFormat::Cf32}) {
        if (name == recording_format_name(format)) {
            return format;
        }
    }
    return RecordingFormat::Unknown;
}

// The extension after the last dot, lowercased, or empty. A dot inside a
// directory name is not an extension, which is why the separator is looked
// for first.
[[nodiscard]] inline std::string recording_extension(std::string_view path)
{
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 >= name.size()) {
        return {};
    }
    std::string out(name.substr(dot + 1));
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// The format a raw file's extension names, which is the engine's own
// inference in sample_format_from_extension: exactly the five names and
// nothing looser. The extension match is case-sensitive THERE and not here,
// which is one of the two places this preview is kinder than the engine;
// recording_plan.h states the format explicitly in the URI whenever it
// inferred one, so the engine never has to repeat the inference.
[[nodiscard]] inline RecordingFormat recording_format_from_extension(std::string_view path)
{
    return recording_format_from_name(recording_extension(path));
}

// The sidecar a dataset implies, as the engine's sigmf_meta_path_for does it:
// x.sigmf-data gives x.sigmf-meta and anything else gets the suffix appended.
[[nodiscard]] inline std::string recording_sigmf_meta_for(std::string_view data_path)
{
    constexpr std::string_view kDataSuffix = ".sigmf-data";
    if (data_path.ends_with(kDataSuffix)) {
        std::string out(data_path.substr(0, data_path.size() - kDataSuffix.size()));
        out += ".sigmf-meta";
        return out;
    }
    return std::string(data_path) + ".sigmf-meta";
}

// How the header reaches the disk. Three calls and nothing else, so a test is
// a map from a path to bytes.
struct RecordingFiles {
    std::function<bool(const std::string&)> exists;
    std::function<std::optional<std::uint64_t>(const std::string&)> size;

    // Up to `count` bytes from `offset`; fewer at the end of the file, none
    // when it cannot be read.
    std::function<std::vector<std::uint8_t>(const std::string&, std::uint64_t, std::size_t)> read;
};

namespace recording_detail {

// UTF-8 in and a filesystem path out. A std::string handed to
// std::filesystem::path on Windows is read in the ANSI code page, which turns
// a non-ASCII folder name into a path that does not exist.
[[nodiscard]] inline std::filesystem::path path_of(const std::string& utf8)
{
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

}  // namespace recording_detail

// The real disk.
[[nodiscard]] inline RecordingFiles disk_recording_files()
{
    RecordingFiles files;
    files.exists = [](const std::string& path) {
        std::error_code ec;
        return std::filesystem::is_regular_file(recording_detail::path_of(path), ec) && !ec;
    };
    files.size = [](const std::string& path) -> std::optional<std::uint64_t> {
        std::error_code ec;
        const auto bytes = std::filesystem::file_size(recording_detail::path_of(path), ec);
        if (ec) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(bytes);
    };
    files.read = [](const std::string& path, std::uint64_t offset,
                    std::size_t count) -> std::vector<std::uint8_t> {
        std::ifstream in(recording_detail::path_of(path), std::ios::binary);
        if (!in) {
            return {};
        }
        in.seekg(static_cast<std::streamoff>(offset));
        if (!in) {
            return {};
        }
        std::vector<std::uint8_t> out(count);
        in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(count));
        out.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));
        return out;
    };
    return files;
}

// What the file said, and what the engine would say about it.
struct RecordingHeader {
    RecordingKind kind = RecordingKind::Raw;

    // "RIFF WAV", "RF64", "BW64", "SigMF" or "raw", for the preview line.
    std::string container;

    // The path as chosen, which is what goes in the URI. For SigMF that is
    // whichever of the pair was picked; the engine resolves the other.
    std::string path;

    // Where the samples are. The dataset for SigMF, the file itself otherwise.
    std::string data_path;
    std::uint64_t data_bytes = 0;

    // Every field below is optional, because the containers differ in what they
    // carry and a field nobody set has to be visibly unset rather than a
    // plausible default.
    RecordingFormat format = RecordingFormat::Unknown;

    bool has_rate = false;
    std::int64_t rate = 0;

    // What the container says about channels, in words: a WAV states two and
    // means I and Q, SigMF states one complex channel, a raw file states
    // nothing and is read as I and Q interleaved.
    std::string channels;

    bool has_center = false;
    std::int64_t center_hz = 0;

    // Conditions worth showing that do not stop the open, in the engine's
    // spirit: a WAV with no auxi chunk, a SigMF sidecar with an empty
    // captures array.
    std::vector<std::string> notes;

    // Non-empty when the engine will refuse this file, and why, in words.
    std::string refusal;

    [[nodiscard]] bool readable() const { return refusal.empty(); }

    // Samples in the file at a format. For a raw file that is the format the
    // operator chose; for a container it is the container's own.
    [[nodiscard]] std::uint64_t length_samples(RecordingFormat chosen) const
    {
        const RecordingFormat use = format != RecordingFormat::Unknown ? format : chosen;
        const std::size_t bps = recording_bytes_per_sample(use);
        return bps == 0 ? 0 : data_bytes / bps;
    }
};

namespace recording_detail {

[[nodiscard]] inline std::uint16_t le16(const std::vector<std::uint8_t>& b, std::size_t at)
{
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

[[nodiscard]] inline std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t at)
{
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) |
           (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

[[nodiscard]] inline std::uint64_t le64(const std::vector<std::uint8_t>& b, std::size_t at)
{
    return static_cast<std::uint64_t>(le32(b, at)) |
           (static_cast<std::uint64_t>(le32(b, at + 4)) << 32);
}

[[nodiscard]] inline std::string four_cc(const std::vector<std::uint8_t>& b, std::size_t at)
{
    std::string out;
    for (std::size_t i = 0; i < 4 && at + i < b.size(); ++i) {
        out.push_back(static_cast<char>(b[at + i]));
    }
    return out;
}

[[nodiscard]] inline std::string base_name(std::string_view path)
{
    const std::size_t slash = path.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

[[nodiscard]] inline std::string directory_of(std::string_view path)
{
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash + 1));
}

// ---------------------------------------------------------------------------
// Just enough JSON for a SigMF sidecar
// ---------------------------------------------------------------------------
//
// The engine has its own reader and it is not reachable from here: it lives
// inside core/source/file_source.cpp, which is compiled into the /MT engine.
// This one reads the same grammar and keeps only what the preview uses. Depth
// is bounded, because a sidecar is a file somebody else wrote and a recursive
// parser without a bound is a stack overflow waiting for a deep enough array.

struct Json {
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    bool integral = false;
    std::int64_t integer = 0;
    std::string text;

    // An array's elements, or an object's values in the order of `keys`.
    std::vector<Json> elements;
    std::vector<std::string> keys;

    [[nodiscard]] const Json* member(std::string_view key) const
    {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) {
                return &elements[i];
            }
        }
        return nullptr;
    }
};

class JsonReader {
public:
    explicit JsonReader(std::string_view text) : text_(text) {}

    [[nodiscard]] std::optional<Json> document()
    {
        Json out;
        if (!value(out, 0)) {
            return std::nullopt;
        }
        skip();
        if (at_ != text_.size()) {
            error_ = "text after the end of the document";
            return std::nullopt;
        }
        return out;
    }

    [[nodiscard]] const std::string& error() const { return error_; }

private:
    static constexpr int kMaxDepth = 64;

    void skip()
    {
        while (at_ < text_.size() &&
               (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' ||
                text_[at_] == '\r')) {
            ++at_;
        }
    }

    [[nodiscard]] bool fail(std::string message)
    {
        if (error_.empty()) {
            error_ = std::format("{} at byte {}", message, at_);
        }
        return false;
    }

    [[nodiscard]] bool literal(std::string_view word)
    {
        if (text_.substr(at_, word.size()) != word) {
            return fail("not a JSON value");
        }
        at_ += word.size();
        return true;
    }

    [[nodiscard]] bool string(std::string& out)
    {
        ++at_;
        while (at_ < text_.size()) {
            const char c = text_[at_++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (at_ >= text_.size()) {
                break;
            }
            const char e = text_[at_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    // Kept as the escape rather than decoded. Nothing the
                    // preview reads is outside ASCII: keys are core:*, a
                    // datatype is a short code, and a dataset name with a
                    // non-ASCII character in it is decoded by the engine,
                    // which is the reader that opens it.
                    if (at_ + 4 > text_.size()) {
                        return fail("a truncated \\u escape");
                    }
                    out += "\\u";
                    out += text_.substr(at_, 4);
                    at_ += 4;
                    break;
                }
                default: return fail("an escape JSON does not define");
            }
        }
        return fail("a string with no closing quote");
    }

    [[nodiscard]] bool number(Json& out)
    {
        const std::size_t start = at_;
        bool fractional = false;
        while (at_ < text_.size()) {
            const char c = text_[at_];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+') {
                ++at_;
            } else if (c == '.' || c == 'e' || c == 'E') {
                fractional = true;
                ++at_;
            } else {
                break;
            }
        }
        const std::string_view digits = text_.substr(start, at_ - start);
        double value = 0.0;
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) {
            return fail("a number that does not parse");
        }
        out.kind = Json::Kind::Number;
        out.number = value;
        if (!fractional) {
            std::int64_t whole = 0;
            const auto exact =
                std::from_chars(digits.data(), digits.data() + digits.size(), whole);
            if (exact.ec == std::errc{} && exact.ptr == digits.data() + digits.size()) {
                out.integral = true;
                out.integer = whole;
            }
        }
        return true;
    }

    [[nodiscard]] bool value(Json& out, int depth)
    {
        if (depth > kMaxDepth) {
            return fail("nesting deeper than a SigMF sidecar has any reason to go");
        }
        skip();
        if (at_ >= text_.size()) {
            return fail("the document ends where a value was expected");
        }
        const char c = text_[at_];
        if (c == '{') {
            out.kind = Json::Kind::Object;
            ++at_;
            skip();
            if (at_ < text_.size() && text_[at_] == '}') {
                ++at_;
                return true;
            }
            for (;;) {
                skip();
                if (at_ >= text_.size() || text_[at_] != '"') {
                    return fail("an object key that is not a string");
                }
                std::string key;
                if (!string(key)) {
                    return false;
                }
                skip();
                if (at_ >= text_.size() || text_[at_] != ':') {
                    return fail("an object key with no colon after it");
                }
                ++at_;
                Json member;
                if (!value(member, depth + 1)) {
                    return false;
                }
                out.keys.push_back(std::move(key));
                out.elements.push_back(std::move(member));
                skip();
                if (at_ < text_.size() && text_[at_] == ',') {
                    ++at_;
                    continue;
                }
                if (at_ < text_.size() && text_[at_] == '}') {
                    ++at_;
                    return true;
                }
                return fail("an object that is not closed");
            }
        }
        if (c == '[') {
            out.kind = Json::Kind::Array;
            ++at_;
            skip();
            if (at_ < text_.size() && text_[at_] == ']') {
                ++at_;
                return true;
            }
            for (;;) {
                Json element;
                if (!value(element, depth + 1)) {
                    return false;
                }
                out.elements.push_back(std::move(element));
                skip();
                if (at_ < text_.size() && text_[at_] == ',') {
                    ++at_;
                    continue;
                }
                if (at_ < text_.size() && text_[at_] == ']') {
                    ++at_;
                    return true;
                }
                return fail("an array that is not closed");
            }
        }
        if (c == '"') {
            out.kind = Json::Kind::String;
            return string(out.text);
        }
        if (c == 't') {
            out.kind = Json::Kind::Bool;
            out.boolean = true;
            return literal("true");
        }
        if (c == 'f') {
            out.kind = Json::Kind::Bool;
            return literal("false");
        }
        if (c == 'n') {
            return literal("null");
        }
        return number(out);
    }

    std::string_view text_;
    std::size_t at_ = 0;
    std::string error_;
};

// A SigMF core:datatype on the engine's terms: complex only, little-endian
// only above eight bits, and the four widths the upload kernels take. The
// engine reads no ci24, so neither does this.
[[nodiscard]] inline RecordingFormat sigmf_datatype(std::string_view datatype, std::string& why)
{
    std::string_view rest = datatype;
    if (rest.starts_with("r")) {
        why = std::format(
            "its datatype '{}' is real, and the engine reads complex IQ only", datatype);
        return RecordingFormat::Unknown;
    }
    if (!rest.starts_with("c")) {
        why = std::format("its datatype '{}' is not one SigMF defines", datatype);
        return RecordingFormat::Unknown;
    }
    rest.remove_prefix(1);
    bool big_endian = false;
    if (rest.ends_with("_le")) {
        rest.remove_suffix(3);
    } else if (rest.ends_with("_be")) {
        rest.remove_suffix(3);
        big_endian = true;
    }
    if (big_endian && rest != "u8" && rest != "i8") {
        why = std::format(
            "its datatype '{}' is big-endian, and the engine reads little-endian only", datatype);
        return RecordingFormat::Unknown;
    }
    if (rest == "u8") {
        return RecordingFormat::Cu8;
    }
    if (rest == "i8") {
        return RecordingFormat::Cs8;
    }
    if (rest == "i16") {
        return RecordingFormat::Cs16;
    }
    if (rest == "f32") {
        return RecordingFormat::Cf32;
    }
    why = std::format(
        "its datatype '{}' is not one the engine reads: it takes cu8, ci8, ci16_le and cf32_le",
        datatype);
    return RecordingFormat::Unknown;
}

inline void read_sigmf(RecordingHeader& out, const std::string& meta_path,
                       const RecordingFiles& files)
{
    out.kind = RecordingKind::Sigmf;
    out.container = "SigMF";
    out.channels = "1 complex";

    if (!files.exists(meta_path)) {
        out.refusal = std::format(
            "this is read as SigMF and its sidecar {} is not there, so nothing says what the "
            "samples are",
            base_name(meta_path));
        return;
    }

    const auto size = files.size(meta_path).value_or(0);

    // A sidecar is a few kilobytes. One far past that is not a sidecar, and
    // reading it whole into memory to find out would be the wrong way round.
    constexpr std::uint64_t kSidecarCeiling = 16u * 1024u * 1024u;
    if (size > kSidecarCeiling) {
        out.refusal = std::format("its sidecar {} is {} bytes, which is not a SigMF sidecar",
                                  base_name(meta_path), size);
        return;
    }
    const std::vector<std::uint8_t> bytes =
        files.read(meta_path, 0, static_cast<std::size_t>(size));
    const std::string text(bytes.begin(), bytes.end());

    JsonReader reader(text);
    const std::optional<Json> document = reader.document();
    if (!document || document->kind != Json::Kind::Object) {
        out.refusal = std::format("its sidecar {} is not JSON the engine can read: {}",
                                  base_name(meta_path),
                                  document ? std::string("not an object") : reader.error());
        return;
    }

    const Json* global = document->member("global");
    if (global == nullptr || global->kind != Json::Kind::Object) {
        out.refusal = "its sidecar has no 'global' object, which is where the datatype and the "
                      "rate live";
        return;
    }

    const Json* version = global->member("core:version");
    if (version == nullptr || version->kind != Json::Kind::String ||
        !version->text.starts_with("1.")) {
        out.refusal = "its sidecar does not declare SigMF version 1, which is the version the "
                      "engine reads";
        return;
    }

    const Json* metadata_only = global->member("core:metadata_only");
    if (metadata_only != nullptr && metadata_only->kind == Json::Kind::Bool &&
        metadata_only->boolean) {
        out.refusal = "its sidecar sets core:metadata_only, which says there is no dataset";
        return;
    }

    const Json* datatype = global->member("core:datatype");
    if (datatype == nullptr || datatype->kind != Json::Kind::String) {
        out.refusal = "its sidecar has no core:datatype, which is what says how wide a sample is";
        return;
    }
    std::string why;
    out.format = sigmf_datatype(datatype->text, why);
    if (out.format == RecordingFormat::Unknown) {
        out.refusal = why;
        return;
    }

    const Json* channels = global->member("core:num_channels");
    if (channels != nullptr && channels->kind == Json::Kind::Number &&
        !(channels->integral && channels->integer == 1)) {
        out.channels = std::format("{:g}", channels->number);
        out.refusal = std::format(
            "its sidecar declares {:g} channels, and the engine reads single-channel datasets",
            channels->number);
        return;
    }

    const Json* rate = global->member("core:sample_rate");
    if (rate != nullptr && rate->kind == Json::Kind::Number) {
        if (!(rate->number > 0.0) || rate->number != std::floor(rate->number) ||
            rate->number > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            out.refusal = std::format(
                "its sidecar declares a rate of {:g}, which is not a whole number of samples per "
                "second",
                rate->number);
            return;
        }
        out.has_rate = true;
        out.rate = static_cast<std::int64_t>(rate->number);
    }

    std::uint64_t trailing = 0;
    const Json* trailing_bytes = global->member("core:trailing_bytes");
    if (trailing_bytes != nullptr && trailing_bytes->kind == Json::Kind::Number &&
        trailing_bytes->integral && trailing_bytes->integer > 0) {
        trailing = static_cast<std::uint64_t>(trailing_bytes->integer);
    }

    // The dataset, named or implied, resolved beside the sidecar the way the
    // specification and the engine both do it.
    const Json* dataset = global->member("core:dataset");
    if (dataset != nullptr && dataset->kind == Json::Kind::String && !dataset->text.empty()) {
        const std::filesystem::path named = path_of(dataset->text);
        out.data_path = named.is_absolute() ? dataset->text
                                            : directory_of(meta_path) + dataset->text;
    } else if (meta_path.ends_with(".sigmf-meta")) {
        out.data_path = meta_path.substr(0, meta_path.size() - std::string_view(".sigmf-meta").size()) +
                        ".sigmf-data";
    } else {
        out.refusal = "its sidecar does not end in .sigmf-meta and names no dataset";
        return;
    }

    const std::optional<std::uint64_t> data_size = files.size(out.data_path);
    if (!data_size) {
        out.refusal = std::format("its dataset {} is not there", base_name(out.data_path));
        return;
    }
    if (trailing > *data_size) {
        out.refusal = "its sidecar declares more trailing bytes than the dataset holds";
        return;
    }
    out.data_bytes = *data_size - trailing;

    const Json* captures = document->member("captures");
    if (captures == nullptr || captures->kind != Json::Kind::Array) {
        out.refusal = "its sidecar has no 'captures' array, which is where the centre frequency "
                      "lives";
        return;
    }
    if (captures->elements.empty()) {
        out.notes.emplace_back("the sidecar's captures array is empty, so the recording states "
                               "no centre frequency");
        return;
    }

    // Every segment's centre. One recording that retunes is refused by the
    // engine unless segment= names a piece of it, and this section does not
    // offer segment=: see docs/ui-spectrum.md. So a retune is refused here
    // too, naming why, rather than opened and refused a round trip later.
    bool first = true;
    for (const Json& capture : captures->elements) {
        if (capture.kind != Json::Kind::Object) {
            out.refusal = "a capture segment in the sidecar is not an object";
            return;
        }
        const Json* frequency = capture.member("core:frequency");
        const bool has = frequency != nullptr && frequency->kind == Json::Kind::Number;
        if (has && (!std::isfinite(frequency->number) ||
                    frequency->number != std::floor(frequency->number))) {
            out.refusal = std::format(
                "a capture segment declares a centre of {:g} Hz, which is not a whole number of "
                "hertz",
                frequency->number);
            return;
        }
        const auto hz = has ? static_cast<std::int64_t>(frequency->number) : 0;
        if (first) {
            out.has_center = has;
            out.center_hz = hz;
            first = false;
            continue;
        }
        if (has != out.has_center || hz != out.center_hz) {
            out.refusal = std::format(
                "it retunes: its {} capture segments do not all sit at one centre, and the "
                "engine opens one of them only when segment= names it, which this section does "
                "not offer",
                captures->elements.size());
            return;
        }
    }
    if (captures->elements.size() > 1) {
        out.notes.emplace_back(std::format(
            "{} capture segments, all at one centre, played as one stream",
            captures->elements.size()));
    }
}

inline void read_wav(RecordingHeader& out, const std::string& path, std::uint64_t total,
                     const RecordingFiles& files)
{
    out.kind = RecordingKind::Wav;
    out.data_path = path;

    const std::vector<std::uint8_t> head = files.read(path, 0, 12);
    const std::string signature = four_cc(head, 0);
    if (signature == "RIFX") {
        out.container = "RIFX";
        out.refusal = "it is RIFX, the big-endian RIFF, which the engine does not read";
        return;
    }
    const bool is_rf64 = signature == "RF64" || signature == "BW64";
    out.container = is_rf64 ? signature : std::string("RIFF WAV");

    constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;
    if (!is_rf64 && total > kSentinel) {
        out.refusal = std::format(
            "it is {} bytes and begins with RIFF, whose sizes stop at 4 GiB, so its header has "
            "already wrapped",
            total);
        return;
    }

    bool have_format = false;
    std::uint16_t tag = 0;
    std::uint16_t channels = 0;
    std::uint32_t rate = 0;
    std::uint16_t block_align = 0;
    std::uint16_t bits = 0;
    bool have_data = false;
    std::uint64_t data_offset = 0;
    bool have_ds64 = false;
    std::uint64_t ds64_data = 0;
    bool have_auxi = false;
    std::uint32_t auxi_center = 0;
    std::uint32_t auxi_rate = 0;

    std::uint64_t at = 12;
    while (at + 8 <= total) {
        const std::vector<std::uint8_t> chunk = files.read(path, at, 8);
        if (chunk.size() < 8) {
            out.refusal = std::format("it ends in the middle of a chunk header at byte {}", at);
            return;
        }
        const std::string id = four_cc(chunk, 0);
        const std::uint32_t declared = le32(chunk, 4);
        const std::uint64_t body = at + 8;
        std::uint64_t size = declared;
        if (declared == kSentinel && have_ds64 && id == "data") {
            size = ds64_data;
        }

        if (id == "ds64" && size >= 28) {
            const std::vector<std::uint8_t> payload = files.read(path, body, 28);
            if (payload.size() >= 28) {
                have_ds64 = true;
                ds64_data = le64(payload, 8);
            }
        } else if (id == "fmt ") {
            const std::vector<std::uint8_t> payload =
                files.read(path, body, static_cast<std::size_t>(std::min<std::uint64_t>(size, 40)));
            if (payload.size() < 16) {
                out.refusal = "its fmt chunk is shorter than the 16 bytes WAVEFORMATEX needs";
                return;
            }
            have_format = true;
            tag = le16(payload, 0);
            channels = le16(payload, 2);
            rate = le32(payload, 4);
            block_align = le16(payload, 12);
            bits = le16(payload, 14);
            if (tag == 0xFFFE) {
                if (payload.size() < 26) {
                    out.refusal = "it declares WAVE_FORMAT_EXTENSIBLE with a fmt chunk too short "
                                  "to say what the samples are";
                    return;
                }
                tag = le16(payload, 24);
            }
        } else if (id == "auxi") {
            // The de-facto chunk the SDR# and HDSDR family write. Centre at
            // byte 32 and rate at 36, the offsets core/source/file_source.cpp
            // reads and cross-checks.
            if (size >= 60) {
                const std::vector<std::uint8_t> payload = files.read(path, body, 60);
                if (payload.size() >= 60) {
                    have_auxi = true;
                    auxi_center = le32(payload, 32);
                    auxi_rate = le32(payload, 36);
                }
            } else {
                out.notes.emplace_back("its auxi chunk is too short to hold a centre frequency");
            }
        } else if (id == "data") {
            have_data = true;
            data_offset = body;
            out.data_bytes = size;
        }

        const std::uint64_t advance = size + (size % 2);
        if (advance > total - body) {
            break;
        }
        at = body + advance;
    }

    if (!have_format) {
        out.refusal = "it has no fmt chunk, so nothing in it says what a sample is";
        return;
    }
    out.channels = channels == 2 ? std::string("2, I and Q") : std::to_string(channels);
    if (have_format && rate > 0) {
        out.has_rate = true;
        out.rate = rate;
    }
    if (!have_data) {
        out.refusal = "it has no data chunk";
        return;
    }
    if (out.data_bytes > total - data_offset) {
        out.refusal = std::format(
            "its data chunk declares {} bytes and the file holds {} after it: it was cut short",
            out.data_bytes, total - data_offset);
        return;
    }
    if (channels != 2) {
        out.refusal = std::format(
            "it declares {} channels, and an IQ recording is two, I and Q", channels);
        return;
    }

    if (tag == 1 && bits == 8) {
        out.format = RecordingFormat::Cu8;
    } else if (tag == 1 && bits == 16) {
        out.format = RecordingFormat::Cs16;
    } else if (tag == 1 && bits == 24) {
        out.format = RecordingFormat::Cs24;
    } else if (tag == 3 && bits == 32) {
        out.format = RecordingFormat::Cf32;
    } else {
        out.refusal = std::format(
            "it declares format tag {} at {} bits, and the engine reads 8, 16 and 24-bit PCM and "
            "32-bit float",
            tag, bits);
        return;
    }

    const auto expected_align = static_cast<std::uint16_t>(channels * (bits / 8));
    if (block_align != 0 && block_align != expected_align) {
        out.refusal = std::format(
            "its block align is {} where {} bits across {} channels makes {}", block_align, bits,
            channels, expected_align);
        return;
    }

    if (have_auxi) {
        if (auxi_rate != 0 && rate != 0 && auxi_rate != rate) {
            out.refusal = std::format(
                "its fmt chunk says {} samples per second and its auxi chunk says {}", rate,
                auxi_rate);
            return;
        }
        if (auxi_center != 0) {
            out.has_center = true;
            out.center_hz = auxi_center;
        } else {
            out.notes.emplace_back("its auxi chunk's centre frequency is zero, which recorders "
                                   "write when they had nothing to put there");
        }
    } else {
        out.notes.emplace_back("a WAV with no auxi chunk, so it carries no centre frequency");
    }
}

}  // namespace recording_detail

// Reads what a recording says, in the order core/source/file_source.cpp's
// sniff_container settles the container: a SigMF name, then a sidecar beside
// the path, then a RIFF, RF64 or BW64 signature over WAVE, then raw.
//
// A raw file's format comes from its extension when the extension names one,
// and is left Unknown otherwise; recording_plan.h asks the operator for it.
// Its rate and centre are never in the bytes.
[[nodiscard]] inline RecordingHeader read_recording_header(const std::string& path,
                                                           const RecordingFiles& files)
{
    RecordingHeader out;
    out.path = path;
    out.data_path = path;

    const bool named_sigmf = path.ends_with(".sigmf-meta") || path.ends_with(".sigmf-data");
    if (!named_sigmf && !files.exists(path)) {
        out.container = "raw";
        out.refusal = "the file is not there";
        return out;
    }

    if (named_sigmf || files.exists(recording_sigmf_meta_for(path))) {
        const std::string meta = path.ends_with(".sigmf-meta") ? path
                                                               : recording_sigmf_meta_for(path);
        recording_detail::read_sigmf(out, meta, files);
        return out;
    }

    const std::optional<std::uint64_t> size = files.size(path);
    if (!size) {
        out.container = "raw";
        out.refusal = "the file cannot be read";
        return out;
    }

    const std::vector<std::uint8_t> head = files.read(path, 0, 12);
    if (head.size() >= 12 && recording_detail::four_cc(head, 8) == "WAVE") {
        const std::string signature = recording_detail::four_cc(head, 0);
        if (signature == "RIFF" || signature == "RF64" || signature == "BW64" ||
            signature == "RIFX") {
            recording_detail::read_wav(out, path, *size, files);
            return out;
        }
    }

    out.kind = RecordingKind::Raw;
    out.container = "raw";
    out.channels = "not stated; read as I and Q interleaved";
    out.data_bytes = *size;
    out.format = recording_format_from_extension(path);
    return out;
}

}  // namespace revenant::ui
