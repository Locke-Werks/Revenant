// EngineLink's speech to text half: the switch, its status, each receiver's
// choice, and the transcripts, which go to the decode log and to the span
// waterfall's captions.
//
// Split out the way decoded_link.cpp is, and shaped like it: the same three
// threads, the same reconcile. models/transcription.h holds the rules, with
// cases in ui/tests/test_transcription.cpp; render/caption_layout.h holds the
// captions'.
//
// THE THREE THREADS, FOR THIS FILE SPECIFICALLY
//
//   The SUPERVISOR owns the Client, so it alone calls set_transcription,
//   transcription_status, set_vrx_transcribe and subscribe_transcripts.
//   apply_transcription, forget_transcription and note_transcription are its.
//
//   The CAP'N PROTO EVENT LOOP runs on_transcript and on_transcripts_ended.
//   The first copies the transcript into a capped hand-off and posts one
//   wake; the second records the engine's words and wakes the supervisor,
//   because core/rpc/client.h forbids calling back into the Client from
//   there.
//
//   The QT thread runs the switch, the choices, adopt_transcription and
//   drain_transcripts, and owns the caption feed and the log.
//
// AN ENGINE THAT DOES NOT TRANSCRIBE IS NOT A FAULT. One built before the
// schema's 2026-10-03 block refuses all four calls as unimplemented, which
// core/rpc/client.cpp reports as ErrorCategory::Unimplemented. The first such
// refusal marks the connection as not offering speech to text: the switch is
// greyed and says so on hover, nothing is asked again until the next
// connection, and nothing reaches the status pill or a dialog. The engine
// side was not built when this was written, so that is the path every engine
// on this machine takes today.

#include "models/engine_link.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <QDateTime>
#include <QMetaObject>
#include <QSettings>

#include "models/settings.h"
#include "render/caption_layout.h"

