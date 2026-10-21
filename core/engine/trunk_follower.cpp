#include "core/engine/trunk_follower.h"

#include <algorithm>
#include <utility>

namespace revenant::engine {

const char* trunk_skip_name(TrunkSkip skip) {
    switch (skip) {
        case TrunkSkip::None: return "";
        case TrunkSkip::Encrypted: return "encrypted";
        case TrunkSkip::Denied: return "denied";
        case TrunkSkip::NotAllowed: return "not on the allow list";
        case TrunkSkip::OutsideSpan: return "outside the span";
        case TrunkSkip::Busy: return "every follower busy";
        case TrunkSkip::Refused: return "the engine refused a receiver";
    }
    return "";
}

TrunkFollower::TrunkFollower(TrunkSettings settings) : settings_(settings) {}

TrunkSkip TrunkFollower::refusal(std::uint32_t talkgroup, bool encrypted) const {
    if (encrypted) {
        return TrunkSkip::Encrypted;
    }
    if (rules_.deny.contains(talkgroup)) {
        return TrunkSkip::Denied;
    }
    if (!rules_.allow.empty() && !rules_.allow.contains(talkgroup)) {
        return TrunkSkip::NotAllowed;
    }
    return TrunkSkip::None;
}

std::int32_t TrunkFollower::priority_of(std::uint32_t talkgroup) const {
    const auto it = rules_.priority.find(talkgroup);
    return it == rules_.priority.end() ? 0 : it->second;
}

bool TrunkFollower::in_span(std::int64_t hz) const {
    return !span_ || (hz >= span_->first && hz <= span_->second);
}

TrunkCall* TrunkFollower::find_frequency(std::int64_t hz) {
    const auto it = std::ranges::find(active_, hz, &TrunkCall::frequency_hz);
    return it == active_.end() ? nullptr : &*it;
}

TrunkCall* TrunkFollower::find_vrx(std::uint64_t vrx) {
    if (vrx == 0) {
        return nullptr;
    }
    const auto it = std::ranges::find(active_, vrx, &TrunkCall::vrx);
    return it == active_.end() ? nullptr : &*it;
}

TrunkCall* TrunkFollower::find_id(std::uint64_t id) {
    const auto it = std::ranges::find(active_, id, &TrunkCall::id);
    return it == active_.end() ? nullptr : &*it;
}

std::size_t TrunkFollower::following() const {
    return pending_.size() + static_cast<std::size_t>(std::ranges::count_if(
                                 active_, [](const TrunkCall& c) { return c.vrx != 0; }));
}

TrunkCall& TrunkFollower::start_call(std::uint32_t talkgroup,
                                     std::optional<std::uint32_t> source, std::int64_t hz,
                                     bool encrypted, bool emergency, double now) {
    TrunkCall call;
    call.id = next_id_++;
    call.talkgroup = talkgroup;
    call.source = source;
    call.frequency_hz = hz;
    call.start_s = now;
    call.end_s = now;
    call.encrypted = encrypted;
    call.emergency = emergency;
    call.priority = priority_of(talkgroup);
    active_.push_back(std::move(call));
    ++revision_;
    return active_.back();
}

void TrunkFollower::try_follow(TrunkCall& call, double now, std::uint64_t spare) {
    if (call.vrx != 0 || pending_.contains(call.id)) {
        return;
    }
    call.skip = refusal(call.talkgroup, call.encrypted);
    if (call.skip == TrunkSkip::None && !in_span(call.frequency_hz)) {
        call.skip = TrunkSkip::OutsideSpan;
    }
    if (call.skip == TrunkSkip::None) {
        if (const auto it = refused_.find(call.frequency_hz); it != refused_.end()) {
            if (now - it->second < settings_.refusal_backoff_s) {
                call.skip = TrunkSkip::Refused;
            } else {
                refused_.erase(it);
            }
        }
    }
    if (call.skip != TrunkSkip::None) {
        if (spare != 0) {
            hand_off(spare, now);
        }
        ++revision_;
        return;
    }

    if (spare != 0) {
        // Already on this frequency: nothing to tune.
        call.vrx = spare;
        call.followed = true;
    } else if (following() < settings_.max_followers) {
        pending_.insert(call.id);
        actions_.push_back({TrunkAction::Kind::Open, call.id, 0, call.frequency_hz});
    } else {
        // The lowest priority followed call, and the oldest among equals, is
        // the one that gives way.
        TrunkCall* victim = nullptr;
        for (TrunkCall& other : active_) {
            if (other.vrx == 0 || other.priority >= call.priority) {
                continue;
            }
            if (victim == nullptr || other.priority < victim->priority ||
                (other.priority == victim->priority && other.start_s < victim->start_s)) {
                victim = &other;
            }
        }
        if (victim == nullptr) {
            call.skip = TrunkSkip::Busy;
            ++revision_;
            return;
        }
        const std::uint64_t vrx = victim->vrx;
        victim->vrx = 0;
        victim->skip = TrunkSkip::Busy;
        call.vrx = vrx;
        call.followed = true;
        actions_.push_back({TrunkAction::Kind::Retune, call.id, vrx, call.frequency_hz});
    }
    call.skip = TrunkSkip::None;
    ++revision_;
}

void TrunkFollower::hand_off(std::uint64_t vrx, double now) {
    TrunkCall* best = nullptr;
    for (TrunkCall& call : active_) {
        if (call.vrx != 0 || call.skip != TrunkSkip::Busy || !in_span(call.frequency_hz) ||
            pending_.contains(call.id)) {
            continue;
        }
        if (best == nullptr || call.priority > best->priority ||
            (call.priority == best->priority && call.start_s < best->start_s)) {
            best = &call;
        }
    }
    static_cast<void>(now);
    if (best == nullptr) {
        actions_.push_back({TrunkAction::Kind::Release, 0, vrx, 0});
        ++revision_;
        return;
    }
    best->vrx = vrx;
    best->followed = true;
    best->skip = TrunkSkip::None;
    actions_.push_back({TrunkAction::Kind::Retune, best->id, vrx, best->frequency_hz});
    ++revision_;
}

std::uint64_t TrunkFollower::end_call(std::size_t index, const std::string& reason, bool keep) {
    TrunkCall call = std::move(active_[index]);
    active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(index));
    if (const auto t = terminated_.find(call.id); t != terminated_.end()) {
        call.end_s = t->second;
        terminated_.erase(t);
    }
    if (pending_.erase(call.id) != 0) {
        orphaned_.insert(call.id);
    }
    const std::uint64_t vrx = call.vrx;
    call.vrx = 0;
    call.active = false;
    call.end_reason = reason;
    recent_.push_front(std::move(call));
    while (recent_.size() > settings_.recent_limit) {
        recent_.pop_back();
    }
    ++revision_;
    if (vrx != 0 && !keep) {
        hand_off(vrx, 0.0);
        return 0;
    }
    return vrx;
}

