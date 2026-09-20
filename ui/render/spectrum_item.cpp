#include "render/spectrum_item.h"

#include <algorithm>
#include <cmath>

#include <QBrush>
#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QQuickWindow>

namespace revenant::ui {
namespace {

[[nodiscard]] QColor colour_of(float level)
{
    const Rgb rgb = colour_at(level);
    return QColor(rgb.r, rgb.g, rgb.b);
}

}  // namespace

SpectrumItem::SpectrumItem(QQuickItem* parent) : QQuickPaintedItem(parent)
{
    setFillColor(QColor(8, 10, 14));

    // NO ANTIALIASING, WHICH IS A REVERSAL AND IS MEASURED
    //
    // This used to be on, with the reason that a polyline and a gradient
    // fill both alias badly on a one-pixel feature and a one-bin carrier is
    // exactly that feature. That was true while a column was a logical
    // pixel: a segment then spanned 1.25 device pixels on the display this
    // was checked on, so it had a diagonal to soften. deviceColumns() puts
    // one point per physical pixel, and a segment is now one pixel wide, so
    // there is no diagonal left. What antialiasing does to a vertical spike
    // one pixel wide is spread it over two at half the brightness, which
    // makes a narrow carrier harder to see rather than easier. Side by side
    // at four times magnification the aliased trace is the crisper of the
    // two and the peaks keep their value.
    //
    // It is also almost the entire cost of this item, which is almost the
    // entire cost of the window. At 1290 by 830 against an RTL-SDR v3 at
    // 98.1 MHz with 65536 bins, the waterfall drew 22.6 rows a second of
    // the 36.8 the engine offered with both the fill and the trace
    // antialiased, 23.3 with the fill alone aliased, and 37.0 with both
    // aliased, which is the whole of what arrives. Collapsing this item out
    // of the layout entirely also gave 37.0, so with this off the trace is
    // free. All four measured this session.
    setAntialiasing(false);

    // The fill colour is opaque and covers the item, so nothing behind this
    // shows through and the scene graph can composite it without blending.
    // The gradient under the trace is translucent over that fill, which is
    // a blend inside the item's own texture and not a claim about what is
    // underneath it. This one is here because it is true and not because it
    // bought anything: adding it moved the drawn rate from 22.7 to 22.6
    // rows a second, which is noise.
    setOpaquePainting(true);
}

void SpectrumItem::setLink(EngineLink* link)
{
    if (link_ == link) {
        return;
    }
    if (link_ != nullptr) {
        disconnect(link_, nullptr, this, nullptr);
    }
    link_ = link;
    if (link_ != nullptr) {
        // Direct, because EngineLink emits this from the Qt thread after it
        // has already taken the frame off the event loop thread. The queued
        // hop happens once, inside EngineLink, and not again per item.
        connect(link_, &EngineLink::frameChanged, this, &SpectrumItem::takeFrame);
        connect(link_, &EngineLink::connectionChanged, this,
                &SpectrumItem::onConnectionChanged);
    }
    have_frame_ = false;
    emit linkChanged();
    update();
}

void SpectrumItem::onConnectionChanged()
{
    if (link_ == nullptr || !link_->connected()) {
        // The last trace stays up on a link that went away. It is the last
        // thing the engine actually said, the window says overhead that the
        // link is gone, and blanking would take away the only reading there
        // is at exactly the moment somebody is looking at it.
        return;
    }

    // A new engine. Nothing drawn from the previous one is meaningful here:
    // this one may be on another frequency, with another span, and the axis
    // under the trace has already changed to match.
    have_frame_ = false;
    reduced_bins_ = 0;
    update();
}

void SpectrumItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.width() != oldGeometry.width()) {
        // The reduction changes with the column count, so the headroom does
        // too. Recomputed against the last frame's bin count, which is the
        // one the next frame will almost certainly have.
        resizeColumns(deviceColumns(), reduced_bins_);
        update();
    }
}

int SpectrumItem::deviceColumns() const
{
    const QQuickWindow* const host = window();
    const qreal ratio = host == nullptr ? 1.0 : host->effectiveDevicePixelRatio();
    return std::max(static_cast<int>(std::lround(width() * ratio)), 1);
}

void SpectrumItem::resizeColumns(int columns, std::size_t bins)
{
    const std::size_t wanted = static_cast<std::size_t>(std::max(columns, 1));
    columns_.assign(wanted, kSpectrumFloorDb);
    reduced_bins_ = bins;

    const std::size_t bins_per_column = bins == 0 ? 1 : std::max<std::size_t>(1, bins / wanted);
    headroom_db_ = peak_reduction_headroom_db(bins_per_column);
    emit endsChanged();
}

void SpectrumItem::takeFrame()
{
    if (link_ == nullptr) {
        return;
    }
    const rpc::SpectrumFrame& frame = link_->frame();
    if (frame.power_db.empty()) {
        return;
    }

    // Recomputed per frame rather than only on a resize, because the device
    // pixel ratio changes with no geometry change at all when the window is
    // dragged onto a display with another scale factor.
    const auto wanted = static_cast<std::size_t>(deviceColumns());
    if (columns_.size() != wanted || reduced_bins_ != frame.power_db.size()) {
        resizeColumns(static_cast<int>(wanted), frame.power_db.size());
    }

    reduce_peak(frame.power_db, columns_);
    ends_ = map_ends(frame.floor_db, frame.ceiling_db, headroom_db_);
    have_frame_ = true;

    emit endsChanged();
    update();
}

void SpectrumItem::paint(QPainter* painter)
{
    const qreal w = width();
    const qreal h = height();
    if (w <= 0.0 || h <= 0.0 || !have_frame_ || columns_.empty()) {
        return;
    }

    const float span = ends_.span_db();
    if (span <= 0.0F) {
        return;
    }

    // One point per column, top of the item at the ceiling. A value above
    // the ceiling clamps to the top edge rather than being drawn off the
    // item, which is what saturation should look like.
    QPolygonF trace;
    trace.reserve(static_cast<int>(columns_.size()) + 2);

    const qreal step = w / static_cast<qreal>(columns_.size());
    for (std::size_t c = 0; c < columns_.size(); ++c) {
        const float level = std::clamp((columns_[c] - ends_.floor_db) / span, 0.0F, 1.0F);
        const qreal x = (static_cast<qreal>(c) + 0.5) * step;
        const qreal y = h * (1.0 - static_cast<qreal>(level));
        trace.append(QPointF(x, y));
    }

    // The fill uses the same map as the waterfall, so a feature reads the
    // same colour in both displays and the eye can carry a level between
    // them. Translucent, because the trace itself has to stay legible where
    // it crosses the bright end.
    QLinearGradient gradient(0.0, 0.0, 0.0, h);
    for (int stop = 0; stop <= 8; ++stop) {
        const float level = static_cast<float>(stop) / 8.0F;
        QColor colour = colour_of(level);
        colour.setAlpha(140);
        gradient.setColorAt(1.0 - static_cast<qreal>(level), colour);
    }

    QPolygonF filled = trace;
    filled.append(QPointF(w, h));
    filled.append(QPointF(0.0, h));

    // Both aliased. The constructor has the reasoning and the measurement;
    // it is set here as well because a render hint is per painter and this
    // painter belongs to the scene graph, not to this item.
    painter->setRenderHint(QPainter::Antialiasing, false);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QBrush(gradient));
    painter->drawPolygon(filled);

    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(colour_of(1.0F), 1.0));
    painter->drawPolyline(trace);
}

}  // namespace revenant::ui
