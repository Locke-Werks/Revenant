// The receiver half of EngineLink: the detail pane's receiver, its passband
// and the subscription that draws it.
//
// Split from ui/models/engine_link.cpp because that file is already the
// longest in the client and the two halves have no state in common beyond
// the Client and the supervisor's loop. Same class, same threads, same
// rules; ui/models/engine_link.h holds the contract for both.
//
// THE THREE THREADS, RESTATED FOR THIS HALF
//
// The Qt thread owns every property below and is the only thread that reads
// them. It never makes an RPC call: Client::set_vrx_params blocks for a
// round trip, and a filter edge dragged at sixty hertz would stall the
// window sixty times a second.
//
// The supervisor thread makes every call. A write on the Qt thread records
// what is wanted under receiver_mutex_ and wakes the supervisor, which is
// waiting on a predicate that includes receiver_work_pending_, so the wake
// is immediate rather than up to a poll interval away.
//
// The Cap'n Proto event loop thread delivers passband frames, into the same
// latest-wins hand-off the span uses.
//
// WHY THE DISPLAY DOES NOT WAIT FOR THE ENGINE TO AGREE
//
// A drag redraws from the pane's own request the instant the pointer moves.
// The engine's answer arrives a round trip later and is drawn as a SECOND
// rule whenever it differs, which is the clamp made visible. Waiting for
// the echo instead would make the handle move at the round-trip rate, which
// is exactly the lag that makes a control feel broken, and it would hide
// the one thing worth seeing: that the engine did not give what was asked.

#include "models/engine_link.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include <QMetaObject>
#include <QString>

#include "models/receiver_match.h"

