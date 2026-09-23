// The frequency manager's Qt half. ui/models/frequency_manager.h holds the
// contract and the reasons; models/memories.h and models/memory_import.h hold
// the rules.

#include "models/frequency_manager.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#include "models/engine_link.h"
#include "models/frequency_entry.h"
#include "models/mode_choice.h"
#include "models/settings.h"

namespace revenant::ui {

namespace {

// Past this an import is refused before it is read. The largest real list
// found while writing the importers, a worldwide shortwave schedule, was
// under 3 MB; this is ten times that.
constexpr qint64 kMaxImportBytes = 32LL * 1024 * 1024;

// How many deletes can be undone, newest first.
constexpr std::size_t kUndoDepth = 50;

// How many rows the import preview draws. The count above it is the whole
// file; the rows are a sample to recognise it by.
constexpr std::size_t kPreviewRows = 300;

[[nodiscard]] QString text(const std::string& value)
{
    return QString::fromStdString(value);
}

[[nodiscard]] QStringList text_list(const std::vector<std::string>& values)
{
    QStringList out;
    out.reserve(static_cast<qsizetype>(values.size()));
    for (const std::string& value : values) {
        out.append(text(value));
    }
    return out;
}

struct SortName {
    MemorySort sort;
    QLatin1StringView name;
};

constexpr SortName kSortNames[] = {
    {MemorySort::Name, QLatin1StringView("name")},
    {MemorySort::Frequency, QLatin1StringView("frequency")},
    {MemorySort::Mode, QLatin1StringView("mode")},
    {MemorySort::Tags, QLatin1StringView("tags")},
    {MemorySort::Used, QLatin1StringView("used")},
};

[[nodiscard]] QVariantMap skipped_row(const SkippedLine& line)
{
    return QVariantMap{{QStringLiteral("line"), static_cast<int>(line.line)},
                       {QStringLiteral("what"), text(line.what)},
                       {QStringLiteral("reason"), text(line.reason)}};
}

}  // namespace

FrequencyManager::FrequencyManager(EngineLink& link, Storage storage, QObject* parent)
    : QAbstractListModel(parent), link_(link), storage_(std::move(storage))
{
    load();
    refresh();
}

QString FrequencyManager::defaultPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QString::fromLatin1(kMemoryFileName.data(),
                                      static_cast<qsizetype>(kMemoryFileName.size())));
}

std::int64_t FrequencyManager::now()
{
    return QDateTime::currentSecsSinceEpoch();
}

QString FrequencyManager::local_path(const QString& location)
{
    const QString trimmed = location.trimmed();
    if (trimmed.startsWith(QLatin1StringView("file:"))) {
        return QUrl(trimmed).toLocalFile();
    }
    return trimmed;
}

// ---------------------------------------------------------------------------
// The file
// ---------------------------------------------------------------------------

void FrequencyManager::load()
{
    if (storage_.path.isEmpty()) {
        storage_.writable = false;
        return;
    }

    QFile file(storage_.path);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) {
            fault_ = QStringLiteral("%1 could not be opened (%2), so nothing will be written to it")
                         .arg(storage_.path, file.errorString());
            storage_.writable = false;
            return;
        }
        const QByteArray bytes = file.readAll();
        auto loaded = memory_book_from_json(std::string_view(bytes.constData(),
                                                             static_cast<std::size_t>(bytes.size())));
        if (!loaded) {
            fault_ = QStringLiteral("%1 could not be read, so nothing will be written to it: %2")
                         .arg(storage_.path, text(loaded.error().message));
            storage_.writable = false;
            return;
        }
        book_ = std::move(loaded->book);
        if (!loaded->skipped.empty()) {
            status_ = QStringLiteral("%1 entries in the file were not loaded; the first: %2")
                          .arg(loaded->skipped.size())
                          .arg(text(loaded->skipped.front().what + ": " +
                                    loaded->skipped.front().reason));
        }
        return;
    }

    if (!storage_.migrate) {
        return;
    }

    // THE BOOKMARK LIST, BROUGHT ACROSS ONCE. Only when there is no memory file
    // yet, so it happens on the first run of this version and never again,
    // and the registry value is left as it was: an older client run after
    // this one still has its list. models/memories.h, migrate_bookmarks, has
    // the rule for what crosses.
    const QString stored = QSettings().value(settings::kBookmarks).toString();
    const Migration migration = migrate_bookmarks(stored.toStdString());
    if (migration.memories.empty() && migration.skipped.empty()) {
        return;
    }
    for (Memory memory : migration.memories) {
        memory.id = book_.next_id++;
        book_.memories.push_back(std::move(memory));
    }
    status_ = QStringLiteral("brought %1 bookmarks across from the old list").arg(migration.memories.size());
    if (!migration.skipped.empty()) {
        status_ += QStringLiteral(", and passed over %1 the old list never showed")
                       .arg(migration.skipped.size());
    }
    save();
}