namespace revenant::ui {
namespace {

[[nodiscard]] bool unimplemented(const Error& failure)
{
    return failure.category == ErrorCategory::Unimplemented;
}

// The test feed's pace and shape: a transcript every three and a half
// seconds, two and a half seconds long, that ended a second and a half before
// the newest row, which is about what a real one looks like by the time
// Whisper has finished with it.
constexpr int kFakeTranscriptIntervalMs = 3500;
constexpr double kFakeUtteranceSeconds = 2.5;
constexpr double kFakeLatencySeconds = 1.5;

}  // namespace

// ---------------------------------------------------------------------------
// The Qt thread
// ---------------------------------------------------------------------------

QString EngineLink::transcriptionTone() const
{
    const std::string_view name = transcription_tone_name(transcription_view_.tone);
    return QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
}

QString EngineLink::receiverTranscribe() const
{
    const std::string_view name = transcribe_choice_name(receiver_status_.transcribe);
    return QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
}

QString EngineLink::transcribeNote(const QString& choice, const QString& mode, bool on,
                                   bool transcribing) const
{
    const rpc::TranscribeChoice parsed =
        transcribe_choice_from_name(choice.toStdString()).value_or(rpc::TranscribeChoice::Auto);
    return QString::fromStdString(
        receiver_transcribe_note(parsed, mode.toStdString(), on, transcribing));
}

void EngineLink::toggleTranscription()
{
    // An engine that refused the calls has nothing to switch, and the key
    // would otherwise flip a remembered setting nobody can see take effect.
    if (connected_ && !transcription_view_.offered) {
        return;
    }
    const bool next = !transcription_view_.on;
    transcription_wanted_ = next;
    transcription_set_ = true;
    if (remember_transcription_) {
        QSettings().setValue(settings::kTranscription, next);
    }
    {
        const std::lock_guard<std::mutex> lock(transcription_mutex_);
        requested_transcription_ = next;
        transcription_switch_pending_ = true;
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        transcribe_work_pending_ = true;
    }
    supervisor_wake_.notify_all();

    // Drawn at once, the way the operator set it, until the engine's answer
    // lands a round trip later. A switch that waited for the engine before
    // moving reads as a click that missed.
    if (transcription_view_.on != next) {
        transcription_view_.on = next;
        emit transcriptionChanged();
    }
}

void EngineLink::setReceiverTranscribe(const QString& choice)
{
    const auto parsed = transcribe_choice_from_name(choice.toStdString());
    if (!parsed || pane_key_ == 0) {
        return;
    }
    const auto held = std::find_if(transcribe_choices_.begin(), transcribe_choices_.end(),
                                   [this](const auto& one) { return one.first == pane_key_; });
    if (held == transcribe_choices_.end()) {
        transcribe_choices_.emplace_back(pane_key_, *parsed);
    } else {
        held->second = *parsed;
    }
    post_transcribe_wants();
}

void EngineLink::setRememberTranscription(bool remember)
{
    remember_transcription_ = remember;
    if (remember) {
        return;
    }
    // Back to what a first run has, so a smoke run sends nothing the operator
    // set and draws the switch the same on every machine.
    transcription_wanted_ = false;
    transcription_set_ = false;
    {
        const std::lock_guard<std::mutex> lock(transcription_mutex_);
        requested_transcription_ = false;
        transcription_switch_pending_ = false;
    }
    transcription_view_ = transcription_view(TranscriptionFacts{});
    emit transcriptionChanged();
}

void EngineLink::post_transcribe_wants()
{
    // A choice outlives nothing: a receiver taken out of the rack takes its
    // choice with it, as the engine forgets one when the receiver goes.
    std::erase_if(transcribe_choices_,
                  [this](const auto& one) { return rack_.find(one.first) == nullptr; });

    std::vector<VrxTranscribeWant> wants;
    wants.reserve(rack_.size());
    for (const RackEntry& entry : rack_.entries()) {
        const std::uint64_t id = entry.key == pane_key_ ? receiver_id_ : entry.engine_id;

        // Not an adopted receiver's: whether a plugin's receiver is
        // transcribed is the plugin's to say, and a choice sent from here
        // would be a write to a receiver this window does not own. It is
        // never focused, so the operator has no control that sets one anyway.
        if (id == 0 || entry.adopted) {
            continue;
        }
        VrxTranscribeWant want;
        want.vrx = id;
        for (const auto& [key, choice] : transcribe_choices_) {
            if (key == entry.key) {
                want.choice = choice;
            }
        }
        wants.push_back(want);
    }

    // Compared first, because this hangs off rackChanged and that fires on
    // every level the rack's meters read.
    if (wants == posted_transcribe_wants_) {
        return;
    }
    posted_transcribe_wants_ = wants;
    {
        const std::lock_guard<std::mutex> lock(transcription_mutex_);
        requested_transcribe_wants_ = std::move(wants);
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        transcribe_work_pending_ = true;
    }
    supervisor_wake_.notify_all();
}

void EngineLink::adopt_transcription()
{
    TranscriptionFacts facts;
    {
        const std::lock_guard<std::mutex> lock(transcription_mutex_);
        if (!has_transcription_handover_) {
            return;
        }
        has_transcription_handover_ = false;
        facts = handover_transcription_;
    }
    // The Qt thread's own switch, which may be newer than the one the
    // supervisor read: it is what is drawn until the engine has said.
    facts.wanted = transcription_wanted_;
    const TranscriptionView next = transcription_view(facts);
    if (next == transcription_view_) {
        return;
    }
    transcription_view_ = next;
    emit transcriptionChanged();
}

void EngineLink::drain_transcripts()
{
    // Cleared before the swap, on drain_decoded's argument.
    transcript_wake_pending_.store(false, std::memory_order_release);

    std::vector<rpc::Transcript> transcripts;
    std::vector<std::int64_t> arrived;
    std::uint64_t unkept = 0;
    {
        const std::lock_guard<std::mutex> lock(transcription_mutex_);
        transcripts.swap(pending_transcripts_);
        arrived.swap(pending_transcripts_arrived_ms_);
        unkept = std::exchange(pending_transcripts_unkept_, 0);
    }
    if (transcripts.empty() && unkept == 0) {
        return;
    }

    const auto now_ms = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    recently_adopted_.prune(now_ms);

    std::vector<DecodedLine> lines;
    lines.reserve(transcripts.size());
    for (std::size_t i = 0; i < transcripts.size(); ++i) {
        const rpc::Transcript& transcript = transcripts[i];

        // The rack entry the receiver is, now: its colour ties the caption
        // and the log line to its strip, and its name heads the hover card. A
        // receiver that has left the rack keeps its text and loses its colour.
        int slot = -1;
        std::string label;
        for (const RackEntry& entry : rack_.entries()) {
            const std::uint64_t id = entry.key == pane_key_ ? receiver_id_ : entry.engine_id;
            if (id != 0 && id == transcript.vrx) {
                slot = static_cast<int>(entry.slot);
                label = rack_entry_label(entry);
            }
        }

        // EXCEPT A PLUGIN'S, which has usually left already. The trunk tracker
        // closes a call's receiver when the call ends and Whisper answers a
        // couple of seconds after the speech does, so without this nearly
        // every trunk transcript would arrive colourless and unnamed. See
        // kAdoptedMemoryMs.
        if (slot < 0) {
            if (const auto gone = recently_adopted_.find(transcript.vrx, now_ms)) {
                slot = static_cast<int>(gone->slot);
                label = gone->label;
            }
        }

        const QString when = QDateTime::fromMSecsSinceEpoch(arrived[i])
                                 .toString(QStringLiteral("HH:mm:ss.zzz"));
        lines.push_back(make_transcript_line(transcript, decoded_log_.take_serial(),
                                             when.toStdString(), slot));

        CaptionFeedEntry entry;
        entry.serial = ++caption_serial_;
        entry.transcript = transcript;
        entry.slot = slot;
        entry.card = transcript_card_text(transcript, label);
        caption_feed_.push_back(std::move(entry));
    }
    while (caption_feed_.size() > kCaptionLimit) {
        caption_feed_.pop_front();
    }
    decoded_log_.append(std::move(lines), unkept);
    if (!transcripts.empty()) {
        emit transcriptsArrived();
    }
}

void EngineLink::startFakeTranscripts()
{
    fake_transcripts_.start(kFakeTranscriptIntervalMs);
}

void EngineLink::feed_fake_transcript()
{
    const rpc::SpectrumFrame& newest = frame();
    const auto rate = static_cast<std::uint64_t>(std::max(sourceRate(), 0));
    if (!connected_ || newest.count == 0 || rate == 0) {
        return;
    }
    const std::uint64_t now = newest.start + newest.count;
    const auto latency = static_cast<std::uint64_t>(kFakeLatencySeconds * static_cast<double>(rate));
    const auto length =
        static_cast<std::uint64_t>(kFakeUtteranceSeconds * static_cast<double>(rate));
    if (now <= latency + length) {
        return;
    }

    // Round the rack, so every receiver's colour and passband is exercised,
    // and the span's centre when the rack is empty.
    const std::vector<RackMarker> markers = rackMarkers();
    rpc::Transcript fake;
    fake.sequence = fake_transcript_count_;
    fake.source_end = now - latency;
    fake.source_start = fake.source_end - length;
    fake.start_sample = fake.source_start;
    fake.end_sample = fake.source_end;
    fake.sample_rate = static_cast<std::uint32_t>(rate);
    fake.latency_ms = kFakeLatencySeconds * 1000.0;
    if (markers.empty()) {
        const double centre = (spanLowHz() + spanHighHz()) / 2.0;
        fake.center_hz = static_cast<std::int64_t>(std::llround(centre));
        fake.low_hz = fake.center_hz - 6250;
        fake.high_hz = fake.center_hz + 6250;
        fake.mode = "nfm";
    } else {
        const RackMarker& marker = markers[fake_transcript_count_ % markers.size()];
        fake.center_hz = static_cast<std::int64_t>(std::llround(marker.band.center_hz));
        fake.low_hz = static_cast<std::int64_t>(std::llround(marker.band.low_hz));
        fake.high_hz = static_cast<std::int64_t>(std::llround(marker.band.high_hz));
        const RackEntry* entry = rack_.find(marker.key);
        if (entry != nullptr) {
            fake.vrx = entry->key == pane_key_ ? receiver_id_ : entry->engine_id;
        }
        if (entry != nullptr && entry->key == pane_key_) {
            fake.mode = demod_name(wanted_.demod).toStdString();
        } else if (const HeldView* view = held_view(marker.key); view != nullptr) {
            fake.mode = demod_name(view->params.demod).toStdString();
        }
    }

    // Short, long and doubtful in turn, so wrapping, the ellipsis and the dim
    // line all show; every one says it is a test, so a screenshot of it is
    // never mistaken for something heard on the air.
    static constexpr std::array<std::string_view, 4> kLines = {
        "test caption, fed by --fake-transcripts",
        "test caption: this one runs long on purpose so it wraps onto a second line and then "
        "a third, and is cut with an ellipsis where the caption ends and the hover card "
        "carries on",
        "test caption, doubtful, drawn dim",
        "test caption on a talkgroup",
    };
    const std::size_t which = fake_transcript_count_ % kLines.size();
    fake.text = std::string(kLines[which]);
    fake.confidence = which == 2 ? 0.3F : 0.9F;
    fake.no_speech_prob = 0.05F;
    if (which == 3) {
        fake.fields.push_back(rpc::DecodedField{"talkgroup", std::int64_t{1201}});
    }
    ++fake_transcript_count_;

    {
        const std::lock_guard<std::mutex> lock(transcription_mutex_);
        pending_transcripts_.push_back(std::move(fake));
        pending_transcripts_arrived_ms_.push_back(QDateTime::currentMSecsSinceEpoch());
    }
    drain_transcripts();
}

// ---------------------------------------------------------------------------
// The Cap'n Proto event loop thread
// ---------------------------------------------------------------------------

void EngineLink::on_transcript(const rpc::Transcript& transcript)
{
    const std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
    {
        const std::lock_guard<std::mutex> lock(transcription_mutex_);

        // Capped at the log's own size, for the reason on_decoded_message
        // gives; the log counts what this lets go.
        if (pending_transcripts_.size() >= kDecodedLogCapacity) {
            pending_transcripts_.erase(pending_transcripts_.begin());
            pending_transcripts_arrived_ms_.erase(pending_transcripts_arrived_ms_.begin());
            ++pending_transcripts_unkept_;
        }
        pending_transcripts_.push_back(transcript);
        pending_transcripts_arrived_ms_.push_back(now_ms);
    }
    if (!transcript_wake_pending_.exchange(true, std::memory_order_acq_rel)) {
        QMetaObject::invokeMethod(this, [this] { drain_transcripts(); }, Qt::QueuedConnection);
    }
}

void EngineLink::on_transcripts_ended(const std::string& reason)
{
    {
        const std::lock_guard<std::mutex> lock(transcription_mutex_);
        transcripts_ended_ = true;
        transcripts_ended_reason_ = reason;
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        transcribe_work_pending_ = true;
    }
    supervisor_wake_.notify_all();
}

// ---------------------------------------------------------------------------
// The supervisor thread
// ---------------------------------------------------------------------------

void EngineLink::forget_transcription()
{
    // Nothing is cancelled: the engine took the subscription with it, and it
    // forgets every choice with the receivers it was set on.
    transcripts_subscribed_ = false;
    transcription_switch_failed_ = false;
    vrx_transcribe_sent_.forget();
    work_transcription_ = TranscriptionFacts{};
    {
        const std::lock_guard<std::mutex> lock(transcription_mutex_);
        transcripts_ended_ = false;
        transcripts_ended_reason_.clear();

        // The switch again on the next connection, once there is one to send:
        // a restarted engine comes up with it off.
        if (transcription_switch_sent_) {
            transcription_switch_pending_ = true;
        }
        work_transcription_.wanted = requested_transcription_;
    }
    note_transcription();
}

void EngineLink::apply_transcription(bool probe)
{
    // Cleared before the work, on apply_audio_request's argument.
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        transcribe_work_pending_ = false;
    }
    if (client_ == nullptr) {
        return;
    }

