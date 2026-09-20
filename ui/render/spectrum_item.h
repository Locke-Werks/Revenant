// The instantaneous spectrum, drawn as one column of pixels per bin run.
//
// WHY A PAINTED ITEM AND NOT A QSGRenderNode
//
// ui/render/.gitkeep planned a custom render node sharing the Vulkan device
// and command buffers with the DSP, so raster data never touched host
// memory. That is still the right destination and it is not reachable from
// here: the UI is a separate process for the runtime reason
// core/rpc/types.h sets out, and a frame reaches it as a copied vector of
// floats over a socket. The raster data is already in host memory by the
// time this item sees it, so a painted item is what the data actually is.
// The shared-handle path arrives with a local-client fast path in the
// schema, not before.

#pragma once

#include <vector>

#include <QQuickPaintedItem>
#include <QtQmlIntegration>

#include "models/engine_link.h"
#include "render/spectrum_scale.h"

class QPainter;

namespace revenant::ui {

class SpectrumItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(revenant::ui::EngineLink* link READ link WRITE setLink NOTIFY linkChanged)

    // The two ends this item is actually drawing against, which is the
    // frame's pair plus this item's own reduction correction. Exposed so the
    // axis labels in QML read what was drawn rather than what was sent: the
    // correction depends on how many bins a column covers, so it is a
    // property of this item's width and cannot be computed anywhere else.
    Q_PROPERTY(double drawFloorDb READ drawFloorDb NOTIFY endsChanged)
    Q_PROPERTY(double drawCeilingDb READ drawCeilingDb NOTIFY endsChanged)
    Q_PROPERTY(double headroomDb READ headroomDb NOTIFY endsChanged)

public:
    explicit SpectrumItem(QQuickItem* parent = nullptr);

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

    // Recomputes the reduction headroom for the current column count. The
    // correction is a function of how many bins one column covers, so it
    // changes when the item is resized and not when a frame arrives.
    void resizeColumns(int columns, std::size_t bins);

    EngineLink* link_ = nullptr;
    std::vector<float> columns_;
    MapEnds ends_;
    float headroom_db_ = 0.0F;
    std::size_t reduced_bins_ = 0;
    bool have_frame_ = false;
};

}  // namespace revenant::ui
