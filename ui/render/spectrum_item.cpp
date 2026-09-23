#include "render/spectrum_item.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <utility>

#include <QBrush>
#include <QColor>
#include <QCursor>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QQuickWindow>
#include <QRectF>
#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <QSGRectangleNode>
#include <QSGTexture>
#include <QSGTextureMaterial>
#include <QString>
#include <QWheelEvent>

#include "models/frame_stats.h"
#include "models/label_tune.h"
#include "models/receiver_palette.h"

namespace revenant::ui {
namespace {

[[nodiscard]] QColor colour_of(float level)
{
    const Rgb rgb = colour_at(level);
    return QColor(rgb.r, rgb.g, rgb.b);
}

// Magenta, because the colour map in render/spectrum_scale.cpp runs dark
// blue through teal and yellow-green to cream and never produces this hue.
// An overlay in a colour the map can make is read as a level.
[[nodiscard]] QColor detection_hue(rpc::TrackState state)
{
    switch (state) {
        case rpc::TrackState::Merged:
            // Inside another track's band. The hue carries that, because it
            // is a statement about where the track is rather than about how
            // fresh it is, and the two are independent.
            //
            // It used to carry the freshness as well, on the grounds that
            // core/detect/detector.h says a merged track has evidence and
            // does not decay. That conflated two things. What does not
            // decay is its CONFIDENCE: core/detect/detector.cpp skips both
            // the miss count and the confidence decay for a merged track.
            // Its last_detected still ages exactly like anything else's,
            // and measured against a bursty scene a merged track's trails
            // the newest frame by 50 rows at the median and 110 at the
            // worst, the same distribution as a held one. So it fades on
            // silence with the rest, and only the hue is special.
            return QColor(154, 108, 255);
        case rpc::TrackState::Pending:
        case rpc::TrackState::Live:
        case rpc::TrackState::Held:
            break;
    }
    return QColor(255, 88, 200);
}

// EACH RECEIVER'S MARKER WEARS ITS RACK COLOUR
//
// models/receiver_palette.h has the eight and why they are those eight; the
// strip in the rack, the ruler's band and this marker all take the slot's
// colour from there, so the three read as one receiver. The first is the
// azure this marker was drawn in before there was a rack, which
// render/passband_item.cpp's fill still uses.
//
// The fill is faint and the edges are strong on the focused receiver, which
// is the one the controls act on. A held receiver gets a fainter fill, its
// bar and its tick, and no edge rules: eight sets of full-height rules on a
// crowded band would be a fence in front of the signals.
constexpr int kReceiverFillAlpha = 42;
constexpr int kHeldFillAlpha = 26;
constexpr int kReceiverEdgeAlpha = 220;
constexpr int kHeldBarAlpha = 170;
constexpr int kHeldCentreAlpha = 110;

[[nodiscard]] QColor receiver_qcolour(std::size_t slot, int alpha)
{
    const Rgb8 c = receiver_colour(slot);
    return QColor(c.r, c.g, c.b, alpha);
}

// The bar along the bottom edge of the band, which is what makes a narrow
// filter findable. A 16 kHz receiver on a 2.4 MHz span is ten pixels wide and
// a 500 Hz CW filter is a third of one, so the fill alone is invisible at the
// widths that matter and the two edge rules collapse into one mark.
//
// The BOTTOM edge and not the top. The detection overlay owns the top strip:
// its brackets are drawn from y = 0 and its labels are painted from
// kLabelTopPx down. A receiver bar up there would sit under a plate naming
// some other signal.
constexpr int kReceiverBarAlpha = 235;

// The tuned frequency. White because it has to stay legible against the fill
// and both rules, and faint because it is the extra rather than the point:
// the band is what says whether the filter covers the signal.
const QColor kReceiverCentre{255, 255, 255, 150};

constexpr double kReceiverEdgeWidthPx = 2.0;
constexpr double kReceiverBarHeightPx = 3.0;

// How far the centre tick rises off the bottom bar. Short, so it says which
// side of the carrier an asymmetric filter is on without drawing a third
// full-height line inside a band ten pixels wide.
constexpr double kReceiverCentreTickPx = 9.0;

[[nodiscard]] int scaled_alpha(int full, double fade)
{
    return std::clamp(static_cast<int>(std::lround(static_cast<double>(full) * fade)), 0, 255);
}

[[nodiscard]] QColor with_alpha(QColor colour, int alpha)
{
    colour.setAlpha(alpha);
    return colour;
}

// Megahertz to four places, a tenth of a kilohertz, which is enough to find
// the box on the ruler under the spectrum. The exact hertz goes in the
// window's click readout, where there is room for it and where somebody is
// about to act on it.
//
// WHAT THIS PARAGRAPH USED TO SAY. It began "Megahertz to four places, which
// is the axis label's own precision in qml/Main.qml, so a box and the tick
// under it are read in the same units." The axis is now qml/Ruler.qml, whose
// labels carry as many decimals as their 1-2-5 step needs and no more, so on
// a broadcast span they read to a tenth of a megahertz and the two do not
// share a precision. They share a unit, which is what the reader matches.
//
// SINCE 2026-09-23 A LABELLED BOX IS NAMED FOR WHAT IT IS: "P25", "NFM".
// models/label_tune.h's bracket_text writes it and says why the name stands
// alone; the frequency and the SNR go to the hover card, which is where the
// owner's design pass puts the full detail. An unlabelled box reads as it did.
[[nodiscard]] QString box_label(const DetectionBox& box)
{
    const bool named = box.label.kind != rpc::LabelKind::Unknown && !box.label.name.empty();
    const QString megahertz =
        QString::number(static_cast<double>(box.center_hz) / 1.0e6, 'f', 4);
    const QString lead = named ? QString::fromStdString(box.label.name) : megahertz;

    switch (box.state) {
        case rpc::TrackState::Held:
            return lead + QStringLiteral("  held ") +
                   QString::number(box.silent_seconds, 'f', 1) + QStringLiteral(" s");
        case rpc::TrackState::Merged:
            return lead + QStringLiteral("  merged");
        case rpc::TrackState::Pending:
        case rpc::TrackState::Live:
            break;
    }
    return QString::fromStdString(bracket_text(box.label, box.center_hz, box.snr_2500_db));
}

// THE PLATES ARE SET IN THE THEME'S MONOSPACE, Theme.monoFont at
// Theme.sizeSmall, so a column of frequencies lines up digit under digit and
// a label reads the same on the bracket as in the hover card beside it. Named
// here rather than read from the QML singleton, which a painted item cannot
// see; qml/Theme.qml names this function beside the two properties.
[[nodiscard]] QFont overlay_label_font()
{
    QFont font(QStringLiteral("Cascadia Mono"));
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(11);
    return font;
}

// WHAT THE LABEL USED TO END WITH: the track's confidence to two places, after
// the decibels. That number is a stopwatch, one minus (1 - rise) to the n of
// consecutive detections, and it reads 1.00 on anything that has stayed put
// for about a second and a half, a bump in the noise floor included; the owner
// read it on 2026-09-22 as the detector being certain of a bump. The label
// keeps the frequency and the SNR, which is the calibrated number, and the
// stopwatch goes with the margin and the concentration to the hover card in
// qml/SpanView.qml, where each is named for what it measures.

// THE LABEL DOES NOT FADE, WHICH IS A REVERSAL
//
// The hold fade used to be applied to the plate and the text as well as to
// the box. At the end of a hold that is kDetectionFadeFloor times the two
// alphas below, so the plate reached 40 of 255 and the text 51, and what the
// text says at that moment is how long the track has been silent. That is
// exactly when the number is worth reading, because it is the warning that
// the box is about to go. A reading that dims as it becomes urgent is
// backwards.
//
// So the fade says how much evidence the detector still has, and it says it
// on the thing that represents the evidence, which is the box. What the box
// is called stays legible for as long as the box is on screen.
void paint_label(QPainter& painter, const QString& text, double centre_x, double width_px,
                 const QColor& ink)
{
    const QFontMetricsF metrics(painter.font());
    const double text_width = metrics.horizontalAdvance(text);
    const double plate_width = text_width + 8.0;
    const double plate_height = metrics.height() + 2.0;

    // Centred on the box, then pulled back inside the item rather than
    // clipped. The plates in qml/SpanView.qml follow the same rule; the
    // ruler's labels do not, because a ruler label pulled in lands on its
    // neighbour, so models/ruler.h drops one that would not fit instead.
    const double x =
        std::clamp(centre_x - plate_width / 2.0, 0.0, std::max(0.0, width_px - plate_width));

    QColor plate(6, 8, 16);
    plate.setAlpha(200);
    painter.setPen(Qt::NoPen);
    painter.setBrush(plate);
    painter.drawRect(QRectF(x, 0.0, plate_width, plate_height));

    QColor pen_colour = ink;
    pen_colour.setAlpha(255);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(pen_colour);
    painter.drawText(QRectF(x, 0.0, plate_width, plate_height), Qt::AlignCenter, text);
}

void push_quad(std::vector<OverlayQuad>& out, const QRectF& rect, const QColor& colour)
{
    if (rect.width() <= 0.0 || rect.height() <= 0.0 || colour.alpha() == 0) {
        return;
    }
    out.push_back(OverlayQuad{rect, colour});
}

// A hairline rectangle, as four quads. The scene graph has no stroke: a line
// is a thin filled rectangle, and drawing one as a rectangle with a border
// would be a second mesh for the same pixels.
void push_outline(std::vector<OverlayQuad>& out, const QRectF& rect, double thickness,
                  const QColor& colour)
{
    if (rect.width() <= 0.0 || rect.height() <= 0.0) {
        return;
    }
    const double t = std::min(thickness, std::min(rect.width(), rect.height()));
    push_quad(out, QRectF(rect.left(), rect.top(), rect.width(), t), colour);
    push_quad(out, QRectF(rect.left(), rect.bottom() - t, rect.width(), t), colour);
    push_quad(out, QRectF(rect.left(), rect.top(), t, rect.height()), colour);
    push_quad(out, QRectF(rect.right() - t, rect.top(), t, rect.height()), colour);
}

// The item's background, and the gradient under the trace, as one 1 by 256
// image. A vertical gradient is a function of y alone, so it is a texture
// coordinate rather than a colour per vertex: interpolating colour between
// the trace and the bottom of the item would stretch the whole map into
// whatever height that column happens to have, which makes a quiet column's
// fill as bright as a loud one's.
[[nodiscard]] QImage gradient_image()
{
    QImage image(1, 256, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < 256; ++y) {
        const float level = 1.0F - static_cast<float>(y) / 255.0F;
        const Rgb rgb = colour_at(level);

        // Translucent, because the trace itself has to stay legible where it
        // crosses the bright end. Premultiplied because that is what the
        // scene graph's texture materials expect.
        constexpr int kAlpha = 140;
        const auto premultiplied = [](std::uint8_t channel) {
            return (static_cast<int>(channel) * kAlpha + 127) / 255;
        };
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        row[0] = qRgba(premultiplied(rgb.r), premultiplied(rgb.g), premultiplied(rgb.b),
                       kAlpha);
    }
    return image;
}

// The fill under the trace: one triangle strip, two vertices per column, with
// the gradient sampled by y.
class SpectrumFillNode : public QSGGeometryNode {
public:
    SpectrumFillNode()
        : geometry_(QSGGeometry::defaultAttributes_TexturedPoint2D(), 0)
    {
        geometry_.setDrawingMode(QSGGeometry::DrawTriangleStrip);
        material_.setFiltering(QSGTexture::Linear);
        setGeometry(&geometry_);
        setMaterial(&material_);
    }

