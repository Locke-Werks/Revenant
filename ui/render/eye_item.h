// The eye diagram half of the detail view's symbol panel (#17).
//
// Each trace is two symbol periods of the timed signal centred on a strobe:
// the discriminator for an FSK receiver, the in-phase rail turned onto its
// nearest decision for a PSK one. Traces from the last few frames are kept
// and fade with age, which is the persistence a scope's phosphor gives, so an
// open eye reads as a clear diamond and a closed one as a smear.
//
// Fed from PassbandFrame::symbols, like render/constellation_item.h.

#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include <QColor>
#include <QQuickItem>
#include <QtQmlIntegration>

#include "core/rpc/types.h"
#include "models/engine_link.h"

namespace revenant::ui {

class EyeItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(revenant::ui::EngineLink* link READ link WRITE setLink NOTIFY linkChanged)
    Q_PROPERTY(QColor tint READ tint WRITE setTint NOTIFY tintChanged)
    // Frames of traces kept on screen, at least one.
    Q_PROPERTY(int persistence READ persistence WRITE setPersistence NOTIFY persistenceChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)

public:
    explicit EyeItem(QQuickItem* parent = nullptr);

    [[nodiscard]] EngineLink* link() const { return link_; }
    void setLink(EngineLink* link);

    [[nodiscard]] QColor tint() const { return tint_; }
    void setTint(const QColor& tint);

    [[nodiscard]] int persistence() const { return persistence_; }
    void setPersistence(int frames);

    [[nodiscard]] bool active() const { return !history_.empty(); }

signals:
    void linkChanged();
    void tintChanged();
    void persistenceChanged();
    void activeChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;

private:
    void takeFrame();
    void clear();

    EngineLink* link_ = nullptr;
    QColor tint_{150, 226, 255};
    int persistence_ = 6;

    // Newest first.
    std::deque<std::vector<float>> history_;
    std::uint32_t trace_length_ = 0;
    rpc::SymbolScope latest_;
};

}  // namespace revenant::ui
