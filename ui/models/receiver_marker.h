// Where the receiver is listening, resolved against the span a display is
// drawing. One pure function, because the whole of what can be wrong here is
// arithmetic on four frequencies and a width.
//
// WHAT THIS EXISTS FOR
//
// The spectrum and the waterfall draw the whole span and said nothing about
// where the receiver was parked in it. An operator clicking around a crowded
// band had no way to tell which of the signals on screen they were hearing,
// and no way to see that a 16 kHz filter was sitting on a 145 kHz broadcast
// until the audio told them. models/receiver_match.h says that second thing
// in words; this says both of them in the picture, which is where the
// operator is already looking.
//
// THE PASSBAND AND NOT A CENTRE LINE, WHICH IS THE POINT
//
// A tick at the tuned frequency says where the receiver is and nothing about
// what it can hear. The band between the filter edges is the thing that
// either covers the signal or does not, so the marked region is the edges
// and the tick is extra. The edges wanted are the GRANTED pair off
// VrxPlacement, not the mode's default and not the request: the engine fits
// per edge against the channel it placed the receiver in, so a request for
// 200 kHz can come back as 71 kHz, and a highlight drawn from the request
// would show a filter that was never built.
//
// THE RECEIVER CAN BE OUTSIDE THE SPAN, AND THAT IS NOT A FAULT
//
// Retuning the front end moves the span and leaves the receiver where it was.
// So a band partly or wholly off the display is an ordinary state, and the
// two honest answers are to clip the region and to draw an edge rule only
// where that edge really is. Stretching the band to fit would draw a filter
// at frequencies it does not cover, and pinning an off-span edge to the
// display's boundary would claim the filter ends there.
//
// WHY THIS HOLDS NO Qt. ui/tests links it, on the same terms as
// models/receiver_match.h and render/history_resize.h. Which cases draw
// nothing, where a clip lands and whether an edge is on screen are exactly
// the parts that go wrong, and none of them needs a window.

#pragma once

#include <algorithm>

namespace revenant::ui {

// The receiver's band in ABSOLUTE hertz, which is the frame a display's axis
// is in. The caller adds the receiver's centre to the signed passband pair,
// because the centre is the only place the baseband offsets and the band plan
// meet.
struct ReceiverBand {
    double low_hz = 0.0;
    double high_hz = 0.0;

    // The tuned frequency, which is not generally the midpoint of the other
    // two: a filter may be asymmetric about the carrier, and on CW the
    // passband sits entirely to one side of it.
    double center_hz = 0.0;
};

// One receiver, resolved against one display's geometry, in logical item
// coordinates. Logical rather than device, because that is what a scene graph
// node is positioned in; see the same note on DetectionBox.
struct ReceiverMarker {
    // False means draw nothing at all: no receiver, no span, or a band with
    // no overlap with the span. Every field below is meaningless then.
    bool visible = false;

    // The marked region, clipped into [0, width_px]. Equal ends are a band
    // touching one edge of the span and no more, which draws no fill.
    double fill_left_px = 0.0;
    double fill_right_px = 0.0;

    // The filter edges and the tuned frequency, UNCLIPPED, so a rule lands
    // on the frequency it names. Each is only to be drawn when its flag is
    // set; the flags are what keep an off-span edge from being pinned to the
    // display's boundary.
    double low_edge_px = 0.0;
    double high_edge_px = 0.0;
    double center_px = 0.0;

    bool low_edge_visible = false;
    bool high_edge_visible = false;
    bool center_visible = false;
};

// Where the band lands on a display running from span_low_hz at x = 0 to
// span_high_hz at x = width_px.
//
// The two span ends are EngineLink::spanLowHz and spanHighHz, which are
// EngineLink::frequencyAtFraction evaluated at 0 and 1. That is the same pair
// build_detection_boxes places its boxes from, and passing them in rather
// than reconstructing an axis from bin_zero and bin_width is what stops this
// becoming a second answer to where a frequency is: two mappings agree in the
// middle of the span and part company at the ends, so the highlight would
// look right wherever anybody checked it.
//
// A band whose edges arrived crossed or equal draws nothing. A receiver has a
// width or it has not been placed, and a zero-width filter is the state
// before the engine has answered rather than a filter to mark.
[[nodiscard]] inline ReceiverMarker plan_receiver_marker(const ReceiverBand& band,
                                                         double span_low_hz,
                                                         double span_high_hz,
                                                         double width_px)
{
    ReceiverMarker out;

    const double span_hz = span_high_hz - span_low_hz;
    if (!(span_hz > 0.0) || !(width_px > 0.0) || !(band.high_hz > band.low_hz)) {
        return out;
    }

    const auto to_px = [&](double hertz) {
        return (hertz - span_low_hz) / span_hz * width_px;
    };

    const double left = to_px(band.low_hz);
    const double right = to_px(band.high_hz);

    // Wholly off one side. This is the ordinary state after a front-end
    // retune that moved the span past the receiver, not an error, and the
    // answer is to draw nothing rather than to clamp the band onto the edge
    // it went off.
    if (right < 0.0 || left > width_px) {
        return out;
    }

    out.visible = true;
    out.fill_left_px = std::clamp(left, 0.0, width_px);
    out.fill_right_px = std::clamp(right, 0.0, width_px);

    out.low_edge_px = left;
    out.high_edge_px = right;
    out.center_px = to_px(band.center_hz);

    // Inclusive of the boundaries: a filter edge exactly at the end of the
    // span really is at that pixel. Anything beyond them is off the display
    // and stays undrawn.
    out.low_edge_visible = left >= 0.0 && left <= width_px;
    out.high_edge_visible = right >= 0.0 && right <= width_px;
    out.center_visible = out.center_px >= 0.0 && out.center_px <= width_px;

    return out;
}

}  // namespace revenant::ui
