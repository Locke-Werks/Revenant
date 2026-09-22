// The waterfall: one row of pixels per frame, newest at the top.
//
// WHICH WAY TIME RUNS, AND WHY IT IS THIS WAY
//
// New rows arrive at the top and the history scrolls down. SDR#, SDRuno,
// GQRX, CubicSDR and HDSDR all default to that, so an operator's eye goes to
// the top of a waterfall for "now" before they have read a single label, and
// this display ran the other way until 2026-09-19. docs/ui-spectrum.md now
// states the convention, because the vertical scrub gesture specified there
// inherits the same axis and a gesture that scrubs backwards on a display
// that runs backwards is two mistakes cancelling.
//
// THE RING, AND WHY THERE IS ONE
//
// The obvious implementation scrolls: move every row down one and write the
// new row at the top. That is a copy of the whole image per frame, on the
// GUI thread, at whatever rate the subscription asked for. A ring costs one
// row written and a few quads instead, and the quads are what the scene
// graph was going to do anyway.
//
// The write cursor DECREMENTS. That is the whole of what makes newest-at-top
// cheap: inside a contiguous run of ring rows, increasing index is
// increasing time whichever way the cursor moves, so reading the ring the
// other way round would need a vertical mirror or a row-by-row loop. With
// the cursor moving backwards the newest row is at the lowest recently
// written index, and reading the ring forwards from just past the cursor is
// newest to oldest, top to bottom, with no mirror anywhere.
//
// HOW IT REACHES THE SCREEN, WHICH IS NO LONGER A PAINTED ITEM
//
// render/spectrum_item.h has the reasoning and the measurement. The short
// form: a painted item rasterises and re-uploads the whole display every
// frame to deliver one new row, so the frame rate fell as the window grew.
// Here the ring lives in GPU textures and the scroll is done by moving
// source rectangles, so a frame costs one row of pixels and a handful of
// quads whatever the display's height.
//
// The ring is cut into tiles of kTileRows rows, one texture each, because
// the scene graph's public API can create a texture from an image and cannot
// update part of one. A frame dirties exactly the tile its row landed in, so
// the per-frame upload is the tile and not the display: at 1578 pixels wide
// that is 64 rows rather than the 1123 the tallest window measured this
// session had. Tiles also cost one draw call each, which is what keeps them
// from being one row apiece.
//
// WHAT A RESIZE COSTS, WHICH DEPENDS ON WHICH AXIS MOVED
//
// A WIDTH change discards the history. A column covers a run of bins, and
// the run depends on the column count, so the same stored row means a
// different frequency per pixel at a different width. Rescaling it would
// draw a signal at a frequency it was never at. Losing the picture there is
// visible and honest; moving a carrier is neither.
//
// A HEIGHT change keeps it. The ring is reallocated and the newest
// min(filled rows, new height) rows are copied across with their row spans,
// so a taller item gains empty rows below the history and a shorter one
// drops the oldest.
//
// WHAT THIS PARAGRAPH USED TO SAY
//
// Until 2026-09-20 it was one sentence, "the history is discarded", with the
// bins-per-column argument behind it, and geometryChange called rebuild() on
// any size change at all. The argument is correct and it is about width
// alone: a height change moves no row sideways and changes no column's bins.
// The cost of the overreach was not theoretical. The detail pane opens above
// this item, which resizes it in height only, so the first click-to-tune
// threw away the history the operator had just read to decide what to tune,
// twice, because the pane's layout settles in a second pass.
//
// render/history_resize.h holds the index arithmetic for the height case and
// says why it is not in this file: a wrapped ring index is the part that can
// be off by one, and none of it needs a window to test.
//
// WHAT THE DETECTION OVERLAY IS DOING IN HERE
//
// render/spectrum_item.h owns it, and this file includes that header for it
// rather than growing a copy. Two reasons, both in that file's header block:
// there is one mapping from hertz to pixels, and the two displays draw a
// detection differently on purpose. This one has a time axis, so a detection
// here is a rectangle closed in both axes, and the rows it spans are
// resolved from a ring of sample ranges kept beside the pixels.

#pragma once

#include <QImage>
#include <QQuickItem>
#include <QSize>
#include <QtQmlIntegration>

#include <cstdint>
#include <vector>

#include "models/engine_link.h"
#include "render/history_resize.h"
#include "render/spectrum_item.h"
#include "render/spectrum_scale.h"

class QMouseEvent;
class QHoverEvent;

namespace revenant::ui {

// Rows per history tile. One texture is created per dirty tile per frame, so
// this trades the per-frame upload against the draw call count: the upload
// is one tile and the draw calls are the display's height divided by this.
inline constexpr int kTileRows = 64;

class WaterfallItem : public QQuickItem {
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

signals:
    void linkChanged();
    void endsChanged();
    void selectedDetectionChanged();

