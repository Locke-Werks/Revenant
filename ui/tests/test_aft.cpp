// AftLoop and its measurements: the rules docs/ui-spectrum.md sets for
// automatic frequency tracking, asserted without a window or an engine.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows.
//
// Two kinds of frame. FrameBuilder is noiseless: every bin sits exactly at
// the noise mean unless something is added, which pins a measurement to a
// fraction of a bin. NoisyFrames is the flat pane as the display tap now
// delivers it, one windowed transform per frame with exponentially
// distributed bin power and a 5th percentile measured off the frame itself,
// which is what the gates were re-derived against.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <random>
#include <vector>

#include "models/aft.h"

using Catch::Matchers::WithinAbs;
using revenant::ui::aft_rule_for;
using revenant::ui::AftFrame;
using revenant::ui::AftLoop;
using revenant::ui::AftRule;
using revenant::ui::AftSettings;
using revenant::ui::AftState;
using revenant::ui::kPercentileToNoiseMeanDb;
using revenant::ui::measure_centroid;
using revenant::ui::measure_peak;
using revenant::ui::noise_gate_db;

namespace {

constexpr double kCentre = 7'074'000.0;
constexpr double kBinHz = 10.0;
constexpr int kBins = 512;
constexpr float kNoiseDb = -100.0F;
constexpr double kFrameS = 1.0 / 30.0;

// A frame of kBins around a receiver at centre_hz, with its filter at
// +/- half_width_hz, flat at the noise mean except for what the caller adds.
// Its percentile is where a real frame's would be for that noise mean.
struct FrameBuilder {
    double centre_hz = kCentre;
    double half_width_hz = 1500.0;
    std::vector<float> bins = std::vector<float>(kBins, kNoiseDb);

    [[nodiscard]] double first_bin_hz() const { return centre_hz - (kBins / 2) * kBinHz; }

    [[nodiscard]] double bin_hz_at(int i) const { return first_bin_hz() + i * kBinHz; }

    // A carrier whose lobe is a parabola in decibels, which is what the peak
    // interpolation assumes, so the estimate can be checked to a fraction of
    // a bin.
    FrameBuilder& carrier(double hz, float snr_db)
    {
        for (int i = 0; i < kBins; ++i) {
            const double d = (bin_hz_at(i) - hz) / kBinHz;
            const auto level = static_cast<float>(kNoiseDb + snr_db - 3.0 * d * d);
            bins[static_cast<std::size_t>(i)] = std::max(bins[static_cast<std::size_t>(i)], level);
        }
        return *this;
    }

    // A flat-topped occupied band, the shape a deviation swing averages to.
    FrameBuilder& band(double low_hz, double high_hz, float snr_db)
    {
        for (int i = 0; i < kBins; ++i) {
            if (bin_hz_at(i) >= low_hz && bin_hz_at(i) <= high_hz) {
                bins[static_cast<std::size_t>(i)] = kNoiseDb + snr_db;
            }
        }
        return *this;
    }

