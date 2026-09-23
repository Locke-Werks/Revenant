#include "models/engine_link.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <utility>

#include "core/rpc/token.h"

#include <QMetaObject>
#include <QSettings>
#include <QString>
#include <QVariant>

#include "models/settings.h"

namespace revenant::ui {
namespace {

// The liveness probe, and the reconnect. Both run on this, because they are the
// same question asked from either side of the connection, and because an
// operator restarting an engine should see the window come back inside a breath
// rather than wonder whether it will.
//
// WHAT THIS COMMENT USED TO SAY, WHICH WAS TRUE OF ONE OF THEM:
//
//   "How often the supervisor asks the engine whether it is still there, and
//   how often it retries when there is nothing to connect to. One interval
//   serves both because they are the same question asked from either side of
//   the connection [...]"
//
// It served the probe. supervise() skips a pass early only when `!probe &&
// client_ != nullptr`, so with no client every pass fell through to
// attempt_connect and the retry ran at kDetectionPollInterval instead: four
// connections a second. adopt() runs on each failure, which is where the 240
// passes a minute in its own comment came from. The guard below now tests
// `probe` as well, so the sentence describes the loop.
constexpr std::chrono::milliseconds kSuperviseInterval{1000};

// How long to wait before offering a credential that was just refused.
//
// A refused token is not retried at the interval above, because nothing about
// asking again changes the answer: the engine holds one token and this client
// is offering a different one. Four connections a second against a server that
// will refuse every one is a cost with no return and reads in a log like
// somebody guessing.
//
// IT IS A BACKOFF AND NOT A STOP, which is the whole of why this constant
// exists rather than a flag. attempt_connect resolves the token per attempt on
// purpose, so an operator who runs --new-token, or points --token-file
// somewhere else, gets picked up without restarting the window. Stopping would
// take that away, and ten seconds is short enough that fixing the file feels
// like it worked and long enough that the window is not hammering the port.
constexpr std::chrono::milliseconds kRefusedCredentialInterval{10000};

// The supervisor loop actually wakes on this, and does the liveness and
// reconnect work on every fourth pass so that side keeps its one second.
//
// Detections are polled on every pass, at 4 Hz. One second is visibly
// sluggish for an overlay an operator is clicking on, and anything near the
// frame rate is wasted: the detector decides far less often than the engine
// produces frames, and DetectionList::last_decision is what says whether it
// has. A poll that finds the same decision emits nothing.
constexpr std::chrono::milliseconds kDetectionPollInterval{250};

// Derived rather than written as 4, so that changing either interval cannot
// silently leave the liveness probe running at some other rate than the one
// its own comment claims.
constexpr int kSupervisePassesPerProbe =
    static_cast<int>(kSuperviseInterval / kDetectionPollInterval);

// The rate window, and how often it is checked when no frame is checking it.
// Half a second is long enough that the number is steady to read and short
// enough that a display which stops says so before an operator has finished
// noticing. The tick is half the window, so a window that no frame closes is
// closed within one and a half of them.
constexpr qint64 kRateWindowMs = 500;
constexpr int kRateTickMs = 250;

// A Rational times a plain multiplier, in hertz, with the multiply done
// before the divide. See EngineLink::frequencyAtFraction for why that order
// is the whole point.
//
// A zero denominator answers zero, which is what rpc::Rational::hertz()
// already decided a zero denominator means. A second policy here would make
// the same wire value mean two things depending on which one was called.
[[nodiscard]] double scaled_hertz(const rpc::Rational& value, double multiplier)
{
    return value.denominator == 0
               ? 0.0
               : multiplier * static_cast<double>(value.numerator) /
                     static_cast<double>(value.denominator);
}

}  // namespace

EngineLink::EngineLink(QObject* parent) : QObject(parent)
{
    // WHAT THE OPERATOR LEFT SET LAST TIME.
    //
    // Read in the constructor, which runs on the Qt thread before start()
    // makes the supervisor, so the supervisor's first pass already sees
    // these rather than picking them up a pass later. Nothing here can
    // reach the engine and nothing here emits: no QML has connected yet,
    // and the initial values are what bindings read when they first
    // evaluate.
    //
    // Clamped on the way in, on the same argument setConfidenceBar makes
    // for clamping on the way out: a settings file is editable, and a bar
    // of 2 read straight into the atomic would refuse every poll for the
    // whole session with nothing on screen to explain it.
    const QSettings store;
    confidence_bar_.store(
        std::clamp(store.value(settings::kConfidenceBar, 0.0).toDouble(), 0.0,
                   kMaxConfidenceBar),
        std::memory_order_release);

    // The listen switch, which is a switch and not a state: it survives a
    // retune, a mode change and a reconnect within a session, and this is
    // that same fact across a restart. It is safe this early because it
    // is an atomic and a flag the supervisor has not been started to
    // read yet, and because nothing plays until a receiver exists.
    audio_wanted_.store(store.value(settings::kAudioListen, false).toBool());

    // The bookmark list, which is the one thing read here that is a list rather
    // than a scalar. It reaches nothing and claims nothing at this point: a
    // bookmark does something only when somebody picks it, which is what makes
    // remembering it different from remembering the receiver.
    load_bookmarks();

    // The wheel's clock and flush, one set for the window rather than one per
    // display. Started here so the first reading is a small number rather than
    // whatever the process had been up to, which plan_scroll_tune would read
    // as a settling interval that had long since elapsed.
    scroll_clock_.start();
    scroll_flush_.setSingleShot(true);
    connect(&scroll_flush_, &QTimer::timeout, this, &EngineLink::flush_scroll_tune);
    receiver_scroll_flush_.setSingleShot(true);
    connect(&receiver_scroll_flush_, &QTimer::timeout, this,
            [this] { takeReceiverScroll(0.0); });

    // The decode menu is a function of the pane's receiver and its mode, and
    // both move on these two. update_decode_choices emits only on a change.
    connect(this, &EngineLink::receiverChanged, this, &EngineLink::update_decode_choices);
    connect(this, &EngineLink::receiverStatusChanged, this,
            &EngineLink::update_decode_choices);

    // A receiver asked for on the command line is placed by the first
    // connection whose source can reach it. See setStartupReceiver.
    connect(this, &EngineLink::connectionChanged, this, &EngineLink::place_startup_receiver);

    // The focused strip reads the pane, so every change to the pane is a
    // change to the rack as well.
    connect(this, &EngineLink::receiverChanged, this, &EngineLink::rackChanged);
    connect(this, &EngineLink::receiverStatusChanged, this, &EngineLink::rackChanged);

    // Unity until a strip says otherwise, so the first receiver plays at
    // the level it always has.
    for (std::atomic<float>& gain : mix_gain_) {
        gain.store(1.0F, std::memory_order_relaxed);
    }

    // What a double click's second press measures its first against.
    click_clock_.start();
}

EngineLink::~EngineLink()
{
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        stopping_ = true;
    }
    supervisor_wake_.notify_all();

