#include "core/decode/crc.h"

namespace revenant::decode {

Expected<Crc> Crc::create(const CrcParams& params) {
    if (params.width < 1 || params.width > 64) {
        return fail("a CRC is between 1 and 64 bits wide");
    }
    const std::uint64_t outside = ~crc_mask(params.width);
    if ((params.polynomial & outside) != 0U || (params.init & outside) != 0U ||
        (params.xor_out & outside) != 0U) {
        return fail("a CRC parameter has bits above the CRC's width");
    }
    return Crc(params);
}

}  // namespace revenant::decode
