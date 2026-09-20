// The VFO detail display: one receiver's passband, with its filter edges
// drawn over it and draggable.
//
// WHAT THIS ITEM IS DRAWING, AND WHY THE TWO THINGS HAVE TO AGREE
//
// Underneath is the engine's per-receiver passband spectrum, which is the
// receiver's own complex baseband transformed. Nothing divides the fine
// filter's response out of it, deliberately: the skirts ARE the feature,
// because a filter parked on a signal is judged by where its edges fall
// against the signal's.
//
// Over it are two rules, at the filter's low and high edges, and the fill
// between them. The whole point of the display is that those rules land on
// the same pixels as the skirts in the picture, so the two must be placed
// from ONE axis. They are: PassbandFrame carries its own geometry, bin zero
// is the frequency the fine stage mixed to DC as an exact rational, and both
// the trace and the rules go through EngineLink::passbandFractionAtOffset.
//
// That is what makes CW right with no CW anywhere in this file. On that one
// mode the fine stage translates the carrier to the operator's sidetone
// rather than to DC, so the frame's axis is a pitch below the receiver's
// centre. An item that derived the axis from VrxParams::center would draw
// the filter one pitch out of place on CW and correctly on the other seven,
// which is the shape of a bug nobody finds.
//
// THE MAPPING IS FROZEN FOR THE LENGTH OF A DRAG, AND THAT IS NOT AN
// OPTIMISATION
//
// The pane's span is the demodulation rate, and the demodulation rate is
// derived from the passband. So widening the filter widens the pane,
// rescales the axis, and makes the handle jump backwards out from under the
// pointer, which is unusable. The pixel-to-hertz mapping is therefore taken
// once at drag start and held for the whole gesture. Frames arriving
// mid-drag are drawn into the frozen mapping BY THEIR OWN AXIS, so a
// narrower frame letterboxes and a wider one is cropped; neither is
// stretched, because a stretched frame is a picture claiming a signal is
// somewhere it is not. The mapping then eases back to the live one over
// kRescaleMs, so the operator sees the span change rather than a jump.
//
// THE EASE IS ARMED AT THE RELEASE AND STARTED BY THE FRAME THAT MOVES THE
// SPAN, WHICH IS NOT THE SAME MOMENT. This paragraph used to say "on
// release", and the code did too, which is why the easing never ran for the
// case it was written for: at the release the width change has only been
// queued, so the span is still the one the pane already had and the
// comparison found nothing to animate. See kRescaleArmMs.
//
// WHY THE DISPLAY DOES NOT WAIT FOR THE ENGINE
//
// A drag redraws from EngineLink's own request the instant the pointer
// moves. What the engine granted arrives a round trip later and is drawn as
// a second, dimmer pair of rules whenever it differs, which is the channel
// clamp made visible. See ui/models/receiver_link.cpp.

#pragma once

#include <cstdint>
#include <vector>

#include <QColor>
#include <QElapsedTimer>
#include <QQuickItem>
#include <QString>
#include <QTimer>
#include <QtQmlIntegration>

#include "models/engine_link.h"
#include "render/spectrum_item.h"
#include "render/spectrum_scale.h"

class QMouseEvent;
class QHoverEvent;
class QKeyEvent;

namespace revenant::ui {

// How far either side of an edge's rule counts as grabbing it, in logical
// pixels. Eight is the ordinary slop a splitter handle uses and is wide
// enough to hit with a trackpad without making the fill between two close
// edges unreachable.
inline constexpr double kEdgeGrabSlackPx = 8.0;

// How long the axis takes to ease back to the live span after a drag that
// changed it. Long enough to read as a movement rather than a jump, short
// enough not to be waited on.
inline constexpr int kRescaleMs = 150;

// How long after a gesture the pane keeps waiting for the span to move
// before deciding it is not going to.
//
// The easing cannot start at the release. The width change is queued there,
// not applied: EngineLink holds it for the length of the gesture, the
// supervisor thread makes the call afterwards, and the new demodulation
// rate arrives on a later passband frame. So the release ARMS the easing
// and the frame that carries a different span starts it.
//
// Two seconds covers the supervisor's wake, a round trip and a frame with
// room to spare, and it is short enough that an arm which never fires,
// which is every drag that stayed inside one rate, cannot still be sitting
// there when some unrelated change moves the span later. Nothing is drawn
// differently while armed; the pane is already on the live axis.
inline constexpr int kRescaleArmMs = 2'000;

// What a press landed on.
enum class PassbandGrab : std::uint8_t {
    None,
    LowEdge,
    HighEdge,

    // The fill between the edges: drags both by the same amount, holding
    // the width and moving the offset. This is an operator chasing a
    // drifting signal, and it is not a retune.
    Band,
};

class PassbandItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(revenant::ui::EngineLink* link READ link WRITE setLink NOTIFY linkChanged)

    // True while an edge or the band is being dragged, so the window can
    // show the readout and suppress anything that would move the pane.
    Q_PROPERTY(bool dragging READ dragging NOTIFY dragChanged)

