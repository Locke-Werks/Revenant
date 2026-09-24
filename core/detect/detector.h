// The wideband detector: spectrum frames in, tracks out.
//
// docs/detection.md is the design and this is the part of it that does not
// need a classifier: find what is transmitting across the whole span, measure
// where it is and how wide, and give each one an identity that survives the
// gaps in its own transmission. Probe receivers and click-to-tune are not
// here. Track::classification is filled from outside, through record_probe,
// by core/detect/tier_two.h driving core/engine/probe.h, and nothing in this
// file reads it back: the detector decides exactly what it decided before a
// family existed.
//
// WHAT THIS PARAGRAPH USED TO SAY: "Classification, probe receivers and
// click-to-tune are not here. Track::classification exists and nothing fills
// it, which is deliberate: the field is the seam, not a stub."
//
// WHY THIS RUNS ON THE HOST
//
// core/engine/engine.h is explicit that the spectrum frame is already coming
// back: 65536 bins of four bytes at 305 frames a second, 80 MB/s, against the
// 160 MB/s the sample stream costs going out. The frame has crossed the bus
// before this file sees it, so working on it here adds no bus traffic at all.
// What it costs is CPU, and the two stages cost very differently.
//
// Both figures below are measured rather than estimated, at 65536 bins, in a
// RelWithDebInfo build, by running revenant-cli --detect over the same scene
// at two frame rates and solving the pair. The two terms scale differently,
// so one number per frame would have hidden which of them matters.
//
//   Accumulating costs 0.32 ms per frame. One exp2 and one multiply-add per
//   bin: the frame arrives in decibels and a power average has to be taken in
//   linear power, so the conversion can be neither skipped nor shared. It
//   runs on EVERY frame, because a frame skipped here is energy the detector
//   never sees.
//
//   Deciding costs 7.4 ms. A local percentile across frequency, a pair of
//   prefix sums, a summed-bin search over fourteen widths and the tracker,
//   which is a dozen passes over the same 65536 bins. Most of it is the
//   percentile: thirty-two windows of a quarter of the span each. It does NOT
//   run per frame, because the statistic it works on is already integrated
//   over a second and deciding at 305 Hz would be deciding 305 times on the
//   same evidence. decision_interval_seconds defaults to 0.1.
//
// At 305 frames and ten decisions a second that is 97 ms plus 74 ms, so
// 171 ms of CPU per second of capture: 17 percent of one core, on a machine
// with sixteen. Affordable, so it stays on the host, and it does not grow
// with the sample rate because both terms are bounded by the bin count.
//
// The escape hatch, if a later configuration makes it not affordable, is the
// accumulation rather than the decision: the frame is already on the device
// when it is produced, so the exp2-and-average pass is the piece that moves
// there, and what comes back is then the integrated frame at a tenth of the
// rate instead of the raw one, which cuts the bus traffic as well. The
// decision stays here either way, because a percentile, a sort and a greedy
// assignment are not shaped like a compute dispatch. The cheaper lever
// before that is the percentile: estimating it from every fourth bin of a
// window rather than all of them is statistically almost the same answer for
// a quarter of the work.
//
// WHAT IS SETTLED, FROM docs/detection.md, AND IS NOT A CHOICE MADE HERE
//
//   The detection statistic is computed on SUMS OF ADJACENT BINS, not on
//   single fine bins. The deflection of the statistic is maximised when the
//   summing width matches the signal bandwidth, and single-bin thresholding
//   on a fine transform gives away about 7.8 dB on an SSB signal. Fine bins
//   bound the edges; summed bins decide whether anything is there.
//
//   The noise floor is a low percentile ACROSS FREQUENCY, local in frequency,
//   never a mean. A mean over a region containing a signal is raised by the
//   signal it is measuring.
//
//   A candidate's SNR is stated in the project's 2500 Hz reference bandwidth,
//   per docs/snr-convention.md. A per-bin peak is not comparable across
//   bandwidths and is not any of the three conventions that document fixes.
//
//   Both thresholds belong to the operator. Neither has a right value that
//   suits a quiet VHF band and an HF evening at the same time.
//
// PURITY
//
// Nothing here reads a clock. Every interval is a difference of sample
// indices taken from the frames themselves, divided by the configured source
// rate, exactly as docs/conventions.md requires. A detector replayed over a
// stored capture at forty times realtime produces the same tracks with the
// same ages as it did live.
//
// THREADING
//
// One thread owns a Detector. consume() is called from one thread, and
// tracks() reads state that call wrote. revenant-cli calls it from its
// spectrum sink, on the engine's completion thread; the RPC server calls it
// from a thread of its own, core/rpc/server.cpp's sigid lane, which the
// sink hands frames to. A display on another thread takes a snapshot; it
// does not hold a reference into this object. There is no lock here and none
// is wanted in a per-frame path.
//
// WHAT THE SECOND SENTENCE USED TO SAY: "consume() is called from wherever
// the spectrum sink runs, which is the engine's completion thread". True of
// the server until 2026-09-23.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/characterise/catalogue.h"
#include "core/detect/shape.h"
#include "core/dsp/types.h"
#include "core/engine/engine.h"
#include "core/error.h"
#include "core/identify/identify.h"

namespace revenant::detect {

// The reference bandwidth every reported SNR is stated in.
//
// docs/snr-convention.md: this is the WSJT-X figure and it is the project
// default everywhere. Its only significance is that everybody uses it, which
// is exactly what makes one operator threshold mean one thing across a span
// carrying a 50 Hz carrier and a 200 kHz broadcast at the same time.
inline constexpr double kReferenceBandwidthHz = 2500.0;

// Equal parts the residual rule checks a decaying band in, one at a time, as
// well as checking it whole. See DetectorConfig::residual_rate_tolerance for
// the case that needs it, which is a quiet emitter merged into a loud one
// that stops.
//
// Eight rather than four, and the difference is whether any part of the band
// holds the quiet emitter and nothing else. Measured 2026-09-20 on "a
// transmitter beside one that stopped is not withheld with it": at four
// parts, the part covering the quiet emitter still held three bins of the
// loud one against five of the quiet one, which is enough to make it fall
// with the loud one until that is 20 dB down, and the blackout stayed at
// 4.26 s. At eight the same part is the quiet emitter alone, it does not
// fall at all, and the blackout is 0.00 s.
//
// The bound on going finer is the ripple. A part's measured level carries the
// averaged spectrum's own fluctuation, which is 1/sqrt(N * K) of the level
// over N bins at K effective frames: at the shipped second of averaging and
// this file's 125 frames a second that is 0.27 dB on one bin and 0.10 dB on
// eight, against a fall of about 1.7 dB over the shortest run the rule will
// act on. A part has to hold enough bins for its fall to mean something, and
// eight parts of a narrow candidate is already one or two bins each.
inline constexpr std::size_t kResidualParts = 8;

// How much of the BAND's OWN measured fall each part has to have managed for
// the band to read as emptying everywhere.
//
// Of the measured fall and not of the predicted one, which is the difference
// between a uniformity test and a second rate test. An average with no input
// empties everywhere at once, so every part falls by whatever the whole fell
// by, whatever that is; tying the floor to the prediction instead re-imposes
// a minimum rate, and residual_rate_tolerance at 1.0 is documented to remove
// exactly that. Measured 2026-09-20: against the prediction, both cases that
// open the window to its far end to prove the rule has teeth stopped firing
// at all, "a station that fades keeps its track" withholding 0 candidates
// where it withholds 40 now.
//
// One-sided, because a part falling FASTER than the whole is the ripple on a
// few bins and says nothing on its own. The whole is still bounded above by
// the window.
//
// Half is the loosest value that answers the case and it is loose on purpose.
// A part of a shaped signal is its skirt, whose excess is a small number
// carrying the same relative ripple as a large one, and the rule has to
// survive that: the three-station post-stop scene stops publishing 3.27 to
// 3.30 s after each station stops, unchanged from before this test existed.
// The part that catches the two-emitter case is not near the edge, it has
// stopped falling altogether.
inline constexpr double kResidualPartFloor = 0.5;

// What a track is, once something decides.
//
// The families core/characterise names, one for one, and no more: a family
// the characteriser cannot name is not one this layer can carry. The detector
// does not infer any of them. docs/detection.md puts identification in two
// tiers above this layer, and the second tier answers through
// Detector::record_probe. An unidentified signal is a real answer rather than
// a gap to be filled with the nearest label, which is why Unknown stays the
// value every track is born with and the one noise is supposed to keep.
//
// WHAT THIS USED TO BE: Unknown alone, under "Nothing in this file sets
// anything but Unknown". Nothing in this file sets anything else now either;
// record_probe copies what a probe found.
//
// ON THE WIRE since 2026-09-23, as the label core/detect/label.h makes of a
// track: the owner's decision, recorded in docs/detection.md. WHAT THIS
// PARAGRAPH USED TO SAY: "INTERNAL. docs/detection.md settled that nothing
// goes on the wire as a family, and core/rpc carries no field for this."
enum class Classification : std::uint8_t {
    Unknown = 0,
    Unmodulated,
    AnalogueFm,
    Fsk,
    Psk,
    Ofdm,
};

// How many values Classification has, for a table indexed by one.
inline constexpr std::size_t kClassificationCount =
    static_cast<std::size_t>(Classification::Ofdm) + 1;

[[nodiscard]] const char* classification_name(Classification classification);

// The characteriser's family, as a track carries it.
[[nodiscard]] Classification classification_of(characterise::ModulationFamily family);

// What one tier-two probe said about a track.
//
// Kept whole whether or not the family may drive anything, because a refused
// answer is still evidence: a PSK call with no symbol rate is what the
// characteriser says about a weak carrier and about weak BPSK alike, and a
// reader deciding which one a track is wants to see that it was said.
struct ProbeFinding {
    Classification family = Classification::Unknown;
    double confidence = 0.0;

