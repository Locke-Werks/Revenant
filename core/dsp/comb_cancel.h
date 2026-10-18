// Span-wide comb cancellation: finding a comb of evenly spaced interference
// lines in the whole capture and subtracting it before the channelizer, so the
// spectrum, the waterfall, the detector and every receiver see the cleaned
// stream.
//
// WHAT IT IS FOR. A switching supply, an LED driver, a USB or Ethernet PHY or
// the receiver's own clock puts a train of narrow lines across HF, spaced at
// its switching frequency: dozens to hundreds of lines, each a few dB to 20 dB
// over the floor, all moving together. The lines are one waveform repeating at
// a period, so they can be estimated and subtracted rather than notched, and a
// subtraction costs the programme under a line only what falls inside the
// estimator's bandwidth, a few hertz, where a notch costs the whole bin.
//
// THE MODEL. Line k of the comb sits at baseband frequency
//
//     f_k(t) = c(t) + k * d(t),    k in [k_lo, k_hi]
//
// with c the offset of line 0 and d the spacing. AFFINE, NOT HARMONIC: an
// external source's lines are harmonics n * F of a fundamental, which at
// baseband is c = n0 * F - centre and d = F, so the harmonic case is the affine
// one with c and d tied. Tracking c and d separately covers that case, a comb
// made inside the receiver whose offset is a clock product unrelated to the
// tuning, and a comb that moves with the tuning, without having to know which
// it is first.
//
// THE PERIOD VARIES. A supply's switching frequency moves with its load and
// temperature, so d(t) and c(t) are tracked, not fixed. A change dd in the
// spacing moves line k by k * dd: every line moves, by an amount proportional
// to its index. That proportionality is also what confirms a comb: unrelated
// carriers that happen to sit on a grid do not drift in step with it.
//
// THREE PARTS, AND WHERE EACH RUNS.
//
//   detect     CPU, from a time-averaged power spectrum of the whole span.
//              Finds candidate (c, d, k_lo, k_hi) with no prior: narrow
//              persistent peaks, then the spacing that puts the most of them
//              on one grid, then a least-squares fit of the grid to the peaks.
//              Runs at acquisition and after every retune. detect_comb below.
//
//   track      CPU, per block. Takes each active line's complex projection
//              over each sub-block (from the project kernel), measures each
//              line's phase advance, fits the common model of that advance
//              across lines, alpha + beta * k, weighted by line strength, and
//              steers two loops, one for c and one for d. Also keeps each
//              line's complex amplitude as a slow average of its derotated
//              projection. CombTracker below.
//
//   cancel     GPU, two kernels on the device ring, in place, after the
//              front-end correction and before the impulse excision, which
//              would otherwise cut a pulse-train comb's peaks out as impulses:
//                core/shaders/comb_project.comp   the per-line, per-sub-block
//                                                 projections the tracker reads
//                core/shaders/comb_subtract.comp  synthesises the comb from the
//                                                 tracker's amplitudes and
//                                                 phases and subtracts it
//
// PHASE IS INTEGER. Each line's phase is a uint32 fraction of a cycle and its
// per-sample increment is a uint32 too, so a line's phase at sample n of a
// block is (phase0 + n * step) mod 2^32 with no rounding anywhere, the same on
// the host, the device and the twin. That is the frequency-is-integer rule
// (docs/conventions.md) carried into the oscillator: a uint32 step at 2.4 MS/s
// resolves 0.00056 Hz, and a block of any length accumulates no phase error.
// The top kCombTableLog2 bits index a host-built cosine/sine table, so the
// synthesised tone is bit-identical between kernel and twin. Quantising phase
// to 2^-12 of a cycle bounds the cancellation's depth near 60 dB below each
// line, far below what any comb seen so far stands above the floor.
//
// LATENCY. The tracker reads projections of block b and plans block
// b + frames_in_flight from a prediction, so the loops carry a feed-forward
// frequency term and their bandwidth is set against that delay. A comb moving
// faster than the loops follow is reported as unlocked and left alone rather
// than half-cancelled.
//
// RAW PLUS REMOVED. As in core/dsp/impulse_excision.h, the subtract pass writes
// what it removed beside the cleaned sample, so a client can be shown the comb
// that was taken out.
//
// PROVENANCE, per docs/clean-room.md. The design is this project's own. The
// principle, estimating a periodic interferer and subtracting it rather than
// filtering, is the textbook one of adaptive interference cancellation and of
// synchronous averaging in machinery diagnostics; no document was opened for
// this work and no other program's source was read.

