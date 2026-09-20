#include "render/passband_item.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGNode>
#include <QSGRectangleNode>

namespace revenant::ui {
namespace {

// The overlay's colours.
//
// The fill is deliberately faint. It marks the band without competing with
// the trace underneath, and the trace is what the operator is judging the
// edges against: a fill dark enough to read on its own would hide the skirt
// it is there to be lined up with.
const QColor kBandFill{96, 176, 255, 42};
const QColor kEdgeRule{128, 196, 255, 220};
const QColor kEdgeRuleActive{184, 226, 255, 255};

// What the engine granted, when it differs from what was asked. Dimmer than
// the requested rule and drawn under it, so the pair reads as "asked for
// there, got here" rather than as two filters.
const QColor kGrantedRule{255, 176, 96, 200};

// An edge the channel stopped. Not the same colour as a granted rule that
// merely differs: this one is the operator pushing against a wall, and it
// is a state they can act on by retuning rather than by pulling harder.
const QColor kLimitRule{255, 112, 96, 235};

// The tuned frequency, which is where the passband's own frame has its
// origin and is not generally in the middle of the pane: on CW the axis is
// a sidetone off, and on any mode a filter dragged off centre moves the band
// and not this. One thin rule, always drawn, because an operator reading an
// asymmetric filter needs to know which side of the carrier they are on.
const QColor kCentreRule{255, 255, 255, 90};

constexpr double kRuleWidthPx = 2.0;
constexpr double kCentreWidthPx = 1.0;

// The trace's own colour. Flat rather than the span's gradient: this display
// is one signal at a time and the gradient's job over there is to make a
// crowded band legible.
const QColor kTraceColour{150, 226, 255, 255};

[[nodiscard]] QString hertz_text(double hz)
{
    const double magnitude = std::abs(hz);
    if (magnitude >= 1000.0) {
        return QStringLiteral("%1 kHz").arg(hz / 1000.0, 0, 'f', 3);
    }
    return QStringLiteral("%1 Hz").arg(std::llround(hz));
}

[[nodiscard]] QString signed_hertz_text(double hz)
{
    const QString body = hertz_text(hz);
    return hz >= 0.0 ? QStringLiteral("+") + body : body;
}

// The trace: one line strip, one point per device column. The same shape
// SpectrumItem's TraceNode has and for the same reason, with the x range
// given explicitly because a frame drawn into a frozen mapping does not span
// the item.
class PassbandTraceNode : public QSGGeometryNode {
public:
    PassbandTraceNode() : geometry_(QSGGeometry::defaultAttributes_Point2D(), 0)
    {
        geometry_.setDrawingMode(QSGGeometry::DrawLineStrip);
        geometry_.setLineWidth(1.0F);
        material_.setColor(kTraceColour);
        setGeometry(&geometry_);
        setMaterial(&material_);
    }

    PassbandTraceNode(const PassbandTraceNode&) = delete;
    PassbandTraceNode& operator=(const PassbandTraceNode&) = delete;
    PassbandTraceNode(PassbandTraceNode&&) = delete;
    PassbandTraceNode& operator=(PassbandTraceNode&&) = delete;

    void setTrace(std::span<const float> levels, qreal left_px, qreal right_px,
                  qreal height_px)
    {
        const auto columns = static_cast<int>(levels.size());
        geometry_.allocate(columns);
        if (columns <= 0) {
            markDirty(QSGNode::DirtyGeometry);
            return;
        }

        QSGGeometry::Point2D* vertex = geometry_.vertexDataAsPoint2D();
        const qreal step = (right_px - left_px) / static_cast<qreal>(columns);
        for (int column = 0; column < columns; ++column) {
            const float level = levels[static_cast<std::size_t>(column)];
            vertex[column].set(
                static_cast<float>(left_px + (static_cast<qreal>(column) + 0.5) * step),
                static_cast<float>(height_px * (1.0 - static_cast<qreal>(level))));
        }
        markDirty(QSGNode::DirtyGeometry);
    }

private:
    QSGGeometry geometry_;
    QSGFlatColorMaterial material_;
};

class PassbandNode : public QSGNode {
public:
    QSGRectangleNode* background = nullptr;
    OverlayNode* band = nullptr;
    PassbandTraceNode* trace = nullptr;
    OverlayNode* rules = nullptr;
};

}  // namespace

PassbandItem::PassbandItem(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);

