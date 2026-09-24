// Every action the keyboard can reach, in one table: its id, what it is
// called, where it is grouped, its default keys, where those keys apply, what
// it needs before it can do anything, and the hook that carries it out.
//
// WHY ONE TABLE. The client had a handful of keys, all of them on the filter
// display and written as a switch in render/passband_item.cpp, and the only
// record of them was a sentence in docs/ui-spectrum.md and a hint line under
// the display. Three copies of one fact, and the next key added would have
// been a fourth. Here the table is the fact: the window binds its shortcuts
// from it, the filter display looks its keys up in it, the command palette
// lists it, the key map is drawn from it, and the key map in
// docs/ui-spectrum.md is checked against it by ui/tests/test_key_actions.cpp.
// A key changed here changes everywhere or fails a test.
//
// Qt-free on the rule the rest of ui/models follows, so ui/tests can hold
// it. models/key_map.h is the one-line conversions QML calls.
//
// THE HANDLER HOOK IS A NAME, NOT A FUNCTION POINTER. What most of these do
// is call something on an object that only exists in the running window: a
// dial, a popover, the audio player. So each action names its handler and an
// argument, and ui/qml/Commands.qml holds the one table from handler to call.
// A smoke run asks that table which handlers it lacks and fails on any, so a
// row added here without a handler there is a red run rather than a key that
// silently does nothing. The filter display's keys, the overlay's keys and the
// frequency manager's are handled in C++, in the overlay and in the manager's
// panel, through passband_command, overlay_command and memory_command below,
// whose coverage the tests assert.
//
// WHAT IS NOT HERE, AND WHY.
//
// A list of detections. Nothing in the client lists the detections
// themselves; they are drawn on the span. The detector's settings are the
// control row under the top bar, and the detections key puts the arrow keys on
// its threshold.
//
// WHAT THAT PARAGRAPH USED TO SAY, before 2026-09-23: "The "detections" panel
// is the detector's two thresholds" and "The key opens the panel that exists."
// The panel went when its sliders moved into the control row.
//
// WHAT THIS BLOCK USED TO SAY FIRST: "Next and previous receiver, and solo.
// The engine holds any number of receivers and this client holds one per
// window, so there is no other receiver to move to or to solo against". The
// client holds a rack of up to eight now, and all three are in the table,
// needing a second receiver where there has to be another to move to.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace revenant::ui {

// Where a key applies.
//
// Window keys work in both windows, whatever has focus, except that a text
// field keeps the keys it types with. Filter keys work while the filter
// display has focus, after a click on it or the window key that puts them
// there. Overlay keys work while the command palette or the key map is open.
// Memory keys work while the frequency manager is open and has focus.
enum class KeyContext {
    Window,
    Filter,
    Overlay,
    Memories,
};

// What an action needs before it can do anything, as bits. An action is
// enabled when every bit it needs is in what the window has.
enum KeyNeed : std::uint32_t {
    kNeedsNothing = 0,

    // A connected engine.
    kNeedsEngine = 1U << 0,

    // An open source that will retune.
    kNeedsRetune = 1U << 1,

    // An open source.
    kNeedsSource = 1U << 2,

    // A focused receiver in the rack.
    kNeedsReceiver = 1U << 3,

    // A receiver whose mode AFT has a centre to aim at.
    kNeedsAft = 1U << 4,

    // A receiver whose mode the auto filter can fit.
    kNeedsAutoFilter = 1U << 5,

    // A spectrum on screen, which is what a pinned end is pinned against.
    kNeedsSpectrum = 1U << 6,

    // A second receiver in the rack, which is what moving the focus needs.
    kNeedsSecondReceiver = 1U << 7,

    // A receiver whose mode offers the noise blanker and noise reduction,
    // which is every mode that produces audio; one that offers the manual
    // notch; one that offers the automatic notch. models/noise_controls.h
    // has the table.
    kNeedsNoise = 1U << 8,
    kNeedsNotch = 1U << 9,
    kNeedsAutoNotch = 1U << 10,
};

struct KeyAction {
    std::string_view id;

    // What the palette and the key map call it, lower case, as a phrase an
    // operator would say.
    std::string_view label;
    std::string_view group;

    // Other words the palette should find it by, space separated.
    std::string_view keywords;

    // The default key sequences in Qt's portable text, modifiers first in
    // the order Ctrl, Alt, Shift, Meta. Empty for an action the palette
    // reaches and no key does.
    std::array<std::string_view, 2> keys;

    KeyContext context = KeyContext::Window;
    std::uint32_t needs = kNeedsNothing;

    // The handler in ui/qml/Commands.qml, and what it is handed.
    std::string_view handler;
    std::string_view argument;

    // Shift and Ctrl change the size of this action's step rather than
    // naming a different action. Only the filter display's keys do this.
    bool scaled = false;
};