    bool switch_pending = false;
    bool wanted = false;
    bool ended = false;
    std::string ended_reason;
    std::vector<VrxTranscribeWant> wants;
    {
        const std::lock_guard<std::mutex> lock(transcription_mutex_);
        switch_pending = transcription_switch_pending_;
        wanted = requested_transcription_;
        ended = std::exchange(transcripts_ended_, false);
        ended_reason = transcripts_ended_reason_;
        wants = requested_transcribe_wants_;
    }
    work_transcription_.connected = true;
    work_transcription_.wanted = wanted;

    const auto refuse_all = [this](const Error& failure) {
        work_transcription_.unsupported = true;
        work_transcription_.unsupported_reason = failure.message;
        work_transcription_.known = false;
        work_transcription_.fault.clear();
    };

    if (ended) {
        // Not asked for again until the switch goes round, which is the
        // operator asking: an engine that ends the stream once will end the
        // next one for the same reason, and resubscribing every pass would
        // write its sentence over itself four times a second.
        transcripts_subscribed_ = false;
        work_transcription_.stream_ended =
            ended_reason.empty() ? std::string("it gave no reason.") : ended_reason;
    }

    if (work_transcription_.unsupported) {
        note_transcription();
        return;
    }

    // THE SWITCH. On a new connection when the operator has ever set it, and
    // whenever they move it; after a failure that was not a refusal, once a
    // second rather than four times.
    if (switch_pending && (probe || !transcription_switch_failed_)) {
        auto answer = client_->set_transcription(wanted);
        if (!answer) {
            if (unimplemented(answer.error())) {
                refuse_all(answer.error());
                note_transcription();
                return;
            }
            transcription_switch_failed_ = true;
            work_transcription_.fault = answer.error().message;
        } else {
            transcription_switch_failed_ = false;
            transcription_switch_sent_ = true;
            work_transcription_.status = *answer;
            work_transcription_.known = true;
            work_transcription_.fault.clear();
            work_transcription_.stream_ended.clear();
            const std::lock_guard<std::mutex> lock(transcription_mutex_);
            // Only when nothing newer was asked for meanwhile; otherwise the
            // next pass sends that.
            if (requested_transcription_ == wanted) {
                transcription_switch_pending_ = false;
            }
        }
    }