    // Focusable so the keyboard equivalent works without QML forwarding
    // every key. The pane has to be clicked or tabbed into first, which is
    // what stops an arrow key moving a filter the operator was not looking
    // at.
    setFlag(ItemIsFocusScope, true);
    setActiveFocusOnTab(true);

    rescale_tick_.setInterval(16);
    connect(&rescale_tick_, &QTimer::timeout, this, &PassbandItem::stepRescale);
}

void PassbandItem::setLink(EngineLink* link)
{
    if (link_ == link) {
        return;
    }
    if (link_ != nullptr) {
        disconnect(link_, nullptr, this, nullptr);
    }
    link_ = link;
    if (link_ != nullptr) {
        // Direct, because EngineLink emits all three from the Qt thread
        // after it has already taken the frame off the event loop thread.
        connect(link_, &EngineLink::passbandChanged, this, &PassbandItem::takeFrame);
        connect(link_, &EngineLink::receiverStatusChanged, this, &PassbandItem::takeStatus);
        connect(link_, &EngineLink::receiverChanged, this, &PassbandItem::takeStatus);
        connect(link_, &EngineLink::connectionChanged, this,
                &PassbandItem::onConnectionChanged);
    }
    have_frame_ = false;
    columns_.clear();
    levels_.clear();
    rebuildQuads();
    rebuildReadout();
    emit linkChanged();
    update();
}

QString PassbandItem::selectedEdge() const
{
    switch (selection_) {
        case PassbandGrab::LowEdge: return QStringLiteral("low");
        case PassbandGrab::HighEdge: return QStringLiteral("high");
        case PassbandGrab::Band:
        case PassbandGrab::None: break;
    }
    return QStringLiteral("both");
}

void PassbandItem::setSelectedEdge(const QString& edge)
{
    PassbandGrab wanted = PassbandGrab::Band;
    if (edge == QLatin1StringView("low")) {
        wanted = PassbandGrab::LowEdge;
    } else if (edge == QLatin1StringView("high")) {
        wanted = PassbandGrab::HighEdge;
    }
    if (wanted == selection_) {
        return;
    }
    selection_ = wanted;
    rebuildQuads();
    emit selectedEdgeChanged();
    update();
}

// ---------------------------------------------------------------------------
// The axis
// ---------------------------------------------------------------------------

PassbandItem::Axis PassbandItem::liveAxis() const
{
    if (link_ == nullptr) {
        return {};
    }
    Axis axis;
    axis.low_hz = link_->passbandOffsetAtFraction(0.0);
    axis.high_hz = link_->passbandOffsetAtFraction(1.0);
    return axis;
}

PassbandItem::Axis PassbandItem::drawnAxis() const
{
    if (grab_ != PassbandGrab::None) {
        return frozen_;
    }
    if (!rescaling_) {
        const Axis live = liveAxis();
        return live.valid() ? live : frame_axis_;
    }

    // Eased from the frozen mapping back to the live one. A cubic ease-out,
    // because the interesting part of the movement is the beginning: the
    // operator is watching the band they just widened settle into the pane.
    const Axis live = liveAxis();
    if (!live.valid()) {
        return rescale_from_;
    }
    const double elapsed = static_cast<double>(rescale_clock_.elapsed());
    const double t = std::clamp(elapsed / static_cast<double>(kRescaleMs), 0.0, 1.0);
    const double eased = 1.0 - std::pow(1.0 - t, 3.0);

    Axis axis;
    axis.low_hz = rescale_from_.low_hz + (live.low_hz - rescale_from_.low_hz) * eased;
    axis.high_hz = rescale_from_.high_hz + (live.high_hz - rescale_from_.high_hz) * eased;
    return axis;
}

double PassbandItem::pixelAtOffset(double offset_hz) const
{
    const Axis axis = drawnAxis();
    if (!axis.valid() || width() <= 0.0) {
        return 0.0;
    }
    return (offset_hz - axis.low_hz) / axis.span_hz() * width();
}

