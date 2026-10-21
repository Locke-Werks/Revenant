// The time-shift scrub bar, as EngineLink's half: posting the bounds and the
// scrub position to the supervisor, which talks to the engine, and taking the
// buffer's extent back.
//
// The arithmetic of the bar is models/timeshift_scrub.h, with its cases. What
// is decided here is the order of a transition, because the pane's receiver
// has to be on the stream the span is showing:
//
//   off live   the replay starts first, then the pane's receiver is rebuilt
//              on it, then the span switches to the replay's frames.
//   to live    the pane's receiver is rebuilt on the live stream first, then
//              the replay is stopped, which would otherwise take that
//              receiver with it, then the span switches back.
//
// A move while already off live is a seek: the replay moves and the receiver
// on it keeps running, so nothing is rebuilt.

#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>
#include <utility>

#include <QMetaObject>
#include <QSettings>
#include <QString>

#include "models/engine_link.h"
#include "models/settings.h"
#include "models/timeshift_scrub.h"

namespace revenant::ui {

namespace {

[[nodiscard]] rpc::TimeShiftLimits limits_from(double minutes, double gib)
{
    rpc::TimeShiftLimits out;
    out.seconds = std::max(0.0, minutes) * 60.0;
    out.budget_bytes = static_cast<std::uint64_t>(std::max(0.0, gib) * 1024.0 * 1024.0 * 1024.0);
    return out;
}

}  // namespace

void EngineLink::load_timeshift_settings()
{
    const QSettings store;
    if (store.contains(settings::kTimeShiftMinutes) ||
        store.contains(settings::kTimeShiftBudgetGiB)) {
        timeshift_minutes_ =
            store.value(settings::kTimeShiftMinutes, settings::kTimeShiftMinutesDefault)
                .toDouble();
        timeshift_budget_gib_ =
            store.value(settings::kTimeShiftBudgetGiB, settings::kTimeShiftBudgetGiBDefault)
                .toDouble();
        timeshift_limits_known_.store(true, std::memory_order_release);
        const std::lock_guard<std::mutex> lock(timeshift_mutex_);
        timeshift_limits_ = limits_from(timeshift_minutes_, timeshift_budget_gib_);
        timeshift_limits_pending_ = true;
    }
}

void EngineLink::wake_timeshift()
{
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        source_work_pending_ = true;
    }
    supervisor_wake_.notify_all();
}

void EngineLink::setTimeShiftMinutes(double minutes)
{
    if (!std::isfinite(minutes) || minutes < 0.0 || minutes == timeshift_minutes_) {
        return;
    }
    timeshift_minutes_ = minutes;
    timeshift_limits_known_.store(true, std::memory_order_release);
    QSettings().setValue(settings::kTimeShiftMinutes, minutes);
    {
        const std::lock_guard<std::mutex> lock(timeshift_mutex_);
        timeshift_limits_ = limits_from(timeshift_minutes_, timeshift_budget_gib_);
        timeshift_limits_pending_ = true;
    }
    wake_timeshift();
    emit timeShiftLimitsChanged();
}

void EngineLink::setTimeShiftBudgetGiB(double gib)
{
    if (!std::isfinite(gib) || gib < 0.0 || gib == timeshift_budget_gib_) {
        return;
    }
    timeshift_budget_gib_ = gib;
    timeshift_limits_known_.store(true, std::memory_order_release);
    QSettings().setValue(settings::kTimeShiftBudgetGiB, gib);
    {
        const std::lock_guard<std::mutex> lock(timeshift_mutex_);
        timeshift_limits_ = limits_from(timeshift_minutes_, timeshift_budget_gib_);
        timeshift_limits_pending_ = true;
    }
    wake_timeshift();
    emit timeShiftLimitsChanged();
}

void EngineLink::setTimeShiftSpeed(double speed)
{
    if (!std::isfinite(speed) || speed < 1.0 || speed == timeshift_speed_) {
        return;
    }
    timeshift_speed_ = speed;
    emit timeShiftLimitsChanged();
    // A running replay takes the new pace where it is.
    if (timeshift_view_.replaying) {
        const std::lock_guard<std::mutex> lock(timeshift_mutex_);
        timeshift_seek_pending_ = true;
        timeshift_seek_ago_ = -1.0;
        timeshift_seek_speed_ = speed;
    }
    wake_timeshift();
}

void EngineLink::timeShiftScrub(double position)
{
    const double ago = timeshift_seconds_ago(position, timeshift_view_.held_seconds);
    {
        const std::lock_guard<std::mutex> lock(timeshift_mutex_);
        timeshift_seek_pending_ = true;
        timeshift_seek_ago_ = ago;
        timeshift_seek_speed_ = timeshift_speed_;
    }
    wake_timeshift();
}

void EngineLink::timeShiftLive() { timeShiftScrub(1.0); }

QString EngineLink::timeShiftLabel(double position) const
{
    return QString::fromStdString(
        timeshift_label(timeshift_seconds_ago(position, timeshift_view_.held_seconds)));
}

double EngineLink::timeShiftSecondsAgoAt(double position) const
{
    return timeshift_seconds_ago(position, timeshift_view_.held_seconds);
}

double EngineLink::timeShiftPosition() const
{
    return timeshift_position(timeshift_view_.seconds_ago, timeshift_view_.held_seconds);
}

rpc::VrxParams EngineLink::timeshift_for_add(rpc::VrxParams params)
{
    params.timeshift_replay = timeshift_engaged_;
    timeshift_pane_on_replay_ = timeshift_engaged_;
    return params;
}