    ~SpectrumFillNode() override { delete gradient_; }

    SpectrumFillNode(const SpectrumFillNode&) = delete;
    SpectrumFillNode& operator=(const SpectrumFillNode&) = delete;
    SpectrumFillNode(SpectrumFillNode&&) = delete;
    SpectrumFillNode& operator=(SpectrumFillNode&&) = delete;

    // The texture belongs to this node so the scene graph destroys it on the
    // render thread when the tree goes away, which is the one thread allowed
    // to destroy a QSGTexture.
    void ensureGradient(QQuickWindow* host)
    {
        if (gradient_ != nullptr || host == nullptr) {
            return;
        }
        gradient_ = host->createTextureFromImage(gradient_image());
        material_.setTexture(gradient_);
    }

    void setTrace(std::span<const float> levels, qreal width_px, qreal height_px)
    {
        const auto columns = static_cast<int>(levels.size());
        if (columns <= 0 || gradient_ == nullptr) {
            geometry_.allocate(0);
            markDirty(QSGNode::DirtyGeometry);
            return;
        }

        // Two vertices more than there are columns, at x = 0 and x = width,
        // because the first and last columns are drawn at their centres and
        // the fill has to reach the edges of the item.
        geometry_.allocate((columns + 2) * 2);
        QSGGeometry::TexturedPoint2D* vertex = geometry_.vertexDataAsTexturedPoint2D();
        const qreal step = width_px / static_cast<qreal>(columns);
        const auto bottom = static_cast<float>(height_px);

        int at = 0;
        const auto put = [&](qreal x, float level) {
            const auto top = static_cast<float>(height_px * (1.0 - static_cast<qreal>(level)));
            vertex[at++].set(static_cast<float>(x), top, 0.5F, top / bottom);
            vertex[at++].set(static_cast<float>(x), bottom, 0.5F, 1.0F);
        };

        put(0.0, levels.front());
        for (int column = 0; column < columns; ++column) {
            put((static_cast<qreal>(column) + 0.5) * step,
                levels[static_cast<std::size_t>(column)]);
        }
        put(width_px, levels.back());

        markDirty(QSGNode::DirtyGeometry);
    }

private:
    QSGGeometry geometry_;
    QSGTextureMaterial material_;
    QSGTexture* gradient_ = nullptr;
};

// The trace itself: one line strip, one point per device column.
//
// NO ANTIALIASING, WHICH IS A REVERSAL AND WAS MEASURED
//
// This used to be drawn antialiased, with the reason that a polyline and a
// gradient fill both alias badly on a one-pixel feature and a one-bin carrier
// is exactly that feature. That was true while a column was a logical pixel:
// a segment then spanned 1.25 device pixels on the display this was checked
// on, so it had a diagonal to soften. One point per physical pixel leaves no
// diagonal. What antialiasing does to a vertical spike one pixel wide is
// spread it over two at half the brightness, which makes a narrow carrier
// harder to see rather than easier.
//
// The scene graph draws this aliased without being asked, so the render hint
// that used to say so every frame is gone with the painter.
class TraceNode : public QSGGeometryNode {
public:
    TraceNode() : geometry_(QSGGeometry::defaultAttributes_Point2D(), 0)
    {
        geometry_.setDrawingMode(QSGGeometry::DrawLineStrip);
        geometry_.setLineWidth(1.0F);
        material_.setColor(colour_of(1.0F));
        setGeometry(&geometry_);
        setMaterial(&material_);
    }

