#include "render/waterfall_item.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include <QColor>
#include <QCursor>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QRectF>
#include <QSGImageNode>
#include <QSGNode>
#include <QSGRectangleNode>
#include <QSGTexture>

namespace revenant::ui {
namespace {

// Dark enough that an empty band is not a grey wall and not pure black, so
// the bottom of the colour map is still distinguishable from nothing drawn.
const QColor kBackground(4, 6, 16);

// The item's own node, so the parts are named rather than fetched back out
// of the child list by index.
//
// It owns the tile textures. A QSGTexture may only be destroyed on the
// render thread, and the scene graph destroys this tree there, so hanging
// them off the node is what makes their lifetime correct without a
// scheduled render job.
class WaterfallNode : public QSGNode {
public:
    ~WaterfallNode() override
    {
        // The image nodes point at these textures, so they go first.
        while (QSGNode* child = firstChild()) {
            removeChildNode(child);
            delete child;
        }
        for (QSGTexture* texture : textures) {
            delete texture;
        }
    }

    WaterfallNode() = default;
    WaterfallNode(const WaterfallNode&) = delete;
    WaterfallNode& operator=(const WaterfallNode&) = delete;
    WaterfallNode(WaterfallNode&&) = delete;
    WaterfallNode& operator=(WaterfallNode&&) = delete;

    QSGRectangleNode* background = nullptr;
    QSGNode* tiles = nullptr;
    OverlayNode* overlay = nullptr;
    std::vector<QSGTexture*> textures;
};

// One contiguous run of ring rows, and where it lands on the display. A tile
// is one run unless the write cursor is inside it, which is the seam between
// newest and oldest and puts its two halves at opposite ends of the display.
struct TileRun {
    int tile = 0;
    int source_row = 0;
    int rows = 0;
    int display_row = 0;
};

}  // namespace

WaterfallItem::WaterfallItem(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);

    hovered_label_ = new OverlayLabelItem(this);
    hovered_label_->setVisible(false);
    selected_label_ = new OverlayLabelItem(this);
    selected_label_->setVisible(false);
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
        connect(link_, &EngineLink::detectionsChanged, this, &WaterfallItem::takeDetections);
    }
    // A different link is a different engine, so the pixels go with the
    // sample ranges. Leaving the pixels would show the previous engine's
    // band under the next one's axis, which is the failure onConnectionChanged
    // describes at more length.
    if (!history_.isNull()) {
        history_.fill(kBackground);
        std::fill(tile_dirty_.begin(), tile_dirty_.end(), std::uint8_t{1});
    }
    filled_rows_ = 0;
    write_row_ = std::max(history_.height() - 1, 0);
    std::fill(row_spans_.begin(), row_spans_.end(), RowSpan{});
    boxes_.clear();
    rebuildDetections();
    emit linkChanged();
    update();
}

void WaterfallItem::setSelectedDetection(qulonglong id)
{
    if (selected_detection_ == id) {
        return;
    }
    selected_detection_ = id;
    rebuildDetections();
    emit selectedDetectionChanged();
    update();
}

void WaterfallItem::onConnectionChanged()
{
    if (link_ == nullptr || !link_->connected()) {
        // History from a link that has just gone away is still history, and
        // it is what the engine said while it was there. It stays until the
        // next engine pushes it off the bottom.
        //
        // The detection boxes go, for the reason SpectrumItem gives at the
        // same place: history is a record and a box is an invitation to
        // click, and clicking a track the engine has forgotten would tune a
        // receiver to nothing.
        boxes_.clear();
        rebuildDetections();
        update();
        return;
    }

    // A new engine, and the axis under this item has changed with it. Every
    // stored row was drawn against the previous span, so keeping them would
    // put a signal at a frequency it was never at. That is the same reason
    // this file's header gives for discarding history on a resize, and it
    // applies harder here: a resize keeps the band and only moves the
    // pixels, where a new engine can be tuned somewhere else entirely.
    if (!history_.isNull()) {
        history_.fill(kBackground);
    }
    write_row_ = std::max(history_.height() - 1, 0);
    filled_rows_ = 0;
    reduced_bins_ = 0;
    std::fill(row_spans_.begin(), row_spans_.end(), RowSpan{});
    std::fill(tile_dirty_.begin(), tile_dirty_.end(), std::uint8_t{1});
    boxes_.clear();
    rebuildDetections();
    update();
}

void WaterfallItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        const QSize wanted = deviceSize();
        rebuild(wanted.width(), wanted.height(), reduced_bins_);
        rebuildDetections();
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

int WaterfallItem::readRow() const
{
    const int tall = history_.height();
    if (tall <= 0) {
        return 0;
    }
    return (write_row_ + 1) % tall;
}

void WaterfallItem::rebuild(int columns, int rows, std::size_t bins)
{
    const int wide = std::max(columns, 1);
    const int tall = std::max(rows, 1);

    // RGBX8888 rather than RGB32: the scene graph's plain texture takes a
    // byte-ordered RGB image as it stands and converts anything else on the
    // way to the GPU, which would be a second pass over every tile uploaded.
    // No alpha channel, so the history composites without blending.
    history_ = QImage(wide, tall, QImage::Format_RGBX8888);
    history_.fill(kBackground);

    // The cursor decrements, so it starts at the last row and the first
    // frame written lands there. The display reads from just past it, which
    // puts that first row at the top with the empty ring below it.
    write_row_ = tall - 1;
    filled_rows_ = 0;
    row_spans_.assign(static_cast<std::size_t>(tall), RowSpan{});

    const int tiles = (tall + kTileRows - 1) / kTileRows;
    tile_dirty_.assign(static_cast<std::size_t>(tiles), std::uint8_t{1});

    columns_.assign(static_cast<std::size_t>(wide), kSpectrumFloorDb);
    reduced_bins_ = bins;

    const std::size_t bins_per_column =
        bins == 0 ? 1 : std::max<std::size_t>(1, bins / static_cast<std::size_t>(wide));
    headroom_db_ = peak_reduction_headroom_db(bins_per_column);
    emit endsChanged();
}

