// The URI layer: the grammar, the query parser, and the table that maps a
// scheme onto a backend.
//
// Everything about the text of a source lives here. The backends take structs
// and know nothing about percent escapes or ampersands, which keeps the
// grammar in one reviewable place instead of being reimplemented slightly
// differently per backend.
//
// The rule that shapes the whole file: an unknown query parameter is an
// error. Not a warning, not a log line, not something to ignore. A typo in
// `rate` that silently falls back to a default produces a capture whose
// declared sample rate is wrong, and every frequency derived from it
// afterwards is wrong by the same ratio with nothing anywhere to catch it.
// Failing at open costs a second; not failing costs the capture. siggen's own
// Options::reject_unused takes the same position for the same reason, and
// this is the same idea one layer up.

#include "core/source/registry.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/source/file_source.h"
#include "core/source/synthetic_source.h"

namespace revenant::source {
namespace {

// What a scheme is and whether it turns up in a device list on its own.
//
// THE REGISTRATION SEAM. Adding the RTL-SDR backend at WP5 is one more row
// here, one more case in resolve_uri, and a probe in enumerate_sources that
// asks libusb what is attached. It is a compile-time table and a switch
// rather than a runtime registration list on purpose: with three backends a
// table is reviewable at a glance, while a registration list immediately
// needs an ordering rule, a duplicate rule and a static-initialisation story
// that nothing here yet needs. Swap it for one when a plugin has to add a
// backend from outside this library, and not before.
struct Backend {
    std::string_view scheme;
    std::string_view summary;

    // True when the backend can be listed without being told where to look. A
    // file cannot: it is named, not discovered.
    bool enumerable = false;

    // The URI enumerate_sources() reports, for an enumerable backend.
    std::string_view default_uri;
    std::string_view default_name;
};

constexpr std::array<Backend, 2> kBackends{
    Backend{"synthetic", "a synthesised wideband scene", true, "synthetic:wideband",
            "Synthetic wideband scene"},
    Backend{"file", "a recorded raw IQ file", false, "", ""},
};

[[nodiscard]] std::string known_schemes()
{
    std::string out;
    for (const Backend& backend : kBackends) {
        if (!out.empty()) {
            out += ", ";
        }
        out += backend.scheme;
        out += " (";
        out += backend.summary;
        out += ")";
    }
    return out;
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Percent escapes only. A '+' stays a plus rather than becoming a space:
// that substitution belongs to HTML form encoding, not to URIs, and applying
// it here would corrupt any path containing a plus.
[[nodiscard]] Expected<std::string> percent_decode(std::string_view text, std::string_view what)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out.push_back(text[i]);
            continue;
        }
        if (i + 2 >= text.size()) {
            return fail(std::format("{} ends in a truncated percent escape: '{}'", what, text));
        }
        const int high = hex_value(text[i + 1]);
        const int low = hex_value(text[i + 2]);
        if (high < 0 || low < 0) {
            return fail(std::format("{} contains '%{}{}', which is not a percent escape", what,
                                    text[i + 1], text[i + 2]));
        }
        out.push_back(static_cast<char>(high * 16 + low));
        i += 2;
    }
    return out;
}

