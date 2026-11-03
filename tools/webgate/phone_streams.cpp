#include "tools/webgate/phone_streams.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <utility>
#include <variant>

#include "models/rds_text.h"
#include "models/trunk_aliases.h"
#include "tools/webgate/dispatch.h"
#include "tools/webgate/protocol.h"
#include "tools/webgate/spectrum_reducer.h"

namespace revenant::webgate {

namespace {

// The engine's default dispatch size. Only a first guess: EngineInfo does not
// carry the block size, so the first frame's sample count corrects it.
constexpr double kAssumedBlockSamples = 65'536.0;

// Chunks held between the client thread and the strand. The strand normally
// empties this within a millisecond; the cap only matters if it stalls.
constexpr std::size_t kMaxInboxChunks = 200;

constexpr auto kSourcePollInterval = std::chrono::seconds(1);

// A busy band must not flood the socket, so the strongest are kept.
constexpr std::size_t kMaxDetections = 200;

// Analog and digital-family labels name a modulation; a protocol label names
// what rides on one, the more specific answer, so every kind goes out and
// labelKind says which it is.
[[nodiscard]] std::string_view label_kind_name(rpc::LabelKind kind)
{
    switch (kind) {
    case rpc::LabelKind::AnalogModulation:
    case rpc::LabelKind::DigitalFamily:
        return "modulation";
    case rpc::LabelKind::Protocol:
        return "protocol";
    case rpc::LabelKind::Unknown:
        break;
    }
    return "";
}

[[nodiscard]] std::string detections_event(const std::vector<const rpc::Detection*>& items)
{
    std::string array = "[";
    for (const auto* d : items) {
        if (array.size() > 1) {
            array += ',';
        }
        // The id is a UInt64, which a JSON number cannot carry past 2^53.
        array += JsonObject()
                     .add_string("id", std::to_string(d->id))
                     .add_i64("centerHz", d->center_hz)
                     .add_i64("bandwidthHz", d->bandwidth_hz)
                     .add_double("snrDb", d->snr_2500_db)
                     .add_double("confidence", d->confidence)
                     .add_string("state", d->state == rpc::TrackState::Held ? "held" : "live")
                     .add_string("label", d->label.kind == rpc::LabelKind::Unknown
                                              ? std::string_view()
                                              : std::string_view(d->label.name))
                     .add_string("labelKind", label_kind_name(d->label.kind))
                     .add_double("labelConfidence", d->label.confidence)
                     .add_u64("lastSeen", d->last_seen)
                     .str();
    }
    array += ']';
    return event("detections", JsonObject().add_raw("items", array));
}

// PS and RadioText go out only once every segment has arrived: a partly
// received name is placeholders beside real characters, and the phone would
// show it, then put each fill-in into the feed as a new message.
[[nodiscard]] std::optional<std::string> complete_text(const std::vector<std::uint8_t>& bytes,
                                                       std::uint32_t mask, std::size_t per_segment,
                                                       std::size_t length)
{
    const auto text = ui::render_rds_text(bytes, mask, per_segment, length);
    if (text.segments_total == 0 || text.segments_received != text.segments_total ||
        text.text.empty()) {
        return std::nullopt;
    }
    return text.text;
}

[[nodiscard]] std::string rds_event(std::uint64_t vrx, const rpc::RdsStation& s)
{
    JsonObject out;
    out.add_u64("vrx", vrx);
    if (s.pi_valid) {
        out.add_u64("pi", s.pi);
    } else {
        out.add_null("pi");
    }
    if (s.call_sign.empty()) {
        out.add_null("callSign");
    } else {
        out.add_string("callSign", s.call_sign);
    }
    if (const auto ps = complete_text(s.ps, s.ps_received, 2, 8)) {
        out.add_string("ps", *ps);
    } else {
        out.add_null("ps");
    }
    const std::size_t rt_segment = s.rt_version_b ? 2 : 4;
    if (const auto rt = complete_text(s.rt, s.rt_received, rt_segment, s.rt_length)) {
        out.add_string("rt", *rt);
    } else {
        out.add_null("rt");
    }
    if (s.pty_valid && !s.pty_long_name.empty()) {
        out.add_string("pty", s.pty_long_name);
    } else {
        out.add_null("pty");
    }
    if (s.tp_valid) {
        out.add_bool("tp", s.tp);
    }
    if (s.ta_valid) {
        out.add_bool("ta", s.ta);
    }
    if (s.music_valid) {
        out.add_bool("music", s.music);
    }
    return event("rds", out);
}

[[nodiscard]] std::string hls_url(std::uint64_t vrx)
{
    return "/hls/" + std::to_string(vrx) + "/live.m3u8";
}

[[nodiscard]] std::string fields_json(const std::vector<rpc::DecodedField>& fields)
{
    JsonObject out;
    for (const auto& field : fields) {
        if (const auto* i = field.integer()) {
            out.add_i64(field.key, *i);
        } else if (const auto* d = field.real()) {
            out.add_double(field.key, *d);
        } else if (const auto* b = field.flag()) {
            out.add_bool(field.key, *b);
        } else if (const auto* t = field.text()) {
            out.add_string(field.key, *t);
        }
        // Byte fields are payloads (voice frames, image rows), not something a
        // phone shows as text, so they stay off the socket.
    }
    return out.str();
}

}  // namespace

struct PhoneStreams::Inbox {
    std::mutex mutex;
    Strand* strand = nullptr;
    bool closed = false;
    bool wake_pending = false;
    std::optional<rpc::SpectrumFrame> spectrum;
    std::uint64_t spectrum_skipped = 0;
    std::deque<std::pair<std::uint64_t, rpc::AudioChunk>> audio;
    // Chunks dropped here; each is about one packet's worth of audio lost, so
    // it is reported with the Opus drops.
    std::uint64_t audio_overflow = 0;
    std::deque<std::function<void(PhoneStreams&)>> items;

