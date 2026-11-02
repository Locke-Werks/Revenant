// A generic cyclic redundancy check: any width from 1 to 64 bits, any
// generator, any preset, either bit order in and out, any final XOR.
//
// THE PARAMETERS
//
// The five numbers after the width are the ones R. N. Williams set out in "A
// Painless Guide to CRC Error Detection Algorithms" (1993), which is the form
// every published CRC catalogue uses: the generator without its x^width term,
// the register preset, whether each input octet enters least significant bit
// first, whether the register is reversed before output, and the value XORed
// into the result. A catalogue entry's check value is the CRC of the nine
// ASCII octets "123456789", which is what the tests hold this to.
//
// THE ALGORITHM
//
// Textbook polynomial division (Lin and Costello, "Error Control Coding",
// section 5.1), one octet at a time through a 256-entry table. A register
// that does not reflect its input is kept left-aligned in 64 bits, so the
// same eight-bit table step serves every width including those narrower than
// an octet; a reflecting register is kept right-aligned against the reversed
// generator, which is the same division with every bit order mirrored.
//
// BIT SPANS
//
// update_bits takes one bit per byte, in the order the bits enter the
// division, for fields that are not a whole number of octets. A reflecting
// CRC fed the bits of an octet least significant first gives the same result
// as feeding the octet, and a non-reflecting one fed them most significant
// first does.
//
// CLEAN ROOM
//
// No CRC implementation was read.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/error.h"

namespace revenant::decode {

struct CrcParams {
    unsigned width = 0;
    // Without the x^width term, in the low `width` bits.
    std::uint64_t polynomial = 0;
    std::uint64_t init = 0;
    bool reflect_in = false;
    bool reflect_out = false;
    std::uint64_t xor_out = 0;
};

[[nodiscard]] constexpr std::uint64_t crc_mask(unsigned width) {
    return width >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << width) - 1U);
}

[[nodiscard]] constexpr std::uint64_t crc_reflect(std::uint64_t value, unsigned width) {
    std::uint64_t out = 0;
    for (unsigned i = 0; i < width; ++i) {
        out = (out << 1U) | ((value >> i) & 1U);
    }
    return out;
}

class Crc {
public:
    // The register between update calls. Opaque: its alignment depends on
    // the parameters.
    using State = std::uint64_t;

    // Parameters are not checked here, so the constructor stays usable in a
    // constant expression; create() checks them.
    constexpr explicit Crc(const CrcParams& params) : params_(params) {
        const unsigned w = params.width;
        if (params.reflect_in) {
            const std::uint64_t poly = crc_reflect(params.polynomial & crc_mask(w), w);
            for (unsigned b = 0; b < 256; ++b) {
                std::uint64_t reg = b;
                for (int i = 0; i < 8; ++i) {
                    reg = (reg & 1U) != 0U ? (reg >> 1U) ^ poly : reg >> 1U;
                }
                table_[b] = reg;
            }
            poly_ = poly;
        } else {
            const std::uint64_t poly = (params.polynomial & crc_mask(w)) << (64U - w);
            for (unsigned b = 0; b < 256; ++b) {
                std::uint64_t reg = std::uint64_t{b} << 56U;
                for (int i = 0; i < 8; ++i) {
                    reg = (reg >> 63U) != 0U ? (reg << 1U) ^ poly : reg << 1U;
                }
                table_[b] = reg;
            }
            poly_ = poly;
        }
    }

    // Fails for a width outside 1 to 64, or a polynomial, preset or final
    // XOR with bits above the width.
    [[nodiscard]] static Expected<Crc> create(const CrcParams& params);

    [[nodiscard]] constexpr const CrcParams& params() const { return params_; }

    [[nodiscard]] constexpr State start() const {
        const std::uint64_t init = params_.init & crc_mask(params_.width);
        return params_.reflect_in ? crc_reflect(init, params_.width)
                                  : init << (64U - params_.width);
    }