    // Joining is what makes the rest of this destructor safe, and there is
    // no shortcut. The supervisor owns the Client, the Client owns the event
    // loop thread, and the frame callback captures this and touches
    // staging_, swap_mutex_ and the pending counters. Only when the
    // supervisor has returned is every one of those finished with, because
    // destroying the Client is what joins the loop thread. A queued wake
    // still sitting in Qt's event queue is discarded when a QObject is
    // destroyed, so that one needs nothing here.
    if (supervisor_.joinable()) {
        supervisor_.join();
    }
}

void EngineLink::start(const QString& address, std::uint16_t port, std::uint32_t every_nth)
{
    address_ = address;
    port_ = port;
    // Zero and one both mean every frame, per client.h, and on_frame divides
    // by this. Normalised once here rather than guarded at every use.
    requested_every_nth_ = every_nth == 0 ? 1 : every_nth;
    endpoint_ = QStringLiteral("%1:%2").arg(address).arg(port);

    // Started before the supervisor, so there is no window in which a frame
    // can arrive with no clock behind it. It runs for the life of the
    // object rather than only while connected: report_rate costs two loads
    // and a compare when there is no open window, and a timer started and
    // stopped from the connection state is a second piece of state that can
    // disagree with the first.
    rate_tick_.setInterval(kRateTickMs);
    connect(&rate_tick_, &QTimer::timeout, this, &EngineLink::report_rate);
    rate_tick_.start();

    // Written before the thread exists, so the thread's construction is the
    // synchronisation and none of the three needs a lock.
    supervisor_ = std::thread([this] { supervise(); });
}

void EngineLink::supervise()
{
    int pass = 0;
    for (;;) {
        const bool probe = (pass % kSupervisePassesPerProbe) == 0;
        ++pass;

        if (!probe && client_ != nullptr) {
            // A detection-only pass. The connection was good a quarter of a
            // second ago and a failed detections call will find out for us
            // anyway, so this does not need its own liveness question.
            //
            // The front end goes first, because a retune moves the source
            // centre and every receiver's absolute frequency with it. A
            // receiver applied before it would be placed against the
            // previous centre and moved a pass later, which on a drag is a
            // filter that jumps after the radio has already landed.
            apply_source_request();
            apply_source_tune();

            // Then the receiver work. A drag posts a request and wakes this
            // loop immediately rather than waiting out the poll interval,
            // so this is the path a moving filter edge takes and the status
            // poll below is what reads the grant back.
            apply_receiver_request();

            // After the receiver work and before the status poll. A
            // rebuild issues a new receiver id, and reconciling against
            // the old one would leave the audio on a receiver that no
            // longer exists until the next pass.
            apply_audio_request();
            apply_decode_request();
            poll_receiver_status();
            poll_held_status();
            poll_detections();

            // Only when the switch or the region has just moved. The
            // ordinary RDS poll is on the probe pass once a second, for
            // the reason poll_rds gives, but the first answer after a
            // click has to arrive now: a second of nothing reads as the
            // switch not working.
            bool rds_due = false;
            {
                const std::lock_guard<std::mutex> flag(supervisor_mutex_);
                rds_due = rds_work_pending_;
            }
            if (rds_due) {
                poll_rds();
            }

            std::unique_lock<std::mutex> lock(supervisor_mutex_);
            supervisor_wake_.wait_for(lock, kDetectionPollInterval, [this] {
                return stopping_ || ((receiver_work_pending_ || audio_work_pending_ ||
                                      decode_work_pending_ ||
                                      tune_work_pending_ || rds_work_pending_ ||
                                      source_work_pending_) &&
                                     client_ != nullptr);
            });
            if (stopping_) {
                break;
            }
            continue;
        }

        // A probe pass, so the retry runs at kSuperviseInterval as that
        // constant's own comment says. See the retraction there: without the
        // probe test this ran at kDetectionPollInterval, four times a second,
        // for as long as there was nothing to connect to.
        //
        // retry_after_ is the credential backoff and is the supervisor
        // thread's own, written and read here and in attempt_connect and
        // nowhere else, so it needs no lock. A default-constructed time_point
        // is the epoch, which is always in the past, so the first attempt is
        // not delayed.
        if (client_ == nullptr) {
            if (probe && std::chrono::steady_clock::now() >= retry_after_) {
                static_cast<void>(attempt_connect());
            }
        } else if (auto alive = client_->running(); !alive) {
            // The engine went away. core/rpc/client.cpp keeps the event loop
            // running through a lost connection and fails every later call,
            // so this is the only place that finds out, and the Client has
            // to be destroyed before another can be made: its loop thread is
            // joined by that destructor and nothing else.
            // The receiver goes with it. Nothing has to be removed from an
            // engine that is gone, but the pane's own id and passband have
            // to stop claiming to be about a live receiver, and the next
            // engine will not have one at that id.
            live_receiver_id_ = 0;
            live_pane_key_ = 0;

            // The held receivers went with it too, and the Qt thread puts
            // them back on the next connection the way it puts back the
            // pane's; see adopt().
            forget_held(QString{});

            // The audio subscriptions go the same way, and WITHOUT an
            // unsubscribe: there is no engine to cancel against. See
            // forget_audio.
            forget_audio();

            // Every decoder subscription went with it, on the same terms.
            forget_decoded();

            // The decoder went with the engine, and the station on screen
            // was one engine's reading of one receiver. Nothing has to be
            // torn down on an engine that is gone; what this clears is the
            // claim in the window, which would otherwise sit there naming
            // a call sign as though it were still being received.
            clear_rds(QStringLiteral(
                "the engine went away, so nothing is decoding RDS. The switch stays on "
                "and the decoder is rebuilt on the next connection."));

            client_->unsubscribe_spectrum();
            client_.reset();

            // Cleared before the connection state is published, so a
            // refusal the operator has not fixed does not sit on screen
            // beside "disconnected" claiming the engine said something.
            // detectionFault is only ever about an engine that is there,
            // and clear_ rather than note_ because a held threshold
            // refusal has to go with it or the next engine inherits it.
            clear_detection_fault();
            publish(false, QString::fromStdString(alive.error().message));
        } else {
            // The call came back, so the connection is good, and the bool it
            // came back with is the other half of the answer. An engine
            // binds its port before it runs its graph and goes on answering
            // after the source ends, so false here is an engine that is
            // there and producing nothing. This branch used to be absent and
            // the bool discarded, which made those two engines identical to
            // the window: connected, geometry on screen, and a frame rate
            // frozen at whatever it last was.
            note_running(*alive);
            apply_source_request();
            apply_source_tune();

            // After the source requests and before the receiver's, because a
            // gain change is a front-end setting: an open posted in the same
            // gesture has to land first or the gain goes at the source that is
            // about to be closed.
            apply_source_gain();

            apply_receiver_request();

            // After the receiver's own request, so a release asked for in the
            // same gesture as a tune does not take the receiver that tune just
            // created.
            apply_stranded_release();

            apply_audio_request();
            apply_decode_request();
            poll_receiver_status();
            poll_held_status();

            // On the probe pass only, so once a second. These are a
            // status line and nothing acts on them, so polling them at
            // the detection rate would buy three extra round trips a
            // second and no information.
            poll_audio_stats();
            poll_detections();
            poll_rds();

            // An inventory changes when somebody opens or closes a window,
            // which is not four times a second.
            poll_receiver_inventory();

            // Last, and on the probe pass only. Two more calls that exist
            // for measured fields alone; see poll_source_pacing and
            // poll_front_end.
            //
            // poll_front_end comes after poll_detections on purpose: the
            // engine builds its detector on the first request for
            // detections and the front end's verdict is computed from that
            // detector's own arrays, so asking first on a fresh connection
            // would always read Unmeasured.
            poll_source_pacing(*alive);
            poll_front_end(*alive);
        }

        std::unique_lock<std::mutex> lock(supervisor_mutex_);

        // client_ is in the predicate, not just the flag. A request posted
        // while the link is down cannot be applied, and a predicate that
        // ignored that would return true immediately on every pass and turn
        // the reconnect loop into a spin: the wait would never sleep, and
        // attempt_connect would run as fast as the socket could refuse.
        // client_ is the supervisor's own and is read here on its own
        // thread.
        supervisor_wake_.wait_for(lock, kDetectionPollInterval, [this] {
            return stopping_ || ((receiver_work_pending_ || audio_work_pending_ ||
                                  decode_work_pending_ ||
                                  tune_work_pending_ || rds_work_pending_ ||
                                  source_work_pending_) &&
                                 client_ != nullptr);
        });
        if (stopping_) {
            break;
        }
    }

    if (client_ != nullptr) {
        // drop_receiver stops the audio first, so the tail here needs no
        // separate call. See receiver_link.cpp.
        drop_receiver();
        client_->unsubscribe_spectrum();
        client_.reset();
    }
}

