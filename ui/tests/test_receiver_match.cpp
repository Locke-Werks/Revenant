// classify_fit, format_width and fit_sentence: the arithmetic behind the
// one line that ends either of the two mismatch diagnoses in a glance.
//
// EVERY TEST HERE NAMES THE WRONG IMPLEMENTATION IT REJECTS, in its own
// comment, for the reason test_audio_ring.cpp gives.
//
// The wrong implementations on this path are a comparison that fires on any
// difference at all, which puts a line on every correctly tuned receiver;
// one that ranks the two conditions and shows only the winner, which sends
// the operator to the wrong fix half the time; and one that prints the
// asked width unconditionally, which on the commonest clamp path prints the
// granted width twice.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "models/receiver_match.h"

using revenant::ui::classify_fit;
using revenant::ui::FitFlag;
using revenant::ui::fit_from_status;
using revenant::ui::fit_sentence;
using revenant::ui::format_width;
using revenant::ui::has_flag;
using revenant::ui::ReceiverFit;

TEST_CASE("the 16 kHz receiver on the 145 kHz detection", "[fit]")
{
    // THE FIRST OF THE TWO THAT BIT ON 2026-09-20. A click on a broadcast
    // detection produced an NFM receiver a ninth the width of the signal,
    // and every number on screen was individually correct.
    ReceiverFit fit;
    fit.asked_low = -8'000;
    fit.asked_high = 8'000;
    fit.granted_low = -8'000;
    fit.granted_high = 8'000;
    fit.detection_bandwidth_hz = 145'000.0;

    const std::uint8_t flags = classify_fit(fit);
    CHECK(has_flag(flags, FitFlag::NarrowForSignal));
    CHECK_FALSE(has_flag(flags, FitFlag::Clamped));
    CHECK_FALSE(has_flag(flags, FitFlag::WideForSignal));

    CHECK(fit_sentence(fit) ==
          "16 kHz receiver on a 145 kHz detection: the filter is narrower "
          "than the signal.");
}

TEST_CASE("the receiver the channel clamped", "[fit]")
{
    // THE SECOND. The pane holds a 200 kHz request, the channel carries 71,
    // and the overlay's answer was a second shade of the same colour.
    ReceiverFit fit;
    fit.asked_low = -100'000;
    fit.asked_high = 100'000;
    fit.granted_low = -35'500;
    fit.granted_high = 35'500;
    fit.clamped = true;

    const std::uint8_t flags = classify_fit(fit);
    CHECK(has_flag(flags, FitFlag::Clamped));

    CHECK(fit_sentence(fit) == "asked 200 kHz, channel carries 71 kHz.");
}

TEST_CASE("both at once, and both sentences", "[fit]")
{
    // A 200 kHz request on a 145 kHz signal, clamped to 40 kHz. The two
    // facts have two different fixes: the clamp wants another grid or
    // another frequency, the width wants an edge dragged or a mode change.
    // AN IMPLEMENTATION THAT RANKED THEM sends the operator to whichever
    // one it happened to prefer, and the other stays invisible.
    ReceiverFit fit;
    fit.asked_low = -100'000;
    fit.asked_high = 100'000;
    fit.granted_low = -20'000;
    fit.granted_high = 20'000;
    fit.clamped = true;
    fit.detection_bandwidth_hz = 145'000.0;

    const std::uint8_t flags = classify_fit(fit);
    CHECK(has_flag(flags, FitFlag::Clamped));
    CHECK(has_flag(flags, FitFlag::NarrowForSignal));

    CHECK(fit_sentence(fit) ==
          "40 kHz receiver on a 145 kHz detection: the filter is narrower than "
          "the signal.  asked 200 kHz, channel carries 40 kHz.");
}

