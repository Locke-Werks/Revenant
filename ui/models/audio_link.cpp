// EngineLink's audio half: the subscription, the two callbacks, and the
// hand-off of everything the status strip shows.
//
// Split out of engine_link.cpp the way receiver_link.cpp is, because these
// are one object's members and three separate concerns, and a single file
// holding all three is where a reader stops being able to find the thread
// rule that applies to the function in front of them.
//
// THE THREE THREADS, FOR THIS FILE SPECIFICALLY
//
//   The SUPERVISOR thread owns the Client, so it is the only thread that
//   may call subscribe_audio, unsubscribe_audio or audio_stats. All three
//   block for a round trip. apply_audio_request, poll_audio_stats and
//   stop_audio are its and only its.
//
//   The CAP'N PROTO EVENT LOOP thread runs on_audio_chunk and
//   on_audio_ended. The first touches the ring and nothing else. The second
//   writes a string under a mutex and raises a flag, because client.h
//   forbids calling back into the Client from there and the teardown is a
//   Client call.
//
//   The QT thread runs setAudioWanted, adopt_audio and every getter. It
//   never touches the Client and never touches the ring's contents; the
//   player drains the ring on the sound card's own thread.
//
// WHY THE SUBSCRIPTION IS RECONCILED RATHER THAN COMMANDED
//
// apply_audio_request compares what is wanted against what is held and
// fixes the difference, on every supervisor pass. Nothing else in this
// client starts or stops a stream. The alternative, a subscribe at each
// site that creates a receiver and an unsubscribe at each site that
// destroys one, has five call sites: tune, mode change, the rebuild that a
// refused width turns into, clear, and reconnect. Two of those five are
// inside error handling in receiver_link.cpp and one of them is reached
// from a drag. A stream left running on a receiver that no longer exists
// is inaudible until the engine sends ended, and a stream never started
// after a rebuild is inaudible full stop.

#include "models/engine_link.h"

#include <string>
#include <utility>

#include <QMetaObject>
#include <QSettings>
#include <QVariant>

#include "models/settings.h"

namespace revenant::ui {
namespace {

// What this client asks the engine to hold for it, in milliseconds. The
// engine clamps to 20..5000 and answers with what it granted, and the
// granted value is what everything on this side is sized from: see
// audio/audio_player.h for the three depths and how they relate.
//
// Two hundred rather than the engine's default of 500. The whole budget
// shows up as latency the moment anything is behind, and this is a radio: a
// push-to-talk over that arrives a fifth of a second late is still live,
// and one arriving half a second late is noticeably not. Two hundred still
// rides out a scheduler stall on the GUI thread, which is the stall that
// actually happens on this machine, and a network slow enough to need more
// than that is one where the operator wants to know rather than to not
// notice.
constexpr std::uint32_t kRequestedAudioMillis = 200;

[[nodiscard]] bool same_stats(const rpc::AudioStats& a, const rpc::AudioStats& b)
{
    return a.frames_sent == b.frames_sent && a.frames_dropped == b.frames_dropped &&
           a.drop_events == b.drop_events && a.backlog_frames == b.backlog_frames &&
           a.buffer_frames == b.buffer_frames;
}

}  // namespace

// ---------------------------------------------------------------------------
// The Qt thread
// ---------------------------------------------------------------------------

void EngineLink::setAudioWanted(bool wanted)
{
    if (audio_wanted_.exchange(wanted) == wanted) {
        return;
    }

    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        audio_work_pending_ = true;
    }

    // Woken rather than left for the poll interval, for the reason
    // post_receiver_request gives: a quarter of a second between clicking
    // listen and hearing anything reads as the control not working.
    supervisor_wake_.notify_all();

    // Remembered, because it is a switch. Written here on the click
    // rather than at shutdown, so the setting an operator changed just
    // before something went wrong is the one that survives it.
    QSettings().setValue(settings::kAudioListen, wanted);

    // Emitted on the write and not on the engine's answer, so the switch
    // moves under the pointer. audioActive is the property that waits for
    // the engine, and the two say different things on purpose.
    emit audioChanged();
}

void EngineLink::adopt_audio()
{
    {
        const std::lock_guard<std::mutex> lock(audio_mutex_);
        if (!has_audio_handover_) {
            return;
        }
        has_audio_handover_ = false;
        audio_vrx_ = handover_audio_vrx_;
        audio_granted_millis_ = handover_audio_granted_;
        audio_fault_ = handover_audio_fault_;
        audio_ended_reason_ = handover_audio_ended_;
        audio_stats_ = handover_audio_stats_;
    }
    emit audioChanged();
}

