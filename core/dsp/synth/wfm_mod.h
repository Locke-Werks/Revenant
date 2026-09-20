// Broadcast FM: a stereo or mono composite multiplex, carrying RDS, on an FM
// carrier.
//
// WHY THIS EXISTS
//
// core/decode/rds_bits.h decodes a real FM composite, and until this file
// existed nothing in the tree could produce one on a carrier.
// core/dsp/synth/rds_mod.h renders the composite itself, at baseband, which
// is what the decoder was scored against: modulator straight into
// demodulator, with no radio in between. That closes the clause-reading loop
// and nothing else. It cannot answer whether a receiver in this engine,
// tuned to a station, hands its RDS decoder something a decoder can use,
// because the input to that question is an RF signal and rds_mod does not
// make one.
//
// So this is the missing half. It takes the composite rds_mod renders, adds
// the programme audio and the stereo subcarrier, and frequency modulates a
// complex carrier with the result. What comes out is what a WFM receiver
// sees, and what its discriminator produces from it is a composite again.
//
// HOW IT IS STRUCTURED, AND WHY IT IS NOT A Modulation
//
// core/dsp/synth/modulators.h has an eight-member Modulation enum and one
// Modulator class that switches over it. This is not a ninth member, for the
// same reason RdsModulator is not one: a station is a composite generator
// with a payload and a subcarrier plan, not a mode with a deviation and a
// tone, and folding it into Modulator would put an RDS bit sequence into
// every random emitter a wideband scene draws. rds_mod.h set that precedent
// in this directory and this file follows it. What it does share is the
// contract: pure functions of the absolute sample index, exact integer
// carrier phase from siggen::exact_phase, and bit-identical output whatever
// blocking the caller used.
//
// SPECIFICATION AND PROVENANCE
//
// The RDS layer is EN 50067:1998 clauses 1.1 to 1.7, cited in rds_mod.h and
// in core/decode/rds_bits.h, and it is rendered by RdsModulator rather than
// reproduced here.
//
// The rest is the pilot-tone FM stereo system and the FM broadcast channel
// it sits in. Every figure below names the document that specifies it. Two
// of them were not opened for this work and say so at the field, per
// docs/clean-room.md: they are numbers this project states, not text this
// project transcribed, and a reader checking the code against a standard
// should know which is which before spending money on a copy.
//
// WHAT IS NOT HERE
//
// Every station this file renders carries RDS. There is no switch for a
// station without it, because the pilot is RdsModulator's and turning RDS
// off would mean a second pilot generator with the same job. A plain FM
// station is reachable from core/dsp/synth/modulators.h's Nfm mode at a
// wider deviation, which is not the same signal and is not pretending to be.
//
// The programme audio is tones. Pre-emphasis on a tone is one gain and one
// phase shift, exactly, which is what keeps the whole composite integrable
// in closed form and therefore streamable; a supplied audio buffer would
// need the filter run over it and the integral tabulated per sample, which
// is the bounded, non-streaming path modulators.h describes for its own
// supplied-audio modes. Nothing here needs it yet.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "core/dsp/synth/modulators.h"
#include "core/dsp/synth/rds_mod.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::Complex32;
using dsp::ComplexSpan;
using dsp::Hertz;
using dsp::SampleIndex;
using dsp::SampleRate;

// ---------------------------------------------------------------------------
// Pre-emphasis
// ---------------------------------------------------------------------------