    // Strand only. Null once the streams are gone, so a wake-up already
    // queued on the strand finds nothing to call.
    PhoneStreams* owner = nullptr;
};

// Caller holds inbox->mutex.
void PhoneStreams::wake_locked(const std::shared_ptr<Inbox>& inbox)
{
    if (inbox->wake_pending || inbox->closed) {
        return;
    }
    inbox->wake_pending = true;
    inbox->strand->post([inbox] {
        if (inbox->owner != nullptr) {
            inbox->owner->drain_inbox();
        }
    });
}

void PhoneStreams::put_spectrum(const std::shared_ptr<Inbox>& inbox,
                                const rpc::SpectrumFrame& frame)
{
    const std::lock_guard lock(inbox->mutex);
    if (inbox->closed) {
        return;
    }
    if (inbox->spectrum) {
        ++inbox->spectrum_skipped;
    }
    inbox->spectrum = frame;
    wake_locked(inbox);
}

void PhoneStreams::put_audio(const std::shared_ptr<Inbox>& inbox, std::uint64_t vrx,
                             const rpc::AudioChunk& chunk)
{
    const std::lock_guard lock(inbox->mutex);
    if (inbox->closed) {
        return;
    }
    inbox->audio.emplace_back(vrx, chunk);
    while (inbox->audio.size() > kMaxInboxChunks) {
        inbox->audio.pop_front();
        ++inbox->audio_overflow;
    }
    wake_locked(inbox);
}

void PhoneStreams::put_item(const std::shared_ptr<Inbox>& inbox,
                            std::function<void(PhoneStreams&)> item)
{
    const std::lock_guard lock(inbox->mutex);
    if (inbox->closed) {
        return;
    }
    inbox->items.push_back(std::move(item));
    wake_locked(inbox);
}

PhoneStreams::PhoneStreams(PhoneSession& session, PhoneSocket* socket, Strand& strand,
                           StreamsConfig config)
    : session_(session),
      socket_(socket),
      strand_(strand),
      config_(std::move(config)),
      inbox_(std::make_shared<Inbox>()),
      alive_(std::make_shared<bool>(true))
{
    inbox_->strand = &strand_;
    inbox_->owner = this;
    // Process-wide, because the registry is shared by every phone.
    static std::atomic<std::uint64_t> next_hls_id{1};
    hls_id_ = next_hls_id.fetch_add(1);
}

PhoneStreams::~PhoneStreams()
{
    *alive_ = false;
    {
        const std::lock_guard lock(inbox_->mutex);
        inbox_->closed = true;
        inbox_->owner = nullptr;
    }
    auto& client = session_.client();
    if (spectrum_subscribed_) {
        client.unsubscribe_spectrum();
    }
    for (const auto vrx : audio_subscribed_) {
        client.unsubscribe_audio(vrx);
    }
    for (const auto& [vrx, names] : decoded_) {
        for (const auto& name : names) {
            client.unsubscribe_decoded(vrx, name);
        }
    }
    client.unsubscribe_vrx_events();
    client.unsubscribe_transcripts();
    if (config_.hls != nullptr && hls_) {
        config_.hls->remove(hls_id_);
    }
}

std::chrono::steady_clock::time_point PhoneStreams::now() const
{
    return config_.clock ? config_.clock() : std::chrono::steady_clock::now();
}

HlsSegmenter* PhoneStreams::segmenter(std::uint64_t id) const
{
    return hls_ && id == hls_id_ ? hls_->segmenter.get() : nullptr;
}

void PhoneStreams::deliver_spectrum(const rpc::SpectrumFrame& frame)
{
    put_spectrum(inbox_, frame);
}

void PhoneStreams::deliver_audio(std::uint64_t vrx, const rpc::AudioChunk& chunk)
{
    put_audio(inbox_, vrx, chunk);
}

// ---- Start, commands, ticks ------------------------------------------------

void PhoneStreams::start()
{
    voice_gain_db_ = config_.voice_gain_db ? config_.voice_gain_db() : 0.0;
    subscribe_vrx_events();

    auto inbox = inbox_;
    const auto transcripts = session_.client().subscribe_transcripts(
        [inbox](const rpc::Transcript& t) {
            put_item(inbox, [t](PhoneStreams& self) {
                // Engine-wide stream, limited to receivers the phone has been
                // told of, so a caption never names an id it cannot place.
                if (!self.known_.contains(t.vrx)) {
                    return;
                }
                self.queue_text(event("transcript", JsonObject()
                                                        .add_u64("vrx", t.vrx)
                                                        .add_u64("sequence", t.sequence)
                                                        .add_string("text", t.text)
                                                        .add_i64("centerHz", t.center_hz)
                                                        .add_i64("lowHz", t.low_hz)
                                                        .add_i64("highHz", t.high_hz)
                                                        .add_string("mode", t.mode)
                                                        .add_double("confidence", t.confidence)
                                                        .add_u64("sourceStart", t.source_start)
                                                        .add_u64("sourceEnd", t.source_end)));
            });
        },
        {});
    // An engine that does not transcribe refuses this; the phone just never
    // sees a transcript, which is what that engine means.
    static_cast<void>(transcripts);

    apply_desktop_switches();
    refresh_trunk_settings();
    apply_dv_switches();
    send_dv_state();
    refresh_source();
    send_receiver_list();
    refresh_transcription();
    refresh_display();
    refresh_desktop();
}

void PhoneStreams::apply_desktop_switches()
{
    if (!config_.desktop_settings) {
        return;
    }
    const DesktopSettings saved = config_.desktop_settings();
    auto& client = session_.client();
    // A refusal comes from an engine older than the call, which has nothing to
    // switch, so it is not an error for the phone. The desktop discards it
    // too; it is logged here because the gateway has no checkbox to show the
    // mismatch on.
    const auto note = [](const char* what, const Status& status) {
        if (!status) {
            std::fprintf(stderr, "revenant-webgate: the engine refused %s: %s\n", what,
                         status.error().message.c_str());
        }
    };
    if (saved.detection_threshold_db) {
        note("the desktop's detection threshold",
             client.set_detection_threshold(*saved.detection_threshold_db));
    }
    if (saved.impulse_excision) {
        note("impulse excision", client.set_impulse_excision(*saved.impulse_excision));
    }
    if (saved.comb_cancel) {
        note("comb cancellation", client.set_comb_cancel(*saved.comb_cancel));
    }
    if (saved.spur_cancel) {
        note("spur cancelling", client.set_spur_cancel(*saved.spur_cancel));
    }
    if (saved.transcribe) {
        const auto status = client.set_transcription(*saved.transcribe);
        if (!status) {
            std::fprintf(stderr, "revenant-webgate: the engine refused speech to text: %s\n",
                         status.error().message.c_str());
        }
    }
}

void PhoneStreams::refresh_desktop()
{
    if (!config_.desktop_settings) {
        return;
    }
    std::string now = desktop_event(config_.desktop_settings());
    if (now != sent_desktop_) {
        sent_desktop_ = now;
        queue_text(std::move(now));
    }
}

void PhoneStreams::send_receiver_list()
{
    // Read now rather than from the event replay, which arrives later and
    // one receiver at a time: the phone replaces its whole list with this,
    // so receivers removed while it had no session (taken over, a refused
    // resume) do not linger there as ghosts.
    const auto ids = session_.client().vrx_ids();
    if (!ids) {
        return;
    }
    for (const auto known : session_.receiver_ids()) {
        if (std::ranges::find(*ids, known) == ids->end()) {
            session_.forget(known);
            known_.erase(known);
            described_.erase(known);
        }
    }
    std::string items = "[";
    for (const auto id : *ids) {
        const auto status = session_.client().vrx_status(id);
        if (!status) {
            continue;
        }
        if (session_.has_receiver(id)) {
            session_.remember(id, status->params);
        } else {
            session_.learn(id, status->params, status->owned_by_caller);
        }
        known_.insert(id);
        auto& described = described_[id];
        described.status = *status;
        if (described.opened_by.empty()) {
            described.opened_by = status->owned_by_caller ? "this phone" : "another client";
        }
        if (items.size() > 1) {
            items += ',';
        }
        items += receiver_json(*status, source_center_, session_.decoders(id), described.opened_by);
    }
    items += ']';
    queue_text(event("vrxList", JsonObject().add_raw("items", items)));
    reconcile();
}

void PhoneStreams::refresh_display()
{
    display_.set_settings(config_.display_settings ? config_.display_settings()
                                                   : ui::DisplaySettings{});
    send_display_if_changed();
}

void PhoneStreams::send_display_if_changed()
{
    const std::string& now = display_.event();
    if (now != sent_display_) {
        sent_display_ = now;
        queue_text(now);
    }
}

void PhoneStreams::subscribe_vrx_events()
{
    auto inbox = inbox_;
    const auto subscribed = session_.client().subscribe_vrx_events(
        [inbox](const rpc::VrxEvent& e) {
            put_item(inbox, [e](PhoneStreams& self) { self.handle_vrx_event(e); });
        },
        [inbox](const std::string& reason) {
            // The engine ends a subscriber that fell behind rather than drop
            // an event. Subscribing again replays every receiver as Added,
            // which this ignores, and resumes the removals that matter. Once
            // only: an engine that keeps ending it is going away, and the
            // link events say so.
            put_item(inbox, [reason](PhoneStreams& self) {
                if (!self.vrx_events_resubscribed_) {
                    self.vrx_events_resubscribed_ = true;
                    self.subscribe_vrx_events();
                    return;
                }
                // A removal elsewhere will now go unreported to this phone;
                // say so where an operator and the phone can both see it.
                std::fprintf(stderr, "revenant-webgate: receiver events ended again: %s\n",
                             reason.c_str());
                self.queue_text(
                    event("vrxEventsEnded", JsonObject().add_string("reason", reason)));
            });
        });
    static_cast<void>(subscribed);
}

void PhoneStreams::on_text(std::string_view text)
{
    auto command = parse_command(text);
    if (!command) {
        if (const auto id = peek_id(text)) {
            queue_text(error_reply(*id, command.error().message));
        }
        return;
    }
    queue_text(dispatch(session_, *command));

    for (const auto vrx : session_.receiver_ids()) {
        known_.insert(vrx);
    }
    if (command->op == "openSource" || command->op == "setCenter") {
        refresh_source();
    }
    if (command->op == "autoDv" || command->op == "trunkFollow") {
        send_dv_state();
    }
    if (command->op == "autoDv") {
        // At once rather than at the next poll, so the switch answers with
        // the digital voice already on the span, as the desktop's does.
        last_detection_poll_.reset();
        refresh_detections();
    }
    if (command->op == "trunkFollow" || command->op == "dvTab" || command->op == "setTrunkRules") {
        next_trunk_poll_.reset();
        refresh_trunk();
    }
    if (command->op == "setTranscription") {
        // After the reply, so the phone sees its own answer first.
        refresh_transcription();
    }
    reconcile();
}

std::deque<std::string> PhoneStreams::take_texts()
{
    std::deque<std::string> out;
    out.swap(texts_);
    queued_text_bytes_ = 0;
    return out;
}

void PhoneStreams::resync()
{
    for (const auto vrx : session_.receiver_ids()) {
        known_.insert(vrx);
    }
    reconcile();
}

void PhoneStreams::send_event(std::string json)
{
    queue_text(std::move(json));
}

void PhoneStreams::tick()
{
    const auto t = now();
    // Read here and not per chunk: the source takes a lock and, every two
    // seconds, reads the registry.
    voice_gain_db_ = config_.voice_gain_db ? config_.voice_gain_db() : 0.0;
    if (!last_refresh_ || t - *last_refresh_ >= kSourcePollInterval) {
        refresh_source();
        refresh_transcription();
    }
    if (!last_detection_poll_ || t - *last_detection_poll_ >= config_.detection_poll) {
        refresh_detections();
    }
    refresh_rds();
    refresh_trunk_settings();
    refresh_calls();
    refresh_trunk();
    // The settings source re-reads the registry on its own interval, so this
    // is a comparison on most ticks.
    refresh_display();
    refresh_desktop();

    if (hls_ && hls_->ends_at && *hls_->ends_at <= t) {
        drop_hls();
        reconcile_audio();
    }
}

// ---- Source and spectrum ---------------------------------------------------

std::string PhoneStreams::source_event() const
{
    JsonObject fields;
    fields.add_bool("open", source_open_);
    if (source_open_) {
        fields.add_u64("rateHz", source_rate_)
            .add_i64("centerHz", source_center_)
            .add_u64("epoch", source_epoch_)
            .add_double("realtimeFactor", realtime_factor_)
            .add_double("pacedBy", paced_by_);
        // The spectrum starts half a coarse channel below the band and so
        // stops half a channel short of its top (SpectrumGeometry::bin_zero_hz),
        // and the reducer fills pixels past it with the floor. These are the
        // edges reduce() treats as the spectrum's: bin j spans half a bin
        // either side of first_bin_hz + j * bin_hz.
        const double bin_hz = spectrum_.bin_width.hertz();
        if (spectrum_.enabled() && bin_hz > 0.0) {
            const double first_bin_hz =
                static_cast<double>(source_center_) + spectrum_.bin_zero.hertz();
            fields.add_double("dataLowHz", first_bin_hz - bin_hz / 2.0)
                .add_double("dataHighHz",
                            first_bin_hz + (static_cast<double>(spectrum_.bins) - 0.5) * bin_hz);
        }
    }
    return event("source", fields);
}

void PhoneStreams::announce_state()
{
    // A resumed session may land in a page that was launched after these
    // events went out, so it has seen none of them.
    if (source_known_) {
        sent_realtime_factor_ = realtime_factor_;
        sent_paced_by_ = paced_by_;
        queue_text(source_event());
    }
    send_receiver_list();
    if (!sent_detections_.empty()) {
        queue_text(sent_detections_);
    }
    for (const auto& [vrx, poll] : rds_) {
        if (!poll.sent.empty()) {
            queue_text(poll.sent);
        }
    }
    if (transcription_) {
        sent_transcription_ = transcription_;
        queue_text(transcription_status_event(*transcription_));
    }
    sent_display_ = display_.event();
    queue_text(sent_display_);
    if (!sent_desktop_.empty()) {
        queue_text(sent_desktop_);
    }
    sent_dv_.clear();
    send_dv_state();
    for (const std::string* sent : {&sent_calls_, &sent_talkgroups_, &sent_trunk_}) {
        if (!sent->empty()) {
            queue_text(*sent);
        }
    }
    if (announced_hls_ && hls_ && hls_->segmenter && !hls_->ends_at) {
        queue_text(event("hls", JsonObject()
                                    .add_u64("vrx", *announced_hls_)
                                    .add_u64("stream", hls_id_)
                                    .add_string("url", hls_url(hls_id_))));
    }
}

void PhoneStreams::refresh_transcription()
{
    const auto status = config_.transcription_status ? config_.transcription_status()
                                                     : session_.client().transcription_status();
    if (!status) {
        // An engine without a recogniser refuses, and says nothing is the answer.
        return;
    }
    transcription_ = *status;
    if (sent_transcription_ && !transcription_report_due(*sent_transcription_, *status)) {
        return;
    }
    sent_transcription_ = *status;
    queue_text(transcription_status_event(*status));
}

void PhoneStreams::refresh_detections()
{
    const auto& view = session_.view();
    // Auto DV reads the detections whether or not a phone is looking, which
    // is what lets it keep opening receivers while the phone is locked.
    if (!view && !session_.auto_dv()) {
        return;
    }
    last_detection_poll_ = now();
    // The detector is built by the first request, so nothing is asked of it
    // until a phone is actually looking.
    const auto list = config_.detections ? config_.detections(session_.min_confidence(), 0.0)
                                         : session_.client().detections(session_.min_confidence(), 0.0);
    if (!list) {
        return;
    }
    if (session_.auto_dv()) {
        run_auto_dv(*list);
    }
    if (!view) {
        return;
    }

    // One view-width of margin either side, so a pan reveals tracks already
    // there instead of waiting for the next poll.
    const double low = view->center_hz - 1.5 * view->span_hz;
    const double high = view->center_hz + 1.5 * view->span_hz;
    std::vector<const rpc::Detection*> kept;
    for (const auto& d : list->detections) {
        if (d.state != rpc::TrackState::Live && d.state != rpc::TrackState::Held) {
            continue;
        }
        const double half = static_cast<double>(d.bandwidth_hz) / 2.0;
        const double centre = static_cast<double>(d.center_hz);
        if (centre + half < low || centre - half > high) {
            continue;
        }
        kept.push_back(&d);
    }
    if (kept.size() > kMaxDetections) {
        std::ranges::nth_element(kept, kept.begin() + kMaxDetections, [](const auto* a, const auto* b) {
            return a->snr_2500_db > b->snr_2500_db;
        });
        kept.resize(kMaxDetections);
        std::ranges::sort(kept, [](const auto* a, const auto* b) { return a->center_hz < b->center_hz; });
    }
    // A quiet band repeats the same list every poll; up to 200 items of it.
    std::string json = detections_event(kept);
    if (json != sent_detections_) {
        sent_detections_ = json;
        queue_text(std::move(json));
    }
}

void PhoneStreams::refresh_rds()
{
    // A receiver that went, or became another mode (which is a new id), is
    // no longer read; the engine ends its companion with it.
    std::erase_if(rds_, [this](const auto& entry) {
        const auto* params = session_.last_params(entry.first);
        return params == nullptr || params->demod != rpc::Demod::Wfm;
    });
    const auto t = now();
    for (const auto vrx : session_.receiver_ids()) {
        const auto* params = session_.last_params(vrx);
        if (params == nullptr || params->demod != rpc::Demod::Wfm) {
            continue;
        }
        auto& poll = rds_[vrx];
        if (t < poll.next) {
            continue;
        }
        // RBDS: the owner is in North America, where PTY codes mean other
        // genres and the call sign comes from the PI. Set before the first
        // read, since setting it later clears what the decoder has. One user
        // holds the engine, so no one else's decoder is reset by it.
        if (!poll.region_set) {
            const auto set = config_.set_rds_region
                                 ? config_.set_rds_region(vrx, rpc::RdsRegion::Rbds)
                                 : session_.client().set_rds_region(vrx, rpc::RdsRegion::Rbds);
            if (!set) {
                poll.next = t + config_.rds_retry;
                continue;
            }
            poll.region_set = true;
        }
        const auto station = config_.rds_station ? config_.rds_station(vrx)
                                                 : session_.client().rds_station(vrx);
        // A refusal is "no RDS here" or "not yet" (no channel for the
        // companion, a passband too narrow for the composite), which the phone
        // shows by showing nothing. A faulted decoder is the same to a phone.
        if (!station || !station->fault.empty()) {
            poll.next = t + config_.rds_retry;
            continue;
        }
        poll.next = t + config_.rds_poll;
        std::string json = rds_event(vrx, *station);
        if (json != poll.sent) {
            poll.sent = json;
            queue_text(std::move(json));
        }
    }
}

void PhoneStreams::refresh_source()
{
    last_refresh_ = now();
    const auto info = session_.client().info();
    if (!info) {
        // The engine is unreachable; the link events tell the phone that.
        return;
    }
    const bool open = info->source_rate != 0;
    const bool stream_changed = !source_known_ || open != source_open_ ||
                                info->source_epoch != source_epoch_ ||
                                info->source_rate != source_rate_;
    const bool moved = stream_changed || info->source_center != source_center_;
    const bool pace_report = open && pacing_report_due(sent_realtime_factor_, sent_paced_by_,
                                                       info->realtime_factor, info->source_paced_by);

    source_known_ = true;
    source_open_ = open;
    source_rate_ = info->source_rate;
    source_center_ = info->source_center;
    source_epoch_ = info->source_epoch;
    spectrum_enabled_ = info->spectrum.enabled();
    spectrum_ = info->spectrum;
    realtime_factor_ = info->realtime_factor;
    paced_by_ = info->source_paced_by;

    if (!moved) {
        // Only the factor moved: the stream and the frames in flight stand.
        if (pace_report) {
            sent_realtime_factor_ = realtime_factor_;
            sent_paced_by_ = paced_by_;
            queue_text(source_event());
        }
        return;
    }
    // A frame waiting to be sent was cut from the old stream or labelled with
    // the old centre; sending it under the new one would draw it in the wrong
    // place.
    pending_spectrum_.reset();
    {
        const std::lock_guard lock(inbox_->mutex);
        inbox_->spectrum.reset();
    }

    sent_realtime_factor_ = realtime_factor_;
    sent_paced_by_ = paced_by_;
    queue_text(source_event());

    if (!stream_changed) {
        return;
    }
    if (open && spectrum_enabled_) {
        subscribe_spectrum(every_nth_at_most(static_cast<double>(source_rate_) / kAssumedBlockSamples,
                                         config_.max_fps));
    } else {
        stop_spectrum();
    }
}

void PhoneStreams::subscribe_spectrum(std::uint32_t every_nth)
{
    auto inbox = inbox_;
    const auto subscribed = session_.client().subscribe_spectrum(
        every_nth, [inbox](const rpc::SpectrumFrame& frame) { put_spectrum(inbox, frame); });
    spectrum_subscribed_ = subscribed.has_value();
    every_nth_ = spectrum_subscribed_ ? every_nth : 0;
    every_nth_checked_ = false;
    first_seen_.reset();
}

void PhoneStreams::stop_spectrum()
{
    if (spectrum_subscribed_) {
        session_.client().unsubscribe_spectrum();
    }
    spectrum_subscribed_ = false;
    every_nth_ = 0;
}

std::optional<std::vector<std::uint8_t>> PhoneStreams::cut_spectrum(
    const rpc::SpectrumFrame& frame)
{
    const auto& view = session_.view();
    if (!view || frame.power_db.empty()) {
        return std::nullopt;
    }
    const double bin_hz = frame.geometry.bin_width.hertz();
    if (!(bin_hz > 0.0)) {
        return std::nullopt;
    }
    // bin_zero is the CENTRE of bin 0 relative to baseband DC
    // (core/engine/engine.h, SpectrumGeometry::bin_zero_hz: "Centre frequency
    // of bin zero"), which is what reduce() takes, so no half-bin shift.
    const double first_bin_hz =
        static_cast<double>(source_center_) + frame.geometry.bin_zero.hertz();
    float floor_db = frame.floor_db;
    float ceiling_db = frame.ceiling_db;
    if (!(ceiling_db > floor_db)) {
        floor_db = -140.0F;
        ceiling_db = 0.0F;
    }
    const ReducedColumns columns = reduce_columns(frame.power_db, first_bin_hz, bin_hz, *view);
    if (columns.db.empty()) {
        return std::nullopt;
    }

    if (display_view_ && (display_view_->center_hz != view->center_hz ||
                          display_view_->span_hz != view->span_hz ||
                          display_view_->width_px != view->width_px)) {
        display_.view_changed();
    }
    display_view_ = *view;

    ReducedFrame reduced;
    reduced.source_start = frame.start;
    reduced.view = columns.view;
    // Only the pixels over the spectrum are the desktop's columns: the ones a
    // pan has moved off its edge are no level at all, and draw the bottom.
    std::vector<float>& waterfall = waterfall_columns_;
    std::vector<float>& trace = trace_columns_;
    waterfall.assign(columns.db.size(), -std::numeric_limits<float>::infinity());
    trace.assign(columns.db.size(), -std::numeric_limits<float>::infinity());
    if (columns.end_valid > columns.first_valid) {
        std::vector<float>& shown_columns = shown_columns_;
        shown_columns.assign(columns.db.begin() + columns.first_valid,
                             columns.db.begin() + columns.end_valid);
        // A pixel of NaN bins inside the spectrum is the floor sentinel the
        // desktop's reduction would carry, not a hole in the history.
        for (float& c : shown_columns) {
            if (!std::isfinite(c)) {
                c = ui::kSpectrumFloorDb;
            }
        }
        const std::span<const float> whole(frame.power_db);
        const std::span<const float> shown = whole.subspan(columns.first_bin, columns.bin_count);
        const DisplayFrame facts{frame.start, frame.count, source_rate_, first_bin_hz, floor_db,
                                 ceiling_db};
        // Unzoomed, the offsets are zero and the whole frame is never read.
        const bool zoomed = shown.size() < whole.size();
        const FrameLevels levels = zoomed && config_.frame_levels
                                       ? config_.frame_levels->get(source_epoch_, frame.start, whole)
                                       : FrameLevels::of(whole, zoomed);
        const DisplayRow& row =
            display_.take(facts, levels, shown, shown_columns, columns.bins_per_px);
        std::copy(shown_columns.begin(), shown_columns.end(),
                  waterfall.begin() + columns.first_valid);
        std::copy(row.trace_columns.begin(), row.trace_columns.end(),
                  trace.begin() + columns.first_valid);
        floor_db = row.waterfall.floor_db;
        ceiling_db = row.waterfall.ceiling_db;
        reduced.trace_px = quantise(trace, row.trace.floor_db, row.trace.ceiling_db,
                                    reduced.trace_offset_db, reduced.trace_scale_db);
        send_display_if_changed();
    }
    reduced.px = quantise(waterfall, floor_db, ceiling_db, reduced.offset_db, reduced.scale_db);
    if (reduced.trace_px.empty()) {
        reduced.trace_px = reduced.px;
        reduced.trace_offset_db = reduced.offset_db;
        reduced.trace_scale_db = reduced.scale_db;
    }
    return encode(reduced);
}

// ---- The inbox and the send queue -----------------------------------------

void PhoneStreams::drain_inbox()
{
    std::optional<rpc::SpectrumFrame> frame;
    std::deque<std::pair<std::uint64_t, rpc::AudioChunk>> audio;
    std::deque<std::function<void(PhoneStreams&)>> items;
    {
        const std::lock_guard lock(inbox_->mutex);
        inbox_->wake_pending = false;
        frame.swap(inbox_->spectrum);
        audio.swap(inbox_->audio);
        items.swap(inbox_->items);
        counters_.spectrum_skipped += inbox_->spectrum_skipped;
        inbox_->spectrum_skipped = 0;
        counters_.opus_dropped += inbox_->audio_overflow;
        inbox_->audio_overflow = 0;
    }

    for (auto& item : items) {
        item(*this);
    }
    for (const auto& [vrx, chunk] : audio) {
        handle_audio(vrx, chunk);
    }
    if (frame && !attached()) {
        ++counters_.spectrum_skipped;
    } else if (frame && source_open_) {
        settle_every_nth(*frame);
        if (pending_spectrum_) {
            ++counters_.spectrum_skipped;
        }
        pending_spectrum_ = std::move(*frame);
    }
    pump();
}

// info() does not say how many samples one engine frame covers, and a frame's
// count is not it either (it reads half the dispatch on the fixture). Two
// frames of one subscription say it exactly: the sample index moved by
// delta-start while the engine produced delta-sequence frames. That gives the
// engine's frame rate, and every_nth is recomputed from it once.
void PhoneStreams::settle_every_nth(const rpc::SpectrumFrame& frame)
{
    if (!spectrum_subscribed_ || every_nth_checked_ || source_rate_ == 0) {
        return;
    }
    if (!first_seen_ || frame.sequence <= first_seen_->sequence ||
        frame.start <= first_seen_->start) {
        first_seen_ = FrameMark{frame.sequence, frame.start};
        return;
    }
    const double samples_per_frame = static_cast<double>(frame.start - first_seen_->start) /
                                     static_cast<double>(frame.sequence - first_seen_->sequence);
    every_nth_checked_ = true;
    const auto wanted =
        every_nth_at_most(static_cast<double>(source_rate_) / samples_per_frame, config_.max_fps);
    if (wanted != every_nth_) {
        subscribe_spectrum(wanted);
        every_nth_checked_ = true;
    }
}

void PhoneStreams::queue_text(std::string json)
{
    if (json.empty()) {
        return;
    }
    queued_text_bytes_ += json.size();
    texts_.push_back(std::move(json));
    const auto over = [this] {
        return texts_.size() > config_.max_queued_text ||
               queued_text_bytes_ > config_.max_queued_text_bytes;
    };
    if (over() && socket_ != nullptr) {
        // The phone is not reading. Closing hands it to the resume path,
        // which keeps the session; queueing without bound would not.
        socket_->close();
        detach();
    }
    while (over() && !texts_.empty()) {
        queued_text_bytes_ -= texts_.front().size();
        texts_.pop_front();
        ++counters_.text_dropped;
    }
    pump();
}

void PhoneStreams::attach(PhoneSocket& socket)
{
    socket_ = &socket;
    ++socket_generation_;
    in_flight_ = false;
    if (socket.busy()) {
        // Another session's last write is still going out on this socket.
        in_flight_ = true;
        socket.when_idle([this, alive = alive_, gen = socket_generation_] {
            if (!*alive || gen != socket_generation_) {
                return;
            }
            in_flight_ = false;
            pump();
        });
        return;
    }
    pump();
}

void PhoneStreams::detach()
{
    socket_ = nullptr;
    ++socket_generation_;
    in_flight_ = false;
    opus_.clear();
    pending_spectrum_.reset();
}

void PhoneStreams::pump()
{
    if (in_flight_ || pumping_ || socket_ == nullptr) {
        return;
    }
    pumping_ = true;
    while (!in_flight_ && socket_ != nullptr) {
        OutgoingMessage message;
        if (!texts_.empty()) {
            message.kind = OutgoingMessage::Kind::Text;
            message.text = std::move(texts_.front());
            texts_.pop_front();
            queued_text_bytes_ -= message.text.size();
        } else if (!opus_.empty()) {
            message.kind = OutgoingMessage::Kind::Opus;
            message.binary = std::move(opus_.front());
            opus_.pop_front();
            ++counters_.opus_sent;
        } else if (pending_spectrum_) {
            auto bytes = cut_spectrum(*pending_spectrum_);
            pending_spectrum_.reset();
            if (!bytes) {
                continue;
            }
            message.kind = OutgoingMessage::Kind::Spectrum;
            message.binary = std::move(*bytes);
            ++counters_.spectrum_sent;
        } else {
            break;
        }
        in_flight_ = true;
        socket_->send(std::move(message), [this, alive = alive_, gen = socket_generation_] {
            if (!*alive || gen != socket_generation_) {
                return;
            }
            in_flight_ = false;
            pump();
        });
    }
    pumping_ = false;
}

// ---- Receivers, audio, HLS -------------------------------------------------

void PhoneStreams::reconcile()
{
    reconcile_hls();
    reconcile_audio();
    reconcile_decoded();
}

void PhoneStreams::drop_hls()
{
    if (!hls_) {
        return;
    }
    // Out of the registry first: it borrows the pointer.
    if (config_.hls != nullptr) {
        config_.hls->remove(hls_id_);
    }
    hls_.reset();
}

void PhoneStreams::reconcile_hls()
{
    std::optional<std::uint64_t> target;
    if (config_.hls != nullptr && session_.keep_listening() && session_.audio_vrx() &&
        session_.has_receiver(*session_.audio_vrx())) {
        target = session_.audio_vrx();
    }

    if (hls_) {
        if (target) {
            hls_->ends_at.reset();
        } else if (!hls_->ends_at) {
            hls_->ends_at = now() + config_.hls_linger;
        }
    }

    if (target && !hls_) {
        auto segmenter = std::make_unique<HlsSegmenter>(kPhoneAudioRate);
        if (!segmenter->ok()) {
            queue_text(event("hls", JsonObject()
                                        .add_u64("vrx", *target)
                                        .add_null("url")
                                        .add_string("error", segmenter->error())));
            announced_hls_ = target;
            return;
        }
        config_.hls->set(hls_id_, segmenter.get());
        hls_ = Hls{std::move(segmenter), std::nullopt};
    }

    // Announced again when the audio moves to another receiver, though the URL
    // is the same: the phone follows the receiver by this id.
    if (target != announced_hls_) {
        if (target) {
            queue_text(event("hls", JsonObject()
                                        .add_u64("vrx", *target)
                                        .add_u64("stream", hls_id_)
                                        .add_string("url", hls_url(hls_id_))));
        }
        announced_hls_ = target;
    }
}

std::vector<PhoneStreams::MixMember> PhoneStreams::mix_members() const
{
    std::vector<MixMember> members;
    const auto active = session_.audio_vrx();
    const bool listening = active && session_.has_receiver(*active);
    if (listening) {
        const auto* params = session_.last_params(*active);
        members.push_back({*active, params != nullptr && is_digital_voice(params->demod)});
    }
    // Digital voice receivers join only while audio is on: the active receiver
    // is being heard, or keep listening has its stream up. Not through the
    // linger after either ends, which exists so a quick lock and unlock does
    // not rebuild the stream and has no listener.
    if (listening || (hls_ && !hls_->ends_at)) {
        for (const auto vrx : session_.receiver_ids()) {
            if (listening && vrx == *active) {
                continue;
            }
            const auto* params = session_.last_params(vrx);
            if (params != nullptr && is_digital_voice(params->demod)) {
                members.push_back({vrx, true});
            }
        }
    }
    return members;
}

void PhoneStreams::reconcile_audio()
{
    std::set<std::uint64_t> wanted;
    for (const auto& member : mix_members()) {
        wanted.insert(member.vrx);
    }
    mixer_.set_lead(session_.audio_vrx());

    auto& client = session_.client();
    for (auto it = audio_subscribed_.begin(); it != audio_subscribed_.end();) {
        if (!wanted.contains(*it)) {
            client.unsubscribe_audio(*it);
            audio_.erase(*it);
            mixer_.remove(*it);
            it = audio_subscribed_.erase(it);
        } else {
            ++it;
        }
    }

    auto inbox = inbox_;
    for (const auto vrx : wanted) {
        if (audio_subscribed_.contains(vrx)) {
            continue;
        }
        const auto granted = client.subscribe_audio(
            vrx, 0,
            [inbox, vrx](const rpc::AudioChunk& chunk) { put_audio(inbox, vrx, chunk); },
            [inbox, vrx](const std::string&) {
                // The receiver went or the stream failed. A removal is
                // reported through the receiver events with the engine's
                // reason; this only stops expecting chunks.
                put_item(inbox, [vrx](PhoneStreams& self) {
                    self.audio_subscribed_.erase(vrx);
                    self.audio_.erase(vrx);
                    self.mixer_.remove(vrx);
                });
            });
        if (granted) {
            audio_subscribed_.insert(vrx);
        } else {
            queue_text(event("vrxStatus", JsonObject()
                                              .add_u64("vrx", vrx)
                                              .add_string("audioError", granted.error().message)));
        }
    }
}

void PhoneStreams::reconcile_decoded()
{
    auto& client = session_.client();
    for (auto it = decoded_.begin(); it != decoded_.end();) {
        if (!session_.has_receiver(it->first)) {
            for (const auto& name : it->second) {
                client.unsubscribe_decoded(it->first, name);
                decoder_generation_.erase(std::pair(it->first, name));
            }
            decoded_applied_.erase(it->first);
            it = decoded_.erase(it);
        } else {
            ++it;
        }
    }
    std::erase_if(decoded_applied_, [this](const auto& entry) { return !session_.has_receiver(entry.first); });

    for (const auto vrx : session_.receiver_ids()) {
        // Not recorded as applied, so the next reconcile tries again once the
        // engine's decoder list can be read.
        if (!session_.decoders_known(vrx)) {
            continue;
        }
        auto wanted = session_.decoders(vrx);
        if (const auto it = decoded_applied_.find(vrx); it != decoded_applied_.end() && it->second == wanted) {
            continue;
        }
        auto& running = decoded_[vrx];
        for (auto it = running.begin(); it != running.end();) {
            if (std::ranges::find(wanted, *it) == wanted.end()) {
                client.unsubscribe_decoded(vrx, *it);
                decoder_generation_.erase(std::pair(vrx, *it));
                it = running.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& name : wanted) {
            if (running.contains(name)) {
                continue;
            }
            if (const auto granted = subscribe_decoder(vrx, name)) {
                running.insert(name);
            } else {
                queue_text(event("vrxStatus", JsonObject()
                                                  .add_u64("vrx", vrx)
                                                  .add_string("decoder", name)
                                                  .add_string("decoderError", granted.error().message)));
            }
        }
        decoded_applied_[vrx] = std::move(wanted);
    }
}

std::set<std::string> PhoneStreams::decoding(std::uint64_t vrx) const
{
    const auto it = decoded_.find(vrx);
    return it == decoded_.end() ? std::set<std::string>{} : it->second;
}

Status PhoneStreams::subscribe_decoder(std::uint64_t vrx, const std::string& name)
{
    auto inbox = inbox_;
    const std::uint64_t generation = ++next_decoder_generation_;
    auto on_message = [inbox](const rpc::DecodedMessage& m) {
                put_item(inbox, [m](PhoneStreams& self) {
                    self.queue_text(event("decoded", JsonObject()
                                                         .add_u64("vrx", m.vrx)
                                                         .add_string("decoder", m.decoder)
                                                         .add_string("kind", m.kind)
                                                         .add_string("text", m.text)
                                                         .add_u64("sequence", m.sequence)
                                                         .add_raw("fields", fields_json(m.fields))));
                    self.fold_call(m);
                });
            };
    auto on_ended = [inbox, vrx, name, generation](const std::string& reason) {
                // The decoder stopped on its own (it refused the rate it was
                // fed, say). It is left out of the running set so the phone's
                // next change of set can try it again, and the phone is told.
                put_item(inbox, [vrx, name, reason, generation](PhoneStreams& self) {
                    // A receiver that went (a removal, or a mode change's
                    // replacement) ends its decoders too; vrxRemoved says that.
                    if (!self.session_.has_receiver(vrx)) {
                        return;
                    }
                    // The subscription this ended has since been dropped or
                    // replaced; the one running now is not the one that ended.
                    const auto key = std::pair(vrx, name);
                    if (const auto it = self.decoder_generation_.find(key);
                        it == self.decoder_generation_.end() || it->second != generation) {
                        return;
                    }
                    self.decoder_generation_.erase(key);
                    if (const auto it = self.decoded_.find(vrx); it != self.decoded_.end()) {
                        it->second.erase(name);
                    }
                    self.queue_text(event("vrxStatus", JsonObject()
                                                           .add_u64("vrx", vrx)
                                                           .add_string("decoder", name)
                                                           .add_string("decoderError", reason)));
                });
            };
    const auto subscribed =
        config_.subscribe_decoded
            ? config_.subscribe_decoded(vrx, name, std::move(on_message), std::move(on_ended))
            : session_.client().subscribe_decoded(vrx, name, std::move(on_message),
                                                  std::move(on_ended));
    if (!subscribed) {
        return std::unexpected(subscribed.error());
    }
    decoder_generation_[std::pair(vrx, name)] = generation;
    return {};
}

void PhoneStreams::handle_audio(std::uint64_t vrx, const rpc::AudioChunk& chunk)
{
    if (!audio_subscribed_.contains(vrx) || chunk.channel_count == 0) {
        return;
    }
    auto& path = audio_[vrx];
    // Once per receiver to both the phone and the console: the phone's
    // vrxStatus alone left a silent DV channel with nothing in the log.
    const auto report = [&](const std::string& message) {
        if (path.reported_error) {
            return;
        }
        path.reported_error = true;
        std::fprintf(stderr, "revenant-webgate: receiver %llu audio: %s\n",
                     static_cast<unsigned long long>(vrx), message.c_str());
        queue_text(event("vrxStatus",
                         JsonObject().add_u64("vrx", vrx).add_string("audioError", message)));
    };
    if (chunk.sample_rate == 0) {
        report("the receiver's audio chunk carries no sample rate");
        return;
    }

    // Averaging the channels: the phone path is mono, and interleaved stereo
    // fed to a mono encoder would play at half speed.
    std::vector<float> downmixed;
    std::span<const float> mono = chunk.samples;
    if (chunk.channel_count > 1) {
        const std::size_t channels = chunk.channel_count;
        const std::size_t frames = chunk.samples.size() / channels;
        downmixed.resize(frames);
        for (std::size_t f = 0; f < frames; ++f) {
            float sum = 0.0F;
            for (std::size_t c = 0; c < channels; ++c) {
                sum += chunk.samples[f * channels + c];
            }
            downmixed[f] = sum / static_cast<float>(channels);
        }
        mono = downmixed;
    }
    mono = path.rate.convert(chunk.sample_rate, mono);

    // Every receiver the phone hears goes into the one mix; what leaves it is
    // what both encoders are fed. Digital voice is boosted on the way in by the
    // desktop's setting, whichever receiver the phone is listening to.
    const auto* params = session_.last_params(vrx);
    const bool digital = params != nullptr && is_digital_voice(params->demod);
    mixer_.set_lead(session_.audio_vrx());
    const auto t = now();
    mixer_.push(vrx, digital, voice_gain_db_, mono, t);
    emit_mix(mixer_.take(t), report);
    pump();
}

void PhoneStreams::emit_mix(std::span<const float> mixed,
                            const std::function<void(const std::string&)>& report)
{
    if (mixed.empty()) {
        return;
    }
    const auto active = session_.audio_vrx();
    if (active && attached()) {
        // A different receiver is a different stream to the phone, which
        // starts its sequence again (web/src/audio/live.ts).
        if (mix_opus_for_ != active) {
            mix_opus_.reset();
            mix_opus_seq_ = 0;
            mix_opus_for_ = active;
        }
        if (!mix_opus_) {
            auto created = LiveOpusEncoder::create(kPhoneAudioRate);
            if (!created) {
                report(created.error().message);
            } else {
                mix_opus_ = std::move(*created);
            }
        }
        if (mix_opus_) {
            mix_opus_->push(mixed);
            for (const auto& packet : mix_opus_->take_packets()) {
                opus_.push_back(encode_opus(*active, mix_opus_seq_++, packet));
            }
            while (opus_.size() > config_.max_queued_opus) {
                opus_.pop_front();
                ++counters_.opus_dropped;
            }
        }
    }
    if (hls_ && hls_->segmenter) {
        hls_->segmenter->push(mixed);
    }
}

namespace {

[[nodiscard]] std::string opened_by(const rpc::VrxEvent& e)
{
    switch (e.owner) {
        case rpc::VrxOwnerKind::ThisSession: return "this phone";
        case rpc::VrxOwnerKind::OtherSession: return "another client";
        case rpc::VrxOwnerKind::Plugin: return e.owner_name.empty() ? "a plugin" : e.owner_name;
        case rpc::VrxOwnerKind::Engine: break;
    }
    return "the engine";
}

}  // namespace

void PhoneStreams::send_receiver(std::uint64_t vrx)
{
    const auto it = described_.find(vrx);
    if (it == described_.end()) {
        return;
    }
    queue_text(receiver_event(it->second.status, source_center_, session_.decoders(vrx),
                              it->second.opened_by));
}

void PhoneStreams::handle_vrx_event(const rpc::VrxEvent& e)
{
    if (e.kind != rpc::VrxEventKind::Removed) {
        // Every receiver the engine has is the phone's, so each one is
        // learned and listed whoever opened it. The subscription replays the
        // whole list as Added first, which is how a fresh session hears of
        // receivers opened before it.
        if (!e.status) {
            return;
        }
        // Only this session's own are added again after an engine restart:
        // two sessions each rebuilding every receiver would double them, and
        // a plugin's come back from the plugin.
        const bool opened_here = e.owner == rpc::VrxOwnerKind::ThisSession;
        if (session_.has_receiver(e.vrx)) {
            session_.remember(e.vrx, e.status->params);
        } else {
            session_.learn(e.vrx, e.status->params, opened_here);
        }
        known_.insert(e.vrx);
        described_[e.vrx] = Described{*e.status, opened_by(e)};
        send_receiver(e.vrx);
        reconcile();
        return;
    }
    described_.erase(e.vrx);
    if (!known_.contains(e.vrx)) {
        return;
    }
    known_.erase(e.vrx);
    session_.auto_dv_spawned().erase(e.vrx);
    session_.forget(e.vrx);
    queue_text(event("vrxRemoved",
                     JsonObject().add_u64("vrx", e.vrx).add_string("reason", e.reason)));
    reconcile();
}

// ---- The DV tab ------------------------------------------------------------

std::int64_t PhoneStreams::wall_ms() const
{
    if (config_.wall_clock_ms) {
        return config_.wall_clock_ms();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void PhoneStreams::fold_call(const rpc::DecodedMessage& message)
{
    if (!ui::is_call_decoder(message.decoder)) {
        return;
    }
    // The receiver's frequency and whose it is, which the message does not
    // carry; decoded_link.cpp reads the same two off the desktop's rack.
    std::int64_t hz = 0;
    std::string owner = "user";
    if (const auto it = described_.find(message.vrx); it != described_.end()) {
        hz = source_center_ + it->second.status.params.center;
        owner = call_owner(it->second.opened_by);
    }
    for (const ui::CallFrame& frame : ui::call_frames(message, wall_ms(), hz, owner)) {
        calls_.apply(frame);
        calls_dirty_ = true;
    }
}

void PhoneStreams::refresh_calls()
{
    // A call ends on silence as well as on its terminator, so time alone can
    // change a row.
    const bool expired = calls_.expire(wall_ms());
    if (!expired && !calls_dirty_) {
        return;
    }
    calls_dirty_ = false;
    std::string calls = calls_event(calls_, aliases_);
    if (calls != sent_calls_) {
        sent_calls_ = calls;
        queue_text(std::move(calls));
    }
    std::string groups = talkgroups_event(calls_, aliases_);
    if (groups != sent_talkgroups_) {
        sent_talkgroups_ = groups;
        queue_text(std::move(groups));
    }
}

void PhoneStreams::refresh_trunk_settings()
{
    if (!config_.desktop_settings) {
        return;
    }
    const DesktopSettings saved = config_.desktop_settings();
    if (saved.trunk_aliases != aliases_text_) {
        aliases_text_ = saved.trunk_aliases;
        aliases_ = ui::parse_trunk_aliases(aliases_text_.value_or("")).value;
        // Rows name talkgroups by alias, so a new table is a change to them.
        calls_dirty_ = true;
        sent_trunk_.clear();
    }
    session_.set_desktop_trunk_rules(trunk_rules_from_text(saved.trunk_allow.value_or(""),
                                                           saved.trunk_deny.value_or(""),
                                                           saved.trunk_priority.value_or(""))
                                         .rules);
    const std::uint32_t max = clamp_followers(saved.trunk_max_followers.value_or(kDefaultMaxFollowers));
    if (max != session_.desktop_max_followers()) {
        session_.set_desktop_max_followers(max);
        send_dv_state();
    }
}

void PhoneStreams::send_dv_state()
{
    std::string now = dv_state_event(session_);
    if (now != sent_dv_) {
        sent_dv_ = now;
        queue_text(std::move(now));
    }
}

void PhoneStreams::apply_dv_switches()
{
    auto& client = session_.client();
    if (session_.auto_dv()) {
        // Refused only by an engine older than the call; see op_auto_dv.
        static_cast<void>(client.set_plugin_receivers(false));
    }
    if (const auto follow = session_.trunk_follow()) {
        static_cast<void>(client.set_trunk_rules(session_.wanted_trunk_rules()));
        if (auto set = client.set_trunk_follow(follow->vrx, true, follow->max_followers); !set) {
            std::fprintf(stderr, "revenant-webgate: the engine refused trunk following: %s\n",
                         set.error().message.c_str());
            session_.set_trunk_follow(std::nullopt);
        }
    }
}

void PhoneStreams::refresh_trunk()
{
    if (!session_.dv_watch() && !session_.trunk_follow()) {
        return;
    }
    const auto t = now();
    if (next_trunk_poll_ && t < *next_trunk_poll_) {
        return;
    }
    const auto status =
        config_.trunk_status ? config_.trunk_status() : session_.client().trunk_status();
    std::string json;
    if (!status) {
        next_trunk_poll_ = t + config_.trunk_retry;
        json = trunk_event({}, aliases_, status.error().message);
    } else {
        next_trunk_poll_ = t + config_.trunk_poll;
        if (const auto follow = session_.trunk_follow()) {
            if (!status->enabled) {
                // Following stopped by itself (the receiver went, the span
                // moved): the phone's switch goes with it, as the desktop's
                // solo does.
                session_.set_trunk_follow(std::nullopt);
                send_dv_state();
            } else {
                // Reconciled, not sent once, as trunk_link.cpp does, so a
                // change made elsewhere is put back to what this phone asked.
                auto& client = session_.client();
                if (!same_trunk_rules(status->rules, session_.wanted_trunk_rules())) {
                    static_cast<void>(client.set_trunk_rules(session_.wanted_trunk_rules()));
                }
                if (status->max_followers != follow->max_followers) {
                    static_cast<void>(
                        client.set_trunk_follow(status->control_vrx, true, follow->max_followers));
                }
            }
        }
        json = trunk_event(*status, aliases_);
    }
    if (json != sent_trunk_) {
        sent_trunk_ = json;
        queue_text(std::move(json));
    }
}

void PhoneStreams::run_auto_dv(const rpc::DetectionList& list)
{
    if (!source_open_) {
        return;
    }
    std::vector<PlacedReceiver> placed;
    for (const auto vrx : session_.receiver_ids()) {
        if (const auto* params = session_.last_params(vrx)) {
            placed.push_back(
                PlacedReceiver{vrx, static_cast<double>(source_center_ + params->center)});
        }
    }
    auto& spawned = session_.auto_dv_spawned();
    std::erase_if(spawned, [this](std::uint64_t vrx) { return !session_.has_receiver(vrx); });
    const AutoDvPass pass = plan_auto_dv(list.detections, placed, spawned,
                                         session_.auto_dv_dismissed(), session_.audio_vrx());
    bool changed = false;
    auto& client = session_.client();
    for (const auto vrx : pass.retire) {
        if (client.remove_vrx(vrx)) {
            spawned.erase(vrx);
            session_.forget(vrx);
            changed = true;
        }
    }
    // Held and heard, never selected: the phone stays on what it is listening
    // to. The mode's own filter, as the desktop's place_startup_receiver.
    for (const ui::DvSpawn& spawn : pass.open) {
        rpc::VrxParams params;
        params.demod = spawn.demod;
        params.center = static_cast<std::int64_t>(std::llround(spawn.centre_hz)) - source_center_;
        const auto id = client.add_vrx(params);
        if (!id) {
            std::fprintf(stderr, "revenant-webgate: auto DV could not open %.*s at %.4f MHz: %s\n",
                         static_cast<int>(spawn.protocol.size()), spawn.protocol.data(),
                         spawn.centre_hz / 1e6, id.error().message.c_str());
            continue;
        }
        session_.learn(*id, params);
        spawned.insert(*id);
        known_.insert(*id);
        changed = true;
    }
    if (changed) {
        reconcile();
    }
}

}  // namespace revenant::webgate
