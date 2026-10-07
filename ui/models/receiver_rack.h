// The receiver rack's rules: which receivers the client holds, which one is
// focused, which colour each wears, who is heard, and what a click on the
// span does. Qt-free, so ui/tests holds every one of them.
//
// WHAT THE RACK IS. The architecture handoff's words: "A vertical stack of
// compact strips, one per VRX, each showing label, frequency, mode, a live
// signal meter, and a mute and solo control. Color-matched to the VRX marker
// on the spectrum. Click to focus, which highlights that VRX's passband and
// pulls its controls into the detail panel." The engine holds any number of
// receivers per session; this client holds up to kMaxReceivers of them, one
// focused and the rest held, and the receiver window's detail, passband,
// decode and RDS sections all belong to the focused one.
//
// WHAT IS IDENTIFIED BY WHAT. A rack entry has a KEY, which this client
// issues and which never changes for the entry's life, and an engine id,
// which the engine issues and which changes every time the receiver is
// rebuilt: a mode change is a remove and an add, and so is a width change the
// engine cannot take in place. So everything the operator can see, the
// colour, the mute, the solo, the gain, hangs off the key, and the engine id
// is looked up when a call needs one.
//
// NOTHING HERE SURVIVES A RESTART. Session restore is the owner's decision
// and has not been taken; the rack comes up empty, and says so.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace revenant::ui {

// Sixty-four, the most the engine carries audio for: kMaxAudioReceivers in
// core/engine/audio_egress.h, a fixed table for the same reason the slot
// arrays here are fixed, a reader on another thread that must not chase a
// reallocation. The owner asked on 2026-10-02 for the limit to go, so that the
// automatic P25 receivers can open one per channel heard; this is as far as it
// goes without a growable lock-free audio path.
//
// WHAT THIS USED TO SAY: "Eight, because the palette is eight colours that
// stay apart for every vision models/receiver_palette.h tests, and a ninth
// receiver would have to share one." A ninth does share one now: the colour is
// the slot modulo the palette, receiver_colour in models/receiver_palette.h,
// and the strip's RX number is what tells two of a colour apart.
inline constexpr std::size_t kMaxReceivers = 64;

struct RackEntry {
    std::uint64_t key = 0;

    // Zero until the engine has answered with one, and zero again while a
    // rebuild is in flight.
    std::uint64_t engine_id = 0;

    // The colour and the audio slot, 0 to kMaxReceivers - 1. The same number
    // is the ring the receiver's audio arrives in, so a slot is never shared.
    std::size_t slot = 0;

    bool muted = false;
    bool solo = false;

    // Where the strip's gain control sits, 0 to 1. See rack_gain_amplitude.
    double gain = 1.0;

    // The engine's sentence when it refused to make this receiver, verbatim,
    // and empty otherwise. Held until an id arrives, because a retune that is
    // refused in the same words is not reported twice and would otherwise
    // put the strip back to opening for good.
    std::string refusal;

    // True for a receiver an engine plugin opened and this rack took in, the
    // trunk tracker's voice calls among them; owner is the plugin's name. See
    // models/adopted_receivers.h for which receivers are taken in and why.
    //
    // AN ADOPTED ENTRY IS HEARD AND NOTHING ELSE. Its mute, solo and gain are
    // this client's, because they act on the mix and touch nothing on the
    // engine. It is never focused, since focus hands the receiver to the pane
    // and the pane retunes it, and never removed from here, since the plugin
    // closes it when its call ends. A write to it would be this window
    // arguing with the plugin over a receiver that is the plugin's.
    bool adopted = false;
    std::string owner;
};

// Whether focus may land on an entry: anything this client made, and nothing
// it adopted.
[[nodiscard]] inline bool rack_entry_focusable(const RackEntry& entry)
{
    return !entry.adopted;
}

