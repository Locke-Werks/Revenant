// Which picture the span's lower display draws, and how long the persistence
// picture remembers, remembered between launches.
//
// Three views of one history: the waterfall (render/waterfall_item.h), the
// phosphor persistence spectrum (render/persistence_item.h) and the 3D
// waterfall (render/surface3d_item.h). A singleton rather than a property on
// the span view, because the settings panel offers the decay time and cannot
// see the span view's id.

#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QtQmlIntegration>

#include "models/settings.h"
#include "render/persistence_grid.h"

namespace revenant::ui {

class SpanDisplaySettings : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // "waterfall", "persistence" or "3D", as the selector reads them.
    Q_PROPERTY(QString view READ view WRITE setView NOTIFY viewChanged)
    Q_PROPERTY(double persistenceDecaySeconds READ persistenceDecaySeconds WRITE
                   setPersistenceDecaySeconds NOTIFY persistenceDecaySecondsChanged)

public:
    explicit SpanDisplaySettings(QObject* parent = nullptr) : QObject(parent)
    {
        const QSettings store;
        view_ = clamp_view(store.value(settings::kSpanDisplayView, QStringLiteral("waterfall"))
                               .toString());
        decay_ = clamp_persistence_decay(
            store.value(settings::kPersistenceDecaySeconds, kDefaultPersistenceDecaySeconds)
                .toDouble());
    }

    // Anything else, from an older or a hand-edited store, is the waterfall.
    [[nodiscard]] static QString clamp_view(const QString& view)
    {
        if (view == QStringLiteral("persistence") || view == QStringLiteral("3D")) {
            return view;
        }
        return QStringLiteral("waterfall");
    }

    [[nodiscard]] QString view() const { return view_; }
    void setView(const QString& view)
    {
        const QString next = clamp_view(view);
        if (next == view_) {
            return;
        }
        view_ = next;
        QSettings().setValue(settings::kSpanDisplayView, view_);
        emit viewChanged();
    }

    [[nodiscard]] double persistenceDecaySeconds() const { return decay_; }
    void setPersistenceDecaySeconds(double seconds)
    {
        seconds = clamp_persistence_decay(seconds);
        if (seconds == decay_) {
            return;
        }
        decay_ = seconds;
        QSettings().setValue(settings::kPersistenceDecaySeconds, decay_);
        emit persistenceDecaySecondsChanged();
    }

signals:
    void viewChanged();
    void persistenceDecaySecondsChanged();

private:
    QString view_;
    double decay_ = kDefaultPersistenceDecaySeconds;
};

}  // namespace revenant::ui
