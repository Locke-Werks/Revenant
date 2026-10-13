// The settings window's capture section: the four values being edited, what
// is wrong with each, and applying them.
//
// The rules are models/capture_choice.h and are tested there. This is the
// part with QSettings and signals in it. main() owns the one instance, tells
// it whether the window started its engine, and does the restart when asked;
// the restart is main()'s because the launcher and the watch on it are locals
// of main().
//
// EDITED, THEN APPLIED, rather than live like the display sliders. Every
// change restarts the engine, which drops the source and every receiver, so a
// restart per keystroke in the folder field is not a thing to offer.

#pragma once

#include <QObject>
#include <QString>

#include "models/capture_choice.h"

namespace revenant::ui {

class CaptureSettings : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY draftChanged)
    Q_PROPERTY(QString folder READ folder WRITE setFolder NOTIFY draftChanged)
    Q_PROPERTY(QString retain READ retain WRITE setRetain NOTIFY draftChanged)
    Q_PROPERTY(QString maxAge READ maxAge WRITE setMaxAge NOTIFY draftChanged)

    Q_PROPERTY(QString folderProblem READ folderProblem NOTIFY draftChanged)
    Q_PROPERTY(QString retainProblem READ retainProblem NOTIFY draftChanged)
    Q_PROPERTY(QString maxAgeProblem READ maxAgeProblem NOTIFY draftChanged)
    Q_PROPERTY(QString costText READ costText NOTIFY draftChanged)

    // The draft differs from what is saved, and has no problem.
    Q_PROPERTY(bool canApply READ canApply NOTIFY draftChanged)
    Q_PROPERTY(bool changed READ changed NOTIFY draftChanged)

    Q_PROPERTY(bool engineOurs READ engineOurs NOTIFY ownershipChanged)
    Q_PROPERTY(QString defaultFolder READ defaultFolder CONSTANT)

public:
    explicit CaptureSettings(QObject* parent = nullptr);

    // What QSettings holds, with the defaults filled in. main() reads this to
    // start the engine.
    [[nodiscard]] static CaptureChoice saved();
    [[nodiscard]] static QString defaultFolderPath();

    [[nodiscard]] bool enabled() const { return draft_.enabled; }
    [[nodiscard]] QString folder() const;
    [[nodiscard]] QString retain() const { return QString::fromStdString(draft_.retain); }
    [[nodiscard]] QString maxAge() const { return QString::fromStdString(draft_.max_age); }
    void setEnabled(bool on);
    void setFolder(const QString& folder);
    void setRetain(const QString& retain);
    void setMaxAge(const QString& age);

    [[nodiscard]] QString folderProblem() const;
    [[nodiscard]] QString retainProblem() const;
    [[nodiscard]] QString maxAgeProblem() const;
    [[nodiscard]] QString costText() const;
    [[nodiscard]] bool changed() const;
    [[nodiscard]] bool canApply() const;
    [[nodiscard]] bool engineOurs() const { return engine_ours_; }
    [[nodiscard]] QString defaultFolder() const { return defaultFolderPath(); }

    // Whether this window started the engine it is connected to. Only then
    // does applying restart anything.
    void setEngineOurs(bool ours);

    // Saves the draft and, for an engine this window started, asks main() to
    // restart it. Does nothing with a problem or no change; see
    // plan_capture_apply.
    Q_INVOKABLE void apply();

    // The draft back to what is saved.
    Q_INVOKABLE void revert();

signals:
    void draftChanged();
    void ownershipChanged();
    void restartWanted();

private:
    CaptureChoice draft_;
    CaptureChoice saved_;
    bool engine_ours_ = false;
};

}  // namespace revenant::ui
