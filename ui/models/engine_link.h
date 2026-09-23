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
// THE THREE THREADS THAT REACH THIS OBJECT'S OWN STATE, AND A FOURTH THAT
// REACHES ONE MEMBER OF IT
//
// The Qt thread owns everything a property getter reads and everything an
// item draws. The Cap'n Proto event loop thread, inside the Client, runs
// on_frame. A third thread, the supervisor started by start(), owns the
// Client itself: it connects, subscribes, and from then on asks the engine
// once a second whether it is still there and whether it is running.
//
// The fourth is the sound card's, and it touches audio_rings_, the mix mask,
// the lead slot and the gains, and nothing else here. It is out of this
// block's scope rather than absent from the object, which is a distinction
// this header used to lose by heading the block THE THREE THREADS full stop.
// See audioRingAt() below and audio/audio_ring.h, which counts all four
// because every one of them takes that object's lock.
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

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration>

#include "audio/audio_ring.h"
#include "core/rpc/client.h"
#include "core/rpc/types.h"
#include "models/aft.h"
#include "models/auto_filter.h"
#include "models/bookmarks.h"
#include "models/composite_probe.h"
#include "models/decoded_model.h"
#include "models/mode_choice.h"
#include "models/receiver_gone.h"
#include "models/receiver_marker.h"
#include "models/receiver_rack.h"
#include "models/receiver_scroll.h"
#include "models/scroll_tune.h"
#include "models/front_end_note.h"
#include "models/gain_control.h"
#include "models/source_pacing.h"

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
// maximum from the same constant the clamp uses. ui/qml/DetectionControls.qml's
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
// What does keep it off is the `bar` expression in ui/qml/DetectionControls.qml, which
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
// it does there. ui/qml/DetectionControls.qml prints the stop as a saturation bar rather
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

// WHERE THIS IS COMPILED
//
// Only ui/ compiles this header. The root CMakeLists.txt refuses
// REVENANT_BUILD_UI outright because one cache cannot hold both runtimes, so
// the engine's jobs go past these assertions without compiling them; the ui
// job in .github/workflows/ci.yml configures ui/ on its own against the
// dynamic triplet and is what compiles them for anyone who merges.
//
// WHAT THIS PARAGRAPH USED TO SAY. It was headed "WHERE THIS IS COMPILED,
// WHICH IS NOT CI", said "no CI leg configures ui/", and counted "the 281
// tests" that went past these assertions. The ui job landed on 2026-09-22,
// and the count was a snapshot of a suite that has grown since.
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

// The demodulator names, kDemodNames in models/mode_choice.h, which has why
// they are a copy of engine::demod_name and what pins them.
//
// WHAT THE TABLE USED TO HOLD: eight names, raw to cw, and not p25p1, dstar
// or tetra, so a receiver in one of those read "unknown" here.
[[nodiscard]] inline QString demod_name(rpc::Demod mode) {
    const auto index = static_cast<std::size_t>(mode);
    if (index >= kDemodNames.size()) {
        return QStringLiteral("unknown");
    }
    const std::string_view name = kDemodNames[index];
    return QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
}

// The name back to an ordinal, or nothing when it is not one of the eleven.
// Nothing rather than a default, because a mode nobody meant is a receiver
// tuned to something nobody asked for, and the mode is the one parameter
// where being wrong is inaudible until the recording turns out unusable.
[[nodiscard]] inline std::optional<rpc::Demod> demod_from_name(const QString& name) {
    for (std::size_t i = 0; i < kDemodNames.size(); ++i) {
        const std::string_view known = kDemodNames[i];
        if (name == QLatin1StringView(known.data(), static_cast<qsizetype>(known.size()))) {
            return static_cast<rpc::Demod>(i);
        }
    }
    return std::nullopt;
}

