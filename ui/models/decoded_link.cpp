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

// ---------------------------------------------------------------------------
// Vocoder plugins
// ---------------------------------------------------------------------------

void EngineLink::poll_vocoder_plugins()
{
    // Once per connection. The engine scans at startup and not again, so a
    // second ask would only read the same answer; a plugin added to the
    // folder is reported after the engine restarts, which reconnects.
    if (client_ == nullptr || vocoders_asked_) {
        return;
    }
    vocoders_asked_ = true;

    auto listed = client_->vocoder_plugins();
    {
        const std::lock_guard<std::mutex> lock(decoded_mutex_);
        if (listed) {
            handover_vocoders_ = std::move(*listed);
            handover_vocoder_fault_.clear();
        } else {
            handover_vocoders_ = {};
            handover_vocoder_fault_ = QString::fromStdString(listed.error().message);
        }
        has_vocoder_handover_ = true;
    }
    QMetaObject::invokeMethod(this, [this] { adopt_vocoders(); }, Qt::QueuedConnection);
}

void EngineLink::adopt_vocoders()
{
    {
        const std::lock_guard<std::mutex> lock(decoded_mutex_);
        if (!has_vocoder_handover_) {
            return;
        }
        has_vocoder_handover_ = false;
        vocoders_ = std::move(handover_vocoders_);
        handover_vocoders_ = {};
        vocoder_fault_ = handover_vocoder_fault_;
    }
    vocoders_known_ = true;
    emit vocodersChanged();
}

QString EngineLink::vocoderStatus() const
{
    if (!connected_ || !vocoders_known_) {
        return {};
    }
    // An engine older than the call refuses it, and that is "not reported",
    // never "none": the operator's plugin may well be loaded there.
    if (!vocoder_fault_.isEmpty()) {
        return QStringLiteral("this engine does not report vocoder plugins: %1").arg(vocoder_fault_);
    }
    if (!vocoders_.scanned) {
        return QStringLiteral("this engine did not scan for vocoder plugins");
    }
    return QString::fromStdString(vocoders_.status);
}

QVariantList EngineLink::vocoderFiles() const
{
    QVariantList out;
    if (!connected_ || !vocoders_known_) {
        return out;
    }
    for (const rpc::VocoderPluginFile& file : vocoders_.files) {
        QStringList offers;
        for (const rpc::VocoderOfferInfo& offer : file.offers) {
            // Which modes it would play, or that none would: the line that says
            // why a mode is silent with a plugin loaded.
            QStringList serves;
            for (const std::string& mode : offer.modes) {
                serves.append(QString::fromStdString(mode));
            }
            offers.append(QStringLiteral("%1: %2, %3 bits in, %4 samples out at %5 Hz, %6")
                              .arg(QString::fromStdString(offer.name),
                                   QString::fromStdString(offer.kind))
                              .arg(offer.bit_count)
                              .arg(offer.pcm_frames)
                              .arg(offer.sample_rate)
                              .arg(serves.isEmpty()
                                       ? QStringLiteral("serves no mode")
                                       : QStringLiteral("serves ") + serves.join(QStringLiteral(", "))));
        }
        out.append(QVariantMap{
            {QStringLiteral("file"), QString::fromStdString(file.file)},
            {QStringLiteral("loaded"), file.loaded},
            {QStringLiteral("refusal"), QString::fromStdString(file.refusal)},
            {QStringLiteral("detail"), QString::fromStdString(file.detail)},
            {QStringLiteral("offers"), offers},
        });
    }
    return out;
}

int EngineLink::vocoderCount() const
{
    if (!connected_ || !vocoders_known_) {
        return 0;
    }
    int count = 0;
    for (const rpc::VocoderPluginFile& file : vocoders_.files) {
        count += static_cast<int>(file.offers.size());
    }
    return count;
}

// ---------------------------------------------------------------------------
// Engine plugins
// ---------------------------------------------------------------------------

