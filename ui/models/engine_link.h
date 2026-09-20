// The one object that owns the connection, and the only place a frame
// crosses from the Cap'n Proto event loop onto the Qt thread.
//
// THE THREAD RULE THIS FILE EXISTS TO HOLD
//
// core/rpc/client.h is explicit: every Client method is synchronous from the
// caller's point of view, but the spectrum callback is invoked ON the event
// loop thread, must not call back into the same Client, and must return
// quickly, because everything else that connection does is waiting behind
// it. Nothing about a QObject may be touched from there.
//
// So the callback does three things and no more: it copies the frame into a
// buffer it owns alone, swaps that buffer under a small mutex, and posts a
// wake. The wake is a queued metacall, which is the documented way to reach
// a QObject from a foreign thread, and it carries no payload, so Qt's event
// queue never holds a megabyte of bins.
//
// WHY LATEST-WINS AND NOT A QUEUE
//
// A frame at the shipped geometry is 65536 bins of float, which is 256 KiB,
// and the engine makes one per source block. Handing each one to a queued
// signal would copy it into the event queue and grow without bound the
// moment the GUI thread stalls, which is the failure core/rpc/server.h
// already decided against on the wire. Three buffers cycle here instead: the
// callback's staging copy, the one waiting, and the one the items are
// drawing. A frame arriving before the last was drawn replaces it. For a
// waterfall that is the right answer anyway, since the newest frame is the
// one worth drawing.
//
// THE THREE THREADS
//
// The Qt thread owns everything a property getter reads and everything an
// item draws. The Cap'n Proto event loop thread, inside the Client, runs
// on_frame. A third thread, the supervisor started by start(), owns the
// Client itself: it connects, subscribes, and from then on asks the engine
// once a second whether it is still there and whether it is running.
//
// Those are two questions and Client::running answers both in one call. An
// Expected that failed is a connection that has gone; an Expected holding
// false is an engine that is there, answering, and not driving its graph.
// Reading only the first, which this file did, made a stopped engine
// indistinguishable from a running one for as long as the process stayed up.
//
// The supervisor exists because both halves of that job can block for as
// long as the far end takes. Client::connect does a TCP connect, and
// Client::running waits for a round trip on the loop thread; core/rpc/
// client.cpp's run() is explicit that a lost connection does not end the
// loop, it just makes every later call fail, so the failure is something
// this side has to go and ask for. Asking on a timer on the GUI thread
// would put both of those stalls in front of the window. Here a slow engine
// delays the next reconnection attempt and nothing else.
//
// WHY THERE IS A CLOCK AS WELL
//
// Every number this object publishes is written when a frame arrives. That
// works for all of them but one. frameRate is a statement about frames
// arriving, so the situation that makes it wrong is exactly the situation
// that stops it being written: the engine stops producing and holds the RPC
// connection up, drain() never runs, and a rate computed only in drain()
// keeps reporting the last one it measured. A status line naming a rate over
// a picture that has not moved in a minute is worse than no rate at all,
// because it is believable.
//
// So a QTimer on the Qt thread closes the measurement window when no frame
// does, and report_rate is the one place the window is closed from either
// way. It is not a repaint tick and must never become one: it emits
// rateChanged alone. frameChanged means a new frame is in frame(), and
// render/waterfall_item.cpp advances its ring by one row on every one it
// receives, so a tick that emitted frameChanged would scroll the waterfall
// with copies of the last row while the engine was stopped, which is the
// frozen rate's same lie told in pixels.
//
// THE THREE PLACES A FRAME GOES MISSING, AND WHICH COUNTER HOLDS EACH
//
// This block used to end by saying that Client::frames_dropped reported the
// same decision taken one layer down, so a choppy display could say which
// layer skipped. It reported no such thing, and the correction is recorded
// rather than quietly swapped, because the sentence was plausible enough to
// be believed twice.
//
// That counter is structurally zero. core/rpc/client.cpp increments it only
// on re-entrant delivery, and the client cannot re-enter: frame() is
// answered after the callback returns, and the loop thread is the only
// thread that dispatches frame(). tests/rpc/test_rpc_spectrum.cpp pins it at
// zero against a subscriber slow enough to make the engine drop, on purpose.
// Meanwhile the replacement this file does in on_frame was counted nowhere.
// So the status line read "0 dropped" however badly the GUI stalled, which
// is the one thing a dropped-frame counter must never do.
//
// What is true is that a client which cannot keep up makes the ENGINE drop,
// and the only trace of that on this side is SpectrumFrame::sequence. It is
// the engine's own frame counter, not this subscription's, so the distance
// between two frames that arrive says exactly how many the engine made in
// between and did not send. Split by cause, which is the whole point of
// counting at all:
//
//   framesSkipped          the engine never offered these, because this
//                          client asked for one frame in every_nth. Not a
//                          loss: it grows steadily whenever every_nth is
//                          above one and a healthy display has a large one.
//   framesDroppedByEngine  the engine had a frame at the rate asked for and
//                          threw it away, because the previous one had not
//                          been answered yet, or because its own loop had
//                          not got to it. With one subscriber those are the
//                          same stall: the callback, or whatever sits behind
//                          it, is too slow.
//   framesDroppedByUi      this link replaced a frame in the hand-off slot
//                          before the Qt thread came for it. The GUI thread
//                          is too slow, which is a different stall from the
//                          one above and has a different fix.
//
// framesReceived plus framesDroppedByEngine plus framesSkipped is every
// frame the engine produced between the first that arrived and the latest,
// so a set that stops adding up is a counter that has drifted.
// framesDroppedByUi is not in that sum: those frames did arrive and were
// counted, and were then thrown away here. All four are per connection and
// restart at zero when the supervisor reconnects, because they describe one
// subscription and the sum above is only true within one.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QtQmlIntegration>

#include "core/rpc/client.h"
#include "core/rpc/types.h"

