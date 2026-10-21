// The 3D waterfall: the span's recent history as a lit surface over
// frequency and time, seen in perspective from a fixed camera that a drag
// inside the item orbits and a double click puts back.
// render/surface3d_math.h has the world, the projection and why the
// triangles are sorted rather than depth tested.
//
// The level axis and the colours are the waterfall's, as on the persistence
// view: floor and ceiling bound from WaterfallItem's drawn ends in
// qml/SpanView.qml, the colour map and contrast curve from ScaleSettings.
//
// The cost of a frame is one row reduced into the ring and the coarse mesh
// projected and recoloured, a few thousand vertices whatever the window, in
// one geometry node. Nothing at all while the item is not visible.

#pragma once

#include <QPointF>
#include <QQuickItem>
#include <QtQmlIntegration>

#include <cstdint>
#include <vector>

#include "models/engine_link.h"
#include "models/scale_settings.h"
#include "render/surface3d_math.h"

class QMouseEvent;

namespace revenant::ui {

class Surface3DItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(revenant::ui::EngineLink* link READ link WRITE setLink NOTIFY linkChanged)
    Q_PROPERTY(revenant::ui::ScaleSettings* mapPins READ mapPins WRITE setMapPins
                   NOTIFY mapPinsChanged)
    Q_PROPERTY(double floorDb READ floorDb WRITE setFloorDb NOTIFY endsChanged)
    Q_PROPERTY(double ceilingDb READ ceilingDb WRITE setCeilingDb NOTIFY endsChanged)

public:
    explicit Surface3DItem(QQuickItem* parent = nullptr);

    [[nodiscard]] EngineLink* link() const { return link_; }
    void setLink(EngineLink* link);

    [[nodiscard]] ScaleSettings* mapPins() const { return map_pins_; }
    void setMapPins(ScaleSettings* pins);

    [[nodiscard]] double floorDb() const { return floor_db_; }
    void setFloorDb(double db);
    [[nodiscard]] double ceilingDb() const { return ceiling_db_; }
    void setCeilingDb(double db);

    // Back to the default camera.
    Q_INVOKABLE void resetCamera();

signals:
    void linkChanged();
    void mapPinsChanged();
    void endsChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void takeFrame();
    void clearHistory();

    EngineLink* link_ = nullptr;
    ScaleSettings* map_pins_ = nullptr;
    double floor_db_ = -100.0;
    double ceiling_db_ = -40.0;

    SurfaceHistory history_;
    std::vector<float> columns_;

    Orbit orbit_;
    Orbit drag_from_;
    QPointF press_at_;
    bool dragging_ = false;
};

}  // namespace revenant::ui
