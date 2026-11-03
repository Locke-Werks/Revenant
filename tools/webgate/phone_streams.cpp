#include "tools/webgate/phone_streams.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <utility>
#include <variant>

#include "tools/webgate/dispatch.h"
#include "tools/webgate/protocol.h"
#include "tools/webgate/spectrum_reducer.h"

namespace revenant::webgate {

namespace {

// Opus and the AAC segmenter both run at 48 kHz; anything else would play at
// the wrong pitch, so it is reported rather than resampled here.
constexpr std::uint32_t kPhoneAudioRate = 48'000;

// The engine's default dispatch size. Only a first guess: EngineInfo does not
// carry the block size, so the first frame's sample count corrects it.
constexpr double kAssumedBlockSamples = 65'536.0;

// Chunks held between the client thread and the strand. The strand normally
// empties this within a millisecond; the cap only matters if it stalls.
constexpr std::size_t kMaxInboxChunks = 200;

constexpr auto kSourcePollInterval = std::chrono::seconds(1);

// A busy band must not flood the socket, so the strongest are kept.
constexpr std::size_t kMaxDetections = 200;

// Analog and digital-family labels name a modulation. A protocol label names
// what is carried on top of one, which the phone has no use for here.
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
                     .add_string("label", d->label.kind == rpc::LabelKind::AnalogModulation ||
                                                  d->label.kind == rpc::LabelKind::DigitalFamily
                                              ? std::string_view(d->label.name)
                                              : std::string_view())
                     .add_double("labelConfidence", d->label.confidence)
                     .add_u64("lastSeen", d->last_seen)
                     .str();
    }
    array += ']';
    return event("detections", JsonObject().add_raw("items", array));
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
    for (const auto vrx : decoded_tried_) {
        client.unsubscribe_decoded(vrx, "");
    }
    client.unsubscribe_vrx_events();
    client.unsubscribe_transcripts();
    if (config_.hls != nullptr) {
        for (const auto& [vrx, hls] : hls_) {
            config_.hls->remove(vrx);
        }
    }
}

std::chrono::steady_clock::time_point PhoneStreams::now() const
{
    return config_.clock ? config_.clock() : std::chrono::steady_clock::now();
}

