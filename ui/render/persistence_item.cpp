#include "render/persistence_item.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGNode>
#include <QSGTexture>

#include "render/spectrum_scale.h"
#include "render/waterfall_scale.h"

namespace revenant::ui {

namespace {

// How finely a cell's share of the peak is quantised on its way to a colour.
// The shade is logarithmic, so this is finer than the eye can split it.
constexpr int kShadeSteps = 1024;

// Owns the texture it shows, because a texture handed to the image node and
// replaced next frame has to be freed after the node has let go of it; see
// the note in WaterfallItem::updatePaintNode for the crash that taught that.
class PersistenceNode : public QSGNode {
public:
    ~PersistenceNode() override { delete texture; }

    QSGImageNode* image = nullptr;
    QSGTexture* texture = nullptr;
};

}  // namespace

PersistenceItem::PersistenceItem(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

void PersistenceItem::setLink(EngineLink* link)
{
    if (link_ == link) {
        return;
    }
    if (link_ != nullptr) {
        disconnect(link_, nullptr, this, nullptr);
    }
    link_ = link;
    if (link_ != nullptr) {
        connect(link_, &EngineLink::frameChanged, this, &PersistenceItem::takeFrame);
        // A new engine or a granted retune: every hit was drawn against an
        // axis that is not this one.
        connect(link_, &EngineLink::connectionChanged, this, &PersistenceItem::clearHistory);
    }
    clearHistory();
    emit linkChanged();
}

void PersistenceItem::setMapPins(ScaleSettings* pins)
{
    if (map_pins_ == pins) {
        return;
    }
    if (map_pins_ != nullptr) {
        disconnect(map_pins_, nullptr, this, nullptr);
    }
    map_pins_ = pins;
    if (map_pins_ != nullptr) {
        const auto recolour = [this] {
            buildImage();
            update();
        };
        connect(map_pins_, &ScaleSettings::waterfallContrastChanged, this, recolour);
        connect(map_pins_, &ScaleSettings::waterfallPivotChanged, this, recolour);
    }
    emit mapPinsChanged();
}

void PersistenceItem::setFloorDb(double db)
{
    if (db == floor_db_) {
        return;
    }
    floor_db_ = db;
    emit endsChanged();
}

void PersistenceItem::setCeilingDb(double db)
{
    if (db == ceiling_db_) {
        return;
    }
    ceiling_db_ = db;
    emit endsChanged();
}

void PersistenceItem::setDecaySeconds(double seconds)
{
    seconds = clamp_persistence_decay(seconds);
    if (seconds == decay_seconds_) {
        return;
    }
    decay_seconds_ = seconds;
    emit decaySecondsChanged();
}

void PersistenceItem::itemChange(ItemChange change, const ItemChangeData& value)
{
    // Hidden, it takes no frames, so what it shows on being shown again is a
    // picture from whenever it was last looked at. Starting clean is honest.
    if (change == ItemVisibleHasChanged && value.boolValue) {
        clearHistory();
    }
    QQuickItem::itemChange(change, value);
}

void PersistenceItem::clearHistory()
{
    grid_.clear();
    last_sample_end_ = 0;
    buildImage();
    update();
}

int PersistenceItem::wantedColumns() const
{
    const QQuickWindow* const host = window();
    const qreal ratio = host == nullptr ? 1.0 : host->effectiveDevicePixelRatio();
    const int device = static_cast<int>(std::lround(width() * ratio));
    return std::clamp(device, 1, kPersistenceMaxColumns);
}

void PersistenceItem::takeFrame()
{
    if (link_ == nullptr || !isVisible() || width() <= 0.0 || height() <= 0.0) {
        return;
    }
    const rpc::SpectrumFrame& frame = link_->frame();
    if (frame.power_db.empty()) {
        return;
    }

    const int wide = wantedColumns();
    if (grid_.columns() != wide || grid_.rows() != kPersistenceRows) {
        grid_.resize(wide, kPersistenceRows);
        columns_.assign(static_cast<std::size_t>(wide), kSpectrumFloorDb);
        last_sample_end_ = 0;
    }

    const ViewBins drawn = link_->viewBins(frame.power_db.size());
    const std::span<const float> shown =
        std::span<const float>(frame.power_db).subspan(drawn.first, drawn.count());
    reduce_peak(shown, columns_);

    // The fade runs in source time, as the waterfall's scale does, so a
    // capture replayed fast fades as it would have live. A gap or a first
    // frame counts as its own length, and a stall is held to a second.
    const std::uint64_t frame_end = frame.start + frame.count;
    const int rate = link_->sourceRate();
    double seconds = 0.0;
    if (rate > 0) {
        const std::uint64_t elapsed = frame_end > last_sample_end_ && last_sample_end_ != 0
                                          ? frame_end - last_sample_end_
                                          : frame.count;
        seconds = std::min(static_cast<double>(elapsed) / static_cast<double>(rate), 1.0);
    }
    last_sample_end_ = frame_end;

    grid_.decay(seconds, decay_seconds_);
    grid_.add_trace(columns_, static_cast<float>(floor_db_), static_cast<float>(ceiling_db_));
    buildImage();
    update();
}

void PersistenceItem::buildImage()
{
    const int wide = std::max(grid_.columns(), 1);
    const int tall = std::max(grid_.rows(), 1);
    if (image_.width() != wide || image_.height() != tall) {
        image_ = QImage(wide, tall, QImage::Format_RGBX8888);
    }

    const std::uint32_t background = pack_rgbx(colour_map::kBottom);
    const float peak = grid_.peak();
    if (grid_.columns() == 0 || !(peak > 0.0F)) {
        image_.fill(QColor(colour_map::kBottom.r, colour_map::kBottom.g, colour_map::kBottom.b));
        image_dirty_ = true;
        return;
    }

    // The colour of a share of the peak, once per frame rather than a
    // logarithm per cell.
    const ContrastCurve curve =
        map_pins_ == nullptr ? ContrastCurve{}
                             : curve_for(static_cast<float>(map_pins_->waterfallContrast()),
                                         static_cast<float>(map_pins_->waterfallPivot()));
    const PackedPalette& palette = packed_palette();
    std::array<std::uint32_t, kShadeSteps> shades{};
    shades[0] = background;
    for (int i = 1; i < kShadeSteps; ++i) {
        const float density = peak * static_cast<float>(i) / static_cast<float>(kShadeSteps - 1);
        const float shade = apply_curve(curve, PersistenceGrid::shade(density, peak));
        const auto index = static_cast<std::size_t>(
            std::lround(shade * static_cast<float>(colour_map::kTableSize - 1)));
        shades[static_cast<std::size_t>(i)] = palette[index];
    }

    const float to_step = static_cast<float>(kShadeSteps - 1) / peak;
    for (int r = 0; r < grid_.rows(); ++r) {
        auto* line = reinterpret_cast<std::uint32_t*>(image_.scanLine(r));
        for (int c = 0; c < grid_.columns(); ++c) {
            const float density = grid_.at(c, r);
            if (!(density > 0.0F)) {
                line[c] = background;
                continue;
            }
            // Never step zero for a cell that holds something: a single old
            // hit is the whole point of the display.
            const int step = std::clamp(static_cast<int>(std::ceil(density * to_step)), 1,
                                        kShadeSteps - 1);
            line[c] = shades[static_cast<std::size_t>(step)];
        }
    }
    image_dirty_ = true;
}

QSGNode* PersistenceItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
    if (width() <= 0.0 || height() <= 0.0 || image_.isNull()) {
        delete old_node;
        return nullptr;
    }
    auto* node = static_cast<PersistenceNode*>(old_node);
    if (node == nullptr) {
        node = new PersistenceNode;
        node->image = window()->createImageNode();
        node->image->setOwnsTexture(false);
        // Linear: the grid is stretched to the item, and a density picture
        // reads better blended than blocky.
        node->image->setFiltering(QSGTexture::Linear);
        image_dirty_ = true;
    }

    QSGTexture* retired = nullptr;
    if (image_dirty_ || node->texture == nullptr) {
        retired = node->texture;
        // Copied, because the GUI thread writes the next frame into image_
        // while the render thread may still be uploading this one.
        node->texture = window()->createTextureFromImage(image_.copy());
        image_dirty_ = false;
    }
    node->image->setTexture(node->texture);
    node->image->setRect(QRectF(0.0, 0.0, width(), height()));
    node->image->setSourceRect(QRectF(0.0, 0.0, image_.width(), image_.height()));
    if (node->image->parent() == nullptr) {
        node->appendChildNode(node->image);
    }
    delete retired;
    return node;
}

}  // namespace revenant::ui
