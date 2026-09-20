// The wideband scene synthesizer: many emitters, a noise floor, one stream.
//
// This exists because the hardware does not. The only radio on the desk is an
// RTL-SDR v3, which sustains about 2.4 MS/s of 8-bit IQ, roughly 4.8 MB/s. The
// architecture targets around 20 MHz of instantaneous bandwidth, a polyphase
// channelizer feeding two hundred receivers, and 40 MB/s of continuous
// capture. None of that can be exercised against the radio, so the channelizer
// and the capture writer are developed against this instead. It has to produce
// an arbitrarily wide, densely populated spectrum at an arbitrary rate, faster
// than realtime, for as long as anyone cares to run it.
//
// Three properties make that possible, and all three come from the modulators
// being pure functions of the absolute sample index:
//
//   Streaming. render() takes an absolute range. Nothing is carried between
//   calls, so an hour of 20 MS/s is produced a block at a time and never held
//   in memory. Rendering [0, N) in one call and in N/B calls of B samples
//   gives bit-identical output.
//
//   Seeking. A consumer can jump to the middle of a four-hour scene and get
//   exactly the samples it would have got by generating the first three hours
//   and throwing them away.
//
//   Threading. The output range partitions into disjoint absolute ranges, so
//   workers need no coordination and the result does not depend on how many
//   of them there were.
//
// The ground truth record is the other half of the job. Every emitter reports
// where it was, how wide, when it started and stopped, what it was and at what
// level. M7 scores a burst detector and a classifier against exactly this, so
// the record is designed now, while there is still time to get it right,
// rather than reverse engineered later from whatever the generator happened to
// keep.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"
#include "core/dsp/synth/modulators.h"
#include "core/dsp/synth/wfm_mod.h"

namespace revenant::siggen {

// An emitter that never stops. Scene::create clamps this to the scene duration
// when the scene has one, so it only survives into a truth record for an
// unbounded scene.
inline constexpr SampleIndex kAlwaysOn = std::numeric_limits<SampleIndex>::max();

// What one transmission was, for scoring whatever claims to have found it.
//
// No field here is called snr_db. channel.h states the reason at length and it
// holds just as much here: a signal to noise ratio with no bandwidth attached
// is three different numbers, and at 20 MS/s the gap between the full-band
// figure and the in-band one runs to 40 dB.
// Which generator produced a transmission.
//
// Two, because a broadcast FM station is not a Modulation. core/dsp/synth/
// wfm_mod.h says at length why it is not, and the consequence lands here:
// EmitterTruth carries one field that is only readable for one of the two
// kinds, so the kind has to be readable first.
enum class EmitterKind : std::uint8_t {
    // core/dsp/synth/modulators.h's Modulator, in one of the eight modes.
    // EmitterTruth::modulation names it.
    Modulated = 0,

    // core/dsp/synth/wfm_mod.h's WfmModulator: a broadcast FM station,
    // composite, pilot, RDS and all.
    BroadcastFm,
};

[[nodiscard]] std::string_view emitter_kind_name(EmitterKind kind);

struct EmitterTruth {
    std::uint32_t id = 0;

    EmitterKind kind = EmitterKind::Modulated;

    // READ kind FIRST. This field is meaningless on a BroadcastFm row and is
    // left at its default there, which reads as Cw. There is no Modulation
    // member for a broadcast station and adding one would put an RDS payload
    // into every emitter a random population draws, so the alternatives were
    // a field that lies on one kind of row or an enum that lies about the
    // generator; this is the first, made visible.
    //
    // truth_csv() leaves the modulation cell EMPTY on such a row rather than
    // printing "cw", so a scorer reading the file cannot make the mistake
    // this comment is warning an in-memory reader about.
    Modulation modulation = Modulation::Cw;

    // Where the carrier sits relative to the scene centre. For the
    // suppressed-carrier modes this is not where the energy is, which is why
    // extent is carried separately rather than derived from it.
    Hertz carrier_offset_hz = 0;

    // The occupied band, as edges. A detector reporting the centre of the
    // energy in a USB signal is not wrong, and this is what keeps that from
    // turning into an argument about scoring.
    SpectralExtent extent{};

    SampleIndex start_sample = 0;

    // One past the last sample, or kAlwaysOn for an emitter in an unbounded
    // scene.
    SampleIndex end_sample = 0;

    // Mean power of this emitter alone, linear, as summed into the scene.
    double mean_power = 0.0;

    // Against the noise in this emitter's own occupied bandwidth. This is the
    // number a detector's sensitivity is quoted in.
    double snr_in_occupied_bandwidth_db = 0.0;

    // Against the noise across the whole sample rate, which is the ratio a
    // measurement over the raw buffer would report.
    double snr_in_full_band_db = 0.0;

