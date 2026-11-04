#include "core/detect/tier_two.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <tuple>

#include "core/engine/probe.h"

namespace revenant::detect {

namespace {

// Whether a finding is one an emitter's lines should all carry: a verified
// protocol, a family characterise::may_drive_detection accepted, or the side
// of a talker on a suppressed carrier, which never has a family to accept.
[[nodiscard]] bool settles(const ProbeFinding& finding) {
    return finding.protocol != identify::Protocol::None || finding.may_drive_detection ||
           finding.voice_sideband != characterise::VoiceSideband::Unknown;
}

// A unit's place in the classification schedule, compared field by field in
// the order the header states. A starved unit carries only its wait, so
// starved units go longest wait first whatever else they are.
struct Rank {
    int starved = 1;  // 0 when starved, so it sorts first
    int refine = 0;
    int unviewed = 0;
    int probed = 0;  // 0 never probed, 1 a first identification dwell, 2 a re-look
    double weakness = 0.0;  // minus the SNR, so the strongest sorts first
    dsp::SampleIndex key = 0;
    std::uint64_t id = 0;

    [[nodiscard]] bool operator<(const Rank& other) const {
        return std::tie(starved, refine, unviewed, probed, weakness, key, id) <
               std::tie(other.starved, other.refine, other.unviewed, other.probed,
                        other.weakness, other.key, other.id);
    }
};

[[nodiscard]] bool in_view(std::span<const ViewedSpan> viewed, dsp::Hertz low, dsp::Hertz high) {
    return std::any_of(viewed.begin(), viewed.end(), [&](const ViewedSpan& span) {
        return low <= span.high && high >= span.low;
    });
}

// `waiting_since` is when the unit became eligible: its birth when never
// probed, and its last look plus the retry interval for a re-look.
[[nodiscard]] Rank rank_of(int refine, int probed, bool viewed, double snr_db,
                           dsp::SampleIndex key, dsp::SampleIndex waiting_since,
                           dsp::SampleIndex now, dsp::SampleIndex starve_samples,
                           std::uint64_t id) {
    Rank rank;
    rank.id = id;
    if (refine == 0 && now - waiting_since >= starve_samples) {
        rank.starved = 0;
        rank.key = waiting_since;
        return rank;
    }
    rank.refine = refine;
    rank.unviewed = viewed ? 0 : 1;
    rank.probed = probed;
    rank.weakness = std::isfinite(snr_db) ? -snr_db : 0.0;
    rank.key = key;
    return rank;
}

}  // namespace

std::vector<Track> fold_emitters(std::span<const Track> tracks,
                                 std::span<const TierTwoEmitter> emitters) {
    std::unordered_map<std::uint64_t, std::size_t> by_id;
    by_id.reserve(tracks.size());
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        by_id.emplace(tracks[i].id, i);
    }

    // Track index to the folded detection it went into, and that detection's
    // id, for the lines of every emitter that folded.
    std::unordered_map<std::size_t, std::size_t> folded_into;
    std::vector<Track> folded;
    folded.reserve(emitters.size());

    for (const TierTwoEmitter& emitter : emitters) {
        std::vector<std::size_t> present;
        for (const std::uint64_t id : emitter.tracks) {
            const auto found = by_id.find(id);
            if (found == by_id.end()) {
                continue;
            }
            const TrackState state = tracks[found->second].state;
            if (state == TrackState::Live || state == TrackState::Held) {
                present.push_back(found->second);
            }
        }
        if (present.size() < 2 || emitter.high_edge <= emitter.low_edge) {
            continue;
        }

        // The anchor when it is among them, which it is whenever the emitter
        // list is the same decision's as the tracks; otherwise the strongest.
        std::size_t base = present.front();
        bool anchor_found = false;
        for (const std::size_t at : present) {
            if (tracks[at].id == emitter.anchor) {
                base = at;
                anchor_found = true;
                break;
            }
        }
        if (!anchor_found) {
            for (const std::size_t at : present) {
                if (tracks[at].snr_2500_db > tracks[base].snr_2500_db) {
                    base = at;
                }
            }
        }

        Track out = tracks[base];
        out.center = emitter.low_edge + (emitter.high_edge - emitter.low_edge) / 2;
        out.bandwidth = emitter.high_edge - emitter.low_edge;
        out.merged_into = 0;
        bool any_live = false;
        for (const std::size_t at : present) {
            const Track& line = tracks[at];
            out.id = std::min(out.id, line.id);
            out.confidence = std::max(out.confidence, line.confidence);
            out.first_seen = std::min(out.first_seen, line.first_seen);
            out.last_seen = std::max(out.last_seen, line.last_seen);
            out.last_detected = std::max(out.last_detected, line.last_detected);
            any_live = any_live || line.state == TrackState::Live;
        }
        out.state = any_live ? TrackState::Live : TrackState::Held;

        for (const std::size_t at : present) {
            folded_into[at] = folded.size();
        }
        folded.push_back(std::move(out));
    }