double PassbandItem::offsetAtPixel(double x_px) const
{
    const Axis axis = drawnAxis();
    if (!axis.valid() || width() <= 0.0) {
        return 0.0;
    }
    return axis.low_hz + x_px / width() * axis.span_hz();
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void PassbandItem::rebuildQuads()
{
    fill_quads_.clear();
    rule_quads_.clear();
    if (link_ == nullptr || link_->receiverId() == 0 || width() <= 0.0 || height() <= 0.0) {
        return;
    }

    // Nothing is drawn until there is an axis to draw against. Before the
    // first passband frame every offset maps to the same pixel, and the
    // rules would stack on the left edge looking like a filter of zero
    // width parked at the bottom of the band.
    if (!drawnAxis().valid()) {
        return;
    }

    const double h = height();
    const double low_px = pixelAtOffset(link_->receiverPassbandLow());
    const double high_px = pixelAtOffset(link_->receiverPassbandHigh());

    const double left = std::max(0.0, std::min(low_px, high_px));
    const double right = std::min(width(), std::max(low_px, high_px));
    if (right > left) {
        fill_quads_.push_back(OverlayQuad{QRectF(left, 0.0, right - left, h), kBandFill});
    }

    const auto rule = [&](double x, double thickness, const QColor& colour) {
        if (x < -thickness || x > width() + thickness) {
            return;
        }
        rule_quads_.push_back(
            OverlayQuad{QRectF(x - thickness / 2.0, 0.0, thickness, h), colour});
    };

    // The tuned frequency, first so the edges are drawn over it: an edge
    // sitting on the carrier is a real configuration, and the edge is the
    // one that has to stay readable.
    rule(pixelAtOffset(0.0), kCentreWidthPx, kCentreRule);

    // What the engine granted, whenever it is not what was asked for. Drawn
    // before the requested rules so the two are distinguishable where they
    // nearly coincide: the request is the one on top, because that is the
    // one the operator is moving.
    if (link_->receiverClamped()) {
        rule(pixelAtOffset(link_->receiverGrantedLow()), kRuleWidthPx, kGrantedRule);
        rule(pixelAtOffset(link_->receiverGrantedHigh()), kRuleWidthPx, kGrantedRule);
    }

    const auto rule_colour = [&](PassbandGrab which) {
        if (at_limit_ && (grab_ == which || grab_ == PassbandGrab::Band)) {
            return kLimitRule;
        }
        if (grab_ == which || (grab_ == PassbandGrab::None &&
                               (hovered_ == which || selection_ == which))) {
            return kEdgeRuleActive;
        }
        return kEdgeRule;
    };

    rule(low_px, kRuleWidthPx, rule_colour(PassbandGrab::LowEdge));
    rule(high_px, kRuleWidthPx, rule_colour(PassbandGrab::HighEdge));
}

void PassbandItem::rebuildReadout()
{
    if (link_ == nullptr || link_->receiverId() == 0) {
        if (!readout_.isEmpty()) {
            readout_.clear();
            emit readoutChanged();
        }
        return;
    }

    const int low = link_->receiverPassbandLow();
    const int high = link_->receiverPassbandHigh();
    const int limit = link_->receiverEdgeLimit();

    // Only while a drag is running, and only against the edge being moved.
    // A receiver left parked against the channel's limit is an ordinary
    // state and colouring it red forever would make the warn colour mean
    // nothing.
    const bool was_at_limit = at_limit_;
    at_limit_ = grab_ != PassbandGrab::None && limit > 0 &&
                (low <= -limit || high >= limit);

    QString text = QStringLiteral("%1 to %2   width %3")
                       .arg(signed_hertz_text(low), signed_hertz_text(high),
                            hertz_text(high - low));

    if (link_->receiverClamped()) {
        text += QStringLiteral("   granted %1 to %2")
                    .arg(signed_hertz_text(link_->receiverGrantedLow()),
                         signed_hertz_text(link_->receiverGrantedHigh()));
    }
    if (at_limit_) {
        const bool low_stuck = low <= -limit;
        text += QStringLiteral("   %1 edge is at the channel limit of %2")
                    .arg(low_stuck ? QStringLiteral("low") : QStringLiteral("high"),
                         signed_hertz_text(low_stuck ? -limit : limit));
    }

    // A width the engine has not been given yet, because giving it now
    // would restart the audio and the operator is still moving. Said out
    // loud, because the rule on screen is then in a place the audio is not.
    if (link_->receiverPending()) {
        text += QStringLiteral("   applies on release");
    }

    if (text == readout_ && was_at_limit == at_limit_) {
        return;
    }
    readout_ = std::move(text);
    emit readoutChanged();
}

int PassbandItem::deviceColumns() const
{
    const QQuickWindow* const host = window();
    const qreal ratio = host == nullptr ? 1.0 : host->effectiveDevicePixelRatio();
    return std::max(static_cast<int>(std::lround(width() * ratio)), 1);
}

void PassbandItem::resizeColumns(int columns, std::size_t bins)
{
    const auto wanted = static_cast<std::size_t>(std::max(columns, 1));
    columns_.assign(wanted, kSpectrumFloorDb);
    reduced_bins_ = bins;
}

void PassbandItem::takeFrame()
{
    if (link_ == nullptr) {
        return;
    }
    const rpc::PassbandFrame& frame = link_->passbandFrame();
    if (frame.power_db.empty()) {
        return;
    }

    const auto wanted = static_cast<std::size_t>(deviceColumns());
    if (columns_.size() != wanted || reduced_bins_ != frame.power_db.size()) {
        resizeColumns(static_cast<int>(wanted), frame.power_db.size());
    }

    reduce_peak(frame.power_db, columns_);

    // No headroom correction. peak_reduction_headroom_db exists because the
    // span reduces tens of bins into one column and the peak of a group runs
    // above the percentile the ends were measured from. A passband frame is
    // 512 bins across a pane hundreds of pixels wide, so a column is one bin
    // or two and there is nothing to correct for.
    ends_ = map_ends(frame.floor_db, frame.ceiling_db, 0.0F);

    // The axis this frame was measured on, kept so a frame arriving during a
    // drag is drawn where its own geometry says rather than stretched to
    // fill the frozen pane.
    frame_axis_ = liveAxis();
    have_frame_ = true;

    // A frame is the only thing that can move the pane's span, because the
    // axis comes off the frame's own geometry. So this is where a release
    // that armed the ease finds out whether the engine changed the rate.
    tryRescale();

    rebuildQuads();
    update();
}

void PassbandItem::takeStatus()
{
    rebuildQuads();
    rebuildReadout();
    update();
}

void PassbandItem::onConnectionChanged()
{
    have_frame_ = false;
    columns_.clear();
    levels_.clear();
    frame_axis_ = {};
    rescale_armed_ = false;
    rescaling_ = false;
    rescale_tick_.stop();
    grab_ = PassbandGrab::None;
    hovered_ = PassbandGrab::None;
    at_limit_ = false;
    rebuildQuads();
    rebuildReadout();
    emit dragChanged();
    update();
}

// ---------------------------------------------------------------------------
// The drag
// ---------------------------------------------------------------------------

PassbandGrab PassbandItem::grabAt(double x_px) const
{
    if (link_ == nullptr || link_->receiverId() == 0) {
        return PassbandGrab::None;
    }

    const double low_px = pixelAtOffset(link_->receiverPassbandLow());
    const double high_px = pixelAtOffset(link_->receiverPassbandHigh());

    // The nearer edge wins when both are in reach, which is what happens on
    // a filter narrower than two grab slacks. Without it the low edge would
    // always win and a narrow CW filter could only be widened downwards.
    const double to_low = std::abs(x_px - low_px);
    const double to_high = std::abs(x_px - high_px);
    if (to_low <= kEdgeGrabSlackPx || to_high <= kEdgeGrabSlackPx) {
        return to_low <= to_high ? PassbandGrab::LowEdge : PassbandGrab::HighEdge;
    }

    if (x_px > low_px && x_px < high_px) {
        return PassbandGrab::Band;
    }
    return PassbandGrab::None;
}

void PassbandItem::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || link_ == nullptr) {
        event->ignore();
        return;
    }

    const double x = event->position().x();
    const PassbandGrab grab = grabAt(x);
    if (grab == PassbandGrab::None) {
        // A press on bare spectrum takes the focus and nothing else. It is
        // deliberately not a retune: this pane is a few kilohertz wide and a
        // click that moved the receiver would make it impossible to click
        // into without moving what is being looked at.
        forceActiveFocus();
        event->accept();
        return;
    }

    // The mapping is taken now and held until release. See the header: the
    // pane's span is derived from the passband, so an axis that followed the
    // drag would move the handle out from under the pointer.
    //
    // A new gesture supersedes an ease that was still waiting for the last
    // one's answer: the mapping taken below is the one to come back to.
    rescale_armed_ = false;
    rescaling_ = false;
    rescale_tick_.stop();
    frozen_ = drawnAxis();
    if (!frozen_.valid()) {
        event->ignore();
        return;
    }

    grab_ = grab;
    selection_ = grab;
    press_x_ = x;
    press_low_ = link_->receiverPassbandLow();
    press_high_ = link_->receiverPassbandHigh();

    // Tells the link a gesture is running, which is what makes it send a
    // pan live and hold a width change until the release. See the note on
    // setReceiverPassband.
    link_->beginReceiverDrag();

    forceActiveFocus();
    setCursor(grab == PassbandGrab::Band ? Qt::SizeAllCursor : Qt::SizeHorCursor);
    rebuildQuads();
    rebuildReadout();
    emit dragChanged();
    emit selectedEdgeChanged();
    update();
    event->accept();
}

