// The waveform characteriser: what a signal IS, without decoding it.
//
// WHY THIS IS A STAGE AND NOT A STEP INSIDE THE DETECTOR
//
// core/detect/detector.h yields a centre, a bandwidth, an SNR and a track,
// and none of those identifies anything. What identifies a waveform is its
// symbol rate, its modulation order, its tone count and spacing, its cyclic
// prefix and its frame period, and not one of those is in a power spectrum.
// They are in the complex baseband, so this stage reads IQ.
//
// THE SCOPE, WHICH IS THE PROJECT OWNER'S AND IS NOT NEGOTIABLE
//
// Identify the modulation and protocol of anything, including traffic that
// will never be decodable. Decode only what is published and unencrypted.
// Never attempt decryption. Framing that travels in the clear under an
// encrypted payload is in scope and is often most of the useful
// information. docs/modes.md states the same rule and is where the
// catalogue's rows come from.
//
// Encryption is exactly what this stage is indifferent to. A cyclic feature
// at the symbol rate is there because the pulse train is there, whatever
// the bits say; a cyclic prefix is there because the transmitter copied
// part of each symbol; a frame period is there because a probe or a sync
// word repeats, and a probe is the part of a waveform that cannot be
// encrypted without making it useless as a probe.
//
// NOT WIRED INTO THE ENGINE, DELIBERATELY
//
// This is a pure library over a span of complex samples with a stated rate.
// In the engine an extract would arrive through a receiver and
// Engine::attach_audio_sink, which is the seam core/decode/rds_bits.h
// already uses, and docs/detection.md's "what a probe receiver actually is"
// section works out what that costs: a real DemodStage rather than the raw
// tap, because a raw coarse channel is 112 times the bandwidth of a
// narrowband signal and puts the classifier 20 dB down.
//
// None of that is here. Keeping the stage pure means every estimator in it
// is testable against core/dsp/synth with no device, no graph and no
// scheduler, which is what the cases in tests/characterise do, and the
// wiring is cheaper later for the seam being a span rather than a stage.
//
// WHAT IT REFUSES, AND WHY THAT IS THE POINT
//
// A characteriser that always answers is a characteriser that is sometimes
// confidently wrong, and a wrong label with right numbers under it is worse
// than no label at all: an operator acts on it, and everything they can
// check agrees with it.
//
// So every estimator below returns a refusal that names the number, the
// cause and the fix, and this stage assembles them rather than collapsing
// them into a bare false. A channel of noise comes back refused with the
// margin it measured and the threshold that margin failed. A broadcast FM
// carrier comes back as analogue FM rather than as two-tone FSK, and the
// reason is a measurement of how wide its histogram modes are. An
// unmodulated carrier comes back as an unmodulated carrier.
//
// THE THREE THINGS THIS DOES NOT ESTABLISH, STATED HERE RATHER THAN FOUND
// OUT LATER
//
// One: a constant-envelope signal with no separable tones is reported as
// analogue FM, and a continuous-phase digital mode whose modulation index
// is low enough to merge its tones lands in the same place. Separating
// those wants a symbol-rate line attributed to a symbol rate rather than to
// a modulation tone, and nothing here does that attribution.
//
// Two: the modulation order comes from the M-th power law, which says
// nothing about a multi-tone signal, so it is read only after the tone test
// has ruled one out. core/characterise/cyclostationary.h says why.
//
// Three: the candidate list is every catalogue row the numbers support, and
// several rows share a physical layer exactly. Narrowing them further is
// reading a frame, which is decoding.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/characterise/catalogue.h"
#include "core/characterise/cyclostationary.h"
#include "core/characterise/periodicity.h"
#include "core/characterise/tones.h"
#include "core/characterise/transform.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::characterise {

// Shortest extract the stage will work on.
//
// Set by the estimator with the largest appetite rather than by a round
// number. estimate_tone_structure needs 4096 sample pairs past its
// amplitude gate; autocorrelation needs 1024 and divides by the overlap;
// analysis_segment wants sixteen segments to average. 16384 clears all
// three with margin, and at 48 kS/s it is a third of a second, which is
// shorter than any transmission worth characterising.
inline constexpr std::size_t kMinCharacteriseSamples = 16384;

