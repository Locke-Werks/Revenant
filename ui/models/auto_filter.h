// Auto filter: fitting a receiver's passband to the signal it was tuned onto.
//
// The owner's request of 2026-09-23. A click on a detection puts a receiver on
// a signal with the mode's default filter, and the operator then drags the
// edges until they sit on the signal. This does that drag once, from what the
// pane measures: off by default, a toggle beside AFT, applied on a
// click-to-tune and when it is switched on, and never again until one of
// those happens. docs/ui-spectrum.md, "Auto filter", has the rules; this is
// them.
//
// WHAT IT MEASURES FROM
//
// The passband pane, which since the display tap is the air around the
// receiver with none of its filter in it: a flat noise floor and every signal
// at its true level, so an occupied band can be read off it. One frame is too
// noisy for that, so the frames are averaged, as power, over at least
// min_look_s and min_looks distinct analysis windows, and the occupied bins
// are those more than occupied_db over the noise mean. The noise mean is the
// frames' own 5th percentile plus kPercentileToNoiseMeanDb, averaged the same
// way; models/aft.h derives that constant.
//
// Beside the pane, the detection's occupied width, which is the one other
// measurement of the signal the client holds. It widens a fit and never
// narrows one, because it was measured once at click time on the wide
// spectrum and the pane is the closer look.
//
// WHAT IT DOES NOT USE. core/detect/groups.h follows the lines one emitter
// puts on the spectrum as a set, which is exactly what "out to the outer
// sideband lines" wants for AM. None of it reaches the client: rpc::Detection
// carries no group, and nothing in core/rpc reads that header. So the AM rule
// finds the lines itself, on the pane, by their symmetry about the carrier.
//
// PER MODE
//
//   am        Symmetric about the carrier, which is the receiver's centre,
//             out to the outermost pair of lines that sit mirrored either
//             side of it, chained outward from the carrier across gaps of up
//             to am_bridge_hz. Mirrored, because a neighbour sits on one side
//             only; chained, because a station three channels over can be
//             mirrored by another three channels the other way.
//   usb, lsb  From the suppressed carrier, which is the receiver's centre,
//             to the far edge of the occupied band, on whichever side holds
//             more occupied power by side_ratio_db. The mode's name does not
//             pick the side: the energy does, and a USB receiver on a signal
//             below its centre is fitted to it, which on this engine is the
//             whole of what distinguishes the two. Neither side ahead by the
//             ratio is not a sideband signal, and nothing changes.
//   cw        A tight window around the tone: its occupied run and a main
//             lobe either side, at least tone_min_width_hz wide.
//   narrow    A sideband signal whose occupied band is under narrow_data_hz,
//             the shape of PSK31 or RTTY, gets the CW window rather than one
//             stretched back to the carrier.
//   nfm, wfm  The measured occupied width, symmetric about the receiver's
//   dsb       centre, or the detection's width if that is wider.
//   raw       No demodulator, so no statement of what the signal is. Nothing.
//
// Every fit is clamped to the channel's edge limit, which is what the engine
// grants, and to the minimum width. A fit is only made once that limit and
// the receiver's own edges have come back from the engine.
//
// WHAT IT NEVER DOES. It never moves an edge while the operator is dragging
// one: a fit that comes due during a drag is dropped, and the drag wins. And
// it leaves the filter alone when nothing near the receiver clears the
// occupied level: no signal identified is no change.
//
// This header holds no Qt; ui/tests links it.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "models/aft.h"