void FrequencyManager::save()
{
    if (!storage_.writable) {
        return;
    }
    const QFileInfo info(storage_.path);
    if (!QDir().mkpath(info.absolutePath())) {
        fault_ = QStringLiteral("could not make %1").arg(info.absolutePath());
        emit statusChanged();
        return;
    }
    QSaveFile file(storage_.path);
    const std::string body = memory_book_to_json(book_);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(body.data(), static_cast<qint64>(body.size())) != static_cast<qint64>(body.size()) ||
        !file.commit()) {
        fault_ = QStringLiteral("%1 could not be written: %2").arg(storage_.path, file.errorString());
        emit statusChanged();
        return;
    }
    if (!fault_.isEmpty()) {
        fault_.clear();
        emit statusChanged();
    }
}

void FrequencyManager::changed()
{
    save();
    refresh();
}

void FrequencyManager::say(const QString& status)
{
    status_ = status;
    emit statusChanged();
}

// ---------------------------------------------------------------------------
// The list
// ---------------------------------------------------------------------------

void FrequencyManager::refresh()
{
    std::vector<std::string> tags;
    for (const QString& tag : selected_tags_) {
        tags.push_back(tag.toStdString());
    }
    std::vector<std::size_t> rows =
        list_memories(book_, query_.toStdString(), tags, sort_, descending_);

    // The same rows in the same order is an edit, which keeps the view where
    // it is; anything else is a new list.
    if (rows == rows_) {
        if (!rows_.empty()) {
            emit dataChanged(index(0), index(static_cast<int>(rows_.size()) - 1));
        }
    } else {
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
    }
    ++revision_;
    emit bookChanged();
}

int FrequencyManager::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant FrequencyManager::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || static_cast<std::size_t>(index.row()) >= rows_.size()) {
        return {};
    }
    const Memory& memory = book_.memories[rows_[static_cast<std::size_t>(index.row())]];
    switch (role) {
        case KeyRole: return QVariant::fromValue(static_cast<qulonglong>(memory.id));
        case NameRole: return text(memory.name);
        case LabelRole: return text(memory_label(memory));
        case FreqHzRole: return static_cast<double>(memory.freq_hz);
        case FreqTextRole: return text(format_mhz(memory.freq_hz));
        case ModeRole: return text(memory.mode);
        case ModeLabelRole: {
            const std::string_view label = mode_label(memory.mode);
            return QString::fromUtf8(label.data(), static_cast<qsizetype>(label.size()));
        }
        case TagsRole: return text_list(memory.tags);
        case TagsTextRole: return text(join_tags(memory.tags));
        case NotesRole: return text(memory.notes);
        case GroupRole: return text(memory.group);
        case LowRole: return memory.passband_low;
        case HighRole: return memory.passband_high;
        case UsedTextRole:
            return memory.used == 0
                       ? QString()
                       : QDateTime::fromSecsSinceEpoch(memory.used).toLocalTime().toString(
                             QStringLiteral("yyyy-MM-dd HH:mm"));
        default: return {};
    }
}

QHash<int, QByteArray> FrequencyManager::roleNames() const
{
    return {{KeyRole, "key"},         {NameRole, "name"},       {LabelRole, "label"},
            {FreqHzRole, "freqHz"},   {FreqTextRole, "freqText"}, {ModeRole, "mode"},
            {ModeLabelRole, "modeLabel"}, {TagsRole, "tags"},   {TagsTextRole, "tagsText"},
            {NotesRole, "notes"},     {GroupRole, "group"},     {LowRole, "low"},
            {HighRole, "high"},       {UsedTextRole, "usedText"}};
}

