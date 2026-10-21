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

#include <algorithm>

#include <QColor>
#include <QElapsedTimer>
#include <QObject>
#include <QSettings>
#include <QtQmlIntegration>

#include "models/settings.h"
#include "render/spectrum_scale.h"
#include "render/waterfall_scale.h"

namespace revenant::ui {

class ScaleSettings : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool floorPinned READ floorPinned NOTIFY pinsChanged)
    Q_PROPERTY(double floorDb READ floorDb NOTIFY pinsChanged)
    Q_PROPERTY(bool ceilingPinned READ ceilingPinned NOTIFY pinsChanged)
    Q_PROPERTY(double ceilingDb READ ceilingDb NOTIFY pinsChanged)
    Q_PROPERTY(double waterfallContrast READ waterfallContrast WRITE setWaterfallContrast
                   NOTIFY waterfallContrastChanged)
    Q_PROPERTY(double waterfallPivot READ waterfallPivot WRITE setWaterfallPivot
                   NOTIFY waterfallPivotChanged)
    Q_PROPERTY(bool waterfallAutoPivot READ waterfallAutoPivot WRITE setWaterfallAutoPivot
                   NOTIFY waterfallAutoPivotChanged)
    Q_PROPERTY(double spectrumRangeFit READ spectrumRangeFit WRITE setSpectrumRangeFit
                   NOTIFY spectrumRangeFitChanged)
    Q_PROPERTY(double spectrumSmoothing READ spectrumSmoothing WRITE setSpectrumSmoothing
                   NOTIFY spectrumSmoothingChanged)
    Q_PROPERTY(bool spectrumFloorSmoothing READ spectrumFloorSmoothing
                   WRITE setSpectrumFloorSmoothing NOTIFY spectrumSmoothingChanged)
    Q_PROPERTY(double captionZoom READ captionZoom WRITE setCaptionZoom NOTIFY captionZoomChanged)
    Q_PROPERTY(double minCaptionZoom READ minCaptionZoom CONSTANT)
    Q_PROPERTY(double maxCaptionZoom READ maxCaptionZoom CONSTANT)