TEST_CASE("a clamp whose request this window never stated", "[fit]")
{
    // THE COMMONEST CLAMP PATH, AND THE ONE A NAIVE SENTENCE GETS WRONG. A
    // tune and a mode change both send an empty passband, so the engine
    // answers with the mode's default and EngineLink adopts the granted
    // pair as the request. asked then equals granted and the engine's own
    // flag is the only evidence there was a clamp at all.
    //
    // An implementation that printed the asked width unconditionally says
    // "asked 71 kHz, channel carries 71 kHz", which reads as a bug in the
    // line rather than as a clamp in the engine.
    ReceiverFit fit;
    fit.asked_low = -35'500;
    fit.asked_high = 35'500;
    fit.granted_low = -35'500;
    fit.granted_high = 35'500;
    fit.clamped = true;

    CHECK(has_flag(classify_fit(fit), FitFlag::Clamped));
    CHECK(fit_sentence(fit) == "the channel clamped this filter to 71 kHz.");
}

TEST_CASE("a grant short of the request is a clamp even without the flag", "[fit]")
{
    // The flag is the engine's answer and the subtraction is this window's.
    // Neither alone is enough: the flag catches an edge pulled in with the
    // total width unchanged, and the subtraction catches an engine that
    // granted less without setting it.
    ReceiverFit fit;
    fit.asked_low = -100'000;
    fit.asked_high = 100'000;
    fit.granted_low = -35'500;
    fit.granted_high = 35'500;
    fit.clamped = false;

    CHECK(has_flag(classify_fit(fit), FitFlag::Clamped));
}

TEST_CASE("a filter that fits says nothing", "[fit]")
{
    // THE CASE THAT DECIDES WHETHER ANYBODY READS THE LINE. A filter is
    // supposed to be somewhat narrower than the occupied band the detector
    // measures, because the detector's figure includes the skirts. An
    // implementation comparing for equality puts a line on every correctly
    // tuned receiver in the window.
    ReceiverFit fit;
    fit.asked_low = -6'250;
    fit.asked_high = 6'250;
    fit.granted_low = -6'250;
    fit.granted_high = 6'250;
    fit.detection_bandwidth_hz = 14'000.0;

    CHECK(classify_fit(fit) == 0);
    CHECK(fit_sentence(fit).empty());

    // A WFM receiver on a broadcast signal, which is the pairing that is
    // right.
    ReceiverFit wide;
    wide.asked_low = -80'000;
    wide.asked_high = 80'000;
    wide.granted_low = -80'000;
    wide.granted_high = 80'000;
    wide.detection_bandwidth_hz = 145'000.0;
    CHECK(classify_fit(wide) == 0);
}

TEST_CASE("a filter far wider than the signal is the other half of the mistake", "[fit]")
{
    // A WFM receiver left on a narrowband voice channel passes two parts
    // noise to one part audio. An implementation with only the narrow test
    // says nothing about it, and the operator hears hiss and blames the
    // squelch.
    ReceiverFit fit;
    fit.asked_low = -100'000;
    fit.asked_high = 100'000;
    fit.granted_low = -100'000;
    fit.granted_high = 100'000;
    fit.detection_bandwidth_hz = 12'500.0;

    CHECK(has_flag(classify_fit(fit), FitFlag::WideForSignal));
    CHECK(fit_sentence(fit) ==
          "200 kHz receiver on a 12.5 kHz detection: the filter is far wider "
          "than the signal.");
}

TEST_CASE("CW is not flagged against the carrier it is working", "[fit]")
{
    // A 400 Hz CW filter against a carrier the detector measures as two
    // kilohertz wide is the correct pairing, and it is a fifth of the
    // detected width. WITHOUT THE FLOOR this fires on every CW receiver
    // ever tuned, which is exactly how a warning becomes wallpaper.
    ReceiverFit fit;
    fit.asked_low = -200;
    fit.asked_high = 200;
    fit.granted_low = -200;
    fit.granted_high = 200;
    fit.detection_bandwidth_hz = 2'000.0;

    CHECK(classify_fit(fit) == 0);
}

