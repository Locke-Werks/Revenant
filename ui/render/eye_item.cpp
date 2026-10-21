#include "render/eye_item.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>

#include "render/symbol_scope_map.h"

namespace revenant::ui {
namespace {

const QColor kIdealColour{255, 196, 120, 110};
const QColor kCentreColour{230, 232, 240, 40};

class LineNode : public QSGGeometryNode {
public:
    LineNode() : geometry_(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0)
    {
        geometry_.setDrawingMode(QSGGeometry::DrawLines);
        geometry_.setLineWidth(1.0F);
        setGeometry(&geometry_);
        setMaterial(&material_);
    }

    LineNode(const LineNode&) = delete;
    LineNode& operator=(const LineNode&) = delete;
    LineNode(LineNode&&) = delete;
    LineNode& operator=(LineNode&&) = delete;

    void take(const std::vector<QSGGeometry::ColoredPoint2D>& vertices)
    {
        geometry_.allocate(static_cast<int>(vertices.size()));
        std::copy(vertices.begin(), vertices.end(), geometry_.vertexDataAsColoredPoint2D());
        markDirty(QSGNode::DirtyGeometry);
    }

private:
    QSGGeometry geometry_;
    QSGVertexColorMaterial material_;
};

// Premultiplied, as the material wants.
QSGGeometry::ColoredPoint2D point(double x, double y, const QColor& colour, float alpha)
{
    const float a = static_cast<float>(colour.alphaF()) * alpha;
    QSGGeometry::ColoredPoint2D p{};
    p.set(static_cast<float>(x), static_cast<float>(y),
          static_cast<unsigned char>(std::lround(colour.redF() * a * 255.0)),
          static_cast<unsigned char>(std::lround(colour.greenF() * a * 255.0)),
          static_cast<unsigned char>(std::lround(colour.blueF() * a * 255.0)),
          static_cast<unsigned char>(std::lround(a * 255.0F)));
    return p;
}

}  // namespace

EyeItem::EyeItem(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

void EyeItem::setLink(EngineLink* link)
{
    if (link_ == link) {
        return;
    }
    if (link_ != nullptr) {
        disconnect(link_, nullptr, this, nullptr);
    }
    link_ = link;
    if (link_ != nullptr) {
        connect(link_, &EngineLink::passbandChanged, this, &EyeItem::takeFrame);
    }
    clear();
    emit linkChanged();
    update();
}

void EyeItem::setTint(const QColor& tint)
{
    if (tint == tint_) {
        return;
    }
    tint_ = tint;
    emit tintChanged();
    update();
}

void EyeItem::setPersistence(int frames)
{
    frames = std::max(frames, 1);
    if (frames == persistence_) {
        return;
    }
    persistence_ = frames;
    while (history_.size() > static_cast<std::size_t>(persistence_)) {
        history_.pop_back();
    }
    emit persistenceChanged();
    update();
}

void EyeItem::clear()
{
    const bool was = active();
    history_.clear();
    trace_length_ = 0;
    latest_ = rpc::SymbolScope{};
    if (was) {
        emit activeChanged();
    }
}

void EyeItem::takeFrame()
{
    if (link_ == nullptr) {
        return;
    }
    const rpc::PassbandFrame& frame = link_->passbandFrame();
    if (!frame.symbols.has_value() || frame.symbols->eye_trace_length == 0) {
        if (active()) {
            clear();
            update();
        }
        return;
    }
    const rpc::SymbolScope& scope = *frame.symbols;
    // A different trace shape or family is a different receiver or mode, and
    // its old traces would be drawn on the wrong axis.
    if (scope.eye_trace_length != trace_length_ || scope.kind != latest_.kind) {
        history_.clear();
    }
    const bool was = active();
    trace_length_ = scope.eye_trace_length;
    latest_.kind = scope.kind;
    latest_.ideal = scope.ideal;
    history_.push_front(scope.eye);
    while (history_.size() > static_cast<std::size_t>(persistence_)) {
        history_.pop_back();
    }
    if (!was) {
        emit activeChanged();
    }
    update();
}

QSGNode* EyeItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
    auto* node = static_cast<LineNode*>(old_node);
    if (node == nullptr) {
        node = new LineNode;
    }
    std::vector<QSGGeometry::ColoredPoint2D> vertices;
    const double w = width();
    const double h = height();
    if (w <= 0.0 || h <= 0.0 || trace_length_ < 2 || history_.empty()) {
        node->take(vertices);
        return node;
    }

    // A constellation's eye is one rail, so its levels are the projections
    // of the diagonal points rather than the points themselves.
    rpc::SymbolScope axis = latest_;
    std::vector<float> rails = latest_.ideal;
    if (latest_.kind == rpc::SymbolKind::Constellation) {
        axis.kind = rpc::SymbolKind::Levels;
        axis.ideal = {-1.0F, 1.0F};
        rails = {-0.70710678F, 0.70710678F};
    }
    const double extent = latest_.kind == rpc::SymbolKind::Constellation ? 1.5 : symbol_extent(axis);

    vertices.push_back(point(0.5 * w, 0.0, kCentreColour, 1.0F));
    vertices.push_back(point(0.5 * w, h, kCentreColour, 1.0F));
    for (const float level : rails) {
        const double y = symbol_y(level, extent, h);
        vertices.push_back(point(0.0, y, kIdealColour, 1.0F));
        vertices.push_back(point(w, y, kIdealColour, 1.0F));
    }

    const double step = w / static_cast<double>(trace_length_ - 1);
    for (std::size_t age = history_.size(); age-- > 0;) {
        const std::vector<float>& traces = history_[age];
        const float alpha =
            0.35F * symbol_eye_alpha(age, static_cast<std::size_t>(persistence_));
        const std::size_t count = traces.size() / trace_length_;
        for (std::size_t t = 0; t < count; ++t) {
            const float* trace = traces.data() + t * trace_length_;
            for (std::uint32_t j = 0; j + 1 < trace_length_; ++j) {
                vertices.push_back(point(static_cast<double>(j) * step,
                                         symbol_y(trace[j], extent, h), tint_, alpha));
                vertices.push_back(point(static_cast<double>(j + 1) * step,
                                         symbol_y(trace[j + 1], extent, h), tint_, alpha));
            }
        }
    }
    node->take(vertices);
    return node;
}

}  // namespace revenant::ui
