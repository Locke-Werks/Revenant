// FT4: the FT8 payload, CRC and LDPC code on a faster 4-GFSK waveform in a
// 7.5 s slot. The decoder is core/decode/ft8.h's FtDecoder with this
// protocol description in its configuration.
//
// SPECIFICATION
//
// Franke, Somerville and Taylor, "The FT4 and FT8 Communication Protocols",
// QEX July/August 2020 pp. 7-17: section 4 and Table 3 (two bits per symbol,
// Gray map 00 01 11 10; the four Costas arrays S1 to S4; the ramp symbol R
// and the 105-symbol sequence R S1 M_A S2 M_B S3 M_C S4 R), section 5 and
// Table 4 (T = 0.048 s, h = 1, BT = 1, raised-cosine ramps over the whole of
// each R symbol), and Appendix A (the 77-bit scrambler applied before the
// CRC and parity are computed).
//
// DEVIATION
//
// The paper gives the 0.5 s nominal start only for FT8. FT4 is taken to
// start 0.5 s into its 7.5 s interval as well; it changes nothing but the
// reported DT, since the search covers the first 2.4 s of the slot.

#pragma once

#include "core/decode/ft8.h"

namespace revenant::decode {

[[nodiscard]] const FtProtocol& ft4_protocol();

// FtConfig with the FT4 protocol in place.
[[nodiscard]] FtConfig ft4_config();

}  // namespace revenant::decode