// The audio pre-emphasis curve, as a time constant.
//
// Broadcast FM boosts the top of the audio band before modulation and the
// receiver cuts it by the same amount afterwards. The noise a discriminator
// produces rises with frequency, so the cut at the receiver takes the noise
// down with the boost and the audio comes back flat. Two constants are in
// service worldwide and this renders both.
//
// IT IS ON THE AUDIO, NOT ON THE COMPOSITE, AND THAT IS THE WHOLE POINT
//
// Pre-emphasis is applied to L and R before the stereo matrix. It does not
// touch the pilot, it does not touch the 38 kHz stereo subcarrier, and it
// does not touch the 57 kHz RDS subcarrier. A generator that ran the curve
// over the finished composite instead would boost the RDS subcarrier by
// sqrt(1 + (2*pi*57000*tau)^2), which is 25.1 dB at 50 us and 28.6 dB at
// 75 us, and the resulting signal would decode perfectly while carrying an
// injection level nothing in the standard permits. Nothing downstream would
// complain: the decoder would report a clean eye and a high quality figure,
// and every sensitivity number measured against that signal would be
// optimistic by that much.
//
// tests/tools/test_wfm_mod.cpp measures the RDS component's level IN THE
// COMPOSITE render() produces, by projecting that composite onto
// render_rds_only(), and asserts the level is unchanged across the three
// curves while the audio's own level in the same composite moves by the
// curve's gain. The pilot's level is measured the same way, because the
// curve reaches it too at 15.6 dB.
//
// WHAT THIS PARAGRAPH USED TO SAY
//
// Until 2026-09-20 the sentence above read "tests/tools/test_wfm_mod.cpp
// asserts the RDS component is bit-identical with the curve on and off,
// which is the shape of that mistake." The test did compare exactly that,
// and it could not fail: render_rds_only() forwards to RdsModulator,
// RdsModSpec has no pre-emphasis field, and the three iterations were the
// same computation on the same inputs. Changing FmComposite::render to
// out[i] = float((rds + audio) * preemphasis_gain(curve, 57000)), which is
// the mistake this paragraph describes, left the comparison passing bit for
// bit. Anyone who read this header and concluded a regression guard existed
// is owed the retraction rather than a claim quietly swapped for a better
// one.
enum class Preemphasis : std::uint8_t {
    // No pre-emphasis. Not a broadcast station, and useful precisely because
    // it is the reference the other two are measured against.
    None = 0,

    // 75 microseconds. North America. Specified in the FCC's FM broadcast
    // rules, 47 CFR 73.333, which prints the curve as a figure rather than a
    // time constant; 75 us is the constant that curve is universally quoted
    // as and is what is implemented.
    //
    // NOT OPENED FOR THIS WORK. The figure is stated here from general
    // engineering knowledge of the FM broadcast system, not transcribed from
    // the rule. docs/clean-room.md asks for a citation to name a document
    // somebody read, so this one names the document and says plainly that
    // nobody here read it.
    Us75,

    // 50 microseconds. Europe and most of the rest of the world. Specified
    // in ITU-R BS.450, the FM sound broadcasting transmission standard.
    //
    // NOT OPENED FOR THIS WORK, on the same terms as Us75.
    Eu50,
};

[[nodiscard]] std::string_view preemphasis_name(Preemphasis kind);
[[nodiscard]] Expected<Preemphasis> preemphasis_from_name(std::string_view name);

// The time constant in seconds. Zero for None.
[[nodiscard]] double preemphasis_seconds(Preemphasis kind);

// The gain the curve applies to a tone, as an amplitude ratio.
//
// H(s) = 1 + s*tau, normalised to unity at DC, so a tone at f comes out
// sqrt(1 + (2*pi*f*tau)^2) times larger and atan(2*pi*f*tau) radians ahead.
//
// A real transmitter cannot use 1 + s*tau on its own: the gain grows without
// bound and the deviation with it. It pairs the curve with a 15 kHz audio
// band limit and a deviation limiter, and this renders neither, because the
// programme here is a tone at a frequency the caller chose and validate()
// keeps that inside the 15 kHz band. Stated rather than implemented: the
// limiter is a dynamic process and would make the output depend on the
// programme's history, which is the one thing every generator in this
// directory is built not to do.
[[nodiscard]] double preemphasis_gain(Preemphasis kind, Hertz tone_hz);
[[nodiscard]] double preemphasis_phase_radians(Preemphasis kind, Hertz tone_hz);

// ---------------------------------------------------------------------------
// The programme
// ---------------------------------------------------------------------------

