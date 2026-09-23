// The instantaneous spectrum, drawn as one column of pixels per bin run, and
// the detection overlay both displays share.
//
// WHY THIS IS A QQuickItem AND NOT A QQuickPaintedItem, WHICH IS A REVERSAL
//
// It was a painted item, on the reasoning that a frame reaches this process
// as a copied vector of floats over a socket, so the raster data is already
// in host memory and a painted item is what the data actually is. That is
// still true of where the data comes from and it was the wrong conclusion
// about how to draw it.
//
// QQuickPaintedItem has one path in Qt 6: rasterise through QPainter into an
// indirect image the size of the item in device pixels, then upload that
// whole image, and update() marks the whole item dirty. Three O(W*H) terms
// per frame, the blit, the rasterise and the upload, to deliver one new row
// of W pixels. A waterfall 1578 by 1123 device pixels is 6.8 MB of RGBA by
// arithmetic, and that is what was re-uploaded per frame.
//
// The measured consequence was that the frame rate fell as the window grew:
// 41.6, 22.5 and 18.3 rows a second at waterfall sizes of 1578x363,
// 1578x743 and 1578x1123 device pixels, against one synthetic scene at a
// fixed nine tracks with the engine offering more frames than the window
// drew at any of the three.
//
// So both displays build scene graph nodes instead. The trace is a line strip
// and a textured triangle strip, the waterfall's history is a texture the GPU
// scrolls by moving source rectangles, and the overlay is one batch of
// triangles. Every per-frame term is then O(W), and the same three sizes
// against the same engine settings measured 96.5, 97.3 and 97.6 rows a
// second, which is everything that engine offered. Raising the supply until
// the window was the limit again gave 201.7, 199.1 and 192.9. Flat against
// height either way. All of these were measured this session on GPU 0.
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
// WHAT THE TWO DISPLAYS DO WITH IT IS NOT THE SAME THING
//
// The spectrum has no time axis, so a detection there is a frequency marker
// and nothing else: this signal, this wide, live or held. The waterfall has
// one, so a detection there is bounded in both axes, frequency across and the
// rows the signal was present in down, and it scrolls with the history it
// annotates. A full-height band on the waterfall says nothing about when the
// signal was there and covers the history that would have said it.
//
// The fade follows that split. A held track on the waterfall has a definite
// end, drawn as the bottom edge of its rectangle, so it is drawn solid;
// fading belongs to the spectrum marker alone, where it means the tracker has
// not dropped this track yet and the signal has stopped.
//
// THE RECEIVER MARKER IS THE SAME ARRANGEMENT AND IS DRAWN THE SAME ON BOTH
//
// build_receiver_marker and build_receiver_quads are below, beside the
// detection overlay and for the same reason: one mapping from hertz to
// pixels. Unlike a detection the receiver's mark is identical on the two
// displays, because there is exactly one of it. The argument that made a
// detection a bracket on one display and a rectangle on the other was that
// twenty of them composite to something that hides the picture; one faint
// band does not, and where the receiver is listening is worth saying in the
// same place on both displays so the eye does not have to learn two marks.

#pragma once

#include <cstdint>
#include <vector>

#include <QColor>
#include <QQuickItem>
#include <QQuickPaintedItem>
#include <QRectF>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QString>
#include <QtQmlIntegration>

#include <QElapsedTimer>
#include <QTimer>

#include "core/rpc/types.h"
#include "models/engine_link.h"
#include "models/scale_settings.h"
#include "models/receiver_marker.h"
#include "models/scroll_tune.h"
#include "render/spectrum_scale.h"