// What the strip, the audio section and a transcript's hover card call an
// entry. "RX 3" for one this client made; the plugin's name and the same
// number for one it adopted, "p25trunk 3", so a voice call the operator did
// not open says who opened it. The number is the slot's in both, which is
// what tells two strips of one colour apart.
[[nodiscard]] inline std::string rack_entry_label(const RackEntry& entry)
{
    const std::string number = std::to_string(entry.slot + 1);
    if (!entry.adopted) {
        return "RX " + number;
    }
    return (entry.owner.empty() ? std::string("plugin") : entry.owner) + " " + number;
}

// Whether a reconnection asks the new engine to make this entry's receiver
// again. An adopted one is not: it was the plugin's to open and is the
// plugin's to open again, and the new connection's replay brings back
// whatever the plugin still has. Re-adding it from here would make a second
// receiver nobody owns, which the plugin would never close.
[[nodiscard]] inline bool rack_entry_rejoins(const RackEntry& entry)
{
    return !entry.adopted;
}

// What a strip says about its receiver's existence on the engine.
enum class RackEntryState : std::uint8_t {
    // Asked for and not yet answered.
    Opening,

    // Asked for and refused: there is nothing on the engine behind the strip,
    // and nothing will appear until the operator asks again or removes it.
    Refused,

    // The engine answered with an id.
    Live,
};

[[nodiscard]] inline RackEntryState rack_entry_state(const RackEntry& entry)
{
    if (entry.engine_id != 0) {
        return RackEntryState::Live;
    }
    return entry.refusal.empty() ? RackEntryState::Opening : RackEntryState::Refused;
}

// Said when the engine refused without saying why, so the chip always has a
// sentence behind it.
inline constexpr const char* kRackRefusedFallback =
    "the engine refused to make this receiver and gave no reason";

class ReceiverRack {
public:
    [[nodiscard]] const std::vector<RackEntry>& entries() const { return entries_; }
    [[nodiscard]] std::size_t size() const { return entries_.size(); }
    [[nodiscard]] bool empty() const { return entries_.empty(); }
    [[nodiscard]] bool full() const { return entries_.size() >= kMaxReceivers; }
    [[nodiscard]] std::uint64_t focused() const { return focused_; }

    [[nodiscard]] const RackEntry* find(std::uint64_t key) const
    {
        const auto it = std::find_if(entries_.begin(), entries_.end(),
                                     [key](const RackEntry& e) { return e.key == key; });
        return it == entries_.end() ? nullptr : &*it;
    }

    [[nodiscard]] RackEntry* find(std::uint64_t key)
    {
        const auto it = std::find_if(entries_.begin(), entries_.end(),
                                     [key](const RackEntry& e) { return e.key == key; });
        return it == entries_.end() ? nullptr : &*it;
    }