namespace {

// The engine's token, in the order an operator would expect it to be found.
//
// REVENANT_RPC_TOKEN holds the hex and wins, which is what a launcher or a
// test harness sets. REVENANT_RPC_TOKEN_FILE names a file, which is what the
// engine's own --token-file produces when it runs as a service and its local
// app data is not somewhere this process can read. Neither set means the
// default path, which is the ordinary case: one operator with the engine and
// this window running as the same account, and nothing to configure.
//
// The precedent for reading configuration out of the environment here is
// REVENANT_GPU_INDEX at core/gpu/context.cpp.
[[nodiscard]] Expected<rpc::Token> resolve_token()
{
    if (const char* hex = std::getenv("REVENANT_RPC_TOKEN"); hex != nullptr) {
        return rpc::token_from_hex(hex);
    }
    if (const char* named = std::getenv("REVENANT_RPC_TOKEN_FILE"); named != nullptr) {
        return rpc::load_token(named);
    }
    auto path = rpc::default_token_path();
    if (!path) {
        return std::unexpected(path.error());
    }
    return rpc::load_token(*path);
}

// The line the window shows for a connection that did not happen.
//
// AN ABSENT ENGINE IS NOT AN ERROR AND SHOULD NOT READ AS ONE. It is the
// ordinary state at both ends of an engine's life, and EngineLink::start's own
// header says the window's job is to come up, say so and keep trying. What it
// said instead was whatever kj put in the exception: "connecting to
// 127.0.0.1:17690: the connection to the engine is gone: connect(): connection
// refused", which describes a connection that never existed as one that was
// lost, and hands an operator a syscall name to worry about.
//
// Every other category keeps the engine's own words, because for those the
// detail is the actionable part. A refused credential in particular names the
// file to fix, and shortening it would take that away.
[[nodiscard]] QString describe_failure(const Error& failure, const QString& endpoint)
{
    if (failure.category == ErrorCategory::Unreachable) {
        return QStringLiteral("waiting for an engine at %1").arg(endpoint);
    }
    return QString::fromStdString(failure.message);
}

}  // namespace

// WHAT THIS FUNCTION USED TO SAY ABOUT ITSELF, UNDER THE HEADING "A REFUSED
// TOKEN IS PERMANENT AND THIS LOOP RETRIES IT ANYWAY":
//
//   "The supervisor above treats every failure as transient, so a wrong token
//   writes the same line once a second forever rather than stopping.
//   core/error.h carries a message and an originating API code with no
//   category, so there is nothing to branch on here except the message text,
//   which would break the first time the wording improved. Widening Error is
//   the right fix and it touches every user of Expected in the tree; it is not
//   on this branch, and this comment is here so the next person to see the loop
//   spin knows it is known."
//
// Error carries a category as of 2026-09-21 and this is what branches on it.
// Two clauses of that paragraph were also wrong about the loop rather than
// about the fix: the retry ran four times a second and not once, for the reason
// kSuperviseInterval now records, and the failure it describes is not only a
// wrong token but any credential this process cannot present, a token file with
// a stray character in it among them.
//
// THE BACKOFF IS THE WHOLE OF THE RESPONSE AND IT IS NOT A STOP. See
// kRefusedCredentialInterval: the token is resolved per attempt so that fixing
// it is picked up, and that property is worth more than the few sockets a ten
// second retry costs.
void EngineLink::hold_off(const Error& failure)
{
    if (failure.category == ErrorCategory::Unauthenticated) {
        retry_after_ = std::chrono::steady_clock::now() + kRefusedCredentialInterval;
        return;
    }
    retry_after_ = {};
}

