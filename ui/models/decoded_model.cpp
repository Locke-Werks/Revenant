#include "models/decoded_model.h"

#include <string>
#include <utility>

#include <QClipboard>
#include <QGuiApplication>
#include <QVariantList>
#include <QVariantMap>

namespace revenant::ui {

DecodedLogModel::DecodedLogModel(QObject* parent) : QAbstractListModel(parent) {}

int DecodedLogModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return count();
}

QVariant DecodedLogModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) {
        return {};
    }
    const DecodedLine& line = log_.lines()[static_cast<std::size_t>(index.row())];
    switch (role) {
        case SerialRole: return QVariant::fromValue(static_cast<qulonglong>(line.serial));
        case TimeRole: return QString::fromStdString(line.time);
        case DecoderRole: return QString::fromStdString(line.decoder);
        case TextRole: return QString::fromStdString(line.text);
        case ChipRole: return QString::fromStdString(line.chip);
        case ExpandedRole: return line.expanded;
        case FieldsRole: {
            // Built on demand and only for an open line, which is the one
            // place the view reads it.
            QVariantList out;
            if (!line.expanded) {
                return out;
            }
            for (const DecodedFieldText& field : line.fields) {
                out.append(QVariantMap{{QStringLiteral("key"), QString::fromStdString(field.key)},
                                       {QStringLiteral("value"),
                                        QString::fromStdString(field.value)}});
            }
            return out;
        }
        default: return {};
    }
}

QHash<int, QByteArray> DecodedLogModel::roleNames() const
{
    return {
        {SerialRole, "serial"},   {TimeRole, "time"},     {DecoderRole, "decoder"},
        {TextRole, "text"},       {ChipRole, "chip"},     {FieldsRole, "fields"},
        {ExpandedRole, "expanded"},
    };
}

void DecodedLogModel::append(std::vector<DecodedLine> lines, std::uint64_t unkept)
{
    if (unkept > 0) {
        log_.count_unkept(unkept);
    }
    if (lines.empty()) {
        if (unkept > 0) {
            emit changed();
        }
        return;
    }

    const DecodedLog::AppendPlan plan = log_.plan_append(lines.size());
    if (plan.evict_front > 0) {
        beginRemoveRows(QModelIndex(), 0, static_cast<int>(plan.evict_front) - 1);
        log_.evict_front(plan.evict_front);
        endRemoveRows();
    }

    const int first = count();
    const auto added = static_cast<int>(lines.size() - plan.skip_incoming);
    beginInsertRows(QModelIndex(), first, first + added - 1);
    log_.push_batch(std::move(lines), plan.skip_incoming);
    endInsertRows();
    emit changed();
}

void DecodedLogModel::clear()
{
    beginResetModel();
    log_.clear();
    endResetModel();
    emit changed();
}

void DecodedLogModel::toggleExpanded(int row)
{
    if (row < 0 || row >= count()) {
        return;
    }
    DecodedLine& line = log_.lines()[static_cast<std::size_t>(row)];
    line.expanded = !line.expanded;
    const QModelIndex at = index(row);
    emit dataChanged(at, at, {ExpandedRole, FieldsRole});
}

void DecodedLogModel::copyLine(int row) const
{
    if (row < 0 || row >= count()) {
        return;
    }
    const std::string text = decoded_line_detail(log_.lines()[static_cast<std::size_t>(row)]);
    QGuiApplication::clipboard()->setText(QString::fromStdString(text));
}

void DecodedLogModel::copyAll() const
{
    QGuiApplication::clipboard()->setText(QString::fromStdString(log_.copy_all()));
}

}  // namespace revenant::ui
