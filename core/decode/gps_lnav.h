// GPS L1 C/A LNAV, and QZSS L1 C/A, which carries the same message: complex
// baseband to decoded subframes, ephemeris, clock, almanac, UTC and
// ionospheric parameters, with per-satellite C/N0 and Doppler.
//
// SPECIFICATION
//
// "IS-GPS-200 Revision N", 1 August 2022, Navstar GPS Space Segment /
// Navigation User Segment Interfaces, free from gps.gov. A US Government
// work; field positions and scale factors are transcribed below with their
// table and figure numbers. Cited as "IS-GPS-200N".
//
//   - 20.3.2 and Figure 20-1: 1500-bit frames of five 300-bit subframes of
//     ten 30-bit words, MSB first, subframes 4 and 5 paged 25 ways.
//   - 20.3.3.1, 20.3.3.2 and Figure 20-2: TLM with preamble 10001011 and the
//     integrity status flag in bit 23; HOW with the 17-bit truncated TOW
//     count, alert and anti-spoof flags and the subframe ID in bits 20-22.
//   - 20.3.5 and Table 20-XIV: the (32,26) Hamming parity, both the D29*/D30*
//     link between words and the D30* complement of bits 1-24.
//   - Table 20-I and Figure 20-1 sheet 1: subframe 1 clock and health.
//   - Table 20-III and sheets 2 and 3: ephemeris.
//   - Table 20-V and Table 20-VI, sheet 4: almanac pages, SV IDs 1 to 32.
//   - Tables 20-IX and 20-X, sheet 8: page 18 of subframe 4, UTC and
//     Klobuchar ionospheric parameters.
//
// QZSS: "IS-QZSS-PNT-006", Cabinet Office, Government of Japan. Its L1 C/A
// signal uses the LNAV structure above with PRNs 193 to 202 (Table 3.2.2-1);
// this file decodes it with the same parser and does not interpret the
// QZSS-specific almanac page assignments.
//
// WHAT IS NOT DONE
//
// A position fix. The output is the decoded message and the tracking state,
// and the ephemeris is reported in the document's units (semi-circles,
// seconds, metres to the power the table gives) without being evaluated.
// Subframe 4 and 5 pages other than the almanac pages and page 18 are
// reported as raw words.
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
#include "core/error.h"

namespace revenant::decode::gnss {

// IS-GPS-200N Table 20-XIV. `d24` is d1..d24 with d1 in bit 23. Returns
// D25..D30 with D25 in bit 5.
[[nodiscard]] std::uint32_t lnav_parity(std::uint32_t d24, bool d29_star, bool d30_star);

// A received 30-bit word (D1 in bit 29) and the last two bits of the previous
// word. On a parity match, returns d1..d24.
[[nodiscard]] std::optional<std::uint32_t> lnav_check_word(std::uint32_t word30, bool d29_star,
                                                           bool d30_star);

struct LnavSubframe {
    unsigned prn = 0;
    unsigned id = 0;          // HOW subframe ID, 1 to 5
    std::uint32_t tow_count = 0;  // HOW, 6-second units, of the NEXT subframe
    bool alert = false;
    bool anti_spoof = false;
    bool integrity_status = false;
    unsigned page_sv_id = 0;  // subframes 4 and 5: word 3 bits 63-68
    std::array<std::uint32_t, 10> data{};  // d1..d24 of each word
    SampleIndex sample = 0;   // first sample of the subframe's first bit

