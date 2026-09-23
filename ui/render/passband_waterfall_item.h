// The receiver's own waterfall, under its fine-tuning display.
//
// docs/ui-spectrum.md asks for "a second spectrum and waterfall showing the
// receiver's own passband rather than the wide span", and the waterfall half
// is what shows a drifting carrier as drift rather than as the audio slowly
// going wrong. It draws the same PassbandFrame stream the display above it
// draws, one row per frame, newest at the top, in the same colour map as the
// span waterfall.
//
// WHAT IT BORROWS AND WHAT IT DOES NOT
//
// The ring of rows, the tiles it is uploaded in, the decrementing write cursor
// and the height resize are render/waterfall_item.h's, for the reasons that
// file gives; render/history_resize.h does the index arithmetic for a height
// change here too. There are no detections on this display and no time axis
// anybody reads off it, so none of the sample-range bookkeeping comes across.
//
// What is its own is that the axis moves under it. The span waterfall's axis
// moves only when the front end retunes, and it throws its history away then.
// This one's moves every time the receiver does: a wheel notch, a drag of the
// band, AFT. render/history_shift.h shifts the stored rows sideways by the
// pixels the axis moved, so the history stays where it was in absolute terms
// and a drifting carrier draws as a slant rather than as a jump at every move.
// A change of span discards it, and the span changes only when the passband's
// reach crosses a rung of the display rate, a factor of two or more; a filter
// dragged inside a rung keeps every row.
//
// WHAT THE TRACE IS. The frames are the receiver's display tap, which carries
// none of the receiver's filter, so the rows are the air around the receiver
// with a flat noise floor, and nothing here masks outside the passband.
//
// WHAT THIS PARAGRAPH USED TO SAY: "The frames are the fine stream after the
// receiver's filter, so the noise floor carries the filter's response, and so
// do these rows." And the one above ended "A change of span, which a change
// of filter width makes, discards it."

#pragma once

#include <cstdint>
#include <vector>

#include <QImage>
#include <QQuickItem>
#include <QSize>
#include <QtQmlIntegration>

#include "models/engine_link.h"
#include "render/history_shift.h"

namespace revenant::ui {

class PassbandWaterfallItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(revenant::ui::EngineLink* link READ link WRITE setLink NOTIFY linkChanged)

    // How much of the display the history fills, as on the span waterfall.
    Q_PROPERTY(double historyFraction READ historyFraction NOTIFY historyChanged)

public:
    explicit PassbandWaterfallItem(QQuickItem* parent = nullptr);

    [[nodiscard]] EngineLink* link() const { return link_; }
    void setLink(EngineLink* link);

    [[nodiscard]] double historyFraction() const
    {
        return history_.height() <= 0
                   ? 0.0
                   : static_cast<double>(filled_rows_) / static_cast<double>(history_.height());
    }

signals:
    void linkChanged();
    void historyChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;

private:
    void takeFrame();
    void onConnectionChanged();

    // Empties the history and starts a fresh ring at this size.
    void rebuild(int columns, int rows);

    // Keeps the rows across a height change; see render/history_resize.h.
    void resizeRows(int rows);

    // Moves every stored row sideways; see render/history_shift.h.
    void shiftRows(int pixels);

    // Forgets every row without reallocating.
    void clearRows();

    [[nodiscard]] QSize deviceSize() const;
    [[nodiscard]] int readRow() const;

    EngineLink* link_ = nullptr;

    QImage history_;
    int write_row_ = 0;
    int filled_rows_ = 0;
    std::vector<std::uint8_t> tile_dirty_;
    std::vector<float> columns_;

    // The absolute axis the stored rows are on, and the receiver they came
    // from. A different receiver is a different history.
    HistoryAxis axis_{};
    std::uint64_t receiver_ = 0;
};

}  // namespace revenant::ui
