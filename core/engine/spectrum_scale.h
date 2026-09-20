// The colour map's two ends, tracking the signal.
//
// core/shaders/spectrum_levels.comp measures a low and a high percentile of
// every frame on the device. Those two numbers jump frame to frame, because a
// percentile of a 65536-bin frame is still a sample of a random process and
// because the band itself changes. This is what turns them into something a
// display can be drawn against: a pair of smoothed ends, with the time
// constants docs/ui-spectrum.md asks for.
//
// WHY THE ATTACK AND THE DECAY ARE NOT THE SAME NUMBER
//
// Thirty seconds is the decay, and it is long on purpose: it is what stops the
// waterfall rescaling under the operator every time a transmission ends, which
// is the behaviour that makes an auto-scaled display feel broken without
// anyone being able to name why.
//
// Expansion cannot share it. A signal that appears suddenly is above the
// ceiling the moment it appears, and every bin of it clips to the top of the
// colour map until the ceiling catches up. At thirty seconds that is half a
// minute of a solid bar with no structure in it, on exactly the event the
// display exists to show. So the ends expand in a frame or two and contract
// over the thirty, and "expand" means the ceiling rising or the floor falling,
// which are the two directions that uncover signal rather than hide it.
//
// WHY THE STATE IS HERE AND NOT ON THE DEVICE
//
// The measurement is on the device because a frame is 65536 floats and the
// architecture exists to keep those off the host. The recurrence is two floats
// and it is sequential: frame n's ends depend on frame n-1's. The graph keeps
// three frames in flight and they overlap on the device, so a device-side
// recurrence would need them serialised against a shared state buffer, which
// would cost the overlap to save eight bytes of readback. The completion
// thread is single and in order, which is exactly what a recurrence wants.
//
// WHY NO SINGLE FRAME IS ALLOWED TO MOVE AN END ON ITS OWN
//
// The two paragraphs above are what makes this necessary. The attack is one
// frame period, so one frame is enough to move an end most of the way, and
// the thirty-second decay then holds it wherever it landed. One wrong
// measurement is therefore not a flickering row, it is a wrong colour map for
// half a minute.
//
// Both devices in the conformance matrix can produce that measurement. On the
// integrated Radeon it is the corrupted spectrum frame docs/fft.md dissects,
// which is a concurrency fault in the kernel and arrives as an isolated
// dispatch with the right answers either side of it. On the discrete card it
// is a real wideband burst, a transmitter keying up nearby, which is a
// correct measurement of something that was genuinely there for one frame.
// The smoother cannot tell those apart and does not try to.
//
// So the raw measurement does not reach the recurrence. Each end takes the
// second most extreme of the last outlier_window_frames measurements in the
// direction that end expands: the second largest for the ceiling, the second
// smallest for the floor. At the default window of three that is the median.
// A value that appears once is discarded however large it is; a value that
// appears twice passes at full attack speed.
//
// TWO ALTERNATIVES, AND WHY NEITHER IS THIS
//
// A per-frame cap in decibels attenuates and never rejects. Whatever cap is
// chosen the bad frame still moves the end by the whole cap, and the
// thirty-second decay still holds that displacement, so the defect survives
// at reduced amplitude rather than being fixed. The cap also has to be worth
// several decibels for a genuine 40 dB burst to arrive inside a few frames,
// and several decibels of ceiling error is most of a colour-map step.
//
// Requiring two consecutive frames to agree needs a tolerance in decibels,
// which is a second constant with nothing measured behind it, and it holds
// off a signal that is genuinely ramping, because consecutive frames of a
// rising level never agree. The order statistic has no threshold at all: it
// asks how many frames saw a level, not how close two of them were.
//
// WHAT IT COSTS, MEASURED
//
// Exactly one frame of delay on a genuine step, because the second frame at
// the new level is the one that makes it the second largest. Exactly, not
// approximately: the guarded ceiling on frame n is the unguarded ceiling on
// frame n-1 to four decimal places, which is asserted rather than described.
// At the shipped geometry that is 27.307 ms of delay. The numbers below are
// from the cases in tests/engine/test_spectrum_scale.cpp, which print them.
//
// A sustained 40 dB step is 88.7% covered three frames in, against 96.2%
// without the guard, and reaches that same 96.2% on the fourth frame. In
// decibels: 4.50 dB of the step still uncovered at three frames rather than
// 1.51 dB, and 1.51 dB at four.
//
// A single 40 dB outlier frame moves nothing at all, against 26.58 dB of
// immediate displacement without the guard, of which 9.78 dB is still there
// thirty seconds later and 1.32 dB after ninety.
//
// The honest part of the trade: a burst that really is one frame long is a
// real signal, and the scale now ignores it. It is not invisible, because
// power_db still carries every bin of it and it draws clipped against the
// existing ceiling, but the map is not opened for it. One waterfall row that
// saturates is the price of not having thirty seconds of wrong scale, and
// the row is recoverable from the frame while the thirty seconds is not.
//
// The window is in frames rather than in seconds, which is the opposite of
// the rule the attack and decay follow. What is being rejected is a
// per-dispatch event, so frames are its natural unit: a window in seconds
// would hold a different number of measurements at every block size and the
// rejection guarantee would change with the configuration.
//
// PURITY
//
// Per docs/conventions.md this reads no clock. The elapsed time between two
// frames is handed in, derived by the caller from the frames' sample indices,
// so a capture replayed at forty times realtime scales the same way it did
// live and a test can step thirty seconds without waiting for them.