    [[nodiscard]] AftFrame frame() const
    {
        AftFrame f;
        f.power_db = bins;
        f.first_bin_hz = first_bin_hz();
        f.bin_hz = kBinHz;
        f.percentile_low_db = kNoiseDb - static_cast<float>(kPercentileToNoiseMeanDb);
        f.search_low_hz = centre_hz - half_width_hz;
        f.search_high_hz = centre_hz + half_width_hz;
        return f;
    }
};

// Runs the loop against a carrier at an absolute frequency given by
// carrier_at(t), moving the receiver whenever the loop says to, and returns
// the receiver's final centre. Every move is recorded.
struct Run {
    double centre_hz = kCentre;
    std::vector<double> move_times;
    std::vector<double> move_sizes;
    bool jumped = false;
};

template <typename CarrierAt>
Run simulate(AftLoop& loop, AftRule rule, double seconds, CarrierAt carrier_at, float snr = 30.0F)
{
    Run run;
    for (double t = 0.0; t < seconds; t += kFrameS) {
        FrameBuilder b;
        b.centre_hz = run.centre_hz;
        b.carrier(carrier_at(t), snr);
        const auto step = loop.step(t, run.centre_hz, rule, b.frame());
        run.jumped = run.jumped || step.state == AftState::Jumped;
        if (step.move) {
            run.move_times.push_back(t);
            run.move_sizes.push_back(step.centre_hz - run.centre_hz);
            run.centre_hz = step.centre_hz;
        }
    }
    return run;
}

AftLoop enabled_loop()
{
    AftLoop loop;
    loop.set_enabled(true);
    return loop;
}

// The flat pane with noise in it, at docs/ui-spectrum.md's measured geometry:
// 512 bins of a 1024-point transform of a 37.5 kS/s display stream, 36.62 Hz
// a bin across 18.75 kHz, one frame per 27.3 ms engine block, and a 6 kHz
// filter, which is 163 bins of it. Each frame is independent here, so
// window_s is the frame period.
struct NoisyFrames {
    static constexpr double kRate = 37'500.0;
    static constexpr int kPaneBins = 512;
    static constexpr double kPaneBinHz = kRate / 1024.0;
    static constexpr double kPeriodS = 65'536.0 / 2'400'000.0;

    explicit NoisyFrames(std::uint64_t seed) : rng(seed) {}

    std::mt19937_64 rng;
    std::vector<float> bins = std::vector<float>(kPaneBins, 0.0F);
    double centre_hz = kCentre;
    double half_width_hz = 3'000.0;

    [[nodiscard]] double first_bin_hz() const
    {
        return centre_hz - (kPaneBins / 2) * kPaneBinHz;
    }

    // One frame: complex Gaussian noise of mean power kNoiseDb in every bin,
    // plus, when snr_db is set, a carrier at carrier_hz whose peak-bin power
    // is snr_db over the noise mean, falling off across its main lobe.
    AftFrame next(double carrier_hz, bool carrier_on, float snr_db)
    {
        const double noise = std::pow(10.0, kNoiseDb / 10.0);
        std::normal_distribution<double> gauss(0.0, std::sqrt(noise / 2.0));
        const double peak = std::sqrt(noise * std::pow(10.0, snr_db / 10.0));
        for (int i = 0; i < kPaneBins; ++i) {
            std::complex<double> value(gauss(rng), gauss(rng));
            if (carrier_on) {
                const double d = (first_bin_hz() + i * kPaneBinHz - carrier_hz) / kPaneBinHz;
                if (std::fabs(d) < 2.0) {
                    // A Hann main lobe's amplitude, near enough: one at the
                    // centre and zero two bins out.
                    value += peak * 0.5 * (1.0 + std::cos(3.14159265358979 * d / 2.0));
                }
            }
            bins[static_cast<std::size_t>(i)] =
                static_cast<float>(10.0 * std::log10(std::norm(value)));
        }

        // The engine's rank: floor(bins * 50 / 1000).
        std::vector<float> sorted = bins;
        const auto rank = static_cast<std::ptrdiff_t>(kPaneBins * 50 / 1000);
        std::nth_element(sorted.begin(), sorted.begin() + rank, sorted.end());

        AftFrame f;
        f.power_db = bins;
        f.first_bin_hz = first_bin_hz();
        f.bin_hz = kPaneBinHz;
        f.percentile_low_db = sorted[static_cast<std::size_t>(rank)];
        f.window_s = kPeriodS;
        f.search_low_hz = centre_hz - half_width_hz;
        f.search_high_hz = centre_hz + half_width_hz;
        return f;
    }
};

}  // namespace

