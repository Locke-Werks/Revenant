// The decoded-message log as a list a QML ListView can hold.
//
// A thin wrapper. The cap, the counts, what a line says and what a copy
// contains are models/decoded_log.h's, with their cases in ui/tests; this
// file tells a view which rows are going and coming, and puts text on the
// clipboard.
//
// WHY A LIST MODEL AND NOT A QVariantList PROPERTY. A property would be
// rebuilt whole on every message and handed to the view as a new model, which
// resets its scroll position: an operator reading line 40 of 2000 would be
// thrown back to the top by every page that arrived. Row insertions and
// removals leave the rest of the view where it was.
//
// Qt thread only, all of it. EngineLink appends from its own drain on the Qt
// thread; nothing here is reached from the Cap'n Proto loop or the supervisor.

#pragma once

#include <cstdint>
#include <vector>

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QtQmlIntegration>

#include "models/decoded_log.h"

namespace revenant::ui {

class DecodedLogModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by EngineLink; see EngineLink::decodedLog.")

    Q_PROPERTY(int count READ count NOTIFY changed)

    // A word or two and a sentence, for a chip saying what the log has lost.
    // Both empty when nothing has been.
    Q_PROPERTY(QString droppedLabel READ droppedLabel NOTIFY changed)
    Q_PROPERTY(QString droppedDetail READ droppedDetail NOTIFY changed)

    Q_PROPERTY(int capacity READ capacity CONSTANT)

public:
    enum Role : int {
        SerialRole = Qt::UserRole + 1,
        TimeRole,
        DecoderRole,
        TextRole,
        ChipRole,
        FieldsRole,
        ExpandedRole,
    };

    explicit DecodedLogModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] int count() const { return static_cast<int>(log_.size()); }
    [[nodiscard]] int capacity() const { return static_cast<int>(log_.capacity()); }
    [[nodiscard]] QString droppedLabel() const
    {
        return QString::fromStdString(log_.dropped_label());
    }
    [[nodiscard]] QString droppedDetail() const
    {
        return QString::fromStdString(log_.dropped_detail());
    }

    // Appends what arrived, in order. `unkept` is how many arrived and were
    // let go before they reached here, because the hand-off itself was full.
    void append(std::vector<DecodedLine> lines, std::uint64_t unkept);

    // The serial the next line takes. Lines are made by the caller, which
    // knows when each one arrived.
    [[nodiscard]] std::uint64_t take_serial() { return next_serial_++; }

    Q_INVOKABLE void clear();
    Q_INVOKABLE void toggleExpanded(int row);

    // The line with its fields under it, and the whole log a line each.
    Q_INVOKABLE void copyLine(int row) const;
    Q_INVOKABLE void copyAll() const;

    // Whether the view is showing the newest line. models/decoded_log.h.
    [[nodiscard]] Q_INVOKABLE bool atTail(double content_y, double view_height,
                                          double content_height) const
    {
        return view_at_tail(content_y, view_height, content_height);
    }

signals:
    void changed();

private:
    DecodedLog log_;
    std::uint64_t next_serial_ = 1;
};

}  // namespace revenant::ui
