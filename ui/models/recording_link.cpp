// RecordingLink: the recording section's state, and the strip's.
//
// Nothing here decides anything a test could not reach. It reads a header
// through recording_header.h, plans through recording_plan.h, keeps the list
// through recent_recordings.h and phrases the strip through
// recording_status.h, and converts between those and what QML can hold.

#include "models/recording_link.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include "models/engine_link.h"
#include "models/frequency_entry.h"
#include "models/settings.h"

namespace revenant::ui {

namespace {

[[nodiscard]] QString qs(const std::string& text) { return QString::fromStdString(text); }

// A box's text as a RecordingChoice field: what it parses to, and whether
// anything was typed at all. The centre takes the tuning box's rule, where a
// bare number under a million is megahertz; the rate takes the rate box's,
// where a bare number is always hertz. See frequency_entry.h's BareNumber.
void read_box(const QString& text, BareNumber bare, std::optional<std::int64_t>& value,
              bool& typed)
{
    const QString trimmed = text.trimmed();
    typed = !trimmed.isEmpty();
    value.reset();
    if (!typed) {
        return;
    }
    if (const auto parsed = parse_frequency(trimmed.toStdString(), bare)) {
        value = parsed->hertz;
    }
}

// Every key the preview carries, empty. Held before anything is chosen so a
// binding never reads undefined out of it, which QML reports as a warning and
// a smoke run counts as a failure.
[[nodiscard]] QVariantMap empty_preview()
{
    QVariantMap map;
    for (const char* key : {"name", "folder", "container", "channels", "rate", "formatName",
                            "format", "center", "refusal"}) {
        map.insert(QString::fromLatin1(key), QString());
    }
    map.insert(QStringLiteral("raw"), false);
    map.insert(QStringLiteral("needsRate"), false);
    map.insert(QStringLiteral("needsCenter"), false);
    map.insert(QStringLiteral("notes"), QStringList());
    return map;
}

}  // namespace

RecordingLink::RecordingLink(EngineLink& engine, QString engine_address, bool persist,
                             QObject* parent)
    : QObject(parent), engine_(engine), engine_address_(std::move(engine_address)),
      persist_(persist), preview_(empty_preview())
{
    load_recent();

    connect(&engine_, &EngineLink::openedSourceChanged, this, &RecordingLink::refresh_playback);
    connect(&engine_, &EngineLink::pacingChanged, this, &RecordingLink::refresh_playback);
    connect(&engine_, &EngineLink::runningChanged, this, &RecordingLink::refresh_playback);
    connect(&engine_, &EngineLink::connectionChanged, this, &RecordingLink::refresh_playback);
    replan();
}

QStringList RecordingLink::fileFilters() const
{
    QStringList out;
    for (const std::string& filter : recording_file_filters()) {
        out.append(qs(filter));
    }
    return out;
}

QUrl RecordingLink::folder() const
{
    for (const RecentRecording& entry : recent_) {
        const QFileInfo info(qs(entry.path));
        if (info.dir().exists()) {
            return QUrl::fromLocalFile(info.absolutePath());
        }
    }
    return {};
}

QVariantList RecordingLink::recent() const
{
    const RecordingFiles disk = disk_recording_files();
    QVariantList out;
    for (const RecentRecording& entry : existing_recordings(recent_, disk.exists)) {
        const QFileInfo info(qs(entry.path));
        out.append(QVariantMap{
            {QStringLiteral("path"), qs(entry.path)},
            {QStringLiteral("name"), info.fileName()},
            {QStringLiteral("folder"), QDir::toNativeSeparators(info.absolutePath())},
            {QStringLiteral("centerText"), qs(entry.center_text)},
            {QStringLiteral("rateText"), qs(entry.rate_text)},
            {QStringLiteral("formatText"), qs(entry.format_text)},
        });
    }
    return out;
}

void RecordingLink::setCenterText(const QString& text)
{
    if (text == center_text_) {
        return;
    }
    center_text_ = text;
    replan();
}

void RecordingLink::setRateText(const QString& text)
{
    if (text == rate_text_) {
        return;
    }
    rate_text_ = text;
    replan();
}

void RecordingLink::setFormatText(const QString& text)
{
    if (text == format_text_) {
        return;
    }
    format_text_ = text;
    replan();
}

void RecordingLink::chooseUrl(const QUrl& url)
{
    // A fresh pick from the dialog starts with empty boxes. The previous
    // file's centre is about a different recording, and carrying it over is
    // how a 20 m file opens at a 40 m centre.
    center_text_.clear();
    rate_text_.clear();
    format_text_.clear();
    choose_path(url.toLocalFile());
}

void RecordingLink::chooseRecent(int index)
{
    const QVariantList rows = recent();
    if (index < 0 || index >= rows.size()) {
        return;
    }
    const QVariantMap row = rows.at(index).toMap();
    center_text_ = row.value(QStringLiteral("centerText")).toString();
    rate_text_ = row.value(QStringLiteral("rateText")).toString();
    format_text_ = row.value(QStringLiteral("formatText")).toString();
    choose_path(row.value(QStringLiteral("path")).toString());
}

void RecordingLink::choose_path(const QString& path)
{
    if (path.isEmpty()) {
        return;
    }

    // Absolute, and with forward slashes, which is what QFileInfo hands back
    // and what the URI is built from. A relative path from a command line
    // would otherwise be resolved against the ENGINE's working directory.
    const QString absolute = QFileInfo(path).absoluteFilePath();
    header_ = read_recording_header(absolute.toStdString(), disk_recording_files());
    chosen_ = true;

    // PREFILLED FROM THE FILE WHEN IT RECORDS A CENTRE, in megahertz with six
    // places, which the box's grammar reads back as exactly the recorded hertz
    // at any frequency, including one under a megahertz where a bare hertz
    // figure would be read as megahertz. Left as typed when the operator or a
    // recent entry already put something there, so a disagreement is refused
    // in words rather than silently overwritten.
    if (header_.has_center && center_text_.trimmed().isEmpty()) {
        center_text_ = qs(format_mhz(header_.center_hz));
    }

    const QFileInfo info(absolute);
    QVariantMap map;
    map.insert(QStringLiteral("name"), info.fileName());
    map.insert(QStringLiteral("folder"), QDir::toNativeSeparators(info.absolutePath()));
    map.insert(QStringLiteral("container"), qs(header_.container));
    map.insert(QStringLiteral("channels"), qs(header_.channels));
    map.insert(QStringLiteral("raw"), header_.kind == RecordingKind::Raw);
    map.insert(QStringLiteral("needsRate"), !header_.has_rate);
    map.insert(QStringLiteral("needsCenter"), !header_.has_center);
    map.insert(QStringLiteral("rate"), header_.has_rate
                                           ? QStringLiteral("%1 S/s").arg(header_.rate)
                                           : QStringLiteral("not in the file"));
    map.insert(QStringLiteral("formatName"),
               QString::fromLatin1(recording_format_name(header_.format)));
    map.insert(QStringLiteral("format"),
               header_.format != RecordingFormat::Unknown
                   ? QStringLiteral("%1, %2-bit")
                         .arg(QString::fromLatin1(recording_format_name(header_.format)))
                         .arg(recording_bits_per_component(header_.format))
                   : QStringLiteral("not in the name"));
    map.insert(QStringLiteral("center"),
               header_.has_center
                   ? QStringLiteral("%1 MHz, from the file").arg(qs(format_mhz(header_.center_hz)))
                   : QStringLiteral("not in the file"));

    QStringList notes;
    for (const std::string& note : header_.notes) {
        notes.append(qs(note));
    }

    // THE ONE THING THIS PREVIEW CANNOT KNOW, said when it applies. The path
    // is opened by the engine on the engine's machine; see the note at the
    // top of recording_header.h.
    if (!engine_is_local(engine_address_.toStdString())) {
        notes.append(QStringLiteral(
            "the engine is at %1 and opens this path on its own machine, which this preview "
            "has not read")
                         .arg(engine_address_));
    }
    map.insert(QStringLiteral("notes"), notes);
    map.insert(QStringLiteral("refusal"), qs(header_.refusal));
    preview_ = map;

    guess_text_.clear();
    guess_hz_ = 0.0;
    if (!header_.has_center && header_.readable()) {
        if (const auto guess = guess_center_from_name(header_.path)) {
            guess_hz_ = static_cast<double>(guess->hz);
            guess_text_ = QStringLiteral("%1 MHz, a guess: %2")
                              .arg(qs(format_mhz(guess->hz)), qs(guess->why));
        }
    }

    emit chosenChanged();
    replan();
}

void RecordingLink::useGuess()
{
    if (guess_hz_ <= 0.0) {
        return;
    }
    // In hertz, so the box holds exactly the number the guess was and the
    // bare-number rule cannot read it as anything else.
    setCenterText(QString::number(static_cast<qlonglong>(guess_hz_)));
}

void RecordingLink::replan()
{
    RecordingChoice choice;
    read_box(center_text_, BareNumber::Megahertz, choice.center_hz, choice.center_typed);
    read_box(rate_text_, BareNumber::Hertz, choice.rate, choice.rate_typed);
    choice.format = recording_format_from_name(format_text_.trimmed().toStdString());

    plan_ = chosen_ ? plan_recording_open(header_, choice) : RecordingPlan{};
    if (!chosen_) {
        plan_.blocker = "choose a recording first";
    }

    QVariantMap map;
    map.insert(QStringLiteral("ready"), plan_.ready);
    map.insert(QStringLiteral("uri"), qs(plan_.uri));
    map.insert(QStringLiteral("blocker"), qs(plan_.blocker));
    map.insert(QStringLiteral("length"),
               qs(format_recording_length(plan_.length_samples, plan_.rate)));
    plan_map_ = map;
    emit entryChanged();
}

void RecordingLink::open()
{
    if (!plan_.ready) {
        return;
    }

    opened_uri_ = qs(plan_.uri);
    opened_plan_ = OpenedRecording{plan_.length_samples, plan_.rate,
                                   recording_format_name(plan_.format), plan_.center_hz};
    mismatch_.clear();

    // THE RADIOS' PATH, UNCHANGED. EngineLink::openSource closes whatever is
    // open and opens this, and note_source_epoch then does for a recording
    // what it does for a radio: receivers, audio, decoders, RDS and the
    // detector go, the waterfall starts again, and the picker's listing is
    // refreshed.
    engine_.openSource(opened_uri_);

    recent_ = remember_recording(
        std::move(recent_),
        RecentRecording{header_.path, center_text_.trimmed().toStdString(),
                        rate_text_.trimmed().toStdString(), format_text_.trimmed().toStdString()});
    store_recent();
    emit recentChanged();
}

QStringList RecordingLink::paceOptions() const
{
    QStringList out;
    for (const std::string_view option : kPaceOptions) {
        out.append(QString::fromLatin1(option.data(), static_cast<qsizetype>(option.size())));
    }
    return out;
}

void RecordingLink::setPace(const QString& option)
{
    if (const auto pace = pace_for_option(option.toStdString())) {
        engine_.setSourcePace(*pace);
    }
}

QString RecordingLink::openAtStartup(const QStringList& arguments)
{
    if (arguments.isEmpty()) {
        return {};
    }

    for (qsizetype i = 0; i + 1 < arguments.size(); ++i) {
        const RecordingArgument earlier = split_recording_argument(arguments.at(i).toStdString());
        recent_ = remember_recording(
            std::move(recent_),
            RecentRecording{QFileInfo(qs(earlier.path)).absoluteFilePath().toStdString(),
                            earlier.center_text, {}, {}});
    }
    store_recent();
    emit recentChanged();

    const RecordingArgument last = split_recording_argument(arguments.last().toStdString());
    center_text_ = qs(last.center_text);
    rate_text_.clear();
    format_text_.clear();
    choose_path(qs(last.path));

    if (!plan_.ready) {
        return QStringLiteral("--open-recording %1: %2").arg(arguments.last(), qs(plan_.blocker));
    }
    open();
    return {};
}

void RecordingLink::refresh_playback()
{
    const QVariantMap open = engine_.openedSource();
    const bool is_file = open.value(QStringLiteral("backend")).toString() == QLatin1String("file");
    const auto length = open.value(QStringLiteral("lengthSamples")).toULongLong();
    const bool playing = engine_.connected() && is_file && length > 0;

    PlaybackSample sample;
    sample.delivered = engine_.sourceSamplesDelivered();
    sample.length = length;
    sample.rate = open.value(QStringLiteral("rate")).toLongLong();
    sample.paced_by = engine_.sourcePacedBy();
    sample.realtime_factor = engine_.realtimeFactor();
    sample.running = engine_.engineRunning();
    const PlaybackLine line = describe_playback(sample);

    // Compared once the engine's descriptor is for the URI this window sent,
    // and against the centre the engine reports, which EngineInfo carries and
    // the descriptor does not.
    QString mismatch;
    if (playing && !opened_uri_.isEmpty() &&
        open.value(QStringLiteral("uri")).toString() == opened_uri_) {
        const OpenedRecording engine{
            length, sample.rate, open.value(QStringLiteral("format")).toString().toStdString(),
            static_cast<std::int64_t>(engine_.sourceCenterHz())};
        mismatch = qs(compare_opened_recording(opened_plan_, engine));
    }

    playing_ = playing;
    playing_name_ = open.value(QStringLiteral("displayName")).toString();
    position_text_ = qs(line.position);
    pace_text_ = qs(line.pace);
    pace_option_ = qs(pace_option_for(sample.paced_by));
    pace_fault_ = engine_.sourcePaceFault();
    ended_ = line.ended;
    fraction_ = line.fraction;
    mismatch_ = mismatch;
    emit playbackChanged();
}

void RecordingLink::load_recent()
{
    const QSettings store;
    const QJsonDocument document =
        QJsonDocument::fromJson(store.value(settings::kRecentRecordings).toString().toUtf8());
    recent_.clear();
    for (const QJsonValue& value : document.array()) {
        const QJsonObject entry = value.toObject();
        RecentRecording row;
        row.path = entry.value(QStringLiteral("path")).toString().toStdString();
        row.center_text = entry.value(QStringLiteral("center")).toString().toStdString();
        row.rate_text = entry.value(QStringLiteral("rate")).toString().toStdString();
        row.format_text = entry.value(QStringLiteral("format")).toString().toStdString();
        if (!row.path.empty() && recent_.size() < kRecentRecordingsKept) {
            recent_.push_back(std::move(row));
        }
    }
}

void RecordingLink::store_recent()
{
    if (!persist_) {
        return;
    }
    QJsonArray array;
    for (const RecentRecording& entry : recent_) {
        array.append(QJsonObject{
            {QStringLiteral("path"), qs(entry.path)},
            {QStringLiteral("center"), qs(entry.center_text)},
            {QStringLiteral("rate"), qs(entry.rate_text)},
            {QStringLiteral("format"), qs(entry.format_text)},
        });
    }
    QSettings store;
    store.setValue(settings::kRecentRecordings,
                   QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
}

}  // namespace revenant::ui
