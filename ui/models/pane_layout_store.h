// One pane tree as state the QML draws and the settings remember. The tree
// and its rules are models/pane_layout.h; this is the binding and the
// persistence, and nothing else.
//
// STRUCTURE AND SHARES CHANGE SEPARATELY. tree changes when a pane is moved or
// the layout reset, which rebuilds the frames. revision changes while a border
// is dragged, which only moves them: rebuilding on every mouse move would
// re-parent every display sixty times a second.
//
// HIDDEN PANES. The main tree's receivers leaf is left out while the receivers
// are hidden or popped out. The tree drawn is then the stored one with that
// leaf removed, and dragging is ignored on it: with two panes in the main tree
// there is no border left to drag once one is gone.
//
// WHEN IT IS WRITTEN. A move or a reset is one event and is written at once. A
// drag is many, and QSettings on Windows is a registry write each, so it is
// written when the drag ends (commit) and on the way out. A smoke run reads
// and writes nothing, so its photographs are of the default.

#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <cmath>
#include <utility>

#include "models/pane_layout.h"

namespace revenant::ui {

class PaneLayoutStore : public QObject {
    Q_OBJECT

    Q_PROPERTY(QVariantMap tree READ tree NOTIFY treeChanged)
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(double minimumWidth READ minimumWidth NOTIFY minimumChanged)
    Q_PROPERTY(double minimumHeight READ minimumHeight NOTIFY minimumChanged)
    Q_PROPERTY(QStringList hidden READ hidden WRITE setHidden NOTIFY treeChanged)

public:
    PaneLayoutStore(QString key, PaneNode fallback, std::vector<std::string> panes, bool persist,
                    QObject* parent = nullptr)
        : QObject(parent),
          key_(std::move(key)),
          fallback_(std::move(fallback)),
          panes_(std::move(panes)),
          persist_(persist),
          minimums_(default_pane_minimums()),
          floors_(minimums_),
          stored_(fallback_)
    {
        if (persist_) {
            const QString text = QSettings().value(key_).toString();
            if (auto read = read_pane_tree(text.toStdString());
                read && read->children.size() > 1 && holds_exactly(*read, panes_)) {
                stored_ = std::move(*read);
            }
        }
        rebuild_view();
    }

    [[nodiscard]] QVariantMap tree() const { return to_variant(view_, QString()); }
    [[nodiscard]] int revision() const { return revision_; }
    [[nodiscard]] double minimumWidth() const { return min_size(view_, minimums_).width; }
    [[nodiscard]] double minimumHeight() const { return min_size(view_, minimums_).height; }
    [[nodiscard]] QStringList hidden() const { return hidden_; }

    void setHidden(const QStringList& hidden)
    {
        if (hidden_ == hidden) {
            return;
        }
        hidden_ = hidden;
        rebuild_view();
        emit treeChanged();
        emit minimumChanged();
    }

    // Each child's length along the split at path, for a split length long.
    [[nodiscard]] Q_INVOKABLE QVariantList sizes(const QString& path, double length) const
    {
        QVariantList out;
        const PaneNode* node = node_at(view_, parse_path(path));
        if (node == nullptr || node->leaf()) {
            return out;
        }
        for (const double size : lay_out(node->weights, axis_minimums(*node, minimums_), length)) {
            out.append(size);
        }
        return out;
    }

    // The border after child index of the split at path, moved by delta.
    Q_INVOKABLE void drag(const QString& path, int index, double delta, double length)
    {
        if (!hidden_.isEmpty() || index < 0) {
            return;
        }
        PaneNode* node = node_at(stored_, parse_path(path));
        if (node == nullptr || node->leaf()) {
            return;
        }
        const std::vector<double> mins = axis_minimums(*node, minimums_);
        const std::vector<double> before = lay_out(node->weights, mins, length);
        const std::vector<double> after =
            drag_border(before, mins, static_cast<std::size_t>(index), delta);
        if (after == before) {
            return;
        }
        node->weights = after;
        dirty_ = true;
        rebuild_view();
        ++revision_;
        emit revisionChanged();
    }

    // Shares a split's length evenly between its children: a double click on
    // one of its borders.
    Q_INVOKABLE void equalise(const QString& path)
    {
        if (!hidden_.isEmpty()) {
            return;
        }
        PaneNode* node = node_at(stored_, parse_path(path));
        if (node == nullptr || node->leaf()) {
            return;
        }
        node->weights.assign(node->children.size(), 1.0);
        rebuild_view();
        write();
        ++revision_;
        emit revisionChanged();
    }

    // pane against edge ("left", "right", "top", "bottom") of target.
    Q_INVOKABLE bool move(const QString& pane, const QString& target, const QString& edge)
    {
        const auto side = pane_edge(edge.toStdString());
        if (!hidden_.isEmpty() || !side) {
            return false;
        }
        if (!move_pane(stored_, pane.toStdString(), target.toStdString(), *side)) {
            return false;
        }
        rebuild_view();
        write();
        emit treeChanged();
        emit minimumChanged();
        return true;
    }

    Q_INVOKABLE void reset()
    {
        stored_ = fallback_;
        rebuild_view();
        write();
        emit treeChanged();
        emit minimumChanged();
    }

    // What a pane's content needs, which raises its floor and never lowers it
    // below the one in models/pane_layout.h.
    Q_INVOKABLE void setMinimum(const QString& pane, double width, double height)
    {
        const std::string name = pane.toStdString();
        PaneSize floor;
        if (const auto found = floors_.find(name); found != floors_.end()) {
            floor = found->second;
        }
        const PaneSize wanted{std::max(floor.width, std::ceil(width)),
                              std::max(floor.height, std::ceil(height))};
        PaneSize& held = minimums_[name];
        if (held.width == wanted.width && held.height == wanted.height) {
            return;
        }
        held = wanted;
        ++revision_;
        emit revisionChanged();
        emit minimumChanged();
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
    void minimumChanged();

private:
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
        view_ = stored_;
        for (const QString& pane : std::as_const(hidden_)) {
            if (!view_.leaf()) {
                pane_detail::remove_leaf(view_, pane.toStdString());
                normalise(view_);
            }
        }
    }

    void write()
    {
        dirty_ = false;
        if (persist_) {
            QSettings().setValue(key_, QString::fromStdString(write_pane_tree(stored_)));
        }
    }

    QString key_;
    PaneNode fallback_;
    std::vector<std::string> panes_;
    bool persist_ = false;
    PaneMinimums minimums_;
    PaneMinimums floors_;
    PaneNode stored_;
    PaneNode view_;
    QStringList hidden_;
    int revision_ = 0;
    bool dirty_ = false;
};

}  // namespace revenant::ui