// Rejects a table that offers a stand-in centre on a mode the doc says has
// none to measure: SSB and DSB have no carrier, and raw has no statement of
// what the signal is.
TEST_CASE("each mode gets the doc's rule, and the carrierless ones hold", "[aft]")
{
    CHECK(aft_rule_for("am") == AftRule::Peak);
    CHECK(aft_rule_for("cw") == AftRule::KeyedPeak);
    CHECK(aft_rule_for("nfm") == AftRule::Centroid);
    CHECK(aft_rule_for("wfm") == AftRule::Centroid);
    CHECK(aft_rule_for("usb") == AftRule::Hold);
    CHECK(aft_rule_for("lsb") == AftRule::Hold);
    CHECK(aft_rule_for("dsb") == AftRule::Hold);
    CHECK(aft_rule_for("raw") == AftRule::Hold);
    CHECK(aft_rule_for("") == AftRule::Hold);
}

// Rejects a loop that starts enabled.
TEST_CASE("AFT is off until it is turned on", "[aft]")
{
    AftLoop loop;
    CHECK_FALSE(loop.enabled());
    const Run run = simulate(loop, AftRule::Peak, 5.0, [](double) { return kCentre + 800.0; });
    CHECK(run.move_times.empty());
}

// Rejects nearest-bin peak picking, which makes the effective deadband a bin
// wide whatever it is set to. The level is read against the noise mean now,
// not against the percentile.
TEST_CASE("the peak lands between bins", "[aft]")
{
    FrameBuilder b;
    b.carrier(kCentre + 123.4, 30.0F);
    const auto m = measure_peak(b.frame());
    REQUIRE(m.present);
    CHECK_THAT(m.hz, WithinAbs(kCentre + 123.4, 0.5));
    CHECK_THAT(m.snr_db, WithinAbs(30.0, 0.5));
}

// Rejects searching the whole frame: a louder signal next to the filter is
// not the one the receiver is on.
TEST_CASE("the peak is looked for inside the filter only", "[aft]")
{
    FrameBuilder b;
    b.carrier(kCentre + 300.0, 20.0F).carrier(kCentre + 2000.0, 40.0F);
    const auto m = measure_peak(b.frame());
    REQUIRE(m.present);
    CHECK_THAT(m.hz, WithinAbs(kCentre + 300.0, 1.0));
}

TEST_CASE("the centroid of a symmetric band is its middle", "[aft]")
{
    FrameBuilder b;
    b.band(kCentre + 200.0, kCentre + 1000.0, 25.0F);
    const auto m = measure_centroid(b.frame(), AftSettings{});
    REQUIRE(m.present);
    CHECK_THAT(m.hz, WithinAbs(kCentre + 600.0, kBinHz));
}

