// The trunking follower: which voice grants get a receiver, when that
// receiver moves or goes, and the record of every call either way.
//
// WHAT IT IS. A state machine with no engine, no decoder and no clock of its
// own. The RPC server feeds it the grants a control channel sends (core/
// decode/p25_trunk.h turns TSBKs into those), what the follower receivers'
// own decoders say about the voice, and the time; it answers with actions,
// open a receiver here, move that one there, remove that one, which the
// server carries out and reports back. Kept apart from the server so tests/
// engine/test_trunk_follower.cpp can drive every transition with made-up
// grants and times, which is how plugins/p25trunk was debugged against a
// live system and should not have had to be.
//
// THE RULES, in the order they are applied to a grant:
//   encrypted   never followed and never played, only logged: the digital
//               voice decision (docs/modes.md) is no decryption, and an
//               encrypted call's audio is noise. A call whose grant said
//               clear and whose voice then says encrypted loses its
//               receiver the same way.
//   deny        a talkgroup on the deny list is logged and not followed.
//   allow       a non-empty allow list follows only the talkgroups on it.
//   span        a grant outside the source's span cannot be heard without
//               retuning the front end, which would take the control
//               channel with it. Logged and not followed.
//   busy        every follower receiver is in use. A grant whose talkgroup
//               has a higher priority than the lowest of the followed calls
//               takes that call's receiver, which is retuned rather than
//               removed and reopened; otherwise it waits, and takes the next
//               receiver to come free while it is still on the air.
//
// WHEN A CALL ENDS, as plugins/p25trunk measured on air on 2026-10-06:
//   a terminator (TDU or TDULC) on the voice channel, held terminator_hold_s
//   in case the talkgroup keys up again at once, which only an initial grant
//   cancels, because control channels keep sending updates for a while after
//   a call has ended; nothing heard for hang_s; the channel granted to another
//   talkgroup, or its voice naming one; or its receiver removed by somebody
//   else.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace revenant::engine {

// One talkgroup granted one frequency, whatever the trunking standard.
struct TrunkGrant {
    std::uint32_t talkgroup = 0;
    std::optional<std::uint32_t> source;
    std::int64_t frequency_hz = 0;
    // Whether the grant carried service options at all; an update that did
    // not says nothing about encryption either way.
    bool options_known = false;
    bool encrypted = false;
    bool emergency = false;
    // The grant that starts a call, as against an update repeating one.
    bool initial = false;
};

// What a follower receiver's own decoder said.
struct TrunkVoice {
    bool terminator = false;
    std::optional<std::uint32_t> talkgroup;
    std::optional<std::uint32_t> source;
    bool encrypted = false;
};

struct TrunkRules {
    std::set<std::uint32_t> allow;  // empty follows every talkgroup not denied
    std::set<std::uint32_t> deny;
    std::map<std::uint32_t, std::int32_t> priority;  // absent is 0, higher wins
};

struct TrunkSettings {
    std::size_t max_followers = 2;
    double hang_s = 2.5;
    double terminator_hold_s = 2.0;
    double refusal_backoff_s = 5.0;
    std::size_t recent_limit = 200;
};

// Why an active call has no receiver.
enum class TrunkSkip : std::uint8_t {
    None,
    Encrypted,
    Denied,
    NotAllowed,
    OutsideSpan,
    Busy,
    Refused,
};

[[nodiscard]] const char* trunk_skip_name(TrunkSkip skip);

struct TrunkCall {
    std::uint64_t id = 0;
    std::uint32_t talkgroup = 0;
    std::optional<std::uint32_t> source;
    std::int64_t frequency_hz = 0;
    // The follower's clock, seconds. end_s is the last time it was heard
    // while active, and when it ended once not.
    double start_s = 0.0;
    double end_s = 0.0;
    bool active = true;
    bool encrypted = false;
    bool emergency = false;
    std::int32_t priority = 0;
    // The receiver following it now, 0 for none. followed is whether one
    // ever did, for the log.
    std::uint64_t vrx = 0;
    bool followed = false;
    TrunkSkip skip = TrunkSkip::None;
    std::string end_reason;
};

