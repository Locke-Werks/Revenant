#include "render/hdr_output.h"

#include <algorithm>
#include <cstdio>
#include <memory>

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QSGRendererInterface>
#include <QSettings>
#include <rhi/qrhi.h>

#include "models/settings.h"
#include "render/hdr_map.h"

namespace revenant::ui {
namespace {

// The brightest the top of the colour map is drawn, whatever the display
// claims. Displays report their panel's peak, often for a small patch and a
// short time, and the virtual display this was written on reported 3805 nits.
// A waterfall is a large area held for minutes.
constexpr double kMapPeakNits = 1000.0;

// What Qt Quick reads when it builds a window's swapchain. Set from prepare()
// until the main window's first frame and no longer, so only the main window
// is built with it: the receiver and burst windows are hidden at load and
// build theirs when first shown. Not the per-window property
// _qt_sg_hdr_format, which is read at the same point, because the main window
// renders its first frame inside loadFromModule, before there is a window to
// set it on. Checked against Qt6Quick.dll 6.8.3.
constexpr const char* kHdrVariable = "QSG_RHI_HDR";

}  // namespace

void HdrOutput::prepare()
{
    if (!enabled_ || QGuiApplication::platformName() != QLatin1StringView("windows") ||
        !qEnvironmentVariableIsEmpty(kHdrVariable)) {
        return;
    }
    qputenv(kHdrVariable, "scrgb");
    prepared_ = true;
}

HdrOutput::HdrOutput(QObject* parent) : QObject(parent)
{
    enabled_ = QSettings().value(settings::kHdrOutput, true).toBool();
}

void HdrOutput::setEnabled(bool enabled)
{
    if (enabled == enabled_) {
        return;
    }
    enabled_ = enabled;
    QSettings().setValue(settings::kHdrOutput, enabled_);
    emit enabledChanged();
}

void HdrOutput::attach(QQuickWindow* window, QQmlEngine* engine)
{
    if (!prepared_) {
        return;
    }
    if (window == nullptr || engine == nullptr) {
        qunsetenv(kHdrVariable);
        return;
    }
    window_ = window;
    engine_ = engine;

    // On as soon as there is a root item to put it on, which can be a frame
    // after the first, and off again below if Qt fell back to SDR.
    setLayer(true);

    // The next frame's, on the render thread, where the swapchain exists.
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(
        window, &QQuickWindow::beforeRendering, this,
        [this, window, connection] {
            disconnect(*connection);
            qunsetenv(kHdrVariable);
            bool scrgb = false;
            double sdr_white = 80.0;
            double peak = 80.0;
            QSGRendererInterface* renderer = window->rendererInterface();
            auto* chain = renderer != nullptr
                              ? static_cast<QRhiSwapChain*>(renderer->getResource(
                                    window, QSGRendererInterface::RhiSwapchainResource))
                              : nullptr;
            if (chain != nullptr && chain->format() == QRhiSwapChain::HDRExtendedSrgbLinear) {
                scrgb = true;
                const QRhiSwapChainHdrInfo info = chain->hdrInfo();
                if (info.sdrWhiteLevel > 0.0F) {
                    sdr_white = info.sdrWhiteLevel;
                }
                peak = info.limitsType == QRhiSwapChainHdrInfo::LuminanceInNits
                           ? info.limits.luminanceInNits.maxLuminance
                           : sdr_white * info.limits.colorComponentValue.maxColorComponentValue;
            }
            QMetaObject::invokeMethod(
                this, [this, scrgb, sdr_white, peak] { confirm(scrgb, sdr_white, peak); },
                Qt::QueuedConnection);
        },
        Qt::DirectConnection);
}

void HdrOutput::confirm(bool scrgb, double sdr_white, double peak)
{
    if (!scrgb) {
        setLayer(false);
        return;
    }
    active_ = true;
    sdr_white_ = sdr_white;
    peak_ = std::max(peak, sdr_white);
    hdr::set_headroom(static_cast<float>(std::min(peak_, kMapPeakNits) / sdr_white_));
    std::fprintf(stderr, "revenant-ui: HDR output, SDR white %.0f nits, display peak %.0f nits\n",
                 sdr_white_, peak_);
    emit activeChanged();
    if (window_ != nullptr) {
        window_->update();
    }
}

bool HdrOutput::setLayer(bool on)
{
    if (window_ == nullptr) {
        return false;
    }
    if (stage_ != nullptr) {
        auto* layer = qvariant_cast<QObject*>(stage_->property("layer"));
        if (layer != nullptr) {
            layer->setProperty("enabled", on);
        }
        return true;
    }
    if (!on) {
        return true;
    }

    QQuickItem* root = window_->contentItem();
    QQmlComponent component(engine_, QStringLiteral("Revenant"), QStringLiteral("HdrStage"));
    auto* stage = qobject_cast<QQuickItem*>(component.create());
    if (stage == nullptr || root == nullptr) {
        std::fprintf(stderr, "revenant-ui: no HDR stage, so HDR output stays off: %s\n",
                     qPrintable(component.errorString()));
        delete stage;
        return false;
    }
    stage_ = stage;

    // The popup overlay is made on first use, and one made after the move
    // below would be a child of the root beside the stage, drawn uncorrected.
    // Asking for it makes it now.
    (void)window_->property("overlay");

    // Taken before the stage joins the root, because its layer puts its own
    // ShaderEffectSource beside it there, and that one moved under the stage
    // would be the layer drawing itself: the window came out empty that way.
    const QList<QQuickItem*> children = root->childItems();

    stage->setParent(root);
    stage->setParentItem(root);
    // From the window, because the root item reports no size of its own, and
    // a layer on a zero-sized item draws nothing at all.
    QQuickWindow* window = window_;
    const auto fit = [stage, window] { stage->setSize(window->size()); };
    fit();
    connect(window, &QWindow::widthChanged, stage, fit);
    connect(window, &QWindow::heightChanged, stage, fit);

    // In their order, which is their stacking order: the window's content,
    // then the popup overlay over it.
    for (QQuickItem* child : children) {
        child->setParentItem(stage);
    }
    return true;
}

}  // namespace revenant::ui
