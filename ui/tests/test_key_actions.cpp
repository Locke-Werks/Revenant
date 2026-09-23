// The keyboard table in models/key_actions.h: that it is well formed, that no
// two actions fight over a key, that the filter display's old keys survived
// the move into it, and that the key map in docs/ui-spectrum.md is the one the
// table prints.
//
// EVERY CASE NAMES THE WRONG TABLE IT REJECTS, on the rule the rest of
// ui/tests follows. A keyboard table goes wrong quietly: two shortcuts on one
// key are ambiguous, and Qt answers an ambiguous shortcut by running neither,
// so the key looks dead rather than wrong. A window key that the filter
// display also takes works until the display has focus and then stops. A
// document that lists a key the table no longer binds teaches a key that does
// nothing.
//
// WHAT IS NOT HERE. That ui/qml/Commands.qml has a handler for every handler
// named here: that table is QML, and a smoke run of revenant-ui asks it and
// fails on a gap. That a Shortcut built from this table fires: that is Qt's.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "models/key_actions.h"
#include "models/mode_choice.h"

using revenant::ui::find_key;
using revenant::ui::find_key_action;
using revenant::ui::key_action_enabled;
using revenant::ui::key_chords;
using revenant::ui::key_have;
using revenant::ui::key_missing;
using revenant::ui::key_needs_text;
using revenant::ui::key_sequence_canonical;
using revenant::ui::KeyAction;
using revenant::ui::KeyChord;
using revenant::ui::KeyContext;
using revenant::ui::KeyScale;
using revenant::ui::KeyState;
using revenant::ui::kKeyActions;
using revenant::ui::kModeChoices;
using revenant::ui::memory_command;
using revenant::ui::MemoryCommand;
using revenant::ui::overlay_command;
using revenant::ui::parse_key_chord;
using revenant::ui::passband_command;
using revenant::ui::render_key_map_markdown;

namespace {

[[nodiscard]] std::string chord_text(const KeyChord& chord)
{
    std::string out;
    out += chord.ctrl ? "Ctrl+" : "";
    out += chord.alt ? "Alt+" : "";
    out += chord.shift ? "Shift+" : "";
    out += chord.meta ? "Meta+" : "";
    return out + chord.key;
}

}  // namespace

// Rejects a row added in a hurry: no label leaves a blank line in the palette,
// no group leaves the key map with an entry under no heading, and a repeated
// id makes find_key_action answer for the first and hide the second.
TEST_CASE("every action has an id of its own, a label and a group")
{
    std::set<std::string_view> ids;
    for (const KeyAction& action : kKeyActions) {
        INFO(action.id);
        CHECK_FALSE(action.id.empty());
        CHECK_FALSE(action.label.empty());
        CHECK_FALSE(action.group.empty());
        CHECK(ids.insert(action.id).second);
    }
}

// Rejects a key written two ways. The palette prints the table's text, so a
// row reading "Shift+Ctrl+P" would print a sequence nobody else in the window
// writes, and a second key in slot two with the first empty would be bound
// and never printed.
TEST_CASE("every key is written modifiers first, in one order")
{
    for (const KeyAction& action : kKeyActions) {
        INFO(action.id);
        for (const std::string_view key : action.keys) {
            if (!key.empty()) {
                CHECK(key_sequence_canonical(key));
            }
        }
        if (action.keys[0].empty()) {
            CHECK(action.keys[1].empty());
        }
    }
    CHECK(key_sequence_canonical("Ctrl+Shift+P"));
    CHECK_FALSE(key_sequence_canonical("Shift+Ctrl+P"));
    CHECK_FALSE(key_sequence_canonical("Ctrl+Ctrl+P"));
    CHECK_FALSE(key_sequence_canonical("Ctrl"));
    CHECK(key_sequence_canonical("Ctrl+-"));
    CHECK(key_sequence_canonical("\\"));
}

// Rejects a comparison by string. "Esc" and "Escape" are one key to Qt, and so
// are "ctrl+k" and "Ctrl+K"; a conflict check that compared text would pass a
// table that binds one key twice under two spellings.
TEST_CASE("sequences are compared as keys, not as text")
{
    CHECK(parse_key_chord("Escape") == parse_key_chord("Esc"));
    CHECK(parse_key_chord("ctrl+k") == parse_key_chord("Ctrl+K"));
    CHECK(parse_key_chord("Shift+Ctrl+P") == parse_key_chord("Ctrl+Shift+P"));
    CHECK(parse_key_chord("PageUp") == parse_key_chord("PgUp"));
    CHECK_FALSE(parse_key_chord("Ctrl+K") == parse_key_chord("Ctrl+Shift+K"));

    const KeyChord plus = parse_key_chord("Ctrl++");
    CHECK(plus.ctrl);
    CHECK(plus.key == "+");
    CHECK(parse_key_chord("Ctrl+-").key == "-");
}