#pragma once

#include <complex>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::dsp {

// ---------------------------------------------------------------------------
// Shared limits
// ---------------------------------------------------------------------------

// The most lines the stage will cancel at once. Sized for a full HF span at a
// switching frequency of 10 kHz and up. A detection can hold more (a comb at
// four bins' spacing across a full span is about 3500 lines); CombTracker
// trims, starting from the kCombMaxLines central to the detected range and,
// once locked, swapping a line that has stood out of nothing for 64 blocks
// for the next untried k outward, so the slots settle on lines that are
// there. core/dsp/comb_tracker.cpp has the reasoning.
inline constexpr std::uint32_t kCombMaxLines = 1024;

// The fewest lines that count as a comb. Fewer than this is a handful of
// carriers, which the stage has no business removing.
inline constexpr std::uint32_t kCombMinLines = 8;

// Phase-to-tone table: 2^12 entries of (cos, sin) of 2 pi i / 4096, built on
// the host in double and rounded to float once.
inline constexpr std::uint32_t kCombTableLog2 = 12;
inline constexpr std::uint32_t kCombTableSize = 1U << kCombTableLog2;

// Sub-blocks per block for the projections. The phase advance between
// adjacent sub-blocks is what the tracker's frequency discriminator reads, so
// the sub-block length sets its pull-in range: +/- rate / (2 * sub-block).
inline constexpr std::uint32_t kCombMaxSubBlocks = 16;

// The most combs cancelled at once, sharing kCombMaxLines between them. The
// owner's HF comb (2026-10-12 dumps) is a 15789.4 Hz comb plus its supply-
// ripple sideband comb 100.7 Hz below, at the same spacing: two grids, which
// one affine model cannot hold together, so it locked to one and left every
// other visible line standing. Each is its own comb here. Interleaved sources
// whose lines share a grid only roughly are split by detect_combs the same
// way. Live on 80 m (2026-10-16) every line also carries a ladder of mains-
// ripple sidebands 59.5 Hz apart, which the tracker cancels with its line
// (CombEstimate::sideband_orders): four orders either side make nine slots a
// line, 684 for the strong family's 76, which is what sizes kCombMaxLines.
inline constexpr std::uint32_t kCombMaxCombs = 8;

// An estimate whose lines sit this much on a held comb's lines is a detection
// of what that comb already is, at another spacing or a sub-grid. Shared by
// the bank, which drops such a detection, and by detect_combs, which does not
// spend a pass on one.
inline constexpr double kCombOverlapFraction = 0.25;

// How close a detected line must be to a held one to be that line. A
// detection from the fine spectrum places a line to a few hertz, a coarser
// one to tens; a mains-ripple sideband sits 59.5 Hz from its carrier and is a
// different line.
inline constexpr double kCombSameLineHz = 45.0;

// The most spurs (CombEstimate::spur) cancelled at once, each a tracker of one
// line drawn from the kCombMaxLines combs leave free. Combs have first claim:
// a comb that needs the slots takes them back from the spurs.
inline constexpr std::uint32_t kCombMaxSpurs = 16;

// ---------------------------------------------------------------------------
// Detection
// ---------------------------------------------------------------------------

// A comb, as found or as tracked. Frequencies in hertz at baseband, as doubles:
// these are estimates with fractional-hertz precision, not tunings, and become
// integer phase steps (comb_step) at the one place they reach the oscillator.
struct CombEstimate {
    double offset_hz = 0.0;   // c, the frequency of line k = 0
    double spacing_hz = 0.0;  // d
    std::int32_t k_lo = 0;
    std::int32_t k_hi = -1;   // inclusive; k_hi < k_lo is no comb

    // Lines in [k_lo, k_hi] that stood out of the floor at detection. A grid
    // with gaps is still one comb; the tracker keeps the gaps as lines it
    // does not cancel until they show.
    std::uint32_t lines_found = 0;

    // Median over found lines of each line's height above the local floor, dB.
    float median_height_db = 0.0F;

    // Modulation sidebands carried by every line: orders -sideband_orders to
    // +sideband_orders at multiples of ripple_hz either side. A switcher whose
    // supply ripples puts them there; the tracker cancels them with the line,
    // since cancelling the carrier alone leaves them standing in the
    // waterfall's bins. Zero orders is a bare comb.
    double ripple_hz = 0.0;
    std::int32_t sideband_orders = 0;

