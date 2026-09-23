// The front end the two audio-tone decoders share: receiver audio in, a
// narrow complex baseband around the tone out, plus the one-shot estimate of
// where the tone actually is.
//
// WHY THE AUDIO DECODERS HAVE A FRONT END OF THEIR OWN
//
// PSK31 and CW are decoded from what a receiver's USB or CW demodulator hands
// to attach_audio_sink: real samples at the audio rate, with the wanted signal
// a tone somewhere in the passband, placed there by an operator clicking on a
// waterfall. Neither mode is decodable at the audio rate as it arrives. Both
// want the tone moved to zero hertz, everything else in the passband thrown
// away, and a rate a few hundred times lower. That is the same three steps for
// both, and it is this file.
//
// NO CONSTANT HERE COMES FROM A STANDARD
//
// Mixing, a windowed-sinc low-pass and integer decimation are textbook
// structure. The numbers with citations are the symbol rates and the codes,
// and they live in core/decode/psk31.h and core/decode/cw.h. A reader
// auditing provenance can skip this file, exactly as core/decode/dv_phy.h
// says of itself.
//
// BLOCK INVARIANCE
//
// The mixer's phase is computed from the absolute input sample index, and the
// decimator keeps its history across calls, so the output does not depend on
// how the caller blocked the input. core/dsp/synth/modulators.h requires that
// of every generator for the reason that applies here too: a result that
// depends on the blocking cannot be a reference for anything.
//
// NOTHING HERE IS BIT EXACT AGAINST A GPU TWIN
//
// Host code with no kernel behind it, as for every file in core/decode.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::Complex32;
using dsp::ConstComplexSpan;
using dsp::ConstRealSpan;
using dsp::Hertz;
using dsp::SampleIndex;
using dsp::SampleRate;

struct ToneFrontEndConfig {
    SampleRate rate = 48000;

    // Where the tone is expected in the audio passband. The mixer moves this
    // frequency to zero.
    Hertz centre_hz = 1000;

    // One-sided width kept around the centre, in hertz. Everything the decoder
    // will ever want, including the worst tuning error it claims to capture,
    // has to fit inside it.
    double passband_hz = 100.0;

    // The decimated rate is the highest rate at or above this that divides
    // the input rate exactly. Exactly, because SampleRate is an integer
    // everywhere in this tree and core/decode/dv_phy.h's SymbolSync takes one.
    SampleRate minimum_output_rate = 500;
};

// Mixes real audio to complex baseband around a centre frequency, low-pass
// filters to the passband and decimates by an integer factor.
class ToneFrontEnd {
   public:
    ToneFrontEnd() = default;

    [[nodiscard]] static Expected<ToneFrontEnd> create(const ToneFrontEndConfig& config);

    // Appends every output sample whose input is now complete. Output sample m
    // is centred on input sample m * decimation() - group_delay(), which is how
    // a caller maps anything it finds back to a SampleIndex in the audio.
    void process(ConstRealSpan audio, std::vector<Complex32>& out);

    [[nodiscard]] SampleRate output_rate() const { return output_rate_; }
    [[nodiscard]] std::size_t decimation() const { return decimation_; }

    // Delay of the low-pass, in input samples.
    [[nodiscard]] std::size_t group_delay() const { return (taps_.size() - 1) / 2; }

    void reset();

   private:
    ToneFrontEndConfig config_{};
    std::vector<float> taps_;
    std::size_t decimation_ = 1;
    SampleRate output_rate_ = 0;

    // Mixed samples not yet consumed, starting at absolute input index
    // buffer_start_. The next output is due at input index next_output_.
    std::vector<Complex32> buffer_;
    SampleIndex buffer_start_ = 0;
    SampleIndex next_input_ = 0;
    SampleIndex next_output_ = 0;
};

// A Hamming-windowed sinc low-pass, `taps` odd, unit gain at zero hertz.
[[nodiscard]] Expected<std::vector<float>> design_lowpass(SampleRate rate, double cutoff_hz,
                                                          std::size_t taps);

// Where the strongest spectral line of x^power sits, divided by power, in
// hertz, searching offsets within +/- max_offset_hz.
//
// power 1 finds a carrier. power 2 strips binary phase modulation and power 4
// strips quaternary, which is the standard way to find the carrier of a
// suppressed-carrier signal: raising it to the modulation order collapses the
// constellation onto one point and leaves a line at that multiple of the
// offset.
//
// A scan of the transform at a quarter of a bin, then a parabola through the
// three best points. Returns an error when the span is shorter than 16
// samples or the search reaches past the Nyquist frequency once multiplied by
// power, because an aliased line is found somewhere and reported as the wrong
// answer.
struct ToneEstimate {
    double offset_hz = 0.0;

    // Magnitude of the line over the mean magnitude of the scan, so a caller
    // can tell a clear line from the tallest blade of grass.
    double line_to_mean = 0.0;
};

[[nodiscard]] Expected<ToneEstimate> estimate_tone_offset(ConstComplexSpan samples,
                                                          SampleRate rate, unsigned power,
                                                          double max_offset_hz);

// Where a pair of equal lines half_spacing_hz either side of a centre sits,
// searching centres within +/- max_offset_hz, with no power law applied.
//
// A carrier reversed at every symbol is two tones either side of where the
// carrier would be, so the pair is a suppressed carrier's idle seen directly.
// Raising the signal to a power finds the same centre, but at a low signal to
// noise ratio the noise multiplied by itself and by the signal spreads the
// line's energy across the scan; reading the two tones as they are keeps it.
//
// The statistic is the weaker of the two lines over the mean magnitude of the
// scan, so one strong line, an unmodulated carrier among them, does not pass
// for a pair. The centre is refined by a parabola through the sum of the two
// lines at the three best points. Returns an error on the same inputs
// estimate_tone_offset does, and when the scan's reach, the offset plus half
// the spacing, passes the Nyquist frequency.
[[nodiscard]] Expected<ToneEstimate> estimate_tone_pair(ConstComplexSpan samples, SampleRate rate,
                                                        double half_spacing_hz,
                                                        double max_offset_hz);

}  // namespace revenant::decode
