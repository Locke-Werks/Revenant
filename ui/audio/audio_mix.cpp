#include "audio/audio_mix.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace revenant::ui {
namespace {

// Adds `frames` frames of `in`, at `in_channels`, into `out` at
// `out_channels`, scaled by `gain`.
//
// Mono goes to every output channel, which is what mono means. Stereo goes
// to the first two, or is averaged onto a device with one. Nothing here makes
// a surround decision: a third output channel and beyond get nothing from a
// stereo stream.
void add_mapped(const float* in, int in_channels, float* out, int out_channels,
                std::size_t frames, float gain)
{
    const auto in_width = static_cast<std::size_t>(in_channels);
    const auto out_width = static_cast<std::size_t>(out_channels);
    for (std::size_t i = 0; i < frames; ++i) {
        const float* from = in + i * in_width;
        float* to = out + i * out_width;
        if (in_channels == 1) {
            const float value = gain * from[0];
            for (std::size_t c = 0; c < out_width; ++c) {
                to[c] += value;
            }
        } else if (out_channels == 1) {
            float total = 0.0F;
            for (std::size_t c = 0; c < in_width; ++c) {
                total += from[c];
            }
            to[0] += gain * total / static_cast<float>(in_width);
        } else {
            const std::size_t shared = std::min(in_width, out_width);
            for (std::size_t c = 0; c < shared; ++c) {
                to[c] += gain * from[c];
            }
        }
    }
}

}  // namespace

AudioMix::AudioMix(std::uint32_t out_rate, int out_channels)
    : out_rate_(out_rate), out_channels_(std::max(out_channels, 1))
{
    limiter_.configure(out_rate_);
}

void AudioMix::prepare(Stream& stream, RingFormat format, bool multiplex, bool level)
{
    if (stream.format == format && stream.multiplex == multiplex && stream.level == level) {
        return;
    }
    stream.format = format;
    stream.multiplex = multiplex;
    stream.level = level;
    stream.copyable = false;
    if (multiplex) {
        // Pass the programme band and be 80 dB down at the pilot, which
        // takes the difference channel above it and the RDS subcarrier too.
        stream.resampler.configure(format.sample_rate, out_rate_,
                                   0.5 * (kProgrammeTopHz + kPilotHz),
                                   kPilotHz - kProgrammeTopHz);
    } else {
        // At the device's rate the kernel is built anyway, for the drift
        // trim to read through; the stream is still copied while the trim is
        // exactly zero.
        stream.copyable = format.sample_rate == out_rate_;
        stream.resampler.configure(format.sample_rate, out_rate_, 0.0, 0.0,
                                   EqualRates::interpolate);
    }
    stream.agc.configure(out_rate_);
    stream.deemphasis.configure(out_rate_);
    stream.placed = false;
    stream.history.clear();
    stream.history_start = 0;
}

void AudioMix::place(Stream& stream, double position, double ratio)
{
    stream.position =
        stream.copyable && ratio == 1.0 ? std::round(position) : position;
    stream.placed = true;
    stream.history.clear();
    stream.history_start = 0;
}

bool AudioMix::copying(const Stream& stream, double ratio)
{
    return stream.copyable && ratio == 1.0 && stream.position == std::floor(stream.position);
}

double AudioMix::step_of(const Stream& stream, double ratio)
{
    return copying(stream, ratio) ? 1.0 : stream.resampler.step() * ratio;
}

