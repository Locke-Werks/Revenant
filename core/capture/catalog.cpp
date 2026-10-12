// The capture catalog. catalog.h has the shape; this file has the reading.
//
// Every number in a meta is checked for its type as well as its presence. A
// stream_epoch that arrives as a double has already been rounded to 53 bits,
// and a rounded epoch is a different stream: the segment would be listed, and
// would then never join the segments either side of it, with no message
// anywhere. So a key of the wrong kind is a problem with the key named, the
// same as a key that is missing.

#include "core/capture/catalog.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/source/capabilities.h"
#include "core/source/json.h"

namespace revenant::capture {
namespace {

using source::JsonValue;

// The char8_t constructor, so a root under a name outside the active code
// page is looked for where it actually is. file_source.cpp has the longer
// note on why the narrow constructor is wrong on Windows.
[[nodiscard]] std::filesystem::path path_of(const std::string& utf8)
{
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

[[nodiscard]] std::string utf8_of(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() &&
           text.substr(text.size() - suffix.size()) == suffix;
}

// YYYY-MM-DD and nothing else. Anything else under the root is somebody
// else's file, an export or a note, and is not the catalog's to report.
[[nodiscard]] bool is_day_name(std::string_view name)
{
    if (name.size() != 10 || name[4] != '-' || name[7] != '-') {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (i == 4 || i == 7) {
            continue;
        }
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
    }
    return true;
}

// A sidecar is a few kilobytes. Past this it is a data file with the wrong
// name, and reading it whole would be the cost of the mistake rather than the
// report of it.
constexpr std::uintmax_t kMaxMetaBytes = 64ull * 1024 * 1024;

[[nodiscard]] Expected<std::string> read_meta_text(const std::filesystem::path& path,
                                                   std::uintmax_t size)
{
    if (size > kMaxMetaBytes) {
        return fail(std::format("the meta is {} bytes, past the {} byte ceiling for a sidecar",
                                size, kMaxMetaBytes));
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return fail("the meta could not be opened for reading");
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

// ---------------------------------------------------------------------------
// Typed reads of meta keys
// ---------------------------------------------------------------------------

[[nodiscard]] Expected<std::optional<std::uint64_t>> u64_of(const JsonValue& object,
                                                            std::string_view key)
{
    const JsonValue* value = object.member(key);
    if (value == nullptr) {
        return std::optional<std::uint64_t>{};
    }
    if (value->kind != JsonValue::Kind::Number || !value->unsigned_integral) {
        return fail(std::format("{} is not a whole number at or above zero", key));
    }
    return std::optional<std::uint64_t>{value->uinteger};
}

[[nodiscard]] Expected<std::optional<std::int64_t>> i64_of(const JsonValue& object,
                                                           std::string_view key)
{
    const JsonValue* value = object.member(key);
    if (value == nullptr) {
        return std::optional<std::int64_t>{};
    }
    if (value->kind != JsonValue::Kind::Number || !value->integral) {
        return fail(std::format("{} is not a whole number that fits 64 signed bits", key));
    }
    return std::optional<std::int64_t>{value->integer};
}

// Integer hertz, per docs/conventions.md. Many SigMF writers print rates and
// frequencies as 2400000.0, which is a whole number in a float's clothing and
// is accepted; 2400000.5 is not a rate this engine can represent and is
// refused rather than rounded.
[[nodiscard]] Expected<std::optional<std::int64_t>> hertz_of(const JsonValue& object,
                                                             std::string_view key)
{
    const JsonValue* value = object.member(key);
    if (value == nullptr) {
        return std::optional<std::int64_t>{};
    }
    if (value->kind != JsonValue::Kind::Number) {
        return fail(std::format("{} is not a number", key));
    }
    if (value->integral) {
        return std::optional<std::int64_t>{value->integer};
    }
    const double v = value->number;
    if (!std::isfinite(v) || v != std::floor(v) || std::fabs(v) > 9.0e18) {
        return fail(std::format("{} is {}, which is not a whole number of hertz", key, v));
    }
    return std::optional<std::int64_t>{static_cast<std::int64_t>(v)};
}

[[nodiscard]] Expected<std::optional<double>> real_of(const JsonValue& object,
                                                      std::string_view key)
{
    const JsonValue* value = object.member(key);
    if (value == nullptr) {
        return std::optional<double>{};
    }
    if (value->kind != JsonValue::Kind::Number || !std::isfinite(value->number)) {
        return fail(std::format("{} is not a finite number", key));
    }
    return std::optional<double>{value->number};
}

[[nodiscard]] Expected<std::optional<std::string>> text_of(const JsonValue& object,
                                                           std::string_view key)
{
    const JsonValue* value = object.member(key);
    if (value == nullptr) {
        return std::optional<std::string>{};
    }
    if (value->kind != JsonValue::Kind::String) {
        return fail(std::format("{} is not a string", key));
    }
    return std::optional<std::string>{value->text};
}

// The datatypes the engine's upload kernels convert, spelled as SigMF spells
// them. The writer records a source's native format and nothing else, so this
// is the whole of what a segment can say.
[[nodiscard]] Expected<source::SampleFormat> format_of_datatype(std::string_view datatype)
{
    if (datatype == "cu8" || datatype == "cu8_le") {
        return source::SampleFormat::Cu8;
    }
    if (datatype == "ci8" || datatype == "ci8_le") {
        return source::SampleFormat::Cs8;
    }
    if (datatype == "ci16_le") {
        return source::SampleFormat::Cs16;
    }
    if (datatype == "ci24_le") {
        return source::SampleFormat::Cs24;
    }
    if (datatype == "cf32_le") {
        return source::SampleFormat::Cf32;
    }
    return fail(std::format("core:datatype '{}' is not one a segment can carry: cu8, ci8, "
                            "ci16_le, ci24_le or cf32_le",
                            datatype));
}

[[nodiscard]] Expected<Annotation> annotation_of(const JsonValue& entry)
{
    if (entry.kind != JsonValue::Kind::Object) {
        return fail("an annotation is not a JSON object");
    }
    Annotation out;

    auto start = u64_of(entry, "core:sample_start");
    if (!start) {
        return std::unexpected(start.error());
    }
    if (!start->has_value()) {
        return fail("an annotation has no core:sample_start");
    }
    out.sample_start = **start;

    auto count = u64_of(entry, "core:sample_count");
    if (!count) {
        return std::unexpected(count.error());
    }
    out.sample_count = count->value_or(0);

    auto lower = hertz_of(entry, "core:freq_lower_edge");
    if (!lower) {
        return std::unexpected(lower.error());
    }
    out.freq_lower = lower->value_or(0);
    auto upper = hertz_of(entry, "core:freq_upper_edge");
    if (!upper) {
        return std::unexpected(upper.error());
    }
    out.freq_upper = upper->value_or(0);

    auto confidence = real_of(entry, "revenant:confidence");
    if (!confidence) {
        return std::unexpected(confidence.error());
    }
    out.confidence = confidence->value_or(0.0);
    auto peak = real_of(entry, "revenant:peak_db");
    if (!peak) {
        return std::unexpected(peak.error());
    }
    out.peak_db = peak->value_or(0.0);
    auto snr = real_of(entry, "revenant:snr_db");
    if (!snr) {
        return std::unexpected(snr.error());
    }
    out.snr_db = snr->value_or(0.0);

    auto family = text_of(entry, "revenant:family");
    if (!family) {
        return std::unexpected(family.error());
    }
    out.family = family->value_or(std::string{});
    return out;
}

[[nodiscard]] std::uint64_t file_size_or_zero(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

[[nodiscard]] bool segment_order(const CatalogSegment& a, const CatalogSegment& b)
{
    const std::int64_t sa = a.info.start_ns();
    const std::int64_t sb = b.info.start_ns();
    if (sa != sb) {
        return sa < sb;
    }
    if (a.info.stream_epoch != b.info.stream_epoch) {
        return a.info.stream_epoch < b.info.stream_epoch;
    }
    return a.info.first_index < b.info.first_index;
}

}  // namespace

std::int64_t CatalogSegment::end_ns() const
{
    return dsp::BlockTimestamp{info.end_index(), info.epoch_anchor_ns, info.rate}.wall_clock_ns();
}

Expected<ParsedMeta> parse_segment_meta(std::string_view json, std::uint64_t data_bytes)
{
    auto document = source::parse_json(json);
    if (!document) {
        return std::unexpected(document.error());
    }
    if (document->kind != JsonValue::Kind::Object) {
        return fail("the meta is not a JSON object");
    }
    const JsonValue* global = document->member("global");
    if (global == nullptr || global->kind != JsonValue::Kind::Object) {
        return fail("the meta has no global object");
    }

    ParsedMeta out;

    auto datatype = text_of(*global, "core:datatype");
    if (!datatype) {
        return std::unexpected(datatype.error());
    }
    if (!datatype->has_value()) {
        return fail("global has no core:datatype");
    }
    auto format = format_of_datatype(**datatype);
    if (!format) {
        return std::unexpected(format.error());
    }
    out.info.format = *format;

    auto rate = hertz_of(*global, "core:sample_rate");
    if (!rate) {
        return std::unexpected(rate.error());
    }
    if (!rate->has_value() || **rate <= 0) {
        return fail("global has no positive core:sample_rate");
    }
    out.info.rate = **rate;

    // The three keys without which a segment cannot be placed: which stream,
    // where in it, and when that stream began. Missing any one, the segment
    // is a recording of something at some time, which a file:// URI can still
    // play but the catalog cannot join to anything.
    auto epoch = u64_of(*global, "revenant:stream_epoch");
    if (!epoch) {
        return std::unexpected(epoch.error());
    }
    if (!epoch->has_value()) {
        return fail("global has no revenant:stream_epoch");
    }
    out.info.stream_epoch = **epoch;

    auto first = u64_of(*global, "revenant:first_index");
    if (!first) {
        return std::unexpected(first.error());
    }
    if (!first->has_value()) {
        return fail("global has no revenant:first_index");
    }
    out.info.first_index = **first;

    auto anchor = i64_of(*global, "revenant:epoch_anchor_ns");
    if (!anchor) {
        return std::unexpected(anchor.error());
    }
    if (!anchor->has_value()) {
        return fail("global has no revenant:epoch_anchor_ns");
    }
    out.info.epoch_anchor_ns = **anchor;

    auto dropped = u64_of(*global, "revenant:dropped_before");
    if (!dropped) {
        return std::unexpected(dropped.error());
    }
    out.info.dropped_before = dropped->value_or(0);

    auto reason = text_of(*global, "revenant:close_reason");
    if (!reason) {
        return std::unexpected(reason.error());
    }
    out.close_reason = reason->value_or(std::string{});

    const JsonValue* captures = document->member("captures");
    if (captures == nullptr || captures->kind != JsonValue::Kind::Array ||
        captures->elements.empty() || captures->elements.front().kind != JsonValue::Kind::Object) {
        return fail("the meta has no captures entry");
    }
    // One entry by the format, because a segment is homogeneous. A second one
    // would be a retune inside a segment, which the format forbids and which
    // replay would play at the wrong frequency.
    if (captures->elements.size() != 1) {
        return fail(std::format("the meta has {} captures entries and a segment has exactly one, "
                                "because a segment never spans a retune",
                                captures->elements.size()));
    }
    auto center = hertz_of(captures->elements.front(), "core:frequency");
    if (!center) {
        return std::unexpected(center.error());
    }
    if (!center->has_value()) {
        return fail("the captures entry has no core:frequency");
    }
    out.info.center = **center;

    const std::size_t width = source::bytes_per_sample(out.info.format);
    if (width == 0 || data_bytes % width != 0) {
        return fail(std::format("the data file is {} bytes, which is not a whole number of {} "
                                "samples of {} bytes",
                                data_bytes, source::format_name(out.info.format), width));
    }
    out.info.sample_count = data_bytes / width;

    if (const JsonValue* annotations = document->member("annotations"); annotations != nullptr) {
        if (annotations->kind != JsonValue::Kind::Array) {
            return fail("annotations is not an array");
        }
        out.annotations.reserve(annotations->elements.size());
        for (std::size_t i = 0; i < annotations->elements.size(); ++i) {
            auto one = annotation_of(annotations->elements[i]);
            if (!one) {
                return std::unexpected(with_context(one.error(), std::format("annotation {}", i)));
            }
            out.annotations.push_back(std::move(*one));
        }
    }
    return out;
}

Expected<std::int64_t> parse_capture_instant(std::string_view text)
{
    if (text.empty()) {
        return fail("an empty instant: give an ISO 8601 UTC time such as "
                    "2026-10-12T14:03:07.250Z, or nanoseconds since the Unix epoch");
    }

    // A bare number is epoch nanoseconds. Checked first, because a run of
    // digits is never a valid ISO time and the other way round.
    {
        std::int64_t ns = 0;
        const char* first = text.data();
        const char* last = first + text.size();
        const auto parsed = std::from_chars(first, last, ns);
        if (parsed.ec == std::errc{} && parsed.ptr == last) {
            return ns;
        }
    }

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
        return fail(std::format("'{}' is neither nanoseconds since the Unix epoch nor an ISO 8601 "
                                "UTC time such as 2026-10-12T14:03:07.250Z",
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
            return fail(std::format("'{}' has a decimal point with no fractional seconds", text));
        }
    }
    if (at + 1 != text.size() || text[at] != 'Z') {
        return fail(std::format("'{}' does not end in 'Z'. A capture is indexed in UTC, and a "
                                "local time read as UTC lands whole hours away from what was "
                                "meant.",
                                text));
    }

    const std::chrono::year_month_day ymd{std::chrono::year{year},
                                          std::chrono::month{static_cast<unsigned>(month)},
                                          std::chrono::day{static_cast<unsigned>(day)}};
    if (!ymd.ok() || hour > 23 || minute > 59 || second > 59) {
        return fail(std::format("'{}' is not a real UTC date and time", text));
    }
    const auto days = std::chrono::sys_days(ymd).time_since_epoch().count();
    const std::int64_t seconds = static_cast<std::int64_t>(days) * 86'400 +
                                 static_cast<std::int64_t>(hour) * 3'600 +
                                 static_cast<std::int64_t>(minute) * 60 +
                                 static_cast<std::int64_t>(second);
    return seconds * 1'000'000'000 + sub_second_ns;
}

Expected<Catalog> Catalog::open(std::string root)
{
    Catalog catalog;
    catalog.root_ = std::move(root);
    if (auto scanned = catalog.refresh(); !scanned) {
        return std::unexpected(scanned.error());
    }
    return catalog;
}

Status Catalog::refresh()
{
    namespace fs = std::filesystem;

    const fs::path root = path_of(root_);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return fail(std::format("capture root '{}' is not a directory", root_));
    }

    struct Found {
        fs::path path;
        std::string name;
        std::uint64_t size = 0;
    };
    std::vector<Found> metas;
    std::vector<Found> datas;
    std::vector<CatalogProblem> day_problems;

    fs::directory_iterator days(root, ec);
    if (ec) {
        return fail(std::format("could not list capture root '{}': {}", root_, ec.message()),
                    ec.value());
    }
    for (const fs::directory_entry& day : days) {
        std::error_code day_ec;
        if (!day.is_directory(day_ec) || !is_day_name(utf8_of(day.path().filename()))) {
            continue;
        }
        fs::directory_iterator files(day.path(), day_ec);
        if (day_ec) {
            // One unreadable day is a finding like one unreadable meta: the
            // other days are still there to be replayed.
            day_problems.push_back(CatalogProblem{utf8_of(day.path()), day_ec.message()});
            continue;
        }
        for (const fs::directory_entry& file : files) {
            std::error_code file_ec;
            if (!file.is_regular_file(file_ec)) {
                continue;
            }
            Found found{file.path(), utf8_of(file.path().filename()),
                        file_size_or_zero(file.path())};
            if (ends_with(found.name, kMetaExtension)) {
                metas.push_back(std::move(found));
            } else if (ends_with(found.name, kDataExtension)) {
                datas.push_back(std::move(found));
            }
        }
    }

    std::vector<CatalogSegment> segments;
    std::vector<KnownProblem> known;
    segments.reserve(metas.size());

    for (const Found& meta : metas) {
        const std::string meta_path = utf8_of(meta.path);

        // Unchanged since the last scan: kept as it was, unparsed. Size is
        // the change signal because an offline index pass rewriting a meta
        // to add annotations always makes it longer.
        const auto seen = std::find_if(segments_.begin(), segments_.end(),
                                       [&](const CatalogSegment& s) {
                                           return s.meta_path == meta_path &&
                                                  s.meta_bytes == meta.size;
                                       });
        if (seen != segments_.end()) {
            segments.push_back(std::move(*seen));
            continue;
        }
        const auto bad = std::find_if(known_problems_.begin(), known_problems_.end(),
                                      [&](const KnownProblem& p) {
                                          return p.path == meta_path && p.size == meta.size;
                                      });
        if (bad != known_problems_.end()) {
            known.push_back(std::move(*bad));
            continue;
        }

        const std::string stem =
            meta.name.substr(0, meta.name.size() - kMetaExtension.size());
        const fs::path data_path = meta.path.parent_path() /
                                   path_of(stem + std::string(kDataExtension));
        std::error_code data_ec;
        if (!fs::is_regular_file(data_path, data_ec)) {
            known.push_back(KnownProblem{meta_path, meta.size,
                                         "the meta has no data file beside it"});
            continue;
        }

        auto text = read_meta_text(meta.path, meta.size);
        if (!text) {
            known.push_back(KnownProblem{meta_path, meta.size, text.error().message});
            continue;
        }
        const std::uint64_t data_bytes = file_size_or_zero(data_path);
        auto parsed = parse_segment_meta(*text, data_bytes);
        if (!parsed) {
            known.push_back(KnownProblem{meta_path, meta.size, parsed.error().message});
            continue;
        }

        CatalogSegment segment;
        segment.info = parsed->info;
        segment.data_path = utf8_of(data_path);
        segment.meta_path = meta_path;
        segment.data_bytes = data_bytes;
        segment.meta_bytes = meta.size;
        segment.close_reason = std::move(parsed->close_reason);
        segment.annotations = std::move(parsed->annotations);
        segments.push_back(std::move(segment));
    }

    std::vector<UnfinishedSegment> unfinished;
    const std::string open_data = std::string(kOpenSuffix) + std::string(kDataExtension);
    for (const Found& data : datas) {
        if (ends_with(data.name, open_data)) {
            unfinished.push_back(UnfinishedSegment{utf8_of(data.path), data.size, true});
            continue;
        }
        const std::string stem = data.name.substr(0, data.name.size() - kDataExtension.size());
        const std::string meta_name = stem + std::string(kMetaExtension);
        const bool has_meta = std::any_of(metas.begin(), metas.end(), [&](const Found& m) {
            return m.name == meta_name && m.path.parent_path() == data.path.parent_path();
        });
        if (!has_meta) {
            unfinished.push_back(UnfinishedSegment{utf8_of(data.path), data.size, false});
        }
    }

    std::sort(segments.begin(), segments.end(), segment_order);
    std::sort(unfinished.begin(), unfinished.end(),
              [](const UnfinishedSegment& a, const UnfinishedSegment& b) {
                  return a.data_path < b.data_path;
              });
    std::sort(known.begin(), known.end(),
              [](const KnownProblem& a, const KnownProblem& b) { return a.path < b.path; });

    std::vector<CatalogProblem> problems = std::move(day_problems);
    for (const KnownProblem& p : known) {
        problems.push_back(CatalogProblem{p.path, p.reason});
    }

    segments_ = std::move(segments);
    unfinished_ = std::move(unfinished);
    known_problems_ = std::move(known);
    problems_ = std::move(problems);
    return {};
}

std::vector<CatalogSegment> Catalog::overlapping(std::int64_t from_ns, std::int64_t to_ns) const
{
    std::vector<CatalogSegment> out;
    for (const CatalogSegment& segment : segments_) {
        if (segment.info.start_ns() < to_ns && segment.end_ns() > from_ns) {
            out.push_back(segment);
        }
    }
    return out;
}

std::optional<CatalogSegment> Catalog::containing(std::uint64_t stream_epoch,
                                                  dsp::SampleIndex index) const
{
    for (const CatalogSegment& segment : segments_) {
        if (segment.info.stream_epoch == stream_epoch && index >= segment.info.first_index &&
            index < segment.info.end_index()) {
            return segment;
        }
    }
    return std::nullopt;
}

std::optional<CatalogSegment> Catalog::at_time(std::int64_t instant_ns) const
{
    const CatalogSegment* best = nullptr;
    for (const CatalogSegment& segment : segments_) {
        if (segment.info.start_ns() <= instant_ns && instant_ns < segment.end_ns()) {
            best = &segment;  // later starts sort later, so the last match wins
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    return *best;
}

std::uint64_t Catalog::total_bytes() const
{
    std::uint64_t total = 0;
    for (const CatalogSegment& segment : segments_) {
        total += segment.data_bytes + segment.meta_bytes;
    }
    for (const UnfinishedSegment& data : unfinished_) {
        total += data.data_bytes;
    }
    return total;
}

}  // namespace revenant::capture