    // ONE SUBSCRIPTION PER CONNECTION, whether or not the switch is on: the
    // switch is engine-wide, and another session turning it on should put
    // text on this waterfall too.
    if (!transcripts_subscribed_ && work_transcription_.stream_ended.empty() &&
        (probe || !work_transcription_.known)) {
        auto subscribed = client_->subscribe_transcripts(
            [this](const rpc::Transcript& transcript) { on_transcript(transcript); },
            [this](const std::string& why) { on_transcripts_ended(why); });
        if (!subscribed) {
            if (unimplemented(subscribed.error())) {
                refuse_all(subscribed.error());
                note_transcription();
                return;
            }
            work_transcription_.fault = subscribed.error().message;
        } else {
            transcripts_subscribed_ = true;
        }
    }

    // EACH RECEIVER'S CHOICE, reconciled. See VrxTranscribeSent.
    vrx_transcribe_sent_.prune(wants);
    for (const VrxTranscribeWant& want : vrx_transcribe_sent_.plan(wants)) {
        auto sent = client_->set_vrx_transcribe(want.vrx, want.choice);
        if (!sent && unimplemented(sent.error())) {
            refuse_all(sent.error());
            note_transcription();
            return;
        }
        vrx_transcribe_sent_.note(want);
    }

    // THE STATUS, once a second, which is as often as a download's
    // percentage is worth redrawing. Not on the other passes: nothing here
    // acts on it, and three more round trips a second would buy nothing.
    if (probe) {
        auto status = client_->transcription_status();
        if (!status) {
            if (unimplemented(status.error())) {
                refuse_all(status.error());
            } else {
                work_transcription_.fault = status.error().message;
            }
        } else {
            work_transcription_.status = *status;
            work_transcription_.known = true;
            if (!transcription_switch_failed_ && transcripts_subscribed_) {
                work_transcription_.fault.clear();
            }
        }
    }
    note_transcription();
}

void EngineLink::note_transcription()
{
    if (transcription_posted_ && work_transcription_ == posted_transcription_) {
        return;
    }
    transcription_posted_ = true;
    posted_transcription_ = work_transcription_;
    {
        const std::lock_guard<std::mutex> lock(transcription_mutex_);
        handover_transcription_ = work_transcription_;
        has_transcription_handover_ = true;
    }
    QMetaObject::invokeMethod(this, [this] { adopt_transcription(); }, Qt::QueuedConnection);
}

}  // namespace revenant::ui