    // Zero when the family's own detector found none.
    double symbol_rate_hz = 0.0;
    std::uint32_t order = 0;
    std::uint32_t tone_count = 0;
    double concentration = 0.0;

    // characterise::may_drive_detection on the full result, and the flag it
    // most often refuses on.
    bool may_drive_detection = false;
    bool psk_without_symbol_rate = false;

    // characterise::Characterisation::double_sideband, meaningful on an
    // Unmodulated family only: the carrier has mirrored sidebands.
    bool double_sideband = false;

    // core/identify's answer, which stands on verified framing and not on the
    // family, so it is kept whether or not the family may drive anything.
    identify::Protocol protocol = identify::Protocol::None;
    double protocol_confidence = 0.0;

    // The decision at which it was recorded. record_probe sets it.
    dsp::SampleIndex at = 0;
};

// Where a track is in its life.
//
// Held and Merged are both "not detected this decision" and they are not the
// same thing. A Held track has no evidence and is decaying. A Merged track
// has evidence, it is simply inside another track's band, so it does not
// decay while that lasts. Collapsing the two loses every id on the far side
// of a merge, which is the failure the split rule exists to prevent.
enum class TrackState : std::uint8_t {
    // Seen, not yet born. Counting toward birth_hits and invisible to
    // tracks(). A pending track that misses one decision is discarded, which
    // is what makes birth_hits a consecutive-decision rule.
    Pending = 0,

    // Detected at the most recent decision.
    Live = 1,

    // Not detected, inside its hold, decaying.
    //
    // A Held track's center, bandwidth and snr_2500_db are the last ones a
    // decision measured and nothing rewrites them, so they are stale by
    // construction and correct as far as they go. That is deliberate: a
    // click on a held row tunes a receiver to where the signal was, which is
    // where it comes back, and both the spectrum marker and the waterfall
    // rectangle size themselves from the bandwidth. A blanked geometry is an
    // unclickable, invisible row.
    Held = 2,

    // Not detected because another track's candidate swallowed it.
    Merged = 3,
};

[[nodiscard]] const char* track_state_name(TrackState state);

// One decision's worth of "something is here", before any identity is
// attached. Candidates do not persist and are not what an operator clicks.
struct Candidate {
    // Absolute radio frequency, source_center already added.
    //
    // docs/detection.md: the track carries absolute and converts at the call,
    // so that the detector and the command line cannot disagree about what a
    // number means. Rounded to integer hertz once, here, from the exact
    // rational frequency axis the frame carries.
    //
    // This is the centre of the measured occupied band and nothing else. It
    // is not the logical centre docs/ui-spectrum.md wants for AFT, which is a
    // property of the modulation and needs a classification this layer does
    // not have.
    dsp::Hertz center = 0;

    // Occupied bandwidth: the span holding occupied_power_fraction of the
    // band's excess power over the noise floor, bounded by fine bins.
    dsp::Hertz bandwidth = 0;

    // In the 2500 Hz reference bandwidth. See kReferenceBandwidthHz.
    double snr_2500_db = 0.0;

    // How far this stood above the threshold it had to clear, mapped to zero
    // to one by characterise::margin_confidence: a half exactly at the
    // threshold, 0.82 six decibels above it, 0.93 at twelve, approaching one
    // without arriving.
    //
    // THE CALIBRATED NUMBER Track::confidence IS NOT, and the two are
    // deliberately orthogonal. That one counts consecutive detections and
    // reads no signal quality; this one reads the measurement and nothing
    // about time. A strong station detected once has a high margin and a low
    // confidence. A weak bump present all afternoon has the reverse.
    //
    // IT IS NOT AUTHENTICITY AND MUST NOT BE READ AS ANY. It orders detections
    // by how far their evidence stood above the noise, which is what
    // docs/detection.md asks a threshold to mean. A strong interferer stands
    // well above the noise and scores high, correctly. Telling a signal from a
    // product is a different question that this does not answer and that no
    // field on this struct answers today.
    double margin_confidence = 0.0;

    // The local noise floor this was measured against, per fine bin, and the
    // strongest fine bin in the band. Both dBFS, both diagnostics: neither is
    // comparable across bandwidths and neither is the threshold's unit.
    double noise_floor_dbfs = 0.0;
    double peak_dbfs = 0.0;

    // Inclusive fine-bin bounds in the frame.
    std::uint32_t first_bin = 0;
    std::uint32_t last_bin = 0;

    // What the band looks like, from core/detect/shape.h. A MEASUREMENT AND
    // NOT A VERDICT: nothing in this file thresholds on any of it, and no
    // threshold has been chosen, because choosing one is a measurement against
    // scenes with known truth rather than a constant somebody liked.
    //
    // Carried on the candidate rather than derived later because the arrays it
    // is taken from are this decision's and are overwritten by the next one.
    BandShape shape;
};

// A thing with an identity that persists across frames. This is what an
// operator clicks and what a hold and a decay apply to.
struct Track {
    // Issued in order from one and never reused, so an id in a log names one
    // signal for the life of the process.
    std::uint64_t id = 0;

    TrackState state = TrackState::Pending;

    // What the detector reports this track to be: the family of the most
    // recent probe characterise::may_drive_detection accepted, with that
    // probe's confidence and symbol rate. Unknown, zero and zero until one
    // has.
    //
    // A PROBE THAT WAS REFUSED DOES NOT MOVE THESE, whatever it said. That is
    // the rule docs/detection.md asks of anything routing a family into the
    // detector: Unknown is refused, and so is a PSK call with no symbol rate,
    // because the characteriser says that about a weak carrier as confidently
    // as about weak BPSK. The refused answer is in last_probe below.
    //
    // A later accepted probe replaces an earlier one. A later refused one
    // leaves it: a transmission does not change modulation halfway through,
    // so one probe that could not see the family is not evidence the family
    // went away.
    Classification classification = Classification::Unknown;
    double classification_confidence = 0.0;
    double symbol_rate_hz = 0.0;

    // The same probe's modulation order and tone count, zero where its
    // family carries none, and its double-sideband reading. A label reads
    // these to say BPSK rather than PSK, or AM rather than CW.
    std::uint32_t classification_order = 0;
    std::uint32_t classification_tones = 0;
    bool classification_double_sideband = false;

    // The protocol the most recent probe VERIFIED, with its confidence, and
    // None until one has. core/identify/identify.h claims one only on a sync
    // its decoder checked, never on modulation, so a verified protocol is
    // taken whatever the family said. A later probe that verified nothing
    // leaves it, for the reason the classification is sticky: one dwell that
    // missed the framing is not evidence the protocol went away.
    identify::Protocol protocol = identify::Protocol::None;
    double protocol_confidence = 0.0;

    // The decision at which classification was first set, or zero. Against
    // first_seen this is the time to first classification.
    dsp::SampleIndex classified_at = 0;

