// Receiver pieces shared by the MIL-STD-188-110 serial-tone receivers in
// mil110.h (section 5.3.2) and mil110c.h (Appendix C): the front end that
// turns audio into complex baseband at four samples a symbol, and the
// fractionally spaced decision feedback equaliser with its phase and symbol
// timing loops.
//
// Neither is specified by MIL-STD-188-110C, which describes only the
// transmitter. Both are written from textbook theory:
//
//   - The matched filter is the root raised cosine of roll-off 0.35 that
//     Appendix C, C.5.1.4, recommends for both ends.
//   - The equaliser is a T/2 decision feedback equaliser adapted by
//     normalised LMS (Proakis, Digital Communications, 5th ed., sections 9.5
//     and 10.1), started from a least-squares solve over known symbols when
//     there are enough of them (the same book, section 10.4, the LS
//     criterion), with a second-order decision-directed phase loop.
//   - Symbol timing is tracked by the minimum-MSE timing gradient (Mengali
//     and D'Andrea, Synchronization Techniques for Digital Receivers,
//     chapter 7, data-aided timing recovery): the equaliser's input taps are
//     read at fractional positions by cubic Lagrange interpolation of the
//     four-sample-a-symbol baseband, and a proportional-plus-integral loop
//     moves the read position, so a constant sample clock offset ends up in
//     the integrator. The equaliser could absorb the same error by moving its
//     taps, so the two are kept from wandering against each other by
//     re-centring the taps a whole T/2 step at a time when their energy
//     centroid has moved a step from where training left it.

#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode::mil110_rx {

using C = dsp::Complex32;

// Baseband samples a symbol after the front end. Four rather than two so the
// equaliser's T/2 taps can be read between samples accurately: the baseband
// occupies at most 1620 Hz of the 9600 S/s, where a cubic interpolator is
// close to exact; at two samples a symbol it is not.
inline constexpr int kSps = 4;

class FrontEnd {
public:
    [[nodiscard]] Status init(dsp::SampleRate rate, double carrier_hz,
                                      double symbol_rate);
    void reset();
    void push(dsp::ConstRealSpan audio);

    [[nodiscard]] bool available(std::int64_t index) const {
        return index < base_ + static_cast<std::int64_t>(bb_.size());
    }
    // Zero before the first sample kept.
    [[nodiscard]] C at(std::int64_t index) const {
        if (index < base_) {
            return {};
        }
        return bb_[static_cast<std::size_t>(index - base_)];
    }
    // Cubic Lagrange interpolation between samples.
    [[nodiscard]] C at(double position) const;
    void discard_before(std::int64_t index);

    // Input samples per baseband sample, and the matched filter's delay in
    // input samples, to map a baseband index back to the audio.
    [[nodiscard]] double ratio() const { return ratio_; }
    [[nodiscard]] std::int64_t delay() const { return static_cast<std::int64_t>(rrc_.size() / 2); }
    [[nodiscard]] std::int64_t to_input(double index) const;

private:
    dsp::SampleRate rate_ = 0;
    double carrier_hz_ = 0.0;
    std::vector<float> rrc_;
    std::vector<C> hist_;
    std::size_t hist_pos_ = 0;
    std::int64_t in_count_ = 0;
    double ratio_ = 1.0;
    C interp_[4]{};
    std::int64_t filt_count_ = 0;
    std::int64_t out_count_ = 0;
    std::vector<C> bb_;
    std::int64_t base_ = 0;
};

// What one equaliser output was computed from, kept so it can be adapted
// later (the 75 bit/s receiver decides 32 symbols at once).
struct Snapshot {
    std::vector<C> x;   // feedforward inputs, derotated
    std::vector<C> dx;  // their derivative in baseband samples
    double theta = 0.0;
};

class Equaliser {
public:
    // ff_symbols either side of the symbol at T/2, fb_taps past decisions.
    void start(std::size_t ff_symbols, std::size_t fb_taps, C centre_gain);

    // The baseband position read for symbol n counted from origin.
    [[nodiscard]] double centre(std::int64_t origin, std::int64_t n) const {
        return static_cast<double>(origin + kSps * n) + tau_;
    }
    // The last baseband index the output for symbol n reads.
    [[nodiscard]] std::int64_t reach(std::int64_t origin, std::int64_t n) const;
    [[nodiscard]] std::int64_t earliest(std::int64_t origin, std::int64_t n) const;

    // Output for symbol n after the phase loop, before any descrambling.
    // omega is the carrier offset removed, radians a baseband sample.
    [[nodiscard]] C output(const FrontEnd& fe, std::int64_t origin, double omega,
                           std::int64_t n, Snapshot& s) const;

    // Normalised LMS toward d. timing also moves the timing loop.
    void adapt(const Snapshot& s, C y, C d, double mu, bool timing);
    // The phase loop on one decision, and the loop step for a group of them.
    void loop(C y, C d);
    void loop_angle(double err, double weight);
    void push_feedback(C d);
    // Once a symbol: the phase and timing integrators.
    void advance();

    // Least-squares solve of every tap over the known symbols given, with
    // feedback on the same known symbols. Returns the mean squared error of
    // the fit, or nothing if the system is singular.
    [[nodiscard]] std::optional<double> solve(const FrontEnd& fe, std::int64_t origin, double omega,
                             std::int64_t first, const std::vector<C>& known);

    // Marks where the tap energy sits after training; recentre() keeps it
    // within a T/2 step of here.
    void mark_centre();
    void recentre();

    [[nodiscard]] double sigma2() const { return sigma2_; }
    void track_error(double e2, double a) { sigma2_ = (1.0 - a) * sigma2_ + a * e2; }
    // Measured symbol period error in parts per million (positive: symbols
    // longer than nominal in receiver samples).
    [[nodiscard]] double clock_ppm() const { return rate_mean_ / kSps * 1e6; }
    void enable_timing(bool on) { timing_ = on; }

private:
    [[nodiscard]] double centroid() const;

    std::size_t ff_symbols_ = 0;
    std::vector<C> ff_;
    std::vector<C> fb_;
    std::vector<C> past_;  // newest first
    double theta_ = 0.0;
    double nu_ = 0.0;
    double tau_ = 0.0;   // baseband samples
    double rate_ = 0.0;  // baseband samples a symbol
    double rate_mean_ = 0.0;  // the same, averaged over about four seconds
    double dy2_ = 0.0;   // running mean of |dy/dtau|^2
    double sigma2_ = 0.1;
    double centre_ref_ = 0.0;
    bool timing_ = false;
};

}  // namespace revenant::decode::mil110_rx