    // A single unkeyed carrier rather than a comb: one line at offset_hz,
    // k_lo == k_hi == 0, spacing_hz zero, exempt from kCombMinLines. The
    // wideband detector names these (detect::Track::spur) and the bank holds
    // each in a tracker of one line, which locks to it in frequency and phase
    // and subtracts it exactly as it does a comb's line.
    bool spur = false;

    [[nodiscard]] std::uint32_t line_count() const
    {
        return k_hi < k_lo ? 0U : static_cast<std::uint32_t>(k_hi - k_lo + 1);
    }
};

// What detect_comb reads: a linear power spectrum of the whole span averaged
// over at least a second, in natural order with DC at bins / 2 (the order the
// spectrum kernel hands the UI), bin i centred on (i - bins / 2) * rate / bins.
struct CombSpectrum {
    ConstRealSpan power;  // linear, any consistent scale
    SampleRate rate = 0;
};

// No comb in the spectrum is std::nullopt, not an error. An error is a
// spectrum that cannot be read: empty, rate not positive, too few bins.
[[nodiscard]] Expected<std::optional<CombEstimate>> detect_comb(const CombSpectrum& spectrum);

// Every comb in the spectrum, at most kCombMaxCombs, strongest first by the
// summed power of its found lines. A grid whose even and odd lines, each
// fitted as a grid of its own, sit more than a quarter bin from where the
// joint grid puts them is two sources interleaved, and is reported as the two
// halves at twice the spacing, each split again by the same test. A true
// single comb's halves agree with it and it is reported whole. After each
// comb is taken its peaks are removed and the rest searched again. An empty
// vector is no comb.
//
// `active` is what the bank already holds (CombBank::active_estimates). The
// detection spectrum is taken before the cancellation, so a held comb stands
// in it at full strength and would win the first passes, only to be dropped
// by the bank as held, while a fresh weaker comb never got a pass. Every peak
// on an active grid or its sidebands leaves the search first, and a grid that
// still comes out mostly on an active comb is taken without counting toward
// kCombMaxCombs, so the passes go to combs that are not yet cancelled. Empty
// `active` is the search as it was.
[[nodiscard]] Expected<std::vector<CombEstimate>> detect_combs(
    const CombSpectrum& spectrum, std::span<const CombEstimate> active = {});

// ---------------------------------------------------------------------------
// The kernels' contract
// ---------------------------------------------------------------------------

// A line's integer phase step for a frequency at a rate: round(f / rate * 2^32)
// taken mod 2^32, so a negative frequency is a step above 2^31.
[[nodiscard]] std::uint32_t comb_step(double frequency_hz, SampleRate rate);

// The table both kernels bind, kCombTableSize (cos, sin) pairs.
[[nodiscard]] std::vector<Complex32> comb_table();

// One active line, as both kernels read it from a storage buffer (std430,
// four packed 32-bit words then a vec2). phase0 is the line's phase at the
// dispatch's first sample.
struct CombLine {
    std::uint32_t phase0 = 0;
    std::uint32_t step = 0;
    std::uint32_t pad0 = 0;
    std::uint32_t pad1 = 0;
    Complex32 amplitude{};  // what subtract removes; project ignores it
};
static_assert(sizeof(CombLine) == 6 * sizeof(std::uint32_t),
              "CombLine must be six packed 32-bit words to alias the kernels' std430 struct");

// Push constants of both kernels, eight packed words.
//
// `first` is the ring slot of the dispatch's first sample, as in
// core/dsp/impulse_excision.h. The project pass covers `count` samples split
// into `sub_blocks` equal sub-blocks (count a multiple of sub_blocks) and
// writes lines * sub_blocks projections, line-major:
//
//     proj[l * sub_blocks + s] = sum over n in sub-block s of
//                                x[first + n] * conj(tone_l(n))
//
// with tone_l(n) = table[(phase0_l + n * step_l) >> (32 - kCombTableLog2)].
// The subtract pass writes, for each n in [0, count),
//
//     removed[n] = sum over l ascending of amplitude_l * tone_l(n)
//     cleaned[n] = x[first + n] - removed[n]
//
// Bit-identical between kernel and twin: the twins transcribe the kernels'
// operation order, as core/dsp/vrx_reference.h requires of every twin.
struct CombParams {
    std::uint32_t ring_mask = 0;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    std::uint32_t lines = 0;
    std::uint32_t sub_blocks = 1;
    std::uint32_t removed_mask = 0;
    std::uint32_t pad0 = 0;
    std::uint32_t pad1 = 0;
};
static_assert(sizeof(CombParams) == 8 * sizeof(std::uint32_t),
              "CombParams must be eight packed 32-bit words to alias the kernels' push "
              "constant block");