    // Every probe's answer lands here, accepted or not, and probes counts
    // them. A track with probes at zero has not been looked at; one with
    // probes above zero and classification Unknown has, and the answer was
    // unknown, which is a different thing to tell an operator.
    ProbeFinding last_probe;
    std::uint32_t probes = 0;

    // Absolute, same frame as Candidate::center, lightly smoothed across
    // decisions so a display is not reading measurement noise.
    dsp::Hertz center = 0;
    dsp::Hertz bandwidth = 0;
    double snr_2500_db = 0.0;

    // The same margin measure Candidate carries, taken from this track's
    // SMOOTHED snr_2500_db so it moves with the track rather than with one
    // decision's measurement noise.
    //
    // IT DOES NOT DECAY WHILE A TRACK HOLDS, which is the point of having two
    // numbers. A track that has stopped being detected keeps the margin its
    // last measurement had and loses confidence instead. Decaying both would
    // make them the same number twice and leave nothing answering "how strong
    // was it when it was there".
    double margin_confidence = 0.0;

    // What the band looked like the last time a candidate was actually
    // assigned to this track, straight off Candidate::shape.
    //
    // NOT SMOOTHED, AND IT GOES STALE THE WAY margin_confidence DOES. A
    // holding track keeps the shape its last detection had, because the
    // alternative is decaying a measurement toward nothing, which would read
    // as a band changing shape when what happened is that it stopped being
    // there. BandShape::measured says whether anything was measured at all.
    //
    // Smoothing was considered and not done. The fields are ratios over a
    // window whose width moves with the band, so the same number from two
    // decisions is not always the same quantity, and an exponential average
    // over them would be arithmetic without a meaning. A caller wanting
    // stability over time should watch the series rather than be handed one
    // number pretending to be it.
    BandShape shape;

    // Zero to one, rising on each decision this track was detected in and
    // decaying on each one it was missed from. This is what
    // DetectorConfig::confidence_threshold is compared against, and it is the
    // track's, never any single frame's.
    //
    // IT MEASURES PERSISTENCE AND NOT QUALITY, which the name does not say
    // and a reader should not have to derive. It is `1 - (1 - rise)^n` over n
    // consecutive detections and no term in it reads snr_2500_db, bandwidth,
    // the deflection margin or anything about the shape of the band. Two
    // tracks detected in the same decisions hold bit-identical confidence
    // however strong or weak each one is.
    //
    // So a steady carrier and a steady bump in the noise floor both reach
    // 1.00 in about 1.3 seconds at the shipped rise, and on 2026-09-21 an
    // operator reported exactly that against intermod products. The answer it
    // gives, "this has been here continuously and for how long", is a true
    // and useful answer to a question nobody asked it. docs/detection.md has
    // the retraction. margin_confidence above is the calibrated number, and it
    // is a separate field rather than a correction to this one because the two
    // answer different questions and both are worth thresholding on.
    double confidence = 0.0;

    dsp::SampleIndex first_seen = 0;

    // The most recent decision this track existed at, and the most recent one
    // a candidate was actually assigned to it. They differ exactly by how
    // long it has been holding.
    dsp::SampleIndex last_seen = 0;
    dsp::SampleIndex last_detected = 0;

    // Decisions with and without a candidate. misses resets on a detection.
    std::uint32_t hits = 0;
    std::uint32_t misses = 0;

    // Which coarse channel a receiver on this track would read, with the
    // hysteresis docs/ui-spectrum.md and docs/detection.md both require:
    // place() takes the nearest channel by rounding, so a track near a
    // boundary flips on measurement noise, and each flip is a completely
    // different tap table and a half-megabyte upload.
    //
    // channel is reduced into [0, grid_channels) the same way place() reduces
    // it. channel_index is the signed index before reduction, which is what
    // the hysteresis is carried in. Both are meaningless unless
    // channel_valid, which needs grid_channels and source_rate configured.
    std::int64_t channel_index = 0;
    std::uint32_t channel = 0;
    bool channel_valid = false;

    // The track this one was merged into, while state is Merged. Zero
    // otherwise.
    std::uint64_t merged_into = 0;

    [[nodiscard]] constexpr dsp::SampleIndex age_samples() const {
        return last_seen - first_seen;
    }

    // How long it has been holding. Zero while Live.
    [[nodiscard]] constexpr dsp::SampleIndex silent_samples() const {
        return last_seen - last_detected;
    }
};

struct DetectorConfig {
    // ---- what the frame does not carry -----------------------------------

    // Needed to turn a difference of sample indices into seconds, which is
    // the only clock this object has. Required.
    dsp::SampleRate source_rate = 0;

    // Added to every candidate and track frequency. EngineInfo::source_center.
    dsp::Hertz source_center = 0;

    // Coarse channels in the grid. Zero switches the channel hysteresis off,
    // which is what a caller with no engine behind it does.
    std::uint32_t grid_channels = 0;

    // ---- the operator's two thresholds -----------------------------------

    // Detection, in dB of SNR in the 2500 Hz reference bandwidth. Not
    // compiled in, and this default is a starting point rather than a right
    // answer: it is roughly ten dB above what noise alone produces at the
    // widest summing width and a one second average, which is conservative on
    // a quiet band and will miss things on a busy one.
    double detection_threshold_db = 6.0;

    // Confidence, from zero up to but not including one, for whatever is
    // filtering the track list. Nothing in this file drops a track for
    // failing it; a display and a click-to-tune surface do, which is where
    // the operator's number belongs.
    //
    // The open upper end is load-bearing rather than tidy. Confidence rises
    // by confidence_rise of its remaining distance to one, so in exact
    // arithmetic it approaches one without arriving. A bar of one then hides
    // every track and says nothing about why: measured against an RTL-SDR at
    // 98.1 MHz, a five second run with the bar at one listed no tracks at any
    // of its four intervals while 88 were born. create() refuses that pair
    // rather than accepting a filter that can only be empty.
    //
    // WHAT THIS PARAGRAPH USED TO SAY. "It approaches one and lands on it
    // only when confidence_rise is itself one and a single detection closes
    // the whole gap." That is the exact-arithmetic answer and this does not
    // run in exact arithmetic. The iteration is in IEEE double, and from one
    // ulp below one the increment rounds UP to one for any rise of 0.5 or
    // more: at 0.5 the increment lands exactly on the midpoint and ties to
    // even, which is one. Measured at 0.5 after 53 detections, 0.6 after 40,
    // 0.9 after 16, and stalling one ulp below one at the next double down
    // from 0.5. See require_reachable_confidence in detector.cpp, which held
    // the same wrong claim and refused a bar of one in configurations that
    // could in fact clear it.
    //
    // ui/models/engine_link.h reached the true answer first and carries
    // static_asserts on both sides of the break point. It is a display and
    // this is the authoritative place, so the fact is now stated here as
    // well rather than living two directories away from the header that
    // contradicted it.
    double confidence_threshold = 0.5;

    // ---- integration in time ---------------------------------------------

    // One frame's noise varies enough that a threshold near the floor
    // produces constant false detections. About a second settles it, and a
    // second is also roughly the shortest transmission worth holding.
    //
    // Implemented as an exponential average whose coefficient is recomputed
    // per frame from the actual gap in sample indices, so a dropped frame
    // advances the average by the right amount instead of by one step.
    double average_seconds = 1.0;

    // How often the detection and the tracker run. See the cost note at the
    // top of this file: the average already integrates a second, so deciding
    // per frame decides repeatedly on the same evidence at thirty times the
    // price.
    //
    // AND NEVER MORE OFTEN THAN ONE SPECTRUM WINDOW, which create() enforces
    // and decision_seconds() reports. A window is one transform's duration,
    // the reciprocal of the bin width: 27 ms on the shipped 2.4 MS/s grid,
    // 0.17 s at 96 kS/s on 16 channels, 0.68 s on 64. The tracker's rules are
    // counted in decisions, birth_hits consecutive ones, residual_decisions,
    // a confidence rise and a centre smoothing per decision, and each assumes
    // a decision brings evidence the last one did not have. Where a window is
    // shorter than this interval, which is every grid these were calibrated
    // on, one does. Where it is longer, several decisions in a row read the
    // same window.
    //
    // That became reachable with EngineConfig::spectrum_rows_per_second.
    // Before it a slow source's frame came once per 65536-sample block, never
    // faster than one window, and a decision could not outrun its frames.
    // Since, a 96 kS/s source gets a frame every 3200 samples however long its
    // window, so on 64 channels ten decisions a second read windows 0.68 s
    // long. Measured 2026-09-23 on the KF4FIC 40 m 1359 UT excerpt through
    // the engine: the live tracks per decision held at 3.91, and births went
    // from 31 to 59, merges from 15 to 55 and splits from 1 to 19. With the
    // interval held to one window the same run gives 27, 11 and 2 at 3.71
    // live. docs/detection.md, "Overlapping frames", has every rate.
    //
    // The average is not slowed with it. It still takes every frame, weighted
    // by the source time each adds, so the overlap's extra averaging is kept.
    double decision_interval_seconds = 0.1;

