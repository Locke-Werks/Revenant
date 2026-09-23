// The operator's pins on the span displays' colour map, remembered between
// launches.
//
// One object shared by the spectrum and the waterfall, because they draw one
// span against one pair of ends; a pin on one display and not the other would
// colour a signal on the waterfall differently from the level the trace above
// it reads. The rules for where a pin lands and how two pins keep apart are
// render/spectrum_scale.h, with cases in ui/tests; this holds the state and
// writes it to QSettings.
//
// REMEMBERED, unlike the margin bar. docs/ui-spectrum.md says what a pin is
// for: comparing two captures, where a scale that moves lies about which
// signal was stronger. Comparing captures is something done across sessions,
// and a pin forgotten at every launch would have to be set again by hand to
// the same number to compare against yesterday's.

#pragma once

#include <QObject>
#include <QSettings>
#include <QtQmlIntegration>

#include "models/settings.h"
#include "render/spectrum_scale.h"

namespace revenant::ui {

class ScaleSettings : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool floorPinned READ floorPinned NOTIFY pinsChanged)
    Q_PROPERTY(double floorDb READ floorDb NOTIFY pinsChanged)
    Q_PROPERTY(bool ceilingPinned READ ceilingPinned NOTIFY pinsChanged)
    Q_PROPERTY(double ceilingDb READ ceilingDb NOTIFY pinsChanged)

public:
    explicit ScaleSettings(QObject* parent = nullptr) : QObject(parent)
    {
        const QSettings store;
        pins_.floor_pinned = store.value(settings::kScaleFloorPinned, false).toBool();
        pins_.floor_db = store.value(settings::kScaleFloorDb, 0.0).toFloat();
        pins_.ceiling_pinned = store.value(settings::kScaleCeilingPinned, false).toBool();
        pins_.ceiling_db = store.value(settings::kScaleCeilingDb, 0.0).toFloat();
    }

    [[nodiscard]] bool floorPinned() const { return pins_.floor_pinned; }
    [[nodiscard]] double floorDb() const { return pins_.floor_db; }
    [[nodiscard]] bool ceilingPinned() const { return pins_.ceiling_pinned; }
    [[nodiscard]] double ceilingDb() const { return pins_.ceiling_db; }
    [[nodiscard]] const ScalePins& pins() const { return pins_; }

    // Pins an end where it is drawn now, so the picture does not jump.
    Q_INVOKABLE void pinFloor(double drawn_db)
    {
        store(set_floor_pin(pins_, pin_level(static_cast<float>(drawn_db))));
    }

    Q_INVOKABLE void pinCeiling(double drawn_db)
    {
        store(set_ceiling_pin(pins_, pin_level(static_cast<float>(drawn_db))));
    }

    Q_INVOKABLE void unpinFloor()
    {
        ScalePins next = pins_;
        next.floor_pinned = false;
        store(next);
    }

    Q_INVOKABLE void unpinCeiling()
    {
        ScalePins next = pins_;
        next.ceiling_pinned = false;
        store(next);
    }

    // A pinned end moved by whole steps of kPinStepDb, up for a positive
    // count. Nothing for an end that is automatic.
    Q_INVOKABLE void nudgeFloor(int steps)
    {
        if (pins_.floor_pinned) {
            store(set_floor_pin(pins_, pins_.floor_db + static_cast<float>(steps) * kPinStepDb));
        }
    }

    Q_INVOKABLE void nudgeCeiling(int steps)
    {
        if (pins_.ceiling_pinned) {
            store(set_ceiling_pin(pins_,
                                  pins_.ceiling_db + static_cast<float>(steps) * kPinStepDb));
        }
    }

signals:
    void pinsChanged();

private:
    // Written as it happens, like the volume: a pin is one event an operator
    // made, not a stream of them.
    void store(const ScalePins& next)
    {
        pins_ = next;
        QSettings out;
        out.setValue(settings::kScaleFloorPinned, pins_.floor_pinned);
        out.setValue(settings::kScaleFloorDb, static_cast<double>(pins_.floor_db));
        out.setValue(settings::kScaleCeilingPinned, pins_.ceiling_pinned);
        out.setValue(settings::kScaleCeilingDb, static_cast<double>(pins_.ceiling_db));
        emit pinsChanged();
    }

    ScalePins pins_;
};

}  // namespace revenant::ui
