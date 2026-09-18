// The two predicates a caller uses to find out whether a request is worth
// making before making it.
//
// Both answer from the declaration alone and open nothing. That is the point
// of describing a device rather than probing it: a GUI populating a rate
// dropdown, or a session file being validated before anything is opened, gets
// its answer without touching hardware.

#include "core/source/capabilities.h"

#include <algorithm>

namespace revenant::source {

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
