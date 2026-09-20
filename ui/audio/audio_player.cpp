#include "audio/audio_player.h"

#include <algorithm>
#include <cstring>

#include <QMediaDevices>

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

RingSource::RingSource(AudioRing& ring, RingFormat stream, int out_channels, QObject* parent)
    : QIODevice(parent), ring_(ring), stream_(stream), out_channels_(out_channels)
{
}

qint64 RingSource::bytesAvailable() const
{
    // The sink's whole buffer, always, because readData always fills the
    // whole request. A device that reports what it actually holds would
    // report zero the instant the network hiccupped, and QAudioSink reads a
    // zero-length answer as the end of the stream: it goes Idle and stops.
    // A dropout would then end the audio rather than dip it, which is the
    // one failure mode the starve fill exists to avoid.
    return static_cast<qint64>(out_channels_) * 4 * stream_.sample_rate;
}

qint64 RingSource::readData(char* data, qint64 maxlen)
{
    const qint64 frame_bytes =
        static_cast<qint64>(out_channels_) * static_cast<qint64>(sizeof(float));
    if (data == nullptr || frame_bytes <= 0 || maxlen < frame_bytes) {
        return 0;
    }

    const auto frames = static_cast<std::size_t>(maxlen / frame_bytes);
    const auto out_bytes = static_cast<std::size_t>(frames) *
                           static_cast<std::size_t>(frame_bytes);

    // The stream changed shape under an open sink, which is one timer tick
    // at most: the ring re-establishes on the first chunk at the new rate
    // and tick() reopens the sink. Silence for that tick rather than the
    // new stream's samples, because playing them through a sink opened at
    // the old rate is a tape at the wrong speed.
    if (ring_.format() != stream_) {
        std::memset(data, 0, out_bytes);
        last_source_.store(FrameSource::starved, std::memory_order_relaxed);
        return frames * frame_bytes;
    }

    const std::size_t in_channels = stream_.channel_count;
    const std::size_t floats = frames * in_channels;
    if (scratch_.size() < floats) {
        scratch_.resize(floats);
    }

    const ReadResult result = ring_.read(scratch_.data(), frames);
    last_source_.store(result.last_source, std::memory_order_relaxed);

    if (static_cast<std::size_t>(out_channels_) == in_channels) {
        std::memcpy(data, scratch_.data(), floats * sizeof(float));
        return frames * frame_bytes;
    }

    // THE ONE CONVERSION THIS PLAYER DOES, and it is a copy rather than a
    // decision. A mono frame is written to every output channel, which is
    // what mono means; no gain is applied, nothing is mixed and nothing is
    // resampled. open_sink is what guarantees this is only ever reached
    // with one input channel.
    const auto out_floats = frames * static_cast<std::size_t>(out_channels_);
    if (widened_.size() < out_floats) {
        widened_.resize(out_floats);
    }
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float value = scratch_[frame];
        for (int channel = 0; channel < out_channels_; ++channel) {
            widened_[frame * static_cast<std::size_t>(out_channels_) +
                     static_cast<std::size_t>(channel)] = value;
        }
    }
    std::memcpy(data, widened_.data(), out_floats * sizeof(float));
    return frames * frame_bytes;
}

// ---------------------------------------------------------------------------
// AudioPlayer
// ---------------------------------------------------------------------------

AudioPlayer::AudioPlayer(EngineLink& link, QObject* parent) : QObject(parent), link_(link)
{
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
            fault_ = QStringLiteral(
                "the output device that was selected has gone; playing on the system "
                "default");
            close_sink();
            emit statusChanged();
        }
    }

    emit devicesChanged();
}