void FrequencyManager::setQuery(const QString& query)
{
    if (query == query_) {
        return;
    }
    query_ = query;
    emit viewChanged();
    refresh();
}

QString FrequencyManager::sortKey() const
{
    for (const SortName& entry : kSortNames) {
        if (entry.sort == sort_) {
            return entry.name;
        }
    }
    return {};
}

void FrequencyManager::setSortKey(const QString& key)
{
    for (const SortName& entry : kSortNames) {
        if (key == entry.name && entry.sort != sort_) {
            sort_ = entry.sort;
            emit viewChanged();
            refresh();
            return;
        }
    }
}

void FrequencyManager::setDescending(bool descending)
{
    if (descending == descending_) {
        return;
    }
    descending_ = descending;
    emit viewChanged();
    refresh();
}

QVariantList FrequencyManager::tags() const
{
    QVariantList out;
    for (const TagCount& count : tag_counts(book_)) {
        const QString tag = text(count.tag);
        const bool selected = std::any_of(selected_tags_.begin(), selected_tags_.end(),
                                          [&](const QString& one) {
                                              return one.compare(tag, Qt::CaseInsensitive) == 0;
                                          });
        out.append(QVariantMap{{QStringLiteral("tag"), tag},
                               {QStringLiteral("count"), static_cast<int>(count.count)},
                               {QStringLiteral("selected"), selected}});
    }
    return out;
}

void FrequencyManager::toggleTag(const QString& tag)
{
    const auto found = std::find_if(selected_tags_.begin(), selected_tags_.end(),
                                    [&](const QString& one) {
                                        return one.compare(tag, Qt::CaseInsensitive) == 0;
                                    });
    if (found != selected_tags_.end()) {
        selected_tags_.erase(found);
    } else {
        selected_tags_.append(tag);
    }
    emit viewChanged();
    refresh();
}

void FrequencyManager::clearTags()
{
    if (selected_tags_.isEmpty()) {
        return;
    }
    selected_tags_.clear();
    emit viewChanged();
    refresh();
}

int FrequencyManager::rowOf(qulonglong key) const
{
    for (std::size_t row = 0; row < rows_.size(); ++row) {
        if (book_.memories[rows_[row]].id == key) {
            return static_cast<int>(row);
        }
    }
    return -1;
}

Memory* FrequencyManager::memory(qulonglong key)
{
    return find_memory(book_, static_cast<std::uint64_t>(key));
}

