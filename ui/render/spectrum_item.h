// The instantaneous spectrum, drawn as one column of pixels per bin run, and
// the detection overlay both displays share.
//
// WHY A PAINTED ITEM AND NOT A QSGRenderNode
//
// ui/render/.gitkeep planned a custom render node sharing the Vulkan device
// and command buffers with the DSP, so raster data never touched host
// memory. That is still the right destination and it is not reachable from
// here: the UI is a separate process for the runtime reason
// core/rpc/types.h sets out, and a frame reaches it as a copied vector of
// floats over a socket. The raster data is already in host memory by the
// time this item sees it, so a painted item is what the data actually is.
// The shared-handle path arrives with a local-client fast path in the
// schema, not before.
//
// WHY THE DETECTION OVERLAY IS IN THIS HEADER AND NOT IN EACH ITEM
//
// There is one mapping from hertz to pixels and there has to stay one. The
// axis under both displays reads EngineLink::frequencyAtFraction, and an
// overlay that derived its own x from bin_zero and bin_width would be a
// second answer to the same question: two mappings agree at the centre of
// the span and diverge at the edges, so the boxes would look right where
// anybody checks them and be wrong at the ends of the band. So the overlay
// is built once, here, from the two numbers the axis itself is drawn from,
// and both items call it.
//
// It is a plain struct and three free functions rather than a QObject
// because it holds no state between calls and belongs to neither item.
// render/spectrum_scale.h would be the natural home and is another agent's
// file this session.

#pragma once

#include <cstdint>
#include <vector>

#include <QQuickPaintedItem>
#include <QtQmlIntegration>

#include "core/rpc/types.h"
#include "models/engine_link.h"
#include "render/spectrum_scale.h"

class QMouseEvent;
class QHoverEvent;
class QPainter;

namespace revenant::ui {

// The hold to assume while the engine has not said what its own is.
//
// This is DetectorConfig::bootstrap_hold_seconds as core/detect/detector.h
// declares it, and it is now the fallback rather than the answer.
// rpc::DetectionList carries the running detector's hold, and
// EngineLink::detectorHoldSeconds hands it over, so build_detection_boxes
// prefers that and reaches this only when the property is zero: before the
// engine has answered a detections call once, and against an engine built
// before the field was on the wire. There are no rows to draw in the first
// of those, so the only case where a compiled-in number still decides a fade
// is an old engine.
//
// The fade never reaches zero whichever number it divides by. The detector's
// own drop is what removes a box, and an invisible box is still in the list
// and still clickable, so the fade says how long a track has been quiet and
// never that it has gone.
inline constexpr double kDetectionHoldSeconds = 3.0;

// What is left of a box at the end of its hold. Not zero, per the note
// above: an invisible box is still in the list and still clickable, and a
// click landing on something the operator cannot see is worse than a faint
// box that is about to go.
inline constexpr double kDetectionFadeFloor = 0.20;

// Slack either side of a box for hit testing. A 50 Hz carrier on a 2.4 MHz
// span is a fifty-thousandth of the width, which is a box narrower than the
// cursor: without slack the only clickable detections would be the wide
// ones, and the narrow ones are the interesting ones.
//
// It is also the floor on a box's clickable half-width, which is what stops
// the narrowest box in a cluster winning by being narrowest. See the block
// above detection_depth.
inline constexpr double kDetectionClickSlackPx = 6.0;

// How far a click can land from the one before it and still count as the
// same place, for the cycle detection_clicked runs. Smaller than the click
// slack, so a pointer moved deliberately onto another signal starts a fresh
// cycle, and large enough that a hand that did not quite hold still does
// not.
inline constexpr double kDetectionCycleSlackPx = 3.0;

// One detection, resolved against one display's geometry.
struct DetectionBox {
    std::uint64_t id = 0;
    rpc::TrackState state = rpc::TrackState::Pending;

