// Whether a wideband FM station carries hybrid digital sidebands beside its
// analogue signal, and how strong they are against it.
//
// THE SCOPE, FROM docs/modes.md. HD Radio decoding is excluded on a live
// patent and an unpublished codec. Identifying it is in scope, and this is
// identification only: it reads the averaged power spectrum the detector
// already integrates and nothing else. No sample of the signal is
// demodulated, no OFDM symbol is aligned, and nothing here knows a subcarrier
// spacing, a pilot or a frame. Written from measurement and the public band
// occupancy alone, which is that a hybrid station puts a flat block of energy
// on each side of its analogue signal, from about 129 to 198 kHz off centre,
// well below the analogue level (issue #6).
//
// THE TEST, ALL OF IT ON ONE FRAME OF AVERAGED POWER
//
//   1. Both sideband blocks stand clear of the noise floor: their mean
//      density is kHdMinimumOverFloorDb above the floor's under them.
//   2. Each block stands above the gap between it and the analogue signal
//      (105 to 120 kHz off centre) by kHdMinimumOverGapDb. An analogue FM
//      spectrum falls monotonically from the centre, so its own skirt is
//      always stronger in the gap than further out; a plateau beyond a dip is
//      something added, not the analogue signal spreading.
//   3. Each block is flat: its inner half and outer half within
//      kHdMaximumTiltDb of each other. A skirt tilts, a block does not.
//   4. The two blocks are within kHdMaximumAsymmetryDb of each other in
//      power. A neighbouring station 200 kHz away raises one side only.
//
// MEASURED 2026-11-20 ON AN RTL-SDR AT 2.4 MS/s across the local FM band,
// eight stations (tests/detect/test_hd_hybrid.cpp names the recordings). The
// three with plainly visible flat blocks either side (90.5, 99.9 and 102.7
// MHz) read their blocks 7 to 13 dB over the
// floor, 2.8 to 7 dB over their own gap, within 0.6 dB from inner half to
// outer and within 2.3 dB side to side, at -17 to -21 dBc each. A strong
// analogue-only station at 21.7 dB over the floor had nothing above the floor
// in either block. The thresholds sit between those with room on both sides;
// they were not fitted to the hybrids' own numbers.
//
// WHAT dBc MEANS HERE. Each block's excess power over the floor, summed over
// the block, against the analogue signal's excess summed over +/-100 kHz.
// That is the total power in each sideband relative to the analogue signal,
// which is how the band occupancy is stated, rather than a density ratio.
//
// PURITY. A function of its arguments.

#pragma once

#include <span>

#include "core/dsp/types.h"

namespace revenant::detect {

// The edges of each digital block, hertz from the station's centre.
inline constexpr double kHdInnerHz = 129'000.0;
inline constexpr double kHdOuterHz = 198'000.0;

// The analogue signal the blocks are compared with: +/-100 kHz.
inline constexpr double kHdAnalogueHalfHz = 100'000.0;

// The dip between the analogue signal and each block.
inline constexpr double kHdGapInnerHz = 105'000.0;
inline constexpr double kHdGapOuterHz = 120'000.0;

inline constexpr double kHdMinimumOverFloorDb = 3.0;
inline constexpr double kHdMinimumOverGapDb = 1.5;
inline constexpr double kHdMaximumTiltDb = 3.0;
inline constexpr double kHdMaximumAsymmetryDb = 6.0;

// A track narrower than this is not broadcast FM and is not measured. The
// same 50 kHz label.h separates NFM from WFM by, restated because label.h
// includes the detector and the detector includes this.
inline constexpr dsp::Hertz kHdMinimumTrackBandwidthHz = 50'000;

// Consecutive passing decisions before a track is called hybrid, and
// consecutive failing ones before it stops being. One decision of averaged
// power can be caught by a fade on one side.
inline constexpr unsigned kHdConfirmDecisions = 2;
inline constexpr unsigned kHdReleaseDecisions = 3;

// The name a wideband FM label takes when the track is hybrid.
inline constexpr const char* kHdHybridName = "FM + HD hybrid";

struct HdSidebandReading {
    // False when any band the test reads falls outside the frame, or the
    // analogue signal has no excess at all. Nothing else here means anything
    // unless this is true.
    bool measured = false;

    // All four rules above held.
    bool hybrid = false;

    // The centre the blocks were placed about, baseband hertz: the given
    // centre moved to the analogue signal's power centroid.
    double centre_hz = 0.0;

    // Each block's power against the analogue signal's, dB.
    double lower_dbc = 0.0;
    double upper_dbc = 0.0;

    // Each block's mean density over the floor's, and over its own gap's, dB.
    double lower_over_floor_db = 0.0;
    double upper_over_floor_db = 0.0;
    double lower_over_gap_db = 0.0;
    double upper_over_gap_db = 0.0;

    // Inner half over outer half of each block, dB.
    double lower_tilt_db = 0.0;
    double upper_tilt_db = 0.0;
};

// power and floor are linear, one entry per fine bin, as Detector::
// averaged_power and Detector::noise_floor give them. bin_zero_hz is the
// baseband frequency of bin zero and centre_hz the station's baseband centre
// as the tracker has it.
[[nodiscard]] HdSidebandReading measure_hd_sidebands(std::span<const double> power,
                                                     std::span<const double> floor,
                                                     double bin_zero_hz,
                                                     double bin_width_hz,
                                                     double centre_hz);

}  // namespace revenant::detect
