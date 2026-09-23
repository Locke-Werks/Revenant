// The recording section of the picker and the recording strip under the top
// bar, as one object the QML binds to.
//
// WHY A SEPARATE OBJECT AND NOT MORE OF EngineLink. EngineLink is the
// connection and everything that has to happen on its supervisor thread. This
// is none of that: it reads a header off the local disk, keeps a list in
// QSettings, composes a URI and hands it to EngineLink::openSource, which is
// the same call and the same close-then-open sequence the radios use. What it
// reads back, the open source's name and how far the engine is through it,
// EngineLink publishes as openedSource and sourceSamplesDelivered.
//
// Every rule is in a Qt-free header with cases in ui/tests: the header read in
// models/recording_header.h, the URI and the blockers in
// models/recording_plan.h, the list in models/recent_recordings.h and the
// strip's text in models/recording_status.h. This file converts types and
// holds state.

#pragma once

#include <vector>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include "models/recent_recordings.h"
#include "models/recording_header.h"
#include "models/recording_plan.h"
#include "models/recording_status.h"

namespace revenant::ui {

class EngineLink;

class RecordingLink : public QObject {
    Q_OBJECT

    // ---- the picker's section --------------------------------------------

    Q_PROPERTY(QStringList fileFilters READ fileFilters CONSTANT)

    // Where the dialog opens: the folder of the newest recording on the list,
    // or nothing, which leaves it to the platform.
    Q_PROPERTY(QUrl folder READ folder NOTIFY recentChanged)

    // The recent list, newest first, only the files that are still there.
    // Each row: path, name, folder, centerText, rateText, formatText.
    Q_PROPERTY(QVariantList recent READ recent NOTIFY recentChanged)

    // A file has been picked and its header read.
    Q_PROPERTY(bool chosen READ chosen NOTIFY chosenChanged)

    // What the file says, for the preview. See choose_path and empty_preview
    // in the .cpp for the keys; every value but the flags and the notes is
    // already a string to show.
    Q_PROPERTY(QVariantMap preview READ preview NOTIFY chosenChanged)

    // A centre read off the name, as text to show and as hertz for the "use"
    // button, or empty and zero.
    Q_PROPERTY(QString guessText READ guessText NOTIFY chosenChanged)
    Q_PROPERTY(double guessHz READ guessHz NOTIFY chosenChanged)

    // The three boxes. Held here so a recent pick and a --open-recording can
    // fill them, and so the plan below is recomputed from one place.
    Q_PROPERTY(QString centerText READ centerText WRITE setCenterText NOTIFY entryChanged)
    Q_PROPERTY(QString rateText READ rateText WRITE setRateText NOTIFY entryChanged)
    Q_PROPERTY(QString formatText READ formatText WRITE setFormatText NOTIFY entryChanged)

    // ready, uri and blocker, from plan_recording_open.
    Q_PROPERTY(QVariantMap plan READ plan NOTIFY entryChanged)

    // ---- the strip under the top bar -------------------------------------

    // A recording is what the engine has open: a file backend with a length.
    Q_PROPERTY(bool playing READ playing NOTIFY playbackChanged)
    Q_PROPERTY(QString playingName READ playingName NOTIFY playbackChanged)
    Q_PROPERTY(QString positionText READ positionText NOTIFY playbackChanged)
    Q_PROPERTY(QString paceText READ paceText NOTIFY playbackChanged)

    // The pace control: its options, the one the engine says is in force, or
    // empty for a pace it does not offer, and the engine's refusal of the last
    // one picked. See kPaceOptions and pace_option_for.
    Q_PROPERTY(QStringList paceOptions READ paceOptions CONSTANT)
    Q_PROPERTY(QString paceOption READ paceOption NOTIFY playbackChanged)
    Q_PROPERTY(QString paceFault READ paceFault NOTIFY playbackChanged)

    Q_PROPERTY(bool ended READ ended NOTIFY playbackChanged)
    Q_PROPERTY(double fraction READ fraction NOTIFY playbackChanged)