#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "core/error.h"

namespace revenant::engine {

// The narrowest the colour map is allowed to get, in decibels.
//
// Exported because a display that reduces several bins into one pixel has to
// re-apply it after its own correction, and two copies of the number would
// drift. See SpectrumScaleConfig::minimum_span_db for what it is protecting
// against.
inline constexpr float kSpectrumMinimumSpanDb = 12.0F;

// The most frames of raw measurement either end can be guarded by.
//
// A bound rather than a growable buffer because this is stepped once per
// frame on the completion thread, which is where the graph's readback
// latency is spent, and because nothing sensible lives at the far end of it:
// the window is how many frames an already-ended signal keeps propping the
// ceiling up, so a large one trades the fault this guards against for a
// slower contraction.
inline constexpr std::uint32_t kSpectrumMaxOutlierWindow = 8;

struct SpectrumScaleConfig {
    // Set either end to hold it still. docs/ui-spectrum.md: automatic is the
    // default because it is right almost always, and the exception is
    // comparing two captures, where a scale that moves is a scale that lies
    // about which signal was stronger.
    //
    // Pinning one end leaves the other tracking, which is the common case:
    // a fixed floor at the receiver's known noise level with a ceiling that
    // still follows the traffic.
    std::optional<float> pinned_floor_db;
    std::optional<float> pinned_ceiling_db;

    // Contraction, which is the slow direction and the number
    // docs/ui-spectrum.md names.
    double decay_seconds = 30.0;

    // Expansion. At the shipped geometry, where 2.4 MS/s in 65536 sample
    // blocks puts a frame every 27.307 ms, this is about one frame period,
    // so a step is 66.5 percent gone after one frame and 96.2 percent after
    // three. A 40 dB step is down to 1.5 dB by the third row.
    //
    // A tenth of a second was tried first and was visibly slow. The figure
    // originally recorded here for it, 8 dB of a 40 dB step left after three
    // frames, does not reproduce: one pole at that period leaves
    // 40*exp(-3*0.027307/0.1), which is 17.6 dB. Whatever was measured was
    // not at 0.1 s, and the number is replaced by the arithmetic so that the
    // next person to weigh this constant can check it rather than trust it.
    //
    // In seconds rather than in frames because what the operator sees is a
    // row every few hundred milliseconds whatever the block size is, and
    // because docs/conventions.md wants no hidden dependence on the rate.
    //
    // Those two percentages are the recurrence on its own. What an end
    // actually does is one frame behind them, because outlier_window_frames
    // below holds the first frame of any step back: 88.7 percent after three
    // frames rather than 96.2, and the 96.2 on the fourth. Both figures are
    // measured in tests/engine/test_spectrum_scale.cpp.
    double attack_seconds = 0.025;