// One receiver as the span displays mark it: its band in absolute hertz, its
// colour slot, and whether it is the focused one. EngineLink::rackMarkers
// hands these out, focused one last so it is drawn over the others.
struct RackMarker {
    std::uint64_t key = 0;
    ReceiverBand band;
    std::size_t slot = 0;
    bool focused = false;
};

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

    // ------------------------------------------------------------------
    // Tuning the front end
    // ------------------------------------------------------------------
    //
    // WHERE THE SOURCE IS, WHICH IS NOT WHERE THE RECEIVER IS. This is the
    // centre of the whole captured span. The receiver's own centre is an
    // offset within it, and a retune moves this without moving that: the
    // receiver keeps its baseband offset, so its absolute frequency
    // follows the front end. That is what the engine does and the readouts
    // follow it rather than being corrected here.

    // The source's centre in absolute hertz, which is EngineInfo::
    // sourceCenter. It moves on a granted retune, unlike everything else
    // in EngineInfo, which is why it notifies.
    Q_PROPERTY(double sourceCenterHz READ sourceCenterHz NOTIFY connectionChanged)

    // Whether the source will retune at all, and the range it will take.
    // Answered once per connection by Session.sourceCanRetune, so a
    // control can be greyed out rather than offering something that always
    // refuses. False for every file and synthetic source.
    Q_PROPERTY(bool sourceCanRetune READ sourceCanRetune NOTIFY sourceTuningChanged)
    Q_PROPERTY(double sourceTuneLowHz READ sourceTuneLowHz NOTIFY sourceTuningChanged)
    Q_PROPERTY(double sourceTuneHighHz READ sourceTuneHighHz NOTIFY sourceTuningChanged)

    // WHY THE CONTROL IS GREYED, WHICH HAS TWO CAUSES AND THEY ARE NOT THE
    // SAME NEWS. A source that cannot retune is the ordinary answer for a
    // recording. A client compiled against an engine wire that has no such
    // call is this client's own limitation and must not read as the
    // engine's. models/wire_seam.h says which, and this carries whichever
    // sentence applies.
    Q_PROPERTY(QString sourceRetuneUnavailable READ sourceRetuneUnavailable
                   NOTIFY sourceTuningChanged)

    // The engine refused a retune, in its own words. Kept apart from
    // errorText for the reason detectionFault and receiverFault are: a
    // refused frequency is not a lost engine.
    Q_PROPERTY(QString tuneFault READ tuneFault NOTIFY sourceTuningChanged)

    // The device picker. One signal for the list, the busy flag and the
    // refusal, because one panel reads all three and none of them repaints a
    // display.
    Q_PROPERTY(QVariantList sources READ sources NOTIFY sourcesChanged)
    Q_PROPERTY(bool sourcesBusy READ sourcesBusy NOTIFY sourcesChanged)
    Q_PROPERTY(QString sourceFault READ sourceFault NOTIFY sourcesChanged)

    // Whether a source is open at all, and which stream its indices belong to.
    // On connectionChanged because both are read off EngineInfo, which is
    // replaced whole on every pass that finds the engine there.
    Q_PROPERTY(bool sourceOpen READ sourceOpen NOTIFY connectionChanged)
    Q_PROPERTY(qulonglong sourceEpoch READ sourceEpoch NOTIFY connectionChanged)

    // THE OPEN SOURCE'S GAIN STAGE, and whether there is one to draw.
    //
    // Off Session.sourceDescriptor, which describes the source that is already
    // open and touches no device, rather than off listSources, which describes
    // candidates and opens every device index to do it. That distinction is the
    // whole reason the call exists: an operator who started revenant-engine
    // with a URI on the command line, which is how it is normally run, had
    // never listed anything, so this window knew no stage and could offer no
    // control at all.
    //
    // ONE STAGE AND NOT ALL OF THEM, deliberately, and sourceGainStages says
    // how many the device reported so the window can say when it is showing
    // fewer controls than the device has. An R820T has exactly one stage
    // driving its LNA, mixer and VGA together, which is the device this was
    // written against; an Airspy has three and would want a control each. A
    // single slider labelled with the stage's own name is honest about what it
    // drives, where one slider labelled "gain" over three stages would not be.
    //
    // Empty name means no stage, which is every file and every synthetic scene:
    // their levels are a property of samples already written or of emitters
    // generated at the levels the URI asked for, so there is no amplifier to
    // turn up and the window draws nothing rather than a control that always
    // refuses.
    Q_PROPERTY(QString sourceGainStage READ sourceGainStage NOTIFY sourceGainChanged)
    Q_PROPERTY(int sourceGainStages READ sourceGainStages NOTIFY sourceGainChanged)
    Q_PROPERTY(bool sourceGainHasAuto READ sourceGainHasAuto NOTIFY sourceGainChanged)

    // Where the handle goes, as a fraction of the stage's decibel range, and
    // the step one arrow key moves. models/gain_control.h has the arithmetic
    // and why both are fractions rather than decibels.
    Q_PROPERTY(double sourceGainFraction READ sourceGainFraction NOTIFY sourceGainChanged)
    Q_PROPERTY(double sourceGainStep READ sourceGainStep NOTIFY sourceGainChanged)

    // The gain the DEVICE TOOK, in decibels, and whether that number means
    // anything yet.
    //
    // PLACED FROM THE ANSWER AND NOT FROM THE REQUEST. set_source_gain returns
    // the step the tuner landed on, which on a stepped stage is rarely what was
    // asked: a slider left where the pointer was would show a gain the device
    // is not on. sourceGainKnown is false until a gain has been granted on this
    // connection, because nothing on the wire reports a source's CURRENT gain
    // and a handle parked at a plausible-looking default would be a guess
    // presented as a reading. A window showing "not set from here" is telling
    // the truth about what it knows.
    Q_PROPERTY(double sourceGainDb READ sourceGainDb NOTIFY sourceGainChanged)
    Q_PROPERTY(bool sourceGainKnown READ sourceGainKnown NOTIFY sourceGainChanged)

    // Whether the stage has been handed to the device's own AGC from here.
    // Same caveat as sourceGainKnown: this is what this window last asked for,
    // not a reading, because the wire does not report the mode either.
    Q_PROPERTY(bool sourceGainAuto READ sourceGainAuto NOTIFY sourceGainChanged)

    // A refusal from the last gain change, empty when the last one took. The
    // source's own words: a synthetic scene says its emitter levels are set
    // against the noise, which tells an operator what to do instead.
    Q_PROPERTY(QString sourceGainFault READ sourceGainFault NOTIFY sourceGainChanged)

    // What was asked for and what the source took. A device with a tuning
    // step rounds, and the two differ by up to that step. Both zero until
    // a retune has been attempted on this connection.
    Q_PROPERTY(double tuneRequestedHz READ tuneRequestedHz NOTIFY sourceTuningChanged)
    Q_PROPERTY(double tuneGrantedHz READ tuneGrantedHz NOTIFY sourceTuningChanged)

    // A retune has been answered on this connection, so the pair above
    // means something. Without it a granted centre of zero is
    // indistinguishable from a source tuned to DC.
    Q_PROPERTY(bool tuneAnswered READ tuneAnswered NOTIFY sourceTuningChanged)

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

    // ------------------------------------------------------------------
    // Whether the SOURCE is keeping up, which is a different question
    // from every counter beside it
    // ------------------------------------------------------------------
    //
    // Every other number in this block describes what happened to a frame
    // after the engine made it: dropped on the wire, not drawn by this
    // window, decimated on request. None of them says anything about
    // whether the capture itself is short, and on 2026-09-20 a synthetic
    // source at 0.20x produced chopped audio that the operator spent
    // twenty minutes looking for in the audio path, because "starving"
    // near the volume slider is the only thing that said anything at all.
    //
    // realtimeFactor is samples of capture per wall second over the source
    // rate. sourcePacedBy is the --pace setting, zero for unthrottled, and
    // it is here because a factor of 0.5 means opposite things depending
    // on whether anybody asked for it.
    //
    // POLLED, NOT CONSTANT, unlike the rest of EngineInfo. It is a
    // measurement and it moves, so it is re-read on the probe pass once a
    // second. models/source_pacing.h turns the pair into a verdict and a
    // sentence, with hysteresis, because a threshold that flickers across
    // measurement noise is worse than no line at all.
    Q_PROPERTY(double realtimeFactor READ realtimeFactor NOTIFY pacingChanged)
    Q_PROPERTY(double sourcePacedBy READ sourcePacedBy NOTIFY pacingChanged)

    // The wire carries the measurement at all. False against an engine
    // built before the field existed, and it is what stops a missing
    // field reading as a source stopped dead.
    Q_PROPERTY(bool pacingCarried READ pacingCarried NOTIFY pacingChanged)

    // The sentence, empty when there is nothing to say, which is the
    // ordinary case. A status strip that always carries a line is a
    // status strip nobody reads.
    Q_PROPERTY(QString pacingText READ pacingText NOTIFY pacingChanged)

    // Whether that sentence is bad news. A source running at 40x is a
    // recording being read fast and a source paced at 0.5 was asked for;
    // neither is coloured like a shortfall.
    Q_PROPERTY(bool sourceBehind READ sourceBehind NOTIFY pacingChanged)

    // What the full-span spectrum says about the front end.
    //
    // THE SECOND DIAGNOSIS NOBODY COULD MAKE, and the same shape as the
    // pacing pair above. Measured on air 2026-09-20 with an RTL-SDR v3 at
    // 95.1 MHz: with gain=auto the detector reported three intermodulation
    // products as real tracks at confidence 1.00, and setting gain to 20
    // improved KKFM's measured SNR by 5.7 dB and removed all three. What
    // this window showed was three confident rows in the detection list,
    // which is also what a real band looks like.
    //
    // POLLED ON THE SAME PASS, off SourceStats rather than EngineInfo,
    // because it is a live measurement and EngineInfo is what the engine
    // settled on at open. models/front_end_note.h turns it into a
    // sentence; core/detect/front_end.h is the measurement and is the
    // authority on what it cannot tell apart.
    //
    // UNMEASURED UNTIL SOMETHING ASKS FOR DETECTIONS, because it is
    // computed from the detector's own arrays and the engine builds a
    // detector on demand. This window asks on every detection pass, so it
    // is measured a few seconds after a connection that is drawing.
    Q_PROPERTY(QString frontEndText READ frontEndText NOTIFY pacingChanged)

    // Whether that sentence is bad news. A gain control moving the whole
    // span is a statement; a floor outrunning the signal is what costs
    // detections, and the two must not be the same colour.
    Q_PROPERTY(bool frontEndFault READ frontEndFault NOTIFY pacingChanged)

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

    // The bar on Detection::margin_confidence, which is the other half of the
    // pair and the one an operator chasing interference reaches for.
    //
    // THE SAME RANGE AND THE SAME CLAMP AS confidenceBar, for the same reason:
    // the margin map approaches one without arriving, so a bar of one is a
    // filter that can only be empty and the engine refuses it. The useful
    // range starts at a half, because every published detection cleared the
    // detection threshold and the map is a half AT the threshold, so anything
    // below that passes the whole list.
    //
    // This window's own, like confidenceBar and unlike the dB threshold:
    // nothing on the engine changes and another client's list is unaffected.
    Q_PROPERTY(double marginBar READ marginBar WRITE setMarginBar
                   NOTIFY detectionsChanged)
    Q_PROPERTY(double detectionThresholdDb READ detectionThresholdDb
                   WRITE setDetectionThresholdDb NOTIFY detectionsChanged)

    // The top of confidenceBar's range, which ui/qml/DetectionControls.qml's confidence
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
    //
    // A THIRD WAY IT REACHES ZERO, AND THE OPERATOR DID NOT ASK FOR IT. A
    // receiver is pinned to the absolute frequency it was tuned to, so a
    // retune of the front end that leaves that frequency outside the new span
    // removes the receiver from the engine rather than carrying it along to
    // whatever now lands at its old offset. The pane then empties exactly as
    // it does for a clear, receiver and audio and passband together, with
    // nothing left on screen naming what went. EngineLink::
    // forget_removed_receiver is where that is noticed.
    //
    // So QML reads a receiver disappearing as an ordinary event rather than as
    // a fault. Nothing is published to say it happened: the span moved under
    // the operator's own hand, and the empty pane is the whole of the news.
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

    // WHY THE RECEIVER IS WRONG FOR THE SIGNAL, in one line, or empty.
    //
    // Two mismatches bit on 2026-09-20 and neither said anything. A click
    // on a 145 kHz broadcast detection produced a 16 kHz NFM receiver, and
    // every number on screen was individually correct: the detection said
    // 145 kHz, the readout said 16 kHz, and nothing put the two together.
    // A second receiver asked for 200 kHz and got 71, which the overlay
    // answered with a dimmer shade of the same colour.
    //
    // Both are subtraction over numbers this object already holds.
    // models/receiver_match.h does the arithmetic and writes the sentence,
    // with its own cases in ui/tests, and this carries the result.
    //
    // The detection comparison is only made when the receiver was placed
    // BY a detection. A receiver tuned by hand or from the frequency box
    // has no measured signal behind it, and inventing one to compare
    // against would be worse than saying nothing.
    Q_PROPERTY(QString receiverFitText READ receiverFitText NOTIFY receiverFitChanged)

    // The same, as a word or two for a chip on the receiver's display, with
    // receiverFitText as its tooltip. See fit_label in models/receiver_match.h.
    Q_PROPERTY(QString receiverFitLabel READ receiverFitLabel NOTIFY receiverFitChanged)

    // Automatic frequency tracking on the pane's receiver. Off by default
    // and not remembered across a restart: a loop that moves the receiver
    // on its own is something the operator turns on while watching it. See
    // models/aft.h for the rules and ReceiverDetail.qml for the toggle.
    Q_PROPERTY(bool aftEnabled READ aftEnabled WRITE setAftEnabled NOTIFY aftChanged)

    // Whether the receiver's mode has a centre AFT can aim at. False on
    // usb, lsb, dsb, raw and the three digital modes, where the toggle is
    // shown disabled.
    Q_PROPERTY(bool aftOffered READ aftOffered NOTIFY aftChanged)

    // What the loop is doing, as a word or two for a chip, and the signal's
    // estimated distance from the receiver's centre when it has one.
    Q_PROPERTY(QString aftState READ aftState NOTIFY aftChanged)
    Q_PROPERTY(double aftErrorHz READ aftErrorHz NOTIFY aftChanged)
    Q_PROPERTY(bool aftHasError READ aftHasError NOTIFY aftChanged)

    // Fitting the pane's receiver's filter to the signal it was tuned onto,
    // once per click-to-tune and once when switched on. Off by default and
    // not remembered across a restart, on the same terms as AFT. See
    // models/auto_filter.h for the rules and models/auto_filter_link.cpp for
    // when it runs.
    Q_PROPERTY(bool autoFilterEnabled READ autoFilterEnabled WRITE setAutoFilterEnabled
                   NOTIFY autoFilterChanged)

    // Whether the receiver's mode has a rule. False on raw only.
    Q_PROPERTY(bool autoFilterOffered READ autoFilterOffered NOTIFY receiverChanged)

    // What the last fit did, or that one is measuring, as a word or two.
    Q_PROPERTY(QString autoFilterState READ autoFilterState NOTIFY autoFilterChanged)

    // A width change is drawn and has not been sent, because sending it
    // mid-gesture would break the audio once per pixel. It goes out on
    // release. The readout says so, because a filter that is drawn where
    // the audio is not has to admit it.
    Q_PROPERTY(bool receiverPending READ receiverPending NOTIFY receiverChanged)

    // ------------------------------------------------------------------
    // The rack: every receiver this window holds. Implemented in
    // ui/models/rack_link.cpp, with the rules in models/receiver_rack.h.
    // ------------------------------------------------------------------
    //
    // ONE PANE, SEVERAL RECEIVERS. Everything above this block is the
    // FOCUSED receiver's, which the detail pane, the passband display, the
    // decode section and the RDS section all read, and it is unchanged by
    // there being others: focusing another receiver parks the pane's one
    // among the held receivers and puts the chosen one in the pane. A held
    // receiver keeps running on the engine with its own audio, and its strip
    // shows its frequency, its mode and its level. It is not retuned while it
    // is held, because nothing in the window is on it to retune it with.
    //
    // NOT KEPT ACROSS A RESTART. Session restore is the owner's decision and
    // has not been taken, so the rack comes up empty and its empty state says
    // so. A reconnect within a session puts every receiver back, the way the
    // pane's one always has been.

    // One map per strip, in rack order: key, slot, colour, label, frequency,
    // mode, level, focused, muted, solo, heard, gain and the band's edges in
    // absolute hertz. The ruler and the rack both draw from it.
    Q_PROPERTY(QVariantList rackEntries READ rackEntries NOTIFY rackChanged)
    Q_PROPERTY(int rackCount READ rackCount NOTIFY rackChanged)
    Q_PROPERTY(bool rackFull READ rackFull NOTIFY rackChanged)

    // The focused receiver's key and colour slot, zero and zero with none.
    Q_PROPERTY(qulonglong focusedKey READ focusedKey NOTIFY rackChanged)
    Q_PROPERTY(int focusedSlot READ focusedSlot NOTIFY rackChanged)

    // The last thing the rack has to say that no strip carries: a receiver
    // the engine let go, an add the engine refused, a double click on a full
    // rack. Empty when there is nothing.
    Q_PROPERTY(QString rackNote READ rackNote NOTIFY rackChanged)

    // ------------------------------------------------------------------
    // Bookmarks: places the operator named
    // ------------------------------------------------------------------
    //
    // WHY THIS IS NOT THE RECEIVER REMEMBERED ACROSS A RESTART, which
    // models/settings.h refuses on the grounds that a window coming up
    // already tuned would be claiming a band it had not looked at. Nothing
    // here is recalled unless somebody picks it, so the claim is theirs.
    // models/bookmarks.h holds the rule and the argument.

    // One label per bookmark, in saved order, ready for a list. A bookmark
    // the operator never named is labelled from its own frequency.
    Q_PROPERTY(QStringList bookmarkLabels READ bookmarkLabels NOTIFY bookmarksChanged)

    // Why the last recall or save did not happen, empty when it did.
    //
    // ITS OWN FAULT STRING AND NOT receiverFault, because the commonest
    // refusal is not about the receiver at all: a bookmark outside the span
    // of a file is a fact about the source, and reported on the receiver's
    // line it would read as the receiver having failed.
    Q_PROPERTY(QString bookmarkFault READ bookmarkFault NOTIFY bookmarkFaultChanged)

    // The pane's receiver is already in the list, within its own passband.
    // What a save affordance binds to so it can offer removing instead of
    // saving a second entry on the same station.
    Q_PROPERTY(bool receiverBookmarked READ receiverBookmarked NOTIFY bookmarksChanged)

    // ------------------------------------------------------------------
    // A receiver the engine let go
    // ------------------------------------------------------------------
    //
    // Empty unless the engine removed the pane's receiver behind this
    // window's back, which since 2026-09-21 it does on purpose: a receiver is
    // pinned to the absolute frequency it was tuned to, and a retune that
    // leaves its centre outside the new span drops it rather than dragging it
    // somewhere nobody chose.
    //
    // NOT receiverFault, AND THE DIFFERENCE IS THE LIFETIME. That line lives
    // in the detail pane and the pane is about to be empty, so the one
    // sentence explaining the disappearance would vanish with the thing it
    // explains. This one outlives it and is cleared when the operator tunes
    // somewhere, not when the pane goes.
    Q_PROPERTY(QString receiverGoneText READ receiverGoneText NOTIFY receiverGoneChanged)

    // The frequency to offer the receiver back at, or zero for no offer.
    // Non-zero only when the engine said a retune removed the pane's receiver
    // for its filter shape, which an add at the same frequency answers; see
    // receiver_can_come_back in models/receiver_gone.h. Same lifetime as the
    // sentence beside it.
    Q_PROPERTY(double receiverComebackHz READ receiverComebackHz NOTIFY receiverGoneChanged)

    // ------------------------------------------------------------------
    // Receivers this window does not hold
    // ------------------------------------------------------------------
    //
    // Empty unless the engine is holding receivers beyond the pane's own, and
    // then it says how many and what releasing them would do.
    //
    // WHY THIS IS NOT A REAPER. A receiver outlives the client that created
    // it, which is what lets two windows each hold their own: measured
    // 2026-09-21 with two revenant-ui processes on one engine, each clicked a
    // signal and neither disturbed the other. Closing a window releases its
    // receiver; a client killed outright does not, and the engine then carries
    // a channelizer slot and its GPU work until it is restarted.
    //
    // From here those two look identical. Nothing on the wire says who created
    // a receiver, so a window that reaped what it did not recognise would take
    // the other operator's audio away mid-listen. So this reports, and the
    // release is something a person asks for having read what it will do.
    Q_PROPERTY(QString strandedReceiverText READ strandedReceiverText
                   NOTIFY strandedReceiversChanged)

    // ------------------------------------------------------------------
    // RDS on the receiver the detail pane is on
    // ------------------------------------------------------------------
    //
    // THE WIRE HAS CARRIED THIS SINCE THE DECODER LANDED AND NOTHING READ
    // IT. On 2026-09-20 a real station answered with PI 0x2AF6, call sign
    // KKFM, PS "KKFM", RadioText "98.1 KKFM Weekends" and PTY 6, and the
    // window showed none of it.
    //
    // A SWITCH AND NOT AN ALWAYS-ON POLL, because core/rpc/client.h is
    // explicit that the FIRST rds_station call is what builds the decoder,
    // the same way the first detections call builds the detector. Polling
    // it on every receiver would build a composite decoder behind every
    // receiver an operator ever tuned.
    //
    // IT RAISES THE RECEIVER'S AUDIO RATE, AFTER ASKING THE ENGINE WHETHER
    // IT CAN. core/rpc/client.h names four conditions a receiver must clear
    // and the binding one is that the audio rate has to carry 57 kHz, which
    // in practice means 171000 exactly: three times the subcarrier and 144
    // times the 1187.5 bit/s bit rate. A receiver created the ordinary way
    // takes the engine's default of 48000, whose 24 kHz of Nyquist has
    // destroyed the composite before the decoder is built, so a switch that
    // only polls is a switch that always refuses.
    //
    // WHAT THIS PARAGRAPH USED TO SAY, and it is why the switch shipped
    // refusing: "It does not raise the receiver's audio rate to reach the
    // subcarrier... On the shipped 64-channel grid the channel rate is a
    // fraction of that, so asking for it would be refused and the pane's
    // receiver would be lost to a failed rebuild." The reasoning assumed a
    // 64-channel grid. revenant-engine picks its channel count from the
    // source rate so that one channel carries a 200 kHz broadcast FM
    // receiver, so a 2.4 MS/s dongle gets M=8 and a channel rate of 600000,
    // which clears 171000 comfortably. Anybody tuning broadcast FM, the only
    // band RDS exists on, is already on a grid that can carry it. The 64
    // channels were never the shipped grid for this source; the refusal they
    // predicted was real and its cause was the 48000 default.
    //
    // AND IT IS STILL NOT ASKED BLIND, because the old paragraph's fear was
    // correct about the consequence. A rate change is a remove and an add,
    // and a refused add leaves the pane with no receiver, so the engine is
    // asked first on a receiver nobody is using: Client::add_vrx answers with
    // an id or refuses and destroys nothing, which makes a throwaway receiver
    // at 171000 a question the operator does not pay for. See
    // models/composite_probe.h for the whole mechanism and the two conditions
    // this window answers without asking.
    //
    // THE RATE IS PUT BACK WHEN THE SWITCH GOES OFF. 171000 is the multiplex,
    // which the window can only play as mono programme audio, so a receiver
    // left there has lost its stereo, and the switch would have a permanent
    // side effect nobody asked for. While the switch is on the pane says so.
    //
    // WHAT THIS PARAGRAPH USED TO SAY: "171000 is the multiplex and not
    // programme audio, so a receiver left there is one nobody can listen to",
    // and that "an operator with the audio on hears the composite". Since
    // 2026-09-23 audio/audio_mix.h filters a wfm multiplex to 15 kHz and
    // de-emphasises it on the way into the mix.

    // The operator asked for RDS on the pane's receiver. Sticky across a
    // retune and a reconnect, on the same terms audioWanted is: it is a
    // switch and not a state.
    Q_PROPERTY(bool rdsWanted READ rdsWanted WRITE setRdsWanted NOTIFY rdsChanged)

    // The pane's receiver has been raised to the composite rate, so it is
    // handing out the 171 kHz multiplex rather than programme audio.
    //
    // ON rdsChanged AND NOT receiverChanged, although it is a fact about the
    // receiver's request. The only thing that moves it is the RDS switch and
    // the probe behind it, and a pane that redrew this on every filter edge
    // would be reading a receiver property to learn an RDS one.
    //
    // It exists so the window can say what the audio has become. An operator
    // listening to a stereo station and turning RDS on hears it go mono, and
    // nothing else on screen would account for that. See
    // models/composite_probe.h for why the rate is the receiver's rather than
    // a second receiver's. WHAT THIS USED TO SAY: that the operator "hears
    // the composite" and "the sound is wrong", which was true until the mix
    // played the multiplex's programme band on 2026-09-23.
    Q_PROPERTY(bool rdsCompositeReceiver READ rdsCompositeReceiver NOTIFY rdsChanged)

    // "rds" or "rbds", which is a SETTING and never an inference. The PI
    // code cannot decide it, because the US call sign range collides with
    // European country codes, and getting it wrong is silent: PTY 26 is
    // National Music in one region and Hip-Hop in the other. Written
    // through Session.setRdsRegion, which REBUILDS the decoder and clears
    // everything accumulated, so this window writes it only on a change.
    Q_PROPERTY(QString rdsRegion READ rdsRegion WRITE setRdsRegion NOTIFY rdsChanged)

    // The sentence about the DECODER, which is always present while the
    // switch is on. Four different situations produce a station struct
    // with nothing in it and they need four different actions; see
    // models/rds_view.h.
    Q_PROPERTY(QString rdsStatus READ rdsStatus NOTIFY rdsChanged)

    // rdsStatus in a word or two, for the chip that carries rdsStatus as its
    // tooltip. The reason when one is known, such as the channel being too
    // narrow, and the decoder's state otherwise. See models/rds_view.h.
    Q_PROPERTY(QString rdsLabel READ rdsLabel NOTIFY rdsChanged)

    // Whether that sentence is bad news. A decoder that never locked on a
    // band with no RDS is the ordinary answer and is not.
    Q_PROPERTY(bool rdsIsFault READ rdsIsFault NOTIFY rdsChanged)

    // Groups are arriving, so the fields below mean something. A pane
    // that drew them without this shows a struct's defaults as a station.
    Q_PROPERTY(bool rdsDecoding READ rdsDecoding NOTIFY rdsChanged)

    // The station. Call sign when the region derives one, the PI in hex
    // when it does not, empty before a PI has arrived.
    Q_PROPERTY(QString rdsIdentity READ rdsIdentity NOTIFY rdsChanged)

    // Programme Service and RadioText, as UTF-8 through QString::
    // fromStdString and never as Latin-1. models/rds_text.h has the
    // mapping and the reason: these are EN 50067 Annex E code points, and
    // the upper half is a different alphabet at the same byte values, so
    // the accident renders a plausible wrong letter and never faults.
    Q_PROPERTY(QString rdsPs READ rdsPs NOTIFY rdsChanged)
    Q_PROPERTY(QString rdsRadioText READ rdsRadioText NOTIFY rdsChanged)

    // How much of each has arrived. Shown when the two differ, because
    // "KKFM" and a half-received name are different claims and the
    // placeholders alone are easy to read past on a narrow strip.
    Q_PROPERTY(int rdsPsSegments READ rdsPsSegments NOTIFY rdsChanged)
    Q_PROPERTY(int rdsPsSegmentsTotal READ rdsPsSegmentsTotal NOTIFY rdsChanged)
    Q_PROPERTY(int rdsRtSegments READ rdsRtSegments NOTIFY rdsChanged)
    Q_PROPERTY(int rdsRtSegmentsTotal READ rdsRtSegmentsTotal NOTIFY rdsChanged)

    // "6 Classic Rock", from the wire rather than a table here, because
    // half the table differs between the two regions.
    Q_PROPERTY(QString rdsProgrammeType READ rdsProgrammeType NOTIFY rdsChanged)

    // Traffic Programme and Traffic Announcement, each with the validity
    // flag core/rpc/types.h insists on: TA false is a real state and so is
    // "no 0A group has arrived", and they are not the same.
    Q_PROPERTY(bool rdsTp READ rdsTp NOTIFY rdsChanged)
    Q_PROPERTY(bool rdsTpValid READ rdsTpValid NOTIFY rdsChanged)
    Q_PROPERTY(bool rdsTa READ rdsTa NOTIFY rdsChanged)
    Q_PROPERTY(bool rdsTaValid READ rdsTaValid NOTIFY rdsChanged)

    // The programme type name, the Emergency Warning System and the Traffic
    // Message Channel, which are marks, counts and raw bits and are drawn as
    // that. models/rds_services.h decides every word and every rule here.
    //
    // rdsPtynRuns is a list of {text, corrected}, cut where a correction's
    // mark starts or stops, and reads as one name when joined.
    Q_PROPERTY(bool rdsPtynShown READ rdsPtynShown NOTIFY rdsChanged)
    Q_PROPERTY(QVariantList rdsPtynRuns READ rdsPtynRuns NOTIFY rdsChanged)
    Q_PROPERTY(QString rdsPtynNote READ rdsPtynNote NOTIFY rdsChanged)
    Q_PROPERTY(QString rdsPtynCorrectedDetail READ rdsPtynCorrectedDetail NOTIFY rdsChanged)

    // ews.groups above zero, and the chip that says so with the payload in
    // hexadecimal as its detail.
    Q_PROPERTY(bool rdsEwsSent READ rdsEwsSent NOTIFY rdsChanged)
    Q_PROPERTY(QString rdsEwsLabel READ rdsEwsLabel NOTIFY rdsChanged)
    Q_PROPERTY(QString rdsEwsDetail READ rdsEwsDetail NOTIFY rdsChanged)

    // Whether a TMC service is on air or announced, its counts, and the
    // confirmed payloads in hexadecimal for the expansion under the row.
    Q_PROPERTY(bool rdsTmcShown READ rdsTmcShown NOTIFY rdsChanged)
    Q_PROPERTY(QString rdsTmcLabel READ rdsTmcLabel NOTIFY rdsChanged)
    Q_PROPERTY(QString rdsTmcCounts READ rdsTmcCounts NOTIFY rdsChanged)
    Q_PROPERTY(QString rdsTmcDetail READ rdsTmcDetail NOTIFY rdsChanged)
    Q_PROPERTY(QStringList rdsTmcPayloads READ rdsTmcPayloads NOTIFY rdsChanged)
    Q_PROPERTY(int rdsTmcNotListed READ rdsTmcNotListed NOTIFY rdsChanged)

    // THE HEALTH, WHICH IS THE HALF THAT SAYS WHETHER TO BELIEVE THE REST.
    // A decoder that is not locked has to look different from a station
    // with no RDS, and an empty pane cannot tell you which.
    //
    // The error rate counts corrected blocks as errors, because a
    // corrected block had a burst repaired rather than arriving clean.
    // Negative before any block has been looked at, which is not a rate of
    // zero.
    Q_PROPERTY(double rdsBlockErrorRate READ rdsBlockErrorRate NOTIFY rdsChanged)
    Q_PROPERTY(double rdsBitRateHz READ rdsBitRateHz NOTIFY rdsChanged)
    Q_PROPERTY(double rdsCarrierOffsetHz READ rdsCarrierOffsetHz NOTIFY rdsChanged)
    Q_PROPERTY(double rdsQuality READ rdsQuality NOTIFY rdsChanged)
    Q_PROPERTY(qulonglong rdsGroups READ rdsGroups NOTIFY rdsChanged)

    // The engine refused the poll, in its own words, which on a receiver
    // that cannot carry a composite names which of the four conditions
    // failed. Kept apart from rdsStatus, which is about a decoder that
    // exists.
    Q_PROPERTY(QString rdsFault READ rdsFault NOTIFY rdsChanged)

    // ------------------------------------------------------------------
    // Listening to the receiver the detail pane is on
    // ------------------------------------------------------------------
    //
    // EVERY HEARD RECEIVER, MIXED AT THE CLIENT. The engine serves an audio
    // subscription per receiver and this link takes one on every receiver
    // in the rack that is heard: not muted, and soloed when anything is.
    // Each lands in its own ring, one per rack slot, and the player sums
    // them with each strip's gain. The properties in this block describe the
    // FOCUSED receiver's subscription, which is the one the audio section
    // sits under; the strips carry the rest.
    //
    // WHAT THIS PARAGRAPH USED TO SAY. It was headed "ONE RECEIVER AT A
    // TIME, AND IT IS THE PANE'S", said "this link takes exactly one, on
    // whichever receiver the detail pane holds", and ended "Two streams
    // mixed into one pair of speakers is a mixer with gains and a pan per
    // source, and nothing here decides that on the operator's behalf." The
    // rack is that mixer, with a gain per strip and no pan; the operator
    // decides it with mute and solo.
    //
    // apply_audio_request reconciles the subscriptions against the rack on
    // every supervisor pass rather than each call site remembering to, which
    // is what makes a retune, a mode change, a clear, a focus change, a mute,
    // a reconnect and an ended arrival all one code path.

    // What the operator asked for, which is a switch and not a state: it
    // stays on across a receiver change, a reconnect and a rebuild, and
    // audioActive below is whether there is actually a stream.
    Q_PROPERTY(bool audioWanted READ audioWanted WRITE setAudioWanted NOTIFY audioChanged)

    // Whether the pane's receiver makes audio at all, mode_makes_audio in
    // models/mode_choice.h. False on raw and the three digital modes, whose
    // output is complex baseband for a decoder: the window hides the audio
    // section there, and apply_audio_request does not subscribe on one
    // while the switch is on, because the engine would only refuse it.
    Q_PROPERTY(bool audioOffered READ audioOffered NOTIFY receiverChanged)

    // A subscription exists on the engine right now. False while the
    // switch is on and the pane has no receiver, which is the ordinary
    // state before anything is tuned.
    Q_PROPERTY(bool audioActive READ audioActive NOTIFY audioChanged)

    // Which receiver is being listened to, so the display can say it
    // rather than leaving the operator to assume it is the pane's. Zero
    // when nothing is subscribed.
    Q_PROPERTY(qulonglong audioReceiverId READ audioReceiverId NOTIFY audioChanged)

    // What the engine GRANTED, in milliseconds, which is the number every
    // buffer on this side is derived from. The engine clamps a request to
    // 20..5000 and reports the result, on the EngineInfo::ringClamped
    // precedent: a depth that was quietly changed is a dropout nobody can
    // trace. Zero when nothing is subscribed.
    //
    // It is not the whole clamp. A two-chunk floor is applied in FRAMES
    // when the first chunk arrives, because the server cannot convert
    // milliseconds to frames before it knows the receiver's audio rate.
    // audioBufferFrames below is the depth actually being enforced.
    Q_PROPERTY(uint audioGrantedMillis READ audioGrantedMillis NOTIFY audioChanged)

    // The engine refused a subscription, in its own words. Kept apart from
    // errorText and from receiverFault for the reason those two are kept
    // apart: a raw tap that cannot carry audio is not a lost engine and
    // must not read as one.
    Q_PROPERTY(QString audioFault READ audioFault NOTIFY audioChanged)

    // The receiver went away and the engine said so. This is the one
    // failure in the whole client that has no visible symptom of its own:
    // a spectrum subscription that dies freezes a picture and a frozen
    // picture is obvious from across the room, and this produces silence,
    // which is what a quiet channel with the squelch shut sounds like.
    // core/rpc/client.h's ended callback exists for exactly that and this
    // is where its words are shown.
    //
    // Never set for an unsubscribe this client asked for. Cleared when a
    // new subscription starts.
    Q_PROPERTY(QString audioEndedReason READ audioEndedReason NOTIFY audioChanged)

    // The engine's own counters for this subscription, polled once a
    // second. Separate from the player's counters, which describe the
    // sound card: a wire drop and a starved card sound the same and have
    // different fixes.
    Q_PROPERTY(qulonglong audioFramesDropped READ audioFramesDropped NOTIFY audioChanged)
    Q_PROPERTY(qulonglong audioDropEvents READ audioDropEvents NOTIFY audioChanged)
    Q_PROPERTY(qulonglong audioBacklogFrames READ audioBacklogFrames NOTIFY audioChanged)
    Q_PROPERTY(qulonglong audioBufferFrames READ audioBufferFrames NOTIFY audioChanged)

    // ------------------------------------------------------------------
    // Decoding on the receiver the detail pane is on
    // ------------------------------------------------------------------
    //
    // Session.subscribeDecoded attaches one of the engine's event decoders to
    // a receiver and streams what it recovers. This is the pane's use of it,
    // implemented in ui/models/decoded_link.cpp; which decoders a receiver is
    // offered and what a line says are models/decoded_log.h's.
    //
    // RECONCILED, NOT COMMANDED, the way the audio is and for its reason:
    // apply_decode_request compares what the switch and the pane's receiver
    // ask for against what this client holds on every supervisor pass, so a
    // retune, a mode change, a clear and a reconnect are one code path.

    // The operator asked for decoding on the pane's receiver. A switch and not
    // a state: it stays on across a receiver change and a reconnect, and not
    // across a restart, on AFT's terms.
    Q_PROPERTY(bool decodeWanted READ decodeWanted WRITE setDecodeWanted NOTIFY decodeChanged)

    // What the menu shows, which is the operator's pick when this receiver
    // offers it and the head of the menu when it does not. "auto" or a
    // decoder's registry name.
    Q_PROPERTY(QString decodeChoice READ decodeChoice WRITE setDecodeChoice NOTIFY decodeChanged)

    // The menu for the pane's receiver: auto first when it would attach
    // anything, then every decoder the engine says reads the receiver's mode.
    // Empty when there is no receiver or nothing reads its mode, and the
    // section is hidden then.
    Q_PROPERTY(QStringList decodeChoices READ decodeChoices NOTIFY decodeChanged)

    // The decoders subscribed right now, comma separated, which is what auto
    // turned into. Empty when nothing is.
    Q_PROPERTY(QString decodeAttached READ decodeAttached NOTIFY decodeChanged)

    // Why something the switch asked for is not running, as a word or two
    // and as the engine's sentence: a refused subscription, or a stream the
    // engine ended. Both empty when nothing is wrong.
    Q_PROPERTY(QString decodeLabel READ decodeLabel NOTIFY decodeChanged)
    Q_PROPERTY(QString decodeDetail READ decodeDetail NOTIFY decodeChanged)

    // The log. Survives a change of receiver and a reconnect, because what
    // was decoded is still what was decoded; the operator clears it.
    Q_PROPERTY(revenant::ui::DecodedLogModel* decodedLog READ decodedLog CONSTANT)

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

    // ------------------------------------------------------------------
    // Tuning the front end, and whether it is keeping up. Both are
    // implemented in ui/models/source_link.cpp.
    // ------------------------------------------------------------------

    [[nodiscard]] double realtimeFactor() const { return pacing_.realtime_factor; }
    [[nodiscard]] double sourcePacedBy() const { return pacing_.paced_by; }
    [[nodiscard]] bool pacingCarried() const { return pacing_.carried; }
    [[nodiscard]] QString pacingText() const { return pacing_text_; }
    [[nodiscard]] bool sourceBehind() const { return pacing_is_fault(pacing_verdict_); }

    [[nodiscard]] QString frontEndText() const { return front_end_text_; }
    [[nodiscard]] bool frontEndFault() const { return front_end_is_fault(front_end_); }

    [[nodiscard]] double sourceCenterHz() const {
        return static_cast<double>(info_.source_center);
    }
    [[nodiscard]] bool sourceCanRetune() const { return source_can_retune_; }
    [[nodiscard]] double sourceTuneLowHz() const {
        return static_cast<double>(source_tune_low_);
    }
    [[nodiscard]] double sourceTuneHighHz() const {
        return static_cast<double>(source_tune_high_);
    }
    [[nodiscard]] QString sourceRetuneUnavailable() const {
        return source_retune_unavailable_;
    }
    [[nodiscard]] QString tuneFault() const { return tune_fault_; }
    [[nodiscard]] double tuneRequestedHz() const {
        return static_cast<double>(tune_requested_hz_);
    }
    [[nodiscard]] double tuneGrantedHz() const {
        return static_cast<double>(tune_granted_hz_);
    }
    [[nodiscard]] bool tuneAnswered() const { return tune_answered_; }

    // What a typed string resolves to, before anything is sent, as
    // megahertz to six places. Empty when the text is not a frequency.
    //
    // THE ECHO IS THE WHOLE POINT. models/frequency_entry.h has to guess
    // the unit of a bare number, and the guess is stated here rather than
    // discovered when the radio lands somewhere unexpected. A box binds a
    // label to this and the operator reads the reading before pressing
    // return.
    [[nodiscard]] Q_INVOKABLE QString previewTune(const QString& text) const;

    // Whether the typed string parses at all, so a box can refuse to
    // commit rather than sending something nobody meant.
    [[nodiscard]] Q_INVOKABLE bool tuneTextValid(const QString& text) const;

    // The same parse, as a number, for the boxes in the device picker.
    //
    // HERE AND NOT IN JAVASCRIPT, because the rule is not obvious and is
    // already written down once: ui/models/frequency_entry.h decides that a
    // bare number under a million is megahertz and at or above it is hertz,
    // accumulates digits into a 64-bit mantissa rather than going through a
    // double, and refuses what does not parse. A parseFloat in QML would be a
    // second reading of "98.1" that differs from the tune box's by a factor of
    // a million.
    //
    // ZERO FOR TEXT THAT DOES NOT PARSE, including empty, which is what an
    // untouched box holds. compose_source_uri reads a zero rate as "leave the
    // key off and take the backend's default", so an empty box asks for the
    // default rather than for nothing.
    [[nodiscard]] Q_INVOKABLE double parseHz(const QString& text) const;

    // The same parse for a SAMPLE RATE box, where a bare number is hertz.
    //
    // A SEPARATE CALL AND NOT A FLAG, so a QML author cannot reach for the wrong
    // one by omission. The two differ in exactly one rule and the difference is
    // silent when it bites: the picker's rate box used parseHz until
    // 2026-09-21, and an operator typing 250000 meaning 250 kS/s got 250000 MHz,
    // which settle_rate then clamped to the device's maximum and opened at
    // 3.2 MS/s. See ui/models/frequency_entry.h's BareNumber.
    [[nodiscard]] Q_INVOKABLE double parseRateHz(const QString& text) const;

    // Retune the front end to what the text says. Returns false and writes
    // tuneFault when the text is not a frequency; everything else is the
    // engine's answer and arrives asynchronously, like every other write
    // on this object.
    Q_INVOKABLE bool tuneSource(const QString& text);

    // The same, from a number a band button holds.
    Q_INVOKABLE void tuneSourceHz(double hertz);

    // One wheel event over either span display, accumulated and spent at most
    // once per settling interval.
    //
    // ON THIS OBJECT AND NOT ON THE ITEM, which is the whole point of it being
    // here. models/scroll_tune.h exists because a device retune costs about
    // 330 ms with no samples at all, so "at most one tune goes out per
    // settling interval" is the rule the coalescing is for. Two accumulators
    // cannot enforce it: they are two displays and one radio, and a pointer
    // crossing from the spectrum to the waterfall mid-sweep hands each of them
    // a part of one gesture, each below its own interval and each firing.
    //
    // The items had one apiece until 2026-09-22, and waterfall_item.h said the
    // reason was discussed on ScrollTuneState. It is not, and what that header
    // does argue is the opposite: the settling interval is a property of the
    // radio, so the accumulator belongs with the thing that owns the radio.
    //
    // The item keeps the gesture; this keeps the backlog. Nothing here reads a
    // pointer position, and the flush timer is this object's, so a display
    // being destroyed mid-sweep does not take the pending notches with it.
    //
    // Invokable because the ruler between the two displays is QML, and the
    // wheel over it is the same gesture into the same backlog.
    Q_INVOKABLE void takeScrollTune(double angle_delta_eighths);

    // ---- the device picker -------------------------------------------------
    //
    // WHY THIS IS A LIST OF MAPS AND NOT A QAbstractListModel. There are two
    // or three entries, they are replaced wholesale on every refresh rather
    // than edited, and nothing scrolls. A model exists to make an incremental
    // change cheap on a long list; neither half of that is true here, and it
    // would be a class with four overrides to answer questions nobody asks.
    //
    // Each map carries the descriptor's own fields under the names QML reads,
    // plus the two the panel needs and the wire does not send: the composed
    // tuning envelope, and a length written as a duration.
    [[nodiscard]] QVariantList sources() const { return sources_; }

    // Asks the engine what exists. ASYNCHRONOUS LIKE EVERY OTHER WRITE HERE,
    // and slower than the rest of them: listSources opens every device index
    // to ask, including ones with nothing behind them, so it pays a libusb
    // timeout per absent dongle. sourcesBusy is true while it runs, and the
    // panel says so rather than looking hung.
    Q_INVOKABLE void refreshSources();
    [[nodiscard]] bool sourcesBusy() const { return sources_busy_; }

    // Whether the engine has a source open at all, which is a state the window
    // could not be in before closeSource existed.
    //
    // Read off EngineInfo::source_rate rather than kept as a flag: a flag here
    // would be a second copy of a fact the engine already publishes, and the
    // two would disagree for a poll interval after every open.
    [[nodiscard]] bool sourceOpen() const { return info_.source_rate > 0; }

    // Which stream the indices this window is holding belong to. Zero before
    // any source has been opened on this engine.
    //
    // PUBLISHED BECAUSE THE WATERFALL HAS TO SEE IT. Closing a source and
    // opening another starts a new stream numbered from zero, so a history
    // drawn against the old one is a picture of a different radio. See
    // EngineInfo::source_epoch.
    [[nodiscard]] qulonglong sourceEpoch() const {
        return static_cast<qulonglong>(info_.source_epoch);
    }

    // The gain stage, all off the one descriptor. Qt thread only, like every
    // getter here; gain_stage_ is replaced whole on the pass that reads the
    // descriptor.
    [[nodiscard]] QString sourceGainStage() const {
        return QString::fromStdString(gain_stage_.name);
    }
    [[nodiscard]] int sourceGainStages() const { return gain_stage_count_; }
    [[nodiscard]] bool sourceGainHasAuto() const { return gain_stage_.has_auto; }
    [[nodiscard]] double sourceGainFraction() const {
        return fraction_for_gain(gain_stage_, gain_db_);
    }
    [[nodiscard]] double sourceGainStep() const { return gain_fraction_step(gain_stage_); }
    [[nodiscard]] double sourceGainDb() const { return gain_db_; }
    [[nodiscard]] bool sourceGainKnown() const { return gain_known_; }
    [[nodiscard]] bool sourceGainAuto() const { return gain_auto_; }
    [[nodiscard]] QString sourceGainFault() const { return gain_fault_; }

    // Asks the device for the gain a slider at this fraction means.
    //
    // A FRACTION AND NOT DECIBELS, so the caller cannot skip the settle: the
    // request goes out on a step the stage actually has, which is what makes
    // the answer comparable to what was asked. models/gain_control.h has the
    // mapping and why it is not decibels all the way through.
    //
    // Posted to the supervisor like every other write here, because the call
    // blocks for a round trip and on an RTL-SDR it also stops the transfers
    // for about a third of a second. A slider dragged on the Qt thread would
    // freeze the window for that long per notch, which is the freeze an
    // operator hit when the gain calls went straight at a streaming dongle.
    Q_INVOKABLE void setSourceGainFraction(double fraction);

    // Hands the stage to the device's own AGC, or takes it back.
    //
    // Offered only where sourceGainHasAuto says the device will do it, and it
    // is a choice rather than the sensible setting: README.md has the
    // measurement of what the RTL-SDR's own AGC did to the detector's track
    // list, and whether it is right depends on the antenna.
    Q_INVOKABLE void setSourceGainAuto(bool on);

    // Opens what the picker composed, closing whatever is open first.
    //
    // TWO CALLS ON THE WIRE AND ONE HERE, deliberately. openSource is refused
    // when a source is already open, because a replace that failed on the new
    // URI would have destroyed the working one; that argument is about the
    // ENGINE, which cannot know whether a caller meant to replace. This one
    // can: an operator who picked a device in the panel meant to change to it.
    // So the close and the open are sequenced here, and a failed open leaves
    // the window with no source and the engine's sentence in sourceFault,
    // which is a state the panel shows and can open out of.
    Q_INVOKABLE void openSource(const QString& uri);
    Q_INVOKABLE void closeSource();

    // What the last open or close said when it refused, and empty otherwise.
    // Kept apart from errorText for the reason detectionFault and tuneFault
    // are: that field belongs to the connection, and a refused open on a
    // healthy connection is not a connection problem.
    [[nodiscard]] QString sourceFault() const { return source_fault_; }

    // The URI for a device in the list, with an operator's settings on it.
    //
    // ui/models/source_choice.h does the work and is where the argument for
    // every rule lives: which keys a backend accepts, where a rate lands when
    // the device only takes some of them, and which gain step a request rounds
    // to. This is the QML-facing wrapper and holds no policy of its own.
    //
    // An index outside the list answers empty rather than throwing, because
    // QML will call this during a rebind when the list has just been replaced.
    // TEXT AND NOT NUMBERS, because an empty box has to stay empty all the way
    // down. Passing doubles meant an untouched box arrived as zero and
    // compose_source_uri could not tell that from a request for zero: the centre
    // was clamped into the tune envelope and opened an R820T at its 24 MHz low
    // edge, and the gain snapped to the lowest step in the tuner's table. The
    // parse happens here so the emptiness is preserved and so the rate keeps its
    // own bare-number rule.
    [[nodiscard]] Q_INVOKABLE QString composeSourceUri(int index, const QString& center,
                                                       const QString& rate,
                                                       const QString& gain_db,
                                                       bool gain_auto) const;

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
    [[nodiscard]] double marginBar() const { return margin_bar_; }
    [[nodiscard]] double maxConfidenceBar() const { return kMaxConfidenceBar; }
    void setConfidenceBar(double bar);
    void setMarginBar(double bar);
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

    // How far the detection with this id stood above the detection threshold,
    // zero to one, or a negative number when no such detection is in the list
    // the window currently holds.
    //
    // BY ID RATHER THAN THROUGH THE CLICK SIGNAL, because a click carries what
    // the item knew at the moment of the click and this is read whenever the
    // row repaints. A track that has faded since is gone from the list and
    // says so by being absent, which is the honest answer for a row asking how
    // strong something is RIGHT NOW.
    //
    // NEGATIVE AND NOT ZERO FOR ABSENT. Zero is a real value of this measure,
    // being what a detection exactly at the threshold would read if one could
    // be published, so a caller has to be able to tell "nothing here" from
    // "nothing above the bar".
    [[nodiscard]] Q_INVOKABLE double detectionMargin(qulonglong id) const {
        for (const rpc::Detection& detection : shown_.detections) {
            if (detection.id == id) {
                return detection.margin_confidence;
            }
        }
        return -1.0;
    }

    // The band's excess in its strongest three adjacent bins over its excess
    // in total, or a negative number when there is nothing to report.
    //
    // THE NUMBER THAT SEPARATES A BAND THAT IS NEARLY ALL SIGNAL FROM ONE
    // THAT IS NEARLY ALL FLOOR, which neither of the other two does.
    // detectionMargin says how far it stood above the threshold and the bar
    // above says how long it has been there; a raised patch of noise floor
    // can score well on both. Measured on 20 m on 2026-09-22, an 11.7 kHz
    // patch sitting at confidence 1.00 read 0.02 here against 0.59 to 0.86
    // for the carriers beside it.
    //
    // THAT IS THE WHOLE CLAIM AND IT IS NOT AN INTERFERENCE TEST. Against the
    // product scene, where a station and its own third-order products are in
    // the same frames with truth known by construction, the stations read
    // 0.079 and the products 0.033 while the artefacts elsewhere read 0.155:
    // no bar separates them. core/detect/shape.h has the survey and the
    // reason. A strong interferer can be perfectly concentrated.
    //
    // NEGATIVE FOR ABSENT AND ALSO FOR UNMEASURED, which is the difference
    // from detectionMargin and is deliberate. The wire carries shapeMeasured
    // beside the value precisely because an unmeasured band arrives as 0.0
    // and 0.0 is the most noise-like reading there is. Folding both into one
    // negative means a caller cannot draw "we could not tell" as "certainly
    // junk" by forgetting to check, which is the only way this field can do
    // harm.
    //
    // IT HAS TO BE READ WITH THE BANDWIDTH. Under about five bins it says
    // nothing, because the analysis window spreads one line over that many.
    // Above that it separates a line spectrum from a filled one: measured
    // across one emitter of each family, fsk2 reads 0.519 against bpsk at
    // 0.117 and qpsk at 0.121, while cw, am, nfm, usb and lsb are all too
    // narrow to resolve and read 0.946 alike. A high reading on a wide band
    // is a carrier with sidebands.
    // The track's confidence, the stopwatch the detections bar filters on, or a
    // negative number when the list does not hold it. For the hover card,
    // which names it for what it counts; see the long note on confidenceBar.
    [[nodiscard]] Q_INVOKABLE double detectionConfidence(qulonglong id) const {
        for (const rpc::Detection& detection : shown_.detections) {
            if (detection.id == id) {
                return detection.confidence;
            }
        }
        return -1.0;
    }

    [[nodiscard]] Q_INVOKABLE double detectionConcentration(qulonglong id) const {
        for (const rpc::Detection& detection : shown_.detections) {
            if (detection.id == id) {
                return detection.shape_measured ? detection.concentration : -1.0;
            }
        }
        return -1.0;
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
    [[nodiscard]] QString receiverFitText() const { return receiver_fit_text_; }
    [[nodiscard]] QString receiverFitLabel() const { return receiver_fit_label_; }
    [[nodiscard]] bool aftEnabled() const { return aft_.enabled(); }
    void setAftEnabled(bool on);
    [[nodiscard]] bool aftOffered() const;
    [[nodiscard]] QString aftState() const;
    [[nodiscard]] double aftErrorHz() const { return aft_step_.error_hz; }
    [[nodiscard]] bool aftHasError() const { return aft_step_.have_error; }
    [[nodiscard]] bool autoFilterEnabled() const { return auto_filter_enabled_; }
    void setAutoFilterEnabled(bool on);
    [[nodiscard]] bool autoFilterOffered() const;
    [[nodiscard]] QString autoFilterState() const;

    // Puts the detail pane on a receiver at this absolute frequency in this
    // mode, adding one if there is none and retuning the one there is.
    //
    // The passband is left unstated, so the engine answers with the mode's
    // own default and the pane reads it back off the placement. That is the
    // whole reason this client carries no table of its own: the defaults
    // live in dsp::default_passband, this process links no part of the DSP,
    // and a copy here would be a second table to keep in step.
    //
    // mode is a demodulator name as core/engine/vrx.h spells it, one of
    // kDemodNames in models/mode_choice.h. An empty string keeps the mode
    // the pane already has.
    Q_INVOKABLE void tuneReceiver(double absolute_hz, const QString& mode);

    // The same, from a click on a detection, carrying the bandwidth the
    // detector measured.
    //
    // A SEPARATE ENTRY POINT AND NOT A DEFAULTED ARGUMENT, because the
    // absence of a measurement is a fact worth stating rather than a zero
    // that fell through. tuneReceiver CLEARS the remembered bandwidth: a
    // receiver placed by hand has no signal measurement behind it, and one
    // left over from the previous click would be compared against a band
    // it was never measured in.
    //
    // The passband is still left unstated, so the engine answers with the
    // mode's own default. The measured width is NOT handed over as a
    // passband, for the reason ui/qml/TuneSelection.qml gives at the call site: on
    // USB the occupied band is entirely above a suppressed carrier, so
    // using it as a width parks the filter in the wrong place.
    //
    // AN EMPTY MODE MEANS SOMETHING DIFFERENT HERE than it does on
    // tuneReceiver above. There it keeps the mode the pane has, because
    // nothing else is known. Here it means "pick one from the measurement",
    // through ui::demod_for_detection in models/receiver_match.h: a click
    // on a detection is the one place this client holds a measurement of
    // the signal, and letting the previous receiver's mode ride through it
    // is what put a 16 kHz NFM filter on a 145 kHz broadcast station. A
    // named mode still wins.
    //
    // AND SO DOES A MODE THE OPERATOR ALREADY NAMED ON THIS RECEIVER, which
    // is the qualification demod_touched_ carries. Picking a mode by hand and
    // then clicking the signal again to move the receiver is a retune, not a
    // request for the detector's opinion, and deriving on every click is how
    // broadcast FM took the mode back to wfm after every click. The
    // measurement chooses for a receiver whose mode nobody has stated, which
    // is the case the paragraph above is about.
    Q_INVOKABLE void tuneReceiverToDetection(double absolute_hz, const QString& mode,
                                             double detection_bandwidth_hz);

    // Changes the mode in place as far as the operator is concerned, which
    // is a remove and an add underneath: the demodulator IS the stage and
    // core/engine/graph.cpp refuses to change it on a running receiver.
    //
    // The mode's default passband comes with it UNLESS the operator has
    // moved an edge on this receiver, in which case the edges they set are
    // kept. Moving to a mode whose default is one-sided from a mode whose
    // default is not would otherwise throw away a filter somebody had just
    // placed by hand.
    //
    // THE OPERATOR'S OWN CHOICE OF MODE ARRIVES HERE, AND TODAY NOWHERE
    // ELSE: the eight mode choices in ui/qml/ReceiverDetail.qml are this method's only
    // caller, and the two tune entry points are passed an empty mode by the
    // only call site either of them has. So this is what sets
    // demod_touched_, and it sets it even when the mode asked for is the one
    // already running, because that is still a statement of which mode is
    // wanted. From here on a click on a detection keeps this mode rather than
    // deriving one from the signal's width; see tuneReceiverToDetection.
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

    // The wheel over the fine-tuning display: moves the receiver's centre by
    // a round step sized to the display, one move per short interval however
    // fast the wheel turns. See models/receiver_scroll.h.
    Q_INVOKABLE void takeReceiverScroll(double angle_delta_eighths);

    // Moves the receiver's centre and nothing else: the mode, the edges and
    // the measurement the receiver was tuned from all stay. What the wheel
    // and AFT use, as opposed to tuneReceiver, which is a new frequency typed
    // or clicked and forgets the measurement.
    //
    // THE PASSBAND FRAME IN HAND IS KEPT, which tuneReceiver does not do. Its
    // geometry is absolute, bin zero carried as the frequency the fine stage
    // mixed to DC, so drawn against the moved centre it shows the signal
    // where it now is relative to the receiver until the next frame arrives.
    // Dropping it here would blank the display on every notch of the wheel.
    Q_INVOKABLE void moveReceiverCentre(double absolute_hz);

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

    // ------------------------------------------------------------------
    // The rack surface. Implemented in ui/models/rack_link.cpp.
    // ------------------------------------------------------------------
    //
    // QT THREAD ONLY, and every write ends in a rack operation posted to the
    // supervisor in order with the pane's own requests; see RackOp below for
    // why the order is the whole of the design.

    [[nodiscard]] QVariantList rackEntries() const;
    [[nodiscard]] int rackCount() const { return static_cast<int>(rack_.size()); }
    [[nodiscard]] bool rackFull() const { return rack_.full(); }
    [[nodiscard]] qulonglong focusedKey() const { return pane_key_; }
    [[nodiscard]] int focusedSlot() const;
    [[nodiscard]] QString rackNote() const { return rack_note_; }

    // Every receiver's band for the span displays, focused one last.
    [[nodiscard]] std::vector<RackMarker> rackMarkers() const;

    // Puts a held receiver in the pane and the pane's one among the held.
    Q_INVOKABLE void focusReceiver(qulonglong key);

    // The receiver step places down the rack from the focused one, wrapping.
    Q_INVOKABLE void focusNextReceiver(int step);

    // A new receiver at this frequency, in the pane, focused. An empty mode
    // takes the focused receiver's, which is what an operator adding a second
    // receiver on the same band wants. Refused in words when the rack is full.
    Q_INVOKABLE void addReceiver(double absolute_hz, const QString& mode);

    // A single click on the span at pointer_hz, which resolved to center_hz
    // and a measured bandwidth when it was on a detection. Inside another
    // receiver's band it focuses that receiver; anywhere else it tunes the
    // focused one, or opens the first. Answers whether it tuned, which is
    // whether the click readout has a new click to show.
    Q_INVOKABLE bool spanClick(double pointer_hz, double center_hz, double bandwidth_hz);

    // The second click of a double click at the same place. Adds a receiver
    // there and puts the focused one back where the first click found it.
    // See classify_span_click in models/receiver_rack.h. Answers whether it
    // added one.
    Q_INVOKABLE bool spanDoubleClick(double pointer_hz, double center_hz, double bandwidth_hz);

    Q_INVOKABLE void setReceiverMuted(qulonglong key, bool muted);
    Q_INVOKABLE void toggleReceiverSolo(qulonglong key);
    Q_INVOKABLE void setReceiverGain(qulonglong key, double position);

    // Removes any receiver in the rack, the focused one through
    // removeReceiver, which then focuses the one that took its place.
    Q_INVOKABLE void removeRackReceiver(qulonglong key);

    // Another receiver for main()'s --receiver after the first, placed as a
    // held receiver once the first connection with a source can reach it.
    void addStartupReceiver(double absolute_hz, const QString& mode);

    // ------------------------------------------------------------------
    // The mix, for AudioPlayer. Any thread; see the members.
    // ------------------------------------------------------------------

    // One ring per rack slot. The ring for a slot outlives every
    // subscription that writes into it, for the reason audioRing gave: the
    // callback dies with the Client, which the supervisor owns.
    [[nodiscard]] AudioRing& audioRingAt(std::size_t slot) { return audio_rings_[slot]; }

    // The slot whose stream sets the sink's format and the player's status,
    // or -1 when nothing is subscribed. The focused receiver's when it is
    // heard, and otherwise the first heard one down the rack.
    [[nodiscard]] int mixLeadSlot() const { return mix_lead_slot_.load(std::memory_order_acquire); }

    // Which slots hold a live subscription, as bits.
    [[nodiscard]] std::uint32_t mixMask() const { return mix_mask_.load(std::memory_order_acquire); }

    // The strip gain for a slot, as an amplitude.
    [[nodiscard]] float mixGain(std::size_t slot) const {
        return mix_gain_[slot].load(std::memory_order_relaxed);
    }

    // Which slots hold a receiver whose mode hands out audio at the level
    // its signal came in at, as bits: mode_needs_level in
    // models/mode_choice.h. AudioMix levels those.
    [[nodiscard]] std::uint32_t mixLevelMask() const {
        return mix_level_mask_.load(std::memory_order_acquire);
    }

    // Which slots hold a wfm receiver, as bits. AudioMix plays one at the
    // multiplex rate as programme audio.
    [[nodiscard]] std::uint32_t mixWfmMask() const {
        return mix_wfm_mask_.load(std::memory_order_acquire);
    }

    // The lead subscription's grant, which sizes the sink.
    [[nodiscard]] std::uint32_t mixGrantedMillis() const {
        return mix_granted_millis_.load(std::memory_order_acquire);
    }

    // ------------------------------------------------------------------
    // The bookmark surface. Implemented in ui/models/bookmark_link.cpp.
    // ------------------------------------------------------------------
    //
    // ALL OF IT IS QT THREAD ONLY AND NONE OF IT MAKES AN RPC CALL. A save
    // is a registry write and a recall is the same two writes the frequency
    // box and the mode buttons already make, so the supervisor learns about
    // it the way it learns about those.

    [[nodiscard]] QString receiverGoneText() const { return receiver_gone_text_; }
    [[nodiscard]] double receiverComebackHz() const
    {
        return static_cast<double>(receiver_comeback_hz_);
    }

    // Puts a receiver back at receiverComebackHz in the pane's mode, which is
    // tuneReceiver with the mode left alone. Does nothing when there is no
    // offer.
    Q_INVOKABLE void addGoneReceiverBack();

    [[nodiscard]] QString strandedReceiverText() const { return stranded_text_; }

    // Removes every receiver the engine holds except the one this window is
    // on, if any. Asked for explicitly, never on a timer and never at startup.
    Q_INVOKABLE void releaseStrandedReceivers();

    [[nodiscard]] QStringList bookmarkLabels() const;
    [[nodiscard]] QString bookmarkFault() const { return bookmark_fault_; }
    [[nodiscard]] bool receiverBookmarked() const;

    // Saves the pane's current receiver under this name, which may be empty.
    //
    // THE RECEIVER AND NOT THE LAST DETECTION, because the receiver is what
    // the operator is listening to and has possibly nudged since the click.
    // Refuses when there is no receiver, because there is then nothing to
    // describe, and saving the span centre instead would file a bookmark on
    // a frequency nobody chose.
    Q_INVOKABLE void saveBookmark(const QString& name);

    // Forgets one, by its index in bookmarkLabels.
    Q_INVOKABLE void removeBookmark(int index);

    // Renames one in place. An empty name puts it back to being labelled
    // from its own frequency.
    Q_INVOKABLE void renameBookmark(int index, const QString& name);

    // Puts the pane's receiver on this bookmark, retuning the front end
    // first if the frequency is outside the span and the source can move.
    //
    // A RETUNE MEANS THIS RETURNS BEFORE THE RECEIVER EXISTS. tuneSourceHz
    // posts to the supervisor and answers a round trip later, so the
    // bookmark is held and placed when the granted centre arrives. Until
    // then bookmarkFault says what is being waited for, because a click
    // that appears to do nothing for a round trip is indistinguishable from
    // one that was dropped.
    Q_INVOKABLE void recallBookmark(int index);

    // ------------------------------------------------------------------
    // The RDS surface. Implemented in ui/models/rds_link.cpp.
    // ------------------------------------------------------------------

    [[nodiscard]] bool rdsWanted() const { return rds_wanted_.load(); }
    void setRdsWanted(bool wanted);

    // Read off the pane's own request rather than off a status, so it is true
    // from the moment the rebuild is posted rather than a round trip later.
    // What it claims is that this window has asked for the composite rate on
    // this receiver, which is the thing the sentence it drives is about.
    [[nodiscard]] bool rdsCompositeReceiver() const { return carries_composite(wanted_); }

    [[nodiscard]] QString rdsRegion() const;
    void setRdsRegion(const QString& region);

    [[nodiscard]] QString rdsStatus() const { return rds_status_; }
    [[nodiscard]] QString rdsLabel() const { return rds_label_; }
    [[nodiscard]] bool rdsIsFault() const { return rds_is_fault_; }
    [[nodiscard]] bool rdsDecoding() const { return rds_decoding_; }
    [[nodiscard]] QString rdsIdentity() const { return rds_identity_; }
    [[nodiscard]] QString rdsPs() const { return rds_ps_; }
    [[nodiscard]] QString rdsRadioText() const { return rds_radio_text_; }
    [[nodiscard]] int rdsPsSegments() const { return rds_ps_segments_; }
    [[nodiscard]] int rdsPsSegmentsTotal() const { return rds_ps_segments_total_; }
    [[nodiscard]] int rdsRtSegments() const { return rds_rt_segments_; }
    [[nodiscard]] int rdsRtSegmentsTotal() const { return rds_rt_segments_total_; }
    [[nodiscard]] QString rdsProgrammeType() const { return rds_programme_type_; }
    [[nodiscard]] bool rdsTp() const { return rds_station_.tp; }
    [[nodiscard]] bool rdsTpValid() const { return rds_station_.tp_valid; }
    [[nodiscard]] bool rdsTa() const { return rds_station_.ta; }
    [[nodiscard]] bool rdsTaValid() const { return rds_station_.ta_valid; }
    [[nodiscard]] bool rdsPtynShown() const { return rds_ptyn_shown_; }
    [[nodiscard]] QVariantList rdsPtynRuns() const { return rds_ptyn_runs_; }
    [[nodiscard]] QString rdsPtynNote() const { return rds_ptyn_note_; }
    [[nodiscard]] QString rdsPtynCorrectedDetail() const { return rds_ptyn_corrected_detail_; }
    [[nodiscard]] bool rdsEwsSent() const { return rds_ews_sent_; }
    [[nodiscard]] QString rdsEwsLabel() const { return rds_ews_label_; }
    [[nodiscard]] QString rdsEwsDetail() const { return rds_ews_detail_; }
    [[nodiscard]] bool rdsTmcShown() const { return rds_tmc_shown_; }
    [[nodiscard]] QString rdsTmcLabel() const { return rds_tmc_label_; }
    [[nodiscard]] QString rdsTmcCounts() const { return rds_tmc_counts_; }
    [[nodiscard]] QString rdsTmcDetail() const { return rds_tmc_detail_; }
    [[nodiscard]] QStringList rdsTmcPayloads() const { return rds_tmc_payloads_; }
    [[nodiscard]] int rdsTmcNotListed() const { return rds_tmc_not_listed_; }
    [[nodiscard]] double rdsBlockErrorRate() const { return rds_block_error_rate_; }
    [[nodiscard]] double rdsBitRateHz() const { return rds_station_.health.bit_rate_hz; }
    [[nodiscard]] double rdsCarrierOffsetHz() const {
        return rds_station_.health.carrier_offset_hz;
    }
    [[nodiscard]] double rdsQuality() const { return rds_station_.health.quality; }
    [[nodiscard]] qulonglong rdsGroups() const {
        return static_cast<qulonglong>(rds_station_.health.groups_decoded);
    }
    [[nodiscard]] QString rdsFault() const { return rds_fault_; }

    // ------------------------------------------------------------------
    // The audio surface
    // ------------------------------------------------------------------

    [[nodiscard]] bool audioWanted() const { return audio_wanted_.load(); }
    void setAudioWanted(bool wanted);
    [[nodiscard]] bool audioOffered() const {
        return mode_makes_audio(receiverDemod().toStdString());
    }

    [[nodiscard]] bool audioActive() const { return audio_vrx_ != 0; }
    [[nodiscard]] qulonglong audioReceiverId() const { return audio_vrx_; }
    [[nodiscard]] uint audioGrantedMillis() const { return audio_granted_millis_; }
    [[nodiscard]] QString audioFault() const { return audio_fault_; }
    [[nodiscard]] QString audioEndedReason() const { return audio_ended_reason_; }

    [[nodiscard]] qulonglong audioFramesDropped() const {
        return static_cast<qulonglong>(audio_stats_.frames_dropped);
    }
    [[nodiscard]] qulonglong audioDropEvents() const {
        return static_cast<qulonglong>(audio_stats_.drop_events);
    }
    [[nodiscard]] qulonglong audioBacklogFrames() const {
        return static_cast<qulonglong>(audio_stats_.backlog_frames);
    }
    [[nodiscard]] qulonglong audioBufferFrames() const {
        return static_cast<qulonglong>(audio_stats_.buffer_frames);
    }

    // The hand-offs the Cap'n Proto event loop thread writes chunks into
    // and the sound card's thread drains, one per rack slot, reached through
    // audioRingAt above. They live HERE and not on the player, because the
    // callback that writes them is owned by the Client, the Client is owned
    // by the supervisor thread, and this destructor is what joins that
    // thread. A ring owned by the player would be destroyed while a callback
    // could still be writing to it, on an object-destruction order QML and
    // main() both get to influence.
    //
    // Every thread that touches one holds its own lock, so the accessor
    // hands out a reference and not a copy. There are FOUR of them and
    // audio/audio_ring.h names them: the event loop writes chunks, the
    // sound card's pull thread reads, the Qt thread snapshots, and the
    // supervisor resets and resizes. This comment said "both threads" until
    // 2026-09-20, which undercounted the writers by one and is corrected
    // rather than swapped, because the count is the whole of why the
    // accessor is shaped this way.
    //
    // WHAT THE ACCESSOR USED TO BE: audioRing(), one ring, for the one
    // receiver the pane was listening to.

    // ------------------------------------------------------------------
    // The decode surface. Implemented in ui/models/decoded_link.cpp.
    // ------------------------------------------------------------------

    [[nodiscard]] bool decodeWanted() const { return decode_wanted_.load(); }
    void setDecodeWanted(bool wanted);

    [[nodiscard]] QString decodeChoice() const { return decode_choice_shown_; }
    void setDecodeChoice(const QString& choice);

    [[nodiscard]] QStringList decodeChoices() const { return decode_choices_; }
    [[nodiscard]] QString decodeAttached() const { return decode_attached_; }
    [[nodiscard]] QString decodeLabel() const { return decode_label_; }
    [[nodiscard]] QString decodeDetail() const { return decode_detail_; }
    [[nodiscard]] DecodedLogModel* decodedLog() { return &decoded_log_; }

    // The engine's one sentence about a decoder, for the menu's tooltip.
    // Empty for auto and for a name the engine did not list.
    [[nodiscard]] Q_INVOKABLE QString decoderDescription(const QString& name) const;

    // A receiver to open, and what to decode on it, as soon as a source is
    // open that reaches the frequency. For main()'s --receiver and --decode,
    // which exist so the decode section can be exercised and photographed
    // without anybody clicking. Once per call: the first connection that can
    // place it does, and nothing after that repeats it.
    void setStartupReceiver(double absolute_hz, const QString& mode, const QString& decoder);

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

    // receiverFitText changed. Its own signal because it is derived from
    // BOTH the request and the grant, so neither receiverChanged nor
    // receiverStatusChanged covers it, and a property may name only one.
    void receiverFitChanged();

    // Any of the aft properties changed.
    void aftChanged();

    // Any of the auto filter properties changed.
    void autoFilterChanged();

    // The auto filter has just moved the edges, from these. The pane eases
    // its rules from here to the new edges, so the fit is seen happen.
    void autoFilterFitted(int from_low, int from_high);

    // The bookmark list changed: one was saved, removed or renamed. Also
    // emitted when the pane moves to another receiver, because
    // receiverBookmarked is about the receiver and not about the list.
    void bookmarksChanged();

    void bookmarkFaultChanged();

    // The engine let the pane's receiver go, or the operator has tuned since
    // and the sentence about it has been cleared.
    void receiverGoneChanged();

    // The rack changed: a receiver was added, removed, focused, muted,
    // soloed or given a gain, a held receiver's level or frequency came back,
    // or the focused one moved. Also emitted beside receiverChanged and
    // receiverStatusChanged, because the focused strip reads the pane.
    void rackChanged();

    // A click on the span focused, retuned, opened or added a receiver, so
    // the receiver window comes forward. Emitted by spanClick and
    // spanDoubleClick for the clicks click_brings_receivers_forward in
    // models/window_raise.h names; Main.qml does the raising.
    void receiverWindowWanted();

    // The engine's receiver inventory changed, or this window's place in it
    // did.
    void strandedReceiversChanged();

    // The source's tuning surface changed: the range came back on a new
    // connection, a retune was granted, or one was refused. Separate from
    // connectionChanged because that one is read as a new engine by every
    // item holding history, and a refused retune is not one.
    //
    // A GRANTED retune emits connectionChanged as well, and deliberately:
    // the span moved, so every row already drawn was drawn under a
    // frequency axis that no longer applies, which is exactly the case
    // that signal exists for.
    void sourceTuningChanged();

    // The stage, the handle, the readout and the refusal. One signal because
    // one panel reads all of them and none of them repaints a display.
    void sourceGainChanged();

    // The device list, the refresh's busy flag, or the last open or close
    // refusal moved.
    void sourcesChanged();

    // The pacing measurement moved, or its verdict did. Emitted only on a
    // change, because it is polled once a second for the life of the
    // window and the answer is the same almost every time.
    void pacingChanged();

    // The RDS switch moved, the decoder's state changed, or a new station
    // snapshot arrived. One signal for all of it, because one pane reads
    // all of it and none of it repaints a display.
    void rdsChanged();

    // The audio subscription changed: it started, it stopped, the engine
    // refused it, the receiver went away, or the engine's counters moved.
    // One signal for all of them, because every one of them is read by the
    // same status strip and none of them repaints anything.
    void audioChanged();

    // The decode switch, the menu, the choice, what is attached, or a
    // refusal moved. Not emitted for a line arriving; the log model has its
    // own rows for that.
    void decodeChanged();

private:
    // The supervisor thread, and the two halves of what it does.
    void supervise();
    [[nodiscard]] bool attempt_connect();
    void publish(bool connected, QString error);

    // Supervisor thread. Reads Error::category and decides when the next
    // connection attempt is worth making. See kRefusedCredentialInterval in
    // engine_link.cpp for why a refused credential is backed off rather than
    // stopped, and why every other failure is retried at the ordinary rate.
    void hold_off(const Error& failure);

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

    // When the next connection attempt is worth making. Supervisor thread
    // only, written and read in supervise() and hold_off() and nowhere else,
    // so unlike stopping_ above it needs no lock.
    //
    // The epoch is always in the past, which is what makes the first attempt
    // immediate and what a successful connection resets it to.
    std::chrono::steady_clock::time_point retry_after_{};

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

    // Zero passes everything, which is what an operator who has never touched
    // it should get.
    std::atomic<double> margin_bar_{0.0};

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

    // Supervisor thread, on a failed status read and nowhere else. Works out
    // whether the ENGINE has removed the pane's receiver, which it does when a
    // retune leaves the receiver's centre outside the new span, and asks the
    // Qt thread to empty the pane when it has.
    //
    // WRITTEN TO DO NOTHING, because almost every failed read is something
    // else: a connection that has gone, a rebuild this client posted and has
    // not applied, or a receiver the engine still has and could not describe to
    // a client older than its schema. All three return without touching
    // anything, and tearing a receiver down on one of them would lose an
    // operator's receiver for no reason, which is worse than the state this
    // exists to end.
    //
    // The evidence it acts on is Client::vrx_ids, the engine's own list of
    // receivers, rather than the refusal's wording or its category. The body
    // says why neither of those will do.
    void forget_removed_receiver(const Error& failure);

    // Supervisor thread. Adds a receiver for the pane, subscribes its
    // passband, and hands the id to the Qt thread under key, the rack entry
    // it is for. Removes the previous one first, because the pane holds one.
    [[nodiscard]] bool recreate_receiver(const rpc::VrxParams& params, std::uint64_t key);
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

    // Qt thread. Drops the held passband frame and the flag saying one has
    // arrived, which together are the detail pane's whole picture: the frame
    // carries the axis passbandFrequencyAtFraction reads, and the flag is
    // what suppresses the "waiting for the first passband frame" plate.
    //
    // ONE FUNCTION BECAUSE THE TWO MUST NEVER BE CLEARED SEPARATELY, AND
    // CALLED FROM EVERY TRANSITION THAT INVALIDATES THEM. passband_active_
    // used to be set in drain_passband and cleared in removeReceiver and the
    // connection adopt alone, so it latched across a retune and a mode
    // change. An nfm receiver switched to raw is the entrance that needs no
    // race: subscribe_passband is refused for a raw tap by contract, no
    // frame ever arrives for the new receiver, and the pane went on drawing
    // the nfm trace with the waiting plate suppressed. The axis was wrong
    // with it, because bin_zero came off the dead frame while
    // receiverCenterHz came off the live receiver, and that mapping is what
    // places the drawn filter rules and answers grabAt.
    //
    // Returns whether anything was actually dropped, so a caller on the
    // frame path emits only when there is news.
    bool reset_passband_display();

    // Qt thread. The request the pane holds, fitted to the engine's limits.
    [[nodiscard]] std::pair<int, int> fit_edges(int low, int high) const;

    // Qt thread. Recomputes receiverFitText from the request, the grant
    // and the detection that placed the receiver, and emits only when it
    // changed.
    //
    // CALLED FROM BOTH SIDES OF THE ASYNCHRONY, which is the whole reason
    // it is a function and not a getter. The request moves on the Qt
    // thread and the grant arrives from the supervisor, and a line derived
    // from both has to be rebuilt on either. post_receiver_request covers
    // every write, because every Q_INVOKABLE ends there, and
    // adopt_receiver_status covers every answer.
    void update_receiver_fit();

    // The body both Q_INVOKABLE tune entry points share. Private because
    // the bandwidth argument is not a thing QML should be able to make up:
    // it is a measurement or it is absent.
    //
    // mode_named_by_operator says whether the mode was stated by a caller or
    // derived here from a measurement, which is what decides whether it sets
    // demod_touched_. A mode this client chose must not record the operator as
    // having chosen it, or the first detection click would pin the detector's
    // own guess for the life of the receiver.
    void tune_receiver(double absolute_hz, const QString& mode,
                       double detection_bandwidth_hz, bool mode_named_by_operator);

    // The bandwidth of the detection this receiver was placed by, or zero
    // for one placed by hand. Zero suppresses the signal comparison
    // entirely; see tuneReceiverToDetection.
    double tuned_detection_bandwidth_ = 0.0;
    QString receiver_fit_text_;
    QString receiver_fit_label_;

    // Qt thread only. The pane's own authoritative copy of the request,
    // which is what the overlay draws and what the supervisor sends. Its
    // center is BASEBAND, the frame VrxParams is in; receiverCenterHz adds
    // the source's centre back for the label.
    rpc::VrxParams wanted_;
    qulonglong receiver_id_ = 0;

    // The absolute frequency the pane was last tuned to, kept because it
    // cannot be rebuilt once the front end moves: wanted_.center is an offset
    // against the source centre of the moment it was set, and the receiver is
    // removed precisely because that centre changed. Qt thread only.
    std::int64_t receiver_absolute_hz_ = 0;

    // Why the pane is empty when the engine emptied it. Qt thread only, and
    // deliberately outliving the pane; see the property.
    QString receiver_gone_text_;

    // Where to offer that receiver back, zero for no offer. Set and cleared
    // with receiver_gone_text_. Qt thread only.
    std::int64_t receiver_comeback_hz_ = 0;

    // The last answer receiverBookmarked gave, so a receiver move that does
    // not change it emits nothing. Qt thread only.
    //
    // WHY THIS IS NOT PREMATURE. post_receiver_request is reached on every
    // mouse move of a passband PAN, which is sent immediately where a width
    // change is held to the end of the gesture, and bookmarksChanged is also
    // what bookmarkLabels is notified by. Emitting it unconditionally there
    // would rebuild a QStringList at pointer rate to say nothing, which is the
    // argument note_receiver_fault already makes about waking the Qt thread.
    bool receiver_bookmarked_ = false;

    // Recomputes receiverBookmarked and emits only when it moved.
    void note_receiver_bookmarked();

    // What the engine says it is holding, and the sentence derived from it.
    // Qt thread only.
    std::vector<qulonglong> engine_receiver_ids_;
    QString stranded_text_;

    // The supervisor's copy of the same, guarded by receiver_mutex_, plus the
    // operator's request to release what this window does not hold.
    std::vector<qulonglong> handover_receiver_ids_;
    bool has_receiver_ids_ = false;
    bool release_stranded_ = false;

    // Asks the engine what it is holding. On the probe pass only: an
    // inventory changes when somebody opens or closes a window, which is not
    // four times a second.
    void poll_receiver_inventory();

    // Performs a release the Qt thread asked for. Supervisor thread.
    void apply_stranded_release();

    void adopt_receiver_inventory();

    // The wheel's accumulator, its clock and its flush, one set for the whole
    // window. Qt thread only.
    //
    // A QElapsedTimer and not a wall clock, because what is measured is how
    // long the radio has had to recover from the last retune, and a wall clock
    // stepping backwards under a scrolling operator would release the whole
    // backlog at once.
    ScrollTuneState scroll_tune_;
    QElapsedTimer scroll_clock_;
    QTimer scroll_flush_;

    // The same for the wheel over the fine-tuning display, which moves the
    // receiver rather than the front end and settles far faster.
    ReceiverScrollState receiver_scroll_;
    QTimer receiver_scroll_flush_;

    // Automatic frequency tracking, run once per passband frame on the Qt
    // thread; see models/aft_link.cpp. aft_vrx_ is the receiver the loop's
    // measurements came from, so a rebuilt receiver starts it afresh.
    AftLoop aft_;
    AftStep aft_step_;
    std::uint64_t aft_vrx_ = 0;
    void run_aft();

    // The operator touched the receiver: the loop holds, then reacquires.
    void aft_yield();

    // The auto filter, run once per passband frame on the Qt thread while a
    // fit is due; see models/auto_filter_link.cpp. auto_filter_due_ is set by
    // a click-to-tune and by switching it on, and cleared by the fit, by a
    // drag, and by any tuning by hand.
    bool auto_filter_enabled_ = false;
    bool auto_filter_due_ = false;
    AutoFilterAverage auto_filter_average_;
    AutoFilterFit auto_filter_last_{};
    std::uint64_t auto_filter_vrx_ = 0;
    void run_auto_filter();
    void arm_auto_filter();
    void cancel_auto_filter(AutoFilterOutcome why);

    // The timer came due with no wheel behind it: asks whether the accumulator
    // can be spent now.
    void flush_scroll_tune();

    // Arms the single shot for what plan_scroll_tune said to wait, or stops it
    // when nothing is held back.
    void arm_scroll_flush(double wait_ms);

    // Qt thread only. The pane held a receiver when the link went away, so
    // the next connection puts one back from wanted_.
    //
    // A member and not a local, because a reconnection is two adopt() calls
    // and the fact is known in the first and needed in the second. adopt()
    // sets it when it zeroes a non-zero receiver_id_ and clears it on the
    // connecting edge that acts on it; see the block there for what the
    // local it replaced could not do.
    bool restore_receiver_ = false;

    // The operator has moved an edge on this receiver, so a mode change
    // keeps their edges instead of taking the new mode's default.
    bool edges_touched_ = false;

    // ------------------------------------------------------------------
    // Bookmarks. Qt thread only, all of it.
    // ------------------------------------------------------------------

    // The list as held in memory. The registry is written from this rather
    // than read back, so the order in bookmarkLabels is the order saved.
    //
    // READ ONCE IN THE CONSTRUCTOR rather than lazily on first access. A
    // lazy load would have to happen inside a const getter, which means
    // either mutable state or a const_cast to hide a write, and the thing
    // being deferred is a single registry read of a string.
    std::vector<Bookmark> bookmarks_;

    QString bookmark_fault_;

    // A bookmark waiting for the front end to arrive.
    //
    // WHY THERE IS A WAIT AT ALL. tuneSourceHz posts to the supervisor and
    // returns, so source_center still holds the old value on the next line
    // and a receiver placed immediately would go to the offset the OLD
    // centre implies. The bookmark is held here and placed from the same
    // Qt-thread adopt that moves the span, which is the first moment the
    // subtraction in tune_receiver is against the right number.
    std::optional<Bookmark> pending_recall_;

    // Places pending_recall_ now that the front end has moved, or abandons
    // it with a reason if the retune was refused. Called from the source
    // half's adopt, which is where a granted centre reaches the Qt thread.
    void resolve_pending_recall(bool granted);

    // Puts the receiver on a bookmark, assuming it has already been found
    // reachable. The one place the two writes happen, so a recall that
    // needed a retune and one that did not cannot drift apart.
    void place_recall(const Bookmark& mark);

    void load_bookmarks();
    void store_bookmarks() const;

    // The operator has named the mode on this receiver, so a click on a
    // detection keeps it instead of deriving one from the measured bandwidth.
    // The same rule edges_touched_ applies to the passband, with the same
    // lifetime: it is a fact about this receiver and it goes when the receiver
    // does, in removeReceiver.
    //
    // WHY IT EXISTS. Deriving the mode from the detection's width was added on
    // 2026-09-21 to stop rpc::VrxParams::demod's Nfm default riding through a
    // click onto a 145 kHz broadcast station. It derived on every click, which
    // overrides a mode the operator picked by hand as readily as a default
    // nobody picked. On broadcast FM every detection is wider than
    // kNarrowbandChannelHz and ui::demod_for_detection answers Wfm for all of
    // them, so a hand-picked nfm lasted until the next click anywhere inside a
    // box, and an operator reported the mode switching itself back to WFM.
    // EngineLink::tuneReceiverToDetection carries the full mechanism.
    bool demod_touched_ = false;

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

    // The rack entry the request above is for, which is the pane's key when
    // it was posted. A request is always about the receiver the pane held
    // at the time, and after a focus change that is not the receiver the
    // pane holds now.
    std::uint64_t requested_pane_key_ = 0;  // guarded by receiver_mutex_

    rpc::VrxStatus pending_receiver_status_;   // guarded by receiver_mutex_
    bool has_pending_receiver_status_ = false;  // guarded by receiver_mutex_
    std::uint64_t pending_receiver_status_key_ = 0;  // guarded by receiver_mutex_

    // Every engine id the supervisor has settled for a rack entry since the
    // Qt thread last looked, in order: a rebuild, a focus, a park. A list and
    // not one slot, because one supervisor pass can settle two of them, the
    // outgoing receiver of a park and the new one the pane then opens, and a
    // single slot kept only the second.
    //
    // WHAT THIS USED TO BE: pending_receiver_id_ and a flag, for the one
    // receiver the pane held.
    struct SettledId {
        std::uint64_t key = 0;
        qulonglong id = 0;
    };
    std::vector<SettledId> pending_receiver_ids_;  // guarded by receiver_mutex_

    QString pending_receiver_fault_;            // guarded by receiver_mutex_
    bool has_pending_receiver_fault_ = false;   // guarded by receiver_mutex_

    // Supervisor thread only: the receiver it has actually created on the
    // engine, and the fault last handed over so a repeat needs no lock.
    qulonglong live_receiver_id_ = 0;
    QString posted_receiver_fault_;

    // Supervisor thread only: which rack entry the pane's receiver is, and
    // the params it was last given, which a park carries into the held set.
    std::uint64_t live_pane_key_ = 0;
    rpc::VrxParams live_receiver_params_;

    // ------------------------------------------------------------------
    // The rack. Implemented in ui/models/rack_link.cpp.
    // ------------------------------------------------------------------

    // WHY EVERYTHING GOES THROUGH ONE QUEUE. The pane's request above is one
    // slot, and whatever is in it is about the receiver the pane held when it
    // was written. A focus change moves the pane to another receiver, so a
    // request written before it and one written after it are about two
    // different receivers, and a single slot would apply the second to the
    // first. So a rack operation takes the pane's outstanding request with
    // it, and the supervisor applies that request to the receiver it was
    // about BEFORE it performs the operation, then the operations in the
    // order they were posted, then whatever the slot holds by then.
    struct PaneRequest {
        bool wanted = false;
        bool recreate = false;
        bool remove = false;
        rpc::VrxParams params;
        std::uint64_t key = 0;
    };

    struct RackOp {
        enum class Kind : std::uint8_t {
            // The pane's receiver becomes a held one under key. It keeps
            // running and keeps its audio; the pane lets go of its passband
            // and its decoders.
            Park,
            // The held receiver under key becomes the pane's.
            Focus,
            // A new held receiver with these params, under key.
            AddHeld,
            // The held receiver under key is removed from the engine.
            RemoveHeld,
        };
        Kind kind = Kind::Park;
        std::uint64_t key = 0;
        rpc::VrxParams params;
        PaneRequest outgoing;
    };

    // Qt thread. Takes the pane's outstanding request into op and queues op.
    void post_rack_op(RackOp op);

    // Supervisor thread. The slot's content, taken and cleared, with the
    // lock held by the caller.
    [[nodiscard]] PaneRequest take_pane_request_locked();

    // Supervisor thread. The body apply_receiver_request always had, for
    // one request.
    void apply_pane_request(const PaneRequest& request);
    void apply_rack_op(const RackOp& op);

    // Supervisor thread. Reads every held receiver's status, once per pass
    // as the pane's is, and notices one the engine has let go.
    void poll_held_status();

    // Supervisor thread. Forgets every held receiver without asking the
    // engine, because the engine or its source has gone and took them.
    void forget_held(const QString& why);

    // Supervisor thread. The engine id behind a rack key, and its mode, or
    // zero.
    [[nodiscard]] qulonglong id_for_key(std::uint64_t key, rpc::Demod* demod) const;

    // Supervisor thread. Hands the Qt thread a settled id for a key.
    void post_settled_id(std::uint64_t key, qulonglong id);

    // Supervisor thread. Hands the Qt thread what it learned about held
    // receivers. HeldReport is declared below with the rest of the rack.
    struct HeldReport;
    void post_held_reports(std::vector<HeldReport> reports);

    // Qt thread.
    void adopt_held_reports();

    // Qt thread. Moves the pane onto the held receiver under key, parking
    // the pane's own first. The one path every focus change takes.
    void focus_entry(std::uint64_t key);

    // Qt thread. Moves the pane's receiver into the held set and leaves the
    // pane empty. Does nothing when the pane holds no rack entry.
    void park_pane();

    // Qt thread. Empties everything the pane holds about its receiver, which
    // removeReceiver and park_pane share.
    void clear_pane();

    // Qt thread. A rack entry for the pane when it has none, so a tune on an
    // empty pane has somewhere to put its receiver. False when the rack is
    // full.
    [[nodiscard]] bool ensure_pane_entry();

    // Qt thread. A new receiver in the pane at this frequency, parking the
    // focused one; detection_bandwidth_hz is zero for one placed by hand.
    void add_receiver_at(double absolute_hz, const QString& mode, double detection_bandwidth_hz);

    // Qt thread. A held receiver the rack does not show yet, made now: for
    // the second and later --receiver, and for every held receiver a
    // reconnection puts back.
    void add_held_receiver(std::uint64_t key, double absolute_hz, const rpc::VrxParams& params);

    // Qt thread. What the mix is asked for: which rack entries are heard, on
    // which slot, at what gain.
    void post_audio_wants();

    void set_rack_note(const QString& note);

    // Qt thread. What the rack shows for a receiver the pane does not hold.
    struct HeldView {
        rpc::VrxParams params;
        std::int64_t absolute_hz = 0;
        bool edges_touched = false;
        bool demod_touched = false;
        double detection_bandwidth_hz = 0.0;
        int granted_low = 0;
        int granted_high = 0;
        int edge_limit = 0;
        double level_dbfs = -200.0;
    };

    ReceiverRack rack_;
    std::vector<std::pair<std::uint64_t, HeldView>> held_views_;
    std::uint64_t pane_key_ = 0;
    QString rack_note_;

    [[nodiscard]] HeldView* held_view(std::uint64_t key);
    [[nodiscard]] const HeldView* held_view(std::uint64_t key) const;

    // Qt thread. The pane as the first click of a possible double click
    // found it, so the second can put it back. See spanDoubleClick.
    struct ClickSnapshot {
        bool valid = false;
        qint64 at_ms = 0;
        bool had_receiver = false;
        bool opened = false;
        std::uint64_t key = 0;
        rpc::VrxParams wanted;
        std::int64_t absolute_hz = 0;
        bool edges_touched = false;
        bool demod_touched = false;
        double detection_bandwidth_hz = 0.0;
    };
    ClickSnapshot click_snapshot_;
    QElapsedTimer click_clock_;

    // Startup receivers after the first, until a connection can place them.
    std::vector<std::pair<double, QString>> startup_extra_;

    // Guarded by receiver_mutex_. The operations the Qt thread has posted.
    std::vector<RackOp> rack_ops_;

    // Supervisor thread only. The receivers held and not in the pane.
    struct HeldVrx {
        std::uint64_t key = 0;
        qulonglong id = 0;
        rpc::VrxParams params;
    };
    std::vector<HeldVrx> held_;

    // What the supervisor learned about held receivers, for the Qt thread.
    // Also the pane's refused adds: those are the one report about the
    // pane's own entry, because the strip is where a refusal is shown.
    struct HeldReport {
        std::uint64_t key = 0;
        qulonglong id = 0;
        bool has_status = false;
        rpc::VrxStatus status;
        bool gone = false;
        bool refused = false;
        QString why;
    };
    std::vector<HeldReport> handover_held_;  // guarded by receiver_mutex_

    // Set by any write, cleared by the supervisor when it has applied one.
    // It is in the wait predicate, so a drag is applied on the next tick of
    // the loop rather than on the next poll interval: 250 ms of latency on
    // a filter edge is felt as the handle sticking.
    bool receiver_work_pending_ = false;  // guarded by supervisor_mutex_

    // ------------------------------------------------------------------
    // Tuning the front end. Implemented in ui/models/source_link.cpp.
    // ------------------------------------------------------------------

    // Supervisor thread. Asks the source whether it retunes and over what
    // range, once per connection, and hands the answer over. Called from
    // attempt_connect after the EngineInfo is in hand.
    void probe_source_tuning();

    // Supervisor thread. Applies a retune the Qt thread asked for, reads
    // the new EngineInfo back and hands both over. The re-read is not
    // optional: EngineInfo::sourceCenter is what every absolute frequency
    // in this client is derived from, and a retune is the one thing that
    // moves it while the connection stays up.
    void apply_source_tune();

    // Supervisor thread, probe pass only. Re-reads EngineInfo for the
    // pacing pair and hands it over when it has moved.
    //
    // ONE EXTRA ROUND TRIP A SECOND, AND IT BUYS THE ONE DIAGNOSIS NOBODY
    // COULD MAKE. Everything else in EngineInfo is fixed for the life of a
    // connection, so this call exists only for the two measured fields. It
    // is on the probe pass and not the detection pass for the reason
    // poll_audio_stats is: nothing acts on it and a status line does not
    // need four samples a second.
    void poll_source_pacing(bool engine_running);

    // Supervisor thread, probe pass only. Reads SourceStats for the front
    // end's verdict and hands it over when it has moved.
    //
    // A SECOND ROUND TRIP ON THE SAME PASS, because the verdict rides on
    // SourceStats and the pacing pair rides on EngineInfo. Folding them
    // would mean moving one field to the other struct, and neither move is
    // right: EngineInfo is what the engine settled on at open and this
    // moves every decision, while the counters are the source's own and
    // the pacing pair is the graph's.
    void poll_front_end(bool engine_running);

    // Qt thread, queued from the supervisor.
    void adopt_source_tuning();
    void adopt_pacing();
    void adopt_front_end();

    // Supervisor thread, from poll_source_pacing, which already fetches the
    // EngineInfo this reads. Notices that the stream underneath a connection
    // that never dropped has been replaced, and re-establishes everything that
    // went with it.
    void note_source_epoch(const rpc::EngineInfo& info);

    // Supervisor thread. Takes whatever the picker posted and applies it in
    // the order it was posted: the listing, then a close, then an open. That
    // order is the only one that works when a panel refreshes and opens in one
    // gesture, and the engine refuses an open over a live source, so a close
    // that ran after its open would leave nothing running.
    void apply_source_request();

    // Qt thread, queued from apply_source_request.
    void adopt_sources();

    // Supervisor thread. The gain a slider asked for, and the stage to draw
    // one over. Separate from apply_source_request because they are posted by
    // different gestures and a gain change must not wait behind a listing,
    // which opens every device index and takes a libusb timeout per absent
    // one.
    void apply_source_gain();
    void poll_source_gain_stage(std::uint64_t epoch);

    // Qt thread, queued from the two above. adopt_gain takes whether the
    // device actually answered with a gain, which auto-on does not: handing
    // the stage to the AGC means this window stops knowing what the tuner is
    // on, and a readout left standing would be the last manual value presented
    // as current.
    void adopt_gain_stage();
    void adopt_gain(bool granted_known);

    // TWO HANDOVERS AND NOT ONE, BECAUSE THEY ARE WRITTEN BY DIFFERENT
    // EVENTS AND CARRY DIFFERENT FIELDS. The range answer arrives once per
    // connection; the tune answer arrives per write. A single struct would
    // have forced the supervisor to fill in the fields it was not changing
    // by reading the Qt thread's own copies, which is a data race on every
    // one of them.
    std::mutex source_mutex_;

    bool handover_has_range_ = false;      // guarded by source_mutex_
    bool handover_can_retune_ = false;     // guarded by source_mutex_
    std::int64_t handover_tune_low_ = 0;   // guarded by source_mutex_
    std::int64_t handover_tune_high_ = 0;  // guarded by source_mutex_
    QString handover_retune_unavailable_;  // guarded by source_mutex_

    bool handover_has_tune_ = false;          // guarded by source_mutex_
    QString handover_tune_fault_;             // guarded by source_mutex_
    std::int64_t handover_tune_granted_ = 0;  // guarded by source_mutex_
    bool handover_tune_answered_ = false;     // guarded by source_mutex_

    // The receivers the engine said the retune removed, with the frequency
    // each was on. See adopt_source_tuning.
    std::vector<rpc::RetuneRemoval> handover_tune_removed_;  // guarded by source_mutex_

    // The new geometry a granted retune produced, handed over with the
    // rest so the centre and the tuning state land in one adopt. Two
    // adopts would put the old centre on screen beside the new granted
    // frequency for one turn of the event loop, which is a readout
    // claiming the radio is somewhere it is not.
    rpc::EngineInfo handover_tuned_info_;        // guarded by source_mutex_
    bool has_tuned_info_ = false;                // guarded by source_mutex_

    // Written by the Qt thread, consumed by the supervisor. An atomic
    // pair rather than a lock because it is one integer and a flag, and
    // the last write wins by design: an operator typing twice before the
    // supervisor wakes wants the second frequency.
    std::atomic<bool> tune_pending_{false};
    std::atomic<std::int64_t> requested_center_hz_{0};

    // Qt thread only: what the properties above hand out.
    bool source_can_retune_ = false;
    std::int64_t source_tune_low_ = 0;
    std::int64_t source_tune_high_ = 0;
    QString source_retune_unavailable_;
    QString tune_fault_;
    std::int64_t tune_requested_hz_ = 0;
    std::int64_t tune_granted_hz_ = 0;
    bool tune_answered_ = false;

    // Set by tuneSource, cleared by the supervisor when it has applied
    // one. In the wait predicate for the reason receiver_work_pending_ is:
    // a quarter of a second between pressing return and the radio moving
    // reads as the box not working.
    bool tune_work_pending_ = false;  // guarded by supervisor_mutex_

    // ---- the device picker -------------------------------------------------

    // What the Qt thread asked for, taken by the supervisor on its next pass.
    // Under source_mutex_ rather than atomics because a URI is a string, and
    // because a listing and an open posted together have to be applied in that
    // order rather than in whichever the supervisor noticed.
    //
    // want_listing_ is the one of the three the SUPERVISOR also sets, from
    // note_source_epoch, because a replaced source makes every descriptor in
    // the picker a description of the world before the switch. The lock is what
    // makes that safe rather than the thread it is set on.
    bool want_listing_ = false;       // guarded by source_mutex_
    bool want_open_ = false;          // guarded by source_mutex_
    bool want_close_ = false;         // guarded by source_mutex_
    QString wanted_uri_;              // guarded by source_mutex_

    // What the supervisor learned, waiting for the Qt thread to adopt it.
    bool handover_has_sources_ = false;      // guarded by source_mutex_
    std::vector<rpc::SourceDescriptor> handover_sources_;  // guarded by source_mutex_
    bool handover_has_source_fault_ = false;  // guarded by source_mutex_
    QString handover_source_fault_;           // guarded by source_mutex_
    bool handover_sources_busy_ = false;      // guarded by source_mutex_

    // Qt thread only: what the properties above hand out.
    QVariantList sources_;
    std::vector<rpc::SourceDescriptor> source_rows_;
    QString source_fault_;
    bool sources_busy_ = false;

    // Supervisor thread only. The epoch this window last drew against. Zero
    // until the first EngineInfo arrives, which is why the first sighting is
    // not treated as a change: nothing moved, this window is only seeing the
    // number for the first time.
    //
    // Reset to zero by a failed re-subscribe, so the difference stays and the
    // next pass tries again rather than leaving the display unfed forever.
    std::uint64_t seen_source_epoch_ = 0;

    // --- the gain control -------------------------------------------------
    //
    // Qt thread only, all five. The stage is replaced whole when a descriptor
    // arrives, and the three below it are what this window last asked for and
    // was granted.
    rpc::GainStage gain_stage_{};
    int gain_stage_count_ = 0;
    double gain_db_ = 0.0;
    bool gain_known_ = false;
    bool gain_auto_ = false;
    QString gain_fault_;

    // Supervisor thread only. The epoch the gain stage was read for, so the
    // descriptor is fetched once per source rather than once per pass: it
    // touches no device, but it is still a round trip and the stage cannot
    // change under a source that has not been reopened.
    //
    // A DIFFERENT COUNTER FROM seen_source_epoch_, which is the one the
    // spectrum re-subscribe uses and is reset to zero by a failed re-subscribe
    // so that pass tries again. Sharing it would make a failed re-subscribe
    // re-read the descriptor too, and a descriptor read failing would rewind
    // the re-subscribe.
    std::uint64_t gain_stage_epoch_ = 0;
    bool gain_stage_read_ = false;

    // Guarded by source_mutex_, handed from the supervisor to the Qt thread
    // the way every other source fact is.
    bool handover_has_gain_stage_ = false;
    rpc::GainStage handover_gain_stage_{};
    int handover_gain_stage_count_ = 0;

    // Guarded by source_mutex_. What the Qt thread wants the gain to be, and
    // what the supervisor has not applied yet.
    bool want_gain_ = false;
    double want_gain_db_ = 0.0;
    bool want_gain_auto_ = false;
    bool want_gain_auto_set_ = false;

    // The stage's name, copied under source_mutex_ when the request is posted.
    //
    // gain_stage_ above is the Qt thread's and is replaced WHOLE whenever a
    // descriptor arrives. The supervisor reading its std::string was a
    // use-after-free: the Qt thread reassigning the name frees the buffer the
    // supervisor is copying out of, and the window died of it on 2026-09-21.
    // The Qt thread knows the name when it posts, so it sends it.
    std::string want_gain_stage_;

    // Guarded by source_mutex_. The answer, for the handle and the readout.
    bool handover_has_gain_ = false;
    double handover_gain_db_ = 0.0;
    bool handover_gain_auto_ = false;
    QString handover_gain_fault_;

    // Set by refreshSources, openSource and closeSource, and by
    // note_source_epoch when the stream underneath a live connection has been
    // replaced. Cleared by the supervisor when it has applied them. In the wait
    // predicate for the reason tune_work_pending_ is: a device list that
    // arrives a quarter of a second after the button reads as the button not
    // working.
    bool source_work_pending_ = false;  // guarded by supervisor_mutex_

    // The pacing measurement, handed over under source_mutex_ with the
    // rest of what the supervisor learns about the source.
    bool handover_has_pacing_ = false;  // guarded by source_mutex_
    PacingSample handover_pacing_;      // guarded by source_mutex_

    // Supervisor thread only: the last sample handed over, so the
    // comparison that suppresses a repeated post reads nothing the Qt
    // thread owns.
    PacingSample posted_pacing_;

    // Qt thread only. The verdict is held across passes because
    // classify_pacing takes its own previous answer: that is where the
    // hysteresis lives, and holding it here is what keeps the rule itself
    // a pure function a test can drive.
    PacingSample pacing_;
    PacingVerdict pacing_verdict_ = PacingVerdict::NotCarried;
    QString pacing_text_;

    // The front end's verdict, carried on the same three-step path: the
    // supervisor reads it, hands it over under source_mutex_ and posts one
    // metacall, and the Qt thread renders the sentence.
    //
    // NO PREVIOUS-VERDICT STATE, unlike the pacing pair above, because
    // core/detect/front_end.h already carries its own hysteresis over a ten
    // second window. A second filter here would be smoothing an answer that
    // is already smoothed and would make the rule ui/tests drives not the
    // rule the window runs.
    bool handover_has_front_end_ = false;  // guarded by source_mutex_
    FrontEndSample handover_front_end_;    // guarded by source_mutex_

    // Supervisor thread only, for the comparison that suppresses a repeated
    // post.
    FrontEndSample posted_front_end_;

    // Qt thread only.
    FrontEndSample front_end_;
    QString front_end_text_;

    // ------------------------------------------------------------------
    // RDS. Implemented in ui/models/rds_link.cpp.
    // ------------------------------------------------------------------

    // Supervisor thread, probe pass only. Polls the station on the pane's
    // receiver when the switch is on, applies a region change first, and
    // hands whatever came back over.
    //
    // ONCE A SECOND AND NOT FOUR TIMES. A group completes at most every
    // 87.6 ms and the whole surface is a state that accumulates, so the
    // faster rate buys round trips and nothing else. core/rpc/client.h
    // makes the same argument for why this is a poll rather than a
    // subscription.
    void poll_rds();

    // Supervisor thread. Drops the decoder's state from this window's
    // side when there is nothing to poll: the switch went off, the
    // receiver went away, the connection went. The engine keeps its
    // decoder; what this clears is the claim on screen.
    //
    // reason is WHICH of those three, and the empty default is the switch
    // going off. The other two leave the pane asking for RDS with nothing
    // to ask, and the sentence has to say so rather than read as the
    // operator's own choice. models/rds_view.h renders it.
    void clear_rds(const QString& reason = {}, const QString& label = {});

    // Supervisor thread. Hands the Qt thread a sentence about why there is no
    // decoder, with no station behind it. The body of clear_rds, lifted out
    // because the composite gate and the region write both need exactly it
    // and neither wants clear_rds's own bookkeeping: that function also drops
    // rds_polled_vrx_ and rds_region_written_, which is right when the poll is
    // being abandoned and wrong when the next pass is going to carry on.
    //
    // POSTED ON EVERY PASS, unlike note_receiver_fault, which suppresses a
    // repeat. The whole RDS surface is posted on every pass for the reason
    // poll_rds gives, and a refusal that stopped being re-posted would be
    // cleared by the next answer that was not a refusal.
    void post_rds_fault(QString reason, QString label = {});

    // Supervisor thread, from poll_rds and nowhere else. Whether the pane's
    // receiver is running at the rate a composite needs, and the work to get
    // it there when it is not.
    //
    // Returns true only when the receiver is ALREADY at that rate, so a false
    // return is "not yet or not ever" and poll_rds does not go on to build a
    // decoder. Every false return has posted a sentence saying which.
    //
    // THE PROBE IS MADE ONCE PER RECEIVER, not once a second. It is two round
    // trips and a receiver created on the engine, which is not a thing to do
    // four times a minute for as long as a switch is on, and the answer cannot
    // change while the receiver does not: the channel it landed in is fixed by
    // its centre and the grid. rds_composite_probed_vrx_ is what remembers,
    // and a receiver that changes id has to be asked again because a retune
    // can move it into a channel of a different width.
    [[nodiscard]] bool ensure_composite_receiver();

    // Qt thread, queued from ensure_composite_receiver. Puts the composite
    // rate into the pane's own request and posts the rebuild.
    //
    // THE RATE BELONGS TO THE REQUEST AND NOT TO THE SUPERVISOR. wanted_ is
    // what every later retune, mode change and reconnect is rebuilt from, so a
    // rate carried anywhere else would be lost by the first one of those and
    // the decoder would silently go back to reading 48 kHz audio. It also
    // means the rebuild is the ordinary one: post_receiver_request(true) is
    // what a mode change already does, and a rate change is a remove and an
    // add on the same terms.
    void raise_receiver_to_composite();

    // Qt thread, from setRdsWanted going off and from nowhere else. Takes the
    // composite rate back out of the request, so the receiver goes back to
    // programme audio. Does nothing when the rate was never raised.
    void lower_receiver_from_composite();

    // Qt thread, queued from poll_rds.
    void adopt_rds();

    // Written by the Qt thread, read by the supervisor.
    std::atomic<bool> rds_wanted_{false};
    std::atomic<bool> rds_region_rbds_{false};

    // Supervisor thread only: the receiver the decoder was asked for and
    // the region last written, so a repeated write is not made. A
    // set_rds_region call REBUILDS the decoder and clears everything
    // accumulated, so writing the region it already has costs the
    // operator the station they were reading.
    qulonglong rds_polled_vrx_ = 0;
    bool rds_posted_region_rbds_ = false;
    bool rds_region_written_ = false;

    // Supervisor thread only: the receiver the composite probe has already
    // been made for, and the answer it gave.
    //
    // The refusal is HELD rather than re-derived, because re-deriving it means
    // adding a receiver to the engine again. It is re-posted on every pass
    // while it stands, which is what keeps it on screen; see post_rds_fault.
    // Both are keyed on the id, so a rebuild for any reason asks again, and an
    // empty refusal beside a matching id is the probe having said yes.
    qulonglong rds_composite_probed_vrx_ = 0;
    QString rds_composite_refusal_;
    QString rds_composite_label_;           // supervisor thread, beside the refusal

    std::mutex rds_mutex_;
    bool has_rds_handover_ = false;     // guarded by rds_mutex_
    bool handover_rds_answered_ = false;  // guarded by rds_mutex_
    rpc::RdsStation handover_rds_station_;  // guarded by rds_mutex_
    QString handover_rds_fault_;            // guarded by rds_mutex_
    QString handover_rds_label_;            // guarded by rds_mutex_

    // Qt thread only: what the properties above hand out. The station is
    // kept whole rather than flattened, because the flags on it are read
    // straight through and a second copy of each would be a second thing
    // to keep in step.
    rpc::RdsStation rds_station_;
    bool rds_answered_ = false;
    bool rds_is_fault_ = false;
    bool rds_decoding_ = false;
    QString rds_status_;
    QString rds_label_;
    QString rds_identity_;
    QString rds_ps_;
    QString rds_radio_text_;
    QString rds_programme_type_;
    QString rds_fault_;
    int rds_ps_segments_ = 0;
    int rds_ps_segments_total_ = 0;
    int rds_rt_segments_ = 0;
    int rds_rt_segments_total_ = 0;
    double rds_block_error_rate_ = -1.0;
    bool rds_ptyn_shown_ = false;
    QVariantList rds_ptyn_runs_;
    QString rds_ptyn_note_;
    QString rds_ptyn_corrected_detail_;
    bool rds_ews_sent_ = false;
    QString rds_ews_label_;
    QString rds_ews_detail_;
    bool rds_tmc_shown_ = false;
    QString rds_tmc_label_;
    QString rds_tmc_counts_;
    QString rds_tmc_detail_;
    QStringList rds_tmc_payloads_;
    int rds_tmc_not_listed_ = 0;

    // The station's PTYN, EWS and TMC as the properties above hand them out,
    // from models/rds_services.h, or all empty when the view is not
    // decoding. rds_link.cpp.
    void adopt_rds_services(bool decoding);

    // Set by setRdsWanted and setRdsRegion, cleared by the supervisor
    // when it has polled. In the wait predicate, so the first answer
    // arrives on the next tick rather than at the next probe: a second
    // between clicking the switch and anything appearing reads as the
    // switch not working.
    bool rds_work_pending_ = false;  // guarded by supervisor_mutex_

    // ------------------------------------------------------------------
    // Audio. Implemented in ui/models/audio_link.cpp.
    // ------------------------------------------------------------------

    // Supervisor thread. Reconciles the subscriptions against what the
    // operator asked for and which receivers the rack holds. Every audio
    // state change goes through this one function rather than being
    // applied at the site that caused it: a retune, a mode change that
    // rebuilds a receiver, a clear, a focus change, a mute, a solo, a
    // reconnect and an ended arrival all change the same inputs, and call
    // sites remembering to fix the subscriptions are chances to forget one.
    void apply_audio_request();

    // Supervisor thread. Reads the pane receiver's subscription counters off
    // the engine, on the probe pass rather than every pass: they are a status
    // line and a round trip four times a second buys nothing.
    void poll_audio_stats();

    // Supervisor thread. Ends the subscription on this receiver, if this
    // client holds one, with an explicit unsubscribe.
    //
    // THE UNSUBSCRIBE IS WHAT KEEPS ended() MEANING WHAT IT SAYS. The
    // schema is explicit that a client is never sent ended for a cancel it
    // asked for, so removing a receiver without cancelling first would
    // deliver an ended for a removal this client performed, and the window
    // would announce that the receiver went away every time the operator
    // changed mode. drop_receiver and the removal of a held receiver call
    // this before remove_vrx for exactly that reason.
    void stop_audio_for(qulonglong vrx);

    // Supervisor thread. The same for one slot, whatever it holds.
    void stop_audio_slot(std::size_t slot);

    // Supervisor thread. Every subscription forgotten without an unsubscribe,
    // because the engine or its source has gone and took them.
    void forget_audio();

    // Supervisor thread. The mask and the lead slot the player reads.
    void publish_mix();

    // Supervisor thread. Hands the Qt thread whatever changed. Posts only
    // when something did.
    void note_audio();

    // Qt thread, queued from note_audio.
    void adopt_audio();

    // Invoked on the Cap'n Proto event loop thread. Touches the slot's ring
    // and nothing else, which is the whole point of the ring.
    void on_audio_chunk(std::size_t slot, const rpc::AudioChunk& chunk);

    // Invoked on the Cap'n Proto event loop thread. Records the engine's
    // words and wakes the supervisor; the teardown itself happens there,
    // because client.h forbids calling back into the Client from here.
    void on_audio_ended(std::size_t slot, qulonglong vrx, const std::string& reason);

    std::array<AudioRing, kMaxReceivers> audio_rings_;

    // Written by the Qt thread, read by the supervisor. A switch and not a
    // state; see audioWanted.
    std::atomic<bool> audio_wanted_{false};

    // Supervisor thread only: the receiver each slot holds a subscription
    // on, and the grant that came back with it.
    struct AudioSub {
        qulonglong vrx = 0;
        std::uint32_t granted = 0;

        // The receiver's mode, fixed for its life because a mode change is a
        // new receiver. publish_mix reads it for the two mode masks.
        rpc::Demod demod = rpc::Demod::Nfm;
    };
    std::array<AudioSub, kMaxReceivers> live_audio_{};

    // Supervisor thread only: receivers whose stream the engine ended, which
    // are not asked again. The ids of receivers that have gone, so the list
    // only matters until the next connection and is cleared there.
    std::vector<qulonglong> audio_ended_vrx_;

    // Written by the Qt thread under audio_mutex_: which rack entries are
    // heard, and on which slot.
    struct AudioWant {
        std::uint64_t key = 0;
        std::size_t slot = 0;
    };
    std::vector<AudioWant> requested_audio_wants_;  // guarded by audio_mutex_

    // What the player reads on its own threads. Written by the supervisor,
    // except the gains, which are the Qt thread's.
    std::atomic<int> mix_lead_slot_{-1};
    std::atomic<std::uint32_t> mix_mask_{0};
    std::atomic<std::uint32_t> mix_granted_millis_{0};
    std::array<std::atomic<float>, kMaxReceivers> mix_gain_{};
    std::atomic<std::uint32_t> mix_level_mask_{0};
    std::atomic<std::uint32_t> mix_wfm_mask_{0};

    // Supervisor thread only: the pane receiver's subscription, derived from
    // live_audio_ on every pass, which is what the audio section describes.
    qulonglong live_audio_vrx_ = 0;
    std::uint32_t live_audio_granted_ = 0;

    // Supervisor thread only: what it currently believes, which is what
    // note_audio hands over.
    QString work_audio_fault_;
    QString work_audio_ended_;
    rpc::AudioStats work_audio_stats_;

    // Supervisor thread only: the last thing handed over, so the
    // comparison that suppresses a repeated post needs no lock and reads
    // nothing the Qt thread owns.
    qulonglong posted_audio_vrx_ = 0;
    std::uint32_t posted_audio_granted_ = 0;
    QString posted_audio_fault_;
    QString posted_audio_ended_;
    rpc::AudioStats posted_audio_stats_;

    std::mutex audio_mutex_;
    bool has_audio_handover_ = false;             // guarded by audio_mutex_
    qulonglong handover_audio_vrx_ = 0;           // guarded by audio_mutex_
    std::uint32_t handover_audio_granted_ = 0;    // guarded by audio_mutex_
    QString handover_audio_fault_;                // guarded by audio_mutex_
    QString handover_audio_ended_;                // guarded by audio_mutex_
    rpc::AudioStats handover_audio_stats_;        // guarded by audio_mutex_

    // Written by the EVENT LOOP thread in on_audio_ended and consumed by
    // the supervisor. Under the same lock rather than an atomic, because
    // the reason is a string and the flag is only meaningful with it.
    struct AudioEnded {
        std::size_t slot = 0;
        qulonglong vrx = 0;
        std::string reason;
    };
    std::vector<AudioEnded> audio_ended_;  // guarded by audio_mutex_

    // Qt thread only: what the properties above hand out.
    qulonglong audio_vrx_ = 0;
    std::uint32_t audio_granted_millis_ = 0;
    QString audio_fault_;
    QString audio_ended_reason_;
    rpc::AudioStats audio_stats_;

    // Set by setAudioWanted and by on_audio_ended, cleared by the
    // supervisor when apply_audio_request has run. In the wait predicate
    // for the reason receiver_work_pending_ is: a quarter of a second
    // between clicking listen and hearing anything reads as the control
    // not working.
    bool audio_work_pending_ = false;  // guarded by supervisor_mutex_

    // ------------------------------------------------------------------
    // Decoding. Implemented in ui/models/decoded_link.cpp.
    // ------------------------------------------------------------------

    // Supervisor thread. Fetches the engine's decoder list once per
    // connection, then reconciles the subscriptions against the switch, the
    // choice and the pane's receiver. Every decode state change goes through
    // here, for the reason apply_audio_request gives.
    void apply_decode_request();

    // Supervisor thread. Cancels every decoder subscription this client
    // holds, before its receiver is removed, so the engine's ended() keeps
    // meaning a removal somebody else made. drop_receiver calls it beside
    // stop_audio_for for that reason.
    void stop_decoded();

    // Supervisor thread. The engine or its source went, taking every
    // subscription with it; nothing to cancel. The list is asked for again,
    // because the next engine may be a different build.
    void forget_decoded();

    // Supervisor thread. Hands the Qt thread what changed, when it did.
    void note_decode();

    // Qt thread.
    void adopt_decode();
    void drain_decoded();
    void update_decode_choices();
    void place_startup_receiver();

    // Invoked on the Cap'n Proto event loop thread. The first copies the
    // message into the hand-off and posts one wake; the second records the
    // engine's words and wakes the supervisor, which does the rest.
    void on_decoded_message(const rpc::DecodedMessage& message);
    void on_decoded_ended(std::uint64_t vrx, const std::string& decoder,
                          const std::string& reason);

    // Qt thread only.
    DecodedLogModel decoded_log_;
    std::vector<rpc::DecoderInfo> decoder_infos_;
    QString decode_choice_ = QStringLiteral("auto");
    QString decode_choice_shown_;
    QStringList decode_choices_;
    QString decode_attached_;
    QString decode_label_;
    QString decode_detail_;

    // The startup receiver, until a connection can place it.
    bool startup_pending_ = false;
    double startup_hz_ = 0.0;
    QString startup_mode_;
    QString startup_decoder_;

    // Written by the Qt thread, read by the supervisor.
    std::atomic<bool> decode_wanted_{false};

    // The Cap'n Proto loop's hand-off to the Qt thread, and the ended
    // arrivals and the choice for the supervisor, all under one lock: each
    // is a few strings and nothing waits on it for long.
    struct DecodedEnded {
        std::uint64_t vrx = 0;
        std::string decoder;
        std::string reason;
    };
    std::mutex decoded_mutex_;
    std::vector<rpc::DecodedMessage> pending_decoded_;       // guarded by decoded_mutex_
    std::vector<std::int64_t> pending_decoded_arrived_ms_;   // guarded by decoded_mutex_
    std::uint64_t pending_decoded_unkept_ = 0;               // guarded by decoded_mutex_
    std::vector<DecodedEnded> pending_decoded_ended_;        // guarded by decoded_mutex_
    std::string requested_decode_choice_ = "auto";           // guarded by decoded_mutex_
    bool has_decode_handover_ = false;                       // guarded by decoded_mutex_
    std::vector<rpc::DecoderInfo> handover_decoder_infos_;   // guarded by decoded_mutex_
    bool handover_has_infos_ = false;                        // guarded by decoded_mutex_
    QString handover_decode_attached_;                       // guarded by decoded_mutex_
    QString handover_decode_label_;                          // guarded by decoded_mutex_
    QString handover_decode_detail_;                         // guarded by decoded_mutex_
    std::atomic<bool> decoded_wake_pending_{false};

    // Supervisor thread only. What the engine can attach, whether it has been
    // asked this connection, and what this client holds on which receiver.
    std::vector<rpc::DecoderInfo> work_decoder_infos_;
    bool decoder_infos_asked_ = false;
    QString decoder_infos_fault_;
    rpc::Demod live_receiver_demod_ = rpc::Demod::Nfm;
    qulonglong live_decoded_vrx_ = 0;
    std::vector<std::string> live_decoded_;

    // Each decoder whose stream the engine ended, on which receiver, whether
    // because the receiver was removed or because the decoder refused what it
    // delivered. That decoder is not subscribed there again, and the others
    // on the receiver keep running: a removed receiver refuses and a refusing
    // decoder refuses again, and either would write a second sentence over
    // the engine's own. The switch going off clears it. models/decoded_log.h
    // has why it is per decoder.
    //
    // WHAT THIS USED TO BE: one receiver id, "The receiver a stream on it
    // ended", which one decoder's ended() set, and every other decoder on
    // that receiver was cancelled on the next pass.
    EndedDecoders decode_ended_;

    // The receiver and choice the refusals below were collected for, so a
    // refused name is asked once and not on every pass.
    qulonglong decode_tried_vrx_ = 0;
    std::string decode_tried_choice_;
    std::vector<std::pair<std::string, std::string>> decode_refusals_;

    // The last hand-off, so a pass with nothing new posts nothing.
    QString posted_decode_attached_;
    QString posted_decode_label_;
    QString posted_decode_detail_;

    // Set by setDecodeWanted, setDecodeChoice and an ended arrival, cleared by
    // the supervisor when it has reconciled. In the wait predicate for the
    // reason audio_work_pending_ is.
    bool decode_work_pending_ = false;  // guarded by supervisor_mutex_
};

}  // namespace revenant::ui
