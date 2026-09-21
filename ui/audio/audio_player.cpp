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

    const std::size_t in_channels = stream_.channel_count;
    const std::size_t floats = frames * in_channels;
    if (scratch_.size() < floats) {
        scratch_.resize(floats);
    }

    // stream_ goes IN, so the ring compares it against its own format under
    // the one lock it takes for the copy. Asking format() first and read()
    // second is two locked calls with a window between them, and the writer
    // is the Cap'n Proto event loop: a receiver that changed rate in that
    // window would be copied into scratch_ at the new channel count while
    // every length here was worked out from the old one.
    const ReadResult result = ring_.read(scratch_.data(), frames, stream_);
    last_source_.store(result.last_source, std::memory_order_relaxed);

    if (result.format_moved) {
        // The stream changed shape under an open sink, which is one timer
        // tick at most: the ring re-establishes on the first chunk at the
        // new rate and tick() reopens the sink. The ring copied nothing and
        // left scratch_ alone, so the silence is written here at the SINK's
        // channel count, which is the only width that fits this buffer.
        // Silence rather than the new stream's samples, because playing
        // those through a sink opened at the old rate is a tape at the
        // wrong speed.
        std::memset(data, 0, out_bytes);
        last_source_.store(FrameSource::starved, std::memory_order_relaxed);

        // Counted, because starved is a lie about the cause here and it is
        // the only report the ring can make: from inside the ring this is
        // silence reaching the card, and from out here it is silence
        // reaching the card BECAUSE THIS SINK IS THE WRONG SHAPE. tick()
        // reads the count, says which one it is on the status line, and
        // raises a fault when it is still true on the following pass. See
        // the mismatch block in tick().
        format_moved_pulls_.fetch_add(1, std::memory_order_relaxed);
        return frames * frame_bytes;
    }

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
            sink_fault_ = QStringLiteral(
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

    // The widening is a NOTE and not a fault, kept apart because the two
    // mean opposite things to the operator: a fault is something to act on
    // and this is a statement that nothing needs acting on, the audio is
    // reaching the card exactly as the engine sent it and simply on more
    // than one channel. Filing it as a fault would train the operator to
    // ignore the line that also carries a device that has gone.
    note_ = widened;

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
    for (const QString& one : {device_fault_, sink_fault_, format_fault_}) {
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
    open_generation_ = 0;
    active_device_.clear();
    sink_millis_ = 0;

    // The mismatch belonged to the sink that is going, and the next
    // RingSource starts its own count at zero. Leaving moved_pulls_ where
    // it was would read the next sink's first pull as a count going
    // BACKWARDS, which is a difference, which is a mismatch reported on a
    // sink that has only just opened at the right format.
    moved_pulls_ = 0;
    shown_mismatch_ = false;
    format_fault_.clear();

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

    AudioRing& r = ring();

    // ONE CALL, BECAUSE THE FORMAT AND THE GENERATION HAVE TO AGREE.
    //
    // This was format() and then format_generation(), two locked calls with
    // the Cap'n Proto event loop free to run between them. A chunk at a new
    // rate landing in that window returned the OLD format with the NEW
    // generation, and the reopen below then opened a sink at the old rate
    // and filed it under the new generation. Nothing asked for a reopen
    // afterwards, because the only thing that asks is a generation past the
    // recorded one and the recorded one was already current. Every pull
    // after that found the format moved and wrote silence, for as long as
    // the receiver stayed at that rate. See AudioRing::Snapshot.
    const AudioRing::Snapshot ring_state = r.snapshot();
    const RingFormat format = ring_state.format;
    const std::uint64_t generation = ring_state.generation;

    // What the sink's own thread did with the pulls since the last pass. A
    // pull that found the format moved wrote silence at the sink's width
    // and took nothing from the ring. Read BEFORE the reopen below, which
    // is what ends it: the operator is still owed the truth about the 50 ms
    // it lasted, and a mismatch that outlives several passes is a sink that
    // is not being reopened at all.
    const std::uint64_t moved_pulls =
        pull_ == nullptr ? moved_pulls_ : pull_->format_moved_pulls();
    const bool mismatch = moved_pulls != moved_pulls_;
    moved_pulls_ = moved_pulls;

    const bool want = link_.audioActive();

    if (!want) {
        if (sink_ != nullptr) {
            close_sink();
        }

        // THE THIRD THING THAT CLEARS THE LATCH, AND THE ONE AN OPERATOR
        // REACHES FOR FIRST.
        //
        // sink_failed_ was cleared by setDevice and refresh_devices alone,
        // and close_sink does not touch it, so switching listen off and
        // back on after a device error hit the branch below and did
        // nothing. That is the first thing anyone tries and it looked like
        // the switch was broken rather than the output.
        //
        // Cleared on !want rather than on !format.valid(), which is why the
        // two are no longer one branch. The operator turning listening off
        // is a deliberate act and the next attempt is worth making; a
        // stream that has momentarily no format is not an act at all, and
        // clearing there would turn the latch back into the retry loop it
        // exists to stop, at twenty attempts a second.
        sink_failed_ = false;
    } else if (!format.valid()) {
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
    } else if (sink_ == nullptr || generation != open_generation_ ||
               (pull_ != nullptr && pull_->stream() != format)) {
        // A generation past the one the sink was opened for is a stream
        // that changed rate or channel count. Reopened rather than
        // resampled, for the reason the header gives: this process holds no
        // DSP.
        //
        // THE FORMAT COMPARISON IS NOT REDUNDANT WITH THE GENERATION ONE.
        // The generation is a cheap proxy for "the ring moved", and the
        // sink being the wrong shape for the stream is the condition that
        // actually matters. Anything that leaves those two disagreeing is
        // permanent silence, because the generation is the only thing the
        // reopen used to consult and it reads as up to date. The snapshot
        // above closes the one route in that was known; this closes the
        // class, so the next one that gets invented costs 50 ms instead of
        // the whole session.
        //
        // Neither fault is cleared here. A device that refused the format
        // refuses it again on the next tick, so clearing would flicker the
        // message twenty times a second; open_sink owns sink_fault_ and
        // rewrites it either way, and device_fault_ outlives the open on
        // purpose.
        open_sink(format, generation);
    }

    // THE MISMATCH, SAID IN WORDS RATHER THAN LEFT AS SILENCE.
    //
    // RAISED ON THE SECOND CONSECUTIVE PASS, WHICH IS A REPLACEMENT FOR A
    // TICK COUNTER THAT COULD NOT FIRE.
    //
    // What this used to do: mismatch_ticks_ counted passes and the fault
    // needed ten of them, half a second. It never reached two. close_sink()
    // zeroes mismatch_ticks_, open_sink() begins with close_sink(), and the
    // reopen branch above runs on exactly the condition that produces a
    // mismatch, so every pass that counted one also reset the count. The
    // sentence below was unreachable for the whole life of the feature, and
    // the constant behind it read as a tuned threshold.
    //
    // The fix is not a bigger number. It is that a count accumulated across
    // ticks is the wrong shape when the event being counted destroys the
    // counter: the fault belongs on the TRANSITION into the state and is
    // cleared explicitly on the way out.
    //
    // WHY TWO PASSES AND NOT ONE. One pass of mismatch is the ordinary cost
    // of a receiver changing rate, and the reopen on that same pass ends
    // it, so the next pass reads the fresh RingSource's count against a
    // moved_pulls_ that close_sink zeroed and finds no mismatch. source()
    // says "format mismatch" for that 50 ms and nothing else needs saying.
    // Two passes running means the reopen either did not happen or did not
    // stick, which is 100 ms of silence with a healthy wire behind it and
    // is the state an operator cannot tell from a quiet band.
    //
    // Both of the ways in are still caught. A sink that is not being
    // reopened keeps the same pull_ and its count keeps climbing. A
    // receiver whose shape changes faster than a sink can be opened for it
    // gets a fresh pull_ each pass and that one starts reporting moved
    // pulls before the next tick.
    //
    // shown_mismatch_ holds the previous pass's answer and is assigned at
    // the bottom of this function, so reading it here reads the pass
    // before. close_sink() sets it false, which cannot matter from inside
    // tick(): the assignment below overwrites it either way.
    if (!mismatch) {
        format_fault_.clear();
    } else if (shown_mismatch_ && pull_ != nullptr) {
        // NO DURATION AND NO REMEDY IN THE SENTENCE. A tick count in it
        // would rewrite the string twenty times a second, which is the
        // flicker the reopen branch above refuses for the same reason, and
        // the two ways to get here want opposite advice: a sink that is
        // not reopening wants audio toggled off and on, and a receiver
        // whose shape is flapping wants leaving alone. Both want the fact,
        // which is that the card is being fed silence and the wire is not
        // the reason.
        const RingFormat open_at = pull_->stream();
        format_fault_ =
            QStringLiteral("the output is open at %1 Hz on %2 channel(s) and the stream "
                           "is %3 Hz on %4, so every frame reaching the card is silence.")
                .arg(open_at.sample_rate)
                .arg(open_at.channel_count)
                .arg(format.sample_rate)
                .arg(format.channel_count);
    }

    const RingCounts counts = ring_state.counts;
    const FrameSource showing =
        pull_ == nullptr ? FrameSource::idle : pull_->last_source();

    const int buffered_ms = millis_for(ring_state.frames_buffered, format.sample_rate);
    const int ring_ms = millis_for(ring_state.capacity_frames, format.sample_rate);

    const bool changed = mismatch != shown_mismatch_ ||
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
    buffered_millis_ = buffered_ms;
    ring_millis_ = ring_ms;
    shown_source_ = showing;
    shown_mismatch_ = mismatch;

    if (changed) {
        emit statusChanged();
    }
}

QString AudioPlayer::source() const
{
    // AHEAD OF THE FRAME SOURCES, because the ring reports this case as
    // starved and starved is the wrong answer here. Starving means the
    // audio is late, which points at the network or the engine. Nothing is
    // late in a mismatch: the chunks are arriving and the sink is open at a
    // rate they are not. Same silence, opposite place to go looking.
    if (shown_mismatch_) {
        return QStringLiteral("format mismatch");
    }

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