namespace revenant::ui {

// The top of confidenceBar's range, which is the largest double below one
// and deliberately not one.
//
// core/rpc/server.cpp refuses a bar of exactly one and says why: a track's
// confidence rises by a fraction of its remaining distance to one, so it
// approaches one and never arrives, and a bar of one lists nothing however
// strong the signal is. An empty list is also what a dead band looks like,
// so the engine refuses rather than answering emptily.
//
// "Never arrives" is the limit, and the limit is not what the arithmetic
// does at every setting the detector accepts. In doubles the iteration
// stops, and where it stops depends on confidence_rise: at a rise of one
// half or more the last step lands on exactly one, so a saturated track's
// confidence IS one and the engine is refusing a bar that would have
// listed it. confidence_fixed_point below maps that across the whole range
// core/detect/detector.cpp validates, because the paragraphs here turn on
// which part of the map the engine is running in. At the shipped rise the
// sentence above holds as written.
//
// setConfidenceBar clamped to [0, 1] INCLUSIVE until 2026-09-19, which put
// the one value the engine refuses inside the range a writable property
// accepts. Writing 1.0 made every later detections call fail; poll_detections
// swallows a failed poll by design, because that is how a dead engine is
// normally found, so the overlay stopped updating and nothing said why.
//
// EngineLink::maxConfidenceBar publishes this so a control can take its
// maximum from the same constant the clamp uses. ui/qml/Main.qml's
// confidence slider binds its `to` to that property, so the engine's rule is
// written down once. It carried a hardcoded 0.95 until 2026-09-20, which was
// neither this constant nor anything that would follow it.
//
// That binding does not move this particular slider, and the comment there
// says so at length. QQuickSlider::setTo drops an assignment that is
// qFuzzyCompare-equal to the value the property holds, `to` starts at one,
// and this constant is 1.1e-16 short of one, so the slider's top of travel
// is exactly one: measured on Qt 6.8.3, where a `to` of 1 - 1e-11 is taken
// and 1 - 1e-12 is not. So the slider's RANGE does not keep a refused bar
// off the wire, and anything relying on `to` for that is relying on nothing.
//
// What does keep it off is the `bar` expression in ui/qml/Main.qml, which
// pins any handle position at or past the stop to maxConfidenceBar before
// the property is ever written. setConfidenceBar's clamp sits behind that
// as a backstop for a writer that is not the slider.
//
// WHAT THIS PARAGRAPH USED TO SAY
//
// Until 2026-09-20 it ended "setConfidenceBar below is therefore what stands
// between a handle at full travel and the bar the engine refuses. It is not
// a second line behind the slider's range. It is the only line." The first
// half of that was true when it was written and the QML pin landed in the
// same branch a commit later, which made it the second line rather than the
// only one, and the one that no longer fires on a drag. Recorded rather than
// swapped because a reader tracing what the UI actually sends at full travel
// would have been sent to the wrong file.
//
// epsilon is 2^-52 and the spacing of doubles just below one is 2^-53, so
// this is exactly std::nextafter(1.0, 0.0), written in a form that is
// constexpr rather than depending on constexpr <cmath>.
//
// WHAT THIS PARAGRAPH USED TO SAY
//
// Until 2026-09-20 it called the top of the slider's travel "a live position
// rather than a dead stop": that core/detect/detector.cpp's iteration has
// its fixed point at exactly this value, that core/rpc/server.cpp compares
// with >= so a saturated track sits on the bar and is still listed, and that
// an empty list at full travel "is prevented by a margin of one ulp". The
// fixed point is real and is asserted below. The general claim built on it
// is false, and it was measured on the one scene that cannot show the
// failure: a synthetic wideband scene, eight emitters, seed 4242, whose
// emitters never stop transmitting.
//
// What saturation costs. A track is born at confidence_rise and then takes
// confidence_rise of its remaining distance to one on every detection, so
// that distance multiplies by 1 - rise per hit. At the shipped 0.35 the
// fixed point is 84 detections past birth, 85 in all, which is 8.4 seconds
// at decision_interval_seconds = 0.1. They have to be consecutive. A
// decision a track is not detected in multiplies its confidence by
// 2^(-elapsed / confidence_half_life_seconds), and from the fixed point one
// missed decision at those settings costs 78 of the 84 back.
//
// So the top of the travel lists a carrier that has been up for 8.4 seconds
// without a gap in it, and lists nothing else. The band this project tests
// against is the counter-example rather than an edge case: 461 MHz, a two
// second PTT over, 20 decisions and 21 detections counting birth, so
// confidence is 1 - 0.65^21, which is 1 - 1.2e-4 and below the bar. The row
// reads 0 of 1 tracks shown, which is the empty-list-looks-like-a-dead-band
// failure this constant is named for, reached from the other side.
// docs/detection.md describes that traffic exactly: a few seconds of speech
// at a time with long dead air between.
//
// The stop stays reachable and the range stays this wide. Clamping lower
// means choosing a confidence a burst can clear, and that number is a guess
// about traffic a client cannot make: the detector's rise, its half life and
// the band all move it, and two of the three can change under a client that
// never hears about it. What changed instead is that the control says what
// it does there. ui/qml/Main.qml prints the stop as a saturation bar rather
// than as toFixed(2)'s 1.00, which was the one value the engine refuses.
inline constexpr double kMaxConfidenceBar =
    1.0 - std::numeric_limits<double>::epsilon() / 2.0;

// Where detector.cpp's confidence iteration settles, in the arithmetic it
// runs in rather than in the limit.
//
// A track is born at `rise` and every detection replaces c with
// c + (1 - c) * rise. In real arithmetic that approaches one forever. In
// doubles it stops, because past some point the increment rounds away, and
// where it stops is not one value for all rise: the paragraph above turns on
// it being exactly kMaxConfidenceBar and nothing checked that.
[[nodiscard]] constexpr double confidence_fixed_point(double rise) {
    double confidence = rise;
    // Bounded, though not against the input the previous comment named. It
    // said the cap stopped a rise near the detector's 0.001 floor hanging a
    // compile. Measured with MSVC 19.44.35228: that floor converges on its
    // own in 30390 iterations and compiles without complaint, a rise of zero
    // converges on the first iteration, and a rise of -1 runs to negative
    // infinity and stops there in 1023. The cap engages only
    // below a rise of about 0.000292, a third of the floor detector.cpp
    // enforces, so nothing this file or that config can produce reaches it.
    //
    // It stays, because a caller outside that range would otherwise spend
    // hundreds of thousands of iterations in a constant expression on an
    // answer nobody here wants. What changed is what happens when it fires.
    // Falling out of the loop used to return the last iterate, which is not
    // a fixed point, to assertions that read it as one: the two written
    // with < would have passed on it and reported a guard that never ran.
    //
    // A NaN was the obvious replacement and it does not work here. MSVC
    // 19.44.35228 folds a NaN comparison in a constant expression as though
    // it were ordered: with the cap forced low, nan < kMaxConfidenceBar
    // evaluated TRUE at compile time and false at run time in the same
    // binary, so both < assertions would have kept passing. Throwing makes
    // the evaluation non-constant instead, and a non-constant evaluation is
    // not something a comparison rule can reinterpret: every assertion below
    // then fails to compile and the diagnostic names this line. Nothing
    // calls this at run time, and no rise detector.cpp accepts would reach
    // the throw if something did.
    for (int step = 0; step < 100000; ++step) {
        const double next = confidence + (1.0 - confidence) * rise;
        if (next == confidence) {
            return confidence;
        }
        confidence = next;
    }
    throw "confidence_fixed_point did not converge inside its iteration cap";
}

// core/detect/DetectorConfig::confidence_rise, copied rather than included,
// and NOTHING BINDS THE COPY TO THE ORIGINAL. Read the next four paragraphs
// before trusting anything below that mentions 0.35.
//
// It cannot be included. ui/CMakeLists.txt links no part of the engine on
// purpose and states the rule from this side; core/rpc/CMakeLists.txt states
// it from the other. detector.h is not reachable here and making it reachable
// means giving up the two-runtime split, which is a standing decision and not
// an obstacle to route around.
//
// It cannot be checked at runtime either, which was the other way out. The
// detector's configuration is not on the wire: core/rpc/types.h's
// DetectionList carries decisions, total, the detection threshold and the
// hold, and no confidence_rise. The only engine value that reflects the rise
// is a track's confidence after it saturates, which takes tens of seconds of
// unbroken carrier to observe and says nothing until then.
//
// So what the assertions below actually guard, stated plainly because the
// first version of them overstated it. They are arithmetic over an input
// this file owns. They fire when confidence_fixed_point stops agreeing with
// the map measured beside them, which is what a compiler change or a careless
// edit to the iteration would do. They CANNOT fire when somebody edits
// confidence_rise in detector.h, which is the failure the old assertion
// message named, and the lane that added them "proved they fire" by editing
// this copy, which is the guard watching its own input.
//
// What keeps it in step is therefore the map and not the number. The
// assertions pin the whole range detector.cpp validates, [0.001, 1.0]
// inclusive per its require_range call, rather than the one value copied
// here, so a reader who finds the engine on a different rise can place it on
// the map without rerunning anything. Moving this copy to match a changed
// engine is then a one-line edit whose consequences are already written down.
inline constexpr double kDetectorConfidenceRise = 0.35;

// WHERE THIS IS COMPILED, WHICH IS NOT CI
//
// Only ui/ compiles this header, and no CI leg configures ui/: the root
// CMakeLists.txt refuses REVENANT_BUILD_UI outright because one cache cannot
// hold both runtimes. So the 281 tests and every job in
// .github/workflows/ci.yml go past these assertions without compiling them.
// They stop anyone who builds the client, which is everyone who ships it and
// nobody who merges. Closing that needs a CI job configuring ui/ against the
// dynamic triplet, which is a change to that workflow and is not made here.
//
// THE MAP. Measured 2026-09-20 with MSVC 19.44.35228 at /fp:precise, by
// bisection over doubles, and each boundary is asserted below rather than
// described:
//
//   rise <= 0.25            settles BELOW the bar: two ulps below one at
//                           0.25 itself, and further short as the rise
//                           falls, 5.6e-14 short at the 0.001 floor
//   0.25 < rise < 0.5       settles on exactly kMaxConfidenceBar
//   rise >= 0.5             settles on exactly one
//
// The boundaries are exact and adjacent doubles either side of them are
// asserted, because "below a quarter" was the previous wording and it is off
// by the endpoint: a rise of exactly 0.25 stalls too, and the first rise that
// reaches the bar is the very next double above it.
//
// Only the middle band makes the paragraphs on kMaxConfidenceBar true. In the
// bottom band the top of the slider's travel is a bar no track can ever
// clear, so the list there is empty for every signal, which is the failure
// that constant is named for. In the top band a saturated track's confidence
// is exactly one, so the engine's "one is never reached" is false and its
// refusal of a threshold of one refuses a bar that would have worked.

// The shipped rise, which sits in the middle band. This is the identity every
// claim about the top of the slider's travel rests on.
static_assert(confidence_fixed_point(kDetectorConfidenceRise) == kMaxConfidenceBar,
              "confidence_fixed_point no longer puts the shipped rise on kMaxConfidenceBar. "
              "Either the iteration changed or this compiler rounds it differently. The "
              "paragraphs above about the top of the slider's travel are what to fix, not "
              "this number.");

// The bottom of the range detector.cpp accepts, and the bottom band's
// behaviour at its widest. The iteration runs 30390 times here, which is the
// most expensive call in this file and still compiles.
static_assert(confidence_fixed_point(0.001) < kMaxConfidenceBar,
              "confidence_fixed_point disagrees with the measured map at the detector's rise "
              "floor of 0.001");

// The lower boundary, both sides of it. A quarter is IN the bottom band; the
// next double above a quarter is the first that reaches the bar.
static_assert(confidence_fixed_point(0.25) < kMaxConfidenceBar,
              "confidence_fixed_point disagrees with the measured map at rise 0.25, which is "
              "the last rise whose confidence never reaches the bar");
static_assert(confidence_fixed_point(0.25 + std::numeric_limits<double>::epsilon() / 4.0) ==
                  kMaxConfidenceBar,
              "confidence_fixed_point disagrees with the measured map one double above rise "
              "0.25, which is the first rise whose confidence reaches the bar");

// The upper boundary, both sides of it. At a half and above the increment
// from one ulp below rounds up, so a saturated track lands on exactly one.
static_assert(confidence_fixed_point(0.5 - std::numeric_limits<double>::epsilon() / 4.0) ==
                  kMaxConfidenceBar,
              "confidence_fixed_point disagrees with the measured map one double below rise "
              "0.5, which is the last rise that settles on the bar");
static_assert(confidence_fixed_point(0.5) == 1.0,
              "confidence_fixed_point disagrees with the measured map at rise 0.5, which is "
              "the first rise whose confidence reaches exactly one");

// The top of the range detector.cpp accepts. A rise of one closes the whole
// gap on the first detection, which is the case require_reachable_confidence
// in core/detect/detector.cpp singles out as the one where a threshold of one
// is reachable.
static_assert(confidence_fixed_point(1.0) == 1.0,
              "confidence_fixed_point disagrees with the measured map at rise 1.0, the top of "
              "the range the detector accepts");

// The narrowest passband a drag will produce, in hertz.
//
// A display-side stop and not the engine's floor, which the engine does not
// publish and could not: the real limit is wherever design_fine_taps and the
// tap cap refuse, and that depends on the channel rate, so there is no
// constant a client can hold. Fifty hertz is narrower than any filter a
// receiver ships and wide enough that the engine builds it on every grid
// this project uses, so the drag stops somewhere honest rather than handing
// the operator a refusal they cannot act on.
//
// The alternative, letting the drag go narrower and showing the engine's
// refusal, is more truthful and feels broken: the handle keeps moving and
// nothing happens.
inline constexpr int kMinPassbandWidthHz = 50;

// How far a keystroke moves an edge. Ten hertz plain, a hundred with shift,
// one with control, which is the ordinary three-speed a tuning control has.
inline constexpr int kPassbandStepHz = 10;
inline constexpr int kPassbandCoarseStepHz = 100;
inline constexpr int kPassbandFineStepHz = 1;

// The demodulator names, in the ordinal order rpc::Demod declares them, so
// the table and the enum cannot drift the way two hand-written lists would.
//
// A copy of engine::demod_name, and a copy on purpose: this process links no
// part of the engine, which is the whole reason ui/CMakeLists.txt exists.
// core/rpc/convert.h holds the static_asserts that keep rpc::Demod ordinal
// for ordinal with engine::Demod, so the ORDINALS here are pinned by the
// engine's own build even though the spellings are not.
inline constexpr const char* kDemodNames[] = {"raw", "am",  "nfm", "wfm",
                                              "usb", "lsb", "dsb", "cw"};

[[nodiscard]] inline QString demod_name(rpc::Demod mode) {
    const auto index = static_cast<std::size_t>(mode);
    if (index >= std::size(kDemodNames)) {
        return QStringLiteral("unknown");
    }
    return QString::fromLatin1(kDemodNames[index]);
}

// The name back to an ordinal, or nothing when it is not one of the eight.
// Nothing rather than a default, because a mode nobody meant is a receiver
// tuned to something nobody asked for, and the mode is the one parameter
// where being wrong is inaudible until the recording turns out unusable.
[[nodiscard]] inline std::optional<rpc::Demod> demod_from_name(const QString& name) {
    for (std::size_t i = 0; i < std::size(kDemodNames); ++i) {
        if (name == QLatin1StringView(kDemodNames[i])) {
            return static_cast<rpc::Demod>(i);
        }
    }
    return std::nullopt;
}

class EngineLink : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Constructed by main() with the address and port from argv.")

