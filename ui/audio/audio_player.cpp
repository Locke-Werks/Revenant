#include "audio/audio_player.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <QMediaDevices>
#include <QSettings>
#include <QVariant>

#include "models/audio_counters.h"
#include "models/settings.h"

namespace revenant::ui {
namespace {

// How often the Qt thread looks at the ring. See WHY A TIMER at the top of
// audio/audio_player.h: nothing here has to react to an individual chunk,
// and the one event it does react to, a stream changing shape, is absorbed
// by the ring for far longer than this.
constexpr int kStatusTickMs = 50;

// The sink's buffer as a fraction of the granted depth, and the floor under
// it. Twenty milliseconds is the engine's own minimum grant, so a sink
// buffer smaller than that would be shorter than the shortest depth the
// wire can be asked for.
constexpr int kSinkDepthDivisor = 4;
constexpr int kMinSinkMillis = 20;

[[nodiscard]] int millis_for(std::size_t frames, std::uint32_t rate)
{
    if (rate == 0) {
        return 0;
    }
    return static_cast<int>((static_cast<std::uint64_t>(frames) * 1000U) / rate);
}

}  // namespace

// ---------------------------------------------------------------------------
// RingSource
// ---------------------------------------------------------------------------

RingSource::RingSource(EngineLink& link, std::uint32_t out_rate, int out_channels,
                       QAudioFormat::SampleFormat sample_format, QObject* parent)
    : QIODevice(parent), link_(link), mix_(out_rate, out_channels), sample_format_(sample_format)
{
    // No default label: a sample format Qt adds later is refused by
    // open_sink before a source is made for it, and C4062 names the switch
    // here that would need it. See cmake/CompilerFlags.cmake.
    switch (sample_format_) {
        case QAudioFormat::UInt8: bytes_per_sample_ = 1; break;
        case QAudioFormat::Int16: bytes_per_sample_ = 2; break;
        case QAudioFormat::Int32: bytes_per_sample_ = 4; break;
        case QAudioFormat::Float: bytes_per_sample_ = 4; break;
        case QAudioFormat::Unknown:
        case QAudioFormat::NSampleFormats: bytes_per_sample_ = 4; break;
    }
    for (std::size_t slot = 0; slot < kMaxReceivers; ++slot) {
        rings_[slot] = &link_.audioRingAt(slot);
    }
}

qint64 RingSource::bytesAvailable() const
{
    // The sink's whole buffer, always, because readData always fills the
    // whole request. A device that reports what it actually holds would
    // report zero the instant the network hiccupped, and QAudioSink reads a
    // zero-length answer as the end of the stream: it goes Idle and stops.
    // A dropout would then end the audio rather than dip it, which is the
    // one failure mode the starve fill exists to avoid.
    return static_cast<qint64>(mix_.out_channels()) * bytes_per_sample_ *
           static_cast<qint64>(mix_.out_rate());
}

qint64 RingSource::readData(char* data, qint64 maxlen)
{
    const qint64 frame_bytes =
        static_cast<qint64>(mix_.out_channels()) * static_cast<qint64>(bytes_per_sample_);
    if (data == nullptr || frame_bytes <= 0 || maxlen < frame_bytes) {
        return 0;
    }

    const auto frames = static_cast<std::size_t>(maxlen / frame_bytes);
    const std::size_t floats = frames * static_cast<std::size_t>(mix_.out_channels());
    if (mixed_.size() < floats) {
        mixed_.resize(floats);
    }

    // What the rack says about each slot this pull. Read once, here, so a
    // strip moved mid-pull changes the next pull and not half of this one.
    const std::uint32_t mask = link_.mixMask();
    const std::uint32_t wfm = link_.mixWfmMask();
    for (std::size_t slot = 0; slot < kMaxReceivers; ++slot) {
        const std::uint32_t bit = 1U << slot;
        slots_[slot] = MixSlot{.heard = (mask & bit) != 0,
                               .gain = link_.mixGain(slot),
                               .wfm = (wfm & bit) != 0};
    }
    const MixControl control{link_.mixLeadSlot(), slots_};

    // The steady clock, because it is the one EngineLink stamps each chunk's
    // arrival with; the drift loop reads the lead's lag as this less the
    // lead's arrival anchor. See MixPull::lag_seconds.
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
    const MixPull pulled = mix_.pull(rings_, control, mixed_.data(), frames,
                                     static_cast<std::int64_t>(now.count()));

    last_source_.store(pulled.lead < 0 ? FrameSource::idle : pulled.lead_source,
                       std::memory_order_relaxed);
    if (pulled.limited) {
        limited_pulls_.fetch_add(1, std::memory_order_relaxed);
    }
    alignments_.store(mix_.alignments(), std::memory_order_relaxed);
    drift_trim_ppm_.store(mix_.drift().trim() * 1e6, std::memory_order_relaxed);

    // Out in the sink's own sample format. The limiter holds the mix under
    // -1 dBFS, so the integer conversions below clip nothing.
    switch (sample_format_) {
        case QAudioFormat::Float:
        case QAudioFormat::Unknown:
        case QAudioFormat::NSampleFormats:
            std::memcpy(data, mixed_.data(), floats * sizeof(float));
            break;
        case QAudioFormat::Int32:
            for (std::size_t i = 0; i < floats; ++i) {
                const auto value = static_cast<std::int32_t>(
                    std::lround(std::clamp(static_cast<double>(mixed_[i]), -1.0, 1.0) *
                                2147483647.0));
                std::memcpy(data + i * 4, &value, 4);
            }
            break;
        case QAudioFormat::Int16:
            for (std::size_t i = 0; i < floats; ++i) {
                const auto value = static_cast<std::int16_t>(
                    std::lround(std::clamp(mixed_[i], -1.0F, 1.0F) * 32767.0F));
                std::memcpy(data + i * 2, &value, 2);
            }
            break;
        case QAudioFormat::UInt8:
            for (std::size_t i = 0; i < floats; ++i) {
                data[i] = static_cast<char>(static_cast<std::uint8_t>(
                    std::lround(std::clamp(mixed_[i], -1.0F, 1.0F) * 127.0F + 128.0F)));
            }
            break;
    }
    return static_cast<qint64>(frames) * frame_bytes;
}

// ---------------------------------------------------------------------------
// AudioPlayer
// ---------------------------------------------------------------------------

AudioPlayer::AudioPlayer(EngineLink& link, QObject* parent) : QObject(parent), link_(link)
{
    // WHAT THE OPERATOR CHOSE LAST TIME, READ BEFORE THE DEVICE LIST IS
    // BUILT. refresh_devices matches wanted_device_id_ against the
    // enumeration and sets device_index_ from it, so the id has to be in
    // hand first or the first pass resolves to the system default and the
    // remembered choice is lost before anything can apply it.
    //
    // BY ID AND NOT BY INDEX. QMediaDevices reorders its list when a
    // device appears or goes, so an index remembered overnight names
    // whichever output happened to take the slot. An id that no longer
    // matches anything resolves to the system default and says so through
    // device_fault_, which is the same path a headset being unplugged
    // takes and wants the same sentence.
    const QSettings store;
    wanted_device_id_ =
        store.value(settings::kAudioDeviceId).toByteArray();
    volume_ = std::clamp(store.value(settings::kAudioVolume, volume_).toDouble(),
                         qreal{0.0}, qreal{1.0});
    muted_ = store.value(settings::kAudioMuted, false).toBool();

    refresh_devices();

    // The list moves under us when a headset is plugged in or pulled out,
    // and the second of those is one of the shapes this object has to
    // survive. Connected here rather than polled, because Qt already knows.
    // The QMediaDevices instance is parented to this, so it goes when this
    // does and the connection cannot outlive either end.
    devices_ = new QMediaDevices(this);
    connect(devices_, &QMediaDevices::audioOutputsChanged, this,
            [this] { refresh_devices(); });
}

AudioPlayer::~AudioPlayer()
{
    // Before the ring goes, and the ring belongs to EngineLink, which is
    // destroyed after this object in main(). Stopping the sink is what
    // joins its pull thread, so nothing is reading the ring afterwards.
    close_sink();
}

void AudioPlayer::start()
{
    tick_.setInterval(kStatusTickMs);
    connect(&tick_, &QTimer::timeout, this, &AudioPlayer::tick);
    tick_.start();
}

void AudioPlayer::refresh_devices()
{
    const QByteArray was_wanted = wanted_device_id_;

    // The other thing that lets tick() try again after a sink stopped with
    // an error, and the one the message means by "plug it back in": the
    // set of outputs has moved, so the previous failure says nothing about
    // what an attempt would do now.
    sink_failed_ = false;

    device_handles_ = QMediaDevices::audioOutputs();
    device_names_.clear();
    device_names_.append(QStringLiteral("system default"));
    for (const QAudioDevice& one : device_handles_) {
        device_names_.append(one.description());
    }

    // The selection follows the DEVICE and not the slot. audioOutputsChanged
    // reorders the list, so an index held across it points at whichever
    // device took that position, which is how an operator ends up listening
    // on the machine's speakers after unplugging something else.
    if (!was_wanted.isEmpty()) {
        int found = 0;
        for (int i = 0; i < device_handles_.size(); ++i) {
            if (device_handles_.at(i).id() == was_wanted) {
                found = i + 1;
                break;
            }
        }
        if (found != device_index_) {
            device_index_ = found;
            emit deviceChanged();
        }
        if (found == 0) {
            // The device the operator chose has gone. Said in words rather
            // than silently falling back, because falling back silently is
            // how audio ends up coming out of a laptop speaker in a room
            // where that matters.
            //
            // In device_fault_ and not in sink_fault_, which is what makes
            // it survive the reopen that follows a few tens of
            // milliseconds later. See the two-fault note in the header.
            device_fault_ = QStringLiteral(
                "the output device that was selected has gone; playing on the system "
                "default");
            close_sink();
        } else {
            // It is back, and the selection points at it again, so the
            // sentence above has been answered. This is one of the two
            // things that clears it; setDevice is the other.
            device_fault_.clear();
        }
        emit statusChanged();
    }

    emit devicesChanged();
}

void AudioPlayer::setDevice(int index)
{
    if (index < 0 || index > device_handles_.size()) {
        return;
    }

    // PICKING THE DEVICE ALREADY SELECTED IS NOT A NO-OP AFTER IT HAS
    // FAILED, WHICH IS EXACTLY WHAT THE MESSAGE ASKS FOR.
    //
    // This returned on index == device_index_ and nothing else cleared
    // sink_failed_, so "plug it back in and pick it again" did not work when
    // the operator picked the same entry they were already on, which is what
    // plugging the same headset back in leaves them on. The combo reported
    // the right device, the latch stayed set, tick() refused to reopen, and
    // there was no way back to audio short of restarting the window.
    //
    // refresh_devices clears the latch too, and covers the case where Qt
    // notices the device list move. It does not cover a device that stopped
    // with an error while still enumerated, which is the IOError a stalled
    // endpoint raises.
    const bool moved = index != device_index_;
    if (!moved && !sink_failed_) {
        return;
    }

    device_index_ = index;
    wanted_device_id_ =
        index == 0 ? QByteArray{} : device_handles_.at(index - 1).id();

    // The id and not the index, for the reason the constructor gives. An
    // empty one means the system default, which is a promise to follow
    // whatever Windows is defaulting to rather than a device at all, and
    // that promise is worth remembering as much as a named output is.
    QSettings().setValue(settings::kAudioDeviceId, wanted_device_id_);

    // A device change is a new sink. The ring is not touched, so the audio
    // buffered for the old device is played out of the new one rather than
    // thrown away.
    //
    // Both faults go. The operator has just answered the question either
    // of them was asking, which is which output to use, and holding a
    // sentence about a device they have moved off is holding a stale one.
    device_fault_.clear();
    sink_fault_.clear();

    // One of the two things that lets tick() try again after a sink
    // stopped with an error. See that branch.
    sink_failed_ = false;
    close_sink();
    if (moved) {
        emit deviceChanged();
    }
    emit statusChanged();
}

QAudioDevice AudioPlayer::resolve_device(bool& fell_back) const
{
    fell_back = false;
    if (device_index_ == 0 || device_index_ > device_handles_.size()) {
        return QMediaDevices::defaultAudioOutput();
    }
    const QAudioDevice chosen = device_handles_.at(device_index_ - 1);
    if (chosen.isNull()) {
        fell_back = true;
        return QMediaDevices::defaultAudioOutput();
    }
    return chosen;
}

void AudioPlayer::setVolume(qreal value)
{
    const qreal clamped = std::clamp(value, qreal{0.0}, qreal{1.0});
    if (qFuzzyCompare(clamped + 1.0, volume_ + 1.0)) {
        return;
    }
    volume_ = clamped;
    if (sink_ != nullptr) {
        sink_->setVolume(sink_gain());
    }

    // WRITTEN ON EVERY MOVE AND NOT ON EXIT. A window that saves at
    // shutdown loses everything when it is killed, and the setting an
    // operator most wants kept is the one they changed just before
    // something went wrong. The geometry in ui/main.cpp goes the other
    // way and says why.
    //
    // THIS REACHES THE REGISTRY IMMEDIATELY. QSettings on Windows is
    // RegSetValueEx per setValue, not a batch flushed at sync, so a
    // temporary here is a registry write and not a buffered one. What
    // bounds it is the slider: ui/qml/AudioPane.qml gives the volume control
    // a step, so a full-travel drag is at most a hundred distinct values
    // and the fuzzy compare above drops the rest. Without that step a
    // drag would be one write per frame of pointer motion.
    QSettings().setValue(settings::kAudioVolume, volume_);

    emit volumeChanged();
}

void AudioPlayer::setMuted(bool value)
{
    if (muted_ == value) {
        return;
    }
    muted_ = value;

    // The sink keeps running and the ring keeps draining. Stopping it
    // instead would leave the buffered audio in place, and unmuting would
    // then play whatever was live at the moment of the mute, seconds late.
    if (sink_ != nullptr) {
        sink_->setVolume(sink_gain());
    }
    QSettings().setValue(settings::kAudioMuted, muted_);
    emit volumeChanged();
}

qreal AudioPlayer::sink_gain() const
{
    if (muted_) {
        return 0.0;
    }

    // QAudioSink::setVolume is linear in amplitude, so a slider wired
    // straight to it does almost all of its audible work in the top fifth
    // of its travel. Qt's own conversion is what the rest of the platform
    // uses for the same control.
    return static_cast<qreal>(QtAudio::convertVolume(static_cast<float>(volume_),
                                                     QtAudio::LogarithmicVolumeScale,
                                                     QtAudio::LinearVolumeScale));
}

void AudioPlayer::open_sink()
{
    close_sink();

    // This attempt's own verdict, cleared before the attempt so whichever
    // way it goes only its verdict is left behind. device_fault_ is NOT
    // touched: it describes the selection, which this attempt does not
    // change. See the two-fault note in the header.
    sink_fault_.clear();

    bool fell_back = false;
    const QAudioDevice out = resolve_device(fell_back);
    if (out.isNull()) {
        sink_fault_ = QStringLiteral("this machine has no audio output device");
        return;
    }

    // AT THE DEVICE'S OWN RATE AND CHANNEL COUNT, which is the change that
    // ended the RDS dropout. This asked the device for the focused
    // receiver's rate until 2026-09-23. RDS raises a wfm receiver to the
    // 171000 S/s composite, the owner's device refused 32-bit float there,
    // and the audio stopped with this function's own sentence about it. A
    // shared-mode Windows output runs at its mix rate and nothing else, so
    // the sink is opened at what the device prefers and AudioMix resamples
    // every stream to it.
    //
    // Float when the device takes it at that rate, because the mix is float
    // and the limiter's headroom is below full scale either way; otherwise
    // the device's own sample format, which RingSource converts to.
    const QAudioFormat preferred = out.preferredFormat();
    QAudioFormat wanted;
    wanted.setSampleRate(preferred.sampleRate() > 0 ? preferred.sampleRate() : 48'000);
    wanted.setChannelCount(preferred.channelCount() > 0 ? preferred.channelCount() : 2);
    wanted.setSampleFormat(QAudioFormat::Float);
    if (!out.isFormatSupported(wanted)) {
        const QAudioFormat::SampleFormat own = preferred.sampleFormat();
        const bool writable = own == QAudioFormat::Int16 || own == QAudioFormat::Int32 ||
                              own == QAudioFormat::UInt8;
        QAudioFormat fallback = wanted;
        fallback.setSampleFormat(own);
        if (!writable || !out.isFormatSupported(fallback)) {
            sink_fault_ = QStringLiteral(
                              "%1 will not take its own preferred rate, %2 Hz on %3 "
                              "channel(s), in 32-bit float or in a sample format this player "
                              "writes. Pick another output.")
                              .arg(out.description())
                              .arg(wanted.sampleRate())
                              .arg(wanted.channelCount());
            return;
        }
        wanted = fallback;
    }

    // WHAT THIS FUNCTION DID UNTIL 2026-09-23, from here to the sink: it
    // asked for the stream's rate and channel count in float, widened a mono
    // stream by duplication onto a device that refused mono, and otherwise
    // refused with "%1 will not take 32-bit float at %2 Hz on %3
    // channel(s). It offers %4 Hz on %5. Pick another output." That sentence
    // is what the owner saw when RDS raised the receiver to 171000 S/s. The
    // duplication survives in AudioMix, which puts a mono stream on every
    // channel of the device, and the refusal survives only for a device that
    // will not take its own preferred rate.
    const int out_channels = wanted.channelCount();
    const auto out_rate = static_cast<std::uint32_t>(wanted.sampleRate());

    sink_ = std::make_unique<QAudioSink>(out, wanted);

    // The sink's depth, derived from the grant. See THE THREE BUFFER DEPTHS
    // at the top of audio/audio_player.h for why it is a quarter and why it
    // comes off the grant rather than off the request.
    const int granted = static_cast<int>(link_.mixGrantedMillis());
    sink_millis_ = std::max(kMinSinkMillis, granted / kSinkDepthDivisor);
    const auto sink_frames = static_cast<qsizetype>(
        (static_cast<std::uint64_t>(sink_millis_) * out_rate) / 1000U);
    sink_->setBufferSize(sink_frames * out_channels * wanted.bytesPerSample());
    sink_->setVolume(sink_gain());

    connect(sink_.get(), &QAudioSink::stateChanged, this,
            [this](QAudio::State state) { handle_sink_state(state); });

    pull_ = std::make_unique<RingSource>(link_, out_rate, out_channels, wanted.sampleFormat());
    pull_->open(QIODevice::ReadOnly);
    sink_->start(pull_.get());

    sink_rate_ = out_rate;
    sink_channels_ = out_channels;
    active_device_ = out.description();

    if (fell_back) {
        // resolve_device could not use the selection, so this sink is on
        // the fallback. Same sentence refresh_devices writes and the same
        // field, so the two paths cannot overwrite each other with
        // different words for the same condition.
        device_fault_ = QStringLiteral(
            "the output device that was selected has gone; playing on the system default");
    }
}

QString AudioPlayer::fault() const
{
    // All of them, when more than one holds. They are separate conditions
    // and an operator whose chosen headset has gone AND whose fallback
    // refuses the format needs to read both sentences to know what to do.
    QStringList parts;
    for (const QString& one : {device_fault_, sink_fault_}) {
        if (!one.isEmpty()) {
            parts.append(one);
        }
    }
    return parts.join(QStringLiteral("  "));
}

void AudioPlayer::close_sink()
{
    if (sink_ != nullptr) {
        // stop() before the QIODevice goes, because the sink's own thread
        // is reading through it and stop() is what joins that thread.
        sink_->stop();
        sink_.reset();
    }
    pull_.reset();
    sink_rate_ = 0;
    sink_channels_ = 0;
    active_device_.clear();
    sink_millis_ = 0;

    // The note describes the sink that is going, so it goes with it.
    // Neither fault does: close_sink is on the path a refused format takes
    // and on the path a vanished device takes, and clearing here would
    // erase the sentence that said why in both cases.
    note_.clear();
}

void AudioPlayer::handle_sink_state(QAudio::State state)
{
    // No default label, so a new QtAudio::State raises C4062 here. See
    // cmake/CompilerFlags.cmake.
    switch (state) {
        case QAudio::ActiveState:
        case QAudio::SuspendedState:
            break;
        case QAudio::IdleState:
            // The sink has nothing to play at this instant. This used to
            // say it was "reachable only with no stream at all", which
            // overstated it: a starving network does not get here, because
            // RingSource fills every request at least one frame wide, but
            // the backend reports Idle in the ordinary course between
            // start() and the first pull, and readData answers a request
            // narrower than one frame with zero bytes, which is the same
            // report.
            //
            // None of those want action. tick() closes the sink when the
            // stream is gone, and the other two are answered by the next
            // pull.
            break;
        case QAudio::StoppedState: {
            const QtAudio::Error why = sink_ == nullptr ? QtAudio::NoError : sink_->error();
            if (why == QtAudio::NoError) {
                break;
            }
            // A USB headset pulled out of the socket arrives here, as
            // IOError or FatalError depending on how far the backend got.
            // Said in words, because a sink that has stopped is silence and
            // silence is what a quiet channel sounds like: the same
            // argument core/rpc/client.h makes for the ended callback.
            sink_fault_ = QStringLiteral(
                              "the output device stopped (%1). Pick another output, or "
                              "plug it back in and pick it again.")
                              .arg(static_cast<int>(why));

            // NOT close_sink() HERE. This runs from the QAudioSink's own
            // stateChanged, so the sink whose signal is on the stack would
            // be destroyed under it. tick() does the close on the next
            // pass with nothing of the sink's live.
            //
            // Recording the fault and leaving the sink in place was the
            // whole bug: playing() reads sink_ != nullptr, so the window
            // went on claiming to be playing out of a device that had
            // stopped, and the reopen branch in tick() only fires when
            // sink_ is null or the generation moved, so nothing ever
            // reopened either.
            sink_failed_ = true;
            emit statusChanged();
            break;
        }
    }
}

void AudioPlayer::tick()
{
    // THE STATUS LINE IS NOT ONLY COUNTERS, so the compare at the bottom
    // cannot be only counters either. Everything open_sink, close_sink and
    // handle_sink_state write during this pass is snapshotted here and
    // compared after. It used to compare the counters and the two depths
    // alone, and every one of these changes on a pass where no frame
    // moved: a device that refuses the format writes a fault with the ring
    // empty and every counter still zero, and the operator was shown
    // nothing until the next frame happened to arrive, which on a refused
    // device is never.
    const QString was_fault = fault();
    const QString was_note = note_;
    const QString was_active = active_device_;
    const int was_sink_millis = sink_millis_;

    if (sink_failed_ && sink_ != nullptr) {
        // handle_sink_state saw the sink stop with an error and could not
        // take it down from inside the sink's own signal. Done here, where
        // nothing of the sink's is on the stack, so playing() stops
        // claiming a device that has stopped.
        close_sink();
    }

    // The lead ring, whose frames the status line describes. See THE MIX in
    // the header for which one that is. One snapshot, for the reason
    // AudioRing::Snapshot gives.
    const int lead = link_.mixLeadSlot();
    const AudioRing::Snapshot ring_state =
        lead < 0 ? AudioRing::Snapshot{}
                 : link_.audioRingAt(static_cast<std::size_t>(lead)).snapshot();
    const RingFormat format = ring_state.format;

    // TWO DIFFERENT QUESTIONS, READ FROM TWO DIFFERENT PROPERTIES.
    //
    // A stream exists, which is what decides whether a sink should be open
    // at all. It goes false on its own: every heard receiver is cleared or
    // muted, the engine tears the subscriptions down, the link reconnects.
    //
    // WHAT THIS USED TO READ: link_.audioActive(), the pane's own
    // subscription. With a rack the pane's receiver can be muted while
    // another plays, so the question is whether the mix has a lead.
    const bool streaming = lead >= 0;

    // The operator turned listening off, which is a deliberate act and is
    // the only one of the two that clears the latch below.
    const bool listening = link_.audioWanted();

    if (!listening) {
        // THE THIRD THING THAT CLEARS THE LATCH, AND THE ONE AN OPERATOR
        // REACHES FOR FIRST.
        //
        // sink_failed_ was cleared by setDevice and refresh_devices alone,
        // and close_sink does not touch it, so switching listen off and
        // back on after a device error hit the branch below and did
        // nothing. That is the first thing anyone tries and it looked like
        // the switch was broken rather than the output.
        //
        // WHAT THIS BRANCH USED TO TEST. Until 2026-09-20 it was one arm of
        // the chain below, on `!want`, and want was link_.audioActive().
        // That is the presence of a STREAM and not the listen switch, and
        // the comment beside it already said "the operator turning listening
        // off is a deliberate act", which was the other property. The gap is
        // not cosmetic: audioActive goes false with the switch still on
        // every time the pane's receiver is cleared, a mode change rebuilds
        // the receiver, or the link reconnects, and EngineLink's own header
        // says the switch is sticky across all three. So a failed device was
        // re-attempted on every one of those, twenty times a second for as
        // long as the gap lasted, which is the retry loop the latch exists
        // to stop.
        //
        // It is its own `if` and no longer an arm of the chain below,
        // because the two conditions are now different: the sink still has
        // to be closed the moment the stream goes, whatever the switch says.
        sink_failed_ = false;
    }

    if (!streaming) {
        if (sink_ != nullptr) {
            close_sink();
        }
    } else if (sink_failed_) {
        // NOT REOPENED UNTIL SOMETHING CHANGES. A device that has just
        // failed fails again, and this branch runs twenty times a second,
        // so an automatic retry is a loop that rewrites the same error
        // forever and never lets the operator read it. The latch is
        // cleared by the operator picking a device and by the device list
        // changing, which are the two things that make another attempt
        // worth making, and both are what the message asks for.
    } else if (sink_ == nullptr) {
        // Opened at the device's format, whatever the streams are. Neither
        // fault is cleared here: a device that refused refuses again on the
        // next tick, so clearing would flicker the message twenty times a
        // second; open_sink owns sink_fault_ and rewrites it either way, and
        // device_fault_ outlives the open on purpose.
        //
        // WHAT THIS BRANCH USED TO BE: a reopen whenever the lead ring's
        // format generation moved or its format differed from the sink's,
        // "reopened rather than resampled, for the reason the header gives:
        // this process holds no DSP", with a second branch for a lead that
        // moved to another ring at the same format and a block raising a
        // fault when the sink stayed at the wrong format for two passes. A
        // stream's shape is AudioMix's business now and none of it applies.
        open_sink();
    }

    // What was adapted, said in words. Computed each pass because the lead
    // can move to a stream at another rate without the sink reopening.
    note_ = sink_ == nullptr ? QString{} : describe_adaptation(format);

    const RingCounts counts = ring_state.counts;
    const FrameSource showing =
        pull_ == nullptr ? FrameSource::idle : pull_->last_source();

    const int buffered_ms = millis_for(ring_state.frames_buffered, format.sample_rate);
    const int ring_ms = millis_for(ring_state.capacity_frames, format.sample_rate);

    // Late audio across every heard receiver, not the lead's alone: the lead
    // is read from its oldest frame and is never the one that skips. One
    // snapshot per ring, for the reason AudioRing::Snapshot gives.
    std::array<SkippedFrames, kMaxReceivers> skipped{};
    const std::uint32_t heard = link_.mixMask();
    for (std::size_t slot = 0; slot < kMaxReceivers; ++slot) {
        if ((heard & (1U << slot)) != 0) {
            const AudioRing::Snapshot one = link_.audioRingAt(slot).snapshot();
            skipped[slot] = SkippedFrames{one.counts.frames_skipped, one.format.sample_rate};
        }
    }
    const double skipped_ms = skipped_millis(skipped);

    // Rounded to what the readout prints, so a trim moving in its third
    // decimal is not a status change twenty times a second.
    const double trim_ppm =
        pull_ == nullptr ? 0.0 : std::round(pull_->drift_trim_ppm() * 10.0) / 10.0;
    const std::uint64_t realigned = pull_ == nullptr ? 0 : pull_->alignments();

    const bool changed = skipped_ms != late_skipped_millis_ || trim_ppm != drift_trim_ppm_ ||
                         realigned != realignments_ ||
                         counts.frames_written != counts_.frames_written ||
                         counts.frames_filled != counts_.frames_filled ||
                         counts.frames_overrun != counts_.frames_overrun ||
                         counts.frames_starved != counts_.frames_starved ||
                         counts.resyncs != counts_.resyncs ||
                         counts.gap_events != counts_.gap_events ||
                         buffered_ms != buffered_millis_ || ring_ms != ring_millis_ ||
                         showing != shown_source_ || fault() != was_fault ||
                         note_ != was_note || active_device_ != was_active ||
                         sink_millis_ != was_sink_millis;

    counts_ = counts;
    late_skipped_millis_ = skipped_ms;
    drift_trim_ppm_ = trim_ppm;
    realignments_ = realigned;
    buffered_millis_ = buffered_ms;
    ring_millis_ = ring_ms;
    shown_source_ = showing;

    if (changed) {
        emit statusChanged();
    }
}

QString AudioPlayer::describe_adaptation(RingFormat lead) const
{
    if (!lead.valid() || sink_rate_ == 0) {
        return {};
    }

    // A NOTE AND NOT A FAULT, kept apart because the two mean opposite
    // things to the operator: a fault is something to act on and this is a
    // statement that nothing needs acting on. Filing it as a fault would
    // train the operator to ignore the line that also carries a device that
    // has gone.
    QStringList parts;
    if (lead.sample_rate != sink_rate_) {
        parts.append(QStringLiteral("the focused receiver's %1 Hz is resampled to %2's %3 Hz.")
                         .arg(lead.sample_rate)
                         .arg(active_device_)
                         .arg(sink_rate_));
    }
    if (lead.channel_count == 1 && sink_channels_ > 1) {
        parts.append(QStringLiteral("its one channel is copied to all %1 outputs.")
                         .arg(sink_channels_));
    }
    return parts.join(QStringLiteral("  "));
}

QString AudioPlayer::source() const
{
    // No default label; see cmake/CompilerFlags.cmake.
    switch (shown_source_) {
        case FrameSource::idle:
            return QStringLiteral("waiting");
        case FrameSource::audio:
            return QStringLiteral("audio");
        case FrameSource::gated:
            return QStringLiteral("squelched");
        case FrameSource::gap_fill:
            return QStringLiteral("gap");
        case FrameSource::starved:
            return QStringLiteral("starving");
    }
    return QStringLiteral("waiting");
}

bool AudioPlayer::squelchOpen() const
{
    return shown_source_ == FrameSource::audio;
}

}  // namespace revenant::ui
