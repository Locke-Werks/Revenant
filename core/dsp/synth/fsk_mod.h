// Transmitters for the two-level FSK text and data modes, written from the
// same clauses as the decoders in core/decode.
//
// WHY THESE EXIST
//
// The reason core/dsp/synth/dv_mod.h gives, unchanged: none of these
// decoders has a GPU kernel behind it and so none has a bit-exact twin, and a
// round trip through a transmitter written from the same clauses is what
// stands in its place. A round trip that fails means one end misread the
// document.
//
// WHAT A ROUND TRIP DOES AND DOES NOT PROVE
//
// That the two ends agree, and not that either agrees with a real
// transmitter. Where a document leaves something to practice, the practice is
// chosen once, in the decoder header that cites it, and used from there by
// both ends, so the round trip is blind to it by construction. The tests
// that check the decoders against the documents' own printed examples are
// the ones that can see a misreading shared by both ends.
//
// WHAT THESE PRODUCE
//
// Real audio, as a receiver would hand it to attach_audio_sink, because that
// is what the decoders read. Tones are continuous-phase, since every one of
// these modes is keyed by shifting an oscillator rather than switching
// between two.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/decode/ax25.h"
#include "core/decode/rtty.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::Hertz;
using dsp::SampleRate;

// ---------------------------------------------------------------------------
// RTTY
// ---------------------------------------------------------------------------

struct RttyModConfig {
    SampleRate rate = 48000;

    // Defaults from decode::RttyConfig, which cites them as practice.
    double baud = 45.45;
    Hertz mark_hz = 2125;
    Hertz shift_hz = 170;
    bool space_above_mark = true;

    // ITU-T S.3 Table 1: 1.5 units at 50 and 75 baud.
    double stop_units = decode::kRttyStopUnits;

    // Idle mark before the first character and after the last, in units, so
    // the receiver's filter has settled before the first start element and a
    // capture does not end inside the last stop element.
    double lead_units = 8.0;
    double tail_units = 8.0;

    double amplitude = 0.5;

    // Added to both tones, to measure what a mistuned receiver loses. Not an
    // integer, because the mistuning of a real receiver is not.
    double tone_offset_hz = 0.0;
};

// Text to ITA2 combinations, beginning with a letter shift as S.1 clause 4.4
// requires of the first combination sent, and with a shift inserted wherever
// the case has to change. Characters ITA2 cannot carry are an error rather
// than being dropped, so a test cannot quietly send less than it thinks.
[[nodiscard]] Expected<std::vector<std::uint8_t>> ita2_encode_text(std::u32string_view text);

// Renders combinations as start-stop FSK audio: per S.1 clause 3.2 and S.3
// Table 1, a start unit at condition A, five code units with element 1
// first, and a stop element of stop_units at condition Z.
[[nodiscard]] Expected<std::vector<float>> rtty_render(const RttyModConfig& config,
                                                       std::span<const std::uint8_t> combinations);

// ---------------------------------------------------------------------------
// AX.25 over Bell 202 AFSK
// ---------------------------------------------------------------------------

struct Ax25ModConfig {
    SampleRate rate = 48000;
    Hertz mark_hz = decode::kBell202MarkHz;
    Hertz space_hz = decode::kBell202SpaceHz;
    double baud = decode::kBell202Baud;

    // Flags before the first frame, between frames and after the last.
    // Finnegan and Benson section 3 find no document fixing any of the
    // three and report a commercial TNC sending 3, 7 and 5; the lead here
    // is longer because it stands in for the transmitter delay a receiver
    // spends settling, which the paper puts at hundreds of milliseconds.
    std::size_t leading_flags = 32;
    std::size_t flags_between = 4;
    std::size_t trailing_flags = 4;

    double amplitude = 0.5;

    // Level of the space tone relative to mark, in dB. Finnegan and Benson
    // section 4 describe pre-emphasis and de-emphasis tilting the two tones
    // by up to 10 dB either way on real stations; this reproduces it.
    double space_gain_db = 0.0;

    // Fractional error of the transmitter's bit clock, so the receiver's
    // clock loop has something to track.
    double baud_error = 0.0;
};

// Address through information for one frame, per AX.25 2.2 clause 3 and
// Figure 3.1: destination, source, up to eight repeaters, control, the PID
// where clause 3.4 puts one, and the information field.
struct Ax25FrameSpec {
    decode::Ax25Address destination;
    decode::Ax25Address source;
    std::vector<decode::Ax25Address> repeaters;
    std::uint8_t control = decode::kAx25ControlUi;
    std::optional<std::uint8_t> pid = decode::kAx25PidNoLayer3;
    std::vector<std::uint8_t> information;
};

[[nodiscard]] Expected<std::vector<std::uint8_t>> ax25_frame_octets(const Ax25FrameSpec& frame);

// The bits on the air before NRZI: flags, and each frame's octets with its
// FCS appended, least significant bit first, bit stuffed per clause 3.6.
[[nodiscard]] std::vector<std::uint8_t> hdlc_bits(
    std::span<const std::vector<std::uint8_t>> frames, const Ax25ModConfig& config);

// NRZI then AFSK: a zero changes tone and a one does not.
[[nodiscard]] Expected<std::vector<float>> afsk_render_bits(const Ax25ModConfig& config,
                                                            std::span<const std::uint8_t> bits);

// hdlc_bits then afsk_render_bits, for frames given as address through
// information.
[[nodiscard]] Expected<std::vector<float>> ax25_render(
    const Ax25ModConfig& config, std::span<const std::vector<std::uint8_t>> frames);

}  // namespace revenant::siggen
