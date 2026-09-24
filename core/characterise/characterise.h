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
// A PURE LIBRARY, WHICH THE ENGINE CALLS AND WHICH CALLS NOTHING BACK
//
// This is a library over a span of complex samples with a stated rate. The
// engine's probe pool, core/engine/probe.h, is where an extract arrives from a
// receiver: Demod::Raw built through the fine stage, so the extract is mixed
// to DC and filtered near the signal rather than being a raw coarse channel
// 112 times too wide. The pool calls characterise() on its own worker thread
// and nothing here knows it exists.
//
// Keeping the stage pure means every estimator in it is testable against
// core/dsp/synth with no device, no graph and no scheduler, which is what the
// cases in tests/characterise do, and the wiring was cheaper for the seam
// being a span rather than a stage.
//
// WHAT THIS BLOCK USED TO SAY, under the heading "NOT WIRED INTO THE ENGINE,
// DELIBERATELY": "In the engine an extract would arrive through a receiver
// and Engine::attach_audio_sink" and "None of that is here." The extract does
// not arrive through attach_audio_sink, because a probe is not a receiver any
// client can see, and it is here now.
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
#include <cstdint>
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
    // nowhere near it at that index. Voice is not at that index: its RMS
    // deviation is a fraction of its peak, and a carrier modulated by it keeps
    // most of its power, so it does reach 0.5. The branch then reads the
    // sidebands and the envelope, which is am_sideband_share below, and names
    // AM, low-index FM or a bare carrier.
    //
    // WHAT THE LAST SENTENCE USED TO SAY: "What reaches 0.5 is a carrier that
    // is not modulated at all." Voice-shaped NFM at 2.5 kHz peak deviation in
    // tools/siggen's labelled scene reached 0.58 to 0.60.
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

    // The occupied bandwidth the DETECTOR measured for the signal this extract
    // was taken to look at, in hertz, or zero when there is no detection
    // behind the extract. core/engine/probe.cpp passes ProbeRequest::
    // occupied_hz; a caller characterising a file by hand passes nothing.
    //
    // THE RULE IT FEEDS. A family whose symbol rate is wider than that
    // bandwidth is refused. A linear modulation occupies its symbol rate times
    // one plus its rolloff, and an FSK signal at least its symbol rate plus
    // its shift, so a signal cannot be keyed faster than it is wide. What the
    // characteriser reads at a rate like that is something else: a modulating
    // tone, or the gap between two lines.
    //
    // docs/detection.md measured the case on 2026-09-23. The detector reports
    // an NFM emitter as its Bessel comb, one track per line 146 to 183 Hz
    // wide, a probe sized to one line sees a fragment of an 11 kHz signal, and
    // it came back PSK or FSK at 1000 and 2000 baud, the modulating tone and
    // its double: 57 wrong calls of 57 on NFM tracks and 33 of 33 on the
    // two-tone SSB lines. This rule refuses every one of them, because every
    // one names a rate five to fourteen times its track's width.
    //
    // The detector's reported width is its 99 percent occupied-power span,
    // which on a raised-cosine signal is 0.87 of the nominal occupied band
    // (docs/detection.md, "What the wide end costs"). At rolloff 0.35 that is
    // still 1.17 times the symbol rate, so a real PSK track clears the rule
    // by 17 percent and no tolerance is added to it.
    double detection_bandwidth_hz = 0.0;

    // THE TONE-PAIR CHECK, in the PSK branch.
    //
    // Two tones of comparable level square to a line at their difference,
    // which the squared-envelope detector reads as a symbol clock, and they
    // light the M-th power law at exponent two. So two carriers pass every
    // test the PSK branch makes. Measured on 2026-09-23 through the probe
    // pool: two-tone SSB at 700 and 1900 Hz came back PSK of order 2 at
    // 1200.1 baud and confidence 1.00, at every level from 5 to 30 dB.
    //
    // What separates them is the spectrum. A PSK signal is filled: its three
    // strongest adjacent bins hold 0.06 of its power. Two tones are two lines,
    // each holding about half. So the branch is refused when the two strongest
    // three-bin windows, taken at least four bins apart, hold at least this
    // share of the band's excess power over its floor, the third strongest
    // holds under tone_pair_third_fraction of the weaker of the two, and the
    // two sit the claimed symbol rate apart, to within two bins or
    // tone_pair_rate_tolerance of the rate, whichever is wider.
    //
    // A half, because real BPSK and QPSK at 1200 baud read 0.065 to 0.075 for
    // the same two windows and two-tone SSB 0.82 at 5 dB in 2500 Hz rising to
    // 0.997 at 30 dB, with the third line under 0.005 of the weaker tone;
    // tests/characterise/test_consistency.cpp measures both sides at the
    // probe pool's 12000 S/s floor bucket.
    double tone_pair_fraction = 0.5;
    double tone_pair_third_fraction = 0.25;
    double tone_pair_rate_tolerance = 0.02;

    // THE SPECTRAL SIDEBAND READING on an Unmodulated call, measured and
    // reported in Characterisation::sideband_share and sideband_symmetry, and
    // no longer what decides AM: carrier_in_phase_balance below does, and
    // says why this one could not.
    //
    // WHAT THIS PARAGRAPH USED TO SAY at its head: "THE DOUBLE-SIDEBAND
    // READING on an Unmodulated call, which is what lets a label say AM rather
    // than CW", and what follows is how the reading was taken while it did.
    //
    // A carrier with sidebands mirrored about it is double sideband. So the
    // band's excess power more than am_sideband_min_offset_hz from the carrier
    // is measured on each side, bin against mirrored bin, and the reading is
    // taken when that power is at least am_sideband_share of the band's
    // excess and the two sides agree to am_sideband_symmetry, the sum of the
    // smaller of each mirrored pair over the sum of the larger.
    //
    // The offset keeps a keyed carrier out: keying at 25 WPM with 5 ms edges
    // puts its sidebands inside about 100 Hz of the carrier, and speech on an
    // AM carrier starts at 300 Hz. tests/characterise/test_consistency.cpp
    // measures AM, a keyed carrier and a bare one at 30, 20 and 10 dB in
    // 2500 Hz, at 12000 S/s: AM at index 0.8 on a full-scale tone puts 0.24 to
    // 0.28 of its excess in sidebands mirroring to 0.81 to 0.998, and on
    // speech-shaped audio at an RMS of 0.3 only 0.054 to 0.107, mirroring to
    // 0.53 to 0.98; the keyed and bare carriers put nothing there at 20 and
    // 30 dB and 0.062 at most at 10, mirroring to 0.34 at most. So the share
    // bar is low, 0.02, and the symmetry bar does the separating at low SNR,
    // where noise lifts both populations' share alike.
    //
    // WHAT THE SHARE BAR USED TO BE: 0.05, measured only against a full-scale
    // tone. Speech-shaped AM sat on it at 0.054, and the AM emitter in
    // tools/siggen's labelled scene fell under it and was labelled CW.
    double am_sideband_min_offset_hz = 150.0;
    double am_sideband_share = 0.02;
    double am_sideband_symmetry = 0.5;

    // THE CARRIER'S SIDEBANDS AGAINST ITS OWN PHASE, which is what decides AM,
    // FM at a low index and a bare or keyed carrier on an Unmodulated call.
    //
    // AM's sidebands are in phase with its carrier and narrowband FM's are in
    // quadrature with it: an AM wave is C(1 + m a(t)) and a small phase
    // deviation is C(1 + j b(t)), the phasor argument every communications
    // text makes for narrowband FM against AM (A. B. Carlson, Communication
    // Systems, the narrowband FM section). So the extract is mixed to the
    // carrier, its residual phase followed in 20 ms blocks, and the power in
    // the voice channel, 300 to 3000 Hz from the carrier, split between the
    // in-phase and the quadrature parts. Noise that is circularly symmetric
    // puts the same power in both, so the difference is the modulation's
    // whatever the SNR, which the spectral reading this replaced was not.
    //
    // A carrier is AM when (in - quad) / (in + quad) reaches
    // carrier_in_phase_balance and (in - quad) is at least
    // carrier_sideband_excess of the carrier's own power; FM at a low index
    // when the same two hold the other way round. MEASURED in
    // tests/characterise/test_voice.cpp at 12000 and 24000 S/s, 30 to 10 dB
    // in 2500 Hz, two draws each: AM on speech reads a balance of +0.12 to
    // +0.95 and an excess of +0.031 to +0.046; FM on speech at 2.5 and 5 kHz
    // deviation -0.09 to -0.40 and -0.18 to -0.36; a keyed carrier -0.029 to
    // +0.017 and -0.012 to +0.006, a bare one -0.022 to -0.005 and under
    // 0.004, PSK31 -0.015 to -0.003. Both bars sit at least two and a half
    // times inside every population. docs/detection.md, "Voice".
    //
    // WHAT DECIDED THIS BEFORE: the band's excess power beyond
    // am_sideband_min_offset_hz, mirrored to am_sideband_symmetry, with the
    // envelope's variance net of the noise choosing between AM and FM. The
    // excess was taken over the 20th percentile of the whole extract, which
    // on a probe's extract lands in the filter's skirt, so at 10 dB in 2500 Hz
    // a keyed carrier's own noise read 0.17 of the band as mirrored sidebands
    // and was called AM. The spectral numbers are still measured and
    // reported, in sideband_share and sideband_symmetry.
    double carrier_in_phase_balance = 0.05;
    double carrier_sideband_excess = 0.01;

    // The least spectral_concentration at which a carrier whose sidebands
    // read in phase or in quadrature is taken as that carrier, under the
    // carrier_concentration a bare one needs. A wide extract of AM at a
    // modest SNR puts enough noise beside the carrier to pull it under the
    // half: measured at 48000 S/s and 10 dB in 2500 Hz, AM on speech read
    // 0.49 and went to the PSK branch at 440 to 480 baud, with its sidebands
    // reading 0.12 to 0.16 in phase. BPSK, 2FSK and noise read under 0.05 in
    // tests/characterise/test_voice.cpp. Speech on single sideband reads up
    // to 0.38, over this bar, and is kept out by the voice rule below rather
    // than by it: a talker is never taken as a carrier with sidebands. The
    // quadrature reading is the discriminator here, not this bar.
    double carrier_sideband_concentration = 0.3;

    // THE VOICE BRANCHES, which read a talker off an extract the other
    // branches would give a family it does not have.
    //
    // Speech moves its level at a syllable's rate: the envelope modulation
    // spectrum of running speech peaks near 4 Hz (T. Houtgast and H. J. M.
    // Steeneken, JASA 77(3), 1985). On a suppressed carrier that movement is
    // the signal's own power, and it pauses between phrases. On FM the power
    // stays put and it is the deviation that comes and goes.
    //
    // Single sideband: an envelope that is not constant, a detection at least
    // voice_min_width_hz wide, and syllabic_depth at least voice_syllabic_depth
    // and at least voice_syllabic_ratio times syllabic_fast. The ratio keeps
    // out noise and data, whose envelope moves as much or more above 10 Hz as
    // below it; the width keeps out the narrow modes whose envelope moves as a
    // talker's does, a keyed carrier and PSK31's phase reversals, which the
    // detector measures at 183 Hz and under on the 36.6 Hz grid. A talker's
    // detection is the voice channel or a stretch of it: 324 Hz was the
    // narrowest stretch of single sideband speech the voice survey saw
    // probed, and a probe centred on a stretch still collects the whole
    // channel. A detection at least voice_carrier_width_hz wide is taken as a
    // talker even when a voiced sound's harmonics lift its extract over
    // carrier_concentration, and a talker is never taken as a carrier with
    // sidebands under it: AM's carrier holds its power through the pauses,
    // and its syllabic_depth read 0.09 at most in the same file against the
    // 0.4 bar, while speech on single sideband once read 0.32 concentrated and
    // in phase with a harmonic, and was called AM. It refuses the family, since none here is
    // single sideband, and says which side from side_centroid: speech holds
    // most of its power low in the voice channel (the long-term average
    // speech spectrum of D. Byrne et al., JASA 96(4), 1994, falls above 500
    // Hz), which on an upper sideband is the band's low edge, next to the
    // suppressed carrier.
    //
    // FM: an envelope constant against the noise inside the passband and a
    // deviation that moves by fm_voice_frequency_syllabic of its mean between
    // 2 and 10 Hz. That is AnalogueFm with the voice flag.
    //
    // MEASURED in tests/characterise/test_voice.cpp either side of every bar,
    // at 12000 and 24000 S/s from 30 to 5 dB in 2500 Hz: speech on single
    // sideband reads a syllabic_depth of 0.86 to 1.66 at a ratio to
    // syllabic_fast of 3.5 to 7.5, and a side_centroid of 0.57 to 0.68 on its
    // own side; noise 0.20 to 0.32 at 0.42 to 0.48, PSK31 0.20 to 0.22 at 0.62
    // to 0.65, BPSK and 2FSK under 0.09, AM on speech under 0.10; a keyed
    // carrier 0.66 to 0.81 at 2.7 to 3.3, which the width and the carrier
    // branch keep out. FM on speech at 5 kHz deviation reads a
    // frequency_syllabic_depth of 0.215 to 0.81 from 30 to 10 dB, so its
    // weakest draws fall under the bar and are named by the quadrature reading
    // or not at all; BPSK, 2FSK, noise, a bare carrier and AM read 0.11 at
    // most. docs/detection.md, "Voice", has the table.
    double voice_syllabic_depth = 0.4;
    double voice_syllabic_ratio = 2.0;
    double voice_min_width_hz = 250.0;
    double voice_carrier_width_hz = 1'000.0;
    double voice_side_centroid = 0.1;
    double fm_voice_frequency_syllabic = 0.25;

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