// The audio half of the multiplex: what is in the 0 to 15 kHz sum channel and
// in the 38 kHz difference channel.
//
// The pilot is NOT here. It belongs to RdsModSpec, because RdsModulator
// generates it and the 57 kHz subcarrier is locked to it, and generating a
// second one here would give the composite two pilots that agree until
// clock_error_ppm is non-zero.
struct FmProgramme {
    // Stereo puts (L-R)/2 on the 38 kHz subcarrier, which is the second
    // harmonic of the pilot. Mono transmits the left tone alone at baseband
    // and nothing at 38 kHz.
    //
    // Stereo requires the pilot: a receiver has no way to regenerate a
    // suppressed 38 kHz subcarrier without it, so create() refuses stereo
    // with RdsModSpec::pilot_enabled off rather than rendering a signal that
    // decodes as mono and reads in a spec file as stereo.
    bool stereo = true;

    // The two programme tones, before the matrix. Equal frequencies with the
    // default zero phase difference make L and R identical, so (L-R)/2 is
    // zero and the transmission is a mono programme going out through a
    // stereo transmitter, which is what most of the band actually is.
    //
    // 15 kHz is the top of the FM audio band: the sum channel has to stay
    // clear of the pilot at 19 kHz, and the difference channel's upper
    // sideband has to stay clear of the RDS subcarrier's lower edge at
    // 54.625 kHz. validate() enforces 30 Hz to 15 kHz.
    Hertz left_tone_hz = 1000;
    Hertz right_tone_hz = 1000;

    // Peak deviation a single full-scale channel contributes, before
    // pre-emphasis.
    //
    // The matrix does not raise it. |(L+R)/2| + |(L-R)/2| is max(|L|, |R|)
    // for any L and R, so a stereo pair at full scale peaks exactly where a
    // mono programme at full scale does, and the composite's audio never
    // exceeds this figure however the two tones line up. Pre-emphasis does
    // raise it, by preemphasis_gain(), and is the reason a station running
    // the curve has to run a quieter programme to stay inside 75 kHz.
    //
    // 52.5 kHz is 70 percent of 75 kHz, which with a 9 percent pilot and a
    // 2.7 percent RDS injection leaves the composite peak just under full
    // deviation with no pre-emphasis. It is a default that produces a legal
    // station, not a figure out of a standard.
    Hertz audio_deviation_hz = 52500;

    Preemphasis preemphasis = Preemphasis::Eu50;
};

// ---------------------------------------------------------------------------
// The station
// ---------------------------------------------------------------------------

struct WfmSpec {
    // Sample rate of the complex output.
    //
    // This has to carry the whole FM signal, not just the composite. Carson's
    // rule puts the occupied bandwidth at 2*(deviation + 59375), which is
    // 268750 Hz at 75 kHz of deviation, and the spectrum does not stop there.
    // validate() refuses a rate below the Carson figure and says so; four
    // times the 171000 the RDS decoder wants, 684000, is the rate the tests
    // use and leaves the aliased residue far below the data.
    SampleRate rate = 684000;

    // Offset of the FM carrier from baseband DC.
    Hertz carrier_offset = 0;

    // Envelope amplitude. FM is constant envelope, so every output sample has
    // exactly this magnitude and nominal_mean_power() is its square.
    double amplitude = 1.0;

    double initial_phase = 0.0;

    // Peak deviation the composite's full scale corresponds to.
    //
    // 75 kHz is the broadcast figure: EN 50067:1998 clause 1.3 states it as
    // the cap on the whole multiplex, which is the citation rds_mod.h already
    // carries and the reason its composite is normalised so that 1.0 means
    // this.
    //
    // CHANGING IT RESCALES EVERY OTHER DEVIATION IN THIS SPEC, and a reader
    // has to know that before reading the hertz figures below as transmitted
    // hertz. rds_mod.h divides its levels by kCompositePeakDeviationHz, which
    // is fixed at 75000, and FmProgramme does the same, so a field that says
    // 2000 Hz of RDS injection means 2000 Hz only while this field is 75000.
    // Halve this and the station transmits half of every stated deviation:
    // the ratios, which is what an injection level actually is, are
    // unchanged. Stated rather than normalised away because a spec file full
    // of hertz that are not hertz is worse than one arithmetic step.
    Hertz peak_deviation_hz = kCompositePeakDeviationHz;

