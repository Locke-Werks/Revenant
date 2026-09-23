// The receiver window's noise controls, models/noise_controls.h.
//
// EVERY CASE NAMES THE WRONG ANSWER IT REJECTS, on the rule the rest of
// ui/tests follows.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "core/rpc/types.h"
#include "models/noise_controls.h"

using revenant::rpc::Demod;
using revenant::rpc::VrxParams;
using revenant::ui::fit_noise_to_mode;
using revenant::ui::noise_offer;
using revenant::ui::noise_stage_from_name;
using revenant::ui::noise_stage_note;
using revenant::ui::noise_summary;
using revenant::ui::NoiseStage;
using revenant::ui::set_noise_stage;
using revenant::ui::toggle_noise_stage;

namespace {

[[nodiscard]] VrxParams everything_on(Demod mode)
{
    VrxParams params;
    params.demod = mode;
    params.nb_enabled = true;
    params.notch_enabled = true;
    params.notch_hz = 1'300;
    params.auto_notch_enabled = true;
    params.nr_enabled = true;
    return params;
}

}  // namespace

// REJECTS a table that offers a stage the engine refuses, or greys out one it
// takes. The rows are docs/noise.md's, which core/dsp/noise_reference.cpp
// implements.
TEST_CASE("each mode offers the stages the engine takes on it", "[ui][noise]")
{
    struct Row {
        Demod mode;
        bool blanker;
        bool notch;
        bool auto_notch;
        bool reduction;
    };
    const Row rows[] = {
        {Demod::Am, true, true, true, true},     {Demod::Usb, true, true, true, true},
        {Demod::Lsb, true, true, true, true},    {Demod::Dsb, true, true, true, true},
        {Demod::Cw, true, true, false, true},    {Demod::Nfm, true, false, false, true},
        {Demod::Wfm, true, false, false, true},  {Demod::Raw, false, false, false, false},
        {Demod::P25p1, false, false, false, false},
        {Demod::Dstar, false, false, false, false},
        {Demod::Tetra, false, false, false, false},
        {Demod::Dmr, false, false, false, false},
    };
    for (const Row& row : rows) {
        INFO("mode " << static_cast<int>(row.mode));
        const auto offer = noise_offer(row.mode);
        CHECK(offer.blanker == row.blanker);
        CHECK(offer.notch == row.notch);
        CHECK(offer.auto_notch == row.auto_notch);
        CHECK(offer.reduction == row.reduction);
    }
}

// REJECTS a mode change that carries a stage the new mode refuses, which the
// engine answers by refusing the whole receiver, and one that drops a stage
// the new mode takes.
TEST_CASE("a mode change keeps what the new mode takes and drops the rest", "[ui][noise]")
{
    VrxParams params = everything_on(Demod::Usb);
    params.demod = Demod::Cw;
    fit_noise_to_mode(params);
    CHECK(params.nb_enabled);
    CHECK(params.notch_enabled);
    CHECK_FALSE(params.auto_notch_enabled);
    CHECK(params.nr_enabled);

    params = everything_on(Demod::Usb);
    params.demod = Demod::Nfm;
    fit_noise_to_mode(params);
    CHECK(params.nb_enabled);
    CHECK_FALSE(params.notch_enabled);
    CHECK_FALSE(params.auto_notch_enabled);
    CHECK(params.nr_enabled);

    params = everything_on(Demod::Usb);
    params.demod = Demod::P25p1;
    fit_noise_to_mode(params);
    CHECK(noise_summary(params).empty());
    CHECK_FALSE(params.nb_enabled);
    CHECK_FALSE(params.nr_enabled);

    // The figures ride along even for a stage that went off, so switching
    // back finds them where they were left.
    params = everything_on(Demod::Usb);
    params.nr_strength = 0.8;
    params.demod = Demod::Raw;
    fit_noise_to_mode(params);
    CHECK(params.nr_strength == 0.8);
}

