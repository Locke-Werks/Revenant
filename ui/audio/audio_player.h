// The sound card end of the audio path: QAudioSink, the device picker,
// volume and mute, and the words that say what is coming out of it.
//
// It owns no subscription. EngineLink does, because subscribe_audio is a
// blocking RPC call and only the supervisor thread may make one, and
// EngineLink owns the AudioRings, one per rack slot, for the same reason it
// owns the spectrum hand-off: the RPC callback writes into them and the
// callback dies with the Client, which the supervisor joins.
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
// So a 50 ms timer on the Qt thread takes ONE snapshot of the lead ring,
// which carries the format and the counters together, and the one thing it
// reacts to is a stream starting or stopping. The cost of that is up to
// 50 ms between the first chunk arriving and the sink opening, which is
// inside the ring's own depth and is therefore not a dropout.
//
// WHAT THIS PARAGRAPH USED TO SAY: that the timer reacts to "the open sink
// no longer matching the stream: a stream that started, or changed rate or
// channel count". The sink is opened at the DEVICE's format since
// 2026-09-23 and every stream is resampled to it, so a stream changing shape
// is AudioMix's business on the pull thread and never a reopen. The snapshot
// is still one call rather than several, for the reason AudioRing::Snapshot
// gives.
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
// THE TWO CLOCKS, AND THE LOOP THAT HOLDS THE RING BETWEEN THEM
//
// The engine's audio clock and the sound card's are two different clocks.
// Measured 2026-09-20 against a synthetic wideband source at pace 1.00, seed
// 4242, on an nfm receiver at 48 kHz: the ring's depth rose from 23 ms to
// 42 ms over 64 seconds, so the engine was about 0.03 percent fast, and the
// starve count grew by 82 frames in that minute against 2891 accumulated in
// the first second while the sink was opening. Left alone, a drift that way
// fills the ring in about ten minutes and then loses about one chunk every
// nine; a drift the other way starves at the same rate.
//
// Since 2026-09-23 AudioMix reads every stream a few hundred ppm at most off
// its nominal step, chosen by DriftTrim's loop on the lead's lag behind its
// arrivals, which holds the ring where it settled; audio/drift_trim.h has
// the loop and what it measured. RingSource hands the mix the pull's
// steady-clock reading for that, the same clock EngineLink stamps each chunk's
// arrival with. The trim is on screen in ppm beside the depths, and the
// overrun and starve counters stay, for a drift past the loop's 500 ppm
// clamp and for everything that is not drift. When the lead's ring does
// evict, AudioMix moves every stream up past the hole together, so the
// receivers stay aligned across it.
//
// WHAT THIS PARAGRAPH USED TO SAY, under the heading THE RING FILLS OR
// EMPTIES SLOWLY AND THAT IS NOT A FAULT: "nothing here disciplines one to
// the other", and "Neither is corrected. Correcting it means resampling by a
// fraction of a percent under a loop that measures the drift", followed by a
// retraction ending "what it still does not have is the measurement loop a
// drift correction would need." That loop is DriftTrim.

#pragma once

#include <array>
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

#include "audio/audio_mix.h"
#include "audio/audio_ring.h"
#include "models/engine_link.h"

namespace revenant::ui {

// The QIODevice QAudioSink pulls through. Lives on the Qt thread and is
// READ on the sink's own thread, which is the whole of why it touches
// nothing but the rings, its own mix and a handful of atomics.
//
// THE MIX. Every slot in EngineLink::mixMask holds a subscription on a
// heard receiver, each in its own ring. A pull hands every one of those
// rings to an AudioMix made at the sink's format, which resamples each to
// it, aligns them on the lead's instant, levels the amplitude-detected
// modes, scales each by its strip's gain, sums and limits. audio/audio_mix.h
// has how, and what the mix before it decided instead.
//
// WHAT THIS BLOCK USED TO SAY, under "WHAT WAS CHOSEN, BECAUSE THE CODE
// COULD NOT SETTLE IT": "a ring at another rate or channel count than the
// lead's is left out of the mix rather than resampled, because this process
// holds no DSP", "Alignment: none. Each ring plays from its own head", and
// "Level: a plain sum, so eight loud receivers can exceed full scale". The
// owner approved the best-effort mix that replaced all three on 2026-09-23.
class RingSource final : public QIODevice {
    Q_OBJECT

public:
    // The sink's own format, fixed for the life of this source: its rate,
    // its channel count and the sample format it takes. Float unless the
    // device takes none, in which case the mix is converted on the way out;
    // the limiter holds it under full scale, so the conversion clips nothing.
    RingSource(EngineLink& link, std::uint32_t out_rate, int out_channels,
               QAudioFormat::SampleFormat sample_format, QObject* parent = nullptr);