void EngineLink::poll_engine_plugins()
{
    // Once per connection, for the reason poll_vocoder_plugins gives: the
    // engine scans its plugins folder at startup and not again.
    if (client_ == nullptr || plugins_asked_) {
        return;
    }
    plugins_asked_ = true;

    auto listed = client_->engine_plugins();
    {
        const std::lock_guard<std::mutex> lock(decoded_mutex_);
        if (listed) {
            handover_plugins_ = std::move(*listed);
            handover_plugin_fault_.clear();
        } else {
            handover_plugins_ = {};
            handover_plugin_fault_ = QString::fromStdString(listed.error().message);
        }
        has_plugin_handover_ = true;
    }
    QMetaObject::invokeMethod(this, [this] { adopt_plugins(); }, Qt::QueuedConnection);
}

void EngineLink::adopt_plugins()
{
    {
        const std::lock_guard<std::mutex> lock(decoded_mutex_);
        if (!has_plugin_handover_) {
            return;
        }
        has_plugin_handover_ = false;
        plugins_ = std::move(handover_plugins_);
        handover_plugins_ = {};
        plugin_fault_ = handover_plugin_fault_;
    }
    plugins_known_ = true;
    emit pluginsChanged();
}

QString EngineLink::pluginStatus() const
{
    if (!connected_ || !plugins_known_) {
        return {};
    }
    // Refused by an engine older than the call: "not reported", never "none".
    if (!plugin_fault_.isEmpty()) {
        return QStringLiteral("this engine does not report engine plugins: %1").arg(plugin_fault_);
    }
    if (!plugins_.scanned) {
        return QStringLiteral("this engine did not scan for engine plugins");
    }
    return QString::fromStdString(plugins_.status);
}

QVariantList EngineLink::pluginFiles() const
{
    QVariantList out;
    if (!connected_ || !plugins_known_) {
        return out;
    }
    for (const rpc::EnginePluginFile& file : plugins_.files) {
        out.append(QVariantMap{
            {QStringLiteral("file"), QString::fromStdString(file.file)},
            {QStringLiteral("name"), QString::fromStdString(file.name)},
            {QStringLiteral("version"), QString::fromStdString(file.version)},
            {QStringLiteral("loaded"), file.loaded},
            {QStringLiteral("running"), file.running},
            {QStringLiteral("refusal"), QString::fromStdString(file.refusal)},
            {QStringLiteral("detail"), QString::fromStdString(file.detail)},
        });
    }
    return out;
}

