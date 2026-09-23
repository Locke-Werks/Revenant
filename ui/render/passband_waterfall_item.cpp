#include "render/passband_waterfall_item.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGNode>
#include <QSGRectangleNode>
#include <QSGTexture>

#include "render/history_resize.h"
#include "render/spectrum_scale.h"

namespace revenant::ui {
namespace {

// Rows per uploaded tile, as on the span waterfall and for the same trade.
constexpr int kTileRows = 64;

const QColor kBackground(colour_map::kBottom.r, colour_map::kBottom.g, colour_map::kBottom.b);

// Owns the tile textures, which may only be destroyed on the render thread;
// see the same class in render/waterfall_item.cpp.
class PassbandWaterfallNode : public QSGNode {
public:
    PassbandWaterfallNode() = default;
    PassbandWaterfallNode(const PassbandWaterfallNode&) = delete;
    PassbandWaterfallNode& operator=(const PassbandWaterfallNode&) = delete;
    PassbandWaterfallNode(PassbandWaterfallNode&&) = delete;
    PassbandWaterfallNode& operator=(PassbandWaterfallNode&&) = delete;

    ~PassbandWaterfallNode() override
    {
        while (QSGNode* child = firstChild()) {
            removeChildNode(child);
            delete child;
        }
        for (QSGTexture* texture : textures) {
            delete texture;
        }
    }

