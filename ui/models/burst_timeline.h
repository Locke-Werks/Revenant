// The burst timeline's bookkeeping: transmissions as bursts, one lane per
// frequency, with what was said and decoded during each pinned to it.
//
// Qt-free and wire-free, so ui/tests can link it, on the argument
// models/capture_note.h makes. models/burst_timeline_model.cpp feeds it from
// EngineLink's detection list, transcripts, decoded messages and adopted
// receivers, and hands its rows to ui/qml/BurstWindow.qml.
//
// ONE CLOCK: UTC NANOSECONDS. Detections count in source samples, decoded
// messages in a receiver's own stream, captures in wall clock. The model turns
// each into wall clock once, at arrival, because a burst is looked up against
// the capture by wall clock and exported with one, and two clocks inside the
// bookkeeping would be two answers to "when".
//
// WHY A BURST AND NOT A DETECTION. A detector track outlives a transmission:
// it is held after the signal stops and may be detected again on the same id.
// A burst is one stretch of activity, closed once nothing has been seen on its
// lane for the hang time, so a repeater keyed ten times is ten bursts on one
// lane rather than one bar across the hour.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace revenant::ui {

inline constexpr std::int64_t kBurstSecondNs = 1'000'000'000LL;

struct BurstConfig {
    // A lane that goes quiet for longer than this closes its burst; activity
    // inside it extends the same one. Two seconds covers a PTT gap on a
    // conversational channel without merging separate calls.
    std::int64_t hang_ns = 2 * kBurstSecondNs;

    // Two centres closer than this are one lane, so a carrier's estimate
    // wandering by a few hundred hertz between decisions does not open a
    // second row for the same channel.
    std::int64_t lane_tolerance_hz = 5'000;

    // How much history is kept: the older of the two bounds wins.
    std::int64_t max_age_ns = 24LL * 3600LL * kBurstSecondNs;
    std::size_t max_bursts = 20'000;

    // Attachments are capped per burst, so one long call with a chatty
    // decoder cannot hold thousands of rows.
    std::size_t max_attachments = 64;

    // An attachment that matched nothing yet is held this long in case its
    // burst is still to be observed: a transcript can land before the
    // detector's next decision does.
    std::int64_t pending_ns = 30 * kBurstSecondNs;
    std::size_t max_pending = 512;
};

// Something was active on a frequency over [start_ns, end_ns]. `source` keys
// an observation to the thing that produced it, a detector track or a
// receiver, so that consecutive observations of one source extend one burst.
struct BurstObservation {
    std::uint64_t source = 0;
    std::int64_t center_hz = 0;
    std::int64_t bandwidth_hz = 0;
    std::int64_t start_ns = 0;
    std::int64_t end_ns = 0;
    std::string mode;
    double snr_db = 0.0;
};

enum class BurstAttachmentKind { Transcript, Decoded };

struct BurstAttachment {
    BurstAttachmentKind kind = BurstAttachmentKind::Decoded;
    std::int64_t start_ns = 0;
    std::int64_t end_ns = 0;
    std::int64_t hz = 0;
    // The decoder's name for a decoded message, the mode for a transcript.
    std::string origin;
    std::string text;
    // Identifiers as "key=value": talkgroup, unit, callsign, PI.
    std::vector<std::string> ids;
};

struct Burst {
    std::uint64_t id = 0;
    std::uint64_t source = 0;
    std::int64_t lane_hz = 0;
    std::int64_t center_hz = 0;
    std::int64_t bandwidth_hz = 0;
    std::int64_t start_ns = 0;
    std::int64_t end_ns = 0;
    bool open = true;
    std::string mode;
    double peak_snr_db = 0.0;
    std::vector<BurstAttachment> attachments;

    [[nodiscard]] std::int64_t duration_ns() const { return std::max<std::int64_t>(0, end_ns - start_ns); }

    // Every identifier any attachment carried, once each, in arrival order.
    [[nodiscard]] std::vector<std::string> ids() const
    {
        std::vector<std::string> out;
        for (const BurstAttachment& a : attachments) {
            for (const std::string& term : a.ids) {
                if (std::find(out.begin(), out.end(), term) == out.end()) {
                    out.push_back(term);
                }
            }
        }
        return out;
    }

    [[nodiscard]] std::string transcript() const
    {
        std::string out;
        for (const BurstAttachment& a : attachments) {
            if (a.kind != BurstAttachmentKind::Transcript) {
                continue;
            }
            if (!out.empty()) {
                out += ' ';
            }
            out += a.text;
        }
        return out;
    }

