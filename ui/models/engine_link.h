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
// A frame at the shipped geometry is 65536 bins of float, which is 256 KiB,
// and the engine makes one per source block. Handing each one to a queued
// signal would copy it into the event queue and grow without bound the
// moment the GUI thread stalls, which is the failure core/rpc/server.h
// already decided against on the wire. Three buffers cycle here instead: the
// callback's staging copy, the one waiting, and the one the items are
// drawing. A frame arriving before the last was drawn replaces it. For a
// waterfall that is the right answer anyway, since the newest frame is the
// one worth drawing.
//
// THE THREE THREADS
//
// The Qt thread owns everything a property getter reads and everything an
// item draws. The Cap'n Proto event loop thread, inside the Client, runs
// on_frame. A third thread, the supervisor started by start(), owns the
// Client itself: it connects, subscribes, and from then on asks the engine
// once a second whether it is still there.
//
// The supervisor exists because both halves of that job can block for as
// long as the far end takes. Client::connect does a TCP connect, and
// Client::running waits for a round trip on the loop thread; core/rpc/
// client.cpp's run() is explicit that a lost connection does not end the
// loop, it just makes every later call fail, so the failure is something
// this side has to go and ask for. Asking on a timer on the GUI thread
// would put both of those stalls in front of the window. Here a slow engine
// delays the next reconnection attempt and nothing else.
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
// counted, and were then thrown away here. All four are per connection and
// restart at zero when the supervisor reconnects, because they describe one
// subscription and the sum above is only true within one.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include <QElapsedTimer>
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

    // EngineInfo, flattened to what a status line shows. Constant for the
    // length of one connection: the engine's geometry is fixed at
    // Engine::create and a change to it is a new engine, which here means a
    // new connection and another connectionChanged.
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY connectionChanged)
    Q_PROPERTY(QString deviceVendor READ deviceVendor NOTIFY connectionChanged)
    Q_PROPERTY(bool deviceDiscrete READ deviceDiscrete NOTIFY connectionChanged)
    Q_PROPERTY(int sourceRate READ sourceRate NOTIFY connectionChanged)
    Q_PROPERTY(int channelRate READ channelRate NOTIFY connectionChanged)
    Q_PROPERTY(int gridChannels READ gridChannels NOTIFY connectionChanged)
    Q_PROPERTY(int bins READ bins NOTIFY connectionChanged)
    Q_PROPERTY(bool spectrumEnabled READ spectrumEnabled NOTIFY connectionChanged)

    // The engine did not build what was asked for, and the sentence saying
    // so. Constant for one connection for the same reason as the fields
    // above it, and false on a connection that never came up.
    //
    // A clamp is silent everywhere else: the engine takes 2048 channels when
    // 4096 were asked for and goes on serving frames that look correct, and
    // the operator finds out when a frequency lands in the wrong channel.
    // core/engine/engine.cpp says exactly that where it sets the field.
    Q_PROPERTY(bool clamped READ clamped NOTIFY connectionChanged)
    Q_PROPERTY(QString clampReason READ clampReason NOTIFY connectionChanged)

    // The span the frames cover, in absolute hertz. Both are
    // frequencyAtFraction at the two ends; see the comment on it for why the
    // arithmetic is shaped the way it is.
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

    // Rows a second actually reaching the items, which is neither the rate
    // the engine offers nor the rate this link receives. Those three differ
    // the moment anything is dropping, and this is the one an operator is
    // looking at the display to judge: it is how fast the picture moves.
    // framesReceived minus framesDroppedByUi is the same quantity totalled
    // rather than per second.
    Q_PROPERTY(double frameRate READ frameRate NOTIFY frameChanged)

