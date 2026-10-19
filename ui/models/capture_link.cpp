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
#include <string>
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

void EngineLink::searchCapture(const QString& window, const QString& band,
                               const QString& minDuration, const QString& decoder,
                               const QString& decoded)
{
    auto terms = parse_decoded_terms(decoded.toStdString());
    if (!terms) {
        capture_hits_.clear();
        capture_occupancy_.clear();
        capture_search_fault_ =
            QStringLiteral("could not read \"%1\" as decoded terms; write them as "
                           "mmsi=366123456 talkgroup=101")
                .arg(decoded.trimmed());
        capture_search_busy_ = false;
        emit captureHitsChanged();
        return;
    }
    double low = spanLowHz();
    double high = spanHighHz();
    if (!band.trimmed().isEmpty()) {
        const auto typed = parse_capture_band(band.toStdString());
        if (!typed) {
            capture_hits_.clear();
            capture_occupancy_.clear();
            capture_search_fault_ =
                QStringLiteral("could not read \"%1\" as a band; write it as 162.4M:162.6M")
                    .arg(band.trimmed());
            capture_search_busy_ = false;
            emit captureHitsChanged();
            return;
        }
        low = static_cast<double>(typed->low_hz);
        high = static_cast<double>(typed->high_hz);
    }
    {
        const std::lock_guard<std::mutex> lock(capture_search_mutex_);
        want_capture_search_ = true;
        want_capture_low_hz_ = low;
        want_capture_high_hz_ = high;
        want_capture_window_ns_ = capture_choice_ns(kCaptureWindows, window.toStdString(), 1);
        want_capture_min_ns_ =
            capture_choice_ns(kCaptureMinDurations, minDuration.toStdString(), 0);
        want_capture_decoder_ = decoder == QStringLiteral("any") ? std::string{}
                                                                  : decoder.toStdString();
        want_capture_decoded_ = std::move(*terms);
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
    std::int64_t window_ns = kCaptureWindowNs;
    std::int64_t min_ns = 0;
    std::string decoder;
    std::vector<std::pair<std::string, std::string>> decoded;
    {
        const std::lock_guard<std::mutex> lock(capture_search_mutex_);
        if (!std::exchange(want_capture_search_, false)) {
            return;
        }
        low = want_capture_low_hz_;
        high = want_capture_high_hz_;
        window_ns = want_capture_window_ns_;
        min_ns = want_capture_min_ns_;
        decoder = want_capture_decoder_;
        decoded = want_capture_decoded_;
    }

    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const CaptureWindow window = capture_window(now_ns, window_ns, low, high);
    rpc::CaptureQuery query;
    query.from_ns = window.from_ns;
    query.to_ns = window.to_ns;
    query.min_duration_ns = min_ns;
    query.decoder = decoder;
    query.decoded = decoded;
    if (window.freq_high_hz > window.freq_low_hz) {
        query.freq_low_hz = window.freq_low_hz;
        query.freq_high_hz = window.freq_high_hz;
    }

    // Asked only with a band, because occupancy needs one; a refusal leaves the strip empty and the list stands on its own.
    QVariantList columns;
    if (window.freq_high_hz > window.freq_low_hz) {
        rpc::CaptureOccupancyRequest request;
        request.from_ns = window.from_ns;
        request.to_ns = window.to_ns;
        request.bucket_ns = window_ns / kCaptureOccupancyBuckets;
        request.freq_low_hz = window.freq_low_hz;
        request.freq_high_hz = window.freq_high_hz;
        // The same filter as the list, so the strip shows where the hits are.
        request.filter.min_duration_ns = min_ns;
        request.filter.decoder = decoder;
        request.filter.decoded = decoded;
        if (auto buckets = client_->capture_occupancy(request)) {
            for (const rpc::CaptureBucket& bucket : *buckets) {
                QVariantMap map;
                map.insert(QStringLiteral("occupied"), bucket.occupied);
                map.insert(QStringLiteral("captured"), bucket.captured);
                columns.append(map);
            }
        }
    }

    QVariantList rows;
    QString fault;
    auto answer = client_->capture_search(query);
    if (answer) {
        std::vector<CaptureHitIn> hits;
        hits.reserve(answer->hits.size());
        for (const rpc::CaptureHit& hit : answer->hits) {
            hits.push_back({hit.start_ns, hit.end_ns, hit.freq_lower_hz, hit.freq_upper_hz,
                            hit.confidence, hit.family, hit.replay_uri, hit.decoder,
                            hit.decoded});
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
            map.insert(QStringLiteral("decoded"), QString::fromStdString(row.decoded));
            rows.append(map);
        }
    } else {
        fault = QString::fromStdString(answer.error().message);
    }

    {
        const std::lock_guard<std::mutex> lock(capture_search_mutex_);
        handover_has_capture_hits_ = true;
        handover_capture_hits_ = std::move(rows);
        handover_capture_occupancy_ = std::move(columns);
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
        capture_occupancy_ = std::move(handover_capture_occupancy_);
        capture_search_fault_ = std::move(handover_capture_search_fault_);
        handover_capture_hits_.clear();
        handover_capture_occupancy_.clear();
        handover_capture_search_fault_.clear();
    }
    capture_search_busy_ = false;
    emit captureHitsChanged();
}

}  // namespace revenant::ui