    [[nodiscard]] constexpr State update(State reg, std::span<const std::uint8_t> octets) const {
        if (params_.reflect_in) {
            for (const std::uint8_t octet : octets) {
                reg = (reg >> 8U) ^ table_[(reg ^ octet) & 0xFFU];
            }
        } else {
            for (const std::uint8_t octet : octets) {
                reg = (reg << 8U) ^ table_[((reg >> 56U) ^ octet) & 0xFFU];
            }
        }
        return reg;
    }

    // One bit per byte, any nonzero byte a one.
    [[nodiscard]] constexpr State update_bits(State reg, std::span<const std::uint8_t> bits) const {
        for (const std::uint8_t bit : bits) {
            const std::uint64_t in = bit != 0U ? 1U : 0U;
            if (params_.reflect_in) {
                reg ^= in;
                reg = (reg & 1U) != 0U ? (reg >> 1U) ^ poly_ : reg >> 1U;
            } else {
                reg ^= in << 63U;
                reg = (reg >> 63U) != 0U ? (reg << 1U) ^ poly_ : reg << 1U;
            }
        }
        return reg;
    }

    [[nodiscard]] constexpr std::uint64_t finish(State reg) const {
        const unsigned w = params_.width;
        std::uint64_t value = params_.reflect_in ? reg : reg >> (64U - w);
        if (params_.reflect_in != params_.reflect_out) {
            value = crc_reflect(value, w);
        }
        return (value ^ params_.xor_out) & crc_mask(w);
    }

    [[nodiscard]] constexpr std::uint64_t compute(std::span<const std::uint8_t> octets) const {
        return finish(update(start(), octets));
    }

    [[nodiscard]] constexpr std::uint64_t compute_bits(std::span<const std::uint8_t> bits) const {
        return finish(update_bits(start(), bits));
    }

private:
    CrcParams params_;
    std::uint64_t poly_ = 0;
    std::array<std::uint64_t, 256> table_{};
};

// Catalogue parameter sets the tests check. Names as in the published
// catalogues; the aliases in brackets are the older names.
namespace crc_catalogue {
// CRC-16/IBM-3740 (CRC-16/CCITT-FALSE).
inline constexpr CrcParams kCrc16CcittFalse{16, 0x1021, 0xFFFF, false, false, 0};
// CRC-16/IBM-SDLC (X.25), the AX.25 frame check sequence.
inline constexpr CrcParams kCrc16X25{16, 0x1021, 0xFFFF, true, true, 0xFFFF};
// CRC-16/KERMIT.
inline constexpr CrcParams kCrc16Kermit{16, 0x1021, 0, true, true, 0};
// CRC-32/ISO-HDLC.
inline constexpr CrcParams kCrc32{32, 0x04C11DB7, 0xFFFFFFFF, true, true, 0xFFFFFFFF};
// CRC-24/OPENPGP.
inline constexpr CrcParams kCrc24OpenPgp{24, 0x864CFB, 0xB704CE, false, false, 0};
// CRC-14/DARC and CRC-14/GSM.
inline constexpr CrcParams kCrc14Darc{14, 0x0805, 0, true, true, 0};
inline constexpr CrcParams kCrc14Gsm{14, 0x202D, 0, false, false, 0x3FFF};
// CRC-12/DECT, CRC-12/UMTS (CRC-12/3GPP), CRC-12/CDMA2000, CRC-12/GSM.
inline constexpr CrcParams kCrc12Dect{12, 0x80F, 0, false, false, 0};
inline constexpr CrcParams kCrc12Umts{12, 0x80F, 0, false, true, 0};
inline constexpr CrcParams kCrc12Cdma2000{12, 0xF13, 0xFFF, false, false, 0};
inline constexpr CrcParams kCrc12Gsm{12, 0xD31, 0, false, false, 0xFFF};
// Narrow and full-width extremes.
inline constexpr CrcParams kCrc3Gsm{3, 0x3, 0, false, false, 0x7};
inline constexpr CrcParams kCrc5Usb{5, 0x05, 0x1F, true, true, 0x1F};
inline constexpr CrcParams kCrc64Xz{64, 0x42F0E1EBA9EA3693ULL, ~0ULL, true, true, ~0ULL};
}  // namespace crc_catalogue

}  // namespace revenant::decode
