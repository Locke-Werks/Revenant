#include "render/spectrum_item.h"

#include <algorithm>
#include <cmath>

#include <QBrush>
#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>

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
    // The trace is a polyline over a gradient fill, both of which alias
    // badly on a one-pixel-wide feature, and a one-bin carrier is exactly
    // that feature.
    setAntialiasing(true);
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
    }
    have_frame_ = false;
    emit linkChanged();
    update();
}

void SpectrumItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.width() != oldGeometry.width()) {
        // The reduction changes with the column count, so the headroom does
        // too. Recomputed against the last frame's bin count, which is the
        // one the next frame will almost certainly have.
        resizeColumns(static_cast<int>(newGeometry.width()), reduced_bins_);
        update();
    }
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

    const auto wanted = static_cast<std::size_t>(std::max(static_cast<int>(width()), 1));
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

    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QBrush(gradient));
    painter->drawPolygon(filled);

    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(colour_of(1.0F), 1.0));
    painter->drawPolyline(trace);
}

}  // namespace revenant::ui