// Which side of its suppressed carrier a talker on single sideband sits.
enum class VoiceSideband : std::uint8_t {
    Unknown = 0,
    Upper,
    Lower,
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

    // The tone-pair measurement, CharacteriseConfig::tone_pair_fraction's
    // rule: the share of the band's excess power in its two strongest
    // three-bin windows, the third strongest over the weaker of those two, and
    // how far apart the two sit. Negative when the spectrum had no excess to
    // share out. psk_tone_pair is set when the rule refused the PSK branch.
    double tone_pair_share = -1.0;
    double tone_pair_third = -1.0;
    double tone_pair_spacing_hz = 0.0;
    bool psk_tone_pair = false;

    // Set when CharacteriseConfig::detection_bandwidth_hz was given and the
    // family's own symbol rate was wider than it. The family is then refused,
    // Unknown with the reason in the refusal, and symbol_rate keeps the rate
    // that was measured so a reader can see what was refused.
    bool symbol_rate_exceeds_detection = false;

    // CharacteriseConfig::am_sideband_share's reading, on an Unmodulated call
    // only: the band's excess power away from the carrier as a share of all
    // of it and how well the two sides mirror each other. Negative shares mean
    // it was not measured. Reported; nothing decides on it.
    double sideband_share = -1.0;
    double sideband_symmetry = -1.0;