    void setTrace(std::span<const float> levels, qreal width_px, qreal height_px)
    {
        const auto columns = static_cast<int>(levels.size());
        geometry_.allocate(columns);
        if (columns <= 0) {
            markDirty(QSGNode::DirtyGeometry);
            return;
        }

        QSGGeometry::Point2D* vertex = geometry_.vertexDataAsPoint2D();
        const qreal step = width_px / static_cast<qreal>(columns);
        for (int column = 0; column < columns; ++column) {
            const float level = levels[static_cast<std::size_t>(column)];
            vertex[column].set(
                static_cast<float>((static_cast<qreal>(column) + 0.5) * step),
                static_cast<float>(height_px * (1.0 - static_cast<qreal>(level))));
        }
        markDirty(QSGNode::DirtyGeometry);
    }

private:
    QSGGeometry geometry_;
    QSGFlatColorMaterial material_;
};

// The item's own node, so the parts are named rather than fetched back out
// of the child list by index.
class SpectrumNode : public QSGNode {
public:
    QSGRectangleNode* background = nullptr;
    SpectrumFillNode* fill = nullptr;
    TraceNode* trace = nullptr;
    OverlayNode* overlay = nullptr;
};

}  // namespace

void build_detection_boxes(const EngineLink& link, double width_px,
                           std::vector<DetectionBox>& out)
{
    out.clear();
    if (width_px <= 0.0 || !link.connected()) {
        return;
    }

    // THE ONLY HERTZ TO PIXEL CONVERSION IN THE CLIENT, AND WHY IT IS THIS
    //
    // The axis is drawn from EngineLink::frequencyAtFraction, which is
    // affine in the fraction: it takes the fraction to a bin index and the
    // bin index through one rational multiply. spanLowHz and spanHighHz are
    // that same function evaluated at 0 and 1, so inverting between them is
    // not a second derivation of the mapping, it is the first one solved for
    // its argument. Reconstructing x from bin_zero and bin_width here would
    // be the second derivation, and the two would agree in the middle of the
    // span and part company at the ends, which is the failure that looks
    // correct wherever anybody thinks to check it.
    const double low_hz = link.spanLowHz();
    const double high_hz = link.spanHighHz();
    const double span_hz = high_hz - low_hz;
    if (!(span_hz > 0.0)) {
        return;
    }

    // Seconds come from the source sample rate and nothing else.
    // docs/detection.md and core/detect/detector.h both put the detector's
    // only clock on the sample index, which is what makes a replayed capture
    // decay at the rate it was recorded rather than the rate it is drawn.
    const double sample_rate = static_cast<double>(link.sourceRate());

    // The end of the newest frame, which is exactly the index the detector
    // calls "now": core/detect/detector.cpp takes frame.start + frame.count
    // as the decision instant. Using it here rather than the track's own
    // last_seen is what makes the fade move smoothly. last_seen advances once
    // per decision and the frame clock advances once per block, so the track
    // pair alone would step the fade a few times across the whole hold.
    const std::uint64_t frame_now = link.frame().start + link.frame().count;

    // The running detector's own hold, off DetectionList, so the fade
    // reaches its floor when the detector is about to drop the track and not
    // at whatever rate this process was compiled with. Zero is the engine
    // not having said, which is either before the first detections call has
    // come back or an engine older than the field, so the compiled-in
    // fallback covers both. See kDetectionHoldSeconds.
    const double reported_hold = link.detectorHoldSeconds();
    const double hold_seconds = reported_hold > 0.0 ? reported_hold : kDetectionHoldSeconds;

    const std::vector<rpc::Detection>& detections = link.detections();
    out.reserve(detections.size());

    for (const rpc::Detection& detection : detections) {
        DetectionBox box;
        box.id = detection.id;
        box.state = detection.state;
        box.center_hz = detection.center_hz;
        box.bandwidth_hz = detection.bandwidth_hz;
        box.snr_2500_db = detection.snr_2500_db;
        box.confidence = detection.confidence;
        box.first_seen = detection.first_seen;
        box.last_detected = detection.last_detected;
        box.label = detection.label;

        const double centre = static_cast<double>(detection.center_hz);
        const double half = static_cast<double>(detection.bandwidth_hz) / 2.0;
        box.center_px = (centre - low_hz) / span_hz * width_px;
        box.left_px = (centre - half - low_hz) / span_hz * width_px;
        box.right_px = (centre + half - low_hz) / span_hz * width_px;

        const bool ages =
            detection.state == rpc::TrackState::Held || detection.state == rpc::TrackState::Merged;
        if (ages && sample_rate > 0.0) {
            // Two clocks say how long this has been quiet and neither is
            // wrong. The detector's own pair is the authority and the frame
            // clock is the finer of the two, so the later of them moves at
            // the display's rate without ever claiming more silence than the
            // detector has actually observed at its last decision.
            const std::uint64_t by_track = detection.silent_samples();
            const std::uint64_t by_frame =
                frame_now > detection.last_detected ? frame_now - detection.last_detected : 0;
            box.silent_seconds = static_cast<double>(std::max(by_track, by_frame)) / sample_rate;

            const double remaining = 1.0 - box.silent_seconds / hold_seconds;
            box.fade = std::clamp(remaining, kDetectionFadeFloor, 1.0);
        }

        out.push_back(box);
    }
}

void build_detection_quads(const std::vector<DetectionBox>& boxes, double width_px,
                           double height_px, std::uint64_t selected_id,
                           std::uint64_t hovered_id, DetectionStyle style,
                           std::vector<OverlayQuad>& out)
{
    out.clear();
    if (boxes.empty() || width_px <= 0.0 || height_px <= 0.0) {
        return;
    }

    // A BRACKET PER TRACK AND A FILL ONLY FOR THE CHOSEN ONE, WHICH IS A
    // REVERSAL AND IS MEASURED
    //
    // This drew every track as a translucent band down the whole item, at 30
    // of 255 alpha, on the reasoning that a band says "this much bandwidth"
    // more plainly than two lines do. Against the RTL-SDR at 98.1 MHz with
    // the threshold at its 6 dB default the detector reported 155 tracks in
    // a 2.4 MHz span, most of them clustered, and twenty overlapping bands
    // at 30 alpha each composite to something indistinguishable from solid:
    // measured in an earlier session, the waterfall under the clusters was
    // not readable at all, which is the one thing the overlay must not do to
    // the display it is annotating.
    //
    // So: a filled bracket along the top edge, a notch at the centre, and
    // hairlines down the edges, all solid and all rectangles. The fill is
    // kept for the chosen and the hovered track alone, where there is one of
    // it and it is the thing being looked at.
    for (const DetectionBox& box : boxes) {
        const bool chosen = box.id != 0 && box.id == selected_id;
        const bool under_cursor = box.id != 0 && box.id == hovered_id;
        const QColor hue = detection_hue(box.state);

        // A box narrower than a pixel is still a detection and still has to
        // be visible. Widened for drawing only: the hit test and the tune
        // frequency both use the unwidened numbers, so a narrow carrier is
        // not reported as wider than it is.
        const double left = std::min(box.left_px, box.center_px - 0.5);
        const double right = std::max(box.right_px, box.center_px + 0.5);
        const double wide = right - left;

        switch (style) {
            case DetectionStyle::Band: {
                // The chosen box does not fade. The fade is a comparison
                // against the other boxes on screen, saying which of them the
                // detector still has evidence for, and a selection has
                // already answered that question: the operator said this is
                // the one being looked at. A held track the operator
                // explicitly picked used to dim to the fade floor like any
                // other, so the box they asked to watch was the faintest
                // thing on the display.
                const double fade = chosen ? 1.0 : box.fade;

                if (chosen || under_cursor) {
                    push_quad(out, QRectF(left, 0.0, wide, height_px),
                              with_alpha(hue, scaled_alpha(chosen ? 64 : 40, fade)));
                }

                // The bracket. Its height is the second thing that says live
                // from held, after the fade: three pixels of solid bar
                // against one. The fade alone is a comparison against the
                // other boxes on screen, and a lone held track has nothing to
                // be compared with.
                const double bracket = box.state == rpc::TrackState::Held ? 1.0 : 3.0;
                const QColor bar = with_alpha(hue, scaled_alpha(chosen ? 255 : 220, fade));
                push_quad(out, QRectF(left, 0.0, wide, bracket), bar);

                // The centre, which is what a click tunes to, marked so the
                // operator can see it is not necessarily where the energy is.
                // See the note on SpectrumItem::tuneRequested.
                push_quad(out, QRectF(box.center_px - 0.5, 0.0, 1.0, bracket + 5.0), bar);

                // Hairlines down the edges, which are what carry the
                // bandwidth over the trace. Skipped on a box too narrow to
                // have two distinct edges, where they would draw the same
                // pixel twice.
                if (wide >= 2.0) {
                    const QColor edge = with_alpha(hue, scaled_alpha(130, fade));
                    push_quad(out, QRectF(left, 0.0, 1.0, height_px), edge);
                    push_quad(out, QRectF(right - 1.0, 0.0, 1.0, height_px), edge);
                }

                if (chosen) {
                    // White rather than a brighter magenta. The selection has
                    // to be legible against a saturated carrier, which is
                    // where the colour map is already at its own brightest.
                    push_outline(out, QRectF(left - 1.0, 0.0, wide + 2.0, height_px), 1.0,
                                 QColor(255, 255, 255, scaled_alpha(230, fade)));
                }
                break;
            }

            case DetectionStyle::Rows: {
                // No rows means the track's evidence is older than anything
                // still on screen. Drawing it anywhere here would put a mark
                // over history the signal was not in; the spectrum above
                // still carries it as a frequency marker.
                if (!box.time_bounded) {
                    break;
                }

                const double top = box.top_px;
                const double tall = std::max(box.bottom_px - box.top_px, 1.0);
                const QRectF extent(left, top, wide, tall);

                // No fade down here, which is the other half of the split
                // this file's header describes. A held track's rectangle
                // already ends where the signal stopped, so dimming it would
                // say the same thing twice and hide the picture underneath
                // while doing it.
                if (chosen || under_cursor) {
                    push_quad(out, extent, with_alpha(hue, chosen ? 64 : 40));
                }

                const QColor edge = with_alpha(hue, chosen ? 255 : 170);
                if (wide >= 2.0) {
                    push_outline(out, extent, 1.0, edge);
                } else {
                    // Too narrow for four sides. One bar the height of the
                    // rectangle still says both of the things that matter:
                    // this frequency, these rows.
                    push_quad(out, QRectF(left, top, std::max(wide, 1.0), tall), edge);
                }

                if (chosen) {
                    push_outline(out,
                                 QRectF(left - 2.0, top - 2.0, wide + 4.0, tall + 4.0), 1.0,
                                 QColor(255, 255, 255, 230));
                }
                break;
            }
        }
    }
}

OverlayLabel detection_label(const DetectionBox& box)
{
    return OverlayLabel{box_label(box), box.center_px, detection_hue(box.state)};
}

void build_detection_labels(const std::vector<DetectionBox>& boxes,
                            std::uint64_t selected_id, std::vector<OverlayLabel>& out)
{
    out.clear();

    // Strongest first, skipping any that would collide with one already
    // placed, then drawn left to right. A smudge of overlapping plates says
    // less than three clean ones and a gap. The selected box is appended last
    // whatever it collides with, because the operator asked for that one
    // specifically.
    //
    // WHAT THE FIRST SENTENCE USED TO SAY: "Left to right, skipping any that
    // would collide with the one before it." On the labelled scene an AM
    // station is three tracks, a carrier at 24.7 dB and its sidebands at
    // 8.8 dB either side, and left to right put the lower sideband's plate up
    // and dropped the carrier's; the plate a station keeps should be the one
    // for the part of it that is loudest.
    const QFontMetricsF metrics(overlay_label_font());
    const DetectionBox* chosen = nullptr;

    struct Candidate {
        OverlayLabel label;
        double start = 0.0;
        double end = 0.0;
        double snr = 0.0;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(boxes.size());
    for (const DetectionBox& box : boxes) {
        if (box.id != 0 && box.id == selected_id) {
            chosen = &box;
            continue;
        }
        OverlayLabel label = detection_label(box);
        const double plate_width = metrics.horizontalAdvance(label.text) + 8.0;
        const double start = label.center_px - plate_width / 2.0;
        candidates.push_back({std::move(label), start, start + plate_width, box.snr_2500_db});
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.snr > b.snr; });

    std::vector<const Candidate*> placed;
    for (const Candidate& candidate : candidates) {
        const bool clear = std::none_of(placed.begin(), placed.end(), [&](const Candidate* other) {
            return candidate.start < other->end + 4.0 && other->start < candidate.end + 4.0;
        });
        if (clear) {
            placed.push_back(&candidate);
        }
    }
    std::sort(placed.begin(), placed.end(), [](const Candidate* a, const Candidate* b) {
        return a->label.center_px < b->label.center_px;
    });
    for (const Candidate* candidate : placed) {
        out.push_back(candidate->label);
    }

    if (chosen != nullptr) {
        OverlayLabel label = detection_label(*chosen);
        label.ink = QColor(255, 255, 255);
        out.push_back(std::move(label));
    }
}

double overlay_label_height()
{
    const QFontMetricsF metrics(overlay_label_font());
    return metrics.height() + 2.0;
}

std::vector<PlacedReceiverMarker> build_receiver_markers(const EngineLink& link,
                                                        double width_px)
{
    std::vector<PlacedReceiverMarker> out;

    // No receiver is the ordinary state on a window that has not been clicked
    // in yet. rackMarkers answers nothing while disconnected, and a band with
    // no width plans to an invisible marker, so neither needs a test here.
    for (const RackMarker& one : link.rackMarkers()) {
        // The band is absolute, which is the frame spanLowHz and spanHighHz
        // are in. See the note on the declaration for why the grant is
        // preferred and why there is a fallback at all.
        PlacedReceiverMarker placed;
        placed.marker =
            plan_receiver_marker(one.band, link.spanLowHz(), link.spanHighHz(), width_px);
        placed.slot = one.slot;
        placed.focused = one.focused;
        if (placed.marker.visible) {
            out.push_back(placed);
        }
    }
    return out;
}

void build_receiver_quads(const PlacedReceiverMarker& placed, double height_px,
                          std::vector<OverlayQuad>& out)
{
    const ReceiverMarker& marker = placed.marker;
    if (!marker.visible || height_px <= 0.0) {
        return;
    }

    const double fill_wide = marker.fill_right_px - marker.fill_left_px;

    // The band itself. Faint for the reason passband_item.cpp gives about its
    // own fill: this sits over the measurement the operator is judging the
    // filter against, and a fill dark enough to read on its own would hide
    // the signal it is there to be lined up with. On the waterfall that
    // matters more, because the history under it is the picture.
    push_quad(out, QRectF(marker.fill_left_px, 0.0, fill_wide, height_px),
              receiver_qcolour(placed.slot,
                               placed.focused ? kReceiverFillAlpha : kHeldFillAlpha));

    // The bar, along the clipped extent rather than the true one, because it
    // is the "listening here" cue and has to be on screen to be one. It says
    // nothing about where an edge is, so clipping it claims nothing.
    const double bar_top = std::max(0.0, height_px - kReceiverBarHeightPx);
    push_quad(out,
              QRectF(marker.fill_left_px, bar_top, std::max(fill_wide, 1.0),
                     std::min(kReceiverBarHeightPx, height_px)),
              receiver_qcolour(placed.slot,
                               placed.focused ? kReceiverBarAlpha : kHeldBarAlpha));

    // The tuned frequency, before the edges so an edge sitting on the carrier
    // stays the readable one. That ordering is passband_item.cpp's and the
    // same argument holds: a filter edge on the carrier is a real
    // configuration and the edge is what the operator is placing.
    if (marker.center_visible) {
        const double tick = std::min(kReceiverCentreTickPx, height_px);
        push_quad(out, QRectF(marker.center_px - 0.5, height_px - tick, 1.0, tick),
                  placed.focused ? kReceiverCentre
                                 : QColor(255, 255, 255, kHeldCentreAlpha));
    }

    if (!placed.focused) {
        return;
    }

    // The edges, at the frequencies they actually name and only when those
    // are on screen. An off-span edge is left undrawn rather than pinned to
    // the boundary; models/receiver_marker.h has the argument.
    const QColor edge = receiver_qcolour(placed.slot, kReceiverEdgeAlpha);
    const auto rule = [&out, height_px, &edge](double x_px) {
        push_quad(out, QRectF(x_px - kReceiverEdgeWidthPx / 2.0, 0.0, kReceiverEdgeWidthPx,
                              height_px),
                  edge);
    };
    if (marker.low_edge_visible) {
        rule(marker.low_edge_px);
    }
    if (marker.high_edge_visible) {
        rule(marker.high_edge_px);
    }
}

OverlayLabelItem::OverlayLabelItem(QQuickItem* parent) : QQuickPaintedItem(parent)
{
    // Transparent everywhere the plates are not, and no mouse or hover, so
    // the display underneath still receives every click that lands on a
    // label. A child item that accepted events would eat exactly the clicks
    // aimed at the box it names.
    setAntialiasing(false);
    setOpaquePainting(false);
    setAcceptedMouseButtons(Qt::NoButton);
    setAcceptHoverEvents(false);
}

void OverlayLabelItem::setLabels(std::vector<OverlayLabel> labels)
{
    const bool same =
        labels.size() == labels_.size() &&
        std::equal(labels.begin(), labels.end(), labels_.begin(),
                   [](const OverlayLabel& lhs, const OverlayLabel& rhs) {
                       return lhs.text == rhs.text && lhs.ink == rhs.ink &&
                              std::abs(lhs.center_px - rhs.center_px) < 0.25;
                   });
    if (same) {
        return;
    }
    labels_ = std::move(labels);
    setVisible(!labels_.empty());
    update();
}

void OverlayLabelItem::paint(QPainter* painter)
{
    painter->setFont(overlay_label_font());
    for (const OverlayLabel& label : labels_) {
        paint_label(*painter, label.text, label.center_px, width(), label.ink);
    }
}

OverlayNode::OverlayNode() : geometry_(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0)
{
    geometry_.setDrawingMode(QSGGeometry::DrawTriangles);
    setGeometry(&geometry_);
    setMaterial(&material_);
}

void OverlayNode::setQuads(const std::vector<OverlayQuad>& quads)
{
    // Six vertices a rectangle and one draw call for all of them, which is
    // why the overlay costs the same at five hundred tracks as at twelve.
    geometry_.allocate(static_cast<int>(quads.size()) * 6);
    QSGGeometry::ColoredPoint2D* vertex = geometry_.vertexDataAsColoredPoint2D();

    int at = 0;
    for (const OverlayQuad& quad : quads) {
        // QSGVertexColorMaterial wants premultiplied colour.
        const int alpha = quad.colour.alpha();
        const auto scale = [alpha](int channel) {
            return static_cast<uchar>((channel * alpha + 127) / 255);
        };
        const uchar r = scale(quad.colour.red());
        const uchar g = scale(quad.colour.green());
        const uchar b = scale(quad.colour.blue());
        const auto a = static_cast<uchar>(alpha);

        const auto x0 = static_cast<float>(quad.rect.left());
        const auto y0 = static_cast<float>(quad.rect.top());
        const auto x1 = static_cast<float>(quad.rect.right());
        const auto y1 = static_cast<float>(quad.rect.bottom());

        vertex[at++].set(x0, y0, r, g, b, a);
        vertex[at++].set(x1, y0, r, g, b, a);
        vertex[at++].set(x0, y1, r, g, b, a);
        vertex[at++].set(x1, y0, r, g, b, a);
        vertex[at++].set(x1, y1, r, g, b, a);
        vertex[at++].set(x0, y1, r, g, b, a);
    }

    markDirty(QSGNode::DirtyGeometry);
}

double detection_depth(const DetectionBox& box, double x_px)
{
    // The slack is added to the half-width rather than compared separately,
    // which is what makes depth <= 1 the same test as the old pair of edge
    // comparisons: left - slack <= x <= right + slack is exactly
    // |x - centre| <= half + slack. std::max guards a box whose edges
    // arrived crossed, which no detector should produce and which would
    // otherwise divide by something smaller than the slack.
    const double half = std::max(0.0, (box.right_px - box.left_px) / 2.0);
    return std::abs(x_px - box.center_px) / (half + kDetectionClickSlackPx);
}

void detections_at(const std::vector<DetectionBox>& boxes, double x_px,
                   std::vector<std::uint64_t>& out)
{
    out.clear();

    // Depth is wanted for the sort and would otherwise be recomputed inside
    // the comparator, which is a divide per comparison across a list that
    // reached 139 tracks on the radio in an earlier session.
    std::vector<std::pair<double, const DetectionBox*>> scored;
    scored.reserve(boxes.size());
    for (const DetectionBox& box : boxes) {
        if (box.id == 0) {
            continue;
        }
        const double depth = detection_depth(box, x_px);
        if (depth > 1.0) {
            continue;
        }
        scored.emplace_back(depth, &box);
    }

    std::sort(scored.begin(), scored.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.first != rhs.first) {
            return lhs.first < rhs.first;
        }
        // Equal depth means the pointer cannot tell the two apart by
        // position. The louder one is the one the operator can see.
        if (lhs.second->snr_2500_db != rhs.second->snr_2500_db) {
            return lhs.second->snr_2500_db > rhs.second->snr_2500_db;
        }
        // A total order, so the list does not reshuffle between frames when
        // two tracks agree on both. Ids are issued in order, so this is the
        // older track first.
        return lhs.second->id < rhs.second->id;
    });

    out.reserve(scored.size());
    for (const auto& entry : scored) {
        out.push_back(entry.second->id);
    }
}

