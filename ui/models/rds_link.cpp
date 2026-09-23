// EngineLink's RDS surface: the poll, the region, and the station as a
// pane shows it.
//
// Split from ui/models/engine_link.cpp on the same grounds audio_link.cpp
// and receiver_link.cpp are: same class, same threads, same rules, and
// ui/models/engine_link.h holds the contract.
//
// THE WIRE HAS CARRIED THIS ALL ALONG. core/rpc/client.h's rds_station has
// been served since the decoder landed, tests/rpc/test_rpc_rds.cpp drives
// it through a real engine, and on 2026-09-20 a real station answered with
// a call sign, a PS, a RadioText and a PTY while the window showed none of
// it. The whole of this file is reading what was already there.
//
// WHY THE FIRST CALL IS A DECISION AND NOT A POLL. core/rpc/client.h is
// explicit that the first rds_station call BUILDS the decoder on that
// receiver, the way the first detections call builds the detector. So this
// is behind a switch: an always-on poll would stand a composite decoder up
// behind every receiver anybody ever tuned, on an engine shared with other
// clients.
//
// WHY THE SWITCH RAISES THE AUDIO RATE, AND ASKS FIRST. Four conditions
// have to hold before a receiver can carry a composite and the binding one
// is the audio rate: 57 kHz has to survive the audio decimation, which
// means 171000 samples a second, and a receiver created the ordinary way
// takes the engine's default of 48000. So the switch cannot just poll; it
// has to rebuild the receiver at the composite rate. It asks the engine
// whether that rate is grantable before touching the operator's receiver,
// on a throwaway one that costs nothing when it is refused. See
// ui/models/composite_probe.h and ensure_composite_receiver below.
//
// WHAT THIS PARAGRAPH USED TO SAY: "the binding one on the shipped grid is
// the audio rate... a 64-channel grid over 2.4 MS/s has channels a
// fraction of that wide. The engine refuses and names WHICH condition
// failed." The refusal was real and the diagnosis was not. revenant-engine
// sizes its channel count so one channel carries a 200 kHz broadcast FM
// receiver, so 2.4 MS/s gets M=8 and a 600000 S/s channel, which clears
// 171000 comfortably. What refused every poll was the 48000 default this
// client never overrode.
//
// A REFUSAL IS STILL THE USEFUL ANSWER AND NOT A FAILURE, wherever one
// comes from. The engine names WHICH condition failed, and that sentence is
// the thing the operator needs, so it goes on screen verbatim rather than
// being turned into "RDS unavailable".

#include "models/engine_link.h"

#include <mutex>
#include <string>
#include <utility>

#include <QMetaObject>
#include <QString>

#include "core/rpc/client.h"
#include "core/rpc/types.h"
#include "models/composite_probe.h"
#include "models/rds_view.h"

namespace revenant::ui {
namespace {

// What the pane says while the rebuild at the composite rate is in flight.
//
// A function and not a file-scope QString, because a QString built before
// QCoreApplication exists is a static with a heap allocation behind it and
// this file is compiled into a GUI binary whose statics are constructed in an
// order nothing here controls. QStringLiteral's data is in the binary and the
// wrapper is free, so the call costs nothing worth a static for.
[[nodiscard]] QString raising_rate_sentence()
{
    return QStringLiteral(
        "raising this receiver to 171000 S/s so the 57 kHz subcarrier survives the "
        "audio decimation. The audio restarts while it rebuilds.");
}

}  // namespace

void EngineLink::setRdsWanted(bool wanted)
{
    if (rds_wanted_.load() == wanted) {
        return;
    }
    rds_wanted_.store(wanted);

    if (!wanted) {
        // Cleared here and not left for the supervisor. The operator has
        // just said they are not looking at this, and a station frozen on
        // screen behind a switch that is off is a reading nothing stands
        // behind.
        rds_answered_ = false;
        rds_station_ = {};
        rds_status_.clear();
        rds_identity_.clear();
        rds_ps_.clear();
        rds_radio_text_.clear();
        rds_programme_type_.clear();
        rds_fault_.clear();
        rds_is_fault_ = false;
        rds_decoding_ = false;
        rds_ps_segments_ = 0;
        rds_ps_segments_total_ = 0;
        rds_rt_segments_ = 0;
        rds_rt_segments_total_ = 0;
        rds_block_error_rate_ = -1.0;

        // AND THE RECEIVER GOES BACK TO PROGRAMME AUDIO. 171000 is the
        // multiplex, which is not a thing anybody listens to, so a receiver
        // left at it is one the operator cannot hear the station on. Leaving
        // it would make this switch a control with a permanent side effect,
        // discoverable only by turning the audio on afterwards and finding
        // the station gone. The cost is one more audio restart, which is what
        // the switch going on already paid.
        //
        // Not conditional on the audio switch. The rate is a property of the
        // receiver and the next thing to subscribe to it inherits whatever it
        // was left at, so tying this to whether anyone is listening now would
        // hand the multiplex to whoever listens next.
        lower_receiver_from_composite();
    }

    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        rds_work_pending_ = true;
    }
    supervisor_wake_.notify_all();