    [[nodiscard]] std::string decoded() const
    {
        std::string out;
        for (const BurstAttachment& a : attachments) {
            if (a.kind != BurstAttachmentKind::Decoded || a.text.empty()) {
                continue;
            }
            if (!out.empty()) {
                out += " | ";
            }
            out += a.text;
        }
        return out;
    }
};

class BurstTimeline {
public:
    explicit BurstTimeline(BurstConfig config = {}) : config_(config) {}

    [[nodiscard]] const BurstConfig& config() const { return config_; }
    [[nodiscard]] const std::deque<Burst>& bursts() const { return bursts_; }
    [[nodiscard]] std::size_t pending() const { return pending_.size(); }

    // Folds one observation in: extends the open burst of the same source,
    // or the open burst on the same lane that it reaches within the hang
    // time, or opens a new burst.
    void observe(const BurstObservation& obs)
    {
        if (obs.end_ns < obs.start_ns) {
            return;
        }
        const std::int64_t lane = lane_for(obs.center_hz);
        Burst* target = nullptr;
        // Open bursts are few and nearly always the newest, so the walk
        // stops once it has passed all of them rather than reading a day of
        // history per observation.
        std::size_t opens_seen = 0;
        for (auto it = bursts_.rbegin(); it != bursts_.rend() && opens_seen < open_count_; ++it) {
            if (!it->open) {
                continue;
            }
            ++opens_seen;
            const bool same_source = obs.source != 0 && it->source == obs.source;
            const bool same_lane = it->lane_hz == lane;
            if ((same_source || same_lane) && obs.start_ns <= it->end_ns + config_.hang_ns &&
                obs.end_ns >= it->start_ns - config_.hang_ns) {
                target = &*it;
                break;
            }
        }
        if (target == nullptr) {
            Burst burst;
            burst.id = next_id_++;
            burst.source = obs.source;
            burst.lane_hz = lane;
            burst.center_hz = obs.center_hz;
            burst.bandwidth_hz = obs.bandwidth_hz;
            // A source seen before keeps its old start; a burst on it begins
            // no earlier than the last one ended, or a held track's first
            // sighting would stretch every later burst back to it.
            burst.start_ns = std::max(obs.start_ns, last_end_of(obs.source));
            burst.end_ns = std::max(obs.end_ns, burst.start_ns);
            burst.mode = obs.mode;
            burst.peak_snr_db = obs.snr_db;
            insert_sorted(std::move(burst));
        } else {
            target->start_ns = std::min(target->start_ns, std::max(obs.start_ns, last_end_of(obs.source)));
            target->end_ns = std::max(target->end_ns, obs.end_ns);
            target->center_hz = obs.center_hz;
            target->bandwidth_hz = std::max(target->bandwidth_hz, obs.bandwidth_hz);
            if (!obs.mode.empty()) {
                target->mode = obs.mode;
            }
            target->peak_snr_db = std::max(target->peak_snr_db, obs.snr_db);
            if (obs.source != 0) {
                target->source = obs.source;
            }
        }
        // Pending attachments are retried on the tick: retrying them here
        // was every pending one against every burst, per observation.
    }

    // Pins a transcript or decoded message to the burst it overlaps in both
    // time and frequency, the one it overlaps longest when there are several.
    // Held as pending when it overlaps none yet.
    void attach(BurstAttachment att, std::int64_t now_ns)
    {
        if (!place(att)) {
            pending_.push_back({std::move(att), now_ns});
            while (pending_.size() > config_.max_pending) {
                pending_.pop_front();
            }
        }
    }

    // Closes bursts quiet for longer than the hang time, drops pending
    // attachments that never found a burst, and trims history to its bounds.
    void tick(std::int64_t now_ns)
    {
        retry_pending();
        std::size_t opens_seen = 0;
        const std::size_t opens = open_count_;
        for (auto it = bursts_.rbegin(); it != bursts_.rend() && opens_seen < opens; ++it) {
            if (!it->open) {
                continue;
            }
            ++opens_seen;
            if (now_ns - it->end_ns > config_.hang_ns) {
                it->open = false;
                --open_count_;
                remember_end(*it);
            }
        }
        while (!pending_.empty() && now_ns - pending_.front().second > config_.pending_ns) {
            pending_.pop_front();
        }
        const std::int64_t oldest = now_ns - config_.max_age_ns;
        // Ordered by start, so the front is the oldest and an aged burst
        // behind a younger one is caught on a later tick.
        while (!bursts_.empty() &&
               (bursts_.size() > config_.max_bursts ||
                (!bursts_.front().open && bursts_.front().end_ns < oldest))) {
            forget_front();
        }
    }