[[nodiscard]] Status validate(const CombParams& params);

// Twins of the two kernels. `raw` and `cleaned` are separate so a conformance
// test can compare them; the engine binds one ring as both.
[[nodiscard]] Status reference_comb_project(const CombParams& params, ConstComplexSpan raw,
                                            std::span<const CombLine> lines,
                                            ConstComplexSpan table, ComplexSpan projections);
[[nodiscard]] Status reference_comb_subtract(const CombParams& params, ConstComplexSpan raw,
                                             std::span<const CombLine> lines,
                                             ConstComplexSpan table, ComplexSpan cleaned,
                                             ComplexSpan removed);

// ---------------------------------------------------------------------------
// Tracking
// ---------------------------------------------------------------------------

enum class CombLock : std::uint8_t {
    kIdle,       // nothing detected, nothing cancelled
    kAcquiring,  // detected, loops pulling in, nothing cancelled yet
    kLocked,     // cancelling
    kLost,       // was locked, the model stopped fitting; cancellation off
};

// What the stage reports, for the RPC and the UI indicator.
struct CombStatus {
    CombLock lock = CombLock::kIdle;
    CombEstimate comb{};
    std::uint32_t lines_cancelled = 0;

    // Mean over cancelled lines of the line's power before minus the
    // residual's power at the line after, dB. The figure the UI shows.
    float suppression_db = 0.0F;

    // The loops' current estimate of how fast the spacing is moving, Hz/s.
    float spacing_drift_hz_per_s = 0.0F;
};

// One block's projections, as read back from the project pass of block
// `start`. Line-major, lines * sub_blocks, the lines being exactly those the
// tracker handed out for that block, in that order. sub_blocks may be a
// multiple of what the tracker planned: a projection is a sum over samples, so
// adjacent sub-blocks add exactly into a longer one, and the tracker folds
// them. That is how one dispatch serves trackers that want different lengths.
struct CombObservation {
    SampleIndex start = 0;
    std::uint32_t count = 0;
    std::uint32_t sub_blocks = 0;
    std::span<const Complex32> projections;
};

// What the graph dispatches for one block: the lines with their phases at the
// block's first sample, and the amplitudes subtract removes. Empty `lines` is
// a block the stage leaves alone.
struct CombBlockPlan {
    SampleIndex start = 0;
    std::uint32_t count = 0;
    std::uint32_t sub_blocks = 1;
    std::vector<CombLine> lines;
    bool subtract = false;  // false while acquiring: project only
};

// The host side of the stage. Pure: a function of the detections and
// observations it is handed and of nothing else, no clock, no global, so the
// engine, the behaviour tests and a replay of a capture all drive it the same
// way. Single-threaded: the graph's recording thread owns it.
class CombTracker {
public:
    CombTracker() = default;
    // max_lines is the tracker's share of kCombMaxLines.
    explicit CombTracker(SampleRate rate, std::uint32_t max_lines = kCombMaxLines);

    // Forget everything; the next detection starts acquisition again. Called
    // on every retune and source change.
    void reset();

    // A fresh detection. Ignored while locked to a comb it agrees with; starts
    // acquisition otherwise.
    void detected(const CombEstimate& estimate);

    // The projections of an earlier block's project pass, in block order.
    void observe(const CombObservation& observation);

    // The plan for the block [start, start + count), which has just been
    // written to the ring and is planned before any observation of it.
    [[nodiscard]] CombBlockPlan plan(SampleIndex start, std::uint32_t count);

    [[nodiscard]] CombStatus status() const;

    // Acquiring or locked: holding a comb.
    [[nodiscard]] bool active() const
    {
        return lock_ == CombLock::kAcquiring || lock_ == CombLock::kLocked;
    }

    // The estimate describes the comb this tracker holds.
    [[nodiscard]] bool agrees(const CombEstimate& estimate) const;

    // Fraction of the estimate's lines within tolerance_hz of one of this
    // tracker's lines: a detection of lines already owned, at another
    // spacing, would subtract them twice.
    [[nodiscard]] double overlap(const CombEstimate& estimate, double tolerance_hz) const;

    [[nodiscard]] std::uint32_t lines() const { return static_cast<std::uint32_t>(lines_.size()); }

    // The tracked sideband ripple in hertz, zero for a bare comb.
    [[nodiscard]] double ripple_hz() const;

