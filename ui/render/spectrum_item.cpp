#include "render/spectrum_item.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

#include <QBrush>
#include <QColor>
#include <QCursor>
#include <QFontMetricsF>
#include <QHoverEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QPolygonF>
#include <QQuickWindow>
#include <QRectF>
#include <QString>

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
            // Inside another track's band. A different hue rather than a
            // different alpha, because a merged track is not a faint one:
            // core/detect/detector.h says it has evidence and does not
            // decay, so fading it would say the opposite of what is true.
            return QColor(154, 108, 255);
        case rpc::TrackState::Pending:
        case rpc::TrackState::Live:
        case rpc::TrackState::Held:
            break;
    }
    return QColor(255, 88, 200);
}

[[nodiscard]] int scaled_alpha(int full, double fade)
{
    return std::clamp(static_cast<int>(std::lround(static_cast<double>(full) * fade)), 0, 255);
}

// Clear of the bracket and the centre notch, which are drawn from the top
// edge down. A label overlapping its own mark reads as a rendering fault.
inline constexpr double kLabelTopPx = 9.0;

// Megahertz to four places, which is the axis label's own precision in
// qml/Main.qml, so a box and the tick under it are read in the same units.
// The exact hertz goes in the window's tune readout, where there is room for
// it and where somebody is about to act on it.
[[nodiscard]] QString box_label(const DetectionBox& box)
{
    const QString megahertz =
        QString::number(static_cast<double>(box.center_hz) / 1.0e6, 'f', 4);

    switch (box.state) {
        case rpc::TrackState::Held:
            return megahertz + QStringLiteral("  held ") +
                   QString::number(box.silent_seconds, 'f', 1) + QStringLiteral(" s");
        case rpc::TrackState::Merged:
            return megahertz + QStringLiteral("  merged");
        case rpc::TrackState::Pending:
        case rpc::TrackState::Live:
            break;
    }
    return megahertz + QStringLiteral("  ") + QString::number(box.snr_2500_db, 'f', 1) +
           QStringLiteral(" dB  ") + QString::number(box.confidence, 'f', 2);
}

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
void paint_label(QPainter& painter, const QString& text, double centre_x, double top_y,
                 double width_px, const QColor& ink)
{
    const QFontMetricsF metrics(painter.font());
    const double text_width = metrics.horizontalAdvance(text);
    const double text_height = metrics.height();
    const double plate_width = text_width + 8.0;
    const double plate_height = text_height + 2.0;

    // Centred on the box, then pulled back inside the item rather than
    // clipped, the same rule the axis labels in qml/Main.qml follow.
    const double x =
        std::clamp(centre_x - plate_width / 2.0, 0.0, std::max(0.0, width_px - plate_width));

    QColor plate(6, 8, 16);
    plate.setAlpha(200);
    painter.setPen(Qt::NoPen);
    painter.setBrush(plate);
    painter.drawRect(QRectF(x, top_y, plate_width, plate_height));

    QColor pen_colour = ink;
    pen_colour.setAlpha(255);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(pen_colour);
    painter.drawText(QRectF(x, top_y, plate_width, plate_height), Qt::AlignCenter, text);
}

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

        const double centre = static_cast<double>(detection.center_hz);
        const double half = static_cast<double>(detection.bandwidth_hz) / 2.0;
        box.center_px = (centre - low_hz) / span_hz * width_px;
        box.left_px = (centre - half - low_hz) / span_hz * width_px;
        box.right_px = (centre + half - low_hz) / span_hz * width_px;

        if (detection.state == rpc::TrackState::Held && sample_rate > 0.0) {
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
    // reached 139 tracks on the radio this session.
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

    std::size_t pick = 0;
    bool continuing = false;
    if (cycle.id != 0 && std::abs(x_px - cycle.anchor_px) <= kDetectionCycleSlackPx) {
        const auto found = std::find(ranked.begin(), ranked.end(), cycle.id);
        if (found != ranked.end()) {
            pick = (static_cast<std::size_t>(found - ranked.begin()) + 1) % ranked.size();
            continuing = true;
        }
    }

    // The anchor stays where the cycle started rather than following each
    // click, so a run of clicks that each drift a pixel cannot walk the
    // anchor across the display while still counting as one place.
    if (!continuing) {
        cycle.anchor_px = x_px;
    }

    result.id = ranked[pick];
    result.rank = static_cast<int>(pick) + 1;
    cycle.id = result.id;
    return result;
}

