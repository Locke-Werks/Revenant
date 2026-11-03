// The trunk follower's rules as the operator writes them and as the engine
// reports them: the allow, deny and priority texts made into one rule set,
// and the order-blind comparison that decides whether the engine's copy has
// drifted. One copy for the desktop's trunking panel (models/trunk_link.cpp)
// and the phone gateway (tools/webgate/dv_feed.h), so the two reconcile to
// the same rules.
//
// No Qt, so ui/tests/test_trunk_aliases.cpp links without it.

#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

#include "core/rpc/types.h"
#include "models/trunk_aliases.h"

namespace revenant::ui {

struct TrunkRulesParsed {
    rpc::TrunkRuleSet rules;
    // Entries across the three texts that did not parse.
    int skipped = 0;
};

[[nodiscard]] inline TrunkRulesParsed parse_trunk_rules(std::string_view allow,
                                                        std::string_view deny,
                                                        std::string_view priority)
{
    TrunkRulesParsed out;
    auto a = parse_trunk_groups(allow);
    auto d = parse_trunk_groups(deny);
    auto p = parse_trunk_priorities(priority);
    out.rules.allow = std::move(a.value);
    out.rules.deny = std::move(d.value);
    for (const auto& [tg, level] : p.value) {
        out.rules.priorities.push_back({tg, level});
    }
    out.skipped = a.skipped + d.skipped + p.skipped;
    return out;
}

[[nodiscard]] inline rpc::TrunkRuleSet sorted_trunk_rules(rpc::TrunkRuleSet rules)
{
    std::ranges::sort(rules.allow);
    std::ranges::sort(rules.deny);
    std::ranges::sort(rules.priorities, {}, &rpc::TrunkPriorityRule::talkgroup);
    return rules;
}

// The engine may hand the lists back in another order.
[[nodiscard]] inline bool same_trunk_rules(const rpc::TrunkRuleSet& a, const rpc::TrunkRuleSet& b)
{
    const auto x = sorted_trunk_rules(a);
    const auto y = sorted_trunk_rules(b);
    if (x.allow != y.allow || x.deny != y.deny || x.priorities.size() != y.priorities.size()) {
        return false;
    }
    for (std::size_t i = 0; i < x.priorities.size(); ++i) {
        if (x.priorities[i].talkgroup != y.priorities[i].talkgroup ||
            x.priorities[i].priority != y.priorities[i].priority) {
            return false;
        }
    }
    return true;
}

}  // namespace revenant::ui