public:
    // The hovered caption's growth over its resting 11 px. 1 is none; the
    // default is the 16 px it was fixed at before this was a setting, and the
    // most is three times that.
    static constexpr double kMinCaptionZoom = 1.0;
    static constexpr double kDefaultCaptionZoom = 16.0 / 11.0;
    static constexpr double kMaxCaptionZoom = 3.0 * kDefaultCaptionZoom;

    explicit ScaleSettings(QObject* parent = nullptr) : QObject(parent)
    {
        const QSettings store;
        pins_.floor_pinned = store.value(settings::kScaleFloorPinned, false).toBool();
        pins_.floor_db = store.value(settings::kScaleFloorDb, 0.0).toFloat();
        pins_.ceiling_pinned = store.value(settings::kScaleCeilingPinned, false).toBool();
        pins_.ceiling_db = store.value(settings::kScaleCeilingDb, 0.0).toFloat();
        waterfall_contrast_ = clamp_contrast(store.value(settings::kWaterfallContrast, 0.0).toDouble());
        waterfall_pivot_ = clamp_pivot(
            store.value(settings::kWaterfallPivot, kDefaultCurvePivot).toDouble());
        manual_pivot_ = waterfall_pivot_;
        waterfall_auto_pivot_ = store.value(settings::kWaterfallAutoPivot, false).toBool();
        spectrum_range_fit_ = clamp_fit(store.value(settings::kSpectrumRangeFit, 0.0).toDouble());
        spectrum_smoothing_ = clamp_smoothing(
            store.value(settings::kSpectrumSmoothing, kDefaultSmoothDecaySeconds).toDouble());
        spectrum_floor_smoothing_ = store.value(settings::kSpectrumFloorSmoothing, true).toBool();
        caption_zoom_ =
            clamp_caption_zoom(store.value(settings::kCaptionZoom, kDefaultCaptionZoom).toDouble());
    }

    [[nodiscard]] double captionZoom() const { return caption_zoom_; }
    [[nodiscard]] static double minCaptionZoom() { return kMinCaptionZoom; }
    [[nodiscard]] static double maxCaptionZoom() { return kMaxCaptionZoom; }

    void setCaptionZoom(double value)
    {
        value = clamp_caption_zoom(value);
        if (value == caption_zoom_) {
            return;
        }
        caption_zoom_ = value;
        QSettings().setValue(settings::kCaptionZoom, value);
        emit captionZoomChanged();
    }

    [[nodiscard]] double waterfallContrast() const { return waterfall_contrast_; }
    [[nodiscard]] double waterfallPivot() const { return waterfall_pivot_; }
    [[nodiscard]] double spectrumRangeFit() const { return spectrum_range_fit_; }
    [[nodiscard]] double spectrumSmoothing() const { return spectrum_smoothing_; }
    [[nodiscard]] bool spectrumFloorSmoothing() const { return spectrum_floor_smoothing_; }

    void setSpectrumSmoothing(double seconds)
    {
        seconds = clamp_smoothing(seconds);
        if (seconds == spectrum_smoothing_) {
            return;
        }
        spectrum_smoothing_ = seconds;
        QSettings().setValue(settings::kSpectrumSmoothing, seconds);
        emit spectrumSmoothingChanged();
    }

    void setSpectrumFloorSmoothing(bool on)
    {
        if (on == spectrum_floor_smoothing_) {
            return;
        }
        spectrum_floor_smoothing_ = on;
        QSettings().setValue(settings::kSpectrumFloorSmoothing, on);
        emit spectrumSmoothingChanged();
    }

    void setWaterfallContrast(double value)
    {
        value = clamp_contrast(value);
        if (value == waterfall_contrast_) {
            return;
        }
        waterfall_contrast_ = value;
        QSettings().setValue(settings::kWaterfallContrast, value);
        emit waterfallContrastChanged();
    }

    // A hand-set pivot while the automatic one is on is remembered for when
    // it goes off, and does not move the picture now.
    void setWaterfallPivot(double value)
    {
        value = clamp_pivot(value);
        if (value == manual_pivot_ && (waterfall_auto_pivot_ || value == waterfall_pivot_)) {
            return;
        }
        manual_pivot_ = value;
        QSettings().setValue(settings::kWaterfallPivot, value);
        if (!waterfall_auto_pivot_ && value != waterfall_pivot_) {
            waterfall_pivot_ = value;
            emit waterfallPivotChanged();
        }
    }

    [[nodiscard]] bool waterfallAutoPivot() const { return waterfall_auto_pivot_; }

    void setWaterfallAutoPivot(bool on)
    {
        if (on == waterfall_auto_pivot_) {
            return;
        }
        waterfall_auto_pivot_ = on;
        QSettings().setValue(settings::kWaterfallAutoPivot, on);
        follower_.reset();
        emit waterfallAutoPivotChanged();
        if (!on && waterfall_pivot_ != manual_pivot_) {
            waterfall_pivot_ = manual_pivot_;
            emit waterfallPivotChanged();
        }
    }

    // A waterfall's measured pivot, render/waterfall_scale.h auto_pivot, as
    // often as it likes. Ignored while the automatic pivot is off. Eased by
    // AutoPivotFollower on the time between offers, and never persisted.
    Q_INVOKABLE void offerAutoPivot(double pivot)
    {
        if (!waterfall_auto_pivot_ || !(pivot == pivot)) {
            return;
        }
        const double seconds = offer_clock_.isValid() ? offer_clock_.restart() / 1000.0 : 0.0;
        if (!offer_clock_.isValid()) {
            offer_clock_.start();
        }
        offerAutoPivotAfter(pivot, seconds);
    }

    // offerAutoPivot with the time since the last offer given, for tests.
    void offerAutoPivotAfter(double pivot, double seconds)
    {
        if (!waterfall_auto_pivot_ || !(pivot == pivot)) {
            return;
        }
        const double next = clamp_pivot(follower_.step(static_cast<float>(pivot), seconds));
        if (next != waterfall_pivot_) {
            waterfall_pivot_ = next;
            emit waterfallPivotChanged();
        }
    }

    void setSpectrumRangeFit(double value)
    {
        value = clamp_fit(value);
        if (value == spectrum_range_fit_) {
            return;
        }
        spectrum_range_fit_ = value;
        QSettings().setValue(settings::kSpectrumRangeFit, value);
        emit spectrumRangeFitChanged();
    }

    [[nodiscard]] bool floorPinned() const { return pins_.floor_pinned; }
    [[nodiscard]] double floorDb() const { return pins_.floor_db; }
    [[nodiscard]] bool ceilingPinned() const { return pins_.ceiling_pinned; }
    [[nodiscard]] double ceilingDb() const { return pins_.ceiling_db; }
    [[nodiscard]] const ScalePins& pins() const { return pins_; }

    // For the curve drawn in the settings panel: where x on the map, 0 to 1,
    // lands under the contrast and pivot in force, and the waterfall's colour
    // at a point of the map.
    Q_INVOKABLE double curveAt(double x) const
    {
        return apply_curve(curve_for(static_cast<float>(waterfall_contrast_),
                                     static_cast<float>(waterfall_pivot_)),
                           static_cast<float>(x));
    }

    Q_INVOKABLE QColor paletteAt(double x) const
    {
        const colour_map::Srgb8 c = colour_map::colour_at(static_cast<float>(x));
        return QColor(c.r, c.g, c.b);
    }

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
    void waterfallContrastChanged();
    void waterfallPivotChanged();
    void waterfallAutoPivotChanged();
    void spectrumRangeFitChanged();
    void spectrumSmoothingChanged();
    void captionZoomChanged();