bool WaterfallItem::rowsForSamples(std::uint64_t from_sample, std::uint64_t to_sample,
                                   int& top_row, int& bottom_row) const
{
    const int tall = history_.height();
    if (tall <= 0 || row_spans_.empty()) {
        return false;
    }
    const int read = readRow();
    const auto span_at = [&](int display_row) {
        return row_spans_[static_cast<std::size_t>((read + display_row) % tall)];
    };

    // Both bounds arrive as one PAST the last sample of the frame the
    // evidence came from, because the detector stamps first_seen and
    // last_detected with frame.start + frame.count. A stored row covers the
    // half-open range [start, start + count), so comparing an exclusive end
    // against a row's inclusive contents resolved both edges one row newer
    // than the frame that justified them: the row immediately newer than
    // frame B begins exactly at B.start + B.count and matched.
    //
    // Converted once, here, rather than at each comparison, so the two
    // searches below cannot drift apart. Zero is guarded because these are
    // unsigned and a detection on the very first frame of a stream would
    // otherwise wrap to the far end of the ring.
    const std::uint64_t last_in = to_sample == 0 ? 0 : to_sample - 1;
    const std::uint64_t first_in = from_sample == 0 ? 0 : from_sample - 1;

    // Display rows run newest to oldest, so both a row's first sample and
    // its last are non-increasing down the display and both searches below
    // are binary rather than a scan. Rows never written hold a zero span,
    // which sorts to the oldest end and fails the second test, so they are
    // excluded without a filled-row check.
    int low = 0;
    int high = tall;
    while (low < high) {
        const int mid = low + (high - low) / 2;
        if (span_at(mid).start <= last_in) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }
    top_row = low;

    low = 0;
    high = tall;
    while (low < high) {
        const int mid = low + (high - low) / 2;
        const RowSpan span = span_at(mid);
        if (span.start + span.count > first_in) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    bottom_row = low - 1;

    return top_row < tall && bottom_row >= 0 && top_row <= bottom_row;
}

void WaterfallItem::resolveRows()
{
    const int tall = history_.height();
    if (tall <= 0 || filled_rows_ == 0) {
        return;
    }
    const qreal h = height();

    for (DetectionBox& box : boxes_) {
        int top_row = 0;
        int bottom_row = 0;

        // WHICH EDGE CLOSES A RECTANGLE, AND WHOSE LEADING EDGE IS NOW
        //
        // The closing edge is last_detected and never last_seen: the two
        // differ by exactly the hold for a held track, and a rectangle drawn
        // to last_seen would claim the signal was there through the hold.
        //
        // A live track runs to the newest row instead. Measured this session
        // against a bursty synthetic scene, a Live track's last_detected
        // trails the newest frame by 6 rows at the median and 12 at the
        // worst, which is the detector deciding less often than the engine
        // makes frames; closing there leaves a gap between the signal and the
        // mark on it.
        //
        // Merged does not get that treatment, which this got wrong first. In
        // the same run a Merged track trailed by 50 rows at the median and
        // 110 at the worst, the same distribution as a Held one.
        // core/detect/detector.h says a merged track has evidence and does
        // not decay, and what that means in the numbers is that it keeps its
        // place in the list while its own last detection stays where it was.
        // Running its rectangle to the newest row claimed three seconds of
        // history the signal was not detected in.
        // A rectangle closes where the signal stopped, never where the
        // tracker did, and that is true of EVERY state including Live.
        //
        // This ran a Live track to the newest row, which looked right and
        // was the same mistake as drawing to last_seen, only harder to see.
        // A track stays Live until a decision misses it, and a decision
        // misses it only after the detector has stopped finding it, so
        // between a burst ending and the tracker noticing, the rectangle
        // kept growing over rows the signal was never in. Measured against a
        // seeded bursty scene: a stopped burst stays Live for 5 to 9
        // seconds, so the box trailed its own signal by that much.
        //
        // The cosmetic argument for running to now was that a Live track's
        // last_detected trails the newest frame by 6 rows at the median and
        // 12 at the worst, leaving a small gap between the signal and the
        // mark on it. That gap is not an error to compensate for. It is the
        // rectangle being exactly as current as the detector's last
        // decision, which is the most it can honestly claim.
        //
        // The one thing that could ever justify bridging a silence is
        // knowing the signal's protocol implies one, because then the
        // transmission is continuing rather than the detector guessing.
        // Nothing classifies yet, Classification is Unknown for everything,
        // so there is no basis for it and none should be invented here.
        if (!rowsForSamples(box.first_seen, box.last_detected, top_row, bottom_row)) {
            box.time_bounded = false;
            continue;
        }

        // By display row rather than by pixel, and both ends from the same
        // expression, so the rectangle's edges land on the same boundaries
        // the tile quads are placed at and a row is either inside or outside.
        box.top_px = h * static_cast<qreal>(top_row) / static_cast<qreal>(tall);
        box.bottom_px = h * static_cast<qreal>(bottom_row + 1) / static_cast<qreal>(tall);
        box.time_bounded = true;
    }
}

void WaterfallItem::rebuildDetections()
{
    if (link_ == nullptr) {
        boxes_.clear();
    } else {
        build_detection_boxes(*link_, width(), boxes_);
    }
    resolveRows();
    build_detection_quads(boxes_, width(), height(), selected_detection_, hovered_detection_,
                          DetectionStyle::Rows, quads_);
    placeLabels();
}

void WaterfallItem::placeLabels()
{
    // A label belongs to the top edge of its rectangle, so it ages down the
    // display with the rows it names. Pulled back inside the item rather
    // than clipped, the same rule the plates in qml/Main.qml follow.
    const double strip = overlay_label_height();
    const auto place = [&](OverlayLabelItem* item, std::uint64_t id) {
        const auto found = std::find_if(boxes_.begin(), boxes_.end(),
                                        [id](const DetectionBox& box) {
                                            return box.id != 0 && box.id == id;
                                        });
        if (found == boxes_.end() || !found->time_bounded) {
            item->setLabels({});
            item->setVisible(false);
            return;
        }
        item->setX(0.0);
        item->setWidth(width());
        item->setHeight(strip);
        item->setY(std::clamp(found->top_px + 1.0, 0.0, std::max(0.0, height() - strip)));
        std::vector<OverlayLabel> one;
        one.push_back(detection_label(*found));
        item->setLabels(std::move(one));
    };

    place(selected_label_, selected_detection_);
    // The chosen box is already named, so naming it twice would draw one
    // plate over another at the same place.
    place(hovered_label_,
          hovered_detection_ == selected_detection_ ? 0 : hovered_detection_);
}

void WaterfallItem::takeDetections()
{
    rebuildDetections();
    update();
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
        const Rgb rgb = colour_at(level);
        row[c] = 0xFF000000U | (static_cast<std::uint32_t>(rgb.b) << 16) |
                 (static_cast<std::uint32_t>(rgb.g) << 8) | static_cast<std::uint32_t>(rgb.r);
    }

    // The same index as the pixels, because a rectangle on this display is
    // bounded by the rows a track was present in and this is the only record
    // of which samples a row covers.
    row_spans_[static_cast<std::size_t>(write_row_)] = RowSpan{frame.start, frame.count};
    tile_dirty_[static_cast<std::size_t>(write_row_ / kTileRows)] = 1;

    write_row_ = (write_row_ + tall - 1) % tall;
    filled_rows_ = std::min(filled_rows_ + 1, tall);

    // After the row is in, because the newest row is the leading edge of
    // every live rectangle. This frame also carries a later sample index, so
    // every held track on the spectrum is that much further into its decay.
    rebuildDetections();

    emit endsChanged();
    update();
}

