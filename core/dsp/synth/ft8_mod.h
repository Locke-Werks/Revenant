// An FT8 and FT4 transmitter, written from the same paper as the receiver
// in core/decode/ft8.h, for round-trip tests.
//
// It renders audio the way a sound card feeds an SSB transmitter: the
// waveform of core/decode/ft8.h's ft_reference moved up to `frequency_hz`,
// placed `start_seconds` into a buffer. Every constant comes from the
// decoder's FtProtocol, never restated here.

#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "core/decode/ft8.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::SampleRate;

struct FtModConfig {
    decode::FtProtocol protocol = decode::ft8_protocol();
    SampleRate rate = 12000;
    // Frequency of tone 0.
    double frequency_hz = 1000.0;
    // Where the first symbol starts in the buffer.
    double start_seconds = 0.5;
    double amplitude = 0.5;
};

// Adds the transmission of `payload` into `audio`, which must already be
// sized; samples past its end are dropped.
[[nodiscard]] Status ft_add(const FtModConfig& config, const decode::Wsjt77& payload,
                            std::span<float> audio);

// Packs `text` with core/decode/wsjt77.h and adds it.
[[nodiscard]] Status ft_add_text(const FtModConfig& config, std::string_view text,
                                 std::span<float> audio);

}  // namespace revenant::siggen
