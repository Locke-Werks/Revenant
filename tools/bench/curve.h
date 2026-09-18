// Storage and comparison for a BER/SNR curve.
//
// One curve per commit, checked in, is what makes a regression visible. Two
// things follow from that and both shape this file.
//
// The file must be byte-identical for two runs of the same sweep, or a diff in
// review is noise and nobody reads it. So the JSON is written by hand with
// sorted keys and fixed precision, and nothing that cannot affect the result
// (the thread count, a timestamp, a hostname) is written at all.
//
// The number a human cares about is not a bit error rate, it is the SNR at
// which the decoder stops working. A curve that moves 0.3 dB to the right has
// lost 0.3 dB of sensitivity no matter what the BER at any single point did,
// so comparison reports the interpolated shift in dB of the threshold crossing
// alongside the per-point deltas.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.h"
#include "tools/bench/sweep.h"

namespace revenant::bench {

// Bumped when the on-disk shape changes. A reader refuses a version it does not
// know rather than silently misreading a field.
inline constexpr int kCurveSchemaVersion = 1;

struct CurvePoint {
    double snr_db = 0.0;
    double ber = 0.0;
    std::uint64_t trials = 0;
    std::uint64_t bits_total = 0;
    std::uint64_t bit_errors = 0;
    std::uint64_t decoded_count = 0;
};

struct Curve {
    // The modulation or protocol the sweep exercised, for example "bpsk".
    std::string mode;

    // What was under test, for example "reference-bpsk-coherent/sps1". Two
    // curves with different subjects are still comparable, and comparison says
    // so, because comparing a new decoder against the reference is a thing
    // somebody will want to do on purpose.
    std::string subject;

    // Supplied by the caller. CI passes the commit it is building; nothing here
    // shells out to git, because a DSP library that reads the environment is a
    // DSP library that behaves differently in a test.
    std::string commit;

    SweepConfig config;
    std::vector<CurvePoint> points;
};

[[nodiscard]] Curve make_curve(std::string mode,
                               std::string subject,
                               std::string commit,
                               const SweepConfig& config,
                               const std::vector<SweepPoint>& points);

[[nodiscard]] std::string curve_to_json(const Curve& curve);
[[nodiscard]] Expected<Curve> curve_from_json(std::string_view text);

// Both open in binary mode so the bytes on disk do not depend on the platform's
// line ending convention, which would defeat the point of a stable file.
[[nodiscard]] Status write_curve_file(std::string_view path, const Curve& curve);
[[nodiscard]] Expected<Curve> read_curve_file(std::string_view path);

struct RegressionOptions {
    // The BER level at which sensitivity is measured. 1e-2 is the usual
    // operating point for an uncoded curve; a coded mode would use its own.
    double ber_threshold = 1e-2;

    // A point is called worse when its BER exceeds the reference BER by more
    // than this factor, plus a counting-noise floor derived from the number of
    // bits measured. Without the floor, a reference point that happened to see
    // zero errors flags on a single error in the candidate.
    double ber_ratio_tolerance = 1.5;

    // How far the threshold crossing may move to the right before it counts as
    // a regression. 0.2 dB is below what any real change in a decoder produces
    // and above the scatter of a sweep with a hundred errors per point.
    double sensitivity_tolerance_db = 0.2;
};

struct PointComparison {
    double snr_db = 0.0;
    double reference_ber = 0.0;
    double candidate_ber = 0.0;
    std::uint64_t reference_bit_errors = 0;
    std::uint64_t candidate_bit_errors = 0;
    bool worsened = false;
};

struct CurveComparison {
    std::vector<PointComparison> points;

    // Quiet NaN when the curve never crosses the threshold inside its swept
    // range, which is a badly chosen range rather than a decoder result.
    double reference_sensitivity_db = 0.0;
    double candidate_sensitivity_db = 0.0;

    // Candidate minus reference. Positive means the candidate needs more SNR
    // to reach the same BER, which is the direction that is bad.
    double sensitivity_delta_db = 0.0;
    bool has_sensitivity_delta = false;

    std::size_t worsened_points = 0;
    bool regressed = false;

    // Everything a reader needs to know that is not a pass or a fail: points
    // present on one side only, a mode or subject mismatch, a curve that never
    // crosses the threshold.
    std::vector<std::string> notes;
};

[[nodiscard]] Expected<CurveComparison> compare_curves(const Curve& reference,
                                                       const Curve& candidate,
                                                       const RegressionOptions& options = {});

// SNR at which the curve crosses below `ber_threshold`, interpolated linearly
// in log10(BER) against dB because that is the space in which a BER curve is
// close to a straight line. Quiet NaN if the curve does not cross within its
// range. Points need not be sorted.
[[nodiscard]] double sensitivity_db(const std::vector<CurvePoint>& points, double ber_threshold);

}  // namespace revenant::bench