    // Non-empty when the engine opened something other than what the preview
    // read. See compare_opened_recording.
    Q_PROPERTY(QString mismatch READ mismatch NOTIFY playbackChanged)

public:
    // `persist` false keeps the recent list in memory, which is what a smoke
    // run wants: it writes no settings, for the reason main.cpp gives.
    // `engine_address` is where the engine is, which decides whether the
    // preview can be about the file the engine will open.
    RecordingLink(EngineLink& engine, QString engine_address, bool persist,
                  QObject* parent = nullptr);

    [[nodiscard]] QStringList fileFilters() const;
    [[nodiscard]] QUrl folder() const;
    [[nodiscard]] QVariantList recent() const;
    [[nodiscard]] bool chosen() const { return chosen_; }
    [[nodiscard]] QVariantMap preview() const { return preview_; }
    [[nodiscard]] QString guessText() const { return guess_text_; }
    [[nodiscard]] double guessHz() const { return guess_hz_; }
    [[nodiscard]] QString centerText() const { return center_text_; }
    [[nodiscard]] QString rateText() const { return rate_text_; }
    [[nodiscard]] QString formatText() const { return format_text_; }
    [[nodiscard]] QVariantMap plan() const { return plan_map_; }

    [[nodiscard]] bool playing() const { return playing_; }
    [[nodiscard]] QString playingName() const { return playing_name_; }
    [[nodiscard]] QString positionText() const { return position_text_; }
    [[nodiscard]] QString paceText() const { return pace_text_; }
    [[nodiscard]] QStringList paceOptions() const;
    [[nodiscard]] QString paceOption() const { return pace_option_; }
    [[nodiscard]] QString paceFault() const { return pace_fault_; }
    [[nodiscard]] bool ended() const { return ended_; }
    [[nodiscard]] double fraction() const { return fraction_; }
    [[nodiscard]] QString mismatch() const { return mismatch_; }

    void setCenterText(const QString& text);
    void setRateText(const QString& text);
    void setFormatText(const QString& text);

    // A file from the dialog, which hands back a URL.
    Q_INVOKABLE void chooseUrl(const QUrl& url);

    // A row of the recent list, with the boxes it was last opened with.
    Q_INVOKABLE void chooseRecent(int index);

    // Copies the name's guess into the centre box. A guess is never applied
    // on its own: see guess_center_from_name.
    Q_INVOKABLE void useGuess();

    // Sends the plan's URI through EngineLink::openSource and puts the file at
    // the top of the recent list. Does nothing when the plan is not ready,
    // which the button's enabled state already says.
    Q_INVOKABLE void open();

    // One of paceOptions, sent through EngineLink::setSourcePace. The segment
    // fills when the engine answers, not when the click lands.
    Q_INVOKABLE void setPace(const QString& option);

    // --open-recording, each PATH[:CENTER] in the order given. Every one but
    // the last goes on the recent list as though it had been opened; the last
    // is chosen, its centre put in the box, and opened if the plan is ready.
    // Returns a sentence for stderr when it could not be opened, or empty.
    [[nodiscard]] QString openAtStartup(const QStringList& arguments);

signals:
    void recentChanged();
    void chosenChanged();
    void entryChanged();
    void playbackChanged();

private:
    void choose_path(const QString& path);
    void replan();
    void refresh_playback();
    void load_recent();
    void store_recent();

    EngineLink& engine_;
    QString engine_address_;
    bool persist_ = false;

    std::vector<RecentRecording> recent_;

    bool chosen_ = false;
    RecordingHeader header_;
    QVariantMap preview_;
    QString guess_text_;
    double guess_hz_ = 0.0;
    QString center_text_;
    QString rate_text_;
    QString format_text_;
    RecordingPlan plan_;
    QVariantMap plan_map_;

    // What the last open planned, compared against what the engine reports
    // once its descriptor for that URI arrives.
    QString opened_uri_;
    OpenedRecording opened_plan_;

    bool playing_ = false;
    QString playing_name_;
    QString position_text_;
    QString pace_text_;
    QString pace_option_;
    QString pace_fault_;
    bool ended_ = false;
    double fraction_ = 0.0;
    QString mismatch_;
};

}  // namespace revenant::ui
