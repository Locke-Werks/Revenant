// GLONASS L1OF navigation message: complex baseband holding one or more FDMA
// channels to verified strings and the immediate (ephemeris) information of
// strings 1 to 4, with per-channel C/N0 and Doppler.
//
// SPECIFICATION
//
// "GLONASS Interface Control Document, Navigational radiosignal in bands L1,
// L2, Edition 5.1", Russian Institute of Space Device Engineering, 2008, in
// its free English translation. Cited as "ICD 5.1".
//
//   - 3.3.1.1 and Table 3.1: channel k on 1602 MHz + k x 0.5625 MHz.
//   - 3.3.2.1: the 511-chip ranging code, in core/decode/gnss_acq.h.
//   - 3.3.2.2 and 4.3, Figure 4.3: two-second strings, 85 bits of data at
//     50 bit/s in relative code, Modulo-2 added to a 10 ms meander, then a
//     0.3 s time mark of 30 chips at 10 ms, 111110001101110101000010010110.
//     Bit 85 is transmitted first and is an idle zero.
//   - 4.7 and Table 4.13: the Hamming check, bits 1-8, and the rule for
//     correcting one error and erasing on more.
//   - Tables 4.5 and 4.6: strings 1 to 4, the fields and their positions.
//     Signed words are sign and magnitude, "the MSB is the sign bit ... 0
//     corresponds to +".
//
// TWO READINGS THE TEXT LEAVES OPEN
//
// The figure that would settle them, Fig. 3.4, does not survive the
// translation's text layer, so both are this decoder's reading and both are
// exercised against the matching transmitter in
// core/dsp/synth/glonass_mod.h, which does not prove them against a real
// satellite:
//
//   - relative code taken as r[n] = d[n] XOR r[n-1] in transmission order,
//     which makes the receiver's 180 degree carrier ambiguity irrelevant;
//   - the meander's first 10 ms half taken as zero. Pairing the halves as
//     (first - second) gives the relative bit either way up, and the relative
//     code absorbs the sign, so the choice cannot change a decoded bit.
//
// WHAT IS NOT DONE
//
// Strings 5 to 15 (almanac and the time-scale words) are verified and
// reported raw. No position is computed from the immediate information.
//
// CLEAN ROOM
//
// No GNSS implementation was read. See core/decode/gnss_acq.h.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "core/decode/gnss_acq.h"
#include "core/decode/gps_lnav.h"
#include "core/error.h"

namespace revenant::decode::gnss {

// ICD 5.1 Table 4.13 index sets for C1..C7, as string bit numbers.
[[nodiscard]] bool glonass_in_check_set(unsigned check, unsigned bit);

// The 85 bits of a string, index = string bit number (index 0 unused).
using GlonassStringBits = std::array<std::uint8_t, 86>;

enum class GlonassCheck : std::uint8_t { Clean, Corrected, Erased };

// ICD 5.1 4.7. Corrects `bits` in place when one bit is wrong.
[[nodiscard]] GlonassCheck glonass_check_string(GlonassStringBits& bits);

// The eight check bits for data bits 9..85, written into bits 1..8.
void glonass_fill_check(GlonassStringBits& bits);

// The 30 time-mark chips, 0/1, ICD 5.1 3.3.2.2.
[[nodiscard]] std::array<std::uint8_t, 30> glonass_time_mark();

struct GlonassString {
    int channel = 0;  // frequency number k
    unsigned number = 0;  // m, bits 81-84
    GlonassStringBits bits{};
    GlonassCheck check = GlonassCheck::Clean;
    SampleIndex sample = 0;  // first sample of the string's first symbol

    // Bits `hi` down to `lo`, MSB first.
    [[nodiscard]] std::uint32_t field(unsigned hi, unsigned lo) const;
};

// Immediate information, strings 1 to 4. Kilometres, km/s, km/s^2 and
// seconds, as Table 4.5 gives them.
struct GlonassEphemeris {
    int channel = 0;
    unsigned tk_h = 0, tk_min = 0, tk_30s = 0;
    unsigned tb_min = 0;
    unsigned bn = 0;
    unsigned p1 = 0;
    bool p2 = false, p3 = false, p4 = false;
    std::array<double, 3> pos_km{}, vel_km_s{}, acc_km_s2{};
    double gamma_n = 0.0;
    double tau_n_s = 0.0;
    double delta_tau_n_s = 0.0;
    unsigned en_days = 0;
    unsigned ft = 0;
    unsigned nt_days = 0;
    unsigned slot = 0;  // n
    unsigned m_type = 0;  // M
    unsigned p = 0;
    bool ln3 = false;
};

// Needs strings 1, 2, 3 and 4 of one frame, in order.
[[nodiscard]] std::optional<GlonassEphemeris> glonass_ephemeris(
    std::span<const GlonassString, 4> strings);

// Soft 10 ms symbols to strings: finds the time mark and pairs the 170
// symbols before it.
class GlonassStringSync {
public:
    explicit GlonassStringSync(int channel) : channel_(channel) {}
    void push(double soft, SampleIndex sample, std::vector<GlonassString>& out);

private:
    int channel_;
    std::vector<double> sym_;
    std::vector<SampleIndex> at_;
};

struct GlonassL1Config {
    dsp::SampleRate sample_rate = 4'096'000;
    // Where channel 0's carrier, 1602 MHz, sits relative to the stream's DC.
    double k0_offset_hz = 0.0;
    std::vector<int> channels;  // empty: every k from -7 to +6 inside the band
    FrontEndConfig front_end;
    unsigned status_periods = 1000;
};

struct GlonassL1Output {
    std::vector<AcquisitionEvent> acquisitions;  // id is k + 100
    std::vector<SatelliteStatus> status;         // prn field is k + 100
    std::vector<GlonassString> strings;
    std::vector<GlonassEphemeris> ephemerides;
};

class GlonassL1Decoder {
public:
    [[nodiscard]] static Expected<GlonassL1Decoder> create(GlonassL1Config config);
    void feed(std::span<const dsp::Complex32> samples, GlonassL1Output& out);

    static constexpr unsigned kIdBias = 100;

private:
    GlonassL1Decoder(GlonassL1Config config, FrontEnd fe)
        : config_(std::move(config)), fe_(std::move(fe)) {}

    struct Chan {
        LnavBitSync sync{10};
        GlonassStringSync strings;
        std::vector<GlonassString> recent;
    };

    GlonassL1Config config_;
    FrontEnd fe_;
    std::map<unsigned, Chan> chans_;
    FrontEndOutput scratch_;
};

}  // namespace revenant::decode::gnss