std::uint64_t detection_at(const std::vector<DetectionBox>& boxes, double x_px)
{
    // The best one only, without the allocation detections_at needs, because
    // this runs on every hover event.
    std::uint64_t best = 0;
    double best_depth = std::numeric_limits<double>::max();
    double best_snr = 0.0;

    for (const DetectionBox& box : boxes) {
        if (box.id == 0) {
            continue;
        }
        const double depth = detection_depth(box, x_px);
        if (depth > 1.0) {
            continue;
        }
        // The same order as the comparator above, written out: depth, then
        // SNR, then the lower id. A hover disagreeing with the click that
        // follows it would move the selection off the box under the pointer.
        const bool better = depth < best_depth ||
                            (depth == best_depth &&
                             (box.snr_2500_db > best_snr ||
                              (box.snr_2500_db == best_snr && box.id < best)));
        if (better) {
            best_depth = depth;
            best_snr = box.snr_2500_db;
            best = box.id;
        }
    }
    return best;
}

ClickResult detection_clicked(const std::vector<DetectionBox>& boxes, double x_px,
                              ClickCycle& cycle)
{
    std::vector<std::uint64_t> ranked;
    detections_at(boxes, x_px, ranked);

    ClickResult result;
    result.candidates = static_cast<int>(ranked.size());
    if (ranked.empty()) {
        cycle = ClickCycle{};
        return result;
    }

    // A click off the anchor is a new place and starts a new walk. The anchor
    // stays where the walk started rather than following each click, so a run
    // of clicks that each drift a pixel cannot walk the anchor across the
    // display while still counting as one place.
    if (!cycle.anchored || std::abs(x_px - cycle.anchor_px) > kDetectionCycleSlackPx) {
        cycle.anchor_px = x_px;
        cycle.anchored = true;
        cycle.shown.clear();
    }

    const auto already_shown = [&cycle](std::uint64_t id) {
        return std::find(cycle.shown.begin(), cycle.shown.end(), id) != cycle.shown.end();
    };

    // The best candidate this walk has not answered yet. ranked is in depth
    // order, so this is the nearest unseen box rather than the next position
    // in a list that may have changed shape since the last click.
    auto next = std::find_if_not(ranked.begin(), ranked.end(), already_shown);
    if (next == ranked.end()) {
        // Everything under the pointer has been shown, so wrap.
        cycle.shown.clear();
        next = ranked.begin();
    }

    cycle.shown.push_back(*next);
    result.id = *next;
    result.rank = static_cast<int>(cycle.shown.size());

    // Whether the click after this one advances or starts over, which is the
    // difference between the two sentences the window can print.
    result.exhausted =
        std::find_if_not(ranked.begin(), ranked.end(), already_shown) == ranked.end();
    return result;
}

