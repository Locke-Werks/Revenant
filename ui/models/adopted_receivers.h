// Receivers an engine plugin opened, and the rack taking them in. Qt-free, so
// ui/tests holds the rules; ui/models/rack_link.cpp applies them.
//
// WHY THE RACK ADOPTS ANYTHING. The trunk tracker, then the p25trunk plugin
// and now the engine's built-in follower, runs inside the engine and opens a P25 receiver on every voice call it follows,
// then removes it when the call ends. The rack showed only the receivers this
// client opened, so those calls were transcribed (the transcript subscription
// is engine-wide) and never heard. The engine now tells every session about
// every receiver and who owns it, and this file says which of them the rack
// takes in.
//
// WHAT ADOPTED MEANS. Where the receiver came from, not what may be done
// with it: the operator focuses, tunes and removes it like any other. See
// RackEntry::adopted in models/receiver_rack.h.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/rpc/types.h"

namespace revenant::ui {

// Which owners' receivers the rack takes in: a plugin's and another
// session's. The engine serves one user (the single-user lease), so a
// receiver the phone opened, or this window opened on an earlier connection,
// is this operator's too.
//
// NOT THE ENGINE'S: an engine older than owner_kind reports Engine for every
// receiver, this window's own among them, and adopting those would list each
// one twice. THIS SESSION'S are already in the rack, under the keys this
// client gave them.
[[nodiscard]] constexpr bool rack_adopts(rpc::VrxOwnerKind owner)
{
    return owner == rpc::VrxOwnerKind::Plugin || owner == rpc::VrxOwnerKind::OtherSession;
}

// How long a removed adopted receiver's slot and name are kept for its
// transcripts. Whisper answers a second and a half to two seconds after the
// speech ends, and the trunk tracker closes the receiver when the call ends,
// which is usually before that. Thirty seconds is fifteen times the wait, and
// short enough that the slot's colour is not still claimed by a call long
// over when a new receiver takes it.
inline constexpr std::int64_t kAdoptedMemoryMs = 30'000;

// What a transcript of a receiver is attributed to: its strip's slot, for the
// colour, and its label, for the hover card.
struct ReceiverAttribution {
    std::size_t slot = 0;
    std::string label;

    friend bool operator==(const ReceiverAttribution&, const ReceiverAttribution&) = default;
};

// The adopted receivers that have gone, by engine id, for kAdoptedMemoryMs
// after they went. Qt thread only, like the rack.
class RecentlyAdopted {
public:
    // The receiver under this engine id went at now_ms. A second note for the
    // same id replaces the first, since ids are not reused on one engine and
    // a repeat is the same removal reported twice.
    void remember(std::uint64_t vrx, ReceiverAttribution who, std::int64_t now_ms)
    {
        if (vrx == 0) {
            return;
        }
        prune(now_ms);
        std::erase_if(gone_, [vrx](const Gone& g) { return g.vrx == vrx; });
        gone_.push_back(Gone{vrx, std::move(who), now_ms});
    }

    // What a transcript of this engine id is attributed to, if the receiver
    // went within kAdoptedMemoryMs of now_ms.
    [[nodiscard]] std::optional<ReceiverAttribution> find(std::uint64_t vrx,
                                                          std::int64_t now_ms) const
    {
        for (const Gone& g : gone_) {
            if (g.vrx == vrx && now_ms - g.at_ms <= kAdoptedMemoryMs) {
                return g.who;
            }
        }
        return std::nullopt;
    }

    void prune(std::int64_t now_ms)
    {
        std::erase_if(gone_,
                      [now_ms](const Gone& g) { return now_ms - g.at_ms > kAdoptedMemoryMs; });
    }

    // For a connection that has gone. The next engine issues ids from the
    // start again, so its receiver 3 would otherwise carry this one's name.
    void clear() { gone_.clear(); }

    [[nodiscard]] std::size_t size() const { return gone_.size(); }

private:
    struct Gone {
        std::uint64_t vrx = 0;
        ReceiverAttribution who;
        std::int64_t at_ms = 0;
    };
    std::vector<Gone> gone_;
};

}  // namespace revenant::ui
