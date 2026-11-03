// tools/webgate/desktop_settings.h: the phone following what the desktop has
// saved. Each case names the wrong implementation it rejects.

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "tools/webgate/desktop_settings.h"

namespace {

using revenant::webgate::DesktopSettings;
using revenant::webgate::DesktopSettingsSource;
using revenant::webgate::DesktopValueReader;
using revenant::webgate::desktop_event;
using revenant::webgate::parse_source_uri;
using revenant::webgate::read_desktop_settings;

using Values = std::map<std::string, std::string>;

// Keys are "group/name", the spelling of ui/models/settings.h.
[[nodiscard]] DesktopValueReader reader_of(Values values)
{
    return [values = std::move(values)](std::string_view group,
                                        std::string_view name) -> std::optional<std::string> {
        const auto at = values.find(std::string(group) + "/" + std::string(name));
        if (at == values.end()) {
            return std::nullopt;
        }
        return at->second;
    };
}

[[nodiscard]] Values owners_registry()
{
    return {
        {"engine/lastSource",
         "rtlsdr://0?agc=1&bw=3000000&direct=auto&freq=903536000&rate=3200000&gain=auto"},
        {"radio/impulseExcision", "true"},
        {"radio/combCancel", "false"},
        {"radio/spurCancel", "true"},
        {"detections/thresholdDb", "9"},
        {"detections/confidenceBar", "0.91"},
        {"speech/transcribe", "true"},
    };
}

}  // namespace

TEST_CASE("every saved key is read from its group", "[webgate][desktop]")
{
    const DesktopSettings s = read_desktop_settings(reader_of(owners_registry()));
    REQUIRE(s.last_source.has_value());
    CHECK(s.last_source->rfind("rtlsdr://0?agc=1", 0) == 0);
    CHECK(s.impulse_excision == true);
    // False is a value, not an absence: the desktop sends it.
    CHECK(s.comb_cancel == false);
    CHECK(s.spur_cancel == true);
    CHECK(s.transcribe == true);
    CHECK(s.detection_threshold_db == 9.0);
    CHECK(s.confidence_bar == 0.91);
}

TEST_CASE("a key the desktop never wrote is absent", "[webgate][desktop]")
{
    // Sending a default for these would impose a number on an engine another
    // client has already set, which the desktop's own rule forbids.
    const DesktopSettings s = read_desktop_settings(reader_of({}));
    CHECK_FALSE(s.last_source.has_value());
    CHECK_FALSE(s.impulse_excision.has_value());
    CHECK_FALSE(s.comb_cancel.has_value());
    CHECK_FALSE(s.spur_cancel.has_value());
    CHECK_FALSE(s.transcribe.has_value());
    CHECK_FALSE(s.detection_threshold_db.has_value());
    CHECK_FALSE(s.confidence_bar.has_value());
    CHECK(read_desktop_settings({}) == DesktopSettings{});
}

TEST_CASE("values are read by the desktop's restore rules", "[webgate][desktop]")
{
    Values v{{"detections/thresholdDb", "500"},
             {"detections/confidenceBar", "1.5"},
             {"radio/combCancel", "maybe"},
             {"engine/lastSource", ""}};
    const DesktopSettings s = read_desktop_settings(reader_of(v));
    // Refused and not clamped: 500 dB is a detector that finds nothing.
    CHECK_FALSE(s.detection_threshold_db.has_value());
    // Clamped to what the engine and the phone's slider take.
    CHECK(s.confidence_bar == 0.99);
    CHECK_FALSE(s.comb_cancel.has_value());
    CHECK_FALSE(s.last_source.has_value());

    v = {{"detections/confidenceBar", "nan"}, {"detections/thresholdDb", "-61"}};
    const DesktopSettings bad = read_desktop_settings(reader_of(v));
    CHECK_FALSE(bad.confidence_bar.has_value());
    CHECK_FALSE(bad.detection_threshold_db.has_value());
}

TEST_CASE("a source URI splits into backend, device and parameters", "[webgate][desktop]")
{
    const auto p = parse_source_uri(
        "rtlsdr://0?agc=1&bw=3000000&direct=auto&freq=903536000&rate=3200000&gain=auto");
    REQUIRE(p.has_value());
    CHECK(p->backend == "rtlsdr");
    CHECK(p->device == "0");
    REQUIRE(p->params.size() == 6);
    // In the order written, which the phone keeps when it composes.
    CHECK(p->params[0] == std::pair<std::string, std::string>("agc", "1"));
    CHECK(p->params[3] == std::pair<std::string, std::string>("freq", "903536000"));
    CHECK(p->params[5] == std::pair<std::string, std::string>("gain", "auto"));

    const auto bare = parse_source_uri("rtlsdr://00000001");
    REQUIRE(bare.has_value());
    CHECK(bare->device == "00000001");
    CHECK(bare->params.empty());

    CHECK_FALSE(parse_source_uri("").has_value());
    CHECK_FALSE(parse_source_uri("nonsense").has_value());
}

TEST_CASE("the desktop event carries the radio and the bar", "[webgate][desktop]")
{
    const std::string text = desktop_event(read_desktop_settings(reader_of(owners_registry())));
    CHECK(text.rfind(R"({"event":"desktop",)", 0) == 0);
    CHECK(text.find(R"("confidenceBar":0.91)") != std::string::npos);
    CHECK(text.find(R"("backend":"rtlsdr")") != std::string::npos);
    CHECK(text.find(R"("device":"0")") != std::string::npos);
    CHECK(text.find(R"("freq":"903536000")") != std::string::npos);
    CHECK(text.find(R"("gain":"auto")") != std::string::npos);

    // Nothing saved says nothing, so a phone keeps its own defaults.
    CHECK(desktop_event({}) == R"({"event":"desktop"})");
}

TEST_CASE("the registry is re-read on an interval", "[webgate][desktop]")
{
    using Clock = DesktopSettingsSource::Clock;
    Values values{{"detections/confidenceBar", "0.5"}};
    int reads = 0;
    Clock::time_point now = Clock::now();
    DesktopSettingsSource source(
        [&](std::string_view group, std::string_view name) -> std::optional<std::string> {
            ++reads;
            const auto at = values.find(std::string(group) + "/" + std::string(name));
            return at == values.end() ? std::nullopt : std::optional(at->second);
        },
        [&] { return now; }, std::chrono::seconds(2));

    CHECK(source.current().confidence_bar == 0.5);
    const int after_first = reads;
    values["detections/confidenceBar"] = "0.7";
    now += std::chrono::milliseconds(1500);
    CHECK(source.current().confidence_bar == 0.5);
    CHECK(reads == after_first);
    now += std::chrono::milliseconds(600);
    CHECK(source.current().confidence_bar == 0.7);
}
