#include "render/surface3d_item.h"

#include <algorithm>
#include <cstring>
#include <span>

#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGNode>
#include <QSGRectangleNode>
#include <QSGVertexColorMaterial>

#include "render/colour_map.h"
#include "render/spectrum_scale.h"
#include "render/waterfall_scale.h"

namespace revenant::ui {

namespace {

// The oldest row is drawn this much darker than the newest, which is the
// cue that says which way time runs once the camera has turned.
constexpr float kFarDimming = 0.45F;

class SurfaceNode : public QSGNode {
public:
    QSGRectangleNode* background = nullptr;
    QSGGeometryNode* mesh = nullptr;
    DrawOrder order{};
    bool indexed = false;
};

}  // namespace

Surface3DItem::Surface3DItem(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setClip(true);
    setAcceptedMouseButtons(Qt::LeftButton);
    columns_.assign(static_cast<std::size_t>(kSurfaceColumns), kSpectrumFloorDb);
}

void Surface3DItem::setLink(EngineLink* link)
{
    if (link_ == link) {
        return;
    }
    if (link_ != nullptr) {
        disconnect(link_, nullptr, this, nullptr);
    }
    link_ = link;
    if (link_ != nullptr) {
        connect(link_, &EngineLink::frameChanged, this, &Surface3DItem::takeFrame);
        // A new engine or a granted retune; see PersistenceItem::setLink.
        connect(link_, &EngineLink::connectionChanged, this, &Surface3DItem::clearHistory);
    }
    clearHistory();
    emit linkChanged();
}

void Surface3DItem::setMapPins(ScaleSettings* pins)
{
    if (map_pins_ == pins) {
        return;
    }
    if (map_pins_ != nullptr) {
        disconnect(map_pins_, nullptr, this, nullptr);
    }
    map_pins_ = pins;
    if (map_pins_ != nullptr) {
        connect(map_pins_, &ScaleSettings::waterfallContrastChanged, this,
                [this] { update(); });
        connect(map_pins_, &ScaleSettings::waterfallPivotChanged, this, [this] { update(); });
    }
    emit mapPinsChanged();
}

void Surface3DItem::setFloorDb(double db)
{
    if (db == floor_db_) {
        return;
    }
    floor_db_ = db;
    emit endsChanged();
    update();
}

void Surface3DItem::setCeilingDb(double db)
{
    if (db == ceiling_db_) {
        return;
    }
    ceiling_db_ = db;
    emit endsChanged();
    update();
}

void Surface3DItem::resetCamera()
{
    orbit_ = Orbit{};
    update();
}

void Surface3DItem::clearHistory()
{
    history_.clear();
    update();
}

void Surface3DItem::takeFrame()
{
    if (link_ == nullptr || !isVisible()) {
        return;
    }
    const rpc::SpectrumFrame& frame = link_->frame();
    if (frame.power_db.empty()) {
        return;
    }
    const ViewBins drawn = link_->viewBins(frame.power_db.size());
    const std::span<const float> shown =
        std::span<const float>(frame.power_db).subspan(drawn.first, drawn.count());
    reduce_peak(shown, columns_);
    history_.push(columns_);
    update();
}

void Surface3DItem::mousePressEvent(QMouseEvent* event)
{
    dragging_ = true;
    press_at_ = event->position();
    drag_from_ = orbit_;
    event->accept();
}

void Surface3DItem::mouseMoveEvent(QMouseEvent* event)
{
    if (!dragging_) {
        event->ignore();
        return;
    }
    const QPointF moved = event->position() - press_at_;
    orbit_ = drag_from_.dragged(static_cast<float>(moved.x()), static_cast<float>(moved.y()));
    update();
    event->accept();
}

void Surface3DItem::mouseReleaseEvent(QMouseEvent* event)
{
    dragging_ = false;
    event->accept();
}

void Surface3DItem::mouseDoubleClickEvent(QMouseEvent* event)
{
    resetCamera();
    event->accept();
}

QSGNode* Surface3DItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
    const auto w = static_cast<float>(width());
    const auto h = static_cast<float>(height());
    if (w <= 0.0F || h <= 0.0F) {
        delete old_node;
        return nullptr;
    }

    const int columns = history_.columns();
    const int rows = history_.rows();

    auto* node = static_cast<SurfaceNode*>(old_node);
    if (node == nullptr) {
        node = new SurfaceNode;
        node->background = window()->createRectangleNode();
        node->background->setColor(
            QColor(colour_map::kBottom.r, colour_map::kBottom.g, colour_map::kBottom.b));
        node->appendChildNode(node->background);

        node->mesh = new QSGGeometryNode;
        auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(),
                                         columns * rows, 0, QSGGeometry::UnsignedShortType);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        geometry->setVertexDataPattern(QSGGeometry::StreamPattern);
        geometry->setIndexDataPattern(QSGGeometry::StaticPattern);
        node->mesh->setGeometry(geometry);
        node->mesh->setFlag(QSGNode::OwnsGeometry);
        // Vertex colours, so the whole surface is one node and one draw call.
        // The material always blends, which is what makes the scene graph
        // honour the index order the painter's rule depends on.
        node->mesh->setMaterial(new QSGVertexColorMaterial);
        node->mesh->setFlag(QSGNode::OwnsMaterial);
        node->appendChildNode(node->mesh);
    }
    node->background->setRect(QRectF(0.0, 0.0, width(), height()));

    const SurfaceCamera camera(orbit_, w, h);
    QSGGeometry* geometry = node->mesh->geometry();

    const DrawOrder order = draw_order(camera);
    if (!node->indexed || order.oldest_first != node->order.oldest_first ||
        order.high_first != node->order.high_first) {
        const std::vector<std::uint16_t> indices = grid_indices(columns, rows, order);
        geometry->allocate(columns * rows, static_cast<int>(indices.size()));
        std::memcpy(geometry->indexDataAsUShort(), indices.data(),
                    indices.size() * sizeof(std::uint16_t));
        node->order = order;
        node->indexed = true;
        node->mesh->markDirty(QSGNode::DirtyGeometry);
    }

    const float floor_db = static_cast<float>(floor_db_);
    const float span_db = std::max(static_cast<float>(ceiling_db_ - floor_db_), 1.0F);
    const ContrastCurve curve =
        map_pins_ == nullptr ? ContrastCurve{}
                             : curve_for(static_cast<float>(map_pins_->waterfallContrast()),
                                         static_cast<float>(map_pins_->waterfallPivot()));

    QSGGeometry::ColoredPoint2D* vertices = geometry->vertexDataAsColoredPoint2D();
    for (int age = 0; age < rows; ++age) {
        const std::span<const float> row = history_.row(age);
        const float fog =
            1.0F - kFarDimming * static_cast<float>(age) / static_cast<float>(std::max(rows - 1, 1));
        for (int c = 0; c < columns; ++c) {
            const float level =
                apply_curve(curve, (row[static_cast<std::size_t>(c)] - floor_db) / span_db);
            const Projected at = camera.project(surface_point(c, columns, age, rows, level));
            const colour_map::Srgb8 colour = colour_map::colour_at(level);
            vertices[age * columns + c].set(
                at.x, at.y, static_cast<unsigned char>(static_cast<float>(colour.r) * fog),
                static_cast<unsigned char>(static_cast<float>(colour.g) * fog),
                static_cast<unsigned char>(static_cast<float>(colour.b) * fog), 255);
        }
    }
    node->mesh->markDirty(QSGNode::DirtyGeometry);
    return node;
}

}  // namespace revenant::ui