    Q_PROPERTY(bool connected READ connected NOTIFY connectionChanged)
    Q_PROPERTY(QString endpoint READ endpoint CONSTANT)
    Q_PROPERTY(QString errorText READ errorText NOTIFY connectionChanged)

    // Whether the engine is driving its graph, which is a different question
    // from whether this link can reach it, and the only one of the two that
    // says whether frames are coming. An engine answers RPC calls from the
    // moment it binds its port, which core/engine/engine.cpp puts before
    // run() and leaves true after the source ends, so connected and
    // engineRunning disagree at both ends of a session and for as long as a
    // stopped engine is left up.
    //
    // It has its own signal and not connectionChanged, because the render
    // items treat connectionChanged as a new engine and clear their history
    // on it. A status line pairs this with frameRate: connected with
    // engineRunning false and 0.0 rows/s is an engine that is there and
    // stopped, which is a state an operator can act on, and it used to read
    // exactly like a healthy one.
    Q_PROPERTY(bool engineRunning READ engineRunning NOTIFY runningChanged)

    // EngineInfo, flattened to what a status line shows. Constant for the
    // length of one connection: the engine's geometry is fixed at
    // Engine::create and a change to it is a new engine, which here means a
    // new connection and another connectionChanged.
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY connectionChanged)
    Q_PROPERTY(QString deviceVendor READ deviceVendor NOTIFY connectionChanged)
    Q_PROPERTY(bool deviceDiscrete READ deviceDiscrete NOTIFY connectionChanged)
    Q_PROPERTY(int sourceRate READ sourceRate NOTIFY connectionChanged)
    Q_PROPERTY(int channelRate READ channelRate NOTIFY connectionChanged)
    Q_PROPERTY(int gridChannels READ gridChannels NOTIFY connectionChanged)
    Q_PROPERTY(int bins READ bins NOTIFY connectionChanged)
    Q_PROPERTY(bool spectrumEnabled READ spectrumEnabled NOTIFY connectionChanged)

    // The engine did not build what was asked for, and the sentence saying
    // so. Constant for one connection for the same reason as the fields
    // above it, and false on a connection that never came up.
    //
    // A clamp is silent everywhere else: the engine takes 2048 channels when
    // 4096 were asked for and goes on serving frames that look correct, and
    // the operator finds out when a frequency lands in the wrong channel.
    // core/engine/engine.cpp says exactly that where it sets the field.
    Q_PROPERTY(bool clamped READ clamped NOTIFY connectionChanged)
    Q_PROPERTY(QString clampReason READ clampReason NOTIFY connectionChanged)

    // The span the frames cover, in absolute hertz. Both are
    // frequencyAtFraction at the two ends; see the comment on it for why the
    // arithmetic is shaped the way it is.
    Q_PROPERTY(double spanLowHz READ spanLowHz NOTIFY connectionChanged)
    Q_PROPERTY(double spanHighHz READ spanHighHz NOTIFY connectionChanged)

    // Per frame. framesDropped is the two unrequested losses added together,
    // which is the one number a status line has room for; the two beside it
    // say which layer, and framesSkipped is decimation that was asked for
    // rather than a loss at all. See the block at the top of this file.
    Q_PROPERTY(qulonglong framesReceived READ framesReceived NOTIFY frameChanged)
    Q_PROPERTY(qulonglong framesDropped READ framesDropped NOTIFY frameChanged)
    Q_PROPERTY(qulonglong framesDroppedByEngine READ framesDroppedByEngine NOTIFY frameChanged)
    Q_PROPERTY(qulonglong framesDroppedByUi READ framesDroppedByUi NOTIFY frameChanged)
    Q_PROPERTY(qulonglong framesSkipped READ framesSkipped NOTIFY frameChanged)
    Q_PROPERTY(qulonglong sequence READ sequence NOTIFY frameChanged)
    Q_PROPERTY(double floorDb READ floorDb NOTIFY frameChanged)
    Q_PROPERTY(double ceilingDb READ ceilingDb NOTIFY frameChanged)

    // Rows a second actually reaching the items, which is neither the rate
    // the engine offers nor the rate this link receives. Those three differ
    // the moment anything is dropping, and this is the one an operator is
    // looking at the display to judge: it is how fast the picture moves.
    // framesReceived minus framesDroppedByUi is the same quantity totalled
    // rather than per second.
    //
    // rateChanged and not frameChanged, because this is the one property
    // that has to be able to change when no frame arrives. See WHY THERE IS
    // A CLOCK AS WELL at the top of this file.
    Q_PROPERTY(double frameRate READ frameRate NOTIFY rateChanged)

    // The two detection knobs, which are different things and must not read
    // as one control.
    //
    // confidenceBar is client side. It is the min_confidence argument to
    // Client::detections and it filters what comes back over the wire;
    // moving it changes nothing about the engine. Writes are clamped into
    // [0, kMaxConfidenceBar], which is the half-open range the engine
    // accepts rather than the closed one this used to allow.
    //
    // detectionThresholdDb is engine side. Writing it calls
    // Client::set_detection_threshold and changes what the detector decides
    // at all. Reading it returns DetectionList::detection_threshold_db,
    // which is the value IN FORCE rather than the one last asked for,
    // because several clients can set it and the last writer wins. A
    // control that echoed back its own request would lie the moment a
    // second client existed.
    Q_PROPERTY(double confidenceBar READ confidenceBar WRITE setConfidenceBar
                   NOTIFY detectionsChanged)
    Q_PROPERTY(double detectionThresholdDb READ detectionThresholdDb
                   WRITE setDetectionThresholdDb NOTIFY detectionsChanged)

    // The top of confidenceBar's range, which ui/qml/Main.qml's confidence
    // slider takes as its `to` rather than carrying a copy of. CONSTANT
    // because this is a bound on what core/rpc/server.cpp will answer at
    // all, fixed at compile time and the same for every engine.
    //
    // There is no matching property for detectionThresholdDb's bounds. That
    // one is engine side and the engine does not publish its limits, so a
    // control there is guessing either way and a property here would only
    // move the guess.
    Q_PROPERTY(double maxConfidenceBar READ maxConfidenceBar CONSTANT)

    // Rows after the confidence bar, and rows the detector holds before it,
    // so a short list and a filtered one are distinguishable on screen.
    Q_PROPERTY(uint detectionCount READ detectionCount NOTIFY detectionsChanged)
    Q_PROPERTY(uint detectionTotal READ detectionTotal NOTIFY detectionsChanged)

    // Zero decisions is the detector having been built and not having
    // decided yet, which is a different thing from an empty band and reads
    // identically without this.
    Q_PROPERTY(qulonglong detectionDecisions READ detectionDecisions
                   NOTIFY detectionsChanged)

    // The engine sample index of the last decision. Decay is measured from
    // this against sourceRate, never against a wall clock: a capture
    // replayed at forty times realtime has to decay at the rate it was
    // recorded, not the rate it is drawn.
    Q_PROPERTY(qulonglong lastDecisionSample READ lastDecisionSample
                   NOTIFY detectionsChanged)

    // Seconds of SOURCE time the detector keeps a track it has stopped
    // detecting, before it drops the track. DetectorConfig::
    // bootstrap_hold_seconds, read back off DetectionList.
    //
    // Named for the detector because it is the engine's number and not a
    // display setting. It says when a box will DISAPPEAR, which is the whole
    // of what it says; whether the interval before that is drawn as a fade,
    // a single dim step or no change at all is the display's own decision.
    // ui/render/spectrum_item.h carried a compiled-in copy of the value and
    // labelled it an assumption about the engine's configuration, which is
    // the assumption this property removes.
    //
    // Zero until the engine has answered a detections call once, and zero
    // from an engine built before the field existed. There are no rows to
    // draw at either of those moments, but a display dividing by this has to
    // check rather than assume.
    Q_PROPERTY(double detectorHoldSeconds READ detectorHoldSeconds
                   NOTIFY detectionsChanged)

    // Why the engine refused something this link asked of the detector,
    // when the engine is there and refusing rather than gone. Empty when
    // nothing it has been asked for is outstanding, and empty while the
    // connection is down.
    //
    // A detection call fails for two unrelated reasons and this link used to
    // treat them as one. Either the engine went away, which is connected and
    // errorText's business and is how a dead engine is normally found here,
    // since detections are polled four times for every liveness probe. Or
    // the engine answered and said no: a bar or a threshold outside the
    // range it accepts, or an engine with no spectrum stage, which cannot
    // build a detector at all and refuses every poll for as long as it runs.
    // Only the second is something an operator can act on, and it used to be
    // invisible, because poll_detections discards a failure by design.
    //
    // Told apart by asking rather than by parsing the message. On a failed
    // poll the link asks the engine whether it is still running, which costs
    // one extra round trip on the failure path and none on the ordinary one.
    // An answer means the connection is up and the refusal was about the
    // request, and this carries the engine's own sentence, which names the
    // bound that was missed. No answer means the engine went away, and this
    // is emptied: two error strings on screen for one event is worse than
    // one, and errorText is the one that belongs to a lost connection.
    //
    // TWO KINDS OF REFUSAL, WHICH DO NOT LAST THE SAME LENGTH OF TIME
    //
    // A refused POLL is a condition. The bar, the threshold in force or an
    // engine with no spectrum stage makes every pass fail, so the string is
    // rewritten as often as the link polls and goes away by itself the pass
    // the poll succeeds. That is the case the paragraphs above describe.
    //
    // A refused THRESHOLD WRITE is one event. The write is attempted once
    // and nothing repeats it, so published on the same terms it was cleared
    // by the next successful poll before an operator could read it, which
    // is the same as never publishing it. It is held instead, until the
    // operator writes another threshold and that write's own verdict
    // replaces it, or until the connection goes. A refused poll outranks it
    // while one is happening, because a frozen list is the worse news, and
    // the held write reappears if the poll starts working again with the
    // write still unanswered.
    Q_PROPERTY(QString detectionFault READ detectionFault NOTIFY detectionFaultChanged)

    // ------------------------------------------------------------------
    // The receiver the detail pane is on
    // ------------------------------------------------------------------
    //
    // ONE RECEIVER, AND WHY THAT IS NOT THE ENGINE'S SHAPE. The engine has
    // no primary receiver and core/engine/vrx.h argues at length that it
    // must not grow one: a channelizer that makes channels nearly free is
    // the whole reason multi-VFO stops being an accessory to a tuner. What
    // is single here is the DETAIL PANE, which is one pane showing one
    // receiver's passband, and this is that pane's state rather than the
    // engine's. A second pane is a second set of these, not a change to the
    // engine.
    //
    // EVERY WRITE HERE IS ASYNCHRONOUS. An RPC call blocks for a round trip
    // and core/rpc/client.h forbids making one from the frame callback, so
    // the Qt thread cannot make one either without stalling the window. A
    // write records what is wanted and wakes the supervisor, which applies
    // it and polls the status back. So these properties are the UI's own
    // authoritative copy on the way out and the engine's answer on the way
    // in, and the two differ for one supervisor pass after every write.
    // That is deliberate: a drag that waited for the engine to echo before
    // it redrew would move at the round-trip rate.

    // Zero when the pane is on no receiver, which is the state the window
    // comes up in and returns to when the engine goes away.
    Q_PROPERTY(qulonglong receiverId READ receiverId NOTIFY receiverChanged)

    // Absolute radio frequency, which is params.center plus the source's
    // own centre. Absolute because that is what an operator reads and what
    // is printed on a band plan; the baseband conversion belongs here
    // because EngineInfo::sourceCenter is what makes it possible and this
    // is the object that holds one.
    Q_PROPERTY(double receiverCenterHz READ receiverCenterHz NOTIFY receiverChanged)

    Q_PROPERTY(QString receiverDemod READ receiverDemod NOTIFY receiverChanged)

    // The passband this pane is asking for, in signed hertz from the
    // receiver's centre. Written by the drag and by the keyboard, read back
    // by the overlay so the rule it draws is the one the operator is
    // moving rather than the one the engine last echoed.
    Q_PROPERTY(int receiverPassbandLow READ receiverPassbandLow NOTIFY receiverChanged)
    Q_PROPERTY(int receiverPassbandHigh READ receiverPassbandHigh NOTIFY receiverChanged)

    // What the engine granted, which differs from the pair above only when
    // the channel could not carry it. Drawn in a second, dimmer shade, and
    // the difference between the two is what says WHICH edge was clamped.
    Q_PROPERTY(int receiverGrantedLow READ receiverGrantedLow NOTIFY receiverStatusChanged)
    Q_PROPERTY(int receiverGrantedHigh READ receiverGrantedHigh NOTIFY receiverStatusChanged)
    Q_PROPERTY(bool receiverClamped READ receiverClamped NOTIFY receiverStatusChanged)

    // How far either edge may reach before the channel refuses it, in hertz
    // from the receiver's centre. The drag stops here rather than letting
    // the operator pull into a clamp, because a handle that keeps moving
    // while the filter does not is a handle that lies.
    //
    // Zero until the engine has answered once, which is a limit a drag must
    // check rather than assume.
    Q_PROPERTY(int receiverEdgeLimit READ receiverEdgeLimit NOTIFY receiverStatusChanged)

    // The rate the fine stage resampled to, which is the width of the
    // passband frame's axis. A change to it is a remove and an add rather
    // than a push constant, so a drag watches it to tell a move it can send
    // live from one that will break the audio.
    Q_PROPERTY(int receiverDemodRate READ receiverDemodRate NOTIFY receiverStatusChanged)

    Q_PROPERTY(double receiverLevelDbfs READ receiverLevelDbfs NOTIFY receiverStatusChanged)

    // Whether a passband frame has arrived for this receiver, so the pane
    // can say "waiting" rather than drawing an empty band as a dead one.
    Q_PROPERTY(bool passbandActive READ passbandActive NOTIFY passbandChanged)

    // Why the engine refused something this pane asked of a receiver.
    // Empty when nothing is outstanding. Kept separate from errorText for
    // the reason detectionFault is: a refused passband is not a lost
    // engine and must not read as one.
    Q_PROPERTY(QString receiverFault READ receiverFault NOTIFY receiverFaultChanged)

    // A width change is drawn and has not been sent, because sending it
    // mid-gesture would break the audio once per pixel. It goes out on
    // release. The readout says so, because a filter that is drawn where
    // the audio is not has to admit it.
    Q_PROPERTY(bool receiverPending READ receiverPending NOTIFY receiverChanged)