private:
    [[nodiscard]] static double clamp_caption_zoom(double value)
    {
        return value == value ? std::clamp(value, kMinCaptionZoom, kMaxCaptionZoom)
                              : kDefaultCaptionZoom;
    }

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

    // A hand-edited registry value outside 0 to 1, or not a number, reads as
    // the nearest end rather than extrapolating the blend past either look.
    [[nodiscard]] static double clamp_fit(double value)
    {
        return value > 0.0 ? std::min(value, 1.0) : 0.0;
    }

    // The contrast runs on past tight to the hot ends; see HistoryLevels::ends.
    [[nodiscard]] static double clamp_contrast(double value)
    {
        return value > 0.0 ? std::min(value, static_cast<double>(kMaxWaterfallContrast))
                           : 0.0;
    }

    [[nodiscard]] static double clamp_pivot(double value)
    {
        return value == value ? std::clamp(value, static_cast<double>(kMinCurvePivot),
                                           static_cast<double>(kMaxCurvePivot))
                              : static_cast<double>(kDefaultCurvePivot);
    }

    [[nodiscard]] static double clamp_smoothing(double seconds)
    {
        return seconds > 0.0 ? std::min(seconds, kMaxSmoothDecaySeconds) : 0.0;
    }

    ScalePins pins_;
    double waterfall_contrast_ = 0.0;
    double waterfall_pivot_ = kDefaultCurvePivot;
    double manual_pivot_ = kDefaultCurvePivot;
    bool waterfall_auto_pivot_ = false;
    AutoPivotFollower follower_;
    QElapsedTimer offer_clock_;
    double spectrum_range_fit_ = 0.0;
    double spectrum_smoothing_ = kDefaultSmoothDecaySeconds;
    bool spectrum_floor_smoothing_ = true;
    double caption_zoom_ = kDefaultCaptionZoom;
};

}  // namespace revenant::ui