FrameSource AudioMix::render(Stream& stream, AudioRing& ring, std::size_t count, double ratio)
{
    const int channels = stream.format.channel_count;
    const auto width = static_cast<std::size_t>(channels);
    stream.rendered.assign(count * width, 0.0F);
    if (count == 0) {
        return FrameSource::idle;
    }

    // A stream placed before its own first frame, which is where a receiver
    // added after the lead's instant lands: silence until its audio starts.
    const bool copy = copying(stream, ratio);
    const double step = step_of(stream, ratio);
    std::size_t leading = 0;
    if (stream.position < 0.0) {
        leading = std::min(count, static_cast<std::size_t>(std::ceil(-stream.position / step)));
        stream.position += static_cast<double>(leading) * step;
    }
    const std::size_t produce = count - leading;
    if (produce == 0) {
        return FrameSource::idle;
    }

    // A copy reads no frame ahead of the playhead, but keeps the kernel's
    // half width of frames behind it once it has them, so the pull on which
    // the trim first moves off zero finds the frames its kernel reaches back
    // for. Reading them from the ring instead would find them already taken
    // and play the kernel's first outputs against silence: a click at the
    // moment the loop engages.
    //
    // WHAT WAS HERE: a passthrough branch that read_at straight into
    // `rendered` and kept no history, since a stream at the device's rate
    // was always a copy until the drift trim.
    const int half = stream.resampler.half_width();
    const double last_position = stream.position + static_cast<double>(produce - 1) * step;
    std::int64_t lo = static_cast<std::int64_t>(std::floor(stream.position)) - half + 1;
    const std::int64_t hi =
        static_cast<std::int64_t>(std::floor(last_position)) + (copy ? 0 : half);
    if (copy) {
        // Nothing behind the playhead is fetched for a copy: before the first
        // pull there is none held, and reaching back into the ring for it
        // would count the frames a placement passed over as not skipped.
        const auto at = static_cast<std::int64_t>(stream.position);
        const auto held_from = static_cast<std::int64_t>(stream.history_start);
        lo = stream.history.empty() ? at : std::clamp(lo, std::min(held_from, at), at);
    }
    const std::uint64_t from = lo < 0 ? 0U : static_cast<std::uint64_t>(lo);

    std::size_t held = stream.history.size() / width;
    const std::uint64_t held_end = stream.history_start + held;
    if (held == 0 || from < stream.history_start || from > held_end) {
        stream.history.clear();
        stream.history_start = from;
        held = 0;
    } else if (from > stream.history_start) {
        const auto drop = static_cast<std::size_t>(from - stream.history_start);
        stream.history.erase(stream.history.begin(),
                             stream.history.begin() + static_cast<std::ptrdiff_t>(drop * width));
        stream.history_start = from;
        held -= drop;
    }

    FrameSource source = FrameSource::idle;
    const std::uint64_t end = stream.history_start + held;
    if (hi >= 0 && static_cast<std::uint64_t>(hi) >= end) {
        const auto need = static_cast<std::size_t>(static_cast<std::uint64_t>(hi) - end + 1);
        stream.history.resize((held + need) * width, 0.0F);
        const ReadResult read =
            ring.read_at(&stream.history[held * width], need, end, stream.format);
        if (read.format_moved) {
            std::fill(stream.history.begin() + static_cast<std::ptrdiff_t>(held * width),
                      stream.history.end(), 0.0F);
        }
        source = read.last_source;
        held += need;
    }

    if (copy) {
        const auto at = static_cast<std::uint64_t>(stream.position);
        if (at >= stream.history_start && at + produce <= stream.history_start + held) {
            const auto offset = static_cast<std::size_t>(at - stream.history_start);
            std::memcpy(&stream.rendered[leading * width], &stream.history[offset * width],
                        produce * width * sizeof(float));
        }
    } else {
        for (std::size_t k = 0; k < produce; ++k) {
            const double p = stream.position + static_cast<double>(k) * step;
            stream.resampler.evaluate(stream.history.data(), stream.history_start, held, channels,
                                      p, &stream.rendered[(leading + k) * width]);
        }
    }
    stream.position += static_cast<double>(produce) * step;
    return source;
}

