// What a band LOOKS like, measured from the averaged spectrum the detector
// already holds.
//
// docs/detection.md splits identification into two tiers and this is the first
// one: "From the spectrum, free, on frames the detector already has: bandwidth,
// symmetry about the centre, presence of a carrier spike, how the shape moves
// over time." Tier two is core/characterise, which reads complex baseband and
// needs a probe receiver that does not exist yet.
//
// WHY THIS EXISTS. The detector decides on power alone. A band clears the
// threshold when its summed excess over the local noise floor is large enough,
// and nothing anywhere asks whether the excess is SHAPED like a transmission.
// An operator reported the consequence on 2026-09-21: intermod products and
// raised patches of noise floor listed as detections, indistinguishable from
// stations.
//
// WHAT THESE NUMBERS ARE AND ARE NOT. They are measurements, not a
// classification and not a verdict. Nothing in this header decides that a band
// is interference. It reports shape, and what a threshold on any of it should
// be is a question for measurement against scenes with known truth, not for a
// constant picked here. Any constant below is a unit or a guard, never a
// decision boundary.
//
// WHY NOT characterise::occupied_band AND spectral_concentration, which exist
// and take a spectrum. Both take a PowerSpectrum, which owns a std::vector of
// its bins, so using them per candidate means copying a sub-band and
// allocating inside the decision loop, once per candidate per decision. These
// take a span of the detector's own array instead. The two answer different
// questions in any case: spectral_concentration is a carrier test over a whole
// extract, and occupied_band is the bandwidth measurement the detector already
// does for itself.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace revenant::detect {

// Everything here is measured in EXCESS: linear power per fine bin with the
// local noise floor already subtracted and negatives clamped to zero. That is
// the quantity the detector's own excess_cumulative_ is built from, and it is
// the right frame because the floor varies across the span: the same band
// sitting on a floor 10 dB higher is the same signal.

// How the excess is distributed across one band.
struct BandShape {
    // Peak excess over mean excess, across the band.
    //
    // One for a perfectly flat band. About two for a band whose bins are
    // exponentially distributed, which is what an averaged patch of NOISE
    // looks like, and higher the wider the band. Large for a carrier, which
    // puts nearly all its power in the three bins the analysis window spreads
    // it over.
    double peak_to_mean = 0.0;

    // Power in the lower half of the band over power in the whole band, so
    // exactly 0.5 when the band is balanced about its own centre.
    //
    // A DIRECT MEASURE OF THE SIDEBAND MODES. USB puts everything above a
    // suppressed carrier and LSB everything below, and docs/detection.md
    // names "a 2.8 kHz asymmetric block with no carrier is SSB" as the case
    // tier one can finish by itself. It is also what a click-to-tune surface
    // needs and does not have: revenant.capnp refuses a logicalCentreHz field
    // today because deriving one needs a classification.
    double lower_fraction = 0.0;

    // How far the band's edges reach beyond where its power is, as a fraction
    // of the band's own width. Zero for a band that stops dead at its edges.
    //
    // THIS IS THE ONE AIMED AT THE COMPLAINT. A transmission has edges: a
    // filter shaped them, and the excess falls from full to nothing within a
    // few bins. A patch of noise floor that the floor estimator did not fully
    // track has no edges at all, it sags away gradually in both directions,
    // and a smeared intermodulation product from modulated parents is wider
    // than either parent and shaped like neither.
    //
    // Measured outward from the band rather than inside it, because the
    // detector has ALREADY trimmed the band to its occupied power fraction,
    // so the inside of the band is the same shape for everything by
    // construction and the tails are exactly what the trim threw away.
    double skirt_fraction = 0.0;

    // Bins that were actually available outside the band when the skirts were
    // walked, each side summed. Zero means the band ran into the end of the
    // spectrum or into its neighbour and skirt_fraction measured nothing.
    //
    // CARRIED SO A CONSUMER CAN REFUSE RATHER THAN BELIEVE A ZERO. A band
    // against the edge of the span reports no skirt, which reads as the
    // sharpest possible edge and is the most signal-like answer there is.
    // That is the shape of bug that makes a measurement worse than none.
    std::size_t skirt_bins_available = 0;

