// What a detection is, in the words the bracket on the span prints.
//
// THE OWNER'S REQUEST OF 2026-09-23, which retired docs/detection.md's
// "nothing goes on the wire as a family": "Digital protocols should be
// detected on the waterfall and spectrum along with modulation. The pink block
// that brackets a signal should show what the signal is: modulation if analog,
// detected digi mode if digital, and it should set the receiver accordingly
// based on that information."
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
//      its sidebands mirror about it, and analogue FM is NFM or WFM by the
//      track's width. Digital ones give the family with its order or tone
//      count where the probe measured one.
//   3. Nothing. Unknown is a real answer, docs/detection.md says why, and a
//      track that was probed and named nothing gets no label rather than the
//      nearest one.
//
// WHAT IT CANNOT SAY
//
// USB and LSB. The characteriser has no single-sideband family, and relative
// to a carrier that is not transmitted the two differ only in which side
// their power sits, which docs/detection.md measured no field here can read.
// A sideband signal is labelled nothing, which is the rule above rather than
// a gap in it.
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

struct TrackLabel {
    LabelKind kind = LabelKind::Unknown;

    // "P25", "NFM", "BPSK" and the like; empty for Unknown.
    std::string_view name;

    // The protocol's own confidence for a protocol, the accepted family's for
    // the rest, zero for Unknown.
    double confidence = 0.0;

    // Whether a client may set a receiver from this label. True for a
    // verified protocol and for a family characterise::may_drive_detection
    // accepted, which are the only two ways a track gets a label at all, and
    // false for Unknown. Carried as its own field rather than read off the
    // kind so the rule can tighten without a client changing.
    bool may_drive = false;

    // The symbol rate the family's probe measured, zero where none was, for
    // the hover card. Not part of the name: a bracket a few pixels high has no
    // room for it.
    double symbol_rate_hz = 0.0;
};

[[nodiscard]] TrackLabel label_track(const Track& track);

}  // namespace revenant::detect