    std::int64_t center_hz = 0;
    std::int64_t bandwidth_hz = 0;
    double snr_2500_db = 0.0;
    double confidence = 0.0;

    // Logical item coordinates. left and right are the edges of the measured
    // occupied bandwidth and are allowed outside the item, because a signal
    // at the edge of the span really does run off it and clamping the box
    // would draw it narrower than it is.
    double left_px = 0.0;
    double right_px = 0.0;
    double center_px = 0.0;

    // Seconds since the detector last had evidence for this track, from the
    // engine's sample clock and never from a wall clock. Zero unless held.
    double silent_seconds = 0.0;

    // 1 while live, falling toward kDetectionFadeFloor across the hold.
    double fade = 1.0;
};

enum class DetectionStyle : std::uint8_t {
    // Over the trace: a filled band, both edges, and a label on every box
    // with room for one.
    Band,

    // Over the waterfall: the edges and the centre, no fill, and a label
    // only on the selected box. A filled band the height of a waterfall
    // covers the history it is pointing at, and the waterfall is where the
    // operator is checking whether the box is on the right carrier.
    Edges,
};

// Resolves every detection the link is holding into this display's
// coordinates, and works out the fade from the engine's sample indices.
//
// width_px is logical, because that is what QQuickPaintedItem::paint draws
// in. The frame reduction elsewhere in this file works in device pixels; the
// two are different jobs and mixing them puts the boxes a scale factor away
// from the trace.
void build_detection_boxes(const EngineLink& link, double width_px,
                           std::vector<DetectionBox>& out);

void paint_detections(QPainter& painter, const std::vector<DetectionBox>& boxes,
                      double width_px, double height_px, std::uint64_t selected_id,
                      std::uint64_t hovered_id, DetectionStyle style);

// WHICH BOX A CLICK MEANS, WHEN SEVERAL OF THEM CONTAIN IT
//
// The narrowest containing box used to win, on the reasoning that a narrow
// signal inside a wide one is otherwise unreachable. What that rule actually
// does is give the narrowest box in a cluster every pixel of its slack: it
// wins from six pixels away against a box the pointer is sitting on the
// centre of, so it is wrong everywhere it overlaps anything and not only at
// the edges.
//
// WHAT THE DETECTOR ACTUALLY PRODUCES HERE, BECAUSE IT CHANGES THE PROBLEM
//
// Measured this session against the RTL-SDR at 98.1 MHz on GPU 0, at the
// shipped 6 dB threshold, from revenant-cli --detect with the confidence bar
// at zero: 105 tracks across the span, and the broadcast station is not one
// of them. It arrives as a ladder of about thirty tracks between 2.4 and
// 7.0 kHz wide, shoulder to shoulder from 98.035 to 98.175 MHz, the widest
// anywhere in the block 6.98 kHz and the strongest 6.979 kHz at
// 98.1022 MHz, 29.1 dB. Their brackets abut, which is why the block reads on
// screen as one wide box and why the wide box is not there to be clicked.
// The same run carried a 146 Hz track at 96.896356 MHz inside the other
// broadcast block, which is the sub-track class this rule has to survive.
//
// So no rule can return "the station" at this threshold, and the guarantee
// worth making is the reachable one: a click returns the detection the
// pointer is actually on, and a narrow one never captures a click that is
// nearer something else.
//
// What a click carries is a position relative to what is drawn, so the rule
// is depth: how far the click is from a box's centre as a fraction of that
// box's own clickable half-width. Dead centre is 0, the outer edge of the
// clickable region is 1, anything above 1 is not a candidate, and the lowest
// depth wins. The centre of the wide box then beats every sub-track that is
// not also centred there, while a sub-track still wins wherever the pointer
// is nearer its centre in its own terms, which is everywhere except the wide
// box's middle. It is the same containment test as before rather than a
// second one, because the half-width includes the slack: a box contains the
// click exactly when its depth is at most 1.
//
// Two things fall out of it and both are wanted. A box narrower than the
// slack gets a clickable half-width of the slack, so a cluster of sub-pixel
// tracks is reached by position and not by which of them is narrowest. And
// nothing is hidden: every box containing the click is still a candidate,
// and detection_clicked walks the rest of them.
//
// The change is visible on the radio at one pointer width. Measured this
// session: a click on the block's centre returned a 705 Hz track whose
// centre was a quarter of a pixel away, and moving the pointer four logical
// pixels right returned a 3.05 kHz track 1.4 pixels away, while the 705 Hz
// track was 3.2 pixels off and so still inside the six-pixel slack that used
// to be all it needed to win. Narrowest-wins would have answered the 705 Hz
// track both times.
[[nodiscard]] double detection_depth(const DetectionBox& box, double x_px);

// Every box containing x, best first. Ties are broken by SNR and then by id:
// equal depth means two boxes the pointer cannot tell apart by position,
// where the one the operator can see is the louder of them, and the id stops
// the order moving between frames when the SNRs match too.
void detections_at(const std::vector<DetectionBox>& boxes, double x_px,
                   std::vector<std::uint64_t>& out);

// The best one, or zero for nothing. Track ids are issued from one and never
// reused, per core/detect/detector.h, so zero is not a track and can carry
// "no detection here".
//
// This is what hover reads. A pointer moving across the display names one
// box per position and never cycles, because the cycle is a property of
// clicking the same place twice and a pointer is not clicking.
[[nodiscard]] std::uint64_t detection_at(const std::vector<DetectionBox>& boxes,
                                         double x_px);

// Where a run of clicks is anchored and what the last of them chose. One per
// item, because each display has its own pointer; the selection the two
// share is the window's and is written back to both.
struct ClickCycle {
    double anchor_px = 0.0;
    std::uint64_t id = 0;
};

struct ClickResult {
    // Zero for a click that landed on no detection.
    std::uint64_t id = 0;