[[nodiscard]] std::string lowercased(std::string_view text)
{
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// The URI
// ---------------------------------------------------------------------------

struct ParsedUri {
    std::string original;

    // Lowercased, because RFC 3986 says a scheme is case-insensitive and a
    // user typing File:// should not get a different error from one typing
    // file://.
    std::string scheme;

    // Decoded. For file this is the filesystem path; for synthetic it is the
    // scene kind.
    std::string body;

    std::string_view query;
};

[[nodiscard]] Expected<ParsedUri> split_uri(std::string_view uri)
{
    const std::size_t colon = uri.find(':');
    if (colon == 0 || colon == std::string_view::npos) {
        return fail(std::format(
            "'{}' is not a source URI: it needs a scheme. Known schemes are {}.", uri,
            known_schemes()));
    }

    ParsedUri parsed;
    parsed.original = std::string(uri);
    parsed.scheme = lowercased(uri.substr(0, colon));

    std::string_view rest = uri.substr(colon + 1);
    const std::size_t question = rest.find('?');
    if (question != std::string_view::npos) {
        parsed.query = rest.substr(question + 1);
        rest = rest.substr(0, question);
    }

    std::string_view authority;
    if (rest.starts_with("//")) {
        rest.remove_prefix(2);
        const std::size_t slash = rest.find('/');
        if (slash == std::string_view::npos) {
            authority = rest;
            rest = {};
        } else {
            authority = rest.substr(0, slash);
            rest = rest.substr(slash);
        }
    }

    auto decoded_path = percent_decode(rest, "the path in the URI");
    if (!decoded_path) {
        return std::unexpected(decoded_path.error());
    }
    std::string body = std::move(*decoded_path);

    if (parsed.scheme == "file") {
        const std::string host = lowercased(authority);
        if (host.empty() || host == "localhost") {
            // file:///C:/captures/x.cf32 arrives here as "/C:/captures/x.cf32".
            // The leading slash is the URI's, not the path's, and Windows
            // will not open a path that starts with one before a drive
            // letter.
            if (body.size() >= 3 && body[0] == '/' && body[2] == ':') {
                body.erase(0, 1);
            }
        } else {
            // file://server/share/x.cf32 is a UNC path, which Windows takes
            // as \\server\share\x.cf32. Rebuilding it rather than rejecting
            // it means a capture on a network share needs no special case.
            auto decoded_host = percent_decode(authority, "the host in the URI");
            if (!decoded_host) {
                return std::unexpected(decoded_host.error());
            }
            body = "//" + *decoded_host + body;
        }
    } else if (!authority.empty()) {
        auto decoded_authority = percent_decode(authority, "the authority in the URI");
        if (!decoded_authority) {
            return std::unexpected(decoded_authority.error());
        }
        body = *decoded_authority + body;
    }

    parsed.body = std::move(body);
    return parsed;
}

// ---------------------------------------------------------------------------
// The query
// ---------------------------------------------------------------------------

// Same shape as siggen's Options, deliberately: read each key with a
// fallback, then reject whatever nobody read. The list of accepted keys is
// therefore the list of keys somebody asked for, which cannot drift out of
// step with the code the way a hand-maintained list does.
class Query {
public:
    [[nodiscard]] static Expected<Query> parse(std::string_view text);

    [[nodiscard]] bool present(std::string_view key) const;

    [[nodiscard]] Expected<std::int64_t> integer(std::string_view key, std::int64_t fallback);
    [[nodiscard]] Expected<std::uint64_t> unsigned_integer(std::string_view key,
                                                           std::uint64_t fallback);
    [[nodiscard]] Expected<double> real(std::string_view key, double fallback);
    [[nodiscard]] Expected<std::string> text(std::string_view key, std::string fallback);
    [[nodiscard]] Expected<bool> boolean(std::string_view key, bool fallback);

    [[nodiscard]] Status reject_unknown(std::string_view backend) const;

private:
    struct Entry {
        std::string key;
        std::string value;
        bool used = false;
    };

    [[nodiscard]] Entry* find(std::string_view key);
    [[nodiscard]] std::string accepted_keys() const;

    std::vector<Entry> entries_;
    std::vector<std::string> requested_;
};

Expected<Query> Query::parse(std::string_view text)
{
    Query query;
    if (text.empty()) {
        return query;
    }

    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t amp = text.find('&', start);
        const std::string_view piece =
            text.substr(start, amp == std::string_view::npos ? std::string_view::npos
                                                             : amp - start);
        if (piece.empty()) {
            return fail("the URI query has an empty parameter, which is a stray '&'");
        }

        const std::size_t equals = piece.find('=');
        if (equals == std::string_view::npos) {
            return fail(std::format(
                "query parameter '{}' has no value; every parameter is key=value, and a bare "
                "flag would have to mean something the reader has to guess at",
                piece));
        }
        if (equals == 0) {
            return fail("the URI query has a parameter with an empty name");
        }

        auto key = percent_decode(piece.substr(0, equals), "a query parameter name");
        if (!key) {
            return std::unexpected(key.error());
        }
        auto value = percent_decode(piece.substr(equals + 1), "a query parameter value");
        if (!value) {
            return std::unexpected(value.error());
        }

        for (const Entry& existing : query.entries_) {
            if (existing.key == *key) {
                return fail(std::format(
                    "query parameter '{}' appears twice, with '{}' and '{}'. Which one was "
                    "meant is not something to guess at.",
                    *key, existing.value, *value));
            }
        }

        query.entries_.push_back(Entry{std::move(*key), std::move(*value), false});

        if (amp == std::string_view::npos) {
            break;
        }
        start = amp + 1;
    }

    return query;
}