    FmProgramme programme{};

    // The RDS layer, and the pilot.
    //
    // Used as given except for two fields. rate is overwritten with
    // WfmSpec::rate, because one composite cannot have two sample rates; a
    // different value here is replaced rather than refused, and spec()
    // reports what was actually used. mono_tone_hz and mono_deviation_hz are
    // refused when set, because the programme audio is FmProgramme's job and
    // a tone arriving through this field would bypass the stereo matrix and
    // the pre-emphasis curve both.
    RdsModSpec rds{};
};

[[nodiscard]] Status validate(const WfmSpec& spec);

// The band the station occupies, by Carson's rule against the highest
// component of the composite, which is the RDS subcarrier's upper edge at
// 57000 + 2375 Hz.
[[nodiscard]] Expected<SpectralExtent> occupied_extent(const WfmSpec& spec);

// ---------------------------------------------------------------------------
// The composite
// ---------------------------------------------------------------------------

// The real multiplex, at WfmSpec::rate, scaled so that 1.0 is
// WfmSpec::peak_deviation_hz. Same convention as rds_mod.h, which is where
// the pilot and the 57 kHz subcarrier come from.
//
// Exposed separately from WfmModulator because it is what a test wants to
// compare a discriminator's output against, and because the pre-emphasis
// property above is a statement about this signal rather than about the RF.
class FmComposite {
public:
    // Reads rate, programme and rds from the spec and ignores the four RF
    // fields, which belong to WfmModulator. One spec struct rather than two
    // so that a station is described in one place.
    [[nodiscard]] static Expected<FmComposite> create(WfmSpec spec);

    // Absolute samples [start, start + out.size()). Pure and blocking
    // independent.
    void render(SampleIndex start, dsp::RealSpan out) const;

    // The three parts on their own, at the scale they have in the composite.
    //
    // They sum to render() to within the float rounding and not exactly:
    // render() adds the pilot and the data in double and rounds once, and
    // these round each part on its own, so the sum can sit an ulp away. Do
    // not write a test that compares the two with ==.
    void render_rds_only(SampleIndex start, dsp::RealSpan out) const;
    void render_pilot_only(SampleIndex start, dsp::RealSpan out) const;
    void render_audio_only(SampleIndex start, dsp::RealSpan out) const;

    // The time integral of render(), in samples, from the buffer's origin to
    // the given index, and exactly zero at index zero.
    //
    // Exact, not quadrature: the pilot, the two programme tones and the four
    // products that make up the 38 kHz difference channel all integrate in
    // closed form, and the RDS term does too for the reason
    // RdsModulator::supports_integral() gives. This is what lets an FM phase
    // be a pure function of the absolute index at bounded cost, with nothing
    // accumulated and nothing tabulated per sample.
    [[nodiscard]] double integral(SampleIndex index) const;

    [[nodiscard]] const WfmSpec& spec() const { return spec_; }
    [[nodiscard]] const RdsModulator& rds() const { return rds_; }

    // The largest |composite| the spec can produce, from the component
    // amplitudes rather than from a render. Above 1.0 the station is over
    // deviating; that is reachable on purpose, for the same reason
    // AmParams::modulation_index above 1.0 is.
    [[nodiscard]] double peak_bound() const { return peak_bound_; }

private:
    // RdsModulator has no public default constructor, deliberately: there is
    // no such thing as a default RDS transmitter. So this takes one rather
    // than default-constructing and filling it in, which is what every other
    // create() in this directory does.
    explicit FmComposite(RdsModulator rds) : rds_(std::move(rds)) {}