bool EngineLink::attempt_connect()
{
    // Resolved per attempt rather than once, so an operator who minted a
    // token after starting this window gets picked up by the next pass of the
    // supervisor loop instead of having to restart it.
    auto token = resolve_token();
    if (!token) {
        hold_off(token.error());
        publish(false, describe_failure(token.error(), endpoint_));
        return false;
    }

    auto client = rpc::Client::connect(address_.toStdString(), port_, *token);
    if (!client) {
        hold_off(client.error());
        publish(false, describe_failure(client.error(), endpoint_));
        return false;
    }
    client_ = std::move(*client);
    retry_after_ = {};

    auto info = client_->info();
    if (!info) {
        const QString message = QString::fromStdString(info.error().message);
        client_.reset();
        publish(false, message);
        return false;
    }

    // Asked here and not left to the supervisor's next pass, so the first
    // thing the window is told about this engine is the truth rather than an
    // assumption held for up to a second. Connecting to an engine that is up
    // and not running is the ordinary case at both ends of its life, so the
    // assumption would be wrong exactly when someone was watching for it.
    auto alive = client_->running();
    if (!alive) {
        const QString message = QString::fromStdString(alive.error().message);
        client_.reset();
        publish(false, message);
        return false;
    }

    // Everything the two engine-side counters are derived from, back to
    // zero. They describe one subscription, and the sum the header block
    // promises is only true within one: a sequence from the engine that just
    // died has nothing to say about the distance to a sequence from the one
    // that replaced it.
    every_nth_ = requested_every_nth_;
    first_sequence_ = 0;
    delivered_ = 0;
    have_span_ = false;

    // The supervisor's own shadow of what it last told the Qt thread about
    // audio. Zeroed here rather than left, because adopt() has just
    // cleared the Qt side for the same reason and a shadow that still held
    // the previous engine's values would suppress the first hand-off on
    // this one: note_audio compares against it and posts nothing when they
    // match.
    work_audio_fault_.clear();
    work_audio_ended_.clear();
    work_audio_stats_ = {};
    posted_audio_vrx_ = 0;
    posted_audio_granted_ = 0;
    posted_audio_fault_.clear();
    posted_audio_ended_.clear();
    posted_audio_stats_ = {};

    // The ended that the event loop thread raised and nobody picked up.
    // It is the only piece of audio state that survived a reconnect,
    // because everything else here is the supervisor's own and this list
    // is written from the Cap'n Proto thread under audio_mutex_. An ended
    // arriving just before the connection dropped would be found by the
    // first apply_audio_request on the NEW engine, which would then refuse
    // to listen to a receiver id the new engine has reissued and put the
    // previous engine's sentence about a receiver that no longer exists
    // onto this connection's status line.
    {
        const std::lock_guard<std::mutex> lock(audio_mutex_);
        audio_ended_.clear();
    }
    audio_ended_vrx_.clear();
    {
        const std::lock_guard<std::mutex> lock(swap_mutex_);
        has_ready_ = false;
        pending_received_ = 0;
        pending_dropped_engine_ = 0;
        pending_dropped_ui_ = 0;
        pending_skipped_ = 0;
    }

    // Published before the subscription exists, so the Qt thread is told
    // about the new engine before the first frame drawn against it arrives.
    // The other order works, because an item reads a frame's own bin count
    // rather than EngineInfo's, but it puts a frame on screen under the
    // previous engine's frequency axis for as long as the queued adopt takes
    // to run, and that axis is a claim about where a signal is.
    {
        const std::lock_guard<std::mutex> lock(state_mutex_);
        handover_connected_ = true;
        handover_running_ = *alive;
        handover_info_ = *info;
        handover_error_.clear();
    }
    QMetaObject::invokeMethod(this, [this] { adopt(); }, Qt::QueuedConnection);

    // Asked once per connection, because it is a property of the source
    // and the source does not change under a live engine. A window that
    // asked per pass would spend a round trip a second on an answer that
    // is the same every time; a window that never asked would have to
    // offer the control and let it refuse, which is what this replaces.
    probe_source_tuning();

    // The pacing measurement belongs to the source this connection found,
    // so the suppression state goes with the connection. Without this a
    // new engine whose first sample happened to match the previous one's
    // would never be published, and the window would keep showing a
    // sentence about a source that is no longer there.
    posted_pacing_ = {};

    // An engine built with no spectrum stage is the default and is what a
    // headless recording runs. Connecting to one is not a failure, so the
    // window comes up, says the spectrum is off, and skips the subscription
    // rather than asking for frames that will never arrive.
    if (!info->spectrum.enabled()) {
        return true;
    }

    const auto status = client_->subscribe_spectrum(
        requested_every_nth_,
        [this](const rpc::SpectrumFrame& frame) { on_frame(frame); });
    if (!status) {
        const QString message = QString::fromStdString(status.error().message);
        client_.reset();
        publish(false, message);
        return false;
    }
    return true;
}

// Only ever called with connected false today: the successful path in
// attempt_connect has an EngineInfo and a running flag to hand over as well,
// so it writes the handover itself. handover_info_ is deliberately left alone
// here, because a link that has just gone away has no geometry and QML gates
// every getter on connected; clearing it would buy nothing and would flicker
// the status line through zeroes when the same engine comes back.
//
// handover_running_ is cleared rather than left, because it is not gated the
// same way: engineRunning is a claim about an engine this link can reach, and
// there is no engine to reach. The clear is unconditional on the strength of
// the sentence above, which the one caller shape keeps true.
void EngineLink::publish(bool connected, QString error)
{
    {
        const std::lock_guard<std::mutex> lock(state_mutex_);
        handover_connected_ = connected;
        handover_running_ = false;
        handover_error_ = std::move(error);
    }
    QMetaObject::invokeMethod(this, [this] { adopt(); }, Qt::QueuedConnection);
}