public:
    explicit EngineLink(QObject* parent = nullptr);
    ~EngineLink() override;

    EngineLink(const EngineLink&) = delete;
    EngineLink& operator=(const EngineLink&) = delete;
    EngineLink(EngineLink&&) = delete;
    EngineLink& operator=(EngineLink&&) = delete;

    // Starts the supervisor thread and returns immediately. Nothing about
    // this call can fail in a way worth reporting to the caller: an engine
    // that is not running yet is the ordinary case, and the window's job is
    // to come up, say so, and keep trying. What went wrong is in errorText
    // and what state the link is in is in connected.
    //
    // every_nth is passed straight to Client::subscribe_spectrum. The engine
    // drops the rest before copying them, so asking for fewer costs it less.
    //
    // Call it on the Qt thread. It starts the rate clock as well as the
    // supervisor, and a QTimer started on any other thread never fires.
    void start(const QString& address, std::uint16_t port, std::uint32_t every_nth);

    [[nodiscard]] bool connected() const { return connected_; }
    [[nodiscard]] bool engineRunning() const { return engine_running_; }
    [[nodiscard]] QString endpoint() const { return endpoint_; }
    [[nodiscard]] QString errorText() const { return error_text_; }

    [[nodiscard]] QString deviceName() const;
    [[nodiscard]] QString deviceVendor() const;
    [[nodiscard]] bool deviceDiscrete() const { return info_.device.discrete; }
    [[nodiscard]] int sourceRate() const { return static_cast<int>(info_.source_rate); }
    [[nodiscard]] int channelRate() const { return static_cast<int>(info_.channel_rate); }
    [[nodiscard]] int gridChannels() const { return static_cast<int>(info_.grid.channels); }
    [[nodiscard]] int bins() const { return static_cast<int>(info_.spectrum.bins); }
    [[nodiscard]] bool spectrumEnabled() const { return info_.spectrum.enabled(); }

    // Absolute hertz at a fraction of the drawn span: 0 is the outer edge of
    // the first bin and 1 the outer edge of the last, which is where a
    // display's left and right edges are.
    //
    // This is the only conversion from the wire's rationals to hertz in the
    // client, and it is at the label, which is what docs/conventions.md asks
    // for. The shape of the arithmetic is the point. bin_width arrives as
    // 2400000/65536 on the shipped geometry, which is 36.62109375 Hz and not
    // a number a float holds; rounding it to the 36.621 Hz the engine prints
    // and then multiplying by 65536 bins puts the top of the span about six
    // hertz from where the engine has it. So the bin index is multiplied
    // into the numerator and the divide happens once, at the end. Every
    // product here is a whole number well inside what a double holds
    // exactly, so the only rounding in the result is the last one.
    //
    // It is Q_INVOKABLE rather than a property because the number wanted
    // depends on where a tick is, and a display decides that. A QML binding
    // calling it cannot see the properties it reads, so a binding that has
    // to follow the connection reads one of the notifying properties itself.
    [[nodiscard]] Q_INVOKABLE double frequencyAtFraction(double fraction) const;

    [[nodiscard]] double spanLowHz() const { return frequencyAtFraction(0.0); }
    [[nodiscard]] double spanHighHz() const { return frequencyAtFraction(1.0); }

    [[nodiscard]] bool clamped() const { return info_.ring_clamped; }
    [[nodiscard]] QString clampReason() const;

    [[nodiscard]] qulonglong framesReceived() const { return frames_received_; }
    [[nodiscard]] qulonglong framesDropped() const {
        return frames_dropped_engine_ + frames_dropped_ui_;
    }
    [[nodiscard]] qulonglong framesDroppedByEngine() const { return frames_dropped_engine_; }
    [[nodiscard]] qulonglong framesDroppedByUi() const { return frames_dropped_ui_; }
    [[nodiscard]] qulonglong framesSkipped() const { return frames_skipped_; }
    [[nodiscard]] qulonglong sequence() const { return display_.sequence; }
    [[nodiscard]] double floorDb() const { return display_.floor_db; }
    [[nodiscard]] double ceilingDb() const { return display_.ceiling_db; }
    [[nodiscard]] double frameRate() const { return frame_rate_; }

    // The frame the items draw. Qt thread only, and valid until the next
    // frameChanged: the buffer it refers to is swapped back into the pool
    // when the next frame is taken. An item that wants to keep a row copies
    // the row, not this.
    [[nodiscard]] const rpc::SpectrumFrame& frame() const { return display_; }

    [[nodiscard]] double confidenceBar() const { return confidence_bar_; }
    [[nodiscard]] double maxConfidenceBar() const { return kMaxConfidenceBar; }
    void setConfidenceBar(double bar);
    [[nodiscard]] double detectionThresholdDb() const { return shown_.detection_threshold_db; }
    void setDetectionThresholdDb(double threshold_db);

    [[nodiscard]] uint detectionCount() const {
        return static_cast<uint>(shown_.detections.size());
    }
    [[nodiscard]] uint detectionTotal() const { return shown_.total; }
    [[nodiscard]] qulonglong detectionDecisions() const {
        return static_cast<qulonglong>(shown_.decisions);
    }
    [[nodiscard]] qulonglong lastDecisionSample() const {
        return static_cast<qulonglong>(shown_.last_decision);
    }
    [[nodiscard]] double detectorHoldSeconds() const {
        return shown_.detector_hold_seconds;
    }
    [[nodiscard]] QString detectionFault() const { return detection_fault_; }

    // What the items draw. Qt thread only, and valid until the next
    // detectionsChanged, on the same terms frame() is: the supervisor swaps
    // a new list in and this reference then names the old one. An item
    // keeping a selection keeps the id, not a pointer into here.
    [[nodiscard]] const std::vector<rpc::Detection>& detections() const {
        return shown_.detections;
    }

    // ------------------------------------------------------------------
    // The receiver surface
    // ------------------------------------------------------------------

    [[nodiscard]] qulonglong receiverId() const { return receiver_id_; }
    [[nodiscard]] double receiverCenterHz() const;
    [[nodiscard]] QString receiverDemod() const;
    [[nodiscard]] int receiverPassbandLow() const {
        return static_cast<int>(wanted_.passband_low);
    }
    [[nodiscard]] int receiverPassbandHigh() const {
        return static_cast<int>(wanted_.passband_high);
    }
    [[nodiscard]] int receiverGrantedLow() const {
        return static_cast<int>(receiver_status_.placement.granted_low);
    }
    [[nodiscard]] int receiverGrantedHigh() const {
        return static_cast<int>(receiver_status_.placement.granted_high);
    }
    [[nodiscard]] bool receiverClamped() const {
        return receiver_status_.placement.bandwidth_clamped;
    }
    [[nodiscard]] int receiverEdgeLimit() const { return receiver_edge_limit_; }
    [[nodiscard]] int receiverDemodRate() const {
        return static_cast<int>(receiver_status_.demod_rate);
    }
    [[nodiscard]] double receiverLevelDbfs() const { return receiver_status_.level_dbfs; }
    [[nodiscard]] bool passbandActive() const { return passband_active_; }
    [[nodiscard]] QString receiverFault() const { return receiver_fault_; }
    [[nodiscard]] bool receiverPending() const { return width_uncommitted_; }

    // Puts the detail pane on a receiver at this absolute frequency in this
    // mode, adding one if there is none and retuning the one there is.
    //
    // The passband is left unstated, so the engine answers with the mode's
    // own default and the pane reads it back off the placement. That is the
    // whole reason this client carries no table of its own: the defaults
    // live in dsp::default_passband, this process links no part of the DSP,
    // and a copy here would be a second table to keep in step.
    //
    // mode is a demodulator name as core/engine/vrx.h spells it: raw, am,
    // nfm, wfm, usb, lsb, dsb, cw. An empty string keeps the mode the pane
    // already has.
    Q_INVOKABLE void tuneReceiver(double absolute_hz, const QString& mode);

    // Changes the mode in place as far as the operator is concerned, which
    // is a remove and an add underneath: the demodulator IS the stage and
    // core/engine/graph.cpp refuses to change it on a running receiver.
    //
    // The mode's default passband comes with it UNLESS the operator has
    // moved an edge on this receiver, in which case the edges they set are
    // kept. Moving to a mode whose default is one-sided from a mode whose
    // default is not would otherwise throw away a filter somebody had just
    // placed by hand.
    Q_INVOKABLE void setReceiverDemod(const QString& mode);

    // Moves the passband edges. Signed hertz from the receiver's centre,
    // low strictly below high.
    //
    // Clamped here to the engine's own limits before it is sent: to
    // receiverEdgeLimit either side, and to kMinPassbandWidthHz apart. The
    // engine would refuse or fit a request outside those, and a handle that
    // kept moving while the filter did not is a handle that lies.
    //
    // Marks the edges as touched, which is what stops a later mode change
    // replacing them with that mode's default.
    //
    // A PAN GOES OUT NOW; A WIDTH CHANGE WAITS FOR THE RELEASE.
    //
    // Which of the two this is decides whether the engine can take it in
    // place. Moving a filter of a fixed width is a push constant and a new
    // tap table, which is free. Changing its width moves the tap count,
    // because Kaiser sets the filter's length from its transition and the
    // transition is half a width, and a different tap count is a different
    // pipeline: the engine refuses it in place and the only way to apply it
    // is to remove the receiver and add it again, which stops the audio.
    //
    // So a pan is sent on every mouse move and the audio follows the
    // pointer. A widen is drawn immediately and held until
    // commitReceiverPassband, so one gesture costs one break in the audio
    // rather than one per pixel. beginReceiverDrag and endReceiverDrag are
    // what tell this object a gesture is running; outside one, every change
    // goes out at once, which is what a keystroke wants.
    //
    // A pan is free AWAY FROM THE FOLD and not against it. The transition
    // the planner can afford is bounded by the distance from the passband's
    // nearer edge to plus or minus half the demodulation rate, so on a
    // receiver whose band nearly fills its rate, panning towards the fold
    // shortens that distance and lengthens the filter. The tap count moves
    // and the engine refuses, exactly as it does for a widen. Nothing here
    // predicts it: the arithmetic lives in plan_vrx and this process links
    // none of the DSP, so the refusal is what says so and the recreate
    // behind it is what carries the gesture through.
    //
    // ONCE, AT THE RELEASE, WHICH IS NOT WHAT IT USED TO DO. The refusal
    // arrives on every pass of the drag, because the pan is re-posted on
    // every mouse move, and answering each one with a rebuild cost a
    // teardown, an add and a passband resubscription per supervisor pass
    // for as long as the pointer was down. Only the WIDTH was ever held
    // back here; a fixed-width pan went out live and the refusal behind it
    // was paid for in full. See receiver_drag_live_.
    Q_INVOKABLE void setReceiverPassband(int low, int high);

    // A gesture is starting and ending. Between them a width change is
    // drawn and not sent; the end sends whatever is outstanding.
    Q_INVOKABLE void beginReceiverDrag();
    Q_INVOKABLE void endReceiverDrag();

    // Sends an outstanding width change now. endReceiverDrag calls it.
    Q_INVOKABLE void commitReceiverPassband();

    // Moves both edges by the same amount, holding the width. This is the
    // drag on the fill between the handles.
    Q_INVOKABLE void nudgeReceiverPassband(int delta_hz);

    // Puts the mode's own default passband back, and clears the touched
    // flag so a later mode change follows the mode again.
    Q_INVOKABLE void resetReceiverPassband();

    // Takes the pane off its receiver and removes it from the engine.
    Q_INVOKABLE void removeReceiver();

    // Absolute hertz at a fraction across the passband frame, on the same
    // half-bin convention frequencyAtFraction uses for the span: 0 is the
    // outer edge of the first bin and 1 the outer edge of the last.
    //
    // The axis comes off the frame's own geometry, so this is correct on CW
    // without knowing anything about CW: bin zero is carried as what the
    // fine stage mixed to DC, which on that one mode is a pitch below the
    // receiver's centre. See PassbandGeometry.
    [[nodiscard]] Q_INVOKABLE double passbandFrequencyAtFraction(double fraction) const;

    // The inverse, and the one a drag actually needs: where a pixel
    // fraction lands as a signed offset from the receiver's centre, which
    // is the frame VrxParams::passbandLow and passbandHigh are in.
    [[nodiscard]] Q_INVOKABLE double passbandOffsetAtFraction(double fraction) const;

    // Where an offset from the receiver's centre sits across the frame, as
    // a fraction. The inverse of the line above, and what draws a rule at
    // an edge.
    [[nodiscard]] Q_INVOKABLE double passbandFractionAtOffset(double offset_hz) const;

    // The passband frame the detail items draw. Qt thread only, and valid
    // until the next passbandChanged, on the same terms frame() is.
    [[nodiscard]] const rpc::PassbandFrame& passbandFrame() const { return passband_display_; }