    [[nodiscard]] std::optional<std::size_t> index_of(std::uint64_t key) const
    {
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].key == key) {
                return i;
            }
        }
        return std::nullopt;
    }

    // A new entry at the bottom of the rack, on the lowest colour slot no
    // other entry holds, or nothing when the rack is full. It is not focused;
    // the caller decides that. Keys are issued from one and never reused, so a
    // key held by something that has gone cannot come to name a new receiver.
    [[nodiscard]] std::optional<std::uint64_t> add()
    {
        if (full()) {
            return std::nullopt;
        }
        std::array<bool, kMaxReceivers> used{};
        for (const RackEntry& e : entries_) {
            used[e.slot] = true;
        }
        std::size_t slot = 0;
        while (slot < kMaxReceivers && used[slot]) {
            ++slot;
        }
        RackEntry entry;
        entry.key = next_key_++;
        entry.slot = slot;
        entries_.push_back(entry);
        return entry.key;
    }

    // An entry for a receiver an engine plugin opened, already live under
    // the engine's id, on the lowest free slot as any other; nothing when the
    // rack is full. Never focused, here or later; see RackEntry::adopted.
    [[nodiscard]] std::optional<std::uint64_t> add_adopted(std::string owner,
                                                           std::uint64_t engine_id)
    {
        const auto key = add();
        if (!key) {
            return std::nullopt;
        }
        RackEntry& entry = entries_.back();
        entry.adopted = true;
        entry.owner = std::move(owner);
        entry.engine_id = engine_id;
        return key;
    }

    // The adopted entry for an engine id, or nothing. Adopted entries only:
    // an id this client's own entry carries can be zero or stale while a
    // rebuild is in flight, and the plugin's events never name one of those.
    [[nodiscard]] const RackEntry* find_adopted(std::uint64_t engine_id) const
    {
        if (engine_id == 0) {
            return nullptr;
        }
        const auto it =
            std::find_if(entries_.begin(), entries_.end(), [engine_id](const RackEntry& e) {
                return e.adopted && e.engine_id == engine_id;
            });
        return it == entries_.end() ? nullptr : &*it;
    }

    // Takes the entry out. When it was the focused one, focus moves to the
    // entry that took its place in the rack, or the one above it when it was
    // the last, which is what closing a tab does; to nothing when the rack is
    // now empty. Answers the key focus moved to, zero for none.
    //
    // Adopted entries are stepped over on the way, in both directions, so
    // closing the receiver beside a trunk call's strip lands on the next one
    // the operator made and never on the call.
    std::uint64_t remove(std::uint64_t key)
    {
        const auto index = index_of(key);
        if (!index) {
            return focused_;
        }
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(*index));
        if (focused_ != key) {
            return focused_;
        }
        focused_ = 0;
        for (std::size_t i = *index; i < entries_.size(); ++i) {
            if (rack_entry_focusable(entries_[i])) {
                focused_ = entries_[i].key;
                return focused_;
            }
        }
        for (std::size_t i = std::min(*index, entries_.size()); i > 0; --i) {
            if (rack_entry_focusable(entries_[i - 1])) {
                focused_ = entries_[i - 1].key;
                return focused_;
            }
        }
        return focused_;
    }

    // Focuses an entry the rack holds and focus may land on; anything else
    // is refused and nothing moves.
    bool focus(std::uint64_t key)
    {
        const RackEntry* entry = find(key);
        if (entry == nullptr || !rack_entry_focusable(*entry)) {
            return false;
        }
        focused_ = key;
        return true;
    }

    // The entry step places away from the focused one, wrapping at both ends,
    // or zero when there is nothing to move to. Does not move focus. Counted
    // over the entries focus may land on, so stepping through the rack walks
    // past every adopted strip rather than stopping on one it cannot take.
    [[nodiscard]] std::uint64_t neighbour(int step) const
    {
        std::vector<std::uint64_t> ring;
        ring.reserve(entries_.size());
        for (const RackEntry& e : entries_) {
            if (rack_entry_focusable(e)) {
                ring.push_back(e.key);
            }
        }
        if (ring.size() < 2) {
            return 0;
        }
        const auto here = std::find(ring.begin(), ring.end(), focused_);
        const auto n = static_cast<long long>(ring.size());
        const long long from =
            here == ring.end() ? 0 : static_cast<long long>(here - ring.begin());
        long long to = (from + step) % n;
        if (to < 0) {
            to += n;
        }
        return ring[static_cast<std::size_t>(to)];
    }

    // Takes every adopted entry out and answers them, for a connection that
    // has gone: their ids were that engine's and their receivers the plugin's
    // on it. Focus never rests on one, so it does not move.
    std::vector<RackEntry> drop_adopted()
    {
        std::vector<RackEntry> out;
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->adopted) {
                out.push_back(std::move(*it));
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
        return out;
    }

    void clear()
    {
        entries_.clear();
        focused_ = 0;
    }

    // The engine that issued every id has gone. The entries stay, because a
    // reconnection puts their receivers back. A refusal goes too: it was that
    // engine's, and the reconnection asks the new one again. Adopted entries
    // are not put back by this client, so the caller takes them out with
    // drop_adopted when the engine goes, and any this leaves alone came on
    // the connection that is up: their ids are the only thing that names them.
    void forget_engine_ids()
    {
        for (RackEntry& e : entries_) {
            if (e.adopted) {
                continue;
            }
            e.engine_id = 0;
            e.refusal.clear();
        }
    }

    // The engine made the entry's receiver, or rebuilt it, under this id, or
    // under zero when the receiver has gone. Only a real id answers a
    // refusal; zero leaves it standing, since nothing was made. Answers
    // whether the entry is held.
    bool settle(std::uint64_t key, std::uint64_t engine_id)
    {
        RackEntry* entry = find(key);
        if (entry == nullptr) {
            return false;
        }
        entry->engine_id = engine_id;
        if (engine_id != 0) {
            entry->refusal.clear();
        }
        return true;
    }

    // The engine refused to make the entry's receiver. Whatever id the entry
    // held is gone with it, since a rebuild removes before it adds, so the
    // strip stops claiming a receiver the engine no longer has. Answers
    // whether the entry is held.
    bool refuse(std::uint64_t key, std::string why)
    {
        RackEntry* entry = find(key);
        if (entry == nullptr) {
            return false;
        }
        entry->engine_id = 0;
        entry->refusal = why.empty() ? std::string{kRackRefusedFallback} : std::move(why);
        return true;
    }

    // Solo on one entry is exclusive, as on a console's solo-in-place with
    // one channel soloed: turning it on turns every other solo off, and
    // turning it off leaves none. Answers the entry's new state.
    bool toggle_solo(std::uint64_t key)
    {
        RackEntry* entry = find(key);
        if (entry == nullptr) {
            return false;
        }
        const bool on = !entry->solo;
        for (RackEntry& e : entries_) {
            e.solo = false;
        }
        entry->solo = on;
        return on;
    }

    [[nodiscard]] bool any_solo() const
    {
        return std::any_of(entries_.begin(), entries_.end(),
                           [](const RackEntry& e) { return e.solo; });
    }

    // Whether an entry's audio reaches the mix. With a solo in force only the
    // soloed entry is heard, whatever its mute says, so soloing a muted strip
    // is how to hear it alone without touching the mute; with none, every
    // entry that is not muted is heard.
    [[nodiscard]] bool heard(std::uint64_t key) const
    {
        const RackEntry* entry = find(key);
        if (entry == nullptr) {
            return false;
        }
        if (any_solo()) {
            return entry->solo;
        }
        return !entry->muted;
    }

