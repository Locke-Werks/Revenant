// The sound card end of the audio path: QAudioSink, the device picker,
// volume and mute, and the words that say what is coming out of it.
//
// It owns no subscription. EngineLink does, because subscribe_audio is a
// blocking RPC call and only the supervisor thread may make one, and
// EngineLink owns the AudioRing for the same reason it owns the spectrum
// hand-off: the RPC callback writes into it and the callback dies with the
// Client, which the supervisor joins.
//
// SO THE SPLIT IS: THIS SAYS WHAT THE CARD IS DOING AND THE LINK SAYS WHAT
// THE WIRE IS DOING. A starve and a wire drop both sound like a click and
// have different fixes, so the two sets of counters are not merged into one
// "dropped" number on screen.
//
// WHY A TIMER AND NOT A SIGNAL FROM THE RING
//
// The ring is written on the Cap'n Proto event loop thread. Reaching a
// QObject from there means a queued metacall, and the whole reason the ring
// exists is that the audio payload must not travel through Qt's event
// queue. A wake-up with no payload would work, on the pattern
// ui/models/engine_link.h uses for spectrum frames, and it buys nothing
// here: nothing on this side has to react to an individual chunk. The sink
// pulls on its own thread whether or not this object is awake, and what
// this object publishes is a status line that a human reads at twenty
// frames a second at most.
//
// So a 50 ms timer on the Qt thread takes ONE snapshot of the ring, which
// carries the format, the generation and the counters together, and the
// one thing it reacts to is the open sink no longer matching the stream: a
// stream that started, or changed rate or channel count. The cost of that
// is up to 50 ms between the first chunk arriving and the sink opening,
// which is inside the ring's own depth and is therefore not a dropout.
//
// One snapshot and not three reads. Two separately locked reads of a ring
// the Cap'n Proto event loop is writing can disagree with each other, and
// the pair that did was format and generation: see AudioRing::Snapshot for
// what that cost.
//
// THE THREE BUFFER DEPTHS AND HOW THEY RELATE
//
// The engine holds up to bufferMillisGranted for this subscription and
// evicts from the front past that. The ring holds the same number, so a
// stall of up to the grant is absorbed once rather than twice: audio older
// than the grant never leaves the engine, so a deeper ring here would
// allocate for frames that cannot arrive.
//
// QAudioSink's own buffer is set to a QUARTER of the grant, floored at the
// engine's own 20 ms minimum. It is the only one of the three that is
// always full, because audio handed to the card cannot be taken back, so it
// is the only one that is always latency. A quarter keeps the card fed
// across a scheduler hiccup and leaves the other three quarters of the
// budget in the two buffers that only fill when something is wrong.
//
// The number is derived from the grant and never from a constant here. The
// engine clamps bufferMillis to 20..5000 and reports what it granted for a
// reason core/rpc/revenant.capnp states: a depth that quietly changed is a
// dropout nobody can trace. Picking the sink's depth off the request rather
// than the grant would reintroduce exactly that.
//
// THE RING FILLS OR EMPTIES SLOWLY AND THAT IS NOT A FAULT
//
// The engine's audio clock and the sound card's are two different clocks
// and nothing here disciplines one to the other. Measured 2026-09-20
// against a synthetic wideband source at pace 1.00, seed 4242, on an
// nfm receiver at 48 kHz: the ring's depth rose from 23 ms to 42 ms over
// 64 seconds, so the engine was about 0.03 percent fast, and the starve
// count grew by 82 frames in that minute against 2891 accumulated in the
// first second while the sink was opening.
//
// Extrapolated, a drift that way fills the ring in about ten minutes and
// then loses about one chunk every nine, which is what AudioRing's front
// eviction and its overrun counter exist for. A drift the other way starves
// at the same rate. Neither is corrected, because correcting it means
// resampling by a fraction of a percent, which is a DSP stage this process
// is not the place for, and the alternative of dropping or repeating whole
// chunks is audible in a way the drift is not. What is provided instead is
// that both counters are on screen, so a display that is quietly losing a
// chunk every few minutes says so.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QList>
#include <QMediaDevices>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtQmlIntegration>

