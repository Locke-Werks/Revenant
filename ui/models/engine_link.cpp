#include "models/engine_link.h"

#include <chrono>
#include <cstdint>
#include <utility>

#include <QMetaObject>
#include <QString>

namespace revenant::ui {
namespace {

// How often the supervisor asks the engine whether it is still there, and
// how often it retries when there is nothing to connect to. One interval
// serves both because they are the same question asked from either side of
// the connection, and because an operator restarting an engine should see
// the window come back inside a breath rather than wonder whether it will.
constexpr std::chrono::milliseconds kSuperviseInterval{1000};

// A Rational times a plain multiplier, in hertz, with the multiply done
// before the divide. See EngineLink::frequencyAtFraction for why that order
// is the whole point.
//
// A zero denominator answers zero, which is what rpc::Rational::hertz()
// already decided a zero denominator means. A second policy here would make
// the same wire value mean two things depending on which one was called.
[[nodiscard]] double scaled_hertz(const rpc::Rational& value, double multiplier)
{
    return value.denominator == 0
               ? 0.0
               : multiplier * static_cast<double>(value.numerator) /
                     static_cast<double>(value.denominator);
}

}  // namespace

EngineLink::EngineLink(QObject* parent) : QObject(parent) {}

EngineLink::~EngineLink()
{
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        stopping_ = true;
    }
    supervisor_wake_.notify_all();

    // Joining is what makes the rest of this destructor safe, and there is
    // no shortcut. The supervisor owns the Client, the Client owns the event
    // loop thread, and the frame callback captures this and touches
    // staging_, swap_mutex_ and the pending counters. Only when the
    // supervisor has returned is every one of those finished with, because
    // destroying the Client is what joins the loop thread. A queued wake
    // still sitting in Qt's event queue is discarded when a QObject is
    // destroyed, so that one needs nothing here.
    if (supervisor_.joinable()) {
        supervisor_.join();
    }
}

void EngineLink::start(const QString& address, std::uint16_t port, std::uint32_t every_nth)
{
    address_ = address;
    port_ = port;
    // Zero and one both mean every frame, per client.h, and on_frame divides
    // by this. Normalised once here rather than guarded at every use.
    requested_every_nth_ = every_nth == 0 ? 1 : every_nth;
    endpoint_ = QStringLiteral("%1:%2").arg(address).arg(port);

    // Written before the thread exists, so the thread's construction is the
    // synchronisation and none of the three needs a lock.
    supervisor_ = std::thread([this] { supervise(); });
}

void EngineLink::supervise()
{
    for (;;) {
        if (client_ == nullptr) {
            static_cast<void>(attempt_connect());
        } else if (auto alive = client_->running(); !alive) {
            // The engine went away. core/rpc/client.cpp keeps the event loop
            // running through a lost connection and fails every later call,
            // so this is the only place that finds out, and the Client has
            // to be destroyed before another can be made: its loop thread is
            // joined by that destructor and nothing else.
            client_->unsubscribe_spectrum();
            client_.reset();
            publish(false, QString::fromStdString(alive.error().message));
        }

        std::unique_lock<std::mutex> lock(supervisor_mutex_);
        supervisor_wake_.wait_for(lock, kSuperviseInterval, [this] { return stopping_; });
        if (stopping_) {
            break;
        }
    }

    if (client_ != nullptr) {
        client_->unsubscribe_spectrum();
        client_.reset();
    }
}

