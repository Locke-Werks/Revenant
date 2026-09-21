// Spectrum frames from a synthetic scene, on the host, with ground truth.
//
// test_detector.cpp builds its frames bin by bin from a chosen mean, which is
// the right instrument for asking whether a known rectangle comes back at the
// right centre and the right width. It is the wrong instrument for asking why
// one wide signal reads as ten narrow ones, because a rectangle has none of
// the interior structure that question is about: a broadcast FM carrier is
// tens of decibels above its own sidebands, and that contrast is the thing
// under investigation.
//
// So this fixture runs the real path. core/dsp/synth renders the scene's IQ,
// core/dsp's channelizer twins turn it into the channel ring, and
// reference_spectrum turns that into exactly the frame the device produces.
// The twins are proved bit-exact against the kernels by
// tests/reference/test_pfb.cpp and tests/reference/test_spectrum.cpp, so a
// frame from here and a frame from GPU 0 differ in nothing that matters to a
// detector, and the whole chain runs with no device and no driver.
//
// What that buys, and it is the reason this file exists: siggen::Scene reports
// EmitterTruth for every burst. A known occupied band, a known start sample
// and a known stop sample, all chosen rather than measured. A detector can be
// scored against those numbers instead of against somebody's reading of a
// waterfall.
//
// COST. The twins are written for clarity, not speed, and the branch filter is
// the expensive one: M slots by taps_per_branch taps for every D input sample.
// Measured this session on this machine, a 2.4 MS/s scene runs at about a
// second and a half of wall time per second of scene, single threaded, and
// that figure barely moves with the channel count. Keep a scene to a handful
// of seconds.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "core/detect/detector.h"
#include "core/dsp/pfb.h"
#include "core/dsp/synth/wideband.h"
#include "core/dsp/types.h"
#include "core/engine/engine.h"
#include "core/error.h"

namespace revenant::test {

// The grid the frames are produced on.
//
// The defaults are the shipped configuration, which is what revenant-engine
// runs against the RTL-SDR: 2.4 MS/s, 64 coarse channels, a 2048-point
// second-stage transform, 65536 bins at 36.6 Hz. Not a scaled-down stand-in,
// because two of the detector's limits are counted in bins and one is counted
// in peaks, so a frame a sixteenth of the size does not reach them and a case
// run at that size reports a detector that works.
//
// The cost is not what it looks like. The branch filter dominates and its work
// per input sample is M*taps/D, which is 34 either way, so a bigger grid is
// nearly free here; the second-stage transform costs LESS at 64 channels,
// because 2048 points of a 75 kS/s channel is 36.6 frames a second against
// 586 at 512 points of a 300 kS/s one.
struct SceneGeometry {
    dsp::SampleRate rate = 2'400'000;
    std::uint32_t channels = 64;
    std::uint32_t transform = 2048;

    [[nodiscard]] dsp::GridParams grid() const;

    // Bins across the whole span, and hertz per bin.
    [[nodiscard]] std::size_t bins() const;
    [[nodiscard]] double bin_width_hz() const;

    // Source samples between one frame and the next. The spectrum stage
    // transforms N non-overlapping coarse-channel samples, and a coarse
    // channel sample is D source samples.
    [[nodiscard]] std::uint64_t frame_step() const;