// REJECTS a notch that stays on the side of the carrier the new sideband
// discards, which the engine refuses as landing on no audio frequency.
TEST_CASE("a sideband change moves the notch to keep its audio frequency", "[ui][noise]")
{
    VrxParams params = everything_on(Demod::Usb);
    params.demod = Demod::Lsb;
    fit_noise_to_mode(params);
    CHECK(params.notch_enabled);
    CHECK(params.notch_hz == -1'300);

    params.demod = Demod::Usb;
    fit_noise_to_mode(params);
    CHECK(params.notch_hz == 1'300);

    // AM hears both sides, so the sign stays where it is.
    params.notch_hz = -700;
    params.demod = Demod::Am;
    fit_noise_to_mode(params);
    CHECK(params.notch_hz == -700);

    // A CW notch on the carrier's own audio frequency of zero lands on no
    // audio and goes off rather than being refused.
    params.demod = Demod::Cw;
    params.cw_pitch = 700;
    fit_noise_to_mode(params);
    CHECK_FALSE(params.notch_enabled);
}

// REJECTS a key that switches on a stage the mode does not offer, and a
// toggle that does nothing twice.
TEST_CASE("a stage toggles only where it is offered", "[ui][noise]")
{
    VrxParams params;
    params.demod = Demod::Cw;
    CHECK_FALSE(toggle_noise_stage(params, NoiseStage::AutoNotch));
    CHECK_FALSE(params.auto_notch_enabled);

    CHECK(toggle_noise_stage(params, NoiseStage::Reduction));
    CHECK(params.nr_enabled);
    CHECK(toggle_noise_stage(params, NoiseStage::Reduction));
    CHECK_FALSE(params.nr_enabled);

    // Switching off is always allowed, whatever the mode.
    params.auto_notch_enabled = true;
    CHECK(set_noise_stage(params, NoiseStage::AutoNotch, false));
    CHECK_FALSE(params.auto_notch_enabled);

    // A USB notch left on the discarded side is moved across when it is
    // switched on, so the click lands on audio rather than on a refusal.
    params = VrxParams{};
    params.demod = Demod::Usb;
    params.notch_hz = -900;
    CHECK(toggle_noise_stage(params, NoiseStage::Notch));
    CHECK(params.notch_enabled);
    CHECK(params.notch_hz == 900);

    CHECK(noise_stage_from_name("nb") == NoiseStage::Blanker);
    CHECK(noise_stage_from_name("notch") == NoiseStage::Notch);
    CHECK(noise_stage_from_name("auto_notch") == NoiseStage::AutoNotch);
    CHECK(noise_stage_from_name("nr") == NoiseStage::Reduction);
    CHECK_FALSE(noise_stage_from_name("anr").has_value());
}

// REJECTS a closed panel that says something while nothing is on, and one
// that hides a stage that is.
TEST_CASE("the closed panel's line is empty until something is on", "[ui][noise]")
{
    VrxParams params;
    params.demod = Demod::Usb;
    CHECK(noise_summary(params).empty());

    params = everything_on(Demod::Usb);
    params.nr_strength = 0.5;
    CHECK(noise_summary(params) == "blanker, notch 1.30 kHz, auto notch, NR 50%");

    // A stage whose flag is set on a mode that does not offer it is not
    // running, so the line does not claim it.
    params.demod = Demod::Nfm;
    CHECK(noise_summary(params) == "blanker, NR 50%");
}

TEST_CASE("a greyed-out stage says why", "[ui][noise]")
{
    CHECK(noise_stage_note(Demod::Usb, NoiseStage::AutoNotch).empty());
    CHECK(std::string(noise_stage_note(Demod::Cw, NoiseStage::AutoNotch)).find("cw") !=
          std::string::npos);
    CHECK_FALSE(noise_stage_note(Demod::Nfm, NoiseStage::Notch).empty());
    CHECK_FALSE(noise_stage_note(Demod::Raw, NoiseStage::Blanker).empty());
}