class QMouseEvent;
class QHoverEvent;
class QPainter;
class QWheelEvent;

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

    // The rows this track was present in, as logical y in the display that
    // resolved them, and whether they were resolved at all. Only a display
    // with a time axis can answer this, so only the waterfall fills these in;
    // see WaterfallItem::resolveRows. On the spectrum they stay unset and the
    // marker spans the item.
    double top_px = 0.0;
    double bottom_px = 0.0;
    bool time_bounded = false;

    // The detector's own sample clock, which is what a display with a time
    // axis draws the rows from.
    //
    // last_seen is deliberately not here. It is the last frame the tracker
    // CONSIDERED this track in and last_detected is the last one the signal
    // was actually in, so for a held track they differ by exactly the hold:
    // closing a rectangle on last_seen would stretch every held track across
    // the seconds it spent being held, which is the opposite of what the
    // rectangle is for.
    std::uint64_t first_seen = 0;
    std::uint64_t last_detected = 0;

    // Seconds since the detector last had evidence for this track, from the
    // engine's sample clock and never from a wall clock. Zero unless held.
    double silent_seconds = 0.0;

    // 1 while live, falling toward kDetectionFadeFloor across the hold.
    double fade = 1.0;
};

enum class DetectionStyle : std::uint8_t {
    // Over the trace, where there is no time axis: a bracket, both edges and
    // the centre down the full height, faded by how much evidence the
    // detector still has, and a label on every box with room for one.
    Band,

    // Over the waterfall, where there is one: a rectangle closed on all four
    // sides, spanning the rows the signal was actually present in, drawn
    // solid because those rows already say when it stopped. A label only on
    // the box under the pointer and on the chosen one.
    Rows,
};

// One rectangle of the overlay, in logical item coordinates. Both displays
// draw their overlay as a single batch of these, so the cost of the overlay
// is one draw call whether there are two tracks or five hundred.
struct OverlayQuad {
    QRectF rect;
    QColor colour;
};

// One label of the overlay. The plate is centred on center_px and pulled back
// inside the item by whoever paints it; the y is the strip's, not the label's,
// because every label in one strip shares it.
struct OverlayLabel {
    QString text;
    double center_px = 0.0;
    QColor ink;
};

// Resolves every detection the link is holding into this display's
// coordinates, and works out the fade from the engine's sample indices.
//
// width_px is logical, because that is what a scene graph node is positioned
// in. The frame reduction elsewhere works in device pixels; the two are
// different jobs and mixing them puts the boxes a scale factor away from the
// trace.
void build_detection_boxes(const EngineLink& link, double width_px,
                           std::vector<DetectionBox>& out);

// The overlay's rectangles for one display. Everything about the difference
// between a frequency marker and a bounded rectangle is in here rather than
// in the two items.
void build_detection_quads(const std::vector<DetectionBox>& boxes, double width_px,
                           double height_px, std::uint64_t selected_id,
                           std::uint64_t hovered_id, DetectionStyle style,
                           std::vector<OverlayQuad>& out);

// What one box is called, and in the colour its state is drawn in.
[[nodiscard]] OverlayLabel detection_label(const DetectionBox& box);

// The labels for the Band style, left to right, skipping any that would
// collide with the one before it. The chosen box is always in the list and is
// always last, so it is drawn over whatever it collides with.
void build_detection_labels(const std::vector<DetectionBox>& boxes,
                            std::uint64_t selected_id, std::vector<OverlayLabel>& out);

// How tall a label strip has to be for the application font.
[[nodiscard]] double overlay_label_height();

// Clear of the bracket and the centre notch, which are drawn from the top
// edge down. A label overlapping its own mark reads as a rendering fault.
inline constexpr double kLabelTopPx = 9.0;

// ---------------------------------------------------------------------------
// Where the receiver is listening
// ---------------------------------------------------------------------------
//
// In this header for the same reason the detection overlay is: there is one
// mapping from hertz to pixels and both displays have to use it. The marker
// goes through EngineLink::spanLowHz and spanHighHz exactly as the boxes do.
//
// models/receiver_marker.h holds the arithmetic and the argument for the
// passband being the marked thing rather than the tuned frequency. These two
// are what turns it into quads: which of EngineLink's numbers the band comes
// off, and what the mark looks like.

