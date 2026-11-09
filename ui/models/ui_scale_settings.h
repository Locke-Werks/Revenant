// The interface scale: a factor on top of the display scaling Windows sets,
// for everything the client draws. The owner asked for it on 2026-11-09, with
// the tiled layout, to diverge from the system's scaling; each pane has its
// own scale on top of this one (models/pane_layout_store.h).
//
// APPLIED AT START. Qt reads its scale factor once, when the application is
// made, from QT_SCALE_FACTOR, so main.cpp reads the setting before that and
// sets the variable, and a change here takes effect on the next run. The
// settings panel says so. A QT_SCALE_FACTOR already in the environment wins,
// and a smoke run ignores the setting so its photographs are at one.

#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <cmath>

#include "models/settings.h"

namespace revenant::ui {

// The steps offered, as percentages.
inline constexpr std::array<int, 7> kUiScaleSteps = {75, 85, 100, 110, 125, 150, 175};

// The stored scale held to one of the steps, so a hand-edited registry value
// cannot start the client unreadable.
[[nodiscard]] inline int ui_scale_percent(double stored)
{
    const int wanted = static_cast<int>(std::lround(stored * 100.0));
    int best = 100;
    int best_gap = 1000;
    for (const int step : kUiScaleSteps) {
        const int gap = std::abs(step - wanted);
        if (gap < best_gap) {
            best = step;
            best_gap = gap;
        }
    }
    return best;
}

class UiScaleSettings : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QStringList choices READ choices CONSTANT)
    Q_PROPERTY(QString current READ current NOTIFY currentChanged)
    Q_PROPERTY(bool pending READ pending NOTIFY currentChanged)

public:
    explicit UiScaleSettings(QObject* parent = nullptr) : QObject(parent)
    {
        percent_ = ui_scale_percent(
            QSettings().value(settings::kUiScale, 1.0).toDouble());
        started_ = percent_;
    }

    [[nodiscard]] static QStringList choices()
    {
        QStringList out;
        for (const int step : kUiScaleSteps) {
            out.append(QString::number(step) + QLatin1Char('%'));
        }
        return out;
    }

    [[nodiscard]] QString current() const { return QString::number(percent_) + QLatin1Char('%'); }

    // Chosen, and not yet in effect until the next run.
    [[nodiscard]] bool pending() const { return percent_ != started_; }

    Q_INVOKABLE void pick(const QString& choice)
    {
        QString digits = choice;
        digits.remove(QLatin1Char('%'));
        bool ok = false;
        const int percent = ui_scale_percent(digits.toInt(&ok) / 100.0);
        if (!ok || percent == percent_) {
            return;
        }
        percent_ = percent;
        QSettings().setValue(settings::kUiScale, percent_ / 100.0);
        emit currentChanged();
    }

signals:
    void currentChanged();

private:
    int percent_ = 100;
    int started_ = 100;
};

}  // namespace revenant::ui
