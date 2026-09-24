// The receiver window's noise controls: which of the three stages a mode
// offers, what a mode change does to them, the ranges the controls move
// within, and the line that says what is on while the panel is closed.
//
// The engine is the authority on all of it. core/dsp/noise_reference.cpp
// decides what a mode offers and engine::place refuses a request that asks
// for more, by name. This header answers the same questions ahead of the
// call so the window can grey out a control rather than let the operator
// click it into a refusal. It is a copy, since the client does not link the
// engine, and ui/tests/test_noise_controls.cpp holds it mode by mode to the
// table docs/noise.md states; a copy that drifted costs a greyed-out control
// the engine would have taken, or a click the engine refuses in its own
// words, and never a wrong setting applied. docs/noise.md says what each
// stage does and has the measured figures.
//
// QUIET WHEN OFF. Every stage is off by default, the controls live behind the
// filter panel's expansion, and noise_summary is empty while nothing is on,
// so a window whose operator never touches noise says nothing about it.
//
// Qt-free on the rule the rest of ui/models follows, so ui/tests can hold it.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "core/rpc/types.h"

namespace revenant::ui {

// What a mode offers. The engine's rules, repeated for the window:
//
//   blanker    every mode that produces audio
//   notch      AM, USB, LSB, DSB and CW, whose audio frequency follows from
//              where a signal sits in the passband
//   auto notch the same less CW, where the steady tone is the signal
//   reduction  every mode that produces audio
//
// No default label: a twelfth mode has to say what it offers here, as it has
// to in core/dsp/noise_reference.cpp.
struct NoiseOffer {
    bool blanker = false;
    bool notch = false;
    bool auto_notch = false;
    bool reduction = false;
};

[[nodiscard]] constexpr NoiseOffer noise_offer(rpc::Demod mode)
{
    switch (mode) {
        case rpc::Demod::Am:
        case rpc::Demod::Sam:
        case rpc::Demod::Usb:
        case rpc::Demod::Lsb:
        case rpc::Demod::Dsb: return {true, true, true, true};
        case rpc::Demod::Cw: return {true, true, false, true};
        case rpc::Demod::Nfm:
        case rpc::Demod::Wfm: return {true, false, false, true};
        case rpc::Demod::Raw:
        case rpc::Demod::P25p1:
        case rpc::Demod::Dstar:
        case rpc::Demod::Tetra:
        case rpc::Demod::Dmr: return {};
    }
    return {};
}

// The ranges engine::place accepts. Stated here so a control's travel is the
// range, and a value at either end of it is one the engine takes.
inline constexpr double kNbThresholdMinDb = 3.0;
inline constexpr double kNbThresholdMaxDb = 40.0;
inline constexpr double kNotchDepthMinDb = 3.0;
inline constexpr double kNotchDepthMaxDb = 80.0;
inline constexpr int kNotchWidthMinHz = 10;
inline constexpr int kNotchWidthMaxHz = 2000;
inline constexpr double kNrStrengthMin = 0.0;
inline constexpr double kNrStrengthMax = 1.0;

[[nodiscard]] constexpr double clamp_nb_threshold(double db)
{
    return std::clamp(db, kNbThresholdMinDb, kNbThresholdMaxDb);
}
[[nodiscard]] constexpr double clamp_notch_depth(double db)
{
    return std::clamp(db, kNotchDepthMinDb, kNotchDepthMaxDb);
}
[[nodiscard]] constexpr int clamp_notch_width(int hz)
{
    return std::clamp(hz, kNotchWidthMinHz, kNotchWidthMaxHz);
}
[[nodiscard]] constexpr double clamp_nr_strength(double strength)
{
    return std::clamp(strength, kNrStrengthMin, kNrStrengthMax);
}

// The audio frequency a notch at passband_hz lands on, which is what the
// operator hears it as, or zero where it lands on none: the discarded side of
// USB or LSB, or the carrier itself. The engine's own mapping is
// dsp::notch_audio_hz.
[[nodiscard]] inline double notch_audio_hz(rpc::Demod mode, std::int64_t passband_hz,
                                           std::int64_t cw_pitch)
{
    const auto f = static_cast<double>(passband_hz);
    switch (mode) {
        case rpc::Demod::Usb: return f > 0.0 ? f : 0.0;
        case rpc::Demod::Lsb: return f < 0.0 ? -f : 0.0;
        case rpc::Demod::Am:
        case rpc::Demod::Sam:
        case rpc::Demod::Dsb: return std::abs(f);
        case rpc::Demod::Cw: return std::abs(f + static_cast<double>(cw_pitch));
        case rpc::Demod::Raw:
        case rpc::Demod::Nfm:
        case rpc::Demod::Wfm:
        case rpc::Demod::P25p1:
        case rpc::Demod::Dstar:
        case rpc::Demod::Tetra:
        case rpc::Demod::Dmr: return 0.0;
    }
    return 0.0;
}

// What a mode change does to the noise settings, applied to params whose
// demod has just been set to the new mode.
//
// A stage the new mode does not offer is switched off, because the engine
// would refuse the whole receiver over it and a mode change that fails over a
// setting the operator made on another mode is the wrong answer. A stage it
// does offer is kept, with its figures, which is the point: switching LSB to
// USB on a station with a whistle keeps the notch.
//
// The notch keeps its AUDIO frequency across a sideband change. On USB a
// 1300 Hz whistle sits at +1300 from the carrier and on LSB at -1300, so the
// sign follows the sideband. A CW notch that would land on the carrier's own
// audio frequency of zero is switched off rather than refused.
inline void fit_noise_to_mode(rpc::VrxParams& params)
{
    const NoiseOffer offer = noise_offer(params.demod);
    params.nb_enabled = params.nb_enabled && offer.blanker;
    params.notch_enabled = params.notch_enabled && offer.notch;
    params.auto_notch_enabled = params.auto_notch_enabled && offer.auto_notch;
    params.nr_enabled = params.nr_enabled && offer.reduction;

    if (params.demod == rpc::Demod::Usb) {
        params.notch_hz = std::abs(params.notch_hz);
    } else if (params.demod == rpc::Demod::Lsb) {
        params.notch_hz = -std::abs(params.notch_hz);
    }
    if (params.notch_enabled &&
        !(notch_audio_hz(params.demod, params.notch_hz, params.cw_pitch) > 0.0)) {
        params.notch_enabled = false;
    }
}

enum class NoiseStage : std::uint8_t {
    Blanker,
    Notch,
    AutoNotch,
    Reduction,
};

// The names the key table and the window use for the four stages.
[[nodiscard]] constexpr std::optional<NoiseStage> noise_stage_from_name(std::string_view name)
{
    if (name == "nb") {
        return NoiseStage::Blanker;
    }
    if (name == "notch") {
        return NoiseStage::Notch;
    }
    if (name == "auto_notch") {
        return NoiseStage::AutoNotch;
    }
    if (name == "nr") {
        return NoiseStage::Reduction;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr bool noise_stage_offered(rpc::Demod mode, NoiseStage stage)
{
    const NoiseOffer offer = noise_offer(mode);
    switch (stage) {
        case NoiseStage::Blanker: return offer.blanker;
        case NoiseStage::Notch: return offer.notch;
        case NoiseStage::AutoNotch: return offer.auto_notch;
        case NoiseStage::Reduction: return offer.reduction;
    }
    return false;
}

[[nodiscard]] constexpr bool noise_stage_on(const rpc::VrxParams& params, NoiseStage stage)
{
    switch (stage) {
        case NoiseStage::Blanker: return params.nb_enabled;
        case NoiseStage::Notch: return params.notch_enabled;
        case NoiseStage::AutoNotch: return params.auto_notch_enabled;
        case NoiseStage::Reduction: return params.nr_enabled;
    }
    return false;
}

// Switches one stage on or off. Returns false, and changes nothing, when the
// mode does not offer it or when switching it on would be refused: a manual
// notch sitting on the side of the carrier the mode discards is moved to the
// side it keeps first, and one that still lands on no audio stays off.
inline bool set_noise_stage(rpc::VrxParams& params, NoiseStage stage, bool on)
{
    if (on && !noise_stage_offered(params.demod, stage)) {
        return false;
    }
    switch (stage) {
        case NoiseStage::Blanker: params.nb_enabled = on; return true;
        case NoiseStage::Notch:
            if (on) {
                rpc::VrxParams trial = params;
                trial.notch_enabled = true;
                fit_noise_to_mode(trial);
                if (!trial.notch_enabled) {
                    return false;
                }
                params.notch_hz = trial.notch_hz;
            }
            params.notch_enabled = on;
            return true;
        case NoiseStage::AutoNotch: params.auto_notch_enabled = on; return true;
        case NoiseStage::Reduction: params.nr_enabled = on; return true;
    }
    return false;
}

inline bool toggle_noise_stage(rpc::VrxParams& params, NoiseStage stage)
{
    return set_noise_stage(params, stage, !noise_stage_on(params, stage));
}

// The line beside the filter panel's handle while it is closed: what is on,
// in the order the stages run, and nothing at all when nothing is.
[[nodiscard]] inline std::string noise_summary(const rpc::VrxParams& params)
{
    const NoiseOffer offer = noise_offer(params.demod);
    std::string out;
    auto add = [&out](const std::string& part) {
        if (!out.empty()) {
            out += ", ";
        }
        out += part;
    };
    if (params.nb_enabled && offer.blanker) {
        add("blanker");
    }
    if (params.notch_enabled && offer.notch) {
        add(std::format("notch {:.2f} kHz",
                        notch_audio_hz(params.demod, params.notch_hz, params.cw_pitch) /
                            1000.0));
    }
    if (params.auto_notch_enabled && offer.auto_notch) {
        add("auto notch");
    }
    if (params.nr_enabled && offer.reduction) {
        add(std::format("NR {:.0f}%", 100.0 * params.nr_strength));
    }
    return out;
}

// Why a stage the panel shows is greyed out on this mode, in a few words for
// the control's own label. Empty when it is offered.
[[nodiscard]] constexpr std::string_view noise_stage_note(rpc::Demod mode, NoiseStage stage)
{
    if (noise_stage_offered(mode, stage)) {
        return {};
    }
    if (stage == NoiseStage::AutoNotch && mode == rpc::Demod::Cw) {
        return "not on cw, where the steady tone is the signal";
    }
    if (mode == rpc::Demod::Raw || mode == rpc::Demod::P25p1 || mode == rpc::Demod::Dstar ||
        mode == rpc::Demod::Tetra || mode == rpc::Demod::Dmr) {
        return "not on a complex tap, which has no audio";
    }
    return "not on FM, whose audio is not placed by the passband";
}

}  // namespace revenant::ui
