// EngineLink's decode half: the decoder list, the subscriptions on the pane's
// receiver, and the hand-off of every message to the log.
//
// Split out the way audio_link.cpp is, and shaped like it: the same three
// threads, the same reconcile, the same rule that ended() must keep meaning a
// removal somebody else made.
//
// THE THREE THREADS, FOR THIS FILE SPECIFICALLY
//
//   The SUPERVISOR owns the Client, so it alone calls decoders,
//   subscribe_decoded and unsubscribe_decoded. apply_decode_request,
//   stop_decoded, forget_decoded and note_decode are its.
//
//   The CAP'N PROTO EVENT LOOP runs on_decoded_message and on_decoded_ended.
//   The first copies the message into a capped hand-off and posts one wake,
//   and the second records the engine's words and wakes the supervisor,
//   because core/rpc/client.h forbids calling back into the Client from
//   there.
//
//   The QT thread runs the setters, adopt_decode, drain_decoded and every
//   getter, and owns the log.
//
// WHY THE LOG IS NOT CLEARED WITH THE RECEIVER
//
// What was decoded stays decoded when the operator moves on, and a log that
// emptied itself on every retune would lose the page an operator tuned away
// from in order to read. Each line carries the receiver it came from in its
// expansion. The clear is theirs.

#include "models/engine_link.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <QDateTime>
#include <QMetaObject>

