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

// Renders a scene and hands out its frames one at a time.
//
// Frames are produced in batches, because the branch filter amortises over a
// long dispatch and a per-frame call would redo its history every time. The
// caller sees one frame per next() either way.
class SceneFrames {
public:
    [[nodiscard]] static Expected<SceneFrames> create(const SceneGeometry& geometry,
                                                      const siggen::SceneSpec& spec);

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
    siggen::Modulation modulation = siggen::Modulation::Cw;

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
};

// Scores a whole run. Constructed from the scene's truth, then fed the track
// list at every decision.
class SceneScorer {
public:
    SceneScorer(const siggen::Scene& scene, dsp::Hertz source_center);

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

    std::vector<Live> emitters_;
};

}  // namespace revenant::test