    // Whether anything was measured at all. False for an empty band, a band
    // with no excess in it, or one whose numbers could not be formed.
    bool measured = false;
};

// How far out a skirt walk is allowed to look, as a multiple of the band's own
// width.
//
// A BOUND AND NOT A DECISION. It stops the walk from running the length of the
// span on a band that never comes down, and it caps the reported fraction so a
// consumer reads "at least this gradual" rather than a number that grows with
// whatever happened to be nearby. Two widths each side is enough to separate a
// filtered edge, which falls inside a small fraction of a width, from a sag
// that is still going at twice the width.
inline constexpr double kSkirtReachWidths = 2.0;

// The fraction of the band's mean excess at which a skirt is judged to have
// ended.
//
// A UNIT, being the level the walk measures TO. A tenth is far enough below
// the band to be clear of it and far enough above the floor's own ripple not
// to be chasing noise: the floor estimate is built to sit under the ripple,
// so bins just outside a real signal sit near zero excess rather than at a
// tenth of a signal's mean.
inline constexpr double kSkirtEndFraction = 0.1;

// Measure one band.
//
// excess is the whole spectrum's excess, and first and last are inclusive bin
// bounds into it, exactly as Candidate carries them. The whole array is passed
// rather than a sub-span because the skirt walk has to look OUTSIDE the band,
// which a sub-span has by definition thrown away.
//
// low_limit and high_limit bound the walk, and are how a caller keeps one
// band's skirts out of its neighbour: a band that stops because the next
// signal started has not measured its own edge, and reporting that as a sharp
// one would be reading the neighbour.
[[nodiscard]] inline BandShape measure_band(std::span<const double> excess,
                                            std::size_t first,
                                            std::size_t last,
                                            std::size_t low_limit,
                                            std::size_t high_limit)
{
    BandShape shape;
    if (excess.empty() || last < first || last >= excess.size()) {
        return shape;
    }
    if (low_limit > first || high_limit < last + 1 || high_limit > excess.size()) {
        return shape;
    }

    const std::size_t width = last - first + 1;

    double total = 0.0;
    double peak = 0.0;
    for (std::size_t i = first; i <= last; ++i) {
        const double value = excess[i] > 0.0 ? excess[i] : 0.0;
        total += value;
        if (value > peak) {
            peak = value;
        }
    }
    if (!(total > 0.0)) {
        return shape;
    }

    const double mean = total / static_cast<double>(width);
    shape.peak_to_mean = peak / mean;

    // Split on the band's own centre. An odd width has a middle bin, and it is
    // counted in neither half: putting it in one would make a symmetric band
    // of odd width read as asymmetric by half a bin's worth, which on a narrow
    // band is not small.
    const std::size_t half = width / 2;
    double lower = 0.0;
    for (std::size_t i = 0; i < half; ++i) {
        const double value = excess[first + i];
        lower += value > 0.0 ? value : 0.0;
    }
    double upper = 0.0;
    for (std::size_t i = 0; i < half; ++i) {
        const double value = excess[last - i];
        upper += value > 0.0 ? value : 0.0;
    }
    const double halves = lower + upper;
    shape.lower_fraction = halves > 0.0 ? lower / halves : 0.5;

    const double end_level = mean * kSkirtEndFraction;
    const auto reach = static_cast<std::size_t>(static_cast<double>(width) * kSkirtReachWidths);

    std::size_t below = 0;
    const std::size_t low_stop = first - low_limit < reach ? first - low_limit : reach;
    while (below < low_stop && excess[first - below - 1] > end_level) {
        ++below;
    }

    std::size_t above = 0;
    const std::size_t high_room = high_limit - (last + 1);
    const std::size_t high_stop = high_room < reach ? high_room : reach;
    while (above < high_stop && excess[last + above + 1] > end_level) {
        ++above;
    }

    shape.skirt_bins_available = low_stop + high_stop;
    shape.skirt_fraction =
        static_cast<double>(below + above) / static_cast<double>(width);
    shape.measured = true;
    return shape;
}

}  // namespace revenant::detect
