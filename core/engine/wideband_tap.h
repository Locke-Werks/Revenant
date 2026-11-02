// The wideband tap: the source's own stream, on the host, for a decoder whose
// signal is wider than any receiver delivers.
//
// WHY THIS EXISTS. Every receiver reads one coarse channel of the polyphase
// channelizer, and a channel is roughly the source rate over M times two: 75
// kS/s at M = 64 on a 2.4 MS/s source. A decoder's raw tap is further capped
// at core/rpc/decoders.h's kRawTapRateCap, 192000 S/s. DAB at 1.536 MHz, GNSS
// C/A at 1.023 Mchip/s, HRPT at 665.4 kbit/s and DVB-S do not fit in either,
// and no amount of widening one channel fixes that: the channelizer is the
// wrong place to get them from. They need the samples before the channelizer.
//
// WHERE IT READS. The engine already hands every SourceBlock to two host
// consumers before and after the graph, the capture writer and the time-shift
// buffer, both on the source's delivery thread. The tap is a third on the same
// chain. It does on that thread exactly what the writer does: copies the
// packed bytes into a ring and returns. The bytes stay in the source's native
// format, so an RTL-SDR's cu8 crosses as two bytes a sample and is widened
// on the tap's own thread; core/source/source.h has why converting on the
// delivery thread is the cost the architecture exists to avoid.
//
// WHAT IT DELIVERS. On its own worker thread: the samples widened to float
// with core/dsp/convert.h's scalar twins (the same numbers the GPU kernels
// produce), mixed by -offset so the requested frequency sits at DC, lowpass
// filtered and decimated by an integer to the requested rate, and handed to a
// sink as interleaved I/Q. A decimation of one skips the filter and the mix
// alone runs; an offset of zero skips the mix.
//
// WHAT A SLOW CONSUMER COSTS. Itself, and only itself. The producer side is
// write_or_drop: a full ring loses the newest bytes and counts them, the
// next chunk carries dropped_before, and nothing upstream waits. The radio
// never drops a sample because a wideband decoder fell behind.
//
// The filter is the one core/dsp/vrx_reference.h already designs for audio
// decimation, a Kaiser-windowed sinc normalised to unit gain at DC: Oppenheim
// and Schafer section 7.5.3, and Crochiere and Rabiner chapter 2 for the
// integer decimator and its alias bound.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "core/dsp/types.h"
#include "core/engine/spsc_ring.h"
#include "core/error.h"
#include "core/source/source.h"

namespace revenant::engine {

using WidebandTapId = std::uint64_t;

struct WidebandTapConfig {
    // Where the wanted signal sits, relative to the source's tuned centre.
    // Mixed to DC before decimation.
    dsp::Hertz offset = 0;

    // The rate delivered. Must divide the source rate exactly; the integer
    // decimator has no fractional phase on purpose (see the header). Zero
    // takes the source rate.
    dsp::SampleRate output_rate = 0;

    // How much of the stream the ring holds before it starts dropping, in
    // seconds at the source rate.
    double ring_seconds = 0.5;
};

struct WidebandChunk {
    // Index of the first output frame, in output samples from the tap's
    // start. Advances by frames() per chunk plus any loss.
    dsp::SampleIndex start = 0;
    dsp::SampleRate rate = 0;

    // Absolute frequency at DC in this chunk: the source centre at the time
    // plus the tap's offset. Zero when the source block said nothing about
    // its centre and none was known.
    dsp::Hertz center = 0;

    // Interleaved I then Q.
    std::span<const float> iq;

    // Output frames lost immediately before this chunk because the ring was
    // full. start already accounts for them.
    std::uint64_t dropped_before = 0;

    [[nodiscard]] std::uint64_t frames() const { return iq.size() / 2; }
};

using WidebandSink = std::function<Status(const WidebandChunk&)>;

class WidebandTap {
public:
    // Validates the rates and designs the filter. The worker starts here.
    [[nodiscard]] static Expected<std::unique_ptr<WidebandTap>> create(
        const WidebandTapConfig& config, dsp::SampleRate source_rate,
        source::SampleFormat format, WidebandSink sink);

    ~WidebandTap();
    WidebandTap(const WidebandTap&) = delete;
    WidebandTap& operator=(const WidebandTap&) = delete;
    WidebandTap(WidebandTap&&) = delete;
    WidebandTap& operator=(WidebandTap&&) = delete;

    // The delivery thread. Never blocks, never fails the stream. A block in a
    // format other than the one the tap was built for stops this tap and is
    // reported by error().
    void push(const source::SourceBlock& block, dsp::Hertz center) noexcept;

    // Joins the worker. Idempotent. Returns the first error the sink or the
    // tap itself raised, if any.
    Status stop();

    [[nodiscard]] dsp::SampleRate output_rate() const { return output_rate_; }
    [[nodiscard]] std::uint32_t decimation() const { return decimation_; }
    [[nodiscard]] std::size_t filter_taps() const { return taps_.size(); }
    [[nodiscard]] std::uint64_t dropped_source_samples() const {
        return dropped_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool failed() const { return failed_.load(std::memory_order_acquire); }

private:
    WidebandTap() = default;
    void run();
    void process(std::span<const std::byte> packed, std::size_t samples);

    WidebandTapConfig config_{};
    dsp::SampleRate source_rate_ = 0;
    dsp::SampleRate output_rate_ = 0;
    std::uint32_t decimation_ = 1;
    source::SampleFormat format_ = source::SampleFormat::Cf32;
    std::size_t bytes_per_sample_ = 0;
    WidebandSink sink_;

    std::unique_ptr<SpscRing<std::byte>> ring_;
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> pending_drop_{0};
    std::atomic<dsp::Hertz> center_{0};

    // Worker state.
    std::vector<float> taps_;
    std::vector<float> history_;  // 2 * taps_.size(), interleaved, circular
    std::size_t history_pos_ = 0;
    std::uint32_t phase_ = 0;  // input samples since the last output
    double nco_re_ = 1.0;
    double nco_im_ = 0.0;
    double step_re_ = 1.0;
    double step_im_ = 0.0;
    std::uint32_t since_renorm_ = 0;
    dsp::SampleIndex out_index_ = 0;
    std::vector<std::byte> scratch_;
    std::vector<float> out_;

    std::thread worker_;
    std::mutex wake_lock_;
    std::condition_variable wake_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> failed_{false};
    Status error_;
};

}  // namespace revenant::engine