MixPull AudioMix::pull(std::span<AudioRing* const> rings, const MixControl& control, float* out,
                       std::size_t frames, std::int64_t now_ns)
{
    MixPull result;
    const auto out_width = static_cast<std::size_t>(out_channels_);
    sum_.assign(frames * out_width, 0.0F);

    const std::size_t offered = std::min(rings.size(), control.strips.size());
    if (streams_.size() < offered) {
        streams_.resize(offered);
    }

    // One snapshot per ring for the whole pull, so the lead's instant and
    // every other stream's anchor are read once.
    std::array<AudioRing::Snapshot, 16> snaps{};
    std::array<bool, 16> usable{};
    const std::size_t used = std::min<std::size_t>(offered, snaps.size());
    int lead = -1;
    for (std::size_t i = 0; i < used; ++i) {
        if (!control.strips[i].heard || rings[i] == nullptr) {
            streams_[i].placed = false;
            continue;
        }
        snaps[i] = rings[i]->snapshot();
        usable[i] = snaps[i].has_stream && snaps[i].format.valid();
        if (!usable[i]) {
            streams_[i].placed = false;
            continue;
        }
        const RingFormat format = snaps[i].format;
        const bool multiplex = control.strips[i].wfm && format.sample_rate >= kMultiplexRateHz;
        prepare(streams_[i], format, multiplex, control.strips[i].level);
    }
    if (control.lead >= 0 && static_cast<std::size_t>(control.lead) < used &&
        usable[static_cast<std::size_t>(control.lead)]) {
        lead = control.lead;
    } else {
        for (std::size_t i = 0; i < used; ++i) {
            if (usable[i]) {
                lead = static_cast<int>(i);
                break;
            }
        }
    }

    if (lead < 0) {
        if (out != nullptr) {
            std::memset(out, 0, frames * out_width * sizeof(float));
        }
        return result;
    }
    result.lead = lead;

    const auto lead_slot = static_cast<std::size_t>(lead);
    Stream& head = streams_[lead_slot];
    const AudioRing::Snapshot& lead_snap = snaps[lead_slot];
    const double lead_rate = static_cast<double>(head.format.sample_rate);

    // The drift loop measures a stream it has followed, so a new lead is
    // measured afresh. See WHAT RESTARTS THE MEASUREMENT in
    // audio/drift_trim.h.
    if (lead != last_lead_) {
        drift_.rebase();
        last_lead_ = lead;
    }
    const double ratio = correct_drift_ ? drift_.ratio() : 1.0;

    // The lead: from its oldest frame when it starts, moved up past a hole
    // its own ring opened, and never read past its newest frame.
    if (!head.placed) {
        place(head, static_cast<double>(lead_snap.head_index), ratio);
        drift_.rebase();
    } else {
        // The next frame the lead takes from its ring: the end of what it
        // already holds, or its position when it holds nothing yet.
        //
        // WHAT THIS USED TO SAY: "its position for a copy, and otherwise".
        // A copy holds the frames behind its playhead too since the drift
        // trim, so the end of what it holds is the next frame for both.
        const auto width = static_cast<std::size_t>(head.format.channel_count);
        const double oldest = static_cast<double>(lead_snap.head_index);
        bool hole = false;
        if (head.history.empty()) {
            hole = std::floor(head.position) < oldest;
        } else {
            hole = static_cast<double>(head.history_start + head.history.size() / width) <
                   oldest;
        }

        // And a stream that restarted its index under the lead, which puts
        // the position further ahead of the newest frame than the ring could
        // ever have held. See RingCounts::restarts.
        hole = hole || std::floor(head.position) >
                           static_cast<double>(lead_snap.next_index + lead_snap.capacity_frames);
        if (hole) {
            place(head, oldest, ratio);
            ++alignments_;
            drift_.rebase();
        }
    }

    std::size_t count = 0;
    {
        const double margin =
            copying(head, ratio) ? 0.0 : static_cast<double>(head.resampler.half_width());
        const double room = static_cast<double>(lead_snap.next_index) - 1.0 - margin -
                            head.position;
        if (room >= 0.0) {
            count = std::min(frames,
                             static_cast<std::size_t>(std::floor(room / step_of(head, ratio))) +
                                 1);
        }
    }

    const double lead_time = head.position / lead_rate;
    FrameSource lead_source = render(head, *rings[lead_slot], count, ratio);
    result.fill_seconds =
        (static_cast<double>(lead_snap.next_index) - head.position) / lead_rate;
    result.timed = now_ns != AudioRing::kNoArrival && lead_snap.has_anchor;
    result.lag_seconds =
        result.timed ? static_cast<double>(now_ns - lead_snap.anchor_ns) * 1e-9 -
                           head.position / lead_rate
                     : result.fill_seconds;

    // The lag and the fill differ by a constant, which is how far the
    // anchor's line runs ahead of the last chunk on average, so moving from
    // one to the other is a new level to hold.
    if (result.timed != last_timed_) {
        drift_.rebase();
        last_timed_ = result.timed;
    }
    const double elapsed = static_cast<double>(frames) / static_cast<double>(out_rate_);
    if (count < frames) {
        rings[lead_slot]->note_starved(frames - count);
        lead_source = FrameSource::starved;
        drift_.rebase();
    }
    drift_.observe(result.lag_seconds, elapsed);
    result.lead_source = lead_source;
    result.frames_played = count;

    auto mix_in = [&](Stream& stream, const MixSlot& slot) {
        const int channels = stream.format.channel_count;
        if (stream.level) {
            stream.agc.process(stream.rendered.data(), count, channels);
        }
        if (stream.multiplex) {
            stream.deemphasis.process(stream.rendered.data(), count, channels);
        }
        add_mapped(stream.rendered.data(), channels, sum_.data(), out_channels_, count,
                   slot.gain);
    };
    mix_in(head, control.strips[lead_slot]);

    // Every other stream, at the lead's instant.
    for (std::size_t i = 0; i < used; ++i) {
        if (!usable[i] || i == lead_slot) {
            continue;
        }
        Stream& stream = streams_[i];
        const double rate = static_cast<double>(stream.format.sample_rate);
        const bool anchored = snaps[i].has_anchor && lead_snap.has_anchor;
        const double target =
            anchored ? (lead_time + static_cast<double>(lead_snap.anchor_ns - snaps[i].anchor_ns) *
                                        1e-9) *
                           rate
                     : 0.0;
        if (!stream.placed) {
            place(stream, anchored ? target : static_cast<double>(snaps[i].head_index), ratio);
        } else if (anchored && std::abs(stream.position - target) > kAlignTolerance * rate) {
            place(stream, target, ratio);
            ++alignments_;
        }
        static_cast<void>(render(stream, *rings[i], count, ratio));
        mix_in(stream, control.strips[i]);
    }

    result.limited = limiter_.process(sum_.data(), frames, out_channels_);
    if (out != nullptr) {
        std::memcpy(out, sum_.data(), frames * out_width * sizeof(float));
    }
    return result;
}

}  // namespace revenant::ui
