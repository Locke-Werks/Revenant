#include "tools/webgate/desktop_settings.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <map>
#include <memory>

#include "models/detector_scope.h"
#include "models/detector_settings.h"
#include "models/radio_memory.h"
#include "tools/webgate/protocol.h"

namespace revenant::webgate {

namespace {

// QSettings writes a bool as "true" or "false" text; "1" and "0" are what a
// hand-edited value looks like.
[[nodiscard]] std::optional<bool> parse_bool(std::string_view text)
{
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "true" || lower == "1") {
        return true;
    }
    if (lower == "false" || lower == "0") {
        return false;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<double> parse_double(std::string_view text)
{
    double value = 0.0;
    const char* end = text.data() + text.size();
    const auto [at, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || at != end || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

// The highest bar the engine takes; the phone's slider stops at the same
// figure (web/src/ui/signals.ts CONFIDENCE_MAX).
constexpr double kConfidenceTop = 0.99;

[[nodiscard]] std::optional<std::int64_t> parse_int(std::string_view text)
{
    std::int64_t value = 0;
    const char* end = text.data() + text.size();
    const auto [at, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || at != end) {
        return std::nullopt;
    }
    return value;
}

// engine/lastSource holds the radio as it was OPENED. What the radio was left
// at (rate, centre, gain, direct sampling) is kept per radio beside it, and
// the desktop lays that over the URI when it reopens: EngineLink::
// restored_last_source, which calls restore_radio_uri. This is the same call
// over the same keys, so a gain the owner changed after opening is the gain the
// phone opens with. source/lastRadio says which radio the URI was.
[[nodiscard]] std::string restore_from_radio_memory(const DesktopValueReader& read,
                                                    std::string uri)
{
    const auto last = read("source", "lastRadio");
    if (!last || last->empty()) {
        return uri;
    }
    // The registry is the owner's, but a path built from its text is
    // narrowed to what the desktop writes in the first place.
    const std::string radio = ui::radio_settings_id(*last);
    const std::string group = "source/radios/" + radio;

    ui::RadioMemory memory;
    if (const auto text = read(group, ui::kRadioCentreLeaf)) {
        if (const auto value = parse_int(*text)) {
            memory.centre_hz = ui::read_positive_setting(*value);
        }
    }
    if (const auto text = read(group, ui::kRadioRateLeaf)) {
        if (const auto value = parse_int(*text)) {
            memory.rate = ui::read_positive_setting(*value);
        }
    }
    if (const auto text = read(group, ui::kRadioGainLeaf)) {
        ui::read_gain_setting(*text, memory);
    }
    std::optional<std::string> direct;
    if (const auto text = read(group, "directSampling")) {
        direct = ui::direct_sampling_choice(*text);
    }
    std::optional<std::string_view> direct_view;
    if (direct) {
        direct_view = *direct;
    }
    return ui::restore_radio_uri(uri, memory, direct_view);
}

}  // namespace

DesktopSettings read_desktop_settings(const DesktopValueReader& read)
{
    DesktopSettings s;
    if (!read) {
        return s;
    }
    const auto bool_of = [&](std::string_view group, std::string_view name) {
        const auto text = read(group, name);
        return text ? parse_bool(*text) : std::nullopt;
    };
    const auto double_of = [&](std::string_view group, std::string_view name) {
        const auto text = read(group, name);
        return text ? parse_double(*text) : std::nullopt;
    };
    if (auto uri = read("engine", "lastSource"); uri && !uri->empty()) {
        s.last_source = restore_from_radio_memory(read, std::move(*uri));
    }
    s.impulse_excision = bool_of("radio", "impulseExcision");
    s.comb_cancel = bool_of("radio", "combCancel");
    s.spur_cancel = bool_of("radio", "spurCancel");
    s.transcribe = bool_of("speech", "transcribe");
    s.detection_threshold_db =
        ui::restore_detection_threshold(double_of("detections", "thresholdDb"));
    if (const auto bar = double_of("detections", "confidenceBar")) {
        s.confidence_bar = ui::restore_detection_bar(bar, kConfidenceTop);
    }
    s.trunk_aliases = read("trunk", "aliases");
    s.trunk_allow = read("trunk", "allow");
    s.trunk_deny = read("trunk", "deny");
    s.trunk_priority = read("trunk", "priority");
    if (const auto text = read("trunk", "maxFollowers")) {
        s.trunk_max_followers = parse_int(*text);
    }
    return s;
}

DesktopValueReader registry_desktop_reader()
{
    return [](std::string_view group, std::string_view name) -> std::optional<std::string> {
        // QSettings' "source/radios/<id>" is the registry key source\radios\<id>.
        // Only the groups the desktop writes under are read.
        const std::string_view top = group.substr(0, group.find('/'));
        if (top != "engine" && top != "radio" && top != "speech" && top != "detections" &&
            top != "source" && top != "trunk") {
            return std::nullopt;
        }
        std::string key(group);
        std::replace(key.begin(), key.end(), '/', '\\');
        return registry_key_reader(key)(name);
    };
}

std::optional<ParsedSource> parse_source_uri(std::string_view uri)
{
    const std::size_t scheme = uri.find("://");
    if (scheme == std::string_view::npos || scheme == 0) {
        return std::nullopt;
    }
    ParsedSource out;
    out.uri = std::string(uri);
    out.backend = std::string(uri.substr(0, scheme));
    std::string_view rest = uri.substr(scheme + 3);
    const std::size_t mark = rest.find('?');
    out.device = std::string(rest.substr(0, mark));
    if (mark != std::string_view::npos) {
        std::string_view query = rest.substr(mark + 1);
        while (!query.empty()) {
            const std::size_t amp = query.find('&');
            const std::string_view part = query.substr(0, amp);
            query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
            if (part.empty()) {
                continue;
            }
            const std::size_t eq = part.find('=');
            out.params.emplace_back(std::string(part.substr(0, eq)),
                                    eq == std::string_view::npos
                                        ? std::string{}
                                        : std::string(part.substr(eq + 1)));
        }
    }
    return out;
}

std::string desktop_event(const DesktopSettings& settings)
{
    JsonObject fields;
    if (settings.confidence_bar) {
        fields.add_double("confidenceBar", *settings.confidence_bar);
    }
    if (settings.last_source) {
        if (const auto parsed = parse_source_uri(*settings.last_source)) {
            std::string params = "{";
            for (std::size_t i = 0; i < parsed->params.size(); ++i) {
                if (i != 0) {
                    params += ',';
                }
                params += json_string(parsed->params[i].first) + ':' +
                          json_string(parsed->params[i].second);
            }
            params += '}';
            fields.add_raw("radio", JsonObject()
                                        .add_string("uri", parsed->uri)
                                        .add_string("backend", parsed->backend)
                                        .add_string("device", parsed->device)
                                        .add_raw("params", params)
                                        .str());
        }
    }
    return event("desktop", fields);
}

DesktopSettingsSource::DesktopSettingsSource(DesktopValueReader reader,
                                             std::function<Clock::time_point()> clock,
                                             std::chrono::milliseconds interval)
    : reader_(std::move(reader)), clock_(std::move(clock)), interval_(interval)
{
}

DesktopSettings DesktopSettingsSource::current()
{
    const auto now = clock_ ? clock_() : Clock::now();
    const std::lock_guard lock(mutex_);
    if (!read_at_ || now - *read_at_ >= interval_) {
        settings_ = read_desktop_settings(reader_);
        read_at_ = now;
    }
    return settings_;
}

}  // namespace revenant::webgate