    // ---- the noise floor, across frequency -------------------------------

    // Points across the span at which the floor is estimated, interpolated
    // between. Thirty-two is one per two coarse channels on the shipped grid.
    std::uint32_t noise_knots = 32;

    // Width of each estimate's window, in knot spacings. Eight at the default
    // knot count makes each window a quarter of the span, overlapping
    // eightfold.
    //
    // This is the number that decides whether a wide signal can hide its own
    // floor, and it is set by the widest thing anyone expects to detect
    // rather than by how local the estimate ought to be. A 200 kHz broadcast
    // at 2.4 MS/s across 65536 bins is 5461 bins wide. A window of an eighth
    // of the span would be two thirds broadcast, which puts both percentiles
    // below inside the signal and makes the floor the signal. A quarter of
    // the span is 600 kHz there, and the broadcast is a third of it.
    std::uint32_t noise_window_knots = 8;

    // Two low percentiles, which between them give the noise level and the
    // noise spread without either one being contaminated by a signal that
    // fills less than sixty percent of the window.
    //
    // A percentile alone is a biased estimate of a LEVEL: the tenth
    // percentile of an unaveraged spectrum sits 10 dB under its own mean, and
    // that bias would land straight in every reported SNR. Differencing two
    // of them recovers the spread, the spread gives the level, and the level
    // plus a few standard deviations is what separates noise from signal.
    // Both are taken from the lower half so a busy band does not move them.
    double noise_percentile = 0.10;
    double noise_percentile_high = 0.40;

    // How far above the estimated level a bin may sit and still be averaged
    // into the floor, in standard deviations of the averaged noise.
    //
    // Adaptive rather than a fixed number of decibels, because what counts as
    // "above the noise" depends entirely on how long the frame has been
    // integrated. At a second of averaging the noise holds to a few tenths of
    // a decibel and a signal one decibel up is plainly a signal; at a tenth
    // of a second the same bin is ordinary. A fixed 10 dB window, which this
    // had first, quietly averaged a 4.6 dB wide signal into its own floor and
    // cost 3 dB off its reported SNR.
    double noise_excision_sigma = 4.0;

    // ---- the summed-bin search -------------------------------------------

    // Widest sum, in fine bins. Zero derives an eighth of the frame rounded
    // down to a power of two, which at 2.4 MS/s over 65536 bins is 300 kHz
    // and covers a wideband FM broadcast.
    std::uint32_t max_sum_bins = 0;

    // Ceiling on how many scale-space peaks one decision considers, so a
    // pathological frame cannot make this allocate without bound. Zero
    // derives one per fine bin, which is what the shipped configuration uses.
    //
    // The peaks kept are the STRONGEST this many, not the first this many.
    // That distinction is the whole of it, and getting it wrong cost a
    // broadcast station: the ladder is walked narrowest rung first, so a
    // budget spent in arrival order is spent entirely on the narrowest rungs
    // and the widest ones are never searched. See the note in
    // find_candidates() for what that measured like on the radio.
    //
    // With that fixed this is a sensitivity knob rather than a correctness
    // one: no setting can lose a wide signal to a narrow one, and a small one
    // can still lose a WEAK signal to a loud one, because a very peaky
    // emitter spends the budget on its own windows.
    //
    // One per bin rather than a round number. The ladder's rungs are powers
    // of two, so the positions it can report sum to under twice the bin
    // count whatever the frame holds; a budget of one per bin is therefore
    // over half of everything that could ever be produced, and it scales with
    // the frame instead of being right at one transform size.
    //
    // Measured this session, 65536 bins, a scene holding eight emitters from
    // 50 Hz to 150 kHz including two with resolvable interior lines:
    //
    //   budget 4096   19.7 candidates per decision, 11.1 ms, the 6.5 kHz
    //                 emitter evicted entirely and the 27 kHz one seen at
    //                 70 percent of the decisions it was transmitting at
    //   budget 16384  28.9 candidates, 11.8 ms, both found, both at 95
    //                 percent
    //   budget 65536  31.0 candidates, 11.5 ms, and the budget stops binding
    //   budget 262144 no change from 65536 on any figure
    //
    // So the cost of the whole range is under half a millisecond a decision
    // against a decision that costs eleven, and the sensitivity it buys is
    // a whole emitter. The frame, not the budget, is what this stage costs.
    std::uint32_t max_peaks = 0;

    // How far above the local noise floor a neighbouring bin must sit to be
    // grown into a detection. Two bars, and the level is the larger of them:
    // edge_floor_sigma standard deviations of the averaged noise, and
    // edge_floor_fraction of the mean per-bin floor, both taken over the seed.
    //
    // The ladder is powers of two, so the window that wins on a 21-bin signal
    // is 16 bins and sits wholly inside it. Every position from bin 0 to bin
    // 5 of that signal scores identically, so the window's placement inside
    // the signal is arbitrary and its edges are the ladder's, not the
    // signal's. Growing outward until the excess reaches the noise is what
    // turns a detection back into a measurement; the occupied-bandwidth trim
    // below then takes off whatever the growth overshot.
    //
    // The sigma bar carries it, and it is adaptive for the same reason
    // noise_excision_sigma is. What a growing edge has to clear is the
    // averaged noise's RIPPLE, not the averaged noise's POWER, and the two
    // are a fixed ratio only at one integration time. The spread the floor
    // estimator already measures between its two percentiles is that ripple,
    // per bin, in the same linear units as the floor: at the shipped second
    // of averaging it is about four percent of the floor, and at the fiftieth
    // of a second some cases run at it is a quarter of the floor. A level
    // stated as a multiple of it therefore means the same thing at every
    // integration time, which a level stated as a multiple of the floor does
    // not.
    //
    // Three is the multiple. It is the same argument the birth rule makes
    // about per-decision false alarms: growth steps over a run shorter than
    // split_gap_bins, so stopping takes eight consecutive bins under the
    // level, and three sigma makes a noise bin clearing it a one-in-a-
    // thousand event.
    //
    // edge_floor_fraction is a backstop under the sigma bar, for the case a
    // long average drives the ripple down toward the interpolation error in
    // floor_ itself. Zero switches the backstop off and leaves the sigma bar
    // alone; zero in both switches growth's level off entirely, which is what
    // a case isolating some other behaviour wants.
    //
    // WHAT THIS PARAGRAPH USED TO SAY, FIRST RETRACTION. This knob was
    // edge_excess_fraction, default 0.25, and the level was that fraction of
    // the SEED'S OWN mean excess: "how far below a detection's own mean
    // per-bin excess a neighbouring bin may sit and still be grown into it".
    // The reasoning above about the ladder's edges being arbitrary was and is
    // right. The bar it set was wrong, and it was wrong in a way that only a
    // shaped spectrum exposes. A signal-relative level is high when the
    // signal is loud: measured on the 20 dB ladder it landed 27.7 to 35.2
    // times the local noise floor, so growth stopped wherever the signal fell
    // 6 dB under its own mean rather than where the signal ended. A root
    // raised cosine at rolloff 0.5 spends two thirds of its occupied band in
    // transition skirt, so growth stopped well inside it: the 166.5 kHz
    // emitter's 4096-bin rung grew by exactly zero bins against 4546 bins of
    // truth on 128 of 128 decisions, and the 30 kHz emitter's 512-bin rung
    // reached 650 against 819. The skirt left outside the accepted band was
    // then found again on each side as its own candidate, which is the
    // three-way split the bar recorded at rolloff 0.5.
    //
    // Simply lowering the old fraction does not work and was measured too.
    // The lowest value that clears the 166.5 kHz emitter is about 0.001, and
    // there the tone-driven 16 kHz NFM comb goes from 5 track ids to 7 with
    // spill rising from 0.00 to 0.44, because growth with no absolute floor
    // under it crosses a Bessel comb's nulls. The two requirements pull in
    // opposite directions on a signal-relative bar and in the same direction
    // on a noise-relative one.
    //
    // WHAT THIS PARAGRAPH USED TO SAY, SECOND RETRACTION. The replacement was
    // edge_floor_fraction alone, default 1.0, and this block said: "One is
    // the mean floor itself, so growth stops at 3 dB over the floor. Measured
    // 2026-09-20 on the keyed ladder, 0.25, 0.5, 1.0, 2.0 and 4.0 all produce
    // the same tracks at all three rolloffs, so this is not a tuned number."
    // Do not act on that sentence. The sweep was run at one in-band SNR and
    // it certified insensitivity there, not independence of level, and the
    // bar it was run against could not see the case where the number bites.
    //
    // A flat emitter's per-bin excess over the floor IS its SNR in its own
    // occupied bandwidth, exactly, whatever the bin width or the bandwidth.
    // Every scene emitter in tests/detect was at 20 or 30 dB in occupied
    // bandwidth, so all five swept values sat 14 to 26 dB below the signal's
    // own flat top and none of them could stop growth anywhere but in the
    // skirt. At 0 dB in occupied bandwidth a level of 1.0 stops growth at bin
    // one: measured on the unit grid, a 48-bin rectangular emitter at 12 dB
    // in the 2500 Hz reference is 0.825 of the floor per bin, it grew by
    // exactly zero bins, and the accepted band stayed at the 32-bin ladder
    // rung that won it. 32 kHz reported against 48 kHz of truth, coverage
    // 0.67, under the 0.70 the scene bar itself enforces. The crossover was
    // 12.8 dB in the reference bandwidth at that width and 17.8 dB for a
    // 149.85 kHz station, so the whole weak-and-wide corner was lost.
    double edge_floor_fraction = 0.0;
    double edge_floor_sigma = 3.0;