namespace revenant::ui {
namespace {

// A Rational as a double. The same policy as engine_link.cpp's local
// helper and a zero denominator answers zero, which is what
// rpc::Rational::hertz() already decided it means.
[[nodiscard]] double rational_hertz(const rpc::Rational& value)
{
    return value.denominator == 0 ? 0.0
                                  : static_cast<double>(value.numerator) /
                                        static_cast<double>(value.denominator);
}

}  // namespace

// ---------------------------------------------------------------------------
// Property getters, Qt thread
// ---------------------------------------------------------------------------

double EngineLink::receiverCenterHz() const
{
    // params.center is baseband, which is the only frame the grid has. The
    // source's centre is what turns it into the number on a band plan, and
    // this object is the one that holds both.
    return static_cast<double>(info_.source_center) + static_cast<double>(wanted_.center);
}

QString EngineLink::receiverDemod() const { return demod_name(wanted_.demod); }

// ---------------------------------------------------------------------------
// The axis of the passband frame
// ---------------------------------------------------------------------------

double EngineLink::passbandFrequencyAtFraction(double fraction) const
{
    const rpc::PassbandGeometry& geometry = passband_display_.geometry;
    if (!geometry.enabled()) {
        return 0.0;
    }

    // The same half-bin convention frequencyAtFraction uses for the span,
    // and for the same reason: fraction zero is the outer edge of the first
    // bin, which is where the item's own left edge is.
    const double bin = fraction * static_cast<double>(geometry.bins) - 0.5;
    return static_cast<double>(info_.source_center) + rational_hertz(geometry.bin_zero) +
           rational_hertz(geometry.bin_width) * bin;
}

double EngineLink::passbandOffsetAtFraction(double fraction) const
{
    if (!passband_display_.geometry.enabled()) {
        return 0.0;
    }

    // Absolute, then back to the frame the passband edges are in, which is
    // hertz from the receiver's own centre. Going through absolute rather
    // than subtracting bin_zero directly is what makes this right on CW
    // with no CW in it: bin_zero is carried as whatever the fine stage
    // mixed to DC, which on that mode is a pitch below the centre, and the
    // subtraction below puts the pitch back where it belongs.
    return passbandFrequencyAtFraction(fraction) - receiverCenterHz();
}

double EngineLink::passbandFractionAtOffset(double offset_hz) const
{
    const rpc::PassbandGeometry& geometry = passband_display_.geometry;
    if (!geometry.enabled()) {
        return 0.0;
    }
    const double width = rational_hertz(geometry.bin_width);
    if (width == 0.0) {
        return 0.0;
    }

    // The inverse of the two above, solved for fraction. Written out rather
    // than derived by a search, because a rule drawn at an edge has to land
    // on the same pixel the drag reads that edge back from.
    const double absolute = receiverCenterHz() + offset_hz;
    const double bin =
        (absolute - static_cast<double>(info_.source_center) -
         rational_hertz(geometry.bin_zero)) /
        width;
    return (bin + 0.5) / static_cast<double>(geometry.bins);
}

// ---------------------------------------------------------------------------
// Writes, Qt thread
// ---------------------------------------------------------------------------

std::pair<int, int> EngineLink::fit_edges(int low, int high) const
{
    // Inverted or empty first, because everything below assumes low < high.
    if (low >= high) {
        high = low + kMinPassbandWidthHz;
    }

    // The channel's limit, when the engine has said what it is. Zero means
    // it has not answered yet, and a drag before the first status is
    // ordinary: the pane is usable the moment a receiver is added and the
    // status is a poll interval behind. Nothing is clamped in that window
    // and the engine fits the request instead, which is what the granted
    // pair is for.
    if (receiver_edge_limit_ > 0) {
        low = std::max(low, -receiver_edge_limit_);
        high = std::min(high, receiver_edge_limit_);
    }

    // The width stop. Applied after the channel limits so an edge pushed
    // against the channel cannot be widened back out past it: the other
    // edge moves instead.
    if (high - low < kMinPassbandWidthHz) {
        if (receiver_edge_limit_ > 0 && high >= receiver_edge_limit_) {
            low = high - kMinPassbandWidthHz;
        } else {
            high = low + kMinPassbandWidthHz;
        }
    }

    return {low, high};
}

void EngineLink::update_receiver_fit()
{
    // Both halves off the same status. See fit_from_status for why the pane's
    // own live request is the wrong number here: it is what the operator has
    // drawn, and during a widen drag the engine has not been asked for it.
    const ReceiverFit fit = fit_from_status(
        receiver_status_.params.passband_low, receiver_status_.params.passband_high,
        receiver_status_.placement.granted_low, receiver_status_.placement.granted_high,
        receiver_status_.placement.bandwidth_clamped, tuned_detection_bandwidth_);

    // Nothing at all when the pane holds no receiver. The status struct
    // keeps its last values across a clear on some paths, and a sentence
    // about a receiver that is gone reads as a sentence about the next
    // one.
    QString text;
    if (receiver_id_ != 0) {
        text = QString::fromStdString(fit_sentence(fit));
    }

    if (text == receiver_fit_text_) {
        return;
    }
    receiver_fit_text_ = text;
    emit receiverFitChanged();
}

void EngineLink::tuneReceiverToDetection(double absolute_hz, const QString& mode,
                                         double detection_bandwidth_hz)
{
    tune_receiver(absolute_hz, mode,
                  detection_bandwidth_hz > 0.0 ? detection_bandwidth_hz : 0.0);
}

// A receiver placed by hand has no measured signal behind it, so the
// bandwidth goes to zero and the comparison is suppressed. Leaving the
// previous click's measurement in place would compare the new receiver
// against a band it was never measured in.
void EngineLink::tuneReceiver(double absolute_hz, const QString& mode)
{
    tune_receiver(absolute_hz, mode, 0.0);
}

void EngineLink::tune_receiver(double absolute_hz, const QString& mode,
                               double detection_bandwidth_hz)
{
    bool moved = false;

    if (mode.isEmpty()) {
        // Keep the mode the pane has.
    } else if (auto parsed = demod_from_name(mode)) {
        if (*parsed != wanted_.demod) {
            wanted_.demod = *parsed;
            moved = true;

            // A new mode means a new default passband unless the operator
            // has placed the edges themselves on this receiver.
            if (!edges_touched_) {
                wanted_.passband_low = 0;
                wanted_.passband_high = 0;
            }
        }
    } else {
        receiver_fault_ = QStringLiteral("'%1' is not one of raw, am, nfm, wfm, usb, lsb, "
                                         "dsb, cw")
                              .arg(mode);
        emit receiverFaultChanged();
        return;
    }

    // Absolute in, baseband out. place() reads VrxParams::center as an
    // offset from the source's own centre and nothing between here and
    // there rebases it, so the subtraction is this caller's job and is done
    // here because this is the object holding EngineInfo::sourceCenter.
    const std::int64_t center =
        static_cast<std::int64_t>(std::llround(absolute_hz)) - info_.source_center;
    moved = moved || center != wanted_.center;
    wanted_.center = center;
    wanted_.bandwidth = 0;

    // The held frame was measured around the old centre in the old mode, so
    // the moment either moves it is describing a receiver that no longer
    // exists. Dropped here rather than waited out, because the frame that
    // would replace it may never come: a raw tap is refused a passband
    // subscription outright. See reset_passband_display.
    //
    // Only when something actually moved. Re-tuning to the frequency already
    // held is what a second click on the same detection does, and blanking
    // the pane for it would flash the waiting plate for no reason.
    if (moved && reset_passband_display()) {
        emit passbandChanged();
    }

    // A tune is a new receiver whenever the pane is on none, and a retune
    // of the one it has otherwise. Moving the dial is a push constant and a
    // new tap table, which the engine does in place.
    //
    // Recorded before the request goes out, so post_receiver_request's own
    // call to update_receiver_fit sees the measurement this tune came with
    // rather than the previous click's.
    tuned_detection_bandwidth_ = detection_bandwidth_hz;

    post_receiver_request(receiver_id_ == 0);
}

void EngineLink::setReceiverDemod(const QString& mode)
{
    auto parsed = demod_from_name(mode);
    if (!parsed) {
        receiver_fault_ =
            QStringLiteral("'%1' is not one of raw, am, nfm, wfm, usb, lsb, dsb, cw")
                .arg(mode);
        emit receiverFaultChanged();
        return;
    }
    if (*parsed == wanted_.demod) {
        return;
    }

    wanted_.demod = *parsed;
    if (!edges_touched_) {
        // Unstated, so the engine answers with the mode's own default and
        // the pane reads it back off the placement. This client carries no
        // copy of that table on purpose; see resolve_passband.
        wanted_.passband_low = 0;
        wanted_.passband_high = 0;
        wanted_.bandwidth = 0;
    }

    // The frame in hand belongs to the old mode's receiver, which is about
    // to be removed. This is the entrance reset_passband_display exists for:
    // nfm to raw, where subscribe_passband is refused by contract and no
    // later frame corrects the pane.
    if (reset_passband_display()) {
        emit passbandChanged();
    }

    // A remove and an add, because the demodulator IS the stage: the engine
    // refuses to change it on a running receiver and says so in as many
    // words. The pane keeps its own identity across the swap, so nothing
    // above here has to know.
    post_receiver_request(true);
}

void EngineLink::setReceiverPassband(int low, int high)
{
    const auto [fitted_low, fitted_high] = fit_edges(low, high);
    if (fitted_low == wanted_.passband_low && fitted_high == wanted_.passband_high) {
        return;
    }

    wanted_.passband_low = fitted_low;
    wanted_.passband_high = fitted_high;
    wanted_.bandwidth = 0;
    edges_touched_ = true;

    // A PAN NOW, A WIDEN ON RELEASE. See the note on this method in
    // ui/models/engine_link.h for why the two are different: the engine
    // takes a pan as a push constant and a new tap table, and a width
    // change is a remove and an add because the tap count moves with it.
    //
    // Sending a widen on every mouse move would therefore tear the audio
    // once per pixel. Drawing it and sending one on release costs one
    // break for the whole gesture, which is the least a change of filter
    // width can cost at all.
    const int width = fitted_high - fitted_low;
    if (dragging_ && sent_width_ != 0 && width != sent_width_) {
        width_uncommitted_ = true;
        drag_changed_ = true;
        emit receiverChanged();
        return;
    }

    post_receiver_request(false);

    // After the post, not before: post_receiver_request is what a fresh
    // request looks like and it clears the gesture's held state, so a flag
    // set first would be wiped by the very send it is recording.
    if (dragging_) {
        drag_changed_ = true;
    }
}

void EngineLink::beginReceiverDrag()
{
    dragging_ = true;
    drag_changed_ = false;
    receiver_drag_live_.store(true, std::memory_order_release);
}

void EngineLink::endReceiverDrag()
{
    dragging_ = false;

    // Cleared BEFORE the commit below, so the request that commit posts is
    // the one the supervisor is allowed to rebuild for. See
    // receiver_drag_live_.
    receiver_drag_live_.store(false, std::memory_order_release);
    commitReceiverPassband();
}

void EngineLink::commitReceiverPassband()
{
    // Either a width was held back, or the gesture moved the band at all.
    // The second is what carries a pan the engine refused mid-drag: it was
    // sent, refused and deliberately not rebuilt for, so the release is the
    // only thing that can apply it. A pan the engine took costs one extra
    // retune here, which is a push constant and a new tap table.
    if (!width_uncommitted_ && !drag_changed_) {
        return;
    }
    width_uncommitted_ = false;
    drag_changed_ = false;
    post_receiver_request(false);
}

void EngineLink::nudgeReceiverPassband(int delta_hz)
{
    if (delta_hz == 0) {
        return;
    }

    // Both edges by the same amount, so the width is held and the offset
    // moves. This is the drag on the fill between the two handles, and it
    // is not a retune: the receiver stays where it is and its filter slides
    // across it, which is what an operator chasing a drifting signal on SSB
    // is actually doing.
    const int low = static_cast<int>(wanted_.passband_low) + delta_hz;
    const int high = static_cast<int>(wanted_.passband_high) + delta_hz;

    // Fitted as a pair rather than edge by edge, so a pan that reaches the
    // channel's limit stops rather than narrowing.
    if (receiver_edge_limit_ > 0) {
        if (low < -receiver_edge_limit_ || high > receiver_edge_limit_) {
            return;
        }
    }

    setReceiverPassband(low, high);
}

void EngineLink::resetReceiverPassband()
{
    wanted_.passband_low = 0;
    wanted_.passband_high = 0;
    wanted_.bandwidth = 0;

    // Cleared, so a later mode change follows the mode again. The operator
    // asking for the default back is asking for exactly that.
    edges_touched_ = false;

    post_receiver_request(false);
}

void EngineLink::removeReceiver()
{
    if (receiver_id_ == 0) {
        return;
    }

    receiver_id_ = 0;
    static_cast<void>(reset_passband_display());
    receiver_status_ = {};
    receiver_edge_limit_ = 0;
    edges_touched_ = false;

    // The measurement belonged to the receiver that has just gone, and a
    // receiver placed later by hand has none.
    tuned_detection_bandwidth_ = 0.0;

    {
        const std::lock_guard<std::mutex> lock(receiver_mutex_);
        has_receiver_request_ = false;
        receiver_request_recreates_ = false;
        receiver_request_removes_ = true;
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        receiver_work_pending_ = true;
    }
    supervisor_wake_.notify_all();

    emit receiverChanged();
    emit receiverStatusChanged();
    emit passbandChanged();
    update_receiver_fit();
}

void EngineLink::post_receiver_request(bool recreate)
{
    // What the engine is being given, so the next change can be told from
    // it without asking. Recorded here rather than in setReceiverPassband,
    // because a tune or a mode change resolves the passband too and a
    // width remembered from before one of those is a width nobody has.
    width_uncommitted_ = false;
    drag_changed_ = false;
    sent_width_ = static_cast<int>(wanted_.passband_high - wanted_.passband_low);

    {
        const std::lock_guard<std::mutex> lock(receiver_mutex_);
        requested_params_ = wanted_;
        has_receiver_request_ = true;

        // Sticky, because two writes can land between supervisor passes and
        // a recreate followed by an ordinary retune still has to recreate.
        // The reverse loses a receiver: an in-place retune applied to a
        // stage that was going to be rebuilt leaves the old mode running.
        receiver_request_recreates_ = receiver_request_recreates_ || recreate;
        receiver_request_removes_ = false;
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        receiver_work_pending_ = true;
    }

    // Woken rather than left for the poll interval. A quarter of a second
    // between a filter edge moving on screen and moving in the audio is
    // felt as the control sticking, and the supervisor's wait predicate
    // includes this flag for exactly that.
    supervisor_wake_.notify_all();

    emit receiverChanged();

    // Rebuilt for the RECEIVER's sake rather than the request's: the fit
    // line reads both of its numbers off the last status, so a write does
    // not move it, but a write that creates a receiver moves whether there
    // is a line at all. The next status is what changes the words.
    update_receiver_fit();
}

// ---------------------------------------------------------------------------
// The supervisor's side
// ---------------------------------------------------------------------------

void EngineLink::apply_receiver_request()
{
    // The flag is cleared BEFORE the request is taken, not after. A write
    // landing in the window between the two sets it again and raises a
    // fresh wake, which costs one pass here that finds nothing. Clearing
    // afterwards would lose that write and leave a dragged edge sitting
    // until the next poll interval, which is the one thing a drag cannot
    // afford. The same argument drain() makes about the frame wake.
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        receiver_work_pending_ = false;
    }