HlsSegmenter* PhoneStreams::segmenter(std::uint64_t vrx) const
{
    const auto it = hls_.find(vrx);
    return it == hls_.end() ? nullptr : it->second.segmenter.get();
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
    subscribe_vrx_events();

    auto inbox = inbox_;
    const auto transcripts = session_.client().subscribe_transcripts(
        [inbox](const rpc::Transcript& t) {
            put_item(inbox, [t](PhoneStreams& self) {
                // Engine-wide stream; a phone sees transcripts of its own
                // receivers, as it sees only its own receivers elsewhere.
                if (!self.known_owned_.contains(t.vrx)) {
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

    refresh_source();
    refresh_transcription();
    refresh_display();
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

    for (const auto vrx : session_.owned_ids()) {
        known_owned_.insert(vrx);
    }
    if (command->op == "openSource" || command->op == "setCenter") {
        refresh_source();
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
    for (const auto vrx : session_.owned_ids()) {
        known_owned_.insert(vrx);
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
    if (!last_refresh_ || t - *last_refresh_ >= kSourcePollInterval) {
        refresh_source();
        refresh_transcription();
    }
    if (!last_detection_poll_ || t - *last_detection_poll_ >= config_.detection_poll) {
        refresh_detections();
    }
    // The settings source re-reads the registry on its own interval, so this
    // is a comparison on most ticks.
    refresh_display();

    bool dropped = false;
    for (auto it = hls_.begin(); it != hls_.end();) {
        if (it->second.ends_at && *it->second.ends_at <= t) {
            if (config_.hls != nullptr) {
                config_.hls->remove(it->first);
            }
            it = hls_.erase(it);
            dropped = true;
        } else {
            ++it;
        }
    }
    if (dropped) {
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
    if (transcription_) {
        sent_transcription_ = transcription_;
        queue_text(transcription_status_event(*transcription_));
    }
    sent_display_ = display_.event();
    queue_text(sent_display_);
    if (announced_hls_) {
        if (const auto it = hls_.find(*announced_hls_);
            it != hls_.end() && it->second.segmenter && !it->second.ends_at) {
            queue_text(event("hls", JsonObject()
                                        .add_u64("vrx", *announced_hls_)
                                        .add_string("url", hls_url(*announced_hls_))));
        }
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
    if (!view) {
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
    queue_text(detections_event(kept));
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

void PhoneStreams::drop_hls(std::uint64_t vrx)
{
    if (!hls_.contains(vrx)) {
        return;
    }
    // Out of the registry first: it borrows the pointer.
    if (config_.hls != nullptr) {
        config_.hls->remove(vrx);
    }
    hls_.erase(vrx);
}

void PhoneStreams::reconcile_hls()
{
    std::optional<std::uint64_t> target;
    if (config_.hls != nullptr && session_.keep_listening() && session_.audio_vrx() &&
        session_.owns(*session_.audio_vrx())) {
        target = session_.audio_vrx();
    }

    std::vector<std::uint64_t> gone;
    for (auto& [vrx, hls] : hls_) {
        if (!session_.owns(vrx)) {
            gone.push_back(vrx);
        } else if (target == vrx) {
            hls.ends_at.reset();
        } else if (!hls.ends_at) {
            hls.ends_at = now() + config_.hls_linger;
        }
    }
    for (const auto vrx : gone) {
        drop_hls(vrx);
    }

    if (target && !hls_.contains(*target)) {
        auto segmenter = std::make_unique<HlsSegmenter>(kPhoneAudioRate);
        if (!segmenter->ok()) {
            queue_text(event("hls", JsonObject()
                                        .add_u64("vrx", *target)
                                        .add_null("url")
                                        .add_string("error", segmenter->error())));
            announced_hls_ = target;
            return;
        }
        config_.hls->set(*target, segmenter.get());
        hls_[*target].segmenter = std::move(segmenter);
    }

    if (target != announced_hls_) {
        if (target) {
            queue_text(event(
                "hls", JsonObject().add_u64("vrx", *target).add_string("url", hls_url(*target))));
        }
        announced_hls_ = target;
    }
}

void PhoneStreams::reconcile_audio()
{
    std::set<std::uint64_t> wanted;
    if (const auto vrx = session_.audio_vrx(); vrx && session_.owns(*vrx)) {
        wanted.insert(*vrx);
    }
    for (const auto& [vrx, hls] : hls_) {
        wanted.insert(vrx);
    }

    auto& client = session_.client();
    for (auto it = audio_subscribed_.begin(); it != audio_subscribed_.end();) {
        if (!wanted.contains(*it)) {
            client.unsubscribe_audio(*it);
            audio_.erase(*it);
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
    for (auto it = decoded_tried_.begin(); it != decoded_tried_.end();) {
        if (!session_.owns(*it)) {
            client.unsubscribe_decoded(*it, "");
            it = decoded_tried_.erase(it);
        } else {
            ++it;
        }
    }

    auto inbox = inbox_;
    for (const auto vrx : session_.owned_ids()) {
        if (decoded_tried_.contains(vrx)) {
            continue;
        }
        decoded_tried_.insert(vrx);
        // An empty decoder name asks for the one named after the receiver's
        // mode. Analog modes have none and are refused, which is the answer:
        // nothing to decode.
        const auto subscribed = client.subscribe_decoded(
            vrx, "",
            [inbox](const rpc::DecodedMessage& m) {
                put_item(inbox, [m](PhoneStreams& self) {
                    self.queue_text(event("decoded", JsonObject()
                                                         .add_u64("vrx", m.vrx)
                                                         .add_string("decoder", m.decoder)
                                                         .add_string("kind", m.kind)
                                                         .add_string("text", m.text)
                                                         .add_u64("sequence", m.sequence)
                                                         .add_raw("fields", fields_json(m.fields))));
                });
            },
            {});
        static_cast<void>(subscribed);
    }
}

void PhoneStreams::handle_audio(std::uint64_t vrx, const rpc::AudioChunk& chunk)
{
    if (!audio_subscribed_.contains(vrx) || chunk.channel_count == 0) {
        return;
    }
    auto& path = audio_[vrx];
    if (chunk.sample_rate != kPhoneAudioRate) {
        if (!path.reported_error) {
            path.reported_error = true;
            queue_text(event(
                "vrxStatus",
                JsonObject().add_u64("vrx", vrx).add_string(
                    "audioError", "the receiver's audio is at " +
                                      std::to_string(chunk.sample_rate) +
                                      " Hz and the phone's audio path takes 48000 Hz only")));
        }
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

    if (session_.audio_vrx() == vrx && attached()) {
        if (!path.opus) {
            auto created = LiveOpusEncoder::create(kPhoneAudioRate);
            if (!created) {
                if (!path.reported_error) {
                    path.reported_error = true;
                    queue_text(event("vrxStatus", JsonObject()
                                                      .add_u64("vrx", vrx)
                                                      .add_string("audioError",
                                                                  created.error().message)));
                }
            } else {
                path.opus = std::move(*created);
            }
        }
        if (path.opus) {
            path.opus->push(mono);
            for (const auto& packet : path.opus->take_packets()) {
                opus_.push_back(encode_opus(vrx, path.next_seq++, packet));
            }
            while (opus_.size() > config_.max_queued_opus) {
                opus_.pop_front();
                ++counters_.opus_dropped;
            }
        }
    }

    if (const auto it = hls_.find(vrx); it != hls_.end()) {
        it->second.segmenter->push(mono);
    }
    pump();
}

void PhoneStreams::handle_vrx_event(const rpc::VrxEvent& e)
{
    if (e.kind != rpc::VrxEventKind::Removed || !known_owned_.contains(e.vrx)) {
        return;
    }
    known_owned_.erase(e.vrx);
    session_.forget(e.vrx);
    drop_hls(e.vrx);
    queue_text(event("vrxRemoved",
                     JsonObject().add_u64("vrx", e.vrx).add_string("reason", e.reason)));
    reconcile();
}

}  // namespace revenant::webgate
