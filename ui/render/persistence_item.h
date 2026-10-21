// The phosphor persistence spectrum: the span's trace accumulated into a
// density picture that fades, x frequency and y level, coloured by how often
// the trace has been in each cell. render/persistence_grid.h has the
// accumulation and the reasons for its shape.
//
// The level axis is the waterfall's: the floor and ceiling are bound from
// WaterfallItem's drawn ends in qml/SpanView.qml, which already obey the
// operator's pins, so a pin moves this picture the way it moves the others.
// The density goes through the same colour map and the same contrast curve.
//
// The cost of a frame is one fade over the grid, one trace added, and one
// texture the size of the grid built and uploaded. The grid is at most
// kPersistenceMaxColumns by kPersistenceRows whatever the window, and the
// item does nothing while it is not visible, so the waterfall view pays for
// none of it.

#pragma once

#include <QImage>
#include <QQuickItem>
#include <QtQmlIntegration>

#include <cstdint>
#include <vector>

#include "models/engine_link.h"
#include "models/scale_settings.h"
#include "render/persistence_grid.h"

namespace revenant::ui {

class PersistenceItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(revenant::ui::EngineLink* link READ link WRITE setLink NOTIFY linkChanged)
    Q_PROPERTY(revenant::ui::ScaleSettings* mapPins READ mapPins WRITE setMapPins
                   NOTIFY mapPinsChanged)
    Q_PROPERTY(double floorDb READ floorDb WRITE setFloorDb NOTIFY endsChanged)
    Q_PROPERTY(double ceilingDb READ ceilingDb WRITE setCeilingDb NOTIFY endsChanged)
    Q_PROPERTY(double decaySeconds READ decaySeconds WRITE setDecaySeconds
                   NOTIFY decaySecondsChanged)

public:
    explicit PersistenceItem(QQuickItem* parent = nullptr);

    [[nodiscard]] EngineLink* link() const { return link_; }
    void setLink(EngineLink* link);

    [[nodiscard]] ScaleSettings* mapPins() const { return map_pins_; }
    void setMapPins(ScaleSettings* pins);

    [[nodiscard]] double floorDb() const { return floor_db_; }
    void setFloorDb(double db);
    [[nodiscard]] double ceilingDb() const { return ceiling_db_; }
    void setCeilingDb(double db);

    [[nodiscard]] double decaySeconds() const { return decay_seconds_; }
    void setDecaySeconds(double seconds);

signals:
    void linkChanged();
    void mapPinsChanged();
    void endsChanged();
    void decaySecondsChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

private:
    void takeFrame();
    void clearHistory();

    // The grid's colours for the curve in force, indexed by a cell's density
    // as a fraction of the peak. Rebuilt when the peak or the curve moves,
    // which is every frame for the peak, at kShadeSteps entries.
    void buildImage();

    [[nodiscard]] int wantedColumns() const;

    EngineLink* link_ = nullptr;
    ScaleSettings* map_pins_ = nullptr;
    double floor_db_ = -100.0;
    double ceiling_db_ = -40.0;
    double decay_seconds_ = kDefaultPersistenceDecaySeconds;

    PersistenceGrid grid_;
    std::vector<float> columns_;
    QImage image_;
    bool image_dirty_ = false;
    std::uint64_t last_sample_end_ = 0;
};

}  // namespace revenant::ui