// The receiver's granted passband against this display's geometry, or an
// invisible marker when there is nothing to mark.
//
// THE GRANTED PAIR AND NOT THE REQUEST, with one fallback. VrxPlacement is
// what the engine actually built and is the only pair that answers "does the
// filter cover this signal". The fallback is the pane's own request, and it
// covers the round trip between a tune and the first status: the client sends
// an empty passband to ask for the mode's default, so during that window the
// grant is a pair of zeros while the request may already hold edges the
// operator dragged. Drawing nothing there would blink the marker off at every
// mode change.
[[nodiscard]] ReceiverMarker build_receiver_marker(const EngineLink& link, double width_px);

// The marker's rectangles, APPENDED rather than assigned, so the receiver
// draws over the detection overlay that is already in out. The receiver is
// the one mark the operator put there on purpose; a detection bracket drawn
// over it would hide the answer to the question the bracket raised.
void build_receiver_quads(const ReceiverMarker& marker, double height_px,
                          std::vector<OverlayQuad>& out);

// ---------------------------------------------------------------------------
// The wheel walks the front end, and the backlog is not an item's
// ---------------------------------------------------------------------------
//
// WHAT THIS SECTION USED TO DECLARE. take_scroll_tune took an EngineLink and
// one display's ScrollTuneState, accumulated a wheel event against it and
// returned the milliseconds to wait, so each item armed its own flush timer
// against its own accumulator.
//
// That could not keep the promise the coalescing exists to make. A device
// retune costs about 330 ms with no samples at all, so models/scroll_tune.h is
// built around at most one tune going out per settling interval, and two
// accumulators cannot enforce one rule: a pointer crossing from the spectrum
// to the waterfall mid-sweep hands each of them a part of one gesture, each
// below its own interval and each firing. Two displays, one radio.
//
// It lives on EngineLink now, which is the object that owns the radio and the
// one both items already talk to. An item resolves the gesture and hands over
// the eighths; see EngineLink::takeScrollTune.

// The overlay's text, and the one thing in either display still rasterised by
// QPainter.
//
// Text is the case the scene graph does not make cheaper without a glyph
// cache of its own, and a QQuickPaintedItem the height of one label is
// already O(W) and constant in the height of the display it sits on: at 1580
// device pixels wide that is about 150 KB a frame against the 6.7 MB the
// whole display used to cost. So the labels keep QPainter and the pictures
// stop using it.
//
// One of these is one strip of labels that all share a y. The spectrum has a
// single strip at the top; the waterfall's labels follow the top edge of the
// rectangle they name, so it has one per labelled box.
class OverlayLabelItem : public QQuickPaintedItem {
    Q_OBJECT

public:
    explicit OverlayLabelItem(QQuickItem* parent = nullptr);

    // Repaints only when the text or the placement actually changed, because
    // this is called once per frame per display and most frames do not move
    // a label.
    void setLabels(std::vector<OverlayLabel> labels);

    void paint(QPainter* painter) override;

private:
    std::vector<OverlayLabel> labels_;
};

// The overlay's rectangles as one geometry node. Shared by both displays,
// because the only thing that differs between them is which rectangles they
// ask for.
class OverlayNode : public QSGGeometryNode {
public:
    OverlayNode();

    void setQuads(const std::vector<OverlayQuad>& quads);

private:
    QSGGeometry geometry_;
    QSGVertexColorMaterial material_;
};

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
// Measured in an earlier session against the RTL-SDR at 98.1 MHz on GPU 0, at
// the shipped 6 dB threshold, from revenant-cli --detect with the confidence
// bar at zero: 105 tracks across the span, and the broadcast station is not
// one of them. It arrives as a ladder of about thirty tracks between 2.4 and
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
// The change is visible on the radio at one pointer width. Measured in an
// earlier session: a click on the block's centre returned a 705 Hz track
// whose centre was a quarter of a pixel away, and moving the pointer four
// logical pixels right returned a 3.05 kHz track 1.4 pixels away, while the
// 705 Hz track was 3.2 pixels off and so still inside the six-pixel slack
// that used to be all it needed to win. Narrowest-wins would have answered
// the 705 Hz track both times.
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