// The filter display's step sizes, which the key map states. render/
// passband_item.cpp static_asserts that models/engine_link.h agrees, so the
// sentence the key map prints cannot drift from the steps the keys take.
inline constexpr int kFilterKeyStepHz = 10;
inline constexpr int kFilterKeyCoarseStepHz = 100;
inline constexpr int kFilterKeyFineStepHz = 1;

// One press of the window's widen and narrow keys, the same hundred hertz the
// receiver panel's two width buttons move.
inline constexpr int kFilterWidenStepHz = 100;

// clang-format off
inline constexpr std::array kKeyActions{
    // Tuning the front end.
    KeyAction{"tune.up", "step the radio up by the tuning digit", "tuning", "frequency dial increase",
              {"Alt+Up", ""}, KeyContext::Window, kNeedsRetune, "tune.step", "1"},
    KeyAction{"tune.down", "step the radio down by the tuning digit", "tuning", "frequency dial decrease",
              {"Alt+Down", ""}, KeyContext::Window, kNeedsRetune, "tune.step", "-1"},
    KeyAction{"tune.coarser", "move the tuning digit left", "tuning", "frequency dial coarser bigger step",
              {"Alt+Left", ""}, KeyContext::Window, kNeedsRetune, "tune.digit", "1"},
    KeyAction{"tune.finer", "move the tuning digit right", "tuning", "frequency dial finer smaller step",
              {"Alt+Right", ""}, KeyContext::Window, kNeedsRetune, "tune.digit", "-1"},
    KeyAction{"tune.page_up", "page the radio up a tenth of the span", "tuning", "frequency scroll",
              {"PgUp", ""}, KeyContext::Window, kNeedsRetune, "tune.page", "1"},
    KeyAction{"tune.page_down", "page the radio down a tenth of the span", "tuning", "frequency scroll",
              {"PgDown", ""}, KeyContext::Window, kNeedsRetune, "tune.page", "-1"},
    KeyAction{"tune.type", "type a frequency for the radio", "tuning", "frequency dial enter go to",
              {"Ctrl+G", ""}, KeyContext::Window, kNeedsRetune, "tune.type", ""},
    KeyAction{"tune.band", "jump to a band", "tuning", "band plan menu",
              {"Ctrl+J", ""}, KeyContext::Window, kNeedsRetune, "palette.bands", ""},

    // The receiver, and the rack of them. Adding makes a new receiver and
    // focuses it; the focused one is what every other receiver key acts on.
    KeyAction{"receiver.add", "put a new receiver on the span centre", "receiver", "add new vfo rack",
              {"Ctrl+N", ""}, KeyContext::Window, kNeedsSource, "receiver.add", ""},
    KeyAction{"receiver.centre", "move the receiver to the span centre", "receiver", "vfo tune middle",
              {"", ""}, KeyContext::Window, kNeedsReceiver, "receiver.centre", ""},
    KeyAction{"receiver.next", "focus the next receiver", "receiver", "vfo rack switch down",
              {"Ctrl+PgDown", ""}, KeyContext::Window, kNeedsSecondReceiver, "receiver.next", "1"},
    KeyAction{"receiver.previous", "focus the previous receiver", "receiver", "vfo rack switch up",
              {"Ctrl+PgUp", ""}, KeyContext::Window, kNeedsSecondReceiver, "receiver.next", "-1"},
    KeyAction{"receiver.solo", "solo the receiver or stop soloing", "receiver", "vfo rack alone listen audio",
              {"Ctrl+Shift+S", ""}, KeyContext::Window, kNeedsReceiver, "receiver.solo", ""},
    KeyAction{"receiver.type", "type a frequency for the receiver", "receiver", "frequency dial vfo go to",
              {"Ctrl+Shift+G", ""}, KeyContext::Window, kNeedsReceiver, "receiver.type", ""},
    KeyAction{"receiver.remove", "remove the receiver", "receiver", "close delete vfo",
              {"Ctrl+Del", ""}, KeyContext::Window, kNeedsReceiver, "receiver.remove", ""},
    KeyAction{"receiver.aft", "turn AFT on or off", "receiver", "automatic frequency tracking follow drift",
              {"Ctrl+T", ""}, KeyContext::Window, kNeedsAft, "receiver.aft", ""},
    KeyAction{"receiver.auto_filter", "turn the auto filter on or off", "receiver", "fit bandwidth automatic",
              {"Ctrl+Shift+F", ""}, KeyContext::Window, kNeedsAutoFilter, "receiver.auto_filter", ""},
    KeyAction{"noise.blanker", "turn the noise blanker on or off", "noise", "nb impulse ignition clicks",
              {"Ctrl+Shift+B", ""}, KeyContext::Window, kNeedsNoise, "receiver.noise", "nb"},
    KeyAction{"noise.notch", "turn the notch on or off", "noise", "manual whistle tone filter",
              {"Ctrl+Shift+N", ""}, KeyContext::Window, kNeedsNotch, "receiver.noise", "notch"},
    KeyAction{"noise.auto_notch", "turn the automatic notch on or off", "noise", "anf heterodyne whistle tone",
              {"Ctrl+Shift+A", ""}, KeyContext::Window, kNeedsAutoNotch, "receiver.noise", "auto_notch"},
    KeyAction{"noise.reduction", "turn noise reduction on or off", "noise", "nr hiss dsp",
              {"Ctrl+Shift+R", ""}, KeyContext::Window, kNeedsNoise, "receiver.noise", "nr"},

    // The receiver's mode. The nine on the selector's row take Alt and their
    // place on it, sam last on Alt+9 so no key an operator already has
    // moved; the digital four are the palette's. Their order is
    // kModeChoices', which ui/tests holds this table to.
    KeyAction{"mode.am", "switch the receiver to am", "mode", "mode demodulator",
              {"Alt+1", ""}, KeyContext::Window, kNeedsReceiver, "receiver.mode", "am"},
    KeyAction{"mode.nfm", "switch the receiver to nfm", "mode", "mode demodulator fm narrow",
              {"Alt+2", ""}, KeyContext::Window, kNeedsReceiver, "receiver.mode", "nfm"},
    KeyAction{"mode.wfm", "switch the receiver to wfm", "mode", "mode demodulator fm wide broadcast",
              {"Alt+3", ""}, KeyContext::Window, kNeedsReceiver, "receiver.mode", "wfm"},
    KeyAction{"mode.usb", "switch the receiver to usb", "mode", "mode demodulator ssb upper sideband",
              {"Alt+4", ""}, KeyContext::Window, kNeedsReceiver, "receiver.mode", "usb"},
    KeyAction{"mode.lsb", "switch the receiver to lsb", "mode", "mode demodulator ssb lower sideband",
              {"Alt+5", ""}, KeyContext::Window, kNeedsReceiver, "receiver.mode", "lsb"},
    KeyAction{"mode.dsb", "switch the receiver to dsb", "mode", "mode demodulator double sideband",
              {"Alt+6", ""}, KeyContext::Window, kNeedsReceiver, "receiver.mode", "dsb"},
    KeyAction{"mode.cw", "switch the receiver to cw", "mode", "mode demodulator morse",
              {"Alt+7", ""}, KeyContext::Window, kNeedsReceiver, "receiver.mode", "cw"},
    KeyAction{"mode.raw", "switch the receiver to raw", "mode", "mode demodulator iq baseband",
              {"Alt+8", ""}, KeyContext::Window, kNeedsReceiver, "receiver.mode", "raw"},
    KeyAction{"mode.sam", "switch the receiver to sam", "mode",
              "mode demodulator synchronous am carrier fading",
              {"Alt+9", ""}, KeyContext::Window, kNeedsReceiver, "receiver.mode", "sam"},
    KeyAction{"mode.p25p1", "switch the receiver to P25", "mode", "mode digital p25p1",
              {"", ""}, KeyContext::Window, kNeedsReceiver, "receiver.mode", "p25p1"},
    KeyAction{"mode.dstar", "switch the receiver to D-STAR", "mode", "mode digital dstar",
              {"", ""}, KeyContext::Window, kNeedsReceiver, "receiver.mode", "dstar"},
    KeyAction{"mode.tetra", "switch the receiver to TETRA", "mode", "mode digital",
              {"", ""}, KeyContext::Window, kNeedsReceiver, "receiver.mode", "tetra"},
    KeyAction{"mode.dmr", "switch the receiver to DMR", "mode", "mode digital dmr tier",
              {"", ""}, KeyContext::Window, kNeedsReceiver, "receiver.mode", "dmr"},

    // The receiver's filter, from anywhere.
    KeyAction{"filter.widen", "widen the filter", "filter", "bandwidth passband wider",
              {"Ctrl+=", ""}, KeyContext::Window, kNeedsReceiver, "filter.widen", "1"},
    KeyAction{"filter.narrow", "narrow the filter", "filter", "bandwidth passband narrower",
              {"Ctrl+-", ""}, KeyContext::Window, kNeedsReceiver, "filter.widen", "-1"},
    KeyAction{"filter.default", "put the mode's default filter back", "filter", "bandwidth passband reset",
              {"Ctrl+0", ""}, KeyContext::Window, kNeedsReceiver, "filter.default", ""},
    KeyAction{"filter.keys", "put the arrow keys on the filter edges", "filter", "bandwidth passband focus edit",
              {"Ctrl+E", ""}, KeyContext::Window, kNeedsReceiver, "filter.keys", ""},

    // Audio. The player carries one receiver's audio, so these need nothing.
    KeyAction{"audio.mute", "mute or unmute the audio", "audio", "silence sound",
              {"Ctrl+M", ""}, KeyContext::Window, kNeedsNothing, "audio.mute", ""},
    KeyAction{"audio.louder", "turn the volume up", "audio", "louder sound level",
              {"Alt+=", ""}, KeyContext::Window, kNeedsNothing, "audio.volume", "1"},
    KeyAction{"audio.quieter", "turn the volume down", "audio", "quieter sound level",
              {"Alt+-", ""}, KeyContext::Window, kNeedsNothing, "audio.volume", "-1"},

    // The colour map's ends, [ for the low one and ] for the high one, as on
    // the filter display.
    KeyAction{"scale.floor", "pin or unpin the spectrum floor", "display", "colour map scale lock bottom",
              {"Ctrl+[", ""}, KeyContext::Window, kNeedsSpectrum, "scale.pin", "floor"},
    KeyAction{"scale.ceiling", "pin or unpin the spectrum ceiling", "display", "colour map scale lock top",
              {"Ctrl+]", ""}, KeyContext::Window, kNeedsSpectrum, "scale.pin", "ceiling"},

    // The detector's threshold in the control row, which used to be a panel
    // this key opened.
    KeyAction{"detector.keys", "put the arrow keys on the detection threshold", "display",
              "thresholds detector detections snr margin held",
              {"Ctrl+Shift+D", ""}, KeyContext::Window, kNeedsSpectrum, "detector.keys", ""},

    // Panels and windows.
    KeyAction{"panel.radio", "open the radio picker", "panels", "device source open choose",
              {"Ctrl+O", ""}, KeyContext::Window, kNeedsEngine, "panel.open", "radio"},
    KeyAction{"panel.memories", "open the frequency manager", "panels",
              "bookmarks marks memories channels saved scan import export chirp sdr",
              {"Ctrl+B", ""}, KeyContext::Window, kNeedsNothing, "panel.open", "memories"},
    KeyAction{"memory.save", "save the receiver as a memory", "panels", "bookmark mark memory store",
              {"Ctrl+D", ""}, KeyContext::Window, kNeedsReceiver, "memory.save", ""},
    KeyAction{"window.receivers", "show or hide the receivers", "panels", "vfo rack dock window",
              {"Ctrl+R", ""}, KeyContext::Window, kNeedsNothing, "window.receivers", ""},
    KeyAction{"window.pop", "pop the receivers out or dock them", "panels",
              "vfo rack window undock float separate screen", {"Ctrl+Shift+W", ""},
              KeyContext::Window, kNeedsNothing, "window.pop", ""},

    // Finding the rest.
    KeyAction{"palette.open", "open the command palette", "help", "search commands find",
              {"Ctrl+K", "Ctrl+Shift+P"}, KeyContext::Window, kNeedsNothing, "palette.open", ""},
    KeyAction{"keymap.open", "show the key map", "help", "shortcuts keys keyboard help",
              {"F1", ""}, KeyContext::Window, kNeedsNothing, "keymap.open", ""},

    // The filter display's own keys, which were the client's only keys until
    // this table. Unchanged, and looked up here now.
    KeyAction{"edge.low", "select the low edge", "filter edges", "",
              {"[", ""}, KeyContext::Filter, kNeedsReceiver, "", ""},
    KeyAction{"edge.high", "select the high edge", "filter edges", "",
              {"]", ""}, KeyContext::Filter, kNeedsReceiver, "", ""},
    KeyAction{"edge.both", "select both edges", "filter edges", "",
              {"\\", ""}, KeyContext::Filter, kNeedsReceiver, "", ""},
    KeyAction{"edge.down", "move the selection down", "filter edges", "",
              {"Left", ""}, KeyContext::Filter, kNeedsReceiver, "", "", true},
    KeyAction{"edge.up", "move the selection up", "filter edges", "",
              {"Right", ""}, KeyContext::Filter, kNeedsReceiver, "", "", true},
    KeyAction{"edge.widen", "widen both edges", "filter edges", "",
              {"Up", ""}, KeyContext::Filter, kNeedsReceiver, "", "", true},
    KeyAction{"edge.narrow", "narrow both edges", "filter edges", "",
              {"Down", ""}, KeyContext::Filter, kNeedsReceiver, "", "", true},
    KeyAction{"edge.default", "put the mode's default filter back", "filter edges", "",
              {"Home", ""}, KeyContext::Filter, kNeedsReceiver, "", ""},
    KeyAction{"edge.cancel", "cancel a drag", "filter edges", "",
              {"Esc", ""}, KeyContext::Filter, kNeedsReceiver, "", ""},

    // The palette's and the key map's own keys.
    KeyAction{"overlay.next", "next entry", "palette", "",
              {"Down", ""}, KeyContext::Overlay, kNeedsNothing, "", ""},
    KeyAction{"overlay.previous", "previous entry", "palette", "",
              {"Up", ""}, KeyContext::Overlay, kNeedsNothing, "", ""},
    KeyAction{"overlay.run", "run the chosen entry", "palette", "",
              {"Return", "Enter"}, KeyContext::Overlay, kNeedsNothing, "", ""},
    KeyAction{"overlay.close", "close", "palette", "",
              {"Esc", ""}, KeyContext::Overlay, kNeedsNothing, "", ""},

    // The frequency manager's own keys. Recall wants a receiver's engine and
    // the rest act on the list alone; the panel greys what cannot run.
    KeyAction{"memories.next", "next memory", "memories", "",
              {"Down", ""}, KeyContext::Memories, kNeedsNothing, "", ""},
    KeyAction{"memories.previous", "previous memory", "memories", "",
              {"Up", ""}, KeyContext::Memories, kNeedsNothing, "", ""},
    KeyAction{"memories.recall", "recall to the focused receiver", "memories", "",
              {"Return", "Enter"}, KeyContext::Memories, kNeedsSource, "", ""},
    KeyAction{"memories.recall_new", "recall into a new receiver", "memories", "",
              {"Shift+Return", "Shift+Enter"}, KeyContext::Memories, kNeedsSource, "", ""},
    KeyAction{"memories.edit", "rename the memory", "memories", "",
              {"F2", ""}, KeyContext::Memories, kNeedsNothing, "", ""},
    KeyAction{"memories.delete", "delete the memory", "memories", "",
              {"Del", ""}, KeyContext::Memories, kNeedsNothing, "", ""},
    KeyAction{"memories.undo", "undo the last delete", "memories", "",
              {"Ctrl+Z", ""}, KeyContext::Memories, kNeedsNothing, "", ""},
    KeyAction{"memories.search", "search the memories", "memories", "",
              {"Ctrl+F", ""}, KeyContext::Memories, kNeedsNothing, "", ""},
    KeyAction{"memories.close", "close the frequency manager", "memories", "",
              {"Esc", ""}, KeyContext::Memories, kNeedsNothing, "", ""},
};
// clang-format on

