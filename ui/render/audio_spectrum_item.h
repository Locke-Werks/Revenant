// The audio spectrum strip under the detail view: one receiver's demodulated
// audio after the notch and noise reduction, from 0 Hz at the left to the
// audio bandwidth at the right, filled under a trace in the passband's colour.
//
// The passband display cannot show the notch in FM, nor the noise reduction
// in any mode, because it is taken ahead of the demodulator. This strip is
// what the listener gets, so a notch reads here as a hole at its audio
// frequency and NR as a floor that drops. Each manual notch is drawn as a
// thin tick at its audio frequency.
//
// The row rides on the passband frame, PassbandFrame::audio_power_db, and is
// only there while the receiver's VrxParams::display_audio_spectrum is set.
// The arithmetic is render/audio_spectrum_map.h.

#pragma once

#include <vector>

#include <QQuickItem>
#include <QVariantList>
#include <QtQmlIntegration>

#include "models/engine_link.h"
#include "render/audio_spectrum_map.h"
#include "render/spectrum_item.h"

namespace revenant::ui {

class AudioSpectrumItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(revenant::ui::EngineLink* link READ link WRITE setLink NOTIFY linkChanged)

    // The highest audio frequency shown, normally the receiver's audio
    // bandwidth. Zero or above the frame's Nyquist shows up to the Nyquist.
    Q_PROPERTY(double maxHz READ maxHz WRITE setMaxHz NOTIFY maxHzChanged)

    // Audio hertz of each notch tick. Empty takes the frame's own
    // audio_notch_hz, which is every enabled manual notch.
    Q_PROPERTY(QVariantList notchHz READ notchHz WRITE setNotchHz NOTIFY notchHzChanged)

    // True while frames carry an audio row, so the strip can hide itself.
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)

    // The shown top in hertz, for an axis label.
    Q_PROPERTY(double topHz READ topHz NOTIFY activeChanged)

public:
    explicit AudioSpectrumItem(QQuickItem* parent = nullptr);

    [[nodiscard]] EngineLink* link() const { return link_; }
    void setLink(EngineLink* link);

    [[nodiscard]] double maxHz() const { return max_hz_; }
    void setMaxHz(double hz);

    [[nodiscard]] QVariantList notchHz() const { return notch_hz_; }
    void setNotchHz(const QVariantList& hz);

    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] double topHz() const { return top_hz_; }

signals:
    void linkChanged();
    void maxHzChanged();
    void notchHzChanged();
    void activeChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;

private:
    void takeFrame();
    void rebuild();
    void setActive(bool active, double top_hz);

    EngineLink* link_ = nullptr;
    double max_hz_ = 0.0;
    QVariantList notch_hz_;
    bool active_ = false;
    double top_hz_ = 0.0;

    std::vector<float> row_;
    double bin_hz_ = 0.0;
    std::vector<double> frame_ticks_;
    AudioStripScale scale_;
    std::vector<float> scale_scratch_;

    std::vector<float> columns_;
    std::vector<float> levels_;
    std::vector<OverlayQuad> tick_quads_;
};

}  // namespace revenant::ui