void AudioPlayer::setDevice(int index)
{
    if (index < 0 || index > device_handles_.size()) {
        return;
    }
    if (index == device_index_) {
        return;
    }

    device_index_ = index;
    wanted_device_id_ =
        index == 0 ? QByteArray{} : device_handles_.at(index - 1).id();

    // A device change is a new sink. The ring is not touched, so the audio
    // buffered for the old device is played out of the new one rather than
    // thrown away.
    fault_.clear();
    close_sink();
    emit deviceChanged();
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

void AudioPlayer::open_sink(RingFormat format, std::uint64_t generation)
{
    close_sink();

    bool fell_back = false;
    const QAudioDevice out = resolve_device(fell_back);
    if (out.isNull()) {
        fault_ = QStringLiteral("this machine has no audio output device");
        return;
    }

    QAudioFormat wanted;
    wanted.setSampleRate(static_cast<int>(format.sample_rate));
    wanted.setChannelCount(static_cast<int>(format.channel_count));
    wanted.setSampleFormat(QAudioFormat::Float);

    int out_channels = static_cast<int>(format.channel_count);
    QString widened;

    if (!out.isFormatSupported(wanted)) {
        // A DEVICE THAT REFUSES MONO IS THE ORDINARY CASE, NOT AN EDGE ONE.
        //
        // Every engine built from this tree sets AudioChunk::channelCount
        // to 1, and plenty of outputs take stereo and nothing else: the
        // default output on the machine this was written on is a Blackmagic
        // DeckLink Mini Monitor 4K, which offers 48000 Hz on 2 channels and
        // refuses the same rate on 1. Refusing outright there means the
        // operator can never listen on the device their machine is already
        // using, which is not a defensible answer to "play the audio".
        //
        // So a mono stream is widened by DUPLICATION, which is a copy and
        // not a conversion. Every output channel gets the same sample, no
        // gain is applied and nothing is mixed, so the audio that reaches
        // the card is bit for bit the audio the engine sent. That is a
        // different kind of change from the two refused below and it is
        // why it is allowed.
        //
        // Only from ONE channel. Fitting a stereo stream into some other
        // channel count is a downmix or an upmix with a matrix in it, which
        // is a decision about what the operator should hear, and nothing
        // here is entitled to make it.
        const QAudioFormat preferred = out.preferredFormat();
        QAudioFormat widened_format = wanted;
        widened_format.setChannelCount(preferred.channelCount());

        if (format.channel_count == 1 && preferred.channelCount() > 1 &&
            out.isFormatSupported(widened_format)) {
            wanted = widened_format;
            out_channels = preferred.channelCount();
            widened = QStringLiteral("%1 takes no mono, so the one channel is copied to "
                                     "all %2 of its outputs.")
                          .arg(out.description())
                          .arg(out_channels);
        } else {
            // REFUSED RATHER THAN CONVERTED, and the reason is headroom.
            // The schema is explicit that a demodulator puts a fully
            // modulated signal at exactly full scale and a settling AGC
            // overshoots it, so samples above 1.0 are ordinary here.
            // Converting to the 16-bit integer format such a device
            // usually does accept would clip every one of them, and
            // clipping an AGC overshoot sounds like the radio is
            // distorting rather than like the player is. Resampling is
            // refused on its own terms: it is a DSP decision and
            // ui/CMakeLists.txt links no part of core/dsp on purpose.
            //
            // So the operator is told which device refused what, and what
            // that device does want, and picks another output.
            fault_ = QStringLiteral(
                         "%1 will not take 32-bit float at %2 Hz on %3 channel(s). It "
                         "offers %4 Hz on %5. Pick another output.")
                         .arg(out.description())
                         .arg(format.sample_rate)
                         .arg(format.channel_count)
                         .arg(preferred.sampleRate())
                         .arg(preferred.channelCount());
            return;
        }
    }

    sink_ = std::make_unique<QAudioSink>(out, wanted);

    // The sink's depth, derived from the grant. See THE THREE BUFFER DEPTHS
    // at the top of audio/audio_player.h for why it is a quarter and why it
    // comes off the grant rather than off the request.
    const int granted = static_cast<int>(link_.audioGrantedMillis());
    sink_millis_ = std::max(kMinSinkMillis, granted / kSinkDepthDivisor);
    const auto sink_frames =
        static_cast<qsizetype>((static_cast<std::uint64_t>(sink_millis_) *
                                format.sample_rate) /
                               1000U);
    sink_->setBufferSize(sink_frames * out_channels *
                         static_cast<qsizetype>(sizeof(float)));
    sink_->setVolume(sink_gain());

    connect(sink_.get(), &QAudioSink::stateChanged, this,
            [this](QAudio::State state) { handle_sink_state(state); });

    pull_ = std::make_unique<RingSource>(ring(), format, out_channels);
    pull_->open(QIODevice::ReadOnly);
    sink_->start(pull_.get());

    open_generation_ = generation;
    active_device_ = out.description();

    // A sink is open, so whatever the last attempt left behind is answered.
    //
    // The widening is a NOTE and not a fault, kept apart because the two
    // mean opposite things to the operator: a fault is something to act on
    // and this is a statement that nothing needs acting on, the audio is
    // reaching the card exactly as the engine sent it and simply on more
    // than one channel. Filing it as a fault would train the operator to
    // ignore the line that also carries a device that has gone.
    note_ = widened;
    fault_.clear();
    if (fell_back) {
        fault_ = QStringLiteral(
            "the output device that was selected has gone; playing on the system default");
    }
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
    open_generation_ = 0;
    active_device_.clear();
    sink_millis_ = 0;

    // The note describes the sink that is going, so it goes with it. The
    // fault does NOT: close_sink is on the path a refused format takes,
    // and clearing here would erase the sentence that said why.
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
            // The sink ran out of data. RingSource always fills, so this is
            // reachable only with no stream at all, and tick() closes the
            // sink for that. Nothing to do and nothing to say.
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
            fault_ = QStringLiteral(
                         "the output device stopped (%1). Pick another output, or plug it "
                         "back in and pick it again.")
                         .arg(static_cast<int>(why));
            emit statusChanged();
            break;
        }
    }
}

void AudioPlayer::tick()
{
    AudioRing& r = ring();
    const RingFormat format = r.format();
    const std::uint64_t generation = r.format_generation();

    const bool want = link_.audioActive();

    if (!want || !format.valid()) {
        if (sink_ != nullptr) {
            close_sink();
        }
    } else if (sink_ == nullptr || generation != open_generation_) {
        // A generation past the one the sink was opened for is a stream
        // that changed rate or channel count. Reopened rather than
        // resampled, for the reason the header gives: this process holds no
        // DSP.
        //
        // fault_ is NOT cleared here. A device that refused the format
        // refuses it again on the next tick, so clearing would flicker the
        // message twenty times a second; open_sink rewrites it either way.
        open_sink(format, generation);
    }

    const RingCounts counts = r.counts();
    const std::size_t buffered = r.frames_buffered();
    const FrameSource showing =
        pull_ == nullptr ? FrameSource::idle : pull_->last_source();

    const int buffered_ms = millis_for(buffered, format.sample_rate);
    const int ring_ms = millis_for(r.capacity_frames(), format.sample_rate);

    const bool changed = counts.frames_written != counts_.frames_written ||
                         counts.frames_filled != counts_.frames_filled ||
                         counts.frames_overrun != counts_.frames_overrun ||
                         counts.frames_starved != counts_.frames_starved ||
                         counts.resyncs != counts_.resyncs ||
                         counts.gap_events != counts_.gap_events ||
                         buffered_ms != buffered_millis_ || ring_ms != ring_millis_ ||
                         showing != shown_source_;

    counts_ = counts;
    buffered_millis_ = buffered_ms;
    ring_millis_ = ring_ms;
    shown_source_ = showing;

    if (changed) {
        emit statusChanged();
    }
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
