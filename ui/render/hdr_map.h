// The span displays' colour map with headroom above SDR white, for the
// waterfall's tiles and the spectrum's fill.
//
// WHY ONLY THESE. HDR is for the one thing on the window that is a picture of
// a measurement: a strong carrier is brighter than the UI's white because it is
// stronger, and on an SDR screen the top of the map has nowhere to go. The
// rest of the window, text and controls included, stays at SDR white, which is
// what render/hdr_output.h arranges.
//
// HOW. shaders/hdr_map.frag reads the level back off the colour, through the
// map's lightness, and lifts the top part of the map in linear light, up to
// headroom() times SDR white. The textures are the same 8-bit ones as ever, so
// the waterfall's ring, its tiles and the scrub view are untouched. headroom()
// is 1 until the window has confirmed an scRGB swapchain, and at 1 the shader
// passes every texel through as it is, so SDR draws exactly what it drew.
//
// The software scene graph, which the offscreen platform uses for smoke runs,
// draws no custom material, so make_map_image() hands it a stock image node.

#pragma once

#include <QRectF>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGTexture>

class QQuickWindow;
class QSGNode;

namespace revenant::ui::hdr {

// How many times SDR white the top of the map reaches. Written once on the GUI
// thread when the swapchain is known and read on the render thread every frame.
void set_headroom(float headroom);
[[nodiscard]] float headroom();

class MapMaterial : public QSGMaterial {
public:
    explicit MapMaterial(bool translucent);

    [[nodiscard]] QSGMaterialType* type() const override;
    [[nodiscard]] QSGMaterialShader* createShader(
        QSGRendererInterface::RenderMode mode) const override;
    [[nodiscard]] int compare(const QSGMaterial* other) const override;

    void setTexture(QSGTexture* texture) { texture_ = texture; }
    [[nodiscard]] QSGTexture* texture() const { return texture_; }

    void setFiltering(QSGTexture::Filtering filtering) { filtering_ = filtering; }
    [[nodiscard]] QSGTexture::Filtering filtering() const { return filtering_; }

private:
    QSGTexture* texture_ = nullptr;
    QSGTexture::Filtering filtering_ = QSGTexture::Nearest;
};

// A textured rectangle drawn through MapMaterial: what a QSGImageNode is for
// the waterfall, with the texture not owned.
class MapImageNode : public QSGGeometryNode {
public:
    MapImageNode();

    void setTexture(QSGTexture* texture);
    void setFiltering(QSGTexture::Filtering filtering);

    // Source in texture pixels, as QSGImageNode::setSourceRect takes it.
    void place(const QRectF& rect, const QRectF& source);

private:
    QSGGeometry geometry_;
    MapMaterial material_;
    QRectF rect_;
    QRectF source_;  // normalised
};

// A map image node for this window: MapImageNode on the hardware scene graph,
// a QSGImageNode not owning its texture on the software one. Nearest filtering
// either way.
[[nodiscard]] QSGNode* make_map_image(QQuickWindow* window, QSGTexture* texture);

// Points a node from make_map_image() at a texture and a place.
void place_map_image(QSGNode* node, QSGTexture* texture, const QRectF& rect,
                     const QRectF& source);

}  // namespace revenant::ui::hdr
