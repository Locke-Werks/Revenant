// Whether the noise floor is following the strongest signal on the span.
//
// WHAT HAPPENED, AND WHY THIS IS NOT CALLED AN OVERLOAD DETECTOR
//
// Measured on air 2026-09-20, an RTL-SDR v3 centred at 95.1 MHz in an
// ordinary suburban FM environment: with gain=auto the wideband detector
// reported three intermodulation products as real tracks at confidence 1.00.
// Setting gain to 20 improved the measured SNR of KKFM at 98.1 MHz by 5.7 dB
// and every phantom disappeared. Nothing in the engine, on the wire or in the
// client said the front end was in trouble, so the phantoms read as stations.
//
// What follows detects a CORRELATED FLOOR LIFT. That is a symptom of front
// end compression and it is not a measurement of compression. The honest
// sentence is the one this file's verdicts are named after: the noise floor
// across the whole span is moving with the strongest signal on the span, and
// moving faster than it. Everything below is about how much that is worth and
// what else produces it.
//
// WHAT IT COSTS, WHICH IS THE REASON IT IS SHAPED THIS WAY
//
// core/detect/detector.h already computes two arrays every decision and
// publishes both: averaged_power(), the integrated linear power per fine bin,
// and noise_floor(), the local percentile floor under it interpolated from 32
// knots. Those are the two things this needs, so the measurement adds one
// linear pass over the averaged spectrum at the decision rate and nothing
// else. At the shipped 65536 bins and a 0.1 s decision interval that pass is
// one compare and one add per bin, ten times a second, against a decision
// that detector.h measures at 7.4 ms. Nothing is accumulated per frame and no
// second statistic is estimated.
//
// A histogram of the ADC's own samples would be the direct measurement and it
// is not available at that price. The samples cross the bus as cu8 and widen
// on the device during upload, so counting how often they reach the rails
// means a new reduction in core/shaders and a new readback. That is a
// different lane. This file uses what has already been computed.
//
// THE DISCRIMINATOR
//
// A busy band and a compressed front end both raise what a display calls the
// noise floor, and separating them is the whole problem. Three facts do it.
//
//   A busy band lifts SOME of the span. Another station occupies the bins it
//   occupies. A compressed front end lifts ALL of it at once, because the
//   products of a nonlinearity land everywhere, so the test is taken per
//   segment and the verdict is carried by the WEAKEST segment rather than by
//   the mean. One segment lifting proves nothing here.
//
//   A gain change lifts everything too, and it lifts the floor and the signal
//   BY THE SAME NUMBER OF DECIBELS, because it is a multiplication. So the
//   quantity is not the lift, it is the lift per decibel the strongest signal
//   moved: the slope of the segment's floor against the span's peak. A gain
//   change, including the tuner AGC that gain=auto leaves running, sits at a
//   slope of one.
//
//   A nonlinearity does not. Third-order products rise 3 dB for every 1 dB
//   at the input, and second-order products rise 2 dB, so the floor outruns
//   the signal driving it and the slope goes well above one. That is the
//   published behaviour of a memoryless power series and it is why
//   third-order intercept is quoted the way it is: see ITU-R SM.332 on
//   intermodulation in receivers, and any treatment of the two-tone test.
//
// So the statistic is a slope, measured per segment, over a sliding window of
// decisions, and the verdicts below are the three places that slope can sit.
//
// WHAT IT CANNOT TELL APART. Read this before quoting the flag at anyone.
//
//   A broadband interferer from a real nonlinearity. A switching supply, an
//   arcing power line, a cheap LED lamp or a computer monitor that comes on
//   at the same time as a strong signal rises lifts every segment at once
//   with no relation to the receiver at all. If it happens to correlate with
//   the strongest signal over the window, this reports FloorFollowsSignal and
//   is wrong about the cause. It is right about the observation.
//
//   Compression that was there at the first decision and never lifted. The
//   statistic is a slope, so it needs the strongest signal to MOVE. A front
//   end held in hard compression by a signal that never varies produces a
//   high floor and a flat one, and this reports Steady. That is the case the
//   on-air measurement above was taken in, and it is why piece C of this work
//   changed the shipped default rather than relying on the flag: a flag that
//   needs the band to move cannot be the only thing standing between an
//   operator and a phantom.
//
//   Which stage is compressing. The LNA, the mixer, the IF amplifier and the
//   ADC all produce products and this sees the sum of them. It cannot say
//   whether the answer is less gain, an attenuator or a filter.
//
//   A track that is a product from a track that is a station. It says the
//   span is behaving nonlinearly. It does not mark individual detections, and
//   it cannot: on a US FM band every station sits on the 200 kHz raster, so
//   2*f1 - f2 for any pair of stations lands exactly on another legal channel
//   frequency. A frequency-coincidence test there flags real stations as
//   often as products and is worse than saying nothing.
//
//   Anything at all when no detector is running. This reads the detector's
//   own arrays. core/rpc/server.cpp builds a detector on the first request
//   for detections, so the observation is Unmeasured until something asks.
//
// WHAT IT DOES NOT DO, ON PURPOSE. It never touches the gain. The operator
// sets gain and the engine says what it sees. An engine that retuned its own
// front end would be a second AGC fighting the one in the tuner, and the
// on-air measurement above is what one AGC already costs.
//
// THREADING. One thread owns a FrontEndMonitor, the same way one thread owns
// a Detector. observe() writes state that observation() reads and there is no
// lock. core/rpc/server.cpp calls both under the lock it already holds for
// the detector.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/error.h"

