// EngineLink's side of the noise controls and the AGC switch: each setter
// builds the next request and sends it as a retune in place. The noise rules
// are all in models/noise_controls.h and the AGC's in the engine; this file
// is plumbing.

#include "models/engine_link.h"

#include "models/noise_controls.h"

namespace revenant::ui {

void EngineLink::change_noise(const rpc::VrxParams& next)
{
    const bool same = next.nb_enabled == wanted_.nb_enabled &&
                      next.nb_threshold_db == wanted_.nb_threshold_db &&
                      next.notch_enabled == wanted_.notch_enabled &&
                      next.notch_hz == wanted_.notch_hz &&
                      next.notch_depth_db == wanted_.notch_depth_db &&
                      next.notch_width_hz == wanted_.notch_width_hz &&
                      next.auto_notch_enabled == wanted_.auto_notch_enabled &&
                      next.nr_enabled == wanted_.nr_enabled &&
                      next.nr_strength == wanted_.nr_strength;
    if (same) {
        return;
    }
    wanted_ = next;

    // Tuning, never shape, so an ordinary retune: no remove and no add, and
    // the audio carries on through it. With no receiver in the pane the
    // settings wait in the request for the one that opens next.
    if (receiverId() == 0) {
        emit receiverChanged();
        return;
    }
    post_receiver_request(false);
}

void EngineLink::setNoiseBlanker(bool on)
{
    rpc::VrxParams next = wanted_;
    if (set_noise_stage(next, NoiseStage::Blanker, on)) {
        change_noise(next);
    }
}

void EngineLink::setNoiseBlankerThresholdDb(double db)
{
    rpc::VrxParams next = wanted_;
    next.nb_threshold_db = clamp_nb_threshold(db);
    change_noise(next);
}

void EngineLink::setNotchEnabled(bool on)
{
    rpc::VrxParams next = wanted_;
    if (set_noise_stage(next, NoiseStage::Notch, on)) {
        change_noise(next);
    }
}

void EngineLink::setNotchHz(int hz)
{
    rpc::VrxParams next = wanted_;
    next.notch_hz = hz;
    // A notch dragged onto the side of the carrier this mode discards, or
    // onto the carrier itself, lands on no audio and would be refused. It
    // goes off instead, and on again when it is moved back.
    if (next.notch_enabled &&
        !(ui::notch_audio_hz(next.demod, next.notch_hz, next.cw_pitch) > 0.0)) {
        next.notch_enabled = false;
    }
    change_noise(next);
}

void EngineLink::setNotchDepthDb(double db)
{
    rpc::VrxParams next = wanted_;
    next.notch_depth_db = clamp_notch_depth(db);
    change_noise(next);
}

void EngineLink::setNotchWidthHz(int hz)
{
    rpc::VrxParams next = wanted_;
    next.notch_width_hz = clamp_notch_width(hz);
    change_noise(next);
}

void EngineLink::setAutoNotch(bool on)
{
    rpc::VrxParams next = wanted_;
    if (set_noise_stage(next, NoiseStage::AutoNotch, on)) {
        change_noise(next);
    }
}

void EngineLink::setNoiseReduction(bool on)
{
    rpc::VrxParams next = wanted_;
    if (set_noise_stage(next, NoiseStage::Reduction, on)) {
        change_noise(next);
    }
}

void EngineLink::setNoiseReductionStrength(double strength)
{
    rpc::VrxParams next = wanted_;
    next.nr_strength = clamp_nr_strength(strength);
    change_noise(next);
}

bool EngineLink::noiseBlankerOffered() const
{
    return noise_stage_offered(wanted_.demod, NoiseStage::Blanker);
}

bool EngineLink::notchOffered() const
{
    return noise_stage_offered(wanted_.demod, NoiseStage::Notch);
}

bool EngineLink::autoNotchOffered() const
{
    return noise_stage_offered(wanted_.demod, NoiseStage::AutoNotch);
}

QString EngineLink::noiseNote() const
{
    const auto note = noise_stage_note(wanted_.demod, NoiseStage::Blanker);
    return QString::fromUtf8(note.data(), static_cast<qsizetype>(note.size()));
}

QString EngineLink::notchNote() const
{
    const auto note = noise_stage_note(wanted_.demod, NoiseStage::Notch);
    return QString::fromUtf8(note.data(), static_cast<qsizetype>(note.size()));
}

QString EngineLink::autoNotchNote() const
{
    const auto note = noise_stage_note(wanted_.demod, NoiseStage::AutoNotch);
    return QString::fromUtf8(note.data(), static_cast<qsizetype>(note.size()));
}

QString EngineLink::noiseSummary() const
{
    return QString::fromStdString(noise_summary(wanted_));
}

double EngineLink::notchAudioHz() const
{
    return ui::notch_audio_hz(wanted_.demod, wanted_.notch_hz, wanted_.cw_pitch);
}

void EngineLink::setAgcEnabled(bool on)
{
    if (wanted_.agc_enabled == on) {
        return;
    }
    wanted_.agc_enabled = on;

    // Tuning, never shape, on the same terms as the noise controls: a retune
    // in place, and with no receiver in the pane the setting waits in the
    // request for the one that opens next.
    if (receiverId() == 0) {
        emit receiverChanged();
        return;
    }
    post_receiver_request(false);
}

bool EngineLink::agcOffered() const
{
    return mode_takes_agc(demod_name(wanted_.demod).toStdString());
}

void EngineLink::toggleNoiseStage(const QString& stage)
{
    const auto parsed = noise_stage_from_name(stage.toStdString());
    if (!parsed) {
        return;
    }
    rpc::VrxParams next = wanted_;
    if (toggle_noise_stage(next, *parsed)) {
        change_noise(next);
    }
}

}  // namespace revenant::ui