    // The comb as currently tracked, over the whole range it was detected
    // across and not only the lines holding slots, with its ripple and
    // sideband orders: what detect_combs is told is already held.
    [[nodiscard]] CombEstimate estimate() const;

    // Holding a single spur rather than a comb (CombEstimate::spur).
    [[nodiscard]] bool spur() const { return spur_; }

    // The offset the detection that started it named, before any tracking:
    // the frequency a spur is judged by when deciding whether a receiver is
    // listening to it, the same one the request was filtered on.
    [[nodiscard]] double detected_offset_hz() const { return initial_offset_hz_; }

    // Whether any slot this tracker plans, sidebands included, sits within
    // tolerance_hz of the frequency: what a spur must not be started on, or
    // the line is subtracted twice.
    [[nodiscard]] bool holds_frequency(double frequency_hz, double tolerance_hz) const;

    // An identity the bank gives it, kept across reset and restarts, so the
    // bank can find a tracker's rows in an observation by who planned them.
    void set_id(std::uint64_t id) { id_ = id; }
    [[nodiscard]] std::uint64_t id() const { return id_; }

private:
    // Value members only; the tracker is copyable so a test can fork it.

    // One parameter of the model, c or d, as an exact fixed-point polynomial
    // in the sample index: phase in cycles * 2^64, frequency in cycles per
    // sample * 2^64 and its rate of change in cycles per sample^2 * 2^64, all
    // wrapping mod 2^64, so advancing it by n samples is exact integer
    // arithmetic and two advances compose to one.
    struct Axis {
        std::uint64_t phase = 0;
        std::uint64_t freq = 0;
        std::uint64_t rate = 0;
    };

    // An active line and what has been learnt about it. m is the sideband
    // order, zero for the line itself.
    struct Line {
        std::int32_t k = 0;
        std::int32_t m = 0;
        std::complex<double> amp{};  // per-sample amplitude against the model's phase
        bool seeded = false;         // amp holds a phase reference
        bool fresh = true;           // no observation folded into the averages yet
        double coherent = 0.0;       // average |tone|^2 per sample, noise-biased
        double noise = 0.0;          // average noise power per sample near the line
        double before = 0.0;         // average line power seen by subtract
        double after = 0.0;          // average power left after subtract
        double block_power = 0.0;    // average |block amplitude|^2, for a sideband's coherence
        std::uint32_t weak_blocks = 0;
        bool in_band = true;
    };

    // What was handed out for one block, kept until its observation arrives.
    struct PlannedLine {
        std::uint32_t slot = 0;
        std::int32_t k = 0;
        std::int32_t m = 0;
        std::uint32_t phase0 = 0;
        std::uint32_t step = 0;
        Complex32 amp{};
        bool in_band = true;
    };
    struct PlanRecord {
        SampleIndex start = 0;
        SampleIndex mid = 0;
        std::uint32_t count = 0;
        std::uint32_t sub_blocks = 1;
        std::uint64_t generation = 0;
        bool subtract = false;
        std::vector<PlannedLine> lines;
    };

    enum class Stage : std::uint8_t { kFrequency, kPhase };

    void start_acquisition(const CombEstimate& estimate);
    void advance_to(SampleIndex index);
    void correct(SampleIndex at, double dc_phase, double dc_freq, double dc_rate,
                 double dd_phase, double dd_freq, double dd_rate);
    void correct_ripple(SampleIndex at, double dp, double df, double dr);
    void replace_weak_lines();
    [[nodiscard]] double line_snr(const Line& line, std::uint32_t count) const;
    [[nodiscard]] bool stands_out(std::size_t slot, std::uint32_t count) const;

    SampleRate rate_ = 0;
    std::uint32_t max_lines_ = kCombMaxLines;
    // A spur is one line, so every count that asks a comb for kCombMinLines
    // asks a spur for one.
    bool spur_ = false;
    std::uint32_t min_lines_ = kCombMinLines;
    std::uint64_t id_ = 0;
    CombLock lock_ = CombLock::kIdle;
    Stage stage_ = Stage::kFrequency;
    std::uint64_t generation_ = 0;

    bool anchored_ = false;
    SampleIndex anchor_ = 0;  // the index the axes are evaluated at
    Axis c_{};
    Axis d_{};
    Axis r_{};  // the sideband ripple, line (k, m) at c + k d + m r
    double initial_offset_hz_ = 0.0;
    double initial_spacing_hz_ = 0.0;
    double initial_ripple_hz_ = 0.0;
    std::int32_t orders_ = 0;

