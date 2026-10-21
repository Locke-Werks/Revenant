// EngineLink's side of the noise controls, the AGC switch and the squelch:
// each setter builds the next request and sends it as a retune in place. The
// noise rules are all in models/noise_controls.h, the squelch slider's in
// models/squelch_control.h and the AGC's in the engine; this file is
// plumbing.

#include "models/engine_link.h"

#include <cmath>
#include <cstddef>
#include <utility>

#include <QVariantMap>

#include "models/noise_controls.h"
#include "models/squelch_control.h"

namespace revenant::ui {

void EngineLink::change_noise(const rpc::VrxParams& next)
{
    const bool same = next.nb_enabled == wanted_.nb_enabled &&
                      next.nb_threshold_db == wanted_.nb_threshold_db &&
                      next.notch_enabled == wanted_.notch_enabled &&
                      next.notch_hz == wanted_.notch_hz &&
                      next.notch_depth_db == wanted_.notch_depth_db &&
                      next.notch_width_hz == wanted_.notch_width_hz &&
                      next.notches == wanted_.notches &&
                      next.auto_notch_enabled == wanted_.auto_notch_enabled &&
                      next.nr_enabled == wanted_.nr_enabled &&
                      next.nr_strength == wanted_.nr_strength;
    if (same) {
        return;
    }
    wanted_ = next;
    wanted_.notches[0] = notch_slot(wanted_, 0);

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

QVariantList EngineLink::notches() const
{
    QVariantList out;
    for (std::size_t i = 0; i < rpc::kMaxNotches; ++i) {
        const rpc::NotchEntry notch = notch_slot(wanted_, i);
        out.append(QVariantMap{
            {QStringLiteral("enabled"), notch.enabled},
            {QStringLiteral("hz"), static_cast<int>(notch.hz)},
            {QStringLiteral("depthDb"), notch.depth_db},
            {QStringLiteral("widthHz"), static_cast<int>(notch.width_hz)},
            {QStringLiteral("audioHz"),
             ui::notch_audio_hz(wanted_.demod, notch.hz, wanted_.cw_pitch)},
        });
    }
    return out;
}

namespace {

[[nodiscard]] bool slot_ok(int index)
{
    return index >= 0 && static_cast<std::size_t>(index) < rpc::kMaxNotches;
}

}  // namespace

void EngineLink::setNotchEnabledAt(int index, bool on)
{
    if (!slot_ok(index)) {
        return;
    }
    if (index == 0) {
        setNotchEnabled(on);
        return;
    }
    rpc::VrxParams next = wanted_;
    rpc::NotchEntry notch = notch_slot(next, static_cast<std::size_t>(index));
    if (on && (!noise_stage_offered(next.demod, NoiseStage::Notch) ||
               !(ui::notch_audio_hz(next.demod, notch.hz, next.cw_pitch) > 0.0))) {
        return;
    }
    notch.enabled = on;
    set_notch_slot(next, static_cast<std::size_t>(index), notch);
    change_noise(next);
}

void EngineLink::setNotchHzAt(int index, int hz)
{
    if (!slot_ok(index)) {
        return;
    }
    if (index == 0) {
        setNotchHz(hz);
        return;
    }
    rpc::VrxParams next = wanted_;
    rpc::NotchEntry notch = notch_slot(next, static_cast<std::size_t>(index));
    notch.hz = hz;
    // As for slot 0: a notch moved onto no audio goes off rather than refused.
    if (notch.enabled && !(ui::notch_audio_hz(next.demod, notch.hz, next.cw_pitch) > 0.0)) {
        notch.enabled = false;
    }
    set_notch_slot(next, static_cast<std::size_t>(index), notch);
    change_noise(next);
}

void EngineLink::setNotchAudioHzAt(int index, double audio_hz)
{
    if (!(audio_hz > 0.0)) {
        return;
    }
    setNotchHzAt(index,
                 static_cast<int>(notch_offset_for_audio(wanted_.demod, audio_hz, wanted_.cw_pitch)));
}

void EngineLink::setNotchDepthDbAt(int index, double db)
{
    if (!slot_ok(index)) {
        return;
    }
    rpc::VrxParams next = wanted_;
    rpc::NotchEntry notch = notch_slot(next, static_cast<std::size_t>(index));
    notch.depth_db = clamp_notch_depth(db);
    set_notch_slot(next, static_cast<std::size_t>(index), notch);
    change_noise(next);
}

void EngineLink::setNotchWidthHzAt(int index, int hz)
{
    if (!slot_ok(index)) {
        return;
    }
    rpc::VrxParams next = wanted_;
    rpc::NotchEntry notch = notch_slot(next, static_cast<std::size_t>(index));
    notch.width_hz = clamp_notch_width(hz);
    set_notch_slot(next, static_cast<std::size_t>(index), notch);
    change_noise(next);
}

int EngineLink::addNotch(double audio_hz, double width_hz)
{
    if (!(audio_hz > 0.0) || !noise_stage_offered(wanted_.demod, NoiseStage::Notch)) {
        return -1;
    }
    for (std::size_t i = 0; i < rpc::kMaxNotches; ++i) {
        if (notch_slot(wanted_, i).enabled) {
            continue;
        }
        rpc::VrxParams next = wanted_;
        rpc::NotchEntry notch = notch_slot(next, i);
        notch.hz = notch_offset_for_audio(next.demod, audio_hz, next.cw_pitch);
        notch.width_hz = clamp_notch_width(static_cast<int>(std::lround(width_hz)));
        notch.enabled = ui::notch_audio_hz(next.demod, notch.hz, next.cw_pitch) > 0.0;
        if (!notch.enabled) {
            return -1;
        }
        set_notch_slot(next, i, notch);
        change_noise(next);
        return static_cast<int>(i);
    }
    return -1;
}

void EngineLink::removeNotch(int index)
{
    setNotchEnabledAt(index, false);
}

void EngineLink::addNotches(const QVariantList& notches)
{
    for (const QVariant& item : notches) {
        const QVariantMap map = item.toMap();
        const double audio = map.value(QStringLiteral("audioHz")).toDouble();
        const double width = map.value(QStringLiteral("widthHz"), 100.0).toDouble();
        // A carrier already under an enabled notch is not notched twice.
        bool taken = false;
        for (std::size_t i = 0; i < rpc::kMaxNotches; ++i) {
            const rpc::NotchEntry notch = notch_slot(wanted_, i);
            if (notch.enabled &&
                std::abs(ui::notch_audio_hz(wanted_.demod, notch.hz, wanted_.cw_pitch) - audio) <
                    static_cast<double>(notch.width_hz) / 2.0) {
                taken = true;
                break;
            }
        }
        if (!taken && addNotch(audio, width) < 0) {
            return;
        }
    }
}

void EngineLink::setDisplaySplit(bool on)
{
    if (wanted_.display_split == on) {
        return;
    }
    wanted_.display_split = on;
    if (receiverId() == 0) {
        emit receiverChanged();
        return;
    }
    post_receiver_request(false);
}

void EngineLink::setDisplayAudioSpectrum(bool on)
{
    if (wanted_.display_audio_spectrum == on) {
        return;
    }
    wanted_.display_audio_spectrum = on;
    if (receiverId() == 0) {
        emit receiverChanged();
        return;
    }
    post_receiver_request(false);
}

QVariantMap EngineLink::receiverSetup() const
{
    QVariantList list;
    for (std::size_t i = 0; i < rpc::kMaxNotches; ++i) {
        const rpc::NotchEntry notch = notch_slot(wanted_, i);
        list.append(QVariantMap{
            {QStringLiteral("enabled"), notch.enabled},
            {QStringLiteral("hz"), static_cast<qlonglong>(notch.hz)},
            {QStringLiteral("depthDb"), notch.depth_db},
            {QStringLiteral("widthHz"), static_cast<qlonglong>(notch.width_hz)},
        });
    }
    return QVariantMap{
        {QStringLiteral("hz"), receiverId() == 0 ? 0.0 : receiverCenterHz()},
        {QStringLiteral("mode"), receiverDemod()},
        {QStringLiteral("low"), edges_touched_ ? static_cast<int>(wanted_.passband_low) : 0},
        {QStringLiteral("high"), edges_touched_ ? static_cast<int>(wanted_.passband_high) : 0},
        {QStringLiteral("autoNotch"), wanted_.auto_notch_enabled},
        {QStringLiteral("notches"), list},
    };
}

void EngineLink::applyPreset(const QVariantMap& preset)
{
    rpc::VrxParams next = wanted_;
    next.auto_notch_enabled = preset.value(QStringLiteral("autoNotch")).toBool() &&
                              noise_stage_offered(next.demod, NoiseStage::AutoNotch) &&
                              auto_notch_pitch_ok(next);
    const QVariantList list = preset.value(QStringLiteral("notches")).toList();
    for (std::size_t i = 0; i < rpc::kMaxNotches; ++i) {
        rpc::NotchEntry notch{};
        if (std::cmp_less(i, list.size())) {
            const QVariantMap item = list[static_cast<qsizetype>(i)].toMap();
            notch.enabled = item.value(QStringLiteral("enabled")).toBool();
            notch.hz = item.value(QStringLiteral("hz"), 1000).toLongLong();
            notch.depth_db = clamp_notch_depth(item.value(QStringLiteral("depthDb"), 40.0).toDouble());
            notch.width_hz = clamp_notch_width(item.value(QStringLiteral("widthHz"), 100).toInt());
        }
        set_notch_slot(next, i, notch);
    }
    fit_noise_to_mode(next);
    change_noise(next);

    const int low = preset.value(QStringLiteral("low")).toInt();
    const int high = preset.value(QStringLiteral("high")).toInt();
    if (low == 0 && high == 0) {
        return;
    }
    if (low != static_cast<int>(wanted_.passband_low) ||
        high != static_cast<int>(wanted_.passband_high)) {
        setReceiverPassband(low, high);
        commitReceiverPassband();
    }
}

QVariantList EngineLink::passbandCarriers() const
{
    QVariantList out;
    for (const rpc::PassbandCarrier& carrier : passband_display_.carriers) {
        out.append(QVariantMap{
            {QStringLiteral("offsetHz"), carrier.offset_hz},
            {QStringLiteral("widthHz"), carrier.width_hz},
            {QStringLiteral("levelDb"), static_cast<double>(carrier.level_db)},
            {QStringLiteral("ageSeconds"), static_cast<double>(carrier.age_seconds)},
        });
    }
    return out;
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

void EngineLink::setSquelchDbfs(double dbfs)
{
    const double next = normalise_squelch(dbfs);
    if (wanted_.squelch_dbfs == next) {
        return;
    }
    wanted_.squelch_dbfs = next;

    // On the AGC switch's terms: a retune in place, never a remove and an
    // add, so the audio carries on through a drag of the slider.
    if (receiverId() == 0) {
        emit receiverChanged();
        return;
    }
    post_receiver_request(false);
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
