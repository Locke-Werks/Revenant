// The (174, 91) LDPC code of FT8 and FT4.
//
// SPECIFICATION
//
// Franke, Somerville and Taylor, "The FT4 and FT8 Communication Protocols",
// QEX July/August 2020 pp. 7-17, section 3. The two matrices are not printed
// in the paper; they are the files generator.dat (83 rows by 91 columns) and
// parity.dat (174 columns, three checks each) in its reference [14],
// ft4_ft8_protocols.tgz, which section 9 of the paper places in the public
// domain. ft_ldpc.cpp carries them unchanged, transcribed by script.
//
// A code word is the 91 message-plus-CRC bits followed by 83 parity bits, and
// parity bit i is the modulo-2 sum of the message bits row i of the generator
// names. The decoder is core/decode/ldpc.h's, fed the parity checks.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/ldpc.h"
#include "core/error.h"

namespace revenant::decode {

inline constexpr std::size_t kFtN = 174;
inline constexpr std::size_t kFtK = 91;

// One entry per parity check: the code word bits it constrains, zero-based.
[[nodiscard]] const std::vector<std::vector<std::uint32_t>>& ft_ldpc_checks();

// `message` holds 91 bits, `codeword` receives 174.
void ft_ldpc_encode(std::span<const std::uint8_t> message, std::span<std::uint8_t> codeword);

[[nodiscard]] Expected<LdpcDecoder> ft_ldpc_decoder(std::size_t max_iterations = 50);

}  // namespace revenant::decode
