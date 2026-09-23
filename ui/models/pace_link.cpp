// The recording strip's pace control, as EngineLink's half: posting a pace to
// the supervisor and taking the engine's answer back.
//
// Moves values between the Qt thread and the supervisor the way
// calibration_link.cpp does and decides nothing. Which paces the strip offers
// and how it names them is models/recording_status.h, with its cases.

#include <mutex>
#include <string>
#include <utility>

#include <QMetaObject>
#include <QString>

#include "models/engine_link.h"
#include "models/source_pacing.h"

namespace revenant::ui {

void EngineLink::setSourcePace(double pace) {
    {
        const std::lock_guard<std::mutex> lock(pace_mutex_);
        want_pace_ = true;
        want_pace_value_ = pace;
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        source_work_pending_ = true;
    }
    supervisor_wake_.notify_all();
}

void EngineLink::apply_source_pace() {
    if (client_ == nullptr) {
        return;
    }
    double pace = 0.0;
    {
        const std::lock_guard<std::mutex> lock(pace_mutex_);
        if (!std::exchange(want_pace_, false)) {
            return;
        }
        pace = want_pace_value_;
    }

    auto answer = client_->set_source_pace(pace);
    {
        const std::lock_guard<std::mutex> lock(pace_mutex_);
        handover_has_pace_ = true;
        handover_pace_granted_ = answer.has_value();
        handover_pace_ = answer ? *answer : 0.0;
        handover_pace_fault_ =
            answer ? QString{} : QString::fromStdString(answer.error().message);
    }
    QMetaObject::invokeMethod(this, [this] { adopt_source_pace(); }, Qt::QueuedConnection);
}

void EngineLink::adopt_source_pace() {
    bool granted = false;
    double pace = 0.0;
    {
        const std::lock_guard<std::mutex> lock(pace_mutex_);
        if (!std::exchange(handover_has_pace_, false)) {
            return;
        }
        pace_fault_ = handover_pace_fault_;
        granted = handover_pace_granted_;
        pace = handover_pace_;
    }

    // THE ANSWER IS THE PACE IN FORCE, so it is shown now rather than at the
    // next once-a-second poll, which reads the same number off EngineInfo and
    // finds nothing to change. The verdict is recomputed from it because the
    // verdict reads the setting: a factor that was behind at 4x is not at max.
    if (granted) {
        pacing_.paced_by = pace;
        pacing_verdict_ = classify_pacing(pacing_, pacing_verdict_);
        pacing_text_ = QString::fromStdString(pacing_sentence(pacing_verdict_, pacing_));
    }
    emit pacingChanged();
}

}  // namespace revenant::ui
