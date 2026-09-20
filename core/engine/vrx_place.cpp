// Where a receiver attaches to the coarse grid, and what the fine stage has
// to undo to get from there to what was asked for.
//
// Two functions, both pure, both integer. Nothing here rounds a frequency:
// the whole reason core/dsp/pfb.h carries ChannelCentre as an exact rational
// is that k*rate/M is frequently not a whole hertz, and a receiver that
// rounded it at the point the channel was described would carry a tuning
// offset nobody could later source. The residual this computes is the input
// to the fine stage's NCO, and it stays exact right up to the single
// reduction into fixed-point turns in core/dsp/vrx_reference.cpp.
//
// WHAT FRAME params.center IS IN.
//
// place() is handed the grid, the source rate and the request, and nothing
// else. It is not told where the source is tuned, so it cannot convert an
// absolute radio frequency into a position on the grid, and it therefore
// reads VrxParams::center as a frequency in the SOURCE'S OWN BASEBAND FRAME:
// hertz from the source's centre, in [-rate/2, +rate/2].
//
// That is the whole contract, and there is no rebase hidden behind it.
// Engine::add_vrx passes the caller's params to this function untouched and
// stores those same params, so VrxStatus reads back in baseband as well.
// Converting from an absolute frequency belongs to the caller, which is the
// only party that has EngineInfo::source_center: tools/cli/main.cpp does the
// subtraction for a frequency the operator typed, and the bench and the
// tests build offsets directly. core/engine/vrx.h argues why the offset is
// the API rather than the absolute frequency the name "center" suggests.
//
// This is stated in both places because the two readings differ by the whole
// local oscillator and the failure is silent on a source that declares no
// centre, where source_center is zero and the two numbers coincide.
//
// The two places have not always agreed. Until 2026-09-19 core/engine/vrx.h
// documented center as an absolute radio frequency, and core/rpc/
// revenant.capnp, core/rpc/types.h and docs/detection.md each recorded that
// disagreement, one of them saying the engine was expected to rebase. The
// header was the side that was wrong. Nothing below changed when it was
// corrected, because nothing below has ever rebased: this function is handed
// no tuned centre to rebase against, which is the argument the paragraph
// above makes and the reason the offset reading is the one that survived.

#include "core/engine/vrx.h"

#include <cstdlib>
#include <format>
#include <numeric>
#include <string>
#include <string_view>

#include "core/dsp/pfb.h"
#include "core/dsp/vrx_reference.h"