    std::vector<Line> lines_;
    std::int32_t range_lo_ = 0;  // the detection's k range, for replacements
    std::int32_t range_hi_ = -1;
    std::int32_t next_lo_ = 0;   // next untried k below and above the active set
    std::int32_t next_hi_ = 0;
    bool next_take_hi_ = true;

    std::vector<PlanRecord> records_;
    std::uint32_t last_count_ = 0;

    std::uint32_t observed_ = 0;      // observations since acquisition started
    std::uint32_t settle_blocks_ = 0;  // consecutive blocks meeting the stage exit test
    bool wide_ = true;                 // short sub-blocks, for the widest pull-in
    std::uint32_t bad_blocks_ = 0;     // consecutive blocks failing the lock test
    bool seed_pending_ = false;
};

// Up to kCombMaxCombs trackers behind one tracker's interface, so the graph
// dispatches one project and one subtract pass for all of them. A plan is the
// trackers' plans concatenated, at the largest sub-block count any of them
// asked for; an observation is split back by the line counts handed out for
// that block and each tracker folds the sub-blocks to its own.
//
// Detections are matched, not replayed: an estimate a running tracker agrees
// with, or whose lines mostly sit on a running tracker's lines, is already
// held and is dropped; anything else starts in a free tracker, idle or lost,
// with an even share of the slots no running tracker holds.
class CombBank {
public:
    CombBank() = default;
    explicit CombBank(SampleRate rate);

    void reset();
    void detected(std::span<const CombEstimate> estimates);
    void observe(const CombObservation& observation);
    [[nodiscard]] CombBlockPlan plan(SampleIndex start, std::uint32_t count);

    // Spurs to hold (CombEstimate::spur), each started in a tracker of one
    // line unless a running tracker already holds it, up to kCombMaxSpurs and
    // the slots combs leave free. A spur missing from a later call is NOT let
    // go: once it is cancelled the wideband detector no longer sees it, so
    // its absence says nothing. A spur tracker ends when it loses lock, which
    // it measures on the raw line, when the detector withdraws it, or by
    // drop_spurs_in or drop_spurs. A spur that went is not started again for
    // a few seconds.
    void spurs(std::span<const CombEstimate> estimates);

    // Lets go of every spur within kCombSameLineHz of one of these offsets:
    // the detector has stopped calling the carrier there a spur while it is
    // still there, because a probe named a family or it began to key.
    void withdraw_spurs(std::span<const double> frequencies_hz);

    // A baseband range an open receiver is listening to, edges included.
    struct SpurBand {
        double low_hz = 0.0;
        double high_hz = 0.0;
        [[nodiscard]] bool contains(double f) const { return f >= low_hz && f <= high_hz; }
    };

    // Lets go of every spur whose detected offset lies in one of the bands.
    void drop_spurs_in(std::span<const SpurBand> bands);
    void drop_spurs();

    // Spur trackers locked and cancelling.
    [[nodiscard]] std::uint32_t spurs_cancelled() const;

    // Locked if any comb is; lines and suppression over all of them; the comb
    // reported is the one cancelling the most lines. Spurs are left out, and
    // counted by spurs_cancelled.
    [[nodiscard]] CombStatus status() const;

    [[nodiscard]] std::span<const CombTracker> trackers() const { return trackers_; }

    // Every active tracker's current grid, CombTracker::estimate. A copy, so
    // the recording thread can hand it to the detection thread.
    [[nodiscard]] std::vector<CombEstimate> active_estimates() const;

private:
    // One tracker's run of rows in a block's observation.
    struct Row {
        std::uint64_t id = 0;
        std::uint32_t lines = 0;
    };
    struct Split {
        SampleIndex start = 0;
        std::uint32_t sub_blocks = 1;
        std::vector<Row> rows;  // in plan order
    };
    // A spur that went, kept from starting again until `until`.
    struct HoldOff {
        double offset_hz = 0.0;
        SampleIndex until = 0;
    };

    void add(CombTracker tracker);
    void hold_off(const CombTracker& tracker);
    void erase_spurs_if(const std::function<bool(const CombTracker&)>& drop);

    SampleRate rate_ = 0;
    std::vector<CombTracker> trackers_;
    std::vector<Split> splits_;
    std::vector<HoldOff> hold_off_;
    std::uint64_t next_id_ = 1;
    SampleIndex now_ = 0;  // the latest block planned
};

}  // namespace revenant::dsp