namespace revenant::ui {

enum class AutoFilterRule {
    None,
    Carrier,
    Sideband,
    Tone,
    Occupied,
};

// Mode names as core/engine/vrx.h spells them.
[[nodiscard]] constexpr AutoFilterRule auto_filter_rule_for(std::string_view demod)
{
    if (demod == "am") {
        return AutoFilterRule::Carrier;
    }
    if (demod == "usb" || demod == "lsb") {
        return AutoFilterRule::Sideband;
    }
    if (demod == "cw") {
        return AutoFilterRule::Tone;
    }
    if (demod == "nfm" || demod == "wfm" || demod == "dsb") {
        return AutoFilterRule::Occupied;
    }
    return AutoFilterRule::None;
}

struct AutoFilterSettings {
    // A bin is occupied when the averaged pane stands this far over the noise
    // mean there. With four independent looks averaged, a noise bin clears 6
    // dB about once in ten thousand.
    float occupied_db = 6.0F;

    // How long the pane is averaged before a fit, and in how many distinct
    // analysis windows at least.
    double min_look_s = 0.4;
    int min_looks = 4;

    // Gaps inside one signal's occupied band, bridged when it is grown out
    // from its loudest bin. Voice on SSB and a deviation swing on FM both
    // leave notches a bin or two wide once averaged; a neighbour is further.
    double bridge_hz = 150.0;
    double sideband_bridge_hz = 300.0;

    // The furthest an AM carrier's lines may sit from the last pair found
    // and still be the same station: far enough to reach a 1 kHz test tone's
    // lines from the carrier, short of the 9 and 10 kHz channel spacings.
    double am_bridge_hz = 1'500.0;

    // A sideband signal narrower than this is data, and gets a tone window.
    double narrow_data_hz = 500.0;

    // The narrowest CW or data window, how far under the tone's own peak its
    // window reaches, and the margin it takes outside that, which is one bin
    // rather than a main lobe: the lobe is already inside what was measured,
    // and a CW filter is judged by how little it passes beside the tone.
    double tone_min_width_hz = 100.0;
    float tone_depth_db = 20.0F;
    double tone_margin_bins = 1.0;

    // Added outside every measured edge: the analysis window's main lobe is
    // about two bins either side of a line, so a measured edge is that much
    // inside where the signal's last line actually is.
    double margin_bins = 2.0;

    // SSB: the side with the energy must lead the other by this much.
    float side_ratio_db = 3.0F;
};

// The pane averaged over enough frames to read an occupied band from. Frames
// are added in absolute hertz off their own geometry; a frame on a different
// axis, which a retune or a rung change brings, starts the average again.
class AutoFilterAverage {
public:
    void reset()
    {
        power_.clear();
        frames_ = 0;
        looks_ = 0;
        first_s_ = 0.0;
        counted_s_ = 0.0;
        noise_power_ = 0.0;
        first_bin_hz_ = 0.0;
        bin_hz_ = 0.0;
    }

    void add(std::span<const float> power_db, double first_bin_hz, double bin_hz,
             float percentile_low_db, double window_s, double now_s)
    {
        if (power_db.empty() || !(bin_hz > 0.0)) {
            return;
        }
        const bool same_axis = power_.size() == power_db.size() &&
                               std::fabs(bin_hz - bin_hz_) <= 1e-9 * bin_hz &&
                               std::fabs(first_bin_hz - first_bin_hz_) <= 1e-3 * bin_hz;
        if (!same_axis) {
            reset();
            power_.assign(power_db.size(), 0.0);
            first_bin_hz_ = first_bin_hz;
            bin_hz_ = bin_hz;
            first_s_ = now_s;
            counted_s_ = now_s;
            looks_ = 1;
        } else if (now_s - counted_s_ >= window_s) {
            ++looks_;
            counted_s_ = now_s;
        }
        for (std::size_t i = 0; i < power_db.size(); ++i) {
            power_[i] += std::pow(10.0, static_cast<double>(power_db[i]) / 10.0);
        }
        noise_power_ += std::pow(
            10.0, (static_cast<double>(percentile_low_db) + kPercentileToNoiseMeanDb) / 10.0);
        ++frames_;
        last_s_ = now_s;
    }

    [[nodiscard]] bool ready(const AutoFilterSettings& settings) const
    {
        return frames_ > 0 && looks_ >= settings.min_looks &&
               last_s_ - first_s_ >= settings.min_look_s;
    }