namespace revenant::engine {
namespace {

// Every demodulator's name, in enumerator order, so the table and the enum
// cannot drift: demod_name() is the single spelling and this reads it back.
constexpr Demod kAllDemods[] = {Demod::Raw, Demod::Am,  Demod::Nfm, Demod::Wfm,
                                Demod::Usb, Demod::Lsb, Demod::Dsb, Demod::Cw};

// Rounds a quotient of integers to the nearest integer, halves away from zero.
//
// Written out rather than done in double. At a 20 MS/s source with 2048
// channels the numerator is around 2e13, which a double still represents
// exactly, but the margin is four bits and a wider grid or a faster source
// spends them. Integer division has no margin to spend.
[[nodiscard]] std::int64_t divide_nearest(std::int64_t numerator, std::int64_t denominator) {
    const std::int64_t quotient = numerator / denominator;
    const std::int64_t remainder = numerator % denominator;

    // Both operands of the comparison are magnitudes, so the sign of the
    // numerator does not change which way the half rounds.
    const std::int64_t twice = 2 * (remainder < 0 ? -remainder : remainder);
    if (twice < (denominator < 0 ? -denominator : denominator)) {
        return quotient;
    }
    return (remainder < 0) ? quotient - 1 : quotient + 1;
}

}  // namespace

Expected<Demod> demod_from_name(std::string_view name) {
    for (const Demod mode : kAllDemods) {
        if (name == demod_name(mode)) {
            return mode;
        }
    }

    std::string known;
    for (const Demod mode : kAllDemods) {
        if (!known.empty()) {
            known += ", ";
        }
        known += demod_name(mode);
    }
    return fail(std::format("unknown demodulator '{}', expected one of: {}", name, known));
}

Expected<VrxPlacement> place(const dsp::GridParams& grid, dsp::SampleRate rate,
                             const VrxParams& params) {
    // The grid's own shape is core/dsp/pfb.h's business: a power-of-two
    // channel count, a decimation that divides it, a nonzero tap count.
    // Calling validate here rather than restating those rules means a
    // receiver is rejected by exactly the grid the kernels reject, and keeps
    // being rejected if the rules move.
    if (const Status valid = dsp::validate(grid); !valid) {
        return std::unexpected(with_context(valid.error(), "place"));
    }

    if (rate <= 0) {
        return fail(std::format("place: sample rate must be positive, got {}", rate));
    }

    // The channel stream's rate is rate/D, and it has to be a whole number of
    // samples per second because SampleRate is integral and because the
    // resampler's recurrence is exact integer arithmetic over it. D is a power
    // of two in every grid validate() accepts, so this rejects an odd source
    // rate rather than a misconfigured grid, and the fix is the source's rate
    // rather than anything here.
    if (rate % static_cast<dsp::SampleRate>(grid.decimation) != 0) {
        return fail(std::format(
            "place: source rate {} is not divisible by the grid's decimation {}, so the "
            "channel rate would not be a whole number of samples per second",
            rate, grid.decimation));
    }
    const dsp::SampleRate channel_rate = rate / static_cast<dsp::SampleRate>(grid.decimation);

    const dsp::Hertz nyquist = rate / 2;
    if (params.center > nyquist || params.center < -nyquist) {
        return fail(std::format(
            "place: centre {} Hz is outside the source's baseband span of +/-{} Hz. This "
            "frequency is relative to the source's tuned centre, not absolute; subtract "
            "EngineInfo::source_center from an absolute frequency before passing it",
            params.center, nyquist));
    }

    if (params.bandwidth <= 0) {
        return fail(std::format("place: bandwidth must be positive, got {} Hz",
                                params.bandwidth));
    }

    // Nearest grid channel. Minimising |centre - s*rate/M| over the signed
    // channel index s is minimising |centre*M - s*rate|, so s is the nearest
    // integer to centre*M/rate and the whole choice is integer arithmetic.
    // centre is bounded by rate/2 above, so the product is at most
    // rate*M/2: 2e13 for a 20 MS/s source on a 2048-channel grid, eleven
    // orders of magnitude inside int64.
    const auto channels = static_cast<std::int64_t>(grid.channels);
    const std::int64_t signed_index =
        divide_nearest(static_cast<std::int64_t>(params.center) * channels,
                       static_cast<std::int64_t>(rate));

    // Reduce into [0, M). centre == +rate/2 lands on s == M/2, which
    // channel_centre folds back to -rate/2: the same channel, and the same
    // frequency, because the spectrum is periodic in rate.
    std::int64_t wrapped = signed_index % channels;
    if (wrapped < 0) {
        wrapped += channels;
    }
    const auto channel = static_cast<std::uint32_t>(wrapped);

    VrxPlacement placement;
    placement.channel = channel;
    placement.channel_centre = dsp::channel_centre(grid, rate, channel);
    placement.channel_rate = channel_rate;

    // The residual the fine stage mixes out: requested minus actual, as the
    // exact rational it is. channel_centre is already reduced, so its
    // denominator divides M and the product below is bounded by rate*M/2.
    const std::int64_t centre_denominator = placement.channel_centre.denominator;
    placement.residual_numerator = static_cast<std::int64_t>(params.center) * centre_denominator -
                                   placement.channel_centre.numerator;
    placement.residual_denominator = centre_denominator;

    // Fold into (-rate/2, +rate/2]. This is not defensive: a request at
    // exactly +rate/2 lands on channel M/2, whose centre channel_centre
    // reports as -rate/2 because that is the representative it folds to, and
    // the difference between the two is a whole span. They are the same
    // frequency, and the residual the fine stage mixes has to be the small
    // one rather than the span.
    const std::int64_t span =
        static_cast<std::int64_t>(rate) * placement.residual_denominator;
    while (2 * placement.residual_numerator > span) {
        placement.residual_numerator -= span;
    }
    while (2 * placement.residual_numerator <= -span) {
        placement.residual_numerator += span;
    }

    const std::int64_t divisor =
        std::gcd(placement.residual_numerator, placement.residual_denominator);
    if (divisor > 1) {
        placement.residual_numerator /= divisor;
        placement.residual_denominator /= divisor;
    }

    // What one channel can actually deliver to this receiver, given where the
    // receiver landed inside it. The definition and its derivation are in
    // core/dsp/vrx_reference.h so that place() and the planner cannot disagree
    // about it; the caller is told that it was reduced, and asks the planner
    // for the figure.
    const dsp::Hertz widest = dsp::max_channel_bandwidth(placement);
    placement.bandwidth_clamped = params.bandwidth > widest;

    return placement;
}

}  // namespace revenant::engine
