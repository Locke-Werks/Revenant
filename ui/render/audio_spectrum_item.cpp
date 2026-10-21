#include "render/audio_spectrum_item.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

#include <QQuickWindow>
#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGNode>
#include <QSGRectangleNode>

namespace revenant::ui {
namespace {

// The passband trace's colour, so the strip reads as the same receiver.
const QColor kTraceColour{150, 226, 255, 255};
const QColor kFillColour{150, 226, 255, 56};
const QColor kTickColour{230, 232, 240, 170};

// One strip node, either the filled area or the line over it, in one flat
// colour. The fill is a triangle strip down to the bottom edge.
class StripNode : public QSGGeometryNode {
public:
    StripNode(bool filled, const QColor& colour)
        : geometry_(QSGGeometry::defaultAttributes_Point2D(), 0), filled_(filled)
    {
        geometry_.setDrawingMode(filled ? QSGGeometry::DrawTriangleStrip
                                        : QSGGeometry::DrawLineStrip);
        geometry_.setLineWidth(1.0F);
        material_.setColor(colour);
        setGeometry(&geometry_);
        setMaterial(&material_);
    }

    StripNode(const StripNode&) = delete;
    StripNode& operator=(const StripNode&) = delete;
    StripNode(StripNode&&) = delete;
    StripNode& operator=(StripNode&&) = delete;

