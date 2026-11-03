// The phone's DV tab, gateway side: Auto DV, trunk following and the call
// tables, built on the desktop's own rules so the two clients cannot disagree.
//
// WHAT IS THE DESKTOP'S AND WHAT IS NEW HERE.
//   ui/models/dv_spawn.h     which detections get a receiver, and which of
//                            Auto DV's own receivers go back off a control
//                            channel. Used as is.
//   ui/models/call_log.h     the call and talkgroup rows, folded from decoded
//                            messages by call_frames(). Used as is.
//   ui/models/trunk_aliases.h the alias table and the allow, deny and priority
//                            lists in the desktop's text form. Used as is.
// What is here is the gateway's half of EngineLink::spawn_dv_receivers (which
// receivers cover what, how much room is left) and the JSON the phone reads.
//
// AUTO DV RUNS IN THE GATEWAY, per phone session, so a phone that is locked or
// has lost its socket keeps opening receivers the way an unattended desktop
// does. The desktop keeps no Auto DV switch in its settings (EngineLink holds
// it for the life of the window), so there is nothing of the desktop's to
// follow: the switch is the phone session's, and goes with it into a session
// rebuilt after an engine restart.
//
// EVENTS, all JSON, each sent only when its text changes, and again on resume:
//
//   "calls"       {items:[row]}       newest first, at most kPhoneCallRows
//   "talkgroups"  {items:[row]}       newest first, at most kPhoneCallRows
//     row: {id, protocol, system, target, alias?, source, group, encrypted,
//           emergency, frequencyHz, vrx, slot, owner, firstMs, lastMs,
//           state: "live" | "ended" | "granted", count}
//     firstMs and lastMs are wall clock milliseconds since the epoch. alias is
//     the desktop's name for a P25 or DMR talkgroup, present only when it has
//     one. "granted" is a grant on a control channel that no receiver took.
//   "trunk"       the engine's trunk status, trunk_status_json below
//   "dv"          {autoDv, follow:{vrx, maxFollowers} | null, maxFollowers}
//                 the phone session's switches, and the follower count the
//                 desktop last chose, for the control's starting value

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/rpc/types.h"
#include "models/call_log.h"
#include "models/dv_spawn.h"

namespace revenant::webgate {

using TrunkAliases = std::map<std::uint32_t, std::string>;

// The rows a calls or talkgroups event carries. The desktop keeps 500; a phone
// on a cell link reading a narrow table has no use for more than a screenful
// or two, and every change resends the event.
inline constexpr std::size_t kPhoneCallRows = 100;

// The desktop's rack limit, ui/models/receiver_rack.h, so Auto DV on the phone
// stops where the desktop's would.
inline constexpr std::size_t kAutoDvMaxReceivers = 64;

// The desktop's follower count when it has never saved one, and its bounds
// (ui/models/trunk_link.cpp).
inline constexpr std::uint32_t kDefaultMaxFollowers = 2;
inline constexpr std::uint32_t kMinFollowers = 1;
inline constexpr std::uint32_t kMaxFollowers = 16;

// A receiver mode that carries digital voice: the ones the DV tab is shown for.
[[nodiscard]] bool is_dv_demod(rpc::Demod demod);

// One engine receiver, at its absolute frequency.
struct PlacedReceiver {
    std::uint64_t vrx = 0;
    double hz = 0.0;
};

struct AutoDvPass {
    std::vector<ui::DvSpawn> open;
    // Receivers Auto DV opened that now sit on a control channel.
    std::vector<std::uint64_t> retire;
};

// EngineLink::spawn_dv_receivers without the rack: hand back what an earlier
// pass opened on a control channel (not the one the phone is listening to, for
// the reason the desktop spares its pane), then plan receivers for every
// spawnable detection no receiver and no dismissal covers.
[[nodiscard]] AutoDvPass plan_auto_dv(std::span<const rpc::Detection> detections,
                                      std::span<const PlacedReceiver> receivers,
                                      const std::set<std::uint64_t>& spawned,
                                      std::span<const double> dismissed_hz,
                                      std::optional<std::uint64_t> listening,
                                      std::size_t max_receivers = kAutoDvMaxReceivers);

// What a call row's owner reads as: the desktop's "user" for anything a client
// opened, the plugin's name ("trunk") for a follower.
[[nodiscard]] std::string call_owner(std::string_view opened_by);

[[nodiscard]] std::string call_rows_json(const std::deque<ui::CallRow>& rows,
                                         const TrunkAliases& aliases,
                                         std::size_t limit = kPhoneCallRows);
[[nodiscard]] std::string calls_event(const ui::CallLog& log, const TrunkAliases& aliases);
[[nodiscard]] std::string talkgroups_event(const ui::CallLog& log, const TrunkAliases& aliases);

// {enabled, controlVrx, maxFollowers, tsbks, grants, note, fault?,
//  calls:[{id, talkgroup, alias?, source?, frequencyHz, active, encrypted,
//          emergency, followed, vrx, skip, endReason, priority}],
//  rules:{allow:[...], deny:[...], priorities:[{talkgroup, priority}]}}
// fault is the engine's refusal of trunkStatus, when it refused.
[[nodiscard]] std::string trunk_status_json(const rpc::TrunkStatusInfo& status,
                                            const TrunkAliases& aliases,
                                            std::string_view fault = {});
[[nodiscard]] std::string trunk_event(const rpc::TrunkStatusInfo& status,
                                      const TrunkAliases& aliases, std::string_view fault = {});

// The desktop's rule texts made into the engine's rule set, the way
// EngineLink::store_trunk_rules does (without the solo, which is the desktop
// panel's own and not saved). The second member counts entries skipped.
struct ParsedTrunkRules {
    rpc::TrunkRuleSet rules;
    int skipped = 0;
};
[[nodiscard]] ParsedTrunkRules trunk_rules_from_text(std::string_view allow, std::string_view deny,
                                                     std::string_view priority);
[[nodiscard]] std::string trunk_rules_json(const rpc::TrunkRuleSet& rules);

// Order-blind, as trunk_link.cpp compares what the engine reports.
[[nodiscard]] bool same_trunk_rules(const rpc::TrunkRuleSet& a, const rpc::TrunkRuleSet& b);

[[nodiscard]] std::uint32_t clamp_followers(std::int64_t count);

}  // namespace revenant::webgate