public:
    explicit EngineLink(QObject* parent = nullptr);
    ~EngineLink() override;

    EngineLink(const EngineLink&) = delete;
    EngineLink& operator=(const EngineLink&) = delete;
    EngineLink(EngineLink&&) = delete;
    EngineLink& operator=(EngineLink&&) = delete;

    // Starts the supervisor thread and returns immediately. Nothing about
    // this call can fail in a way worth reporting to the caller: an engine
    // that is not running yet is the ordinary case, and the window's job is
    // to come up, say so, and keep trying. What went wrong is in errorText
    // and what state the link is in is in connected.
    //
    // every_nth is passed straight to Client::subscribe_spectrum. The engine
    // drops the rest before copying them, so asking for fewer costs it less.
    void start(const QString& address, std::uint16_t port, std::uint32_t every_nth);

    [[nodiscard]] bool connected() const { return connected_; }
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

    // Absolute hertz at a fraction of the drawn span: 0 is the outer edge of
    // the first bin and 1 the outer edge of the last, which is where a
    // display's left and right edges are.
    //
    // This is the only conversion from the wire's rationals to hertz in the
    // client, and it is at the label, which is what docs/conventions.md asks
    // for. The shape of the arithmetic is the point. bin_width arrives as
    // 2400000/65536 on the shipped geometry, which is 36.62109375 Hz and not
    // a number a float holds; rounding it to the 36.621 Hz the engine prints
    // and then multiplying by 65536 bins puts the top of the span about six
    // hertz from where the engine has it. So the bin index is multiplied
    // into the numerator and the divide happens once, at the end. Every
    // product here is a whole number well inside what a double holds
    // exactly, so the only rounding in the result is the last one.
    //
    // It is Q_INVOKABLE rather than a property because the number wanted
    // depends on where a tick is, and a display decides that. A QML binding
    // calling it cannot see the properties it reads, so a binding that has
    // to follow the connection reads one of the notifying properties itself.
    [[nodiscard]] Q_INVOKABLE double frequencyAtFraction(double fraction) const;

    [[nodiscard]] double spanLowHz() const { return frequencyAtFraction(0.0); }
    [[nodiscard]] double spanHighHz() const { return frequencyAtFraction(1.0); }

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
    [[nodiscard]] double frameRate() const { return frame_rate_; }

    // The frame the items draw. Qt thread only, and valid until the next
    // frameChanged: the buffer it refers to is swapped back into the pool
    // when the next frame is taken. An item that wants to keep a row copies
    // the row, not this.
    [[nodiscard]] const rpc::SpectrumFrame& frame() const { return display_; }

signals:
    // The link came up or went away. An item holding history keyed to one
    // engine's geometry clears it here, on the edge into connected: the next
    // engine may be on another frequency, and a row drawn under the old span
    // would put a signal where it never was.
    void connectionChanged();

    // A new frame is in frame(). Emitted on the Qt thread, so an item may
    // connect to it directly and repaint from the slot.
    void frameChanged();

private:
    // The supervisor thread, and the two halves of what it does.
    void supervise();
    [[nodiscard]] bool attempt_connect();
    void publish(bool connected, QString error);

    // Qt thread, queued from publish(). Takes the connection state the
    // supervisor left and tells QML.
    void adopt();

    // Invoked on the Cap'n Proto event loop thread. Touches nothing but its
    // own buffers, the swap mutex and one atomic.
    void on_frame(const rpc::SpectrumFrame& frame);

    // Qt thread. Takes whatever is waiting and tells the items.
    void drain();

    // Supervisor thread only, other than the read in start() that sets them.
    std::unique_ptr<rpc::Client> client_;
    QString address_;
    std::uint16_t port_ = 0;
    std::uint32_t requested_every_nth_ = 1;

    QString endpoint_;  // set once by start(), read from any thread after

    // Event loop thread only, so the copy out of the callback's argument
    // happens outside the lock and reuses this vector's capacity.
    rpc::SpectrumFrame staging_;

    // Event loop thread only once frames are flowing, and the arithmetic
    // behind the two engine-side counters. The supervisor writes all four
    // before it calls subscribe_spectrum, and that call hands work to the
    // loop thread and waits for it, so the loop thread cannot read a stale
    // one and there is no subscription alive to race with anyway.
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

    // The supervisor's side of the connection state, handed to the Qt thread
    // under this lock and picked up in adopt(). EngineInfo carries strings,
    // so it cannot be an atomic and a property getter must never read the
    // supervisor's copy directly.
    std::mutex state_mutex_;
    bool handover_connected_ = false;  // guarded by state_mutex_
    rpc::EngineInfo handover_info_;    // guarded by state_mutex_
    QString handover_error_;           // guarded by state_mutex_

    std::thread supervisor_;
    std::mutex supervisor_mutex_;
    std::condition_variable supervisor_wake_;
    bool stopping_ = false;  // guarded by supervisor_mutex_

    // Qt thread only.
    rpc::SpectrumFrame display_;
    rpc::EngineInfo info_;
    bool connected_ = false;
    QString error_text_;
    qulonglong frames_received_ = 0;
    qulonglong frames_dropped_engine_ = 0;
    qulonglong frames_dropped_ui_ = 0;
    qulonglong frames_skipped_ = 0;

    // Rows a second, over a window rather than since the connection came
    // up, so a display that stalls now reads as stalled now. frames_drawn_
    // counts drains, which is one repaint each, and is what the rate is
    // taken from; frames_received_ would answer a different question and
    // reads high by exactly the frames this link threw away.
    qulonglong frames_drawn_ = 0;
    QElapsedTimer rate_timer_;
    qulonglong rate_mark_ = 0;
    double frame_rate_ = 0.0;

    // One wake outstanding at a time. Without it a burst of frames posts a
    // metacall each, and the GUI thread then runs a queue of calls that all
    // find the same single frame waiting.
    std::atomic<bool> wake_pending_{false};
};

}  // namespace revenant::ui
