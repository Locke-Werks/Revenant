// EngineLink's audio half: the subscriptions, one per heard receiver in the
// rack, the two callbacks, and the hand-off of everything the status strip
// shows.
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
//   stop_audio_for, stop_audio_slot and forget_audio are its and only its.
//
//   The CAP'N PROTO EVENT LOOP thread runs on_audio_chunk and
//   on_audio_ended. The first touches its slot's ring and nothing else. The second
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

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

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

void EngineLink::on_audio_chunk(std::size_t slot, const rpc::AudioChunk& chunk)
{
    // The whole of what this thread does with audio. Everything the chunk
    // means, the gap in front of it and the state of the gate inside it,
    // is worked out in AudioRing::write under the ring's own lock. See the
    // block at the top of audio/audio_ring.h for what that lock costs this
    // thread and why it is the trade that was taken.
    //
    // Timed as it comes off the wire, which is what lets AudioMix put
    // receivers whose indices share no origin on one timeline. See THE
    // ARRIVAL ANCHOR on AudioRing::write.
    const auto arrival = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
    audio_rings_[slot].write(chunk, static_cast<std::int64_t>(arrival.count()));
}

void EngineLink::on_audio_ended(std::size_t slot, qulonglong vrx, const std::string& reason)
{
    {
        const std::lock_guard<std::mutex> lock(audio_mutex_);
        audio_ended_.push_back(AudioEnded{slot, vrx, reason});
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

void EngineLink::stop_audio_slot(std::size_t slot)
{
    const qulonglong vrx = live_audio_[slot].vrx;
    if (vrx == 0) {
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
        client_->unsubscribe_audio(vrx);
    }

    live_audio_[slot] = {};
    audio_rings_[slot].reset();
}

void EngineLink::stop_audio_for(qulonglong vrx)
{
    if (vrx == 0) {
        return;
    }
    for (std::size_t slot = 0; slot < live_audio_.size(); ++slot) {
        if (live_audio_[slot].vrx == vrx) {
            stop_audio_slot(slot);
        }
    }
    if (live_audio_vrx_ == vrx) {
        live_audio_vrx_ = 0;
        live_audio_granted_ = 0;
    }
}

void EngineLink::forget_audio()
{
    // NOT stop_audio_slot. There is no engine to cancel against, and
    // Client::unsubscribe_audio on a dead connection would block the
    // supervisor for a round trip that cannot happen. The rings are emptied
    // so that reconnecting does not play the previous engine's last quarter
    // second before the new streams start.
    for (std::size_t slot = 0; slot < live_audio_.size(); ++slot) {
        live_audio_[slot] = {};
        audio_rings_[slot].reset();
    }
    live_audio_vrx_ = 0;
    live_audio_granted_ = 0;
    work_audio_stats_ = {};
    audio_ended_vrx_.clear();
    {
        const std::lock_guard<std::mutex> lock(audio_mutex_);
        audio_ended_.clear();
    }
    publish_mix();
}

void EngineLink::publish_mix()
{
    std::uint32_t mask = 0;
    std::uint32_t wfm = 0;
    int pane_slot = -1;
    for (std::size_t slot = 0; slot < live_audio_.size(); ++slot) {
        if (live_audio_[slot].vrx == 0) {
            continue;
        }
        mask |= 1U << slot;
        if (live_audio_[slot].demod == rpc::Demod::Wfm) {
            wfm |= 1U << slot;
        }
        if (live_audio_[slot].vrx == live_receiver_id_) {
            pane_slot = static_cast<int>(slot);
        }
    }

    // THE LEAD IS THE FOCUSED RECEIVER'S STREAM WHEN IT IS HEARD, and
    // otherwise the first heard one down the rack. The lead decides the
    // rate the sink is opened at, so it should be the receiver the operator
    // is looking at; a lead that jumped to whichever slot was lowest would
    // reopen the sink on every focus change between receivers at two rates.
    int lead = pane_slot;
    if (lead < 0) {
        std::vector<AudioWant> wants;
        {
            const std::lock_guard<std::mutex> lock(audio_mutex_);
            wants = requested_audio_wants_;
        }
        for (const AudioWant& want : wants) {
            if (want.slot < live_audio_.size() && live_audio_[want.slot].vrx != 0) {
                lead = static_cast<int>(want.slot);
                break;
            }
        }
    }
    if (lead < 0 && mask != 0) {
        for (std::size_t slot = 0; slot < live_audio_.size(); ++slot) {
            if ((mask & (1U << slot)) != 0) {
                lead = static_cast<int>(slot);
                break;
            }
        }
    }

    mix_granted_millis_.store(
        lead < 0 ? 0U : live_audio_[static_cast<std::size_t>(lead)].granted,
        std::memory_order_release);
    // The mode mask before the mask that admits a slot, so the pull thread
    // never sees a slot heard with a stale treatment.
    //
    // WHAT THIS USED TO SAY: "The two mode masks". The other was the level
    // mask, which told the mix's own AGC which slots to level; the engine
    // levels what it sends now, so it went with that AGC.
    mix_wfm_mask_.store(wfm, std::memory_order_release);
    mix_mask_.store(mask, std::memory_order_release);
    mix_lead_slot_.store(lead, std::memory_order_release);
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

    std::vector<AudioEnded> ended;
    std::vector<AudioWant> wants;
    {
        const std::lock_guard<std::mutex> lock(audio_mutex_);
        ended.swap(audio_ended_);
        wants = requested_audio_wants_;
    }

    // The ended arrivals first, because they change what is held before
    // anything below asks what is held.
    for (AudioEnded& one : ended) {
        // NOT stop_audio_slot. core/rpc/client.cpp has already dropped this
        // side's capability and the server has already ended its own, so
        // there is nothing to cancel and a cancel would be refused. What is
        // left is to stop claiming to hold a subscription.
        if (one.slot < live_audio_.size() && live_audio_[one.slot].vrx == one.vrx) {
            live_audio_[one.slot] = {};
            audio_rings_[one.slot].reset();
        }

        // That receiver is not asked again. ended means the receiver went
        // away or its stream failed, and asking again would be refused and
        // write a second sentence over the engine's own.
        audio_ended_vrx_.push_back(one.vrx);

        if (one.vrx == live_receiver_id_) {
            work_audio_ended_ =
                one.reason.empty()
                    ? QStringLiteral("the engine ended the audio stream and gave no reason")
                    : QString::fromStdString(one.reason);
            work_audio_fault_.clear();
            work_audio_stats_ = {};
        }
    }

    // WHAT AN ENDED ARRIVAL USED TO DO: switch listening off, so the
    // reconcile would not resubscribe on the pane's stale id. With a rack
    // that silenced every other receiver for the sake of one, so the
    // receiver whose stream ended is remembered instead and the switch stays
    // where the operator put it.

    // What should be subscribed, by slot. Nothing on a receiver that makes
    // no audio: raw, D-STAR and TETRA hand out complex baseband, the engine
    // refuses audio on one in words, and that refusal would be the only
    // thing the section had to show; the window hides the section there
    // instead. A P25 receiver's audio is its voice since 2026-09-23.
    std::array<qulonglong, kMaxReceivers> desired{};
    std::array<rpc::Demod, kMaxReceivers> desired_demod{};
    if (audio_wanted_.load(std::memory_order_acquire)) {
        for (const AudioWant& want : wants) {
            if (want.slot >= desired.size()) {
                continue;
            }
            rpc::Demod demod = rpc::Demod::Nfm;
            const qulonglong vrx = id_for_key(want.key, &demod);
            if (vrx == 0 || !mode_makes_audio(demod_name(demod).toStdString())) {
                continue;
            }
            if (std::find(audio_ended_vrx_.begin(), audio_ended_vrx_.end(), vrx) !=
                audio_ended_vrx_.end()) {
                continue;
            }
            desired[want.slot] = vrx;
            desired_demod[want.slot] = demod;
        }
    }

    for (std::size_t slot = 0; slot < live_audio_.size(); ++slot) {
        if (live_audio_[slot].vrx != 0 && live_audio_[slot].vrx != desired[slot]) {
            // Muted, soloed away, rebuilt under a new id, moved out of the
            // rack, or the operator switched off. Either way the stream that
            // is running is not one that is wanted.
            stop_audio_slot(slot);
        }
    }

    const bool audible = mode_makes_audio(demod_name(live_receiver_demod_).toStdString());
    if (live_receiver_id_ != 0 && !audible) {
        // A refusal from before the mode changed is about a receiver that
        // is not there any more.
        work_audio_fault_.clear();
    }

    if (client_ != nullptr) {
        for (std::size_t slot = 0; slot < live_audio_.size(); ++slot) {
            const qulonglong want = desired[slot];
            if (want == 0 || live_audio_[slot].vrx == want) {
                continue;
            }

            // Counters restart with the subscription: they describe one
            // stream and the previous receiver's gaps say nothing about
            // this one's.
            AudioRing& ring = audio_rings_[slot];
            ring.reset_counts();
            ring.set_depth_millis(kRequestedAudioMillis);

            auto granted = client_->subscribe_audio(
                want, kRequestedAudioMillis,
                [this, slot](const rpc::AudioChunk& chunk) { on_audio_chunk(slot, chunk); },
                [this, slot, want](const std::string& why) { on_audio_ended(slot, want, why); });
            if (!granted) {
                // The engine refused: no such receiver or a source that is
                // not open, in its own words. Not routed into errorText, for
                // the reason detectionFault is not: a refused subscription
                // is not a lost engine and must not read as one. Said on the
                // audio section when it is the focused receiver's, and
                // asked again next pass either way.
                if (want == live_receiver_id_) {
                    work_audio_fault_ = QString::fromStdString(granted.error().message);
                }
                continue;
            }

            live_audio_[slot] = AudioSub{want, *granted, desired_demod[slot]};

            // THE RING IS RESIZED FROM THE GRANT AND NOT FROM THE REQUEST,
            // and the reset is what makes that stick. subscribe_audio
            // installs the chunk callback BEFORE it sends the request, so a
            // chunk can reach the ring while this call is still waiting for
            // the answer, and that chunk would have established the ring at
            // the provisional depth above. The reset clears the format, so
            // the next chunk re-establishes at the depth the engine actually
            // granted.
            //
            // It costs whatever arrived during the round trip, which on
            // loopback is nothing and on a link slow enough to matter is a
            // few chunks at the very start of a stream. The alternative is a
            // ring whose depth silently differs from the grant, which is the
            // failure the grant is reported to prevent.
            ring.set_depth_millis(*granted);
            ring.reset();

            if (want == live_receiver_id_) {
                work_audio_fault_.clear();
                work_audio_ended_.clear();
                work_audio_stats_ = {};
            }
        }
    }

    // The focused receiver's subscription, which is what the audio section
    // describes. Its counters go when it changes, because they were about
    // another stream.
    qulonglong pane_vrx = 0;
    std::uint32_t pane_granted = 0;
    if (live_receiver_id_ != 0) {
        for (const AudioSub& sub : live_audio_) {
            if (sub.vrx == live_receiver_id_) {
                pane_vrx = sub.vrx;
                pane_granted = sub.granted;
            }
        }
    }
    if (pane_vrx != live_audio_vrx_) {
        work_audio_stats_ = {};
    }
    live_audio_vrx_ = pane_vrx;
    live_audio_granted_ = pane_granted;

    publish_mix();
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