#include "audio/audio_ring.h"
#include "models/engine_link.h"

namespace revenant::ui {

// The QIODevice QAudioSink pulls through. Lives on the Qt thread and is
// READ on the sink's own thread, which is the whole of why it touches
// nothing but the ring and two atomics.
class RingSource final : public QIODevice {
    Q_OBJECT

public:
    // stream is the format the RING produces and out_channels is the
    // channel count the SINK was opened at. They differ in exactly one
    // case, a mono stream on a device that will not take mono, and the
    // duplication that bridges them is in readData. See open_sink.
    RingSource(AudioRing& ring, RingFormat stream, int out_channels,
               QObject* parent = nullptr);

    [[nodiscard]] bool isSequential() const override { return true; }
    [[nodiscard]] qint64 bytesAvailable() const override;

    // What the last frame handed to the card was. Read by the Qt thread on
    // its timer, written by the sink's thread on every pull.
    [[nodiscard]] FrameSource last_source() const {
        return last_source_.load(std::memory_order_relaxed);
    }

    // The format this source was built for, fixed for its life, so no lock
    // and no atomic. It is what AudioPlayer::tick compares the ring against
    // to decide whether the open sink is still the right shape.
    [[nodiscard]] RingFormat stream() const { return stream_; }

    // How many pulls have found the ring's format moved and written silence
    // instead of audio. Written by the sink's thread, read by the Qt thread
    // on its timer.
    //
    // A COUNT AND NOT A FLAG, because the Qt thread needs to know whether
    // the mismatch is HAPPENING and not only whether it happened once. A
    // flag it cleared would race the pull thread setting it again, and a
    // flag it did not clear would latch on the one ordinary case, the 50 ms
    // between a receiver changing rate and the reopen. A count moving
    // between two ticks says the sink is writing silence right now.
    [[nodiscard]] std::uint64_t format_moved_pulls() const {
        return format_moved_pulls_.load(std::memory_order_relaxed);
    }

protected:
    qint64 readData(char* data, qint64 maxlen) override;

    // A sink is output only. Returning -1 rather than 0 so a caller that
    // writes here fails rather than silently discarding.
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    AudioRing& ring_;

    // The format the sink was opened FOR, which is fixed for its life. The
    // ring's current format can differ from it for up to one timer tick
    // after a receiver changes rate, and the byte arithmetic here has to
    // follow the sink or it writes the wrong number of bytes into a buffer
    // the sink sized.
    //
    // Handed to AudioRing::read rather than checked against format() first,
    // so the comparison and the copy happen under one lock. See that
    // declaration: the two-call form was a real race between this thread
    // and the Cap'n Proto event loop, not a theoretical one.
    RingFormat stream_;

    // The sink's channel count, which is stream_.channel_count except on a
    // device that refuses mono.
    int out_channels_ = 1;

    // Read into this and memcpy out, rather than casting the char* the sink
    // hands over. A float write through a reinterpreted char* is only
    // defined when that pointer is suitably aligned and nothing in
    // QIODevice promises it. Sized on first use and reused, so the pull
    // path allocates once.
    std::vector<float> scratch_;

    // The mono frames widened to the sink's channel count, when those two
    // differ. Separate from scratch_ because the ring's read has to see a
    // buffer at the RING's channel count, and written through rather than
    // straight into the sink's char buffer for the alignment reason above.
    std::vector<float> widened_;

    std::atomic<FrameSource> last_source_{FrameSource::idle};
    std::atomic<std::uint64_t> format_moved_pulls_{0};
};

class AudioPlayer : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Constructed by main() alongside the EngineLink it plays.")