// THE CONFLICT TEST. Rejects two actions on one key in one context. Qt runs
// neither of two enabled shortcuts on one sequence and reports the key as
// ambiguous only to a signal nobody here connects, so the symptom is a dead
// key. Every chord an action answers to is counted, the Shift and Ctrl
// variants of a scaled one included, and whatever the actions' enabled rules:
// two rules that are never true together today are one change away from
// being true together.
TEST_CASE("no two actions share a key in one context")
{
    for (const KeyContext context : {KeyContext::Window, KeyContext::Filter,
                                     KeyContext::Overlay, KeyContext::Memories}) {
        std::vector<std::pair<std::string, std::string_view>> seen;
        for (const KeyAction& action : kKeyActions) {
            if (action.context != context) {
                continue;
            }
            for (const KeyChord& chord : key_chords(action)) {
                const std::string text = chord_text(chord);
                for (const auto& [other_text, other_id] : seen) {
                    INFO(action.id << " and " << other_id << " both take " << text);
                    CHECK(other_text != text);
                }
                seen.emplace_back(text, action.id);
            }
        }
    }
}

// Rejects a window key that the filter display, the overlay or the frequency
// manager also takes. Those see a key before the window's shortcuts do, so
// such a key would work from everywhere except the place the operator
// happened to be, and which place that is changes with every click.
TEST_CASE("no window key is shadowed by the filter display, the overlay or the manager")
{
    for (const KeyAction& window : kKeyActions) {
        if (window.context != KeyContext::Window) {
            continue;
        }
        for (const KeyChord& chord : key_chords(window)) {
            INFO(window.id << " on " << chord_text(chord));
            CHECK(find_key(KeyContext::Filter, chord).action == nullptr);
            CHECK(find_key(KeyContext::Overlay, chord).action == nullptr);
            CHECK(find_key(KeyContext::Memories, chord).action == nullptr);
        }
    }
}

// Rejects the move into the table changing what the filter display's keys do.
// These are the keys docs/ui-spectrum.md listed before the table existed, and
// the steps render/passband_item.cpp took with each modifier.
TEST_CASE("the filter display keeps its keys and their steps")
{
    const auto id_of = [](std::string_view sequence) {
        const auto hit = find_key(KeyContext::Filter, sequence);
        return hit.action != nullptr ? hit.action->id : std::string_view();
    };
    CHECK(id_of("[") == "edge.low");
    CHECK(id_of("]") == "edge.high");
    CHECK(id_of("\\") == "edge.both");
    CHECK(id_of("Left") == "edge.down");
    CHECK(id_of("Right") == "edge.up");
    CHECK(id_of("Up") == "edge.widen");
    CHECK(id_of("Down") == "edge.narrow");
    CHECK(id_of("Home") == "edge.default");
    CHECK(id_of("Esc") == "edge.cancel");

    CHECK(find_key(KeyContext::Filter, "Left").scale == KeyScale::Normal);
    CHECK(find_key(KeyContext::Filter, "Shift+Left").scale == KeyScale::Coarse);
    CHECK(find_key(KeyContext::Filter, "Ctrl+Left").scale == KeyScale::Fine);

    // Shift wins when both are held, as it did in the switch this replaced.
    CHECK(find_key(KeyContext::Filter, "Ctrl+Shift+Up").scale == KeyScale::Coarse);

    // Alt is not a step size, so an arrow with Alt is not the display's. The
    // window's tuning keys are Alt and the arrows, and would otherwise stop
    // working whenever the display had focus.
    CHECK(find_key(KeyContext::Filter, "Alt+Left").action == nullptr);

    // An unscaled key with a modifier is a different key. Ctrl+[ is the
    // window's floor pin and must not select the low edge.
    CHECK(find_key(KeyContext::Filter, "Ctrl+[").action == nullptr);
}

// Rejects a filter or overlay row with no handler. The display and the overlay
// switch on these commands, so a row with no command is a key the key map
// prints and nothing runs.
TEST_CASE("every filter and overlay action has its command")
{
    for (const KeyAction& action : kKeyActions) {
        INFO(action.id);
        switch (action.context) {
            case KeyContext::Window: CHECK_FALSE(action.handler.empty()); break;
            case KeyContext::Filter: CHECK(passband_command(action.id).has_value()); break;
            case KeyContext::Overlay: CHECK(overlay_command(action.id).has_value()); break;
            case KeyContext::Memories: CHECK(memory_command(action.id).has_value()); break;
        }
    }
}