    // Same signal and the same caveats as SpectrumItem::tuneRequested, which
    // carries the note about the measured centre not being the logical one
    // and what the three counts are.
    void tuneRequested(qulonglong id, double center_hz, double bandwidth_hz, int candidates,
                       int rank, bool exhausted);

protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void mousePressEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;

    // Scrolling walks the SOURCE's centre along, exactly as it does over the
    // spectrum, and into the same accumulator: see EngineLink::takeScrollTune.
    //
    // Vertical scroll over this display has a second meaning waiting to be
    // written: docs/ui-spectrum.md wants it scrubbing through a recording. The
    // two do not collide, because a recording is precisely a source that
    // refuses to retune, so this handler ignores the event on one and leaves
    // the gesture free.
    void wheelEvent(QWheelEvent* event) override;

private:
    // What one stored row covers, in the engine's own sample indices. Kept
    // in a ring beside the pixels and written in the same place, because a
    // rectangle on this display is bounded by the rows a track was present
    // in and nothing else in the client knows which rows those are.
    struct RowSpan {
        std::uint64_t start = 0;
        std::uint64_t count = 0;
    };

    void takeFrame();
    void takeDetections();

    // The receiver moved, or the engine answered about it. See the same slot
    // on SpectrumItem for why both signals land in one place.
    void takeReceiver();

    void onConnectionChanged();
    void rebuild(int columns, int rows, std::size_t bins);

    // Reallocates the ring at a new HEIGHT and carries the rows over. Only
    // legal when the width has not moved; see the note in geometryChange
    // for why the width case cannot do this. render/history_resize.h holds
    // the index arithmetic, and ui/tests covers it.
    void resizeRows(int rows);

    // Everything drawn over the history: the detection rectangles and their
    // labels, and the receiver's passband over the top of those. See the same
    // function on SpectrumItem.
    void rebuildOverlay();

    void placeLabels();
    void setHovered(std::uint64_t id);

    // The newest row, which is the top of the display. The cursor decrements,
    // so it points one past the newest.
    [[nodiscard]] int readRow() const;

    // The rows covering [from_sample, to_sample], as display rows counted
    // from the top. Returns false when the history holds none of them, which
    // is an ordinary answer for a track whose evidence has scrolled off.
    [[nodiscard]] bool rowsForSamples(std::uint64_t from_sample, std::uint64_t to_sample,
                                      int& top_row, int& bottom_row) const;

    // Fills in every box's top and bottom from its sample range.
    void resolveRows();

    // The ring is sized in physical pixels, not logical ones, so one stored
    // pixel is one screen pixel. A ring built at logical size is stretched on
    // the way out: on the 1.25 scaling this was checked on it put each column
    // across a pixel and a quarter, which nearest-neighbour turns into a
    // column repeated at uneven intervals. Filtering it instead would smear a
    // one-bin carrier into something that reads as bandwidth, which is why
    // the tiles are drawn with QSGTexture::Nearest.
    [[nodiscard]] QSize deviceSize() const;

    EngineLink* link_ = nullptr;

    // Rows are written at write_row_ and it decrements, so the newest row is
    // the one just past it and the oldest is the one it points at.
    //
    // filled_rows_ no longer decides whether anything is drawn: the ring is
    // filled with the background colour, so drawing a row that has never been
    // written draws the background. It gates the row search instead, which
    // has nothing to answer before the first frame.
    QImage history_;
    int write_row_ = 0;
    int filled_rows_ = 0;
    std::vector<RowSpan> row_spans_;

    // Which tiles have had a row written since their texture was built. Only
    // the tile the cursor is in can be dirty from a frame; a resize or a new
    // engine dirties them all.
    std::vector<std::uint8_t> tile_dirty_;

    std::vector<float> columns_;
    MapEnds ends_;
    float headroom_db_ = 0.0F;
    std::size_t reduced_bins_ = 0;

    std::vector<DetectionBox> boxes_;
    std::vector<OverlayQuad> quads_;
    std::uint64_t selected_detection_ = 0;
    std::uint64_t hovered_detection_ = 0;
    ClickCycle click_cycle_;

    // One strip per labelled box rather than one strip for the display,
    // because down here a label belongs to the top edge of its rectangle and
    // two rectangles are rarely at the same height. At most the chosen box
    // and the one under the pointer are named; a label per track would cover
    // the history the rectangles are pointing at.
    OverlayLabelItem* hovered_label_ = nullptr;
    OverlayLabelItem* selected_label_ = nullptr;
};

}  // namespace revenant::ui