    // The device list as a picker shows it. Index 0 is always "system
    // default", which is not a device: it is a promise to follow whatever
    // Windows is currently defaulting to, which is what an operator who
    // switches headsets expects and what a named device cannot do.
    Q_PROPERTY(QStringList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(int device READ device WRITE setDevice NOTIFY deviceChanged)

    // What is actually open, which differs from the selection when the
    // selected device has gone and the player fell back. Empty when nothing
    // is open.
    Q_PROPERTY(QString activeDevice READ activeDevice NOTIFY statusChanged)

    // Zero to one, on a perceptual curve. QAudioSink::setVolume is linear
    // in amplitude, so a slider wired straight to it does almost all of its
    // work in the top fifth of its travel.
    Q_PROPERTY(qreal volume READ volume WRITE setVolume NOTIFY volumeChanged)

    // Mute holds the timeline. The sink keeps pulling and the ring keeps
    // draining at zero gain, so unmuting lands on live audio rather than
    // replaying whatever was buffered when it was muted.
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY volumeChanged)

    // Whether a sink is open and running.
    Q_PROPERTY(bool playing READ playing NOTIFY statusChanged)

    // What the LAST frame handed to the card was, in words: waiting, audio,
    // squelched, gap, starving, format mismatch. Five of those six are
    // silence and they are five different things. The first five come
    // straight off FrameSource in audio/audio_ring.h.
    //
    // THE SIXTH IS THIS OBJECT'S OWN AND IT OUTRANKS THE RING'S REPORT. A
    // pull that finds the ring's format moved writes silence and takes
    // nothing, and the ring counts that as starved because starved is what
    // the card plays. Starving means the audio is LATE, which sends the
    // operator to the network, and nothing is late here: the sink is open
    // at the wrong rate for the stream. See the mismatch branch in tick().
    Q_PROPERTY(QString source READ source NOTIFY statusChanged)

    // The squelch, pulled out of source as its own flag so an indicator can
    // bind to it without parsing a string. It follows the audio reaching
    // the card rather than the newest chunk off the wire.
    Q_PROPERTY(bool squelchOpen READ squelchOpen NOTIFY statusChanged)

    // The device refused the format, or the device went away. Empty when
    // there is nothing wrong. Separate from EngineLink::audioFault, which
    // is the engine refusing a subscription: one is fixed by picking
    // another output and the other is not.
    //
    // WHAT SURVIVES A REOPEN AND WHAT DOES NOT. Three faults are kept
    // behind this one string because they have different lifetimes, and
    // merging them is how the most important one got erased. See
    // device_fault_, sink_fault_ and format_fault_ below.
    Q_PROPERTY(QString fault READ fault NOTIFY statusChanged)

    // Something the player ADAPTED rather than something wrong: today the
    // only one is a mono stream duplicated onto a device that takes no
    // mono. Kept apart from fault for the reason open_sink gives, that a
    // line carrying both trains the operator to ignore it.
    Q_PROPERTY(QString note READ note NOTIFY statusChanged)

    // The three depths from the note at the top of this file, in
    // milliseconds, so the derivation is on screen rather than only in the
    // comment. bufferedMillis is what is in the ring right now.
    Q_PROPERTY(int bufferedMillis READ bufferedMillis NOTIFY statusChanged)
    Q_PROPERTY(int ringMillis READ ringMillis NOTIFY statusChanged)
    Q_PROPERTY(int sinkMillis READ sinkMillis NOTIFY statusChanged)