// Rejects a centroid over every bin within the window of the loudest, which
// is the whole filter once the signal is weaker than the window is deep: a
// second, weaker band inside the filter pulled the old centroid 167 Hz
// towards itself. The occupied band is the run containing the loudest, and a
// gap of 49 bins ends it.
TEST_CASE("the centroid is taken over the occupied band only", "[aft]")
{
    FrameBuilder b;
    b.half_width_hz = 1'500.0;
    b.band(kCentre - 400.0, kCentre + 400.0, 15.0F);
    b.band(kCentre + 900.0, kCentre + 1'400.0, 10.0F);
    const auto m = measure_centroid(b.frame(), AftSettings{});
    REQUIRE(m.present);
    CHECK_THAT(m.hz, WithinAbs(kCentre, kBinHz));
}

// Rejects a loop with no deadband, which hunts.
TEST_CASE("an error inside the deadband moves nothing", "[aft]")
{
    AftLoop loop = enabled_loop();
    const Run run = simulate(loop, AftRule::Peak, 10.0, [](double) { return kCentre + 20.0; });
    CHECK(run.move_times.empty());
}

// Rejects a loop that jumps straight to the measurement, and one that moves
// on every frame: moves are spaced by the interval and each is bounded by the
// slew, and it still arrives.
TEST_CASE("a large error is closed at the rate limit", "[aft]")
{
    const AftSettings settings;
    AftLoop loop(settings);
    loop.set_enabled(true);
    const double carrier = kCentre + 1000.0;
    const Run run = simulate(loop, AftRule::Peak, 20.0, [&](double) { return carrier; });

    REQUIRE_FALSE(run.move_times.empty());
    const double largest = settings.max_slew_hz_per_s * settings.min_interval_s;
    for (const double size : run.move_sizes) {
        CHECK(size > 0.0);
        CHECK(size <= largest + 1e-9);
    }
    for (std::size_t i = 1; i < run.move_times.size(); ++i) {
        CHECK(run.move_times[i] - run.move_times[i - 1] >= settings.min_interval_s - 1e-9);
    }
    CHECK(std::fabs(carrier - run.centre_hz) <= settings.deadband_hz);
    CHECK_FALSE(run.jumped);
}

// Rejects a loop that moves on an empty frame, where the "peak" is whichever
// noise bin was loudest.
TEST_CASE("with no signal it holds still", "[aft]")
{
    AftLoop loop = enabled_loop();
    FrameBuilder b;
    for (int i = 0; i < kBins; ++i) {
        b.bins[static_cast<std::size_t>(i)] = kNoiseDb + static_cast<float>((i * 7) % 5);
    }
    for (double t = 0.0; t < 5.0; t += kFrameS) {
        const auto step = loop.step(t, kCentre, AftRule::Peak, b.frame());
        CHECK_FALSE(step.move);
        CHECK(step.state == AftState::NoSignal);
    }
}

// The gate the loop replaced, shown to be wrong on the flat pane before the
// loop is shown to be right on it: the loudest noise bin in a 6 kHz filter
// stands 15 dB and more over the frame's own 5th percentile in every frame, so
// the 10 dB "above the floor" the old gate asked for is met by noise alone.
// Against the noise mean it clears the re-derived gate about one frame in
// twenty, which is what that gate was sized for.
TEST_CASE("noise alone clears a gate measured from the percentile", "[aft]")
{
    constexpr std::uint64_t kSeed = 20'260'923;
    INFO("seed " << kSeed);
    NoisyFrames noise(kSeed);
    const AftSettings settings;
    CHECK_THAT(noise_gate_db(164, settings.false_alarm, settings.min_snr_db),
               WithinAbs(9.08, 0.05));

    int over_old_gate = 0;
    int over_new_gate = 0;
    constexpr int kFrames = 4'000;
    for (int i = 0; i < kFrames; ++i) {
        const AftFrame f = noise.next(0.0, false, 0.0F);
        const auto m = measure_peak(f, kNoiseDb);
        REQUIRE(m.present);
        CHECK(m.bins == 163);
        const float peak_db = m.snr_db + kNoiseDb;
        over_old_gate += peak_db - f.percentile_low_db >= 10.0F ? 1 : 0;
        over_new_gate +=
            m.snr_db >= noise_gate_db(m.bins, settings.false_alarm, settings.min_snr_db) ? 1 : 0;
    }
    CHECK(over_old_gate == kFrames);
    CHECK(over_new_gate > kFrames / 40);
    CHECK(over_new_gate < kFrames / 10);
}

// Rejects gating against one frame's percentile: it scatters by about 0.86 dB
// on 512 bins, which lets noise through the 9 dB gate in about 18% of frames
// instead of 5%. The loop's average sits within a fraction of a decibel of the
// true mean after a second.
TEST_CASE("the noise reference is averaged over frames", "[aft]")
{
    constexpr std::uint64_t kSeed = 20'260'926;
    INFO("seed " << kSeed);
    NoisyFrames noise(kSeed);
    AftLoop loop = enabled_loop();
    float worst_single = 0.0F;
    for (double t = 0.0; t < 3.0; t += NoisyFrames::kPeriodS) {
        const AftFrame f = noise.next(0.0, false, 0.0F);
        worst_single = std::max(worst_single, std::fabs(f.noise_db() - kNoiseDb));
        static_cast<void>(loop.step(t, kCentre, AftRule::Peak, f));
        if (t > 1.0) {
            CHECK(std::fabs(loop.noise_db() - kNoiseDb) < 0.5F);
        }
    }
    CHECK(worst_single > 1.0F);
}

// Rejects the old gates on the new pane, where noise inside the filter
// stands about 20 dB over the percentile: two minutes of noise and nothing
// else, on every rule and four seeds, must not move the receiver once. Twelve
// seeds were run when this was written and none moved it.
TEST_CASE("on the flat pane, noise alone holds on every rule", "[aft]")
{
    for (std::uint64_t run = 0; run < 4; ++run) {
        const std::uint64_t seed = 20'260'924 + 1'000 * run;
        INFO("seed " << seed);
        for (const AftRule rule : {AftRule::Peak, AftRule::KeyedPeak, AftRule::Centroid}) {
            INFO("rule " << static_cast<int>(rule));
            NoisyFrames noise(seed + static_cast<std::uint64_t>(rule));
            AftLoop loop = enabled_loop();
            int moves = 0;
            int acquiring = 0;
            for (double t = 0.0; t < 120.0; t += NoisyFrames::kPeriodS) {
                const auto step = loop.step(t, kCentre, rule, noise.next(0.0, false, 0.0F));
                moves += step.move ? 1 : 0;
                acquiring += step.state == AftState::Acquiring ? 1 : 0;
            }
            CHECK(moves == 0);

            // The gate does pass noise now and then, which is why confirmation
            // exists; a run that never reached acquiring would be a gate set
            // so high it proves nothing about the hold.
            CHECK(acquiring > 0);
        }
    }
}

// Rejects a gate so high that it holds on noise by holding on everything: a
// carrier whose peak bin is 10 dB over the flat floor, 400 Hz off, is found
// and followed to within the deadband, on four seeds.
TEST_CASE("on the flat pane, a carrier 10 dB up is followed", "[aft]")
{
    const AftSettings settings;
    for (std::uint64_t run = 0; run < 4; ++run) {
        const std::uint64_t seed = 20'260'925 + 1'000 * run;
        INFO("seed " << seed);
        for (const AftRule rule : {AftRule::Peak, AftRule::KeyedPeak}) {
            INFO("rule " << static_cast<int>(rule));
            NoisyFrames noise(seed + static_cast<std::uint64_t>(rule));
            AftLoop loop(settings);
            loop.set_enabled(true);
            const double carrier = kCentre + 400.0;
            double centre = kCentre;
            int moves = 0;
            for (double t = 0.0; t < 20.0; t += NoisyFrames::kPeriodS) {
                noise.centre_hz = centre;
                const auto step = loop.step(t, centre, rule, noise.next(carrier, true, 10.0F));
                if (step.move) {
                    ++moves;
                    centre = step.centre_hz;
                }
            }
            CHECK(moves >= 4);
            CHECK(std::fabs(carrier - centre) <=
                  std::max(settings.deadband_hz, NoisyFrames::kPaneBinHz) + 5.0);
        }
    }
}

// Rejects a loop that fights the dial: tuning by hand wins for the hold, and
// the loop then starts again from where the receiver was left.
TEST_CASE("tuning by hand holds the loop, then it resumes", "[aft]")
{
    const AftSettings settings;
    AftLoop loop(settings);
    loop.set_enabled(true);
    loop.operator_tuned(0.0);

    FrameBuilder b;
    b.carrier(kCentre + 600.0, 30.0F);
    bool moved_during_hold = false;
    bool moved_after = false;
    for (double t = 0.0; t < settings.operator_hold_s + 2.0; t += kFrameS) {
        const auto step = loop.step(t, kCentre, AftRule::Peak, b.frame());
        if (t < settings.operator_hold_s) {
            CHECK(step.state == AftState::Yielding);
            moved_during_hold = moved_during_hold || step.move;
        } else {
            moved_after = moved_after || step.move;
        }
    }
    CHECK_FALSE(moved_during_hold);
    CHECK(moved_after);
}

// Rejects forget() clearing the hold. A mode change is tuning by hand and a
// new receiver underneath, so the new receiver's first frame arrives just
// after operator_tuned; the loop was seen on screen correcting two seconds
// after a mode change when forget() took the hold with it.
TEST_CASE("a new receiver does not end the operator's hold", "[aft]")
{
    const AftSettings settings;
    AftLoop loop(settings);
    loop.set_enabled(true);
    loop.operator_tuned(0.0);
    loop.forget();

    FrameBuilder b;
    b.carrier(kCentre + 600.0, 30.0F);
    const auto step = loop.step(0.1, kCentre, AftRule::Peak, b.frame());
    CHECK(step.state == AftState::Yielding);
    CHECK_FALSE(step.move);
}

// Rejects a loop that chases whatever is loudest: a signal a kilohertz away
// appearing in a frame is not drift, and it is refused however long it stays.
TEST_CASE("a jump is not chased, even after a long wait", "[aft]")
{
    AftLoop loop = enabled_loop();
    double centre = kCentre;
    for (double t = 0.0; t < 3.0; t += kFrameS) {
        FrameBuilder b;
        b.centre_hz = centre;
        b.carrier(kCentre + 10.0, 30.0F);
        const auto step = loop.step(t, centre, AftRule::Peak, b.frame());
        if (step.move) {
            centre = step.centre_hz;
        }
    }
    const double settled = centre;
    bool first = true;
    for (double t = 3.0; t < 60.0; t += kFrameS) {
        FrameBuilder b;
        b.centre_hz = centre;
        b.carrier(kCentre + 1010.0, 30.0F);
        const auto step = loop.step(t, centre, AftRule::Peak, b.frame());
        // The first frame of it is an outlier, which the loop does not act
        // on and does not announce; see the next case.
        if (!first) {
            CHECK(step.state == AftState::Jumped);
        }
        CHECK_FALSE(step.move);
        first = false;
    }
    CHECK(centre == settled);
}

// Rejects announcing every outlier as a jump. On the flat pane a weak carrier
// is outshone by a noise bin elsewhere in the filter now and then, and the
// chip flashed "signal jumped" on a loop that was tracking; and rejects acting
// on it, which was a 65 Hz move off a carrier the loop was locked to.
TEST_CASE("one frame elsewhere is an outlier, not a jump", "[aft]")
{
    AftLoop loop = enabled_loop();
    FrameBuilder on;
    on.carrier(kCentre + 10.0, 30.0F);
    double t = 0.0;
    AftState before = AftState::Off;
    for (; t < 2.0; t += kFrameS) {
        before = loop.step(t, kCentre, AftRule::Peak, on.frame()).state;
    }
    REQUIRE(before == AftState::Locked);

    FrameBuilder elsewhere;
    elsewhere.carrier(kCentre + 900.0, 30.0F);
    const auto outlier = loop.step(t, kCentre, AftRule::Peak, elsewhere.frame());
    CHECK(outlier.state == AftState::Locked);
    CHECK_FALSE(outlier.move);
    t += kFrameS;
    CHECK(loop.step(t, kCentre, AftRule::Peak, on.frame()).state == AftState::Locked);
}

// Rejects a jump threshold tight enough to trip on real drift.
TEST_CASE("slow drift is followed", "[aft]")
{
    AftLoop loop = enabled_loop();
    const auto carrier_at = [](double t) { return kCentre + 5.0 * t; };
    const Run run = simulate(loop, AftRule::Peak, 60.0, carrier_at);
    CHECK_FALSE(run.jumped);
    CHECK(std::fabs(carrier_at(60.0) - run.centre_hz) <= 60.0);
}

// Rejects CW tracking through the gaps, where the loudest bin is noise or a
// key click rather than the carrier.
//
// WHAT THIS CASE USED TO DO: hold a carrier between the old 10 dB signal gate
// and the 15 dB key-down gate and expect key-up. Both were measured from a
// percentile that sat in the filter's stopband, and neither survived the flat
// pane; key-down is now the same noise gate everything else clears. So the
// key is keyed: it holds while up, having confirmed while down.
TEST_CASE("CW holds while the key is up", "[aft]")
{
    const AftSettings settings;
    AftLoop loop(settings);
    loop.set_enabled(true);

    FrameBuilder down;
    down.carrier(kCentre + 400.0, 30.0F);
    const FrameBuilder up;

    bool confirmed = false;
    for (double t = 0.0; t < 5.0; t += kFrameS) {
        const bool key_down = std::fmod(t, 0.4) < 0.2;
        const auto step =
            loop.step(t, kCentre, AftRule::KeyedPeak, key_down ? down.frame() : up.frame());
        if (step.have_error) {
            confirmed = true;
        }
        if (!key_down && confirmed) {
            CHECK(step.state == AftState::KeyUp);
            CHECK_FALSE(step.move);
        }
    }
    CHECK(confirmed);
}

// Rejects a centroid rule that acts on single frames, which on FM wanders
// with the modulation: it confirms, averages, and only then moves.
TEST_CASE("the centroid rule averages before it moves", "[aft]")
{
    const AftSettings settings;
    AftLoop loop(settings);
    loop.set_enabled(true);
    FrameBuilder b;
    b.band(kCentre - 400.0, kCentre + 1200.0, 25.0F);

    bool moved_early = false;
    bool moved_later = false;
    double first_move = 0.0;
    for (double t = 0.0; t < settings.centroid_tau_s + 2.0; t += kFrameS) {
        const auto step = loop.step(t, kCentre, AftRule::Centroid, b.frame());
        if (t < settings.centroid_tau_s) {
            moved_early = moved_early || step.move;
            CHECK((step.state == AftState::Acquiring || step.state == AftState::Averaging));
        } else if (step.move && !moved_later) {
            moved_later = true;
            first_move = step.centre_hz - kCentre;
        }
    }
    CHECK_FALSE(moved_early);
    CHECK(moved_later);
    CHECK(first_move > 0.0);
}

// Rejects a centroid that, once a noise frame or two has cleared the gate,
// carries on averaging through the frames that did not: that is how noise at
// one frame in twenty would earn a move. A gap longer than the rule allows
// starts it again.
TEST_CASE("a centroid that loses the signal while averaging starts again", "[aft]")
{
    const AftSettings settings;
    AftLoop loop(settings);
    loop.set_enabled(true);
    FrameBuilder signal;
    signal.band(kCentre + 200.0, kCentre + 1000.0, 25.0F);
    const FrameBuilder quiet;

    double t = 0.0;
    for (; t < 0.5; t += kFrameS) {
        static_cast<void>(loop.step(t, kCentre, AftRule::Centroid, signal.frame()));
    }
    for (int i = 0; i <= settings.centroid_max_gap_frames; ++i, t += kFrameS) {
        static_cast<void>(loop.step(t, kCentre, AftRule::Centroid, quiet.frame()));
    }
    const auto step = loop.step(t, kCentre, AftRule::Centroid, signal.frame());
    CHECK(step.state == AftState::Acquiring);
}

// Rejects inventing a centre for sideband: the suppressed carrier is the
// receiver's centre, so there is nothing to track and nothing moves.
TEST_CASE("a mode with no rule never moves", "[aft]")
{
    AftLoop loop = enabled_loop();
    const Run run = simulate(loop, AftRule::Hold, 5.0, [](double) { return kCentre + 800.0; });
    CHECK(run.move_times.empty());
}

// WHAT THIS CASE USED TO DO: expect a move on the very first frame. A first
// frame is now only a candidate, confirmed over confirm_windows windows, so
// the loop is run until it moves and then turned off.
TEST_CASE("turning it off stops it at once", "[aft]")
{
    AftLoop loop = enabled_loop();
    FrameBuilder b;
    b.carrier(kCentre + 800.0, 30.0F);
    bool moved = false;
    double t = 0.0;
    for (; t < 1.0 && !moved; t += kFrameS) {
        moved = loop.step(t, kCentre, AftRule::Peak, b.frame()).move;
    }
    REQUIRE(moved);
    loop.set_enabled(false);
    const auto step = loop.step(t + 1.0, kCentre, AftRule::Peak, b.frame());
    CHECK(step.state == AftState::Off);
    CHECK_FALSE(step.move);
}