[[nodiscard]] constexpr const KeyAction* find_key_action(std::string_view id)
{
    for (const KeyAction& action : kKeyActions) {
        if (action.id == id) {
            return &action;
        }
    }
    return nullptr;
}

// What each context is called in the key map.
struct KeyContextInfo {
    KeyContext context;
    std::string_view name;
};

inline constexpr std::array kKeyContexts{
    KeyContextInfo{KeyContext::Window, "anywhere"},
    KeyContextInfo{KeyContext::Filter, "on the filter display"},
    KeyContextInfo{KeyContext::Overlay, "in the palette and the key map"},
    KeyContextInfo{KeyContext::Memories, "in the frequency manager"},
};

[[nodiscard]] constexpr std::string_view key_context_name(KeyContext context)
{
    for (const KeyContextInfo& info : kKeyContexts) {
        if (info.context == context) {
            return info.name;
        }
    }
    return {};
}

// "Ctrl+K or Ctrl+Shift+P", or empty for an action with no key.
[[nodiscard]] inline std::string key_text(const KeyAction& action)
{
    std::string out;
    for (const std::string_view key : action.keys) {
        if (key.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += " or ";
        }
        out += key;
    }
    return out;
}

// Anything true of every key in a context, for the key map to say once above
// them. Built from the table and the step constants rather than written out,
// so the sentence names the key that is bound and the step that is taken.
[[nodiscard]] inline std::string key_context_note(KeyContext context)
{
    switch (context) {
        case KeyContext::Window:
            return "In either window, except that a text field keeps the keys it types with.";
        case KeyContext::Filter: {
            const KeyAction* focus = find_key_action("filter.keys");
            return "After a click on the display, or " +
                   (focus != nullptr ? key_text(*focus) : std::string("its key")) +
                   " from anywhere. A step is " + std::to_string(kFilterKeyStepHz) + " Hz, " +
                   std::to_string(kFilterKeyCoarseStepHz) + " Hz with Shift and " +
                   std::to_string(kFilterKeyFineStepHz) + " Hz with Ctrl.";
        }
        case KeyContext::Overlay:
            return "While the command palette or the key map is open.";
        case KeyContext::Memories:
            return "While the frequency manager is open. Its search field keeps Del and Ctrl+Z "
                   "for its own text, and Down moves from the field into the list.";
    }
    return {};
}

