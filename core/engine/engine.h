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
//
// THE SPECTRUM SURFACE IS A DELIBERATE ADDITION TO A FILE TREATED AS FROZEN
//
// Everything above was written before there was any way to see a signal that
// no receiver was tuned to, and docs/detection.md records the consequence:
// the waterfall, the detector and the fine-tuning display are all downstream
// of a full-span spectrum, and there was no method here that returned one.
//
// It is shaped exactly like the audio sink, and that is the argument for it
// rather than for a polling method. A frame is produced by the device once
// per block whether anyone is looking or not, it is already coming back
// through a readback buffer on the completion thread, and a poll would either
// hand back the frame that happened to be latest, which drops most of them
// and tears the one it returns, or grow a queue this layer would then own. A
// callback delivers each frame once, on the thread that read it, with its
// buffer valid for the call and not after.
//
// A spectrum frame is metadata rather than samples, and it is not cheap. An
// earlier version of this comment called it "a fraction of a percent of what
// the stream itself would cost to bring home", which is wrong by about two
// orders of magnitude and was the only argument offered for letting it cross
// a bus this file declares closed.
//
// Measured at the defaults above. M*N/2 at 64 channels and a 2048-point
// transform is 65536 bins of four bytes, so 256 KiB per frame, one frame per
// block. At 20 MS/s a block is 65536 samples, so that is 305 frames a second
// and 80 MB/s coming back, against the 160 MB/s the cf32 stream costs going
// out. Half, not a fraction of a percent.
//
// It is still the right trade and the reason is different from the one that
// was written down. The frame is a REDUCTION: 80 MB/s buys the display and
// the detector everything they need about the whole span, where bringing the
// span itself home would cost twice that and leave both of them to transform
// it on the host. What makes it affordable is that it is bounded by the bin
// count rather than by the sample rate, so it does not grow when the radio
// gets wider. A caller budgeting PCIe or callback latency should use the
// number above and not the adjective. Nothing here returns complex baseband.

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

    // How fast a Demand source is asked to deliver, as a multiple of
    // realtime. Zero, the default, is unthrottled, and unthrottled is still
    // what happens when nothing is holding a stopwatch.
    //
    // This is not a throttle on the engine and there is still no code path
    // for faster than realtime. It is the source being asked to deliver on a
    // clock, which is the one thing a live monitor needs and a radio supplies
    // for free: a capture replayed as fast as the GPU retires it produces
    // audio a loudspeaker cannot accept, and what the listener hears is the
    // monitor's backlog being trimmed rather than the recording. A Paced
    // source ignores this, because its own hardware already sets the rate.
    double pace = 0.0;

    // Points in the full-span spectrum's per-channel transform. Zero, the
    // default, builds no spectrum stage at all.
    //
    // Off by default because it is not free: a pipeline, a twiddle circle, a
    // window, and one readback region per frame in flight, for a frame nobody
    // has asked to see. A headless recording pays none of it.
    //
    // The stage is not one transform across the whole span. The span is
    // covered by transforming each coarse channel the channelizer has already
    // produced and keeping the central half of each one, which is what fits
    // the shared-memory budget docs/fft.md scopes the FFT kernel to. The
    // number here is therefore per channel, so a frame is
    // grid.channels * spectrum_transform / 2 bins wide. It is clamped to what
    // this device's shared memory holds, and EngineInfo::spectrum reports
    // what was settled on.
    std::uint32_t spectrum_transform = 0;
};

// The frequency axis of a spectrum frame, and how wide one is.
//
// Both frequencies are exact rationals rather than integer hertz, for the
// reason dsp::ChannelCentre gives: a bin is source_rate / (D * N) hertz wide
// and a coarse channel centre is k * source_rate / M, and neither is a whole
// number of hertz in general. Rounding either here would put an unsourceable
// offset into every frequency a person clicks on, which is what the
// integer-hertz convention exists to prevent. A display converts once, at the
// point it draws a label.
//
// Both are relative to the source's baseband DC, the same frame VrxParams is
// in. Add EngineInfo::source_center for absolute radio frequency.
struct SpectrumGeometry {
    // Points in one coarse channel's transform, and the central half of that
    // which is kept.
    std::uint32_t transform = 0;
    std::uint32_t bins_per_channel = 0;