void EngineLink::note_running(bool running)
{
    {
        const std::lock_guard<std::mutex> lock(state_mutex_);
        if (handover_running_ == running) {
            // The answer to a question asked once a second for the life of
            // the window, and almost always the same answer. Posting a
            // metacall for each would wake the GUI thread every second to
            // tell it nothing.
            return;
        }
        handover_running_ = running;
    }

    // adopt_running and not adopt. adopt emits connectionChanged, which
    // render/waterfall_item.cpp and render/spectrum_item.cpp read as a new
    // engine: the waterfall fills its ring with background and the trace
    // forgets its bin count. An engine pausing is not a new engine, and the
    // history drawn from it is still what that engine said.
    QMetaObject::invokeMethod(this, [this] { adopt_running(); }, Qt::QueuedConnection);
}

void EngineLink::adopt()
{
    const bool was_connected = connected_;
    const bool was_running = engine_running_;
    {
        const std::lock_guard<std::mutex> lock(state_mutex_);
        connected_ = handover_connected_;
        engine_running_ = handover_running_;
        error_text_ = handover_error_;
        if (connected_) {
            info_ = handover_info_;
        }
    }

    if (connected_ && !was_connected) {
        // A fresh subscription counts from zero on the supervisor's side, so
        // the display's copies go with it rather than carrying the previous
        // engine's totals into a new sum.
        frames_received_ = 0;
        frames_dropped_engine_ = 0;
        frames_dropped_ui_ = 0;
        frames_skipped_ = 0;
        frames_drawn_ = 0;
    }

    // THE FRAME GOES WITH THE CONNECTION, ON BOTH EDGES, FOR THE REASON
    // EVERYTHING ELSE IN THIS FUNCTION DOES.
    //
    // The bins in display_ were measured by one engine against one span, and
    // info_ has just been replaced with the next engine's. Left alone they
    // are read back immediately: this function emits connectionChanged and
    // then frameChanged, SpectrumItem::onConnectionChanged clears have_frame_
    // on the first, and takeFrame re-reads frame() on the second, finds the
    // PREVIOUS engine's bins still there and sets have_frame_ again against
    // the NEW engine's axis. Run an engine at 98.1 MHz, stop it, start one at
    // 461 MHz: the trace and the top waterfall row draw 98.1 MHz energy under
    // 461 MHz labels, and it stays until another frame arrives, which on a
    // no-spectrum build or an engine that is up and not running is never.
    //
    // Worse on the way down, where adopt() runs on every failed reconnect:
    // a minute of downtime is about 240 passes, each appending another copy
    // of the dead engine's last row to the waterfall.
    //
    // Cleared unconditionally rather than only on the way up. The empty frame
    // is what makes both takeFrame slots hit their early return, and that is
    // the whole mechanism; the keep-the-last-trace behaviour on a disconnect
    // is unaffected, because SpectrumItem draws from its own columns_ and
    // WaterfallItem from its own ring.
    display_ = {};

    // Zeroed on both edges rather than only on the way down. A rate is a
    // statement about one connection, and the connection that just ended and
    // the one that just began have each drawn nothing under it. Leaving it
    // on the way up worked only because the disconnect before it had zeroed
    // it, which is a fact about the other branch and not about this one.
    frame_rate_ = 0.0;
    rate_timer_.invalidate();
    rate_mark_ = frames_drawn_;

    // Detections go with the connection, on both edges and for the same
    // reason the counters do. They are absolute radio frequencies measured
    // by one engine's detector, and the next engine may be tuned somewhere
    // else entirely, so a box left on screen across a reconnect would sit
    // over a frequency that was never scanned. Ids restart at zero as well,
    // so a held selection would silently become a different signal.
    {
        const std::lock_guard<std::mutex> lock(detection_mutex_);
        pending_detections_ = {};
        has_pending_detections_ = false;
    }
    shown_ = {};
    emit detectionsChanged();

    // The receiver goes with the connection, on both edges. Its id was
    // issued by one engine and the next engine issues from one again, so a
    // pane still claiming that id would be pointed at whichever receiver
    // the new engine happened to create first. The REQUEST is kept: the
    // frequency, mode and edges the operator set are theirs and a
    // reconnection should put them back, which is what the tune below does.
    {
        const std::lock_guard<std::mutex> lock(receiver_mutex_);
        has_pending_receiver_status_ = false;
        pending_receiver_ids_.clear();
        handover_held_.clear();
    }

    // The audio goes with the connection on both edges, for the reason
    // the receiver does: the subscription was on an id one engine issued.
    // The SWITCH is kept, the same way the receiver's request is, so an
    // operator who was listening is listening again once the receiver is
    // back. The reconcile in apply_audio_request is what puts it back, and
    // it needs no help here.
    //
    // audio_ended_reason_ is cleared rather than left. It names a receiver
    // on an engine that is gone, and leaving it would sit beside
    // "disconnected" claiming the current engine said something.
    {
        const std::lock_guard<std::mutex> lock(audio_mutex_);
        has_audio_handover_ = false;
    }
    audio_vrx_ = 0;
    audio_granted_millis_ = 0;
    audio_fault_.clear();
    audio_ended_reason_.clear();
    audio_stats_ = {};
    emit audioChanged();

    // WHETHER THE PANE HAD A RECEIVER, CARRIED ACROSS THE TWO adopt() CALLS
    // A RECONNECTION MAKES RATHER THAN RECOMPUTED INSIDE ONE.
    //
    // Not "wanted_.center is non-zero": a receiver tuned exactly to the
    // source's own centre is an ordinary thing to want and would never have
    // been restored.
    //
    // WHAT THIS USED TO BE. A local `const bool had_receiver = receiver_id_
    // != 0;` read a few lines above the clear below and tested against
    // `connected_ && !was_connected` in the same pass. No single pass can
    // meet that. A reconnection is two adopts: on the way down the id is
    // there and connected_ is false, and on the way up connected_ is true
    // and the id was zeroed by the pass before. So the restore below never
    // ran once, an engine restart silently lost the operator's receiver and
    // the audio that follows it, and the empty pane read as the engine's
    // fault. Three comments in this function assured the reader it recovered.
    //
    // So the fact is recorded where it is known and read where it is needed.
    // Set and never cleared while the link is down, because adopt() runs
    // again on every failed reconnect and every later pass finds the id
    // already zero.
    //
    // A rack entry with an add still in flight counts, which is what the
    // pane's key says when its id has not come back yet.
    if (receiver_id_ != 0 || pane_key_ != 0) {
        restore_receiver_ = true;
    }
    receiver_id_ = 0;
    receiver_status_ = {};
    receiver_edge_limit_ = 0;
    static_cast<void>(reset_passband_display());
    emit receiverStatusChanged();
    emit passbandChanged();

    // Every rack entry's id was issued by the engine that has gone, on the
    // same argument as the pane's.
    rack_.forget_engine_ids();

    if (connected_ && !was_connected && restore_receiver_) {
        // The pane had a receiver before the engine went away, so put it
        // back rather than making the operator retune by hand. Recreated
        // rather than retuned, because there is nothing on the new engine
        // to retune.
        //
        // Cleared here and only here: this is the pass that acts on it, and
        // leaving it set would recreate a receiver the operator removed
        // after the reconnection on whatever reconnection came next.
        restore_receiver_ = false;
        if (!ensure_pane_entry()) {
            emit receiverChanged();
        } else {
            post_receiver_request(true);
        }
    } else {
        emit receiverChanged();
    }

    // And the rack's held receivers, on the same terms and for the same
    // reason. Each goes back at the absolute frequency it was on, against
    // the new engine's centre.
    if (connected_ && !was_connected) {
        for (const RackEntry& entry : rack_.entries()) {
            if (entry.key == pane_key_) {
                continue;
            }
            if (const HeldView* view = held_view(entry.key); view != nullptr) {
                add_held_receiver(entry.key, static_cast<double>(view->absolute_hz),
                                  view->params);
            }
        }
    }
    emit rackChanged();

    emit connectionChanged();
    if (engine_running_ != was_running) {
        emit runningChanged();
    }
    emit frameChanged();
    emit rateChanged();
}

