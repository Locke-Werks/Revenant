#include "models/engine_link.h"

#include <cstdint>
#include <utility>

#include <QMetaObject>
#include <QString>

namespace revenant::ui {

EngineLink::EngineLink(QObject* parent) : QObject(parent) {}

EngineLink::~EngineLink()
{
    if (client_ == nullptr) {
        return;
    }

    // Order matters and there is only one order that is safe. The callback
    // captures this, so the subscription has to end before any member it
    // touches goes away, and destroying the Client is what joins the event
    // loop thread. A queued wake still sitting in Qt's event queue is
    // discarded when a QObject is destroyed, so that one needs nothing here.
    client_->unsubscribe_spectrum();
    client_.reset();
}

bool EngineLink::open(const QString& address, std::uint16_t port, std::uint32_t every_nth)
{
    endpoint_ = QStringLiteral("%1:%2").arg(address).arg(port);

    auto client = rpc::Client::connect(address.toStdString(), port);
    if (!client) {
        error_text_ = QString::fromStdString(client.error().message);
        emit connectionChanged();
        return false;
    }
    client_ = std::move(*client);

    auto info = client_->info();
    if (!info) {
        error_text_ = QString::fromStdString(info.error().message);
        client_.reset();
        emit connectionChanged();
        return false;
    }
    info_ = std::move(*info);

    // An engine built with no spectrum stage is the default and is what a
    // headless recording runs. Connecting to one is not a failure, so the
    // window comes up, says the spectrum is off, and skips the subscription
    // rather than asking for frames that will never arrive.
    if (info_.spectrum.enabled()) {
        // Zero and one both mean every frame, per client.h, and on_frame
        // divides by this. Normalised once here rather than guarded at every
        // use, and set before the subscription exists so the loop thread
        // cannot see the default.
        every_nth_ = every_nth == 0 ? 1 : every_nth;
        first_sequence_ = 0;
        delivered_ = 0;
        have_span_ = false;

        const auto status =
            client_->subscribe_spectrum(every_nth, [this](const rpc::SpectrumFrame& frame) {
                on_frame(frame);
            });
        if (!status) {
            error_text_ = QString::fromStdString(status.error().message);
            client_.reset();
            emit connectionChanged();
            return false;
        }
    }

    error_text_.clear();
    emit connectionChanged();
    return true;
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

double EngineLink::spanLowHz() const
{
    // The outer edge of bin zero, half a bin below its centre. Matching
    // tools/cli/main.cpp's axis exactly, so the same capture read in both
    // places reports the same band edges.
    return static_cast<double>(info_.source_center) + info_.spectrum.bin_zero.hertz() -
           0.5 * info_.spectrum.bin_width.hertz();
}

double EngineLink::spanHighHz() const
{
    return static_cast<double>(info_.source_center) + info_.spectrum.bin_zero.hertz() +
           (static_cast<double>(info_.spectrum.bins) - 0.5) * info_.spectrum.bin_width.hertz();
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

    emit frameChanged();
}

}  // namespace revenant::ui