    rpc::VrxParams params;
    bool wanted = false;
    bool recreate = false;
    bool remove = false;
    {
        const std::lock_guard<std::mutex> lock(receiver_mutex_);
        wanted = has_receiver_request_;
        recreate = receiver_request_recreates_;
        remove = receiver_request_removes_;
        params = requested_params_;
        has_receiver_request_ = false;
        receiver_request_recreates_ = false;
        receiver_request_removes_ = false;
    }

    if (remove) {
        drop_receiver();
        return;
    }
    if (!wanted) {
        return;
    }

    if (recreate || live_receiver_id_ == 0) {
        static_cast<void>(recreate_receiver(params));
        return;
    }

    if (auto applied = client_->set_vrx_params(live_receiver_id_, params); !applied) {
        // The one refusal that is not a mistake: the engine says a change
        // to the filter's shape is a remove and an add, because rebuilding
        // a pipeline under a command buffer already in flight is not
        // something the stage can do. So do the remove and the add, keep
        // the pane's identity across it, and say what happened rather than
        // dropping the gesture.
        //
        // Recognised by the engine's own words rather than by a code,
        // because the wire carries no code. core/engine/vrx_stage.cpp is
        // the one place that phrase is written.
        const QString message = QString::fromStdString(applied.error().message);
        if (message.contains(QStringLiteral("remove and an add"))) {
            // NOT WHILE THE POINTER IS STILL DOWN. A pan towards the fold
            // is refused for the same reason a widen is, and the drag
            // re-posts it every time the pointer moves, so rebuilding here
            // costs one teardown, one add and one passband resubscription
            // per supervisor pass for the whole gesture. The pane keeps
            // drawing the request either way, the granted rules keep
            // showing the engine is elsewhere, and endReceiverDrag posts
            // the final position, which is the one rebuild the gesture
            // actually needs. See EngineLink::receiver_drag_live_.
            //
            // Clear the fault on the way out. Deferring a rebuild is not a
            // fault, and every other exit from this function writes the
            // string it means, so returning without writing one leaves
            // whatever the last pass said. Concretely: cross a shape
            // boundary once, release, and the rebuild posts "the audio
            // restarted". Start a second drag and that sentence stays on
            // screen for the whole gesture while nothing has restarted,
            // because this return is the only path that does not rewrite
            // it.
            if (receiver_drag_live_.load(std::memory_order_acquire)) {
                note_receiver_fault(QString{});
                return;
            }
            if (recreate_receiver(params)) {
                note_receiver_fault(QStringLiteral(
                    "the audio restarted: that filter width changed the demodulation rate, "
                    "which the engine cannot do in place"));
                return;
            }
        }
        note_receiver_fault(message);
        return;
    }