TEST_CASE("nothing is said before the engine has answered", "[fit]")
{
    // A granted width of zero is a receiver that has not been placed yet.
    // An implementation without this guard divides the first poll's zeros
    // into a sentence claiming a 0 Hz receiver on a 145 kHz signal, which
    // is on screen for the quarter second before the real answer arrives.
    ReceiverFit fit;
    fit.detection_bandwidth_hz = 145'000.0;
    CHECK(classify_fit(fit) == 0);
    CHECK(fit_sentence(fit).empty());

    // And a receiver placed by hand, with no detection behind it, is never
    // compared against one. There is no signal measurement, and inventing
    // one is worse than saying nothing.
    ReceiverFit by_hand;
    by_hand.asked_low = -8'000;
    by_hand.asked_high = 8'000;
    by_hand.granted_low = -8'000;
    by_hand.granted_high = 8'000;
    CHECK(classify_fit(by_hand) == 0);
}

TEST_CASE("a width is written the way the operator said it", "[fit]")
{
    // "16 kHz", not "16.0 kHz" and not "16000 Hz". The trailing zero is
    // dropped because the operator said sixteen kilohertz, and an
    // implementation using a float formatter prints six decimals or a
    // locale's decimal comma.
    CHECK(format_width(16'000) == "16 kHz");
    CHECK(format_width(145'000) == "145 kHz");
    CHECK(format_width(71'000) == "71 kHz");
    CHECK(format_width(12'500) == "12.5 kHz");
    CHECK(format_width(2'500) == "2.5 kHz");
    CHECK(format_width(999) == "999 Hz");
    CHECK(format_width(1'000) == "1 kHz");
    CHECK(format_width(0) == "0 Hz");

    // The decade boundaries, and the rounding at each. An implementation
    // that divided by 1000 in integers would print 1.5 MHz as "1 MHz".
    CHECK(format_width(1'500'000) == "1.5 MHz");
    CHECK(format_width(1'000'000) == "1 MHz");
    CHECK(format_width(2'400'000) == "2.4 MHz");
    CHECK(format_width(1'050) == "1.1 kHz");
    CHECK(format_width(1'040) == "1 kHz");

    // Negative, which a granted pair cannot produce but a subtraction of
    // two of them can if either side is read in the wrong order. It prints
    // rather than wrapping into an enormous positive.
    CHECK(format_width(-16'000) == "-16 kHz");
}

TEST_CASE("a widen drag the engine has not answered yet says nothing", "[fit]")
{
    // THE WRONG IMPLEMENTATION THIS REJECTS: building the fit from the
    // pane's own live request and the engine's last grant. Those are two
    // different requests during a drag. setReceiverPassband draws a widen
    // and deliberately does not send it until the gesture ends, because a
    // width change is a remove and an add and sending one per mouse move
    // tears the audio once per pixel. So the pane holds 40 kHz, the engine
    // still holds the 16 kHz it granted, and the subtraction reports a
    // clamp on a channel that was never asked for anything.
    //
    // Reading both numbers off the same status is what makes that
    // unavailable. The echo is the request the grant answered.
    const ReceiverFit mid_drag = fit_from_status(-8'000, 8'000, -8'000, 8'000, false, 0.0);
    CHECK(classify_fit(mid_drag) == 0);
    CHECK(fit_sentence(mid_drag).empty());

    // A pan hid the defect, which is why it survived: both edges move by
    // the same amount and the width is held, so asked and granted matched
    // even when they came from two different requests.
    const ReceiverFit panned = fit_from_status(-4'000, 12'000, -4'000, 12'000, false, 0.0);
    CHECK(fit_sentence(panned).empty());
}

TEST_CASE("the grant is compared with the request it answered", "[fit]")
{
    // The clamp still speaks. 200 kHz asked, 71 kHz granted, both off one
    // status, which is the second of the two mismatches from 2026-09-20.
    const ReceiverFit clamped =
        fit_from_status(-100'000, 100'000, -35'500, 35'500, true, 0.0);
    CHECK(has_flag(classify_fit(clamped), FitFlag::Clamped));
    CHECK(fit_sentence(clamped) == "asked 200 kHz, channel carries 71 kHz.");

    // And the empty-passband path still reaches the other sentence: a tune
    // sends zeros, the status echoes zeros, and printing an asked width of
    // nothing would read as a bug in the line.
    const ReceiverFit defaulted = fit_from_status(0, 0, -35'500, 35'500, true, 0.0);
    CHECK(fit_sentence(defaulted) == "the channel clamped this filter to 71 kHz.");
}