void PassbandItem::mouseMoveEvent(QMouseEvent* event)
{
    if (grab_ == PassbandGrab::None || link_ == nullptr) {
        event->ignore();
        return;
    }

    // A delta from the press rather than an absolute position, so the edge
    // does not jump to the pointer on the first pixel of movement. Rounded
    // to whole hertz here, so the number the operator reads is the number
    // the engine is given.
    const double delta_hz =
        (event->position().x() - press_x_) * frozen_.span_hz() / width();
    const auto delta = static_cast<int>(std::llround(delta_hz));

    switch (grab_) {
        case PassbandGrab::LowEdge:
            if (event->modifiers().testFlag(Qt::ShiftModifier)) {
                // The symmetric widen an AM or NFM operator wants: the
                // mirror edge moves by the negated delta, so the band grows
                // about the tuned frequency rather than towards it.
                link_->setReceiverPassband(press_low_ + delta, press_high_ - delta);
            } else {
                link_->setReceiverPassband(press_low_ + delta, press_high_);
            }
            break;
        case PassbandGrab::HighEdge:
            if (event->modifiers().testFlag(Qt::ShiftModifier)) {
                link_->setReceiverPassband(press_low_ - delta, press_high_ + delta);
            } else {
                link_->setReceiverPassband(press_low_, press_high_ + delta);
            }
            break;
        case PassbandGrab::Band:
            link_->setReceiverPassband(press_low_ + delta, press_high_ + delta);
            break;
        case PassbandGrab::None: break;
    }

    rebuildQuads();
    rebuildReadout();
    update();
    event->accept();
}