    // One sinusoid of the analytic half of the composite: everything except
    // the RDS term. amplitude * sin(2*pi*(turns_per_sample*t + phase_turns)).
    //
    // A list rather than named fields because the 38 kHz difference channel
    // is a product of two sinusoids, which is a pair of sinusoids at the sum
    // and difference frequencies, and writing that out by hand at render time
    // and again in the integral is how the two stop agreeing.
    struct Sinusoid {
        double amplitude = 0.0;
        double turns_per_sample = 0.0;
        double phase_turns = 0.0;
    };

    [[nodiscard]] double audio_at(SampleIndex index) const;

    WfmSpec spec_{};
    RdsModulator rds_;
    std::vector<Sinusoid> audio_{};
    double integral_base_ = 0.0;
    double peak_bound_ = 0.0;
};

// ---------------------------------------------------------------------------
// The modulator
// ---------------------------------------------------------------------------

class WfmModulator {
public:
    [[nodiscard]] static Expected<WfmModulator> create(WfmSpec spec);

    // Absolute samples [start, start + out.size()). Pure: the same absolute
    // range always produces the same samples, whatever blocking the caller
    // used to ask for it.
    void render(SampleIndex start, ComplexSpan out) const;

    // The same, summed into out and scaled, for the wideband scene.
    void accumulate(SampleIndex start, ComplexSpan out, double gain) const;

    [[nodiscard]] const WfmSpec& spec() const { return composite_.spec(); }
    [[nodiscard]] const FmComposite& composite() const { return composite_; }

    [[nodiscard]] SpectralExtent occupied_extent() const { return extent_; }

    // Constant envelope, so this is amplitude squared exactly, whatever the
    // programme is doing.
    [[nodiscard]] double nominal_mean_power() const { return nominal_power_; }

    // The RDS bits as handed in, and as put on the subcarrier after clause
    // 1.6's differential encoding. A round trip compares against the first.
    [[nodiscard]] const std::vector<std::uint8_t>& payload_bits() const
    {
        return composite_.spec().rds.bits;
    }
    [[nodiscard]] const std::vector<std::uint8_t>& transmitted_bits() const
    {
        return composite_.rds().transmitted_bits();
    }

    // Samples covering the whole bit sequence plus the shaping filter's
    // run-in and run-out. Rendering fewer truncates the last bits; rendering
    // more keeps transmitting with no data on the subcarrier.
    [[nodiscard]] std::size_t nominal_sample_count() const
    {
        return composite_.rds().nominal_sample_count();
    }

private:
    explicit WfmModulator(FmComposite composite) : composite_(std::move(composite)) {}

    FmComposite composite_;
    SpectralExtent extent_{};
    double nominal_power_ = 0.0;

    // 2*pi*peak_deviation/rate, the factor that turns the composite's
    // integral in samples into carrier phase in radians.
    double phase_scale_ = 0.0;
};

// ---------------------------------------------------------------------------
// One-shot generation
// ---------------------------------------------------------------------------

struct GeneratedStation {
    std::vector<Complex32> samples;

    // The composite that modulated them, at the same indices. Carried
    // because the interesting comparison for this mode is against the
    // composite rather than against a bit stream: a discriminator's output
    // should be this, and a test that only checked the bits could not tell a
    // correct composite from one whose audio and data were swapped in level.
    std::vector<float> composite;

    std::vector<std::uint8_t> payload_bits;
    std::vector<std::uint8_t> transmitted_bits;

    SpectralExtent extent{};

    // Measured over the buffer produced, not assumed.
    double measured_mean_power = 0.0;
    double peak_magnitude = 0.0;
    double composite_peak = 0.0;
    double composite_mean_power = 0.0;

    // The RDS component alone, measured the same way. This is the signal
    // power an SNR on the data is calibrated against; see rds_mod.h.
    double rds_peak = 0.0;
    double rds_mean_power = 0.0;

    // True when composite_peak exceeds 1.0, which is deviation past
    // WfmSpec::peak_deviation_hz. Reported rather than clamped.
    bool over_deviated = false;
};

// sample_count of zero means nominal_sample_count().
[[nodiscard]] Expected<GeneratedStation> generate_wfm(const WfmSpec& spec,
                                                      std::size_t sample_count = 0);

}  // namespace revenant::siggen
