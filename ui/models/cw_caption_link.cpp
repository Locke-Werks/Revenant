// EngineLink's CW captions: text beside every detected CW signal on the span
// waterfall, with no receiver open.
//
// WHY THE UI SUBSCRIBES AND NOT THE ENGINE. The engine already serves a
// decoder on its own stream per subscription (subscribeWidebandDecoded), and
// the detector already labels CW. Joining them here needs no schema change and
// no second lifetime inside the detector: a carrier that comes is subscribed,
// one that goes is dropped, and the engine's one user is this window. The
// phone does not see the text, which is the price, and the engine serves one
// user at a time anyway.
//
// Each line goes on captionFeed() as a transcript, so the waterfall places,
// scrolls and ages it exactly as it does speech. It is placed on the rows the
// waterfall drew when the line arrived, because a tap's sample count starts
// at the tap and is not the source index the rows are kept in; a line is at
// most a few seconds of keying, so the caption sits at the bottom of what it
// says.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <QMetaObject>

#include "models/engine_link.h"
#include "models/transcription.h"
#include "render/caption_layout.h"

namespace revenant::ui {

namespace {

constexpr std::string_view kCwDecoder = "cw_wide";

// Each feed is a tap on the source's delivery chain and a decoder on a lane;
// a band full of CW should not open a hundred of them.
constexpr std::size_t kMaxCwFeeds = 16;

// Two detections this close are the same carrier. The tracker's centre for
// one keyed carrier was seen 360 Hz apart between ids on the hf scene, and a
// second feed that far off would decode the same station again through the
// skirt of cw_wide's +/-170 Hz low-pass.
constexpr std::int64_t kCwSameCarrierHz = 500;

// A source retune further than this rebuilds every feed at its carrier's new
// offset, since a tap's mix is fixed when it is attached.
constexpr std::int64_t kCwRetuneHz = 100;

// How long a feed outlives its carrier's last detection. Long enough for the
// gaps between a keyed carrier's words and between a beacon's repeats, short
// enough that a station that has gone stops costing a tap.
constexpr auto kCwGrace = std::chrono::seconds(20);

// How long a carrier the engine refused, or whose feed it ended, is left
// before it is asked again.
constexpr auto kCwRetry = std::chrono::seconds(30);

// The caption's band when the detector measured the carrier narrower: wide
// enough that the leader has a passband to point at.
constexpr std::int64_t kCwMinHalfWidthHz = 60;

// A line's length in samples, for the burst timeline; the waterfall reads
// only where it ended.
constexpr double kCwLineSeconds = 4.0;

[[nodiscard]] bool is_cw(const rpc::Detection& detection)
{
    return detection.state != rpc::TrackState::Merged && detection.label.name == "CW";
}

[[nodiscard]] double field_real(const rpc::DecodedMessage& message, std::string_view key)
{
    const rpc::DecodedField* field = message.field(key);
    if (field == nullptr || field->real() == nullptr) {
        return 0.0;
    }
    return *field->real();
}

[[nodiscard]] bool same_carrier(std::int64_t a, std::int64_t b)
{
    return std::abs(a - b) <= kCwSameCarrierHz;
}

}  // namespace

void EngineLink::want_cw_captions()
{
    std::vector<CwCaptionBand> wanted;
    if (connected_) {
        for (const rpc::Detection& detection : shown_.detections) {
            if (!is_cw(detection) ||
                std::ranges::any_of(wanted, [&detection](const CwCaptionBand& b) {
                    return same_carrier(b.center_hz, detection.center_hz);
                })) {
                continue;
            }
            const std::int64_t half =
                std::max<std::int64_t>(detection.bandwidth_hz / 2, kCwMinHalfWidthHz);
            wanted.push_back(CwCaptionBand{
                .offset_hz = detection.center_hz - info_.source_center,
                .center_hz = detection.center_hz,
                .low_hz = detection.center_hz - half,
                .high_hz = detection.center_hz + half,
            });
        }
    }
    const std::lock_guard<std::mutex> lock(cw_caption_mutex_);
    cw_wanted_ = std::move(wanted);
}

void EngineLink::apply_cw_captions()
{
    if (client_ == nullptr) {
        return;
    }
    std::vector<CwCaptionBand> wanted;
    std::vector<std::int64_t> ended;
    {
        const std::lock_guard<std::mutex> lock(cw_caption_mutex_);
        ended.swap(cw_ended_);
        wanted = cw_wanted_;
    }
    const auto now = std::chrono::steady_clock::now();

    // What ends a feed is a source closing or a refusal, and either would end
    // the next one the same way, so the carrier waits out kCwRetry.
    for (const std::int64_t centre : ended) {
        if (std::erase_if(cw_feeds_, [centre](const CwCaptionFeed& f) {
                return f.band.center_hz == centre;
            }) > 0) {
            cw_refusals_.push_back(CwCaptionRefusal{centre, now});
        }
    }
    std::erase_if(cw_refusals_,
                  [now](const CwCaptionRefusal& r) { return now - r.at > kCwRetry; });

    // A feed keeps the offset it was attached at while its carrier's centre
    // wanders inside the low-pass; only a source retune, which moves the
    // offset of everything, rebuilds it.
    for (auto feed = cw_feeds_.begin(); feed != cw_feeds_.end();) {
        const auto want = std::ranges::find_if(wanted, [&feed](const CwCaptionBand& b) {
            return same_carrier(b.center_hz, feed->band.center_hz);
        });
        bool drop = false;
        if (want != wanted.end()) {
            const std::int64_t source_then = feed->band.center_hz - feed->band.offset_hz;
            const std::int64_t source_now = want->center_hz - want->offset_hz;
            drop = std::abs(source_now - source_then) > kCwRetuneHz;
            feed->last_wanted = now;
        } else {
            drop = now - feed->last_wanted > kCwGrace;
        }
        if (drop) {
            client_->unsubscribe_decoded(feed->key, kCwDecoder);
            feed = cw_feeds_.erase(feed);
        } else {
            ++feed;
        }
    }

    for (const CwCaptionBand& want : wanted) {
        if (cw_feeds_.size() >= kMaxCwFeeds) {
            break;
        }
        const bool fed = std::ranges::any_of(cw_feeds_, [&want](const CwCaptionFeed& f) {
            return same_carrier(f.band.center_hz, want.center_hz);
        });
        const bool refused = std::ranges::any_of(cw_refusals_, [&want](const CwCaptionRefusal& r) {
            return same_carrier(r.center_hz, want.center_hz);
        });
        if (fed || refused) {
            continue;
        }
        auto subscribed = client_->subscribe_wideband_decoded(
            kCwDecoder, want.offset_hz,
            [this, want](const rpc::DecodedMessage& message) { on_cw_caption(want, message); },
            [this, centre = want.center_hz](const std::string&) { on_cw_caption_ended(centre); });
        if (!subscribed) {
            // An engine without the decoder, or a source too slow for it.
            cw_refusals_.push_back(CwCaptionRefusal{want.center_hz, now});
            continue;
        }
        cw_feeds_.push_back(CwCaptionFeed{subscribed->key, want, now});
    }
}

void EngineLink::forget_cw_captions()
{
    cw_feeds_.clear();
    cw_refusals_.clear();
    const std::lock_guard<std::mutex> lock(cw_caption_mutex_);
    cw_ended_.clear();
    cw_wanted_.clear();
}

void EngineLink::on_cw_caption(const CwCaptionBand& band, const rpc::DecodedMessage& message)
{
    if (message.text.empty()) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(cw_caption_mutex_);
        // Bounded for a stalled Qt thread, on kCaptionLimit's terms: more
        // than the waterfall would keep is never drawn.
        if (cw_pending_lines_.size() >= kCaptionLimit) {
            cw_pending_lines_.erase(cw_pending_lines_.begin());
        }
        cw_pending_lines_.push_back(CwCaptionLine{band, message});
    }
    if (!cw_wake_pending_.exchange(true, std::memory_order_acq_rel)) {
        QMetaObject::invokeMethod(this, [this] { drain_cw_captions(); }, Qt::QueuedConnection);
    }
}

