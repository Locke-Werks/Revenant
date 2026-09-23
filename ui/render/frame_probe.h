// The Qt half of `revenant-ui --frame-stats FILE`: reads frame timestamps off
// each QQuickWindow and the engine link's counters, and hands them to
// models/frame_stats.h, which does the arithmetic and writes the JSON.
//
// Nothing here exists in a run without the flag. main.cpp constructs one of
// these only when the flag is given, and the display items' hooks are
// models/frame_stats.h's FrameCost, which is a relaxed load and a branch
// while no recorder is installed.

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QString>

#include "models/engine_link.h"
#include "models/frame_stats.h"

namespace revenant::ui {

class FrameProbe : public QObject {
    Q_OBJECT

public:
    // Called before QGuiApplication exists, because the scene graph reads
    // whether to take GPU timestamps when it creates its QRhi, and that is
    // earlier than any window this process can reach.
    static void prepare();

    FrameProbe(QString path, EngineLink& link, QObject* parent = nullptr);
    ~FrameProbe() override;

    FrameProbe(const FrameProbe&) = delete;
    FrameProbe& operator=(const FrameProbe&) = delete;
    FrameProbe(FrameProbe&&) = delete;
    FrameProbe& operator=(FrameProbe&&) = delete;

    // Starts timing a window's frames under name. The first window watched
    // is the one the stderr line and the screen's refresh rate come from.
    void watch(QQuickWindow* window, const QString& name);

    // Closes the measured window, writes the JSON and prints the line.
    // Called once, on the way out.
    void finish();

    // Whether finish wrote the file. A run asked for stats that could not
    // write them has failed, whatever else it did.
    [[nodiscard]] bool written() const { return written_; }

private:
    struct Counters {
        std::uint64_t received = 0;
        std::uint64_t dropped_engine = 0;
        std::uint64_t dropped_client = 0;
        std::uint64_t skipped = 0;
    };

    struct Watched {
        QPointer<QQuickWindow> window;
        int index = 0;
    };

    void onEngineFrame();
    void openMeasurement();
    [[nodiscard]] Counters counters() const;
    [[nodiscard]] FrameRunInfo runInfo() const;

    QString path_;
    EngineLink& link_;
    FrameRecorder recorder_;
    std::vector<Watched> windows_;

    bool warmup_started_ = false;
    bool open_ = false;
    bool finished_ = false;
    bool written_ = false;
    std::int64_t opened_ns_ = 0;
    std::int64_t closed_ns_ = 0;
    Counters at_open_;
    std::uint64_t to_display_ = 0;

    std::atomic<bool> render_thread_{false};
};

}  // namespace revenant::ui