void PassbandItem::mouseReleaseEvent(QMouseEvent* event)
{
    if (grab_ == PassbandGrab::None) {
        event->ignore();
        return;
    }

    grab_ = PassbandGrab::None;
    at_limit_ = false;
    setCursor(Qt::ArrowCursor);
    if (link_ != nullptr) {
        // Sends whatever width change the gesture held back, which is one
        // break in the audio for the whole drag rather than one per pixel.
        link_->endReceiverDrag();
    }
    armRescale();

    rebuildQuads();
    rebuildReadout();
    emit dragChanged();
    update();
    event->accept();
}

void PassbandItem::armRescale()
{
    // ARMED AT RELEASE, STARTED WHEN THE AXIS ACTUALLY MOVES, WHICH IS NOT
    // THE SAME MOMENT AND USED TO BE TREATED AS ONE.
    //
    // The pane's span is the demodulation rate, and the demodulation rate
    // is the engine's answer. At release the width change has only been
    // queued: EngineLink holds it back for the length of the gesture, the
    // supervisor thread has still to make the call, and the new rate
    // arrives on a later passband frame. So liveAxis() here is the span the
    // pane already has, the comparison against the frozen mapping found
    // nothing had moved, and the easing this function exists for never ran
    // for the one case it exists for: the pane snapped when the new rate
    // landed.
    //
    // Starting the animation blind instead would be worse. It would ease
    // towards a span that is about to change, and the change would land
    // mid-ease.
    //
    // THE THREE FIELDS ARE CLEARED ON EVERY PATH, INCLUDING THE ONE THAT
    // GIVES UP, BECAUSE beginRescale DID AND THE REWRITE STOPPED.
    //
    // Whether that leaked a running timer was worked out rather than
    // guessed, and it did not. Both callers are gated on grab_ being a
    // real grab, grab_ is set only by mousePressEvent, and mousePressEvent
    // clears these same three fields before it sets it. Nothing between a
    // press and a release can put them back: tryRescale is the only thing
    // that starts the tick and it returns at once unless rescale_armed_ is
    // set, which only this function sets. The early return below could not
    // even be reached, because frozen_ is written in exactly one place and
    // that place refuses the gesture when the axis comes back invalid.
    //
    // That is four non-local facts holding up one early return, in a
    // function whose whole subject is a timer. Restating the clear costs
    // two statements and a stop on an already-stopped QTimer, and it makes
    // the function answer for its own state instead of the caller's.
    rescaling_ = false;
    rescale_tick_.stop();

    if (!frozen_.valid()) {
        rescale_armed_ = false;
        return;
    }

    rescale_from_ = frozen_;
    rescale_armed_ = true;
    rescale_clock_.start();

    // A pan the engine took live may already have moved the span before the
    // pointer came up, in which case there is nothing to wait for.
    tryRescale();
}

