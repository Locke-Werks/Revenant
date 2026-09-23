// The calibration section's half of EngineLink: posting a change, reading the
// engine's calibration back, and the known-carrier measurement.
//
// Every rule is in models/calibration.h with its cases in ui/tests. This file
// moves values between the Qt thread and the supervisor the way
// source_link.cpp does for the gain, and adds nothing of its own.

#include <mutex>
#include <string>
#include <utility>

#include <QMetaObject>
#include <QString>

#include "models/calibration.h"
#include "models/engine_link.h"
#include "models/frequency_entry.h"

namespace revenant::ui {

QString EngineLink::calibrationPpm() const {
    return QString::fromStdString(format_ppm(calibration_.settings.correction_ppb));
}

QString EngineLink::calibrationMeasured() const {
    return QString::fromStdString(describe_front_end(calibration_));
}

QString EngineLink::calibrationPpmProblem(const QString& text) const {
    if (text.trimmed().isEmpty()) {
        return {};
    }
    const PpmEntry entry = parse_ppm(text.toStdString());
    return entry.ppb ? QString{} : QString::fromStdString(entry.reason);
}

void EngineLink::post_calibration(const rpc::CalibrationSettings& settings) {
    {
        const std::lock_guard<std::mutex> lock(calibration_mutex_);
        want_calibration_ = true;
        want_calibration_settings_ = settings;
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        source_work_pending_ = true;
    }
    supervisor_wake_.notify_all();
}

void EngineLink::setCalibrationPpm(const QString& text) {
    const PpmEntry entry = parse_ppm(text.toStdString());
    if (!entry.ppb) {
        calibration_fault_ = QString::fromStdString(entry.reason);
        emit calibrationChanged();
        return;
    }
    rpc::CalibrationSettings settings = calibration_.settings;
    settings.correction_ppb = *entry.ppb;
    post_calibration(settings);
}

void EngineLink::setCalibrationDcRemoval(bool on) {
    rpc::CalibrationSettings settings = calibration_.settings;
    settings.dc_removal = on;
    post_calibration(settings);
}

void EngineLink::setCalibrationIqCorrection(bool on) {
    rpc::CalibrationSettings settings = calibration_.settings;
    settings.iq_correction = on;
    post_calibration(settings);
}

QString EngineLink::measureCarrierText(const QString& known) const {
    const auto parsed = parse_frequency(known.toStdString());
    if (!parsed) {
        return known.trimmed().isEmpty()
                   ? QString{}
                   : QStringLiteral("'%1' is not a frequency").arg(known.trimmed());
    }
    const CarrierMeasurement measured =
        measure_against_carrier(parsed->hertz, detections(), info_.source_center,
                                calibration_.settings.correction_ppb);
    return QString::fromStdString(measured.text);
}

void EngineLink::applyMeasuredCarrier(const QString& known) {
    const auto parsed = parse_frequency(known.toStdString());
    if (!parsed) {
        return;
    }
    const CarrierMeasurement measured =
        measure_against_carrier(parsed->hertz, detections(), info_.source_center,
                                calibration_.settings.correction_ppb);
    if (!measured.found) {
        calibration_fault_ = QString::fromStdString(measured.text);
        emit calibrationChanged();
        return;
    }
    rpc::CalibrationSettings settings = calibration_.settings;
    settings.correction_ppb = measured.ppb;
    post_calibration(settings);
}

void EngineLink::apply_calibration() {
    if (client_ == nullptr) {
        return;
    }
    rpc::CalibrationSettings settings;
    {
        const std::lock_guard<std::mutex> lock(calibration_mutex_);
        if (!std::exchange(want_calibration_, false)) {
            return;
        }
        settings = want_calibration_settings_;
    }

    auto answer = client_->set_calibration(settings);
    {
        const std::lock_guard<std::mutex> lock(calibration_mutex_);
        handover_has_calibration_fault_ = true;
        if (answer) {
            handover_calibration_fault_.clear();
            handover_has_calibration_ = true;
            handover_calibration_ = *answer;
        } else {
            handover_calibration_fault_ = QString::fromStdString(answer.error().message);
        }
    }
    QMetaObject::invokeMethod(this, [this] { adopt_calibration(); }, Qt::QueuedConnection);
}

void EngineLink::poll_calibration() {
    if (client_ == nullptr) {
        return;
    }
    auto answer = client_->calibration();
    if (!answer) {
        // An engine written before calibration refuses the call, and a lost
        // one is the liveness probe's to report. Neither is this section's
        // fault to show.
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(calibration_mutex_);
        handover_has_calibration_ = true;
        handover_calibration_ = *answer;
    }
    QMetaObject::invokeMethod(this, [this] { adopt_calibration(); }, Qt::QueuedConnection);
}

void EngineLink::adopt_calibration() {
    bool changed = false;
    {
        const std::lock_guard<std::mutex> lock(calibration_mutex_);
        if (handover_has_calibration_) {
            handover_has_calibration_ = false;
            calibration_ = handover_calibration_;
            changed = true;
        }
        if (handover_has_calibration_fault_) {
            handover_has_calibration_fault_ = false;
            calibration_fault_ = handover_calibration_fault_;
            changed = true;
        }
    }
    if (changed) {
        emit calibrationChanged();
    }
}

}  // namespace revenant::ui
