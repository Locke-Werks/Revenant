#include "audio/audio_ring.h"

#include <algorithm>
#include <cstring>

namespace revenant::ui {
namespace {

// The floor the ring is allocated at, expressed in chunks rather than in
// milliseconds, and applied only when a chunk turns out to be longer than
// the granted depth.
//
// The engine applies a two-chunk floor to its own queue for a reason
// core/rpc/revenant.capnp states at length: a depth under two chunks evicts
// every chunk before it can be sent, the client hears nothing, and every
// other check still passes. The same arithmetic applies on this side with
// one extra term, because the card is pulling out of this ring while a
// chunk is being written into it. Four is two for the engine's reason and
// two for the card's.
constexpr std::size_t kMinChunksBuffered = 4;

}  // namespace

void AudioRing::set_depth_millis(std::uint32_t millis)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    depth_millis_ = millis;
}

void AudioRing::establish(RingFormat incoming)
{
    format_ = incoming;
    ++format_generation_;

    // Milliseconds to frames, which is the conversion the engine cannot do
    // when it answers subscribeAudio and this object cannot do before the
    // first chunk either. Both need the receiver's audio rate and the wire
    // does not carry it anywhere else.
    const std::uint64_t frames =
        (static_cast<std::uint64_t>(depth_millis_) * incoming.sample_rate) / 1000U;

    // One frame rather than zero at the bottom, so the modulo arithmetic
    // below has a divisor. A depth of zero is a caller that subscribed
    // without recording the grant, and it starves rather than dividing by
    // zero; the counters say which.
    capacity_ = std::max<std::size_t>(static_cast<std::size_t>(frames), 1U);

    samples_.assign(capacity_ * incoming.channel_count, 0.0F);
    sources_.assign(capacity_, static_cast<std::uint8_t>(FrameSource::idle));

    head_ = 0;
    buffered_ = 0;
    have_stream_ = false;
    next_index_ = 0;
    last_source_ = FrameSource::idle;
}

void AudioRing::grow_to(std::size_t frames)
{
    if (frames <= capacity_ || !format_.valid()) {
        return;
    }

    const std::size_t channels = format_.channel_count;
    std::vector<float> samples(frames * channels, 0.0F);
    std::vector<std::uint8_t> sources(frames, static_cast<std::uint8_t>(FrameSource::idle));

    // Laid out from zero rather than copied wrapped, because the new ring
    // is a different size and the old head offset means nothing in it.
    for (std::size_t i = 0; i < buffered_; ++i) {
        const std::size_t from = (head_ + i) % capacity_;
        std::memcpy(&samples[i * channels], &samples_[from * channels],
                    channels * sizeof(float));
        sources[i] = sources_[from];
    }

    samples_.swap(samples);
    sources_.swap(sources);
    capacity_ = frames;
    head_ = 0;
}

void AudioRing::evict_front(std::size_t frames)
{
    const std::size_t going = std::min(frames, buffered_);
    head_ = (head_ + going) % capacity_;
    buffered_ -= going;
    counts_.frames_overrun += going;
}

void AudioRing::push(const float* samples, std::size_t frames, FrameSource source)
{
    if (frames == 0) {
        return;
    }

    // A single chunk longer than the whole ring cannot be written at any
    // head position, so the ring grows to hold several of them rather than
    // the write failing. This is the path a very slow source block or a
    // very low granted depth takes.
    if (frames > capacity_) {
        grow_to(frames * kMinChunksBuffered);
    }

    if (buffered_ + frames > capacity_) {
        evict_front(buffered_ + frames - capacity_);
    }

    const std::size_t channels = format_.channel_count;
    std::size_t written = 0;
    while (written < frames) {
        const std::size_t at = (head_ + buffered_) % capacity_;
        const std::size_t run = std::min(frames - written, capacity_ - at);
        if (samples == nullptr) {
            std::memset(&samples_[at * channels], 0, run * channels * sizeof(float));
        } else {
            std::memcpy(&samples_[at * channels], &samples[written * channels],
                        run * channels * sizeof(float));
        }
        std::memset(&sources_[at], static_cast<int>(source), run);
        buffered_ += run;
        written += run;
    }
}

void AudioRing::push_silence(std::size_t frames, FrameSource source)
{
    push(nullptr, frames, source);
}

