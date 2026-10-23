#include "render/hdr_map.h"

#include <atomic>
#include <cstring>

#include <QMatrix4x4>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGMaterialShader>
#include <QSGRendererInterface>

#include "render/colour_map.h"

namespace revenant::ui::hdr {
namespace {

std::atomic<float> g_headroom{1.0F};

// Where on the map the lift starts, as a level in [0, 1]. Below it the map is
// exactly as in SDR, so the noise and the weak signals keep the contrast the
// map was designed for; above it a level climbs towards the headroom along a
// square, so the lift starts gently rather than as a visible step.
constexpr float kKnee = 0.5F;

// The block both shader stages declare; shaders/hdr_map.frag.
struct Block {
    float matrix[16];
    float opacity;
    float headroom;
    float knee;
    float bottom_l;
    float top_l;
};

class MapShader : public QSGMaterialShader {
public:
    MapShader()
    {
        setShaderFileName(VertexStage, QStringLiteral(":/shaders/hdr_map.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/shaders/hdr_map.frag.qsb"));
    }

    bool updateUniformData(RenderState& state, QSGMaterial* /*now*/,
                           QSGMaterial* /*before*/) override
    {
        QByteArray* data = state.uniformData();
        if (data->size() < static_cast<qsizetype>(sizeof(Block))) {
            return false;
        }
        Block block{};
        std::memcpy(block.matrix, state.combinedMatrix().constData(), sizeof(block.matrix));
        block.opacity = state.opacity();
        block.headroom = headroom();
        block.knee = kKnee;
        block.bottom_l = static_cast<float>(colour_map::srgb8_to_oklab(colour_map::kBottom).l);
        block.top_l = static_cast<float>(colour_map::kTopLightness);
        std::memcpy(data->data(), &block, sizeof(block));
        return true;
    }

    void updateSampledImage(RenderState& state, int binding, QSGTexture** texture,
                            QSGMaterial* now, QSGMaterial* /*before*/) override
    {
        if (binding != 1) {
            return;
        }
        auto* material = static_cast<MapMaterial*>(now);
        QSGTexture* t = material->texture();
        if (t == nullptr) {
            return;
        }
        t->setFiltering(material->filtering());
        t->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
        *texture = t;
    }
};

}  // namespace

void set_headroom(float value)
{
    g_headroom.store(value > 1.0F ? value : 1.0F, std::memory_order_relaxed);
}

float headroom()
{
    return g_headroom.load(std::memory_order_relaxed);
}

MapMaterial::MapMaterial(bool translucent)
{
    setFlag(Blending, translucent);
}

QSGMaterialType* MapMaterial::type() const
{
    static QSGMaterialType kind;
    return &kind;
}

QSGMaterialShader* MapMaterial::createShader(QSGRendererInterface::RenderMode /*mode*/) const
{
    return new MapShader;
}

int MapMaterial::compare(const QSGMaterial* other) const
{
    const auto* that = static_cast<const MapMaterial*>(other);
    const qint64 mine = texture_ != nullptr ? texture_->comparisonKey() : 0;
    const qint64 theirs = that->texture_ != nullptr ? that->texture_->comparisonKey() : 0;
    if (mine != theirs) {
        return mine < theirs ? -1 : 1;
    }
    return static_cast<int>(filtering_) - static_cast<int>(that->filtering_);
}

MapImageNode::MapImageNode()
    : geometry_(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4), material_(false)
{
    geometry_.setDrawingMode(QSGGeometry::DrawTriangleStrip);
    setGeometry(&geometry_);
    setMaterial(&material_);
}

void MapImageNode::setTexture(QSGTexture* texture)
{
    if (material_.texture() == texture) {
        return;
    }
    material_.setTexture(texture);
    markDirty(QSGNode::DirtyMaterial);
}

void MapImageNode::setFiltering(QSGTexture::Filtering filtering)
{
    if (material_.filtering() == filtering) {
        return;
    }
    material_.setFiltering(filtering);
    markDirty(QSGNode::DirtyMaterial);
}

void MapImageNode::place(const QRectF& rect, const QRectF& source)
{
    // Compared in texture coordinates rather than pixels, because a new tile
    // texture of another size under the same source rect is a new geometry.
    const QSGTexture* texture = material_.texture();
    const QSize size = texture != nullptr ? texture->textureSize() : QSize(1, 1);
    const qreal tw = size.width() > 0 ? size.width() : 1;
    const qreal th = size.height() > 0 ? size.height() : 1;
    const QRectF normalised(source.x() / tw, source.y() / th, source.width() / tw,
                            source.height() / th);
    if (rect == rect_ && normalised == source_) {
        return;
    }
    rect_ = rect;
    source_ = normalised;
    QSGGeometry::updateTexturedRectGeometry(&geometry_, rect, normalised);
    markDirty(QSGNode::DirtyGeometry);
}

QSGNode* make_map_image(QQuickWindow* window, QSGTexture* texture)
{
    const QSGRendererInterface* renderer = window->rendererInterface();
    if (renderer != nullptr && renderer->graphicsApi() == QSGRendererInterface::Software) {
        QSGImageNode* image = window->createImageNode();
        image->setOwnsTexture(false);
        image->setFiltering(QSGTexture::Nearest);
        image->setTexture(texture);
        return image;
    }
    auto* image = new MapImageNode;
    image->setFiltering(QSGTexture::Nearest);
    image->setTexture(texture);
    return image;
}

void place_map_image(QSGNode* node, QSGTexture* texture, const QRectF& rect,
                     const QRectF& source)
{
    if (auto* mine = dynamic_cast<MapImageNode*>(node)) {
        mine->setTexture(texture);
        mine->place(rect, source);
        return;
    }
    auto* image = static_cast<QSGImageNode*>(node);
    image->setTexture(texture);
    image->setSourceRect(source);
    image->setRect(rect);
}

}  // namespace revenant::ui::hdr
