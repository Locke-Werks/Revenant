#include "render/passband_waterfall_item.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGNode>
#include <QSGRectangleNode>
#include <QSGTexture>

#include <QDateTime>

#include "models/frame_stats.h"
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
    TimeMarkerNode* markers = nullptr;
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

void PassbandWaterfallItem::setScaleSettings(ScaleSettings* settings)
{
    if (scale_settings_ == settings) {
        return;
    }
    if (scale_settings_ != nullptr) {
        disconnect(scale_settings_, nullptr, this, nullptr);
    }
    scale_settings_ = settings;
    if (scale_settings_ != nullptr) {
        connect(scale_settings_, &ScaleSettings::waterfallContrastChanged, this,
                &PassbandWaterfallItem::recolourAll);
        connect(scale_settings_, &ScaleSettings::waterfallPivotChanged, this,
                &PassbandWaterfallItem::recolourAll);
    }
    recolourAll();
    emit scaleSettingsChanged();
}

void PassbandWaterfallItem::setTimeMarkerSeconds(int seconds)
{
    seconds = std::max(seconds, 0);
    if (time_marker_seconds_ == seconds) {
        return;
    }
    time_marker_seconds_ = seconds;
    // The markers already drawn were spaced by the old interval, and a
    // history with two spacings in it reads as a stall.
    marks_.clear();
    marker_labels_.clear();
    emit timeMarkerSecondsChanged();
    update();
}

void PassbandWaterfallItem::setSplit(bool split)
{
    if (split_ == split) {
        return;
    }
    split_ = split;
    emit splitChanged();
}

void PassbandWaterfallItem::takeSplitRow(std::uint64_t sequence, std::span<const float> power_db)
{
    split_row_.assign(power_db.begin(), power_db.end());
    split_sequence_ = sequence;
    split_held_ = true;
}

