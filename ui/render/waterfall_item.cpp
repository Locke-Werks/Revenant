#include "render/waterfall_item.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <QColor>
#include <QPainter>
#include <QQuickWindow>
#include <QRectF>

namespace revenant::ui {

WaterfallItem::WaterfallItem(QQuickItem* parent) : QQuickPaintedItem(parent)
{
    setFillColor(QColor(4, 6, 16));
    // Nearest-neighbour. The image is one pixel per column and one per row
    // by construction, so any smoothing here is resampling a picture that
    // is already at its native size, and on a one-bin carrier it turns a
    // single bright pixel into a smear that reads as bandwidth.
    setSmooth(false);
    setAntialiasing(false);
    // Every pixel of this item is written every paint, by an opaque fill
    // followed by two blits from an RGB32 image with no alpha in it, so the
    // scene graph does not need to blend the item over what is behind it.
    // Declaring that is the only way it can know.
    setOpaquePainting(true);
}

void WaterfallItem::setLink(EngineLink* link)
{
    if (link_ == link) {
        return;
    }
    if (link_ != nullptr) {
        disconnect(link_, nullptr, this, nullptr);
    }
    link_ = link;
    if (link_ != nullptr) {
        connect(link_, &EngineLink::frameChanged, this, &WaterfallItem::takeFrame);
        connect(link_, &EngineLink::connectionChanged, this,
                &WaterfallItem::onConnectionChanged);
    }
    filled_rows_ = 0;
    write_row_ = 0;
    emit linkChanged();
    update();
}

void WaterfallItem::onConnectionChanged()
{
    if (link_ == nullptr || !link_->connected()) {
        // History from a link that has just gone away is still history, and
        // it is what the engine said while it was there. It stays until the
        // next engine pushes it off the top.
        return;
    }

    // A new engine, and the axis under this item has changed with it. Every
    // stored row was drawn against the previous span, so keeping them would
    // put a signal at a frequency it was never at. That is the same reason
    // this file's header gives for discarding history on a resize, and it
    // applies harder here: a resize keeps the band and only moves the
    // pixels, where a new engine can be tuned somewhere else entirely.
    if (!history_.isNull()) {
        history_.fill(QColor(4, 6, 16));
    }
    write_row_ = 0;
    filled_rows_ = 0;
    reduced_bins_ = 0;
    update();
}

void WaterfallItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        const QSize wanted = deviceSize();
        rebuild(wanted.width(), wanted.height(), reduced_bins_);
        update();
    }
}

QSize WaterfallItem::deviceSize() const
{
    const QQuickWindow* const host = window();
    const qreal ratio = host == nullptr ? 1.0 : host->effectiveDevicePixelRatio();
    return QSize(std::max(static_cast<int>(std::lround(width() * ratio)), 1),
                 std::max(static_cast<int>(std::lround(height() * ratio)), 1));
}

void WaterfallItem::rebuild(int columns, int rows, std::size_t bins)
{
    const int wide = std::max(columns, 1);
    const int tall = std::max(rows, 1);

    // RGB32 rather than ARGB32: every pixel is opaque, and the premultiplied
    // formats cost a conversion on every blit for an alpha channel nothing
    // here varies.
    history_ = QImage(wide, tall, QImage::Format_RGB32);
    history_.fill(QColor(4, 6, 16));
    write_row_ = 0;
    filled_rows_ = 0;

    columns_.assign(static_cast<std::size_t>(wide), kSpectrumFloorDb);
    reduced_bins_ = bins;

    const std::size_t bins_per_column =
        bins == 0 ? 1 : std::max<std::size_t>(1, bins / static_cast<std::size_t>(wide));
    headroom_db_ = peak_reduction_headroom_db(bins_per_column);
    emit endsChanged();
}

void WaterfallItem::takeFrame()
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
    const QSize wanted = deviceSize();
    const int wide = wanted.width();
    const int tall = wanted.height();
    if (history_.width() != wide || history_.height() != tall ||
        reduced_bins_ != frame.power_db.size()) {
        rebuild(wide, tall, frame.power_db.size());
    }

    reduce_peak(frame.power_db, columns_);
    ends_ = map_ends(frame.floor_db, frame.ceiling_db, headroom_db_);

    const float span = ends_.span_db();
    if (span <= 0.0F) {
        return;
    }

    auto* row = reinterpret_cast<std::uint32_t*>(history_.scanLine(write_row_));
    for (int c = 0; c < wide; ++c) {
        const float level =
            std::clamp((columns_[static_cast<std::size_t>(c)] - ends_.floor_db) / span, 0.0F,
                       1.0F);
        row[c] = colour_argb_at(level);
    }

    write_row_ = (write_row_ + 1) % tall;
    filled_rows_ = std::min(filled_rows_ + 1, tall);

    emit endsChanged();
    update();
}

void WaterfallItem::paint(QPainter* painter)
{
    if (history_.isNull() || filled_rows_ == 0) {
        return;
    }

    const int tall = history_.height();
    const int wide = history_.width();

    // Two blits, because the oldest row is at the cursor and the newest is
    // the one before it. The first piece is everything from the cursor to
    // the bottom of the ring, the second is everything before the cursor,
    // and together they read top to bottom as oldest to newest.
    const int upper = tall - write_row_;

    // The destination is in the item's own coordinates, which are logical,
    // and the ring is in physical pixels, so the whole image maps onto the
    // whole item and the scale works out to one to one. The split between
    // the two pieces is placed by proportion rather than by counting rows,
    // because at a fractional device pixel ratio there is no whole logical
    // row to count: an integer split leaves a seam of background showing
    // through at the join.
    const qreal split = height() * static_cast<qreal>(upper) / static_cast<qreal>(tall);

    painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter->drawImage(QRectF(0.0, 0.0, width(), split), history_,
                       QRectF(0.0, static_cast<qreal>(write_row_), static_cast<qreal>(wide),
                              static_cast<qreal>(upper)));
    if (write_row_ > 0) {
        painter->drawImage(
            QRectF(0.0, split, width(), height() - split), history_,
            QRectF(0.0, 0.0, static_cast<qreal>(wide), static_cast<qreal>(write_row_)));
    }
}

}  // namespace revenant::ui