    std::vector<Track> result;
    result.reserve(tracks.size());
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (folded_into.contains(i)) {
            continue;
        }
        Track track = tracks[i];
        if (track.merged_into != 0) {
            if (const auto parent = by_id.find(track.merged_into); parent != by_id.end()) {
                if (const auto into = folded_into.find(parent->second);
                    into != folded_into.end()) {
                    track.merged_into = folded[into->second].id;
                }
            }
        }
        result.push_back(std::move(track));
    }
    for (Track& track : folded) {
        result.push_back(std::move(track));
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const Track& a, const Track& b) { return a.center < b.center; });
    return result;
}

Expected<TierTwo> TierTwo::create(const TierTwoConfig& config) {
    if (config.source_rate <= 0) {
        return fail("TierTwo needs the source rate to turn sample indices into seconds");
    }
    if (!std::isfinite(config.reprobe_seconds) || config.reprobe_seconds < 0.0) {
        return fail(std::format("reprobe_seconds must be zero or more, got {}",
                                config.reprobe_seconds));
    }
    if (!std::isfinite(config.starve_seconds) || config.starve_seconds < 0.0) {
        return fail(std::format("starve_seconds must be zero or more, got {}",
                                config.starve_seconds));
    }
    if (config.emitter_gap_hz < 0) {
        return fail(std::format("emitter_gap_hz must be zero or more, got {}",
                                config.emitter_gap_hz));
    }
    if (!std::isfinite(config.emitter_min_fill) || config.emitter_min_fill < 0.0 ||
        config.emitter_min_fill > 1.0) {
        return fail(std::format("emitter_min_fill must be between zero and one, got {}",
                                config.emitter_min_fill));
    }
    TierTwo out;
    out.config_ = config;
    if (config.emitter_gap_hz > 0) {
        auto grouper = LineGrouper::create(LineGroupConfig{.edge_gap_hz = config.emitter_gap_hz});
        if (!grouper) {
            return std::unexpected(with_context(grouper.error(), "tier two"));
        }
        out.grouper_.emplace(std::move(*grouper));
    }
    return out;
}

