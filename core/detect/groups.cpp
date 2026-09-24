#include "core/detect/groups.h"

#include <algorithm>
#include <format>
#include <utility>

namespace revenant::detect {

namespace {

[[nodiscard]] bool groupable(TrackState state)
{
    switch (state) {
        case TrackState::Live:
        case TrackState::Held: return true;
        case TrackState::Pending:
        case TrackState::Merged: return false;
    }
    return false;
}

}  // namespace

Expected<LineGrouper> LineGrouper::create(const LineGroupConfig& config)
{
    if (config.gap_hz < 0 || config.edge_gap_hz < 0 ||
        (config.gap_hz == 0 && config.edge_gap_hz == 0)) {
        return fail(std::format(
            "LineGrouper: gap_hz was {} and edge_gap_hz {}. At least one must be positive and "
            "neither negative: both at zero would group nothing, and a negative gap has no "
            "meaning; a caller that wants no groups should not make one.",
            config.gap_hz, config.edge_gap_hz));
    }
    LineGrouper grouper;
    grouper.config_ = config;
    return grouper;
}

std::span<const GroupMember> LineGrouper::members(const LineGroup& group) const
{
    if (static_cast<std::size_t>(group.first_member) + group.member_count > members_.size()) {
        return {};
    }
    return std::span<const GroupMember>(members_.data() + group.first_member,
                                        group.member_count);
}

void LineGrouper::reset()
{
    groups_.clear();
    members_.clear();
    previous_groups_.clear();
    previous_members_.clear();
    have_decision_ = false;
    last_now_ = 0;
}

Status LineGrouper::observe(std::span<const Track> tracks, dsp::SampleIndex now)
{
    if (have_decision_ && now < last_now_) {
        return fail(std::format(
            "LineGrouper: a decision at sample {} arrived after one at {}. Time runs one way "
            "on a capture, so this is a different capture: reset() before feeding it.",
            now, last_now_));
    }
    have_decision_ = true;
    last_now_ = now;
    ++stats_.decisions;

    std::swap(groups_, previous_groups_);
    std::swap(members_, previous_members_);
    groups_.clear();
    members_.clear();

    eligible_.clear();
    for (const Track& track : tracks) {
        if (groupable(track.state)) {
            eligible_.push_back(&track);
        }
    }
    // By centre, then id, so two tracks at the same hertz land in the same
    // order on every run and the chain below is a function of the list.
    std::sort(eligible_.begin(), eligible_.end(), [](const Track* a, const Track* b) {
        if (a->center != b->center) {
            return a->center < b->center;
        }
        return a->id < b->id;
    });

    // Chains of neighbours within the gap. A chain of one is a line on its
    // own and is not a group.
    //
    // Two rules, either of which chains. The centre gap is the original one.
    // The edge gap chains a track whose lower edge sits within it of the
    // highest upper edge in the chain so far, which is how the pieces of one
    // filled emitter meet: a carrier and the sidebands either side of it, or
    // the stretches of a talker's spectrum the detector cut apart. Running
    // over the chain's highest edge rather than the neighbour's, because a
    // narrow line can sit inside a wide band's extent with its centre past it.
    const auto low_edge = [](const Track& track) { return track.center - track.bandwidth / 2; };
    const auto high_edge = [](const Track& track) {
        return track.center + (track.bandwidth - track.bandwidth / 2);
    };
    std::size_t first = 0;
    while (first < eligible_.size()) {
        std::size_t last = first;
        dsp::Hertz reach = high_edge(*eligible_[first]);
        while (last + 1 < eligible_.size()) {
            const Track& next = *eligible_[last + 1];
            const bool by_centre =
                config_.gap_hz > 0 && next.center - eligible_[last]->center <= config_.gap_hz;
            const bool by_edge = config_.edge_gap_hz > 0 && low_edge(next) - reach <= config_.edge_gap_hz;
            if (!by_centre && !by_edge) {
                break;
            }
            ++last;
            reach = std::max(reach, high_edge(next));
        }

        if (last > first) {
            LineGroup group;
            group.first_member = static_cast<std::uint32_t>(members_.size());
            group.member_count = static_cast<std::uint32_t>(last - first + 1);

            std::size_t strongest = first;
            dsp::SampleIndex earliest = eligible_[first]->first_seen;
            dsp::SampleIndex latest = eligible_[first]->first_seen;
            for (std::size_t i = first; i <= last; ++i) {
                const Track& track = *eligible_[i];
                const Track& best = *eligible_[strongest];
                if (track.snr_2500_db > best.snr_2500_db ||
                    (track.snr_2500_db == best.snr_2500_db && track.id < best.id)) {
                    strongest = i;
                }
                earliest = std::min(earliest, track.first_seen);
                latest = std::max(latest, track.first_seen);
            }

            group.anchor = eligible_[strongest]->id;
            group.anchor_center = eligible_[strongest]->center;
            group.span = eligible_[last]->center - eligible_[first]->center;
            group.birth_spread = latest - earliest;
            group.low_edge = low_edge(*eligible_[first]);
            group.high_edge = high_edge(*eligible_[first]);
            for (std::size_t i = first; i <= last; ++i) {
                group.low_edge = std::min(group.low_edge, low_edge(*eligible_[i]));
                group.high_edge = std::max(group.high_edge, high_edge(*eligible_[i]));
            }

            for (std::size_t i = first; i <= last; ++i) {
                const Track& track = *eligible_[i];
                GroupMember member;
                member.track = track.id;
                member.state = track.state;
                member.center = track.center;
                member.bandwidth = track.bandwidth;
                member.snr_2500_db = track.snr_2500_db;
                member.offset = track.center - group.anchor_center;
                member.spacing = i > first ? track.center - eligible_[i - 1]->center : 0;
                member.first_seen = track.first_seen;
                member.joined = now;
                members_.push_back(member);
            }
            groups_.push_back(group);
        }
        first = last + 1;
    }

    // Which previous group each current group continues. Counted by shared
    // track ids, because an id is the tracker's statement that a line is the
    // same line, and centres move.
    overlaps_.clear();
    for (std::uint32_t c = 0; c < groups_.size(); ++c) {
        const std::span<const GroupMember> now_members = members(groups_[c]);
        for (std::uint32_t p = 0; p < previous_groups_.size(); ++p) {
            const LineGroup& before = previous_groups_[p];
            std::uint32_t shared = 0;
            for (std::uint32_t k = 0; k < before.member_count; ++k) {
                const std::uint64_t id = previous_members_[before.first_member + k].track;
                shared += static_cast<std::uint32_t>(
                    std::any_of(now_members.begin(), now_members.end(),
                                [id](const GroupMember& m) { return m.track == id; }));
            }
            if (shared > 0) {
                overlaps_.push_back(Overlap{.current = c, .previous = p, .shared = shared});
            }
        }
    }

    // Most shared members first, and on a tie the OLDER previous group, so a
    // group that splits leaves its id with the half carrying more of its
    // lines and a group that absorbs another keeps the id that has been up
    // longer. Every key is total, so the outcome is a function of the input.
    std::sort(overlaps_.begin(), overlaps_.end(),
              [this](const Overlap& a, const Overlap& b) {
                  if (a.shared != b.shared) {
                      return a.shared > b.shared;
                  }
                  const LineGroup& pa = previous_groups_[a.previous];
                  const LineGroup& pb = previous_groups_[b.previous];
                  if (pa.formed != pb.formed) {
                      return pa.formed < pb.formed;
                  }
                  if (pa.id != pb.id) {
                      return pa.id < pb.id;
                  }
                  return a.current < b.current;
              });

    previous_taken_.assign(previous_groups_.size(), 0);
    for (const Overlap& overlap : overlaps_) {
        LineGroup& group = groups_[overlap.current];
        if (group.id != 0 || previous_taken_[overlap.previous] != 0) {
            continue;
        }
        previous_taken_[overlap.previous] = 1;

        const LineGroup& before = previous_groups_[overlap.previous];
        group.id = before.id;
        group.formed = before.formed;

        const std::span<const GroupMember> earlier(
            previous_members_.data() + before.first_member, before.member_count);
        for (std::uint32_t k = 0; k < group.member_count; ++k) {
            GroupMember& member = members_[group.first_member + k];
            const auto kept =
                std::find_if(earlier.begin(), earlier.end(),
                             [&member](const GroupMember& m) { return m.track == member.track; });
            if (kept != earlier.end()) {
                member.joined = kept->joined;
            } else {
                ++stats_.joins;
            }
        }
        const std::span<const GroupMember> current = members(group);
        for (const GroupMember& gone : earlier) {
            if (std::none_of(current.begin(), current.end(), [&gone](const GroupMember& m) {
                    return m.track == gone.track;
                })) {
                ++stats_.leaves;
            }
        }
    }

    for (LineGroup& group : groups_) {
        if (group.id == 0) {
            group.id = next_id_++;
            group.formed = now;
            ++stats_.groups_formed;
        }
        dsp::SampleIndex together = 0;
        for (const GroupMember& member : members(group)) {
            together = std::max(together, member.joined);
        }
        group.together_since = together;
        if (now - together > stats_.longest_together) {
            stats_.longest_together = now - together;
            stats_.longest_together_group = group.id;
        }
    }
    for (const std::uint8_t taken : previous_taken_) {
        if (taken == 0) {
            ++stats_.groups_ended;
        }
    }

    return {};
}

}  // namespace revenant::detect
