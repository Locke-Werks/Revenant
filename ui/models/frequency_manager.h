// The frequency manager's Qt half: the memory file on disk, the list the
// panel draws, the import preview, and the reach into EngineLink for a recall.
//
// Every rule it applies is in a Qt-free header with cases in ui/tests:
// models/memories.h (the model, the file, search, sort, undo, scan lists) and
// models/memory_import.h (the three importers and the CHIRP export). What is
// here is conversion, file I/O and signals, on the rule models/ui_rules.h
// states for itself.
//
// WHERE THE FILE IS. QStandardPaths::AppDataLocation plus memories.json, which
// with the organisation and application names main() sets is
// %APPDATA%\Locke Werks\Revenant\memories.json on Windows. The panel prints
// the path, so an operator backing it up does not have to know that.
//
// WRITTEN WHOLE ON EVERY CHANGE, through QSaveFile, so a crash or a full disk
// mid-write leaves the previous file rather than half of a new one. A
// thousand memories is about 300 KB of JSON, which is a write nobody sees.
//
// A FILE THAT CANNOT BE READ IS NEVER WRITTEN. A memory file that fails to
// parse, or that a newer client wrote, puts the manager in a read-only state
// with the reason on screen. Rewriting it from an empty book would be the one
// way this feature could destroy an operator's list, so it is the one thing
// it refuses outright.
//
// A SMOKE RUN WRITES NOTHING, on the rule main.cpp states for every other
// setting: a run on a developer's machine leaves their memories alone. It
// reads no file unless --memories names one, and never migrates the registry.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <QAbstractListModel>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQmlIntegration>

#include "models/memories.h"
#include "models/memory_import.h"

namespace revenant::ui {

class EngineLink;

class FrequencyManager : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Constructed by main() with the engine link and where the file lives.")

    // The search box, the tag chips and the sort, which together decide what
    // the list shows. See list_memories.
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY viewChanged)
    Q_PROPERTY(QStringList selectedTags READ selectedTags NOTIFY viewChanged)
    Q_PROPERTY(QString sortKey READ sortKey WRITE setSortKey NOTIFY viewChanged)
    Q_PROPERTY(bool descending READ descending WRITE setDescending NOTIFY viewChanged)

    // How many are listed and how many there are.
    Q_PROPERTY(int listed READ listed NOTIFY bookChanged)
    Q_PROPERTY(int total READ total NOTIFY bookChanged)

    // Every tag with its count, as maps of tag, count and selected.
    Q_PROPERTY(QVariantList tags READ tags NOTIFY bookChanged)

    // Bumped on every change to the book, so a binding that asks a question
    // through a method can name it and be asked again.
    Q_PROPERTY(int revision READ revision NOTIFY bookChanged)

    Q_PROPERTY(QString filePath READ filePath CONSTANT)
    Q_PROPERTY(bool writable READ writable NOTIFY statusChanged)

    // The last thing worth saying about the file or an action on it, and a
    // fault that is still true: a file that could not be read or written.
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString fault READ fault NOTIFY statusChanged)

    Q_PROPERTY(bool canUndo READ canUndo NOTIFY bookChanged)
    Q_PROPERTY(QString undoText READ undoText NOTIFY bookChanged)

    // Scan lists, as maps of name, kind, summary and size. The model only;
    // nothing scans yet.
    Q_PROPERTY(QVariantList scanLists READ scanLists NOTIFY bookChanged)

    // An import waiting to be confirmed: where it came from, what it would
    // add (the first few hundred, as maps of label, freq and mode) and every
    // entry it would not add, with the line and the reason.
    Q_PROPERTY(bool importPending READ importPending NOTIFY importChanged)
    Q_PROPERTY(QString importSummary READ importSummary NOTIFY importChanged)
    Q_PROPERTY(QVariantList importAdds READ importAdds NOTIFY importChanged)
    Q_PROPERTY(QVariantList importSkips READ importSkips NOTIFY importChanged)