Query::Entry* Query::find(std::string_view key)
{
    for (Entry& entry : entries_) {
        if (entry.key == key) {
            return &entry;
        }
    }
    return nullptr;
}

bool Query::present(std::string_view key) const
{
    for (const Entry& entry : entries_) {
        if (entry.key == key) {
            return true;
        }
    }
    return false;
}

std::string Query::accepted_keys() const
{
    std::string out;
    for (const std::string& key : requested_) {
        if (!out.empty()) {
            out += ", ";
        }
        out += key;
    }
    return out;
}

Status Query::reject_unknown(std::string_view backend) const
{
    for (const Entry& entry : entries_) {
        if (!entry.used) {
            return fail(std::format(
                "the {} backend does not take a query parameter called '{}'. It accepts: {}. "
                "An unknown parameter is refused rather than ignored, because a typo that "
                "falls back to a default produces a capture whose metadata is wrong and "
                "nothing later can tell.",
                backend, entry.key, accepted_keys()));
        }
    }
    return {};
}

Expected<std::int64_t> Query::integer(std::string_view key, std::int64_t fallback)
{
    requested_.emplace_back(key);
    Entry* entry = find(key);
    if (entry == nullptr) {
        return fallback;
    }
    entry->used = true;

    std::string_view body = entry->value;
    if (body.starts_with('+')) {
        body.remove_prefix(1);
    }

    std::int64_t value = 0;
    const char* first = body.data();
    const char* last = first + body.size();
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last) {
        return fail(std::format(
            "'{}' is not a whole number, and {}= takes one. Frequencies and sample rates are "
            "integer hertz throughout, per docs/conventions.md, so 20e6 and 20.0 are both "
            "refused rather than rounded.",
            entry->value, key));
    }
    return value;
}

Expected<std::uint64_t> Query::unsigned_integer(std::string_view key, std::uint64_t fallback)
{
    requested_.emplace_back(key);
    Entry* entry = find(key);
    if (entry == nullptr) {
        return fallback;
    }
    entry->used = true;

    std::string_view body = entry->value;
    if (body.starts_with('+')) {
        body.remove_prefix(1);
    }

    std::uint64_t value = 0;
    const char* first = body.data();
    const char* last = first + body.size();
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last) {
        return fail(std::format("'{}' is not a whole number at or above zero, and {}= takes one",
                                entry->value, key));
    }
    return value;
}

Expected<double> Query::real(std::string_view key, double fallback)
{
    requested_.emplace_back(key);
    Entry* entry = find(key);
    if (entry == nullptr) {
        return fallback;
    }
    entry->used = true;

    double value = 0.0;
    const char* first = entry->value.data();
    const char* last = first + entry->value.size();
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last || !std::isfinite(value)) {
        return fail(std::format("'{}' is not a finite number, and {}= takes one", entry->value,
                                key));
    }
    return value;
}

Expected<std::string> Query::text(std::string_view key, std::string fallback)
{
    requested_.emplace_back(key);
    Entry* entry = find(key);
    if (entry == nullptr) {
        return fallback;
    }
    entry->used = true;
    return entry->value;
}

Expected<bool> Query::boolean(std::string_view key, bool fallback)
{
    requested_.emplace_back(key);
    Entry* entry = find(key);
    if (entry == nullptr) {
        return fallback;
    }
    entry->used = true;

    const std::string value = lowercased(entry->value);
    if (value == "1" || value == "true" || value == "on" || value == "yes") {
        return true;
    }
    if (value == "0" || value == "false" || value == "off" || value == "no") {
        return false;
    }
    return fail(std::format("'{}' is not a yes or a no, and {}= takes one: 1, true, on, yes, 0, "
                            "false, off or no",
                            entry->value, key));
}