void EngineLink::on_cw_caption_ended(std::int64_t center_hz)
{
    {
        const std::lock_guard<std::mutex> lock(cw_caption_mutex_);
        cw_ended_.push_back(center_hz);
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        decode_work_pending_ = true;
    }
    supervisor_wake_.notify_all();
}

void EngineLink::drain_cw_captions()
{
    cw_wake_pending_.store(false, std::memory_order_release);
    std::vector<CwCaptionLine> lines;
    {
        const std::lock_guard<std::mutex> lock(cw_caption_mutex_);
        lines.swap(cw_pending_lines_);
    }
    const rpc::SpectrumFrame& newest = frame();
    const auto rate = static_cast<std::uint64_t>(std::max(sourceRate(), 0));
    if (lines.empty() || !connected_ || newest.count == 0 || rate == 0) {
        return;
    }
    const std::uint64_t now = newest.start + newest.count;
    const auto length = static_cast<std::uint64_t>(kCwLineSeconds * static_cast<double>(rate));

    for (CwCaptionLine& line : lines) {
        const double wpm = field_real(line.message, "wpm");
        rpc::Transcript t;
        t.sequence = line.message.sequence;
        t.source_end = now;
        t.source_start = now > length ? now - length : 0;
        t.start_sample = line.message.start_sample;
        t.end_sample = line.message.end_sample;
        t.sample_rate = line.message.sample_rate;
        t.center_hz = line.band.center_hz;
        t.low_hz = line.band.low_hz;
        t.high_hz = line.band.high_hz;
        t.mode = "cw";
        t.text = std::move(line.message.text);
        t.confidence = 1.0F;
        t.fields = std::move(line.message.fields);

        CaptionFeedEntry entry;
        entry.serial = ++caption_serial_;
        entry.slot = -1;
        entry.card = std::format("CW  ·  {}{}\n{}\ndecoded without a receiver",
                                 exact_megahertz_text(t.center_hz),
                                 wpm > 0.0 ? std::format("  ·  {:.0f} WPM", wpm) : std::string(),
                                 one_line(t.text));
        entry.transcript = std::move(t);
        caption_feed_.push_back(std::move(entry));
    }
    while (caption_feed_.size() > kCaptionLimit) {
        caption_feed_.pop_front();
    }
    emit transcriptsArrived();
}

}  // namespace revenant::ui