    // Fraction of a detection's excess power its reported bandwidth holds.
    // The ITU occupied-bandwidth definition, which is a measurement rather
    // than a threshold in decibels that would mean something different for
    // every shape.
    double occupied_power_fraction = 0.99;

    // Two adjacent signals produce more deflection summed together than
    // either does alone, so the search prefers the pair and hands back one
    // band spanning both. An interior run of at least this many bins sitting
    // within split_gap_db of the local floor is taken as a real gap and
    // splits the band.
    //
    // Eight bins rather than one: RTTY is two tones about 170 Hz apart, which
    // is under one bin at 305 Hz and under five at 36.6 Hz, and splitting a
    // two-tone signal into two detections is the wrong answer for the same
    // reason merging two stations is.
    //
    // IT IS A BIN COUNT AND ITS REASON IS A FREQUENCY, WHICH MAKES IT WRONG ON
    // A FINER GRID BY WHATEVER THE RATIO IS. This is the one constant in this
    // struct that does not scale with the geometry and cannot be derived from
    // it, and Engine::open_source now chooses a finer transform when a source
    // asks for one, so the mismatch is reachable rather than hypothetical.
    //
    // Worked through, because the size of it is the point. The paragraph above
    // defends RTTY at 36.6 Hz per bin, where 170 Hz is 4.6 bins and eight has
    // margin. An HF source asking for PSK31 at four bins across does not get a
    // longer transform: dsp::kMaxSpectrumTransform is 2048 and the shipped
    // default is already there. The CHANNEL COUNT is where HF resolution comes
    // from. On the 96 kS/s recordings in docs/recordings.md, at 64 channels,
    // which revenant-cli asked for there until 2026-09-23, the grid is
    // 1.465 Hz per bin and 170 Hz is 116 bins; left to Engine::open_source,
    // as revenant-cli now leaves them, they open on 16 channels at 5.86 Hz,
    // where it is 29. Either way eight is a gap RTTY's own two tones
    // clear, and docs/detection.md has the synthetic RTTY that does split.
    //
    // WHAT THIS PARAGRAPH USED TO SAY: "An HF recording asking for PSK31 at
    // four bins across gets a 16384-point transform, about 1.9 Hz per bin:
    // 170 Hz is then 89 bins". No transform that long can be built. The
    // twiddle table shared with the channelizer caps it at 2048, so the
    // conclusion held and the mechanism it named did not.
    //
    // WHY THIS IS NOT FIXED BY MAKING IT A FREQUENCY. A single frequency is
    // wrong across bands in the other direction. Eight bins at 36.6 Hz is about
    // 293 Hz; carried to HF as a frequency it would refuse to split two FT8
    // signals 60 Hz apart, and a crowded FT8 sub-band is exactly where they sit
    // that close. The honest rule is relative to the narrowest signal worth
    // telling apart, which is source::ResolutionRequest::narrowest_signal_hz
    // and is already plumbed as far as the grid. Applying it here changes what
    // the detector does at the shipped VHF geometry: narrowest is 12.5 kHz
    // there, half of it is 170 bins, and this is eight.
    //
    // So it is left alone and recorded. Re-tuning it means measuring against
    // real HF, and there is nothing to measure against: docs/detection.md's
    // scoring needs ground truth and a recording carries none. Set this per
    // band from the caller until then; the field is configurable for exactly
    // that reason.
    std::uint32_t split_gap_bins = 8;
    double split_gap_db = 1.0;

    // ---- the tracker -----------------------------------------------------

    // Consecutive decisions a candidate must be seen at before it becomes a
    // track. docs/detection.md: this is the actual false-alarm knob and it
    // belongs next to the threshold, because the threshold alone is a
    // per-decision figure and this is what turns it into a rate.
    std::uint32_t birth_hits = 3;

    // Refuses a birth past this many tracks IN ANY STATE, counted as
    // DetectorStats::births_refused rather than silently ignored.
    //
    // WHAT THIS COMMENT USED TO SAY: "refuses a birth past this many LIVE
    // tracks". It is not live ones, and the difference is not small.
    // update_tracks tests the survivor list it is building, which carries
    // Pending, Live, Held and Merged, so the bound is reached by a span
    // whose tracks are all still pending or all being held through a silence
    // exactly as it is by a span of live ones. tracks() leaves Pending out
    // altogether, so the published list can be well under this number at the
    // decision that refuses a birth.
    //
    // Counting every state is the right shape, because the bound is on the
    // allocation and a track of any state costs the same. What was wrong was
    // the description, which invites sizing this against how many signals
    // are expected to be transmitting at once. Size it against how many
    // bands can be in play, including the noise crossings that are born
    // Pending and discarded a decision later.
    std::uint32_t max_tracks = 512;

    // How long a track survives with no evidence at all.
    //
    // NOT per classification, which docs/detection.md calls out as circular:
    // a track has to survive the silence long enough to be classified, so
    // setting the hold from the mode means the mode is never learned. FT8 is
    // the case that fixes the number: it transmits 12.6 seconds in every 15
    // and is silent for 2.4, so anything under about three seconds kills the
    // track before a classifier could ever run.
    //
    // WHAT THIS PARAGRAPH USED TO SAY AT ITS END: "It is narrowed once a
    // classification exists, and nothing sets one yet." Tier two sets one
    // now, through record_probe, and this is still not narrowed by it. A
    // family is not a mode, a hold per mode is a table nobody has measured,
    // and docs/detection.md records that nothing a probe finds changes what
    // the detector does.
    double bootstrap_hold_seconds = 3.0;

    // ---- telling a stopped signal from its own echo -----------------------

