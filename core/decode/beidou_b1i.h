// BeiDou B1I, D1 navigation message (MEO and IGSO satellites): complex
// baseband to verified subframes, clock, ionospheric and ephemeris
// parameters, with per-satellite C/N0 and Doppler.
//
// SPECIFICATION
//
// "BeiDou Navigation Satellite System Signal In Space Interface Control
// Document, Open Service Signal B1I (Version 3.0)", China Satellite
// Navigation Office, February 2019, BDS-SIS-ICD-B1I-3.0, free in English.
// Cited as "B1I ICD".
//
//   - 4.3, Table 4-1: the ranging code, in core/decode/gnss_acq.h.
//   - 5.1.3, Figures 5-1 to 5-4, Table 5-2: BCH(15,11,1) with g(X) = 1 + X +
//     X^4, and the bit-by-bit interleave of two codewords into words 2-10.
//     Word 1 carries 15 uncoded bits then one codeword.
//   - 5.2.1: the 20-bit Neumann-Hoffman code on every millisecond of D1.
//   - 5.2.2, 5.2.3, Figures 5-6 and 5-8 to 5-10: 300-bit subframes,
//     preamble 11100010010, FraID in bits 16-18, SOW in bits 19-26 and
//     31-42.
//   - 5.2.4.5 to 5.2.4.12, Tables 5-5, 5-7 and 5-10: the subframe 1 to 3
//     fields and their scale factors.
//
// WHAT IS NOT DONE
//
// D2, the 500 bit/s message of the GEO satellites (PRN 1 to 5 and 59 to 63):
// they are acquired and tracked like any other, but their symbols are not
// framed. Subframes 4 and 5 of D1 are verified and reported raw. No position
// is computed.
//
// WHY TWO PREAMBLES
//
// BCH(15,11,1) is a perfect code: every 15-bit word is within one error of a
// codeword, so a decode that "succeeds" says nothing about whether the
// alignment is right. A subframe is accepted only when the preamble also
// appears 300 bits later, which is what the ICD's fixed subframe length
// guarantees and noise does not.
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

// B1I ICD 5.1.3. `info` is 11 bits, first-sent in bit 10; returns the 15-bit
// codeword, first-sent in bit 14, parity in the low four bits.
[[nodiscard]] std::uint16_t b1i_bch_encode(std::uint16_t info);
// Remainder of a 15-bit word divided by g(X), D3D2D1D0 in bits 3..0.
[[nodiscard]] unsigned b1i_bch_syndrome(std::uint16_t word);
// Corrects at most one error; returns the 11 information bits and whether a
// bit was changed.
[[nodiscard]] std::uint16_t b1i_bch_decode(std::uint16_t word, bool& corrected);

struct B1iSubframe {
    unsigned prn = 0;
    unsigned fra_id = 0;
    std::uint32_t sow = 0;
    // Information bits at their B1I ICD figure positions 1..300; parity
    // positions hold zero.
    std::array<std::uint8_t, 301> bits{};
    unsigned corrections = 0;
    SampleIndex sample = 0;

    [[nodiscard]] std::uint32_t field(unsigned first, unsigned count) const;
};

struct B1iClockIono {
    bool sat_h1 = false;
    unsigned aodc = 0, urai = 0, week = 0, aode = 0;
    double toc_s = 0.0;
    double tgd1_s = 0.0, tgd2_s = 0.0;
    std::array<double, 4> alpha{}, beta{};
    double a0 = 0.0, a1 = 0.0, a2 = 0.0;
};

// Semi-circles for angles, as Table 5-10.
struct B1iEphemeris {
    double toe_s = 0.0, sqrt_a = 0.0, e = 0.0, omega_sc = 0.0, delta_n_sc_s = 0.0, m0_sc = 0.0;
    double omega0_sc = 0.0, omega_dot_sc_s = 0.0, i0_sc = 0.0, idot_sc_s = 0.0;
    double cuc_rad = 0.0, cus_rad = 0.0, crc_m = 0.0, crs_m = 0.0, cic_rad = 0.0, cis_rad = 0.0;
};

[[nodiscard]] std::optional<B1iClockIono> b1i_clock_iono(const B1iSubframe& sf1);
[[nodiscard]] std::optional<B1iEphemeris> b1i_ephemeris(const B1iSubframe& sf2,
                                                        const B1iSubframe& sf3);

// Finds the NH code phase from phase-locked prompts, then emits one soft bit
// per 20 ms with the NH code wiped.
class B1iNhSync {
public:
    void push(const Prompt& p, std::vector<std::pair<double, SampleIndex>>& out);
    [[nodiscard]] bool synced() const { return synced_; }

private:
    std::array<double, 20> ring_{};
    std::array<SampleIndex, 20> at_{};
    std::array<double, 20> metric_{};
    unsigned seen_ = 0;
    bool synced_ = false;
    unsigned offset_ = 0;
};

class B1iFramer {
public:
    explicit B1iFramer(unsigned prn) : prn_(prn) {}
    void push(std::uint8_t bit, SampleIndex sample, std::vector<B1iSubframe>& out);

private:
    unsigned prn_;
    std::vector<std::uint8_t> bits_;
    std::vector<SampleIndex> at_;
};

struct BeidouB1iConfig {
    dsp::SampleRate sample_rate = 4'096'000;
    double offset_hz = 0.0;  // where 1561.098 MHz sits relative to DC
    std::vector<unsigned> prns;  // empty: 1 to 63
    FrontEndConfig front_end;
    unsigned status_periods = 1000;
};

struct BeidouB1iOutput {
    std::vector<AcquisitionEvent> acquisitions;
    std::vector<SatelliteStatus> status;
    std::vector<B1iSubframe> subframes;
    std::vector<std::pair<unsigned, B1iClockIono>> clocks;
    std::vector<std::pair<unsigned, B1iEphemeris>> ephemerides;
};

class BeidouB1iDecoder {
public:
    [[nodiscard]] static Expected<BeidouB1iDecoder> create(BeidouB1iConfig config);
    void feed(std::span<const dsp::Complex32> samples, BeidouB1iOutput& out);

    // D1 is broadcast by MEO and IGSO satellites; Table 4-1 marks 1-5 and
    // 59-63 as GEO, which carry D2.
    [[nodiscard]] static bool is_geo(unsigned prn) { return prn <= 5 || prn >= 59; }

private:
    BeidouB1iDecoder(BeidouB1iConfig config, FrontEnd fe)
        : config_(std::move(config)), fe_(std::move(fe)) {}

    struct Sat {
        B1iNhSync sync;
        B1iFramer framer;
        std::optional<B1iSubframe> sf2;
    };

    BeidouB1iConfig config_;
    FrontEnd fe_;
    std::map<unsigned, Sat> sats_;
    FrontEndOutput scratch_;
};

}  // namespace revenant::decode::gnss