// Rejects the mode keys drifting from the selector. Alt and a number is a
// place on the row the receiver window draws, and a table that numbered them
// in its own order would put Alt+4 on a mode that is fifth on screen.
TEST_CASE("the mode keys follow the selector's order")
{
    int row_place = 0;
    for (const auto& choice : kModeChoices) {
        const KeyAction* action = find_key_action("mode." + std::string(choice.name));
        INFO(choice.name);
        REQUIRE(action != nullptr);
        CHECK(action->argument == choice.name);
        CHECK(action->handler == "receiver.mode");
        if (choice.digital) {
            CHECK(action->keys[0].empty());
        } else {
            ++row_place;
            CHECK(action->keys[0] == "Alt+" + std::to_string(row_place));
        }
    }
}

// Rejects the palette losing either of the keys it opens on. Ctrl+K and
// Ctrl+Shift+P are the two an operator arriving from another tool already
// has in their hands, and the palette is where every other key is found.
TEST_CASE("the palette opens on both keys and the key map on one")
{
    const KeyAction* palette = find_key_action("palette.open");
    REQUIRE(palette != nullptr);
    CHECK(palette->keys[0] == "Ctrl+K");
    CHECK(palette->keys[1] == "Ctrl+Shift+P");
    CHECK(find_key(KeyContext::Window, "Ctrl+Shift+P").action == palette);
    CHECK(find_key(KeyContext::Window, "F1").action == find_key_action("keymap.open"));
}

// Rejects a rule that lets an action run without what it acts on, which in
// the window is a key that throws a warning into the log or, worse, tunes a
// source that cannot retune and puts its refusal in the banner.
TEST_CASE("an action is enabled only when the window has what it needs")
{
    KeyState state;
    const auto have = [&] { return key_have(state); };
    const KeyAction& tune = *find_key_action("tune.up");
    const KeyAction& mute = *find_key_action("audio.mute");
    const KeyAction& aft = *find_key_action("receiver.aft");

    CHECK(key_action_enabled(mute, have()));
    CHECK_FALSE(key_action_enabled(tune, have()));
    CHECK(key_needs_text(key_missing(tune.needs, have())) == "needs an engine");

    state.connected = true;
    CHECK(key_needs_text(key_missing(tune.needs, have())) == "needs an open radio");
    state.source_open = true;
    CHECK_FALSE(key_action_enabled(tune, have()));
    CHECK(key_needs_text(key_missing(tune.needs, have())) == "this radio does not retune");
    state.can_retune = true;
    CHECK(key_action_enabled(tune, have()));

    // AFT offered by a receiver that is not there is not offered.
    state.aft_offered = true;
    CHECK_FALSE(key_action_enabled(aft, have()));
    state.receiver = true;
    CHECK(key_action_enabled(aft, have()));

    // And nothing counts without an engine, whatever the rest says.
    state.connected = false;
    CHECK_FALSE(key_action_enabled(aft, have()));
}

// Rejects next and previous offered with nothing to move to, which is a key
// that does nothing, and a second receiver counted without a first.
// Rejects a noise key enabled on a mode that does not offer its stage, which
// is a key that does nothing, and one offered with no receiver to act on.
TEST_CASE("each noise key needs a receiver whose mode offers its stage")
{
    KeyState state;
    state.connected = true;
    state.source_open = true;
    const auto have = [&] { return key_have(state); };
    const KeyAction& blanker = *find_key_action("noise.blanker");
    const KeyAction& notch = *find_key_action("noise.notch");
    const KeyAction& auto_notch = *find_key_action("noise.auto_notch");
    const KeyAction& reduction = *find_key_action("noise.reduction");

    // Offered with no receiver is not offered.
    state.noise_offered = true;
    state.notch_offered = true;
    state.auto_notch_offered = true;
    CHECK_FALSE(key_action_enabled(blanker, have()));
    CHECK(key_needs_text(key_missing(blanker.needs, have())) == "needs a receiver");

    // A CW receiver: the blanker, the notch and noise reduction, and not the
    // automatic notch.
    state.receiver = true;
    state.auto_notch_offered = false;
    CHECK(key_action_enabled(blanker, have()));
    CHECK(key_action_enabled(notch, have()));
    CHECK(key_action_enabled(reduction, have()));
    CHECK_FALSE(key_action_enabled(auto_notch, have()));
    CHECK(key_needs_text(key_missing(auto_notch.needs, have())) == "not offered on this mode");

    // A complex tap: none of them.
    state.noise_offered = false;
    state.notch_offered = false;
    CHECK_FALSE(key_action_enabled(blanker, have()));
    CHECK_FALSE(key_action_enabled(notch, have()));
    CHECK_FALSE(key_action_enabled(reduction, have()));
}