SpectrumItem::SpectrumItem(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);

    labels_ = new OverlayLabelItem(this);
    labels_->setVisible(false);
}

void SpectrumItem::setLink(EngineLink* link)
{
    if (link_ == link) {
        return;
    }
    if (link_ != nullptr) {
        disconnect(link_, nullptr, this, nullptr);
    }
    link_ = link;
    if (link_ != nullptr) {
        // Direct, because EngineLink emits this from the Qt thread after it
        // has already taken the frame off the event loop thread. The queued
        // hop happens once, inside EngineLink, and not again per item.
        connect(link_, &EngineLink::frameChanged, this, &SpectrumItem::takeFrame);
        connect(link_, &EngineLink::connectionChanged, this,
                &SpectrumItem::onConnectionChanged);
        connect(link_, &EngineLink::detectionsChanged, this, &SpectrumItem::takeDetections);

        // Both halves of the receiver's news. receiverChanged is emitted by
        // the WRITE, so the marker follows a retune and a drag at the pointer's
        // rate; receiverStatusChanged is the engine's answer, which is where
        // the granted width the marker prefers comes from. Neither covers the
        // other: a mode change moves the centre before the engine has said
        // anything, and a clamp changes the width with the centre unmoved.
        //
        // Both arrive through rackChanged, which EngineLink raises for each
        // of them and for every held receiver's news as well, so that is the
        // one connection. Connecting all three drew every pane change twice.
        connect(link_, &EngineLink::rackChanged, this, &SpectrumItem::takeReceiver);
    }
    have_frame_ = false;
    boxes_.clear();
    rebuildOverlay();
    emit linkChanged();
    update();
}

