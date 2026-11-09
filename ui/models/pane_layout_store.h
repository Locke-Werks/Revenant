// One pane tree as state the QML draws and the settings remember. The tree
// and its rules are models/pane_layout.h; this is the binding and the
// persistence, and nothing else.
//
// STRUCTURE AND SHARES CHANGE SEPARATELY. tree changes when a pane is moved,
// hidden or shown, or the layout reset, which rebuilds the frames. revision
// changes while a border is dragged, a pane collapsed or scaled, or a minimum
// moves, which only resizes them: rebuilding on every mouse move would
// re-parent every display sixty times a second.
//
// HIDDEN PANES. A pane is left out of the drawn tree while the receivers are
// popped out or hidden, or while a section has nothing to show. The view
// remembers where each of its nodes came from in the stored tree, so a drag
// on the view lands there, and a pane coming back finds its place and share.
//
// WHAT A PANE'S MINIMUM IS. What its content needs at a scale of one, which
// the QML reports, raised to the floor in models/pane_layout.h, times the
// pane's own scale, plus its title strip. A collapsed pane is its strip.
//
// WHEN IT IS WRITTEN. A move, a reset, a collapse or a scale is one event and
// is written at once. A drag is many, and QSettings on Windows is a registry
// write each, so it is written when the drag ends (commit) and on the way
// out. A smoke run reads and writes nothing, so its photographs are of the
// default.

#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <cmath>
#include <set>
#include <utility>

#include "models/pane_layout.h"

namespace revenant::ui {

class PaneLayoutStore : public QObject {
    Q_OBJECT

    Q_PROPERTY(QVariantMap tree READ tree NOTIFY treeChanged)
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(double minimumWidth READ minimumWidth NOTIFY revisionChanged)
    Q_PROPERTY(double minimumHeight READ minimumHeight NOTIFY revisionChanged)
    Q_PROPERTY(QStringList hidden READ hidden WRITE setHidden NOTIFY treeChanged)
    Q_PROPERTY(QStringList collapsed READ collapsed NOTIFY revisionChanged)
    Q_PROPERTY(QVariantMap scales READ scales NOTIFY revisionChanged)
    Q_PROPERTY(double strip READ strip CONSTANT)
    Q_PROPERTY(double collapsedBar READ collapsedBar CONSTANT)
    Q_PROPERTY(double spanShare READ spanShare WRITE setSpanShare NOTIFY spanShareChanged)

public:
    PaneLayoutStore(QString key, PaneNode fallback, std::vector<std::string> panes, bool persist,
                    QObject* parent = nullptr)
        : QObject(parent),
          key_(std::move(key)),
          fallback_(std::move(fallback)),
          panes_(std::move(panes)),
          persist_(persist),
          needs_(default_pane_minimums()),
          floors_(needs_),
          stored_(fallback_)
    {
        if (persist_) {
            const QSettings store;
            const QString text = store.value(key_).toString();
            if (auto read = read_pane_tree(text.toStdString());
                read && read->children.size() > 1 && holds_exactly(*read, panes_)) {
                stored_ = std::move(*read);
            }
            read_panes(store.value(key_ + QStringLiteral("Panes")).toString());
            const double share = store.value(key_ + QStringLiteral("Span"), span_share_).toDouble();
            if (share > 0.05 && share < 0.95) {
                span_share_ = share;
            }
        }
        rebuild_view();
    }

    [[nodiscard]] QVariantMap tree() const
    {
        if (!view_) {
            QVariantMap empty;
            empty.insert(QStringLiteral("path"), QString());
            empty.insert(QStringLiteral("leaf"), true);
            empty.insert(QStringLiteral("pane"), QString());
            empty.insert(QStringLiteral("horizontal"), false);
            empty.insert(QStringLiteral("children"), QVariantList());
            return empty;
        }
        return to_variant(*view_, QString());
    }
    [[nodiscard]] int revision() const { return revision_; }
    [[nodiscard]] double minimumWidth() const { return view_ ? min_size(*view_, minimums_).width : 0.0; }
    [[nodiscard]] double minimumHeight() const { return view_ ? min_size(*view_, minimums_).height : 0.0; }
    [[nodiscard]] QStringList hidden() const { return hidden_; }
    [[nodiscard]] double strip() const { return kPaneStrip; }
    [[nodiscard]] double collapsedBar() const { return kPaneCollapsedBar; }