    // On an Unmodulated call, the carrier's voice-channel sidebands sit in
    // phase with it: AM. CharacteriseConfig::carrier_in_phase_balance.
    //
    // WHAT THIS USED TO MEAN: the spectral sidebands above cleared their two
    // bars and the envelope was not flat net of noise.
    bool double_sideband = false;

    // On an Unmodulated call, the same sidebands sit in quadrature with it:
    // FM at a low index, since AM's sidebands are its envelope. The family is
    // then AnalogueFm at a half, not Unmodulated: voice on narrowband FM keeps
    // most of its power in its carrier and reaches the carrier bar.
    //
    // WHAT THIS USED TO MEAN: the spectral sidebands cleared their bars on an
    // envelope whose variance net of noise was under
    // constant_envelope_variance. tests/characterise/test_consistency.cpp
    // measured that at 0.001 to 0.031 for three tones on FM at an index under
    // a half and 0.26 to 0.30 for the same audio on AM, and still holds both
    // cases under the reading that replaced it.
    bool low_index_fm = false;

    // The envelope's normalised power variance less what the extract's own
    // noise accounts for, on an Unmodulated call only; see characterise.cpp.
    // Zero elsewhere.
    double envelope_variance_net = 0.0;

    // The noise power inside the extract's passband, as a total. The 20th
    // percentile of the bins within a quarter of the rate of the centre,
    // which is the half of the bucket a probe passes, taken as the per-bin
    // level, and each bin's own level where it is lower, which is the filter
    // skirt. OccupiedBand::noise_floor is the 20th percentile of the whole
    // extract, and on a probe's extract that lands in the skirt, under the
    // noise the signal actually sits in.
    double inband_noise = 0.0;

