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

TEST_CASE("the radio's remembered state is laid over the opened URI", "[webgate][desktop]")
{
    // The cases of ui/tests/test_radio_memory.cpp "the reopen URI carries what
    // the radio was left at", through the registry keys the desktop writes.
    Values v{
        {"engine/lastSource", "rtlsdr://0?rate=2400000&freq=98100000&gain=20.7"},
        {"source/lastRadio", "rtlsdr:00000001"},
        {"source/radios/rtlsdr:00000001/centreHz", "7100000"},
        {"source/radios/rtlsdr:00000001/sampleRate", "1024000"},
        {"source/radios/rtlsdr:00000001/gain", "auto"},
        {"source/radios/rtlsdr:00000001/directSampling", "q"},
    };
    CHECK(read_desktop_settings(reader_of(v)).last_source ==
          "rtlsdr://0?direct=q&freq=7100000&rate=1024000&gain=auto");

    // A gain the owner changed after opening is the one the phone opens with.
    v = {
        {"engine/lastSource", "rtlsdr://0?rate=2400000&freq=98100000&gain=auto"},
        {"source/lastRadio", "rtlsdr:00000001"},
        {"source/radios/rtlsdr:00000001/gain", "33.8"},
    };
    CHECK(read_desktop_settings(reader_of(v)).last_source ==
          "rtlsdr://0?rate=2400000&freq=98100000&gain=33.8");

    // Direct "off" takes direct= away; junk and zero read as nothing kept.
    v = {
        {"engine/lastSource", "rtlsdr://0?freq=7100000&direct=q"},
        {"source/lastRadio", "rtlsdr:00000001"},
        {"source/radios/rtlsdr:00000001/directSampling", "off"},
        {"source/radios/rtlsdr:00000001/sampleRate", "0"},
        {"source/radios/rtlsdr:00000001/gain", "loud"},
    };
    CHECK(read_desktop_settings(reader_of(v)).last_source == "rtlsdr://0?freq=7100000");

    // No lastRadio: the desktop cannot say whose memory it is either, and the
    // URI stays as opened.
    v = {
        {"engine/lastSource", "rtlsdr://0?rate=2400000&freq=98100000"},
        {"source/radios/rtlsdr:00000001/sampleRate", "1024000"},
    };
    CHECK(read_desktop_settings(reader_of(v)).last_source ==
          "rtlsdr://0?rate=2400000&freq=98100000");

    // A backend with no tuner never gains a freq=.
    v = {
        {"engine/lastSource", "synthetic://two-tones?rate=48000"},
        {"source/lastRadio", "synthetic:x"},
        {"source/radios/synthetic:x/centreHz", "7100000"},
    };
    CHECK(read_desktop_settings(reader_of(v)).last_source == "synthetic://two-tones?rate=48000");

    // The radio id is narrowed the way the desktop writes it, so a path
    // separator in the registry text cannot reach another key.
    v = {
        {"engine/lastSource", "rtlsdr://0?freq=1"},
        {"source/lastRadio", "a/../b"},
        {"source/radios/a_.._b/sampleRate", "1024000"},
        {"source/radios/a/../b/sampleRate", "9"},
    };
    CHECK(read_desktop_settings(reader_of(v)).last_source == "rtlsdr://0?freq=1&rate=1024000");
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

// Against this machine's real registry, so hidden: run it by name. QSettings
// writes the per-radio centre and rate as REG_QWORD (checked with reg query on
// 2026-11-03: centreHz REG_QWORD 0x1b7d5a80, sampleRate REG_QWORD 0x30d400),
// and the reader has to hand a number back as decimal text for parse_int.
TEST_CASE("numeric registry values read back as decimal text", "[.][registry][desktop]")
{
    const auto read = revenant::webgate::registry_desktop_reader();
    const auto radio = read("source", "lastRadio");
    if (!radio) {
        SKIP("the desktop has never opened a radio on this account");
    }
    const auto rate = read("source/radios/" + *radio, "sampleRate");
    if (!rate) {
        SKIP("no sample rate remembered for the last radio");
    }
    CHECK(rate->find_first_not_of("0123456789") == std::string::npos);
    CHECK(std::stoll(*rate) > 0);
    const auto settings = read_desktop_settings(read);
    CHECK(settings.last_source.has_value());
}