    [[nodiscard]] std::vector<float> power_db() const
    {
        std::vector<float> out(power_.size());
        for (std::size_t i = 0; i < power_.size(); ++i) {
            out[i] = static_cast<float>(10.0 * std::log10(power_[i] / frames_));
        }
        return out;
    }

    [[nodiscard]] float noise_db() const
    {
        return frames_ == 0 ? 0.0F
                            : static_cast<float>(10.0 * std::log10(noise_power_ / frames_));
    }

    [[nodiscard]] double first_bin_hz() const { return first_bin_hz_; }
    [[nodiscard]] double bin_hz() const { return bin_hz_; }
    [[nodiscard]] int frames() const { return frames_; }
    [[nodiscard]] int looks() const { return looks_; }

private:
    std::vector<double> power_;
    int frames_ = 0;
    int looks_ = 0;
    double first_s_ = 0.0;
    double last_s_ = 0.0;
    double counted_s_ = 0.0;
    double noise_power_ = 0.0;
    double first_bin_hz_ = 0.0;
    double bin_hz_ = 0.0;
};

struct AutoFilterInput {
    AutoFilterRule rule = AutoFilterRule::None;

    // The averaged pane: decibels per bin, the absolute hertz at the centre
    // of bin zero, the bin width, and the noise mean.
    std::span<const float> power_db;
    double first_bin_hz = 0.0;
    double bin_hz = 0.0;
    float noise_db = 0.0F;

    // The receiver's centre in absolute hertz, and its edges now, in hertz
    // from that centre.
    double centre_hz = 0.0;
    int low_hz = 0;
    int high_hz = 0;

    // The detection's occupied width, or zero for a receiver tuned by hand.
    double detection_width_hz = 0.0;

    // How far either edge may reach, which the channel sets; zero while the
    // engine has not said.
    int edge_limit_hz = 0;
    int min_width_hz = 50;

    // The operator has an edge or the band under the pointer.
    bool dragging = false;
};

enum class AutoFilterOutcome {
    Idle,       // on, and nothing has asked for a fit yet
    Fitted,     // new edges, to be applied
    Unchanged,  // the fit is the filter already there
    Waiting,    // the engine has not said the edge limit or the edges yet
    NoRule,     // raw: nothing to fit
    NoSignal,   // nothing near the receiver is occupied
    Ambiguous,  // sideband: neither side leads
    Dragging,   // the operator is dragging, or touched the filter; left alone
};

struct AutoFilterFit {
    AutoFilterOutcome outcome = AutoFilterOutcome::Idle;
    int low_hz = 0;
    int high_hz = 0;
};

namespace detail {

// Offsets from the receiver's centre, in bins and hertz, and occupancy, over
// the averaged pane.
class Pane {
public:
    Pane(const AutoFilterInput& in, float occupied_db)
        : in_(in), threshold_db_(in.noise_db + occupied_db)
    {
    }

    [[nodiscard]] std::ptrdiff_t size() const
    {
        return static_cast<std::ptrdiff_t>(in_.power_db.size());
    }

    // The bin nearest this offset from the centre, which may be off the pane.
    [[nodiscard]] std::ptrdiff_t bin_at(double offset_hz) const
    {
        return static_cast<std::ptrdiff_t>(
            std::llround((in_.centre_hz + offset_hz - in_.first_bin_hz) / in_.bin_hz));
    }

    [[nodiscard]] double offset_of(std::ptrdiff_t bin) const
    {
        return in_.first_bin_hz + static_cast<double>(bin) * in_.bin_hz - in_.centre_hz;
    }

    [[nodiscard]] bool on(std::ptrdiff_t bin) const { return bin >= 0 && bin < size(); }

    [[nodiscard]] bool occupied(std::ptrdiff_t bin) const
    {
        return on(bin) && in_.power_db[static_cast<std::size_t>(bin)] > threshold_db_;
    }