    [[nodiscard]] engine::SpectrumGeometry spectrum() const;
};

// A memoryless front end between the scene and the channelizer.
//
// WHY A FIXTURE NEEDS ONE. core/detect/front_end.h measures whether the noise
// floor is following the strongest signal on the span, and the thing it is
// looking for is a receiver being driven past its linear range. A scene is
// linear by construction: siggen adds emitters and noise and nothing in the
// path multiplies them together. So without this there is no way to produce
// the failure the monitor exists to catch, and the only evidence would stay
// what it is today, one afternoon on air with a dongle.
//
// THE MODEL, AND WHY IT IS THIS ONE. A memoryless power series truncated at
// the third term, on the complex envelope:
//
//     y = g x - a3 |g x|^2 (g x)
//
// That is the standard bandpass representation and it is where third-order
// intercept comes from: see ITU-R SM.332 on intermodulation in receivers, or
// any treatment of the two-tone test. Two strong tones through it produce
// products at 2*f1 - f2 and 2*f2 - f1, which is the phantom detection the
// on-air measurement of 2026-09-20 recorded three of.
//
// The products the MONITOR sees are not those two. Expanding |x|^2 x with
// x = s + w, strong signals plus noise, leaves terms in 2|s|^2 w and s^2 w*.
// Both are noise shaped and land across the whole span, and both scale with
// the strong signals' power, which is exactly "the floor rises faster than
// the signal driving it". The discrete products and the broadband lift are
// the same nonlinearity seen twice.
//
// THE GAIN SWING IS NOT DECORATION EITHER. The monitor fits a slope, so it
// needs the strongest signal to move; a front end held in steady compression
// produces a high floor and a flat one and reads as Steady, which that file
// says out loud. A slow sinusoidal gain is what a tuner AGC does in the
// field, it is deterministic, and with a3 at zero it is also the control
// case: a pure multiplication moves the floor and the signal by the same
// number of decibels, which is a slope of exactly one.
struct FrontEndModel {
    // Voltage gain at the middle of the swing.
    double gain = 1.0;

    // Peak-to-peak swing of that gain, in dB, over one period. Zero holds
    // the gain still.
    double swing_db = 0.0;
    double swing_period_seconds = 1.0;

    // Third-order coefficient, in the same voltage units, so a3 = 0 is a
    // perfectly linear front end and the whole model collapses to the gain.
    double third_order = 0.0;

    [[nodiscard]] bool active() const {
        return gain != 1.0 || swing_db != 0.0 || third_order != 0.0;
    }

    // The gain at an absolute sample index, as a voltage multiplier. A pure
    // function of the index, which is what keeps SceneFrames reproducible
    // from any starting point the way siggen::Scene::render is.
    [[nodiscard]] double gain_at(dsp::SampleIndex index, dsp::SampleRate rate) const;

    // Applies the model in place over [start, start + out.size()).
    void apply(dsp::SampleIndex start, dsp::SampleRate rate, dsp::ComplexSpan out) const;
};

// Renders a scene and hands out its frames one at a time.
//
// Frames are produced in batches, because the branch filter amortises over a
// long dispatch and a per-frame call would redo its history every time. The
// caller sees one frame per next() either way.
class SceneFrames {
public:
    // The front end defaults to a perfectly linear unity gain, so every
    // case written before it existed is unchanged and does not mention it.
    [[nodiscard]] static Expected<SceneFrames> create(const SceneGeometry& geometry,
                                                      const siggen::SceneSpec& spec,
                                                      const FrontEndModel& front_end = {});

    SceneFrames(SceneFrames&&) noexcept = default;
    SceneFrames& operator=(SceneFrames&&) noexcept = default;
    SceneFrames(const SceneFrames&) = delete;
    SceneFrames& operator=(const SceneFrames&) = delete;

    // The next frame, or an error. power_db points into this object and is
    // valid until the following call, exactly as the engine's frame is.
    [[nodiscard]] Expected<engine::SpectrumFrame> next();

    [[nodiscard]] const siggen::Scene& scene() const { return *scene_; }
    [[nodiscard]] const engine::SpectrumGeometry& spectrum() const { return spectrum_; }
    [[nodiscard]] const SceneGeometry& geometry() const { return geometry_; }

    // Frames available before the scene's declared duration runs out. Zero for
    // an unbounded scene.
    [[nodiscard]] std::uint64_t frames_available() const;

private:
    SceneFrames() = default;

    [[nodiscard]] Status fill_batch();

    SceneGeometry geometry_{};
    engine::SpectrumGeometry spectrum_{};
    FrontEndModel front_end_{};

    // Held by pointer because siggen::Scene has no move assignment and this
    // class needs one to come back out of Expected.
    std::shared_ptr<siggen::Scene> scene_;

    std::vector<float> prototype_;
    std::vector<dsp::Complex32> coarse_twiddles_;
    std::vector<dsp::Complex32> fine_twiddles_;
    std::vector<float> window_;

    // One batch of frames' worth of input, and the two intermediate buffers.
    std::vector<dsp::Complex32> iq_;
    std::vector<dsp::Complex32> branches_;
    std::vector<dsp::Complex32> channel_ring_;
    std::vector<float> power_db_;