// A need with what it stands on: a radio that retunes is an open radio, an
// open radio is on an engine, AFT is a receiver's. Written into the table
// once each, and expanded here, so a row cannot name the top of a chain and
// be enabled with the bottom missing.
[[nodiscard]] constexpr std::uint32_t key_needs_expanded(std::uint32_t needs)
{
    if ((needs & (kNeedsAft | kNeedsAutoFilter | kNeedsSecondReceiver | kNeedsNoise |
                  kNeedsNotch | kNeedsAutoNotch)) != 0) {
        needs |= kNeedsReceiver;
    }
    if ((needs & (kNeedsRetune | kNeedsSpectrum)) != 0) {
        needs |= kNeedsSource;
    }
    if ((needs & (kNeedsSource | kNeedsReceiver)) != 0) {
        needs |= kNeedsEngine;
    }
    return needs;
}

// What an action needs and the window lacks.
[[nodiscard]] constexpr std::uint32_t key_missing(std::uint32_t needs, std::uint32_t have)
{
    return key_needs_expanded(needs) & ~have;
}

[[nodiscard]] constexpr bool key_action_enabled(const KeyAction& action, std::uint32_t have)
{
    return key_missing(action.needs, have) == 0;
}

// What the window has, by name, turned into the bits the table's needs are
// written in. Each flag is derived from the one before where it has to be: a
// receiver's AFT is not offered without the receiver.
struct KeyState {
    bool connected = false;
    bool source_open = false;
    bool can_retune = false;
    bool receiver = false;
    bool second_receiver = false;
    bool aft_offered = false;
    bool auto_filter_offered = false;
    bool spectrum_drawing = false;
    bool noise_offered = false;
    bool notch_offered = false;
    bool auto_notch_offered = false;
};