    // What this side has counted. Split by cause; see RingCounts.
    Q_PROPERTY(qulonglong gapFilledFrames READ gapFilledFrames NOTIFY statusChanged)
    Q_PROPERTY(qulonglong gapWireFrames READ gapWireFrames NOTIFY statusChanged)
    Q_PROPERTY(qulonglong gapUpstreamFrames READ gapUpstreamFrames NOTIFY statusChanged)
    Q_PROPERTY(qulonglong overrunFrames READ overrunFrames NOTIFY statusChanged)
    Q_PROPERTY(qulonglong starvedFrames READ starvedFrames NOTIFY statusChanged)
    Q_PROPERTY(qulonglong gapEvents READ gapEvents NOTIFY statusChanged)
    Q_PROPERTY(qulonglong resyncs READ resyncs NOTIFY statusChanged)

public:
    explicit AudioPlayer(EngineLink& link, QObject* parent = nullptr);
    ~AudioPlayer() override;

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;
    AudioPlayer(AudioPlayer&&) = delete;
    AudioPlayer& operator=(AudioPlayer&&) = delete;

    // Starts the status timer. Call it on the Qt thread before the event
    // loop runs, for the reason EngineLink::start gives: a QTimer started
    // on any other thread never fires.
    void start();

    [[nodiscard]] QStringList devices() const { return device_names_; }
    [[nodiscard]] int device() const { return device_index_; }
    void setDevice(int index);

    [[nodiscard]] QString activeDevice() const { return active_device_; }

    [[nodiscard]] qreal volume() const { return volume_; }
    void setVolume(qreal value);
    [[nodiscard]] bool muted() const { return muted_; }
    void setMuted(bool value);

    [[nodiscard]] bool playing() const { return sink_ != nullptr; }
    [[nodiscard]] QString source() const;
    [[nodiscard]] bool squelchOpen() const;
    [[nodiscard]] QString fault() const;
    [[nodiscard]] QString note() const { return note_; }

    [[nodiscard]] int bufferedMillis() const { return buffered_millis_; }
    [[nodiscard]] int ringMillis() const { return ring_millis_; }
    [[nodiscard]] int sinkMillis() const { return sink_millis_; }

    [[nodiscard]] qulonglong gapFilledFrames() const { return counts_.frames_filled; }
    [[nodiscard]] qulonglong gapWireFrames() const { return counts_.frames_gap_wire; }
    [[nodiscard]] qulonglong gapUpstreamFrames() const {
        return counts_.frames_gap_upstream;
    }
    [[nodiscard]] qulonglong overrunFrames() const { return counts_.frames_overrun; }
    [[nodiscard]] qulonglong starvedFrames() const { return counts_.frames_starved; }
    [[nodiscard]] qulonglong gapEvents() const { return counts_.gap_events; }
    [[nodiscard]] qulonglong resyncs() const { return counts_.resyncs; }

signals:
    void devicesChanged();
    void deviceChanged();
    void volumeChanged();
    void statusChanged();

private:
    // Qt thread. The timer's whole body: reopen if the stream changed
    // shape, close if there is no stream, and republish the status line.
    void tick();

    // Qt thread. Opens a sink on the selected device at the stream's own
    // format, or records why it could not.
    void open_sink(RingFormat format, std::uint64_t generation);
    void close_sink();

    // Qt thread. The selected device, or the system default when the
    // selection is 0 or names a device that has gone.
    [[nodiscard]] QAudioDevice resolve_device(bool& fell_back) const;

    // Qt thread. Rebuilds devices() and keeps the selection pointing at the
    // same device across the change where it can.
    void refresh_devices();

    void handle_sink_state(QAudio::State state);

    // The linear gain the sink is given, from volume_ and muted_.
    [[nodiscard]] qreal sink_gain() const;

    // The ring belongs to EngineLink, because the RPC callback writes into
    // it and that callback dies with the Client the supervisor owns. This
    // object only reads it.
    [[nodiscard]] AudioRing& ring() const { return link_.audioRing(); }

    EngineLink& link_;
    QMediaDevices* devices_ = nullptr;

    std::unique_ptr<QAudioSink> sink_;
    std::unique_ptr<RingSource> pull_;

    // The generation the open sink was built for. A ring generation past
    // this one is a stream that changed rate or channel count under it.
    std::uint64_t open_generation_ = 0;

