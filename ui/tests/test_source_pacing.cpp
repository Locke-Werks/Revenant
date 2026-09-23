// classify_pacing and pacing_sentence, which are the rule that would have
// saved twenty minutes on 2026-09-20.
//
// EVERY TEST HERE NAMES THE WRONG IMPLEMENTATION IT REJECTS, in its own
// comment, for the reason test_audio_ring.cpp gives.
//
// The wrong implementations on this path are a bare `factor < 1.0`, which
// fires on ordinary measurement noise and trains the operator to ignore the
// line; a rule that ignores sourcePacedBy, which calls a deliberate 0.5 a
// fault; and a rule with no hysteresis, which flickers the line on and off
// across the threshold and is worse than no line at all.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "models/source_pacing.h"

using revenant::ui::classify_pacing;
using revenant::ui::pacing_is_fault;
using revenant::ui::PacingSample;
using revenant::ui::pacing_sentence;
using revenant::ui::PacingVerdict;

namespace {

[[nodiscard]] PacingSample running(double factor, double paced = 0.0)
{
    PacingSample sample;
    sample.realtime_factor = factor;
    sample.carried = true;
    sample.window_seconds = 2.0;
    sample.paced_by = paced;
    sample.engine_running = true;
    return sample;
}

// The verdict a fresh display reaches, which is the one a status line shows
// the first time it is looked at.
[[nodiscard]] PacingVerdict fresh(const PacingSample& sample)
{
    return classify_pacing(sample, PacingVerdict::Realtime);
}

}  // namespace

TEST_CASE("the bench case is called behind and says where to look", "[pacing]")
{
    // 0.20x is what the synthetic source was doing while the operator was
    // in the audio path. An implementation that only reported the number
    // and left the reading to a person passes nothing here: the sentence is
    // the deliverable.
    const PacingSample sample = running(0.20);
    const PacingVerdict verdict = fresh(sample);

    CHECK(verdict == PacingVerdict::Behind);
    CHECK(pacing_is_fault(verdict));

    const std::string text = pacing_sentence(verdict, sample);
    CHECK(text == "the source is behind: capture ran at 0.20x realtime over the last 2.0 s, "
                  "so the gaps are upstream of the audio path.");
}

// Rejects a client that calls a source behind on a mean over the whole run,
// which is what an engine built before realtimeWindowSeconds sends. On the
// owner's RTL-SDR that mean lost a third of a second for every retune and
// never got it back, so "source behind" came up after a few tunes and stayed
// while the audio was fine. The line is for a source that is behind now.
TEST_CASE("a mean over the whole run is not called behind", "[pacing]")
{
    PacingSample sample = running(0.83);
    sample.window_seconds = 0.0;

    const PacingVerdict verdict = fresh(sample);
    CHECK(verdict == PacingVerdict::WholeRun);
    CHECK_FALSE(pacing_is_fault(verdict));
    CHECK(pacing_sentence(verdict, sample).empty());

    // Including one that was showing behind when the engine behind it
    // changed.
    CHECK(classify_pacing(sample, PacingVerdict::Behind) == PacingVerdict::WholeRun);
}

// Rejects a sentence that states a rate with no span, which an operator
// reads as the rate now whatever it was measured over.
TEST_CASE("the behind sentence names the window it was measured over", "[pacing]")
{
    PacingSample sample = running(0.84);
    sample.window_seconds = 2.1;
    CHECK(pacing_sentence(fresh(sample), sample).find("over the last 2.1 s") !=
          std::string::npos);
}

// A stall the engine did not ask for, read once a second the way the client
// polls it: two seconds of the dip and then the engine's window has passed
// it. Rejects a verdict that holds on after the figure has come back, which
// is the lifetime mean's failure moved into the client.
TEST_CASE("a stall's line goes when the window has passed it", "[pacing]")
{
    PacingVerdict verdict = PacingVerdict::Realtime;
    for (const double factor : {1.0, 0.84, 0.85}) {
        verdict = classify_pacing(running(factor), verdict);
    }
    CHECK(verdict == PacingVerdict::Behind);
    verdict = classify_pacing(running(0.9995), verdict);
    CHECK(verdict == PacingVerdict::Realtime);
}

TEST_CASE("an engine with no such field says nothing at all", "[pacing]")
{
    // carried false is every engine built before the surface existed, and
    // it must produce no line. AN IMPLEMENTATION THAT DEFAULTED THE FACTOR
    // TO 0.0 AND COMPARED IT would declare every older engine permanently
    // behind, which is a false alarm on the one display whose credibility
    // this whole feature depends on.
    PacingSample sample;
    sample.carried = false;
    sample.engine_running = true;

    const PacingVerdict verdict = fresh(sample);
    CHECK(verdict == PacingVerdict::NotCarried);
    CHECK_FALSE(pacing_is_fault(verdict));
    CHECK(pacing_sentence(verdict, sample).empty());
}

TEST_CASE("nothing measured yet is not a shortfall", "[pacing]")
{
    // A factor of zero on a running engine is the first pass after a
    // connection, before any wall time has elapsed. An implementation
    // comparing it against the threshold declares 0.00x behind and puts the
    // warning on screen for the first second of every session.
    CHECK(fresh(running(0.0)) == PacingVerdict::Unmeasured);

    // And a stopped engine measures nothing. Its last factor is frozen at
    // whatever it was, so an implementation that kept classifying would
    // leave "the source is behind" on screen beside "engine stopped",
    // which are two answers to one question.
    PacingSample stopped = running(0.20);
    stopped.engine_running = false;
    CHECK(fresh(stopped) == PacingVerdict::Unmeasured);
    CHECK(pacing_sentence(PacingVerdict::Unmeasured, stopped).empty());
}