void WaterfallItem::setHovered(std::uint64_t id)
{
    if (hovered_detection_ == id) {
        return;
    }
    hovered_detection_ = id;
    setCursor(id == 0 ? Qt::ArrowCursor : Qt::PointingHandCursor);
    rebuildDetections();
    update();
}

void WaterfallItem::hoverMoveEvent(QHoverEvent* event)
{
    setHovered(detection_at(boxes_, event->position().x()));
    event->accept();
}

void WaterfallItem::hoverLeaveEvent(QHoverEvent* event)
{
    setHovered(0);
    event->accept();
}

void WaterfallItem::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }

    // Its own cycle and not the spectrum item's, because each display has
    // its own pointer and "the same place" is a fact about one of them. See
    // ClickCycle in render/spectrum_item.h.
    const double x = event->position().x();
    const ClickResult hit = detection_clicked(boxes_, x, click_cycle_);
    if (hit.id != 0) {
        const auto found =
            std::find_if(boxes_.begin(), boxes_.end(),
                         [&hit](const DetectionBox& box) { return box.id == hit.id; });
        if (found != boxes_.end()) {
            emit tuneRequested(hit.id, static_cast<double>(found->center_hz),
                               static_cast<double>(found->bandwidth_hz), hit.candidates,
                               hit.rank, hit.exhausted);
            event->accept();
            return;
        }
    }

    const double fraction = width() > 0.0 ? x / width() : 0.0;
    const double hz = link_ == nullptr ? 0.0 : link_->frequencyAtFraction(fraction);
    emit tuneRequested(0, hz, 0.0, 0, 0, false);
    event->accept();
}

