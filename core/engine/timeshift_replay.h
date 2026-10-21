// A replay of the time-shift buffer: a second graph, fed from host memory at a
// chosen past instant instead of from the radio.
//
// WHY A SECOND GRAPH AND NOT A DELAYED RECEIVER ON THE LIVE ONE
//
// The live graph's receivers read the device ring, which holds a fraction of a
// second and is indexed by the live stream. A receiver two minutes behind it
// would need either a device ring two minutes deep, which is gigabytes of VRAM
// on a wide source, or a second path into the fine stage that every kernel
// would have to learn. A second graph needs neither: it is the same coarse
// chain and the same stages, built from the same grid, prototype and format,
// fed by a thread here instead of a source. Every demodulator, the passband
// and the full-span spectrum work on it unchanged, and the replay's spectrum
// is what the span view draws while the scrub bar is off live.
//
// The cost is a second channel ring and a second spectrum stage while a
// replay runs, and nothing while one does not: the graph exists only between
// start and stop.
//
// TIME
//
// The replay graph's stream index starts at zero and never jumps, because a
// graph refuses an index that moves backwards and a seek is exactly that. What
// moves instead is the offset between the replay's index and the live
// stream's, which is how a seek lands without rebuilding anything: receivers
// open on the replay keep running and simply hear the new instant.
//
// The pace is a multiple of real time, one or more. At one a replay is a
// fixed delay behind live; above one it closes on live and, once it arrives,
// runs at live's own pace a block behind, because there is nothing newer to
// read.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/dsp/pfb.h"
#include "core/dsp/types.h"
#include "core/engine/engine.h"
#include "core/engine/graph.h"
#include "core/engine/timeshift.h"
#include "core/error.h"
#include "core/gpu/context.h"

namespace revenant::engine {

struct TimeShiftReplayConfig {
    // The live graph's, with flow forced to Demand: the replay thread is the
    // producer, and blocking it is the backpressure a Paced flow would have
    // turned into dropped samples.
    GraphConfig graph;
    dsp::PrototypeFilter prototype;
    std::vector<dsp::Complex32> twiddles;

    // The absolute live index to start from, clamped into the buffer.
    dsp::SampleIndex start_index = 0;
    double speed = 1.0;
};

// The fastest a replay runs. Past this the GPU is the limit anyway, and a
// pacer that never sleeps starves the live graph of the queue.
inline constexpr double kMaxTimeShiftSpeed = 16.0;

class TimeShiftReplay {
public:
    [[nodiscard]] static Expected<std::unique_ptr<TimeShiftReplay>> create(
        const gpu::Context& context, const TimeShiftBuffer& buffer,
        const TimeShiftReplayConfig& config);

    ~TimeShiftReplay();
    TimeShiftReplay(const TimeShiftReplay&) = delete;
    TimeShiftReplay& operator=(const TimeShiftReplay&) = delete;

    [[nodiscard]] Graph& graph() { return *graph_; }
    [[nodiscard]] const Graph& graph() const { return *graph_; }

    // Wraps the sink so every frame says it is a replay and carries the wall
    // clock of its first sample. Control plane.
    [[nodiscard]] Status set_spectrum_sink(SpectrumSink sink);

    // Moves the read position and the pace. Taken by the replay thread before
    // its next block. Any thread.
    void seek(dsp::SampleIndex live_index, double speed);

    // The live index of the next sample the replay will feed.
    [[nodiscard]] dsp::SampleIndex position() const;
    [[nodiscard]] double speed() const;

    // The live index a replay-stream index was read from. Approximate across
    // a seek, by the block in flight.
    [[nodiscard]] dsp::SampleIndex live_index_of(dsp::SampleIndex replay_index) const;

    // Joins the replay thread. Idempotent; the destructor calls it.
    void stop();

private:
    TimeShiftReplay(const TimeShiftBuffer& buffer);
    void run();

    const TimeShiftBuffer& buffer_;
    std::unique_ptr<Scheduler> scheduler_;
    std::unique_ptr<DeviceRing> ring_;
    std::unique_ptr<Graph> graph_;

    std::size_t block_samples_ = 0;
    source::SampleFormat format_ = source::SampleFormat::Cf32;
    dsp::SampleRate rate_ = 0;

    std::atomic<dsp::SampleIndex> cursor_{0};
    std::atomic<dsp::SampleIndex> replay_index_{0};
    std::atomic<double> speed_{1.0};
    std::atomic<bool> seek_pending_{false};
    std::atomic<dsp::SampleIndex> seek_to_{0};
    std::atomic<bool> stopping_{false};

    std::thread thread_;
};

}  // namespace revenant::engine
