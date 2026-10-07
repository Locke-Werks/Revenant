// The rack taking in receivers an engine plugin opened: which owners it
// adopts, that focus never lands on one, what one is called, that a
// reconnection does not make one again, and how long a gone one's name is kept
// for its late transcripts. The rules are models/adopted_receivers.h and
// models/receiver_rack.h.
//
// Each case names the wrong implementation it rejects, on the convention
// test_receiver_marker.cpp states.

#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/rpc/types.h"
#include "models/adopted_receivers.h"
#include "models/receiver_rack.h"

using revenant::rpc::VrxOwnerKind;
using revenant::ui::kAdoptedMemoryMs;
using revenant::ui::kMaxReceivers;
using revenant::ui::rack_adopts;
using revenant::ui::rack_entry_focusable;
using revenant::ui::rack_entry_label;
using revenant::ui::rack_entry_rejoins;
using revenant::ui::RackBand;
using revenant::ui::RackEntry;
using revenant::ui::ReceiverAttribution;
using revenant::ui::receiver_under;
using revenant::ui::ReceiverRack;
using revenant::ui::RecentlyAdopted;

// Rejects a rack that takes in another operator's receivers, which is this
// window listening over their shoulder, or the engine's own, or this
// session's a second time under a key it did not issue.
TEST_CASE("only a plugin's receivers are adopted")
{
    CHECK(rack_adopts(VrxOwnerKind::Plugin));
    CHECK_FALSE(rack_adopts(VrxOwnerKind::OtherSession));
    CHECK_FALSE(rack_adopts(VrxOwnerKind::Engine));
    CHECK_FALSE(rack_adopts(VrxOwnerKind::ThisSession));
}

// Rejects an adopted entry that is born opening, which would read "opening"
// over a receiver that is already running, or born without its owner.
TEST_CASE("an adopted entry is live under the plugin's id and carries its name")
{
    ReceiverRack rack;
    const auto mine = *rack.add();
    const auto call = rack.add_adopted("p25trunk", 41);
    REQUIRE(call.has_value());

    const RackEntry* entry = rack.find(*call);
    REQUIRE(entry != nullptr);
    CHECK(entry->adopted);
    CHECK(entry->owner == "p25trunk");
    CHECK(entry->engine_id == 41);
    CHECK(entry->slot == 1);
    CHECK_FALSE(rack.find(mine)->adopted);

    CHECK(rack.find_adopted(41) == entry);
    CHECK(rack.find_adopted(0) == nullptr);
    CHECK(rack.find_adopted(42) == nullptr);

    // An id this client's own entry carries is not an adopted one.
    REQUIRE(rack.settle(mine, 7));
    CHECK(rack.find_adopted(7) == nullptr);
}

// Rejects adoption past the rack's size, which would hand a slot, and so an
// audio ring, to two receivers.
TEST_CASE("a full rack adopts nothing")
{
    ReceiverRack rack;
    for (std::size_t i = 0; i < kMaxReceivers; ++i) {
        REQUIRE(rack.add().has_value());
    }
    CHECK_FALSE(rack.add_adopted("p25trunk", 9).has_value());
    CHECK(rack.size() == kMaxReceivers);
}

// Rejects labels built three ways in three places, which is how the strip,
// the audio section and a transcript's card came to disagree; and an adopted
// strip called "RX 2", which hides that the operator did not open it.
TEST_CASE("a strip is named RX and its slot, or the plugin and its slot")
{
    RackEntry mine;
    mine.slot = 2;
    CHECK(rack_entry_label(mine) == "RX 3");

    RackEntry call;
    call.slot = 2;
    call.adopted = true;
    call.owner = "p25trunk";
    CHECK(rack_entry_label(call) == "p25trunk 3");

    call.owner.clear();
    CHECK(rack_entry_label(call) == "plugin 3");

    // The audio section's fallback with no receiver, as it always read.
    CHECK(rack_entry_label(RackEntry{}) == "RX 1");
}

// Rejects any focus path that lands on an adopted entry: the pane retunes
// whatever it holds, and this receiver is the plugin's.
TEST_CASE("focus never lands on an adopted entry")
{
    ReceiverRack rack;
    const auto a = *rack.add();
    const auto call = *rack.add_adopted("p25trunk", 5);
    const auto b = *rack.add();

    CHECK_FALSE(rack_entry_focusable(*rack.find(call)));
    CHECK(rack_entry_focusable(*rack.find(a)));

    // Directly.
    REQUIRE(rack.focus(a));
    CHECK_FALSE(rack.focus(call));
    CHECK(rack.focused() == a);

    // Next and previous step over it, both ways and around the end.
    CHECK(rack.neighbour(1) == b);
    CHECK(rack.neighbour(-1) == b);
    REQUIRE(rack.focus(b));
    CHECK(rack.neighbour(1) == a);

    // One focusable entry and an adopted one is nothing to step to.
    rack.remove(b);
    CHECK(rack.neighbour(1) == 0);
}