    // IS-GPS-200N subframe bit numbering, 1 to 300, data bits only.
    [[nodiscard]] std::uint32_t bits(unsigned first, unsigned count) const;
};

struct LnavClock {
    unsigned week = 0;
    unsigned l2_codes = 0;
    unsigned ura_index = 0;
    unsigned health = 0;
    unsigned iodc = 0;
    bool l2_p_data = false;
    double tgd_s = 0.0;
    double toc_s = 0.0;
    double af2 = 0.0, af1 = 0.0, af0 = 0.0;
};

// Semi-circles where IS-GPS-200N uses them.
struct LnavEphemeris {
    unsigned iode = 0;
    double crs_m = 0.0, delta_n_sc_s = 0.0, m0_sc = 0.0, cuc_rad = 0.0, e = 0.0, cus_rad = 0.0;
    double sqrt_a = 0.0, toe_s = 0.0;
    bool fit_interval = false;
    unsigned aodo = 0;
    double cic_rad = 0.0, omega0_sc = 0.0, cis_rad = 0.0, i0_sc = 0.0, crc_m = 0.0, omega_sc = 0.0;
    double omega_dot_sc_s = 0.0, idot_sc_s = 0.0;
};

struct LnavAlmanac {
    unsigned sv_id = 0;
    double e = 0.0, toa_s = 0.0, delta_i_sc = 0.0, omega_dot_sc_s = 0.0;
    unsigned health = 0;
    double sqrt_a = 0.0, omega0_sc = 0.0, omega_sc = 0.0, m0_sc = 0.0, af0_s = 0.0, af1 = 0.0;
};

struct LnavUtcIono {
    std::array<double, 4> alpha{};
    std::array<double, 4> beta{};
    double a0_s = 0.0, a1 = 0.0, tot_s = 0.0;
    unsigned wnt = 0, wn_lsf = 0, dn = 0;
    int dt_ls = 0, dt_lsf = 0;
};

[[nodiscard]] std::optional<LnavClock> lnav_clock(const LnavSubframe& sf);
// Needs subframes 2 and 3 with matching IODE.
[[nodiscard]] std::optional<LnavEphemeris> lnav_ephemeris(const LnavSubframe& sf2,
                                                          const LnavSubframe& sf3);
[[nodiscard]] std::optional<LnavAlmanac> lnav_almanac(const LnavSubframe& sf);
[[nodiscard]] std::optional<LnavUtcIono> lnav_utc_iono(const LnavSubframe& sf);

// 20 code periods to a bit: finds the edge from the sign-change histogram
// once the carrier is phase locked, then emits one hard bit (1 for a
// negative prompt) per 20 periods.
class LnavBitSync {
public:
    explicit LnavBitSync(unsigned periods_per_bit = 20) : n_(periods_per_bit), hist_(n_, 0) {}
    // Appends (bit, sample of the bit's first period) to `out`.
    void push(const Prompt& p, std::vector<std::pair<std::uint8_t, SampleIndex>>& out);
    [[nodiscard]] bool synced() const { return synced_; }

private:
    unsigned n_;
    std::vector<unsigned> hist_;
    unsigned seen_ = 0;
    double last_i_ = 0.0;
    bool synced_ = false;
    unsigned edge_ = 0;
    double acc_ = 0.0;
    unsigned count_ = 0;
    SampleIndex bit_start_ = 0;
};

// Bits to parity-checked subframes.
class LnavFramer {
public:
    explicit LnavFramer(unsigned prn) : prn_(prn) {}
    void push(std::uint8_t bit, SampleIndex sample, std::vector<LnavSubframe>& out);

private:
    unsigned prn_;
    std::vector<std::uint8_t> bits_;
    std::vector<SampleIndex> samples_;
};

struct SatelliteStatus {
    unsigned prn = 0;
    double cn0_dbhz = 0.0;
    double doppler_hz = 0.0;
    bool phase_locked = false;
    bool bit_synced = false;
    SampleIndex sample = 0;
};

struct GpsL1caConfig {
    dsp::SampleRate sample_rate = 2'048'000;
    double offset_hz = 0.0;  // where 1575.42 MHz sits relative to the stream's DC
    std::vector<unsigned> prns;  // empty: GPS 1 to 32
    FrontEndConfig front_end;    // sample_rate is overwritten from above
    // A status line per satellite every this many code periods.
    unsigned status_periods = 1000;
};

struct GpsL1caOutput {
    std::vector<AcquisitionEvent> acquisitions;
    std::vector<SatelliteStatus> status;
    std::vector<LnavSubframe> subframes;
    std::vector<std::pair<unsigned, LnavClock>> clocks;
    std::vector<std::pair<unsigned, LnavEphemeris>> ephemerides;
    std::vector<LnavAlmanac> almanacs;
    std::vector<LnavUtcIono> utc_iono;
};

class GpsL1caDecoder {
public:
    [[nodiscard]] static Expected<GpsL1caDecoder> create(GpsL1caConfig config);
    void feed(std::span<const dsp::Complex32> samples, GpsL1caOutput& out);

private:
    explicit GpsL1caDecoder(GpsL1caConfig config, FrontEnd fe)
        : config_(std::move(config)), fe_(std::move(fe)) {}

    struct Sat {
        LnavBitSync sync;
        LnavFramer framer;
        std::optional<LnavSubframe> sf2;
        std::optional<LnavSubframe> sf3;
    };

    GpsL1caConfig config_;
    FrontEnd fe_;
    std::map<unsigned, Sat> sats_;
    FrontEndOutput scratch_;
};

}  // namespace revenant::decode::gnss