TEST_CASE("a healthy source carries no line", "[pacing]")
{
    // The ordinary case, and the one that decides whether anybody reads the
    // line when it does appear.
    for (const double factor : {0.98, 0.995, 1.0, 1.005, 1.02}) {
        const PacingSample sample = running(factor);
        const PacingVerdict verdict = fresh(sample);
        CHECK(verdict == PacingVerdict::Realtime);
        CHECK(pacing_sentence(verdict, sample).empty());
    }
}

TEST_CASE("a deliberate pace is not a fault", "[pacing]")
{
    // THE WHOLE REASON sourcePacedBy IS ON THE WIRE. An implementation that
    // read realtimeFactor alone calls this 0.50x behind, and the operator
    // who typed --pace 0.5 gets a warning about the thing they asked for.
    const PacingSample sample = running(0.50, 0.50);
    const PacingVerdict verdict = fresh(sample);

    CHECK(verdict == PacingVerdict::Paced);
    CHECK_FALSE(pacing_is_fault(verdict));
    CHECK(pacing_sentence(verdict, sample) ==
          "the source is paced at 0.50x on purpose and is holding it.");
}

TEST_CASE("a paced source can still be behind its own pace", "[pacing]")
{
    // The threshold is a ratio against the target, so it holds at any pace.
    // An implementation that compared a paced source against 1.0 would
    // never reach this state, and a source asked for 0.5 and delivering
    // 0.2 would read as working.
    const PacingSample sample = running(0.20, 0.50);
    const PacingVerdict verdict = fresh(sample);

    CHECK(verdict == PacingVerdict::PacedAndBehind);
    CHECK(pacing_is_fault(verdict));
    CHECK(pacing_sentence(verdict, sample) ==
          "the source is behind: it was paced at 0.50x and ran at 0.20x over the last "
          "2.0 s, so the gaps are upstream of the audio path.");

    // And one holding its pace within the band is not.
    CHECK(fresh(running(0.495, 0.50)) == PacingVerdict::Paced);
}

TEST_CASE("faster than realtime is a statement and not a warning", "[pacing]")
{
    // A file read as fast as the disk allows. An implementation with a
    // single two-sided threshold would colour this the same as a shortfall
    // and send the operator looking for a fault in a replay that is
    // working perfectly.
    const PacingSample sample = running(40.0);
    const PacingVerdict verdict = fresh(sample);

    CHECK(verdict == PacingVerdict::Ahead);
    CHECK_FALSE(pacing_is_fault(verdict));
    CHECK(pacing_sentence(verdict, sample) ==
          "capture is running at 40.00x realtime, which is a recording being "
          "read as fast as it can be.");
}

TEST_CASE("hysteresis stops the line flickering", "[pacing]")
{
    // A SINGLE THRESHOLD FAILS HERE. 0.98 sits between the two bounds: it
    // is not low enough to raise the warning and not high enough to clear
    // one, so the answer depends on what was showing, which is the whole
    // point.
    CHECK(classify_pacing(running(0.98), PacingVerdict::Realtime) ==
          PacingVerdict::Realtime);
    CHECK(classify_pacing(running(0.98), PacingVerdict::Behind) == PacingVerdict::Behind);

    // Entering needs the lower bound.
    CHECK(classify_pacing(running(0.96), PacingVerdict::Realtime) ==
          PacingVerdict::Behind);

    // Leaving needs the upper one.
    CHECK(classify_pacing(running(0.995), PacingVerdict::Behind) ==
          PacingVerdict::Realtime);

    // A whole trajectory, which is what a pure function taking its own
    // previous answer buys: the state is the caller's and a test can drive
    // every step of it.
    PacingVerdict verdict = PacingVerdict::Realtime;
    for (const double factor : {1.00, 0.99, 0.985, 0.98, 0.975}) {
        verdict = classify_pacing(running(factor), verdict);
        CHECK(verdict == PacingVerdict::Realtime);
    }
    verdict = classify_pacing(running(0.95), verdict);
    CHECK(verdict == PacingVerdict::Behind);
    for (const double factor : {0.96, 0.975, 0.98, 0.985}) {
        verdict = classify_pacing(running(factor), verdict);
        CHECK(verdict == PacingVerdict::Behind);
    }
    verdict = classify_pacing(running(1.0), verdict);
    CHECK(verdict == PacingVerdict::Realtime);

    // The hysteresis carries across the paced pair too, which it would not
    // if the two verdicts were compared by identity rather than by whether
    // they mean behind.
    CHECK(classify_pacing(running(0.49, 0.50), PacingVerdict::PacedAndBehind) ==
          PacingVerdict::PacedAndBehind);
}

TEST_CASE("the factor prints to two places and rounds half up", "[pacing]")
{
    // std::to_string on a double gives six decimals, so a sentence built
    // from it reads "0.200000x". The integer path here is what keeps the
    // text comparable in a test at all.
    CHECK(pacing_sentence(PacingVerdict::Ahead, running(1.0)).find("1.00x") !=
          std::string::npos);
    CHECK(pacing_sentence(PacingVerdict::Behind, running(0.206)).find("0.21x") !=
          std::string::npos);
    CHECK(pacing_sentence(PacingVerdict::Behind, running(0.204)).find("0.20x") !=
          std::string::npos);
    CHECK(pacing_sentence(PacingVerdict::Behind, running(0.005)).find("0.01x") !=
          std::string::npos);
}