QStringList FrequencyManager::modes() const
{
    QStringList out;
    for (const ModeChoice& choice : kModeChoices) {
        out.append(QString::fromUtf8(choice.name.data(), static_cast<qsizetype>(choice.name.size())));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Saving, recalling, editing
// ---------------------------------------------------------------------------

int FrequencyManager::addFromReceiver(const QString& name)
{
    if (link_.receiverId() == 0) {
        say(QStringLiteral("no receiver to save"));
        return -1;
    }
    Memory memory;
    memory.name = name.trimmed().toStdString();
    memory.freq_hz = static_cast<std::int64_t>(std::llround(link_.receiverCenterHz()));
    memory.mode = link_.receiverDemod().toStdString();
    memory.passband_low = link_.receiverPassbandLow();
    memory.passband_high = link_.receiverPassbandHigh();
    if (const std::string problem = memory_problem(memory); !problem.empty()) {
        say(QStringLiteral("this receiver has %1 to save").arg(text(problem)));
        return -1;
    }
    if (duplicate_of(book_, memory.freq_hz, memory.name) != nullptr) {
        say(QStringLiteral("%1 is already saved").arg(text(memory_label(memory))));
        return -1;
    }
    const std::uint64_t id = add_memory(book_, std::move(memory), now());
    say(QStringLiteral("saved %1").arg(text(memory_label(*find_memory(book_, id)))));
    changed();
    return rowOf(id);
}

bool FrequencyManager::savedNear(double hz, double tolerance_hz, int) const
{
    return memory_near(book_, static_cast<std::int64_t>(std::llround(hz)),
                       static_cast<std::int64_t>(std::llround(std::max(tolerance_hz, 0.0)))) != nullptr;
}

void FrequencyManager::recall(qulonglong key, bool new_receiver)
{
    Memory* target = memory(key);
    if (target == nullptr) {
        return;
    }
    target->used = now();
    const Memory copy = *target;
    changed();
    link_.recallPlace(static_cast<double>(copy.freq_hz), text(copy.mode), copy.passband_low,
                      copy.passband_high, text(copy.name), new_receiver);
}

bool FrequencyManager::setName(qulonglong key, const QString& name)
{
    Memory* target = memory(key);
    if (target == nullptr) {
        return false;
    }
    const std::string value = name.trimmed().toStdString();
    if (value == target->name) {
        return true;
    }
    target->name = value;
    changed();
    return true;
}

bool FrequencyManager::setFrequency(qulonglong key, const QString& typed)
{
    Memory* target = memory(key);
    if (target == nullptr) {
        return false;
    }
    const auto parsed = parse_frequency(typed.toStdString(), BareNumber::Megahertz);
    if (!parsed || parsed->hertz <= 0) {
        say(QStringLiteral("\"%1\" is not a frequency").arg(typed.trimmed()));
        return false;
    }
    if (parsed->hertz == target->freq_hz) {
        return true;
    }
    target->freq_hz = parsed->hertz;
    changed();
    return true;
}

bool FrequencyManager::setMode(qulonglong key, const QString& mode)
{
    Memory* target = memory(key);
    const std::string value = mode.toStdString();
    if (target == nullptr || !memory_mode_known(value)) {
        return false;
    }
    if (value == target->mode) {
        return true;
    }
    // A mode's edges belong to it; the new mode takes its own default.
    target->mode = value;
    target->passband_low = 0;
    target->passband_high = 0;
    changed();
    return true;
}

bool FrequencyManager::setTags(qulonglong key, const QString& typed)
{
    Memory* target = memory(key);
    if (target == nullptr) {
        return false;
    }
    std::vector<std::string> tags = split_tags(typed.toStdString());
    if (tags == target->tags) {
        return true;
    }
    target->tags = std::move(tags);
    // A chip for a tag nobody carries any more would filter to nothing.
    selected_tags_.removeIf([&](const QString& chosen) {
        return std::none_of(book_.memories.begin(), book_.memories.end(), [&](const Memory& one) {
            return memory_has_tags(one, {chosen.toStdString()});
        });
    });
    changed();
    return true;
}

bool FrequencyManager::setNotes(qulonglong key, const QString& notes)
{
    Memory* target = memory(key);
    if (target == nullptr) {
        return false;
    }
    target->notes = notes.toStdString();
    changed();
    return true;
}

bool FrequencyManager::setGroup(qulonglong key, const QString& group)
{
    Memory* target = memory(key);
    if (target == nullptr) {
        return false;
    }
    target->group = group.trimmed().toStdString();
    changed();
    return true;
}

// ---------------------------------------------------------------------------
// Delete and undo
// ---------------------------------------------------------------------------

void FrequencyManager::remove(qulonglong key)
{
    auto gone = delete_memory(book_, static_cast<std::uint64_t>(key));
    if (!gone) {
        return;
    }
    say(QStringLiteral("deleted %1").arg(text(memory_label(gone->memory))));
    undo_.push_back(std::move(*gone));
    if (undo_.size() > kUndoDepth) {
        undo_.erase(undo_.begin());
    }
    changed();
}

QString FrequencyManager::undoText() const
{
    return undo_.empty() ? QString()
                         : QStringLiteral("undo deleting %1").arg(text(memory_label(undo_.back().memory)));
}

void FrequencyManager::undo()
{
    if (undo_.empty()) {
        return;
    }
    DeletedMemory gone = std::move(undo_.back());
    undo_.pop_back();
    say(QStringLiteral("put back %1").arg(text(memory_label(gone.memory))));
    restore_memory(book_, std::move(gone));
    changed();
}

// ---------------------------------------------------------------------------
// Import and export
// ---------------------------------------------------------------------------

void FrequencyManager::previewImport(const QString& location)
{
    const QString path = local_path(location);
    pending_import_.reset();
    if (path.isEmpty()) {
        say(QStringLiteral("name a file to import"));
        emit importChanged();
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        say(QStringLiteral("%1 could not be opened: %2").arg(path, file.errorString()));
        emit importChanged();
        return;
    }
    if (file.size() > kMaxImportBytes) {
        say(QStringLiteral("%1 is larger than any memory list this reads").arg(path));
        emit importChanged();
        return;
    }
    const QByteArray bytes = file.readAll();
    const QString name = QFileInfo(path).fileName();
    auto result = import_memories(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())),
                                  name.toStdString());
    if (!result) {
        say(QStringLiteral("%1 was not imported: %2").arg(name, text(result.error().message)));
        emit importChanged();
        return;
    }
    PendingImport pending;
    pending.source = name;
    pending.format = result->format;
    pending.plan = plan_import(book_, *result);
    pending.skipped = std::move(result->skipped);
    pending_import_ = std::move(pending);
    emit importChanged();
}

