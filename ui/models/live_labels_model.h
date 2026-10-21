// The span's overlay labels as a list QML can repeat over, and the two
// preferences that govern them.
//
// A thin wrapper: models/live_labels.h decides which labels exist and
// render/label_layout.h where they go, both with their cases in ui/tests.
// This measures the text, follows the view and the detection list, and turns
// a click into a tune.
//
// Qt thread only.

#pragma once

#include <cstdint>
#include <vector>

#include <QAbstractListModel>
#include <QFont>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QtQmlIntegration>

#include "models/live_labels.h"
#include "models/settings.h"
#include "render/label_layout.h"

namespace revenant::ui {

class EngineLink;

// Shown or not, and which categories. Its own singleton rather than more
// properties on BandPlanSettings so the bar's settings stay the bar's.
class SpanLabelSettings : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool showLabels READ showLabels WRITE setShowLabels NOTIFY changed)
    Q_PROPERTY(int categories READ categories NOTIFY changed)

public:
    explicit SpanLabelSettings(QObject* parent = nullptr);

    [[nodiscard]] bool showLabels() const { return show_; }
    [[nodiscard]] int categories() const { return static_cast<int>(categories_); }
    void setShowLabels(bool show);

    [[nodiscard]] Q_INVOKABLE bool category(int bit) const;
    Q_INVOKABLE void setCategory(int bit, bool on);

signals:
    void changed();

private:
    bool show_ = settings::kShowSpanLabelsDefault;
    std::uint32_t categories_ = kLabelAll;
};

class SpanLabelModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(revenant::ui::EngineLink* link READ link WRITE setLink NOTIFY linkChanged)
    Q_PROPERTY(double viewWidth READ viewWidth WRITE setViewWidth NOTIFY labelsChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY labelsChanged)
    Q_PROPERTY(int categories READ categories WRITE setCategories NOTIFY labelsChanged)
    Q_PROPERTY(int region READ region WRITE setRegion NOTIFY labelsChanged)
    Q_PROPERTY(QFont font READ font WRITE setFont NOTIFY labelsChanged)
    Q_PROPERTY(double padding READ padding WRITE setPadding NOTIFY labelsChanged)
    Q_PROPERTY(int rowsUsed READ rowsUsed NOTIFY labelsChanged)
    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY labelsChanged)

public:
    enum Role : int {
        TextRole = Qt::UserRole + 1,
        X0Role,
        PlateWidthRole,
        RowRole,
        AnchorXRole,
        OpacityRole,
        LiveRole,
        HzRole,
        ModeRole,
    };

    explicit SpanLabelModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] EngineLink* link() const { return link_; }
    void setLink(EngineLink* link);
    [[nodiscard]] double viewWidth() const { return width_; }
    void setViewWidth(double width);
    [[nodiscard]] bool enabled() const { return enabled_; }
    void setEnabled(bool on);
    [[nodiscard]] int categories() const { return static_cast<int>(categories_); }
    void setCategories(int mask);
    [[nodiscard]] int region() const { return static_cast<int>(region_); }
    void setRegion(int mask);
    [[nodiscard]] QFont font() const { return font_; }
    void setFont(const QFont& font);
    [[nodiscard]] double padding() const { return padding_; }
    void setPadding(double padding);
    [[nodiscard]] int rowsUsed() const { return rows_used_; }
    [[nodiscard]] int hiddenCount() const { return hidden_; }

    // Tunes a receiver to the label at this row with the label's mode: a live
    // one through the detection path, so its label sets the decoder too.
    Q_INVOKABLE void activate(int row);

signals:
    void linkChanged();
    void labelsChanged();

private:
    struct Row {
        SpanLabel label;
        LabelPlacement place;
    };

    void takeSpan();
    void rebuild();

    EngineLink* link_ = nullptr;
    double width_ = 0.0;
    bool enabled_ = true;
    std::uint32_t categories_ = kLabelAll;
    std::uint32_t region_ = kRegionUSCA;
    QFont font_;
    double padding_ = 6.0;

    double laid_low_hz_ = 0.0;
    double laid_high_hz_ = 0.0;
    std::vector<Row> rows_;
    int rows_used_ = 0;
    int hidden_ = 0;
};

}  // namespace revenant::ui