// ---------------------------------------------------------------------------
// Backend configuration from a URI
// ---------------------------------------------------------------------------

[[nodiscard]] Expected<FileSourceConfig> file_config_of(const ParsedUri& uri)
{
    if (uri.body.empty()) {
        return fail("a file URI needs a path: file:///C:/captures/hf.cf32?rate=2400000");
    }

    auto query = Query::parse(uri.query);
    if (!query) {
        return std::unexpected(query.error());
    }

    auto rate = query->integer("rate", 0);
    if (!rate) {
        return std::unexpected(rate.error());
    }
    auto format_name = query->text("format", "");
    if (!format_name) {
        return std::unexpected(format_name.error());
    }
    auto center = query->integer("center", 0);
    if (!center) {
        return std::unexpected(center.error());
    }
    auto anchor = query->integer("anchor_ns", 0);
    if (!anchor) {
        return std::unexpected(anchor.error());
    }
    auto accuracy = query->integer("anchor_accuracy_ns", 0);
    if (!accuracy) {
        return std::unexpected(accuracy.error());
    }
    auto ppm = query->real("ppm_uncertainty", 0.0);
    if (!ppm) {
        return std::unexpected(ppm.error());
    }
    auto name = query->text("name", "");
    if (!name) {
        return std::unexpected(name.error());
    }

    const bool anchor_given = query->present("anchor_ns");
    const bool accuracy_given = query->present("anchor_accuracy_ns");

    if (auto clean = query->reject_unknown("file"); !clean) {
        return std::unexpected(clean.error());
    }

    if (*rate <= 0) {
        return fail("a file URI needs rate= with a positive integer number of samples per "
                    "second: a raw IQ file has no header, so nothing in the bytes says what "
                    "rate they were taken at");
    }

    SampleFormat format = SampleFormat::Cf32;
    if (format_name->empty()) {
        auto inferred = sample_format_from_extension(uri.body);
        if (!inferred) {
            return std::unexpected(inferred.error());
        }
        format = *inferred;
    } else {
        auto named = sample_format_from_name(*format_name);
        if (!named) {
            return std::unexpected(named.error());
        }
        format = *named;
    }

    FileSourceConfig config;
    config.uri = uri.original;
    config.path = uri.body;
    config.display_name = std::move(*name);
    config.rate = *rate;
    config.format = format;
    config.center_hz = *center;
    config.anchor_given = anchor_given;
    config.anchor_ns = *anchor;
    config.anchor_accuracy_given = accuracy_given;
    config.anchor_accuracy_ns = *accuracy;
    config.ppm_uncertainty = *ppm;
    return config;
}