    std::uint32_t frames_per_batch_ = 0;
    std::uint32_t frame_in_batch_ = 0;

    dsp::SampleIndex next_sample_ = 0;
    dsp::SampleIndex frame_start_ = 0;
    std::uint64_t sequence_ = 0;
};

// What the detector said about one emitter, scored against its truth record.
struct EmitterScore {
    std::uint32_t emitter_id = 0;

    // Which generator made the emitter, and its mode when that question has
    // an answer. Nullopt on a broadcast FM row: siggen::EmitterTruth's
    // modulation field is meaningless there and reads as Cw, so copying it
    // unguarded would score a 268750 Hz station as a keyed carrier.
    siggen::EmitterKind kind = siggen::EmitterKind::Modulated;
    std::optional<siggen::Modulation> modulation{};

    // From the truth record, absolute hertz in the frame the detector reports.
    dsp::Hertz truth_low_hz = 0;
    dsp::Hertz truth_high_hz = 0;
    dsp::Hertz truth_bandwidth_hz = 0;
    double truth_snr_occupied_db = 0.0;
    dsp::SampleIndex truth_start = 0;
    dsp::SampleIndex truth_end = 0;

    // Distinct track ids that ever overlapped this emitter's band while it was
    // transmitting. One is the wanted answer.
    std::size_t track_ids = 0;

    // Most tracks overlapping the band at any single decision, which separates
    // a signal cut into simultaneous pieces from one whose id churns over
    // time. Both read as "many tracks" in a list and they are different
    // faults.
    std::size_t peak_simultaneous = 0;

    // The best single track, chosen by how much of the truth extent it covers.
    // Zero id when nothing ever overlapped.
    std::uint64_t best_track = 0;
    double best_coverage = 0.0;   // covered truth band over truth bandwidth
    double best_spill = 0.0;      // track band outside the truth, over truth bandwidth
    dsp::Hertz best_centre_error_hz = 0;
    dsp::Hertz best_bandwidth_hz = 0;

    // Decisions the emitter was transmitting at, and decisions at least one
    // track overlapped it. Their ratio is the detection rate.
    std::size_t decisions_on = 0;
    std::size_t decisions_detected = 0;

    // How far the best-overlapping track's centre and edges move from one
    // decision to the next, mean and worst, in hertz.
    //
    // This is the figure the tracker's association_overlap has to survive. A
    // candidate that moves further between decisions than the overlap rule
    // tolerates starts a new track every decision, which reads as a churning
    // id rather than as a measurement that wanders, so the two have to be
    // compared and not just each looked at.
    double centre_step_mean_hz = 0.0;
    double centre_step_max_hz = 0.0;
    double bandwidth_step_mean_hz = 0.0;

    // Worst overlap, as a fraction of the narrower band, between what the
    // best track was at one decision and what it was at the next. The
    // tracker's association_overlap is compared against exactly this
    // quantity, so a value under it is an id that could not survive.
    double worst_self_overlap = 1.0;

    // Longest run of consecutive decisions one id survived while the emitter
    // was on, over the number of decisions it was on. One is a track that
    // lasted the whole transmission.
    double best_lifetime_fraction = 0.0;

    // ---- the window after the emitter stops -------------------------------
    //
    // Everything above is scored while the emitter is transmitting, and for a
    // long time that was the whole instrument: observe() returned early on
    // every decision outside [truth_start, truth_end], so what a track did
    // after its signal stopped was not measured at all.
    //
    // It needs scoring differently rather than more of the same. After the
    // stop there is nothing left to be right ABOUT, so coverage against truth
    // is not the question. The questions are whether the row went away on
    // time and whether it moved while it was waiting, because a row in a
    // client is clickable and a click tunes a receiver to its centre at its
    // bandwidth.
    //
    // Followed by ID, not by band. A neighbour's genuine track can overlap
    // this emitter's extent and is not this emitter's ghost, and following
    // the id is also what keeps a ghost visible in the numbers as it walks
    // away from the band it was born in.

    // The window that was asked for, in source seconds past truth_end. Zero
    // for an emitter that never stops, and for a scorer built without one.
    double post_window_seconds = 0.0;

