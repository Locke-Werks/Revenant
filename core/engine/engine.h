// The engine: the whole chain from a source to audio, and the only thing that
// knows the order the stages run in.
//
// It is headless and has no Qt anywhere. The GUI is one client of the RPC
// surface above this, a script is another, and a remote instance is a third.
// That falls out for free only if it is true from the start, which is why the
// engine is built and tested with no client at all.
//
// The one promise everything else rests on: samples cross the bus once, into
// the device ring, and nothing returns to host memory except audio PCM,
// decoded symbols and detection metadata. Any method here that would break
// that does not exist.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "core/dsp/pfb.h"
#include "core/dsp/types.h"
#include "core/engine/device_ring.h"
#include "core/engine/vrx.h"
#include "core/error.h"
#include "core/gpu/context.h"
#include "core/source/registry.h"
#include "core/source/source.h"

namespace revenant::engine {

struct EngineConfig {
    // -1 chooses, or REVENANT_GPU_INDEX decides. The conformance matrix passes
    // an explicit index.
    int gpu_index = -1;

    // How much capture history the device ring holds. Shrunk to fit the
    // device, and what was achieved is reported rather than assumed.
    double ring_seconds = 8.0;

    // Coarse grid. channels must be a power of two and decimation must divide
    // it; the project uses 2x oversampling, so decimation is channels/2.
    std::uint32_t channels = 64;
    std::uint32_t taps_per_branch = 17;

    // Audio rate every receiver resamples to unless it asks for another.
    dsp::SampleRate audio_rate = 48'000;

    // Samples the upload stages at a time. Larger is fewer submissions and
    // more latency.
    std::size_t block_samples = 65'536;
};

// What the engine settled on, which is frequently not what was asked for.
struct EngineInfo {
    gpu::DeviceInfo device;
    RingGeometry ring;
    dsp::GridParams grid;
    dsp::SampleRate source_rate = 0;
    dsp::SampleRate channel_rate = 0;
    dsp::Hertz channel_spacing = 0;
};

// One receiver's audio, handed to the caller on the host.
//
// This and the spectrum frame are the only things that come back across the
// bus. Everything else the engine computes stays on the device.
struct AudioChunk {
    VrxId vrx;
    dsp::SampleIndex start = 0;
    dsp::SampleRate rate = 0;

    // Interleaved if stereo, mono otherwise. Real, not complex: a demodulator
    // that hands out complex baseband is the raw tap, and that goes to a
    // decoder rather than here.
    std::span<const float> samples;
    std::uint32_t channels = 1;
};

using AudioSink = std::function<Status(const AudioChunk&)>;

class Engine {
public:
    [[nodiscard]] static Expected<std::unique_ptr<Engine>> create(const EngineConfig& config);

    virtual ~Engine() = default;

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Opens a source by URI and sizes the grid and the ring against it. The
    // engine takes ownership: a source outliving the graph that reads it is a
    // use-after-free waiting for a scheduling accident.
    [[nodiscard]] virtual Status open_source(std::string_view uri) = 0;
    [[nodiscard]] virtual const source::SourceCapabilities& source_capabilities() const = 0;

    [[nodiscard]] virtual const EngineInfo& info() const = 0;

    // Adding a receiver must not rebuild the coarse stage, and nothing in
    // VrxParams appears in GridParams, so it cannot.
    [[nodiscard]] virtual Expected<VrxId> add_vrx(const VrxParams& params) = 0;
    [[nodiscard]] virtual Status remove_vrx(VrxId id) = 0;
    [[nodiscard]] virtual Status set_vrx_params(VrxId id, const VrxParams& params) = 0;
    [[nodiscard]] virtual Expected<VrxStatus> vrx_status(VrxId id) const = 0;
    [[nodiscard]] virtual std::vector<VrxId> vrx_ids() const = 0;

    // Routes a receiver's audio to a callback. One sink per receiver; setting
    // a second replaces the first.
    [[nodiscard]] virtual Status set_audio_sink(VrxId id, AudioSink sink) = 0;

    // Runs the graph until the source ends or stop() is called. The source's
    // own thread drives it; this returns once the stream is finished and
    // hands back whatever ended it.
    [[nodiscard]] virtual Status run() = 0;
    [[nodiscard]] virtual Status stop() = 0;
    [[nodiscard]] virtual bool running() const = 0;

    // Counters, not logs. An overrun is a correctness event: a recording that
    // looks continuous and is not, with nothing downstream able to tell, is
    // the failure mode a log line produces.
    [[nodiscard]] virtual source::SourceStats source_stats() const = 0;

protected:
    Engine() = default;
};

}  // namespace revenant::engine