    // When a transmission stops, its energy does not leave the integrated
    // spectrum; it decays out of the exponential average. The detection test
    // cannot tell the difference, so the band keeps clearing the threshold
    // and keeps being published as a live detection for as long as the
    // residual stays above it. That is (snr_2500_db - detection_threshold_db)
    // divided by the decay rate, and on a broadcast station it is far longer
    // than the hold: measured 2026-09-20 on a 111 kbaud QPSK emitter at 30 dB
    // in 149.85 kHz, the row stayed Live for 9.06 s after the emitter
    // stopped, then Held for 2.95 s. Twelve seconds of published row against
    // a three second hold, the first nine of it at confidence 1.00 with
    // silent_samples() at zero, so nothing on the wire marked it stale.
    //
    // The discriminator is the decay rate itself. An exponential average with
    // time constant average_seconds falls at 10/ln(10) / average_seconds dB
    // per second once its input goes to zero, and nothing else falls at
    // exactly that rate. Measured 4.26, 5.39 and 11.0 dB/s at average_seconds
    // 1.0, 0.8 and 0.4 against a predicted 4.34, 5.43 and 10.86: within two
    // percent over a factor of 2.5 in the time constant.
    //
    // A candidate is suppressed when its band's mean per-bin excess has
    // fallen at residual_decisions consecutive decisions, AND the total fall
    // over that run is within residual_rate_tolerance, relatively, of what
    // pure decay would produce over the same span of source seconds, AND no
    // one of the kResidualParts parts of the band has fallen by less than
    // kResidualPartFloor of what the band as a whole fell by. Suppression is
    // at the candidate and not at the track on purpose: a track-level rule
    // leaves the candidate free, and a new track is then born from it every
    // time the old one times out.
    //
    // THE PART TEST IS NOT A REFINEMENT. Without it the rule silences
    // transmitters that never stopped, and the table further down cannot see
    // it happen, because every cell of that table is one emitter. See TWO
    // EMITTERS below.
    //
    // Measured over a band fixed at the run's start rather than over the
    // candidate's current extent. snr_2500_db scales with the extent the
    // search happened to accept that decision, so a band that splits, or one
    // whose skirts drop under the growth level as it decays, registers a fall
    // that is geometry and not decay. The mean excess over a fixed support
    // has no such term, and a split then shows up as one step of the wrong
    // SIZE, which the window below rejects.
    //
    // A WINDOW AND NOT A FLOOR, AND WHY THAT IS THE WHOLE RULE
    //
    // The tolerance is two-sided. An earlier form took any fall of at least
    // 0.6 of the predicted one, on the argument that pure decay is the
    // FASTEST the average can fall so anything faster also has no input. The
    // argument is sound about the average and does not carry to the measured
    // statistic, which also moves with the band: a fall faster than predicted
    // is measurement or geometry, not a louder silence. And the lower half of
    // that rule is what does the damage, because it catches every real fade
    // slower than the average's own rate as well.
    //
    // So the run's rate has to LAND ON the predicted rate rather than merely
    // reach some fraction of it. The residual lands within two percent of it,
    // measured above, and a quarter is a wide window around that. Measured
    // 2026-09-20 on the three-station scene, the width is inert from 0.10 to
    // 0.50: all three stations stop being published 3.27 to 3.30 s after they
    // stop, at every one of those five settings.
    //
    // A run whose rate leaves the window starts again at the decision that
    // left it, rather than carrying its history forward. Ripple alone
    // produces a fall about half the time, so four consecutive falls turn up
    // on their own once in sixteen decisions per band, and a run that began
    // that way before a real stop drags flat seconds into the rate. That is
    // what the insensitivity above rests on: without the restart the first
    // station stayed Live for 1.11 s against 0.33 for the other two, and the
    // width had to reach 0.50 before it caught up.
    //
    // The window is relative to the predicted fall, so it scales with the
    // run's span in SECONDS and not with its length in decisions. A run that
    // is four decisions of a fast decision interval spans little time, the
    // predicted fall over it is small, and the absolute tolerance is small
    // with it, so ripple keeps the rule from firing until the run is long
    // enough to be sure. residual_decisions is a floor under the run, not the
    // thing that sets its noise immunity.
    //
    // Zero decisions switches the rule off entirely, which is what a case
    // isolating some other behaviour wants. A tolerance of zero also switches
    // it off, since no measured fall equals the prediction exactly. A
    // tolerance of one switches it fully ON, because the lower edge of the
    // window reaches zero and every fall inside the run qualifies. The two
    // ends of the range are not symmetric and the upper one is the dangerous
    // one.
    //
    // WHAT THIS PARAGRAPH USED TO SAY, TWICE. Two statements of this rule's
    // cost have now been wrong, and the second was wrong in the same way as
    // the first: one reachable shape measured, and the result written down as
    // though it governed all of them.
    //
    // FIRST CLAIM. The knob was residual_decay_fraction, default 0.6, the
    // test was one-sided, and this block said: "The cost of a false positive
    // is bounded and small. A genuine signal fading at that rate is
    // suppressed, its track goes Held rather than being dropped, and the
    // first decision at which the SNR stops falling publishes the candidate
    // again and the track keeps its id. So the failure mode is a momentary
    // Held state, not a lost track."
    //
    // WHY IT WAS WRONG. That held only while the fade was shorter than the
    // hold. At the shipped average_seconds the old bar was 0.6 * 4.343 =
    // 2.606 dB/s sustained, so a signal fading 8 dB over 3 s, which is
    // ordinary mobile VHF or an HF evening, was withheld from the fourth
    // consecutive falling decision onward. update_tracks then reached
    // silent > hold_samples three seconds later and DROPPED the track, and
    // because the suppression sits on the candidate no replacement was born
    // either, so a transmitter still 20 dB over the noise floor left the list
    // entirely. Both bars asserted that outcome as a pass, because both only
    // counted ids appearing.
    //
    // SECOND CLAIM, which was the correction to the first. "A 33-bin emitter
    // starting 60 dB over the reference bandwidth keeps its track through a
    // fall of 4, 8, 13, 20 and 30 dB over three seconds, and loses it at
    // 40 dB over three seconds: 13.3 dB/s, three times the rate an emptying
    // average falls at."
    //
    // WHY THAT WAS WRONG TOO. Every rung of that ladder ran for three
    // seconds, so it moved depth and rate together along one axis and then
    // reported the answer as a rate. It is not a rate, and 13.3 dB/s is about
    // four times too generous for anyone sizing a fade budget from it. The
    // 30 dB rung was not even a survival: it had 1.0 s of scene after the
    // fade, which stops the frames 0.09 s before the hold expires, so it
    // recorded a track that had not been dropped YET. Given a tail long
    // enough for the hold to run, that same fade is starved for 8.22 s and
    // the track goes.
    //
    // WHAT IS ACTUALLY GOING ON. The cost is paid in seconds of starvation
    // against bootstrap_hold_seconds, and depth and rate set different halves
    // of it. Feed an exponential average of time constant tau an input
    // falling at b nepers per second and it solves to
    //
    //     A(t) = k P0 exp(-b t) - (k - 1) P0 exp(-t / tau),  k = 1/(1 - b tau)
    //
    // so the average sits above the input throughout and the measured fall
    // TRAILS the input's, rising towards it rather than starting there.
    //
    //   RATE decides whether the measured rate can reach the window at all.
    //   It converges to the input rate when b tau < 1 and to 1/tau when
    //   b tau >= 1, so a fade slower than the window's lower edge,
    //   0.75 * 4.343 = 3.26 dB/s at the shipped second, never reaches it at
    //   any depth.
    //
    //   Past that edge it converges, but converging takes several time
    //   constants and a fade only lasts depth/rate, so being able to reach
    //   the window is not the same as having time to. Read the 8 dB row
    //   below: 0.00 s starved at 3.60, 4.34 and 6.00 dB/s, every one of them
    //   past the edge. At 6.00 the fade is over in 1.33 s with the measured
    //   rate still at 3.07 dB/s and the rule has never fired.
    //
    //   An earlier draft of this bullet said a fade faster than the edge
    //   reaches the window "eventually whatever its depth", which the table
    //   three lines below it contradicts. That is the THIRD statement of
    //   this rule's cost to over-generalise one axis, after the two
    //   retracted above, so treat the table as the answer and this prose as
    //   a reading of it.
    //
    //   DEPTH decides how long it stays fired. When the fade stops, the
    //   average is left above the input by however far it lagged, which grows
    //   with the depth, and it goes on emptying at its own rate: dead centre
    //   of the window. So suppression outlives the fade, and the total is
    //   what the three second hold is spent against.
    //
    // THE MEASURED ANSWER, swept over both axes. Same 33-bin emitter at 60 dB
    // on the unit grid; cells are the longest stretch in seconds with no
    // candidate published, and * marks a lost track. "how long the residual
    // rule starves a fade" in tests/detect/test_detector.cpp is this table and
    // re-measures every cell of it on each run.
    //
    // EVERY CELL IS ONE EMITTER FADING, and that is a property of the table
    // and not of the rule. Read it as the cost of the rule to a signal that
    // is alone in its band. TWO EMITTERS below is the other case and it is
    // not on this surface anywhere.
    //
    //   depth  2.00   3.00   3.20   3.30   3.60   4.34   6.00  10.00  13.30 dB/s
    //       8  0.00   0.00   0.00   0.00   0.00   0.00   0.00   0.10   0.42
    //      13  0.00   0.00   0.00   0.00   0.00   0.00   1.14   2.18   2.50
    //      16  0.00   0.00   0.00   0.00   0.00   0.62   2.50   3.43*  3.64*
    //      20  0.00   0.00   0.00   0.00   0.00   2.18   3.95*  4.89*  5.10*
    //      25  0.00   0.00   0.00   0.00   1.56   4.06*  5.72*  6.55*  6.76*
    //      30  0.00   0.00   0.00   0.00   3.43*  5.82*  7.38*  8.22*  8.32*
    //      40  0.00   0.00   0.00   0.52   6.86*  9.15* 10.61* 10.92* 10.19*
    //
    // Read it as two statements rather than as one number.
    //
    //   Below 3.26 dB/s the rule never fires, at any depth. That is the only
    //   part of the surface a rate on its own describes, and it is what to
    //   size against when the depth is not known in advance.
    //
    //   Above it the budget is a DEPTH, between 13 and 25 dB, narrowing as
    //   the rate rises: 25 dB survives at 3.6 dB/s, 20 dB at 4.34, 16 dB at
    //   6.0 and 13 dB at 10.0 and beyond. A 30 dB fade loses its track at
    //   every rate above 3.6 dB/s, including the ones the old bar called
    //   safe.
    //
    // A signal fading deeper than that is leaving, and losing it is still a
    // lost track rather than a momentary Held one. What is no longer claimed
    // is that anyone can tell which case they are in from the rate.
    //
    // TWO EMITTERS, which is the case the surface above cannot reach and the
    // one the part test exists for.
    //
    // Two signals closer together than split_gap_bins come back as ONE
    // candidate. When the loud one stops, the mean over that band falls at
    // exactly the rate an emptying average falls at, because the loud one
    // dominates the sum, and it goes on doing so until the loud one's
    // residual has come down to the quiet one's level: the whole separation
    // in decibels, at 4.343 dB/s, which at 20 dB is 4.6 s against a 3.0 s
    // hold. The candidate withheld through all of that is the one describing
    // the quiet emitter, which never stopped. It is dropped from tracks(),
    // nothing is reborn from it because the suppression sits on the
    // candidate, and candidates_residual increments the whole time, which
    // reads as the rule working.
    //
    // Measured 2026-09-20, "a transmitter beside one that stopped is not
    // withheld with it" in tests/detect/test_detector.cpp: a 40 dB emitter
    // seven bins wide, two bins of bare noise from a 33-bin emitter at 60 dB
    // that stops. Without the part test, 5.51 s with no candidate over the
    // quiet emitter and 2.81 s with no track over it at all. With it, 0.00
    // and 0.10, and the 0.10 is the merged track's id going to the loud one's
    // remnant when the band finally splits, which is the merge rule and not
    // this one.
    //
    // The part test costs the single-emitter surface above nothing: every
    // cell of that table is the same after it as before, because a lone
    // emitter's band empties everywhere at once and every part of it passes.
    double residual_rate_tolerance = 0.25;
    std::uint32_t residual_decisions = 4;