    note_receiver_fault(QString{});
}

bool EngineLink::recreate_receiver(const rpc::VrxParams& params)
{
    drop_receiver();

    auto added = client_->add_vrx(params);
    if (!added) {
        note_receiver_fault(QString::fromStdString(added.error().message));
        return false;
    }
    live_receiver_id_ = *added;

    // The passband subscription is reattached with the receiver, because it
    // was keyed on the id that has just gone. A failure here is not fatal
    // to the receiver: an engine built with no passband stage runs the
    // audio perfectly well and simply has no detail display, so the fault
    // is reported and the receiver stays.
    if (auto watched = client_->subscribe_passband(
            live_receiver_id_, 1,
            [this](const rpc::PassbandFrame& frame) { on_passband_frame(frame); });
        !watched) {
        note_receiver_fault(QString::fromStdString(watched.error().message));
    } else {
        note_receiver_fault(QString{});
    }

    {
        const std::lock_guard<std::mutex> lock(receiver_mutex_);
        pending_receiver_id_ = live_receiver_id_;
        has_pending_receiver_id_ = true;
    }
    QMetaObject::invokeMethod(
        this, [this] { adopt_receiver_status(); }, Qt::QueuedConnection);
    return true;
}

void EngineLink::drop_receiver()
{
    // AUDIO FIRST, AND OUTSIDE THE EARLY RETURN BELOW.
    //
    // The order matters and the placement does too. The engine sends
    // ended() for a subscription whose receiver goes away and never for a
    // cancel this client asked for, so removing the receiver before
    // cancelling delivers an ended for a removal this client performed.
    // The window would then announce that the receiver went away on every
    // mode change, because a mode change is a remove and an add.
    //
    // Above the early return because the audio subscription and the
    // receiver id are separate pieces of state, and a path that clears one
    // without the other leaves a stream running against an id nothing is
    // tracking. stop_audio returns immediately when there is nothing to
    // stop.
    stop_audio();

    if (live_receiver_id_ == 0 || client_ == nullptr) {
        live_receiver_id_ = 0;
        return;
    }

    client_->unsubscribe_passband(live_receiver_id_);

    // Discarded: a remove that failed because the engine no longer has the
    // receiver has achieved what was wanted, and one that failed for any
    // other reason is about to be followed by an add whose own failure is
    // the one worth reporting.
    static_cast<void>(client_->remove_vrx(live_receiver_id_));
    live_receiver_id_ = 0;
}