struct TrunkAction {
    enum class Kind : std::uint8_t { Open, Retune, Release };
    Kind kind = Kind::Open;
    std::uint64_t call = 0;  // Open and Retune: the call the receiver is for
    std::uint64_t vrx = 0;   // Retune and Release
    std::int64_t frequency_hz = 0;
};

class TrunkFollower {
public:
    explicit TrunkFollower(TrunkSettings settings = {});

    // Applied to calls already on the air as well: one that is now denied
    // loses its receiver, one that is now allowed gets one if there is room.
    void set_rules(TrunkRules rules, double now);
    [[nodiscard]] const TrunkRules& rules() const { return rules_; }

    // The frequencies a receiver can be opened on, [low, high]. Followed
    // calls outside a new span lose their receivers. No span set means no
    // limit, which is what the tests mostly want.
    void set_span(std::int64_t low_hz, std::int64_t high_hz, double now);

    void grant(const TrunkGrant& grant, double now);
    void voice(std::uint64_t vrx, const TrunkVoice& voice, double now);

    // The outcome of an Open action. A receiver opened for a call that ended
    // meanwhile is released at once.
    void opened(std::uint64_t call, std::uint64_t vrx);
    void open_failed(std::uint64_t call, double now);
    // A follower receiver went without being asked to, or a Retune of it
    // failed and the server removed it.
    void vrx_gone(std::uint64_t vrx, double now);

    void tick(double now);

    // Ends every call and releases every receiver: the control channel was
    // lost or following was switched off.
    void stop(const std::string& reason, double now);

    [[nodiscard]] std::vector<TrunkAction> take_actions();

    // Active calls, oldest first, then ended ones, newest first.
    [[nodiscard]] std::vector<TrunkCall> calls() const;
    [[nodiscard]] const std::vector<TrunkCall>& active() const { return active_; }
    // Bumped on every change a client would want to redraw for.
    [[nodiscard]] std::uint64_t revision() const { return revision_; }
    [[nodiscard]] std::size_t following() const;

private:
    [[nodiscard]] TrunkSkip refusal(std::uint32_t talkgroup, bool encrypted) const;
    [[nodiscard]] std::int32_t priority_of(std::uint32_t talkgroup) const;
    [[nodiscard]] bool in_span(std::int64_t hz) const;
    [[nodiscard]] TrunkCall* find_frequency(std::int64_t hz);
    [[nodiscard]] TrunkCall* find_vrx(std::uint64_t vrx);
    [[nodiscard]] TrunkCall* find_id(std::uint64_t id);

    TrunkCall& start_call(std::uint32_t talkgroup, std::optional<std::uint32_t> source,
                          std::int64_t hz, bool encrypted, bool emergency, double now);
    // Gives the call a receiver if the rules and the room allow, or records
    // why not. `spare` is a receiver already on its frequency to take over.
    void try_follow(TrunkCall& call, double now, std::uint64_t spare = 0);
    // A receiver no call needs any more: the best waiting call gets it, or
    // it is released.
    void hand_off(std::uint64_t vrx, double now);
    // Ends the call at `index`; its receiver is handed off unless `keep`
    // asks for it back.
    std::uint64_t end_call(std::size_t index, const std::string& reason, bool keep);
    void drop_follower(TrunkCall& call, TrunkSkip why, double now);

    TrunkSettings settings_;
    TrunkRules rules_;
    std::optional<std::pair<std::int64_t, std::int64_t>> span_;

    std::vector<TrunkCall> active_;
    std::deque<TrunkCall> recent_;
    std::vector<TrunkAction> actions_;

    // Calls with an Open outstanding, and Opens whose call has ended.
    std::set<std::uint64_t> pending_;
    std::set<std::uint64_t> orphaned_;
    // Per call: still terminated, and since when.
    std::map<std::uint64_t, double> terminated_;
    std::map<std::int64_t, double> refused_;
    // Talkgroups whose last grant with options said encrypted, so an update
    // without options for one is not taken for a clear call.
    std::set<std::uint32_t> encrypted_groups_;

    std::uint64_t next_id_ = 1;
    std::uint64_t revision_ = 0;
};

}  // namespace revenant::engine
