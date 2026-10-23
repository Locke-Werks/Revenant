// HDR output for the main window, detected at launch.
//
// WHAT IT DOES. Asks Qt Quick for an scRGB swapchain on the main window, and
// after the first frame reads back what it got. Qt checks the request against
// the display the window is on and falls back to SDR when that display is not
// in HDR mode, so the request is the detection: no flag, nothing to set.
//
// WHY THE WHOLE WINDOW GOES THROUGH A LAYER. An scRGB swapchain takes linear
// light, and everything in Qt Quick, its controls and text included, writes
// sRGB-encoded colour. Left alone the window comes out washed out, its near
// black background a lifted slate grey; seen on 2026-10-22. So while HDR is
// on, the root item renders into a half-float layer exactly as it would to an
// SDR screen, and one pass (shaders/hdr_composite.frag) decodes it to linear
// and scales it to the SDR white level Windows is set to. The UI then matches
// every other SDR window on the desktop, and the span's colour map, the one
// thing drawn past 1.0, is brighter than that white. render/hdr_map.h has how.
//
// A separate HDR child window for the displays was considered and dropped: a
// native child draws over everything in its parent, and the span's plates,
// pins, cards and scale numbers are drawn over the displays.
//
// AT LAUNCH ONLY. Moving the window to another screen, or switching Windows
// HDR on or off, takes effect at the next launch. The setting turns it off
// entirely, also at the next launch, since the swapchain format is chosen when
// the window first renders.
//
// Only the main window. The receiver window and the burst window stay SDR,
// which an SDR swapchain on an HDR desktop shows correctly.

#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>

class QQmlEngine;

namespace revenant::ui {

class HdrOutput : public QObject {
    Q_OBJECT

    // The setting, which applies at the next launch.
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    // Whether this launch's main window has an scRGB swapchain.
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(double sdrWhiteNits READ sdrWhiteNits NOTIFY activeChanged)
    Q_PROPERTY(double peakNits READ peakNits NOTIFY activeChanged)
    // What the composite pass multiplies by: SDR white over scRGB's 80 nits.
    Q_PROPERTY(double compositeScale READ compositeScale NOTIFY activeChanged)

public:
    explicit HdrOutput(QObject* parent = nullptr);

    [[nodiscard]] bool enabled() const { return enabled_; }
    void setEnabled(bool enabled);

    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] double sdrWhiteNits() const { return sdr_white_; }
    [[nodiscard]] double peakNits() const { return peak_; }
    [[nodiscard]] double compositeScale() const { return active_ ? sdr_white_ / 80.0 : 1.0; }

    // Before the QML is loaded, since the main window renders its first frame
    // while it loads. Does nothing when the setting is off, when the platform
    // is not the Windows one, which a smoke run's offscreen platform is not,
    // or when QSG_RHI_HDR is already set by hand.
    void prepare();

    // Straight after loading, with the main window.
    void attach(QQuickWindow* window, QQmlEngine* engine);

signals:
    void enabledChanged();
    void activeChanged();

private:
    bool setLayer(bool on);
    void confirm(bool scrgb, double sdr_white, double peak);

    QPointer<QQuickWindow> window_;
    // qml/HdrStage.qml, which the window's content is moved under.
    QPointer<QQuickItem> stage_;
    QQmlEngine* engine_ = nullptr;
    bool enabled_ = true;
    bool prepared_ = false;
    bool active_ = false;
    double sdr_white_ = 80.0;
    double peak_ = 80.0;
};

}  // namespace revenant::ui
