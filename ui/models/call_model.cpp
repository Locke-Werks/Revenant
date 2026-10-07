#include "models/call_model.h"

#include <QDateTime>

namespace revenant::ui {
namespace {

[[nodiscard]] QString clock_text(std::int64_t ms)
{
    if (ms == 0) {
        return {};
    }
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("HH:mm:ss"));
}

[[nodiscard]] QString frequency_text(std::int64_t hz)
{
    if (hz == 0) {
        return {};
    }
    return QString::number(static_cast<double>(hz) / 1e6, 'f', 5);
}

}  // namespace

CallRowsModel::CallRowsModel(QObject* parent) : QAbstractListModel(parent) {}

int CallRowsModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return count();
}

QVariant CallRowsModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) {
        return {};
    }
    const CallRow& row = shown_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case ProtocolRole: return QString::fromStdString(row.protocol);
        case SystemRole: return QString::fromStdString(row.system);
        case TargetRole: return QString::fromStdString(row.target);
        case SourceRole: return QString::fromStdString(row.source);
        case GroupRole: return row.group;
        case EncryptedRole: return row.encrypted;
        case EmergencyRole: return row.emergency;
        case FrequencyRole: return frequency_text(row.frequency_hz);
        case VrxRole: return QVariant::fromValue(static_cast<qulonglong>(row.vrx));
        case SlotRole: return row.slot;
        case OwnerRole: return QString::fromStdString(row.owner);
        case FirstHeardRole: return clock_text(row.first_ms);
        case LastHeardRole: return clock_text(row.last_ms);
        case DurationRole:
            return QStringLiteral("%1 s").arg(
                static_cast<double>(row.last_ms - row.first_ms) / 1000.0, 0, 'f', 1);
        case ActiveRole: return row.active;
        case GrantedRole: return row.granted;
        case CountRole: return QVariant::fromValue(static_cast<qulonglong>(row.count));
        default: return {};
    }
}

QHash<int, QByteArray> CallRowsModel::roleNames() const
{
    return {
        {ProtocolRole, "protocol"},   {SystemRole, "system"},       {TargetRole, "target"},
        {SourceRole, "source"},       {GroupRole, "group"},         {EncryptedRole, "encrypted"},
        {EmergencyRole, "emergency"}, {FrequencyRole, "frequency"}, {VrxRole, "vrx"},
        {SlotRole, "slot"},           {OwnerRole, "owner"},         {FirstHeardRole, "firstHeard"},
        {LastHeardRole, "lastHeard"}, {DurationRole, "duration"},   {ActiveRole, "active"},
        {GrantedRole, "granted"},     {CountRole, "count"},
    };
}

void CallRowsModel::sync(const std::deque<CallRow>& rows)
{
    // Row insertions and removals rather than a reset, so a view the
    // operator scrolled down stays where they put it.
    std::size_t fresh = rows.size();
    if (!shown_.empty()) {
        const std::uint64_t head = shown_.front().serial;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].serial == head) {
                fresh = i;
                break;
            }
        }
    }
    if (!shown_.empty() && fresh == rows.size()) {
        // Everything shown has gone at once, which only a clear does.
        beginResetModel();
        shown_.assign(rows.begin(), rows.end());
        endResetModel();
        emit countChanged();
        return;
    }

    const std::size_t kept = rows.size() - fresh;
    const int before = count();
    if (kept < shown_.size()) {
        beginRemoveRows(QModelIndex(), static_cast<int>(kept), count() - 1);
        shown_.resize(kept);
        endRemoveRows();
    }
    if (fresh > 0) {
        beginInsertRows(QModelIndex(), 0, static_cast<int>(fresh) - 1);
        shown_.insert(shown_.begin(), rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(fresh));
        endInsertRows();
    }
    for (std::size_t i = fresh; i < rows.size(); ++i) {
        shown_[i] = rows[i];
    }
    if (rows.size() > fresh) {
        emit dataChanged(index(static_cast<int>(fresh)), index(count() - 1));
    }
    if (count() != before) {
        emit countChanged();
    }
}

CallLogModel::CallLogModel(QObject* parent) : QObject(parent)
{
    timer_.setInterval(250);
    connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
}

void CallLogModel::set_feeds(int feeds)
{
    if (feeds == feeds_) {
        return;
    }
    feeds_ = feeds;
    emit feedsChanged();
}

void CallLogModel::apply(const std::vector<CallFrame>& frames)
{
    if (frames.empty()) {
        return;
    }
    for (const CallFrame& frame : frames) {
        log_.apply(frame);
    }
    publish();
}

void CallLogModel::clear()
{
    log_.clear();
    publish();
}

void CallLogModel::tick()
{
    if (log_.expire(QDateTime::currentMSecsSinceEpoch())) {
        publish();
    } else if (!log_.any_active()) {
        timer_.stop();
    }
}

void CallLogModel::publish()
{
    calls_.sync(log_.calls());
    groups_.sync(log_.groups());
    if (log_.any_active() && !timer_.isActive()) {
        timer_.start();
    }
}

}  // namespace revenant::ui