bool EngineLink::attempt_connect()
{
    auto client = rpc::Client::connect(address_.toStdString(), port_);
    if (!client) {
        publish(false, QString::fromStdString(client.error().message));
        return false;
    }
    client_ = std::move(*client);

    auto info = client_->info();
    if (!info) {
        const QString message = QString::fromStdString(info.error().message);
        client_.reset();
        publish(false, message);
        return false;
    }

    // Everything the two engine-side counters are derived from, back to
    // zero. They describe one subscription, and the sum the header block
    // promises is only true within one: a sequence from the engine that just
    // died has nothing to say about the distance to a sequence from the one
    // that replaced it.
    every_nth_ = requested_every_nth_;
    first_sequence_ = 0;
    delivered_ = 0;
    have_span_ = false;
    {
        const std::lock_guard<std::mutex> lock(swap_mutex_);
        has_ready_ = false;
        pending_received_ = 0;
        pending_dropped_engine_ = 0;
        pending_dropped_ui_ = 0;
        pending_skipped_ = 0;
    }

    // Published before the subscription exists, so the Qt thread is told
    // about the new engine before the first frame drawn against it arrives.
    // The other order works, because an item reads a frame's own bin count
    // rather than EngineInfo's, but it puts a frame on screen under the
    // previous engine's frequency axis for as long as the queued adopt takes
    // to run, and that axis is a claim about where a signal is.
    {
        const std::lock_guard<std::mutex> lock(state_mutex_);
        handover_connected_ = true;
        handover_info_ = *info;
        handover_error_.clear();
    }
    QMetaObject::invokeMethod(this, [this] { adopt(); }, Qt::QueuedConnection);

    // An engine built with no spectrum stage is the default and is what a
    // headless recording runs. Connecting to one is not a failure, so the
    // window comes up, says the spectrum is off, and skips the subscription
    // rather than asking for frames that will never arrive.
    if (!info->spectrum.enabled()) {
        return true;
    }

    const auto status = client_->subscribe_spectrum(
        requested_every_nth_,
        [this](const rpc::SpectrumFrame& frame) { on_frame(frame); });
    if (!status) {
        const QString message = QString::fromStdString(status.error().message);
        client_.reset();
        publish(false, message);
        return false;
    }
    return true;
}

// Only ever called with connected false today: the successful path in
// attempt_connect has an EngineInfo to hand over as well, so it writes the
// handover itself. handover_info_ is deliberately left alone here, because a
// link that has just gone away has no geometry and QML gates every getter on
// connected; clearing it would buy nothing and would flicker the status line
// through zeroes when the same engine comes back.
void EngineLink::publish(bool connected, QString error)
{
    {
        const std::lock_guard<std::mutex> lock(state_mutex_);
        handover_connected_ = connected;
        handover_error_ = std::move(error);
    }
    QMetaObject::invokeMethod(this, [this] { adopt(); }, Qt::QueuedConnection);
}

void EngineLink::adopt()
{
    const bool was_connected = connected_;
    {
        const std::lock_guard<std::mutex> lock(state_mutex_);
        connected_ = handover_connected_;
        error_text_ = handover_error_;
        if (connected_) {
            info_ = handover_info_;
        }
    }

    if (connected_ && !was_connected) {
        // A fresh subscription counts from zero on the supervisor's side, so
        // the display's copies go with it rather than carrying the previous
        // engine's totals into a new sum.
        frames_received_ = 0;
        frames_dropped_engine_ = 0;
        frames_dropped_ui_ = 0;
        frames_skipped_ = 0;
        frames_drawn_ = 0;
    }
    if (!connected_) {
        frame_rate_ = 0.0;
    }
    rate_timer_.invalidate();
    rate_mark_ = frames_drawn_;

    emit connectionChanged();
    emit frameChanged();
}

QString EngineLink::deviceName() const
{
    return QString::fromStdString(info_.device.name);
}

QString EngineLink::deviceVendor() const
{
    return QString::fromStdString(info_.device.vendor);
}

QString EngineLink::clampReason() const
{
    return QString::fromStdString(info_.ring_clamp_reason);
}

double EngineLink::frequencyAtFraction(double fraction) const
{
    // Half a bin below the first bin's centre at fraction zero, half a bin
    // above the last bin's centre at fraction one: the outer edges, which is
    // where a display's own edges are and what tools/cli/main.cpp's axis
    // uses, so the same capture read in both places reports the same band.
    const double bin = fraction * static_cast<double>(info_.spectrum.bins) - 0.5;
    return static_cast<double>(info_.source_center) +
           scaled_hertz(info_.spectrum.bin_zero, 1.0) +
           scaled_hertz(info_.spectrum.bin_width, bin);
}

