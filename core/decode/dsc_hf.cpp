#include "core/decode/dsc_hf.h"

namespace revenant::decode {

DscConfig dsc_hf_config(SampleRate rate, DscHfSideband sideband) {
    DscConfig c;
    c.rate = rate;
    c.bit_rate = kDscHfBitRate;
    // Clause 1.4 on USB audio; LSB turns the spectrum over.
    c.y_hz = sideband == DscHfSideband::Usb ? kDscHfYHz : kDscHfBHz;
    c.b_hz = sideband == DscHfSideband::Usb ? kDscHfBHz : kDscHfYHz;
    return c;
}

std::size_t dsc_hf_dot_bits(const DscCall& call, bool to_coast_or_acknowledgement) {
    // Clause 3.4.1: distress alerts, and anything in the distress category
    // (acknowledgements and relays), always take 200 bits.
    if (call.format == kDscFormatDistress || call.category == kDscCategoryDistress) {
        return kDscHfLongDotBits;
    }
    // Clause 3.4.2: acknowledgements to individual calls and calls to coast
    // stations take 20; everything else addressed to ships takes 200.
    return to_coast_or_acknowledgement ? kDscHfShortDotBits : kDscHfLongDotBits;
}

}  // namespace revenant::decode