namespace revenant::ui {
namespace {

[[nodiscard]] QString joined(const std::vector<std::string>& names)
{
    QString out;
    for (const std::string& name : names) {
        if (!out.isEmpty()) {
            out += QStringLiteral(", ");
        }
        out += QString::fromStdString(name);
    }
    return out;
}

[[nodiscard]] std::vector<std::string> to_std(const QStringList& list)
{
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(list.size()));
    for (const QString& item : list) {
        out.push_back(item.toStdString());
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The Qt thread
// ---------------------------------------------------------------------------

void EngineLink::setDecodeWanted(bool wanted)
{
    if (decode_wanted_.exchange(wanted) == wanted) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        decode_work_pending_ = true;
    }
    supervisor_wake_.notify_all();
    emit decodeChanged();
}

void EngineLink::setDecodeChoice(const QString& choice)
{
    if (choice.isEmpty() || (choice == decode_choice_ && choice == decode_choice_shown_)) {
        return;
    }
    decode_choice_ = choice;
    {
        const std::lock_guard<std::mutex> lock(decoded_mutex_);
        requested_decode_choice_ = choice.toStdString();
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        decode_work_pending_ = true;
    }
    supervisor_wake_.notify_all();
    update_decode_choices();
    emit decodeChanged();
}

QString EngineLink::decoderDescription(const QString& name) const
{
    const std::string wanted = name.toStdString();
    for (const rpc::DecoderInfo& info : decoder_infos_) {
        if (info.name == wanted) {
            return QString::fromStdString(info.description);
        }
    }
    return {};
}

void EngineLink::update_decode_choices()
{
    QStringList menu;
    if (receiver_id_ != 0) {
        for (const std::string& name :
             decoder_choices(decoder_infos_, demod_name(wanted_.demod).toStdString())) {
            menu.append(QString::fromStdString(name));
        }
    }
    const QString shown = QString::fromStdString(
        effective_decoder_choice(decode_choice_.toStdString(), to_std(menu)));

    // Compared before emitting, because this is connected to receiverChanged
    // and that fires on every pointer move of a drag. A menu rebuilt at that
    // rate would close under the operator's pointer.
    if (menu == decode_choices_ && shown == decode_choice_shown_) {
        return;
    }
    decode_choices_ = menu;
    decode_choice_shown_ = shown;
    emit decodeChanged();
}

void EngineLink::adopt_decode()
{
    {
        const std::lock_guard<std::mutex> lock(decoded_mutex_);
        if (!has_decode_handover_) {
            return;
        }
        has_decode_handover_ = false;
        decode_attached_ = handover_decode_attached_;
        decode_label_ = handover_decode_label_;
        decode_detail_ = handover_decode_detail_;
        if (handover_has_infos_) {
            handover_has_infos_ = false;
            decoder_infos_ = std::move(handover_decoder_infos_);
            handover_decoder_infos_.clear();
        }
    }
    update_decode_choices();
    emit decodeChanged();
}

void EngineLink::drain_decoded()
{
    // Cleared before the swap, so a message landing after it posts a wake of
    // its own rather than waiting for the next one.
    decoded_wake_pending_.store(false, std::memory_order_release);

    std::vector<rpc::DecodedMessage> messages;
    std::vector<std::int64_t> arrived;
    std::uint64_t unkept = 0;
    {
        const std::lock_guard<std::mutex> lock(decoded_mutex_);
        messages.swap(pending_decoded_);
        arrived.swap(pending_decoded_arrived_ms_);
        unkept = std::exchange(pending_decoded_unkept_, 0);
    }
    if (messages.empty() && unkept == 0) {
        return;
    }

    std::vector<DecodedLine> lines;
    lines.reserve(messages.size());
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const QString when = QDateTime::fromMSecsSinceEpoch(arrived[i])
                                 .toString(QStringLiteral("HH:mm:ss.zzz"));
        lines.push_back(
            make_decoded_line(messages[i], decoded_log_.take_serial(), when.toStdString()));
    }
    decoded_log_.append(std::move(lines), unkept);
}

void EngineLink::setStartupReceiver(double absolute_hz, const QString& mode,
                                    const QString& decoder)
{
    startup_pending_ = true;
    startup_hz_ = absolute_hz;
    startup_mode_ = mode;
    startup_decoder_ = decoder;

    if (!decoder.isEmpty()) {
        decode_choice_ = decoder;
        {
            const std::lock_guard<std::mutex> lock(decoded_mutex_);
            requested_decode_choice_ = decoder.toStdString();
        }
        decode_wanted_.store(true, std::memory_order_release);
    }
    place_startup_receiver();
}

void EngineLink::place_startup_receiver()
{
    if (!startup_pending_ || !connected_ || !sourceOpen()) {
        return;
    }
    startup_pending_ = false;

    // Said on stderr and not in the window: this is a command line asking for
    // something the source cannot give, and the person who typed it is
    // reading the console.
    if (startup_hz_ < spanLowHz() || startup_hz_ > spanHighHz()) {
        std::fprintf(stderr,
                     "--receiver %.0f Hz is outside the span this source covers, %.0f to %.0f "
                     "Hz, so no receiver was opened\n",
                     startup_hz_, spanLowHz(), spanHighHz());
        return;
    }
    tuneReceiver(startup_hz_, startup_mode_);
}

// ---------------------------------------------------------------------------
// The Cap'n Proto event loop thread
// ---------------------------------------------------------------------------

void EngineLink::on_decoded_message(const rpc::DecodedMessage& message)
{
    const std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
    {
        const std::lock_guard<std::mutex> lock(decoded_mutex_);

        // Capped at the log's own size. A GUI thread stalled long enough to
        // fill this has lost the oldest either way, and the log counts them
        // with the lines its cap let go.
        if (pending_decoded_.size() >= kDecodedLogCapacity) {
            pending_decoded_.erase(pending_decoded_.begin());
            pending_decoded_arrived_ms_.erase(pending_decoded_arrived_ms_.begin());
            ++pending_decoded_unkept_;
        }
        pending_decoded_.push_back(message);
        pending_decoded_arrived_ms_.push_back(now_ms);
    }

    // One wake outstanding at a time, for the reason wake_pending_ gives.
    if (!decoded_wake_pending_.exchange(true, std::memory_order_acq_rel)) {
        QMetaObject::invokeMethod(this, [this] { drain_decoded(); }, Qt::QueuedConnection);
    }
}

void EngineLink::on_decoded_ended(std::uint64_t vrx, const std::string& decoder,
                                  const std::string& reason)
{
    {
        const std::lock_guard<std::mutex> lock(decoded_mutex_);
        pending_decoded_ended_.push_back(DecodedEnded{vrx, decoder, reason});
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        decode_work_pending_ = true;
    }
    supervisor_wake_.notify_all();
}

// ---------------------------------------------------------------------------
// The supervisor thread
// ---------------------------------------------------------------------------

void EngineLink::stop_decoded()
{
    if (client_ != nullptr) {
        // Cancelled by name before the receiver goes, which is what keeps
        // ended() about somebody else's removal. See stop_audio.
        for (const std::string& name : live_decoded_) {
            client_->unsubscribe_decoded(live_decoded_vrx_, name);
        }
    }
    live_decoded_.clear();
    live_decoded_vrx_ = 0;
}

void EngineLink::forget_decoded()
{
    // Nothing is cancelled: the engine, or its source, took every
    // subscription with it.
    live_decoded_.clear();
    live_decoded_vrx_ = 0;
    decode_ended_vrx_ = 0;
    work_decode_ended_.clear();
    decode_refusals_.clear();
    decode_tried_vrx_ = 0;
    decode_tried_choice_.clear();
    decoder_infos_asked_ = false;
    decoder_infos_fault_.clear();
    {
        const std::lock_guard<std::mutex> lock(decoded_mutex_);
        pending_decoded_ended_.clear();
    }
    note_decode();
}

void EngineLink::apply_decode_request()
{
    // Cleared before the work, on apply_audio_request's argument.
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        decode_work_pending_ = false;
    }
    if (client_ == nullptr) {
        return;
    }

    std::vector<DecodedEnded> ended;
    std::string choice;
    {
        const std::lock_guard<std::mutex> lock(decoded_mutex_);
        ended.swap(pending_decoded_ended_);
        choice = requested_decode_choice_;
    }

    // Ended arrivals first, because they change what is held. The client has
    // already forgotten the subscription, so there is nothing to cancel.
    for (const DecodedEnded& gone : ended) {
        if (gone.vrx == live_decoded_vrx_) {
            std::erase(live_decoded_, gone.decoder);
            if (live_decoded_.empty()) {
                live_decoded_vrx_ = 0;
            }
        }
        decode_ended_vrx_ = gone.vrx;
        work_decode_ended_ =
            QStringLiteral("%1: %2")
                .arg(QString::fromStdString(gone.decoder),
                     gone.reason.empty()
                         ? QStringLiteral("the engine ended the stream and gave no reason")
                         : QString::fromStdString(gone.reason));
    }

