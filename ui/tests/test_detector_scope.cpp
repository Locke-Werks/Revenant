// Which radio the detector's settings are kept under and which stored value
// wins, models/detector_scope.h; and the band plan's region checkboxes,
// models/region_choice.h.
//
// The owner's report of 2026-10-07: the threshold went back to 6 dB on every
// start, which is too low for their radio. These hold the keying that keeps a
// value per radio, the fallback to every radio's value, and the rule that a
// value nobody set leaves the engine at its own default.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include "models/detector_scope.h"
#include "models/region_choice.h"

using revenant::ui::DetectorField;
using revenant::ui::detector_field_from_name;
using revenant::ui::detector_field_name;
using revenant::ui::detector_key;
using revenant::ui::kDetectorFields;
using revenant::ui::kEngineDefaultThresholdDb;
using revenant::ui::kRegionCA;
using revenant::ui::kRegionUS;
using revenant::ui::kRegionUSCA;
using revenant::ui::radio_settings_id;
using revenant::ui::resolve_detector_setting;
using revenant::ui::restore_region;
using revenant::ui::scope_name;
using revenant::ui::SettingScope;
using revenant::ui::threshold_on_radio_change;
using revenant::ui::toggle_region;
using revenant::ui::write_scope;

TEST_CASE("the shared keys are the ones earlier builds wrote", "[detector_scope]")
{
    // A value saved before keying by radio has to be every radio's fallback
    // now, not an orphan. detector_link.cpp checks the same against
    // models/settings.h at compile time.
    CHECK(detector_key(DetectorField::Threshold, "") == "detections/thresholdDb");
    CHECK(detector_key(DetectorField::ConfidenceBar, "") == "detections/confidenceBar");
    CHECK(detector_key(DetectorField::MarginBar, "") == "detections/marginBar");
}

TEST_CASE("a radio's keys sit under its calibration key", "[detector_scope]")
{
    const std::string id = radio_settings_id("rtlsdr:00000001");
    CHECK(id == "rtlsdr:00000001");
    CHECK(detector_key(DetectorField::Threshold, id) ==
          "detections/radios/rtlsdr:00000001/thresholdDb");

    // Two radios never share a key, and neither shares the fallback's.
    const std::string other = radio_settings_id("airspy:A0B1C2");
    CHECK(detector_key(DetectorField::Threshold, id) !=
          detector_key(DetectorField::Threshold, other));
    CHECK(detector_key(DetectorField::Threshold, other) !=
          detector_key(DetectorField::Threshold, ""));
}

TEST_CASE("a serial cannot split the key into extra groups", "[detector_scope]")
{
    // QSettings reads '/' and '\' as group separators.
    CHECK(radio_settings_id("soapy:driver=x/serial\\1") == "soapy:driver_x_serial_1");
    CHECK(radio_settings_id("hackrf:0000 1") == "hackrf:0000_1");
    CHECK(radio_settings_id("").empty());

    const std::string key = detector_key(DetectorField::MarginBar, radio_settings_id("a/b"));
    CHECK(key == "detections/radios/a_b/marginBar");
}

TEST_CASE("the radio's own value wins, then every radio's, then the default",
          "[detector_scope]")
{
    const auto own = resolve_detector_setting(14.0, 10.0);
    CHECK(own.value == 14.0);
    CHECK(own.scope == SettingScope::ThisRadio);

    const auto shared = resolve_detector_setting(std::nullopt, 10.0);
    CHECK(shared.value == 10.0);
    CHECK(shared.scope == SettingScope::AllRadios);

    const auto none = resolve_detector_setting(std::nullopt, std::nullopt);
    CHECK_FALSE(none.value.has_value());
    CHECK(none.scope == SettingScope::EngineDefault);
}

TEST_CASE("a value is written under the radio when it has an identity", "[detector_scope]")
{
    CHECK(write_scope("rtlsdr:00000001") == SettingScope::ThisRadio);
    // A recording or a synthetic scene has no serial and so no key.
    CHECK(write_scope("") == SettingScope::AllRadios);
}

TEST_CASE("a threshold nobody set leaves the engine's default alone", "[detector_scope]")
{
    const auto unset = resolve_detector_setting(std::nullopt, std::nullopt);

    // Nothing sent before and nothing stored: nothing is sent, so a first run
    // imposes no value on an engine another client may have set.
    CHECK_FALSE(threshold_on_radio_change(unset, false).has_value());

    // Something was sent for the previous radio: the engine keeps that across
    // a source change, so the new radio is put back at the default rather
    // than inheriting it.
    CHECK(threshold_on_radio_change(unset, true) == kEngineDefaultThresholdDb);
    CHECK(kEngineDefaultThresholdDb == 6.0);

    // A stored value is always sent.
    const auto stored = resolve_detector_setting(12.5, std::nullopt);
    CHECK(threshold_on_radio_change(stored, false) == 12.5);
    CHECK(threshold_on_radio_change(stored, true) == 12.5);
}

TEST_CASE("the settings window's field names round trip", "[detector_scope]")
{
    for (const DetectorField field : kDetectorFields) {
        CHECK(detector_field_from_name(detector_field_name(field)) == field);
    }
    CHECK(detector_field_from_name("threshold") == DetectorField::Threshold);
    CHECK_FALSE(detector_field_from_name("thresholdDb").has_value());
    CHECK_FALSE(detector_field_from_name("").has_value());
}

TEST_CASE("the scope words are what the settings window compares against",
          "[detector_scope]")
{
    // ui/qml/SettingsPanel.qml compares these strings literally.
    CHECK(scope_name(SettingScope::ThisRadio) == "this radio");
    CHECK(scope_name(SettingScope::AllRadios) == "all radios");
    CHECK(scope_name(SettingScope::EngineDefault) == "default");
}

TEST_CASE("the region checkboxes never leave the band plan empty", "[region_choice]")
{
    CHECK(toggle_region(kRegionUSCA, kRegionCA, false) == kRegionUS);
    CHECK(toggle_region(kRegionUS, kRegionCA, true) == kRegionUSCA);

    // The last region stays on.
    CHECK(toggle_region(kRegionUS, kRegionUS, false) == kRegionUS);
    CHECK(toggle_region(kRegionCA, kRegionCA, false) == kRegionCA);

    CHECK(restore_region(0U) == kRegionUSCA);
    CHECK(restore_region(kRegionCA) == kRegionCA);
    CHECK(restore_region(kRegionUS | 0x80U) == kRegionUS);
    CHECK(restore_region(0x80U) == kRegionUSCA);
}