namespace revenant::detect {

// Equal parts of the span the slope is measured in, each on its own.
//
// The claim is "every part of the span moved", so this is the resolution at
// which "every" is checked, and it trades two failures against each other. Too
// few parts and one wide station occupying a whole part makes that part's
// floor move with its own occupancy, which is the busy-band false positive.
// Too many and each part holds few enough bins that its floor estimate carries
// its own ripple into the slope.
//
// Sixteen at the shipped 65536 bins is 4096 bins a part, which is 150 kHz at
// 2.4 MS/s: comfortably wider than one broadcast FM station's 200 kHz raster
// slot is loud in, and 4096 bins is enough that the averaged spectrum's own
// fluctuation is negligible against the decibel the slope is measured over.
// See detector.h on kResidualParts, which makes the same trade at the other
// end of the scale and for the same reason.
inline constexpr std::size_t kFrontEndSegments = 16;

// How far back the slope looks, in source seconds.
//
// An exponential window rather than a ring of decisions, so the arithmetic is
// six running sums per segment and a dropped decision costs the right amount
// of weight rather than one step. Ten seconds is about a hundred decisions at
// the shipped interval, long enough to hold several AGC excursions or a fade,
// short enough that the answer describes the band now rather than the band
// when the session started.
inline constexpr double kFrontEndWindowSeconds = 10.0;

// Effective observations the window must hold before any verdict is given.
//
// At a 0.1 s decision interval and a 10 s window the weight settles near 100,
// so thirty is about three seconds of decisions. Below that the sums are
// dominated by whatever the first few decisions happened to see.
inline constexpr double kFrontEndMinWeight = 30.0;

// How far the strongest signal on the span must have moved, as a standard
// deviation in decibels over the window, before a slope against it means
// anything.
//
// A slope is a ratio and its denominator is this spread. detector.h states
// the averaged spectrum's own fluctuation as 0.27 dB on one bin and 0.10 dB
// on eight at the shipped second of averaging; a segment floor is thousands
// of bins and its ripple is far below either. One decibel of drive movement
// against that is enough leverage that a slope of 1.5 is a 1.5 dB floor
// movement and not noise. Below it the monitor says Unmeasured rather than
// guessing, which is the same third state SourcePacing takes for a factor
// nobody has measured yet.
inline constexpr double kFrontEndMinDriveSpreadDb = 1.0;

// Correlation every segment must reach before its slope is quoted.
//
// A slope fitted through a cloud is a number with no statement behind it. Six
// tenths is not a tuned value: it is low enough that a real relationship
// carrying ordinary measurement scatter clears it, and high enough that a
// segment whose floor is essentially still, where the fit divides two small
// numbers, does not.
inline constexpr double kFrontEndCorrelationFloor = 0.6;

// Slope above which the floor is called to be moving with the span at all.
//
// Under one, because a gain change is exactly one and the measurement has
// scatter on it. Anything under this is a floor that is not following.
inline constexpr double kFrontEndScalingSlope = 0.7;

// Slope above which the floor is outrunning the signal driving it, and the
// slope it has to fall back below to stop being called that.
//
// Second-order products predict two, third-order predict three. Entering at
// 1.5 sits clear above the gain-change slope of one and well under the
// cheapest thing it is looking for, so the band between is where a mixture of
// a gain change and a mild nonlinearity lands and is reported as neither.
//
// A REAL SPAN DOES NOT REACH THREE AND THE THRESHOLD IS SET FOR THAT.
// Measured 2026-09-21 in "the flag raises on a front end driven into its
// third order", which runs eight stations through a memoryless cubic hard
// enough to compress its peaks by half: slope 2.13, against 0.99 for the
// identical scene and gain swing with the cubic switched off. Three is what
// the products alone would give; the measured floor is a mixture of those
// products and the thermal noise still underneath them, and the weakest
// segment, which is the one that carries the verdict, is the one with the
// least product power on it. A threshold near three would have refused that
// case.
//
// The two values are a hysteresis, on the argument models/source_pacing.h
// makes for the same construction: a verdict that flickers across one
// threshold on measurement noise trains an operator to ignore the line, which
// is the state this replaces.
inline constexpr double kFrontEndNonlinearEnter = 1.5;
inline constexpr double kFrontEndNonlinearLeave = 1.3;

enum class FrontEndVerdict : std::uint8_t {
    // No detector, not enough decisions yet, or the strongest signal on the
    // span has not moved enough to measure a slope against. A third state and
    // not a clean bill of health.
    Unmeasured = 0,