void PassbandItem::tryRescale()
{
    if (!rescale_armed_) {
        return;
    }

    const Axis live = liveAxis();
    if (!live.valid()) {
        return;
    }

    // Compared as a fraction rather than in hertz because the pane is
    // anything from a few hundred hertz to a couple of hundred kilohertz
    // wide.
    const double moved = std::abs(live.span_hz() - rescale_from_.span_hz());
    if (moved >= 0.001 * std::max(live.span_hz(), 1.0)) {
        rescale_armed_ = false;
        rescaling_ = true;
        rescale_clock_.start();
        rescale_tick_.start();
        return;
    }

    // A drag that stayed inside one demodulation rate is the ordinary case
    // and there is nothing to animate, but this cannot be told apart from
    // an answer still in flight without waiting. The deadline is what stops
    // an arm that will never fire from easing some unrelated rate change
    // minutes later out of a stale mapping.
    if (rescale_clock_.elapsed() >= kRescaleArmMs) {
        rescale_armed_ = false;
    }
}

void PassbandItem::stepRescale()
{
    if (!rescaling_) {
        rescale_tick_.stop();
        return;
    }
    if (rescale_clock_.elapsed() >= kRescaleMs) {
        rescaling_ = false;
        rescale_tick_.stop();
    }
    rebuildQuads();
    update();
}

void PassbandItem::hoverMoveEvent(QHoverEvent* event)
{
    const PassbandGrab was = hovered_;
    hovered_ = grabAt(event->position().x());
    switch (hovered_) {
        case PassbandGrab::LowEdge:
        case PassbandGrab::HighEdge: setCursor(Qt::SizeHorCursor); break;
        case PassbandGrab::Band: setCursor(Qt::SizeAllCursor); break;
        case PassbandGrab::None: setCursor(Qt::ArrowCursor); break;
    }
    if (hovered_ != was) {
        rebuildQuads();
        update();
    }
    event->accept();
}

void PassbandItem::hoverLeaveEvent(QHoverEvent* event)
{
    if (hovered_ != PassbandGrab::None) {
        hovered_ = PassbandGrab::None;
        setCursor(Qt::ArrowCursor);
        rebuildQuads();
        update();
    }
    event->accept();
}

// ---------------------------------------------------------------------------
// The keyboard
// ---------------------------------------------------------------------------

void PassbandItem::nudgeSelectedEdge(int step_hz)
{
    if (link_ == nullptr || link_->receiverId() == 0 || step_hz == 0) {
        return;
    }

    const int low = link_->receiverPassbandLow();
    const int high = link_->receiverPassbandHigh();

    switch (selection_) {
        case PassbandGrab::LowEdge: link_->setReceiverPassband(low + step_hz, high); break;
        case PassbandGrab::HighEdge: link_->setReceiverPassband(low, high + step_hz); break;
        case PassbandGrab::Band:
        case PassbandGrab::None:
            // Both edges, so the passband pans and its width is held.
            link_->nudgeReceiverPassband(step_hz);
            break;
    }

    rebuildQuads();
    rebuildReadout();
    update();
}

void PassbandItem::resetPassband()
{
    if (link_ == nullptr) {
        return;
    }
    link_->resetReceiverPassband();
    rebuildQuads();
    rebuildReadout();
    update();
}