    // Zero for the modes with no symbol rate, and for a broadcast FM
    // station, whose RDS bit clock is not a symbol rate a classifier could
    // read off the RF.
    double symbol_rate_baud = 0.0;

    // BroadcastFm only, and zero on a Modulated row: the largest |composite|
    // the station's spec can reach, as a fraction of WfmSpec::
    // peak_deviation_hz. FmComposite::peak_bound().
    //
    // Above 1.0 the station deviates past the figure `extent` was computed
    // from, which is reachable on purpose and is not clamped. Carried here
    // because a scorer otherwise reads a 268750 Hz Carson bandwidth off this
    // row with nothing saying the deviation it was computed against is
    // exceeded. generate_wfm() reports the same condition as a measurement
    // over its own buffer, and a scene has no equivalent: it never renders
    // a station on its own.
    //
    // A BOUND AND NOT A MEASUREMENT. It is the sum of the pilot's, the
    // audio's and the data's own peaks, which do not occur together, so a
    // rendered buffer always reads below it and a row at 0.99 is not a
    // promise that the station stayed legal by a hair.
    double composite_peak_bound = 0.0;

    // The payload seed, so this exact emitter can be rebuilt on its own.
    // Zero on a BroadcastFm row: that payload is a bit sequence the caller
    // handed over rather than one drawn from a seed, so the caller already
    // has it and there is no number here that would reproduce it.
    std::uint64_t payload_seed = 0;

    [[nodiscard]] bool open_ended() const { return end_sample == kAlwaysOn; }

    // The modulation field with the kind check attached, so a reader cannot
    // take it off a row where it means nothing. Nullopt on a BroadcastFm
    // row.
    //
    // The bare field stays public because it is what the generator fills
    // in, and because a struct of plain data is what the truth record is.
    // Every consumer in the tree goes through this.
    [[nodiscard]] std::optional<Modulation> readable_modulation() const
    {
        return (kind == EmitterKind::Modulated) ? std::optional<Modulation>{modulation}
                                                : std::nullopt;
    }
};

struct EmitterPlacement {
    ModulatorSpec modulator{};

    // When true, modulator.common.amplitude is ignored and the level is derived
    // from the scene noise floor to hit the requested ratio. That is almost
    // always what a test wants: an absolute amplitude means nothing without
    // knowing what the noise is doing.
    bool use_snr = true;
    double snr_in_occupied_bandwidth_db = 20.0;

    SampleIndex start_sample = 0;
    SampleIndex end_sample = kAlwaysOn;
};

// A broadcast FM station in the scene, carrying a known RDS bitstream.
//
// Placed by hand and never drawn by RandomPopulation. A station is 268750 Hz
// wide, it carries a payload somebody chose, and there is no sensible random
// bit sequence to give one, so it is not in the palette and Modulation has
// no member for it. Put the offsets where you want them.
struct WfmStationPlacement {
    WfmSpec station{};

    // Overwritten with the scene's rate, for the reason WfmSpec::rds.rate is
    // overwritten with the station's: one stream, one sample rate. A station
    // wider than the scene, or one whose skirt reaches past Nyquist at the
    // offset given, is refused by WfmSpec's own validate() with the numbers
    // in the message.
    //
    // Same meaning as EmitterPlacement's two fields.
    bool use_snr = true;
    double snr_in_occupied_bandwidth_db = 30.0;

    SampleIndex start_sample = 0;
    SampleIndex end_sample = kAlwaysOn;
};

// Fills a span with emitters nobody had to place by hand. The point is density:
// a channelizer that works against four signals can still fall over against
// two hundred.
struct RandomPopulation {
    // Distinct frequency slots. Each slot gets one modulator, one payload and
    // one set of parameters.
    std::size_t emitter_count = 0;

    // Transmissions per slot. One means the slot runs for the whole scene. More
    // than one turns the slot into a bursty emitter and needs a bounded scene,
    // because the bursts have to be laid out across a known duration. Every
    // burst is its own truth record, since every burst is its own detection
    // target.
    std::size_t bursts_per_emitter = 1;

    // Placement window, relative to the scene centre. Emitters are placed so
    // their whole occupied band falls inside this and inside Nyquist.
    Hertz span_low_hz = 0;
    Hertz span_high_hz = 0;

    double snr_in_occupied_bandwidth_db_min = 5.0;
    double snr_in_occupied_bandwidth_db_max = 35.0;

    double min_burst_seconds = 0.05;
    double max_burst_seconds = 2.0;

    // Empty means every mode.
    std::vector<Modulation> palette{};
};

struct SceneSpec {
    SampleRate rate = 20'000'000;

