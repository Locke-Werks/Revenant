// Paces the client's two windows so that the one GUI thread they share waits
// for vsync once a refresh and not twice, and not for most of every refresh.
//
// WHY THIS EXISTS. Qt's threaded render loop gives each window a render
// thread, but the GUI thread still hands every frame over in
// QSGThreadedRenderLoop::polishAndSync, which posts the sync to the render
// thread and blocks until it is done. Qt 6 begins the frame before the sync
// (QSGRenderThread::syncAndRender, "Begin the frame before syncing"), and on
// Direct3D 11 beginning a frame is where the swap chain waits for the display:
// QRhiD3D11::beginFrame waits on the frame latency waitable object. So a
// vsync-paced window holds the GUI thread through its vsync wait on every
// frame, and with two of them the GUI thread was held through one window's
// wait and then the other's. Measured on 2026-09-23 at 120 Hz with both
// windows kept on top: 16.9% of the main window's frames missed a refresh,
// and over them the render thread had waited 14.9 ms to be asked.
//
// WHAT IT DOES, THREE THINGS.
//
// The receiver window gets a swap interval of 0, for which Qt makes its swap
// chain without the waitable object and presents without waiting, so its
// render thread never waits for the display and neither does the GUI thread
// on its behalf.
//
// Its update requests are then held here, and it is drawn straight after each
// main-window frame is handed over, which is just after the main window's
// vsync, so it still draws once a refresh and in step with the main window.
// Unpaced, it drew 8174 frames in 61 s at whatever moment the GUI thread was
// free, and 17.0% of them came more than 1.5 refreshes apart.
//
// And the main window's own request is held for half a refresh after its last
// frame was presented. Its render thread cannot begin the next frame before
// the next vsync anyway, so a request made sooner only parks the GUI thread in
// polishAndSync, and a GUI thread parked there cannot take engine frames: the
// client's latest-wins slot replaced 41% of them with no hold and 7.8% with
// one of 4 ms, over 60 s runs each.
//
// When the main window is not drawing, because nothing on it changed or it is
// minimised, a held receiver request is let through after one and a half
// refresh periods instead, so the receiver window never waits on a window
// that is not going to draw.
//
// A window with a swap interval of 0 may tear where Windows gives it an
// independent flip, which it does only to a window covering a whole screen
// with no frame. The receiver window keeps its frame, and is drawn just after
// the vblank in any case.
//
// SINCE 2026-09-23 THE RECEIVERS START DOCKED IN THE MAIN WINDOW and the
// receiver window is shown only while they are popped out. The pacer stays
// installed either way. A hidden follower asks for no frames, so the first two
// of the three things above, which are the receiver window's, do nothing while
// it is hidden, and the third, the main window's hold, is worth keeping on its
// own: with the receiver window
// closed it cut the engine frames the client replaced from 42.4% to 12.8%.
// ui/main.cpp says the same where it installs this.

#pragma once

#include <atomic>
#include <cstdint>

#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QTimer>

class QEvent;

namespace revenant::ui {

class WindowPacer : public QObject {
public:
    // Asks for follower to be presented without waiting for vsync. Qt reads
    // the swap interval when the window's swap chain is made, so this has to
    // run before the window is first shown; it returns false, and changes
    // nothing that will take effect, if the window already exists.
    static bool unthrottle(QQuickWindow* follower);

    // Paces lead and draws follower after each of lead's frames, from now on.
    // Neither window is owned; both are watched and either may go first.
    WindowPacer(QQuickWindow* lead, QQuickWindow* follower, QObject* parent = nullptr);

    WindowPacer(const WindowPacer&) = delete;
    WindowPacer& operator=(const WindowPacer&) = delete;
    WindowPacer(WindowPacer&&) = delete;
    WindowPacer& operator=(WindowPacer&&) = delete;
    ~WindowPacer() override = default;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void drawLead(QEvent* event);
    [[nodiscard]] bool followerWaiting() const;
    void drawFollower();
    [[nodiscard]] double periodMs() const;

    QPointer<QQuickWindow> lead_;
    QPointer<QQuickWindow> follower_;

    // Lets a held lead request through half a refresh after the last swap.
    QTimer lead_hold_;
    // Lets a held follower request through when the lead is not drawing.
    QTimer follower_fallback_;

    // When the lead last presented, in steady_clock nanoseconds, written on
    // its render thread and read here. Zero until the first frame.
    std::atomic<std::int64_t> lead_swapped_ns_{0};

    // Set while this object re-sends an update request it caught, so that the
    // re-sent one goes through.
    bool in_lead_ = false;
    bool in_follower_ = false;

    // A window asked for a frame and has not been drawn since.
    bool lead_held_ = false;
    bool follower_held_ = false;
};

}  // namespace revenant::ui