void paint_detections(QPainter& painter, const std::vector<DetectionBox>& boxes,
                      double width_px, double height_px, std::uint64_t selected_id,
                      std::uint64_t hovered_id, DetectionStyle style)
{
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
    // measured this session, the waterfall under the clusters was not
    // readable at all, which is the one thing the overlay must not do to the
    // display it is annotating.
    //
    // It was also most of the cost. That run drew 19.0 and then 21.8 rows a
    // second and left 557 frames of 991 undrawn, where an earlier session
    // measured this same window at 37.0 with no overlay at all. Both dashed
    // and dotted pens are in that: Qt strokes a dashed line through the dash
    // stroker rather than as a span, and there were two of them per track
    // per item per frame.
    //
    // So: a filled bracket along the top edge, a notch at the centre, and
    // hairlines down the edges, all solid and all rectangles. The fill is
    // kept for the chosen and the hovered track alone, where there is one of
    // it and it is the thing being looked at.
    //
    // After, on the same radio and the same window: 39.0 rows a second with
    // 12 tracks and 36.5 with 507, at 34 frames undrawn of 3514 and 28 of
    // 2384. Both measured this session. The overlay no longer costs anything
    // this counter can see, and it costs the same at 507 tracks as at 12.
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);

    for (const DetectionBox& box : boxes) {
        const bool chosen = box.id != 0 && box.id == selected_id;
        const bool under_cursor = box.id != 0 && box.id == hovered_id;
        const QColor hue = detection_hue(box.state);

        // The chosen box does not fade. The fade is a comparison against the
        // other boxes on screen, saying which of them the detector still has
        // evidence for, and a selection has already answered that question:
        // the operator said this is the one being looked at. A held track
        // the operator explicitly picked used to dim to the fade floor like
        // any other, so the box they asked to watch was the faintest thing
        // on the display. The state is still legible without the fade, from
        // the bracket height and from the label: checked on the radio this
        // session, a selected track at "held 2.3 s" drew its outline, fill
        // and label at full alpha while the unselected held boxes beside it
        // were down at the floor.
        const double fade = chosen ? 1.0 : box.fade;

        // A box narrower than a pixel is still a detection and still has to
        // be visible. Widened for drawing only: the hit test and the tune
        // frequency both use the unwidened numbers, so a narrow carrier is
        // not reported as wider than it is.
        const double left = std::min(box.left_px, box.center_px - 0.5);
        const double right = std::max(box.right_px, box.center_px + 0.5);

        if (chosen || under_cursor) {
            QColor fill = hue;
            fill.setAlpha(scaled_alpha(chosen ? 64 : 40, fade));
            painter.setBrush(fill);
            painter.drawRect(QRectF(left, 0.0, right - left, height_px));
        }

        // The bracket. Its height is the second thing that says live from
        // held, after the fade: three pixels of solid bar against one. The
        // fade alone is a comparison against the other boxes on screen, and
        // a lone held track has nothing to be compared with.
        const double bracket = box.state == rpc::TrackState::Held ? 1.0 : 3.0;
        QColor bar = hue;
        bar.setAlpha(scaled_alpha(chosen ? 255 : 220, fade));
        painter.setBrush(bar);
        painter.drawRect(QRectF(left, 0.0, right - left, bracket));

        // The centre, which is what a click tunes to, marked so the operator
        // can see it is not necessarily where the energy is. See the note on
        // SpectrumItem::tuneRequested.
        painter.drawRect(QRectF(box.center_px - 0.5, 0.0, 1.0, bracket + 5.0));

        // Hairlines down the edges, which are what carry the bandwidth over
        // the waterfall. Skipped on a box too narrow to have two distinct
        // edges, where they would draw the same pixel twice.
        if (right - left >= 2.0) {
            QColor edge = hue;
            edge.setAlpha(scaled_alpha(style == DetectionStyle::Band ? 130 : 105, fade));
            painter.setBrush(edge);
            painter.drawRect(QRectF(left, 0.0, 1.0, height_px));
            painter.drawRect(QRectF(right - 1.0, 0.0, 1.0, height_px));
        }

        if (chosen) {
            // White rather than a brighter magenta. The selection has to be
            // legible against a saturated carrier, which is where the colour
            // map is already at its own brightest.
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(255, 255, 255, scaled_alpha(230, fade)), 1.0));
            painter.drawRect(QRectF(left - 1.0, 0.0, (right - left) + 2.0, height_px - 1.0));
            painter.setPen(Qt::NoPen);
        }
    }

    // Labels last, so no box's edge is drawn over a label, and left to
    // right, skipping any that would collide with the one before it. A
    // smudge of overlapping frequencies says less than three clean ones and
    // a gap. The selected box is drawn afterwards whatever it collides with,
    // because the operator asked for that one specifically.
    const QFontMetricsF metrics(painter.font());
    double claimed_to = -1.0e9;
    const DetectionBox* chosen_box = nullptr;

    for (const DetectionBox& box : boxes) {
        if (box.id != 0 && box.id == selected_id) {
            chosen_box = &box;
            continue;
        }
        // Over the waterfall only the box under the pointer is named. A
        // label per track down there covers the history the boxes are
        // pointing at, and the spectrum above is already carrying them.
        if (style == DetectionStyle::Edges && !(box.id != 0 && box.id == hovered_id)) {
            continue;
        }

        const QString text = box_label(box);
        const double plate_width = metrics.horizontalAdvance(text) + 8.0;
        const double start = box.center_px - plate_width / 2.0;
        if (start < claimed_to + 4.0) {
            continue;
        }
        claimed_to = start + plate_width;
        paint_label(painter, text, box.center_px, kLabelTopPx, width_px,
                    detection_hue(box.state));
    }

    if (chosen_box != nullptr) {
        paint_label(painter, box_label(*chosen_box), chosen_box->center_px, kLabelTopPx,
                    width_px, QColor(255, 255, 255));
    }

    painter.restore();
}