[[nodiscard]] constexpr std::uint32_t key_have(const KeyState& state)
{
    std::uint32_t have = kNeedsNothing;
    if (!state.connected) {
        return have;
    }
    have |= kNeedsEngine;
    if (state.source_open) {
        have |= kNeedsSource;
        if (state.can_retune) {
            have |= kNeedsRetune;
        }
        if (state.spectrum_drawing) {
            have |= kNeedsSpectrum;
        }
    }
    if (state.receiver) {
        have |= kNeedsReceiver;
        if (state.second_receiver) {
            have |= kNeedsSecondReceiver;
        }
        if (state.aft_offered) {
            have |= kNeedsAft;
        }
        if (state.auto_filter_offered) {
            have |= kNeedsAutoFilter;
        }
        if (state.noise_offered) {
            have |= kNeedsNoise;
        }
        if (state.notch_offered) {
            have |= kNeedsNotch;
        }
        if (state.auto_notch_offered) {
            have |= kNeedsAutoNotch;
        }
    }
    return have;
}

// Why an action cannot run, from the bits it lacks, in the order an operator
// would have to fix them. Empty when it lacks nothing.
[[nodiscard]] constexpr std::string_view key_needs_text(std::uint32_t missing)
{
    if ((missing & kNeedsEngine) != 0) {
        return "needs an engine";
    }
    if ((missing & kNeedsSource) != 0) {
        return "needs an open radio";
    }
    if ((missing & kNeedsRetune) != 0) {
        return "this radio does not retune";
    }
    if ((missing & kNeedsSpectrum) != 0) {
        return "needs the spectrum";
    }
    if ((missing & kNeedsReceiver) != 0) {
        return "needs a receiver";
    }
    if ((missing & kNeedsSecondReceiver) != 0) {
        return "needs a second receiver";
    }
    if ((missing & (kNeedsAft | kNeedsAutoFilter | kNeedsNoise | kNeedsNotch |
                    kNeedsAutoNotch)) != 0) {
        return "not offered on this mode";
    }
    return {};
}