// ---------------------------------------------------------------------------
// The Cap'n Proto event loop thread
// ---------------------------------------------------------------------------

void EngineLink::on_audio_chunk(const rpc::AudioChunk& chunk)
{
    // The whole of what this thread does with audio. Everything the chunk
    // means, the gap in front of it and the state of the gate inside it,
    // is worked out in AudioRing::write under the ring's own lock. See the
    // block at the top of audio/audio_ring.h for what that lock costs this
    // thread and why it is the trade that was taken.
    audio_ring_.write(chunk);
}

void EngineLink::on_audio_ended(const std::string& reason)
{
    {
        const std::lock_guard<std::mutex> lock(audio_mutex_);
        audio_ended_pending_ = true;
        audio_ended_text_ = reason;
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        audio_work_pending_ = true;
    }

    // The supervisor does the rest. core/rpc/client.h forbids calling back
    // into the Client from this thread, and every part of the teardown,
    // including finding out whether the pane's receiver is still there, is
    // a Client call.
    supervisor_wake_.notify_all();
}

// ---------------------------------------------------------------------------
// The supervisor thread
// ---------------------------------------------------------------------------

void EngineLink::stop_audio()
{
    if (live_audio_vrx_ == 0) {
        return;
    }

    if (client_ != nullptr) {
        // Cancelled explicitly, and this is the line that keeps ended()
        // meaning what audioEndedReason says it means. The schema promises
        // a client is never sent ended for a cancel it asked for, so a
        // receiver removed without this would deliver an ended for a
        // removal this client performed, and the window would announce
        // that the receiver went away every time the operator changed
        // mode. See drop_receiver in receiver_link.cpp, which is the caller
        // that makes it matter.
        //
        // Client::unsubscribe_audio is synchronous on the event loop, so
        // when it returns the chunk callback has been erased and no
        // callback is in flight. That is what makes resetting the ring on
        // the next line safe from this thread.
        client_->unsubscribe_audio(live_audio_vrx_);
    }

    live_audio_vrx_ = 0;
    live_audio_granted_ = 0;
    audio_ring_.reset();
}

