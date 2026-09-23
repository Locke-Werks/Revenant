// EngineLink's account of which source is open, by name, and how far into it
// the engine has read.
//
// WHY IT EXISTS. Until the recording section, nothing in the window named the
// source that was open: source_link.cpp's note_source_epoch records that the
// identity an operator read after a switch was the row they had picked in the
// picker. A recording needs more than a row. The strip under the top bar shows
// its name, its position against its length and its pace, and all three belong
// to the source the ENGINE has open, which may be one this window never picked:
// an engine started with a URI on its command line, or one another window
// opened.
//
// NO NEW ROUND TRIP ON THE ORDINARY PASS. The descriptor is Session.
// sourceDescriptor, fetched once per epoch per connection, the way
// poll_source_gain_stage fetches the same call for the gain stage. The
// delivered count rides on the SourceStats poll_front_end already makes.

#include "models/engine_link.h"

#include <cstdint>
#include <mutex>

#include <QMetaObject>
#include <QString>
#include <QVariantMap>

#include "core/rpc/client.h"
#include "core/rpc/types.h"

namespace revenant::ui {

void EngineLink::poll_open_source(std::uint64_t epoch)
{
    if (client_ == nullptr) {
        return;
    }

    // ONCE PER EPOCH, EXCEPT WHILE A RECORDING IS OPEN. A close with no open
    // after it leaves the epoch where it was, since the engine keeps the
    // number of the stream it last served, so an epoch test alone would leave
    // a closed recording's name under the top bar for as long as the window
    // stayed up. While a recording is open this asks every pass, which is one
    // round trip a second for as long as a file plays and none for a radio.
    const bool recording =
        posted_open_source_.value(QStringLiteral("lengthSamples")).toULongLong() > 0;
    if (open_source_read_ && epoch == open_source_epoch_ && !recording) {
        return;
    }

    auto described = client_->source_descriptor();
    if (!described) {
        // Left to the next pass, as poll_source_gain_stage leaves it. The
        // liveness probe owns a lost engine.
        return;
    }
    const bool new_stream = !open_source_read_ || epoch != open_source_epoch_;
    open_source_epoch_ = epoch;
    open_source_read_ = true;

    QVariantMap map;
    if (described->has_value()) {
        const rpc::SourceDescriptor& open = **described;
        map.insert(QStringLiteral("uri"), QString::fromStdString(open.uri));
        map.insert(QStringLiteral("backend"), QString::fromStdString(open.backend));
        map.insert(QStringLiteral("displayName"), QString::fromStdString(open.display_name));
        map.insert(QStringLiteral("lengthSamples"),
                   static_cast<qulonglong>(open.length_samples));

        // A file reports its one rate as both bounds, so the maximum is its
        // rate. A dongle's maximum is not its rate, which is why nothing reads
        // this for a live device: EngineInfo::sourceRate is that.
        map.insert(QStringLiteral("rate"), static_cast<qlonglong>(open.max_rate));
        map.insert(QStringLiteral("format"),
                   QString::fromLatin1(rpc::sample_format_name(open.native_format)));
        map.insert(QStringLiteral("seekable"), open.seekable);
        map.insert(QStringLiteral("epoch"), static_cast<qulonglong>(epoch));
    }

    if (!new_stream && map == posted_open_source_) {
        return;
    }
    posted_open_source_ = map;

    {
        const std::lock_guard<std::mutex> lock(open_source_mutex_);
        handover_has_open_source_ = true;
        handover_open_source_ = map;

        // A new stream starts its count again. Posted with the descriptor so
        // the strip never draws one source's name over the last one's
        // position.
        if (new_stream) {
            delivered_posted_ = false;
            handover_has_delivered_ = true;
            handover_delivered_ = 0;
        }
    }
    QMetaObject::invokeMethod(this, [this] { adopt_open_source(); }, Qt::QueuedConnection);
}

void EngineLink::note_samples_delivered(std::uint64_t delivered)
{
    // Posted on a change only. On a live radio it changes every pass, which is
    // one metacall a second; on an engine between sources it does not change
    // at all and wakes nothing.
    if (delivered_posted_ && delivered == posted_delivered_) {
        return;
    }
    delivered_posted_ = true;
    posted_delivered_ = delivered;
    {
        const std::lock_guard<std::mutex> lock(open_source_mutex_);
        handover_has_delivered_ = true;
        handover_delivered_ = delivered;
    }
    QMetaObject::invokeMethod(this, [this] { adopt_open_source(); }, Qt::QueuedConnection);
}

void EngineLink::adopt_open_source()
{
    bool moved = false;
    {
        const std::lock_guard<std::mutex> lock(open_source_mutex_);
        if (handover_has_open_source_) {
            handover_has_open_source_ = false;
            open_source_ = handover_open_source_;
            moved = true;
        }
        if (handover_has_delivered_) {
            handover_has_delivered_ = false;
            samples_delivered_ = static_cast<qulonglong>(handover_delivered_);
            moved = true;
        }
    }
    if (moved) {
        emit openedSourceChanged();
    }
}

}  // namespace revenant::ui