    emit rdsChanged();
}

QString EngineLink::rdsRegion() const
{
    return rds_region_rbds_.load() ? QStringLiteral("rbds") : QStringLiteral("rds");
}

void EngineLink::setRdsRegion(const QString& region)
{
    // Two spellings and nothing else. An unrecognised one is ignored
    // rather than defaulted, because defaulting is how a client ends up
    // silently reading a US station under the European PTY table.
    bool rbds = false;
    if (region == QStringLiteral("rbds")) {
        rbds = true;
    } else if (region != QStringLiteral("rds")) {
        return;
    }

    if (rds_region_rbds_.load() == rbds) {
        return;
    }
    rds_region_rbds_.store(rbds);

    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        rds_work_pending_ = true;
    }
    supervisor_wake_.notify_all();

    emit rdsChanged();
}

// reason is why there is no decoder, and empty means the operator turned
// the switch off, which is the one case where silence is what they asked
// for. Everything else has to say so.
//
// WHAT THIS POSTED UNTIL 2026-09-21: nothing but answered=false, on every
// path. models/rds_view.h renders that as "not asking for RDS on this
// receiver.", so a pane with the RDS switch ON read exactly backwards the
// moment the engine went away: the window was asking, there was nothing to
// ask, and the sentence sent the operator to a switch that was already on.
void EngineLink::clear_rds(const QString& reason, const QString& label)
{
    if (rds_polled_vrx_ == 0 && !rds_region_written_) {
        return;
    }
    rds_polled_vrx_ = 0;
    rds_region_written_ = false;

    // AND THE PROBE'S ANSWER, because it was about a receiver this window is
    // no longer polling. The id it was keyed on would match again on a
    // reconnect that handed back the same number, which is not the same
    // receiver and need not be in the same channel.
    rds_composite_probed_vrx_ = 0;
    rds_composite_refusal_.clear();
    rds_composite_label_.clear();

    post_rds_fault(reason, label);
}