void SpectrumItem::setMapPins(ScaleSettings* pins)
{
    if (map_pins_ == pins) {
        return;
    }
    if (map_pins_ != nullptr) {
        disconnect(map_pins_, nullptr, this, nullptr);
    }
    map_pins_ = pins;
    if (map_pins_ != nullptr) {
        // A pin moves the trace at once, redrawn from the frame in hand, so
        // the operator sees where the end went while their hand is still on
        // the control rather than at the next frame.
        connect(map_pins_, &ScaleSettings::pinsChanged, this, &SpectrumItem::takeFrame);
    }
    emit mapPinsChanged();
    takeFrame();
}

void SpectrumItem::setSelectedDetection(qulonglong id)
{
    if (selected_detection_ == id) {
        return;
    }
    selected_detection_ = id;
    rebuildOverlay();
    emit selectedDetectionChanged();
    update();
}

void SpectrumItem::onConnectionChanged()
{
    if (link_ == nullptr || !link_->connected()) {
        // The last trace stays up on a link that went away. It is the last
        // thing the engine actually said, the window says overhead that the
        // link is gone, and blanking would take away the only reading there
        // is at exactly the moment somebody is looking at it.
        //
        // The boxes do not get that treatment. A trace is a measurement and
        // stays true; a detection box is an invitation to click, and a stale
        // one would tune a receiver to a track the engine has forgotten.
        boxes_.clear();
        rebuildOverlay();
        update();
        return;
    }

    // A new engine. Nothing drawn from the previous one is meaningful here:
    // this one may be on another frequency, with another span, and the axis
    // under the trace has already changed to match.
    have_frame_ = false;
    reduced_bins_ = 0;
    boxes_.clear();

    // The click cycle's remembered answers are ids from the previous
    // engine's detector, and every detector issues from one. A second click
    // at the same pixel after a reconnect would be ranked against a list of
    // ids that now mean other signals. Narrow, and one line to close.
    click_cycle_ = {};
    hovered_detection_ = 0;

    rebuildOverlay();
    update();
}

void SpectrumItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.width() != oldGeometry.width()) {
        // THE TRACE IS NOT BLANKED HERE, WHICH IS A REVERSAL.
        //
        // This called resizeColumns, and resizeColumns assigns every column
        // to kSpectrumFloorDb. Against a running engine the next frame
        // overwrote them tens of milliseconds later and nobody saw it.
        // Against a stopped engine, a disconnected one or a build with no
        // spectrum stage there is no next frame, so dragging the window
        // wider left a flat line at the floor under honest floor and
        // ceiling labels. That reads as an empty band, which is a
        // measurement, rather than as no measurement.
        //
        // The old samples are kept and stretched across the new width
        // instead, which is what setTrace does with any column count: they
        // are the last thing the engine said and they are still true, only
        // their horizontal resolution is stale, and the next frame fixes
        // that. render/passband_item.cpp's geometryChange has always left
        // its columns alone for the same reason.
        //
        // The headroom does have to move, because it is a property of the
        // reduction and the reduction is bins per COLUMN. Recomputed
        // against the last frame's bin count, which is the one the next
        // frame will almost certainly have.
        recomputeHeadroom(deviceColumns());
    }
    if (newGeometry.size() != oldGeometry.size()) {
        rebuildOverlay();
        update();
    }
}

int SpectrumItem::deviceColumns() const
{
    const QQuickWindow* const host = window();
    const qreal ratio = host == nullptr ? 1.0 : host->effectiveDevicePixelRatio();
    return std::max(static_cast<int>(std::lround(width() * ratio)), 1);
}

void SpectrumItem::resizeColumns(int columns, std::size_t bins)
{
    const std::size_t wanted = static_cast<std::size_t>(std::max(columns, 1));
    columns_.assign(wanted, kSpectrumFloorDb);
    reduced_bins_ = bins;
    recomputeHeadroom(columns);
}

void SpectrumItem::recomputeHeadroom(int columns)
{
    const std::size_t wanted = static_cast<std::size_t>(std::max(columns, 1));
    const std::size_t bins_per_column =
        reduced_bins_ == 0 ? 1 : std::max<std::size_t>(1, reduced_bins_ / wanted);
    headroom_db_ = peak_reduction_headroom_db(bins_per_column);
    emit endsChanged();
}

void SpectrumItem::rebuildOverlay()
{
    if (link_ == nullptr) {
        boxes_.clear();
    } else {
        build_detection_boxes(*link_, width(), boxes_);
    }
    build_detection_quads(boxes_, width(), height(), selected_detection_, hovered_detection_,
                          DetectionStyle::Band, quads_);

    // After the boxes and into the same batch, so the receiver's band is
    // composited over the brackets rather than under them. build_detection_
    // quads assigns and this appends, which is the only ordering that works:
    // one geometry node is one draw call at one depth, so what is drawn over
    // what is decided by position in this vector.
    if (link_ != nullptr) {
        for (const PlacedReceiverMarker& placed : build_receiver_markers(*link_, width())) {
        build_receiver_quads(placed, height(), quads_);
    }
    }

    // The labels are a child item, so they are placed here on the Qt thread
    // and never from updatePaintNode, which runs on the render thread.
    std::vector<OverlayLabel> labels;
    build_detection_labels(boxes_, selected_detection_, labels);
    labels_->setX(0.0);
    labels_->setY(kLabelTopPx);
    labels_->setWidth(width());
    labels_->setHeight(overlay_label_height());
    labels_->setLabels(std::move(labels));
}