    void clear()
    {
        bursts_.clear();
        lane_counts_.clear();
        open_count_ = 0;
        pending_.clear();
        last_ends_.clear();
    }

    // Lanes in ascending frequency, one per distinct lane any kept burst has.
    [[nodiscard]] std::vector<std::int64_t> lanes() const
    {
        std::vector<std::int64_t> out;
        out.reserve(lane_counts_.size());
        for (const auto& [hz, count] : lane_counts_) {
            out.push_back(hz);
        }
        return out;
    }

    // Bursts with any part in [from_ns, to_ns], oldest first.
    [[nodiscard]] std::vector<const Burst*> visible(std::int64_t from_ns, std::int64_t to_ns) const
    {
        std::vector<const Burst*> out;
        for (const Burst& b : bursts_) {
            if (b.start_ns <= to_ns && b.end_ns >= from_ns) {
                out.push_back(&b);
            }
        }
        return out;
    }

    [[nodiscard]] const Burst* find(std::uint64_t id) const
    {
        for (const Burst& b : bursts_) {
            if (b.id == id) {
                return &b;
            }
        }
        return nullptr;
    }

private:
    [[nodiscard]] std::int64_t lane_for(std::int64_t hz) const
    {
        // The nearest lane within tolerance: the first at or above, or the
        // one below it.
        std::int64_t best = hz;
        std::int64_t best_gap = config_.lane_tolerance_hz + 1;
        const auto consider = [&](std::int64_t lane) {
            const std::int64_t gap = std::llabs(lane - hz);
            if (gap <= config_.lane_tolerance_hz && gap < best_gap) {
                best = lane;
                best_gap = gap;
            }
        };
        const auto above = lane_counts_.lower_bound(hz);
        if (above != lane_counts_.end()) {
            consider(above->first);
        }
        if (above != lane_counts_.begin()) {
            consider(std::prev(above)->first);
        }
        return best;
    }

    void forget_front()
    {
        const Burst& b = bursts_.front();
        if (b.open) {
            --open_count_;
        }
        if (const auto lane = lane_counts_.find(b.lane_hz); lane != lane_counts_.end()) {
            if (--lane->second == 0) {
                lane_counts_.erase(lane);
            }
        }
        bursts_.pop_front();
    }

    void insert_sorted(Burst burst)
    {
        ++lane_counts_[burst.lane_hz];
        if (burst.open) {
            ++open_count_;
        }
        auto at = std::upper_bound(bursts_.begin(), bursts_.end(), burst.start_ns,
                                   [](std::int64_t start, const Burst& b) { return start < b.start_ns; });
        bursts_.insert(at, std::move(burst));
    }

    [[nodiscard]] std::int64_t last_end_of(std::uint64_t source) const
    {
        if (source == 0) {
            return std::numeric_limits<std::int64_t>::min();
        }
        for (const auto& [s, end] : last_ends_) {
            if (s == source) {
                return end;
            }
        }
        return std::numeric_limits<std::int64_t>::min();
    }

    void remember_end(const Burst& b)
    {
        if (b.source == 0) {
            return;
        }
        for (auto& [s, end] : last_ends_) {
            if (s == b.source) {
                end = std::max(end, b.end_ns);
                return;
            }
        }
        last_ends_.emplace_back(b.source, b.end_ns);
        // Detector ids are never reused, so the oldest are dead weight.
        if (last_ends_.size() > 4096) {
            last_ends_.erase(last_ends_.begin(), last_ends_.begin() + 1024);
        }
    }