    [[nodiscard]] bool isSequential() const override { return true; }
    [[nodiscard]] qint64 bytesAvailable() const override;

    // What the last frame handed to the card was. Read by the Qt thread on
    // its timer, written by the sink's thread on every pull.
    [[nodiscard]] FrameSource last_source() const {
        return last_source_.load(std::memory_order_relaxed);
    }

    // How many pulls the limiter turned something down in, since this
    // source was made. Written by the sink's thread, read by the Qt thread.
    [[nodiscard]] std::uint64_t limited_pulls() const {
        return limited_pulls_.load(std::memory_order_relaxed);
    }

    // Times the mix moved a stream to the lead's instant. See
    // AudioMix::alignments.
    [[nodiscard]] std::uint64_t alignments() const {
        return alignments_.load(std::memory_order_relaxed);
    }

    // The mix's clock trim after the last pull, in ppm. See
    // AudioMix::drift.
    [[nodiscard]] double drift_trim_ppm() const {
        return drift_trim_ppm_.load(std::memory_order_relaxed);
    }

protected:
    qint64 readData(char* data, qint64 maxlen) override;

    // A sink is output only. Returning -1 rather than 0 so a caller that
    // writes here fails rather than silently discarding.
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    EngineLink& link_;
    AudioMix mix_;
    QAudioFormat::SampleFormat sample_format_;
    int bytes_per_sample_ = 4;

    // The mix is written into this and converted or copied out, rather than
    // written through the char* the sink hands over. A float write through a
    // reinterpreted char* is only defined when that pointer is suitably
    // aligned and nothing in QIODevice promises it. Sized on first use and
    // reused, so the pull path allocates once.
    std::vector<float> mixed_;

    std::array<MixSlot, kMaxReceivers> slots_{};
    std::array<AudioRing*, kMaxReceivers> rings_{};

    std::atomic<FrameSource> last_source_{FrameSource::idle};
    std::atomic<std::uint64_t> limited_pulls_{0};
    std::atomic<std::uint64_t> alignments_{0};
    std::atomic<double> drift_trim_ppm_{0.0};
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
    // squelched, gap, starving. Four of those five are silence and they are
    // four different things. They come straight off FrameSource in
    // audio/audio_ring.h, for the lead stream.
    //
    // WHAT THIS PARAGRAPH USED TO SAY: that there was a sixth, "format
    // mismatch", this object's own, for a sink open at another rate than the
    // stream. The sink is open at the device's format and every stream is
    // resampled to it since 2026-09-23, so there is no such state.
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
    // WHAT SURVIVES A REOPEN AND WHAT DOES NOT. Two faults are kept behind
    // this one string because they have different lifetimes, and merging
    // them is how the more important one got erased. See device_fault_ and
    // sink_fault_ below. There were three until 2026-09-23; the third,
    // format_fault_, described a sink open at another rate than the stream,
    // which cannot happen now.
    Q_PROPERTY(QString fault READ fault NOTIFY statusChanged)

    // Something the player ADAPTED rather than something wrong: the focused
    // receiver's stream resampled to the device's rate, or a mono stream
    // copied to every channel of the device. Kept apart from fault for the
    // reason open_sink gives, that a line carrying both trains the operator
    // to ignore it.
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

