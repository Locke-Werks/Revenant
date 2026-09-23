// The probe pool: receivers the engine places on detections for itself, to
// hand core/characterise an extract sized to the signal.
//
// WHAT docs/detection.md SETTLED AND THIS FILE DOES NOT REOPEN
//
// Tier two needs complex baseband mixed to DC and filtered near the signal,
// because the raw tap is one whole coarse channel with the carrier wherever
// the residual left it, which on the shipped VHF grid puts a 2.8 kHz signal
// 20 dB down in its own extract. The fine stage does that, and a probe is
// Demod::Raw built through it: VrxStageRequest::fine_stage_complex_tap in
// core/engine/graph.h is the factory decision, and the kernel path it reaches
// is the Raw passthrough tests/reference/test_vrx.cpp already holds bit-exact
// against its twin. No kernel was added for this.
//
// Nothing a probe learns goes on the wire as a family, and a probe itself is
// not a receiver anybody else can see: the graph leaves VrxRole::Probe out of
// vrx_ids, and Engine refuses a probe's id on every public receiver method.
//
// THE NUMBERS, EACH OF WHICH IS A CHOICE MADE HERE
//
//   Pool size. EngineConfig::probe_receivers, four in revenant-cli. A probe
//   is a real receiver with a fine filter and a passthrough on every block
//   whether or not it is capturing, so the pool is a standing GPU cost and
//   its size is what bounds it. docs/detection.md has the measured cost.
//
//   Rate buckets. 1500 S/s times a power of two, 1500 up to 192000. A probe
//   runs at the smallest bucket at least four times the detection's occupied
//   bandwidth AND at least kProbeFloorRate, and never above the grid's channel
//   rate. Four times, so the signal fills at most half the passband and the
//   characteriser's occupied-band estimate has floor on both sides to read.
//
//   Passband. Half the bucket, centred, so the half-width is a quarter of the
//   rate. That is the widest passband whose demodulation rate is exactly the
//   bucket: dsp::minimum_demod_rate's shape floor for a width W is 1.5 W,
//   which is 0.75 of the rate and rounds up to it. It is also what makes a
//   retune within a bucket always legal. place() clamps a passband only past
//   (channel_rate - 2|residual|)/2 either side, the residual never exceeds a
//   quarter of the channel rate on a 2x oversampled grid, so a half-width of a
//   quarter of any bucket at or under the channel rate is never clamped, the
//   tap count never moves, and dsp::VrxShape stays the same wherever on the
//   grid the probe is sent. docs/detection.md's "retune() does not refuse on
//   bandwidth; what has to match is the demodulation-rate bucket" is exactly
//   this, and kMaxFineTaps capping the narrow and medium shapes is why no
//   other dimension was worth a bucket of its own.
//
//   Dwell. kProbeDwellSeconds of baseband, and never fewer than
//   characterise::kMinCharacteriseSamples. kProbeFloorRate is the lowest
//   bucket that holds that many in the stated dwell, so on a grid whose
//   channel rate allows it every probe is exactly the stated dwell. A grid too
//   fine to carry the floor, the 3000 S/s channels revenant-cli pins at 96 kS/s
//   of HF, stretches the dwell to reach the sample floor instead and says so on
//   the outcome.
//
// THREADING
//
// submit() and take() are one producer and one consumer, each through a
// lock-free ring, so the caller may be the engine's completion thread: that is
// where revenant-cli runs the detector. A probe's audio sink runs on the
// completion thread too, and it takes no lock either. It claims its capture
// buffer with a compare-and-swap and hands it back the same way.
//
// Everything slow is on the pool's own worker thread: placing and retuning the
// receivers, which take the graph's control lock, and characterise(), which
// costs milliseconds to tens of milliseconds of CPU per extract and must not
// run where GPU readbacks are retired.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "core/characterise/catalogue.h"
#include "core/characterise/characterise.h"
#include "core/dsp/pfb.h"
#include "core/dsp/types.h"
#include "core/engine/vrx.h"
#include "core/error.h"

namespace revenant::engine {

class Graph;

// The buckets, lowest first. See the header comment for why these.
inline constexpr std::array<dsp::SampleRate, 8> kProbeRates = {
    1'500, 3'000, 6'000, 12'000, 24'000, 48'000, 96'000, 192'000};

// Baseband collected per probe, when the bucket allows it.
inline constexpr double kProbeDwellSeconds = 2.0;

// The lowest bucket whose stated dwell holds the characteriser's sample floor:
// 16384 samples in two seconds is 8192 S/s, and the bucket above that is
// 12000.
inline constexpr dsp::SampleRate kProbeFloorRate = 12'000;

// A signal may occupy at most this fraction of its probe's rate.
inline constexpr dsp::SampleRate kProbeRateOverOccupied = 4;

// Output samples discarded after a probe is placed or retuned, before the
// capture starts. The fine filter is at most dsp::kMaxFineTaps channel
// samples long, which is at most that many output samples at any bucket at or
// under the channel rate, so this clears its history of whatever it was
// pointed at before.
inline constexpr std::uint32_t kProbeSettleSamples = 256;

// Receivers a pool may hold, whatever EngineConfig asks for.
inline constexpr std::uint32_t kMaxProbeReceivers = 16;

static_assert(kProbeFloorRate * kProbeDwellSeconds >=
                  static_cast<double>(characterise::kMinCharacteriseSamples),
              "the floor bucket must hold the characteriser's sample floor in one dwell");

// What a probe will be built as for a detection this wide on this grid.
struct ProbeShape {
    dsp::SampleRate rate = 0;

    // The passband, as VrxParams::bandwidth: half the rate, centred.
    dsp::Hertz bandwidth = 0;

