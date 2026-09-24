// Speech-shaped audio for the analogue voice scenes: something a detector and
// a characteriser can be measured against that behaves like a person talking
// into a microphone rather than like a test tone or like steady noise.
//
// WHY NOT BAND-LIMITED NOISE, which is what tools/siggen/labelled.cpp used
// first. Steady noise has none of the three properties that tell voice from
// data: it never pauses, its level never moves at a syllable's rate, and it
// has no pitch. A classifier tuned on it learns nothing about speech, and an
// SSB signal made from it is a filled block that never goes quiet, which a
// real sideband transmission does every time the talker draws breath.
//
// WHAT THIS MAKES, and where each number comes from. Every figure is either a
// published one or this file's choice, and the choices are marked as such.
//
//   Phrases and pauses. Talk for 1.2 to 3.0 s, then silence for 0.25 to
//   0.9 s, both uniform. THIS FILE'S CHOICE, of the order Brady measured for
//   talkspurts and the gaps inside them in two-way telephone conversation
//   (P. T. Brady, "A statistical analysis of on-off patterns in 16
//   conversations", Bell System Technical Journal 47(1), 1968).
//
//   Syllables. 150 to 300 ms each, uniform, so about four a second, each a
//   raised-cosine swell to a level drawn from 0.5 to 1.0 of full scale. Four
//   a second is where the envelope modulation spectrum of running speech
//   peaks (T. Houtgast and H. J. M. Steeneken, "A review of the MTF concept in
//   room acoustics and its use for estimating speech intelligibility in
//   auditoria", JASA 77(3), 1985, which puts the peak near 3 to 4 Hz).
//
//   Voicing. Four syllables in five are voiced: a glottal pulse train at a
//   fundamental of 100 to 140 Hz, THIS FILE'S CHOICE inside the adult male
//   range, falling by a fifth across each phrase, which is the declination
//   intonation studies describe, with 1 percent cycle-to-cycle jitter. The
//   pulses go through a one-pole low pass, which gives the source the roughly
//   -12 dB per octave glottal slope less the +6 dB per octave of radiation at
//   the lips that G. Fant's source-filter model combines (Acoustic Theory of
//   Speech Production, 1960). The fifth syllable is unvoiced: white noise.
//
//   Vowels. Each syllable is shaped by the first three formants of one of ten
//   American English vowels, the male averages of G. E. Peterson and H. L.
//   Barney, "Control methods used in a study of the vowels", JASA 24(2),
//   1952, through a cascade of the second-order digital resonators D. H.
//   Klatt gives in "Software for a cascade/parallel formant synthesizer",
//   JASA 67(3), 1980, at bandwidths of 60, 90 and 120 Hz, THIS FILE'S CHOICE
//   inside the range Klatt's defaults span. An unvoiced syllable goes through
//   one resonance at 2500 Hz, 1000 Hz wide.
//
//   The channel. Band-limited to 300 to 3000 Hz, the voice channel land
//   mobile and amateur transmitters pass (ITU-T G.712 states 300 to 3400 Hz
//   for telephony; 3000 is the figure the scenes here already used).
//
//   Level. Scaled so the RMS over the talking part, pauses excluded, is 0.3
//   of full scale, and clipped at one, which is where a transmitter's
//   limiter sits. A modulator reads one as full scale: an AM index at one or
//   an FM deviation at its peak.
//
// PURITY
//
// A function of its arguments. Randomness comes from the caller's seed
// through std::mt19937_64, drawn from the engine's words directly rather
// than through a standard distribution, for the reason core/dsp/synth/
// modulators.h gives: two standard libraries may consume the engine
// differently.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/dsp/types.h"

namespace revenant::siggen_speech {

struct SpeechSpec {
    dsp::SampleRate rate = 48'000;
    std::size_t samples = 0;
    std::uint64_t seed = 1;

    // RMS over the talking part, pauses excluded, before the clip at one.
    double talking_rms = 0.3;
};

struct Speech {
    std::vector<float> audio;

    // One flag per sample: true inside a phrase, false in a pause. A caller
    // that wants the level during speech rather than across the pauses reads
    // it off this.
    std::vector<std::uint8_t> talking;

    // The share of samples inside a phrase.
    double talking_fraction = 0.0;
};

[[nodiscard]] Speech synthesise_speech(const SpeechSpec& spec);

}  // namespace revenant::siggen_speech
