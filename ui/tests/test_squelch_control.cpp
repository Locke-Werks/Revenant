// The receiver panel's squelch slider, models/squelch_control.h.
//
// EVERY CASE NAMES THE WRONG ANSWER IT REJECTS, on the rule the rest of
// ui/tests follows.

#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "core/rpc/types.h"
#include "models/squelch_control.h"

using revenant::ui::kSquelchOpenDbfs;
using revenant::ui::kSquelchSliderHighDbfs;
using revenant::ui::kSquelchSliderLowDbfs;
using revenant::ui::normalise_squelch;
using revenant::ui::slider_from_squelch;
using revenant::ui::squelch_from_slider;
using revenant::ui::squelch_gate_text;
using revenant::ui::squelch_is_open_setting;
using revenant::ui::squelch_text;

// REJECTS an open position that sends something other than the engine's own
// default. The bottom of the range as a number is a squelch that shuts on
// silence; the default is one that never shuts.
TEST_CASE("the slider's bottom stop sends the permanently open default", "[squelch]")
{
    CHECK(kSquelchOpenDbfs == revenant::rpc::VrxParams{}.squelch_dbfs);
    CHECK(squelch_from_slider(kSquelchSliderLowDbfs) == kSquelchOpenDbfs);
    CHECK(squelch_from_slider(kSquelchSliderLowDbfs + 0.4) == kSquelchOpenDbfs);
    CHECK(squelch_from_slider(kSquelchSliderLowDbfs - 30.0) == kSquelchOpenDbfs);
    CHECK(squelch_from_slider(std::numeric_limits<double>::quiet_NaN()) == kSquelchOpenDbfs);
}

// REJECTS a position above the stop that rounds back down onto it, or one
// that goes out unrounded and fills the wire with fractions of a decibel.
TEST_CASE("a position above the stop is a threshold, whole decibels", "[squelch]")
{
    CHECK(squelch_from_slider(kSquelchSliderLowDbfs + 1.0) == kSquelchSliderLowDbfs + 1.0);
    CHECK(squelch_from_slider(-73.4) == -73.0);
    CHECK(squelch_from_slider(-73.6) == -74.0);
    CHECK(squelch_from_slider(kSquelchSliderHighDbfs) == kSquelchSliderHighDbfs);
    CHECK(squelch_from_slider(kSquelchSliderHighDbfs + 25.0) == kSquelchSliderHighDbfs);
}

// REJECTS a handle that lands in the middle of the range for an open
// receiver, and one that leaves the track for a threshold set outside it.
TEST_CASE("a threshold puts the handle where it reads", "[squelch]")
{
    CHECK(slider_from_squelch(kSquelchOpenDbfs) == kSquelchSliderLowDbfs);
    CHECK(slider_from_squelch(-300.0) == kSquelchSliderLowDbfs);
    CHECK(slider_from_squelch(-150.0) == kSquelchSliderLowDbfs);
    CHECK(slider_from_squelch(-64.0) == -64.0);
    CHECK(slider_from_squelch(5.0) == kSquelchSliderHighDbfs);

    // Round trip over the whole track, so a position the operator leaves the
    // handle at is the one it is drawn at when the receiver comes back.
    for (double db = kSquelchSliderLowDbfs + 1.0; db <= kSquelchSliderHighDbfs; db += 1.0) {
        CHECK(slider_from_squelch(squelch_from_slider(db)) == db);
    }
}

// REJECTS treating any threshold under the default as a real one. The level
// never reads below -200, so -300 shuts nothing either.
TEST_CASE("anything at or under the default is the open setting", "[squelch]")
{
    CHECK(squelch_is_open_setting(kSquelchOpenDbfs));
    CHECK(squelch_is_open_setting(-300.0));
    CHECK(squelch_is_open_setting(std::numeric_limits<double>::quiet_NaN()));
    CHECK_FALSE(squelch_is_open_setting(-199.0));
    CHECK(normalise_squelch(-300.0) == kSquelchOpenDbfs);
    CHECK(normalise_squelch(-41.5) == -42.0);
}

// REJECTS "-200 dBFS" beside the slider, which reads as a setting when it is
// the absence of one, and a threshold outside the range shown as the end the
// handle is pinned at rather than the number it is.
TEST_CASE("the threshold in words", "[squelch]")
{
    CHECK(squelch_text(kSquelchOpenDbfs) == "open");
    CHECK(squelch_text(-73.0) == "-73 dBFS");
    CHECK(squelch_text(-150.0) == "-150 dBFS");
}

// REJECTS a gate indication on a receiver whose squelch cannot shut, and a
// "shut" before the first status, whose squelch_open defaults to false.
TEST_CASE("the gate is reported only when it can shut and has been measured", "[squelch]")
{
    CHECK(squelch_gate_text(kSquelchOpenDbfs, true, false).empty());
    CHECK(squelch_gate_text(kSquelchOpenDbfs, true, true).empty());
    CHECK(squelch_gate_text(-70.0, false, false).empty());
    CHECK(squelch_gate_text(-70.0, true, false) == "shut");
    CHECK(squelch_gate_text(-70.0, true, true) == "open");
}