// Where a run of clicks is anchored and which tracks it has already answered.
// One per item, because each display has its own pointer; the selection the
// two share is the window's and is written back to both.
struct ClickCycle {
    double anchor_px = 0.0;
    bool anchored = false;

    // In the order they were answered. This is the whole of what makes the
    // cycle survive the list changing under it: see detection_clicked.
    std::vector<std::uint64_t> shown;
};

struct ClickResult {
    // Zero for a click that landed on no detection.
    std::uint64_t id = 0;

    // How many boxes contain the click now, and how far into the cycle this
    // answer is, counting from one. The window shows both, because a rule
    // that picks one of seven overlapping tracks has to say that it did.
    //
    // rank counts the cycle and not the depth order. Those were the same
    // number while the cycle walked a fixed list, and they are not once the
    // list can change between two clicks in one place, which on a live band
    // it does constantly.
    int candidates = 0;
    int rank = 0;

    // The cycle has now answered every box containing the click, so the next
    // click in the same place starts again at the nearest. The window says
    // which of the two is about to happen rather than promising a next one
    // unconditionally.
    bool exhausted = false;
};

// What a click at x chooses, advancing the cycle.
//
// The depth rule answers the first click at a place. Each later click in the
// same place answers the best candidate the cycle has not answered yet, which
// is what keeps a sub-track reachable in the middle of a wide box, where
// depth deliberately prefers the wide one. When every candidate has been
// answered the cycle clears and the next click starts at the nearest again.
//
// THE CYCLE REMEMBERS WHAT IT SHOWED, NOT WHERE IT WAS IN THE LIST, AND THAT
// IS THE FIX
//
// It used to remember the last id and advance one position past it. When the
// track it remembered was gone from the freshly ranked list, which on a live
// band is the ordinary case rather than the exception, it silently restarted
// at rank 1, so the window promised "click again for the next" and the next
// click answered the first again. Observed against the RTL-SDR at 98.1 MHz:
// a click reported "1 of 6 here", and the next click at the same pixel
// reported "1 of 5" on a different track.
//
// A set of what has been shown survives exactly that. A track that vanishes
// is skipped rather than restarting the walk, a track that appears mid-cycle
// is reachable, and every click at one place answers something the operator
// has not already been shown until there is nothing left to show. The window
// then says "click again for the next" while that is true and "click again to
// start over" when it is not.
[[nodiscard]] ClickResult detection_clicked(const std::vector<DetectionBox>& boxes,
                                            double x_px, ClickCycle& cycle);

class SpectrumItem : public QQuickItem {
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

    // The operator's pins on either end of the colour map, shared with the
    // other span display. Null draws against the frame's ends alone.
    Q_PROPERTY(revenant::ui::ScaleSettings* mapPins READ mapPins WRITE setMapPins
                   NOTIFY mapPinsChanged)

    // Which track is drawn as chosen. Written by QML and never by this item,
    // so the two displays share one selection: a click emits tuneRequested,
    // the window decides what that means, and both items follow. An item
    // that set its own would break the binding the other one is reading.
    Q_PROPERTY(qulonglong selectedDetection READ selectedDetection WRITE setSelectedDetection
                   NOTIFY selectedDetectionChanged)

    // The track under the pointer, or zero. Read by the hover card in
    // qml/SpanView.qml, which is where the numbers that no longer fit on a
    // label go.
    Q_PROPERTY(qulonglong hoveredDetection READ hoveredDetection
                   NOTIFY hoveredDetectionChanged)

public:
    explicit SpectrumItem(QQuickItem* parent = nullptr);

    [[nodiscard]] EngineLink* link() const { return link_; }
    void setLink(EngineLink* link);

    [[nodiscard]] double drawFloorDb() const { return ends_.floor_db; }
    [[nodiscard]] double drawCeilingDb() const { return ends_.ceiling_db; }
    [[nodiscard]] double headroomDb() const { return headroom_db_; }

    [[nodiscard]] ScaleSettings* mapPins() const { return map_pins_; }
    void setMapPins(ScaleSettings* pins);