    // The spectrum's share of the height it and the waterfall divide inside
    // the span pane. Written when the drag ends, like a border's.
    [[nodiscard]] double spanShare() const { return span_share_; }

    void setSpanShare(double share)
    {
        share = std::clamp(share, 0.05, 0.95);
        if (share == span_share_) {
            return;
        }
        span_share_ = share;
        dirty_ = true;
        emit spanShareChanged();
    }

    [[nodiscard]] QStringList collapsed() const
    {
        QStringList out;
        for (const std::string& pane : collapsed_) {
            out.append(QString::fromStdString(pane));
        }
        return out;
    }

    [[nodiscard]] QVariantMap scales() const
    {
        QVariantMap out;
        for (const std::string& pane : panes_) {
            out.insert(QString::fromStdString(pane), scale_of(pane));
        }
        return out;
    }

    void setHidden(const QStringList& hidden)
    {
        if (hidden_ == hidden) {
            return;
        }
        hidden_ = hidden;
        rebuild_view();
        emit treeChanged();
        bump();
    }

    // Each child's length along the split at path, for a split length long.
    [[nodiscard]] Q_INVOKABLE QVariantList sizes(const QString& path, double length) const
    {
        QVariantList out;
        const PaneNode* node = view_node(path);
        if (node == nullptr || node->leaf()) {
            return out;
        }
        for (const double size : lay_out(node->weights, axis_minimums(*node, minimums_), length,
                                         fixed_children(*node, collapsed_))) {
            out.append(size);
        }
        return out;
    }

    // The border after child index of the split at path, moved by delta.
    Q_INVOKABLE void drag(const QString& path, int index, double delta, double length)
    {
        PaneNode* view = view_node(path);
        if (view == nullptr || view->leaf() || index < 0) {
            return;
        }
        PaneNode* stored = node_at(stored_, view->source);
        if (stored == nullptr || stored->leaf()) {
            return;
        }
        const std::vector<double> mins = axis_minimums(*view, minimums_);
        const std::vector<bool> fixed = fixed_children(*view, collapsed_);
        const std::vector<double> before = lay_out(view->weights, mins, length, fixed);
        const std::vector<double> after =
            drag_border(before, mins, static_cast<std::size_t>(index), delta, fixed);
        if (after == before) {
            return;
        }
        store_view_sizes(*stored, *view, after);
        for (std::size_t i = 0; i < view->kept.size(); ++i) {
            view->weights[i] = stored->weights[view->kept[i]];
        }
        dirty_ = true;
        bump();
    }

    // Shares a split's length evenly between its children: a double click on
    // one of its borders.
    Q_INVOKABLE void equalise(const QString& path)
    {
        PaneNode* view = view_node(path);
        if (view == nullptr || view->leaf()) {
            return;
        }
        PaneNode* stored = node_at(stored_, view->source);
        if (stored == nullptr || stored->leaf()) {
            return;
        }
        store_view_sizes(*stored, *view, std::vector<double>(view->children.size(), 1.0));
        rebuild_view();
        write();
        bump();
    }

    // pane against edge ("left", "right", "top", "bottom") of target.
    Q_INVOKABLE bool move(const QString& pane, const QString& target, const QString& edge)
    {
        const auto side = pane_edge(edge.toStdString());
        if (!side) {
            return false;
        }
        if (!move_pane(stored_, pane.toStdString(), target.toStdString(), *side)) {
            return false;
        }
        rebuild_view();
        write();
        emit treeChanged();
        bump();
        return true;
    }