    bool place(BurstAttachment& att)
    {
        Burst* best = nullptr;
        std::int64_t best_overlap = -1;
        std::int64_t best_gap = 0;
        // Newest first, and no further back than an hour before it began: a
        // burst that started earlier and still overlaps it is a receiver held
        // open that long, and the walk would otherwise read the whole day.
        constexpr std::int64_t kReachNs = 3600 * kBurstSecondNs;
        for (auto it = bursts_.rbegin(); it != bursts_.rend(); ++it) {
            Burst& b = *it;
            if (b.start_ns < att.start_ns - kReachNs) {
                break;
            }
            const std::int64_t half = std::max(b.bandwidth_hz / 2, config_.lane_tolerance_hz);
            const std::int64_t gap = std::llabs(att.hz - b.center_hz);
            if (gap > half && std::llabs(att.hz - b.lane_hz) > half) {
                continue;
            }
            // A decoded message is stamped when it completed, so the burst's
            // hang time counts as part of it.
            const std::int64_t lo = std::max(att.start_ns, b.start_ns);
            const std::int64_t hi = std::min(att.end_ns, b.end_ns + config_.hang_ns);
            if (hi < lo) {
                continue;
            }
            const std::int64_t overlap = hi - lo;
            if (overlap > best_overlap || (overlap == best_overlap && gap < best_gap)) {
                best = &b;
                best_overlap = overlap;
                best_gap = gap;
            }
        }
        if (best == nullptr) {
            return false;
        }
        if (best->attachments.size() < config_.max_attachments) {
            best->attachments.push_back(std::move(att));
        }
        return true;
    }

    void retry_pending()
    {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (place(it->first)) {
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }

    BurstConfig config_;
    std::deque<Burst> bursts_;
    // Kept beside the history so neither a lane lookup nor a walk for open
    // bursts reads all of it.
    std::map<std::int64_t, std::size_t> lane_counts_;
    std::size_t open_count_ = 0;
    std::deque<std::pair<BurstAttachment, std::int64_t>> pending_;
    std::vector<std::pair<std::uint64_t, std::int64_t>> last_ends_;
    std::uint64_t next_id_ = 1;
};

// ---- export ----------------------------------------------------------------

// 2026-10-20T14:03:07.250Z, to the millisecond.
[[nodiscard]] inline std::string burst_utc_text(std::int64_t ns)
{
    using namespace std::chrono;
    const sys_time<milliseconds> ms{milliseconds{ns / 1'000'000}};
    const auto secs = floor<seconds>(ms);
    const auto frac = (ms - secs).count();
    return std::format("{:%Y-%m-%dT%H:%M:%S}.{:03}Z", secs, frac);
}

// RFC 4180: a field holding a comma, quote or line break is quoted, with its
// quotes doubled.
[[nodiscard]] inline std::string burst_csv_field(std::string_view text)
{
    if (text.find_first_of(",\"\r\n") == std::string_view::npos) {
        return std::string(text);
    }
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"') {
            out += '"';
        }
        out += c;
    }
    out += '"';
    return out;
}

inline constexpr std::string_view kBurstCsvHeader =
    "start_utc,end_utc,duration_s,frequency_hz,bandwidth_hz,mode,peak_snr_db,ids,decoded,transcript";

[[nodiscard]] inline std::string burst_csv(const std::vector<const Burst*>& bursts)
{
    std::string out(kBurstCsvHeader);
    out += "\r\n";
    for (const Burst* b : bursts) {
        std::string ids;
        for (const std::string& id : b->ids()) {
            if (!ids.empty()) {
                ids += ' ';
            }
            ids += id;
        }
        out += std::format("{},{},{:.3f},{},{},{},{:.1f},{},{},{}\r\n", burst_utc_text(b->start_ns),
                           burst_utc_text(b->end_ns),
                           static_cast<double>(b->duration_ns()) / static_cast<double>(kBurstSecondNs),
                           b->center_hz, b->bandwidth_hz, burst_csv_field(b->mode), b->peak_snr_db,
                           burst_csv_field(ids), burst_csv_field(b->decoded()),
                           burst_csv_field(b->transcript()));
    }
    return out;
}

// ---- capture ---------------------------------------------------------------

// A capture:// replay over [from_ns, to_ns] on the root another replay URI
// names, which is the only way this client learns the capture's root: the
// engine composes one per search hit (core/rpc/server.cpp's replay_uri).
// nullopt when `any_hit_uri` is not a capture URI.
[[nodiscard]] inline std::optional<std::string> burst_replay_uri(std::string_view any_hit_uri,
                                                                 std::int64_t from_ns,
                                                                 std::int64_t to_ns)
{
    if (!any_hit_uri.starts_with("capture:")) {
        return std::nullopt;
    }
    const std::string_view root = any_hit_uri.substr(0, any_hit_uri.find('?'));
    return std::format("{}?from={}&to={}&pace=1", root, from_ns, to_ns);
}

}  // namespace revenant::ui