    // How many boxes contained the click, and where the chosen one sits in
    // that order, counting from one. The window shows both, because a rule
    // that picks one of seven overlapping tracks has to say that it did.
    int candidates = 0;
    int rank = 0;
};

// What a click at x chooses, advancing the cycle.
//
// The depth rule answers the first click. A second click in the same place
// takes the next candidate and the last wraps to the first, which is what
// keeps a sub-track reachable in the middle of a wide box, where depth
// deliberately prefers the wide one. The cycle restarts whenever the pointer
// moves off the anchor or the track it last chose leaves the list, so it
// never walks somewhere the operator did not point.
//
// That second restart is common and not a fault. Against the RTL-SDR at
// 98.1 MHz this session the detector split and merged tracks inside the
// broadcast block fast enough that a second click a few seconds later found
// the first pick gone and answered rank 1 of a list that had changed size,
// alongside clicks that did advance to rank 2. A cycle is therefore worth
// offering and not worth promising: the window says how many candidates
// there were, and restarting on a vanished track is better than advancing
// past it into whatever now occupies that position.
[[nodiscard]] ClickResult detection_clicked(const std::vector<DetectionBox>& boxes,
                                            double x_px, ClickCycle& cycle);

class SpectrumItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(revenant::ui::EngineLink* link READ link WRITE setLink NOTIFY linkChanged)

    // The two ends this item is actually drawing against, which is the
    // frame's pair plus this item's own reduction correction. Exposed so the
    // axis labels in QML read what was drawn rather than what was sent: the
    // correction depends on how many bins a column covers, so it is a
    // property of this item's width and cannot be computed anywhere else.
    Q_PROPERTY(double drawFloorDb READ drawFloorDb NOTIFY endsChanged)
    Q_PROPERTY(double drawCeilingDb READ drawCeilingDb NOTIFY endsChanged)
    Q_PROPERTY(double headroomDb READ headroomDb NOTIFY endsChanged)