// ---------------------------------------------------------------------------
// Key sequences
// ---------------------------------------------------------------------------

namespace key_detail {

[[nodiscard]] constexpr char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] constexpr bool starts_with_nocase(std::string_view text, std::string_view prefix)
{
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (lower(text[i]) != lower(prefix[i])) {
            return false;
        }
    }
    return true;
}

inline constexpr std::array<std::string_view, 4> kModifiers = {"Ctrl", "Alt", "Shift", "Meta"};

// Spellings Qt accepts for one key, mapped to the one its portable text
// writes, so "Escape" and "Esc" are one key when two actions are compared.
struct KeyAlias {
    std::string_view from;
    std::string_view to;
};

inline constexpr std::array kKeyAliases{
    KeyAlias{"escape", "esc"},  KeyAlias{"delete", "del"},      KeyAlias{"pageup", "pgup"},
    KeyAlias{"pagedown", "pgdown"}, KeyAlias{"insert", "ins"},
};

}  // namespace key_detail

// A key sequence split into its modifiers and its key.
struct KeyChord {
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool meta = false;
    std::string key;

    friend bool operator==(const KeyChord&, const KeyChord&) = default;
};

// Reads "Ctrl+Shift+P" into a chord. Modifiers are recognised in any order
// and any case; the key is what is left, lower-cased and with Qt's alternative
// spellings folded to one, so that two sequences naming the same keys compare
// equal. "Ctrl++" is Ctrl and the plus key. Empty text is an empty chord.
[[nodiscard]] inline KeyChord parse_key_chord(std::string_view text)
{
    KeyChord chord;
    bool matched = true;
    while (matched && !text.empty()) {
        matched = false;
        for (const std::string_view modifier : key_detail::kModifiers) {
            if (text.size() > modifier.size() + 1 &&
                key_detail::starts_with_nocase(text, modifier) &&
                text[modifier.size()] == '+') {
                if (modifier == "Ctrl") {
                    chord.ctrl = true;
                } else if (modifier == "Alt") {
                    chord.alt = true;
                } else if (modifier == "Shift") {
                    chord.shift = true;
                } else {
                    chord.meta = true;
                }
                text.remove_prefix(modifier.size() + 1);
                matched = true;
                break;
            }
        }
    }
    for (const char c : text) {
        chord.key.push_back(key_detail::lower(c));
    }
    for (const key_detail::KeyAlias& alias : key_detail::kKeyAliases) {
        if (chord.key == alias.from) {
            chord.key = std::string(alias.to);
        }
    }
    return chord;
}