public:
    enum Role {
        KeyRole = Qt::UserRole + 1,
        NameRole,
        LabelRole,
        FreqHzRole,
        FreqTextRole,
        ModeRole,
        ModeLabelRole,
        TagsRole,
        TagsTextRole,
        NotesRole,
        GroupRole,
        LowRole,
        HighRole,
        UsedTextRole,
    };

    struct Storage {
        // Empty for no file at all.
        QString path;
        bool writable = true;

        // Bring the registry bookmark list across when the file does not
        // exist yet.
        bool migrate = true;
    };

    FrequencyManager(EngineLink& link, Storage storage, QObject* parent = nullptr);

    // Where a normal run keeps the file.
    [[nodiscard]] static QString defaultPath();

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] QString query() const { return query_; }
    void setQuery(const QString& query);
    [[nodiscard]] QStringList selectedTags() const { return selected_tags_; }
    [[nodiscard]] QString sortKey() const;
    void setSortKey(const QString& key);
    [[nodiscard]] bool descending() const { return descending_; }
    void setDescending(bool descending);

    [[nodiscard]] int listed() const { return static_cast<int>(rows_.size()); }
    [[nodiscard]] int total() const { return static_cast<int>(book_.memories.size()); }
    [[nodiscard]] QVariantList tags() const;
    [[nodiscard]] int revision() const { return revision_; }
    [[nodiscard]] QString filePath() const { return storage_.path; }
    [[nodiscard]] bool writable() const { return storage_.writable; }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] QString fault() const { return fault_; }
    [[nodiscard]] bool canUndo() const { return !undo_.empty(); }
    [[nodiscard]] QString undoText() const;
    [[nodiscard]] QVariantList scanLists() const;
    [[nodiscard]] bool importPending() const { return pending_import_.has_value(); }
    [[nodiscard]] QString importSummary() const;
    [[nodiscard]] QVariantList importAdds() const;
    [[nodiscard]] QVariantList importSkips() const;

    // The filter's tag chips.
    Q_INVOKABLE void toggleTag(const QString& tag);
    Q_INVOKABLE void clearTags();

    // Saves the focused receiver as a memory under this name, which may be
    // empty. Answers the row it landed on in the list, or -1 when there was
    // no receiver or it is already saved under that name.
    Q_INVOKABLE int addFromReceiver(const QString& name);

    // Whether a memory sits within tolerance_hz of this frequency. revision is
    // only there so a binding re-asks when the book changes.
    Q_INVOKABLE bool savedNear(double hz, double tolerance_hz, int revision) const;

    // Recalls a memory into the focused receiver, or into a new one in the
    // rack, through EngineLink::recallPlace. Marks it used.
    Q_INVOKABLE void recall(qulonglong key, bool new_receiver);

    // Edits in place, by key. A frequency is read the way the tuning box
    // reads one; tags are typed comma separated. Each answers whether the
    // edit was taken, and a refusal says why in status.
    Q_INVOKABLE bool setName(qulonglong key, const QString& name);
    Q_INVOKABLE bool setFrequency(qulonglong key, const QString& text);
    Q_INVOKABLE bool setMode(qulonglong key, const QString& mode);
    Q_INVOKABLE bool setTags(qulonglong key, const QString& text);
    Q_INVOKABLE bool setNotes(qulonglong key, const QString& notes);
    Q_INVOKABLE bool setGroup(qulonglong key, const QString& group);

    Q_INVOKABLE void remove(qulonglong key);
    Q_INVOKABLE void undo();

    // The row a memory is listed on, or -1.
    Q_INVOKABLE int rowOf(qulonglong key) const;

    // Reads a file and previews what importing it would do; nothing is added
    // until commitImport. location is a path or a file:// URL, which is what
    // a drop hands over.
    Q_INVOKABLE void previewImport(const QString& location);
    Q_INVOKABLE void commitImport();
    Q_INVOKABLE void cancelImport();

    // Writes what is listed now to a file: "json" for Revenant's own format,
    // "chirp" for CHIRP's CSV. Answers whether it was written.
    Q_INVOKABLE bool exportListed(const QString& location, const QString& format);

    // A suggested file for an export, in the user's documents.
    Q_INVOKABLE QString suggestedExportPath(const QString& format) const;

    // A scan list of what is listed now, or a range. Each answers the
    // refusal, or empty when the list was added.
    Q_INVOKABLE QString addScanListFromListed(const QString& name);
    Q_INVOKABLE QString addScanRange(const QString& name, const QString& low,
                                     const QString& high, const QString& step,
                                     const QString& mode);
    Q_INVOKABLE void removeScanList(int index);

    // Every mode a memory can hold, in the selector's order.
    Q_INVOKABLE QStringList modes() const;

signals:
    void viewChanged();
    void bookChanged();
    void statusChanged();
    void importChanged();

private:
    EngineLink& link_;
    Storage storage_;
    MemoryBook book_;

    // Indices into book_.memories, in list order.
    std::vector<std::size_t> rows_;

    QString query_;
    QStringList selected_tags_;
    MemorySort sort_ = MemorySort::Frequency;
    bool descending_ = false;
    int revision_ = 0;
    QString status_;
    QString fault_;

    // Most recent last. Bounded, since each holds a whole memory.
    std::vector<DeletedMemory> undo_;

    struct PendingImport {
        QString source;
        ImportFormat format = ImportFormat::Revenant;
        ImportPlan plan;
        std::vector<SkippedLine> skipped;
    };
    std::optional<PendingImport> pending_import_;

    void load();
    void save();
    void changed();
    void refresh();
    void say(const QString& status);
    [[nodiscard]] Memory* memory(qulonglong key);
    [[nodiscard]] static std::int64_t now();
    [[nodiscard]] static QString local_path(const QString& location);
};

}  // namespace revenant::ui