    void setTrace(std::span<const float> levels, qreal width_px, qreal height_px)
    {
        const auto columns = static_cast<int>(levels.size());
        geometry_.allocate(filled_ ? 2 * columns : columns);
        if (columns <= 0) {
            markDirty(QSGNode::DirtyGeometry);
            return;
        }
        QSGGeometry::Point2D* vertex = geometry_.vertexDataAsPoint2D();
        const qreal step = width_px / static_cast<qreal>(columns);
        for (int column = 0; column < columns; ++column) {
            const auto x = static_cast<float>((static_cast<qreal>(column) + 0.5) * step);
            const auto y = static_cast<float>(
                height_px * (1.0 - static_cast<qreal>(levels[static_cast<std::size_t>(column)])));
            if (filled_) {
                vertex[2 * column].set(x, static_cast<float>(height_px));
                vertex[2 * column + 1].set(x, y);
            } else {
                vertex[column].set(x, y);
            }
        }
        markDirty(QSGNode::DirtyGeometry);
    }

private:
    QSGGeometry geometry_;
    QSGFlatColorMaterial material_;
    bool filled_;
};

class AudioStripNode : public QSGNode {
public:
    QSGRectangleNode* background = nullptr;
    StripNode* fill = nullptr;
    StripNode* line = nullptr;
    OverlayNode* ticks = nullptr;
};

}  // namespace

AudioSpectrumItem::AudioSpectrumItem(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

void AudioSpectrumItem::setLink(EngineLink* link)
{
    if (link_ == link) {
        return;
    }
    if (link_ != nullptr) {
        disconnect(link_, nullptr, this, nullptr);
    }
    link_ = link;
    if (link_ != nullptr) {
        connect(link_, &EngineLink::passbandChanged, this, &AudioSpectrumItem::takeFrame);
    }
    row_.clear();
    scale_ = AudioStripScale{};
    setActive(false, 0.0);
    rebuild();
    emit linkChanged();
    update();
}

void AudioSpectrumItem::setMaxHz(double hz)
{
    if (hz == max_hz_) {
        return;
    }
    max_hz_ = hz;
    emit maxHzChanged();
    rebuild();
    update();
}

void AudioSpectrumItem::setNotchHz(const QVariantList& hz)
{
    if (hz == notch_hz_) {
        return;
    }
    notch_hz_ = hz;
    emit notchHzChanged();
    rebuild();
    update();
}

void AudioSpectrumItem::setActive(bool active, double top_hz)
{
    if (active == active_ && top_hz == top_hz_) {
        return;
    }
    active_ = active;
    top_hz_ = top_hz;
    emit activeChanged();
}

void AudioSpectrumItem::takeFrame()
{
    if (link_ == nullptr) {
        return;
    }
    const rpc::PassbandFrame& frame = link_->passbandFrame();
    if (frame.audio_power_db.empty() || frame.audio_bin_hz <= 0.0) {
        // A receiver change empties the frame, and a scale carried over would
        // be the old receiver's.
        if (!row_.empty()) {
            row_.clear();
            scale_ = AudioStripScale{};
        }
        setActive(false, 0.0);
        rebuild();
        update();
        return;
    }
    row_.assign(frame.audio_power_db.begin(), frame.audio_power_db.end());
    bin_hz_ = frame.audio_bin_hz;
    frame_ticks_.assign(frame.audio_notch_hz.begin(), frame.audio_notch_hz.end());

    const double top = audio_strip_top_hz(bin_hz_, row_.size(), max_hz_);
    // The range is taken over what is shown, so a bandwidth below Nyquist
    // does not let the roll-off above it set the floor.
    const auto shown =
        std::min(row_.size(), static_cast<std::size_t>(std::floor(top / bin_hz_)) + 1);
    scale_.update(std::span<const float>(row_).first(shown), scale_scratch_);
    setActive(true, top);
    rebuild();
    update();
}

void AudioSpectrumItem::rebuild()
{
    tick_quads_.clear();
    const double w = width();
    const double h = height();
    if (row_.empty() || w <= 0.0 || h <= 0.0 || top_hz_ <= 0.0) {
        columns_.clear();
        levels_.clear();
        return;
    }
    const QQuickWindow* const host = window();
    const qreal ratio = host == nullptr ? 1.0 : host->effectiveDevicePixelRatio();
    const auto count = static_cast<std::size_t>(std::max(std::lround(w * ratio), 1L));
    columns_.resize(count);
    audio_strip_columns(row_, bin_hz_, top_hz_, columns_);
    levels_.resize(count);
    for (std::size_t c = 0; c < count; ++c) {
        levels_[c] = scale_.level(columns_[c]);
    }

    const auto tick = [&](double hz) {
        if (hz < 0.0 || hz > top_hz_) {
            return;
        }
        const double x = audio_strip_x(hz, top_hz_, w);
        tick_quads_.push_back(OverlayQuad{QRectF(x - 0.5, 0.0, 1.0, h), kTickColour});
    };
    if (!notch_hz_.isEmpty()) {
        for (const QVariant& hz : notch_hz_) {
            tick(hz.toDouble());
        }
    } else {
        for (const double hz : frame_ticks_) {
            tick(hz);
        }
    }
}

void AudioSpectrumItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
    QQuickItem::geometryChange(new_geometry, old_geometry);
    if (new_geometry.size() != old_geometry.size()) {
        rebuild();
        update();
    }
}

QSGNode* AudioSpectrumItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
    const qreal w = width();
    const qreal h = height();
    if (w <= 0.0 || h <= 0.0) {
        delete old_node;
        return nullptr;
    }
    auto* node = static_cast<AudioStripNode*>(old_node);
    if (node == nullptr) {
        node = new AudioStripNode;
        node->background = window()->createRectangleNode();
        node->background->setColor(QColor(8, 10, 14));
        node->fill = new StripNode(true, kFillColour);
        node->line = new StripNode(false, kTraceColour);
        node->ticks = new OverlayNode;
        node->appendChildNode(node->background);
        node->appendChildNode(node->fill);
        node->appendChildNode(node->line);
        node->appendChildNode(node->ticks);
    }
    node->background->setRect(0.0, 0.0, w, h);
    node->fill->setTrace(levels_, w, h);
    node->line->setTrace(levels_, w, h);
    node->ticks->setQuads(tick_quads_);
    return node;
}

}  // namespace revenant::ui