QString FrequencyManager::importSummary() const
{
    if (!pending_import_) {
        return {};
    }
    const std::string_view format = import_format_name(pending_import_->format);
    return QStringLiteral("%1, read as %2: %3 to add, %4 already here, %5 skipped")
        .arg(pending_import_->source,
             QString::fromUtf8(format.data(), static_cast<qsizetype>(format.size())))
        .arg(pending_import_->plan.to_add.size())
        .arg(pending_import_->plan.duplicates.size())
        .arg(pending_import_->skipped.size());
}

QVariantList FrequencyManager::importAdds() const
{
    QVariantList out;
    if (!pending_import_) {
        return out;
    }
    const auto& adds = pending_import_->plan.to_add;
    for (std::size_t i = 0; i < adds.size() && i < kPreviewRows; ++i) {
        const std::string_view mode = mode_label(adds[i].mode);
        out.append(QVariantMap{
            {QStringLiteral("label"), text(memory_label(adds[i]))},
            {QStringLiteral("freqText"), text(format_mhz(adds[i].freq_hz))},
            {QStringLiteral("mode"), QString::fromUtf8(mode.data(), static_cast<qsizetype>(mode.size()))},
            {QStringLiteral("group"), text(adds[i].group)}});
    }
    return out;
}

QVariantList FrequencyManager::importSkips() const
{
    QVariantList out;
    if (!pending_import_) {
        return out;
    }
    for (const SkippedLine& line : pending_import_->skipped) {
        out.append(skipped_row(line));
    }
    for (const SkippedLine& line : pending_import_->plan.duplicates) {
        out.append(skipped_row(line));
    }
    return out;
}

void FrequencyManager::commitImport()
{
    if (!pending_import_) {
        return;
    }
    PendingImport pending = std::move(*pending_import_);
    pending_import_.reset();
    const std::size_t added = apply_import(book_, std::move(pending.plan), now());
    say(QStringLiteral("imported %1 from %2").arg(added).arg(pending.source));
    emit importChanged();
    changed();
}

void FrequencyManager::cancelImport()
{
    if (!pending_import_) {
        return;
    }
    pending_import_.reset();
    emit importChanged();
}

QString FrequencyManager::suggestedExportPath(const QString& format) const
{
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return QDir(documents).filePath(format == QLatin1StringView("chirp")
                                        ? QStringLiteral("revenant-memories.csv")
                                        : QStringLiteral("revenant-memories.json"));
}