    // Coarse channels the frame tiles, and the total bins across the span.
    std::uint32_t channels = 0;
    std::uint32_t bins = 0;

    std::int64_t bin_width_numerator = 0;
    std::int64_t bin_width_denominator = 1;
    std::int64_t bin_zero_numerator = 0;
    std::int64_t bin_zero_denominator = 1;

    [[nodiscard]] constexpr bool enabled() const { return bins != 0; }

    [[nodiscard]] constexpr double bin_width_hz() const {
        return bin_width_denominator == 0 ? 0.0
                                          : static_cast<double>(bin_width_numerator) /
                                                static_cast<double>(bin_width_denominator);
    }

    // Centre frequency of bin zero.
    //
    // Half a coarse channel below the negative edge of the band, because the
    // lowest channel is centred on the Nyquist frequency and owns the half
    // channel either side of it. The part below -rate/2 is the same physical
    // spectrum as the part below +rate/2, since a sampled band is periodic,
    // and on any real receiver both are the front end's roll-off. A display
    // labels it as written and the axis stays continuous.
    [[nodiscard]] constexpr double bin_zero_hz() const {
        return bin_zero_denominator == 0 ? 0.0
                                         : static_cast<double>(bin_zero_numerator) /
                                               static_cast<double>(bin_zero_denominator);
    }
};

// What the engine settled on, which is frequently not what was asked for.
struct EngineInfo {
    gpu::DeviceInfo device;
    RingGeometry ring;
    dsp::GridParams grid;
    dsp::SampleRate source_rate = 0;
    dsp::SampleRate channel_rate = 0;
    dsp::Hertz channel_spacing = 0;

    // Left empty when EngineConfig::spectrum_transform was zero.
    SpectrumGeometry spectrum{};

    // What the source's baseband DC corresponds to in real radio frequency,
    // read once when the source was opened.
    //
    // Every frequency in VrxParams is an offset from baseband DC, because
    // that is the only frame the grid has. A person tunes in absolute hertz,
    // so something has to hold the constant that relates the two, and a
    // caller that had to parse it back out of the source URI would be
    // reimplementing each backend's grammar to do it. Zero for a source whose
    // baseband is not a translation of anything, which is what a synthetic
    // scene with no declared centre is.
    dsp::Hertz source_center = 0;
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

// One full-span spectrum frame, handed to the caller on the host.
//
// One frame per dispatch, which is one per source block, so the frame rate is
// the source rate over EngineConfig::block_samples and is not something a
// consumer chooses. A consumer that wants fewer decimates in time; a consumer
// that wants more asks for smaller blocks, which costs submissions.
struct SpectrumFrame {
    // Power per bin in decibels relative to full scale, ascending in
    // frequency across the whole span with no gaps and nothing counted twice.
    //
    // Valid for the duration of the call and not after. A consumer that keeps
    // it copies it, the same as AudioChunk::samples.
    std::span<const float> power_db;

    SpectrumGeometry geometry;

    // The source samples this frame's window covers, as a half-open range
    // [start, start + count). Absolute from the start of the stream, so it
    // lines up with an AudioChunk's start and with anything else indexed the
    // way docs/conventions.md says time is indexed. The window is N coarse
    // channel samples, so it spans N * grid.decimation source samples, and
    // the filter's group delay has already been taken off.
    dsp::SampleIndex start = 0;
    dsp::SampleIndex count = 0;

    // Frames delivered before this one. A waterfall that skipped a row knows
    // it skipped a row, rather than drawing a gap as though the band went
    // quiet.
    std::uint64_t sequence = 0;
};

using SpectrumSink = std::function<Status(const SpectrumFrame&)>;

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

    // Routes the full-span spectrum to a callback. One sink for the engine,
    // because there is one span; setting a second replaces the first and an
    // empty one stops delivery.
    //
    // Refused when EngineConfig::spectrum_transform was zero, because the
    // stage is built with the rest of the coarse chain when the source is
    // opened and adding one later would mean rebuilding it, which is the one
    // thing the graph promises never to do while a stream is running.
    [[nodiscard]] virtual Status set_spectrum_sink(SpectrumSink sink) = 0;

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