    // How much the extract's power moves at a syllable's rate: the RMS of the
    // 10 ms frame powers band-passed to 2 to 10 Hz, over the signal's mean
    // power net of inband_noise. syllabic_fast is the same from 10 to 40 Hz.
    // Negative when the extract is under 64 frames or holds no signal power.
    double syllabic_depth = -1.0;
    double syllabic_fast = -1.0;

    // The same band-pass over the instantaneous frequency's RMS deviation in
    // each 10 ms frame, over its mean: how much the deviation comes and goes
    // at a syllable's rate. Negative when too few samples cleared the
    // amplitude gate.
    double frequency_syllabic_depth = -1.0;

    // The centroid of the excess power over inband_noise, within half the
    // detection's width of the extract's centre, as a fraction of that half
    // width: negative below the centre, positive above.
    double side_centroid = 0.0;

    // The carrier's sidebands in the voice channel, 300 to 3000 Hz from it,
    // split into the part in phase with the carrier and the part in
    // quadrature: (in - quad) / (in + quad), and (in - quad) over the
    // carrier's own power. Zero when not measured.
    double carrier_iq_balance = 0.0;
    double carrier_in_phase_excess = 0.0;

    // The envelope's normalised power variance less what inband_noise
    // accounts for, which is what the voice branches judge a constant
    // envelope by.
    double envelope_variance_inband = 0.0;