void PassbandWaterfallItem::recolourAll()
{
    const int wide = history_.width();
    const int tall = history_.height();
    if (history_.isNull() ||
        codes_.size() != static_cast<std::size_t>(wide) * static_cast<std::size_t>(tall)) {
        return;
    }
    const float contrast = contrastInForce();
    const float pivot = pivotInForce();
    for (int row = 0; row < tall; ++row) {
        colour_passband_row(
            codes_.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(wide), wide,
            row_scales_[static_cast<std::size_t>(row)], contrast, pivot,
            reinterpret_cast<std::uint32_t*>(history_.scanLine(row)));
    }
    std::fill(tile_dirty_.begin(), tile_dirty_.end(), std::uint8_t{1});
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
    split_columns_.assign(static_cast<std::size_t>(wide), kSpectrumFloorDb);
    codes_.assign(static_cast<std::size_t>(wide) * static_cast<std::size_t>(tall), kNoLevel);
    row_scales_.assign(static_cast<std::size_t>(tall), PassbandRowScale{});
    marks_.assign(tall);
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
    std::fill(codes_.begin(), codes_.end(), kNoLevel);
    marks_.clear();
    marker_labels_.clear();
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
    const auto row_cells = static_cast<std::size_t>(wide);
    std::vector<std::uint16_t> codes(row_cells * static_cast<std::size_t>(tall), kNoLevel);
    std::vector<PassbandRowScale> scales(static_cast<std::size_t>(tall), PassbandRowScale{});
    const auto row_bytes = row_cells * sizeof(std::uint32_t);
    for (int row = 0; row < plan.rows_kept; ++row) {
        const int from = history_source_row(plan, old_tall, row);
        std::memcpy(fresh.scanLine(row), history_.constScanLine(from), row_bytes);
        std::copy_n(codes_.begin() + static_cast<std::ptrdiff_t>(row_cells * static_cast<std::size_t>(from)),
                    row_cells,
                    codes.begin() + static_cast<std::ptrdiff_t>(row_cells * static_cast<std::size_t>(row)));
        scales[static_cast<std::size_t>(row)] = row_scales_[static_cast<std::size_t>(from)];
    }
    marks_.remap(plan, old_tall, tall);
    history_ = std::move(fresh);
    codes_ = std::move(codes);
    row_scales_ = std::move(scales);
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
    for (int row = 0; row < history_.height(); ++row) {
        shift_row(reinterpret_cast<std::uint32_t*>(history_.scanLine(row)), wide, pixels,
                  background);
        shift_row(codes_.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(wide),
                  wide, pixels, kNoLevel);
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
    const FrameCost cost(FrameItem::PassbandWaterfall, FramePhase::Take);
    if (link_ == nullptr) {
        return;
    }
    const rpc::PassbandFrame& frame = link_->passbandFrame();
    if (frame.power_db.empty()) {
        // The link empties the frame when the receiver changes identity,
        // mode or frequency. The history is not thrown away for that: the
        // next frame says by its own axis whether the rows still line up.
        //
        // A pane with no receiver at all is the exception. The rows are the
        // band the last receiver sat in, and the next one tuned into the same
        // stretch of it would join them with no mark where the time went.
        if (link_->receiverId() == 0 && axis_.valid()) {
            axis_ = {};
            clearRows();
            update();
        }
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

    // NOT RESET ON A NEW RECEIVER ID. A width change the engine will not take
    // in place, and every mode change, rebuilds the pane's receiver under a
    // new id, and this used to start a fresh history on each. The rows are the
    // display tap, which carries none of the receiver's filter, in absolute
    // hertz, so they describe the band rather than the receiver: the axis
    // below is the whole test of whether they still line up, and a width
    // dragged inside a rung leaves it exactly where it was.
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

    // The right half from the processed row, when one came for this frame.
    // Reduced over the whole width like the frame, so both halves put every
    // hertz in the same column.
    const bool split_now = split_ && split_held_ && split_sequence_ == frame.sequence &&
                           split_row_.size() == frame.power_db.size();
    if (split_now) {
        reduce_peak(split_row_, split_columns_);
        const auto half = static_cast<std::ptrdiff_t>(wide / 2);
        std::copy(split_columns_.begin() + half, split_columns_.end(), columns_.begin() + half);
    }

    // No reduction headroom, for the reason render/passband_item.cpp gives:
    // a column here is a bin or two. The ends are the frame's whole measure,
    // not either half's, so the two halves are drawn to one scale.
    const PassbandRowScale scale = passband_row_scale(
        frame.floor_db, frame.ceiling_db, frame.percentile_low_db, frame.percentile_high_db);
    if (scale.padded.span_db() <= 0.0F) {
        return;
    }

    std::uint16_t* codes =
        codes_.data() + static_cast<std::size_t>(write_row_) * static_cast<std::size_t>(wide);
    for (int c = 0; c < wide; ++c) {
        codes[c] = encode_level(columns_[static_cast<std::size_t>(c)]);
    }
    row_scales_[static_cast<std::size_t>(write_row_)] = scale;
    colour_passband_row(codes, wide, scale, contrastInForce(), pivotInForce(),
                        reinterpret_cast<std::uint32_t*>(history_.scanLine(write_row_)));
    marks_.write(write_row_, QDateTime::currentMSecsSinceEpoch(), time_marker_seconds_);

    tile_dirty_[static_cast<std::size_t>(write_row_ / kTileRows)] = 1;
    write_row_ = (write_row_ + tall - 1) % tall;
    if (filled_rows_ < tall) {
        filled_rows_ += 1;
        emit historyChanged();
    }
    marker_labels_.sync(marks_.visible(readRow()));
    update();
}

QSGNode* PassbandWaterfallItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
    const FrameCost cost(FrameItem::PassbandWaterfall, FramePhase::Sync);
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
        node->markers = new TimeMarkerNode;
        node->appendChildNode(node->background);
        node->appendChildNode(node->tiles);
        node->appendChildNode(node->markers);
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

    node->markers->sync(window(), marker_labels_, marks_.visible(read), tall, h);

    for (QSGTexture* texture : retired) {
        delete texture;
    }
    return node;
}

}  // namespace revenant::ui