void SpectrumItem::takeDetections()
{
    rebuildOverlay();
    update();
}

void SpectrumItem::takeReceiver()
{
    rebuildOverlay();
    update();
}

void SpectrumItem::takeFrame()
{
    const FrameCost cost(FrameItem::Spectrum, FramePhase::Take);
    if (link_ == nullptr) {
        return;
    }
    const rpc::SpectrumFrame& frame = link_->frame();
    if (frame.power_db.empty()) {
        return;
    }

    // Recomputed per frame rather than only on a resize, because the device
    // pixel ratio changes with no geometry change at all when the window is
    // dragged onto a display with another scale factor.
    const auto wanted = static_cast<std::size_t>(deviceColumns());
    if (columns_.size() != wanted || reduced_bins_ != frame.power_db.size()) {
        resizeColumns(static_cast<int>(wanted), frame.power_db.size());
    }

    reduce_peak(frame.power_db, columns_);
    ends_ = resolve_ends(frame.floor_db, frame.ceiling_db, headroom_db_, pinsInForce());
    have_frame_ = true;

    // The detection list has not changed, but the clock the fade is measured
    // against has: this frame carries a later sample index, so every held
    // track is that much further into its decay. Rebuilding here is what
    // makes the fade move at the frame rate instead of the poll rate.
    rebuildOverlay();

    emit endsChanged();
    update();
}

void SpectrumItem::setHovered(std::uint64_t id)
{
    if (hovered_detection_ == id) {
        return;
    }
    hovered_detection_ = id;
    setCursor(id == 0 ? Qt::ArrowCursor : Qt::PointingHandCursor);
    rebuildOverlay();
    emit hoveredDetectionChanged();
    update();
}

void SpectrumItem::hoverMoveEvent(QHoverEvent* event)
{
    setHovered(detection_at(boxes_, event->position().x()));
    event->accept();
}

void SpectrumItem::hoverLeaveEvent(QHoverEvent* event)
{
    setHovered(0);
    event->accept();
}

void SpectrumItem::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }

    const double x = event->position().x();

    // The frequency under the pointer, through the same mapping the boxes
    // were placed with. It goes out with every click, because whether the
    // click landed inside another receiver's band is decided on it.
    const double fraction = width() > 0.0 ? x / width() : 0.0;
    const double hz = link_ == nullptr ? 0.0 : link_->frequencyAtFraction(fraction);

    const ClickResult hit = detection_clicked(boxes_, x, click_cycle_);
    if (hit.id != 0) {
        const auto found =
            std::find_if(boxes_.begin(), boxes_.end(),
                         [&hit](const DetectionBox& box) { return box.id == hit.id; });
        if (found != boxes_.end()) {
            emit tuneRequested(hit.id, static_cast<double>(found->center_hz),
                               static_cast<double>(found->bandwidth_hz), hit.candidates,
                               hit.rank, hit.exhausted, hz);
            event->accept();
            return;
        }
    }

    // Nothing there. The frequency still goes out, so a click on bare
    // spectrum reads back as a frequency rather than as nothing happening.
    // Zero for the id, because track ids start at one.
    emit tuneRequested(0, hz, 0.0, 0, 0, false, hz);
    event->accept();
}

void SpectrumItem::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }

    // The box under the pointer without stepping the cycle, which the first
    // press already stepped: the second press of a double click is not a
    // request for the next box down.
    const double x = event->position().x();
    const double fraction = width() > 0.0 ? x / width() : 0.0;
    const double hz = link_ == nullptr ? 0.0 : link_->frequencyAtFraction(fraction);
    const std::uint64_t id = detection_at(boxes_, x);
    const auto found = std::find_if(boxes_.begin(), boxes_.end(),
                                    [id](const DetectionBox& box) { return box.id == id; });
    if (id != 0 && found != boxes_.end()) {
        emit addRequested(id, static_cast<double>(found->center_hz),
                          static_cast<double>(found->bandwidth_hz), hz);
    } else {
        emit addRequested(0, hz, 0.0, hz);
    }
    event->accept();
}

void SpectrumItem::wheelEvent(QWheelEvent* event)
{
    const double eighths = scroll_tune_eighths(event->angleDelta().x(), event->angleDelta().y());
    if (eighths == 0.0) {
        event->ignore();
        return;
    }

    // IGNORED RATHER THAN SWALLOWED ON A SOURCE THAT CANNOT RETUNE, so the
    // event is still on offer to whatever is behind this item. Over the
    // waterfall on a recording that matters: docs/ui-spectrum.md has vertical
    // scroll scrubbing through the capture, which is the gesture that belongs
    // to exactly the sources this one refuses, so accepting here would take the
    // wheel away from it before it is written.
    if (link_ == nullptr || !link_->sourceCanRetune()) {
        event->ignore();
        return;
    }

    // The gesture is this item's and the backlog is the window's. EngineLink
    // holds the accumulator, the clock and the flush, because the settling
    // interval is a property of the radio and there is one radio behind both
    // displays. See EngineLink::takeScrollTune.
    link_->takeScrollTune(eighths);
    event->accept();
}

QSGNode* SpectrumItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
    const FrameCost cost(FrameItem::Spectrum, FramePhase::Sync);
    const qreal w = width();
    const qreal h = height();
    if (w <= 0.0 || h <= 0.0) {
        delete old_node;
        return nullptr;
    }

    auto* node = static_cast<SpectrumNode*>(old_node);
    if (node == nullptr) {
        node = new SpectrumNode;
        node->background = window()->createRectangleNode();

        // Opaque and covering the item, so the scene graph composites this
        // without reading what is behind it.
        node->background->setColor(QColor(8, 10, 14));
        node->fill = new SpectrumFillNode;
        node->trace = new TraceNode;
        node->overlay = new OverlayNode;

        node->appendChildNode(node->background);
        node->appendChildNode(node->fill);
        node->appendChildNode(node->trace);
        node->appendChildNode(node->overlay);
    }

    node->background->setRect(0.0, 0.0, w, h);
    node->fill->ensureGradient(window());

    // The two nodes that draw the frame want the same normalised levels, so
    // they are computed once here rather than twice from the decibels.
    const float span = ends_.span_db();
    if (have_frame_ && !columns_.empty() && span > 0.0F) {
        levels_.resize(columns_.size());
        for (std::size_t column = 0; column < columns_.size(); ++column) {
            levels_[column] =
                std::clamp((columns_[column] - ends_.floor_db) / span, 0.0F, 1.0F);
        }
    } else {
        levels_.clear();
    }

    node->fill->setTrace(levels_, w, h);
    node->trace->setTrace(levels_, w, h);
    node->overlay->setQuads(quads_);
    return node;
}

}  // namespace revenant::ui