    // Samples collected, and the seconds that is. Longer than
    // kProbeDwellSeconds only when the grid's channel rate is under
    // kProbeFloorRate.
    std::uint32_t samples = 0;
    double seconds = 0.0;
};

// The bucket and dwell for a detection, or a refusal naming why none fits.
// Pure: the occupied bandwidth and the grid's channel rate and nothing else.
[[nodiscard]] Expected<ProbeShape> probe_shape(dsp::Hertz occupied_hz,
                                               dsp::SampleRate channel_rate);

// One thing to probe. No default member initialisers, because it crosses a
// SpscRing, which zero-fills its storage rather than constructing it.
struct ProbeRequest {
    // The caller's own key, handed back unchanged on the outcome.
    // detect::TierTwo passes a track id.
    std::uint64_t tag;

    // Hertz from the source's baseband DC, as VrxParams::center: an absolute
    // frequency less EngineInfo::source_center.
    dsp::Hertz center;

    // The detection's occupied bandwidth, which picks the bucket.
    dsp::Hertz occupied_hz;
};

enum class ProbeStatus : std::uint8_t {
    // characterise() ran on a full dwell. The family can still be Unknown,
    // which is a real answer and the one noise is supposed to get.
    Characterised = 0,

    // Wider than a quarter of the largest bucket the grid's channel rate
    // carries. Nothing was placed.
    TooWide,

    // engine::place refused the centre, which is outside the span.
    Unplaced,

    // A source retune or a close ended it before the dwell was full. The
    // extract would have straddled two front-end centres.
    Cancelled,

    // The graph refused the receiver, or characterise() returned an error
    // rather than a characterisation. Counted, and the pool carries on.
    Failed,
};

[[nodiscard]] const char* probe_status_name(ProbeStatus status);

// What one probe found. Plain data for the same reason ProbeRequest is.
struct ProbeOutcome {
    std::uint64_t tag;
    ProbeStatus status;

    // characterise::Characterisation, flattened to what a track can carry.
    characterise::ModulationFamily family;
    double confidence;

    // Zero when the family's own detector found none, which is the
    // characteriser's rule and not this file's: Unmodulated and AnalogueFm
    // never carry one, and a PSK call without one is flagged below.
    double symbol_rate_hz;
    std::uint32_t order;
    std::uint32_t tone_count;
    double concentration;

    // characterise::may_drive_detection on the full result, and the flag it
    // most often refuses on. A result it refuses is still a result, and
    // detect::Detector::record_probe keeps it without letting it change the
    // family the detector reports.
    bool may_drive_detection;
    bool psk_without_symbol_rate;

    // What was asked and what was built for it.
    dsp::Hertz center;
    dsp::Hertz occupied_hz;
    dsp::SampleRate rate;
    std::uint32_t samples;

    // The receiver's output-stream index of the first captured sample, at
    // `rate`. Not a source index: AudioChunk::start counts this receiver's
    // own frames.
    dsp::SampleIndex first_sample;

    // Which receiver ran it, and whether it had to be built for this probe
    // rather than retuned to it.
    std::uint32_t slot;
    bool built;

    // Host time characterise() took, on the pool's worker.
    double characterise_ms;
};

struct ProbeStats {
    std::uint64_t submitted = 0;

    // Outcomes by status, which sum to what has been taken plus what is
    // waiting to be.
    std::uint64_t characterised = 0;
    std::uint64_t too_wide = 0;
    std::uint64_t unplaced = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t failed = 0;

    // Submissions the request ring had no room for, and outcomes the result
    // ring had no room for. Both are the caller submitting faster than it
    // takes, which detect::TierTwo does not do.
    std::uint64_t refused = 0;
    std::uint64_t outcomes_dropped = 0;

    // Receivers built, and probes that reused one in place. The pool's
    // reason to exist is the second number growing faster than the first.
    std::uint64_t builds = 0;
    std::uint64_t retunes = 0;

    // Receivers the pool holds now, and how many of them are collecting.
    std::uint32_t receivers = 0;
    std::uint32_t busy = 0;
    std::uint32_t size = 0;

    double characterise_ms_total = 0.0;
};

struct ProbePoolConfig {
    // Receivers, at most kMaxProbeReceivers.
    std::uint32_t size = 4;

    dsp::GridParams grid{};
    dsp::SampleRate source_rate = 0;
    dsp::SampleRate channel_rate = 0;

    // The first probe receiver's id. Ids count up from here and are never
    // reused, so a stale one names nothing. Engine puts them above every id
    // add_vrx can issue.
    std::uint32_t first_id = 0x8000'0000U;
};

// Owned by the engine, created with the graph and destroyed before it.
class ProbePool {
public:
    [[nodiscard]] static Expected<std::unique_ptr<ProbePool>> create(Graph& graph,
                                                                     const ProbePoolConfig& config);

    // Stops the worker and removes every probe receiver from the graph.
    ~ProbePool();

    ProbePool(const ProbePool&) = delete;
    ProbePool& operator=(const ProbePool&) = delete;
    ProbePool(ProbePool&&) = delete;
    ProbePool& operator=(ProbePool&&) = delete;

    // One producer thread. Refused when the request ring is full.
    [[nodiscard]] Status submit(const ProbeRequest& request);

    // One consumer thread. Copies out what has finished, oldest first.
    [[nodiscard]] std::size_t take(std::span<ProbeOutcome> out);

    // Control plane. Ends every probe that is collecting and every one still
    // queued, each with a Cancelled outcome. Called when the front end
    // moves, because an extract that straddles a retune is two signals.
    void cancel_all();

    [[nodiscard]] ProbeStats stats() const;

    // The id space, so Engine can refuse a probe's id on a public method.
    [[nodiscard]] bool owns(VrxId id) const;

private:
    ProbePool() = default;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace revenant::engine
