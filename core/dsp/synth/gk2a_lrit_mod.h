// GK-2A LRIT transmitter for tests: LRIT files built with the header records
// GK2A table 4.16 lists for each file type, through the CGMS transport, to
// 128 ksym/s BPSK with the parameters of core/decode/gk2a_lrit.h.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/decode/gk2a_lrit.h"
#include "core/dsp/synth/xrit_mod.h"

namespace revenant::siggen {

// An uncompressed 8-bit image segment file: primary, image structure (1),
// annotation (4) and image segment identification (128) headers.
[[nodiscard]] std::vector<std::uint8_t> gk2a_image_file(const std::string& annotation, std::uint16_t columns,
                                                        std::uint16_t lines, const std::vector<std::uint8_t>& pixels,
                                                        decode::Gk2aSegment segment);

// An alphanumeric text file (file type 2) with an annotation header.
[[nodiscard]] std::vector<std::uint8_t> gk2a_text_file(const std::string& annotation, const std::string& text);

[[nodiscard]] XritBpskSpec gk2a_bpsk_spec(double sample_rate = 512000.0);

}  // namespace revenant::siggen