private:
    std::vector<RackEntry> entries_;
    std::uint64_t focused_ = 0;
    std::uint64_t next_key_ = 1;
};

// ---------------------------------------------------------------------------
// The strip's gain
// ---------------------------------------------------------------------------

// The strip gain control's travel, in decibels at the bottom and the top.
// The top is unity: a strip can take a receiver down against the others and
// never push one past full scale, which the mix below would have to clip.
inline constexpr double kRackGainFloorDb = -40.0;

// Amplitude for a gain control position, 0 to 1. Linear in decibels over the
// travel, so the middle of the slider is -20 dB and sounds like the middle;
// the very bottom is silence rather than -40 dB, because a strip dragged all
// the way down is being turned off.
[[nodiscard]] inline double rack_gain_amplitude(double position)
{
    if (!(position > 0.0)) {
        return 0.0;
    }
    const double p = std::min(position, 1.0);
    return std::pow(10.0, (kRackGainFloorDb * (1.0 - p)) / 20.0);
}

// "-12 dB", "0 dB" or "off", for the strip's readout.
[[nodiscard]] inline std::string rack_gain_text(double position)
{
    if (!(position > 0.0)) {
        return "off";
    }
    const double db = kRackGainFloorDb * (1.0 - std::min(position, 1.0));
    const long rounded = std::lround(db);
    return std::to_string(rounded == 0 ? 0 : rounded) + " dB";
}

// ---------------------------------------------------------------------------
// A click on the span
// ---------------------------------------------------------------------------

// A receiver's band in absolute hertz, for the hit test.
struct RackBand {
    std::uint64_t key = 0;
    double low_hz = 0.0;
    double high_hz = 0.0;

