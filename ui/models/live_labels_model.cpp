#include "models/live_labels_model.h"

#include <algorithm>
#include <cmath>

#include <QFontMetricsF>

#include "models/engine_link.h"

namespace revenant::ui {

SpanLabelSettings::SpanLabelSettings(QObject* parent) : QObject(parent)
{
    const QSettings store;
    show_ = store.value(settings::kShowSpanLabels, settings::kShowSpanLabelsDefault).toBool();
    categories_ = store.value(settings::kSpanLabelCategories, settings::kSpanLabelCategoriesDefault)
                      .toUInt() & kLabelAll;
}

void SpanLabelSettings::setShowLabels(bool show)
{
    if (show == show_) {
        return;
    }
    show_ = show;
    QSettings().setValue(settings::kShowSpanLabels, show);
    emit changed();
}

bool SpanLabelSettings::category(int bit) const
{
    return (categories_ & static_cast<std::uint32_t>(bit)) != 0;
}

void SpanLabelSettings::setCategory(int bit, bool on)
{
    const auto b = static_cast<std::uint32_t>(bit) & kLabelAll;
    const std::uint32_t next = on ? (categories_ | b) : (categories_ & ~b);
    if (next == categories_) {
        return;
    }
    categories_ = next;
    QSettings().setValue(settings::kSpanLabelCategories, categories_);
    emit changed();
}

SpanLabelModel::SpanLabelModel(QObject* parent) : QAbstractListModel(parent) {}

int SpanLabelModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant SpanLabelModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }
    const Row& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case TextRole: return QString::fromStdString(row.label.text);
        case X0Role: return row.place.x0;
        case PlateWidthRole: return row.place.x1 - row.place.x0;
        case RowRole: return row.place.row;
        case AnchorXRole: return row.place.anchor_x;
        case OpacityRole: return row.label.opacity;
        case LiveRole: return row.label.live;
        case HzRole: return static_cast<double>(row.label.hz);
        case ModeRole: return QString::fromStdString(row.label.mode);
        default: return {};
    }
}

QHash<int, QByteArray> SpanLabelModel::roleNames() const
{
    return {
        {TextRole, "text"},       {X0Role, "x0"},           {PlateWidthRole, "plateWidth"},
        {RowRole, "row"},         {AnchorXRole, "anchorX"}, {OpacityRole, "labelOpacity"},
        {LiveRole, "live"},       {HzRole, "hz"},           {ModeRole, "mode"},
    };
}

void SpanLabelModel::setLink(EngineLink* link)
{
    if (link_ == link) {
        return;
    }
    if (link_ != nullptr) {
        disconnect(link_, nullptr, this, nullptr);
    }
    link_ = link;
    if (link_ != nullptr) {
        // Every frame signals; takeSpan makes that two compares unless the
        // span moved. Detections and the view always rebuild.
        connect(link_, &EngineLink::frameChanged, this, &SpanLabelModel::takeSpan);
        connect(link_, &EngineLink::connectionChanged, this, &SpanLabelModel::rebuild);
        connect(link_, &EngineLink::viewChanged, this, &SpanLabelModel::rebuild);
        connect(link_, &EngineLink::detectionsChanged, this, &SpanLabelModel::rebuild);
    }
    rebuild();
    emit linkChanged();
}

void SpanLabelModel::setViewWidth(double width)
{
    if (width == width_) {
        return;
    }
    width_ = width;
    rebuild();
}

void SpanLabelModel::setEnabled(bool on)
{
    if (on == enabled_) {
        return;
    }
    enabled_ = on;
    rebuild();
}

void SpanLabelModel::setCategories(int mask)
{
    const auto m = static_cast<std::uint32_t>(mask);
    if (m == categories_) {
        return;
    }
    categories_ = m;
    rebuild();
}

void SpanLabelModel::setRegion(int mask)
{
    const auto m = static_cast<std::uint32_t>(mask);
    if (m == region_) {
        return;
    }
    region_ = m;
    rebuild();
}

void SpanLabelModel::setFont(const QFont& font)
{
    if (font == font_) {
        return;
    }
    font_ = font;
    rebuild();
}

void SpanLabelModel::setPadding(double padding)
{
    if (padding == padding_) {
        return;
    }
    padding_ = padding;
    rebuild();
}

void SpanLabelModel::takeSpan()
{
    if (link_ == nullptr) {
        return;
    }
    if (link_->viewLowHz() != laid_low_hz_ || link_->viewHighHz() != laid_high_hz_) {
        rebuild();
    }
}

void SpanLabelModel::rebuild()
{
    SpanLabelView view;
    view.width_px = width_;
    view.regions = region_;
    view.categories = categories_;
    if (link_ != nullptr) {
        view.low_hz = link_->viewLowHz();
        view.high_hz = link_->viewHighHz();
    }
    laid_low_hz_ = view.low_hz;
    laid_high_hz_ = view.high_hz;

    std::vector<SpanLabel> labels;
    if (enabled_ && link_ != nullptr && link_->connected()) {
        labels = merge_span_labels(link_->detections(), view);
    }

    const QFontMetricsF metrics(font_);
    std::vector<LabelCandidate> candidates;
    candidates.reserve(labels.size());
    for (const SpanLabel& label : labels) {
        candidates.push_back(LabelCandidate{
            span_label_x(label, view),
            metrics.horizontalAdvance(QString::fromStdString(label.text)) + 2.0 * padding_,
            label.priority});
    }
    const LabelLayout layout = layout_labels(candidates, width_);

    std::vector<Row> next;
    next.reserve(layout.placed.size());
    int rows_used = 0;
    for (const LabelPlacement& place : layout.placed) {
        next.push_back(Row{labels[place.index], place});
        rows_used = std::max(rows_used, place.row + 1);
    }

    // Same count keeps the delegates and only changes their data, so a label
    // that merely moved is not destroyed and made again every frame.
    if (next.size() == rows_.size() && !rows_.empty()) {
        rows_ = std::move(next);
        emit dataChanged(index(0), index(static_cast<int>(rows_.size()) - 1));
    } else if (!(next.empty() && rows_.empty())) {
        beginResetModel();
        rows_ = std::move(next);
        endResetModel();
    }
    rows_used_ = rows_used;
    hidden_ = static_cast<int>(layout.hidden);
    emit labelsChanged();
}

void SpanLabelModel::activate(int row)
{
    if (link_ == nullptr || row < 0 || row >= static_cast<int>(rows_.size())) {
        return;
    }
    const SpanLabel& label = rows_[static_cast<std::size_t>(row)].label;
    const QString mode = QString::fromStdString(label.mode);
    if (label.live) {
        link_->tuneReceiverToDetection(static_cast<double>(label.hz), mode,
                                       static_cast<double>(label.bandwidth_hz),
                                       label.detection_id);
    } else {
        link_->tuneReceiver(static_cast<double>(label.hz), mode);
    }
}

}  // namespace revenant::ui
