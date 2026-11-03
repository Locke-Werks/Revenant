#include "tools/webgate/dv_feed.h"

#include <algorithm>
#include <charconv>
#include <cmath>

#include "models/trunk_aliases.h"
#include "tools/webgate/protocol.h"

namespace revenant::webgate {

namespace {

[[nodiscard]] std::string join(const std::vector<std::string>& items)
{
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            out += ',';
        }
        out += items[i];
    }
    return out + ']';
}

[[nodiscard]] std::string u32_array(const std::vector<std::uint32_t>& values)
{
    std::vector<std::string> items;
    items.reserve(values.size());
    for (const auto v : values) {
        items.push_back(std::to_string(v));
    }
    return join(items);
}

// The alias of a row's talkgroup, where the row names one the desktop has a
// name for. Only P25 and DMR group calls carry a talkgroup number.
[[nodiscard]] const std::string* alias_of(const ui::CallRow& row, const TrunkAliases& aliases)
{
    if (!row.group || (row.protocol != "P25" && row.protocol != "DMR") || row.target.empty()) {
        return nullptr;
    }
    std::uint32_t tg = 0;
    const auto* end = row.target.data() + row.target.size();
    const auto [ptr, ec] = std::from_chars(row.target.data(), end, tg);
    if (ec != std::errc() || ptr != end) {
        return nullptr;
    }
    const auto it = aliases.find(tg);
    return it == aliases.end() ? nullptr : &it->second;
}

[[nodiscard]] std::string_view row_state(const ui::CallRow& row)
{
    if (row.granted) {
        return "granted";
    }
    return row.active ? "live" : "ended";
}

}  // namespace

bool is_dv_demod(rpc::Demod demod)
{
    return demod == rpc::Demod::P25p1 || demod == rpc::Demod::Dmr || demod == rpc::Demod::Dstar ||
           demod == rpc::Demod::Tetra;
}

AutoDvPass plan_auto_dv(std::span<const rpc::Detection> detections,
                        std::span<const PlacedReceiver> receivers,
                        const std::set<std::uint64_t>& spawned, std::span<const double> dismissed_hz,
                        std::optional<std::uint64_t> listening, std::size_t max_receivers)
{
    AutoDvPass pass;
    std::vector<ui::DvSpawned> mine;
    for (const PlacedReceiver& r : receivers) {
        if (spawned.contains(r.vrx) && r.vrx != listening) {
            mine.push_back(ui::DvSpawned{r.vrx, r.hz});
        }
    }
    pass.retire = ui::plan_dv_retirements(detections, mine);

    std::vector<double> covered(dismissed_hz.begin(), dismissed_hz.end());
    std::size_t kept = 0;
    for (const PlacedReceiver& r : receivers) {
        if (std::ranges::find(pass.retire, r.vrx) != pass.retire.end()) {
            continue;
        }
        covered.push_back(r.hz);
        ++kept;
    }
    const std::size_t room = max_receivers - std::min(kept, max_receivers);
    pass.open = ui::plan_dv_spawns(detections, covered, room).open;
    return pass;
}

std::string call_owner(std::string_view opened_by)
{
    if (opened_by.empty() || opened_by == "this phone" || opened_by == "another client") {
        return "user";
    }
    return std::string(opened_by);
}

std::string call_rows_json(const std::deque<ui::CallRow>& rows, const TrunkAliases& aliases,
                           std::size_t limit)
{
    std::vector<std::string> items;
    for (const ui::CallRow& row : rows) {
        if (items.size() >= limit) {
            break;
        }
        JsonObject o;
        o.add_u64("id", row.serial)
            .add_string("protocol", row.protocol)
            .add_string("system", row.system)
            .add_string("target", row.target);
        if (const std::string* alias = alias_of(row, aliases)) {
            o.add_string("alias", *alias);
        }
        o.add_string("source", row.source)
            .add_bool("group", row.group)
            .add_bool("encrypted", row.encrypted)
            .add_bool("emergency", row.emergency)
            .add_i64("frequencyHz", row.frequency_hz)
            .add_u64("vrx", row.vrx)
            .add_i64("slot", row.slot)
            .add_string("owner", row.owner)
            .add_i64("firstMs", row.first_ms)
            .add_i64("lastMs", row.last_ms)
            .add_string("state", row_state(row))
            .add_u64("count", row.count);
        items.push_back(o.str());
    }
    return join(items);
}

