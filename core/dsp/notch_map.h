// Where an audio-frequency notch sits in a receiver's passband, by mode.
//
// The notch runs on demodulated audio (core/shaders/noise_line.comp), so a
// display that wants to draw it, or to show what it removes, has to go the
// other way: from the audio frequency the operator hears to the signed offset
// from the receiver's centre where that audio came from. One table, shared by
// the engine's display path and the UI, so the two cannot disagree.
//
// The inverse of dsp::notch_audio_hz in core/dsp/noise_reference.h, except on
// NFM: there the engine reads an offset's magnitude as the audio frequency so
// the control has an axis, but a discriminator's audio has no place in the
// passband, so for drawing NFM is audio only, like WFM and the digital voice
// modes.

#pragma once

#include <array>
#include <cstdint>

#include "core/engine/vrx.h"

namespace revenant::dsp {

struct NotchOffsets {
    // Signed hertz from the receiver's centre, count of them used.
    std::array<double, 2> hz{};
    std::uint8_t count = 0;

    // The mode makes audio but the notch has no passband location: draw it as
    // a marker, with no attenuation.
    bool audio_only = false;
};

// USB: +f. LSB: -f. AM, SAM and DSB: both. CW: f - pitch, since the fine
// stage mixes the carrier to the pitch with upper-sideband sense. NFM, WFM and
// the digital voice modes: none, audio only. Raw makes no audio: none, and not
// audio only either. A non-positive audio_hz has no location on any mode.
[[nodiscard]] constexpr NotchOffsets notch_offsets(engine::Demod mode, double audio_hz,
                                                   double cw_pitch_hz = 0.0) {
    NotchOffsets out;
    switch (mode) {
        case engine::Demod::Usb:
            out.hz[0] = audio_hz;
            out.count = 1;
            break;
        case engine::Demod::Lsb:
            out.hz[0] = -audio_hz;
            out.count = 1;
            break;
        case engine::Demod::Am:
        case engine::Demod::Sam:
        case engine::Demod::Dsb:
            out.hz[0] = -audio_hz;
            out.hz[1] = audio_hz;
            out.count = 2;
            break;
        case engine::Demod::Cw:
            out.hz[0] = audio_hz - cw_pitch_hz;
            out.count = 1;
            break;
        case engine::Demod::Nfm:
        case engine::Demod::Wfm:
        case engine::Demod::P25p1:
        case engine::Demod::Dstar:
        case engine::Demod::Tetra:
        case engine::Demod::Dmr: out.audio_only = true; break;
        case engine::Demod::Raw: break;
    }
    if (!(audio_hz > 0.0)) {
        out.count = 0;
    }
    return out;
}

}  // namespace revenant::dsp
