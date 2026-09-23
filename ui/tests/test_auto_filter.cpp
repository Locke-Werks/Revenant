// fit_auto_filter and its average: the per-mode rules docs/ui-spectrum.md,
// "Auto filter", sets, asserted on hand-built panes.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows.
//
// The panes are the averaged display tap at docs/ui-spectrum.md's measured
// geometry, 512 bins at 36.62 Hz with the receiver's centre on bin 256, flat
// at the noise mean except for what a case adds. A line is a parabola in
// decibels over its main lobe, which is roughly what a Hann window draws.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <complex>
#include <cstdint>
#include <random>
#include <vector>

#include "models/auto_filter.h"

using Catch::Matchers::WithinAbs;
using revenant::ui::auto_filter_label;
using revenant::ui::auto_filter_rule_for;
using revenant::ui::AutoFilterAverage;
using revenant::ui::AutoFilterFit;
using revenant::ui::AutoFilterInput;
using revenant::ui::AutoFilterOutcome;
using revenant::ui::AutoFilterRule;
using revenant::ui::AutoFilterSettings;
using revenant::ui::fit_auto_filter;
using revenant::ui::kPercentileToNoiseMeanDb;

namespace {

constexpr double kCentre = 1'215'000.0;
constexpr double kBinHz = 37'500.0 / 1024.0;
constexpr int kBins = 512;
constexpr float kNoiseDb = -100.0F;

struct Pane {
    std::vector<float> bins = std::vector<float>(kBins, kNoiseDb);

    [[nodiscard]] static double offset_at(int i) { return (i - kBins / 2) * kBinHz; }

    Pane& line(double offset_hz, float snr_db)
    {
        for (int i = 0; i < kBins; ++i) {
            const double d = (offset_at(i) - offset_hz) / kBinHz;
            const auto level = static_cast<float>(kNoiseDb + snr_db - 3.0 * d * d);
            bins[static_cast<std::size_t>(i)] = std::max(bins[static_cast<std::size_t>(i)], level);
        }
        return *this;
    }

    Pane& band(double low_hz, double high_hz, float snr_db)
    {
        for (int i = 0; i < kBins; ++i) {
            if (offset_at(i) >= low_hz && offset_at(i) <= high_hz) {
                bins[static_cast<std::size_t>(i)] =
                    std::max(bins[static_cast<std::size_t>(i)], kNoiseDb + snr_db);
            }
        }
        return *this;
    }

    [[nodiscard]] AutoFilterInput input(AutoFilterRule rule, int low, int high) const
    {
        AutoFilterInput in;
        in.rule = rule;
        in.power_db = bins;
        in.first_bin_hz = kCentre - (kBins / 2) * kBinHz;
        in.bin_hz = kBinHz;
        in.noise_db = kNoiseDb;
        in.centre_hz = kCentre;
        in.low_hz = low;
        in.high_hz = high;
        in.edge_limit_hz = 18'750;
        in.min_width_hz = 50;
        return in;
    }
};

// The main lobe either side of a measured edge, which every fit adds.
const double kMargin = AutoFilterSettings{}.margin_bins * kBinHz;

}  // namespace

// Rejects offering a fit on raw, where nothing says what the signal is, and
// sending the two sidebands different rules.
TEST_CASE("each mode gets its auto filter rule", "[autofilter]")
{
    CHECK(auto_filter_rule_for("am") == AutoFilterRule::Carrier);
    CHECK(auto_filter_rule_for("usb") == AutoFilterRule::Sideband);
    CHECK(auto_filter_rule_for("lsb") == AutoFilterRule::Sideband);
    CHECK(auto_filter_rule_for("cw") == AutoFilterRule::Tone);
    CHECK(auto_filter_rule_for("nfm") == AutoFilterRule::Occupied);
    CHECK(auto_filter_rule_for("wfm") == AutoFilterRule::Occupied);
    CHECK(auto_filter_rule_for("dsb") == AutoFilterRule::Occupied);
    CHECK(auto_filter_rule_for("raw") == AutoFilterRule::None);
}