void EngineLink::poll_detections()
{
    // Supervisor thread. Client::detections blocks for a round trip, and
    // core/rpc/client.h forbids re-entering the Client from the frame
    // callback, so neither the Qt thread nor the Cap'n Proto loop thread
    // can do this.

    // The engine-side write first, so a threshold the operator moved is in
    // force before the list that reports it back is fetched. The other
    // order would show the old threshold for one poll and read as the
    // control having been ignored.
    if (threshold_pending_.exchange(false, std::memory_order_acq_rel)) {
        const double wanted = requested_threshold_db_.load(std::memory_order_acquire);

        // Cleared before the attempt rather than after it, so whichever way
        // this write goes its own verdict is the only thing left behind. A
        // write the engine takes is what answers the refusal before it.
        threshold_fault_.clear();

        // A refusal is deliberately not routed into errorText, because that
        // field is the connection's and setting it would make a rejected
        // slider value read as a lost engine. It used to be discarded
        // entirely, on the reasoning that detectionThresholdDb reads back
        // the value in force so the control snaps to what the engine has.
        // That is true and it is not the whole answer: snapping back says
        // the write was refused and never says which bound was missed, and
        // the engine's own message does. So it goes to detectionFault,
        // which exists for exactly this, rather than nowhere.
        //
        // Held in a member rather than published straight out, because
        // nothing repeats this call. A refused poll is renewed every pass
        // and clears itself when the poll works; this happens once, and
        // published the same way it was overwritten with an empty string by
        // the very next poll, which succeeds because a rejected write
        // changed nothing. One poll interval on screen is not a fault an
        // operator can read. It stands until another write answers it.
        if (auto applied = client_->set_detection_threshold(wanted); !applied) {
            threshold_fault_ = QString::fromStdString(applied.error().message);
        }
    }

    auto listed = client_->detections(confidence_bar_.load(std::memory_order_acquire),
                                      margin_bar_.load(std::memory_order_acquire));
    if (!listed) {
        // Two unrelated failures arrive here as the same Expected, and only
        // one of them is anything an operator can act on. Ask which.
        //
        // running() answering at all means the connection is up, so the
        // refusal was about the request: a bar or a threshold outside what
        // the engine accepts, or an engine with no spectrum stage, which
        // cannot build a detector and refuses every poll for as long as it
        // runs. That is reported, in the engine's own words, because the
        // message names the bound that was missed.
        //
        // running() failing too means the engine went away. That belongs to
        // the liveness probe, which is where the client is torn down, and
        // is how a dead engine is normally found here, since this runs four
        // times for every probe. Nothing is torn down from this branch and
        // the fault is cleared, because connected and errorText are about
        // to say the same thing better.
        //
        // One extra round trip, on the failure path only. The ordinary pass
        // costs what it always did.
        if (!client_->running().has_value()) {
            clear_detection_fault();
            return;
        }

        // Outranks a threshold write that is still unanswered, which stays
        // held and comes back when the poll does. A frozen list is the
        // worse news and the row has one line.
        note_detection_fault(QString::fromStdString(listed.error().message));
        return;
    }

    // The poll was accepted, so the only thing left to report is a
    // threshold write the engine would not take. Empty when there is none.
    note_detection_fault(threshold_fault_);

    {
        const std::lock_guard<std::mutex> lock(detection_mutex_);
        pending_detections_ = std::move(*listed);
        has_pending_detections_ = true;
    }
    QMetaObject::invokeMethod(this, [this] { adopt_detections(); }, Qt::QueuedConnection);
}

void EngineLink::adopt_detections()
{
    rpc::DetectionList taken;
    {
        const std::lock_guard<std::mutex> lock(detection_mutex_);
        if (!has_pending_detections_) {
            return;
        }
        taken = std::move(pending_detections_);
        has_pending_detections_ = false;
    }

    // Emit only on a real change. An overlay repainting four times a second
    // over a band where nothing is happening is the cost this avoids, and
    // last_decision is the engine's own statement that it has decided
    // again. The threshold and the row count are compared as well because
    // the operator can move the bar without the engine deciding anything.
    const bool decided = taken.last_decision != shown_.last_decision;
    const bool resized = taken.detections.size() != shown_.detections.size() ||
                         taken.total != shown_.total;
    const bool retuned = taken.detection_threshold_db != shown_.detection_threshold_db;

    shown_ = std::move(taken);
    if (decided || resized || retuned) {
        emit detectionsChanged();
    }
}