    QSGRectangleNode* background = nullptr;
    QSGNode* tiles = nullptr;
    std::vector<QSGTexture*> textures;
};

}  // namespace

PassbandWaterfallItem::PassbandWaterfallItem(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

void PassbandWaterfallItem::setLink(EngineLink* link)
{
    if (link_ == link) {
        return;
    }
    if (link_ != nullptr) {
        disconnect(link_, nullptr, this, nullptr);
    }
    link_ = link;
    if (link_ != nullptr) {
        connect(link_, &EngineLink::passbandChanged, this, &PassbandWaterfallItem::takeFrame);
        connect(link_, &EngineLink::connectionChanged, this,
                &PassbandWaterfallItem::onConnectionChanged);
    }
    clearRows();
    emit linkChanged();
    update();
}

void PassbandWaterfallItem::onConnectionChanged()
{
    // A new engine, or none: the receiver the rows came from is gone either
    // way, and a history under the next one's axis would be somebody else's.
    clearRows();
    update();
}

QSize PassbandWaterfallItem::deviceSize() const
{
    const QQuickWindow* const host = window();
    const qreal ratio = host == nullptr ? 1.0 : host->effectiveDevicePixelRatio();
    return QSize(std::max(static_cast<int>(std::lround(width() * ratio)), 1),
                 std::max(static_cast<int>(std::lround(height() * ratio)), 1));
}

int PassbandWaterfallItem::readRow() const
{
    const int tall = history_.height();
    return tall <= 0 ? 0 : (write_row_ + 1) % tall;
}

void PassbandWaterfallItem::rebuild(int columns, int rows)
{
    const int wide = std::max(columns, 1);
    const int tall = std::max(rows, 1);
    history_ = QImage(wide, tall, QImage::Format_RGBX8888);
    columns_.assign(static_cast<std::size_t>(wide), kSpectrumFloorDb);
    axis_ = {};
    clearRows();
}

void PassbandWaterfallItem::clearRows()
{
    if (!history_.isNull()) {
        history_.fill(kBackground);
    }
    write_row_ = std::max(history_.height() - 1, 0);
    filled_rows_ = 0;
    const int tiles = (history_.height() + kTileRows - 1) / kTileRows;
    tile_dirty_.assign(static_cast<std::size_t>(std::max(tiles, 0)), std::uint8_t{1});
    emit historyChanged();
}

void PassbandWaterfallItem::resizeRows(int rows)
{
    const int wide = history_.width();
    const int old_tall = history_.height();
    const int tall = std::max(rows, 1);
    const HistoryRemap plan = plan_history_resize(old_tall, write_row_, filled_rows_, tall);

    QImage fresh(wide, tall, QImage::Format_RGBX8888);
    fresh.fill(kBackground);
    const auto row_bytes = static_cast<std::size_t>(wide) * sizeof(std::uint32_t);
    for (int row = 0; row < plan.rows_kept; ++row) {
        std::memcpy(fresh.scanLine(row),
                    history_.constScanLine(history_source_row(plan, old_tall, row)), row_bytes);
    }
    history_ = std::move(fresh);
    write_row_ = plan.write_row;
    filled_rows_ = plan.rows_kept;
    const int tiles = (tall + kTileRows - 1) / kTileRows;
    tile_dirty_.assign(static_cast<std::size_t>(tiles), std::uint8_t{1});
    emit historyChanged();
}

void PassbandWaterfallItem::shiftRows(int pixels)
{
    const int wide = history_.width();
    if (pixels == 0 || wide <= 0) {
        return;
    }
    const auto background = static_cast<std::uint32_t>(
        0xFF000000U | (static_cast<std::uint32_t>(kBackground.blue()) << 16U) |
        (static_cast<std::uint32_t>(kBackground.green()) << 8U) |
        static_cast<std::uint32_t>(kBackground.red()));
    const int moved = std::min(std::abs(pixels), wide);
    const auto keep = static_cast<std::size_t>(wide - moved);

    for (int row = 0; row < history_.height(); ++row) {
        auto* line = reinterpret_cast<std::uint32_t*>(history_.scanLine(row));
        if (pixels > 0) {
            std::memmove(line + moved, line, keep * sizeof(std::uint32_t));
            std::fill(line, line + moved, background);
        } else {
            std::memmove(line, line + moved, keep * sizeof(std::uint32_t));
            std::fill(line + keep, line + wide, background);
        }
    }
    std::fill(tile_dirty_.begin(), tile_dirty_.end(), std::uint8_t{1});
}

void PassbandWaterfallItem::geometryChange(const QRectF& new_geometry,
                                           const QRectF& old_geometry)
{
    QQuickItem::geometryChange(new_geometry, old_geometry);
    if (new_geometry.size() == old_geometry.size()) {
        return;
    }
    const QSize wanted = deviceSize();
    if (!history_.isNull() && wanted.width() == history_.width() &&
        wanted.height() != history_.height()) {
        resizeRows(wanted.height());
    } else if (wanted != history_.size()) {
        rebuild(wanted.width(), wanted.height());
    }
    update();
}

void PassbandWaterfallItem::takeFrame()
{
    if (link_ == nullptr) {
        return;
    }
    const rpc::PassbandFrame& frame = link_->passbandFrame();
    if (frame.power_db.empty()) {
        // The link empties the frame when the receiver changes identity,
        // mode or frequency. The history is not thrown away for that: the
        // next frame says by its own axis whether the rows still line up.
        return;
    }

    const QSize wanted = deviceSize();
    if (history_.size() != wanted) {
        if (!history_.isNull() && wanted.width() == history_.width()) {
            resizeRows(wanted.height());
        } else {
            rebuild(wanted.width(), wanted.height());
        }
    }
    const int wide = history_.width();
    const int tall = history_.height();

    if (frame.vrx != receiver_) {
        receiver_ = frame.vrx;
        axis_ = {};
    }

    const HistoryShift plan =
        plan_history_shift(axis_, link_->passbandFrequencyAtFraction(0.0),
                           link_->passbandFrequencyAtFraction(1.0), wide);
    if (plan.reset) {
        clearRows();
    } else {
        shiftRows(plan.shift_px);
    }
    axis_ = plan.axis;

    reduce_peak(frame.power_db, columns_);

    // No reduction headroom, for the reason render/passband_item.cpp gives:
    // a column here is a bin or two.
    const MapEnds ends = map_ends(frame.floor_db, frame.ceiling_db, 0.0F);
    const float span = ends.span_db();
    if (span <= 0.0F) {
        return;
    }

    auto* row = reinterpret_cast<std::uint32_t*>(history_.scanLine(write_row_));
    for (int c = 0; c < wide; ++c) {
        const float level = std::clamp(
            (columns_[static_cast<std::size_t>(c)] - ends.floor_db) / span, 0.0F, 1.0F);
        const Rgb rgb = colour_at(level);
        row[c] = 0xFF000000U | (static_cast<std::uint32_t>(rgb.b) << 16) |
                 (static_cast<std::uint32_t>(rgb.g) << 8) | static_cast<std::uint32_t>(rgb.r);
    }
    tile_dirty_[static_cast<std::size_t>(write_row_ / kTileRows)] = 1;
    write_row_ = (write_row_ + tall - 1) % tall;
    if (filled_rows_ < tall) {
        filled_rows_ += 1;
        emit historyChanged();
    }
    update();
}

QSGNode* PassbandWaterfallItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
    const qreal w = width();
    const qreal h = height();
    if (w <= 0.0 || h <= 0.0 || history_.isNull()) {
        delete old_node;
        return nullptr;
    }

    auto* node = static_cast<PassbandWaterfallNode*>(old_node);
    if (node == nullptr) {
        node = new PassbandWaterfallNode;
        node->background = window()->createRectangleNode();
        node->background->setColor(kBackground);
        node->tiles = new QSGNode;
        node->appendChildNode(node->background);
        node->appendChildNode(node->tiles);
    }
    node->background->setRect(0.0, 0.0, w, h);

    const int wide = history_.width();
    const int tall = history_.height();
    const auto tiles = static_cast<std::size_t>((tall + kTileRows - 1) / kTileRows);

    // Replaced textures are freed only after every node has let go of them;
    // see render/waterfall_item.cpp for the crash that taught this.
    std::vector<QSGTexture*> retired;
    if (node->textures.size() != tiles) {
        retired.insert(retired.end(), node->textures.begin(), node->textures.end());
        node->textures.assign(tiles, nullptr);
        std::fill(tile_dirty_.begin(), tile_dirty_.end(), std::uint8_t{1});
    }
    for (std::size_t tile = 0; tile < tiles; ++tile) {
        if (node->textures[tile] != nullptr && tile_dirty_[tile] == 0) {
            continue;
        }
        const int first = static_cast<int>(tile) * kTileRows;
        const int rows = std::min(kTileRows, tall - first);
        const QImage source(history_.constScanLine(first), wide, rows, history_.bytesPerLine(),
                            history_.format());
        retired.push_back(node->textures[tile]);
        node->textures[tile] = window()->createTextureFromImage(source.copy());
        tile_dirty_[tile] = 0;
    }

    struct Run {
        int tile;
        int source_row;
        int rows;
        int display_row;
    };
    const int read = readRow();
    std::vector<Run> runs;
    for (std::size_t tile = 0; tile < tiles; ++tile) {
        const int first = static_cast<int>(tile) * kTileRows;
        const int last = std::min(first + kTileRows, tall);
        const auto add = [&](int from, int to) {
            runs.push_back(Run{static_cast<int>(tile), from - first, to - from,
                               (from - read + tall) % tall});
        };
        if (read > first && read < last) {
            add(first, read);
            add(read, last);
        } else {
            add(first, last);
        }
    }

    while (static_cast<std::size_t>(node->tiles->childCount()) < runs.size()) {
        QSGImageNode* image = window()->createImageNode();
        image->setOwnsTexture(false);
        image->setFiltering(QSGTexture::Nearest);

        // A texture before it joins the tree; see the same line in
        // render/waterfall_item.cpp for the software renderer crash.
        image->setTexture(node->textures.front());
        node->tiles->appendChildNode(image);
    }
    while (static_cast<std::size_t>(node->tiles->childCount()) > runs.size()) {
        QSGNode* last = node->tiles->lastChild();
        node->tiles->removeChildNode(last);
        delete last;
    }
    for (std::size_t at = 0; at < runs.size(); ++at) {
        const Run& run = runs[at];
        auto* image = static_cast<QSGImageNode*>(node->tiles->childAtIndex(static_cast<int>(at)));
        image->setTexture(node->textures[static_cast<std::size_t>(run.tile)]);
        image->setSourceRect(0.0, static_cast<qreal>(run.source_row), static_cast<qreal>(wide),
                             static_cast<qreal>(run.rows));
        const qreal top = h * static_cast<qreal>(run.display_row) / static_cast<qreal>(tall);
        const qreal bottom =
            h * static_cast<qreal>(run.display_row + run.rows) / static_cast<qreal>(tall);
        image->setRect(QRectF(0.0, top, w, bottom - top));
    }

    for (QSGTexture* texture : retired) {
        delete texture;
    }
    return node;
}

}  // namespace revenant::ui