    // The RF centre this baseband represents. Carried into the scene for the
    // record and never used in the arithmetic: everything here is relative to
    // baseband DC.
    Hertz center_hz = 0;

    // Zero means unbounded, which is legal for continuous emitters and not for
    // bursty ones.
    SampleIndex duration_samples = 0;

    // Nanoseconds since the Unix epoch at sample 0. Supplied by the caller, not
    // read from a clock here, because a generator that reads a clock cannot
    // reproduce its own output.
    std::int64_t epoch_anchor_ns = 0;

    std::uint64_t seed = 0;

    bool add_noise = true;

    // Total noise power across the whole sample-rate bandwidth, in dB relative
    // to a full-scale amplitude of 1.0, so -60 means a total noise power of
    // 1e-6. Stated full band because that is the only level that is a property
    // of the buffer rather than of a bandwidth someone has to agree on.
    double noise_power_full_band_dbfs = -60.0;

    std::vector<EmitterPlacement> emitters{};
    std::vector<WfmStationPlacement> fm_stations{};
    RandomPopulation random{};

    // Zero means one per hardware thread. The output does not depend on this.
    std::size_t worker_threads = 0;
};

class Scene {
public:
    [[nodiscard]] static Expected<Scene> create(const SceneSpec& spec);

    // Writes absolute samples [start, start + out.size()). Pure: the same
    // absolute range always produces the same samples, regardless of blocking
    // or worker count.
    void render(SampleIndex start, ComplexSpan out) const;

    [[nodiscard]] const std::vector<EmitterTruth>& truth() const { return truth_; }

    [[nodiscard]] SampleRate rate() const { return rate_; }
    [[nodiscard]] Hertz center_hz() const { return center_hz_; }
    [[nodiscard]] SampleIndex duration_samples() const { return duration_; }
    [[nodiscard]] std::int64_t epoch_anchor_ns() const { return epoch_anchor_ns_; }

    // Total noise power across the sample-rate bandwidth, linear.
    [[nodiscard]] double noise_power_full_band() const { return noise_power_; }

    [[nodiscard]] dsp::BlockTimestamp timestamp_at(SampleIndex start) const;

private:
    Scene() = default;

    struct Burst {
        // Index into modulators_ or into stations_, according to kind.
        std::size_t modulator = 0;
        EmitterKind kind = EmitterKind::Modulated;
        SampleIndex start = 0;
        SampleIndex end = 0;
        double gain = 1.0;
    };

    void render_range(SampleIndex start, ComplexSpan out) const;
    void fill_noise(SampleIndex start, ComplexSpan out) const;
    [[nodiscard]] std::size_t plan_workers(std::size_t count) const;

    SampleRate rate_ = 0;
    Hertz center_hz_ = 0;
    SampleIndex duration_ = 0;
    std::int64_t epoch_anchor_ns_ = 0;
    std::uint64_t seed_ = 0;
    std::size_t worker_threads_ = 0;

    bool add_noise_ = true;
    double noise_power_ = 0.0;
    double noise_sigma_ = 0.0;

    // Bursts of one slot share a modulator, and therefore a payload. One
    // modulator per slot rather than per burst is what keeps a scene with a
    // hundred thousand bursts inside a few megabytes.
    std::vector<Modulator> modulators_{};

    // Broadcast FM stations, indexed by a Burst whose kind is BroadcastFm.
    // A separate vector rather than a variant because the two generators
    // have nothing in common beyond accumulate(), and a station is placed by
    // hand so there are never many of them.
    std::vector<WfmModulator> stations_{};

    // Sorted by start_sample, with truth_ held in the same order so an index
    // means the same thing in both.
    std::vector<Burst> bursts_{};
    std::vector<EmitterTruth> truth_{};

    // The longest burst in the scene, which bounds how far back a block has to
    // look for an emitter that is still running.
    SampleIndex longest_burst_ = 0;
    bool unbounded_burst_ = false;
};

// Receives one block. Returning an error stops the stream and the error is
// passed straight back to the caller.
using BlockSink = std::function<Status(const dsp::BlockTimestamp&, ConstComplexSpan)>;

// Produces count samples from start, block_samples at a time, without ever
// holding more than one block. This is the path an hour of 20 MS/s takes to a
// file or a channelizer.
[[nodiscard]] Status stream_scene(const Scene& scene,
                                  SampleIndex start,
                                  SampleIndex count,
                                  std::size_t block_samples,
                                  const BlockSink& sink);

// The truth table as CSV, header row first. An open-ended emitter leaves
// end_sample empty rather than printing the sentinel, so a scorer reading the
// file does not have to know what 18446744073709551615 means.
[[nodiscard]] std::string truth_csv(const Scene& scene);

}  // namespace revenant::siggen