void AudioRing::write(const rpc::AudioChunk& chunk)
{
    const RingFormat incoming{chunk.sample_rate, chunk.channel_count};

    const std::lock_guard<std::mutex> lock(mutex_);

    if (!incoming.valid()) {
        // WHAT THIS PARAGRAPH USED TO SAY
        //
        // Until 2026-09-20 it read "A rate of zero cannot open a device and
        // a channel count of zero makes AudioChunk::frames a divide by
        // zero." The second half is false and was false when it was
        // written: core/rpc/types.h guards that division and returns zero
        // frames for a zero channel count. It is recorded rather than
        // swapped because the same sentence was repeated on
        // RingCounts::malformed_chunks and in the test that covers this
        // branch, so anyone who took it from one of those three is owed
        // the retraction.
        //
        // WHAT THIS BRANCH ACTUALLY PREVENTS, which is worse than a divide
        // and is the reason it stays: the line below this one establishes
        // on any format that differs from the current one. An invalid
        // format differs, so ONE malformed chunk in the middle of a
        // healthy stream would re-establish the ring on it: everything
        // buffered dropped with no counter charged for it, the format
        // generation bumped, and AudioPlayer closing the sink because the
        // format it now reads is invalid. A single bad chunk would cost
        // the whole ring's depth of real audio and a sink reopen.
        //
        // A zero rate also survives establish() in a shape nothing else
        // handles: capacity_ floors at one frame, grow_to refuses to grow
        // an invalid format, and push then drives buffered_ past capacity_.
        //
        // Counted rather than asserted: this is wire data and a client does
        // not get to decide that the far end is impossible.
        ++counts_.malformed_chunks;
        return;
    }

    if (incoming != format_) {
        establish(incoming);
    }

    const std::uint64_t frames64 = chunk.frames();

    if (!have_stream_) {
        // The first chunk's own index is not a gap. The stream opens late
        // by design: a demodulator produces nothing until its filter
        // support is inside real samples, and the engine emits no chunk for
        // a dispatch that produced none.
        have_stream_ = true;
        next_index_ = chunk.sample_index;
    }

    std::uint64_t gap = 0;
    if (chunk.sample_index > next_index_) {
        gap = chunk.sample_index - next_index_;
    } else if (chunk.sample_index < next_index_) {
        // Backwards. The engine's per-pair invariant says this cannot
        // happen inside one receiver's stream, so reaching it means the
        // stream is not the one this object was following: an id reused by
        // a later receiver is the way that happens. There is nothing to
        // splice, so the baseline moves and the count says it did.
        ++counts_.restarts;
        next_index_ = chunk.sample_index;
    }

    if (gap > 0) {
        ++counts_.gap_events;

        // Split exactly the way core/rpc/revenant.capnp asks for it to be
        // split. framesDroppedBefore is what the SERVER's queue evicted,
        // so that much of the gap is this client being too slow, and
        // anything past it went missing upstream in the engine. Two causes
        // with two different fixes, so they are not added together.
        const std::uint64_t wire = std::min(gap, chunk.frames_dropped_before);
        counts_.frames_gap_wire += wire;
        counts_.frames_gap_upstream += gap - wire;
    }

    // Whether the gap plus the chunk can be placed at all, written so that
    // the sum cannot overflow.
    //
    // A chunk with NO gap in front of it always fits, whatever its length,
    // because push grows the ring for one longer than the whole capacity.
    // Only the gap can force a resync: growing the ring to hold a gap would
    // mean allocating for the worst stall the network ever has and then
    // keeping that memory for the life of the stream.
    const bool fits =
        gap == 0 || (gap <= capacity_ && frames64 <= capacity_ - gap);

    if (!fits) {
        // The resync. Filling this gap would evict live audio to make room
        // for silence, which is the wrong way round, so everything buffered
        // goes and the chunk starts a fresh timeline. The jump is real and
        // frames_gap_discarded plus resyncs is the only record of it.
        //
        // frames_overrun is deliberately not charged for the drop: that
        // counter means the card was too slow to take what arrived, and
        // this is the opposite fault.
        head_ = 0;
        buffered_ = 0;
        ++counts_.resyncs;
        counts_.frames_gap_discarded += gap;
    } else if (gap > 0) {
        push_silence(static_cast<std::size_t>(gap), FrameSource::gap_fill);
        counts_.frames_filled += gap;
    }

    if (frames64 > 0) {
        push(chunk.samples.data(), static_cast<std::size_t>(frames64),
             chunk.squelch_open ? FrameSource::audio : FrameSource::gated);
        counts_.frames_written += frames64;
    }

    next_index_ = chunk.sample_index + frames64;
}