    // pane against edge of the whole area the view node at path covers: "" is
    // the window. See move_pane_beside_area.
    Q_INVOKABLE bool moveBeside(const QString& pane, const QString& path, const QString& edge)
    {
        const auto side = pane_edge(edge.toStdString());
        const PaneNode* area = view_node(path);
        if (!side || area == nullptr) {
            return false;
        }
        // The stored node the view's came from, hidden panes and all, since
        // that is the area the stored tree has to find again.
        const PaneNode* stored_area = node_at(stored_, area->source);
        if (stored_area == nullptr) {
            return false;
        }
        std::vector<std::string> held;
        collect_panes(*stored_area, held);
        if (!move_pane_beside_area(stored_, pane.toStdString(), {held.begin(), held.end()}, *side)) {
            return false;
        }
        rebuild_view();
        write();
        emit treeChanged();
        bump();
        return true;
    }

    Q_INVOKABLE void reset()
    {
        stored_ = fallback_;
        span_share_ = kDefaultSpanShare;
        emit spanShareChanged();
        collapsed_.clear();
        scales_.clear();
        needs_ = floors_;
        rebuild_view();
        write();
        emit treeChanged();
        bump();
    }

    Q_INVOKABLE void toggleCollapsed(const QString& pane)
    {
        const std::string name = pane.toStdString();
        if (!collapsed_.erase(name)) {
            collapsed_.insert(name);
        }
        refresh_minimums();
        write();
        bump();
    }

    // The pane's own scale, kept between kPaneScaleLow and kPaneScaleHigh and
    // to the nearest five percent.
    Q_INVOKABLE void setScale(const QString& pane, double scale)
    {
        const double held =
            std::clamp(std::round(scale * 20.0) / 20.0, kPaneScaleLow, kPaneScaleHigh);
        const std::string name = pane.toStdString();
        if (scale_of(name) == held) {
            return;
        }
        if (held == 1.0) {
            scales_.erase(name);
        } else {
            scales_[name] = held;
        }
        refresh_minimums();
        write();
        bump();
    }

    // What a pane's content needs at a scale of one, title strip excluded.
    // The width only ever rises for the rest of the run; see the note inside.
    // A reset of the layout starts the measuring again.
    Q_INVOKABLE void setMinimum(const QString& pane, double width, double height)
    {
        const std::string name = pane.toStdString();
        PaneSize floor;
        if (const auto found = floors_.find(name); found != floors_.end()) {
            floor = found->second;
        }
        // The widest seen, so a label that grows and shrinks as it reads does
        // not move a border back and forth. Not the tallest: a section opening
        // and closing is a real change in what the pane holds, and a height
        // kept from it squeezed every other pane for the rest of the run.
        PaneSize& held = needs_[name];
        const PaneSize wanted{std::max({floor.width, held.width, std::ceil(width)}),
                              std::max(floor.height, std::ceil(height))};
        if (held.width == wanted.width && held.height == wanted.height) {
            return;
        }
        held = wanted;
        refresh_minimums();
        bump();
    }

    // The end of a drag. See WHEN IT IS WRITTEN above.
    Q_INVOKABLE void commit()
    {
        if (dirty_) {
            write();
        }
    }

signals:
    void treeChanged();
    void revisionChanged();
    void spanShareChanged();

private:
    [[nodiscard]] double scale_of(const std::string& pane) const
    {
        const auto found = scales_.find(pane);
        return found == scales_.end() ? 1.0 : found->second;
    }

    void refresh_minimums()
    {
        minimums_.clear();
        for (const std::string& pane : panes_) {
            if (collapsed_.contains(pane)) {
                minimums_[pane] = PaneSize{kPaneCollapsedBar, kPaneStrip};
                continue;
            }
            PaneSize need;
            if (const auto found = needs_.find(pane); found != needs_.end()) {
                need = found->second;
            }
            const double scale = scale_of(pane);
            minimums_[pane] = PaneSize{std::ceil(need.width * scale),
                                       std::ceil(need.height * scale) + kPaneStrip};
        }
    }

    void bump()
    {
        ++revision_;
        emit revisionChanged();
    }

