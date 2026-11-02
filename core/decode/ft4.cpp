#include "core/decode/ft4.h"

#include <string_view>

namespace revenant::decode {

namespace {

FtProtocol make_ft4() {
    FtProtocol p;
    p.name = "FT4";
    p.tones = 4;
    p.bits_per_symbol = 2;
    // Table 3, columns 1 and 3.
    p.gray = {0b00, 0b01, 0b11, 0b10};
    // Section 4: R S1 M_A S2 M_B S3 M_C S4 R.
    static constexpr int kS[4][4] = {{0, 1, 3, 2}, {1, 0, 2, 3}, {2, 3, 1, 0}, {3, 2, 0, 1}};
    p.layout.push_back(kFtRamp);
    for (int g = 0; g < 4; ++g) {
        for (const int c : kS[g]) p.layout.push_back(c);
        if (g < 3) p.layout.insert(p.layout.end(), 29, kFtData);
    }
    p.layout.push_back(kFtRamp);
    // Table 4: T = 0.048 s, 576 samples at 12 kHz; section 5: BT = 1 and the
    // ramp over the whole R symbol.
    p.samples_per_symbol = 576;
    p.gaussian_bt = 1.0;
    p.ramp_samples = 576;
    p.slot_seconds = 7.5;
    p.nominal_start_seconds = 0.5;
    // NOT SPECIFIED: 666.7 Hz baseband, 32 samples per symbol.
    p.decimation = 18;
    // Appendix A.
    constexpr std::string_view kScrambler =
        "0100101001011110100010"
        "0110110100101100001000"
        "1010011110010101010110"
        "11111000101";
    static_assert(kScrambler.size() == 77);
    p.scramble = true;
    for (std::size_t i = 0; i < 77; ++i) p.scrambler[i] = kScrambler[i] == '1' ? 1 : 0;
    return p;
}

}  // namespace

const FtProtocol& ft4_protocol() {
    static const FtProtocol p = make_ft4();
    return p;
}

FtConfig ft4_config() {
    FtConfig c;
    c.protocol = ft4_protocol();
    return c;
}

}  // namespace revenant::decode
