// EngineLink's half of the per-radio memory: sampling the open radio's rate,
// centre and gain, writing them once they settle, and reading them back for
// the picker. The rules are in ui/models/radio_memory.h, where ui/tests can
// reach them.

#include "models/engine_link.h"

#include <QSettings>
#include <QString>
#include <QVariantMap>

#include "models/engine_start.h"
#include "models/settings.h"

namespace revenant::ui {

namespace {

[[nodiscard]] QString key_for(std::string_view radio_id, std::string_view leaf)
{
    return QString::fromStdString(radio_setting_key(radio_id, leaf));
}

[[nodiscard]] RadioMemory load_radio_memory(const QSettings& store, std::string_view radio_id)
{
    RadioMemory memory;
    if (radio_id.empty()) {
        return memory;
    }
    memory.centre_hz = read_positive_setting(
        store.value(key_for(radio_id, kRadioCentreLeaf), 0).toLongLong());
    memory.rate =
        read_positive_setting(store.value(key_for(radio_id, kRadioRateLeaf), 0).toLongLong());
    read_gain_setting(store.value(key_for(radio_id, kRadioGainLeaf)).toString().toStdString(),
                      memory);
    return memory;
}

[[nodiscard]] std::optional<std::string> load_direct_sampling(const QSettings& store,
                                                              std::string_view radio_id)
{
    if (radio_id.empty()) {
        return std::nullopt;
    }
    const QString stored =
        store.value(QString::fromStdString(direct_sampling_key(radio_id))).toString();
    return direct_sampling_choice(stored.toStdString());
}

void store_radio_snapshot(const RadioSnapshot& snapshot)
{
    if (snapshot.radio_id.empty()) {
        return;
    }
    QSettings store;
    const RadioMemory& memory = snapshot.memory;
    if (memory.centre_hz.has_value()) {
        store.setValue(key_for(snapshot.radio_id, kRadioCentreLeaf),
                       static_cast<qlonglong>(*memory.centre_hz));
    }
    if (memory.rate.has_value()) {
        store.setValue(key_for(snapshot.radio_id, kRadioRateLeaf),
                       static_cast<qlonglong>(*memory.rate));
    }
    if (memory.has_gain()) {
        store.setValue(key_for(snapshot.radio_id, kRadioGainLeaf),
                       QString::fromStdString(gain_setting_text(memory)));
    }

    // Paired with engine/lastSource, which names the device by index and so
    // cannot say which radio's values the reopen should read.
    store.setValue(QString::fromLatin1(kLastRadioKey.data(),
                                       static_cast<qsizetype>(kLastRadioKey.size())),
                   QString::fromStdString(snapshot.radio_id));
}

}  // namespace

RadioSnapshot EngineLink::current_radio_snapshot() const
{
    RadioSnapshot snapshot;
    if (!connected_ || !sourceOpen()) {
        return snapshot;
    }

    // ONLY ONCE THE IDENTITY AND THE GEOMETRY DESCRIBE THE SAME STREAM. The
    // descriptor and EngineInfo arrive on separate handovers, so for a moment
    // after a radio change the new serial can sit beside the old radio's
    // centre and rate, and saving then would file one radio's numbers under
    // the other.
    const auto described_epoch = open_source_.value(QStringLiteral("epoch")).toULongLong();
    if (open_source_.isEmpty() || described_epoch != info_.source_epoch) {
        return snapshot;
    }

    const std::string backend =
        open_source_.value(QStringLiteral("backend")).toString().toStdString();
    const std::string serial =
        open_source_.value(QStringLiteral("serial")).toString().toStdString();
    if (backend.empty() || serial.empty()) {
        return snapshot;
    }

    // A recording that happens to carry a serial is still not a front end.
    const std::string uri = open_source_.value(QStringLiteral("uri")).toString().toStdString();
    if (!remembers_as_last_radio(uri)) {
        return snapshot;
    }

    snapshot.radio_id = radio_settings_id(backend + ":" + serial);
    if (info_.source_center > 0) {
        snapshot.memory.centre_hz = info_.source_center;
    }
    if (info_.source_rate > 0) {
        snapshot.memory.rate = static_cast<std::int64_t>(info_.source_rate);
    }

    // The gain only once the device has said what it is on, or that it is on
    // auto. A handle the operator is dragging is a request, and a request the
    // stage refused is not what the radio was left at.
    if (gain_auto_) {
        snapshot.memory.gain_auto = true;
    } else if (gain_known_) {
        snapshot.memory.gain_db = gain_db_;
    }
    return snapshot;
}

void EngineLink::note_radio_state()
{
    // A smoke run remembers no radio, the rule kLastSource already follows.
    if (!remember_last_source_) {
        return;
    }
    if (!radio_save_clock_.isValid()) {
        radio_save_clock_.start();
    }
    const std::int64_t now = radio_save_clock_.elapsed();
    if (auto write_now = radio_save_.note(current_radio_snapshot(), now)) {
        store_radio_snapshot(*write_now);
    }
    if (radio_save_.pending()) {
        radio_save_timer_.start(static_cast<int>(radio_save_.wait_ms(now)));
    } else {
        radio_save_timer_.stop();
    }
}

void EngineLink::save_due_radio_state()
{
    const std::int64_t now = radio_save_clock_.isValid() ? radio_save_clock_.elapsed() : 0;
    if (auto due = radio_save_.due(now)) {
        store_radio_snapshot(*due);
    } else if (radio_save_.pending()) {
        radio_save_timer_.start(static_cast<int>(radio_save_.wait_ms(now)));
    }
}

void EngineLink::flush_radio_state()
{
    radio_save_timer_.stop();
    if (auto pending = radio_save_.flush()) {
        store_radio_snapshot(*pending);
    }
}

QVariantMap EngineLink::rememberedRadio(int index) const
{
    QVariantMap out;
    out.insert(QStringLiteral("known"), false);
    if (index < 0 || static_cast<std::size_t>(index) >= source_rows_.size()) {
        return out;
    }
    const rpc::SourceDescriptor& source = source_rows_[static_cast<std::size_t>(index)];
    const std::string radio = descriptor_calibration_key(source);
    if (radio.empty()) {
        return out;
    }

    const QSettings store;
    const std::string radio_id = radio_settings_id(radio);
    const RadioMemory memory = load_radio_memory(store, radio_id);
    if (memory == RadioMemory{}) {
        return out;
    }

    // Through restore_radio_choice, so the boxes show the values the open
    // will actually compose, with the direct sampling mode already in force
    // for the centre.
    const SourceChoice choice =
        restore_radio_choice(source, memory, load_direct_sampling(store, radio_id));

    out.insert(QStringLiteral("known"), true);
    // With its unit, because the centre box reads a bare number under 1 MHz
    // as megahertz, and a 500 kHz centre on the Q branch would come back as
    // 500 MHz.
    out.insert(QStringLiteral("centre"),
               choice.center_hz ? QString::number(*choice.center_hz) + QStringLiteral(" Hz")
                                : QString());
    out.insert(QStringLiteral("rate"), choice.rate ? QString::number(*choice.rate) : QString());
    QString gain;
    bool gain_auto = false;
    if (!choice.gains.empty()) {
        gain_auto = choice.gains.front().automatic;
        if (choice.gains.front().db.has_value()) {
            gain = QString::fromStdString(detail::decimal(*choice.gains.front().db));
        }
    }
    out.insert(QStringLiteral("gain"), gain);
    out.insert(QStringLiteral("gainAuto"), gain_auto);
    return out;
}

QString EngineLink::restored_last_source(const QString& remembered) const
{
    if (remembered.isEmpty()) {
        return remembered;
    }
    const QSettings store;
    const std::string radio_id =
        store
            .value(QString::fromLatin1(kLastRadioKey.data(),
                                       static_cast<qsizetype>(kLastRadioKey.size())))
            .toString()
            .toStdString();
    if (radio_id.empty()) {
        return remembered;
    }
    const RadioMemory memory = load_radio_memory(store, radio_id);
    const std::optional<std::string> direct = load_direct_sampling(store, radio_id);
    std::optional<std::string_view> direct_view;
    if (direct.has_value()) {
        direct_view = *direct;
    }
    return QString::fromStdString(
        restore_radio_uri(remembered.toStdString(), memory, direct_view));
}

}  // namespace revenant::ui