void TrunkFollower::drop_follower(TrunkCall& call, TrunkSkip why, double now) {
    const std::uint64_t vrx = call.vrx;
    call.vrx = 0;
    call.skip = why;
    ++revision_;
    if (vrx != 0) {
        hand_off(vrx, now);
    }
}

void TrunkFollower::set_rules(TrunkRules rules, double now) {
    rules_ = std::move(rules);
    for (TrunkCall& call : active_) {
        call.priority = priority_of(call.talkgroup);
    }
    ++revision_;
    // By id, because hand_off and try_follow can move a receiver between
    // calls while this walks them.
    std::vector<std::uint64_t> ids;
    for (const TrunkCall& call : active_) {
        ids.push_back(call.id);
    }
    for (const std::uint64_t id : ids) {
        TrunkCall* call = find_id(id);
        if (call == nullptr) {
            continue;
        }
        const TrunkSkip why = refusal(call->talkgroup, call->encrypted);
        if (why != TrunkSkip::None) {
            if (call->vrx != 0) {
                drop_follower(*call, why, now);
            } else {
                call->skip = why;
            }
        } else if (call->vrx == 0) {
            try_follow(*call, now);
        }
    }
}

void TrunkFollower::set_span(std::int64_t low_hz, std::int64_t high_hz, double now) {
    span_ = std::make_pair(low_hz, high_hz);
    refused_.clear();
    std::vector<std::uint64_t> ids;
    for (const TrunkCall& call : active_) {
        ids.push_back(call.id);
    }
    for (const std::uint64_t id : ids) {
        TrunkCall* call = find_id(id);
        if (call == nullptr) {
            continue;
        }
        if (!in_span(call->frequency_hz)) {
            if (call->vrx != 0) {
                drop_follower(*call, TrunkSkip::OutsideSpan, now);
            } else {
                call->skip = TrunkSkip::OutsideSpan;
            }
        } else if (call->vrx == 0 && (call->skip == TrunkSkip::OutsideSpan ||
                                      call->skip == TrunkSkip::Refused)) {
            try_follow(*call, now);
        }
    }
    ++revision_;
}