    // The live hertz readout. Both edges and the width while a drag is
    // running, and what the engine granted when one is not, so the strip
    // never goes blank and never shows a stale drag.
    Q_PROPERTY(QString readout READ readout NOTIFY readoutChanged)

    // Which edge the keyboard moves: "low", "high" or "both". Written by
    // QML as well as by a click, so a control can show it.
    Q_PROPERTY(QString selectedEdge READ selectedEdge WRITE setSelectedEdge
                   NOTIFY selectedEdgeChanged)

    // True when the edge being dragged has been stopped by the channel
    // rather than by the pointer, so the rule can be drawn in the warn
    // colour and the readout can say which edge and at what.
    Q_PROPERTY(bool atLimit READ atLimit NOTIFY readoutChanged)

public:
    explicit PassbandItem(QQuickItem* parent = nullptr);

    [[nodiscard]] EngineLink* link() const { return link_; }
    void setLink(EngineLink* link);

    [[nodiscard]] bool dragging() const { return grab_ != PassbandGrab::None; }
    [[nodiscard]] QString readout() const { return readout_; }
    [[nodiscard]] bool atLimit() const { return at_limit_; }

    [[nodiscard]] QString selectedEdge() const;
    void setSelectedEdge(const QString& edge);

    // The keyboard equivalent, also reachable from QML so a toolbar button
    // can do the same thing. step_hz is signed and is applied to whichever
    // edge the selection names.
    Q_INVOKABLE void nudgeSelectedEdge(int step_hz);

    // Puts the mode's default passband back and clears the selection.
    Q_INVOKABLE void resetPassband();

protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

signals:
    void linkChanged();
    void dragChanged();
    void readoutChanged();
    void selectedEdgeChanged();

private slots:
    void takeFrame();
    void takeStatus();
    void onConnectionChanged();

private:
    // Where the pane's axis currently is, in hertz from the receiver's
    // centre. Live when nothing is dragging, frozen during a drag, and
    // interpolated between the two while the release animation runs.
    struct Axis {
        double low_hz = 0.0;
        double high_hz = 0.0;

        [[nodiscard]] double span_hz() const { return high_hz - low_hz; }
        [[nodiscard]] bool valid() const { return high_hz > low_hz; }
    };

    [[nodiscard]] Axis liveAxis() const;
    [[nodiscard]] Axis drawnAxis() const;

    [[nodiscard]] double pixelAtOffset(double offset_hz) const;
    [[nodiscard]] double offsetAtPixel(double x_px) const;

    // What a press at this x would grab, given where the edges are drawn.
    [[nodiscard]] PassbandGrab grabAt(double x_px) const;

    void rebuildQuads();
    void rebuildReadout();
    void resizeColumns(int columns, std::size_t bins);
    [[nodiscard]] int deviceColumns() const;

    // Arms the ease at the end of a gesture; starts it when a frame brings
    // a span that actually differs. See kRescaleArmMs.
    void armRescale();
    void tryRescale();
    void stepRescale();

    EngineLink* link_ = nullptr;

    // The reduced trace, in the same shape SpectrumItem keeps one: one
    // column per device pixel, peak-reduced, normalised at paint time.
    std::vector<float> columns_;
    std::vector<float> levels_;
    std::size_t reduced_bins_ = 0;
    MapEnds ends_{};
    bool have_frame_ = false;

    // The frame's own axis, in hertz from the receiver's centre, taken when
    // the frame arrived. Held rather than recomputed so a frame drawn into
    // a frozen mapping is placed by the axis it was measured on.
    Axis frame_axis_{};

    // Two batches rather than one, because the fill has to composite UNDER
    // the trace and the rules OVER it, and one geometry node is one draw
    // call at one depth. Sorting them apart by width at paint time was the
    // first shape of this and got a very narrow band drawn as a rule.
    std::vector<OverlayQuad> fill_quads_;
    std::vector<OverlayQuad> rule_quads_;
    QString readout_;
    bool at_limit_ = false;

    PassbandGrab grab_ = PassbandGrab::None;
    PassbandGrab hovered_ = PassbandGrab::None;

    // Where the pointer went down and what the edges were then, so a drag
    // is applied as a delta from the press rather than as an absolute
    // position. Absolute would make the filter jump to the pointer on the
    // first pixel of movement.
    double press_x_ = 0.0;
    int press_low_ = 0;
    int press_high_ = 0;

    // The mapping held for the length of a gesture, and the one being eased
    // away from afterwards.
    Axis frozen_{};
    Axis rescale_from_{};
    QTimer rescale_tick_;

    // The gesture ended and the span has not moved yet. See kRescaleArmMs.
    bool rescale_armed_ = false;
    bool rescaling_ = false;

    // Times whichever of the two phases is running: the wait while armed,
    // then the ease once it starts. One clock rather than two, because the
    // phases never overlap and tryRescale restarts it at the handover.
    QElapsedTimer rescale_clock_;

    // Which edge the keyboard moves. Both means the passband pans.
    PassbandGrab selection_ = PassbandGrab::Band;
};

}  // namespace revenant::ui