    // False for an adopted receiver, which a click cannot focus.
    bool focusable = true;
};

// The receiver whose band a frequency is inside, other than the focused one,
// or zero. Where bands overlap the narrowest wins, because a narrow receiver
// inside a wide one can only be reached by clicking inside both, and the wide
// one has plenty of its own band left to be clicked on.
//
// THE FOCUSED RECEIVER IS NOT A TARGET. A click inside its own band is a
// retune within it, which is the fine adjustment a click has always been.
//
// NOR IS AN ADOPTED ONE. Its band is drawn, and a click inside it is a click
// on the span like any other: it tunes the focused receiver there, which is
// how to listen to a trunk call's channel with a receiver of one's own.
[[nodiscard]] inline std::uint64_t receiver_under(const std::vector<RackBand>& bands,
                                                  double hz, std::uint64_t focused)
{
    std::uint64_t best = 0;
    double best_width = 0.0;
    for (const RackBand& band : bands) {
        if (band.key == focused || !band.focusable || !(band.high_hz > band.low_hz)) {
            continue;
        }
        if (hz < band.low_hz || hz > band.high_hz) {
            continue;
        }
        const double width = band.high_hz - band.low_hz;
        if (best == 0 || width < best_width) {
            best = band.key;
            best_width = width;
        }
    }
    return best;
}

// What a click on the spectrum, the waterfall or the ruler does.
enum class SpanClick : std::uint8_t {
    // Inside another receiver's band: that receiver is focused, and nothing
    // is retuned.
    Focus,

    // Anywhere else, with a focused receiver: it moves there.
    Retune,

    // Anywhere else, with an empty rack: the first receiver opens there.
    Open,

    // The second click of a double click, away from every receiver but the
    // focused one: a new receiver opens there and takes the focus, and the
    // one the first click moved goes back to where it was.
    Add,

    // The same, with the rack full: the first click's retune stands and the
    // rack says why nothing was added.
    Full,

    // A second click that has nothing to add to what the first one did: it
    // focused a receiver, or opened the first.
    Nothing,
};

struct SpanClickInput {
    bool double_click = false;

    // receiver_under at the click, against the bands as they were before it.
    std::uint64_t under = 0;

    // The rack held a receiver before this click.
    bool had_receiver = false;

    // The first click of a double click opened the rack's first receiver.
    bool first_click_opened = false;

    std::size_t count = 0;
};

[[nodiscard]] constexpr SpanClick classify_span_click(const SpanClickInput& in)
{
    if (!in.double_click) {
        if (in.under != 0) {
            return SpanClick::Focus;
        }
        return in.had_receiver ? SpanClick::Retune : SpanClick::Open;
    }
    if (in.under != 0 || in.first_click_opened || !in.had_receiver) {
        return SpanClick::Nothing;
    }
    return in.count >= kMaxReceivers ? SpanClick::Full : SpanClick::Add;
}

// The rule in one line, for the hint under the rack and the ruler's tooltip.
// One sentence and not a table, so the place it is read is the place the
// gesture is made.
inline constexpr const char* kSpanClickHint =
    "Click the span to tune the focused receiver. Double-click to add a receiver there. "
    "Click inside another receiver's band to focus it.";

// The same rule in the few words the foot of the rack has room for. The rack
// is shorter docked under the span than it was in a window of its own, and the
// sentence above took three lines of it; the sentence is still the ruler's
// tooltip, where the gesture is made.
inline constexpr const char* kSpanClickHintShort =
    "Click the span to tune, double-click to add a receiver, click a band to focus it.";

// Said in place of an Add when the rack is full.
inline constexpr const char* kRackFullText =
    "The rack holds 64 receivers, the most the engine carries audio for. Remove one to add "
    "another.";

// Said where the strips would be when there are none, and the reason the rack
// comes up that way.
inline constexpr const char* kRackEmptyText =
    "No receivers. Click a signal on the spectrum, or a place on the ruler, and one opens "
    "here. Receivers are not kept across a restart.";

}  // namespace revenant::ui