    // A talker, from CharacteriseConfig::voice_syllabic_depth's rules: true
    // on the FM voice branch, whose family is AnalogueFm, and on the single
    // sideband one, whose family is Unknown because none here is single
    // sideband. voice_sideband is the side on the second and Unknown on the
    // first.
    bool voice = false;
    VoiceSideband voice_sideband = VoiceSideband::Unknown;

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
// certifies nothing else. The two rules of 2026-09-23, a symbol rate wider
// than the detection and a PSK call that is two tones, refuse inside
// characterise() and so arrive here as Unknown. docs/detection.md measured
// that the family call on real HF cannot carry a detection decision on its
// own; a caller that routes a family into detection passes through here first
// and still owes its own measurement of what it is doing.
//
// core/engine/probe.h asks it of every probe, and detect::Detector::
// record_probe lets a family change Track::classification only when it says
// yes. Nothing the detector decides reads that field. What reaches the wire is
// core/detect/label.h's label, which gives a family only when this said yes,
// by the owner's decision of 2026-09-23.
//
// WHAT THIS PARAGRAPH USED TO SAY AT ITS END, twice over. First: "Nothing in
// this tree routes one yet, and nothing goes on the wire as a family." Then:
// "Nothing the detector decides reads that field, and nothing goes on the
// wire as a family."
[[nodiscard]] bool may_drive_detection(const Characterisation& result);

}  // namespace revenant::characterise