[[nodiscard]] std::vector<std::string> split_commas(std::string_view text)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string_view piece =
            text.substr(start, comma == std::string_view::npos ? std::string_view::npos
                                                               : comma - start);
        if (!piece.empty()) {
            out.emplace_back(piece);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

[[nodiscard]] Expected<SyntheticSourceConfig> synthetic_config_of(const ParsedUri& uri)
{
    const std::string kind = uri.body.empty() ? std::string("wideband") : uri.body;
    if (kind != "wideband") {
        return fail(std::format(
            "'{}' is not a synthetic scene this backend knows. The only kind is "
            "synthetic:wideband, which is the many-emitter scene from tools/siggen.",
            kind));
    }

    auto query = Query::parse(uri.query);
    if (!query) {
        return std::unexpected(query.error());
    }

    // Caught before the general unknown-key message, because somebody
    // arriving from the siggen command line will reach for it and the
    // specific answer is more use than a list.
    if (query->present("threads")) {
        return fail("the synthetic source does not take threads=. Its scene is built with one "
                    "worker on purpose: the engine already owns a thread pool, and a second "
                    "pool sized to the same machine fights the first for the same cores. "
                    "Partitioning a block across the engine pool is the way to use more "
                    "threads here, not this.");
    }

    SyntheticSourceConfig config;

    auto rate = query->integer("rate", config.rate);
    if (!rate) {
        return std::unexpected(rate.error());
    }
    auto center = query->integer("center", 0);
    if (!center) {
        return std::unexpected(center.error());
    }
    auto duration = query->real("duration", 0.0);
    if (!duration) {
        return std::unexpected(duration.error());
    }
    auto samples = query->unsigned_integer("samples", 0);
    if (!samples) {
        return std::unexpected(samples.error());
    }
    auto seed = query->unsigned_integer("seed", 0);
    if (!seed) {
        return std::unexpected(seed.error());
    }
    auto anchor = query->integer("anchor_ns", 0);
    if (!anchor) {
        return std::unexpected(anchor.error());
    }
    auto emitters = query->unsigned_integer("emitters", config.emitters);
    if (!emitters) {
        return std::unexpected(emitters.error());
    }
    auto bursts = query->unsigned_integer("bursts", config.bursts_per_emitter);
    if (!bursts) {
        return std::unexpected(bursts.error());
    }
    auto span_low = query->integer("span_low", 0);
    if (!span_low) {
        return std::unexpected(span_low.error());
    }
    auto span_high = query->integer("span_high", 0);
    if (!span_high) {
        return std::unexpected(span_high.error());
    }
    auto snr_min = query->real("snr_min", config.snr_min_db);
    if (!snr_min) {
        return std::unexpected(snr_min.error());
    }
    auto snr_max = query->real("snr_max", config.snr_max_db);
    if (!snr_max) {
        return std::unexpected(snr_max.error());
    }
    auto min_burst = query->real("min_burst", config.min_burst_seconds);
    if (!min_burst) {
        return std::unexpected(min_burst.error());
    }
    auto max_burst = query->real("max_burst", config.max_burst_seconds);
    if (!max_burst) {
        return std::unexpected(max_burst.error());
    }
    auto noise = query->boolean("noise", true);
    if (!noise) {
        return std::unexpected(noise.error());
    }
    auto noise_dbfs = query->real("noise_dbfs", config.noise_power_full_band_dbfs);
    if (!noise_dbfs) {
        return std::unexpected(noise_dbfs.error());
    }
    auto modes = query->text("modes", "");
    if (!modes) {
        return std::unexpected(modes.error());
    }
    auto name = query->text("name", "");
    if (!name) {
        return std::unexpected(name.error());
    }

    const bool span_given = query->present("span_low") || query->present("span_high");
    const bool duration_given = query->present("duration");
    const bool samples_given = query->present("samples");

    if (auto clean = query->reject_unknown("synthetic"); !clean) {
        return std::unexpected(clean.error());
    }

    if (*rate <= 0) {
        return fail(std::format("rate= must be a positive integer number of samples per second, "
                                "got {}",
                                *rate));
    }
    if (duration_given && samples_given) {
        return fail("duration= and samples= both say how long the scene is; give one");
    }
    if (span_given && (!query->present("span_low") || !query->present("span_high"))) {
        return fail("span_low= and span_high= come as a pair: half a placement window is not a "
                    "window");
    }

    dsp::SampleIndex length = 0;
    if (samples_given) {
        length = *samples;
        if (length == 0) {
            return fail("samples=0 asks for nothing. Leave both duration and samples out for an "
                        "unbounded scene, which is what a live source looks like.");
        }
    } else if (duration_given) {
        if (*duration <= 0.0) {
            return fail(std::format("duration= must be a positive number of seconds, got {}",
                                    *duration));
        }
        // Rounded exactly as siggen's command line rounds it, so the same
        // duration at the same rate produces the same sample count and the
        // two stay byte-identical.
        length = static_cast<dsp::SampleIndex>(
            std::llround(*duration * static_cast<double>(*rate)));
        if (length == 0) {
            return fail(std::format("duration={} s at {} S/s rounds to zero samples", *duration,
                                    *rate));
        }
    }

    config.uri = uri.original;
    config.display_name = std::move(*name);
    config.rate = *rate;
    config.center_hz = *center;
    config.duration_samples = length;
    config.seed = *seed;
    config.anchor_ns = *anchor;
    config.add_noise = *noise;
    config.noise_power_full_band_dbfs = *noise_dbfs;
    config.emitters = static_cast<std::size_t>(*emitters);
    config.bursts_per_emitter = static_cast<std::size_t>(*bursts);
    config.span_given = span_given;
    config.span_low_hz = *span_low;
    config.span_high_hz = *span_high;
    config.snr_min_db = *snr_min;
    config.snr_max_db = *snr_max;
    config.min_burst_seconds = *min_burst;
    config.max_burst_seconds = *max_burst;
    config.modes = split_commas(*modes);
    return config;
}

[[nodiscard]] Expected<SourceCapabilities> describe_uri(std::string_view uri)
{
    auto parsed = split_uri(uri);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    if (parsed->scheme == "file") {
        auto config = file_config_of(*parsed);
        if (!config) {
            return std::unexpected(with_context(config.error(), std::string(uri)));
        }
        auto caps = describe_file_source(*config);
        if (!caps) {
            return std::unexpected(with_context(caps.error(), std::string(uri)));
        }
        return caps;
    }

    if (parsed->scheme == "synthetic") {
        auto config = synthetic_config_of(*parsed);
        if (!config) {
            return std::unexpected(with_context(config.error(), std::string(uri)));
        }
        auto caps = describe_synthetic_source(*config);
        if (!caps) {
            return std::unexpected(with_context(caps.error(), std::string(uri)));
        }
        return caps;
    }

    return fail(std::format("'{}' is not a source scheme this build knows. Known schemes are {}.",
                            parsed->scheme, known_schemes()));
}

}  // namespace

