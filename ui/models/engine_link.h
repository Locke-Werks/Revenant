// The one object that owns the connection, and the only place a frame
// crosses from the Cap'n Proto event loop onto the Qt thread.
//
// THE THREAD RULE THIS FILE EXISTS TO HOLD
//
// core/rpc/client.h is explicit: every Client method is synchronous from the
// caller's point of view, but the spectrum callback is invoked ON the event
// loop thread, must not call back into the same Client, and must return
// quickly, because everything else that connection does is waiting behind
// it. Nothing about a QObject may be touched from there.
//
// So the callback does three things and no more: it copies the frame into a
// buffer it owns alone, swaps that buffer under a small mutex, and posts a
// wake. The wake is a queued metacall, which is the documented way to reach
// a QObject from a foreign thread, and it carries no payload, so Qt's event
// queue never holds a megabyte of bins.
//
// WHY LATEST-WINS AND NOT A QUEUE
//
// A frame at the shipped geometry is 256 KiB and the engine makes 305 a
// second. Handing each one to a queued signal would copy it into the event
// queue and grow without bound the moment the GUI thread stalls, which is
// the failure core/rpc/server.h already decided against on the wire. Three
// buffers cycle here instead: the callback's staging copy, the one waiting,
// and the one the items are drawing. A frame arriving before the last was
// drawn replaces it. For a waterfall that is the right answer anyway, since
// the newest frame is the one worth drawing.
//
// THE THREE PLACES A FRAME GOES MISSING, AND WHICH COUNTER HOLDS EACH
//
// This block used to end by saying that Client::frames_dropped reported the
// same decision taken one layer down, so a choppy display could say which
// layer skipped. It reported no such thing, and the correction is recorded
// rather than quietly swapped, because the sentence was plausible enough to
// be believed twice.
//
// That counter is structurally zero. core/rpc/client.cpp increments it only
// on re-entrant delivery, and the client cannot re-enter: frame() is
// answered after the callback returns, and the loop thread is the only
// thread that dispatches frame(). tests/rpc/test_rpc_spectrum.cpp pins it at
// zero against a subscriber slow enough to make the engine drop, on purpose.
// Meanwhile the replacement this file does in on_frame was counted nowhere.
// So the status line read "0 dropped" however badly the GUI stalled, which
// is the one thing a dropped-frame counter must never do.
//
// What is true is that a client which cannot keep up makes the ENGINE drop,
// and the only trace of that on this side is SpectrumFrame::sequence. It is
// the engine's own frame counter, not this subscription's, so the distance
// between two frames that arrive says exactly how many the engine made in
// between and did not send. Split by cause, which is the whole point of
// counting at all:
//
//   framesSkipped          the engine never offered these, because this
//                          client asked for one frame in every_nth. Not a
//                          loss: it grows steadily whenever every_nth is
//                          above one and a healthy display has a large one.
//   framesDroppedByEngine  the engine had a frame at the rate asked for and
//                          threw it away, because the previous one had not
//                          been answered yet, or because its own loop had
//                          not got to it. With one subscriber those are the
//                          same stall: the callback, or whatever sits behind
//                          it, is too slow.
//   framesDroppedByUi      this link replaced a frame in the hand-off slot
//                          before the Qt thread came for it. The GUI thread
//                          is too slow, which is a different stall from the
//                          one above and has a different fix.
//
// framesReceived plus framesDroppedByEngine plus framesSkipped is every
// frame the engine produced between the first that arrived and the latest,
// so a set that stops adding up is a counter that has drifted.
// framesDroppedByUi is not in that sum: those frames did arrive and were
// counted, and were then thrown away here.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include <QObject>
#include <QString>
#include <QtQmlIntegration>

#include "core/rpc/client.h"
#include "core/rpc/types.h"

namespace revenant::ui {

class EngineLink : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Constructed by main() with the address and port from argv.")

    Q_PROPERTY(bool connected READ connected NOTIFY connectionChanged)
    Q_PROPERTY(QString endpoint READ endpoint CONSTANT)
    Q_PROPERTY(QString errorText READ errorText NOTIFY connectionChanged)

