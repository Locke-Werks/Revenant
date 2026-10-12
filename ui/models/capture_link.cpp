// The capture panel's half of EngineLink: posting a search to the supervisor
// and taking the rows back.
//
// Moves values between the Qt thread and the supervisor the way pace_link.cpp
// does and decides nothing. What is asked for and how a hit reads is
// models/capture_browser.h, with its cases.
//
// OPENING A HIT IS NOT HERE. A row's uri goes to openSource like any radio or
// recording, which closes what is open first, so a replay and the way back to
// the live radio are the picker's one path and not a second one beside it.

#include <chrono>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include <QDateTime>
#include <QMetaObject>
#include <QString>
#include <QVariantMap>

#include "core/rpc/client.h"
#include "core/rpc/types.h"
#include "models/capture_browser.h"
#include "models/engine_link.h"

namespace revenant::ui {

void EngineLink::searchCapture()
{
    {
        const std::lock_guard<std::mutex> lock(capture_search_mutex_);
        want_capture_search_ = true;
        want_capture_low_hz_ = spanLowHz();
        want_capture_high_hz_ = spanHighHz();
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        source_work_pending_ = true;
    }
    supervisor_wake_.notify_all();
    capture_search_busy_ = true;
    emit captureHitsChanged();
}

void EngineLink::apply_capture_search()
{
    if (client_ == nullptr) {
        return;
    }
    double low = 0.0;
    double high = 0.0;
    {
        const std::lock_guard<std::mutex> lock(capture_search_mutex_);
        if (!std::exchange(want_capture_search_, false)) {
            return;
        }
        low = want_capture_low_hz_;
        high = want_capture_high_hz_;
    }

    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const CaptureWindow window = capture_window(now_ns, low, high);
    rpc::CaptureQuery query;
    query.from_ns = window.from_ns;
    query.to_ns = window.to_ns;
    if (window.freq_high_hz > window.freq_low_hz) {
        query.freq_low_hz = window.freq_low_hz;
        query.freq_high_hz = window.freq_high_hz;
    }

    QVariantList rows;
    QString fault;
    auto answer = client_->capture_search(query);
    if (answer) {
        std::vector<CaptureHitIn> hits;
        hits.reserve(answer->hits.size());
        for (const rpc::CaptureHit& hit : answer->hits) {
            hits.push_back({hit.start_ns, hit.end_ns, hit.freq_lower_hz, hit.freq_upper_hz,
                            hit.confidence, hit.family, hit.replay_uri});
        }
        const std::int64_t offset = QDateTime::currentDateTime().offsetFromUtc();
        for (const CaptureRow& row : capture_rows(std::move(hits), offset)) {
            QVariantMap map;
            map.insert(QStringLiteral("time"), QString::fromStdString(row.time));
            map.insert(QStringLiteral("duration"), QString::fromStdString(row.duration));
            map.insert(QStringLiteral("frequency"), QString::fromStdString(row.frequency));
            map.insert(QStringLiteral("label"), QString::fromStdString(row.label));
            map.insert(QStringLiteral("confidence"), QString::fromStdString(row.confidence));
            map.insert(QStringLiteral("uri"), QString::fromStdString(row.replay_uri));
            rows.append(map);
        }
    } else {
        fault = QString::fromStdString(answer.error().message);
    }

    {
        const std::lock_guard<std::mutex> lock(capture_search_mutex_);
        handover_has_capture_hits_ = true;
        handover_capture_hits_ = std::move(rows);
        handover_capture_search_fault_ = std::move(fault);
    }
    QMetaObject::invokeMethod(this, [this] { adopt_capture_search(); }, Qt::QueuedConnection);
}

void EngineLink::adopt_capture_search()
{
    {
        const std::lock_guard<std::mutex> lock(capture_search_mutex_);
        if (!std::exchange(handover_has_capture_hits_, false)) {
            return;
        }
        capture_hits_ = std::move(handover_capture_hits_);
        capture_search_fault_ = std::move(handover_capture_search_fault_);
        handover_capture_hits_.clear();
        handover_capture_search_fault_.clear();
    }
    capture_search_busy_ = false;
    emit captureHitsChanged();
}

}  // namespace revenant::ui