    [[nodiscard]] static std::vector<std::size_t> parse_path(const QString& path)
    {
        std::vector<std::size_t> out;
        if (path.isEmpty()) {
            return out;
        }
        for (const QString& part : path.split(QLatin1Char('.'))) {
            bool ok = false;
            const uint index = part.toUInt(&ok);
            if (!ok) {
                return {static_cast<std::size_t>(-1)};
            }
            out.push_back(index);
        }
        return out;
    }

    [[nodiscard]] PaneNode* view_node(const QString& path)
    {
        return view_ ? node_at(*view_, parse_path(path)) : nullptr;
    }

    [[nodiscard]] const PaneNode* view_node(const QString& path) const
    {
        return view_ ? node_at(*view_, parse_path(path)) : nullptr;
    }

    [[nodiscard]] static QVariantMap to_variant(const PaneNode& node, const QString& path)
    {
        QVariantMap out;
        out.insert(QStringLiteral("path"), path);
        out.insert(QStringLiteral("leaf"), node.leaf());
        out.insert(QStringLiteral("pane"), QString::fromStdString(node.pane));
        out.insert(QStringLiteral("horizontal"), node.horizontal);
        QVariantList children;
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            const QString child_path =
                path.isEmpty() ? QString::number(i) : path + QLatin1Char('.') + QString::number(i);
            children.append(to_variant(node.children[i], child_path));
        }
        out.insert(QStringLiteral("children"), children);
        return out;
    }

    void rebuild_view()
    {
        std::set<std::string, std::less<>> hidden;
        for (const QString& pane : std::as_const(hidden_)) {
            hidden.insert(pane.toStdString());
        }
        view_ = build_view(stored_, hidden);
        refresh_minimums();
    }

    // "pane=scale" or "pane=scale,c" for collapsed, separated by ';'.
    void read_panes(const QString& text)
    {
        for (const QString& entry : text.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            const QStringList pair = entry.split(QLatin1Char('='));
            if (pair.size() != 2) {
                continue;
            }
            const std::string pane = pair[0].toStdString();
            if (std::find(panes_.begin(), panes_.end(), pane) == panes_.end()) {
                continue;
            }
            const QStringList parts = pair[1].split(QLatin1Char(','));
            bool ok = false;
            const double scale = parts[0].toDouble(&ok);
            if (ok && scale >= kPaneScaleLow && scale <= kPaneScaleHigh && scale != 1.0) {
                scales_[pane] = scale;
            }
            if (parts.size() > 1 && parts[1] == QStringLiteral("c")) {
                collapsed_.insert(pane);
            }
        }
    }

    [[nodiscard]] QString write_panes() const
    {
        QStringList out;
        for (const std::string& pane : panes_) {
            const bool folded = collapsed_.contains(pane);
            const double scale = scale_of(pane);
            if (!folded && scale == 1.0) {
                continue;
            }
            out.append(QString::fromStdString(pane) + QLatin1Char('=') + QString::number(scale) +
                       (folded ? QStringLiteral(",c") : QString()));
        }
        return out.join(QLatin1Char(';'));
    }

    void write()
    {
        dirty_ = false;
        if (persist_) {
            QSettings store;
            store.setValue(key_, QString::fromStdString(write_pane_tree(stored_)));
            store.setValue(key_ + QStringLiteral("Panes"), write_panes());
            store.setValue(key_ + QStringLiteral("Span"), span_share_);
        }
    }

    QString key_;
    PaneNode fallback_;
    std::vector<std::string> panes_;
    bool persist_ = false;
    PaneMinimums needs_;
    PaneMinimums floors_;
    PaneMinimums minimums_;
    std::set<std::string, std::less<>> collapsed_;
    std::map<std::string, double, std::less<>> scales_;
    PaneNode stored_;
    std::optional<PaneNode> view_;
    QStringList hidden_;
    int revision_ = 0;
    bool dirty_ = false;
    static constexpr double kDefaultSpanShare = 0.34;
    double span_share_ = kDefaultSpanShare;
};

}  // namespace revenant::ui