    // Decisions inside the window, and the ones the carried id was still in
    // the published list at.
    std::size_t after_decisions = 0;
    std::size_t after_published = 0;

    // Source seconds from truth_end to the last decision the carried id was
    // still published. Zero when it was already gone at the first decision
    // past the stop, which is the wanted answer for an emitter whose
    // detection ends with it.
    double residual_seconds = 0.0;

    // Time the carried id spent in each state inside the window. Held is the
    // hold doing its job. Live after the signal stopped is the row claiming
    // present-tense evidence it does not have, which is the state a consumer
    // cannot see through: silent_samples() is zero and confidence is rising.
    double after_live_seconds = 0.0;
    double after_held_seconds = 0.0;

    // Still published at the last decision the window contained, which means
    // the window truncated the answer and residual_seconds is a lower bound
    // rather than a measurement.
    bool still_published_at_window_end = false;

    // The carried id's geometry at the last decision inside the
    // transmission. This is the last reading taken with a signal under it,
    // and it is what the two figures below are measured against rather than
    // against truth: the question is whether the row MOVED after its
    // evidence ran out.
    dsp::Hertz on_centre_hz = 0;
    dsp::Hertz on_bandwidth_hz = 0;

    // Worst inside the window, and at the last decision the id was alive.
    // The bandwidth figures are ratios to on_bandwidth_hz, and the worst one
    // is whichever is furthest from one in either direction: a band that
    // collapses is as wrong as one that inflates, and the measured drift on
    // a shaped signal went narrow rather than wide.
    dsp::Hertz after_worst_centre_error_hz = 0;
    double after_worst_bandwidth_ratio = 1.0;
    dsp::Hertz after_last_centre_error_hz = 0;
    double after_last_bandwidth_ratio = 1.0;

    // What the carried id's state was at the worst centre error, which is
    // what says whether the drift happened in the hold or before it.
    detect::TrackState after_state_at_worst = detect::TrackState::Pending;

    // Ids other than the carried one that overlapped the truth band inside
    // the window. Anything above zero is a track born out of the residual
    // after the first one was dropped.
    std::size_t after_new_ids = 0;
};

// Scores a whole run. Constructed from the scene's truth, then fed the track
// list at every decision.
class SceneScorer {
public:
    // post_window_seconds is how far past each emitter's stop the scorer
    // keeps watching, in source seconds. Zero scores the transmission only,
    // which is what every case that predates the post-stop fields wants. It
    // has to be long enough to contain the whole answer, because a row still
    // published when the window closes makes residual_seconds a lower bound.
    SceneScorer(const siggen::Scene& scene, dsp::Hertz source_center,
                double post_window_seconds = 0.0);

    // Call once per decision, after the detector has taken it. `now` is
    // Detector::last_decision().
    void observe(dsp::SampleIndex now, std::span<const detect::Track> tracks);

    [[nodiscard]] std::vector<EmitterScore> finish() const;

private:
    struct Live {
        EmitterScore score{};
        std::vector<std::uint64_t> seen_ids;
        std::uint64_t run_id = 0;
        std::size_t run_length = 0;
        std::size_t best_run = 0;

        // The id that was best at the last decision inside the
        // transmission, which is the one the post-stop window follows, plus
        // the ids that turned up in the band after it and the previous
        // decision the window saw.
        std::uint64_t carried_id = 0;
        std::vector<std::uint64_t> after_ids;
        dsp::SampleIndex after_previous = 0;
        double worst_bandwidth_excursion = 0.0;

        // The best-overlapping track at the previous decision, for the
        // step-to-step figures. Valid only while `have_previous` is set,
        // which a gap in the transmission clears.
        bool have_previous = false;
        double previous_low = 0.0;
        double previous_high = 0.0;
        double step_sum = 0.0;
        double width_step_sum = 0.0;
        std::size_t steps = 0;
    };

    void observe_after(Live& live, dsp::SampleIndex now,
                       std::span<const detect::Track> tracks) const;

    std::vector<Live> emitters_;
    double source_rate_ = 1.0;
    dsp::SampleIndex post_window_samples_ = 0;
};

}  // namespace revenant::test
