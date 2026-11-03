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

// Rejects a rack that hides receivers another session of the one user opened,
// or takes in the engine's own, or this session's a second time under a key it
// did not issue.
TEST_CASE("a plugin's and another session's receivers are adopted")
{
    CHECK(rack_adopts(VrxOwnerKind::Plugin));
    CHECK(rack_adopts(VrxOwnerKind::OtherSession));
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

// Rejects a rack that keeps the operator off a receiver someone else opened:
// with one user every receiver is theirs to focus, step to and remove.
TEST_CASE("a receiver opened by another session is focused, stepped to and removed")
{
    ReceiverRack rack;
    const auto a = *rack.add();
    const auto phone = *rack.add_adopted("RX", 5);
    const auto b = *rack.add();

    CHECK(rack_entry_focusable(*rack.find(phone)));
    REQUIRE(rack.focus(a));
    CHECK(rack.neighbour(1) == phone);
    REQUIRE(rack.focus(phone));
    CHECK(rack.focused() == phone);

    // Focus changes nothing about where it came from: the engine's events
    // still find it, and a reconnection still leaves it to the replay.
    CHECK(rack.find(phone)->adopted);
    CHECK(rack.find_adopted(5) == rack.find(phone));
    CHECK_FALSE(rack_entry_rejoins(*rack.find(phone)));

    // Removed like any other, and focus moves on.
    CHECK(rack.remove(phone) == b);
    CHECK(rack.find(phone) == nullptr);

    // One never focused is removed the same way, leaving focus where it was.
    const auto call = *rack.add_adopted("trunk", 9);
    CHECK(rack.remove(call) == b);
    CHECK(rack.find(call) == nullptr);
}

// Rejects a focused trunk call that outlives the engine's Removed event, which
// left one dead strip per call clicked, and focus left on an entry that went.
TEST_CASE("a focused trunk receiver the engine removes leaves and moves focus")
{
    ReceiverRack rack;
    const auto a = *rack.add();
    const auto call = *rack.add_adopted("trunk", 21);
    const auto b = *rack.add();
    REQUIRE(rack.focus(call));

    // What drop_adopted_entry does for the pane's entry: find it by engine
    // id, as the Removed event names it, and remove it.
    const RackEntry* found = rack.find_adopted(21);
    REQUIRE(found != nullptr);
    CHECK(found->key == call);
    CHECK(rack.remove(found->key) == b);
    CHECK(rack.focused() == b);
    CHECK(rack.find_adopted(21) == nullptr);
    CHECK(rack.find(a) != nullptr);
}

// Rejects a reconnection that recreates a focused plugin receiver, an orphan
// nobody follows and the plugin never closes.
TEST_CASE("a lost connection drops a focused adopted entry and does not recreate it")
{
    ReceiverRack rack;
    const auto a = *rack.add();
    const auto call = *rack.add_adopted("trunk", 30);
    REQUIRE(rack.focus(call));
    CHECK_FALSE(rack_entry_rejoins(*rack.find(call)));

    const std::vector<RackEntry> gone = rack.drop_adopted();
    REQUIRE(gone.size() == 1);
    CHECK(gone.front().key == call);
    CHECK(rack.focused() == 0);
    REQUIRE(rack.size() == 1);
    CHECK(rack.find(a) != nullptr);
    for (const RackEntry& e : rack.entries()) {
        CHECK(rack_entry_rejoins(e));
    }
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