    // Minimum band overlap, as a fraction of the narrower band, for a
    // candidate and a track to be considered the same signal.
    double association_overlap = 0.3;

    // Confidence rises by this fraction of its remaining distance to one on
    // every detection, and halves over this many seconds of silence. A track
    // is dropped when it falls below drop_confidence or when it exceeds its
    // hold, whichever comes first.
    double confidence_rise = 0.35;
    double confidence_half_life_seconds = 1.0;
    double drop_confidence = 0.05;

    // How much of a measurement a track takes per decision. One is no
    // smoothing.
    double centre_smoothing = 0.4;

    // Extra distance past the halfway point, in channel widths, that a track
    // must move before it changes coarse channel. Zero is the bare rounding
    // place() does and is what flips on noise.
    double channel_hysteresis = 0.15;
};

struct DetectorStats {
    std::uint64_t frames = 0;
    std::uint64_t decisions = 0;

    // Frames refused because their geometry was not the one create() was
    // configured against. The spectrum stage is built once with the coarse
    // chain, so this should stay zero and a non-zero value means two engines.
    std::uint64_t frames_rejected = 0;

    // Candidates PUBLISHED, summed over decisions, which is what
    // Detector::candidates() held at each of them. Add candidates_residual
    // for what the search produced before the residual rule filtered it.
    std::uint64_t candidates = 0;
    std::uint64_t tracks_born = 0;
    std::uint64_t tracks_dropped = 0;
    std::uint64_t births_refused = 0;
    std::uint64_t merges = 0;
    std::uint64_t splits = 0;

    // Decisions where the scale-space search produced more peaks than
    // max_peaks, so the weakest were evicted. Not a fault: the set kept is
    // the strongest that many and a wide signal cannot be squeezed out by
    // narrow ones. It says the frame is busy enough that raising max_peaks
    // would change what is reported.
    std::uint64_t peaks_overflowed = 0;

    // Candidates withheld because their SNR was falling at the exponential
    // average's own decay rate, so the energy behind them was a residual
    // rather than a transmission. Counted per candidate per decision, so a
    // station that stops once contributes one per decision for the rest of
    // its decay.
    std::uint64_t candidates_residual = 0;

    // Times the residual rule's part test was consulted on a support
    // narrower than kResidualParts, so its parts are one bin each and
    // several of them name the same bin.
    //
    // Not a fault and not a refusal: the test still runs and still answers.
    // It is here because the answer is weaker than the one the rule was
    // measured with and nothing else says so. A part's measured level
    // carries the averaged spectrum's ripple, which is 0.27 dB on one bin
    // against 0.10 dB on eight at the shipped second of averaging, and the
    // shortest run the rule acts on falls about 1.7 dB, so a one-bin part
    // decides a 0.85 dB question with a third of that in noise on it. A run
    // of these against a band the operator expected to be suppressed, or
    // suppressed a band they expected to keep, says the candidate was too
    // narrow for the test rather than that the rule disagreed with them.
    //
    // Counted per candidate per decision, the same as candidates_residual,
    // so the two are comparable.
    std::uint64_t residual_parts_degenerate = 0;
};

class Detector {
public:
    [[nodiscard]] static Expected<Detector> create(const DetectorConfig& config,
                                                   const engine::SpectrumGeometry& geometry);

    Detector(Detector&&) noexcept = default;
    Detector& operator=(Detector&&) noexcept = default;
    Detector(const Detector&) = delete;
    Detector& operator=(const Detector&) = delete;
    ~Detector() = default;

    // Takes one frame. Always accumulates; decides only when decision_seconds()
    // of source time has passed since the last one.
    //
    // WHAT THIS COMMENT USED TO SAY: "decides only when
    // decision_interval_seconds of source time has passed since the last one."
    // Where one spectrum window is longer than that, the window is what has
    // to pass; see decision_interval_seconds.
    //
    // frame.power_db is read during the call and never retained.
    [[nodiscard]] Status consume(const engine::SpectrumFrame& frame);

    // Live, Held and Merged tracks, ascending in frequency. Pending ones are
    // not tracks and are not here. Valid until the next consume().
    [[nodiscard]] std::span<const Track> tracks() const { return tracks_; }

    // What the most recent decision PUBLISHED, ascending in frequency. Empty
    // until the first decision.
    //
    // WHAT THIS COMMENT USED TO SAY: "what the most recent decision found".
    // It is not, and the difference is the residual rule. The search's own
    // output is filtered by reject_residual before anything else sees it, so
    // a band withheld as an exponential average's own echo is absent here as
    // well as from tracks(). What the search found is this many plus that
    // decision's share of DetectorStats::candidates_residual.
    [[nodiscard]] std::span<const Candidate> candidates() const { return candidates_; }

    [[nodiscard]] const DetectorStats& stats() const { return stats_; }
    [[nodiscard]] const DetectorConfig& config() const { return config_; }