void PassbandItem::keyPressEvent(QKeyEvent* event)
{
    if (link_ == nullptr || link_->receiverId() == 0) {
        event->ignore();
        return;
    }

    // One keystroke is one call, with no throttle. They are far too rare to
    // need one, and a throttle would swallow the last of a held repeat,
    // which is the keystroke the operator was watching for.
    const int step = event->modifiers().testFlag(Qt::ShiftModifier)  ? kPassbandCoarseStepHz
                     : event->modifiers().testFlag(Qt::ControlModifier)
                         ? kPassbandFineStepHz
                         : kPassbandStepHz;

    switch (event->key()) {
        case Qt::Key_BracketLeft: setSelectedEdge(QStringLiteral("low")); break;
        case Qt::Key_BracketRight: setSelectedEdge(QStringLiteral("high")); break;
        case Qt::Key_Backslash: setSelectedEdge(QStringLiteral("both")); break;

        case Qt::Key_Left: nudgeSelectedEdge(-step); break;
        case Qt::Key_Right: nudgeSelectedEdge(step); break;

        case Qt::Key_Up:
        case Qt::Key_Down: {
            // Widen and narrow symmetrically, whatever the selection. The
            // two arrows that do not move an edge sideways are the obvious
            // place for the gesture an AM or NFM operator reaches for most.
            const int by = event->key() == Qt::Key_Up ? step : -step;
            link_->setReceiverPassband(link_->receiverPassbandLow() - by,
                                       link_->receiverPassbandHigh() + by);
            rebuildQuads();
            rebuildReadout();
            update();
            break;
        }

        case Qt::Key_Home: resetPassband(); break;

        case Qt::Key_Escape:
            if (grab_ != PassbandGrab::None) {
                // Cancels the drag and puts back what it started from. The
                // gesture is undone rather than merely stopped, which is
                // what Escape means everywhere else.
                link_->setReceiverPassband(press_low_, press_high_);
                grab_ = PassbandGrab::None;
                at_limit_ = false;
                setCursor(Qt::ArrowCursor);
                link_->endReceiverDrag();
                armRescale();
                rebuildQuads();
                rebuildReadout();
                emit dragChanged();
                update();
            }
            break;

        default: event->ignore(); return;
    }

    event->accept();
}

// ---------------------------------------------------------------------------
// The scene graph
// ---------------------------------------------------------------------------

void PassbandItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
    QQuickItem::geometryChange(new_geometry, old_geometry);
    if (new_geometry.size() != old_geometry.size()) {
        rebuildQuads();
        update();
    }
}

QSGNode* PassbandItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
    const qreal w = width();
    const qreal h = height();
    if (w <= 0.0 || h <= 0.0) {
        delete old_node;
        return nullptr;
    }

    auto* node = static_cast<PassbandNode*>(old_node);
    if (node == nullptr) {
        node = new PassbandNode;
        node->background = window()->createRectangleNode();
        node->background->setColor(QColor(8, 10, 14));
        node->band = new OverlayNode;
        node->trace = new PassbandTraceNode;
        node->rules = new OverlayNode;

        // The band's fill under the trace, the rules over it: the operator
        // is lining an edge up against a skirt, so the edge has to be the
        // thing on top and the fill has to be the thing that does not hide
        // the skirt.
        node->appendChildNode(node->background);
        node->appendChildNode(node->band);
        node->appendChildNode(node->trace);
        node->appendChildNode(node->rules);
    }

    node->background->setRect(0.0, 0.0, w, h);

    const float span = ends_.span_db();
    if (have_frame_ && !columns_.empty() && span > 0.0F) {
        levels_.resize(columns_.size());
        for (std::size_t column = 0; column < columns_.size(); ++column) {
            levels_[column] =
                std::clamp((columns_[column] - ends_.floor_db) / span, 0.0F, 1.0F);
        }
    } else {
        levels_.clear();
    }

    // The frame is placed by ITS OWN axis rather than stretched across the
    // item. While nothing is dragging the two are the same and this is the
    // whole width; during a drag a narrower frame letterboxes and a wider
    // one runs off both sides, which is the honest picture.
    const double left = frame_axis_.valid() ? pixelAtOffset(frame_axis_.low_hz) : 0.0;
    const double right = frame_axis_.valid() ? pixelAtOffset(frame_axis_.high_hz) : w;
    node->trace->setTrace(levels_, left, right, h);

    node->band->setQuads(fill_quads_);
    node->rules->setQuads(rule_quads_);
    return node;
}

}  // namespace revenant::ui