Expected<std::vector<SourceDescriptor>> enumerate_sources()
{
    // The synthetic source always, and nothing else.
    //
    // A file is named rather than discovered: there is no directory this
    // could scan that would not be somebody's guess, and a device list that
    // invents entries is worse than a short one. There is no RTL-SDR backend
    // in this build, so no dongle appears here even with one plugged in; the
    // seam for it is the table at the top of this file.
    std::vector<SourceDescriptor> out;
    for (const Backend& backend : kBackends) {
        if (!backend.enumerable) {
            continue;
        }
        SourceDescriptor descriptor;
        descriptor.uri = std::string(backend.default_uri);
        descriptor.display_name = std::string(backend.default_name);
        descriptor.backend = std::string(backend.scheme);
        out.push_back(std::move(descriptor));
    }
    return out;
}

Expected<std::vector<SourceCapabilities>> describe_sources()
{
    auto listed = enumerate_sources();
    if (!listed) {
        return std::unexpected(listed.error());
    }

    std::vector<SourceCapabilities> out;
    out.reserve(listed->size());
    for (const SourceDescriptor& descriptor : *listed) {
        auto caps = describe_uri(descriptor.uri);
        if (!caps) {
            return std::unexpected(caps.error());
        }
        out.push_back(std::move(*caps));
    }
    return out;
}

Expected<std::unique_ptr<Source>> open_source(std::string_view uri)
{
    auto parsed = split_uri(uri);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    if (parsed->scheme == "file") {
        auto config = file_config_of(*parsed);
        if (!config) {
            return std::unexpected(with_context(config.error(), std::string(uri)));
        }
        auto opened = open_file_source(*config);
        if (!opened) {
            return std::unexpected(with_context(opened.error(), std::string(uri)));
        }
        return opened;
    }

    if (parsed->scheme == "synthetic") {
        auto config = synthetic_config_of(*parsed);
        if (!config) {
            return std::unexpected(with_context(config.error(), std::string(uri)));
        }
        auto opened = open_synthetic_source(*config);
        if (!opened) {
            return std::unexpected(with_context(opened.error(), std::string(uri)));
        }
        return opened;
    }

    return fail(std::format("'{}' is not a source scheme this build knows. Known schemes are {}.",
                            parsed->scheme, known_schemes()));
}

}  // namespace revenant::source
