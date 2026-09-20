#include "core/engine/spectrum_scale.h"

#include <cmath>
#include <format>

namespace revenant::engine {
namespace {

// The one-pole coefficient for a time constant, from the elapsed time rather
// than from a fixed frame rate.
//
// A fixed per-frame coefficient would be wrong twice over here. The frame rate
// is the source rate over the block size, so it changes with the
// configuration; and a dispatch whose window was not yet contiguous skips a
// frame, which leaves a gap the recurrence has to cross in one step or the
// scale lags by however long the gap was. exp(-dt/tau) crosses it exactly.
[[nodiscard]] float coefficient(double elapsed_seconds, double time_constant_seconds) {
    if (!(elapsed_seconds > 0.0)) {
        return 0.0F;
    }
    if (!(time_constant_seconds > 0.0)) {
        return 1.0F;
    }
    const double alpha = 1.0 - std::exp(-elapsed_seconds / time_constant_seconds);
    if (alpha <= 0.0) {
        return 0.0F;
    }
    return alpha >= 1.0 ? 1.0F : static_cast<float>(alpha);
}

[[nodiscard]] float approach(float value, float target, float alpha) {
    return value + alpha * (target - value);
}

// Adds one measurement, overwriting the oldest once the window is full.
//
// `window` is validated in create(), so the modulo is a compare rather than a
// division and the index cannot leave the array.
void remember(SpectrumEndHistory& history, float value, std::uint32_t window) {
    history.samples[history.next] = value;
    history.next = history.next + 1 >= window ? 0U : history.next + 1;
    if (history.held < window) {
        ++history.held;
    }
}

// The second most extreme of what is held, in the direction the end expands.
// Precondition: at least one sample is held.
//
// This is the whole outlier rule, and it is a selection rather than a sort
// because the window is three by default and eight at most: a sort of eight
// floats once per frame would not be measurable either, but a single pass
// says what the rule is without the reader having to work out which end of
// the sorted array is meant.
//
// With one sample held this returns that sample, which is what makes the
// first frame after a create or a reset jump rather than sit still.
//
// have_second is not redundant with held. Seeding the runner-up from the
// first sample instead makes that one sample count as both the largest and
// the second largest, so a window holding {-20, -60} answers -20 and a single
// measurement props the end up on its own, which is the whole defect this
// guards against. That version survived the first run of the outlier cases,
// because which slot of the ring the newest sample lands in decides whether
// it shows. "the rejection does not depend on where in the window the bad
// frame lands", in tests/engine/test_spectrum_scale.cpp, is what walks it
// through every slot.
[[nodiscard]] float second_extreme(const SpectrumEndHistory& history, bool upward) {
    float best = history.samples[0];
    float second = best;
    bool have_second = false;
    for (std::uint32_t i = 1; i < history.held; ++i) {
        const float value = history.samples[i];
        if (upward ? value > best : value < best) {
            second = best;
            have_second = true;
            best = value;
            continue;
        }
        if (!have_second || (upward ? value > second : value < second)) {
            second = value;
            have_second = true;
        }
    }
    return second;
}

}  // namespace

Expected<SpectrumScale> SpectrumScale::create(const SpectrumScaleConfig& config) {
    if (!(config.decay_seconds > 0.0) || !std::isfinite(config.decay_seconds)) {
        return fail(std::format("the spectrum scale's decay must be a positive number of "
                                "seconds, got {}",
                                config.decay_seconds));
    }
    if (!(config.attack_seconds > 0.0) || !std::isfinite(config.attack_seconds)) {
        return fail(std::format("the spectrum scale's attack must be a positive number of "
                                "seconds, got {}",
                                config.attack_seconds));
    }
    if (config.attack_seconds > config.decay_seconds) {
        return fail(std::format(
            "the spectrum scale's attack ({} s) is slower than its decay ({} s). "
            "docs/ui-spectrum.md has these the other way round: the decay is the slow one, and "
            "an attack slower than it clips every signal that appears suddenly",
            config.attack_seconds, config.decay_seconds));
    }
    if (!(config.minimum_span_db > 0.0F) || !std::isfinite(config.minimum_span_db)) {
        return fail(std::format("the spectrum scale's minimum span must be a positive number of "
                                "decibels, got {}",
                                config.minimum_span_db));
    }
    if (config.outlier_window_frames == 0 ||
        config.outlier_window_frames > kSpectrumMaxOutlierWindow) {
        return fail(std::format("the spectrum scale's outlier window is {} frames and must be "
                                "between 1 and {}. One is no guard at all, which is a thing to "
                                "ask for; zero is not",
                                config.outlier_window_frames, kSpectrumMaxOutlierWindow));
    }

    const bool floor_pinned = config.pinned_floor_db.has_value();
    const bool ceiling_pinned = config.pinned_ceiling_db.has_value();
    if (floor_pinned && !std::isfinite(*config.pinned_floor_db)) {
        return fail("a pinned spectrum floor must be a finite number of decibels");
    }
    if (ceiling_pinned && !std::isfinite(*config.pinned_ceiling_db)) {
        return fail("a pinned spectrum ceiling must be a finite number of decibels");
    }
    if (floor_pinned && ceiling_pinned && *config.pinned_ceiling_db <= *config.pinned_floor_db) {
        return fail(std::format("a pinned spectrum ceiling of {} dB is not above its pinned "
                                "floor of {} dB, so the colour map has no range",
                                *config.pinned_ceiling_db, *config.pinned_floor_db));
    }

    SpectrumScale scale;
    scale.config_ = config;

    // Somewhere defined to start, for a caller that reads current() before any
    // frame has arrived. The first update() overwrites both.
    scale.levels_.floor_db = config.pinned_floor_db.value_or(-100.0F);
    scale.levels_.ceiling_db = config.pinned_ceiling_db.value_or(0.0F);
    scale.settle();
    return scale;
}

SpectrumScaleLevels SpectrumScale::update(float low_db, float high_db, double elapsed_seconds) {
    // A non-finite percentile is dropped rather than substituted. One NaN
    // through the recurrence makes every later frame NaN, forever, and there
    // is no replacement value that is not an invention. The device is not
    // supposed to produce one; docs/fft.md is the reason that is not the same
    // as it never happening.
    if (std::isfinite(low_db)) {
        remember(low_history_, low_db, config_.outlier_window_frames);
    }
    if (std::isfinite(high_db)) {
        remember(high_history_, high_db, config_.outlier_window_frames);
    }
    if (low_history_.held == 0 || high_history_.held == 0) {
        return levels_;
    }

    // Each end against the second most extreme of its own window, in the
    // direction that end expands. See the header: this is what stops one
    // frame from moving an end that the decay will then hold for half a
    // minute.
    const float low_guarded = second_extreme(low_history_, false);
    const float high_guarded = second_extreme(high_history_, true);

    if (!started_) {
        levels_.floor_db = low_guarded;
        levels_.ceiling_db = high_guarded;
        started_ = true;
        settle();
        return levels_;
    }

    const float attack = coefficient(elapsed_seconds, config_.attack_seconds);
    const float decay = coefficient(elapsed_seconds, config_.decay_seconds);

    // Expansion is the ceiling rising and the floor falling, which are the two
    // directions that uncover signal. Those take the attack; the two that hide
    // it take the decay.
    levels_.ceiling_db = approach(levels_.ceiling_db, high_guarded,
                                  high_guarded > levels_.ceiling_db ? attack : decay);
    levels_.floor_db =
        approach(levels_.floor_db, low_guarded, low_guarded < levels_.floor_db ? attack : decay);

    settle();
    return levels_;
}

void SpectrumScale::settle() {
    const bool floor_pinned = config_.pinned_floor_db.has_value();
    const bool ceiling_pinned = config_.pinned_ceiling_db.has_value();

    if (floor_pinned) {
        levels_.floor_db = *config_.pinned_floor_db;
    }
    if (ceiling_pinned) {
        levels_.ceiling_db = *config_.pinned_ceiling_db;
    }

    // Both pinned is the operator saying exactly what they want, and create()
    // has already refused a pair with no range in it. Widening that would be
    // overriding the instruction rather than protecting the display.
    if (floor_pinned && ceiling_pinned) {
        return;
    }

    if (levels_.ceiling_db - levels_.floor_db >= config_.minimum_span_db) {
        return;
    }

    // Whichever end is free is the one that moves.
    if (ceiling_pinned) {
        levels_.floor_db = levels_.ceiling_db - config_.minimum_span_db;
        return;
    }
    if (floor_pinned) {
        levels_.ceiling_db = levels_.floor_db + config_.minimum_span_db;
        return;
    }

    // With both free the map opens about the midpoint rather than upwards
    // from the floor, and that is the difference between a uniform frame
    // drawing as a flat mid-tone and drawing as nothing at all. A span this
    // narrow means the two percentiles landed in the same histogram bucket,
    // so there is no noise floor to anchor to and every bin in the frame is
    // at the midpoint; hanging the map off the floor would put every one of
    // them on the bottom of the colour ramp.
    const float centre = 0.5F * (levels_.floor_db + levels_.ceiling_db);
    levels_.floor_db = centre - 0.5F * config_.minimum_span_db;
    levels_.ceiling_db = centre + 0.5F * config_.minimum_span_db;
}

}  // namespace revenant::engine
