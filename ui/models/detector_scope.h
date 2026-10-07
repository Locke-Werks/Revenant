// Which radio the detector's settings are kept under, and which stored value
// wins when a radio is opened.
//
// WHY PER RADIO. On 2026-10-07 the owner reported the threshold "keeps going
// back to 6.0 on every start", and that 6 dB is too low for their radio. A
// threshold is a statement about one front end's noise: a dongle with a hot
// LNA and a quiet HF receiver want numbers ten dB apart, so one value for the
// whole machine is wrong for at least one of them every time the operator
// swaps. Each setting is therefore kept twice over:
//
//   detections/radios/<radio>/<leaf>   what was set while that radio was open
//   detections/<leaf>                  the value for every radio, the fallback
//
// and nothing at all is the engine's own default, which is what "a value the
// operator never set" has to keep meaning.
//
// THE RADIO IS THE ENGINE'S CALIBRATION KEY, "rtlsdr:00000001": driver and
// serial, which the engine already keeps a calibration under and publishes on
// the wire (rpc::Calibration::key). It survives a replug into another port and
// a change of centre, which a source URI does not. A source with no serial, a
// recording or a synthetic scene, has an empty key and uses the fallback.
//
// The global keys are the ones models/settings.h has always written, so a
// value an earlier build saved is the fallback for every radio on the first
// run of this one rather than being forgotten.
//
// Qt-free on the rule the rest of ui/models follows, so ui/tests can hold it.

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace revenant::ui {

enum class DetectorField { Threshold, ConfidenceBar, MarginBar };

inline constexpr std::array kDetectorFields{DetectorField::Threshold,
                                            DetectorField::ConfidenceBar,
                                            DetectorField::MarginBar};

// Where the value in use came from.
enum class SettingScope { EngineDefault, AllRadios, ThisRadio };

// The group and leaf names. ui/models/detector_link.cpp static_asserts that
// group + "/" + leaf is exactly the key models/settings.h names, because that
// header is Qt and cannot be included from a test.
inline constexpr std::string_view kDetectorGroup = "detections";
inline constexpr std::string_view kDetectorRadiosGroup = "radios";

[[nodiscard]] constexpr std::string_view detector_leaf(DetectorField field)
{
    switch (field) {
        case DetectorField::Threshold:
            return "thresholdDb";
        case DetectorField::ConfidenceBar:
            return "confidenceBar";
        case DetectorField::MarginBar:
            return "marginBar";
    }
    return "";
}

// The name QML hands the invokables, and the one the settings window shows.
[[nodiscard]] constexpr std::string_view detector_field_name(DetectorField field)
{
    switch (field) {
        case DetectorField::Threshold:
            return "threshold";
        case DetectorField::ConfidenceBar:
            return "held";
        case DetectorField::MarginBar:
            return "margin";
    }
    return "";
}

[[nodiscard]] constexpr std::optional<DetectorField> detector_field_from_name(
    std::string_view name)
{
    for (const DetectorField field : kDetectorFields) {
        if (detector_field_name(field) == name) {
            return field;
        }
    }
    return std::nullopt;
}

// The calibration key made safe to be one path segment of a settings key.
//
// QSettings treats '/' and '\' as group separators, and on Windows the
// registry backend refuses an empty segment, so a serial a driver reports
// with a slash in it would land the value two groups deep and read back from
// nowhere. Anything outside a conservative set becomes '_'. Two keys that
// differ only in such characters would share a slot; serials do not do that
// in practice, and the alternative, an escape scheme, makes the registry
// unreadable to the operator who goes looking.
[[nodiscard]] inline std::string radio_settings_id(std::string_view calibration_key)
{
    std::string out;
    out.reserve(calibration_key.size());
    for (const char c : calibration_key) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                          c == ':';
        out.push_back(keep ? c : '_');
    }
    return out;
}

// The key a field is stored under for one radio, or for every radio when the
// id is empty.
[[nodiscard]] inline std::string detector_key(DetectorField field, std::string_view radio_id)
{
    std::string key(kDetectorGroup);
    key += '/';
    if (!radio_id.empty()) {
        key += kDetectorRadiosGroup;
        key += '/';
        key += radio_id;
        key += '/';
    }
    key += detector_leaf(field);
    return key;
}

struct ResolvedSetting {
    std::optional<double> value;
    SettingScope scope = SettingScope::EngineDefault;
};

// The radio's own value, else every radio's, else none. Each input has already
// been through its field's restore (restore_detection_threshold or
// restore_detection_bar's refusal of NaN), so an unreadable value arrives as
// nothing and falls through to the next level rather than shadowing it.
[[nodiscard]] inline ResolvedSetting resolve_detector_setting(std::optional<double> radio,
                                                              std::optional<double> all)
{
    if (radio.has_value()) {
        return {radio, SettingScope::ThisRadio};
    }
    if (all.has_value()) {
        return {all, SettingScope::AllRadios};
    }
    return {};
}

// Where a value the operator sets now is written: under the open radio when
// there is one with an identity, under every radio otherwise.
[[nodiscard]] inline SettingScope write_scope(std::string_view radio_id)
{
    return radio_id.empty() ? SettingScope::AllRadios : SettingScope::ThisRadio;
}

[[nodiscard]] constexpr std::string_view scope_name(SettingScope scope)
{
    switch (scope) {
        case SettingScope::EngineDefault:
            return "default";
        case SettingScope::AllRadios:
            return "all radios";
        case SettingScope::ThisRadio:
            return "this radio";
    }
    return "";
}

// core/detect/detector.h's DetectorConfig::detection_threshold_db, copied for
// the same reason kDetectorConfidenceRise in models/engine_link.h is: the
// detector's configuration is not on the wire and that header is not
// reachable from here. Used for one thing, sending the engine back to its
// default when the operator resets a threshold the engine is still holding,
// since the engine has no "forget the client's value" call.
inline constexpr double kEngineDefaultThresholdDb = 6.0;

// The threshold to send when the radio changes, or none.
//
// A stored value is sent. With none, the engine default is sent only when this
// window had told the engine something for the radio before: the engine keeps
// a threshold across a source change, so leaving it alone would carry the old
// radio's number onto a radio that never asked for it. With nothing sent
// before, nothing is sent now, so a first run does not impose a value on an
// engine another client has set.
[[nodiscard]] inline std::optional<double> threshold_on_radio_change(
    const ResolvedSetting& resolved, bool sent_before)
{
    if (resolved.value.has_value()) {
        return resolved.value;
    }
    if (sent_before) {
        return kEngineDefaultThresholdDb;
    }
    return std::nullopt;
}

}  // namespace revenant::ui