// The most a PSK call that measured no symbol rate is allowed to claim.
//
// THE OWNER'S DECISION OF 2026-09-22, and the three properties it asks for:
// such a call is KEPT, it is FLAGGED (Characterisation::psk_without_symbol_rate),
// its confidence is CAPPED here, and may_drive_detection() refuses it.
//
// Kept rather than refused, because real PSK loses its symbol rate before it
// loses its order: tests/characterise/test_empty_channel.cpp walks a 2400
// baud BPSK signal down and it is still called PSK with no rate at 5 dB and at
// 0 dB in 2500 Hz, at 0.93 and 0.65. Refusing would throw those away.
//
// Capped, because with no rate the call rests on the M-th power line alone,
// and a bare carrier whose envelope the noise has taken lights that line
// exactly the way BPSK does. Measured in the same file, with no rate:
//
//   real BPSK, 5 and 0 dB in 2500 Hz     0.929 and 0.650, margins 17.7, 8.1 dB
//   a carrier in noise, 0 to -12 dB      0.667 to 0.989, margins 8.4 to 28.7 dB
//
// The two populations overlap over the whole range, so no cap above a half
// separates them and the order's own margin says nothing about which one a
// call belongs to. With a rate, every call measured sat at 0.972 or above.
//
// So a half, which is the same bar the AnalogueFm branch holds itself to and
// for the same reason: the finding is an elimination between things the one
// remaining test cannot tell apart, not a positive identification. It is also
// the least margin_confidence ever reports for a test that passed, so a
// capped call ranks under every PSK call that found its rate and level with
// the weakest one that did.
inline constexpr double kPskWithoutRateConfidence = 0.5;

struct CharacteriseConfig {
    dsp::SampleRate rate = 0;

    CyclicSearch cyclic{};
    ToneSearch tones{};
    OfdmSearch ofdm{};
    FrameSearch frame{};

    // Share of the spectrum's power in its three strongest adjacent bins
    // above which the extract is one unmodulated carrier.
    //
    // 0.5 is deliberately high. A narrowband FM carrier at a low
    // modulation index also concentrates its power in a residual carrier,
    // and the Bessel function that governs it puts 3 percent there at an
    // index of 5, which is ordinary land mobile FM, so the threshold is
    // nowhere near it. What reaches 0.5 is a carrier that is not modulated
    // at all.
    double carrier_concentration = 0.5;

    // Normalised envelope power variance below which the waveform is
    // treated as constant envelope, which rules out every linear
    // modulation.
    //
    // 0.05 against a measured 0.0 for clean FSK and broadcast FM, and over
    // 0.05 for a root-raised-cosine BPSK signal. It is a HIGH-SNR test and
    // says so: noise is what a constant envelope is not, and circularly
    // symmetric complex Gaussian noise reads exactly 1.0 here, so a weak
    // signal in a noisy extract drifts toward the linear side of this line
    // whatever it is. Where that matters the fix is a narrower receiver
    // rather than a different threshold.
    double constant_envelope_variance = 0.05;

    // Share of the occupied bandwidth the instantaneous frequency has to
    // range over before a constant-envelope waveform with no separable
    // tones is called analogue FM rather than refused.
    double analogue_fm_spread_fraction = 0.10;

    // How much of the extract's own power the carrier the M-th power line
    // names has to sit on, as a share of the median power across the occupied
    // band, before the PSK branch is taken.
    //
    // THE RULE. A linear modulation's spectrum peaks at its carrier. Band
    // limited noise squared has no line in it and a CORNER where the band
    // stops, and estimate_modulation_order compares a peak against a local
    // baseline, which a corner defeats, so it names a carrier where the band's
    // power has already fallen away. docs/detection.md found that on real
    // 40 m; tests/characterise/test_empty_channel.cpp reproduces it on
    // synthetic noise narrowed the way the CLI narrows an extract: 62 of 96
    // empty channels called PSK, 22 of them WITH a symbol rate and at up to
    // 0.98 confidence, so the missing-rate flag alone would not have caught
    // them. Every one put its carrier 0.45 to 0.56 of the band's width from
    // the band's centre, which is the edge.
    //
    // WHY POWER AND NOT DISTANCE FROM THE CENTRE, which was tried first. The
    // occupied band of a noise-filled extract is not centred on anything: a
    // real BPSK signal at -7 kHz and 20 dB in 2500 Hz came back with its band
    // centred at +6.06 kHz, its carrier 0.34 of the width away, and a bar on
    // distance refused it. The power at the carrier does not move with where
    // the noise dragged the band.
    //
    // THE MARGINS, from the same file. The 62 order lines on empty channels
    // named carriers sitting on at most 0.195 of the band's median power.
    // Real BPSK and QPSK at 0, +3 and -7 kHz from 30 dB down to 0 dB in
    // 2500 Hz, wherever the order was found, sat on at least 0.88 of it, the
    // low end at 30 dB where the band is the signal's own flat top, and on
    // 2 to 110 times it once noise fills the extract. A half is a factor of
    // 2.6 inside each. With the rule, 0 of the 96 empty channels is named.
    //
    // The level is read at whichever of the carrier's M-fold aliases, rate/M
    // apart, stands highest, because the power law cannot tell those apart
    // and a real carrier reported at an alias is still a real carrier.
    double psk_carrier_level_fraction = 0.5;

