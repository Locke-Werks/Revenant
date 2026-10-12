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

#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

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
        map.insert(QStringLiteral("directSamplingAvailable"), open.direct_sampling_available);
        map.insert(QStringLiteral("directSampling"),
                   QString::fromLatin1(rpc::direct_sampling_mode_name(open.direct_sampling)));
        map.insert(QStringLiteral("upconverter"), open.upconverter);
        map.insert(QStringLiteral("offsetTuningAvailable"), open.offset_tuning_available);
        map.insert(QStringLiteral("serial"), QString::fromStdString(open.serial));
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

void EngineLink::note_direct_branch(rpc::DirectSamplingMode branch)
{
    // On a change only, which under auto is a retune across the tuner's floor
    // and otherwise never.
    if (branch_posted_ && branch == posted_branch_) {
        return;
    }
    branch_posted_ = true;
    posted_branch_ = branch;
    {
        const std::lock_guard<std::mutex> lock(open_source_mutex_);
        handover_has_branch_ = true;
        handover_branch_ = branch;
    }
    QMetaObject::invokeMethod(this, [this] { adopt_open_source(); }, Qt::QueuedConnection);
}

void EngineLink::note_comb_status(const rpc::SourceStats& stats)
{
    // dsp::CombLock's values, which the schema carries as a number.
    QString text;
    bool locked = false;
    if (stats.comb_cancel) {
        switch (stats.comb_lock) {
            case 1: text = QStringLiteral("comb acquiring"); break;
            case 2:
                locked = true;
                text = QStringLiteral("comb locked %1 lines %2 dB")
                           .arg(stats.comb_lines)
                           .arg(-std::abs(static_cast<double>(stats.comb_suppression_db)), 0,
                                'f', 0);
                break;
            case 3: text = QStringLiteral("comb lost"); break;
            default: text = QStringLiteral("comb"); break;
        }
    }
    if (comb_posted_ && text == posted_comb_text_) {
        return;
    }
    comb_posted_ = true;
    posted_comb_text_ = text;
    {
        const std::lock_guard<std::mutex> lock(open_source_mutex_);
        handover_has_comb_ = true;
        handover_comb_text_ = text;
        handover_comb_locked_ = locked;
    }
    QMetaObject::invokeMethod(this, [this] { adopt_open_source(); }, Qt::QueuedConnection);
}

namespace {

// SourceStats' capture fields into the sample models/capture_note.h reads.
//
// A TEMPLATE WITH A REQUIRES TEST, because the wire carries captureOn and the
// rest (core/rpc/revenant.capnp @25 to @31) while rpc::SourceStats in
// core/rpc/types.h, which this client reads, does not copy them yet. Until it
// does the sample stays off and the readout says nothing, which is the honest
// answer for a client that cannot see the capture; once the fields land under
// these names the readout lights up with no change here.
template <typename Stats>
[[nodiscard]] CaptureSample capture_sample_of(const Stats& stats)
{
    CaptureSample sample;
    if constexpr (requires {
                      stats.capture_on;
                      stats.capture_segments_written;
                      stats.capture_bytes_written;
                      stats.capture_blocks_dropped;
                      stats.capture_segments_deleted;
                      stats.capture_retained_bytes;
                      stats.capture_last_error;
                  }) {
        sample.on = stats.capture_on;
        sample.segments_written = stats.capture_segments_written;
        sample.bytes_written = stats.capture_bytes_written;
        sample.blocks_dropped = stats.capture_blocks_dropped;
        sample.segments_deleted = stats.capture_segments_deleted;
        sample.retained_bytes = stats.capture_retained_bytes;
        sample.last_error = std::string(stats.capture_last_error);
    }
    return sample;
}

}  // namespace

void EngineLink::note_capture_status(const rpc::SourceStats& stats)
{
    CaptureLine line = describe_capture(capture_sample_of(stats));

    // Posted on a change only, like the comb's text: the retained size moves
    // with every segment and nothing else here does.
    if (capture_posted_ && line.state == posted_capture_.state &&
        line.text == posted_capture_.text && line.detail == posted_capture_.detail) {
        return;
    }
    capture_posted_ = true;
    posted_capture_ = line;
    {
        const std::lock_guard<std::mutex> lock(open_source_mutex_);
        handover_has_capture_ = true;
        handover_capture_ = std::move(line);
    }
    QMetaObject::invokeMethod(this, [this] { adopt_open_source(); }, Qt::QueuedConnection);
}

void EngineLink::adopt_open_source()
{
    bool capture_moved = false;
    bool comb_moved = false;
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
        if (handover_has_branch_) {
            handover_has_branch_ = false;
            direct_branch_ = QString::fromLatin1(rpc::direct_sampling_mode_name(handover_branch_));
            moved = true;
        }
        if (handover_has_comb_) {
            handover_has_comb_ = false;
            comb_text_ = handover_comb_text_;
            comb_locked_ = handover_comb_locked_;
            comb_moved = true;
        }
        if (handover_has_capture_) {
            handover_has_capture_ = false;
            capture_text_ = QString::fromStdString(handover_capture_.text);
            capture_detail_ = QString::fromStdString(handover_capture_.detail);
            capture_fault_ = handover_capture_.state == CaptureState::Fault;
            capture_moved = true;
        }
    }
    if (moved) {
        emit openedSourceChanged();
    }
    if (comb_moved) {
        emit combStatusChanged();
    }
    if (capture_moved) {
        emit captureStatusChanged();
    }
}

}  // namespace revenant::ui
