// The two predicates a caller uses to find out whether a request is worth
// making before making it.
//
// Both answer from the declaration alone and open nothing. That is the point
// of describing a device rather than probing it: a GUI populating a rate
// dropdown, or a session file being validated before anything is opened, gets
// its answer without touching hardware.

#include "core/source/capabilities.h"

#include <algorithm>
#include <cstdint>
#include <format>

namespace revenant::source {
namespace {

// Three-way comparison of a/b against c/d, for strictly positive integers,
// without ever forming a product. Negative when a/b is the smaller.
//
// The obvious cross-multiply is a*d against c*b, and both sides overflow here
// for inputs that are not absurd: a width in hertz against a channel count
// times a transform size reaches 10^21 before anything about the request
// looks unreasonable. A silent wrap answers the sizing question backwards,
// which is a spectrum too coarse to identify anything and no message
// anywhere.
//
// This is the continued-fraction comparison instead. Each step compares the
// integer parts, and where those agree it continues on the reciprocals of the
// remainders, which reverses the sense. Every value stays bounded by one of
// its own inputs, so nothing can overflow at any depth, and the remainders
// are a Euclidean sequence so it terminates.
[[nodiscard]] int compare_fractions(std::int64_t a, std::int64_t b, std::int64_t c,
                                    std::int64_t d)
{
    int sign = 1;
    while (true) {
        const std::int64_t qa = a / b;
        const std::int64_t qc = c / d;
        if (qa != qc) {
            return qa > qc ? sign : -sign;
        }

        const std::int64_t ra = a % b;
        const std::int64_t rc = c % d;
        if (ra == 0 && rc == 0) {
            return 0;
        }
        if (ra == 0) {
            return -sign;  // a/b stopped at the shared integer part, c/d did not
        }
        if (rc == 0) {
            return sign;
        }

        const std::int64_t na = b;
        const std::int64_t nb = ra;
        const std::int64_t nc = d;
        const std::int64_t nd = rc;
        a = na;
        b = nb;
        c = nc;
        d = nd;
        sign = -sign;
    }
}

// Four bins across the narrowest signal. One bin cannot tell a centre from an
// edge, two cannot show a shape, and four places the centre within a quarter
// of the signal's own width, which is what a mode classifier needs before its
// width measurement means anything.
constexpr std::uint32_t kBinsAcrossNarrowest = 4;

// The top of ITU-R V.431-8 band 7, high frequency, 3 to 30 MHz.
constexpr dsp::Hertz kHfCeilingHz = 30'000'000;

// PSK31 is 31.25 baud BPSK, so about 31 Hz occupied. Peter Martinez G3PLX,
// "PSK31: A New Radio-Teletype Mode", RadCom, December 1998. It is the
// narrowest of the HF modes in the table in the header, so it sets the
// requirement for the whole band.
constexpr dsp::Hertz kNarrowestHfSignalHz = 31;

// The narrowest analogue channel plan in common use above HF. Land mobile
// narrowband is 12.5 kHz, which is what an operator clicking a repeater is
// pointing at.
constexpr dsp::Hertz kNarrowestVhfSignalHz = 12'500;

}  // namespace

bool ResolutionRequest::met_by(std::int64_t bin_width_numerator,
                               std::int64_t bin_width_denominator) const
{
    if (!stated()) {
        return true;
    }
    if (bin_width_denominator <= 0) {
        return false;
    }
    if (bin_width_numerator <= 0) {
        // A bin of zero width resolves everything. Nothing produces one, and
        // answering false here would make a caller chase a finer transform
        // that cannot exist.
        return true;
    }

    // bins * width <= narrowest, rearranged so both sides are fractions:
    // narrowest / bins against numerator / denominator.
    return compare_fractions(narrowest_signal_hz,
                             static_cast<std::int64_t>(bins_across_narrowest),
                             bin_width_numerator, bin_width_denominator) >= 0;
}

ResolutionRequest resolution_for_span(dsp::Hertz center, dsp::SampleRate rate)
{
    ResolutionRequest out;
    if (center <= 0 || rate <= 0) {
        return out;
    }

    const dsp::Hertz low_edge = center - rate / 2;

    out.bins_across_narrowest = kBinsAcrossNarrowest;
    if (low_edge <= kHfCeilingHz) {
        out.narrowest_signal_hz = kNarrowestHfSignalHz;
        out.basis = std::format(
            "PSK31 at {} Hz, on a span whose low edge is {} Hz and so reaches HF",
            kNarrowestHfSignalHz, low_edge);
    } else {
        out.narrowest_signal_hz = kNarrowestVhfSignalHz;
        out.basis = std::format(
            "narrowband land mobile at {} Hz, on a span whose low edge is {} Hz and so sits "
            "above HF",
            kNarrowestVhfSignalHz, low_edge);
    }
    return out;
}

bool SourceCapabilities::supports_rate(dsp::SampleRate rate) const
{
    if (rate <= 0) {
        return false;
    }

    // A populated list is exhaustive. A device with a fixed divider chain
    // cannot be talked into a rate between two of its entries, and pretending
    // otherwise produces a capture whose declared rate is not the rate the
    // samples were taken at.
    if (!sample_rates.empty()) {
        return std::find(sample_rates.begin(), sample_rates.end(), rate) != sample_rates.end();
    }

    // Empty list means continuous between the bounds. A source that declared
    // neither a list nor a bound has described nothing, and the honest answer
    // to "can you do 20 MS/s" in that case is no rather than a shrug that the
    // caller will read as yes.
    if (min_rate <= 0 && max_rate <= 0) {
        return false;
    }
    if (min_rate > 0 && rate < min_rate) {
        return false;
    }
    if (max_rate > 0 && rate > max_rate) {
        return false;
    }
    return true;
}

bool SourceCapabilities::can_tune(dsp::Hertz frequency) const
{
    // Reachability, not exactness. A stepped range still answers true for a
    // frequency between two steps, because tune() lands on the nearest one and
    // returns where it landed. Requiring the caller to pre-round would put the
    // synthesiser's grid arithmetic in two places, and the copy in the caller
    // is the one that goes stale.
    return std::any_of(tune_ranges.begin(), tune_ranges.end(),
                       [frequency](const TuneRange& range) { return range.contains(frequency); });
}

}  // namespace revenant::source
