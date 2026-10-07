// The call table, Calls and Groups, as two lists a QML ListView can hold.
//
// A thin wrapper over models/call_log.h, which has every rule and its cases
// in ui/tests/test_call_log.cpp. This file tells the views which rows came
// and went, and ends calls that fell silent on a timer of its own, since
// silence is the one ending no message announces.
//
// Qt thread only. EngineLink feeds it from its drain on the Qt thread.

#pragma once

#include <cstdint>
#include <vector>

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <QtQmlIntegration>

#include "models/call_log.h"

namespace revenant::ui {

class CallLogModel;

class CallRowsModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by CallLogModel.")

    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role : int {
        ProtocolRole = Qt::UserRole + 1,
        SystemRole,
        TargetRole,
        SourceRole,
        GroupRole,
        EncryptedRole,
        EmergencyRole,
        FrequencyRole,
        VrxRole,
        SlotRole,
        OwnerRole,
        FirstHeardRole,
        LastHeardRole,
        DurationRole,
        ActiveRole,
        GrantedRole,
        CountRole,
    };

    explicit CallRowsModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] int count() const { return static_cast<int>(shown_.size()); }

    // Brings the view to `rows`, which only ever gains rows at the front and
    // loses them at the back, as CallLog's two lists do.
    void sync(const std::deque<CallRow>& rows);

signals:
    void countChanged();

private:
    std::vector<CallRow> shown_;
};

class CallLogModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by EngineLink; see EngineLink::callLog.")

    Q_PROPERTY(revenant::ui::CallRowsModel* calls READ calls CONSTANT)
    Q_PROPERTY(revenant::ui::CallRowsModel* groups READ groups CONSTANT)

    // Digital voice decoders feeding the table right now, across every
    // receiver, which is what the pane shows itself on.
    Q_PROPERTY(int feeds READ feeds NOTIFY feedsChanged)

    Q_PROPERTY(int capacity READ capacity CONSTANT)

public:
    explicit CallLogModel(QObject* parent = nullptr);

    [[nodiscard]] CallRowsModel* calls() { return &calls_; }
    [[nodiscard]] CallRowsModel* groups() { return &groups_; }
    [[nodiscard]] int feeds() const { return feeds_; }
    [[nodiscard]] int capacity() const { return static_cast<int>(kCallLogCapacity); }

    void set_feeds(int feeds);
    void apply(const std::vector<CallFrame>& frames);

    Q_INVOKABLE void clear();

signals:
    void feedsChanged();

private:
    void tick();
    void publish();

    CallLog log_;
    CallRowsModel calls_;
    CallRowsModel groups_;
    QTimer timer_;
    int feeds_ = 0;
};

}  // namespace revenant::ui