// Whether text is written the way this table writes sequences: modifiers
// first, each once, in the order Ctrl, Alt, Shift, Meta. The table is held to
// this so what the palette prints is what a reader would type.
[[nodiscard]] inline bool key_sequence_canonical(std::string_view text)
{
    if (text.empty()) {
        return false;
    }
    std::size_t next_modifier = 0;
    while (true) {
        bool found = false;
        for (std::size_t m = 0; m < key_detail::kModifiers.size(); ++m) {
            const std::string_view modifier = key_detail::kModifiers[m];
            if (text.size() > modifier.size() + 1 && text.starts_with(modifier) &&
                text[modifier.size()] == '+') {
                if (m < next_modifier) {
                    return false;
                }
                next_modifier = m + 1;
                text.remove_prefix(modifier.size() + 1);
                found = true;
                break;
            }
        }
        if (!found) {
            break;
        }
    }
    // What is left is the key alone, and it is not itself a modifier's name.
    for (const std::string_view modifier : key_detail::kModifiers) {
        if (key_detail::starts_with_nocase(text, modifier) && text.size() == modifier.size()) {
            return false;
        }
    }
    return !text.empty();
}

// How a filter key's step is scaled by the modifier held with it.
enum class KeyScale {
    Normal,
    Coarse,
    Fine,
};

struct KeyHit {
    const KeyAction* action = nullptr;
    KeyScale scale = KeyScale::Normal;
};

// The action a chord runs in a context, or none.
//
// An exact match first. Failing that, a scaled action whose key is pressed
// with Shift, Ctrl or both and nothing else, which is how the filter display
// has always read its arrows: Shift is the coarse step and wins over Ctrl
// when both are held.
[[nodiscard]] inline KeyHit find_key(KeyContext context, const KeyChord& pressed)
{
    for (const KeyAction& action : kKeyActions) {
        if (action.context != context) {
            continue;
        }
        for (const std::string_view key : action.keys) {
            if (!key.empty() && parse_key_chord(key) == pressed) {
                return {&action, KeyScale::Normal};
            }
        }
    }
    if (pressed.alt || pressed.meta || !(pressed.shift || pressed.ctrl)) {
        return {};
    }
    KeyChord bare = pressed;
    bare.shift = false;
    bare.ctrl = false;
    for (const KeyAction& action : kKeyActions) {
        if (action.context != context || !action.scaled) {
            continue;
        }
        for (const std::string_view key : action.keys) {
            if (!key.empty() && parse_key_chord(key) == bare) {
                return {&action, pressed.shift ? KeyScale::Coarse : KeyScale::Fine};
            }
        }
    }
    return {};
}

[[nodiscard]] inline KeyHit find_key(KeyContext context, std::string_view sequence)
{
    return find_key(context, parse_key_chord(sequence));
}