    // handle_sink_state saw QAudio::StoppedState with an error on it. It
    // cannot close the sink from inside that sink's own signal, so this
    // says tick() has to, and it then latches the reopen off: a device
    // that has just failed fails again, and this branch runs twenty times
    // a second.
    //
    // THREE THINGS CLEAR IT, AND THEY ARE THE THREE DELIBERATE ACTS THAT
    // MAKE ANOTHER ATTEMPT WORTH MAKING. setDevice, including a re-pick of
    // the entry already selected, which is what plugging the same headset
    // back in leaves the operator on. refresh_devices, which is Qt saying
    // the set of outputs moved. And tick() finding the listen switch off,
    // because turning it off and on again is the first thing anyone tries.
    //
    // The first and the third were missing until 2026-09-20 and the fault
    // message asked for the first of them in as many words. With both
    // absent there was no way back to audio short of restarting the window.
    bool sink_failed_ = false;

    QTimer tick_;

    QStringList device_names_;
    QList<QAudioDevice> device_handles_;
    int device_index_ = 0;

    // Remembered by id rather than by index, because audioOutputsChanged
    // reorders the list and an index would follow whichever device took the
    // slot.
    QByteArray wanted_device_id_;

    QString active_device_;

    // THE TWO FAULTS, AND THE ONE THAT USED TO BE ERASED
    //
    // A fault about the SELECTION: the operator's chosen output has gone
    // and the player is on the system default instead. Reopening a sink
    // does not answer it, because the sink that opens is on the fallback
    // device and the operator still needs to know their pick is not what
    // is playing. It is written by refresh_devices and by open_sink's own
    // fall-back branch, and it is cleared by exactly two things: the
    // operator picking a device, and the device coming back.
    //
    // open_sink used to clear one shared fault_ unconditionally on every
    // successful open. refresh_devices writes this sentence and closes the
    // sink, tick() reopens on the fallback a few tens of milliseconds
    // later, and the clear wiped the only notice the operator ever got.
    QString device_fault_;

    // A fault about the SINK: the last open attempt was refused, or the
    // running sink stopped with an error. This one IS the reopen's own
    // verdict, so it is cleared at the top of every open attempt and
    // rewritten if that attempt fails.
    QString sink_fault_;

    // A fault about the SHAPE: the open sink's format is not the ring's,
    // so every pull is writing silence, and it has been that way for long
    // enough that the reopen which should have ended it plainly is not
    // coming. Held apart from sink_fault_ because open_sink clears that one
    // at the top of every attempt, and this condition is the attempt not
    // being made.
    //
    // Written and cleared only by tick(), which is the one place that can
    // see both formats at once. Empty in the ordinary case, including the
    // one tick of mismatch a receiver changing rate costs.
    QString format_fault_;

    QString note_;
    qreal volume_ = 0.7;
    bool muted_ = false;

    int buffered_millis_ = 0;
    int ring_millis_ = 0;
    int sink_millis_ = 0;
    RingCounts counts_;
    FrameSource shown_source_ = FrameSource::idle;

    // The mismatch, as tick() tracks it across passes. moved_pulls_ is the
    // pull thread's count as of the last pass, so a difference means the
    // sink wrote silence during it; shown_mismatch_ is what source()
    // reports and, read at the top of the next pass, is also what says the
    // mismatch has now lasted two passes and is a fault rather than a
    // reopen in progress. Both are reset by close_sink, because a new pull
    // starts its count at zero.
    //
    // There was an int mismatch_ticks_ here counting consecutive passes
    // towards a threshold of ten. It could never pass one: close_sink()
    // zeroed it, open_sink() begins with close_sink(), and the reopen runs
    // on the same condition that produces the mismatch. See the block in
    // tick() for why the replacement is a transition and not a bigger
    // threshold.
    std::uint64_t moved_pulls_ = 0;
    bool shown_mismatch_ = false;
};

}  // namespace revenant::ui
