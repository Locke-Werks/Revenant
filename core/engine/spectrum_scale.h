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
// PURITY
//
// Per docs/conventions.md this reads no clock. The elapsed time between two
// frames is handed in, derived by the caller from the frames' sample indices,
// so a capture replayed at forty times realtime scales the same way it did
// live and a test can step thirty seconds without waiting for them.

#pragma once

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
};

// The two ends, in the same dBFS the spectrum frame is in.
struct SpectrumScaleLevels {
    float floor_db = 0.0F;
    float ceiling_db = 0.0F;
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
    // minute.
    SpectrumScaleLevels update(float low_db, float high_db, double elapsed_seconds);

    [[nodiscard]] SpectrumScaleLevels current() const { return levels_; }

    // Forgets the history, so the next update jumps again. For a seek, where
    // the band on either side of the cut has nothing to do with the other.
    void reset() { started_ = false; }

private:
    SpectrumScale() = default;

    // Applies the pins and the minimum span to whatever the recurrence
    // produced, which is the only place either is enforced.
    void settle();

    SpectrumScaleConfig config_{};
    SpectrumScaleLevels levels_{};
    bool started_ = false;
};

}  // namespace revenant::engine
