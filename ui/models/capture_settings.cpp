#include "models/capture_settings.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>

#include "models/settings.h"

namespace revenant::ui {
namespace {

bool same(const CaptureChoice& a, const CaptureChoice& b)
{
    return a.enabled == b.enabled && a.folder == b.folder && a.retain == b.retain &&
           a.max_age == b.max_age;
}

std::string trimmed(const QString& text) { return text.trimmed().toStdString(); }

}  // namespace

CaptureSettings::CaptureSettings(QObject* parent)
    : QObject(parent), draft_(saved()), saved_(draft_)
{
}

QString CaptureSettings::defaultFolderPath()
{
    return QDir::toNativeSeparators(
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
            .filePath(QStringLiteral("capture")));
}

CaptureChoice CaptureSettings::saved()
{
    const QSettings store;
    CaptureChoice choice;
    choice.enabled = store.value(settings::kCaptureEnabled, kCaptureEnabledDefault).toBool();
    QString folder = store.value(settings::kCaptureFolder).toString().trimmed();
    if (folder.isEmpty()) {
        folder = defaultFolderPath();
    }
    choice.folder = folder.toStdWString();
    // contains() and not an empty value, because empty is a real choice here:
    // no limit. Only a key never written takes the default.
    choice.retain = store.contains(settings::kCaptureRetain)
                        ? trimmed(store.value(settings::kCaptureRetain).toString())
                        : std::string(kCaptureRetainDefault);
    choice.max_age = trimmed(store.value(settings::kCaptureMaxAge).toString());
    return choice;
}

QString CaptureSettings::folder() const { return QString::fromStdWString(draft_.folder); }

void CaptureSettings::setEnabled(bool on)
{
    if (on == draft_.enabled) {
        return;
    }
    draft_.enabled = on;
    emit draftChanged();
}

void CaptureSettings::setFolder(const QString& folder)
{
    draft_.folder = folder.trimmed().toStdWString();
    emit draftChanged();
}

void CaptureSettings::setRetain(const QString& retain)
{
    draft_.retain = trimmed(retain);
    emit draftChanged();
}

void CaptureSettings::setMaxAge(const QString& age)
{
    draft_.max_age = trimmed(age);
    emit draftChanged();
}

QString CaptureSettings::folderProblem() const
{
    return QString::fromStdString(capture_problems(draft_).folder);
}

QString CaptureSettings::retainProblem() const
{
    return QString::fromStdString(capture_problems(draft_).retain);
}

QString CaptureSettings::maxAgeProblem() const
{
    return QString::fromStdString(capture_problems(draft_).max_age);
}

QString CaptureSettings::costText() const
{
    return QString::fromStdString(capture_cost_sentence(draft_.retain));
}

bool CaptureSettings::changed() const { return !same(draft_, saved_); }

bool CaptureSettings::canApply() const
{
    return plan_capture_apply(engine_ours_, changed(), !capture_problems(draft_).any()) ==
           CaptureApply::RestartEngine;
}

void CaptureSettings::setEngineOurs(bool ours)
{
    if (ours == engine_ours_) {
        return;
    }
    engine_ours_ = ours;
    emit ownershipChanged();
    emit draftChanged();
}

void CaptureSettings::apply()
{
    const CaptureApply plan =
        plan_capture_apply(engine_ours_, changed(), !capture_problems(draft_).any());
    if (plan != CaptureApply::RestartEngine) {
        return;
    }
    QSettings store;
    store.setValue(settings::kCaptureEnabled, draft_.enabled);
    store.setValue(settings::kCaptureFolder, QString::fromStdWString(draft_.folder));
    store.setValue(settings::kCaptureRetain, QString::fromStdString(draft_.retain));
    store.setValue(settings::kCaptureMaxAge, QString::fromStdString(draft_.max_age));
    saved_ = draft_;
    emit draftChanged();
    emit restartWanted();
}

void CaptureSettings::revert()
{
    draft_ = saved_;
    emit draftChanged();
}

}  // namespace revenant::ui
