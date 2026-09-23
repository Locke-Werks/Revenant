// An M17 stream-mode transmitter, written from the same specification as the
// decoder in core/decode/m17.h.
//
// The reason core/dsp/synth/dv_mod.h gives for its three modes applies: the
// round trip stands in for the scalar twin a host decoder does not have, and
// every specified constant is included from the decoder's header rather than
// restated.
//
// WHAT IT SENDS
//
// A complete stream-mode transmission per M17 2.8, Table 2.7: the 1.4.1
// preamble, the Link Setup Frame, one stream frame per payload with the LSF
// spread across their LICH six chunks at a time, the end-of-stream bit on the
// last frame number, and the 1.4.5 End of Transmission marker. Payloads are
// whatever 16 bytes the caller supplies; no Codec 2 is generated.
//
// THE SHAPING FILTER HAS UNIT GAIN AT ZERO HERTZ
//
// M17 1.3 says the symbols pass through the root raised cosine "before
// frequency modulation" and Table 1.1 gives the deviation per symbol. The
// only reading under which a run of +3 symbols deviates by the table's
// 2.4 kHz is a filter whose response at zero hertz is one, with each symbol
// impulse carrying the samples per symbol as its weight. That is what this
// does, and the decoder's receive scaling says the same thing from the other
// end.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/m17.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::Complex32;
using dsp::SampleRate;

struct M17ModConfig {
    SampleRate rate = 48000;
    double amplitude = 1.0;
};

struct M17StreamMessage {
    // Appendix A addresses, already encoded.
    std::uint64_t destination = 0;
    std::uint64_t source = 0;

    // Table 3.7: stream mode, voice. Bit 0 set and data type 10 in bits 2..1.
    std::uint16_t type = 0x0005;
    std::array<std::uint8_t, 14> meta{};

    std::vector<std::array<std::uint8_t, decode::kM17StreamPayloadBytes>> payloads;
};

// The symbols of the whole transmission, in sending order, as the integers
// +/-1 and +/-3 of Table 1.1.
[[nodiscard]] Expected<std::vector<int>> m17_stream_symbols(const M17StreamMessage& message);

// Renders symbols as 4FSK complex baseband per M17 1.3. The sample rate must
// be a whole multiple of 4800.
[[nodiscard]] Expected<std::vector<Complex32>> m17_render_symbols(const M17ModConfig& config,
                                                                  std::span<const int> symbols);

[[nodiscard]] Expected<std::vector<Complex32>> m17_render_stream(const M17ModConfig& config,
                                                                 const M17StreamMessage& message);

}  // namespace revenant::siggen