void EngineLink::note_detection_fault(QString fault)
{
    // Supervisor thread, four times a second for the life of the window.
    // posted_fault_ belongs to this thread alone, which is why the ordinary
    // pass, where the fault is empty and was empty last time, takes no lock
    // and queues nothing.
    if (fault == posted_fault_) {
        return;
    }
    posted_fault_ = fault;

    {
        const std::lock_guard<std::mutex> lock(detection_mutex_);
        pending_fault_ = std::move(fault);
        has_pending_fault_ = true;
    }
    QMetaObject::invokeMethod(this, [this] { adopt_detection_fault(); }, Qt::QueuedConnection);
}

void EngineLink::clear_detection_fault()
{
    threshold_fault_.clear();
    note_detection_fault(QString());
}

void EngineLink::adopt_detection_fault()
{
    QString taken;
    {
        const std::lock_guard<std::mutex> lock(detection_mutex_);
        if (!has_pending_fault_) {
            return;
        }
        taken = std::move(pending_fault_);
        has_pending_fault_ = false;
    }

    // Compared again on this side. The supervisor suppresses a repeat of
    // what it last posted, and two posts can still collapse onto one value
    // here if the fault changed and changed back between them.
    if (taken == detection_fault_) {
        return;
    }
    detection_fault_ = std::move(taken);
    emit detectionFaultChanged();
}

void EngineLink::setConfidenceBar(double bar)
{
    // Clamped rather than refused. This is a slider, and a value the engine
    // will not take would otherwise be refused on every later poll with
    // nothing on screen to say why.
    //
    // The top of the range is kMaxConfidenceBar and NOT one. This clamp was
    // [0, 1] inclusive until 2026-09-19 while core/rpc/server.cpp refused a
    // bar of exactly one, so writing 1.0 to this property made every
    // subsequent detections call fail. poll_detections swallows a failed
    // poll on purpose, because that is how a dead engine is normally found,
    // so the overlay silently stopped updating. See kMaxConfidenceBar for
    // the engine's reasoning about one.
    //
    // This clamp is not redundant with the slider's range, which is the
    // reading it invites. A Slider whose `to` is bound to maxConfidenceBar
    // keeps a `to` of exactly one, because QQuickSlider::setTo drops an
    // assignment within 1e-12 of the value it holds; measured on Qt 6.8.3
    // and written up on kMaxConfidenceBar.
    //
    // The slider is no longer what this catches, though. ui/qml/DetectionControls.qml's
    // `bar` expression pins a handle at or past the stop to
    // maxConfidenceBar before writing the property, so the drag path arrives
    // here already inside the range and the clamp is a no-op on it. What is
    // left for the clamp is every other writer: a QML binding somewhere
    // else, a test, a future settings restore.
    //
    // WHAT THIS PARAGRAPH USED TO SAY
    //
    // Until 2026-09-20 it ended "So one arrives here on every drag to full
    // travel and this line is what turns it into a bar the engine accepts."
    // That was written after the QML pin had already landed in the same
    // branch, so it described a path that no longer existed and made this
    // line look load-bearing for a case it never sees.
    //
    // NaN is dealt with before the clamp because it compares false against
    // both bounds and would pass straight through, which is the same defect
    // one step further out: the engine refuses a non-finite bar too. It
    // goes to the floor, which is this property's own default and passes
    // everything, because there is no slider position it could have meant.
    const double wanted = std::isnan(bar) ? 0.0 : bar;
    const double clamped = std::clamp(wanted, 0.0, kMaxConfidenceBar);
    if (clamped == confidence_bar_.load(std::memory_order_acquire)) {
        return;
    }
    confidence_bar_.store(clamped, std::memory_order_release);

    // THE CLAMPED VALUE AND NOT THE ARGUMENT. This property is remembered
    // across launches, and what is stored has to be what the link is
    // actually using: storing the request would let a bar the engine
    // refuses survive a restart and refuse every poll of the next
    // session, with the slider apparently in a legal place.
    QSettings().setValue(settings::kConfidenceBar, clamped);

    // No signal here. The bar changes what the next poll asks for, and the
    // poll emits detectionsChanged when the answer differs. Emitting now
    // would tell the overlay to redraw a list fetched at the old bar.
}

void EngineLink::setMarginBar(double bar)
{
    // Every argument setConfidenceBar makes above applies here unchanged: the
    // clamp is against the same top of range because the margin map also
    // approaches one without arriving and the engine refuses a bar of one the
    // same way, NaN goes to the floor because it would pass both bounds and
    // there is no slider position it could have meant, and nothing is emitted
    // because the bar changes what the NEXT poll asks for.
    //
    // NOT REMEMBERED ACROSS LAUNCHES, which is the one difference and is
    // deliberate. A confidence bar left high hides tracks that have not been
    // up long, which an operator notices within seconds. A margin bar left
    // high hides weak signals, which looks exactly like a quiet band, and a
    // window that came up filtering them out would be making a claim about
    // the band it had not looked at. That is the same argument
    // models/settings.h makes for not restoring the receiver.
    const double wanted = std::isnan(bar) ? 0.0 : bar;
    const double clamped = std::clamp(wanted, 0.0, kMaxConfidenceBar);
    margin_bar_.store(clamped, std::memory_order_release);
}

void EngineLink::setDetectionThresholdDb(double threshold_db)
{
    requested_threshold_db_.store(threshold_db, std::memory_order_release);
    threshold_pending_.store(true, std::memory_order_release);

    // Also no signal. detectionThresholdDb reads the value in force, which
    // is not this one until the supervisor has sent it and the engine has
    // answered, and reporting it early is the lie this property exists to
    // avoid.
}

void EngineLink::adopt_running()
{
    const bool was_running = engine_running_;
    {
        const std::lock_guard<std::mutex> lock(state_mutex_);
        engine_running_ = handover_running_;
    }

    // note_running only posts on a change, but two of them can coalesce
    // behind an adopt() that already took the newer value, so the compare is
    // here as well and not only there.
    if (engine_running_ != was_running) {
        emit runningChanged();
    }
}

QString EngineLink::deviceName() const
{
    return QString::fromStdString(info_.device.name);
}

QString EngineLink::deviceVendor() const
{
    return QString::fromStdString(info_.device.vendor);
}

QString EngineLink::clampReason() const
{
    return QString::fromStdString(info_.ring_clamp_reason);
}