    // The floor is not following the strongest signal. Nothing to say.
    Steady = 1,

    // Every segment's floor is following it at about one decibel per decibel,
    // which is what a gain change looks like: a multiplication moves the
    // signal and the noise under it by the same amount. With gain=auto that
    // is the tuner's own AGC, and it is worth saying because it explains a
    // display that breathes.
    SpanScales = 2,

    // Every segment's floor is following it FASTER than one for one, so the
    // floor is outrunning the signal driving it. A nonlinearity ahead of the
    // measurement produces exactly this. So do several other things: see WHAT
    // IT CANNOT TELL APART at the top of this file.
    FloorFollowsSignal = 3,
};

[[nodiscard]] const char* front_end_verdict_name(FrontEndVerdict verdict);

// One reading, which is what crosses the wire.
struct FrontEndObservation {
    FrontEndVerdict verdict = FrontEndVerdict::Unmeasured;

    // Decibels of floor movement per decibel the strongest signal on the span
    // moved, taken from the WEAKEST segment, because the claim the verdict
    // makes is about every segment and is only as good as its weakest one.
    // Zero when the verdict is Unmeasured.
    double slope = 0.0;

    // The weakest segment's correlation, on the same argument.
    double correlation = 0.0;

    // Standard deviation of the strongest signal's level over the window, in
    // dB. This is the leverage the slope was fitted with, so it says how much
    // the slope is worth, and it is what kFrontEndMinDriveSpreadDb gates.
    double drive_spread_db = 0.0;

    // How far the span's mean floor sits above the quietest mean floor this
    // monitor has seen, in dB.
    //
    // DESCRIPTIVE AND NOT PART OF ANY VERDICT. It is a session minimum, so it
    // is a running low-water mark and nothing resets it but reset(). It is
    // here because it is the number that makes the sentence concrete: a slope
    // says the floor is following the signal and this says how far it has got.
    double floor_lift_db = 0.0;
};

// Watches the detector's own two arrays and answers the question above.
class FrontEndMonitor {
public:
    FrontEndMonitor() = default;

    // One decision's worth. Both spans are what Detector::averaged_power()
    // and Detector::noise_floor() return: linear power per fine bin, same
    // length, ascending in frequency. elapsed_seconds is the SOURCE time
    // since the previous decision, so a capture replayed at forty times
    // realtime produces the same slope it did live, exactly as detector.h
    // requires of everything on this path.
    //
    // Refuses a mismatched or degenerate pair rather than producing a number
    // from it: a floor array of a different length than the power array is
    // two detectors, which is the fault DetectorStats::frames_rejected exists
    // to make visible one layer down.
    [[nodiscard]] Status observe(std::span<const double> averaged_power,
                                 std::span<const double> noise_floor,
                                 double elapsed_seconds);

    [[nodiscard]] FrontEndObservation observation() const;

    // Forgets everything, including the low-water mark. For a retune, which
    // puts a different piece of spectrum under every segment and makes the
    // history a measurement of somewhere else.
    void reset();

private:
    struct Segment {
        // Exponentially weighted sums of the drive level p and this segment's
        // floor f, both in decibels. Six numbers is a full regression: slope
        // is cov(p,f)/var(p) and the correlation needs var(f) as well.
        double sum_f = 0.0;
        double sum_ff = 0.0;
        double sum_pf = 0.0;
    };

    double weight_ = 0.0;
    double sum_p_ = 0.0;
    double sum_pp_ = 0.0;
    std::array<Segment, kFrontEndSegments> segments_{};

    // Lowest mean floor seen, in dB, and whether anything has been seen.
    double quiet_floor_db_ = 0.0;
    bool have_quiet_ = false;

    FrontEndVerdict verdict_ = FrontEndVerdict::Unmeasured;
    FrontEndObservation last_{};
};

}  // namespace revenant::detect