signals:
    // The link came up or went away. An item holding history keyed to one
    // engine's geometry clears it here, on the edge into connected: the next
    // engine may be on another frequency, and a row drawn under the old span
    // would put a signal where it never was.
    void connectionChanged();

    // A new frame is in frame(). Emitted on the Qt thread, so an item may
    // connect to it directly and repaint from the slot.
    void frameChanged();

    // The engine started or stopped driving its graph, with the connection
    // up across the change. Separate from connectionChanged so that an item
    // clearing history on a new engine does not clear it on an engine that
    // paused, and separate from frameChanged so that nothing repaints.
    void runningChanged();

    // frameRate changed. The only signal here that fires with no frame
    // behind it, which is the whole reason it exists: see WHY THERE IS A
    // CLOCK AS WELL at the top of this file. An item must not treat it as a
    // frame, and a status line reading the rate binds to this.
    void rateChanged();

    // The detector's answer changed and detections() holds the new one.
    //
    // Emitted only when the engine has actually decided again, or when the
    // rows differ, and not on every poll. A band with nothing moving in it
    // should not repaint an overlay four times a second, and
    // DetectionList::last_decision is what makes that distinguishable
    // without comparing the rows every time.
    void detectionsChanged();

    // The engine started refusing a detection call, or stopped refusing.
    //
    // Separate from detectionsChanged, which means there are new rows in
    // detections(). A refusal means the opposite: the rows are not going to
    // change until something is fixed, and an overlay that repainted on it
    // would redraw the same stale list. A status line binds here; nothing
    // that draws boxes should.
    void detectionFaultChanged();

    // The pane moved to another receiver, or its request changed. Emitted
    // by the WRITE and not by the engine's answer, because a drag redraws
    // from its own request and would otherwise move at the round-trip rate.
    void receiverChanged();

    // The engine answered about this receiver: what it granted, what it
    // clamped, what rate it is running at. Separate from receiverChanged
    // so an overlay can redraw the requested rule on every mouse move and
    // the granted one only when there is news.
    void receiverStatusChanged();

    // A new passband frame is in passbandFrame(). Emitted on the Qt
    // thread, so an item may repaint from the slot.
    void passbandChanged();

    void receiverFaultChanged();