SpectrumItem::SpectrumItem(QQuickItem* parent) : QQuickPaintedItem(parent)
{
    setFillColor(QColor(8, 10, 14));

    // NO ANTIALIASING, WHICH IS A REVERSAL AND IS MEASURED
    //
    // This used to be on, with the reason that a polyline and a gradient
    // fill both alias badly on a one-pixel feature and a one-bin carrier is
    // exactly that feature. That was true while a column was a logical
    // pixel: a segment then spanned 1.25 device pixels on the display this
    // was checked on, so it had a diagonal to soften. deviceColumns() puts
    // one point per physical pixel, and a segment is now one pixel wide, so
    // there is no diagonal left. What antialiasing does to a vertical spike
    // one pixel wide is spread it over two at half the brightness, which
    // makes a narrow carrier harder to see rather than easier. Side by side
    // at four times magnification the aliased trace is the crisper of the
    // two and the peaks keep their value.
    //
    // It is also almost the entire cost of this item, which is almost the
    // entire cost of the window. At 1290 by 830 against an RTL-SDR v3 at
    // 98.1 MHz with 65536 bins, the waterfall drew 22.6 rows a second of
    // the 36.8 the engine offered with both the fill and the trace
    // antialiased, 23.3 with the fill alone aliased, and 37.0 with both
    // aliased, which is the whole of what arrives. Collapsing this item out
    // of the layout entirely also gave 37.0, so with this off the trace is
    // free. All four measured in an earlier session.
    setAntialiasing(false);

    // The fill colour is opaque and covers the item, so nothing behind this
    // shows through and the scene graph can composite it without blending.
    // The gradient under the trace is translucent over that fill, which is
    // a blend inside the item's own texture and not a claim about what is
    // underneath it. This one is here because it is true and not because it
    // bought anything: adding it moved the drawn rate from 22.7 to 22.6
    // rows a second, which is noise.
    setOpaquePainting(true);

    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
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
    }
    have_frame_ = false;
    boxes_.clear();
    emit linkChanged();
    update();
}

void SpectrumItem::setSelectedDetection(qulonglong id)
{
    if (selected_detection_ == id) {
        return;
    }
    selected_detection_ = id;
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
        update();
        return;
    }

    // A new engine. Nothing drawn from the previous one is meaningful here:
    // this one may be on another frequency, with another span, and the axis
    // under the trace has already changed to match.
    have_frame_ = false;
    reduced_bins_ = 0;
    boxes_.clear();
    update();
}

void SpectrumItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.width() != oldGeometry.width()) {
        // The reduction changes with the column count, so the headroom does
        // too. Recomputed against the last frame's bin count, which is the
        // one the next frame will almost certainly have.
        resizeColumns(deviceColumns(), reduced_bins_);
        rebuildDetections();
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

    const std::size_t bins_per_column = bins == 0 ? 1 : std::max<std::size_t>(1, bins / wanted);
    headroom_db_ = peak_reduction_headroom_db(bins_per_column);
    emit endsChanged();
}

void SpectrumItem::rebuildDetections()
{
    if (link_ == nullptr) {
        boxes_.clear();
        return;
    }
    build_detection_boxes(*link_, width(), boxes_);
}

void SpectrumItem::takeDetections()
{
    rebuildDetections();
    update();
}

void SpectrumItem::takeFrame()
{
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
    ends_ = map_ends(frame.floor_db, frame.ceiling_db, headroom_db_);
    have_frame_ = true;

    // The detection list has not changed, but the clock the fade is measured
    // against has: this frame carries a later sample index, so every held
    // track is that much further into its decay. Rebuilding here is what
    // makes the fade move at the frame rate instead of the poll rate.
    rebuildDetections();

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
    const ClickResult hit = detection_clicked(boxes_, x, click_cycle_);
    if (hit.id != 0) {
        const auto found =
            std::find_if(boxes_.begin(), boxes_.end(),
                         [&hit](const DetectionBox& box) { return box.id == hit.id; });
        if (found != boxes_.end()) {
            emit tuneRequested(hit.id, static_cast<double>(found->center_hz),
                               static_cast<double>(found->bandwidth_hz), hit.candidates,
                               hit.rank);
            event->accept();
            return;
        }
    }

    // Nothing there. The frequency still goes out, through the same mapping
    // the boxes were placed with, so a click on bare spectrum reads back as
    // a frequency rather than as nothing happening. Zero for the id, because
    // track ids start at one.
    const double fraction = width() > 0.0 ? x / width() : 0.0;
    const double hz = link_ == nullptr ? 0.0 : link_->frequencyAtFraction(fraction);
    emit tuneRequested(0, hz, 0.0, 0, 0);
    event->accept();
}

void SpectrumItem::paint(QPainter* painter)
{
    const qreal w = width();
    const qreal h = height();
    if (w <= 0.0 || h <= 0.0 || !have_frame_ || columns_.empty()) {
        return;
    }

    const float span = ends_.span_db();
    if (span <= 0.0F) {
        return;
    }

    // One point per column, top of the item at the ceiling. A value above
    // the ceiling clamps to the top edge rather than being drawn off the
    // item, which is what saturation should look like.
    QPolygonF trace;
    trace.reserve(static_cast<int>(columns_.size()) + 2);

    const qreal step = w / static_cast<qreal>(columns_.size());
    for (std::size_t c = 0; c < columns_.size(); ++c) {
        const float level = std::clamp((columns_[c] - ends_.floor_db) / span, 0.0F, 1.0F);
        const qreal x = (static_cast<qreal>(c) + 0.5) * step;
        const qreal y = h * (1.0 - static_cast<qreal>(level));
        trace.append(QPointF(x, y));
    }

    // The fill uses the same map as the waterfall, so a feature reads the
    // same colour in both displays and the eye can carry a level between
    // them. Translucent, because the trace itself has to stay legible where
    // it crosses the bright end.
    QLinearGradient gradient(0.0, 0.0, 0.0, h);
    for (int stop = 0; stop <= 8; ++stop) {
        const float level = static_cast<float>(stop) / 8.0F;
        QColor colour = colour_of(level);
        colour.setAlpha(140);
        gradient.setColorAt(1.0 - static_cast<qreal>(level), colour);
    }

    QPolygonF filled = trace;
    filled.append(QPointF(w, h));
    filled.append(QPointF(0.0, h));

    // Both aliased. The constructor has the reasoning and the measurement;
    // it is set here as well because a render hint is per painter and this
    // painter belongs to the scene graph, not to this item.
    painter->setRenderHint(QPainter::Antialiasing, false);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QBrush(gradient));
    painter->drawPolygon(filled);

    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(colour_of(1.0F), 1.0));
    painter->drawPolyline(trace);

    // Over the trace, because a box behind a saturated carrier is invisible
    // and the carrier is what the box is about.
    paint_detections(*painter, boxes_, w, h, selected_detection_, hovered_detection_,
                     DetectionStyle::Band);
}

}  // namespace revenant::ui
