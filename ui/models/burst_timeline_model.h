// The burst timeline as QML reads it: models/burst_timeline.h fed from the
// engine link's existing streams, with no call of its own to the engine.
//
// SOURCES. Transmissions come from the detection list, each detector track's
// newly detected stretch an observation, and from the receivers a plugin
// opened in the rack (the trunk tracker's voice calls, which the VrxEvent
// stream adds and removes), present for as long as the rack holds them.
// Transcripts and decoded messages attach by time and frequency overlap.
//
// Created in QML and given the link, so main.cpp holds nothing for it and a
// window with the timeline closed pays for the bookkeeping only.
//
// Qt thread only.

#pragma once

#include <cstdint>
#include <unordered_map>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration>

#include "models/burst_timeline.h"

namespace revenant::ui {

class EngineLink;

class BurstTimelineModel : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QObject* link READ link WRITE setLink NOTIFY linkChanged)

    // Wall clock now, milliseconds since the epoch: the live edge.
    Q_PROPERTY(double nowMs READ nowMs NOTIFY changed)

    // One map per lane, ascending: hz and a label in MHz.
    Q_PROPERTY(QVariantList lanes READ lanes NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(double oldestMs READ oldestMs NOTIFY changed)

    // Whether a click can jump into the capture: the engine is keeping one.
    Q_PROPERTY(bool captureAvailable READ captureAvailable NOTIFY changed)

    // What the last export, tune or capture jump did, in a sentence.
    Q_PROPERTY(QString note READ note NOTIFY noteChanged)

public:
    explicit BurstTimelineModel(QObject* parent = nullptr);

    [[nodiscard]] QObject* link() const;
    void setLink(QObject* link);

    [[nodiscard]] double nowMs() const;
    [[nodiscard]] QVariantList lanes() const;
    [[nodiscard]] int count() const { return static_cast<int>(timeline_.bursts().size()); }
    [[nodiscard]] double oldestMs() const;
    [[nodiscard]] bool captureAvailable() const;
    [[nodiscard]] QString note() const { return note_; }

    // Bursts overlapping [fromMs, toMs], oldest first: id, laneHz, hz,
    // bandwidthHz, startMs, endMs, open, mode, snr, ids, decoded, transcript.
    [[nodiscard]] Q_INVOKABLE QVariantList bursts(double fromMs, double toMs) const;

    // The visible bursts as CSV, written to file. Sets note either way.
    Q_INVOKABLE bool exportCsv(double fromMs, double toMs, const QUrl& file);

    // Tunes the focused receiver to the burst, by the detection path.
    Q_INVOKABLE void tune(qulonglong id);

    // Replays the burst out of the capture: asks the engine for hits over the
    // burst, takes the capture's root from the first, and opens the burst's
    // own span with a second of lead either side.
    Q_INVOKABLE void openCapture(qulonglong id);

    Q_INVOKABLE void clear();

signals:
    void linkChanged();
    void changed();
    void noteChanged();

private:
    void on_detections();
    void on_transcripts();
    void on_decoded();
    void on_capture_hits();
    void on_tick();
    void set_note(const QString& note);

    // Source sample to wall clock, against the detector's last decision.
    [[nodiscard]] std::int64_t sample_to_ns(std::uint64_t sample) const;

    QPointer<EngineLink> link_;
    BurstTimeline timeline_;
    QTimer tick_;
    QString note_;

    // The anchor sample_to_ns works from.
    std::uint64_t anchor_sample_ = 0;
    std::int64_t anchor_ns_ = 0;
    double rate_ = 0.0;

    // Per detector track, the last_detected sample already folded in.
    std::unordered_map<std::uint64_t, std::uint64_t> track_seen_;

    std::uint64_t caption_serial_ = 0;
    std::uint64_t decoded_serial_ = 0;

    // Receiver ids to where they were last heard, from transcripts, for a
    // decoded message whose receiver has left the rack.
    std::unordered_map<std::uint64_t, std::int64_t> vrx_hz_;

    std::uint64_t capture_pending_ = 0;
    bool capture_search_seen_busy_ = false;
};

}  // namespace revenant::ui
