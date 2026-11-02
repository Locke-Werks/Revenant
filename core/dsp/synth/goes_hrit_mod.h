// GOES HRIT/EMWIN transmitter for tests: EMWIN text and uncompressed image
// files with the header type 129 layout observed in NOAA's sample files (see
// core/decode/goes_hrit.h), through the CGMS transport, to 927 ksym/s BPSK.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/decode/goes_hrit.h"
#include "core/dsp/synth/xrit_mod.h"

namespace revenant::siggen {

[[nodiscard]] decode::XritHeaderRecord noaa_record(const decode::NoaaHeader& h);

// File type 2, annotation and NOAA header with product 9, uncompressed.
[[nodiscard]] std::vector<std::uint8_t> emwin_text_file(const std::string& filename, const std::string& bulletin);

// File type 0, uncompressed 8-bit, image structure and annotation headers.
[[nodiscard]] std::vector<std::uint8_t> goes_image_file(const std::string& annotation, std::uint16_t columns,
                                                        std::uint16_t lines, const std::vector<std::uint8_t>& pixels);

[[nodiscard]] XritBpskSpec goes_hrit_bpsk_spec(double sample_rate = 2.4e6);

}  // namespace revenant::siggen
