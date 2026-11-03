// The desktop's side of the engine's single-user lease. Qt-free, so ui/tests
// holds the rules with a fake link; ui/models/engine_link.cpp applies them.
//
// The engine serves one user at a time (docs/superpowers/specs/
// 2026-11-03-single-user-engine-design.md). This window claims the lease on
// every connection, and a refused claim or a takeover puts a full-window
// notice over everything, because a window drawn under someone else's lease
// would show controls that the engine refuses.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/rpc/types.h"

namespace revenant::ui {

// The holder text this desktop claims with. Every connection the desktop
// opens uses the same text, because the engine lets a claim carrying the
// holder's exact text join the lease, and a second text would refuse the
// desktop to itself.
[[nodiscard]] inline std::string desktop_holder(std::string_view computer)
{
    if (computer.empty()) {
        return "Revenant desktop";
    }
    return "Revenant desktop on " + std::string(computer);
}

struct LeaseRefusal {
    std::string holder;
    std::string since;

    friend bool operator==(const LeaseRefusal&, const LeaseRefusal&) = default;
};

// The engine's refusal, "in use: <holder> since <time>", found anywhere in a
// connect failure's message, since the client may prefix its own context.
// Split on the last " since " because a holder is free text and may contain
// the word, while the time never does.
[[nodiscard]] inline std::optional<LeaseRefusal> parse_lease_refusal(std::string_view message)
{
    constexpr std::string_view kPrefix = "in use: ";
    constexpr std::string_view kSince = " since ";
    const auto start = message.find(kPrefix);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view rest = message.substr(start + kPrefix.size());
    const auto split = rest.rfind(kSince);
    if (split == std::string_view::npos) {
        return std::nullopt;
    }
    LeaseRefusal refusal;
    refusal.holder = std::string(rest.substr(0, split));
    refusal.since = std::string(rest.substr(split + kSince.size()));
    while (!refusal.since.empty() &&
           (refusal.since.back() == '\n' || refusal.since.back() == '\r' ||
            refusal.since.back() == ' ')) {
        refusal.since.pop_back();
    }
    if (refusal.holder.empty() || refusal.since.empty()) {
        return std::nullopt;
    }
    return refusal;
}

// What the window is showing about the lease.
enum class LeaseScreen : std::uint8_t {
    // No notice: connected and holding the lease, or not connected yet.
    None,
    // A claim was refused: "Revenant is in use by ..." with Take over and Quit.
    Refused,
    // Another client took the lease: "Taken over by ..." with Take back and Quit.
    TakenOver,
};

// The lease state machine. One per window, used from the supervisor thread
// and the Qt thread, so the caller serialises access.
class LeaseGate {
public:
    // The mode the next login uses. takeOver only once, right after the
    // operator asked for it, so a later reconnect does not evict someone the
    // operator never chose to evict.
    [[nodiscard]] rpc::LoginMode next_mode() const
    {
        return take_over_asked_ ? rpc::LoginMode::TakeOver : rpc::LoginMode::Claim;
    }

    // Whether the supervisor may log in at all. Not after a takeover: the new
    // holder has the engine, and a retry would either be refused or, once
    // they leave, quietly take it back without the operator asking.
    [[nodiscard]] bool may_connect() const
    {
        return screen_ != LeaseScreen::TakenOver || take_over_asked_;
    }

    // A login succeeded: the lease is this window's.
    void on_connected()
    {
        screen_ = LeaseScreen::None;
        take_over_asked_ = false;
        holder_.clear();
        since_.clear();
    }

    // A login failed with this message. True when it was a lease refusal,
    // which the window shows instead of the connection line.
    bool on_connect_failed(std::string_view message)
    {
        // A failure that is not a refusal (no engine yet) keeps a takeover
        // asked for, so it goes ahead once the engine answers.
        auto refusal = parse_lease_refusal(message);
        if (!refusal) {
            return false;
        }
        take_over_asked_ = false;
        screen_ = LeaseScreen::Refused;
        holder_ = std::move(refusal->holder);
        since_ = std::move(refusal->since);
        return true;
    }

    // The engine said another client took the lease.
    void on_lease_lost(std::string_view new_holder)
    {
        screen_ = LeaseScreen::TakenOver;
        holder_ = new_holder.empty() ? std::string("another client") : std::string(new_holder);
        since_.clear();
        take_over_asked_ = false;
    }

    // Take over from the refusal screen, or Take back from the taken-over
    // screen: the next login evicts the holder.
    void take_over() { take_over_asked_ = true; }

    [[nodiscard]] LeaseScreen screen() const { return screen_; }
    [[nodiscard]] const std::string& holder() const { return holder_; }
    [[nodiscard]] const std::string& since() const { return since_; }

    // The notice's sentence, empty when there is none.
    [[nodiscard]] std::string notice() const
    {
        switch (screen_) {
        case LeaseScreen::Refused:
            return "Revenant is in use by " + holder_ + " since " + since_;
        case LeaseScreen::TakenOver:
            return "Taken over by " + holder_;
        case LeaseScreen::None:
            break;
        }
        return {};
    }

    // The label on the button that evicts the holder.
    [[nodiscard]] std::string action() const
    {
        return screen_ == LeaseScreen::TakenOver ? "Take back" : "Take over";
    }

private:
    LeaseScreen screen_ = LeaseScreen::None;
    std::string holder_;
    std::string since_;
    bool take_over_asked_ = false;
};

enum class LoginOutcome : std::uint8_t { Skipped, Connected, Refused, Failed };

// One login through the gate. connect(mode, holder) logs in and answers the
// failure message, or nothing on success. Shared by EngineLink and the tests'
// fake link, so the tests drive the same sequence the window runs.
template <typename Connect>
LoginOutcome lease_login(LeaseGate& gate, std::string_view holder, Connect&& connect)
{
    if (!gate.may_connect()) {
        return LoginOutcome::Skipped;
    }
    const std::optional<std::string> failure = connect(gate.next_mode(), holder);
    if (!failure) {
        gate.on_connected();
        return LoginOutcome::Connected;
    }
    return gate.on_connect_failed(*failure) ? LoginOutcome::Refused : LoginOutcome::Failed;
}

}  // namespace revenant::ui
