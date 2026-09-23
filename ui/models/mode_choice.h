// The receiver modes this window offers: their names, how the mode selector
// labels and groups them, and which of them the audio section applies to.
//
// Qt-free so ui/tests can hold it, on the rule models/decoded_log.h follows.
// It includes core/rpc/types.h and nothing else of the tree. EngineLink turns
// names into rpc::Demod through kDemodNames, and UiRules hands the selector
// its rows and labels.
//
// ELEVEN MODES, TWO ROWS OF THEM. The selector shows the eight an operator
// reaches for on every band as one row of segments, and puts P25, D-STAR and
// TETRA behind a ninth segment that opens a short list. All eleven are
// demodulators the engine plans, and the three digital ones reach the
// decoders through the fine stage at the rate each was written for, so a raw
// receiver is no longer the only way to them. Grouped rather than added to
// the row, which it shares with the width controls and the level readout:
// eight segments is what it held before, and one more for the group keeps it
// close to that.
//
// WHAT THIS WINDOW USED TO OFFER: the row alone, "am, nfm, wfm, usb, lsb,
// dsb, cw, raw", and a name table in models/engine_link.h that held those
// eight, so a p25p1 receiver made elsewhere read "unknown" here and the
// decode section never offered it p25p1.

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/rpc/types.h"

namespace revenant::ui {

// engine::demod_name's spellings in rpc::Demod's ordinal order, so the table
// and the enum cannot drift the way two hand-written lists would.
//
// A copy on purpose: this process links no part of the engine, which is the
// whole reason ui/CMakeLists.txt exists. core/rpc/convert.h holds the
// static_asserts that keep rpc::Demod ordinal for ordinal with engine::Demod,
// so the ORDINALS here are pinned by the engine's own build even though the
// spellings are not. ui/tests/test_mode_choice.cpp pins each spelling to its
// enumerator.
inline constexpr std::array<std::string_view, 11> kDemodNames = {
    "raw", "am", "nfm", "wfm", "usb", "lsb", "dsb", "cw", "p25p1", "dstar", "tetra"};

static_assert(kDemodNames.size() == static_cast<std::size_t>(rpc::Demod::Tetra) + 1,
              "a demodulator was added to rpc::Demod without a name here");

struct ModeChoice {
    std::string_view name;
    std::string_view label;
    bool digital = false;
};

// In the order the selector draws them. The row keeps the engine's lower-case
// names, which is how it has always read; the digital three are known on the
// air by these spellings and not by the engine's.
inline constexpr std::array<ModeChoice, 11> kModeChoices = {{
    {"am", "am", false},
    {"nfm", "nfm", false},
    {"wfm", "wfm", false},
    {"usb", "usb", false},
    {"lsb", "lsb", false},
    {"dsb", "dsb", false},
    {"cw", "cw", false},
    {"raw", "raw", false},
    {"p25p1", "P25", true},
    {"dstar", "D-STAR", true},
    {"tetra", "TETRA", true},
}};

// What the digital segment reads while the receiver is in none of them.
inline constexpr std::string_view kDigitalGroupLabel = "digital";

[[nodiscard]] constexpr const ModeChoice* find_mode_choice(std::string_view name)
{
    for (const ModeChoice& choice : kModeChoices) {
        if (choice.name == name) {
            return &choice;
        }
    }
    return nullptr;
}

// The label a mode is shown by, or its name when this window has no label
// for it, so a mode from a newer engine still reads as something.
[[nodiscard]] constexpr std::string_view mode_label(std::string_view name)
{
    const ModeChoice* choice = find_mode_choice(name);
    return choice != nullptr ? choice->label : name;
}

[[nodiscard]] constexpr bool mode_is_digital(std::string_view name)
{
    const ModeChoice* choice = find_mode_choice(name);
    return choice != nullptr && choice->digital;
}

// The digital segment's text: the mode in force when it is one of the three,
// so the operator sees which, and the group's name otherwise.
[[nodiscard]] constexpr std::string_view digital_group_label(std::string_view current)
{
    return mode_is_digital(current) ? mode_label(current) : kDigitalGroupLabel;
}

// The names on the row, or behind the digital segment, in drawing order.
[[nodiscard]] inline std::vector<std::string_view> mode_names(bool digital)
{
    std::vector<std::string_view> out;
    for (const ModeChoice& choice : kModeChoices) {
        if (choice.digital == digital) {
            out.push_back(choice.name);
        }
    }
    return out;
}

// Whether a receiver in this mode makes audio, which is whether the audio
// section applies to it at all.
//
// engine::produces_audio's seven analogue modes, and p25p1. raw, dstar and
// tetra hand out complex baseband, two floats a sample, for a decoder to
// read, and the engine refuses an audio subscription on one. So the window
// neither asks nor shows a section that could only say it was refused. A
// p25p1 receiver's audio is its decoded IMBE voice, which subscribeAudio
// serves since 2026-09-23 on the owner's decision that a digital voice
// receiver plays its voice; core/rpc/voice_audio.h has it.
//
// WHAT THIS PARAGRAPH USED TO SAY: "Whether P25 voice should ever come out
// of this section is an open decision, and it is the engine's refusal that
// would change, not this table first." The refusal changed first.
//
// A name this window does not know is taken as making none, for the reason
// demod_from_name gives for refusing one: a guess here would subscribe to a
// stream nobody described.
[[nodiscard]] constexpr bool mode_makes_audio(std::string_view name)
{
    return name == "am" || name == "nfm" || name == "wfm" || name == "usb" || name == "lsb" ||
           name == "dsb" || name == "cw" || name == "p25p1";
}

// Whether a receiver in this mode hands out audio at the level its signal
// came in at, so the mix has to level it before anybody can hear it.
//
// An envelope detector and the four product detectors produce audio in the
// input's own units, and the engine applies no AGC: measured on 2026-09-23,
// the am, usb, lsb, dsb and cw receivers in tests/rpc's harness peaked at
// 3e-7 to 2e-6 on signals 30 to 40 dB over a -100 dBFS floor, where nfm and
// wfm peaked at 10.0 and 4.6. A discriminator is level-independent and the
// vocoder writes full scale, so neither needs it. audio/mix_stages.h's
// LevelAgc is what does the levelling.
[[nodiscard]] constexpr bool mode_needs_level(std::string_view name)
{
    return name == "am" || name == "usb" || name == "lsb" || name == "dsb" || name == "cw";
}

// "raw, am, nfm, ..., tetra", for a refusal that has to say what would have
// worked.
[[nodiscard]] inline std::string demod_names_text()
{
    std::string out;
    for (const std::string_view name : kDemodNames) {
        if (!out.empty()) {
            out += ", ";
        }
        out += name;
    }
    return out;
}

}  // namespace revenant::ui