    // The sample index the most recent decision was taken at, which is one
    // past the last sample it had seen.
    [[nodiscard]] dsp::SampleIndex last_decision() const { return last_decision_; }

    // Source seconds between decisions as run: decision_interval_seconds, or
    // one spectrum window where that is longer. See decision_interval_seconds.
    [[nodiscard]] double decision_seconds() const { return decision_seconds_; }

    // The operator's two knobs, both changeable while running. Validated,
    // because a NaN threshold detects everything or nothing depending on
    // which way the comparison was written.
    [[nodiscard]] Status set_thresholds(double detection_threshold_db,
                                        double confidence_threshold);

    // Records what a tier-two probe found on one track, by id, into
    // Track::last_probe, and into Track::classification only when the
    // finding may drive detection. See Track::classification for the rule.
    //
    // Visible in tracks() at once rather than at the next decision, and
    // carried from decision to decision like every other field of a track.
    // A merge or a split does not move it: a new id from a split starts
    // Unknown, because the band it was measured over is not the band the
    // new track describes.
    //
    // Refused for an id that is not a track any more, which is the ordinary
    // case of a probe outliving its signal. Nothing the detector decides
    // reads any of it.
    [[nodiscard]] Status record_probe(std::uint64_t track_id, const ProbeFinding& finding);

    // The integrated power spectrum the decision works on, linear, one entry
    // per fine bin, and the noise floor estimated under it. Both are for
    // tests and for a display that wants to draw the floor; neither is part
    // of the detection contract.
    [[nodiscard]] std::span<const double> averaged_power() const { return average_; }
    [[nodiscard]] std::span<const double> noise_floor() const { return floor_; }

private:
    Detector() = default;

    void accumulate(const engine::SpectrumFrame& frame, double alpha);
    void decide(dsp::SampleIndex now, double elapsed_seconds);
    void estimate_noise_floor();
    void find_candidates();

    // Measures [first, last] and pushes it if it still clears the threshold.
    // Says whether it did, because the gap split has to know whether any of
    // its segments survived before it falls back to the whole band.
    //
    // own_low and own_high bound the region this band may look at when it
    // measures its skirts: the half-open run between the neighbouring accepted
    // peaks. They are not the band, they are how far outside it the shape
    // measurement is allowed to read before it would be reading somebody else.
    [[nodiscard]] bool emit_candidate(std::size_t first,
                                      std::size_t last,
                                      std::size_t own_low,
                                      std::size_t own_high);

    // Drops the candidates whose energy is the exponential average's own
    // residual. See residual_rate_tolerance.
    void reject_residual(double elapsed_seconds);

    void update_tracks(dsp::SampleIndex now, double elapsed_seconds);
    void update_channel(Track& track) const;

    // One position and width the summed-bin search liked, before the greedy
    // pass decides which of the overlapping ones survives.
    struct Peak {
        std::uint32_t start = 0;
        std::uint32_t width = 0;
        double deflection = 0.0;
    };

    DetectorConfig config_{};
    engine::SpectrumGeometry geometry_{};

    std::size_t bins_ = 0;
    double bin_width_hz_ = 0.0;
    double bin_zero_hz_ = 0.0;

    // Derived once so the per-decision path has no divisions or powers in it.
    double threshold_linear_ = 1.0;
    double gap_ratio_ = 1.0;

    // Standard normal quantiles at the two noise percentiles: their
    // difference scales a measured spread into a standard deviation, and the
    // upper one carries that back to the mean.
    double quantile_span_ = 1.0;
    double high_quantile_ = 0.0;
    std::uint64_t decision_interval_samples_ = 1;

    // One transform's duration, 1 / bin width, and the decision interval in
    // force, which is the configured one or the window, whichever is longer.
    double window_seconds_ = 0.0;
    double decision_seconds_ = 0.0;
    std::uint64_t warmup_samples_ = 1;
    std::size_t noise_window_bins_ = 0;

    // config_.max_peaks with the zero-derives-from-the-frame rule applied,
    // the same way widths_ carries the derived form of max_sum_bins. The
    // configuration keeps what the caller asked for so that config() does
    // not report a number nobody set.
    std::uint32_t peak_budget_ = 1;
    std::vector<std::uint32_t> widths_;

    // Integrated linear power per bin, the floor under it, and the standard
    // deviation of that floor's own ripple. All three are per fine bin and
    // the last two are interpolated between knots. sigma_ is what the growth
    // level is stated in, per edge_floor_sigma.
    std::vector<double> average_;
    std::vector<double> floor_;
    std::vector<double> sigma_;

    // average_ minus floor_, clamped at zero, which is the frame every shape
    // measurement is taken in. Materialised rather than subtracted on demand
    // because measure_band walks OUTSIDE a band and a caller cannot hand it a
    // sub-span of something that does not exist. Built in the same pass as the
    // prefix sums below, so it costs one store per bin.
    //
    // CLAMPED WHERE excess_cumulative_ IS SIGNED, and find_candidates says why
    // at the line that fills both.
    std::vector<double> excess_;

    // Total weight the exponential average has accumulated, which is what
    // keeps the first frames from dominating it. Rises from alpha toward one.
    double weight_ = 0.0;

    // Prefix sums of excess over the floor, of the floor itself and of its
    // ripple, so a summed-bin score or a growth level at any position and
    // width is two subtractions.
    std::vector<double> excess_cumulative_;
    std::vector<double> floor_cumulative_;
    std::vector<double> sigma_cumulative_;

    // Per-position scratch for one width at a time.
    std::vector<float> deflection_;

    // Noise estimation scratch: one window's worth, plus the knot values and
    // their bin positions.
    std::vector<double> window_;
    std::vector<double> knot_floor_;
    std::vector<double> knot_sigma_;
    std::vector<double> knot_position_;

    // Which bins of a refined band are a gap wide enough to split it.
    std::vector<std::uint8_t> separator_;

    std::vector<Peak> peaks_;
    std::vector<Peak> accepted_;

    std::vector<Candidate> candidates_;

    // One band the previous decision measured, carried forward so this
    // decision can ask whether it is still falling. Held per band rather than
    // per track because the suppression has to happen before anything has an
    // identity, and rebuilt every decision: an entry no candidate matches is
    // stale and goes.
    struct Decaying {
        // The candidate's own band this decision, which is what the next
        // decision matches against by overlap.
        std::uint32_t first_bin = 0;
        std::uint32_t last_bin = 0;

        // The band the run's statistic is measured over, fixed at the
        // decision the run started. A fall has to be the power falling and
        // not the accepted extent moving, so the support cannot move with
        // the measurement.
        std::uint32_t run_first_bin = 0;
        std::uint32_t run_last_bin = 0;

        // Mean per-bin excess over run_first_bin..run_last_bin, in decibels:
        // the previous decision's, which is what "still falling" is measured
        // against, and the one the current run of falls started from with the
        // source seconds it has spanned, which is what the rate is measured
        // over.
        double level_db = 0.0;
        double run_level_db = 0.0;
        double run_seconds = 0.0;
        std::uint32_t run = 0;

        // The same level at the run's start, over each equal part of the
        // support taken on its own. An average with no input empties
        // everywhere at once, so every part falls by what the whole falls
        // by; a band holding one dead emitter and one live one does not, and
        // the mean cannot see the difference. See
        // DetectorConfig::residual_rate_tolerance.
        std::array<double, kResidualParts> run_part_db{};
    };
    std::vector<Decaying> decaying_;
    std::vector<Decaying> decaying_next_;

    // Every track including Pending ones. tracks_ is the published subset.
    std::vector<Track> all_;
    std::vector<Track> tracks_;

    // Association scratch, rebuilt per decision.
    struct Pair {
        std::uint32_t track = 0;
        std::uint32_t candidate = 0;
        double score = 0.0;
        dsp::SampleIndex first_seen = 0;
        std::uint64_t id = 0;
    };
    std::vector<Pair> pairs_;
    std::vector<std::uint32_t> track_of_candidate_;
    std::vector<std::uint32_t> candidate_of_track_;

    dsp::SampleIndex previous_start_ = 0;
    dsp::SampleIndex last_decision_ = 0;
    std::uint64_t samples_since_decision_ = 0;
    std::uint64_t samples_accumulated_ = 0;
    bool have_previous_ = false;
    bool have_decided_ = false;

    std::uint64_t next_id_ = 1;
    DetectorStats stats_{};
};

}  // namespace revenant::detect