void TrunkFollower::grant(const TrunkGrant& g, double now) {
    bool encrypted = g.encrypted;
    if (g.options_known) {
        if (g.encrypted) {
            encrypted_groups_.insert(g.talkgroup);
        } else {
            encrypted_groups_.erase(g.talkgroup);
        }
    } else {
        encrypted = encrypted_groups_.contains(g.talkgroup);
    }

    std::uint64_t spare = 0;
    if (TrunkCall* call = find_frequency(g.frequency_hz); call != nullptr) {
        if (call->talkgroup == g.talkgroup) {
            const auto t = terminated_.find(call->id);
            if (t != terminated_.end()) {
                if (!g.initial) {
                    return;  // updates do not hold a terminated call open
                }
                terminated_.erase(t);
            }
            call->end_s = now;
            if (g.source && call->source != g.source) {
                call->source = g.source;
                ++revision_;
            }
            if (g.emergency && !call->emergency) {
                call->emergency = true;
                ++revision_;
            }
            if (encrypted && !call->encrypted) {
                call->encrypted = true;
                drop_follower(*call, TrunkSkip::Encrypted, now);
                return;
            }
            if (call->vrx == 0 &&
                (call->skip == TrunkSkip::Busy || call->skip == TrunkSkip::Refused)) {
                try_follow(*call, now);
            }
            return;
        }
        // The channel went to another talkgroup before this noticed the last
        // call end. Its receiver is already on the frequency.
        const auto index = static_cast<std::size_t>(call - active_.data());
        spare = end_call(index, "channel reassigned", true);
    }

    TrunkCall& call = start_call(g.talkgroup, g.source, g.frequency_hz, encrypted, g.emergency,
                                 now);
    try_follow(call, now, spare);
}

void TrunkFollower::voice(std::uint64_t vrx, const TrunkVoice& v, double now) {
    TrunkCall* call = find_vrx(vrx);
    if (call == nullptr) {
        return;
    }
    if (v.terminator) {
        terminated_.try_emplace(call->id, now);
        return;
    }
    if (v.talkgroup && *v.talkgroup != call->talkgroup) {
        const std::int64_t hz = call->frequency_hz;
        const auto index = static_cast<std::size_t>(call - active_.data());
        const std::uint64_t spare = end_call(index, "voice is another talkgroup", true);
        TrunkCall& next = start_call(*v.talkgroup, v.source, hz,
                                     v.encrypted || encrypted_groups_.contains(*v.talkgroup),
                                     false, now);
        try_follow(next, now, spare);
        return;
    }
    terminated_.erase(call->id);
    call->end_s = now;
    if (v.source && call->source != v.source) {
        call->source = v.source;
        ++revision_;
    }
    if (v.encrypted) {
        call->encrypted = true;
        encrypted_groups_.insert(call->talkgroup);
        drop_follower(*call, TrunkSkip::Encrypted, now);
    }
}

void TrunkFollower::opened(std::uint64_t call_id, std::uint64_t vrx) {
    if (orphaned_.erase(call_id) != 0) {
        hand_off(vrx, 0.0);
        return;
    }
    pending_.erase(call_id);
    TrunkCall* call = find_id(call_id);
    if (call == nullptr) {
        actions_.push_back({TrunkAction::Kind::Release, 0, vrx, 0});
        return;
    }
    call->vrx = vrx;
    call->followed = true;
    call->skip = TrunkSkip::None;
    ++revision_;
}

void TrunkFollower::open_failed(std::uint64_t call_id, double now) {
    orphaned_.erase(call_id);
    pending_.erase(call_id);
    TrunkCall* call = find_id(call_id);
    if (call == nullptr) {
        return;
    }
    refused_[call->frequency_hz] = now;
    call->skip = TrunkSkip::Refused;
    ++revision_;
}

void TrunkFollower::vrx_gone(std::uint64_t vrx, double now) {
    static_cast<void>(now);
    for (std::size_t i = 0; i < active_.size(); ++i) {
        if (active_[i].vrx == vrx) {
            active_[i].vrx = 0;
            end_call(i, "receiver removed", true);
            return;
        }
    }
}

void TrunkFollower::tick(double now) {
    for (std::size_t i = 0; i < active_.size();) {
        const TrunkCall& call = active_[i];
        const auto t = terminated_.find(call.id);
        if (t != terminated_.end() && now - t->second >= settings_.terminator_hold_s) {
            end_call(i, "call ended", false);
        } else if (now - call.end_s >= settings_.hang_s) {
            end_call(i, "nothing heard", false);
        } else {
            ++i;
        }
    }
}

void TrunkFollower::stop(const std::string& reason, double now) {
    static_cast<void>(now);
    while (!active_.empty()) {
        const std::uint64_t vrx = end_call(active_.size() - 1, reason, true);
        if (vrx != 0) {
            actions_.push_back({TrunkAction::Kind::Release, 0, vrx, 0});
        }
    }
}

std::vector<TrunkAction> TrunkFollower::take_actions() {
    std::vector<TrunkAction> out;
    out.swap(actions_);
    return out;
}

std::vector<TrunkCall> TrunkFollower::calls() const {
    std::vector<TrunkCall> out(active_.begin(), active_.end());
    out.insert(out.end(), recent_.begin(), recent_.end());
    return out;
}

}  // namespace revenant::engine