    // Which track is drawn as chosen. Written by QML and never by this item,
    // so the two displays share one selection: a click emits tuneRequested,
    // the window decides what that means, and both items follow. An item
    // that set its own would break the binding the other one is reading.
    Q_PROPERTY(qulonglong selectedDetection READ selectedDetection WRITE setSelectedDetection
                   NOTIFY selectedDetectionChanged)

public:
    explicit SpectrumItem(QQuickItem* parent = nullptr);

    [[nodiscard]] EngineLink* link() const { return link_; }
    void setLink(EngineLink* link);

    [[nodiscard]] double drawFloorDb() const { return ends_.floor_db; }
    [[nodiscard]] double drawCeilingDb() const { return ends_.ceiling_db; }
    [[nodiscard]] double headroomDb() const { return headroom_db_; }

    [[nodiscard]] qulonglong selectedDetection() const { return selected_detection_; }
    void setSelectedDetection(qulonglong id);

    void paint(QPainter* painter) override;

signals:
    void linkChanged();
    void endsChanged();
    void selectedDetectionChanged();

    // A click landed on a detection, or on nothing, in which case id is zero
    // and the other two are the click's own frequency rather than a track's.
    //
    // THE FREQUENCY IN HERE IS THE MEASURED CENTRE AND NOT THE LOGICAL ONE
    //
    // center_hz is rpc::Detection::center_hz, which core/detect/detector.h
    // says in as many words is "the centre of the measured occupied band and
    // nothing else". docs/ui-spectrum.md wants the LOGICAL centre, which is
    // a property of the modulation: RTTY is two tones and its logical centre
    // is the midpoint between mark and space, where no energy is, so a
    // receiver parked on the measured centre of a mark-heavy idle sits on
    // mark and walks when traffic starts. SSB is worse, because the measured
    // centre is about half a bandwidth from the suppressed carrier and
    // wanders with what the speaker is saying.
    //
    // Nothing in the tree computes a logical centre. The schema leaves it
    // out deliberately, the detector's only Classification value is Unknown,
    // and docs/detection.md puts identification two tiers above the layer
    // that would need it. So this is the known-approximate answer, it is
    // correct today only for AM and for the modes whose energy centre is
    // their logical centre, and the window says so next to the number rather
    // than presenting it as a tuning solution.
    //
    // candidates and rank are ClickResult's two counts. They go out with the
    // click rather than being read back off a property, because they belong
    // to that one click: a poll arriving a frame later changes the boxes and
    // would change the counts under a reading the operator is still looking
    // at. Both are zero for a click on bare spectrum.
    void tuneRequested(qulonglong id, double center_hz, double bandwidth_hz, int candidates,
                       int rank);

protected:
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void mousePressEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;

private:
    void takeFrame();
    void takeDetections();
    void onConnectionChanged();
    void rebuildDetections();
    void setHovered(std::uint64_t id);

    // One column per physical pixel, not per logical one. QQuickPaintedItem
    // paints into a texture the size of the item times the window's device
    // pixel ratio, so a trace built at logical width throws away a fifth of
    // the columns on the 1.25 scaling this was checked on, and does it by
    // widening each column's bin run rather than by blurring, which is a
    // real loss of resolution rather than a soft picture.
    [[nodiscard]] int deviceColumns() const;

    // Recomputes the reduction headroom for the current column count. The
    // correction is a function of how many bins one column covers, so it
    // changes when the item is resized and not when a frame arrives.
    void resizeColumns(int columns, std::size_t bins);

    EngineLink* link_ = nullptr;
    std::vector<float> columns_;
    MapEnds ends_;
    float headroom_db_ = 0.0F;
    std::size_t reduced_bins_ = 0;
    bool have_frame_ = false;

    std::vector<DetectionBox> boxes_;
    std::uint64_t selected_detection_ = 0;
    std::uint64_t hovered_detection_ = 0;
    ClickCycle click_cycle_;
};

}  // namespace revenant::ui