void EngineLink::apply_audio_request()
{
    // Cleared BEFORE the work, on the argument apply_receiver_request
    // makes: a write landing in the window between the two raises a fresh
    // wake and costs one pass that finds nothing, where clearing
    // afterwards loses that write entirely.
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        audio_work_pending_ = false;
    }

    // The ended arrival first, because it changes what is held before
    // anything below asks what is held.
    std::string ended;
    bool had_ended = false;
    {
        const std::lock_guard<std::mutex> lock(audio_mutex_);
        if (audio_ended_pending_) {
            audio_ended_pending_ = false;
            ended = std::move(audio_ended_text_);
            audio_ended_text_.clear();
            had_ended = true;
        }
    }

    if (had_ended) {
        // NOT stop_audio. core/rpc/client.cpp has already dropped this
        // side's capability and the server has already ended its own, so
        // there is nothing to cancel and a cancel would be refused. What
        // is left is to stop claiming to hold a subscription.
        live_audio_vrx_ = 0;
        live_audio_granted_ = 0;
        audio_ring_.reset();

        // The switch goes OFF. ended means the receiver went away, so the
        // reconcile below would otherwise resubscribe on the pane's stale
        // id, be refused, and write a second error over the engine's own
        // sentence about why the first one ended. The operator turns it
        // back on when they have a receiver again, which is the same
        // gesture as starting to listen in the first place.
        audio_wanted_.store(false, std::memory_order_release);

        work_audio_ended_ =
            ended.empty()
                ? QStringLiteral("the engine ended the audio stream and gave no reason")
                : QString::fromStdString(ended);
        work_audio_fault_.clear();
        work_audio_stats_ = {};
        note_audio();

        // The Qt thread's own copy of the switch has to follow, and
        // audioWanted reads the atomic directly, so the signal is all that
        // is needed.
        QMetaObject::invokeMethod(
            this, [this] { emit audioChanged(); }, Qt::QueuedConnection);
    }

    // Nothing on a receiver that makes no audio. raw and the digital modes
    // hand out complex baseband, the engine refuses audio on one in words,
    // and that refusal would be the only thing the section had to show; the
    // window hides the section there instead. The switch stays as it was, so
    // a change back to an audio mode starts listening again.
    const bool audible = mode_makes_audio(demod_name(live_receiver_demod_).toStdString());
    const qulonglong want =
        audio_wanted_.load(std::memory_order_acquire) && audible ? live_receiver_id_ : 0;

    if (live_audio_vrx_ != 0 && live_audio_vrx_ != want) {
        // The pane moved to another receiver, or the operator switched
        // off. Either way the stream that is running is on the wrong
        // receiver.
        stop_audio();
        work_audio_stats_ = {};
    }

    if (live_receiver_id_ != 0 && !audible) {
        // A refusal from before the mode changed is about a receiver that
        // is not there any more.
        work_audio_fault_.clear();
    }

    if (want == 0 || live_audio_vrx_ == want || client_ == nullptr) {
        note_audio();
        return;
    }

    // Counters restart with the subscription: they describe one stream and
    // the previous receiver's gaps say nothing about this one's.
    audio_ring_.reset_counts();
    audio_ring_.set_depth_millis(kRequestedAudioMillis);

    auto granted =
        client_->subscribe_audio(want, kRequestedAudioMillis,
                                 [this](const rpc::AudioChunk& chunk) { on_audio_chunk(chunk); },
                                 [this](const std::string& why) { on_audio_ended(why); });
    if (!granted) {
        // The engine refused: no such receiver or a source that is not
        // open, in its own words. A complex tap would be refused too and is
        // never asked; see the gate above. Not routed into errorText, for
        // the reason detectionFault is not: a refused subscription is not
        // a lost engine and must not read as one.
        work_audio_fault_ = QString::fromStdString(granted.error().message);
        note_audio();
        return;
    }

    live_audio_vrx_ = want;
    live_audio_granted_ = *granted;

    // THE RING IS RESIZED FROM THE GRANT AND NOT FROM THE REQUEST, and the
    // reset is what makes that stick. subscribe_audio installs the chunk
    // callback BEFORE it sends the request, so a chunk can reach the ring
    // while this call is still waiting for the answer, and that chunk
    // would have established the ring at the provisional depth above. The
    // reset clears the format, so the next chunk re-establishes at the
    // depth the engine actually granted.
    //
    // It costs whatever arrived during the round trip, which on loopback
    // is nothing and on a link slow enough to matter is a few chunks at
    // the very start of a stream. The alternative is a ring whose depth
    // silently differs from the grant, which is the failure the grant is
    // reported to prevent.
    audio_ring_.set_depth_millis(*granted);
    audio_ring_.reset();

    work_audio_fault_.clear();
    work_audio_ended_.clear();
    work_audio_stats_ = {};
    note_audio();
}

void EngineLink::poll_audio_stats()
{
    if (live_audio_vrx_ == 0 || client_ == nullptr) {
        return;
    }

    auto stats = client_->audio_stats(live_audio_vrx_);
    if (!stats) {
        // Refused once the subscription is over, however it ended, which
        // core/rpc/client.h is explicit about. An ended that has not been
        // picked up yet lands here first, and reporting the refusal would
        // write "this client holds no audio subscription" over the
        // engine's own sentence about why the receiver went away.
        // apply_audio_request says the true thing on the next pass, and
        // this one stays quiet.
        return;
    }

    work_audio_stats_ = *stats;
    note_audio();
}

void EngineLink::note_audio()
{
    if (posted_audio_vrx_ == live_audio_vrx_ &&
        posted_audio_granted_ == live_audio_granted_ &&
        posted_audio_fault_ == work_audio_fault_ &&
        posted_audio_ended_ == work_audio_ended_ &&
        same_stats(posted_audio_stats_, work_audio_stats_)) {
        // Called on every supervisor pass and almost always with nothing
        // to say. Posting anyway would wake the GUI thread four times a
        // second to tell it what it already knows, which is the argument
        // note_running makes about the liveness probe.
        return;
    }

    posted_audio_vrx_ = live_audio_vrx_;
    posted_audio_granted_ = live_audio_granted_;
    posted_audio_fault_ = work_audio_fault_;
    posted_audio_ended_ = work_audio_ended_;
    posted_audio_stats_ = work_audio_stats_;

    {
        const std::lock_guard<std::mutex> lock(audio_mutex_);
        has_audio_handover_ = true;
        handover_audio_vrx_ = live_audio_vrx_;
        handover_audio_granted_ = live_audio_granted_;
        handover_audio_fault_ = work_audio_fault_;
        handover_audio_ended_ = work_audio_ended_;
        handover_audio_stats_ = work_audio_stats_;
    }
    QMetaObject::invokeMethod(this, [this] { adopt_audio(); }, Qt::QueuedConnection);
}

}  // namespace revenant::ui