    // The list, once per connection. A failure is said rather than retried
    // every pass: an engine that cannot list its decoders will not start on
    // the next quarter second.
    if (!decoder_infos_asked_) {
        decoder_infos_asked_ = true;
        auto listed = client_->decoders();
        if (listed) {
            work_decoder_infos_ = std::move(*listed);
            decoder_infos_fault_.clear();
        } else {
            work_decoder_infos_.clear();
            decoder_infos_fault_ =
                QStringLiteral("the engine did not list its decoders: %1")
                    .arg(QString::fromStdString(listed.error().message));
        }
        {
            const std::lock_guard<std::mutex> lock(decoded_mutex_);
            handover_decoder_infos_ = work_decoder_infos_;
            handover_has_infos_ = true;
            has_decode_handover_ = true;
        }
        QMetaObject::invokeMethod(this, [this] { adopt_decode(); }, Qt::QueuedConnection);
    }

    const qulonglong vrx = live_receiver_id_;
    const bool wanted = decode_wanted_.load(std::memory_order_acquire);
    const std::string mode = demod_name(live_receiver_demod_).toStdString();

    std::vector<std::string> target;
    if (wanted && vrx != 0 && vrx != decode_ended_vrx_) {
        target = resolve_decoder_choice(choice, work_decoder_infos_, mode);
    }

    // Held on a receiver the pane has left.
    if (live_decoded_vrx_ != 0 && live_decoded_vrx_ != vrx) {
        stop_decoded();
    }

    // A refusal is an answer about one receiver, one mode and one choice, and
    // is asked again when any of them moves or the switch goes round.
    const std::string tried = choice + "@" + mode;
    if (!wanted || vrx != decode_tried_vrx_ || tried != decode_tried_choice_) {
        decode_refusals_.clear();
        decode_tried_vrx_ = vrx;
        decode_tried_choice_ = tried;
    }

    for (auto held = live_decoded_.begin(); held != live_decoded_.end();) {
        if (std::ranges::find(target, *held) == target.end()) {
            client_->unsubscribe_decoded(live_decoded_vrx_, *held);
            held = live_decoded_.erase(held);
        } else {
            ++held;
        }
    }
    if (live_decoded_.empty()) {
        live_decoded_vrx_ = 0;
    }

    for (const std::string& name : target) {
        const bool held = std::ranges::find(live_decoded_, name) != live_decoded_.end();
        const bool refused = std::ranges::any_of(
            decode_refusals_, [&name](const auto& entry) { return entry.first == name; });
        if (held || refused) {
            continue;
        }

        // Subscribed by name and never by the empty name, so the pair the
        // client holds it under is the pair the unsubscribe above passes.
        auto resolved = client_->subscribe_decoded(
            vrx, name, [this](const rpc::DecodedMessage& message) { on_decoded_message(message); },
            [this, vrx, name](const std::string& why) { on_decoded_ended(vrx, name, why); });
        if (!resolved) {
            decode_refusals_.emplace_back(name, resolved.error().message);
            continue;
        }
        live_decoded_vrx_ = vrx;
        live_decoded_.push_back(name);
        work_decode_ended_.clear();
    }

    note_decode();
}

void EngineLink::note_decode()
{
    const QString attached = joined(live_decoded_);

    QString label;
    QString detail;
    if (!decoder_infos_fault_.isEmpty()) {
        label = QStringLiteral("no decoder list");
        detail = decoder_infos_fault_;
    } else if (!decode_refusals_.empty()) {
        label = decode_refusals_.size() == 1
                    ? QStringLiteral("%1 refused")
                          .arg(QString::fromStdString(decode_refusals_.front().first))
                    : QStringLiteral("%1 refused").arg(decode_refusals_.size());
        for (const auto& [name, sentence] : decode_refusals_) {
            if (!detail.isEmpty()) {
                detail += QStringLiteral("\n\n");
            }
            detail += QStringLiteral("%1: %2").arg(QString::fromStdString(name),
                                                   QString::fromStdString(sentence));
        }
    } else if (!work_decode_ended_.isEmpty()) {
        label = QStringLiteral("stream ended");
        detail = work_decode_ended_;
    }

    if (attached == posted_decode_attached_ && label == posted_decode_label_ &&
        detail == posted_decode_detail_) {
        return;
    }
    posted_decode_attached_ = attached;
    posted_decode_label_ = label;
    posted_decode_detail_ = detail;
    {
        const std::lock_guard<std::mutex> lock(decoded_mutex_);
        has_decode_handover_ = true;
        handover_decode_attached_ = attached;
        handover_decode_label_ = label;
        handover_decode_detail_ = detail;
    }
    QMetaObject::invokeMethod(this, [this] { adopt_decode(); }, Qt::QueuedConnection);
}

}  // namespace revenant::ui
