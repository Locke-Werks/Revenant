// The waterfall: one row of pixels per frame, oldest at the top.
//
// THE RING, AND WHY THERE IS ONE
//
// The obvious implementation scrolls: move every row up one and write the
// new row at the bottom. That is a copy of the whole image per frame, on
// the GUI thread, at whatever rate the subscription asked for. A ring costs
// one row written and two blits instead, and the two blits are what the
// scene graph was going to do anyway.
//
// The ring's oldest row is wherever the write cursor currently points, so
// the image is drawn as two pieces: the rows from the cursor to the end
// first, then the rows before it.
//
// WHAT A RESIZE COSTS
//
// The history is discarded. A column covers a run of bins, and the run
// depends on the column count, so the same stored row means a different
// frequency per pixel at a different width. Rescaling the history would
// draw a signal at a frequency it was never at. Losing the picture on a
// resize is visible and honest; moving a carrier is neither.
//
// WHAT THE DETECTION OVERLAY IS DOING IN HERE
//
// render/spectrum_item.h owns it, and this file includes that header for it
// rather than growing a copy. The reasoning is in the block at the top of
// that file: there is one mapping from hertz to pixels, an overlay with a
// second one agrees at the centre of the span and is offset at the edges,
// and the waterfall is precisely where an operator checks whether a box sits
// on the carrier it names.

#pragma once

#include <QImage>
#include <QQuickPaintedItem>
#include <QSize>
#include <QtQmlIntegration>

#include <cstdint>
#include <vector>

#include "models/engine_link.h"
#include "render/spectrum_item.h"
#include "render/spectrum_scale.h"

class QMouseEvent;
class QHoverEvent;
class QPainter;

namespace revenant::ui {

class WaterfallItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(revenant::ui::EngineLink* link READ link WRITE setLink NOTIFY linkChanged)

    Q_PROPERTY(double drawFloorDb READ drawFloorDb NOTIFY endsChanged)
    Q_PROPERTY(double drawCeilingDb READ drawCeilingDb NOTIFY endsChanged)
    Q_PROPERTY(double headroomDb READ headroomDb NOTIFY endsChanged)

    // Written by QML and never by this item, so the two displays share one
    // selection. See the same property on SpectrumItem.
    Q_PROPERTY(qulonglong selectedDetection READ selectedDetection WRITE setSelectedDetection
                   NOTIFY selectedDetectionChanged)

public:
    explicit WaterfallItem(QQuickItem* parent = nullptr);

    [[nodiscard]] EngineLink* link() const { return link_; }
    void setLink(EngineLink* link);

    [[nodiscard]] double drawFloorDb() const { return ends_.floor_db; }
    [[nodiscard]] double drawCeilingDb() const { return ends_.ceiling_db; }
    [[nodiscard]] double headroomDb() const { return headroom_db_; }

    [[nodiscard]] qulonglong selectedDetection() const { return selected_detection_; }
    void setSelectedDetection(qulonglong id);

    void paint(QPainter* painter) override;

signals:
    void linkChanged();
    void endsChanged();
    void selectedDetectionChanged();

    // Same signal and the same caveats as SpectrumItem::tuneRequested, which
    // carries the note about the measured centre not being the logical one
    // and what the two counts are.
    void tuneRequested(qulonglong id, double center_hz, double bandwidth_hz, int candidates,
                       int rank);

protected:
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void mousePressEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;

private:
    void takeFrame();
    void takeDetections();
    void onConnectionChanged();
    void rebuild(int columns, int rows, std::size_t bins);
    void rebuildDetections();
    void setHovered(std::uint64_t id);

    // The ring is sized in physical pixels, not logical ones, so one stored
    // pixel is one screen pixel. QQuickPaintedItem paints into a texture the
    // size of the item times the window's device pixel ratio, and a ring
    // built at logical size is therefore blown up on the way out: on the
    // 1.25 scaling this was checked on it put each column across a pixel and
    // a quarter, which nearest-neighbour turns into a column repeated at
    // uneven intervals. Smoothing it instead would smear a one-bin carrier
    // into something that reads as bandwidth, which is why setSmooth is off.
    [[nodiscard]] QSize deviceSize() const;

    EngineLink* link_ = nullptr;

    // Rows are written at write_row_ and it advances, so the oldest row is
    // the one it points at.
    QImage history_;
    int write_row_ = 0;
    int filled_rows_ = 0;

    std::vector<float> columns_;
    MapEnds ends_;
    float headroom_db_ = 0.0F;
    std::size_t reduced_bins_ = 0;

    std::vector<DetectionBox> boxes_;
    std::uint64_t selected_detection_ = 0;
    std::uint64_t hovered_detection_ = 0;
    ClickCycle click_cycle_;
};

}  // namespace revenant::ui
