// Gardner symbol timing recovery on complex baseband.
//
// F. M. Gardner, "A BPSK/QPSK Timing-Error Detector for Sampled Receivers",
// IEEE Trans. Commun. 34(5), 1986. The detector compares the midpoint between
// two strobes with the difference across them, so it needs two samples per
// symbol and no decisions, which is why it suits a diagnostic tap that does
// not know whether it is locked. A real signal (an FM discriminator's output)
// goes in with a zero imaginary part and the detector reduces to the real
// form.
//
// Interpolation is cubic Lagrange over four samples, and the loop is a second
// order proportional plus integral filter. Written from the paper and from
// textbook loop design, per docs/clean-room.md.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/dsp/types.h"

namespace revenant::dsp {

class GardnerTiming {
public:
    // loop_bandwidth is the noise bandwidth normalised to the symbol rate.
    explicit GardnerTiming(double samples_per_symbol = 2.0, double loop_bandwidth = 0.01);

    void reset();

    struct Strobe {
        Complex32 symbol;
        // Absolute sample position of the strobe, counted from the first
        // sample pushed since the last reset.
        double position = 0.0;
    };

    // Appends samples and returns every strobe they complete, oldest first.
    // The returned span is valid until the next call.
    std::span<const Strobe> push(std::span<const Complex32> samples);

    // The interpolated input at an absolute position. Held from three
    // symbols before the last push's first strobe; anything older reads the
    // oldest sample still kept.
    [[nodiscard]] Complex32 at(double position) const;

    [[nodiscard]] double samples_per_symbol() const { return sps_; }

    // The current symbol period in samples, nominal plus the integrator.
    [[nodiscard]] double period() const { return sps_ + integrator_; }

    // Accumulated timing correction in symbols, wrapped to [-0.5, 0.5). Zero
    // means the strobes fall where a free-running clock at the nominal rate
    // would have put them.
    [[nodiscard]] double phase() const;

    // Fractional clock error the integrator has settled on: positive when
    // the signal's symbols are longer than nominal.
    [[nodiscard]] double rate_error() const { return integrator_ / sps_; }

private:
    double sps_;
    double kp_;
    double ki_;
    double integrator_ = 0.0;
    double correction_ = 0.0;
    double power_ = 0.0;
    double next_ = 0.0;
    std::uint64_t origin_ = 0;
    std::vector<Complex32> history_;
    std::vector<Strobe> out_;
    Complex32 previous_{};
    bool have_previous_ = false;
};

}  // namespace revenant::dsp
