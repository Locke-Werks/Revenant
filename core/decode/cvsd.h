// CVSD, continuously variable slope delta modulation at 16 or 32 kbit/s: one
// bit per sample in, 8 kHz PCM out. An encoder alongside, for tests and
// modulators.
//
// SPECIFICATION
//
// Implements MIL-STD-188-113, "Interoperability and Performance Standards for
// Analog-to-Digital Conversion Techniques", 17 February 1987, section 5.2 and
// its Appendix C. Read from the everyspec.com scan (download.php?spec=
// MIL-STD-188_113.024875.pdf, fetched 2026-11-02), whose cover notes an
// administrative change only. The document was cancelled without
// replacement in July 2001; it remains the published definition, and the
// IRIG 106 telemetry standard carries its CVSD appendix forward unchanged.
//
// Clauses used: 5.2.1 and figure 2 (the encoder and decoder structure: 3-bit
// shift register, run-of-threes overload algorithm XYZ + X'Y'Z', syllabic
// filter, pulse amplitude modulator, reconstruction integrator), 5.2.2 (the
// 30-percent run-of-threes pattern defines 0 dBm0), 5.2.3.2 (16 and 32
// kbit/s), 5.2.3.4 (overload algorithm), 5.2.3.5 (compression ratio 16:1
// nominal), 5.2.3.6 (syllabic time constant 5 ms, exponential charge and
// discharge), 5.2.3.7 (reconstruction integrator time constant 1 ms),
// 5.2.3.8 (an 800 Hz 0 dBm0 tone gives a 0.30 duty cycle), 5.2.3.9.1 and
// table VI (the decoder reference patterns and their levels), 5.2.3.9.2
// (conversion speed). Appendix C 50.2 to 50.9 for the guidance on each block,
// in particular 50.7 (the integrator is leaky) and 50.9 (the envelope after
// a pattern change).
//
// WHAT THE DOCUMENT LEAVES TO THE IMPLEMENTER, AND WHAT WAS CHOSEN
//
//  1. The syllabic filter's two levels. 5.2.3.5 fixes only their ratio as
//     measured with the two patterns. A one-pole filter charging toward
//     V_max while the overload output is true and toward V_min while it is
//     false settles at V_min + d (V_max - V_min) for duty cycle d, so a 16:1
//     ratio between d = 0.30 and d = 0 makes V_max = 51 V_min.
//  2. Absolute level. 5.2.2 defines 0 dBm0 as whatever the decoder puts out
//     for the 30-percent pattern, which is circular for a digital decoder.
//     Here 0 dBm0 is a sine of peak kCvsdZeroDbm0Peak, a project choice, and
//     V_min is calibrated once per rate so that the 30-percent pattern of
//     table VI decodes to exactly that level. Everything else then follows
//     from the clauses above and is checked by the tests rather than
//     imposed; see note 5 for the one table VI figure that does not follow.
//  3. Filters. 5.2.3.3 leaves the input and output filters to equipment
//     specifications. Both sides here are 8 kHz PCM, so the encoder
//     interpolates to the bit rate and the decoder decimates back, each with
//     a windowed-sinc lowpass at 3.4 kHz. Appendix C 50.8's 40 dB per octave
//     and 45 dB stopband are met.
//  4. The syllabic time constant. 5.2.3.9.2 asks the decoded envelope to
//     reach 90 percent of the 30-percent level within 9 to 14 ms of a switch
//     from the 0-percent pattern, and to fall to 10 percent of it within 6
//     to 9 ms of the switch back. With a 16:1 ratio both scale with the
//     syllabic time constant, and measured on this decoder's output (the 1
//     ms integrator included) the rise needs at least 4.26 ms and the fall
//     at most 3.78 ms: no value meets both. 4.2 ms, inside 5.2.3.6's 5 +- 1
//     ms, measures 8.9 to 9.3 ms rising and 9.2 to 10.0 ms falling, each
//     within about a millisecond of its window, and is used. The tests print
//     the figures.
//  5. Table VI's 0-percent level cannot be met together with 5.2.3.5. The
//     two 16 kbit/s patterns have 800 Hz components in the ratio 1.855 (5.4
//     dB; 1.902 at 32 kbit/s), so at a 16:1 slope ratio the 0-percent pattern
//     decodes about 29.5 dB below the 30-percent one, not 24. Meeting -24
//     would need a slope ratio near 8.6, outside 5.2.3.5's 12 to 21.
//     Appendix C 50.9 gives the final 0-percent amplitude as 0.03 of the
//     30-percent one, which is the -30 dB this decoder produces. The
//     nominal 16:1 is kept and the tests hold the 0-percent level to the
//     arithmetic rather than to table VI.
//  6. Bit polarity. Figure 2 makes the digital output the comparator's sign;
//     a 1 here means the input was above the integrator, so the integrator
//     steps up on a 1.
//
// CLEAN ROOM
//
// No CVSD implementation was read: not spandsp, not any Bluetooth CVSD code,
// not a vendor application note's code listing. The structure is figure 2
// of the standard and the constants are its clauses.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/error.h"