// Rejects stopping at the carrier, which is all the detection's width may
// cover, and rejects going out to whatever is loudest nearby: the fit reaches
// the outer pair of mirrored sideband lines and not the neighbour 4 kHz up,
// which has no mirror.
TEST_CASE("AM is fitted out to its outer sideband lines", "[autofilter]")
{
    Pane pane;
    pane.line(0.0, 30.0F);
    pane.line(-1'000.0, 15.0F).line(1'000.0, 15.0F);
    pane.line(-2'500.0, 12.0F).line(2'500.0, 12.0F);
    pane.line(4'000.0, 20.0F);

    AutoFilterInput in = pane.input(AutoFilterRule::Carrier, -5'000, 5'000);
    in.detection_width_hz = 150.0;
    const AutoFilterFit fit = fit_auto_filter(in);
    REQUIRE(fit.outcome == AutoFilterOutcome::Fitted);
    CHECK(fit.low_hz == -fit.high_hz);
    CHECK(fit.high_hz > 2'500 + static_cast<int>(kMargin));
    CHECK(fit.high_hz < 2'800);
}

// Rejects fitting an AM filter to a carrier the receiver is not on: the
// carrier has to be at the centre, which is where a click on it puts it.
TEST_CASE("AM with no carrier at the centre is no signal", "[autofilter]")
{
    Pane pane;
    pane.line(3'000.0, 30.0F);
    const AutoFilterFit fit = fit_auto_filter(pane.input(AutoFilterRule::Carrier, -5'000, 5'000));
    CHECK(fit.outcome == AutoFilterOutcome::NoSignal);
    CHECK(fit.low_hz == -5'000);
    CHECK(fit.high_hz == 5'000);
}

// Rejects a sideband fit that straddles the suppressed carrier, and one whose
// side comes from the mode's name: USB voice above the carrier is fitted from
// the carrier up to its far edge, LSB voice below from its far edge up to the
// carrier, and a USB receiver sitting on energy below its centre is fitted
// below, because on this engine the edges are the sideband.
TEST_CASE("USB and LSB are fitted from the side the energy is on", "[autofilter]")
{
    Pane above;
    above.band(300.0, 2'700.0, 15.0F);
    const AutoFilterFit usb = fit_auto_filter(above.input(AutoFilterRule::Sideband, 300, 2'700));
    REQUIRE(usb.outcome == AutoFilterOutcome::Fitted);
    CHECK(usb.low_hz == 0);
    CHECK_THAT(usb.high_hz, WithinAbs(2'700.0 + kMargin, kBinHz));

    Pane below;
    below.band(-2'700.0, -300.0, 15.0F);
    const AutoFilterFit lsb =
        fit_auto_filter(below.input(AutoFilterRule::Sideband, -2'700, -300));
    REQUIRE(lsb.outcome == AutoFilterOutcome::Fitted);
    CHECK(lsb.high_hz == 0);
    CHECK_THAT(lsb.low_hz, WithinAbs(-2'700.0 - kMargin, kBinHz));

    const AutoFilterFit usb_on_lower =
        fit_auto_filter(below.input(AutoFilterRule::Sideband, 300, 2'700));
    REQUIRE(usb_on_lower.outcome == AutoFilterOutcome::Fitted);
    CHECK(usb_on_lower.high_hz == 0);
    CHECK(usb_on_lower.low_hz < -2'700);
}

// Rejects guessing a side when neither leads: a signal as strong on both
// sides of the carrier is not a sideband signal, and nothing changes.
TEST_CASE("a sideband receiver with energy even on both sides is left alone", "[autofilter]")
{
    Pane pane;
    pane.band(-2'500.0, -300.0, 15.0F).band(300.0, 2'500.0, 15.0F);
    const AutoFilterFit fit = fit_auto_filter(pane.input(AutoFilterRule::Sideband, 300, 2'700));
    CHECK(fit.outcome == AutoFilterOutcome::Ambiguous);
    CHECK(fit.low_hz == 300);
    CHECK(fit.high_hz == 2'700);
}

// Rejects stretching a data signal's filter back to the carrier: a PSK31
// signal 60 Hz wide at 1 kHz on USB gets a tone window around it.
TEST_CASE("narrow data on a sideband gets a tone window", "[autofilter]")
{
    Pane pane;
    pane.band(1'000.0, 1'060.0, 20.0F);
    const AutoFilterFit fit = fit_auto_filter(pane.input(AutoFilterRule::Sideband, 300, 2'700));
    REQUIRE(fit.outcome == AutoFilterOutcome::Fitted);
    CHECK(fit.low_hz > 800);
    CHECK(fit.high_hz < 1'300);
    CHECK(fit.high_hz - fit.low_hz >= 100);
    CHECK_THAT(0.5 * (fit.low_hz + fit.high_hz), WithinAbs(1'030.0, kBinHz));
}

// Rejects leaving CW at the mode's default width, and centring the window on
// the receiver rather than on the tone: a tone 20 Hz off gets a window around
// the tone, as tight as its lobe allows and no tighter than 100 Hz.
TEST_CASE("CW gets a tight window around the tone", "[autofilter]")
{
    Pane pane;
    pane.line(20.0, 25.0F);
    AutoFilterInput in = pane.input(AutoFilterRule::Tone, -250, 250);
    in.detection_width_hz = 90.0;
    const AutoFilterFit fit = fit_auto_filter(in);
    REQUIRE(fit.outcome == AutoFilterOutcome::Fitted);
    CHECK(fit.high_hz - fit.low_hz >= 100);
    CHECK(fit.high_hz - fit.low_hz <= 300);
    CHECK_THAT(0.5 * (fit.low_hz + fit.high_hz), WithinAbs(20.0, kBinHz));
}

// Rejects the default NFM width on a signal of a different width, and a fit
// narrower than the detection's own measurement: the pane's occupied band sets
// it, and the detection widens it when it measured wider.
TEST_CASE("NFM is fitted to the measured occupied width", "[autofilter]")
{
    Pane pane;
    pane.band(-4'000.0, 4'000.0, 15.0F);
    pane.line(9'000.0, 25.0F);

    AutoFilterInput in = pane.input(AutoFilterRule::Occupied, -6'250, 6'250);
    in.detection_width_hz = 7'000.0;
    const AutoFilterFit fit = fit_auto_filter(in);
    REQUIRE(fit.outcome == AutoFilterOutcome::Fitted);
    CHECK(fit.low_hz == -fit.high_hz);
    CHECK_THAT(fit.high_hz, WithinAbs(4'000.0 + kMargin, kBinHz));

    in.detection_width_hz = 11'000.0;
    const AutoFilterFit wider = fit_auto_filter(in);
    CHECK(wider.high_hz == 5'500);
}

// Rejects a fit the engine would refuse or fit for itself: an occupied band
// wider than the channel allows is clamped to the edge limit.
TEST_CASE("every fit is clamped to what the engine grants", "[autofilter]")
{
    Pane pane;
    pane.band(-8'000.0, 8'000.0, 15.0F);
    AutoFilterInput in = pane.input(AutoFilterRule::Occupied, -6'250, 6'250);
    in.edge_limit_hz = 5'000;
    const AutoFilterFit fit = fit_auto_filter(in);
    REQUIRE(fit.outcome == AutoFilterOutcome::Fitted);
    CHECK(fit.low_hz == -5'000);
    CHECK(fit.high_hz == 5'000);
}

// Rejects fitting to the loudest noise bin: with nothing on the pane, every
// rule leaves the filter exactly where it was.
TEST_CASE("noise only changes nothing", "[autofilter]")
{
    const Pane flat;
    for (const AutoFilterRule rule : {AutoFilterRule::Carrier, AutoFilterRule::Sideband,
                                      AutoFilterRule::Tone, AutoFilterRule::Occupied}) {
        INFO("rule " << static_cast<int>(rule));
        const AutoFilterFit fit = fit_auto_filter(flat.input(rule, -3'000, 3'000));
        CHECK(fit.outcome == AutoFilterOutcome::NoSignal);
        CHECK(fit.low_hz == -3'000);
        CHECK(fit.high_hz == 3'000);
    }
}

// The same on noise that is actually noise: sixteen independent frames of it,
// averaged the way the link averages them, leave nothing occupied.
TEST_CASE("noise only, averaged from noisy frames, changes nothing", "[autofilter]")
{
    constexpr std::uint64_t kSeed = 20'260'927;
    INFO("seed " << kSeed);
    std::mt19937_64 rng(kSeed);
    const double noise = std::pow(10.0, kNoiseDb / 10.0);
    std::normal_distribution<double> gauss(0.0, std::sqrt(noise / 2.0));

    AutoFilterAverage average;
    const double first = kCentre - (kBins / 2) * kBinHz;
    const double period = 65'536.0 / 2'400'000.0;
    std::vector<float> frame(kBins);
    for (int n = 0; n < 16; ++n) {
        for (float& bin : frame) {
            bin = static_cast<float>(10.0 * std::log10(std::norm(
                                         std::complex<double>(gauss(rng), gauss(rng)))));
        }
        average.add(frame, first, kBinHz,
                    kNoiseDb - static_cast<float>(kPercentileToNoiseMeanDb), period,
                    n * period);
    }
    REQUIRE(average.ready(AutoFilterSettings{}));

    const std::vector<float> averaged = average.power_db();
    for (const AutoFilterRule rule : {AutoFilterRule::Carrier, AutoFilterRule::Sideband,
                                      AutoFilterRule::Tone, AutoFilterRule::Occupied}) {
        INFO("rule " << static_cast<int>(rule));
        AutoFilterInput in = Pane{}.input(rule, -3'000, 3'000);
        in.power_db = averaged;
        in.noise_db = average.noise_db();
        CHECK(fit_auto_filter(in).outcome == AutoFilterOutcome::NoSignal);
    }
}

// Rejects fighting the operator: a fit that comes due while an edge is under
// the pointer changes nothing, however clear the signal.
TEST_CASE("a drag in progress changes nothing", "[autofilter]")
{
    Pane pane;
    pane.band(-4'000.0, 4'000.0, 20.0F);
    AutoFilterInput in = pane.input(AutoFilterRule::Occupied, -6'250, 6'250);
    in.dragging = true;
    const AutoFilterFit fit = fit_auto_filter(in);
    CHECK(fit.outcome == AutoFilterOutcome::Dragging);
    CHECK(fit.low_hz == -6'250);
    CHECK(fit.high_hz == 6'250);
}

// Rejects fitting before the engine has answered: with no edge limit there is
// nothing to clamp to, and with no edges the mode's default has not arrived.
TEST_CASE("a fit waits for the engine's limit and edges", "[autofilter]")
{
    Pane pane;
    pane.band(-4'000.0, 4'000.0, 20.0F);
    AutoFilterInput in = pane.input(AutoFilterRule::Occupied, -6'250, 6'250);
    in.edge_limit_hz = 0;
    CHECK(fit_auto_filter(in).outcome == AutoFilterOutcome::Waiting);
    in.edge_limit_hz = 18'750;
    in.low_hz = 0;
    in.high_hz = 0;
    CHECK(fit_auto_filter(in).outcome == AutoFilterOutcome::Waiting);
}

// Rejects a fit reported as new when it is the filter already there.
TEST_CASE("a fit that matches the filter is unchanged", "[autofilter]")
{
    Pane pane;
    pane.band(-4'000.0, 4'000.0, 15.0F);
    const AutoFilterInput in = pane.input(AutoFilterRule::Occupied, -6'250, 6'250);
    const AutoFilterFit first = fit_auto_filter(in);
    REQUIRE(first.outcome == AutoFilterOutcome::Fitted);
    const AutoFilterFit again =
        fit_auto_filter(pane.input(AutoFilterRule::Occupied, first.low_hz, first.high_hz));
    CHECK(again.outcome == AutoFilterOutcome::Unchanged);
}

// Rejects averaging overlapping frames as if each were a new look, and
// averaging across a retune: frames closer than a window count once, and a
// frame on a different axis starts the average again.
TEST_CASE("the average counts looks by window and restarts on a new axis", "[autofilter]")
{
    AutoFilterAverage average;
    const std::vector<float> frame(kBins, kNoiseDb);
    const double first = kCentre - (kBins / 2) * kBinHz;
    // Binary fractions, so the window comparison is exact.
    const double period = 1.0 / 32.0;
    const double window = 4.0 * period;
    for (int n = 0; n < 16; ++n) {
        average.add(frame, first, kBinHz, kNoiseDb, window, n * period);
    }
    CHECK(average.frames() == 16);
    CHECK(average.looks() == 4);

    average.add(frame, first + 500.0, kBinHz, kNoiseDb, window, 16 * period);
    CHECK(average.frames() == 1);
    CHECK(average.looks() == 1);
    CHECK_FALSE(average.ready(AutoFilterSettings{}));
}

TEST_CASE("the chip says what the fit did", "[autofilter]")
{
    AutoFilterFit fit;
    CHECK(auto_filter_label(false, false, fit, "am") == "auto filter off");
    CHECK(auto_filter_label(true, true, fit, "am") == "measuring");
    fit.outcome = AutoFilterOutcome::Fitted;
    fit.low_hz = -4'250;
    fit.high_hz = 4'250;
    CHECK(auto_filter_label(true, false, fit, "am") == "fitted am ±4.25 kHz");
    fit.low_hz = 0;
    fit.high_hz = 2'770;
    CHECK(auto_filter_label(true, false, fit, "usb") == "fitted usb 0 to 2.77 kHz");
    fit.outcome = AutoFilterOutcome::NoSignal;
    CHECK(auto_filter_label(true, false, fit, "nfm") == "no signal to fit");
}
