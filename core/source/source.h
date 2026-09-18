// A source of timestamped IQ.
//
// One interface over a file, a synthesised scene and a radio, and the whole
// engine above it cannot tell which it has. That is not tidiness: the offline
// path is the regression harness, the decoder test bed and structurally half
// the time machine, and it only serves all three if it runs the identical
// graph rather than a parallel code path that drifts.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

#include "core/dsp/types.h"
#include "core/error.h"
#include "core/source/capabilities.h"

namespace revenant::source {

// One block of samples, borrowed for the duration of the sink call and not
// valid after it returns.
//
// The payload is bytes plus a format rather than a span of Complex32 because
// an RTL-SDR delivers unsigned 8-bit pairs. Converting on the host would put a
// pass over every sample back on the CPU and quadruple what crosses the bus,
// which is the exact cost the architecture exists to avoid. The conversion is
// a GPU kernel with a CPU twin, run as part of the upload.
struct SourceBlock {
    // Sample index from stream start, the wall-clock anchor, and the rate.
    // The index is the authority; wall clock is derived.
    dsp::BlockTimestamp stamp;

    SampleFormat format = SampleFormat::Cf32;
    std::size_t sample_count = 0;

    // sample_count * bytes_per_sample(format) bytes.
    std::span<const std::byte> bytes;

    // Samples lost between the previous block and this one, always zero on a
    // Demand source by construction.
    //
    // The gap is also visible in stamp.start, which is the authority. This
    // field exists so that no consumer has to tell a loss from a seek by
    // subtracting two indices and guessing which it was.
    std::uint64_t dropped_before = 0;

    // Monotone, counting blocks rather than samples. A consumer that sees a
    // sequence jump knows it missed a whole block delivery, which is a
    // different fault from samples being lost inside the device.
    std::uint64_t sequence = 0;
};

// Returning an error stops the stream, and the error is handed back through
// Source::stop(). This is the same shape as the BlockSink already in
// tools/siggen/wideband.h, deliberately: one idiom for streamed blocks.
using BlockSink = std::function<Status(const SourceBlock&)>;

struct StreamOptions {
    // 0 takes the source's preferred size.
    std::size_t block_samples = 0;

    // Demand sources only. Where to begin.
    dsp::SampleIndex start_index = 0;

    // Demand sources only. 0 is unthrottled, which is the default and what
    // every test and the milestone exit criterion use. 1.0 paces to realtime,
    // which a GUI replaying a capture wants and nothing else does.
    double pace = 0.0;
};

struct SourceStats {
    std::uint64_t blocks_delivered = 0;
    std::uint64_t samples_delivered = 0;

    // An overrun is a correctness event, so it is counted rather than logged.
    // A log line produces a recording that looks continuous and is not, with
    // nothing downstream able to tell.
    std::uint64_t overrun_events = 0;
    std::uint64_t samples_lost = 0;
    dsp::SampleIndex last_loss_index = 0;

    // Next index the source will produce.
    dsp::SampleIndex write_index = 0;
};

class Source {
public:
    virtual ~Source() = default;

    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
    Source(Source&&) = delete;
    Source& operator=(Source&&) = delete;

    [[nodiscard]] virtual const SourceCapabilities& capabilities() const = 0;

    // Every setter returns what was actually achieved, not void.
    //
    // A synthesiser lands on a nearby frequency, not the requested one, and a
    // caller has to be able to see where. This is a large part of why Hertz is
    // an integer: the difference between asked and got is exact, and a tuning
    // offset that shows up later can be traced to the stage that introduced
    // it instead of being attributed to drift.
    [[nodiscard]] virtual Expected<dsp::Hertz> tune(dsp::Hertz center) = 0;
    [[nodiscard]] virtual dsp::Hertz center() const = 0;

    [[nodiscard]] virtual Expected<dsp::SampleRate> set_sample_rate(dsp::SampleRate rate) = 0;
    [[nodiscard]] virtual dsp::SampleRate sample_rate() const = 0;

    [[nodiscard]] virtual Expected<double> set_gain(std::string_view stage, double db) = 0;
    [[nodiscard]] virtual Status set_gain_auto(std::string_view stage, bool on) = 0;

    // Spawns the source's own thread and calls sink on it.
    //
    // Its own thread and not one from the pool: a Paced source must be
    // serviced on the device's schedule and cannot wait behind DSP work, and a
    // Demand source blocks in its sink on purpose, which would deadlock a pool
    // that the blocking consumer also needs.
    [[nodiscard]] virtual Status start(const StreamOptions& options, BlockSink sink) = 0;

    // Blocks until the source thread has exited. Returns the error that ended
    // the stream, if a sink returned one.
    [[nodiscard]] virtual Status stop() = 0;

    [[nodiscard]] virtual bool running() const = 0;

    // Demand sources only. Fails on others with a message naming the
    // capability rather than the symptom.
    [[nodiscard]] virtual Status seek(dsp::SampleIndex index) = 0;

    [[nodiscard]] virtual SourceStats stats() const = 0;
    [[nodiscard]] virtual ClockQuality clock() const = 0;

protected:
    Source() = default;
};

// There is no close(). The destructor is the close.
//
// A source with an explicit close grows a half-open state, and then a rule
// about what is legal to call in it, and then a bug where something calls one
// of those things.

}  // namespace revenant::source