// Rejects a removal that hands the focus to the trunk call beside the
// receiver that went, in either direction.
TEST_CASE("removing the focused receiver steps over adopted entries")
{
    ReceiverRack rack;
    const auto a = *rack.add();
    const auto b = *rack.add();
    const auto call = *rack.add_adopted("p25trunk", 5);
    const auto c = *rack.add();

    // b goes, the call takes its place, and c is the next one focus may have.
    REQUIRE(rack.focus(b));
    CHECK(rack.remove(b) == c);

    // c was the last, so focus goes up, past the call, to a.
    CHECK(rack.remove(c) == a);

    // a goes and only the call is left: nothing is focused.
    CHECK(rack.remove(a) == 0);
    CHECK(rack.focused() == 0);
    CHECK(rack.find(call) != nullptr);
}

// Rejects a click inside a trunk call's band that focuses the call. Such a
// click is a click on the span, which retunes the focused receiver there.
TEST_CASE("a click inside an adopted band is not a focus")
{
    const std::vector<RackBand> bands{
        {1, 851'000'000.0, 851'012'500.0, true},
        {2, 851'006'000.0, 851'007'000.0, false},  // an adopted narrow one
    };
    CHECK(receiver_under(bands, 851'006'500.0, 0) == 1);
    CHECK(receiver_under(bands, 851'006'500.0, 1) == 0);
}

// Rejects a reconnection that asks the new engine for the plugin's receivers,
// which makes receivers nobody owns and the plugin never closes, and one that
// leaves an adopted strip standing on an id from an engine that has gone.
TEST_CASE("a lost connection takes adopted entries with it and puts back only the rest")
{
    ReceiverRack rack;
    const auto a = *rack.add();
    const auto call = *rack.add_adopted("p25trunk", 12);
    REQUIRE(rack.settle(a, 3));
    REQUIRE(rack.focus(a));

    CHECK(rack_entry_rejoins(*rack.find(a)));
    CHECK_FALSE(rack_entry_rejoins(*rack.find(call)));

    const std::vector<RackEntry> gone = rack.drop_adopted();
    REQUIRE(gone.size() == 1);
    CHECK(gone.front().key == call);
    CHECK(rack.find(call) == nullptr);
    CHECK(rack.focused() == a);

    rack.forget_engine_ids();
    CHECK(rack.find(a)->engine_id == 0);
}

// Rejects forget_engine_ids zeroing an adopted entry's id, which is the only
// thing its Removed event can name it by.
TEST_CASE("forgetting this client's ids leaves an adopted entry's id alone")
{
    ReceiverRack rack;
    const auto call = *rack.add_adopted("p25trunk", 12);
    rack.forget_engine_ids();
    CHECK(rack.find(call)->engine_id == 12);
}

// Rejects a transcript that arrives two seconds after its call ended and has
// lost its strip's colour and name, and a memory that never lets go, which
// would hand a later receiver's transcripts a call long over.
TEST_CASE("a gone adopted receiver is remembered for its late transcripts")
{
    RecentlyAdopted memory;
    memory.remember(41, ReceiverAttribution{2, "p25trunk 3"}, 1'000);

    const auto late = memory.find(41, 3'000);
    REQUIRE(late.has_value());
    CHECK(late->slot == 2);
    CHECK(late->label == "p25trunk 3");

    CHECK(memory.find(41, 1'000 + kAdoptedMemoryMs).has_value());
    CHECK_FALSE(memory.find(41, 1'000 + kAdoptedMemoryMs + 1).has_value());
    CHECK_FALSE(memory.find(42, 3'000).has_value());

    // The same removal reported twice, by the event and by the status poll,
    // is one entry, timed from the later report.
    memory.remember(41, ReceiverAttribution{2, "p25trunk 3"}, 2'000);
    CHECK(memory.size() == 1);
    CHECK(memory.find(41, 2'000 + kAdoptedMemoryMs).has_value());

    // Zero is not an id.
    memory.remember(0, ReceiverAttribution{0, "p25trunk 1"}, 2'000);
    CHECK(memory.size() == 1);

    memory.prune(2'000 + kAdoptedMemoryMs + 1);
    CHECK(memory.size() == 0);

    // A new engine issues ids from the start, so a disconnect forgets all.
    memory.remember(7, ReceiverAttribution{0, "p25trunk 1"}, 5'000);
    memory.clear();
    CHECK_FALSE(memory.find(7, 5'000).has_value());
}