namespace {

// Every eligible track with its rank, unsorted. pick() and step() share it so
// a line on its own and an emitter are ordered by one rule.
[[nodiscard]] std::vector<Rank> rank_tracks(
    std::span<const Track> tracks,
    const std::unordered_map<std::uint64_t, dsp::SampleIndex>& attempts,
    std::span<const std::uint64_t> in_flight, dsp::SampleIndex now,
    dsp::SampleRate source_rate, double reprobe_seconds, double starve_seconds,
    std::span<const ViewedSpan> viewed) {
    const auto reprobe_samples =
        static_cast<dsp::SampleIndex>(reprobe_seconds * static_cast<double>(source_rate));
    const auto starve_samples =
        static_cast<dsp::SampleIndex>(starve_seconds * static_cast<double>(source_rate));
    const auto ranked = [&](int refine, int probed, const Track& track, dsp::SampleIndex key,
                            dsp::SampleIndex waiting_since) {
        const dsp::Hertz half = track.bandwidth / 2;
        return rank_of(refine, probed,
                       in_view(viewed, track.center - half, track.center + half),
                       track.snr_2500_db, key, waiting_since, now, starve_samples, track.id);
    };

    std::vector<Rank> eligible;
    for (const Track& track : tracks) {
        if (track.state != TrackState::Live) {
            continue;
        }
        if (std::find(in_flight.begin(), in_flight.end(), track.id) != in_flight.end()) {
            continue;
        }
        const auto tried = attempts.find(track.id);

        // A track with a family is probed again only to drill down its label,
        // core/detect/refine.h, and only behind everything still unnamed and
        // at kRefineReprobeFactor times the retry interval: the pool is small,
        // and a deeper label is worth less than a first one.
        if (track.classification != Classification::Unknown) {
            if (!refine::wants_refinement(track.refinement)) {
                continue;
            }
            const dsp::SampleIndex since =
                tried == attempts.end() ? track.first_seen : tried->second;
            if (now < since + kRefineReprobeFactor * reprobe_samples) {
                continue;
            }
            eligible.push_back(ranked(1, 0, track, since, since));
            continue;
        }
        if (tried == attempts.end()) {
            eligible.push_back(ranked(0, 0, track, track.first_seen, track.first_seen));
            continue;
        }
        if (now < tried->second + reprobe_samples) {
            continue;
        }
        eligible.push_back(
            ranked(0, 2, track, tried->second, tried->second + reprobe_samples));
    }
    return eligible;
}

}  // namespace

std::vector<std::uint64_t> TierTwo::pick(
    std::span<const Track> tracks,
    const std::unordered_map<std::uint64_t, dsp::SampleIndex>& attempts,
    std::span<const std::uint64_t> in_flight, std::size_t free, dsp::SampleIndex now,
    dsp::SampleRate source_rate, double reprobe_seconds, double starve_seconds,
    std::span<const ViewedSpan> viewed) {
    std::vector<Rank> eligible = rank_tracks(tracks, attempts, in_flight, now, source_rate,
                                             reprobe_seconds, starve_seconds, viewed);
    std::sort(eligible.begin(), eligible.end());
    std::vector<std::uint64_t> out;
    for (const Rank& entry : eligible) {
        if (out.size() >= free) {
            break;
        }
        out.push_back(entry.id);
    }
    return out;
}

std::vector<std::uint64_t> TierTwo::pick_identify(
    std::span<const Track> tracks, const std::unordered_map<std::uint64_t, std::uint32_t>& dwells,
    std::span<const std::uint64_t> in_flight, std::span<const std::uint64_t> already,
    std::size_t free) {
    // (dwells so far, birth, id): first dwells before retries, oldest first.
    std::vector<std::tuple<std::uint32_t, dsp::SampleIndex, std::uint64_t>> eligible;
    const auto listed = [](std::span<const std::uint64_t> ids, std::uint64_t id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };
    for (const Track& track : tracks) {
        const auto given = dwells.find(track.id);
        const std::uint32_t count = given == dwells.end() ? 0 : given->second;
        if (track.state != TrackState::Live || track.probes == 0 ||
            track.protocol != identify::Protocol::None ||
            track.bandwidth > engine::kProbeIdentifyNarrowHz || count >= kIdentifyDwells ||
            listed(in_flight, track.id) || listed(already, track.id)) {
            continue;
        }
        eligible.emplace_back(count, track.first_seen, track.id);
    }
    std::sort(eligible.begin(), eligible.end());
    std::vector<std::uint64_t> out;
    for (const auto& entry : eligible) {
        if (out.size() >= free) {
            break;
        }
        out.push_back(std::get<2>(entry));
    }
    return out;
}