    // The narrowest the colour map is allowed to get.
    //
    // Without it a frame whose content is uniform, which is what a silent
    // source or a fully saturated front end looks like, gives a low and a
    // high percentile in the same histogram bucket. The span is then zero and
    // every bin sits at one end of the map or the other, so the display goes
    // solid and says nothing. Twelve decibels is enough that noise renders as
    // noise.
    //
    // When it bites with both ends free the map opens about the midpoint, so
    // the level the frame is actually at lands in the middle of the ramp.
    // Every consumer can rely on that: the ends are at least this far apart
    // and the frame's own body is between them.
    float minimum_span_db = kSpectrumMinimumSpanDb;

    // Frames of raw measurement each end is guarded by, newest included. The
    // section at the top of this file is the argument for it; this is what it
    // means at the two ends of the range.
    //
    // One is no guard: the measurement reaches the recurrence untouched,
    // which is the behaviour every version before this had and the behaviour
    // the defect case in tests/engine/test_spectrum_scale.cpp reproduces on
    // purpose so that the fix has something to be measured against.
    //
    // Three is the default and rejects any isolated frame at a cost of one
    // frame of delay. Larger rejects no more isolated frames, because the
    // rule already discards anything seen once: what it changes is that a
    // level from further back keeps counting, so the ceiling stays propped up
    // for window minus one frames after a signal ends. Against a
    // thirty-second decay two frames is nothing and eight is still nothing,
    // which is why the ceiling is a bound rather than a decision.
    std::uint32_t outlier_window_frames = 3;
};

// The two ends, in the same dBFS the spectrum frame is in.
struct SpectrumScaleLevels {
    float floor_db = 0.0F;
    float ceiling_db = 0.0F;
};

// One end's last few raw measurements, newest overwriting oldest.
//
// Held by value in the scale, fixed size, no allocation. Only the first
// `held` entries mean anything, because `next` starts at zero and only wraps
// once the window has filled.
struct SpectrumEndHistory {
    std::array<float, kSpectrumMaxOutlierWindow> samples{};
    std::uint32_t held = 0;
    std::uint32_t next = 0;
};

class SpectrumScale {
public:
    [[nodiscard]] static Expected<SpectrumScale> create(const SpectrumScaleConfig& config);

    // One frame's measurement. low_db and high_db are the percentiles the
    // device reported; elapsed_seconds is the source time since the previous
    // call, which is zero or less on the first.
    //
    // The first call jumps to the measurement rather than ramping from
    // whatever the members were initialised to, so a display is correctly
    // scaled on its first row instead of fading in from silence over half a
    // minute. It jumps to the measurement itself and not to a guarded value,
    // because with one frame of history there is nothing to compare against:
    // a corrupted first frame is displayed and then corrected on the second,
    // and no rule available here does better than that.
    SpectrumScaleLevels update(float low_db, float high_db, double elapsed_seconds);

    [[nodiscard]] SpectrumScaleLevels current() const { return levels_; }

    // Forgets the history, so the next update jumps again. For a seek, where
    // the band on either side of the cut has nothing to do with the other.
    //
    // The outlier window is cleared with it. Keeping it would compare the
    // first frames after the cut against measurements of a band that is no
    // longer on the air, which is the one comparison the order statistic has
    // no defence against: two stale frames outvote the new one.
    void reset() {
        started_ = false;
        low_history_ = {};
        high_history_ = {};
    }

private:
    SpectrumScale() = default;

    // Applies the pins and the minimum span to whatever the recurrence
    // produced, which is the only place either is enforced.
    void settle();

    SpectrumScaleConfig config_{};
    SpectrumScaleLevels levels_{};
    SpectrumEndHistory low_history_{};
    SpectrumEndHistory high_history_{};
    bool started_ = false;
};

}  // namespace revenant::engine