TEST_CASE("moving the focus needs a second receiver in the rack")
{
    KeyState state;
    state.connected = true;
    state.source_open = true;
    const auto have = [&] { return key_have(state); };
    const KeyAction& next = *find_key_action("receiver.next");
    const KeyAction& previous = *find_key_action("receiver.previous");
    const KeyAction& solo = *find_key_action("receiver.solo");
    const KeyAction& add = *find_key_action("receiver.add");

    CHECK(key_action_enabled(add, have()));
    CHECK_FALSE(key_action_enabled(solo, have()));

    state.receiver = true;
    CHECK(key_action_enabled(solo, have()));
    CHECK_FALSE(key_action_enabled(next, have()));
    CHECK(key_needs_text(key_missing(next.needs, have())) == "needs a second receiver");

    state.second_receiver = true;
    CHECK(key_action_enabled(next, have()));
    CHECK(key_action_enabled(previous, have()));

    state.receiver = false;
    CHECK_FALSE(key_action_enabled(next, have()));
    CHECK(key_needs_text(key_missing(next.needs, have())) == "needs a receiver");
}

// THE DOCUMENT CHECK. Rejects docs/ui-spectrum.md listing keys the table does
// not bind. The key map there is the text render_key_map_markdown prints,
// between two marker lines, and on a mismatch this prints what belongs there
// so the fix is a paste.
TEST_CASE("the key map in docs/ui-spectrum.md is the table's")
{
    std::ifstream file(REVENANT_DOCS_DIR "/ui-spectrum.md", std::ios::binary);
    REQUIRE(file.good());
    std::string doc((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::erase(doc, '\r');

    const std::string begin_marker =
        "<!-- The key map. Generated from ui/models/key_actions.h and checked by "
        "ui/tests/test_key_actions.cpp: edit the table, not these lines. -->\n\n";
    const std::string end_marker = "<!-- End of the key map. -->";
    const std::size_t begin = doc.find(begin_marker);
    const std::size_t end = doc.find(end_marker);
    REQUIRE(begin != std::string::npos);
    REQUIRE(end != std::string::npos);
    REQUIRE(end > begin);

    const std::string written = doc.substr(begin + begin_marker.size(),
                                           end - begin - begin_marker.size());
    const std::string expected = render_key_map_markdown();

    // Catch wraps a long message, which would make the printed table wrong
    // to paste, so on a mismatch the table goes to a file beside the test's
    // working directory as well.
    if (written != expected) {
        std::ofstream out("key_map_expected.md", std::ios::binary);
        out << expected;
    }
    INFO("docs/ui-spectrum.md should carry, between the markers, what this "
         "test wrote to key_map_expected.md in its working directory");
    CHECK(written == expected);
}

// Rejects the frequency manager's keys drifting from what its panel does with
// them: Return recalls into the focused receiver and Shift+Return into a new
// one, which is the one pair an operator has to be able to tell apart without
// looking, and Ctrl+Z is the undo a delete promises.
TEST_CASE("the frequency manager's keys name the commands its panel runs")
{
    const auto command = [](std::string_view sequence) -> std::optional<MemoryCommand> {
        const auto hit = find_key(KeyContext::Memories, sequence);
        return hit.action != nullptr ? memory_command(hit.action->id) : std::nullopt;
    };
    CHECK(command("Return") == MemoryCommand::Recall);
    CHECK(command("Enter") == MemoryCommand::Recall);
    CHECK(command("Shift+Return") == MemoryCommand::RecallNew);
    CHECK(command("Del") == MemoryCommand::Delete);
    CHECK(command("Ctrl+Z") == MemoryCommand::Undo);
    CHECK(command("Esc") == MemoryCommand::Close);
    CHECK_FALSE(command("Ctrl+Del").has_value());

    // The window keys that open the manager and save into it.
    CHECK(find_key(KeyContext::Window, "Ctrl+B").action == find_key_action("panel.memories"));
    CHECK(find_key(KeyContext::Window, "Ctrl+D").action == find_key_action("memory.save"));
}
