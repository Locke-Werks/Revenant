#include "render/frame_probe.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>

#include <QFile>
#include <QGuiApplication>
#include <QScreen>
#include <QSGRendererInterface>
#include <QThread>
#include <QTimer>

// The RHI's semi-public header, for QRhiCommandBuffer::lastCompletedGpuTime.
// It is the one place Qt exposes how long the GPU spent on a frame, and it is
// why this target links Qt6::GuiPrivate.
#include <rhi/qrhi.h>

namespace revenant::ui {

namespace {

[[nodiscard]] std::string graphics_api_name(QSGRendererInterface::GraphicsApi api)
{
    switch (api) {
    case QSGRendererInterface::Software:
        return "software";
    case QSGRendererInterface::OpenVG:
        return "openvg";
    case QSGRendererInterface::OpenGL:
        return "opengl";
    case QSGRendererInterface::Direct3D11:
        return "d3d11";
    case QSGRendererInterface::Direct3D12:
        return "d3d12";
    case QSGRendererInterface::Vulkan:
        return "vulkan";
    case QSGRendererInterface::Metal:
        return "metal";
    case QSGRendererInterface::Null:
        return "null";
    default:
        return "unknown";
    }
}

}  // namespace

void FrameProbe::prepare()
{
    // QSG_RHI_PROFILE=1 is the scene graph's switch for QRhi::EnableTimestamps.
    // Set only when stats were asked for, so an ordinary run takes no GPU
    // timestamp queries at all. An operator who set it already keeps theirs.
    if (qEnvironmentVariableIsEmpty("QSG_RHI_PROFILE")) {
        qputenv("QSG_RHI_PROFILE", "1");
    }
}

FrameProbe::FrameProbe(QString path, EngineLink& link, QObject* parent)
    : QObject(parent), path_(std::move(path)), link_(link)
{
    g_frame_recorder.store(&recorder_, std::memory_order_release);
    connect(&link_, &EngineLink::frameChanged, this, &FrameProbe::onEngineFrame);
}

FrameProbe::~FrameProbe()
{
    g_frame_recorder.store(nullptr, std::memory_order_release);
}

void FrameProbe::watch(QQuickWindow* window, const QString& name)
{
    if (window == nullptr) {
        return;
    }
    const int index = recorder_.add_window(name.toStdString());
    windows_.push_back(Watched{window, index});

    // Every one of these is emitted on the window's render thread, which is
    // not this object's thread under the threaded loop, so they are direct:
    // a queued timestamp is the time the GUI thread got round to it.
    const auto direct = Qt::DirectConnection;
    FrameRecorder* recorder = &recorder_;
    connect(window, &QQuickWindow::beforeFrameBegin, this,
            [recorder, index] { recorder->frame_began(index, frame_clock_ns()); }, direct);
    connect(window, &QQuickWindow::beforeSynchronizing, this,
            [recorder, index] { recorder->sync_began(index, frame_clock_ns()); }, direct);
    connect(window, &QQuickWindow::afterSynchronizing, this,
            [recorder, index] { recorder->sync_ended(index, frame_clock_ns()); }, direct);
    connect(window, &QQuickWindow::beforeRendering, this,
            [recorder, index] { recorder->render_began(index, frame_clock_ns()); }, direct);
    connect(
        window, &QQuickWindow::afterRendering, this,
        [recorder, index, window] {
            recorder->render_ended(index, frame_clock_ns());
            // Sampled once a frame. Qt updates it when the device finishes a
            // frame, a frame or two behind this one, and reports zero until
            // it has one or when timestamps are off.
            if (QRhiSwapChain* chain = window->swapChain()) {
                if (QRhiCommandBuffer* buffer = chain->currentFrameCommandBuffer()) {
                    recorder->gpu_time(index, buffer->lastCompletedGpuTime() * 1000.0);
                }
            }
        },
        direct);
    connect(
        window, &QQuickWindow::frameSwapped, this,
        [this, recorder, index] {
            recorder->swapped(index, frame_clock_ns());
            if (QThread::currentThread() != QCoreApplication::instance()->thread()) {
                render_thread_.store(true, std::memory_order_relaxed);
            }
        },
        direct);
}

void FrameProbe::onEngineFrame()
{
    if (open_ && !finished_) {
        ++to_display_;
    }
    if (warmup_started_) {
        return;
    }
    warmup_started_ = true;
    QTimer::singleShot(std::chrono::milliseconds(static_cast<int>(kFrameWarmupSeconds * 1000.0)),
                       this, &FrameProbe::openMeasurement);
}

void FrameProbe::openMeasurement()
{
    if (finished_) {
        return;
    }
    for (const Watched& watched : windows_) {
        if (watched.window != nullptr) {
            recorder_.set_window_size(watched.index, watched.window->width(),
                                      watched.window->height());
        }
    }
    at_open_ = counters();
    opened_ns_ = frame_clock_ns();
    recorder_.open(opened_ns_);
    open_ = true;
}

FrameProbe::Counters FrameProbe::counters() const
{
    Counters out;
    out.received = link_.framesReceived();
    out.dropped_engine = link_.framesDroppedByEngine();
    out.dropped_client = link_.framesDroppedByUi();
    out.skipped = link_.framesSkipped();
    return out;
}

FrameRunInfo FrameProbe::runInfo() const
{
    FrameRunInfo run;
    run.platform = QGuiApplication::platformName().toStdString();
    run.render_thread = render_thread_.load(std::memory_order_relaxed);
    run.warmup_seconds = kFrameWarmupSeconds;

    const QQuickWindow* first = windows_.empty() ? nullptr : windows_.front().window.data();
    if (first != nullptr) {
        if (QSGRendererInterface* renderer = first->rendererInterface()) {
            run.graphics_api = graphics_api_name(renderer->graphicsApi());
        }
        if (const QRhi* rhi = first->rhi()) {
            run.adapter = rhi->driverInfo().deviceName.toStdString();
        }
        if (const QScreen* screen = first->screen()) {
            run.screen_name = screen->name().toStdString();
            run.refresh_hz = screen->refreshRate();
            run.screen_width = screen->geometry().width();
            run.screen_height = screen->geometry().height();
            run.device_pixel_ratio = screen->devicePixelRatio();
        }
    }

    if (open_) {
        run.measured_seconds = static_cast<double>(closed_ns_ - opened_ns_) / 1.0e9;
        const Counters now = counters();
        // A reconnect inside the run restarts the link's counters from zero,
        // and a difference across one would wrap. Reported as zero rather
        // than as eighteen quintillion frames.
        const auto since = [](std::uint64_t later, std::uint64_t earlier) {
            return later >= earlier ? later - earlier : 0;
        };
        run.engine_frames_received = since(now.received, at_open_.received);
        run.engine_frames_dropped_by_engine = since(now.dropped_engine, at_open_.dropped_engine);
        run.engine_frames_dropped_by_client = since(now.dropped_client, at_open_.dropped_client);
        run.engine_frames_skipped = since(now.skipped, at_open_.skipped);
        run.engine_frames_to_display = to_display_;
    }

    run.source_rate = link_.sourceRate();
    run.bins = link_.bins();
    run.realtime_factor = link_.realtimeFactor();
    run.receivers = link_.rackCount();
    run.detections = link_.detectionCount();
    return run;
}

void FrameProbe::finish()
{
    if (finished_) {
        return;
    }
    finished_ = true;
    closed_ns_ = frame_clock_ns();
    recorder_.close(closed_ns_);

    const FrameReport report = recorder_.report(runInfo());
    const std::string json = frame_stats_json(report);

    QFile file(path_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(json.data(), static_cast<qint64>(json.size())) !=
            static_cast<qint64>(json.size())) {
        std::fprintf(stderr, "--frame-stats: could not write %s\n", qPrintable(path_));
        return;
    }
    file.close();
    written_ = true;

    if (!open_) {
        std::fputs("frame-stats: no engine frame arrived, so nothing was measured\n", stderr);
        return;
    }
    std::fprintf(stderr, "%s\n", frame_stats_line(report).c_str());
}

}  // namespace revenant::ui