rpc::VrxParams EngineLink::timeshift_for_set(rpc::VrxParams params) const
{
    params.timeshift_replay = timeshift_pane_on_replay_;
    return params;
}

void EngineLink::apply_timeshift(bool probe)
{
    if (client_ == nullptr) {
        timeshift_client_ = nullptr;
        return;
    }

    QString note;

    // A new connection is a new engine, which has no replay and has not been
    // told the bounds.
    if (timeshift_client_ != client_.get()) {
        timeshift_client_ = client_.get();
        timeshift_engaged_ = false;
        timeshift_pane_on_replay_ = false;
        timeshift_viewing_.store(false, std::memory_order_release);
        const std::lock_guard<std::mutex> lock(timeshift_mutex_);
        if (timeshift_limits_known_.load(std::memory_order_acquire)) {
            timeshift_limits_pending_ = true;
        }
    }

    std::optional<rpc::TimeShiftLimits> limits;
    bool seek = false;
    double ago = 0.0;
    double speed = 1.0;
    {
        const std::lock_guard<std::mutex> lock(timeshift_mutex_);
        if (std::exchange(timeshift_limits_pending_, false)) {
            limits = timeshift_limits_;
        }
        seek = std::exchange(timeshift_seek_pending_, false);
        ago = timeshift_seek_ago_;
        speed = timeshift_seek_speed_;
    }

    if (limits.has_value()) {
        // The engine ends the replay on a resize and takes its receivers, so
        // the pane's receiver comes home first.
        if (timeshift_engaged_) {
            seek = true;
            ago = 0.0;
        } else if (auto answer = client_->timeshift(limits); answer) {
            timeshift_extent_ = *answer;
        } else {
            note = QString::fromStdString(answer.error().message);
        }
    }

    if (seek) {
        // Fresh, so the target is measured from what is held now.
        if (auto now = client_->timeshift(std::nullopt); now) {
            timeshift_extent_ = *now;
        }
        if (ago < 0.0) {
            // A pace change only, at the replay's current position.
            if (timeshift_engaged_) {
                if (auto moved =
                        client_->start_timeshift_replay(timeshift_extent_.replay_ns, speed);
                    !moved) {
                    note = QString::fromStdString(moved.error().message);
                }
            }
        } else if (ago == 0.0) {
            if (timeshift_engaged_) {
                timeshift_engaged_ = false;
                if (timeshift_pane_on_replay_ && live_receiver_id_ != 0) {
                    static_cast<void>(recreate_receiver(live_receiver_params_, live_pane_key_));
                }
                timeshift_pane_on_replay_ = false;
                if (auto stopped = client_->stop_timeshift_replay(); !stopped) {
                    note = QString::fromStdString(stopped.error().message);
                }
                timeshift_viewing_.store(false, std::memory_order_release);
            }
            if (limits.has_value()) {
                if (auto answer = client_->timeshift(limits); answer) {
                    timeshift_extent_ = *answer;
                } else {
                    note = QString::fromStdString(answer.error().message);
                }
            }
        } else if (!timeshift_extent_.available) {
            note = QStringLiteral("nothing is held to go back to yet");
        } else {
            const std::int64_t at = timeshift_target_ns(timeshift_extent_.newest_ns, ago);
            if (auto started = client_->start_timeshift_replay(at, speed); !started) {
                note = QString::fromStdString(started.error().message);
            } else {
                timeshift_extent_ = *started;
                if (!timeshift_engaged_) {
                    timeshift_engaged_ = true;
                    if (live_receiver_id_ != 0) {
                        static_cast<void>(
                            recreate_receiver(live_receiver_params_, live_pane_key_));
                    }
                }
                timeshift_viewing_.store(true, std::memory_order_release);
            }
        }
    }

    // Once a second, and every pass while off live so the handle follows the
    // replay as it runs.
    if (!seek && (probe || timeshift_engaged_)) {
        if (auto now = client_->timeshift(std::nullopt); now) {
            timeshift_extent_ = *now;
        } else {
            // An engine older than the call keeps no buffer; the bar says so
            // by staying unavailable.
            timeshift_extent_ = rpc::TimeShiftExtent{};
        }
    }

    // The engine ended the replay on its own, a close or a resize.
    if (timeshift_engaged_ && !timeshift_extent_.replaying && !seek) {
        timeshift_engaged_ = false;
        timeshift_viewing_.store(false, std::memory_order_release);
    }

    post_timeshift(timeshift_extent_, note);
}

void EngineLink::post_timeshift(const rpc::TimeShiftExtent& extent, const QString& note)
{
    TimeShiftView view;
    view.available = extent.available;
    view.held_seconds =
        extent.available ? static_cast<double>(extent.newest_ns - extent.oldest_ns) / 1.0e9 : 0.0;
    view.capacity_seconds = extent.capacity_seconds;
    view.replaying = timeshift_engaged_ && extent.replaying;
    view.seconds_ago = view.replaying && extent.available
                           ? std::max(0.0, static_cast<double>(extent.newest_ns - extent.replay_ns) /
                                               1.0e9)
                           : 0.0;
    view.note = note;
    {
        const std::lock_guard<std::mutex> lock(timeshift_mutex_);
        timeshift_posted_ = view;
    }
    if (timeshift_wake_pending_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    QMetaObject::invokeMethod(
        this,
        [this] {
            timeshift_wake_pending_.store(false, std::memory_order_release);
            {
                const std::lock_guard<std::mutex> lock(timeshift_mutex_);
                timeshift_view_ = timeshift_posted_;
            }
            emit timeShiftChanged();
        },
        Qt::QueuedConnection);
}

}  // namespace revenant::ui
