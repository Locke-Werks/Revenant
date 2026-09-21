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

}  // namespace revenant::characterise
