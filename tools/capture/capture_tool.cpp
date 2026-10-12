#include "tools/capture/capture_tool.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "core/capture/catalog.h"
#include "core/capture/format.h"
#include "core/capture/index.h"
#include "core/capture/search.h"
#include "core/capture/writer.h"
#include "core/source/capabilities.h"
#include "core/source/registry.h"

namespace revenant::capture_tool {

namespace {

namespace fs = std::filesystem;
using capture::Catalog;
using capture::CatalogSegment;

constexpr std::int64_t kNsPerSecond = 1'000'000'000;

// A replay that starts exactly on a hit's first annotated sample cuts off the
// key-up the detector needed a decision or two to see, so the suggested URI
// opens a second early and closes a second late.
constexpr std::int64_t kReplayMarginNs = kNsPerSecond;

// Wider than this and the bars wrap in an 80 column terminal next to the
// timestamp and the percentages.
constexpr std::size_t kBarWidth = 40;

// A bucket count past this is a typo in --bucket, not a chart anybody reads.
constexpr std::int64_t kMaxBuckets = 10'000;

// ---------------------------------------------------------------------------
// Text out
// ---------------------------------------------------------------------------

[[nodiscard]] std::string format_seconds(double seconds)
{
    if (seconds < 60.0) {
        return std::format("{:.3f}s", seconds);
    }
    const auto whole = static_cast<std::int64_t>(seconds);
    const double fraction = seconds - static_cast<double>(whole - whole % 60);
    if (whole < 3600) {
        return std::format("{}m{:06.3f}s", whole / 60, fraction);
    }
    return std::format("{}h{:02}m{:06.3f}s", whole / 3600, (whole / 60) % 60, fraction);
}

[[nodiscard]] std::string format_ns_span(std::int64_t ns)
{
    return format_seconds(static_cast<double>(ns) / static_cast<double>(kNsPerSecond));
}

[[nodiscard]] std::string format_bytes(std::uint64_t bytes)
{
    constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? std::format("{} B", bytes) : std::format("{:.1f} {}", value, units[unit]);
}

[[nodiscard]] std::string format_rate(dsp::SampleRate rate)
{
    if (rate >= 1'000'000) {
        return std::format("{:g} MS/s", static_cast<double>(rate) / 1e6);
    }
    if (rate >= 1'000) {
        return std::format("{:g} kS/s", static_cast<double>(rate) / 1e3);
    }
    return std::format("{} S/s", rate);
}

[[nodiscard]] std::string json_string(std::string_view text)
{
    std::string out = "\"";
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
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

[[nodiscard]] std::string json_number(double value)
{
    return std::isfinite(value) ? std::format("{}", value) : std::string("null");
}

// ---------------------------------------------------------------------------
// The command line
// ---------------------------------------------------------------------------

struct Arguments {
    std::string root;
    std::map<std::string, std::string, std::less<>> values;
    std::set<std::string, std::less<>> switches;

    [[nodiscard]] const std::string* value(std::string_view name) const
    {
        const auto found = values.find(name);
        return found == values.end() ? nullptr : &found->second;
    }
    [[nodiscard]] bool has(std::string_view name) const { return switches.contains(name); }
};

[[nodiscard]] std::string joined(std::span<const std::string_view> names)
{
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        out += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ");
        out += names[i];
    }
    return out;
}

// Each command names the flags it takes, and anything else is refused by
// name, because a misspelt filter that is silently ignored returns every hit
// and reads as an answer.
[[nodiscard]] Expected<Arguments> parse_arguments(std::string_view command,
                                                  std::span<const std::string> rest,
                                                  std::initializer_list<std::string_view> valued,
                                                  std::initializer_list<std::string_view> flags)
{
    Arguments parsed;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        std::string_view arg = rest[i];
        if (!arg.starts_with("--")) {
            if (!parsed.root.empty()) {
                return fail(std::format("{} takes one capture root and was given '{}' and '{}'. "
                                        "Quote a path that contains spaces.",
                                        command, parsed.root, arg));
            }
            parsed.root = std::string(arg);
            continue;
        }

        std::string_view inline_value;
        bool has_inline = false;
        if (const auto equals = arg.find('='); equals != std::string_view::npos) {
            inline_value = arg.substr(equals + 1);
            has_inline = true;
            arg = arg.substr(0, equals);
        }

        if (std::ranges::find(flags, arg) != flags.end()) {
            if (has_inline) {
                return fail(std::format("{} is a switch and takes no value; write {} alone",
                                        arg, arg));
            }
            parsed.switches.emplace(arg);
            continue;
        }
        if (std::ranges::find(valued, arg) != valued.end()) {
            std::string value;
            if (has_inline) {
                value = std::string(inline_value);
            } else if (i + 1 < rest.size()) {
                value = rest[++i];
            } else {
                return fail(std::format("{} needs a value, as in {} <value>", arg, arg));
            }
            if (parsed.values.contains(arg)) {
                return fail(std::format("{} was given twice; give it once", arg));
            }
            parsed.values.emplace(std::string(arg), std::move(value));
            continue;
        }

        std::vector<std::string_view> accepted(valued);
        accepted.insert(accepted.end(), flags.begin(), flags.end());
        if (accepted.empty()) {
            return fail(std::format("{} takes no options and was given {}", command, arg));
        }
        return fail(std::format("{} does not take {}. It takes {}.", command, arg,
                                joined(accepted)));
    }
    if (parsed.root.empty()) {
        return fail(std::format("{} needs a capture root, as in revenant-capture {} "
                                "D:/revenant/capture",
                                command, command));
    }
    return parsed;
}

struct Range {
    std::optional<std::int64_t> from_ns;
    std::optional<std::int64_t> to_ns;
};

[[nodiscard]] Expected<Range> parse_range(const Arguments& args, const Environment& env)
{
    Range range;
    if (const std::string* text = args.value("--from")) {
        auto from = parse_time(*text, "--from", env.now_ns);
        if (!from) {
            return std::unexpected(from.error());
        }
        range.from_ns = *from;
    }
    if (const std::string* text = args.value("--to")) {
        auto to = parse_time(*text, "--to", env.now_ns);
        if (!to) {
            return std::unexpected(to.error());
        }
        range.to_ns = *to;
    }
    if (range.from_ns && range.to_ns && *range.from_ns >= *range.to_ns) {
        return fail(std::format("--from {} is not before --to {}. Swap them, or widen the range.",
                                format_instant(*range.from_ns), format_instant(*range.to_ns)));
    }
    return range;
}

[[nodiscard]] Expected<Catalog> open_catalog(const std::string& root)
{
    auto catalog = Catalog::open(root);
    if (!catalog) {
        return std::unexpected(with_context(catalog.error(), std::format("capture root '{}'", root)));
    }
    return catalog;
}

[[nodiscard]] std::vector<CatalogSegment> segments_in(const Catalog& catalog, const Range& range)
{
    if (!range.from_ns && !range.to_ns) {
        return {catalog.segments().begin(), catalog.segments().end()};
    }
    return catalog.overlapping(range.from_ns.value_or(std::numeric_limits<std::int64_t>::min()),
                               range.to_ns.value_or(std::numeric_limits<std::int64_t>::max()));
}

// An absolute UTF-8 root, so a printed URI still means the same directory
// when it is pasted into a shell somewhere else.
[[nodiscard]] std::string absolute_root(const std::string& root)
{
    std::error_code ec;
    const fs::path path = fs::absolute(fs::path(std::u8string(root.begin(), root.end())), ec);
    if (ec) {
        return root;
    }
    const std::u8string text = path.lexically_normal().generic_u8string();
    std::string out(text.begin(), text.end());
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

[[nodiscard]] std::string path_text(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

[[nodiscard]] fs::path utf8_path(std::string_view text)
{
    return fs::path(std::u8string(text.begin(), text.end()));
}

// ---------------------------------------------------------------------------
// list
// ---------------------------------------------------------------------------

[[nodiscard]] Status run_list(std::span<const std::string> rest, const Environment& env)
{
    auto args = parse_arguments("list", rest, {"--from", "--to"}, {});
    if (!args) {
        return std::unexpected(args.error());
    }
    auto range = parse_range(*args, env);
    if (!range) {
        return std::unexpected(range.error());
    }
    auto catalog = open_catalog(args->root);
    if (!catalog) {
        return std::unexpected(catalog.error());
    }

    const std::vector<CatalogSegment> segments = segments_in(*catalog, *range);
    env.out(std::format("{:<24}  {:>11}  {:>12}  {:>9}  {:<4}  {:>10}  {:<9}  {:>5}", "start (UTC)",
                        "duration", "centre", "rate", "fmt", "size", "closed", "annot"));

    std::uint64_t samples_bytes = 0;
    std::int64_t captured_ns = 0;
    for (const CatalogSegment& segment : segments) {
        const std::int64_t span = segment.end_ns() - segment.info.start_ns();
        captured_ns += span;
        samples_bytes += segment.data_bytes;
        env.out(std::format("{:<24}  {:>11}  {:>12}  {:>9}  {:<4}  {:>10}  {:<9}  {:>5}",
                            format_instant(segment.info.start_ns()), format_ns_span(span),
                            format_frequency(segment.info.center), format_rate(segment.info.rate),
                            source::format_name(segment.info.format),
                            format_bytes(segment.data_bytes + segment.meta_bytes),
                            segment.close_reason.empty() ? "-" : segment.close_reason,
                            segment.annotations.size()));
    }

    env.out("");
    if (segments.empty()) {
        env.out(range->from_ns || range->to_ns ? "no segments in that range"
                                               : "no closed segments under this root");
    } else {
        env.out(std::format("{} segment{}, {} of samples in {}, {} to {}", segments.size(),
                            segments.size() == 1 ? "" : "s", format_ns_span(captured_ns),
                            format_bytes(samples_bytes), format_instant(segments.front().info.start_ns()),
                            format_instant(segments.back().end_ns())));
    }
    env.out(std::format("{} on disk under the root in all", format_bytes(catalog->total_bytes())));

    // A gap is reported per epoch because indices only compare within one: a
    // new epoch is a restarted source, which is a different fact from lost
    // samples and already visible as a new stream.
    std::map<std::uint64_t, std::vector<const CatalogSegment*>> by_epoch;
    for (const CatalogSegment& segment : segments) {
        by_epoch[segment.info.stream_epoch].push_back(&segment);
    }
    std::size_t gaps = 0;
    for (auto& [epoch, run] : by_epoch) {
        std::ranges::sort(run, {}, [](const CatalogSegment* s) { return s->info.first_index; });
        for (std::size_t i = 1; i < run.size(); ++i) {
            const CatalogSegment& before = *run[i - 1];
            const CatalogSegment& after = *run[i];
            if (after.info.first_index <= before.info.end_index()) {
                continue;
            }
            if (gaps++ == 0) {
                env.out("");
                env.out("gaps:");
            }
            const std::uint64_t lost = after.info.first_index - before.info.end_index();
            env.out(std::format("  epoch {:016x}: {} samples ({}) missing from {} to {}", epoch,
                                lost,
                                format_seconds(static_cast<double>(lost) /
                                               static_cast<double>(after.info.rate)),
                                format_instant(before.end_ns()),
                                format_instant(after.info.start_ns())));
        }
    }

    if (!catalog->unfinished().empty()) {
        env.out("");
        env.out("unfinished:");
        for (const auto& file : catalog->unfinished()) {
            env.out(std::format("  {}  {}  {}",
                                file.in_progress ? "being written" : "orphaned     ",
                                format_bytes(file.data_bytes), file.data_path));
        }
        env.out("  A file being written with no writer running is a crash's leftover: "
                "revenant-capture recover finalises it. An orphan has a closed name and no "
                "meta, and nothing can say what its samples were.");
    }
    if (!catalog->problems().empty()) {
        env.out("");
        env.out("problems:");
        for (const auto& problem : catalog->problems()) {
            env.out(std::format("  {}: {}", problem.path, problem.reason));
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Annotations for search and occupancy
// ---------------------------------------------------------------------------

// The catalog's annotations do not say which generator wrote them, and
// --generator filters on that, so the meta is read again here. A meta that
// will not read is said and skipped: one bad sidecar does not hide the rest.
[[nodiscard]] std::vector<capture::IndexedSegment> indexed_segments(
    std::span<const CatalogSegment> segments, const Environment& env)
{
    std::vector<capture::IndexedSegment> out;
    out.reserve(segments.size());
    for (const CatalogSegment& segment : segments) {
        auto annotations = capture::read_annotations(segment.meta_path);
        if (!annotations) {
            env.out(std::format("skipped {}: {}", segment.meta_path, annotations.error().message));
            continue;
        }
        auto stamps = capture::read_index_stamps(segment.meta_path);
        if (!stamps) {
            env.out(std::format("skipped {}: {}", segment.meta_path, stamps.error().message));
            continue;
        }
        out.push_back({segment.data_path, segment.info, std::move(*annotations), std::move(*stamps)});
    }
    return out;
}

[[nodiscard]] Status read_filters(const Arguments& args, capture::Query& query)
{
    if (const std::string* text = args.value("--min-confidence")) {
        double value = 0.0;
        const auto parsed = std::from_chars(text->data(), text->data() + text->size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size() ||
            value < 0.0 || value > 1.0) {
            return fail(std::format("--min-confidence '{}' is not a number from 0 to 1, such as 0.8",
                                    *text));
        }
        query.min_confidence = value;
    }
    if (const std::string* text = args.value("--min-snr")) {
        std::string_view digits = *text;
        if (digits.ends_with("dB") || digits.ends_with("db")) {
            digits.remove_suffix(2);
        }
        double value = 0.0;
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (digits.empty() || parsed.ec != std::errc{} ||
            parsed.ptr != digits.data() + digits.size()) {
            return fail(std::format("--min-snr '{}' is not a level in dB, such as 10 or 6.5",
                                    *text));
        }
        query.min_snr_db = value;
    }
    if (const std::string* text = args.value("--min-duration")) {
        auto duration = parse_duration(*text, "--min-duration");
        if (!duration) {
            return std::unexpected(duration.error());
        }
        query.min_duration_ns = *duration;
    }
    if (const std::string* text = args.value("--family")) {
        query.family = *text;
    }
    if (const std::string* text = args.value("--generator")) {
        if (*text != capture::kDetectGenerator && *text != capture::kIndexGenerator) {
            return fail(std::format("--generator '{}' is not one this capture writes. Use {} for "
                                    "the live detector or {} for revenant-capture index.",
                                    *text, capture::kDetectGenerator, capture::kIndexGenerator));
        }
        query.generator = *text;
    }
    return {};
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

[[nodiscard]] Status run_search(std::span<const std::string> rest, const Environment& env)
{
    auto args = parse_arguments("search", rest,
                                {"--from", "--to", "--freq", "--min-confidence", "--min-snr",
                                 "--min-duration", "--family", "--generator"},
                                {"--json"});
    if (!args) {
        return std::unexpected(args.error());
    }
    auto range = parse_range(*args, env);
    if (!range) {
        return std::unexpected(range.error());
    }
    capture::Query query;
    query.from_ns = range->from_ns;
    query.to_ns = range->to_ns;
    if (const std::string* text = args->value("--freq")) {
        auto band = parse_band(*text, "--freq");
        if (!band) {
            return std::unexpected(band.error());
        }
        query.freq_low = band->first;
        query.freq_high = band->second;
    }
    if (auto filters = read_filters(*args, query); !filters) {
        return std::unexpected(filters.error());
    }

    auto catalog = open_catalog(args->root);
    if (!catalog) {
        return std::unexpected(catalog.error());
    }
    const std::vector<CatalogSegment> segments = segments_in(*catalog, *range);
    const std::vector<capture::IndexedSegment> indexed = indexed_segments(segments, env);
    const std::vector<capture::Hit> hits = capture::run_query(indexed, query);
    const std::string root = absolute_root(args->root);

    const auto replay_of = [&](const capture::Hit& hit) {
        return capture_uri(root, hit.start_ns - kReplayMarginNs, hit.end_ns + kReplayMarginNs);
    };
    const auto vrx_of = [&](const capture::Hit& hit) {
        return format_frequency(hit.freq_lower + (hit.freq_upper - hit.freq_lower) / 2);
    };

    if (args->has("--json")) {
        env.out("[");
        for (std::size_t i = 0; i < hits.size(); ++i) {
            const capture::Hit& hit = hits[i];
            env.out(std::format(
                "  {{\"start\": {}, \"end\": {}, \"start_ns\": {}, \"end_ns\": {}, "
                "\"freq_lower\": {}, \"freq_upper\": {}, \"confidence\": {}, \"snr_db\": {}, "
                "\"peak_db\": {}, \"family\": {}, \"generator\": {}, \"data_path\": {}, "
                "\"stream_epoch\": {}, \"first_index\": {}, \"sample_start\": {}, "
                "\"sample_count\": {}, \"replay\": {}, \"vrx\": {}}}{}",
                json_string(format_instant(hit.start_ns)), json_string(format_instant(hit.end_ns)),
                hit.start_ns, hit.end_ns, hit.freq_lower, hit.freq_upper,
                json_number(hit.confidence), json_number(hit.snr_db), json_number(hit.peak_db),
                json_string(hit.family), json_string(hit.generator), json_string(hit.data_path),
                hit.stream_epoch, hit.first_index, hit.sample_start, hit.sample_count,
                json_string(replay_of(hit)), json_string(vrx_of(hit)),
                i + 1 == hits.size() ? "" : ","));
        }
        env.out("]");
        return {};
    }

    for (const capture::Hit& hit : hits) {
        env.out(std::format("{}  {:>9}  {} to {}  conf {:.2f}  snr {:5.1f} dB  {}  {}",
                            format_instant(hit.start_ns), format_ns_span(hit.end_ns - hit.start_ns),
                            format_frequency(hit.freq_lower), format_frequency(hit.freq_upper),
                            hit.confidence, hit.snr_db, hit.family.empty() ? "-" : hit.family,
                            hit.generator));
        const std::string uri = replay_of(hit);
        env.out(std::format("    replay  {}", uri));
        env.out(std::format("    listen  revenant-cli \"{}\" --vrx {}", uri, vrx_of(hit)));
    }
    env.out(std::format("{} hit{} in {} segment{}", hits.size(), hits.size() == 1 ? "" : "s",
                        indexed.size(), indexed.size() == 1 ? "" : "s"));

    // A segment the index pass already found quiet is an answer, not a gap.
    const auto unannotated = static_cast<std::size_t>(
        std::ranges::count_if(indexed, [](const capture::IndexedSegment& s) {
            return s.annotations.empty() && s.stamps.empty();
        }));
    if (hits.empty() && unannotated > 0) {
        env.out(std::format("{} of them carry no annotations and were never indexed; "
                            "revenant-capture index {} runs the detector over them.",
                            unannotated, args->root));
    }
    return {};
}

// ---------------------------------------------------------------------------
// occupancy
// ---------------------------------------------------------------------------

[[nodiscard]] Status run_occupancy(std::span<const std::string> rest, const Environment& env)
{
    auto args = parse_arguments("occupancy", rest,
                                {"--from", "--to", "--freq", "--bucket", "--min-confidence",
                                 "--min-snr", "--min-duration", "--family", "--generator"},
                                {});
    if (!args) {
        return std::unexpected(args.error());
    }
    auto range = parse_range(*args, env);
    if (!range) {
        return std::unexpected(range.error());
    }
    const std::string* band_text = args->value("--freq");
    if (band_text == nullptr) {
        return fail("occupancy needs a band, as in --freq 162.4M:162.56M");
    }
    auto band = parse_band(*band_text, "--freq");
    if (!band) {
        return std::unexpected(band.error());
    }
    std::int64_t bucket_ns = 60 * kNsPerSecond;
    if (const std::string* text = args->value("--bucket")) {
        auto bucket = parse_duration(*text, "--bucket");
        if (!bucket) {
            return std::unexpected(bucket.error());
        }
        if (*bucket <= 0) {
            return fail(std::format("--bucket '{}' is not a positive duration; write 1m or 10s",
                                    *text));
        }
        bucket_ns = *bucket;
    }

    capture::OccupancyRequest request;
    request.freq_low = band->first;
    request.freq_high = band->second;
    request.bucket_ns = bucket_ns;
    if (auto filters = read_filters(*args, request.filter); !filters) {
        return std::unexpected(filters.error());
    }

    auto catalog = open_catalog(args->root);
    if (!catalog) {
        return std::unexpected(catalog.error());
    }
    const std::vector<CatalogSegment> segments = segments_in(*catalog, *range);
    if (segments.empty() && (!range->from_ns || !range->to_ns)) {
        return fail("there are no segments to chart. Give both --from and --to to chart a range "
                    "regardless.");
    }

    // An open end of the range closes on what is on disk, so a bare
    // "occupancy root --freq ..." charts the whole capture.
    request.from_ns = range->from_ns.value_or(segments.empty() ? 0 : segments.front().info.start_ns());
    std::int64_t last_end = request.from_ns;
    for (const CatalogSegment& segment : segments) {
        last_end = std::max(last_end, segment.end_ns());
    }
    request.to_ns = range->to_ns.value_or(last_end);

    const std::int64_t rows = (request.to_ns - request.from_ns + bucket_ns - 1) / bucket_ns;
    if (rows > kMaxBuckets) {
        return fail(std::format("--bucket {} over {} is {} rows. Use a wider bucket or a narrower "
                                "range; the chart stops at {}.",
                                format_ns_span(bucket_ns),
                                format_ns_span(request.to_ns - request.from_ns), rows, kMaxBuckets));
    }

    const std::vector<capture::IndexedSegment> indexed = indexed_segments(segments, env);
    auto buckets = capture::occupancy(indexed, request);
    if (!buckets) {
        return std::unexpected(buckets.error());
    }

    env.out(std::format("{} to {}, {} buckets of {}", format_frequency(request.freq_low),
                        format_frequency(request.freq_high), buckets->size(),
                        format_ns_span(bucket_ns)));
    env.out("# occupied   - captured and quiet   blank not captured");
    for (const capture::OccupancyBucket& bucket : *buckets) {
        const auto cells = [](double share) {
            return static_cast<std::size_t>(
                std::lround(std::clamp(share, 0.0, 1.0) * static_cast<double>(kBarWidth)));
        };
        // Something present for a sliver of a bucket still gets a mark, or a
        // short burst in a long bucket would draw as quiet.
        std::size_t occupied = cells(bucket.occupied);
        if (occupied == 0 && bucket.occupied > 0.0) {
            occupied = 1;
        }
        std::size_t captured = std::max(cells(bucket.captured), occupied);
        if (captured == 0 && bucket.captured > 0.0) {
            captured = 1;
        }
        std::string bar(occupied, '#');
        bar.append(captured - occupied, '-');
        bar.append(kBarWidth - captured, ' ');
        env.out(std::format("{}  |{}|  {:5.1f}% occupied  {:5.1f}% captured  {} hit{}",
                            format_instant(bucket.start_ns), bar, bucket.occupied * 100.0,
                            bucket.captured * 100.0, bucket.hits, bucket.hits == 1 ? "" : "s"));
    }
    return {};
}

// ---------------------------------------------------------------------------
// index
// ---------------------------------------------------------------------------

// What a later pass needs to judge whether rerunning would see the segment
// differently. The device is left out: it changes the speed, not the result.
[[nodiscard]] std::string index_settings(const capture::IndexOptions& options)
{
    return std::format("channels={} transform={} probes={} pace={} threshold_db={} "
                       "confidence={} average_s={}",
                       options.channels, options.spectrum_transform, options.probes, options.pace,
                       options.detector.detection_threshold_db,
                       options.detector.confidence_threshold, options.detector.average_seconds);
}

[[nodiscard]] Status run_index(std::span<const std::string> rest, const Environment& env)
{
    auto args = parse_arguments("index", rest, {"--from", "--to", "--gpu"}, {"--force"});
    if (!args) {
        return std::unexpected(args.error());
    }
    auto range = parse_range(*args, env);
    if (!range) {
        return std::unexpected(range.error());
    }
    capture::IndexOptions options;
    if (const std::string* text = args->value("--gpu")) {
        int index = 0;
        const auto parsed = std::from_chars(text->data(), text->data() + text->size(), index);
        if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size() || index < 0) {
            return fail(std::format("--gpu '{}' is not a device index. revenant-devices lists "
                                    "them; leave --gpu off to use REVENANT_GPU_INDEX.",
                                    *text));
        }
        options.gpu_index = index;
    }

    auto catalog = open_catalog(args->root);
    if (!catalog) {
        return std::unexpected(catalog.error());
    }
    const std::vector<CatalogSegment> segments = segments_in(*catalog, *range);
    const bool force = args->has("--force");
    const std::string settings = index_settings(options);

    std::size_t indexed = 0;
    std::size_t skipped = 0;
    std::size_t failed = 0;
    for (const CatalogSegment& segment : segments) {
        const std::string name = path_text(utf8_path(segment.data_path).filename());
        auto existing = capture::read_annotations(segment.meta_path);
        if (!existing) {
            env.out(std::format("{}: {}", name, existing.error().message));
            ++failed;
            continue;
        }
        auto stamps = capture::read_index_stamps(segment.meta_path);
        if (!stamps) {
            env.out(std::format("{}: {}", name, stamps.error().message));
            ++failed;
            continue;
        }
        if (capture::indexed_by(*stamps, *existing) && !force) {
            ++skipped;
            continue;
        }

        auto result = capture::index_segment(segment.data_path, segment.info, options);
        if (!result) {
            env.out(std::format("{}: {}", name, result.error().message));
            ++failed;
            continue;
        }
        if (auto written = capture::write_annotations(segment.meta_path, result->annotations,
                                                     capture::kIndexGenerator, settings);
            !written) {
            env.out(std::format("{}: {}", name, written.error().message));
            ++failed;
            continue;
        }
        ++indexed;
        const double speed =
            result->wall_seconds > 0.0 ? result->capture_seconds / result->wall_seconds : 0.0;
        env.out(std::format("{}  {} annotation{}  {} in {}  {:.1f}x realtime", name,
                            result->annotations.size(),
                            result->annotations.size() == 1 ? "" : "s",
                            format_seconds(result->capture_seconds),
                            format_seconds(result->wall_seconds), speed));
    }

    env.out(std::format("{} indexed, {} already indexed{}, {} failed", indexed, skipped,
                        skipped > 0 && !force ? " (--force redoes them)" : "", failed));
    if (failed > 0) {
        return fail(std::format("{} of {} segments could not be indexed; the reasons are above",
                                failed, segments.size()));
    }
    return {};
}

// ---------------------------------------------------------------------------
// extract
// ---------------------------------------------------------------------------

[[nodiscard]] Status run_extract(std::span<const std::string> rest, const Environment& env)
{
    auto args = parse_arguments("extract", rest, {"--from", "--to", "--out"}, {});
    if (!args) {
        return std::unexpected(args.error());
    }
    if (args->value("--from") == nullptr || args->value("--to") == nullptr) {
        return fail("extract needs both ends of the range, as in --from -10m --to now");
    }
    const std::string* out_text = args->value("--out");
    if (out_text == nullptr) {
        return fail("extract needs a directory to copy into, as in --out D:/extracts/tuesday");
    }
    auto range = parse_range(*args, env);
    if (!range) {
        return std::unexpected(range.error());
    }
    auto catalog = open_catalog(args->root);
    if (!catalog) {
        return std::unexpected(catalog.error());
    }
    const std::vector<CatalogSegment> segments = segments_in(*catalog, *range);
    if (segments.empty()) {
        return fail(std::format("nothing was captured between {} and {}; revenant-capture list "
                                "shows what is on disk",
                                format_instant(*range->from_ns), format_instant(*range->to_ns)));
    }

    const fs::path out = utf8_path(*out_text);
    std::error_code ec;
    fs::create_directories(out, ec);
    if (ec) {
        return fail(std::format("could not create --out '{}': {}", *out_text, ec.message()));
    }

    // Checked before the first copy, so a clash leaves the directory as it
    // was rather than holding half an extract.
    for (const CatalogSegment& segment : segments) {
        for (const std::string& from : {segment.data_path, segment.meta_path}) {
            const fs::path to = out / utf8_path(from).filename();
            if (fs::exists(to, ec)) {
                return fail(std::format("'{}' already exists. Extract into an empty directory, or "
                                        "remove it first.",
                                        path_text(to)));
            }
        }
    }

    std::uint64_t bytes = 0;
    for (const CatalogSegment& segment : segments) {
        for (const std::string& from : {segment.data_path, segment.meta_path}) {
            const fs::path to = out / utf8_path(from).filename();
            fs::copy_file(utf8_path(from), to, fs::copy_options::none, ec);
            if (ec) {
                return fail(std::format("copying '{}' to '{}': {}", from, path_text(to),
                                        ec.message()));
            }
        }
        bytes += segment.data_bytes + segment.meta_bytes;
        env.out(std::format("{}  {}", format_instant(segment.info.start_ns()),
                            path_text(utf8_path(segment.data_path).filename())));
    }

    env.out(std::format("copied {} segment{}, {}, into {}", segments.size(),
                        segments.size() == 1 ? "" : "s", format_bytes(bytes), path_text(out)));
    // A segment is copied whole because cutting one means rewriting its meta
    // and its annotations, and a copy of two files is what format.h promises
    // extraction is.
    env.out(std::format("Whole segments: the extract runs from {} to {}, which can be wider than "
                        "the {} to {} asked for.",
                        format_instant(segments.front().info.start_ns()),
                        format_instant(segments.back().end_ns()), format_instant(*range->from_ns),
                        format_instant(*range->to_ns)));
    return {};
}

// ---------------------------------------------------------------------------
// recover
// ---------------------------------------------------------------------------

[[nodiscard]] Status run_recover(std::span<const std::string> rest, const Environment& env)
{
    auto args = parse_arguments("recover", rest, {}, {});
    if (!args) {
        return std::unexpected(args.error());
    }
    auto report = capture::recover_open_segments(utf8_path(args->root));
    if (!report) {
        return std::unexpected(
            with_context(report.error(), std::format("recovering '{}'", args->root)));
    }
    env.out(std::format("{} segment{} finalised, {} empty leftover{} removed", report->recovered,
                        report->recovered == 1 ? "" : "s", report->removed_empty,
                        report->removed_empty == 1 ? "" : "s"));
    for (const std::string& failure : report->failures) {
        env.out(std::format("  not finalised: {}", failure));
    }
    if (!report->failures.empty()) {
        return fail(std::format("{} open segment{} could not be finalised; see above",
                                report->failures.size(), report->failures.size() == 1 ? "" : "s"));
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

Expected<std::int64_t> parse_duration(std::string_view text, std::string_view what)
{
    if (text.empty()) {
        return fail(std::format("{} is empty; write a duration such as 2s, 10m or 1h30m", what));
    }
    double total_ns = 0.0;
    std::string_view rest = text;
    while (!rest.empty()) {
        double value = 0.0;
        const auto parsed = std::from_chars(rest.data(), rest.data() + rest.size(), value,
                                            std::chars_format::fixed);
        if (parsed.ec != std::errc{} || value < 0.0) {
            return fail(std::format("{} '{}' is not a duration; write one such as 2s, 500ms, 10m "
                                    "or 1h30m",
                                    what, text));
        }
        rest.remove_prefix(static_cast<std::size_t>(parsed.ptr - rest.data()));
        std::size_t unit_length = 0;
        while (unit_length < rest.size() &&
               ((rest[unit_length] >= 'a' && rest[unit_length] <= 'z'))) {
            ++unit_length;
        }
        const std::string_view unit = rest.substr(0, unit_length);
        double scale = 0.0;
        if (unit == "ns") {
            scale = 1.0;
        } else if (unit == "us") {
            scale = 1e3;
        } else if (unit == "ms") {
            scale = 1e6;
        } else if (unit == "s") {
            scale = 1e9;
        } else if (unit == "m") {
            scale = 60e9;
        } else if (unit == "h") {
            scale = 3600e9;
        } else if (unit == "d") {
            scale = 86400e9;
        } else if (unit.empty()) {
            return fail(std::format("{} '{}' has no unit. A bare number could be seconds or "
                                    "samples; write {}s, or ms, m, h or d.",
                                    what, text, text));
        } else {
            return fail(std::format("{} '{}' has the unit '{}', which is not one of ns, us, ms, s, "
                                    "m, h or d",
                                    what, text, unit));
        }
        total_ns += value * scale;
        rest.remove_prefix(unit_length);
    }
    if (total_ns > static_cast<double>(std::numeric_limits<std::int64_t>::max() / 2)) {
        return fail(std::format("{} '{}' is longer than anything a capture can hold", what, text));
    }
    return static_cast<std::int64_t>(std::llround(total_ns));
}

Expected<std::int64_t> parse_time(std::string_view text, std::string_view what,
                                  std::int64_t now_ns)
{
    if (text == "now") {
        return now_ns;
    }
    std::string_view relative;
    if (text.starts_with("now-")) {
        relative = text.substr(4);
    } else if (text.starts_with('-') && text.size() > 1 &&
               text.find_first_not_of("-0123456789") != std::string_view::npos) {
        // A leading minus and a unit is "this long ago". A minus and digits
        // alone falls through to the epoch-nanoseconds reading.
        relative = text.substr(1);
    }
    if (!relative.empty()) {
        auto ago = parse_duration(relative, what);
        if (!ago) {
            return std::unexpected(ago.error());
        }
        return now_ns - *ago;
    }

    auto instant = capture::parse_capture_instant(text);
    if (!instant) {
        return fail(std::format("{}: {}. Relative times are also accepted: now, -10m, -1h30m.",
                                what, instant.error().message));
    }
    return instant;
}

Expected<std::pair<dsp::Hertz, dsp::Hertz>> parse_band(std::string_view text,
                                                       std::string_view what)
{
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        auto one = source::parse_frequency(text, what);
        if (!one) {
            return std::unexpected(one.error());
        }
        return std::pair{*one, *one};
    }
    if (text.find(':', colon + 1) != std::string_view::npos) {
        return fail(std::format("{} '{}' has more than one colon; write LOW:HIGH, such as "
                                "162.4M:162.56M",
                                what, text));
    }
    auto low = source::parse_frequency(text.substr(0, colon), what);
    if (!low) {
        return std::unexpected(low.error());
    }
    auto high = source::parse_frequency(text.substr(colon + 1), what);
    if (!high) {
        return std::unexpected(high.error());
    }
    if (*low > *high) {
        return fail(std::format("{} '{}' runs from high to low; write the lower edge first, as in "
                                "{}:{}",
                                what, text, text.substr(colon + 1), text.substr(0, colon)));
    }
    return std::pair{*low, *high};
}

std::string format_instant(std::int64_t ns)
{
    using namespace std::chrono;
    const sys_time<nanoseconds> time{nanoseconds{ns}};
    return std::format("{:%Y-%m-%dT%H:%M:%S}Z", floor<milliseconds>(time));
}

std::string format_frequency(dsp::Hertz hz)
{
    const bool negative = hz < 0;
    const auto magnitude = static_cast<std::uint64_t>(negative ? -hz : hz);
    const std::uint64_t whole = magnitude / 1'000'000;
    const std::uint64_t part = magnitude % 1'000'000;
    std::string out = std::format("{}{}", negative ? "-" : "", whole);
    if (part != 0) {
        std::string digits = std::format("{:06}", part);
        while (digits.back() == '0') {
            digits.pop_back();
        }
        out += '.';
        out += digits;
    }
    out += 'M';
    return out;
}

std::string capture_uri(std::string_view root, std::int64_t from_ns, std::int64_t to_ns)
{
    std::string path(root);
    std::ranges::replace(path, '\\', '/');

    std::string encoded;
    for (const char c : path) {
        const auto byte = static_cast<unsigned char>(c);
        const bool plain = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                           (byte >= '0' && byte <= '9') || c == '-' || c == '.' || c == '_' ||
                           c == '~' || c == '/' || c == ':';
        encoded += plain ? std::string(1, c) : std::format("%{:02X}", byte);
    }

    // A share is //server/share, which the registry reads back as the URI's
    // authority. A drive path needs the third slash so C: is not taken for a
    // host.
    std::string uri = "capture:";
    if (encoded.starts_with("//")) {
        uri += encoded;
    } else if (encoded.starts_with('/')) {
        uri += "//" + encoded;
    } else {
        uri += "///" + encoded;
    }
    return std::format("{}?from={}&to={}", uri, format_instant(from_ns), format_instant(to_ns));
}

std::string usage()
{
    return "revenant-capture: the rolling capture, listed, searched and cut out.\n"
           "\n"
           "  revenant-capture list <root> [--from T] [--to T]\n"
           "      Every closed segment, then totals, gaps inside a stream, unfinished files\n"
           "      and metas the catalog could not read.\n"
           "  revenant-capture search <root> [--from T] [--to T] [--freq LOW:HIGH]\n"
           "                   [--min-confidence X] [--min-snr DB] [--min-duration D]\n"
           "                   [--family S] [--generator G] [--json]\n"
           "      What was seen, one hit a line, with a capture:// URI that replays it and\n"
           "      the revenant-cli line that listens to it.\n"
           "  revenant-capture occupancy <root> --freq LOW:HIGH [--from T] [--to T]\n"
           "                   [--bucket D]\n"
           "      A bar per bucket (default 1m): occupied, captured and quiet, or not\n"
           "      captured at all.\n"
           "  revenant-capture index <root> [--from T] [--to T] [--force] [--gpu N]\n"
           "      Runs the detector over closed segments no index pass has run\n"
           "      over yet, or over all of them with --force. Needs the GPU;\n"
           "      REVENANT_GPU_INDEX picks it unless --gpu does.\n"
           "  revenant-capture extract <root> --from T --to T --out DIR\n"
           "      Copies the whole segments overlapping the range into DIR.\n"
           "  revenant-capture recover <root>\n"
           "      Finalises segments a dead writer left open. Run it only when nothing is\n"
           "      recording into the root: a live writer's open segment looks the same.\n"
           "\n"
           "Times: 2026-10-12T14:03:07.250Z (UTC, Z required), epoch nanoseconds, now,\n"
           "or a duration ago such as -10m or -1h30m.\n"
           "Frequencies: 162.55M, 98.1M, 7074k, 1090000000. A band is LOW:HIGH.\n"
           "Durations: 500ms, 2s, 10m, 1h30m, 1d.\n";
}

Status run(std::span<const std::string> args, const Environment& env)
{
    if (args.empty()) {
        return fail("give a command: list, search, occupancy, index, extract or recover. "
                    "revenant-capture --help shows each.");
    }
    const std::string& command = args.front();
    const std::span<const std::string> rest = args.subspan(1);
    if (command == "list") {
        return run_list(rest, env);
    }
    if (command == "search") {
        return run_search(rest, env);
    }
    if (command == "occupancy") {
        return run_occupancy(rest, env);
    }
    if (command == "index") {
        return run_index(rest, env);
    }
    if (command == "extract") {
        return run_extract(rest, env);
    }
    if (command == "recover") {
        return run_recover(rest, env);
    }
    return fail(std::format("'{}' is not a command. The commands are list, search, occupancy, "
                            "index, extract and recover.",
                            command));
}

}  // namespace revenant::capture_tool
