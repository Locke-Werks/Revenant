// The RTL-SDR front-end switches and the EEPROM serial writer, as EngineLink
// members. The rules are in models/front_end.h with their cases in ui/tests;
// this file reads the open source, writes settings and starts processes.

#include "models/engine_link.h"

#include <cstdint>
#include <string>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QString>
#include <QVariantMap>

#include "models/detector_scope.h"
#include "models/front_end.h"
#include "models/source_choice.h"

namespace revenant::ui {

namespace {

// The radio id the open source's settings are kept under, empty without a
// serial. The same id composeSourceUri reads them back by.
[[nodiscard]] std::string open_radio_id(const QVariantMap& open)
{
    const std::string backend = open.value(QStringLiteral("backend")).toString().toStdString();
    const std::string serial = open.value(QStringLiteral("serial")).toString().toStdString();
    if (backend.empty() || serial.empty()) {
        return {};
    }
    return radio_settings_id(backend + ":" + serial);
}

}  // namespace

QVariantMap EngineLink::frontEnd() const
{
    QVariantMap out;
    const bool rtl = open_source_.value(QStringLiteral("backend")).toString() == "rtlsdr";
    out.insert(QStringLiteral("available"), rtl);
    const std::string uri = open_source_.value(QStringLiteral("uri")).toString().toStdString();
    const FrontEnd now = front_end_of(uri);
    out.insert(QStringLiteral("biasTee"), now.bias_tee);
    out.insert(QStringLiteral("digitalAgc"), now.digital_agc);
    out.insert(QStringLiteral("offsetTuning"), now.offset_tuning);
    out.insert(QStringLiteral("bandwidthHz"), static_cast<double>(now.bandwidth_hz));
    out.insert(QStringLiteral("offsetAvailable"),
               open_source_.value(QStringLiteral("offsetTuningAvailable")).toBool());
    return out;
}

QVariantList EngineLink::tunerBandwidths() const
{
    QVariantList out;
    for (const std::uint32_t hz : kTunerBandwidths) {
        out.append(static_cast<double>(hz));
    }
    return out;
}

// The open URI with only the front-end keys changed, reopened at the centre
// the source is on now, the way setDirectSampling reopens.
void EngineLink::reopenWith(std::string uri)
{
    if (uri.empty()) {
        return;
    }
    if (info_.source_center > 0 && query_value(uri, "freq").has_value()) {
        uri = with_query_value(uri, "freq", std::to_string(info_.source_center));
    }
    openSource(QString::fromStdString(uri));
}

void EngineLink::setFrontEnd(const QString& field, const QVariant& value)
{
    if (open_source_.value(QStringLiteral("backend")).toString() != "rtlsdr") {
        return;
    }
    const std::string uri = open_source_.value(QStringLiteral("uri")).toString().toStdString();
    FrontEnd wanted = front_end_of(uri);
    const std::string id = open_radio_id(open_source_);
    QSettings store;

    if (field == QStringLiteral("biasTee")) {
        wanted.bias_tee = value.toBool();
    } else if (field == QStringLiteral("digitalAgc")) {
        wanted.digital_agc = value.toBool();
        if (!id.empty()) {
            store.setValue(QString::fromStdString(front_end_key(id, "digitalAgc")),
                           wanted.digital_agc);
        }
    } else if (field == QStringLiteral("offsetTuning")) {
        wanted.offset_tuning = value.toBool();
        if (!id.empty()) {
            store.setValue(QString::fromStdString(front_end_key(id, "offsetTuning")),
                           wanted.offset_tuning);
        }
    } else if (field == QStringLiteral("bandwidthHz")) {
        const double hz = value.toDouble();
        wanted.bandwidth_hz = hz > 0.0 ? static_cast<std::uint32_t>(hz) : 0U;
        if (!id.empty()) {
            store.setValue(QString::fromStdString(front_end_key(id, "tunerBandwidth")),
                           wanted.bandwidth_hz);
        }
    } else {
        return;
    }

    const bool offset_available =
        open_source_.value(QStringLiteral("offsetTuningAvailable")).toBool();
    const std::string next = with_front_end(uri, wanted, offset_available);
    if (next != uri) {
        reopenWith(next);
    }
}

void EngineLink::setGainMode(const QString& mode)
{
    if (open_source_.value(QStringLiteral("backend")).toString() != "rtlsdr") {
        return;
    }
    const bool digital = mode == QStringLiteral("digital");
    const bool tuner = mode == QStringLiteral("tuner");
    if (!digital && !tuner && mode != QStringLiteral("manual")) {
        return;
    }

    const std::string uri = open_source_.value(QStringLiteral("uri")).toString().toStdString();
    const FrontEnd now = front_end_of(uri);
    if (now.digital_agc == digital) {
        // The digital AGC is already where it should be, so this is the
        // tuner's loop alone, which is live.
        if (sourceGainAuto() != tuner) {
            setSourceGainAuto(tuner);
        }
        return;
    }

    // A reopen. The gain goes in the URI so the reopened source comes up on
    // the loop that was asked for and, in manual, on the gain on screen now.
    FrontEnd wanted = now;
    wanted.digital_agc = digital;
    const std::string id = open_radio_id(open_source_);
    if (!id.empty()) {
        QSettings().setValue(QString::fromStdString(front_end_key(id, "digitalAgc")), digital);
    }
    std::string next = with_front_end(
        uri, wanted, open_source_.value(QStringLiteral("offsetTuningAvailable")).toBool());
    if (tuner) {
        next = with_query_value(next, "gain", "auto");
    } else if (sourceGainKnown()) {
        next = with_query_value(next, "gain", std::to_string(sourceGainDb()));
    }
    reopenWith(next);
}

void EngineLink::setRadioLabel(const QString& serial, const QString& label)
{
    const std::string key = radio_label_key(serial.toStdString());
    if (key.empty()) {
        return;
    }
    const QString trimmed = label.trimmed();
    QSettings store;
    if (trimmed.isEmpty()) {
        store.remove(QString::fromStdString(key));
    } else {
        store.setValue(QString::fromStdString(key), trimmed);
    }

    // The list carries the label, so it is refreshed in place.
    for (qsizetype i = 0; i < sources_.size(); ++i) {
        QVariantMap row = sources_[i].toMap();
        if (row.value(QStringLiteral("serial")).toString() == serial) {
            row.insert(QStringLiteral("label"), trimmed);
            sources_[i] = row;
        }
    }
    emit sourcesChanged();
}

QString EngineLink::serialProblem(const QString& serial) const
{
    return QString::fromStdString(serial_problem(serial.trimmed().toStdString()));
}

void EngineLink::writeRadioSerial(int index, const QString& serial)
{
    const auto say = [this](const QString& text, bool busy) {
        serial_write_text_ = text;
        serial_write_busy_ = busy;
        emit serialWriteChanged();
    };

    if (serial_write_busy_) {
        return;
    }
    if (index < 0 || static_cast<std::size_t>(index) >= source_rows_.size()) {
        return;
    }
    const QString wanted = serial.trimmed();
    if (const QString problem = serialProblem(wanted); !problem.isEmpty()) {
        say(problem, false);
        return;
    }
    if (sourceOpen()) {
        say(QStringLiteral("close the source first: the engine holds the dongle"), false);
        return;
    }

    // The device index is the body of a listing's rtlsdr://N URI.
    const rpc::SourceDescriptor& row = source_rows_[static_cast<std::size_t>(index)];
    const std::string prefix = "rtlsdr://";
    if (row.backend != "rtlsdr" || row.uri.rfind(prefix, 0) != 0) {
        say(QStringLiteral("only an RTL-SDR has a serial to write"), false);
        return;
    }
    std::string body = row.uri.substr(prefix.size());
    body = body.substr(0, body.find('?'));
    if (body.empty() || body.find_first_not_of("0123456789") != std::string::npos) {
        say(QStringLiteral("this row names its dongle by serial, not by index; "
                           "refresh the list and pick it again"),
            false);
        return;
    }

    const QString tool = QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("revenant-rtl-serial.exe"));
    if (!QFileInfo::exists(tool)) {
        say(QStringLiteral("revenant-rtl-serial.exe is not beside the client"), false);
        return;
    }

    say(QStringLiteral("writing…"), true);
    auto* process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::finished, this,
            [this, process, say](int code, QProcess::ExitStatus status) {
                const QString output = QString::fromLocal8Bit(process->readAll()).trimmed();
                const QString last = output.section(QLatin1Char('\n'), -1).trimmed();
                if (status == QProcess::NormalExit && code == 0) {
                    say(QStringLiteral("written. Unplug the dongle and plug it back in, then "
                                       "refresh the list."),
                        false);
                } else {
                    say(last.isEmpty() ? QStringLiteral("the write failed") : last, false);
                }
                process->deleteLater();
            });
    process->start(tool, {QStringLiteral("set"), QString::fromStdString(body), wanted});
}

}  // namespace revenant::ui