    [[nodiscard]] qulonglong selectedDetection() const { return selected_detection_; }
    void setSelectedDetection(qulonglong id);

    [[nodiscard]] qulonglong hoveredDetection() const { return hovered_detection_; }

signals:
    void linkChanged();
    void endsChanged();
    void mapPinsChanged();
    void selectedDetectionChanged();
    void hoveredDetectionChanged();

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
    // out deliberately, the detector's classification is a family at most
    // and never crosses the wire, and docs/detection.md puts identification
    // two tiers above the layer that would need it. (This used to say "the
    // detector's only Classification value is Unknown".) So this is the known-approximate answer, it is
    // correct today only for AM and for the modes whose energy centre is
    // their logical centre, and the window says so next to the number rather
    // than presenting it as a tuning solution.
    //
    // candidates, rank and exhausted are ClickResult's three. They go out
    // with the click rather than being read back off a property, because they
    // belong to that one click: a poll arriving a frame later changes the
    // boxes and would change the counts under a reading the operator is still
    // looking at. All three are empty for a click on bare spectrum.
    void tuneRequested(qulonglong id, double center_hz, double bandwidth_hz, int candidates,
                       int rank, bool exhausted);

protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void mousePressEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;

    // Scrolling walks the SOURCE's centre, not the receiver's. See
    // EngineLink::takeScrollTune and models/scroll_tune.h.
    void wheelEvent(QWheelEvent* event) override;

private:
    void takeFrame();
    void takeDetections();

    // The receiver moved, or the engine answered about it. Both land here,
    // because the marker is drawn from the grant and falls back to the
    // request, so either piece of news can move it.
    void takeReceiver();

    void onConnectionChanged();

    // Everything drawn over the trace: the detection boxes and their labels,
    // and the receiver's passband over the top of those. One function because
    // they share a width, a height and one hertz-to-pixel mapping, and because
    // every caller that invalidates one invalidates the other.
    void rebuildOverlay();

    void setHovered(std::uint64_t id);

    // One column per physical pixel, not per logical one. The trace is drawn
    // in the item's own logical coordinates, so a trace built at logical
    // width throws away a fifth of the columns on the 1.25 scaling this was
    // checked on, and does it by widening each column's bin run rather than
    // by blurring, which is a real loss of resolution rather than a soft
    // picture.
    [[nodiscard]] int deviceColumns() const;

    // Reallocates the reduced trace at this column count, against a frame
    // of this many bins, and throws away whatever was in it. Only a frame
    // calls this: it has the samples to refill what it just emptied.
    void resizeColumns(int columns, std::size_t bins);

    // The reduction headroom alone, against whatever bin count the last
    // frame had. The correction is a function of how many bins one column
    // covers, so a resize moves it with no frame behind it, and this is the
    // half of resizeColumns a resize is entitled to do. See the reversal
    // recorded in geometryChange for why the other half is not.
    void recomputeHeadroom(int columns);

    EngineLink* link_ = nullptr;
    std::vector<float> columns_;

    // The same columns normalised to [0, 1], because the fill and the trace
    // are two nodes drawing one set of y values and the map from decibels is
    // worth doing once.
    std::vector<float> levels_;
    MapEnds ends_;
    ScaleSettings* map_pins_ = nullptr;

    // The pins in force, or none, for the ends of the next row or trace.
    [[nodiscard]] ScalePins pinsInForce() const
    {
        return map_pins_ == nullptr ? ScalePins{} : map_pins_->pins();
    }
    float headroom_db_ = 0.0F;
    std::size_t reduced_bins_ = 0;
    bool have_frame_ = false;

    std::vector<DetectionBox> boxes_;
    std::vector<OverlayQuad> quads_;
    std::uint64_t selected_detection_ = 0;
    std::uint64_t hovered_detection_ = 0;
    ClickCycle click_cycle_;

    // Created in the constructor and positioned in updatePaintNode, so the
    // labels are a child item and not part of this item's own node.
    OverlayLabelItem* labels_ = nullptr;
};

}  // namespace revenant::ui
