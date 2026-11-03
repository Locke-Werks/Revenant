// The desktop's side of the single-user lease, driven by a fake link: the
// refusal screen, Take over, the taken-over screen, Take back, and the rack
// listing receivers this window did not open. The rules are
// models/lease_gate.h and models/adopted_receivers.h.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/rpc/types.h"

#include "models/lease_gate.h"
#include "models/receiver_rack.h"

using revenant::rpc::LoginMode;
using revenant::ui::desktop_holder;
using revenant::ui::lease_login;
using revenant::ui::LeaseGate;
using revenant::ui::LeaseScreen;
using revenant::ui::LoginOutcome;
using revenant::ui::parse_lease_refusal;
using revenant::ui::shortcut_live;
using revenant::ui::rack_entry_label;
using revenant::ui::ReceiverRack;

namespace {

// An engine with one lease, as the engine keeps it: a claim with the
// holder's text joins, any other claim is refused, takeOver always wins.
struct FakeLink {
    std::string holder;
    std::string since = "2026-11-03T09:15:00-05:00";
    std::vector<LoginMode> modes;
    std::vector<std::string> holders;
    bool up = true;

    std::optional<std::string> operator()(LoginMode mode, std::string_view who)
    {
        modes.push_back(mode);
        holders.emplace_back(who);
        if (!up) {
            return std::string("waiting for an engine at 127.0.0.1:17690");
        }
        if (mode == LoginMode::Claim && !holder.empty() && holder != who) {
            return "login refused: in use: " + holder + " since " + since;
        }
        if (mode != LoginMode::Observe) {
            holder = std::string(who);
        }
        return std::nullopt;
    }
};

const std::string kDesktop = desktop_holder("VALKYRIE");
const std::string kPhone = "phone 10.1.10.57 via webgate";

}  // namespace

TEST_CASE("the desktop claims under one holder naming the machine")
{
    CHECK(kDesktop == "Revenant desktop on VALKYRIE");
    CHECK(desktop_holder("") == "Revenant desktop");

    FakeLink link;
    LeaseGate gate;
    CHECK(lease_login(gate, kDesktop, link) == LoginOutcome::Connected);
    // A second connection of the same window joins its own lease.
    CHECK(lease_login(gate, kDesktop, link) == LoginOutcome::Connected);
    CHECK(link.modes == std::vector<LoginMode>{LoginMode::Claim, LoginMode::Claim});
    CHECK(link.holders == std::vector<std::string>{kDesktop, kDesktop});
    CHECK(gate.screen() == LeaseScreen::None);
    CHECK(gate.notice().empty());
}

TEST_CASE("the refusal is parsed whatever the client put before it")
{
    const auto r = parse_lease_refusal("login: in use: Revenant desktop since lunch since "
                                       "2026-11-03T09:15:00-05:00\n");
    REQUIRE(r.has_value());
    CHECK(r->holder == "Revenant desktop since lunch");
    CHECK(r->since == "2026-11-03T09:15:00-05:00");
    CHECK_FALSE(parse_lease_refusal("waiting for an engine").has_value());
    CHECK_FALSE(parse_lease_refusal("in use: nobody").has_value());
}

TEST_CASE("a refused claim shows the refusal screen with Take over")
{
    FakeLink link;
    link.holder = kPhone;
    LeaseGate gate;

    CHECK(lease_login(gate, kDesktop, link) == LoginOutcome::Refused);
    CHECK(gate.screen() == LeaseScreen::Refused);
    CHECK(gate.notice() ==
          "Revenant is in use by phone 10.1.10.57 via webgate since 2026-11-03T09:15:00-05:00");
    CHECK(gate.action() == "Take over");
    // Retrying is still a claim: the operator has not chosen to evict anyone.
    CHECK(gate.may_connect());
    CHECK(gate.next_mode() == LoginMode::Claim);
}

TEST_CASE("an absent engine is not a refusal")
{
    FakeLink link;
    link.up = false;
    LeaseGate gate;
    CHECK(lease_login(gate, kDesktop, link) == LoginOutcome::Failed);
    CHECK(gate.screen() == LeaseScreen::None);
}

TEST_CASE("Take over logs in with takeOver once and clears the screen")
{
    FakeLink link;
    link.holder = kPhone;
    LeaseGate gate;
    REQUIRE(lease_login(gate, kDesktop, link) == LoginOutcome::Refused);

    gate.take_over();
    CHECK(lease_login(gate, kDesktop, link) == LoginOutcome::Connected);
    CHECK(link.modes.back() == LoginMode::TakeOver);
    CHECK(link.holder == kDesktop);
    CHECK(gate.screen() == LeaseScreen::None);
    // A later reconnect claims again rather than evicting whoever is there.
    CHECK(gate.next_mode() == LoginMode::Claim);
}

TEST_CASE("a takeover shows the taken-over screen and stops logging in")
{
    FakeLink link;
    LeaseGate gate;
    REQUIRE(lease_login(gate, kDesktop, link) == LoginOutcome::Connected);

    gate.on_lease_lost(kPhone);
    link.holder = kPhone;
    CHECK(gate.screen() == LeaseScreen::TakenOver);
    CHECK(gate.notice() == "Taken over by phone 10.1.10.57 via webgate");
    CHECK(gate.action() == "Take back");

    const auto calls = link.modes.size();
    CHECK(lease_login(gate, kDesktop, link) == LoginOutcome::Skipped);
    // Not even once the phone leaves: taking the engine back is the operator's call.
    link.holder.clear();
    CHECK(lease_login(gate, kDesktop, link) == LoginOutcome::Skipped);
    CHECK(link.modes.size() == calls);
}

TEST_CASE("Take back logs in with takeOver, even through an engine restart")
{
    FakeLink link;
    LeaseGate gate;
    REQUIRE(lease_login(gate, kDesktop, link) == LoginOutcome::Connected);
    gate.on_lease_lost(kPhone);
    link.holder = kPhone;

    gate.take_over();
    link.up = false;
    CHECK(lease_login(gate, kDesktop, link) == LoginOutcome::Failed);
    link.up = true;
    CHECK(lease_login(gate, kDesktop, link) == LoginOutcome::Connected);
    CHECK(link.modes.back() == LoginMode::TakeOver);
    CHECK(link.holder == kDesktop);
    CHECK(gate.screen() == LeaseScreen::None);
}

// Rejects application shortcuts that keep tuning behind the notice: they fire
// whatever has focus, so the notice's own key handler cannot stop them.
TEST_CASE("application shortcuts are off while a lease notice is up")
{
    LeaseGate gate;
    CHECK(shortcut_live(true, gate.notice()));
    CHECK_FALSE(shortcut_live(false, gate.notice()));

    gate.on_connect_failed("in use: " + kPhone + " since 2026-11-03T09:15:00-05:00");
    CHECK_FALSE(shortcut_live(true, gate.notice()));

    gate.on_connected();
    CHECK(shortcut_live(true, gate.notice()));

    gate.on_lease_lost(kPhone);
    CHECK_FALSE(shortcut_live(true, gate.notice()));
}

TEST_CASE("receivers opened elsewhere are listed in the rack")
{
    ReceiverRack rack;
    const auto mine = rack.add();
    const auto phone = rack.add_adopted("RX", 7);
    REQUIRE(mine.has_value());
    REQUIRE(phone.has_value());
    CHECK(rack.size() == 2);
    CHECK(rack_entry_label(*rack.find(*phone)) == "RX 2");
    CHECK(rack.find_adopted(7) != nullptr);
}
