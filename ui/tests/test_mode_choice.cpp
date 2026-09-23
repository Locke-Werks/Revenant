// The mode selector's lists and labels, and which modes the audio section
// applies to.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <set>
#include <string_view>
#include <vector>

#include "core/rpc/types.h"
#include "models/aft.h"
#include "models/auto_filter.h"
#include "models/mode_choice.h"

using revenant::rpc::Demod;
using revenant::ui::demod_names_text;
using revenant::ui::digital_group_label;
using revenant::ui::kDemodNames;
using revenant::ui::kModeChoices;
using revenant::ui::mode_is_digital;
using revenant::ui::mode_label;
using revenant::ui::mode_makes_audio;
using revenant::ui::mode_needs_level;
using revenant::ui::mode_names;

namespace {

using Names = std::vector<std::string_view>;

[[nodiscard]] std::string_view name_of(Demod mode)
{
    return kDemodNames[static_cast<std::size_t>(mode)];
}

}  // namespace

// Rejects a table whose spellings slid against the enum, which is a receiver
// in one mode shown and requested as another. The ordinals are pinned by
// core/rpc/convert.h against the engine; the spellings only here.
TEST_CASE("every demodulator name sits at its rpc::Demod ordinal", "[modes]")
{
    CHECK(name_of(Demod::Raw) == "raw");
    CHECK(name_of(Demod::Am) == "am");
    CHECK(name_of(Demod::Nfm) == "nfm");
    CHECK(name_of(Demod::Wfm) == "wfm");
    CHECK(name_of(Demod::Usb) == "usb");
    CHECK(name_of(Demod::Lsb) == "lsb");
    CHECK(name_of(Demod::Dsb) == "dsb");
    CHECK(name_of(Demod::Cw) == "cw");
    CHECK(name_of(Demod::P25p1) == "p25p1");
    CHECK(name_of(Demod::Dstar) == "dstar");
    CHECK(name_of(Demod::Tetra) == "tetra");
}

// Rejects a selector that leaves a demodulator unreachable, which is what the
// digital three were, or offers one twice.
TEST_CASE("the selector offers every demodulator exactly once", "[modes]")
{
    std::multiset<std::string_view> offered;
    for (const auto& choice : kModeChoices) {
        offered.insert(choice.name);
    }
    for (const std::string_view name : kDemodNames) {
        INFO(name);
        CHECK(offered.count(name) == 1);
    }
    CHECK(offered.size() == kDemodNames.size());
}

// Rejects widening the row by the digital modes, and rejects a group that
// holds anything the row already has.
TEST_CASE("the row is the eight it was and the digital group is the other three", "[modes]")
{
    CHECK(mode_names(false) == Names{"am", "nfm", "wfm", "usb", "lsb", "dsb", "cw", "raw"});
    CHECK(mode_names(true) == Names{"p25p1", "dstar", "tetra"});
}

// Rejects showing the engine's spellings for the digital modes, and rejects
// relabelling the row, whose names are what an operator has been clicking.
TEST_CASE("the digital modes carry their on-air names", "[modes]")
{
    CHECK(mode_label("p25p1") == "P25");
    CHECK(mode_label("dstar") == "D-STAR");
    CHECK(mode_label("tetra") == "TETRA");
    CHECK(mode_label("nfm") == "nfm");
    CHECK(mode_label("raw") == "raw");

    // A mode from a newer engine reads as itself rather than as nothing.
    CHECK(mode_label("dmr") == "dmr");
}

// Rejects a group segment that says "digital" while one of its modes is in
// force, which hides which one the receiver is in, and one that names a row
// mode.
TEST_CASE("the digital segment names the digital mode in force", "[modes]")
{
    CHECK(digital_group_label("dstar") == "D-STAR");
    CHECK(digital_group_label("p25p1") == "P25");
    CHECK(digital_group_label("nfm") == "digital");
    CHECK(digital_group_label("raw") == "digital");
    CHECK(digital_group_label("") == "digital");

    CHECK(mode_is_digital("tetra"));
    CHECK_FALSE(mode_is_digital("raw"));
    CHECK_FALSE(mode_is_digital("dmr"));
}

// Rejects an audio section on a complex tap, where the engine refuses the
// subscription and the section could only show that. The list is
// engine::produces_audio's, and raw is in it with the digital three.
TEST_CASE("only the analogue demodulators make audio", "[modes]")
{
    for (const std::string_view name : {"am", "nfm", "wfm", "usb", "lsb", "dsb", "cw"}) {
        INFO(name);
        CHECK(mode_makes_audio(name));
    }
    for (const std::string_view name : {"raw", "p25p1", "dstar", "tetra", "dmr", ""}) {
        INFO(name);
        CHECK_FALSE(mode_makes_audio(name));
    }
}

// Rejects leaving am, ssb and cw at the level their signal came in at, which
// is what the owner heard as no audio at all, and rejects levelling a
// discriminator's or a vocoder's output, which already arrives at a
// listening level whatever the signal.
TEST_CASE("the amplitude-detected modes are the ones levelled", "[modes]")
{
    for (const std::string_view name : {"am", "usb", "lsb", "dsb", "cw"}) {
        INFO(name);
        CHECK(mode_needs_level(name));
    }
    for (const std::string_view name : {"nfm", "wfm", "p25p1", "raw", "dstar", "tetra", ""}) {
        INFO(name);
        CHECK_FALSE(mode_needs_level(name));
    }
}

// Rejects an AFT or an auto filter that acts on a digital receiver. Neither
// has a rule measured on one, and a fit narrower than the mode's channel cuts
// what its decoder reads.
TEST_CASE("AFT holds and the auto filter does nothing on the digital modes", "[modes]")
{
    for (const std::string_view name : mode_names(true)) {
        INFO(name);
        CHECK(revenant::ui::aft_rule_for(name) == revenant::ui::AftRule::Hold);
        CHECK(revenant::ui::auto_filter_rule_for(name) == revenant::ui::AutoFilterRule::None);
    }
}

// Rejects a refusal that lists the eight the window used to know.
TEST_CASE("a refused mode is answered with all eleven names", "[modes]")
{
    CHECK(demod_names_text() == "raw, am, nfm, wfm, usb, lsb, dsb, cw, p25p1, dstar, tetra");
}
