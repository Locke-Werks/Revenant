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

#include <algorithm>
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
    //
    // IT SAYS NOTHING ABOUT A BAND ONLY A FEW BINS WIDE, and a caller has to
    // check the width before believing it. Measured 2026-09-22 across one
    // emitter of each family: cw, am, nfm, usb and lsb are line spectra whose
    // lines the detector reports separately, about five bins each, and all
    // five read between 2.11 and 2.38 whatever produced them. That is the
    // analysis window spreading a single line, not the modulation. The two PSK
    // rows, which are the only ones wide compared with the window, read 1.70
    // and 1.79.
    //
    // AND IT IS NOT A SIGNAL-AGAINST-INTERFERENCE TEST AT ANY WIDTH. In the
    // same measurement a third-order intermodulation product read 2.473, which
    // is between nfm at 2.256 and fsk2 at 2.854. A bar that rejected the
    // product would reject every FSK signal on the air.
    double peak_to_mean = 0.0;

    // The band's excess in its strongest three adjacent bins, over its excess
    // in total.
    //
    // THE SAME QUANTITY characterise::spectral_concentration REPORTS, over a
    // band instead of over a whole extract, so the two tiers answer in one
    // unit and a reader can hold them side by side. That comparison was worth
    // having the moment it was tried: on 2026-09-22 a 4.3 kHz detection on
    // 20 m came back from the characteriser as an unmodulated carrier, which
    // is not a thing 4.3 kHz wide, and the explanation was a narrow carrier
    // sitting inside a band whose reported width was too wide.
    //
    // WHAT IT SEPARATES IS A FILLED BAND FROM AN OVER-WIDE ONE, and that is
    // not the same question peak_to_mean fails at. Measured across 20 m at
    // 1603 UT: five detections between 10 Hz and 301 Hz read peak_to_mean 2.23
    // to 5.34, which is the value a patch of averaged noise reads, while two
    // detections 3.5 and 3.7 kHz wide read 250 and 405. peak_to_mean rises
    // with how much of the band is EMPTY, so a correctly sized narrow band
    // scores like noise by construction and the number is close to useless on
    // its own. Three bins over the total does not have that problem: it is a
    // fraction, it does not grow with the width, and it is the number that
    // already separated on the characteriser's side of the same data.
    //
    // READ IT WITH THE BANDWIDTH BESIDE IT, which is how it becomes an answer
    // rather than a number.
    //
    // BELOW ABOUT FIVE BINS IT SAYS NOTHING, for the reason it says nothing
    // below three: the analysis window spreads one line over about that many,
    // so a band that is one line reads near one whatever produced it. The
    // family survey measures exactly that: cw, am, nfm, usb and lsb all read
    // between 0.946 and 0.947, identical to three places, because at 36.6 Hz
    // the detector reports each of their lines as its own five-bin band.
    //
    // AND RESOLUTION DOES NOT LIFT THEM OUT OF IT, which is the part worth
    // knowing before anyone reaches for a finer transform. The same scene on a
    // grid four times finer puts those five at 4.0 to 6.0 bins, exactly where
    // they were, with their measured widths fallen by four from about 180 Hz
    // to about 43. The width was never the signal: each detection is one
    // spectral line, and a line is as wide as the window makes it. What
    // separates those families lives BETWEEN their lines, which is to say
    // between detections, and no field in this header can reach it.
    //
    // ABOVE THAT IT SEPARATES A LINE SPECTRUM FROM A FILLED ONE, which is the
    // first thing in this tree that has separated anything at the family
    // level. In the same survey fsk2 reads 0.519, being two tones with half
    // the power in one of them, against bpsk at 0.117 and qpsk at 0.121,
    // which are genuinely filled. That is the "separates the broad families"
    // tier one is promised to give.
    //
    // WHAT THIS PARAGRAPH USED TO SAY. It read: "High and narrow is a carrier
    // measured correctly. High and wide is a carrier inside a bandwidth that
    // is an overestimate, which is a detection worth splitting and a band the
    // second tier cannot be asked about fairly." The second sentence was a
    // guess and a sweep of occupied_power_fraction refused it. A 489 Hz band
    // on 20 m reading 0.82 holds about 473 Hz at every fraction from 0.99
    // down to 0.80 and then collapses to 3 Hz at 0.50, so its power is
    // genuinely bimodal: a narrow core and a distinct ring out to a couple of
    // hundred hertz. The width is not an overestimate, the band is a line
    // spectrum, and a carrier with sidebands is what that looks like.
    //
    // NOT A VERDICT AND NOT THRESHOLDED, per this header's own rule. What
    // counts as high is a measurement against known truth that has not been
    // made.
    //
    // AND IT IS NOT AN INTERFERENCE TEST, which was measured rather than
    // assumed and is the same answer peak_to_mean gets. Against the product
    // scene, where a station and its own third-order products appear in the
    // same frames with truth known by construction, the parents read 0.079,
    // the products 0.033 and the artefacts elsewhere 0.155: the population
    // that is nothing at all reads highest, and a real parent moves 0.047 to
    // 0.079 on nothing but whether the front end is linear. The survey is in
    // tests/detect/test_front_end.cpp.
    //
    // The reason is structural and it is why these two fields are not
    // independent evidence. Both are the band's power in its peak against its
    // power in total, differing in whether the width divides in, so on a scene
    // where every population is a FILLED wideband band they both come down to
    // how wide each one is. What concentration separates is a band that is
    // nearly all signal from a band that is nearly all floor.
    //
    // A BAND UNDER THREE BINS READS EXACTLY ONE and means nothing by it: every
    // bin it has is in the window. Check the width first, the same way
    // peak_to_mean's own note says to.
    double concentration = 0.0;

    // Power in the lower half of the band over power in the whole band, so
    // exactly 0.5 when the band is balanced about its own centre.
    //
    // INTENDED AS A DIRECT MEASURE OF THE SIDEBAND MODES. USB puts everything
    // above a suppressed carrier and LSB everything below, and
    // docs/detection.md names "a 2.8 kHz asymmetric block with no carrier is
    // SSB" as the case tier one can finish by itself. It is also what a
    // click-to-tune surface needs and does not have: revenant.capnp refuses a
    // logicalCentreHz field today because deriving one needs a classification.
    //
    // IT HAS NOT BEEN SHOWN TO DO THAT, AND THE ONE MEASUREMENT OF IT LOOKS
    // LIKE AN ARTEFACT. In the family survey usb reads 0.453 and lsb 0.547 at
    // the shipped grid, and 0.465 and 0.535 four times finer. Those pairs
    // MIRROR ABOUT A HALF TO THREE DECIMAL PLACES at both grids, which is not
    // what two independent measurements of two different signals look like.
    //
    // What it is instead: the scene's SSB emitters are two-tone, 700 and
    // 1900 Hz, so USB is two lines above the carrier and LSB the same two
    // mirrored below, and the detector reports each line as its own four-bin
    // band. This measures where a line sits inside its own band, and mirrored
    // lines give mirrored answers. It never sees a 2.8 kHz block, because at
    // no grid is one reported: see the note on concentration above.
    //
    // So the premise is untested rather than disproved. A signal whose
    // asymmetry is a real block, detected as a block, would be the test, and
    // nothing in this tree produces one. Do not build an SSB discriminator on
    // this until something does.
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
    //
    // NO ABSOLUTE READING OF THIS MEANS ANYTHING EITHER, and the same
    // measurement says so: FSK2 reads 0.432 through a perfectly linear front
    // end, four times what a QPSK signal reads while it IS being driven into a
    // cubic. Some modulations simply have skirts.
    //
    // What did separate was the CHANGE. The same QPSK emitters went from 0.028
    // to 0.111 when the nonlinearity was switched on, which is spectral
    // regrowth and is a property of one track over time rather than of a
    // number against a constant. A distortion flag built on this has to
    // compare a track with itself.
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

    // Three adjacent bins because that is what a Hann-windowed tone off a bin
    // centre spreads over, which is the reason
    // characterise::spectral_concentration uses three, and the point of this
    // field is to be that number. The window is clamped inside the band, so a
    // band of one or two bins sums itself and reads one.
    if (width <= 3) {
        shape.concentration = 1.0;
    } else {
        double running = 0.0;
        for (std::size_t i = 0; i < 3; ++i) {
            const double value = excess[first + i];
            running += value > 0.0 ? value : 0.0;
        }
        double best = running;
        for (std::size_t i = first + 3; i <= last; ++i) {
            const double entering = excess[i] > 0.0 ? excess[i] : 0.0;
            const double leaving = excess[i - 3] > 0.0 ? excess[i - 3] : 0.0;
            running += entering - leaving;
            if (running > best) {
                best = running;
            }
        }
        // CLAMPED BECAUSE THE SLIDING SUM CAN DRIFT ABOVE THE TRUE ONE.
        // running carries the window forward by adding the bin entering and
        // subtracting the one leaving, so over a band of thousands of bins it
        // accumulates a few ulps that a fresh sum would not. Every term is
        // non-negative, so best cannot really exceed total; a ratio a shade
        // over one would be arithmetic noise escaping as a value no reader or
        // assertion should have to allow for.
        shape.concentration = std::min(1.0, best / total);
    }

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