ReadResult AudioRing::read(float* out, std::size_t frames, const RingFormat& expect)
{
    const std::lock_guard<std::mutex> lock(mutex_);

    ReadResult result;

    // Read under the same lock as the samples below, which is the whole
    // point of expect being an argument. See THE FORMAT AND THE SAMPLES
    // COME BACK TOGETHER on the declaration.
    result.format = format_;
    result.format_moved = format_ != expect;
    result.last_source = last_source_;

    if (out == nullptr || frames == 0) {
        return result;
    }

    if (result.format_moved || !format_.valid()) {
        // Either the stream changed shape under the caller or there is no
        // stream at all. Both leave the buffer ALONE, because only the
        // caller knows how many floats it holds, and both report the whole
        // request as starved because silence is what the card gets.
        result.frames_starved = frames;
        counts_.frames_starved += frames;
        return result;
    }

    const std::size_t channels = format_.channel_count;
    const std::size_t take = std::min(frames, buffered_);

    std::size_t done = 0;
    while (done < take) {
        const std::size_t run = std::min(take - done, capacity_ - head_);
        std::memcpy(&out[done * channels], &samples_[head_ * channels],
                    run * channels * sizeof(float));

        // The last frame of this run, which after the loop is the last
        // frame handed over. That is deliberately the newest frame going to
        // the card and not the newest frame in the ring: an indicator on
        // the latter leads the sound by the whole depth.
        last_source_ = static_cast<FrameSource>(sources_[head_ + run - 1]);

        head_ = (head_ + run) % capacity_;
        buffered_ -= run;
        done += run;
    }
    result.frames_from_ring = take;

    if (take < frames) {
        const std::size_t short_by = frames - take;
        std::memset(&out[take * channels], 0, short_by * channels * sizeof(float));
        result.frames_starved = short_by;
        counts_.frames_starved += short_by;

        // The starve is what the card plays last, so it is what the
        // indicator says. A read that was partly real audio and partly
        // invented silence is a dip, and a dip that reads as healthy audio
        // is the failure this whole enum exists to prevent.
        last_source_ = FrameSource::starved;
    }

    result.last_source = last_source_;
    return result;
}

void AudioRing::reset()
{
    const std::lock_guard<std::mutex> lock(mutex_);

    // The format goes too, which is what makes set_depth_millis take effect:
    // the next chunk then differs from format_ and re-establishes at the
    // depth the engine has just granted.
    //
    // WHAT THIS PARAGRAPH USED TO SAY
    //
    // Until 2026-09-20 it ended "It also bumps the generation, so a
    // consumer holding an open sink reopens rather than playing the next
    // receiver at the last one's rate." This function has never touched
    // format_generation_. The claim was wrong and the code is right, so the
    // sentence went rather than the behaviour, and it is recorded here
    // because a reader who took it at face value would have concluded that
    // format_generation() alone is enough to notice a reset.
    //
    // Nothing is lost by not bumping. A reset leaves format_ invalid, which
    // AudioPlayer::tick reads as no stream and closes the sink for; the
    // next chunk then re-establishes and THAT bumps the generation, so the
    // reopen happens either way. A reader mid-pull is covered separately
    // and more tightly: read() takes the format the caller is reading at
    // and reports format_moved under its own lock, so it sees the reset on
    // the very next pull rather than at the next timer tick.
    format_ = {};
    capacity_ = 0;
    head_ = 0;
    buffered_ = 0;
    have_stream_ = false;
    next_index_ = 0;
    last_source_ = FrameSource::idle;
    samples_.clear();
    sources_.clear();
}

void AudioRing::reset_counts()
{
    reset();
    const std::lock_guard<std::mutex> lock(mutex_);
    counts_ = {};
}

RingFormat AudioRing::format() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return format_;
}

std::uint64_t AudioRing::format_generation() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return format_generation_;
}

RingCounts AudioRing::counts() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return counts_;
}

std::size_t AudioRing::frames_buffered() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return buffered_;
}

std::size_t AudioRing::capacity_frames() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return capacity_;
}

}  // namespace revenant::ui