std::string calls_event(const ui::CallLog& log, const TrunkAliases& aliases)
{
    return event("calls", JsonObject().add_raw("items", call_rows_json(log.calls(), aliases)));
}

std::string talkgroups_event(const ui::CallLog& log, const TrunkAliases& aliases)
{
    return event("talkgroups", JsonObject().add_raw("items", call_rows_json(log.groups(), aliases)));
}

std::string trunk_rules_json(const rpc::TrunkRuleSet& rules)
{
    std::vector<std::string> priorities;
    for (const auto& p : rules.priorities) {
        priorities.push_back(JsonObject()
                                 .add_u64("talkgroup", p.talkgroup)
                                 .add_i64("priority", p.priority)
                                 .str());
    }
    return JsonObject()
        .add_raw("allow", u32_array(rules.allow))
        .add_raw("deny", u32_array(rules.deny))
        .add_raw("priorities", join(priorities))
        .str();
}

std::string trunk_status_json(const rpc::TrunkStatusInfo& status, const TrunkAliases& aliases,
                              std::string_view fault)
{
    std::vector<std::string> calls;
    for (const auto& call : status.calls) {
        JsonObject c;
        c.add_u64("id", call.id).add_u64("talkgroup", call.talkgroup);
        if (const auto it = aliases.find(call.talkgroup); it != aliases.end()) {
            c.add_string("alias", it->second);
        }
        if (call.source) {
            c.add_u64("source", *call.source);
        }
        c.add_i64("frequencyHz", call.frequency_hz)
            .add_bool("active", call.active)
            .add_bool("encrypted", call.encrypted)
            .add_bool("emergency", call.emergency)
            .add_bool("followed", call.followed)
            .add_u64("vrx", call.vrx)
            .add_string("skip", call.skip)
            .add_string("endReason", call.end_reason)
            .add_i64("priority", call.priority);
        calls.push_back(c.str());
    }
    JsonObject out;
    out.add_bool("enabled", status.enabled)
        .add_u64("controlVrx", status.control_vrx)
        .add_u64("maxFollowers", status.max_followers)
        .add_u64("tsbks", status.tsbks)
        .add_u64("grants", status.grants)
        .add_string("note", status.note);
    if (!fault.empty()) {
        out.add_string("fault", fault);
    }
    out.add_raw("calls", join(calls)).add_raw("rules", trunk_rules_json(status.rules));
    return out.str();
}

std::string trunk_event(const rpc::TrunkStatusInfo& status, const TrunkAliases& aliases,
                        std::string_view fault)
{
    return R"({"event":"trunk",)" + trunk_status_json(status, aliases, fault).substr(1);
}

ParsedTrunkRules trunk_rules_from_text(std::string_view allow, std::string_view deny,
                                       std::string_view priority)
{
    ParsedTrunkRules out;
    auto a = ui::parse_trunk_groups(allow);
    auto d = ui::parse_trunk_groups(deny);
    auto p = ui::parse_trunk_priorities(priority);
    out.rules.allow = std::move(a.value);
    out.rules.deny = std::move(d.value);
    for (const auto& [tg, level] : p.value) {
        out.rules.priorities.push_back({tg, level});
    }
    out.skipped = a.skipped + d.skipped + p.skipped;
    return out;
}

bool same_trunk_rules(const rpc::TrunkRuleSet& a, const rpc::TrunkRuleSet& b)
{
    const auto sorted = [](rpc::TrunkRuleSet r) {
        std::ranges::sort(r.allow);
        std::ranges::sort(r.deny);
        std::ranges::sort(r.priorities, {}, &rpc::TrunkPriorityRule::talkgroup);
        return r;
    };
    const auto x = sorted(a);
    const auto y = sorted(b);
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

std::uint32_t clamp_followers(std::int64_t count)
{
    return static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(count, kMinFollowers, kMaxFollowers));
}

}  // namespace revenant::webgate