// Every chord an action answers to, including the Shift and Ctrl variants of
// a scaled one. The conflict tests compare these.
[[nodiscard]] inline std::vector<KeyChord> key_chords(const KeyAction& action)
{
    std::vector<KeyChord> out;
    for (const std::string_view key : action.keys) {
        if (key.empty()) {
            continue;
        }
        const KeyChord chord = parse_key_chord(key);
        out.push_back(chord);
        if (action.scaled) {
            for (int variant = 1; variant < 4; ++variant) {
                KeyChord scaled = chord;
                scaled.shift = (variant & 1) != 0;
                scaled.ctrl = (variant & 2) != 0;
                out.push_back(scaled);
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// The two handlers that are not in QML
// ---------------------------------------------------------------------------

enum class PassbandCommand {
    SelectLow,
    SelectHigh,
    SelectBoth,
    MoveDown,
    MoveUp,
    Widen,
    Narrow,
    Reset,
    Cancel,
};

// The filter display's command for one of its actions. render/
// passband_item.cpp switches on the answer, with no default, so a command
// added here and not handled there is a warning the CI build stops on.
template <typename Command>
struct NamedCommand {
    std::string_view id;
    Command command;
};

inline constexpr std::array kPassbandCommands{
    NamedCommand<PassbandCommand>{"edge.low", PassbandCommand::SelectLow},
    NamedCommand<PassbandCommand>{"edge.high", PassbandCommand::SelectHigh},
    NamedCommand<PassbandCommand>{"edge.both", PassbandCommand::SelectBoth},
    NamedCommand<PassbandCommand>{"edge.down", PassbandCommand::MoveDown},
    NamedCommand<PassbandCommand>{"edge.up", PassbandCommand::MoveUp},
    NamedCommand<PassbandCommand>{"edge.widen", PassbandCommand::Widen},
    NamedCommand<PassbandCommand>{"edge.narrow", PassbandCommand::Narrow},
    NamedCommand<PassbandCommand>{"edge.default", PassbandCommand::Reset},
    NamedCommand<PassbandCommand>{"edge.cancel", PassbandCommand::Cancel},
};

// The filter display's command for one of its actions. render/
// passband_item.cpp switches on the answer with no default, so a command
// added here and not handled there is a warning the CI build stops on.
[[nodiscard]] constexpr std::optional<PassbandCommand> passband_command(std::string_view id)
{
    for (const auto& named : kPassbandCommands) {
        if (named.id == id) {
            return named.command;
        }
    }
    return std::nullopt;
}

enum class OverlayCommand {
    Next,
    Previous,
    Run,
    Close,
};

inline constexpr std::array kOverlayCommands{
    NamedCommand<OverlayCommand>{"overlay.next", OverlayCommand::Next},
    NamedCommand<OverlayCommand>{"overlay.previous", OverlayCommand::Previous},
    NamedCommand<OverlayCommand>{"overlay.run", OverlayCommand::Run},
    NamedCommand<OverlayCommand>{"overlay.close", OverlayCommand::Close},
};

[[nodiscard]] constexpr std::optional<OverlayCommand> overlay_command(std::string_view id)
{
    for (const auto& named : kOverlayCommands) {
        if (named.id == id) {
            return named.command;
        }
    }
    return std::nullopt;
}

enum class MemoryCommand {
    Next,
    Previous,
    Recall,
    RecallNew,
    Edit,
    Delete,
    Undo,
    Search,
    Close,
};

inline constexpr std::array kMemoryCommands{
    NamedCommand<MemoryCommand>{"memories.next", MemoryCommand::Next},
    NamedCommand<MemoryCommand>{"memories.previous", MemoryCommand::Previous},
    NamedCommand<MemoryCommand>{"memories.recall", MemoryCommand::Recall},
    NamedCommand<MemoryCommand>{"memories.recall_new", MemoryCommand::RecallNew},
    NamedCommand<MemoryCommand>{"memories.edit", MemoryCommand::Edit},
    NamedCommand<MemoryCommand>{"memories.delete", MemoryCommand::Delete},
    NamedCommand<MemoryCommand>{"memories.undo", MemoryCommand::Undo},
    NamedCommand<MemoryCommand>{"memories.search", MemoryCommand::Search},
    NamedCommand<MemoryCommand>{"memories.close", MemoryCommand::Close},
};

// The frequency manager's command for one of its actions. models/key_map.h
// switches on the answer with no default, so a command added here and not
// named there is a warning the CI build stops on.
[[nodiscard]] constexpr std::optional<MemoryCommand> memory_command(std::string_view id)
{
    for (const auto& named : kMemoryCommands) {
        if (named.id == id) {
            return named.command;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// The key map, as the documentation prints it
// ---------------------------------------------------------------------------

// The table docs/ui-spectrum.md carries under "Keys", one row per action that
// has a key, context by context in the table's order. The test compares the
// document against this, character for character, and prints this on a
// mismatch so the fix is a paste.
[[nodiscard]] inline std::string render_key_map_markdown()
{
    std::string out;
    for (const KeyContextInfo& info : kKeyContexts) {
        out += "**";
        out += info.name;
        out += ".** ";
        out += key_context_note(info.context);
        out += "\n\n| Group | Action | Keys |\n| --- | --- | --- |\n";
        for (const KeyAction& action : kKeyActions) {
            if (action.context != info.context || action.keys[0].empty()) {
                continue;
            }
            out += "| ";
            out += action.group;
            out += " | ";
            out += action.label;
            out += " | ";
            bool first = true;
            for (const std::string_view key : action.keys) {
                if (key.empty()) {
                    continue;
                }
                if (!first) {
                    out += ", ";
                }
                out += '`';
                out += key;
                out += '`';
                first = false;
            }
            out += " |\n";
        }
        out += '\n';
    }
    return out;
}

}  // namespace revenant::ui