QSGNode* WaterfallItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
    const qreal w = width();
    const qreal h = height();
    if (w <= 0.0 || h <= 0.0 || history_.isNull()) {
        delete old_node;
        return nullptr;
    }

    auto* node = static_cast<WaterfallNode*>(old_node);
    if (node == nullptr) {
        node = new WaterfallNode;
        node->background = window()->createRectangleNode();
        node->background->setColor(kBackground);
        node->tiles = new QSGNode;
        node->overlay = new OverlayNode;
        node->appendChildNode(node->background);
        node->appendChildNode(node->tiles);
        node->appendChildNode(node->overlay);
    }
    node->background->setRect(0.0, 0.0, w, h);

    const int wide = history_.width();
    const int tall = history_.height();
    const auto tiles = static_cast<std::size_t>((tall + kTileRows - 1) / kTileRows);

    // A REPLACED TEXTURE IS NOT FREED UNTIL EVERY NODE HAS LET GO OF IT
    //
    // Freeing one here, where it is replaced, crashed the window on the
    // first frame: the image nodes still pointed at the old texture, and the
    // next markDirty in this same function took the renderer through
    // QSGOpaqueTextureMaterial::compare into freed memory. The stack was
    // updatePaintNode, QSGNode::markDirty, Renderer::nodeChanged, compare,
    // jmp into 0xfeeefeee. So they are collected and freed at the bottom,
    // after the nodes below have been repointed.
    std::vector<QSGTexture*> retired;

    // A tree the scene graph threw away takes its textures with it, so a
    // count that does not match is every tile rather than none.
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

        // Copied rather than wrapped. A texture built from a view onto the
        // ring is uploaded on the render thread after the GUI thread has
        // been released, and the GUI thread's next frame writes into that
        // same memory, so the wrapped version uploads a torn tile.
        const QImage source(history_.constScanLine(first), wide, rows,
                            history_.bytesPerLine(), history_.format());
        retired.push_back(node->textures[tile]);
        node->textures[tile] = window()->createTextureFromImage(source.copy());
        tile_dirty_[tile] = 0;
    }

    // Where each tile's rows are on the display. A tile is one run unless
    // the write cursor is inside it, in which case its newest rows are at
    // the top of the display and its oldest at the bottom.
    const int read = readRow();
    std::vector<TileRun> runs;
    runs.reserve(tiles + 1);
    for (std::size_t tile = 0; tile < tiles; ++tile) {
        const int first = static_cast<int>(tile) * kTileRows;
        const int last = std::min(first + kTileRows, tall);
        const auto add = [&](int from, int to) {
            runs.push_back(TileRun{static_cast<int>(tile), from - first, to - from,
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
        // Nearest, because one stored pixel is one screen pixel and any
        // filtering here would smear a one-bin carrier into something that
        // reads as bandwidth.
        image->setFiltering(QSGTexture::Nearest);
        node->tiles->appendChildNode(image);
    }
    while (static_cast<std::size_t>(node->tiles->childCount()) > runs.size()) {
        QSGNode* last = node->tiles->lastChild();
        node->tiles->removeChildNode(last);
        delete last;
    }

    for (std::size_t at = 0; at < runs.size(); ++at) {
        const TileRun& run = runs[at];
        auto* image = static_cast<QSGImageNode*>(node->tiles->childAtIndex(static_cast<int>(at)));
        image->setTexture(node->textures[static_cast<std::size_t>(run.tile)]);
        image->setSourceRect(0.0, static_cast<qreal>(run.source_row), static_cast<qreal>(wide),
                             static_cast<qreal>(run.rows));

        // Both edges from the same expression, so the bottom of one run and
        // the top of the next are the same number to the last bit. At a
        // fractional device pixel ratio there is no whole logical row to
        // count, and a rounded split leaves a line of background showing
        // through at every tile boundary.
        const qreal top = h * static_cast<qreal>(run.display_row) / static_cast<qreal>(tall);
        const qreal bottom =
            h * static_cast<qreal>(run.display_row + run.rows) / static_cast<qreal>(tall);
        image->setRect(QRectF(0.0, top, w, bottom - top));
    }

    node->overlay->setQuads(quads_);

    // Every node now points at a live texture, so the ones they were holding
    // can go. See the block above.
    for (QSGTexture* texture : retired) {
        delete texture;
    }
    return node;
}

}  // namespace revenant::ui