int EngineLink::pluginCount() const
{
    if (!connected_ || !plugins_known_) {
        return 0;
    }
    int count = 0;
    for (const rpc::EnginePluginFile& file : plugins_.files) {
        if (file.running) {
            ++count;
        }
    }
    return count;
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

    // The rest of the --receiver list, as held receivers in the rack. The
    // first keeps the focus, so --decode and the grab both describe it.
    for (const auto& [hz, mode] : startup_extra_) {
        if (hz < spanLowHz() || hz > spanHighHz()) {
            std::fprintf(stderr,
                         "--receiver %.0f Hz is outside the span this source covers, so no "
                         "receiver was opened there\n",
                         hz);
            continue;
        }
        const auto key = rack_.add();
        if (!key) {
            std::fputs("--receiver: the rack holds 64 receivers, and the rest were not "
                       "opened\n",
                       stderr);
            break;
        }
        rpc::VrxParams params;
        params.demod = wanted_.demod;
        if (auto parsed = demod_from_name(mode)) {
            params.demod = *parsed;
        }
        add_held_receiver(*key, hz, params);
    }
    startup_extra_.clear();
    post_audio_wants();
    emit rackChanged();
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

    // The call table reads the pane's subscriptions too, so a pair the pane
    // holds is never subscribed a second time for it. See apply_call_feeds.
    on_call_message(message);

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
        // ended() about somebody else's removal. See stop_audio_for.
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
    decode_ended_.clear();
    decode_refusals_.clear();
    decode_tried_vrx_ = 0;
    decode_tried_choice_.clear();
    decoder_infos_asked_ = false;
    decoder_infos_fault_.clear();
    vocoders_asked_ = false;
    plugins_asked_ = false;
    call_feeds_.clear();
    call_feeds_ended_.clear();
    posted_call_feed_count_ = 0;
    call_feed_count_.store(0, std::memory_order_release);
    QMetaObject::invokeMethod(this, [this] { call_log_.set_feeds(0); }, Qt::QueuedConnection);
    {
        const std::lock_guard<std::mutex> lock(decoded_mutex_);
        pending_decoded_ended_.clear();
        pending_call_ended_.clear();
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
    // already forgotten the subscription, so there is nothing to cancel, and
    // only that decoder is left off: the others on the receiver run on.
    for (DecodedEnded& gone : ended) {
        if (gone.vrx == live_decoded_vrx_) {
            std::erase(live_decoded_, gone.decoder);
            if (live_decoded_.empty()) {
                live_decoded_vrx_ = 0;
            }
        }
        decode_ended_.record(gone.vrx, std::move(gone.decoder), std::move(gone.reason));
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

    // The switch going round is the operator asking again, so a decoder
    // whose stream ended is tried once more. A removed receiver then says so
    // in the engine's words, as a refusal.
    if (!wanted) {
        decode_ended_.clear();
    }

    std::vector<std::string> target;
    if (wanted && vrx != 0) {
        target = decode_ended_.still_wanted(
            vrx, resolve_decoder_choice(choice, work_decoder_infos_, mode));
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
    } else if (const EndedChip chip = ended_chip(decode_ended_.on(live_receiver_id_));
               !chip.label.empty()) {
        // Only the decoders that ended, named, so a chip on a receiver still
        // decoding with six others says which one stopped.
        label = QString::fromStdString(chip.label);
        detail = QString::fromStdString(chip.detail);
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

// ---------------------------------------------------------------------------
// The call table's feeds
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] bool same_feed(const auto& a, const auto& b)
{
    return a.vrx == b.vrx && a.decoder == b.decoder;
}

[[nodiscard]] bool holds_feed(const auto& list, const auto& feed)
{
    return std::ranges::any_of(list, [&feed](const auto& known) { return same_feed(known, feed); });
}

// The call table's own pending queue. Far more than a drain ever finds, and
// a bound on what a stalled Qt thread can cost in memory.
constexpr std::size_t kPendingCallsCap = 4096;

}  // namespace

void EngineLink::apply_call_feeds()
{
    if (client_ == nullptr) {
        return;
    }

    std::vector<CallFeed> ended;
    {
        const std::lock_guard<std::mutex> lock(decoded_mutex_);
        ended.swap(pending_call_ended_);
    }
    // Only a feed still held here: one the pane replaced is the pane's now,
    // and an ended() from the replaced subscription says nothing about it.
    for (CallFeed& gone : ended) {
        if (!holds_feed(call_feeds_, gone)) {
            continue;
        }
        std::erase_if(call_feeds_, [&gone](const CallFeed& f) { return same_feed(f, gone); });
        call_feeds_ended_.push_back(std::move(gone));
    }

    // auto's choice for each digital voice receiver, whatever the pane's
    // decode switch says: the table is about the air, not about the pane.
    std::vector<CallFeed> wanted;
    const auto want = [&](qulonglong vrx, rpc::Demod demod) {
        if (vrx == 0 ||
            (demod != rpc::Demod::P25p1 && demod != rpc::Demod::Dmr && demod != rpc::Demod::Dstar)) {
            return;
        }
        for (std::string& name : resolve_decoder_choice(kAutoDecoder, work_decoder_infos_,
                                                        demod_name(demod).toStdString())) {
            if (is_call_decoder(name)) {
                wanted.push_back(CallFeed{vrx, std::move(name)});
            }
        }
    };
    want(live_receiver_id_, live_receiver_demod_);
    for (const HeldVrx& held : held_) {
        want(held.id, held.params.demod);
    }

    const auto pane_holds = [this](const CallFeed& feed) {
        return feed.vrx == live_decoded_vrx_ &&
               std::ranges::find(live_decoded_, feed.decoder) != live_decoded_.end();
    };

    // An ended feed is not asked again while its receiver keeps the mode.
    std::erase_if(call_feeds_ended_,
                  [&wanted](const CallFeed& f) { return !holds_feed(wanted, f); });

    for (auto held = call_feeds_.begin(); held != call_feeds_.end();) {
        if (pane_holds(*held)) {
            held = call_feeds_.erase(held);
        } else if (!holds_feed(wanted, *held)) {
            client_->unsubscribe_decoded(held->vrx, held->decoder);
            held = call_feeds_.erase(held);
        } else {
            ++held;
        }
    }

    int feeding = 0;
    for (const CallFeed& feed : wanted) {
        if (pane_holds(feed) || holds_feed(call_feeds_, feed)) {
            ++feeding;
            continue;
        }
        if (holds_feed(call_feeds_ended_, feed)) {
            continue;
        }
        auto resolved = client_->subscribe_decoded(
            feed.vrx, feed.decoder,
            [this](const rpc::DecodedMessage& message) { on_call_message(message); },
            [this, vrx = feed.vrx, name = feed.decoder](const std::string&) {
                on_call_feed_ended(vrx, name);
            });
        if (!resolved) {
            call_feeds_ended_.push_back(feed);
            continue;
        }
        call_feeds_.push_back(feed);
        ++feeding;
    }

    if (feeding != posted_call_feed_count_) {
        posted_call_feed_count_ = feeding;
        call_feed_count_.store(feeding, std::memory_order_release);
        QMetaObject::invokeMethod(
            this,
            [this] { call_log_.set_feeds(call_feed_count_.load(std::memory_order_acquire)); },
            Qt::QueuedConnection);
    }
}

void EngineLink::on_call_message(const rpc::DecodedMessage& message)
{
    if (!is_call_decoder(message.decoder)) {
        return;
    }
    const std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
    {
        const std::lock_guard<std::mutex> lock(decoded_mutex_);
        if (pending_calls_.size() >= kPendingCallsCap) {
            pending_calls_.erase(pending_calls_.begin());
            pending_calls_arrived_ms_.erase(pending_calls_arrived_ms_.begin());
        }
        pending_calls_.push_back(message);
        pending_calls_arrived_ms_.push_back(now_ms);
    }
    if (!calls_wake_pending_.exchange(true, std::memory_order_acq_rel)) {
        QMetaObject::invokeMethod(this, [this] { drain_calls(); }, Qt::QueuedConnection);
    }
}

void EngineLink::on_call_feed_ended(std::uint64_t vrx, const std::string& decoder)
{
    {
        const std::lock_guard<std::mutex> lock(decoded_mutex_);
        pending_call_ended_.push_back(CallFeed{vrx, decoder});
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        decode_work_pending_ = true;
    }
    supervisor_wake_.notify_all();
}

void EngineLink::drain_calls()
{
    calls_wake_pending_.store(false, std::memory_order_release);

    std::vector<rpc::DecodedMessage> messages;
    std::vector<std::int64_t> arrived;
    {
        const std::lock_guard<std::mutex> lock(decoded_mutex_);
        messages.swap(pending_calls_);
        arrived.swap(pending_calls_arrived_ms_);
    }

    std::vector<CallFrame> frames;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const rpc::DecodedMessage& message = messages[i];

        // The receiver's frequency and whose it is, which the message does
        // not carry. A receiver the rack has already let go keeps neither.
        std::int64_t hz = 0;
        std::string owner;
        for (const RackEntry& entry : rack_.entries()) {
            if (entry.engine_id != message.vrx) {
                continue;
            }
            owner = entry.adopted ? (entry.owner.empty() ? std::string("plugin") : entry.owner)
                                  : std::string("user");
            if (entry.key == pane_key_) {
                hz = receiver_absolute_hz_;
            } else if (const HeldView* view = held_view(entry.key); view != nullptr) {
                hz = view->absolute_hz;
            }
            break;
        }
        if (owner.empty() && message.vrx == receiver_id_) {
            owner = "user";
            hz = receiver_absolute_hz_;
        }
        for (CallFrame& frame : call_frames(message, arrived[i], hz, owner)) {
            frames.push_back(std::move(frame));
        }
    }
    call_log_.apply(frames);
}

}  // namespace revenant::ui
