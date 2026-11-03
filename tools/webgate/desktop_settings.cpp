#include "tools/webgate/desktop_settings.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <map>
#include <memory>

#include "models/detector_settings.h"
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
        s.last_source = std::move(*uri);
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
    return s;
}

DesktopValueReader registry_desktop_reader()
{
    auto readers = std::make_shared<std::map<std::string, DisplayValueReader, std::less<>>>();
    for (const char* group : {"engine", "radio", "speech", "detections"}) {
        readers->emplace(group, registry_key_reader(group));
    }
    return [readers](std::string_view group, std::string_view name) -> std::optional<std::string> {
        const auto it = readers->find(group);
        return it == readers->end() ? std::nullopt : it->second(name);
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