void TierTwo::take(Detector& detector, engine::Engine& engine) {
    if (outcomes_.size() < 32) {
        outcomes_.resize(32);
    }
    for (;;) {
        const std::size_t count = engine.take_probe_outcomes(outcomes_);
        for (std::size_t i = 0; i < count; ++i) {
            const engine::ProbeOutcome& outcome = outcomes_[i];

            // Only an answer to a probe this TierTwo submitted. A server that
            // rebuilds its detector and its TierTwo across a retune numbers
            // tracks from one again, so an answer still in the pool's ring
            // from before would otherwise land on whichever new track took
            // the old id.
            const bool ours =
                std::find(in_flight_.begin(), in_flight_.end(), outcome.tag) != in_flight_.end();
            std::erase(in_flight_, outcome.tag);
            if (!ours) {
                ++stats_.stale;
                continue;
            }
            statuses_[outcome.tag] = outcome.status;

            // Whether this answered a whole emitter, and which lines it had.
            std::optional<GroupProbe> emitter;
            if (const auto found = group_probes_.find(outcome.tag); found != group_probes_.end()) {
                emitter = std::move(found->second);
                group_probes_.erase(found);
            }

            switch (outcome.status) {
                case engine::ProbeStatus::Characterised: break;
                case engine::ProbeStatus::TooWide: ++stats_.too_wide; continue;
                case engine::ProbeStatus::Unplaced: ++stats_.unplaced; continue;
                case engine::ProbeStatus::Cancelled: ++stats_.cancelled; continue;
                case engine::ProbeStatus::Failed: ++stats_.failed; continue;
            }

            ProbeFinding finding;
            finding.family = classification_of(outcome.family);
            finding.confidence = outcome.confidence;
            finding.symbol_rate_hz = outcome.symbol_rate_hz;
            finding.order = outcome.order;
            finding.tone_count = outcome.tone_count;
            finding.concentration = outcome.concentration;
            finding.may_drive_detection = outcome.may_drive_detection;
            finding.psk_without_symbol_rate = outcome.psk_without_symbol_rate;
            finding.double_sideband = outcome.double_sideband;
            finding.voice_sideband = outcome.voice_sideband;
            finding.protocol = outcome.protocol;
            finding.protocol_confidence = outcome.protocol_confidence;
            finding.protocols_unverified = outcome.protocols_unverified;
            finding.p25.tsbk = outcome.p25_tsbk;
            finding.p25.voice = outcome.p25_voice;
            finding.p25.nac = outcome.p25_nac;
            finding.p25.nac_steady = outcome.p25_nac_steady;
            finding.occupied_hz = static_cast<double>(outcome.occupied_hz);

            // Whether this is the track's first family, read before the
            // detector writes it, so the time to first classification is
            // taken once per track and not once per accepted probe.
            bool first = false;
            dsp::SampleIndex born = 0;
            for (const Track& track : detector.tracks()) {
                if (track.id == outcome.tag) {
                    first = track.classification == Classification::Unknown;
                    born = track.first_seen;
                    break;
                }
            }

            // An emitter's answer goes on every line it had, and on the
            // group, so a line that joins it later is given it too.
            bool landed = static_cast<bool>(detector.record_probe(outcome.tag, finding));
            if (emitter.has_value()) {
                for (const std::uint64_t line : emitter->tracks) {
                    if (line == outcome.tag) {
                        continue;
                    }
                    if (detector.record_probe(line, finding)) {
                        landed = true;
                        ++stats_.inherited;
                    }
                }
                if (settles(finding)) {
                    group_findings_[emitter->group] = finding;
                    for (const std::uint64_t line : emitter->tracks) {
                        inherited_[line] = finding;
                    }
                }
            }

            if (!landed) {
                ++stats_.orphaned;
                continue;
            }
            ++stats_.recorded;
            if (finding.protocol != identify::Protocol::None) {
                ++stats_.protocols[static_cast<std::size_t>(finding.protocol)];
            }
            const auto family = static_cast<std::size_t>(finding.family);
            ++stats_.named[family];
            if (!finding.may_drive_detection) {
                ++stats_.refused_by_characterise;
                continue;
            }
            ++stats_.accepted;
            ++stats_.accepted_as[family];
            if (first) {
                const double seconds = static_cast<double>(detector.last_decision() - born) /
                                       static_cast<double>(config_.source_rate);
                stats_.first_classification_seconds_min =
                    stats_.first_classifications == 0
                        ? seconds
                        : std::min(stats_.first_classification_seconds_min, seconds);
                stats_.first_classification_seconds_max =
                    std::max(stats_.first_classification_seconds_max, seconds);
                stats_.first_classification_seconds_total += seconds;
                ++stats_.first_classifications;
            }
        }
        if (count < outcomes_.size()) {
            break;
        }
    }
}