void EngineLink::poll_receiver_status()
{
    if (live_receiver_id_ == 0) {
        return;
    }

    auto status = client_->vrx_status(live_receiver_id_);
    if (!status) {
        // Not reported and not torn down. The liveness probe owns a lost
        // engine, and a receiver that has gone will be recreated by the
        // next request; a fault posted here would be overwritten a quarter
        // of a second later by the poll that succeeded.
        return;
    }

    {
        const std::lock_guard<std::mutex> lock(receiver_mutex_);
        pending_receiver_status_ = *status;
        has_pending_receiver_status_ = true;
    }
    QMetaObject::invokeMethod(
        this, [this] { adopt_receiver_status(); }, Qt::QueuedConnection);
}

void EngineLink::note_receiver_fault(QString fault)
{
    if (fault == posted_receiver_fault_) {
        // The same sentence the supervisor handed over last pass. Posting
        // it again would wake the Qt thread several times a second to tell
        // it nothing, which is the same argument note_running makes.
        return;
    }
    posted_receiver_fault_ = fault;

    {
        const std::lock_guard<std::mutex> lock(receiver_mutex_);
        pending_receiver_fault_ = std::move(fault);
        has_pending_receiver_fault_ = true;
    }
    QMetaObject::invokeMethod(
        this, [this] { adopt_receiver_fault(); }, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// The Qt thread's side
// ---------------------------------------------------------------------------

void EngineLink::adopt_receiver_status()
{
    rpc::VrxStatus status;
    bool have_status = false;
    qulonglong id = 0;
    bool have_id = false;
    {
        const std::lock_guard<std::mutex> lock(receiver_mutex_);
        have_status = has_pending_receiver_status_;
        status = pending_receiver_status_;
        has_pending_receiver_status_ = false;
        have_id = has_pending_receiver_id_;
        id = pending_receiver_id_;
        has_pending_receiver_id_ = false;
    }

    bool identity_moved = false;
    if (have_id && id != receiver_id_) {
        receiver_id_ = id;
        identity_moved = true;

        // The supervisor rebuilt the receiver, so whatever the pane is
        // holding was measured on the one before it. This is the path a
        // rebuild takes that no Qt-thread write went through: a width
        // change the engine refused in place comes back here as a new id
        // and nothing else says so.
        if (reset_passband_display()) {
            emit passbandChanged();
        }
    }

    bool edges_resolved = false;
    if (have_status) {
        receiver_status_ = status;

        // THE ANSWER TO A REQUEST THAT STATED NOTHING BECOMES THE REQUEST.
        //
        // A tune or a mode change sends an empty passband, which is how
        // this client asks for the mode's own default without carrying a
        // copy of the table. Until the answer arrives the pane's own copy
        // is a pair of zeros, and a drag starting from zeros would begin
        // by collapsing the filter onto the tuned frequency and a display
        // drawing them would put both rules on top of each other in the
        // middle of the pane.
        //
        // So the granted pair is adopted the first time it arrives. Not
        // afterwards: once the pane holds edges they are the request, and
        // taking the grant back in every poll would undo a clamped
        // operator's request a quarter of a second after they made it and
        // make the two shades the display draws collapse into one.
        //
        // edges_touched_ is deliberately NOT set. The operator has not
        // moved anything, so a later mode change should still follow the
        // mode.
        if (wanted_.passband_low == 0 && wanted_.passband_high == 0 &&
            status.placement.granted_high > status.placement.granted_low) {
            wanted_.passband_low = status.placement.granted_low;
            wanted_.passband_high = status.placement.granted_high;
            sent_width_ =
                static_cast<int>(wanted_.passband_high - wanted_.passband_low);
            edges_resolved = true;
        }

        // How far an edge may reach before the channel refuses it, derived
        // here because the engine publishes the granted pair and not the
        // limit. On an unclamped receiver the granted pair IS the request,
        // which says nothing about the limit, so the channel rate is what
        // bounds it: the fit allows half of Fc minus the residual either
        // side, and residual is on the placement as the exact rational the
        // engine placed it at.
        //
        // Half of the channel rate minus |residual| is the same figure
        // dsp::max_channel_bandwidth halves, computed from what is on the
        // wire rather than from a constant this client would have to keep
        // in step.
        const double residual = rational_hertz(status.placement.residual);
        const double limit =
            0.5 * static_cast<double>(status.placement.channel_rate) - std::abs(residual);
        receiver_edge_limit_ = limit > 0.0 ? static_cast<int>(limit) : 0;

        emit receiverStatusChanged();
    }

    if (identity_moved || edges_resolved) {
        emit receiverChanged();
    }

    // Every answer from the engine ends here, and an answer is the only
    // thing that moves the fit line's words: the request it compares
    // against is the echo on this same status.
    update_receiver_fit();
}

void EngineLink::adopt_receiver_fault()
{
    QString fault;
    {
        const std::lock_guard<std::mutex> lock(receiver_mutex_);
        if (!has_pending_receiver_fault_) {
            return;
        }
        fault = std::move(pending_receiver_fault_);
        pending_receiver_fault_.clear();
        has_pending_receiver_fault_ = false;
    }

    if (fault == receiver_fault_) {
        return;
    }
    receiver_fault_ = std::move(fault);
    emit receiverFaultChanged();
}

// ---------------------------------------------------------------------------
// Passband frames
// ---------------------------------------------------------------------------

void EngineLink::on_passband_frame(const rpc::PassbandFrame& frame)
{
    // The Cap'n Proto event loop thread. One copy, unavoidable for the same
    // reason the span's is: the argument is the Client's and is valid only
    // for this call. Assigning into a member this thread owns reuses the
    // vector's capacity, so after the first frame it allocates nothing.
    passband_staging_ = frame;

    {
        const std::lock_guard<std::mutex> lock(swap_mutex_);
        std::swap(passband_staging_, passband_ready_);
        has_passband_ready_ = true;
    }

    if (passband_wake_pending_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    QMetaObject::invokeMethod(
        this, [this] { drain_passband(); }, Qt::QueuedConnection);
}

void EngineLink::drain_passband()
{
    // Cleared before the buffer is taken, for the reason drain() gives: a
    // frame landing between the clear and the swap raises a fresh wake,
    // where clearing afterwards would drop it and stop the display.
    passband_wake_pending_.store(false, std::memory_order_release);

    {
        const std::lock_guard<std::mutex> lock(swap_mutex_);
        if (!has_passband_ready_) {
            return;
        }
        std::swap(passband_ready_, passband_display_);
        has_passband_ready_ = false;
    }

    // A frame that belongs to a receiver the pane is not on is drawn by
    // nobody. It can only be one already on the wire when the subscription
    // was replaced, which is the case unsubscribe's ordered cancel narrows
    // and does not close: the add of the next receiver happens on the
    // supervisor and the Qt thread learns the new id separately.
    //
    // TWO THINGS CHANGED HERE AND BOTH MATTER.
    //
    // receiver_id_ == 0 is inside the test rather than an exemption from it.
    // It used to read "the pane does not know its id, so take anything",
    // and the pane holds no id at exactly two moments, after a remove and
    // while the link is down. A frame at either of those belongs to a
    // receiver that is gone.
    //
    // And a rejected frame is DROPPED rather than merely not drawn. The swap
    // above has already put it in passband_display_, which is what
    // passbandFrequencyAtFraction reads whether or not anything repaints, so
    // returning alone left the foreign frame as the pane's axis.
    if (passband_display_.vrx != receiver_id_) {
        if (reset_passband_display()) {
            emit passbandChanged();
        }
        return;
    }

    passband_active_ = true;
    emit passbandChanged();
}

bool EngineLink::reset_passband_display()
{
    if (!passband_active_ && passband_display_.vrx == 0 &&
        passband_display_.power_db.empty()) {
        return false;
    }

    passband_active_ = false;

    // Assigned rather than cleared field by field, so a field added to
    // PassbandFrame cannot be forgotten here. The vector's capacity goes
    // with it, which costs one allocation on the next frame and is the
    // right trade for a pane that may be off a receiver for hours.
    passband_display_ = {};
    return true;
}

}  // namespace revenant::ui