private:
    // The supervisor thread, and the two halves of what it does.
    void supervise();
    [[nodiscard]] bool attempt_connect();
    void publish(bool connected, QString error);

    // Supervisor thread. The engine answered the probe, so the connection is
    // good and this is what it said. Posts nothing when the answer has not
    // changed, because it is asked once a second for the life of the window
    // and the answer is the same almost every time.
    void note_running(bool running);

    // Qt thread, queued from publish(). Takes the connection state the
    // supervisor left and tells QML.
    void adopt();

    // Qt thread, queued from note_running(). The running flag alone, and no
    // connectionChanged: the render items read that signal as a new engine
    // and throw away everything they have drawn.
    void adopt_running();

    // Qt thread. Closes the rate window if it is due, from drain() when a
    // frame closed it and from the tick when nothing did.
    void report_rate();

    // Invoked on the Cap'n Proto event loop thread. Touches nothing but its
    // own buffers, the swap mutex and one atomic.
    void on_frame(const rpc::SpectrumFrame& frame);

    // Qt thread. Takes whatever is waiting and tells the items.
    void drain();

    // Supervisor thread only, other than the read in start() that sets them.
    std::unique_ptr<rpc::Client> client_;
    QString address_;
    std::uint16_t port_ = 0;
    std::uint32_t requested_every_nth_ = 1;

    QString endpoint_;  // set once by start(), read from any thread after

    // Event loop thread only, so the copy out of the callback's argument
    // happens outside the lock and reuses this vector's capacity.
    rpc::SpectrumFrame staging_;

    // Event loop thread only once frames are flowing, and the arithmetic
    // behind the two engine-side counters. The supervisor writes all four
    // before it calls subscribe_spectrum, and that call hands work to the
    // loop thread and waits for it, so the loop thread cannot read a stale
    // one and there is no subscription alive to race with anyway.
    std::uint32_t every_nth_ = 1;
    std::uint64_t first_sequence_ = 0;
    std::uint64_t delivered_ = 0;
    bool have_span_ = false;

    std::mutex swap_mutex_;
    rpc::SpectrumFrame ready_;  // guarded by swap_mutex_
    bool has_ready_ = false;    // guarded by swap_mutex_

    // Written on the event loop thread, read by drain(), both under
    // swap_mutex_, so the frame and the four counters that describe it are
    // one snapshot rather than values read at four instants. The received
    // count is in here rather than taken from Client::frames_received()
    // for that reason alone: frames keep arriving between the swap and the
    // drain, and a received count that had moved on while the other three
    // had not would break the sum the header block promises.
    qulonglong pending_received_ = 0;        // guarded by swap_mutex_
    qulonglong pending_dropped_engine_ = 0;  // guarded by swap_mutex_
    qulonglong pending_dropped_ui_ = 0;      // guarded by swap_mutex_
    qulonglong pending_skipped_ = 0;         // guarded by swap_mutex_

    // The supervisor's side of the connection state, handed to the Qt thread
    // under this lock and picked up in adopt(). EngineInfo carries strings,
    // so it cannot be an atomic and a property getter must never read the
    // supervisor's copy directly.
    std::mutex state_mutex_;
    bool handover_connected_ = false;  // guarded by state_mutex_
    bool handover_running_ = false;    // guarded by state_mutex_
    rpc::EngineInfo handover_info_;    // guarded by state_mutex_
    QString handover_error_;           // guarded by state_mutex_

    std::thread supervisor_;
    std::mutex supervisor_mutex_;
    std::condition_variable supervisor_wake_;
    bool stopping_ = false;  // guarded by supervisor_mutex_

    // Qt thread only.
    rpc::SpectrumFrame display_;
    rpc::EngineInfo info_;
    bool connected_ = false;
    bool engine_running_ = false;
    QString error_text_;
    qulonglong frames_received_ = 0;
    qulonglong frames_dropped_engine_ = 0;
    qulonglong frames_dropped_ui_ = 0;
    qulonglong frames_skipped_ = 0;

    // Rows a second, over a window rather than since the connection came
    // up, so a display that stalls now reads as stalled now. frames_drawn_
    // counts drains, which is one repaint each, and is what the rate is
    // taken from; frames_received_ would answer a different question and
    // reads high by exactly the frames this link threw away.
    //
    // rate_timer_ times the open window and rate_mark_ is frames_drawn_ when
    // it opened, so the two of them plus frames_drawn_ are the whole state
    // report_rate works from and nothing has to be recorded per frame.
    qulonglong frames_drawn_ = 0;
    QElapsedTimer rate_timer_;
    qulonglong rate_mark_ = 0;
    double frame_rate_ = 0.0;

    // The detection poll. Runs on the supervisor thread because
    // Client::detections blocks for a round trip, and core/rpc/client.h
    // forbids re-entering the Client from the frame callback, so the Qt
    // thread and the Cap'n Proto loop thread are both ruled out.
    //
    // Polled faster than the supervisor's own liveness probe, because one
    // second is visibly sluggish for an overlay an operator is clicking,
    // and slower than the frame rate, because the detector decides far less
    // often than the engine produces frames and polling past that buys
    // round trips and nothing else.
    void poll_detections();
    void adopt_detections();

    // Supervisor thread. Hands the Qt thread the reason the engine refused
    // a detection call this pass, or an empty string when it refused
    // nothing. Posts only on a change, which is what keeps an engine that
    // refuses every poll from queueing a metacall four times a second for
    // the life of the window.
    void note_detection_fault(QString fault);

    // Supervisor thread. Empties the property and drops the held threshold
    // refusal with it. The two go together at exactly the moments there is
    // no engine to have refused anything: a connection torn down, and a
    // poll whose failure turned out to be the engine leaving. Clearing one
    // and not the other would put a refusal from the previous engine back
    // on screen on the next pass that polled successfully.
    void clear_detection_fault();

    // Qt thread, queued from note_detection_fault.
    void adopt_detection_fault();

    std::mutex detection_mutex_;
    rpc::DetectionList pending_detections_;   // guarded by detection_mutex_
    bool has_pending_detections_ = false;     // guarded by detection_mutex_
    QString pending_fault_;                   // guarded by detection_mutex_
    bool has_pending_fault_ = false;          // guarded by detection_mutex_

    // Supervisor thread only: the fault last handed over, so the comparison
    // that suppresses a repeat needs no lock and reads nothing the Qt
    // thread owns.
    QString posted_fault_;

    // Supervisor thread only: the engine's refusal of the last threshold
    // write, kept across passes because nothing repeats that call. Empty
    // when the last write was taken, when there has been no write, and
    // whenever the connection is not up. See the second half of
    // detectionFault's comment for why this one outlives its pass.
    QString threshold_fault_;

    // The Qt thread's copy, which is what detections() hands out.
    rpc::DetectionList shown_;

    // The Qt thread's copy of the fault, which is what detectionFault()
    // hands out.
    QString detection_fault_;

    // Read by the supervisor, written by the Qt thread. A double is not
    // torn on any platform this builds for and a stale value costs one poll
    // at the old bar, so it is atomic rather than locked.
    std::atomic<double> confidence_bar_{0.0};

    // Set by the Qt thread when the operator moves the engine-side control,
    // consumed and cleared by the supervisor on its next pass. The write
    // itself is an RPC call and cannot happen on the Qt thread.
    std::atomic<bool> threshold_pending_{false};
    std::atomic<double> requested_threshold_db_{0.0};

    // The clock behind report_rate. Lives on the Qt thread and is started by
    // start(), which main() calls from that thread before the event loop
    // begins; a QTimer started anywhere else would never fire.
    QTimer rate_tick_;

    // One wake outstanding at a time. Without it a burst of frames posts a
    // metacall each, and the GUI thread then runs a queue of calls that all
    // find the same single frame waiting.
    std::atomic<bool> wake_pending_{false};

    // ------------------------------------------------------------------
    // The receiver the detail pane is on
    // ------------------------------------------------------------------

    // Qt thread. Records what the pane now wants and wakes the supervisor
    // to go and apply it. Every Q_INVOKABLE above ends here.
    void post_receiver_request(bool recreate);

    // Supervisor thread. Applies whatever the Qt thread last asked for and
    // then reads the receiver's status back.
    void apply_receiver_request();
    void poll_receiver_status();

    // Supervisor thread. Adds a receiver for the pane, subscribes its
    // passband, and hands the id to the Qt thread. Removes the previous one
    // first, because the pane holds one.
    [[nodiscard]] bool recreate_receiver(const rpc::VrxParams& params);
    void drop_receiver();

    // Supervisor thread. Hands the Qt thread the engine's refusal, or an
    // empty string. Posts only on a change.
    void note_receiver_fault(QString fault);

    // Qt thread, queued from the supervisor.
    void adopt_receiver_status();
    void adopt_receiver_fault();

    // Invoked on the Cap'n Proto event loop thread.
    void on_passband_frame(const rpc::PassbandFrame& frame);

    // Qt thread. Takes whatever passband frame is waiting.
    void drain_passband();

    // Qt thread. The request the pane holds, fitted to the engine's limits.
    [[nodiscard]] std::pair<int, int> fit_edges(int low, int high) const;

    // Qt thread only. The pane's own authoritative copy of the request,
    // which is what the overlay draws and what the supervisor sends. Its
    // center is BASEBAND, the frame VrxParams is in; receiverCenterHz adds
    // the source's centre back for the label.
    rpc::VrxParams wanted_;
    qulonglong receiver_id_ = 0;

    // The operator has moved an edge on this receiver, so a mode change
    // keeps their edges instead of taking the new mode's default.
    bool edges_touched_ = false;

    // A gesture is running, and a width change made during it is drawn and
    // not yet sent. sent_width_ is the width the engine was last given, so
    // a change can be told from a pan without asking the engine.
    bool dragging_ = false;
    bool width_uncommitted_ = false;
    int sent_width_ = 0;

    // The gesture moved the passband at all, width or not, so the release
    // sends the final request even when nothing was held back. A pan that
    // the engine refused mid-drag is only applied by that send, and a pan
    // it accepted costs one redundant retune per gesture, which is a push
    // constant.
    bool drag_changed_ = false;

    // The same fact as dragging_, for the supervisor thread, which is the
    // one that has to decide whether a refusal is worth a rebuild.
    //
    // WHY A REFUSAL MID-DRAG IS NOT A REBUILD. A pan towards the fold
    // lengthens the filter and the engine refuses it, exactly as it does a
    // widen, and apply_receiver_request answers a refusal by removing the
    // receiver and adding it again. During a drag that request is
    // re-posted every time the pointer moves, so the receiver was torn down
    // and rebuilt once per supervisor pass for as long as the operator held
    // the mouse: a stream of audio breaks and a passband subscription
    // reattached each time, for a gesture that is going to end in one
    // position. Held instead, and applied once at the release, which is
    // what endReceiverDrag already does for a width.
    std::atomic<bool> receiver_drag_live_{false};

    // Qt thread only. The engine's last answer about this receiver, and the
    // edge limit derived from it.
    rpc::VrxStatus receiver_status_;
    int receiver_edge_limit_ = 0;
    QString receiver_fault_;
    bool passband_active_ = false;

    // The Qt thread's copy of the passband frame, and the hand-off slot the
    // event loop thread fills. Latest wins, exactly as the span's does and
    // for the same reason.
    rpc::PassbandFrame passband_display_;
    rpc::PassbandFrame passband_staging_;  // event loop thread only
    rpc::PassbandFrame passband_ready_;    // guarded by swap_mutex_
    bool has_passband_ready_ = false;      // guarded by swap_mutex_
    std::atomic<bool> passband_wake_pending_{false};

    // Written by the Qt thread, consumed by the supervisor. The params are
    // a whole struct rather than a set of atomics because they have to be
    // applied as one request: a centre from one drag and a passband from
    // the next would tune a receiver nobody asked for.
    std::mutex receiver_mutex_;
    rpc::VrxParams requested_params_;      // guarded by receiver_mutex_
    bool has_receiver_request_ = false;    // guarded by receiver_mutex_
    bool receiver_request_recreates_ = false;  // guarded by receiver_mutex_
    bool receiver_request_removes_ = false;    // guarded by receiver_mutex_
    rpc::VrxStatus pending_receiver_status_;   // guarded by receiver_mutex_
    bool has_pending_receiver_status_ = false;  // guarded by receiver_mutex_
    qulonglong pending_receiver_id_ = 0;        // guarded by receiver_mutex_
    bool has_pending_receiver_id_ = false;      // guarded by receiver_mutex_
    QString pending_receiver_fault_;            // guarded by receiver_mutex_
    bool has_pending_receiver_fault_ = false;   // guarded by receiver_mutex_

    // Supervisor thread only: the receiver it has actually created on the
    // engine, and the fault last handed over so a repeat needs no lock.
    qulonglong live_receiver_id_ = 0;
    QString posted_receiver_fault_;

    // Set by any write, cleared by the supervisor when it has applied one.
    // It is in the wait predicate, so a drag is applied on the next tick of
    // the loop rather than on the next poll interval: 250 ms of latency on
    // a filter edge is felt as the handle sticking.
    bool receiver_work_pending_ = false;  // guarded by supervisor_mutex_
};

}  // namespace revenant::ui