bool TierTwo::is_emitter(const LineGroup& group) const {
    const dsp::Hertz extent = group.high_edge - group.low_edge;
    if (extent <= 0) {
        return false;
    }
    dsp::Hertz covered = 0;
    for (const GroupMember& member : grouper_->members(group)) {
        covered += member.bandwidth;
    }
    return static_cast<double>(covered) >=
           config_.emitter_min_fill * static_cast<double>(extent);
}

void TierTwo::propagate(Detector& detector) {
    if (!grouper_.has_value()) {
        return;
    }
    for (const LineGroup& group : grouper_->groups()) {
        if (!is_emitter(group)) {
            continue;
        }
        const std::span<const GroupMember> members = grouper_->members(group);

        // The group's own answer, or failing that one a line carried in from
        // an earlier incarnation of the same emitter: the anchor's first.
        const ProbeFinding* answer = nullptr;
        if (const auto found = group_findings_.find(group.id); found != group_findings_.end()) {
            answer = &found->second;
        } else {
            if (const auto found_anchor = inherited_.find(group.anchor);
                found_anchor != inherited_.end()) {
                answer = &found_anchor->second;
            } else {
                for (const GroupMember& member : members) {
                    if (const auto carried = inherited_.find(member.track);
                        carried != inherited_.end()) {
                        answer = &carried->second;
                        break;
                    }
                }
            }
            if (answer != nullptr) {
                answer = &(group_findings_[group.id] = *answer);
            }
        }
        if (answer == nullptr) {
            continue;
        }
        for (const GroupMember& member : members) {
            if (inherited_.contains(member.track)) {
                continue;
            }
            if (detector.record_probe(member.track, *answer)) {
                ++stats_.inherited;
            }
            inherited_[member.track] = *answer;
        }
    }
}