bool FrequencyManager::exportListed(const QString& location, const QString& format)
{
    const QString path = local_path(location);
    if (path.isEmpty()) {
        say(QStringLiteral("name a file to export to"));
        return false;
    }
    std::string body;
    QString note;
    if (format == QLatin1StringView("chirp")) {
        std::vector<Memory> listed;
        listed.reserve(rows_.size());
        for (const std::size_t row : rows_) {
            listed.push_back(book_.memories[row]);
        }
        ChirpExport exported = export_chirp_csv(listed);
        body = std::move(exported.text);
        if (!exported.skipped.empty()) {
            note = QStringLiteral(", leaving out %1 CHIRP has no mode for, the first %2")
                       .arg(exported.skipped.size())
                       .arg(text(exported.skipped.front().what));
        }
    } else {
        body = export_revenant_json(book_, rows_);
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(body.data(), static_cast<qint64>(body.size())) != static_cast<qint64>(body.size()) ||
        !file.commit()) {
        say(QStringLiteral("%1 could not be written: %2").arg(path, file.errorString()));
        return false;
    }
    say(QStringLiteral("wrote %1 listed to %2%3").arg(rows_.size()).arg(path, note));
    return true;
}

// ---------------------------------------------------------------------------
// Scan lists
// ---------------------------------------------------------------------------

QVariantList FrequencyManager::scanLists() const
{
    QVariantList out;
    for (const ScanList& list : book_.scan_lists) {
        const std::int64_t size = scan_list_size(book_, list);
        QString summary;
        if (list.kind == ScanList::Kind::Range) {
            summary = QStringLiteral("%1 to %2 MHz in %3 Hz steps, %4, %5 channels")
                          .arg(text(format_mhz(list.low_hz)), text(format_mhz(list.high_hz)))
                          .arg(list.step_hz)
                          .arg(list.mode.empty() ? QStringLiteral("any mode") : text(list.mode))
                          .arg(size);
        } else {
            summary = QStringLiteral("%1 memories").arg(size);
        }
        out.append(QVariantMap{{QStringLiteral("name"), text(list.name)},
                               {QStringLiteral("kind"),
                                list.kind == ScanList::Kind::Range ? QStringLiteral("range")
                                                                   : QStringLiteral("memories")},
                               {QStringLiteral("summary"), summary},
                               {QStringLiteral("size"), static_cast<double>(size)}});
    }
    return out;
}

QString FrequencyManager::addScanListFromListed(const QString& name)
{
    ScanList list;
    list.name = name.trimmed().toStdString();
    list.kind = ScanList::Kind::Memories;
    for (const std::size_t row : rows_) {
        list.members.push_back(book_.memories[row].id);
    }
    if (list.members.empty()) {
        return QStringLiteral("nothing is listed to make a scan list of");
    }
    if (std::string problem = scan_list_problem(list); !problem.empty()) {
        return text(problem);
    }
    book_.scan_lists.push_back(std::move(list));
    say(QStringLiteral("made scan list %1").arg(name.trimmed()));
    changed();
    return {};
}

QString FrequencyManager::addScanRange(const QString& name, const QString& low, const QString& high,
                                       const QString& step, const QString& mode)
{
    const auto low_hz = parse_frequency(low.toStdString(), BareNumber::Megahertz);
    const auto high_hz = parse_frequency(high.toStdString(), BareNumber::Megahertz);
    // A step is written in hertz unless it says otherwise: "12500" and
    // "12.5k" are the same step, and "12.5" meaning megahertz would never be.
    const auto step_hz = parse_frequency(step.toStdString(), BareNumber::Hertz);
    if (!low_hz || !high_hz) {
        return QStringLiteral("both ends need to be frequencies");
    }
    if (!step_hz) {
        return QStringLiteral("the step needs to be a frequency, as 12500 or 12.5k");
    }
    ScanList list;
    list.name = name.trimmed().toStdString();
    list.kind = ScanList::Kind::Range;
    list.low_hz = low_hz->hertz;
    list.high_hz = high_hz->hertz;
    list.step_hz = step_hz->hertz;
    list.mode = mode.toStdString();
    if (std::string problem = scan_list_problem(list); !problem.empty()) {
        return text(problem);
    }
    book_.scan_lists.push_back(std::move(list));
    say(QStringLiteral("made scan list %1").arg(name.trimmed()));
    changed();
    return {};
}

void FrequencyManager::removeScanList(int index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= book_.scan_lists.size()) {
        return;
    }
    book_.scan_lists.erase(book_.scan_lists.begin() + index);
    changed();
}

}  // namespace revenant::ui