    // EngineInfo, flattened to what a status line shows. Constant after a
    // successful connect: the engine's geometry is fixed at Engine::create
    // and a change to it is a new engine rather than a new value here.
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY connectionChanged)
    Q_PROPERTY(QString deviceVendor READ deviceVendor NOTIFY connectionChanged)
    Q_PROPERTY(bool deviceDiscrete READ deviceDiscrete NOTIFY connectionChanged)
    Q_PROPERTY(int sourceRate READ sourceRate NOTIFY connectionChanged)
    Q_PROPERTY(int channelRate READ channelRate NOTIFY connectionChanged)
    Q_PROPERTY(int gridChannels READ gridChannels NOTIFY connectionChanged)
    Q_PROPERTY(int bins READ bins NOTIFY connectionChanged)
    Q_PROPERTY(bool spectrumEnabled READ spectrumEnabled NOTIFY connectionChanged)

    // The engine did not build what was asked for, and the sentence saying
    // so. Constant after a successful connect for the same reason as the
    // fields above it, and false on a connection that never came up.
    //
    // A clamp is silent everywhere else: the engine takes 2048 channels when
    // 4096 were asked for and goes on serving frames that look correct, and
    // the operator finds out when a frequency lands in the wrong channel.
    // core/engine/engine.cpp says exactly that where it sets the field.
    Q_PROPERTY(bool clamped READ clamped NOTIFY connectionChanged)
    Q_PROPERTY(QString clampReason READ clampReason NOTIFY connectionChanged)

    // The span the frames cover, in absolute hertz. Computed the way
    // tools/cli/main.cpp computes its axis: source_center plus bin_zero plus
    // the half-bin offset to the outer edge. Doubles, because this is the
    // one place a rational is allowed to become a number: it is a label.
    Q_PROPERTY(double spanLowHz READ spanLowHz NOTIFY connectionChanged)
    Q_PROPERTY(double spanHighHz READ spanHighHz NOTIFY connectionChanged)

    // Per frame. framesDropped is the two unrequested losses added together,
    // which is the one number a status line has room for; the two beside it
    // say which layer, and framesSkipped is decimation that was asked for
    // rather than a loss at all. See the block at the top of this file.
    Q_PROPERTY(qulonglong framesReceived READ framesReceived NOTIFY frameChanged)
    Q_PROPERTY(qulonglong framesDropped READ framesDropped NOTIFY frameChanged)
    Q_PROPERTY(qulonglong framesDroppedByEngine READ framesDroppedByEngine NOTIFY frameChanged)
    Q_PROPERTY(qulonglong framesDroppedByUi READ framesDroppedByUi NOTIFY frameChanged)
    Q_PROPERTY(qulonglong framesSkipped READ framesSkipped NOTIFY frameChanged)
    Q_PROPERTY(qulonglong sequence READ sequence NOTIFY frameChanged)
    Q_PROPERTY(double floorDb READ floorDb NOTIFY frameChanged)
    Q_PROPERTY(double ceilingDb READ ceilingDb NOTIFY frameChanged)

