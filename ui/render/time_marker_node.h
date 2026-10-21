// The scene graph half of render/time_markers.h, shared by both waterfalls.
//
// The labels are laid out on the GUI thread when a marker first appears and
// handed to text nodes on the render thread, the split the span waterfall's
// captions use and for their reason: the render thread then needs no font
// engine of its own. A frame only moves the label nodes; the text is rebuilt
// when the set of markers on screen changes, which is once per interval.

#pragma once

#include <QDateTime>
#include <QFont>
#include <QFontMetricsF>
#include <QMatrix4x4>
#include <QQuickWindow>
#include <QSGNode>
#include <QSGTextNode>
#include <QTextLayout>
#include <QTextLine>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "render/spectrum_item.h"
#include "render/time_markers.h"

namespace revenant::ui {

// The captions' face and size, render/waterfall_item.cpp, so the two kinds of
// text on the waterfall read as one.
[[nodiscard]] inline QFont time_marker_font()
{
    QFont font(QStringLiteral("Cascadia Mono"));
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(10);
    return font;
}

inline constexpr double kTimeMarkerTickPx = 10.0;
inline constexpr double kTimeMarkerPadPx = 2.0;

// GUI thread: one laid-out label per marker on screen.
class TimeMarkerLabels {
public:
    TimeMarkerLabels() : line_height_(QFontMetricsF(time_marker_font()).height()) {}

    // Lays out the markers not yet held and forgets the ones gone off screen.
    void sync(const std::vector<std::pair<int, std::int64_t>>& visible)
    {
        bool changed = false;
        const auto shown = [&visible](std::int64_t ms) {
            return std::any_of(visible.begin(), visible.end(),
                               [ms](const auto& one) { return one.second == ms; });
        };
        const auto before = labels_.size();
        std::erase_if(labels_, [&shown](const Label& label) { return !shown(label.ms); });
        changed = labels_.size() != before;
        for (const auto& one : visible) {
            if (find(one.second) != nullptr) {
                continue;
            }
            const QString text =
                QDateTime::fromMSecsSinceEpoch(one.second).toString(QStringLiteral("HH:mm:ss"));
            auto layout = std::make_unique<QTextLayout>(text, time_marker_font());
            layout->beginLayout();
            QTextLine line = layout->createLine();
            line.setPosition(QPointF(0.0, 0.0));
            layout->endLayout();
            const double wide = line.naturalTextWidth();
            labels_.push_back(Label{one.second, std::move(layout), wide});
            changed = true;
        }
        if (changed) {
            generation_ += 1;
        }
    }

    void clear()
    {
        if (!labels_.empty()) {
            labels_.clear();
            generation_ += 1;
        }
    }

    struct Label {
        std::int64_t ms = 0;
        std::unique_ptr<QTextLayout> layout;
        double width = 0.0;
    };

    [[nodiscard]] const Label* find(std::int64_t ms) const
    {
        const auto at = std::find_if(labels_.begin(), labels_.end(),
                                     [ms](const Label& label) { return label.ms == ms; });
        return at == labels_.end() ? nullptr : &*at;
    }

    [[nodiscard]] const std::vector<Label>& labels() const { return labels_; }
    [[nodiscard]] std::uint64_t generation() const { return generation_; }
    [[nodiscard]] double lineHeight() const { return line_height_; }

private:
    std::vector<Label> labels_;
    std::uint64_t generation_ = 0;
    double line_height_ = 12.0;
};

// Render thread: the ticks and plates as one batch of quads, then each
// label's glyphs under a transform for where it is this frame.
class TimeMarkerNode : public QSGNode {
public:
    TimeMarkerNode()
    {
        quads = new OverlayNode;
        texts = new QSGNode;
        appendChildNode(quads);
        appendChildNode(texts);
    }

    // Rows are display rows from the top; the ring holds `rows` of them over
    // `height` logical pixels.
    void sync(QQuickWindow* window, const TimeMarkerLabels& labels,
              const std::vector<std::pair<int, std::int64_t>>& visible, int rows, double height)
    {
        if (generation_ != labels.generation()) {
            while (QSGNode* child = texts->firstChild()) {
                texts->removeChildNode(child);
                delete child;
            }
            places_.clear();
            for (const TimeMarkerLabels::Label& label : labels.labels()) {
                QSGTextNode* text = window->createTextNode();
                text->setColor(kInk);
                text->addTextLayout(QPointF(0.0, 0.0), label.layout.get());
                auto* place = new QSGTransformNode;
                place->appendChildNode(text);
                texts->appendChildNode(place);
                places_.emplace_back(label.ms, place);
            }
            generation_ = labels.generation();
        }

        std::vector<OverlayQuad> drawn;
        drawn.reserve(visible.size() * 2);
        const double line = labels.lineHeight();
        for (const auto& [display_row, ms] : visible) {
            const double y = rows <= 0 ? 0.0
                                       : height * static_cast<double>(display_row) /
                                             static_cast<double>(rows);
            drawn.push_back(OverlayQuad{QRectF(0.0, y, kTimeMarkerTickPx, 1.0), kInk});
            const TimeMarkerLabels::Label* label = labels.find(ms);
            const auto place = std::find_if(places_.begin(), places_.end(),
                                            [ms](const auto& one) { return one.first == ms; });
            if (label == nullptr || place == places_.end()) {
                continue;
            }
            // Under the tick, so the tick marks the row and the label reads
            // as belonging to it; the plate keeps it legible over a bright row.
            const double x = kTimeMarkerTickPx + kTimeMarkerPadPx;
            const double top = y + 1.0;
            drawn.push_back(OverlayQuad{
                QRectF(x - kTimeMarkerPadPx, top, label->width + 2.0 * kTimeMarkerPadPx, line),
                kPlate});
            QMatrix4x4 at;
            at.translate(static_cast<float>(x), static_cast<float>(top));
            if (place->second->matrix() != at) {
                place->second->setMatrix(at);
            }
        }
        quads->setQuads(drawn);
    }

    OverlayNode* quads = nullptr;
    QSGNode* texts = nullptr;

private:
    // The span waterfall's caption ink and plate, render/waterfall_item.cpp.
    inline static const QColor kInk{0xc6, 0xd0, 0xdd, 220};
    inline static const QColor kPlate{6, 8, 16, 200};

    std::uint64_t generation_ = ~std::uint64_t{0};
    std::vector<std::pair<std::int64_t, QSGTransformNode*>> places_;
};

}  // namespace revenant::ui