    // What the mix has done to hold the receivers together, which the
    // counters above, the lead ring's alone, do not show. The trim is
    // DriftTrim's, in ppm, zero with no sink open. Late audio skipped is
    // RingCounts::frames_skipped across every heard receiver, in
    // milliseconds so receivers at different rates add up. Realignments are
    // AudioMix::alignments since the sink opened. models/audio_counters.h
    // decides how each is shown.
    Q_PROPERTY(double driftTrimPpm READ driftTrimPpm NOTIFY statusChanged)
    Q_PROPERTY(double lateSkippedMillis READ lateSkippedMillis NOTIFY statusChanged)
    Q_PROPERTY(qulonglong realignments READ realignments NOTIFY statusChanged)

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

    [[nodiscard]] double driftTrimPpm() const { return drift_trim_ppm_; }
    [[nodiscard]] double lateSkippedMillis() const { return late_skipped_millis_; }
    [[nodiscard]] qulonglong realignments() const { return realignments_; }

signals:
    void devicesChanged();
    void deviceChanged();
    void volumeChanged();
    void statusChanged();

private:
    // Qt thread. The timer's whole body: reopen if the stream changed
    // shape, close if there is no stream, and republish the status line.
    void tick();

    // Qt thread. Opens a sink on the selected device at the DEVICE's own
    // rate and channel count, or records why it could not.
    // WHAT THIS USED TO SAY: "at the stream's own format". See open_sink for
    // what that cost.
    void open_sink();
    void close_sink();

    // Qt thread. What the player adapted, for note(): the lead stream's rate
    // against the sink's, and a mono stream on a device of several channels.
    [[nodiscard]] QString describe_adaptation(RingFormat lead) const;

    // Qt thread. The selected device, or the system default when the
    // selection is 0 or names a device that has gone.
    [[nodiscard]] QAudioDevice resolve_device(bool& fell_back) const;

    // Qt thread. Rebuilds devices() and keeps the selection pointing at the
    // same device across the change where it can.
    void refresh_devices();

    void handle_sink_state(QAudio::State state);

    // The linear gain the sink is given, from volume_ and muted_.
    [[nodiscard]] qreal sink_gain() const;

    EngineLink& link_;
    QMediaDevices* devices_ = nullptr;

    std::unique_ptr<QAudioSink> sink_;
    std::unique_ptr<RingSource> pull_;

    // The format the open sink runs at, the device's. Zero when closed.
    //
    // WHAT WAS HERE: open_generation_ and open_lead_, the lead ring's format
    // generation the sink was opened for, so tick() could reopen when a
    // stream changed rate or channel count under it. A stream's shape no
    // longer decides the sink's, so nothing reopens on it.
    std::uint32_t sink_rate_ = 0;
    int sink_channels_ = 0;

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
    //
    // THE THIRD ONE READ THE WRONG PROPERTY FOR THE REST OF THAT DAY. The
    // branch tested EngineLink::audioActive, which is whether a STREAM
    // exists, while this paragraph said the listen switch. Those part
    // company on every receiver clear, every mode change that rebuilds the
    // receiver and every reconnect, because EngineLink states the switch is
    // sticky across all three, so the latch was being cleared by events the
    // operator did not perform and the retry loop came back. tick() now
    // reads audioWanted for the clear and audioActive for the sink, and the
    // two are named apart there.
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

    // WHAT WAS HERE: format_fault_, "a fault about the SHAPE: the open
    // sink's format is not the ring's, so every pull is writing silence".
    // Gone with the state it described; see open_generation_ above.

    QString note_;
    qreal volume_ = 0.7;
    bool muted_ = false;

    int buffered_millis_ = 0;
    int ring_millis_ = 0;
    int sink_millis_ = 0;
    RingCounts counts_;
    FrameSource shown_source_ = FrameSource::idle;

    double drift_trim_ppm_ = 0.0;
    double late_skipped_millis_ = 0.0;
    std::uint64_t realignments_ = 0;

    // WHAT WAS HERE: moved_pulls_ and shown_mismatch_, which tracked a sink
    // writing silence because it was open at another rate than the stream,
    // and before them a mismatch_ticks_ counter that could never pass one.
    // All three went with the reopen they watched.
};

}  // namespace revenant::ui
