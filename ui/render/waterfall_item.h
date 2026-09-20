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

#pragma once

#include <QImage>
#include <QQuickPaintedItem>
#include <QSize>
#include <QtQmlIntegration>

#include <vector>

#include "models/engine_link.h"
#include "render/spectrum_scale.h"

class QPainter;

namespace revenant::ui {

class WaterfallItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(revenant::ui::EngineLink* link READ link WRITE setLink NOTIFY linkChanged)

    Q_PROPERTY(double drawFloorDb READ drawFloorDb NOTIFY endsChanged)
    Q_PROPERTY(double drawCeilingDb READ drawCeilingDb NOTIFY endsChanged)
    Q_PROPERTY(double headroomDb READ headroomDb NOTIFY endsChanged)

public:
    explicit WaterfallItem(QQuickItem* parent = nullptr);

    [[nodiscard]] EngineLink* link() const { return link_; }
    void setLink(EngineLink* link);

    [[nodiscard]] double drawFloorDb() const { return ends_.floor_db; }
    [[nodiscard]] double drawCeilingDb() const { return ends_.ceiling_db; }
    [[nodiscard]] double headroomDb() const { return headroom_db_; }

    void paint(QPainter* painter) override;

signals:
    void linkChanged();
    void endsChanged();

protected:
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void takeFrame();
    void onConnectionChanged();
    void rebuild(int columns, int rows, std::size_t bins);

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
};

}  // namespace revenant::ui
