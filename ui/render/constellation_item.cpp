#include "render/constellation_item.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>

#include "render/symbol_scope_map.h"

namespace revenant::ui {
namespace {

const QColor kAxisColour{230, 232, 240, 40};
const QColor kIdealColour{255, 196, 120, 220};
const QColor kHistogramColour{230, 232, 240, 90};

// Flat quads in per-vertex colour, so a thousand dots of differing age are
// one draw call. The material wants premultiplied colour.
class QuadBatch {
public:
    void add(double x0, double y0, double x1, double y1, const QColor& colour, float alpha = 1.0F)
    {
        const float a = static_cast<float>(colour.alphaF()) * alpha;
        const auto r = static_cast<unsigned char>(std::lround(colour.redF() * a * 255.0));
        const auto g = static_cast<unsigned char>(std::lround(colour.greenF() * a * 255.0));
        const auto b = static_cast<unsigned char>(std::lround(colour.blueF() * a * 255.0));
        const auto alpha8 = static_cast<unsigned char>(std::lround(a * 255.0F));
        const auto vertex = [&](double x, double y) {
            QSGGeometry::ColoredPoint2D p{};
            p.set(static_cast<float>(x), static_cast<float>(y), r, g, b, alpha8);
            vertices.push_back(p);
        };
        vertex(x0, y0);
        vertex(x1, y0);
        vertex(x0, y1);
        vertex(x1, y0);
        vertex(x1, y1);
        vertex(x0, y1);
    }

    std::vector<QSGGeometry::ColoredPoint2D> vertices;
};

class BatchNode : public QSGGeometryNode {
public:
    BatchNode() : geometry_(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0)
    {
        geometry_.setDrawingMode(QSGGeometry::DrawTriangles);
        setGeometry(&geometry_);
        setMaterial(&material_);
    }

    BatchNode(const BatchNode&) = delete;
    BatchNode& operator=(const BatchNode&) = delete;
    BatchNode(BatchNode&&) = delete;
    BatchNode& operator=(BatchNode&&) = delete;

    void take(const QuadBatch& batch)
    {
        geometry_.allocate(static_cast<int>(batch.vertices.size()));
        std::copy(batch.vertices.begin(), batch.vertices.end(), geometry_.vertexDataAsColoredPoint2D());
        markDirty(QSGNode::DirtyGeometry);
    }

private:
    QSGGeometry geometry_;
    QSGVertexColorMaterial material_;
};

}  // namespace

ConstellationItem::ConstellationItem(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

void ConstellationItem::setLink(EngineLink* link)
{
    if (link_ == link) {
        return;
    }
    if (link_ != nullptr) {
        disconnect(link_, nullptr, this, nullptr);
    }
    link_ = link;
    if (link_ != nullptr) {
        connect(link_, &EngineLink::passbandChanged, this, &ConstellationItem::takeFrame);
    }
    scope_ = rpc::SymbolScope{};
    emit linkChanged();
    emit scopeChanged();
    update();
}

void ConstellationItem::setTint(const QColor& tint)
{
    if (tint == tint_) {
        return;
    }
    tint_ = tint;
    emit tintChanged();
    update();
}

QString ConstellationItem::kind() const
{
    switch (scope_.kind) {
        case rpc::SymbolKind::Levels: return QStringLiteral("levels");
        case rpc::SymbolKind::Constellation: return QStringLiteral("constellation");
        case rpc::SymbolKind::None: break;
    }
    return {};
}

void ConstellationItem::takeFrame()
{
    if (link_ == nullptr) {
        return;
    }
    const rpc::PassbandFrame& frame = link_->passbandFrame();
    if (frame.symbols.has_value()) {
        scope_ = *frame.symbols;
    } else if (scope_.kind != rpc::SymbolKind::None) {
        scope_ = rpc::SymbolScope{};
    } else {
        return;
    }
    emit scopeChanged();
    update();
}

QSGNode* ConstellationItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
    auto* node = static_cast<BatchNode*>(old_node);
    if (node == nullptr) {
        node = new BatchNode;
    }
    QuadBatch batch;
    const double w = width();
    const double h = height();
    if (w <= 0.0 || h <= 0.0) {
        node->take(batch);
        return node;
    }

    const double extent = symbol_extent(scope_);
    const double dot = std::max(1.5, std::min(w, h) / 90.0);

    if (scope_.kind == rpc::SymbolKind::Constellation) {
        // A square plot centred in the item, whatever its aspect.
        const double side = std::min(w, h);
        const double left = 0.5 * (w - side);
        const double top = 0.5 * (h - side);
        batch.add(left, top + 0.5 * side - 0.5, left + side, top + 0.5 * side + 0.5, kAxisColour);
        batch.add(left + 0.5 * side - 0.5, top, left + 0.5 * side + 0.5, top + side, kAxisColour);

        const std::size_t count = scope_.points.size() / 2;
        for (std::size_t i = 0; i < count; ++i) {
            const double x = left + symbol_x(scope_.points[2 * i], extent, side);
            const double y = top + symbol_y(scope_.points[2 * i + 1], extent, side);
            batch.add(x - 0.5 * dot, y - 0.5 * dot, x + 0.5 * dot, y + 0.5 * dot, tint_,
                      0.45F * symbol_dot_alpha(i, count));
        }
        const double arm = 2.5 * dot;
        for (std::size_t i = 0; i + 1 < scope_.ideal.size(); i += 2) {
            const double x = left + symbol_x(scope_.ideal[i], extent, side);
            const double y = top + symbol_y(scope_.ideal[i + 1], extent, side);
            batch.add(x - arm, y - 0.5, x + arm, y + 0.5, kIdealColour);
            batch.add(x - 0.5, y - arm, x + 0.5, y + arm, kIdealColour);
        }
    } else if (scope_.kind == rpc::SymbolKind::Levels) {
        // Time across, level up; the right fifth is the histogram.
        const double plot = 0.8 * w;
        for (const float level : scope_.ideal) {
            const double y = symbol_y(level, extent, h);
            batch.add(0.0, y - 0.5, w, y + 0.5, kIdealColour, 0.6F);
        }
        const std::size_t count = scope_.points.size();
        for (std::size_t i = 0; i < count; ++i) {
            const double x = (static_cast<double>(i) + 0.5) / static_cast<double>(count) * plot;
            const double y = symbol_y(scope_.points[i], extent, h);
            batch.add(x - 0.5 * dot, y - 0.5 * dot, x + 0.5 * dot, y + 0.5 * dot, tint_,
                      0.6F * symbol_dot_alpha(i, count));
        }
        constexpr std::size_t kBins = 64;
        const auto bins = symbol_level_histogram(scope_.points, extent, kBins);
        const double bar = h / static_cast<double>(kBins);
        for (std::size_t b = 0; b < kBins; ++b) {
            if (bins[b] <= 0.0F) {
                continue;
            }
            // Bin zero is the most negative level, at the bottom.
            const double y1 = h - static_cast<double>(b) * bar;
            batch.add(plot, y1 - bar, plot + (w - plot) * static_cast<double>(bins[b]), y1,
                      kHistogramColour);
        }
    }
    node->take(batch);
    return node;
}

}  // namespace revenant::ui
