// What a detection is, in the words the bracket on the span prints.
//
// THE OWNER'S REQUEST OF 2026-09-23. docs/detection.md used to say "nothing
// goes on the wire as a family", and this replaced it: "Digital protocols
// should be detected on the waterfall and spectrum along with modulation. The
// pink block that brackets a signal should show what the signal is:
// modulation if analog, detected digi mode if digital, and it should set the
// receiver accordingly based on that information."
//
// So a track gets a label with a kind, a name, a confidence and whether it may
// drive a receiver, and this is the one function that decides it, from fields
// the detector already carries. It measures nothing. Every input is an answer
// something else was held to account for: the family characterise named and
// may_drive_detection accepted, and the protocol core/identify verified.
//
// THE ORDER, AND WHY EACH STEP IS WHERE IT IS
//
//   1. A verified protocol. It stands on a sync its decoder checked, which no
//      modulation reading can overturn, so it wins over the family whatever
//      the family said, including nothing.
//   2. A family the characteriser named and the detector accepted. Analogue
//      families give a modulation: an unmodulated carrier is CW, or AM when
//      its sidebands in the voice channel sit in phase with it
//      (characterise::CharacteriseConfig::carrier_in_phase_balance), and
//      analogue FM is NFM or WFM by the track's width. Digital ones give the
//      family with its order or tone count where the probe measured one.
//
//      WHAT THE AM CLAUSE USED TO SAY: "or AM when its sidebands mirror about
//      it". A keyed carrier's own noise mirrors too, and read as AM at 10 dB.
//   3. A talker on a suppressed carrier, with no family: USB or LSB by the
//      side the characteriser read it on (Track::voice_sideband, from
//      characterise::Characterisation::voice_sideband through
//      engine::ProbeOutcome). The characteriser has no single-sideband
//      family and refuses one on such a talker, so this cannot come from
//      rule 2, and it comes after rule 2 so an accepted family wins.
//   4. Nothing. Unknown is a real answer, docs/detection.md says why, and a
//      track that was probed and named nothing gets no label rather than the
//      nearest one.
//
// WHAT "WHAT IT CANNOT SAY" USED TO SAY: "USB and LSB, yet. ... but
// engine::ProbeOutcome does not carry that reading to a track, so a sideband
// signal is still labelled nothing." It carries it now, and rule 3 is it.
// Before that it said, after its first sentence: "relative to a carrier that
// is not transmitted the two differ only in which side their power sits,
// which docs/detection.md measured no field here can read."
//
// PURITY
//
// A function of one track. The names are static strings, so a label is valid
// for as long as the program runs.

#pragma once

#include <cstdint>
#include <string_view>

#include "core/detect/detector.h"
#include "core/dsp/types.h"

namespace revenant::detect {

enum class LabelKind : std::uint8_t {
    Unknown = 0,
    AnalogModulation,
    DigitalFamily,
    Protocol,
};

[[nodiscard]] const char* label_kind_name(LabelKind kind);

// Below this an analogue FM track is NFM and at or above it WFM. Broadcast FM
// occupies about 200 kHz and the widest land mobile channel 25 kHz, so any
// value between them separates the two; 50 kHz sits clear of both, and above
// the 16 kHz a 5 kHz-deviation NFM channel occupies by Carson's rule.
inline constexpr dsp::Hertz kWfmMinimumBandwidthHz = 50'000;

// The confidence a USB or LSB label carries. Held to a half, as the
// characteriser holds its analogue FM calls: the side is read from where a
// talker's power sits against the extract's centre, which is an elimination
// of the digital families rather than a positive finding of a sideband
// modulator, and nothing calibrated puts a number on it.
inline constexpr double kVoiceSidebandConfidence = 0.5;

struct TrackLabel {
    LabelKind kind = LabelKind::Unknown;

    // "P25", "NFM", "BPSK" and the like; empty for Unknown.
    std::string_view name;

    // The protocol's own confidence for a protocol, the accepted family's for
    // the rest, kVoiceSidebandConfidence for USB and LSB, zero for Unknown.
    double confidence = 0.0;

    // Whether a client may set a receiver from this label. True for a
    // verified protocol, for a family characterise::may_drive_detection
    // accepted and for a talker's side, which are the only three ways a track
    // gets a label at all, and false for Unknown. WHAT THIS USED TO SAY: "which
    // are the only two ways a track gets a label at all". Carried as its own
    // field rather than read off the kind so the rule can tighten without a
    // client changing.
    bool may_drive = false;

    // The symbol rate the family's probe measured, zero where none was, for
    // the hover card. Not part of the name: a bracket a few pixels high has no
    // room for it.
    double symbol_rate_hz = 0.0;
};

[[nodiscard]] TrackLabel label_track(const Track& track);

}  // namespace revenant::detect