void EngineLink::on_frame(const rpc::SpectrumFrame& frame)
{
    // One copy, and it is unavoidable: the argument is the Client's and is
    // valid only for this call. Assigning into a vector this thread owns
    // reuses its capacity, so the copy is a memcpy after the first frame and
    // allocates nothing on the event loop thread.
    staging_ = frame;

    if (!have_span_) {
        first_sequence_ = frame.sequence;
        have_span_ = true;
    }
    ++delivered_;

    // sequence is the ENGINE's frame counter, not this subscription's, so
    // the distance from the first frame that arrived is exactly how many
    // frames the engine produced across the window this subscription has
    // watched. Both engine-side counters fall out of that one number.
    //
    // core/rpc/server.cpp offers a subscription only the frames whose
    // sequence is a multiple of its every_nth, so one in every_nth of the
    // window was ever a candidate and the rest were never copied. Of the
    // candidates, the ones that did not arrive are the ones the engine threw
    // away because the previous frame had not been answered yet.
    //
    // The division is exact and not a rounding: every sequence that reaches
    // this callback is a multiple of every_nth, so the distance between any
    // two of them is a multiple of it as well. These are counts of whole
    // frames throughout.
    const std::uint64_t stride = every_nth_;
    const std::uint64_t span =
        frame.sequence >= first_sequence_ ? frame.sequence - first_sequence_ : 0;
    const std::uint64_t offered = span / stride + 1;
    const std::uint64_t skipped = span - span / stride;
    const std::uint64_t dropped_by_engine = offered > delivered_ ? offered - delivered_ : 0;

    {
        const std::lock_guard<std::mutex> lock(swap_mutex_);
        if (has_ready_) {
            // The latest-wins replacement this file's header describes. The
            // frame being displaced has already been counted as received and
            // will never be drawn, and this is the only place that knows it
            // happened.
            ++pending_dropped_ui_;
        }
        std::swap(staging_, ready_);
        has_ready_ = true;
        pending_received_ = delivered_;
        pending_dropped_engine_ = dropped_by_engine;
        pending_skipped_ = skipped;
    }

    if (wake_pending_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    // The queued connection client.h asks for. A functor with this as the
    // context object is posted to the thread this object lives on, which is
    // the Qt thread; nothing about the QObject is read or written here
    // beyond what posting an event to it requires.
    QMetaObject::invokeMethod(this, [this] { drain(); }, Qt::QueuedConnection);
}

void EngineLink::drain()
{
    // Cleared before the buffer is taken, not after. A frame landing in the
    // window between the clear and the swap raises a fresh wake, which costs
    // one call here that finds nothing. Clearing afterwards would drop that
    // wake and leave the frame sitting until another arrived, which on a
    // quiet band is a display that stops for no visible reason.
    wake_pending_.store(false, std::memory_order_release);

    {
        const std::lock_guard<std::mutex> lock(swap_mutex_);
        if (!has_ready_) {
            return;
        }
        std::swap(ready_, display_);
        has_ready_ = false;

        // Taken under the same lock as the frame, so the numbers on screen
        // describe the frame on screen and add up against each other.
        //
        // Client::frames_received() would answer the first of these and
        // Client::frames_dropped() looks like it would answer the second.
        // Neither is read: the first moves between the swap and this drain,
        // and the second counts re-entrant delivery, which cannot happen.
        // The header block has the whole of it.
        frames_received_ = pending_received_;
        frames_dropped_engine_ = pending_dropped_engine_;
        frames_dropped_ui_ = pending_dropped_ui_;
        frames_skipped_ = pending_skipped_;
    }

    // Reaching here is one frame drawn, because every drain emits
    // frameChanged and every item repaints from it. Counting drains rather
    // than differencing frames_received_ is the whole point: that one moves
    // by more than a frame whenever the swap slot was overwritten, and a
    // rate taken from it reports the arrival rate while calling itself the
    // display's.
    ++frames_drawn_;

    // A rate over a window, restarted each time it is reported. Half a
    // second is long enough that the number is steady to read and short
    // enough that a display which stops says so before an operator has
    // finished noticing.
    constexpr qint64 kRateWindowMs = 500;
    if (!rate_timer_.isValid()) {
        rate_timer_.start();
        rate_mark_ = frames_drawn_;
    } else if (const qint64 elapsed = rate_timer_.elapsed(); elapsed >= kRateWindowMs) {
        frame_rate_ = static_cast<double>(frames_drawn_ - rate_mark_) * 1000.0 /
                      static_cast<double>(elapsed);
        rate_timer_.restart();
        rate_mark_ = frames_drawn_;
    }

    emit frameChanged();
}

}  // namespace revenant::ui