    // Transform length for the averaged spectrum, or zero to let
    // analysis_segment pick one from the sample count.
    //
    // WHY A CALLER MIGHT WANT TO FIX IT. analysis_segment takes a sixteenth of
    // the extract, so a longer extract gets finer bins, and
    // spectral_concentration is the power in the strongest THREE of them.
    // That makes its window a FREQUENCY that shrinks as the extract grows, and
    // two runs over different lengths of the same signal are then not
    // comparable.
    //
    // Measured on a real 40 m carrier through a 3 kS/s channel on 2026-09-22,
    // with nothing changed but how much was handed over:
    //
    //   11 s    concentration 0.531, and carrier_concentration is 0.5, so the
    //           family came back Unmodulated
    //   20 s    0.467
    //   40 s    0.288
    //   58 s    0.184
    //
    // Three bins is 4.4 Hz at the first and 1.1 Hz at the last, and an HF
    // carrier drifts further than 1.1 Hz in a minute between transmitter
    // stability and propagation Doppler. The measure is right and its answer
    // is a function of an argument the caller did not know it was passing.
    //
    // spectral_concentration's own comment prefers a power fraction to a
    // frequency spread because "a power fraction is the same number at any
    // sample rate". That holds. It is not the same number at any extract
    // LENGTH, and on a drifting carrier that is the axis that bites.
    //
    // Zero keeps the old behaviour exactly, so nothing that does not set this
    // changes. Setting it is how a caller makes two extracts comparable, and
    // the number to set is the one whose three bins span what the signal is
    // allowed to drift.
    std::size_t segment = 0;
};

// Everything measured, the family decided from it, and what that is
// consistent with.
//
// Every sub-result is carried whole, including the ones that refused,
// because the refusals are how a reader checks the decision. A family of
// Fsk with an OfdmStructure that refused on its ceiling says something a
// bare "Fsk" does not.
struct Characterisation {
    ModulationFamily family = ModulationFamily::Unknown;
    double family_confidence = 0.0;

    // True when family is Psk and neither cyclic detector produced a symbol
    // rate. The call is kept, its confidence is held to
    // kPskWithoutRateConfidence, the summary says why, and
    // may_drive_detection() refuses it. See kPskWithoutRateConfidence for
    // the measurement.
    bool psk_without_symbol_rate = false;

    // The extract's power at the power law's carrier, at its highest M-fold
    // alias, over the median power across the occupied band. Negative when
    // no order or no band was found to measure it against.
    //
    // psk_carrier_outside_band is set when it fell under
    // CharacteriseConfig::psk_carrier_level_fraction, in which case the PSK
    // branch was not taken and the refusal says so.
    double psk_carrier_level = -1.0;
    bool psk_carrier_outside_band = false;

    // True when nothing at all was established: no family and no frame
    // period either. A frame period without a family is still a finding,
    // because a repeat is a repeat whatever is repeating, so it is not a
    // refusal.
    bool refused = false;

    // Why no family was established. Non-empty whenever family is
    // Unknown, including the case where a frame period WAS found, because
    // a reader looking at a frame period with no modulation beside it
    // wants to know what each estimator said.
    //
    // It quotes every estimator's own refusal rather than summarising
    // them, which is verbose on purpose: the numbers are how a caller
    // decides whether to take a longer extract, narrow the receiver or
    // accept that the channel is empty, and those are different actions.
    std::string refusal;

    // One line, stating what was found and what it is consistent with.
    // This is the sentence an operator reads.
    std::string summary;

    // The symbol rate the family's own detector produced. Left with
    // found == false for Unmodulated and AnalogueFm even when a detector
    // reported a cycle frequency, because on those two the cycle
    // frequencies are modulation tones and reporting one as a symbol rate
    // would be the confident wrong answer this stage exists to avoid.
    SymbolRateEstimate symbol_rate{};

    // Both detectors' output, whichever was used. A reader comparing them
    // is reading the evidence rather than the conclusion.
    SymbolRateEstimate squared_envelope{};
    SymbolRateEstimate frequency_transition{};

    ToneStructure tones{};
    ModulationOrder order{};
    OfdmStructure ofdm{};
    FramePeriod frame{};

    EnvelopeStats envelope{};
    double spectral_concentration = 0.0;
    OccupiedBand band{};

    std::vector<ProtocolCandidate> candidates;
};

[[nodiscard]] Expected<Characterisation> characterise(dsp::ConstComplexSpan samples,
                                                      const CharacteriseConfig& config);

// Whether this characterisation may be one of the inputs that decides what the
// detector does: a threshold, a hold, whether a track is kept.
//
// NECESSARY, NOT SUFFICIENT. It refuses an Unknown family and a flagged PSK
// call, which are the two cases with a stated reason to refuse, and it
// certifies nothing else. docs/detection.md measured that the family call on
// real HF cannot carry a detection decision on its own; a caller that routes a
// family into detection passes through here first and still owes its own
// measurement of what it is doing. Nothing in this tree routes one yet, and
// nothing goes on the wire as a family.
[[nodiscard]] bool may_drive_detection(const Characterisation& result);

}  // namespace revenant::characterise