public:
    explicit EngineLink(QObject* parent = nullptr);
    ~EngineLink() override;

    EngineLink(const EngineLink&) = delete;
    EngineLink& operator=(const EngineLink&) = delete;
    EngineLink(EngineLink&&) = delete;
    EngineLink& operator=(EngineLink&&) = delete;

    // Connects, reads info() and subscribes. Returns false and fills
    // errorText rather than throwing, because a refused connection is the
    // ordinary case when the engine has not been started yet and the window
    // should come up and say so.
    //
    // every_nth is passed straight to Client::subscribe_spectrum. The engine
    // drops the rest before copying them, so asking for fewer costs it less.
    bool open(const QString& address, std::uint16_t port, std::uint32_t every_nth);

    [[nodiscard]] bool connected() const { return client_ != nullptr; }
    [[nodiscard]] QString endpoint() const { return endpoint_; }
    [[nodiscard]] QString errorText() const { return error_text_; }

    [[nodiscard]] QString deviceName() const;
    [[nodiscard]] QString deviceVendor() const;
    [[nodiscard]] bool deviceDiscrete() const { return info_.device.discrete; }
    [[nodiscard]] int sourceRate() const { return static_cast<int>(info_.source_rate); }
    [[nodiscard]] int channelRate() const { return static_cast<int>(info_.channel_rate); }
    [[nodiscard]] int gridChannels() const { return static_cast<int>(info_.grid.channels); }
    [[nodiscard]] int bins() const { return static_cast<int>(info_.spectrum.bins); }
    [[nodiscard]] bool spectrumEnabled() const { return info_.spectrum.enabled(); }
    [[nodiscard]] double spanLowHz() const;
    [[nodiscard]] double spanHighHz() const;

    [[nodiscard]] bool clamped() const { return info_.ring_clamped; }
    [[nodiscard]] QString clampReason() const;

    [[nodiscard]] qulonglong framesReceived() const { return frames_received_; }
    [[nodiscard]] qulonglong framesDropped() const {
        return frames_dropped_engine_ + frames_dropped_ui_;
    }
    [[nodiscard]] qulonglong framesDroppedByEngine() const { return frames_dropped_engine_; }
    [[nodiscard]] qulonglong framesDroppedByUi() const { return frames_dropped_ui_; }
    [[nodiscard]] qulonglong framesSkipped() const { return frames_skipped_; }
    [[nodiscard]] qulonglong sequence() const { return display_.sequence; }
    [[nodiscard]] double floorDb() const { return display_.floor_db; }
    [[nodiscard]] double ceilingDb() const { return display_.ceiling_db; }

    // The frame the items draw. Qt thread only, and valid until the next
    // frameChanged: the buffer it refers to is swapped back into the pool
    // when the next frame is taken. An item that wants to keep a row copies
    // the row, not this.
    [[nodiscard]] const rpc::SpectrumFrame& frame() const { return display_; }

signals:
    void connectionChanged();

    // A new frame is in frame(). Emitted on the Qt thread, so an item may
    // connect to it directly and repaint from the slot.
    void frameChanged();

private:
    // Invoked on the Cap'n Proto event loop thread. Touches nothing but its
    // own buffers, the swap mutex and one atomic.
    void on_frame(const rpc::SpectrumFrame& frame);

    // Qt thread. Takes whatever is waiting and tells the items.
    void drain();

    std::unique_ptr<rpc::Client> client_;
    QString endpoint_;
    QString error_text_;
    rpc::EngineInfo info_;

    // Event loop thread only, so the copy out of the callback's argument
    // happens outside the lock and reuses this vector's capacity.
    rpc::SpectrumFrame staging_;

    // Event loop thread only as well, and the arithmetic behind the two
    // engine-side counters. every_nth_ is written by open() before
    // subscribe_spectrum is called, and that call synchronises with the loop
    // thread on its way in, so the loop thread cannot read a stale one.
    std::uint32_t every_nth_ = 1;
    std::uint64_t first_sequence_ = 0;
    std::uint64_t delivered_ = 0;
    bool have_span_ = false;

    std::mutex swap_mutex_;
    rpc::SpectrumFrame ready_;  // guarded by swap_mutex_
    bool has_ready_ = false;    // guarded by swap_mutex_

    // Written on the event loop thread, read by drain(), both under
    // swap_mutex_, so the frame and the four counters that describe it are
    // one snapshot rather than values read at four instants. The received
    // count is in here rather than taken from Client::frames_received()
    // for that reason alone: frames keep arriving between the swap and the
    // drain, and a received count that had moved on while the other three
    // had not would break the sum the header block promises.
    qulonglong pending_received_ = 0;        // guarded by swap_mutex_
    qulonglong pending_dropped_engine_ = 0;  // guarded by swap_mutex_
    qulonglong pending_dropped_ui_ = 0;      // guarded by swap_mutex_
    qulonglong pending_skipped_ = 0;         // guarded by swap_mutex_

    // Qt thread only.
    rpc::SpectrumFrame display_;
    qulonglong frames_received_ = 0;
    qulonglong frames_dropped_engine_ = 0;
    qulonglong frames_dropped_ui_ = 0;
    qulonglong frames_skipped_ = 0;

    // One wake outstanding at a time. Without it a burst of frames posts a
    // metacall each, and the GUI thread then runs a queue of calls that all
    // find the same single frame waiting.
    std::atomic<bool> wake_pending_{false};
};

}  // namespace revenant::ui
