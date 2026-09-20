#include "render/waterfall_item.h"

#include <algorithm>
#include <cstdint>

#include <QColor>
#include <QPainter>
#include <QRect>

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
    }
    filled_rows_ = 0;
    write_row_ = 0;
    emit linkChanged();
    update();
}

void WaterfallItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        rebuild(static_cast<int>(newGeometry.width()), static_cast<int>(newGeometry.height()),
                reduced_bins_);
        update();
    }
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

    const int wide = std::max(static_cast<int>(width()), 1);
    const int tall = std::max(static_cast<int>(height()), 1);
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

    painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter->drawImage(QRect(0, 0, wide, upper), history_,
                       QRect(0, write_row_, wide, upper));
    if (write_row_ > 0) {
        painter->drawImage(QRect(0, upper, wide, write_row_), history_,
                           QRect(0, 0, wide, write_row_));
    }
}

}  // namespace revenant::ui
