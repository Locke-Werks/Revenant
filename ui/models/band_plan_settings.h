// The band plan's two preferences, the bar and the regions, as one object
// with change signals.
//
// QSettings has no change signal, so before this the bar read its settings
// once at construction and the band menu read them at every call, and a
// change from anywhere reached neither until a restart. The settings window
// writes through here, and the bar and the menu bind to the properties, so a
// toggle shows at once.

#pragma once

#include <cstdint>

#include <QObject>
#include <QSettings>
#include <QtQml/qqmlregistration.h>

#include "models/band_plan.h"
#include "models/region_choice.h"
#include "models/settings.h"

namespace revenant::ui {

class BandPlanSettings : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool showBar READ showBar WRITE setShowBar NOTIFY showBarChanged)
    Q_PROPERTY(int region READ region NOTIFY regionChanged)
    Q_PROPERTY(bool us READ us WRITE setUs NOTIFY regionChanged)
    Q_PROPERTY(bool canada READ canada WRITE setCanada NOTIFY regionChanged)

public:
    explicit BandPlanSettings(QObject* parent = nullptr) : QObject(parent)
    {
        const QSettings store;
        show_bar_ = store.value(settings::kShowBandBar, settings::kShowBandBarDefault).toBool();
        region_ = restore_region(
            store.value(settings::kBandPlanRegion, settings::kBandPlanRegionDefault).toUInt());
    }

    [[nodiscard]] bool showBar() const { return show_bar_; }
    [[nodiscard]] int region() const { return static_cast<int>(region_); }
    [[nodiscard]] bool us() const { return (region_ & kRegionUS) != 0; }
    [[nodiscard]] bool canada() const { return (region_ & kRegionCA) != 0; }

    void setShowBar(bool show)
    {
        if (show == show_bar_) {
            return;
        }
        show_bar_ = show;
        QSettings().setValue(settings::kShowBandBar, show);
        emit showBarChanged();
    }

    void setUs(bool on) { setRegionMask(toggle_region(region_, kRegionUS, on)); }
    void setCanada(bool on) { setRegionMask(toggle_region(region_, kRegionCA, on)); }

signals:
    void showBarChanged();
    void regionChanged();

private:
    void setRegionMask(std::uint32_t mask)
    {
        if (mask == region_) {
            // A refused uncheck still has to tell the checkbox, which has
            // already drawn itself unchecked.
            emit regionChanged();
            return;
        }
        region_ = mask;
        QSettings().setValue(settings::kBandPlanRegion, mask);
        emit regionChanged();
    }

    bool show_bar_ = settings::kShowBandBarDefault;
    std::uint32_t region_ = kRegionUSCA;
};

}  // namespace revenant::ui
