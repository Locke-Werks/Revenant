// EngineLink's detector settings by radio: which stored value is in force for
// the open radio, where a value the operator sets is written, and the settings
// window's reset and share. The keying is models/detector_scope.h.

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <QSettings>
#include <QString>

#include "models/detector_scope.h"
#include "models/detector_settings.h"
#include "models/engine_link.h"
#include "models/settings.h"

namespace revenant::ui {

namespace {

// The keys models/settings.h has always named are the fallback level here, so
// a value an earlier build saved is not orphaned. Checked at compile time
// because the test target cannot see that header.
constexpr bool same_key(QLatin1StringView qt, DetectorField field)
{
    const std::string_view group = kDetectorGroup;
    const std::string_view leaf = detector_leaf(field);
    const std::string_view key(qt.data(), static_cast<std::size_t>(qt.size()));
    return key.size() == group.size() + 1 + leaf.size() && key.substr(0, group.size()) == group &&
           key[group.size()] == '/' && key.substr(group.size() + 1) == leaf;
}
static_assert(same_key(settings::kDetectionThresholdDb, DetectorField::Threshold));
static_assert(same_key(settings::kConfidenceBar, DetectorField::ConfidenceBar));
static_assert(same_key(settings::kMarginBar, DetectorField::MarginBar));

[[nodiscard]] QString qkey(DetectorField field, std::string_view radio_id)
{
    return QString::fromStdString(detector_key(field, radio_id));
}

[[nodiscard]] std::optional<double> read_number(const QSettings& store, const QString& key)
{
    if (!store.contains(key)) {
        return std::nullopt;
    }
    bool ok = false;
    const double value = store.value(key).toDouble(&ok);
    return ok ? std::optional<double>(value) : std::nullopt;
}

// One level's value as the field's own restore would take it, or nothing, so
// an unreadable radio value falls through to the shared one instead of hiding
// it. A bar is clamped, as restore_detection_bar does; a threshold out of the
// engine's range is refused, as restore_detection_threshold does.
[[nodiscard]] std::optional<double> restore_level(DetectorField field,
                                                  std::optional<double> stored, double top)
{
    if (!stored.has_value()) {
        return std::nullopt;
    }
    if (field == DetectorField::Threshold) {
        return restore_detection_threshold(stored);
    }
    if (std::isnan(*stored)) {
        return std::nullopt;
    }
    return restore_detection_bar(stored, top);
}

[[nodiscard]] std::size_t slot(DetectorField field)
{
    return static_cast<std::size_t>(field);
}

}  // namespace

QString EngineLink::detector_scope_text(DetectorField field) const
{
    return QString::fromUtf8(scope_name(detector_scopes_[slot(field)]));
}

void EngineLink::queue_threshold(double threshold_db)
{
    requested_threshold_db_.store(threshold_db, std::memory_order_release);
    threshold_pending_.store(true, std::memory_order_release);
}

void EngineLink::load_detector_settings(bool send_now)
{
    if (!remember_detector_) {
        return;
    }
    const QSettings store;
    std::array<ResolvedSetting, 3> resolved{};
    for (const DetectorField field : kDetectorFields) {
        const std::optional<double> radio =
            detector_radio_id_.empty()
                ? std::nullopt
                : restore_level(field, read_number(store, qkey(field, detector_radio_id_)),
                                kMaxConfidenceBar);
        const std::optional<double> all =
            restore_level(field, read_number(store, qkey(field, {})), kMaxConfidenceBar);
        resolved[slot(field)] = resolve_detector_setting(radio, all);
        detector_scopes_[slot(field)] = resolved[slot(field)].scope;
    }

    // The bars are this window's own filter, so they take effect at once and
    // an unset one is the floor, which passes everything.
    const auto& confidence = resolved[slot(DetectorField::ConfidenceBar)];
    const auto& margin = resolved[slot(DetectorField::MarginBar)];
    detector_memory_.confidence_bar = confidence.value.value_or(0.0);
    detector_memory_.margin_bar = margin.value.value_or(0.0);
    confidence_bar_.store(detector_memory_.confidence_bar, std::memory_order_release);
    margin_bar_.store(detector_memory_.margin_bar, std::memory_order_release);

    // The threshold is the engine's. What is remembered is what a later
    // connection re-sends; what is queued now is for the engine that is
    // already up, which keeps the previous radio's value across a source
    // change unless told otherwise.
    const auto& threshold = resolved[slot(DetectorField::Threshold)];
    const bool sent_before = threshold_remembered_.load(std::memory_order_acquire);
    detector_memory_.threshold_db = threshold.value;
    if (threshold.value.has_value()) {
        remembered_threshold_db_.store(*threshold.value, std::memory_order_release);
        threshold_remembered_.store(true, std::memory_order_release);
    } else {
        threshold_remembered_.store(false, std::memory_order_release);
    }
    if (send_now) {
        if (const auto send = threshold_on_radio_change(threshold, sent_before)) {
            queue_threshold(*send);
        }
    }

    emit detectorScopeChanged();
    emit detectionsChanged();
}

void EngineLink::note_detector_radio(const std::string& calibration_key)
{
    if (!remember_detector_ || calibration_key == detector_radio_key_) {
        return;
    }
    detector_radio_key_ = calibration_key;
    detector_radio_id_ = radio_settings_id(calibration_key);
    load_detector_settings(true);
}

void EngineLink::store_detector_value(DetectorField field, double value)
{
    if (!remember_detector_) {
        return;
    }
    const SettingScope scope = write_scope(detector_radio_id_);
    QSettings().setValue(
        qkey(field, scope == SettingScope::ThisRadio ? detector_radio_id_ : std::string{}), value);
    if (detector_scopes_[slot(field)] != scope) {
        detector_scopes_[slot(field)] = scope;
        emit detectorScopeChanged();
    }
}

void EngineLink::resetDetectorSetting(const QString& field_name)
{
    const auto field = detector_field_from_name(field_name.toStdString());
    if (!field || !remember_detector_) {
        return;
    }

    // One level at a time, so a radio that had its own value goes back to the
    // shared one first rather than wiping a value every other radio is using.
    QSettings store;
    const SettingScope scope = detector_scopes_[slot(*field)];
    if (scope == SettingScope::ThisRadio) {
        store.remove(qkey(*field, detector_radio_id_));
    } else if (scope == SettingScope::AllRadios) {
        store.remove(qkey(*field, {}));
    }
    store.sync();
    load_detector_settings(true);
}

void EngineLink::shareDetectorSetting(const QString& field_name)
{
    const auto field = detector_field_from_name(field_name.toStdString());
    if (!field || !remember_detector_) {
        return;
    }

    std::optional<double> value;
    switch (*field) {
        case DetectorField::Threshold:
            value = detector_memory_.threshold_db;
            break;
        case DetectorField::ConfidenceBar:
            value = detector_memory_.confidence_bar;
            break;
        case DetectorField::MarginBar:
            value = detector_memory_.margin_bar;
            break;
    }
    // Nothing to share for a threshold that is still the engine's default.
    if (!value.has_value()) {
        return;
    }

    QSettings store;
    store.setValue(qkey(*field, {}), *value);
    if (!detector_radio_id_.empty()) {
        store.remove(qkey(*field, detector_radio_id_));
    }
    store.sync();
    load_detector_settings(true);
}

}  // namespace revenant::ui