double EngineLink::frequencyAtFraction(double fraction) const
{
    // Half a bin below the first bin's centre at fraction zero, half a bin
    // above the last bin's centre at fraction one: the outer edges, which is
    // where a display's own edges are and what tools/cli/main.cpp's axis
    // uses, so the same capture read in both places reports the same band.
    const double bin = fraction * static_cast<double>(info_.spectrum.bins) - 0.5;
    return static_cast<double>(info_.source_center) +
           scaled_hertz(info_.spectrum.bin_zero, 1.0) +
           scaled_hertz(info_.spectrum.bin_width, bin);
}

void EngineLink::on_frame(const rpc::SpectrumFrame& frame)
{
    // One copy, and it is unavoidable: the argument is the Client's and is
    // valid only for this call. Assigning into a vector this thread owns
    // reuses its capacity, so the copy is a memcpy after the first frame and
    // allocates nothing on the event loop thread.
    staging_ = frame;

    if (!have_span_) {
        first_sequence_ = frame.sequence;
        have_span_ = true;
    }
    ++delivered_;

    // sequence is the ENGINE's frame counter, not this subscription's, so
    // the distance from the first frame that arrived is exactly how many
    // frames the engine produced across the window this subscription has
    // watched. Both engine-side counters fall out of that one number.
    //
    // core/rpc/server.cpp offers a subscription only the frames whose
    // sequence is a multiple of its every_nth, so one in every_nth of the
    // window was ever a candidate and the rest were never copied. Of the
    // candidates, the ones that did not arrive are the ones the engine threw
    // away because the previous frame had not been answered yet.
    //
    // The division is exact and not a rounding: every sequence that reaches
    // this callback is a multiple of every_nth, so the distance between any
    // two of them is a multiple of it as well. These are counts of whole
    // frames throughout.
    const std::uint64_t stride = every_nth_;
    const std::uint64_t span =
        frame.sequence >= first_sequence_ ? frame.sequence - first_sequence_ : 0;
    const std::uint64_t offered = span / stride + 1;
    const std::uint64_t skipped = span - span / stride;
    const std::uint64_t dropped_by_engine = offered > delivered_ ? offered - delivered_ : 0;

    {
        const std::lock_guard<std::mutex> lock(swap_mutex_);
        if (has_ready_) {
            // The latest-wins replacement this file's header describes. The
            // frame being displaced has already been counted as received and
            // will never be drawn, and this is the only place that knows it
            // happened.
            ++pending_dropped_ui_;
        }
        std::swap(staging_, ready_);
        has_ready_ = true;
        pending_received_ = delivered_;
        pending_dropped_engine_ = dropped_by_engine;
        pending_skipped_ = skipped;
    }

    if (wake_pending_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    // The queued connection client.h asks for. A functor with this as the
    // context object is posted to the thread this object lives on, which is
    // the Qt thread; nothing about the QObject is read or written here
    // beyond what posting an event to it requires.
    QMetaObject::invokeMethod(this, [this] { drain(); }, Qt::QueuedConnection);
}

void EngineLink::drain()
{
    // Cleared before the buffer is taken, not after. A frame landing in the
    // window between the clear and the swap raises a fresh wake, which costs
    // one call here that finds nothing. Clearing afterwards would drop that
    // wake and leave the frame sitting until another arrived, which on a
    // quiet band is a display that stops for no visible reason.
    wake_pending_.store(false, std::memory_order_release);

    {
        const std::lock_guard<std::mutex> lock(swap_mutex_);
        if (!has_ready_) {
            return;
        }
        std::swap(ready_, display_);
        has_ready_ = false;

        // Taken under the same lock as the frame, so the numbers on screen
        // describe the frame on screen and add up against each other.
        //
        // Client::frames_received() would answer the first of these and
        // Client::frames_dropped() looks like it would answer the second.
        // Neither is read: the first moves between the swap and this drain,
        // and the second counts re-entrant delivery, which cannot happen.
        // The header block has the whole of it.
        frames_received_ = pending_received_;
        frames_dropped_engine_ = pending_dropped_engine_;
        frames_dropped_ui_ = pending_dropped_ui_;
        frames_skipped_ = pending_skipped_;
    }

    // Reaching here is one frame drawn, because every drain emits
    // frameChanged and every item repaints from it. Counting drains rather
    // than differencing frames_received_ is the whole point: that one moves
    // by more than a frame whenever the swap slot was overwritten, and a
    // rate taken from it reports the arrival rate while calling itself the
    // display's.
    ++frames_drawn_;

    if (!rate_timer_.isValid()) {
        // The first frame of a connection opens the window. Until one
        // arrives there is nothing to measure and report_rate leaves the
        // rate at the zero adopt() set.
        rate_timer_.start();
        rate_mark_ = frames_drawn_;
    } else {
        report_rate();
    }

    emit frameChanged();
}

void EngineLink::report_rate()
{
    if (!rate_timer_.isValid()) {
        return;
    }
    const qint64 elapsed = rate_timer_.elapsed();
    if (elapsed < kRateWindowMs) {
        return;
    }
    const qulonglong drawn = frames_drawn_ - rate_mark_;

    // A window with no frames in it is a stopped display only if a frame was
    // due in it. The test is against the rate already measured rather than
    // against a fixed interval, because every_nth sets the cadence and a
    // caller may ask for one frame in hundreds: at a rate low enough that a
    // window holds less than one frame, an empty window is what healthy
    // looks like, and reporting zero for it would flicker the status line
    // between zero and the real rate. Holding the window open instead makes
    // the test self-tuning, since a stopped slow feed still crosses the bar
    // once the window has run one expected interval.
    //
    // A rate of zero takes the other branch on purpose. It is both the state
    // before the first frame of a connection and the state after a stall,
    // and in each the window has to keep closing: a window left open across
    // a minute of silence would measure the first frame after it at one
    // frame per minute and report the resumed display as stopped.
    const bool frame_was_due =
        frame_rate_ <= 0.0 ||
        frame_rate_ * static_cast<double>(elapsed) / 1000.0 >= 1.0;
    if (drawn == 0 && !frame_was_due) {
        return;
    }

    const double rate =
        static_cast<double>(drawn) * 1000.0 / static_cast<double>(elapsed);
    rate_timer_.restart();
    rate_mark_ = frames_drawn_;

    // Only on a change, because the tick reaches here every window for as
    // long as the window is open and a signal per tick would have every
    // binding on the rate re-evaluate four times a second while the number
    // sat still.
    if (rate == frame_rate_) {
        return;
    }
    frame_rate_ = rate;
    emit rateChanged();
}

}  // namespace revenant::ui