namespace revenant::decode {

// 5.2.3.2.
inline constexpr std::uint32_t kCvsdRate16k = 16000;
inline constexpr std::uint32_t kCvsdRate32k = 32000;
inline constexpr std::uint32_t kCvsdPcmRateHz = 8000;

// 5.2.3.6 allows 5 ms +-1 ms; header note 4 says why 4.2 ms. 5.2.3.7 and
// 5.2.3.5 are used at their nominal values.
inline constexpr double kCvsdSyllabicSeconds = 0.0042;
inline constexpr double kCvsdIntegratorSeconds = 0.001;
inline constexpr double kCvsdCompressionRatio = 16.0;

// Header note 2: the peak of a 0 dBm0 sine on the +-1 float scale. Chosen to
// leave about 6 dB above the reference level before clipping, which is the
// headroom a telephone channel's 0 dBm0 has to its overload point.
inline constexpr double kCvsdZeroDbm0Peak = 0.5;

// Table VI as bits, first bit first ("left to right"), one per byte.
// run_of_threes is 0 or 30. Empty for any other rate or percentage.
[[nodiscard]] std::vector<std::uint8_t> cvsd_reference_pattern(std::uint32_t bit_rate,
                                                               int run_of_threes);

// The loop both ends share: shift register, overload algorithm, syllabic
// filter, PAM and leaky integrator (figure 2).
class CvsdLoop {
public:
    explicit CvsdLoop(std::uint32_t bit_rate);

    // Clocks one bit through and returns the integrator output.
    double clock(std::uint8_t bit);

    [[nodiscard]] double integrator() const noexcept { return integrator_; }
    [[nodiscard]] double syllabic() const noexcept { return syllabic_; }
    [[nodiscard]] bool overload() const noexcept { return overload_; }
    void reset();

private:
    std::uint32_t bit_rate_;
    double syllabic_decay_;
    double integrator_decay_;
    double v_min_;
    double v_max_;
    std::uint8_t shift_ = 0;
    int bits_seen_ = 0;
    bool overload_ = false;
    double syllabic_ = 0.0;
    double integrator_ = 0.0;
};

class CvsdDecoder {
public:
    // bit_rate is kCvsdRate16k or kCvsdRate32k; anything else is refused at
    // the first decode().
    explicit CvsdDecoder(std::uint32_t bit_rate);

    // bits one per byte, 0 or 1. Appends one 8 kHz sample per 2 (16 kbit/s)
    // or 4 (32 kbit/s) bits; a remainder waits for the next call.
    [[nodiscard]] Status decode(std::span<const std::uint8_t> bits, std::vector<float>& out);

    void reset();

    [[nodiscard]] std::string_view name() const noexcept { return "CVSD"; }
    [[nodiscard]] std::uint32_t bit_rate() const noexcept { return bit_rate_; }

    // Fraction of clocks in the last decode() call on which the overload
    // algorithm fired, the "duty cycle" of 5.2.3.8.
    [[nodiscard]] double last_duty_cycle() const noexcept { return duty_; }

private:
    std::uint32_t bit_rate_;
    CvsdLoop loop_;
    std::vector<double> taps_;
    std::vector<double> history_;
    std::size_t head_ = 0;
    std::size_t phase_ = 0;
    double duty_ = 0.0;
};

class CvsdEncoder {
public:
    explicit CvsdEncoder(std::uint32_t bit_rate);

    // pcm at 8 kHz; appends 2 or 4 bits per sample.
    [[nodiscard]] Status encode(std::span<const float> pcm, std::vector<std::uint8_t>& bits);

    void reset();

    [[nodiscard]] double last_duty_cycle() const noexcept { return duty_; }

private:
    std::uint32_t bit_rate_;
    CvsdLoop loop_;
    std::vector<double> taps_;
    std::vector<double> history_;
    std::size_t head_ = 0;
    double duty_ = 0.0;
};

}  // namespace revenant::decode