void EngineLink::post_rds_fault(QString reason, QString label)
{
    {
        const std::lock_guard<std::mutex> lock(rds_mutex_);
        has_rds_handover_ = true;
        handover_rds_answered_ = false;
        handover_rds_station_ = {};
        handover_rds_fault_ = std::move(reason);
        handover_rds_label_ = std::move(label);
    }
    QMetaObject::invokeMethod(this, [this] { adopt_rds(); }, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// The audio rate, which is what makes the switch work at all
// ---------------------------------------------------------------------------

bool EngineLink::ensure_composite_receiver()
{
    // The pane's own request, which is what the live receiver was built from.
    // Read here rather than carried on this thread, because the Qt thread owns
    // it and a mode change or a retune moves it: a copy taken when the switch
    // went on would probe with a centre and a mode the receiver no longer has.
    rpc::VrxParams live;
    {
        const std::lock_guard<std::mutex> lock(receiver_mutex_);
        live = requested_params_;
    }

    if (carries_composite(live)) {
        // The rebuild has already happened, or is in flight and the request
        // already carries the rate. Either way there is nothing to ask and the
        // decoder can be built; a receiver still mid-rebuild answers the
        // region write with a refusal naming the id, which is a sentence about
        // one pass and is replaced by the next.
        return true;
    }

    if (rds_composite_probed_vrx_ == live_receiver_id_) {
        if (!rds_composite_refusal_.isEmpty()) {
            // Asked already for this receiver and refused. Held rather than
            // asked again, because asking means creating a receiver on the
            // engine and the answer cannot change while the receiver does not.
            post_rds_fault(rds_composite_refusal_, rds_composite_label_);
            return false;
        }

        // The probe said yes and the request does not carry the rate. Either
        // the rebuild is still in flight, or the operator turned the switch
        // off and on again and setRdsWanted took the rate back out in
        // between. RE-POSTING IS WHAT RECOVERS THE SECOND CASE, and it is why
        // this is not a return on a flag: a gate that trusted "already asked"
        // would leave the second switch-on polling a 48 kHz receiver forever,
        // with a sentence about a rebuild that nothing was going to perform.
        // raise_receiver_to_composite does nothing in the first case.
        QMetaObject::invokeMethod(
            this, [this] { raise_receiver_to_composite(); }, Qt::QueuedConnection);
        post_rds_fault(raising_rate_sentence(), QString(kRdsLabelRaising.data()));
        return false;
    }
    rds_composite_probed_vrx_ = live_receiver_id_;
    rds_composite_refusal_.clear();
    rds_composite_label_.clear();

    // TWO OF THE FOUR CONDITIONS NEED NO ROUND TRIP. The demodulator is a
    // fact about the request and this window holds the request, so asking the
    // engine about a receiver on am would spend a receiver to be told
    // something the client could read off a field. models/composite_probe.h
    // has both and the sentence.
    const CompositeProbe probe =
        plan_composite_probe(live, demod_name(live.demod).toStdString());
    if (!probe.worth_asking) {
        rds_composite_refusal_ = QString::fromStdString(probe.refusal);
        rds_composite_label_ = QString(kRdsLabelWrongMode.data());
        post_rds_fault(rds_composite_refusal_, rds_composite_label_);
        return false;
    }

    // THE THROWAWAY RECEIVER. Client::add_vrx either answers with an id or
    // refuses and destroys nothing, so this is the engine answering the one
    // question the client cannot: whether the channel this receiver landed in
    // can be resampled to 171000 at the passband it holds. The arithmetic is
    // dsp::plan_vrx's and this process links no part of it.
    //
    // It costs a receiver on the engine for the length of two calls. That is
    // the price of not risking the operator's: a rate change is a remove and
    // an add, and an add that failed leaves the pane empty.
    auto added = client_->add_vrx(probe.params);
    if (!added) {
        rds_composite_refusal_ = QString::fromStdString(added.error().message);
        rds_composite_label_ = QString(kRdsLabelRateRefused.data());
        post_rds_fault(rds_composite_refusal_, rds_composite_label_);
        return false;
    }

    // WHAT THE PROBE WAS GRANTED, WHICH THE ADD SUCCEEDING DOES NOT SAY. The
    // fourth condition is read off VrxPlacement::grantedLow and grantedHigh
    // rather than off the request, because each edge is fitted on its own, and
    // a narrow receiver at 171000 is admitted by the planner and then refused
    // by the decoder guard. Read it here, on the receiver nobody is using,
    // rather than finding out after the operator's has been rebuilt.
    //
    // A FAILED READ IS NOT A REFUSAL. The add succeeded, which is the bar this
    // whole mechanism was specified around, and turning one unanswered status
    // call into a switch that does nothing would be worse than letting the
    // engine's own decoder guard have the last word.
    QString grant_refusal;
    if (auto status = client_->vrx_status(*added)) {
        grant_refusal = QString::fromStdString(composite_grant_refusal(
            status->placement.granted_low, status->placement.granted_high));
    }

    // Discarded for the reason drop_receiver discards its own remove: a remove
    // that failed because the engine no longer has the receiver has achieved
    // what was wanted, and there is nothing else this can do about one that
    // failed for another reason.
    static_cast<void>(client_->remove_vrx(*added));

    if (!grant_refusal.isEmpty()) {
        rds_composite_refusal_ = grant_refusal;
        rds_composite_label_ = QString(kRdsLabelChannelNarrow.data());
        post_rds_fault(rds_composite_refusal_, rds_composite_label_);
        return false;
    }

    // Yes. The rebuild is the Qt thread's, because wanted_ is the Qt thread's
    // and post_receiver_request is the one path every write to it takes.
    QMetaObject::invokeMethod(
        this, [this] { raise_receiver_to_composite(); }, Qt::QueuedConnection);

    // And say what is happening, because the rebuild takes a supervisor pass
    // and the audio breaks during it. Silence there reads as the switch having
    // done nothing at all, which is the state this change exists to end.
    post_rds_fault(raising_rate_sentence(), QString(kRdsLabelRaising.data()));
    return false;
}

void EngineLink::raise_receiver_to_composite()
{
    if (carries_composite(wanted_)) {
        return;
    }
    wanted_.audio_rate = kRdsCompositeRateHz;

    // A recreate and not an in-place write. The audio rate sets the
    // decimation, which sets the tap count, which is the filter the engine
    // says in as many words it cannot change under a running stage. Same
    // reason setReceiverDemod posts one.
    post_receiver_request(true);

    // rdsCompositeReceiver is on rdsChanged, and post_receiver_request emits
    // receiverChanged. Without this the sentence about the audio having become
    // the multiplex appears at the next RDS poll rather than at the rebuild.
    emit rdsChanged();
}

void EngineLink::lower_receiver_from_composite()
{
    if (!carries_composite(wanted_)) {
        return;
    }

    // Zero and not 48000, which is the engine's business. VrxParams::audioRate
    // of zero means "the engine's default", and naming a number here would
    // pin this window to one an engine configured otherwise does not use.
    wanted_.audio_rate = 0;

    // NOTHING POSTED FOR AN EMPTY PANE. post_receiver_request is a request to
    // have the receiver in wanted_, and the supervisor answers one against no
    // live receiver by CREATING it, so turning the switch off with no receiver
    // would conjure one at whatever the pane was last tuned to. The rate is
    // dropped either way, which is what the next thing tuned has to inherit,
    // and a receiver put back by a reconnect is rebuilt from wanted_ anyway.
    if (receiver_id_ != 0) {
        post_receiver_request(true);
    }
    emit rdsChanged();
}

void EngineLink::poll_rds()
{
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        rds_work_pending_ = false;
    }

    const qulonglong vrx = live_receiver_id_;

    // Three reasons and they are not the same sentence. Only the switch
    // being off is the operator's own choice, and that one takes the empty
    // reason so the view keeps its "not asking" wording.
    if (!rds_wanted_.load()) {
        clear_rds();
        return;
    }
    if (client_ == nullptr) {
        clear_rds(QStringLiteral("no engine is connected, so nothing is decoding RDS. "
                                 "The switch stays on and the decoder is rebuilt when "
                                 "the connection comes back."),
                  QString(kRdsLabelNoEngine.data()));
        return;
    }
    if (vrx == 0) {
        clear_rds(QStringLiteral("this pane holds no receiver, and the RDS decoder hangs "
                                 "off one. Tune a receiver and the decoder is built on it."),
                  QString(kRdsLabelNoReceiver.data()));
        return;
    }

    // THE RECEIVER CHANGING IS A DIFFERENT DECODER. A rebuild issues a new
    // id and the decoder hangs off the receiver, so anything accumulated
    // belongs to the one that went and the region has to be written again.
    if (vrx != rds_polled_vrx_) {
        rds_polled_vrx_ = vrx;
        rds_region_written_ = false;
    }

    // THE RATE BEFORE THE REGION AND BEFORE THE FIRST POLL. Both of the calls
    // below build the decoder, and both check the four conditions, so either
    // one made against a 48 kHz receiver is a refusal with the subcarrier
    // already destroyed. The gate is what raises the rate first, and it
    // returns false until the receiver is actually running at it. Every false
    // return has posted a sentence, so there is no pass on which the switch is
    // on and the pane says nothing.
    if (!ensure_composite_receiver()) {
        return;
    }

    const bool want_rbds = rds_region_rbds_.load();

    // WRITTEN ONLY WHEN IT HAS TO BE. core/rpc/client.h: this call rebuilds
    // the decoder and clears everything accumulated, and a client that set
    // the region it already had would throw away the station the operator
    // was reading, once a second, forever. It is also settable BEFORE the
    // first poll, which is why it goes first: otherwise the first answer
    // comes back under the wrong region and is then discarded by the write
    // that corrects it.
    if (!rds_region_written_ || want_rbds != rds_posted_region_rbds_) {
        const auto region = want_rbds ? rpc::RdsRegion::Rbds : rpc::RdsRegion::Rds;
        if (auto set = client_->set_rds_region(vrx, region); !set) {
            post_rds_fault(QString::fromStdString(set.error().message),
                           QString(kRdsLabelRefused.data()));
            return;
        }
        rds_region_written_ = true;
        rds_posted_region_rbds_ = want_rbds;
    }

    auto station = client_->rds_station(vrx);

    // POSTED ON EVERY PASS, unlike the detection and audio surfaces. The
    // health counters move continuously on a live station, so a
    // suppression comparison would have to diff the whole struct to answer
    // "did anything change" and would find that it had, every time. One
    // metacall a second carrying a struct nobody redraws from is cheaper
    // than that comparison.
    {
        const std::lock_guard<std::mutex> lock(rds_mutex_);
        has_rds_handover_ = true;
        if (station) {
            handover_rds_answered_ = true;
            handover_rds_station_ = std::move(*station);
            handover_rds_fault_.clear();
        } else {
            handover_rds_answered_ = false;
            handover_rds_station_ = {};
            handover_rds_fault_ = QString::fromStdString(station.error().message);
        }
        handover_rds_label_.clear();
    }
    QMetaObject::invokeMethod(this, [this] { adopt_rds(); }, Qt::QueuedConnection);
}

void EngineLink::adopt_rds()
{
    rpc::RdsStation station;
    bool answered = false;
    QString fault;
    QString label;

    {
        const std::lock_guard<std::mutex> lock(rds_mutex_);
        if (!has_rds_handover_) {
            return;
        }
        has_rds_handover_ = false;
        station = std::move(handover_rds_station_);
        answered = handover_rds_answered_;
        fault = handover_rds_fault_;
        label = handover_rds_label_;
    }

    rds_station_ = std::move(station);
    rds_answered_ = answered;
    rds_fault_ = fault;

    // The whole derivation, on the Qt thread and through the one pure
    // function ui/tests drives. Nothing here decides what an empty station
    // means; models/rds_view.h does, in the order core/rpc/types.h asks
    // for, and this converts the result to QString through fromStdString,
    // which is the UTF-8 conversion. Never fromLatin1: see
    // models/rds_text.h for the byte that makes that mistake invisible.
    const RdsView view = make_rds_view(rds_station_, answered, fault.toStdString());

    rds_status_ = QString::fromStdString(view.status);

    // A reason known more precisely than the state, when the path that posted
    // the fault knew one; a failed station read is "refused" rather than the
    // state's "stopped", because the decoder did not stop, the call failed.
    if (label.isEmpty() && !fault.isEmpty()) {
        label = QString(kRdsLabelRefused.data());
    }
    const auto state_label = rds_state_label(view.state);
    rds_label_ = label.isEmpty()
                     ? QString::fromUtf8(state_label.data(),
                                         static_cast<qsizetype>(state_label.size()))
                     : label;
    rds_is_fault_ = view.is_fault;
    rds_decoding_ = view.state == RdsState::Decoding;
    rds_identity_ = QString::fromStdString(view.identity);
    rds_ps_ = QString::fromStdString(view.ps.text);
    rds_radio_text_ = QString::fromStdString(view.radio_text.text);
    rds_programme_type_ = QString::fromStdString(view.programme_type);
    rds_ps_segments_ = view.ps.segments_received;
    rds_ps_segments_total_ = view.ps.segments_total;
    rds_rt_segments_ = view.radio_text.segments_received;
    rds_rt_segments_total_ = view.radio_text.segments_total;
    rds_block_error_rate_ = view.block_error_rate;

    // The refusal's precedence used to be re-applied here, over the view
    // this function had just built. It is inside make_rds_view now, with
    // the rest of the order, because this file decides nothing about what
    // an empty station means and a second precedence rule out here was
    // reachable only through the window.
    emit rdsChanged();
}

}  // namespace revenant::ui