    [[nodiscard]] float db(std::ptrdiff_t bin) const
    {
        return in_.power_db[static_cast<std::size_t>(bin)];
    }

    // Excess over the noise mean as linear power, occupied bins only, so a
    // side full of noise sums to nothing.
    [[nodiscard]] double excess(std::ptrdiff_t bin) const
    {
        if (!occupied(bin)) {
            return 0.0;
        }
        return std::pow(10.0, static_cast<double>(db(bin)) / 10.0) -
               std::pow(10.0, static_cast<double>(in_.noise_db) / 10.0);
    }

    // The loudest occupied bin in [from, to], or -1.
    [[nodiscard]] std::ptrdiff_t loudest(std::ptrdiff_t from, std::ptrdiff_t to) const
    {
        std::ptrdiff_t best = -1;
        for (std::ptrdiff_t b = std::max<std::ptrdiff_t>(from, 0);
             b <= std::min(to, size() - 1); ++b) {
            if (occupied(b) && (best < 0 || db(b) > db(best))) {
                best = b;
            }
        }
        return best;
    }

    // The occupied run containing seed, bridging gaps of up to `bridge` bins
    // and staying inside [floor_bin, ceiling_bin].
    struct Run {
        std::ptrdiff_t low = 0;
        std::ptrdiff_t high = 0;
    };
    [[nodiscard]] Run grow(std::ptrdiff_t seed, std::ptrdiff_t bridge, std::ptrdiff_t floor_bin,
                           std::ptrdiff_t ceiling_bin) const
    {
        Run run{seed, seed};
        std::ptrdiff_t missed = 0;
        for (std::ptrdiff_t b = seed - 1; b >= std::max<std::ptrdiff_t>(floor_bin, 0); --b) {
            if (occupied(b)) {
                run.low = b;
                missed = 0;
            } else if (++missed > bridge) {
                break;
            }
        }
        missed = 0;
        for (std::ptrdiff_t b = seed + 1; b <= std::min(ceiling_bin, size() - 1); ++b) {
            if (occupied(b)) {
                run.high = b;
                missed = 0;
            } else if (++missed > bridge) {
                break;
            }
        }
        return run;
    }

private:
    const AutoFilterInput& in_;
    float threshold_db_;
};

[[nodiscard]] inline std::ptrdiff_t bins_for(double hz, double bin_hz)
{
    return static_cast<std::ptrdiff_t>(std::ceil(hz / bin_hz));
}

}  // namespace detail

// The fit, or the reason there is none. Pure: the caller applies it.
[[nodiscard]] inline AutoFilterFit fit_auto_filter(const AutoFilterInput& in,
                                                   const AutoFilterSettings& settings = {})
{
    AutoFilterFit fit;
    fit.low_hz = in.low_hz;
    fit.high_hz = in.high_hz;

    if (in.dragging) {
        fit.outcome = AutoFilterOutcome::Dragging;
        return fit;
    }
    if (in.rule == AutoFilterRule::None) {
        fit.outcome = AutoFilterOutcome::NoRule;
        return fit;
    }
    if (in.edge_limit_hz <= 0 || in.high_hz <= in.low_hz || in.power_db.empty() ||
        !(in.bin_hz > 0.0)) {
        fit.outcome = AutoFilterOutcome::Waiting;
        return fit;
    }

    const detail::Pane pane(in, settings.occupied_db);
    const double bin = in.bin_hz;
    const double margin = settings.margin_bins * bin;
    const double reach_now =
        static_cast<double>(std::max(std::abs(in.low_hz), std::abs(in.high_hz)));
    const std::ptrdiff_t centre_bin = pane.bin_at(0.0);
    const std::ptrdiff_t limit_bins = detail::bins_for(in.edge_limit_hz, bin);

    double low = 0.0;
    double high = 0.0;

    switch (in.rule) {
    case AutoFilterRule::Carrier: {
        // The carrier is the receiver's centre, to within the main lobe.
        const std::ptrdiff_t carrier = pane.loudest(centre_bin - 2, centre_bin + 2);
        if (carrier < 0) {
            fit.outcome = AutoFilterOutcome::NoSignal;
            return fit;
        }
        const std::ptrdiff_t bridge = detail::bins_for(settings.am_bridge_hz, bin);
        const auto mirrored = [&](std::ptrdiff_t d) {
            const auto near = [&](std::ptrdiff_t b) {
                return pane.occupied(b - 1) || pane.occupied(b) || pane.occupied(b + 1);
            };
            return near(carrier + d) && near(carrier - d);
        };
        std::ptrdiff_t last = 0;
        for (std::ptrdiff_t d = 1; d <= limit_bins && d - last <= bridge; ++d) {
            if (mirrored(d)) {
                last = d;
            }
        }
        const double half = std::max(static_cast<double>(last) * bin + margin,
                                     in.detection_width_hz / 2.0);
        low = -half;
        high = half;
        break;
    }

    case AutoFilterRule::Sideband: {
        const double reach =
            std::max(reach_now, in.detection_width_hz) * 1.5;
        const std::ptrdiff_t span = std::min(detail::bins_for(reach, bin), limit_bins);
        double above = 0.0;
        double below = 0.0;
        for (std::ptrdiff_t d = 1; d <= span; ++d) {
            above += pane.excess(centre_bin + d);
            below += pane.excess(centre_bin - d);
        }
        if (!(above > 0.0) && !(below > 0.0)) {
            fit.outcome = AutoFilterOutcome::NoSignal;
            return fit;
        }
        const double lead = 10.0 * std::log10(std::max(above, 1e-300) / std::max(below, 1e-300));
        if (std::fabs(lead) < static_cast<double>(settings.side_ratio_db)) {
            fit.outcome = AutoFilterOutcome::Ambiguous;
            return fit;
        }
        const bool up = lead > 0.0;
        const std::ptrdiff_t from = up ? centre_bin + 1 : centre_bin - span;
        const std::ptrdiff_t to = up ? centre_bin + span : centre_bin - 1;
        const std::ptrdiff_t seed = pane.loudest(from, to);
        if (seed < 0) {
            fit.outcome = AutoFilterOutcome::NoSignal;
            return fit;
        }
        const auto run = pane.grow(seed, detail::bins_for(settings.sideband_bridge_hz, bin),
                                   from, to);
        const double run_low = pane.offset_of(run.low);
        const double run_high = pane.offset_of(run.high);
        if (run_high - run_low < settings.narrow_data_hz) {
            // Data: a tone window around it rather than back to the carrier.
            const double mid = 0.5 * (run_low + run_high);
            const double half =
                std::max(0.5 * (run_high - run_low) + settings.tone_margin_bins * bin,
                         settings.tone_min_width_hz / 2.0);
            low = mid - half;
            high = mid + half;
        } else if (up) {
            low = 0.0;
            high = run_high + margin;
        } else {
            low = run_low - margin;
            high = 0.0;
        }
        break;
    }

    case AutoFilterRule::Tone: {
        const double search = std::max({reach_now, in.detection_width_hz / 2.0, 3.0 * bin});
        const std::ptrdiff_t span = detail::bins_for(search, bin);
        const std::ptrdiff_t seed = pane.loudest(centre_bin - span, centre_bin + span);
        if (seed < 0) {
            fit.outcome = AutoFilterOutcome::NoSignal;
            return fit;
        }
        auto run = pane.grow(seed, 1, centre_bin - span, centre_bin + span);

        // Down to tone_depth_db under the peak and no further, so a strong
        // tone's sidelobes do not widen the window it is judged by.
        const float floor_db = pane.db(seed) - settings.tone_depth_db;
        while (run.low < seed && pane.db(run.low) < floor_db) {
            ++run.low;
        }
        while (run.high > seed && pane.db(run.high) < floor_db) {
            --run.high;
        }
        const double run_low = pane.offset_of(run.low);
        const double run_high = pane.offset_of(run.high);
        const double mid = 0.5 * (run_low + run_high);
        const double half = std::max(0.5 * (run_high - run_low) + settings.tone_margin_bins * bin,
                                     settings.tone_min_width_hz / 2.0);
        low = mid - half;
        high = mid + half;
        break;
    }

    case AutoFilterRule::Occupied: {
        const double search = std::max({reach_now, in.detection_width_hz / 2.0, 3.0 * bin});
        const std::ptrdiff_t span = detail::bins_for(search, bin);
        const std::ptrdiff_t seed = pane.loudest(centre_bin - span, centre_bin + span);
        if (seed < 0) {
            fit.outcome = AutoFilterOutcome::NoSignal;
            return fit;
        }
        const auto run = pane.grow(seed, detail::bins_for(settings.bridge_hz, bin),
                                   centre_bin - limit_bins, centre_bin + limit_bins);
        const double measured =
            std::max(std::fabs(pane.offset_of(run.low)), std::fabs(pane.offset_of(run.high))) +
            margin;
        const double half = std::max(measured, in.detection_width_hz / 2.0);
        low = -half;
        high = half;
        break;
    }

    case AutoFilterRule::None:
        break;
    }

    // What the engine grants: the channel's limit either side, then the
    // minimum width, grown about the middle so a tone window stays on the
    // tone.
    const auto limit = static_cast<double>(in.edge_limit_hz);
    low = std::clamp(low, -limit, limit);
    high = std::clamp(high, -limit, limit);
    const auto min_width = static_cast<double>(std::max(in.min_width_hz, 1));
    if (high - low < min_width) {
        const double mid = 0.5 * (low + high);
        low = std::max(mid - min_width / 2.0, -limit);
        high = std::min(low + min_width, limit);
        low = high - min_width;
    }

    fit.low_hz = static_cast<int>(std::lround(low));
    fit.high_hz = static_cast<int>(std::lround(high));
    fit.outcome = fit.low_hz == in.low_hz && fit.high_hz == in.high_hz
                      ? AutoFilterOutcome::Unchanged
                      : AutoFilterOutcome::Fitted;
    return fit;
}

// A word or two for the chip beside the toggle. `measuring` is a fit that is
// due and still averaging.
[[nodiscard]] inline std::string auto_filter_label(bool enabled, bool measuring,
                                                   const AutoFilterFit& last,
                                                   std::string_view demod)
{
    if (!enabled) {
        return "auto filter off";
    }
    if (measuring) {
        return "measuring";
    }
    const auto khz = [](int hz) {
        if (hz == 0) {
            return std::string("0");
        }
        char text[32];
        std::snprintf(text, sizeof text, "%.2f", static_cast<double>(hz) / 1000.0);
        return std::string(text);
    };
    switch (last.outcome) {
    case AutoFilterOutcome::Idle:
        return "fits on the next tune";
    case AutoFilterOutcome::Fitted:
    case AutoFilterOutcome::Unchanged: {
        const std::string what = last.outcome == AutoFilterOutcome::Fitted ? "fitted " : "fits ";
        if (last.low_hz == -last.high_hz) {
            return what + std::string(demod) + " ±" + khz(last.high_hz) + " kHz";
        }
        return what + std::string(demod) + " " + khz(last.low_hz) + " to " + khz(last.high_hz) +
               " kHz";
    }
    case AutoFilterOutcome::Waiting:
        return "waiting for the receiver";
    case AutoFilterOutcome::NoRule:
        return "nothing to fit on " + std::string(demod);
    case AutoFilterOutcome::NoSignal:
        return "no signal to fit";
    case AutoFilterOutcome::Ambiguous:
        return "no clear sideband";
    case AutoFilterOutcome::Dragging:
        return "left to you";
    }
    return "auto filter on";
}

}  // namespace revenant::ui
