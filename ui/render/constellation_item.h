// The constellation half of the detail view's symbol panel (#17).
//
// A PSK receiver's differential points on an I/Q square, with the ideal
// points crossed over them; an FSK receiver's levels as a scatter over time
// with their histogram at the right edge, the ideal levels as lines. Dots are
// drawn translucent and fade with age, so where they pile up is where the
// signal sits.
//
// The scope rides on the passband frame, PassbandFrame::symbols, and is only
// there while the receiver's VrxParams::display_symbols is set. The
// arithmetic is render/symbol_scope_map.h.

#pragma once

#include <vector>

#include <QColor>
#include <QQuickItem>
#include <QString>
#include <QtQmlIntegration>

#include "core/rpc/types.h"
#include "models/engine_link.h"

namespace revenant::ui {

class ConstellationItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(revenant::ui::EngineLink* link READ link WRITE setLink NOTIFY linkChanged)
    Q_PROPERTY(QColor tint READ tint WRITE setTint NOTIFY tintChanged)

    // True while frames carry symbols.
    Q_PROPERTY(bool active READ active NOTIFY scopeChanged)
    // "levels", "constellation" or empty.
    Q_PROPERTY(QString kind READ kind NOTIFY scopeChanged)
    // EVM or symbol deviation error, in percent of the outer ideal.
    Q_PROPERTY(double errorPercent READ errorPercent NOTIFY scopeChanged)
    Q_PROPERTY(double symbolRateHz READ symbolRateHz NOTIFY scopeChanged)
    Q_PROPERTY(double timingOffset READ timingOffset NOTIFY scopeChanged)
    Q_PROPERTY(double rateErrorPpm READ rateErrorPpm NOTIFY scopeChanged)

public:
    explicit ConstellationItem(QQuickItem* parent = nullptr);

    [[nodiscard]] EngineLink* link() const { return link_; }
    void setLink(EngineLink* link);

    [[nodiscard]] QColor tint() const { return tint_; }
    void setTint(const QColor& tint);

    [[nodiscard]] bool active() const { return scope_.kind != rpc::SymbolKind::None; }
    [[nodiscard]] QString kind() const;
    [[nodiscard]] double errorPercent() const { return 100.0 * scope_.error_rms; }
    [[nodiscard]] double symbolRateHz() const { return scope_.symbol_rate_hz; }
    [[nodiscard]] double timingOffset() const { return scope_.timing_offset; }
    [[nodiscard]] double rateErrorPpm() const { return scope_.rate_error_ppm; }

signals:
    void linkChanged();
    void tintChanged();
    void scopeChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;

private:
    void takeFrame();

    EngineLink* link_ = nullptr;
    QColor tint_{150, 226, 255};
    rpc::SymbolScope scope_;
};

}  // namespace revenant::ui