std::optional<engine::ProbeStatus> TierTwo::last_status(std::uint64_t track_id) const {
    const auto found = statuses_.find(track_id);
    if (found == statuses_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::vector<TierTwoEmitter> TierTwo::emitters() const {
    std::vector<TierTwoEmitter> out;
    if (!grouper_.has_value()) {
        return out;
    }
    for (const LineGroup& group : grouper_->groups()) {
        if (!is_emitter(group)) {
            continue;
        }
        TierTwoEmitter emitter;
        emitter.group = group.id;
        emitter.anchor = group.anchor;
        emitter.low_edge = group.low_edge;
        emitter.high_edge = group.high_edge;
        for (const GroupMember& member : grouper_->members(group)) {
            emitter.tracks.push_back(member.track);
        }
        if (const auto found = group_findings_.find(group.id); found != group_findings_.end()) {
            emitter.answered = true;
            emitter.finding = found->second;
        }
        out.push_back(std::move(emitter));
    }
    return out;
}

Status TierTwo::step(Detector& detector, engine::Engine& engine) {
    take(detector, engine);

    const std::span<const Track> tracks = detector.tracks();
    const dsp::SampleIndex now = detector.last_decision();

    if (grouper_.has_value()) {
        if (auto observed = grouper_->observe(tracks, now); !observed) {
            return std::unexpected(with_context(observed.error(), "tier two"));
        }
        propagate(detector);
    }

    // Forget attempts on tracks that have gone, so the maps are bounded by the
    // track list rather than by the length of the run, and on groups that
    // have gone likewise.
    const auto gone = [&](const auto& entry) {
        return std::none_of(tracks.begin(), tracks.end(),
                            [&](const Track& track) { return track.id == entry.first; });
    };
    std::erase_if(attempts_, gone);
    std::erase_if(statuses_, gone);
    std::erase_if(inherited_, gone);
    std::vector<std::uint64_t> live_groups;
    std::vector<std::uint64_t> grouped;
    if (grouper_.has_value()) {
        for (const LineGroup& group : grouper_->groups()) {
            if (!is_emitter(group)) {
                continue;
            }
            live_groups.push_back(group.id);
            for (const GroupMember& member : grouper_->members(group)) {
                grouped.push_back(member.track);
            }
        }
    }
    const auto group_gone = [&](const auto& entry) {
        return std::find(live_groups.begin(), live_groups.end(), entry.first) == live_groups.end();
    };
    std::erase_if(group_attempts_, group_gone);
    std::erase_if(group_findings_, group_gone);
    std::erase_if(group_identify_dwells_, group_gone);

    const engine::ProbeStats pool = engine.probe_stats();
    if (pool.size == 0 || in_flight_.size() >= pool.size) {
        return {};
    }
    const std::size_t free = pool.size - in_flight_.size();
    const auto listed = [](std::span<const std::uint64_t> ids, std::uint64_t id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };

    // The lines on their own: every track that is not in a group.
    std::vector<Track> alone;
    alone.reserve(tracks.size());
    for (const Track& track : tracks) {
        if (!listed(grouped, track.id)) {
            alone.push_back(track);
        }
    }

    // One unit per line on its own and one per emitter, ordered together by
    // the rule the header states. An emitter is ranked on its anchor's SNR,
    // its whole extent against the view, and the decision its group formed as
    // its birth.
    struct Unit {
        Rank rank;
        std::uint64_t tag = 0;
        const LineGroup* group = nullptr;
        bool identify = false;
    };
    std::vector<Unit> units;
    for (const Rank& rank : rank_tracks(alone, attempts_, in_flight_, now, config_.source_rate,
                                        config_.reprobe_seconds, config_.starve_seconds,
                                        viewed_)) {
        units.push_back(Unit{.rank = rank, .tag = rank.id});
    }
    const auto reprobe_samples = static_cast<dsp::SampleIndex>(
        config_.reprobe_seconds * static_cast<double>(config_.source_rate));
    const auto starve_samples = static_cast<dsp::SampleIndex>(
        config_.starve_seconds * static_cast<double>(config_.source_rate));
    if (grouper_.has_value()) {
        for (const LineGroup& group : grouper_->groups()) {
            if (!is_emitter(group) || group_findings_.contains(group.id)) {
                continue;
            }
            const auto anchor = std::find_if(tracks.begin(), tracks.end(), [&](const Track& t) {
                return t.id == group.anchor;
            });
            if (anchor == tracks.end() || anchor->state != TrackState::Live) {
                continue;
            }
            const std::span<const GroupMember> members = grouper_->members(group);
            if (std::any_of(members.begin(), members.end(), [&](const GroupMember& member) {
                    return listed(in_flight_, member.track);
                })) {
                continue;
            }
            const auto tried = group_attempts_.find(group.id);
            if (tried != group_attempts_.end() && now < tried->second + reprobe_samples) {
                continue;
            }
            const bool fresh = tried == group_attempts_.end();
            const dsp::SampleIndex key = fresh ? group.formed : tried->second;
            units.push_back(Unit{
                .rank = rank_of(0, fresh ? 0 : 2, in_view(viewed_, group.low_edge, group.high_edge),
                                anchor->snr_2500_db, key, fresh ? key : key + reprobe_samples, now,
                                starve_samples, group.anchor),
                .tag = group.anchor,
                .group = &group});
        }
    }
    // THE IDENTIFICATION DWELLS, ranked with everything above rather than
    // given what it leaves. A narrow track that has been characterised and
    // not yet verified is owed a long dwell, and its first one ranks behind
    // the never-probed tracks and emitters and ahead of every re-look; a
    // retry ranks with the re-looks. Not level with the never-probed: an AM
    // talker's carrier line, strong and narrow, then took its dwell before its
    // emitter's first probe, verified the syllables as Morse, and read CW on
    // seed 2 of the labelled scene. WHAT THIS USED TO
    // DO: run only on the pool left over after the classification schedule.
    // Once that schedule put strong tracks first, its re-looks of strong
    // tracks kept a pool of four full and RTTY on seed 2 of the labelled
    // scene never had its framing verified in 30 s (2026-11-03).
    std::erase_if(identify_dwells_, gone);
    std::vector<std::uint64_t> identifying;
    std::vector<const LineGroup*> identifying_groups;
    {
        const std::vector<std::uint64_t> chosen;
        // Not a line an emitter's answer reached, and not one called AM: the
        // long dwell is for the narrow data modes, and a talker's syllables
        // key a carrier the way Morse does. In the voice survey a sideband
        // stretch of an AM talker, 907 Hz wide and so narrow by the bar, was
        // given the long dwell at 30 dB while its group had come apart and
        // verified CW.
        std::vector<Track> narrow;
        for (const Track& track : alone) {
            if (!inherited_.contains(track.id) && !track.classification_double_sideband) {
                narrow.push_back(track);
            }
        }
        identifying = pick_identify(narrow, identify_dwells_, in_flight_, chosen, narrow.size());

        // A narrow emitter gets the same long dwells once its first emitter
        // probe has answered and named no protocol: RTTY's two tones read as
        // two lines are one such emitter. First dwells before retries here as
        // well, and otherwise in the grouper's order.
        std::vector<std::pair<std::uint32_t, const LineGroup*>> owed;
        if (grouper_.has_value()) {
            for (const LineGroup& group : grouper_->groups()) {
                if (!is_emitter(group) ||
                    (!group_findings_.contains(group.id) && !group_attempts_.contains(group.id))) {
                    continue;
                }
                const auto given = group_identify_dwells_.find(group.id);
                const std::uint32_t count =
                    given == group_identify_dwells_.end() ? 0 : given->second;
                if (group.high_edge - group.low_edge > engine::kProbeIdentifyNarrowHz ||
                    count >= kIdentifyDwells || listed(chosen, group.anchor)) {
                    continue;
                }
                const auto anchor = std::find_if(tracks.begin(), tracks.end(), [&](const Track& t) {
                    return t.id == group.anchor;
                });
                if (anchor == tracks.end() || anchor->state != TrackState::Live ||
                    anchor->protocol != identify::Protocol::None) {
                    continue;
                }
                const std::span<const GroupMember> members = grouper_->members(group);
                if (std::any_of(members.begin(), members.end(), [&](const GroupMember& member) {
                        return listed(in_flight_, member.track);
                    })) {
                    continue;
                }
                owed.emplace_back(count, &group);
            }
        }
        for (const auto& entry : owed) {
            identifying_groups.push_back(entry.second);
        }
    }

    // One ranked list. A dwell replaces the same track's or emitter's
    // classification re-look, since the long dwell characterises as well.
    {
        const auto snr_of = [&](std::uint64_t id) {
            const auto found = std::find_if(tracks.begin(), tracks.end(),
                                            [id](const Track& t) { return t.id == id; });
            return found == tracks.end() ? 0.0 : found->snr_2500_db;
        };
        const auto born_of = [&](std::uint64_t id) {
            const auto found = std::find_if(tracks.begin(), tracks.end(),
                                            [id](const Track& t) { return t.id == id; });
            return found == tracks.end() ? now : found->first_seen;
        };
        for (const std::uint64_t id : identifying) {
            std::erase_if(units, [id](const Unit& unit) { return unit.tag == id; });
            const auto given = identify_dwells_.find(id);
            const bool retry = given != identify_dwells_.end() && given->second > 0;
            const auto found = std::find_if(tracks.begin(), tracks.end(),
                                            [id](const Track& t) { return t.id == id; });
            const dsp::Hertz half = found == tracks.end() ? 0 : found->bandwidth / 2;
            const dsp::Hertz center = found == tracks.end() ? 0 : found->center;
            // A dwell never starves: it is owed only a bounded number of times.
            units.push_back(Unit{.rank = rank_of(0, retry ? 2 : 1,
                                                 in_view(viewed_, center - half, center + half),
                                                 snr_of(id), born_of(id), now, now,
                                                 starve_samples, id),
                                 .tag = id,
                                 .identify = true});
        }
        for (const LineGroup* group : identifying_groups) {
            std::erase_if(units, [&](const Unit& unit) { return unit.tag == group->anchor; });
            const auto given = group_identify_dwells_.find(group->id);
            const bool retry = given != group_identify_dwells_.end() && given->second > 0;
            units.push_back(
                Unit{.rank = rank_of(0, retry ? 2 : 1,
                                     in_view(viewed_, group->low_edge, group->high_edge),
                                     snr_of(group->anchor), group->formed, now, now,
                                     starve_samples, group->anchor),
                     .tag = group->anchor,
                     .group = group,
                     .identify = true});
        }
    }
    std::sort(units.begin(), units.end(),
              [](const Unit& a, const Unit& b) { return a.rank < b.rank; });
    if (units.size() > free) {
        units.resize(free);
    }
    identifying.clear();
    identifying_groups.clear();
    std::erase_if(units, [&](const Unit& unit) {
        if (!unit.identify) {
            return false;
        }
        if (unit.group != nullptr) {
            identifying_groups.push_back(unit.group);
        } else {
            identifying.push_back(unit.tag);
        }
        return true;
    });
    if (units.empty() && identifying.empty() && identifying_groups.empty()) {
        return {};
    }

    // The widest occupied bandwidth this grid's probes can be asked for: the
    // largest bucket its channels carry, over the most a signal may occupy of
    // one. An emitter wider than that is asked about at its centre, and the
    // probe still passes half its bucket, twice this.
    dsp::Hertz widest = 0;
    for (const dsp::SampleRate rate : engine::kProbeRates) {
        if (rate <= engine.info().channel_rate) {
            widest = rate / engine::kProbeRateOverOccupied;
        }
    }

    const dsp::Hertz source_center = engine.info().source_center;
    const auto submit = [&](std::uint64_t tag, dsp::Hertz center, dsp::Hertz occupied,
                            bool long_dwell) -> Status {
        engine::ProbeRequest request{};
        request.tag = tag;
        request.center = center - source_center;
        request.occupied_hz = occupied;
        request.dwell_seconds = long_dwell ? engine::kProbeIdentifyDwellSeconds : 0.0;
        if (auto submitted = engine.submit_probe(request); !submitted) {
            ++stats_.refused;
            return std::unexpected(with_context(submitted.error(), "tier two"));
        }
        in_flight_.push_back(tag);
        ++stats_.submitted;
        return {};
    };
    const auto submit_group = [&](const LineGroup& group, bool long_dwell) -> Status {
        const dsp::Hertz extent = std::max<dsp::Hertz>(1, group.high_edge - group.low_edge);
        const dsp::Hertz occupied = widest > 0 ? std::min(extent, widest) : extent;
        const dsp::Hertz center = group.low_edge + extent / 2;
        if (auto sent = submit(group.anchor, center, occupied, long_dwell); !sent) {
            return sent;
        }
        GroupProbe probe;
        probe.group = group.id;
        for (const GroupMember& member : grouper_->members(group)) {
            probe.tracks.push_back(member.track);
        }
        group_probes_[group.anchor] = std::move(probe);
        ++stats_.emitter_submitted;
        return {};
    };

    for (const Unit& unit : units) {
        if (unit.group != nullptr) {
            if (auto sent = submit_group(*unit.group, false); !sent) {
                return sent;
            }
            group_attempts_[unit.group->id] = now;
            continue;
        }
        const auto found = std::find_if(tracks.begin(), tracks.end(),
                                        [&](const Track& track) { return track.id == unit.tag; });
        if (found == tracks.end()) {
            continue;
        }
        if (auto sent = submit(unit.tag, found->center, found->bandwidth, false); !sent) {
            return sent;
        }
        attempts_[unit.tag] = now;
    }
    for (const std::uint64_t id : identifying) {
        const auto found = std::find_if(tracks.begin(), tracks.end(),
                                        [id](const Track& track) { return track.id == id; });
        if (found == tracks.end()) {
            continue;
        }
        if (auto sent = submit(id, found->center, found->bandwidth, true); !sent) {
            return sent;
        }
        attempts_[id] = now;
        if (identify_dwells_[id]++ > 0) {
            ++stats_.identify_retried;
        }
        ++stats_.identify_submitted;
    }
    for (const LineGroup* group : identifying_groups) {
        if (auto sent = submit_group(*group, true); !sent) {
            return sent;
        }
        if (group_identify_dwells_[group->id]++ > 0) {
            ++stats_.identify_retried;
        }
        ++stats_.identify_submitted;
    }
    return {};
}

}  // namespace revenant::detect
