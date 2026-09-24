#include "render/window_pacer.h"

#include <chrono>
#include <cmath>

#include <QCoreApplication>
#include <QEvent>
#include <QScreen>
#include <QSurfaceFormat>

// QPlatformWindow::hasPendingUpdateRequest, the one place Qt says whether a
// window has asked for a frame that has not been delivered yet.
#include <qpa/qplatformwindow.h>

namespace revenant::ui {

namespace {

[[nodiscard]] std::int64_t steady_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

bool WindowPacer::unthrottle(QQuickWindow* follower)
{
    if (follower == nullptr) {
        return false;
    }
    QSurfaceFormat format = follower->requestedFormat();
    format.setSwapInterval(0);
    follower->setFormat(format);
    return follower->handle() == nullptr;
}

WindowPacer::WindowPacer(QQuickWindow* lead, QQuickWindow* follower, QObject* parent)
    : QObject(parent), lead_(lead), follower_(follower)
{
    // Precise, because a coarse timer may be 5% late, and a late timer here is
    // a frame late.
    for (QTimer* timer : {&lead_hold_, &follower_fallback_}) {
        timer->setSingleShot(true);
        timer->setTimerType(Qt::PreciseTimer);
    }
    connect(&lead_hold_, &QTimer::timeout, this, [this] {
        if (lead_held_ && lead_ != nullptr) {
            lead_held_ = false;
            QEvent request(QEvent::UpdateRequest);
            drawLead(&request);
        }
    });
    connect(&follower_fallback_, &QTimer::timeout, this, [this] {
        if (follower_held_ && follower_ != nullptr) {
            drawFollower();
        }
    });

    if (lead_ == nullptr || follower_ == nullptr) {
        return;
    }
    // Direct, because it is emitted on the render thread and the time wanted
    // is when the frame went out, not when the GUI thread heard about it.
    connect(
        lead_, &QQuickWindow::frameSwapped, this,
        [this] { lead_swapped_ns_.store(steady_ns(), std::memory_order_relaxed); },
        Qt::DirectConnection);
    lead_->installEventFilter(this);
    follower_->installEventFilter(this);
}

bool WindowPacer::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::UpdateRequest || lead_ == nullptr || follower_ == nullptr) {
        return false;
    }

    if (watched == lead_.data()) {
        if (in_lead_) {
            return false;
        }
        const std::int64_t swapped = lead_swapped_ns_.load(std::memory_order_relaxed);
        const auto ready =
            swapped + static_cast<std::int64_t>(std::llround(0.5 * periodMs() * 1.0e6));
        const std::int64_t now = steady_ns();
        if (swapped != 0 && now < ready) {
            lead_held_ = true;
            if (!lead_hold_.isActive()) {
                // Rounded up: early would only park the GUI thread again.
                lead_hold_.start(static_cast<int>((ready - now + 999'999) / 1'000'000));
            }
            return true;
        }
        lead_hold_.stop();
        lead_held_ = false;
        drawLead(event);
        return true;
    }

    if (watched == follower_.data()) {
        if (in_follower_) {
            return false;
        }
        follower_held_ = true;
        if (!follower_fallback_.isActive()) {
            follower_fallback_.start(static_cast<int>(std::lround(1.5 * periodMs())));
        }
        return true;
    }
    return false;
}

void WindowPacer::drawLead(QEvent* event)
{
    // Delivered from here rather than returned to Qt, so that this call is
    // still on the stack when the hand-over is done: sendEvent returns after
    // the render thread has passed the vsync wait and synced, which is the
    // moment to draw the other window.
    in_lead_ = true;
    QCoreApplication::sendEvent(lead_, event);
    in_lead_ = false;
    if (followerWaiting()) {
        drawFollower();
    }
}

bool WindowPacer::followerWaiting() const
{
    if (follower_ == nullptr || !follower_->isVisible()) {
        return false;
    }
    if (follower_held_) {
        return true;
    }
    // Asked for and not yet delivered: the display items ask on every engine
    // frame, and the platform delivers the request a couple of milliseconds
    // later, which is often after the main window's frame has gone. Counting
    // only requests already caught drew the receiver window 5679 times in 61 s
    // against the main window's 7154.
    const QPlatformWindow* platform = follower_->handle();
    return platform != nullptr && platform->hasPendingUpdateRequest();
}

void WindowPacer::drawFollower()
{
    follower_held_ = false;
    follower_fallback_.stop();
    in_follower_ = true;
    QEvent request(QEvent::UpdateRequest);
    QCoreApplication::sendEvent(follower_, &request);
    in_follower_ = false;
}

double WindowPacer::periodMs() const
{
    double hz = 60.0;
    if (lead_ != nullptr && lead_->screen() != nullptr && lead_->screen()->refreshRate() > 1.0) {
        hz = lead_->screen()->refreshRate();
    }
    return 1000.0 / hz;
}

}  // namespace revenant::ui
