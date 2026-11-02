// GPS L1C CNAV-2: complex baseband at 1575.42 MHz to decoded frames, by
// handover from L1 C/A tracking.
//
// SPECIFICATION
//
// "IS-GPS-800 Revision J" (IS-GPS-800J), 1 August 2022, Navstar GPS Space
// Segment / User Segment L1C Interface, free from gps.gov. A US Government
// work; tables are transcribed with citation in gps_l1c_tables.h.
//
//   - 3.2.2.1.1: the length-10223 Legendre sequence, the Weil code
//     W(t; w) = L(t) xor L((t + w) mod 10223), and the 7-chip expansion
//     0110100 inserted before the p-th Weil chip, giving 10230 chips.
//   - 3.2.2.1.2 and Table 3.2-3: the 1800-bit L1CO overlay from an 11-stage
//     generator, one bit per 10 ms pilot code period, aligned to the frame.
//   - 3.3 and Figure 3.3-2: BOC(1,1) with sine phasing, so chip value b
//     becomes the half-chip pair (b, not b) in the "1 = positive" sense;
//     logic 0 is in phase with the carrier. L1CP is TMBOC: symbols with
//     t mod 33 in {0, 4, 6, 29} are BOC(6,1).
//   - 3.2.1.6.1: L1CD and L1CP carriers in phase.
//   - 3.2.3.1 and Figure 3.2-3: an 18 s frame of 1800 symbols at 100 sym/s,
//     52 symbols of subframe 1 then 1748 interleaved symbols of subframes 2
//     and 3.
//   - 3.2.3.2 and Figure 3.2-4: TOI, 9 bits, the low 8 through the BCH
//     (51,8) generator 763 octal, the ninth added to every symbol and sent
//     first; decoded by correlating against all 256 code words, as the
//     section itself describes.
//   - 3.2.3.3: CRC-24Q, g(X) with g_i = 1 for i in {0,1,3,4,5,6,7,10,11,14,
//     17,18,23,24}, seed 0, over each of subframes 2 and 3.
//   - 3.2.3.4 and Tables 6.2-2 to 6.2-13: rate 1/2 LDPC, H = [A B T; C D E],
//     subframe 2 (1200,600), subframe 3 (548,274), code word (s; p1; p2).
//     Decoded with the generic min-sum decoder in core/decode/ldpc.h.
//   - 3.2.3.5: the 38-row by 46-column block interleaver, written by rows,
//     read by columns.
//   - 3.5.2, 3.5.3 and Figure 3.5-1: the subframe 2 fields this file reads
//     (WN, ITOW, top, L1C health, URA_ED index, toe); 3.5.4: subframe 3
//     begins with the 8-bit PRN and the 6-bit page number.
//
// HANDOVER
//
// L1C is not searched for cold. IS-GPS-800J 3.2.2.1 makes the L1C codes
// time synchronised, and IS-GPS-200N makes C/A synchronised to GPS time, so
// once C/A is tracked an L1C code epoch coincides with one of the next ten
// C/A code epochs, at the same carrier Doppler and the same code Doppler
// ratio. Which one is unknown without the time of week. The decoder tries all
// ten: at each candidate C/A epoch, with the C/A loop's fractional code phase
// carried over, it correlates two 10 ms pilot periods non-coherently and
// takes the largest, accepting it when it beats the mean of the other nine
// by `handover_threshold`. That is ten correlations instead of a 10230-chip
// by Doppler-bin search.
//
// TRACKING
//
// The pilot is tracked with the shared TrackingChannel (gnss_acq.h), its code
// expanded to BOC(1,1) half-chips at 2.046 Mchip/s, and the data channel is
// read as that channel's auxiliary correlator at the same code phase and
// carrier. The pilot replica is plain BOC(1,1): the four BOC(6,1) symbols in
// every 33 of TMBOC correlate to almost nothing against it, which loses
// 20 log10(29/33) = 1.1 dB of pilot power and keeps the replica one table.
// At the 2 to 4 MS/s a dongle delivers, most of the BOC(6,1) energy is
// outside the tap anyway.
//
// FRAME SYNC
//
// The pilot overlay is known and aligned to the frame (Figure 3.3-1), so
// correlating 300 locked pilot prompts against every rotation of the 1800-bit
// overlay finds the frame edge and, through the sign of the peak, the absolute
// carrier phase. The data symbols take their polarity from that, so no
// symbol-level preamble search is needed.
//
// DEVIATIONS
//
// None from the document's definitions. Subframe 3 pages are reported as raw
// bits after the PRN and page number; subframe 2 fields after toe are left in
// the raw bits.
//
// CLEAN ROOM
//
// No GNSS implementation was read. See core/decode/gnss_acq.h.

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "core/decode/gnss_acq.h"
#include "core/decode/ldpc.h"
#include "core/error.h"

namespace revenant::decode::gnss {

// ---------------------------------------------------------------------------
// Codes. All +1/-1 with logic 0 as +1.

// L(t), t = 0..10222, as 0/1.
[[nodiscard]] const std::vector<std::uint8_t>& l1c_legendre();

// L1CP (pilot) or L1CD (data) ranging code, PRN 1 to 63, 10230 chips.
[[nodiscard]] std::vector<std::int8_t> l1c_code(unsigned prn, bool pilot);

// L1CO overlay, PRN 1 to 63, 1800 bits.
[[nodiscard]] std::vector<std::int8_t> l1c_overlay(unsigned prn);

// A code expanded to BOC(1,1) half-chips with sine phasing.
[[nodiscard]] std::vector<std::int8_t> boc11(std::span<const std::int8_t> code);

// ---------------------------------------------------------------------------
// Channel coding. Bits one per byte, 0 or 1, first transmitted first.

// The 52 symbols of subframe 1 for a 9-bit TOI.
[[nodiscard]] std::array<std::uint8_t, 52> l1c_toi_encode(unsigned toi);
// Soft symbols, positive for 0. Returns the TOI.
[[nodiscard]] unsigned l1c_toi_decode(std::span<const float> soft);

// CRC-24Q remainder, p1 in bit 23.
[[nodiscard]] std::uint32_t crc24q(std::span<const std::uint8_t> bits);

// The parity checks of H for subframe 2 or 3, ready for LdpcDecoder.
[[nodiscard]] std::vector<std::vector<std::uint32_t>> l1c_ldpc_checks(unsigned subframe);

// The 3.2.3.4 encoder: 600 or 274 information bits in, twice as many out.
[[nodiscard]] std::vector<std::uint8_t> l1c_ldpc_encode(unsigned subframe,
                                                        std::span<const std::uint8_t> info);

// 3.2.3.5. 1748 symbols in either direction.
[[nodiscard]] std::vector<std::uint8_t> l1c_interleave(std::span<const std::uint8_t> in);
template <typename T>
[[nodiscard]] std::vector<T> l1c_deinterleave(std::span<const T> in) {
    std::vector<T> out(in.size());
    for (std::size_t r = 0; r < 38; ++r) {
        for (std::size_t c = 0; c < 46; ++c) {
            out[r * 46 + c] = in[c * 38 + r];
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Decoded output.

struct L1cSubframe2 {
    unsigned wn = 0;        // 13 bits, bits 1-13
    unsigned itow = 0;      // 8 bits, 14-21
    unsigned top = 0;       // 11 bits, 22-32, units of 300 s
    bool l1c_health = false;  // bit 33
    int ura_ed = 0;         // 5 bits two's complement, 34-38
    unsigned toe = 0;       // 11 bits, 39-49, units of 300 s
};

struct L1cFrame {
    unsigned prn = 0;
    SampleIndex sample = 0;  // first sample of the frame's first symbol
    unsigned toi = 0;
    bool sf2_ok = false;  // CRC passed after LDPC
    bool sf3_ok = false;
    std::size_t sf2_iterations = 0, sf3_iterations = 0;
    std::vector<std::uint8_t> sf2;  // 576 data bits
    std::vector<std::uint8_t> sf3;  // 250 data bits
    L1cSubframe2 clock;            // when sf2_ok
    unsigned sf3_prn = 0, sf3_page = 0;  // when sf3_ok
};

struct L1cHandover {
    unsigned prn = 0;
    SampleIndex sample = 0;  // the L1C epoch chosen
    bool success = false;
    unsigned ca_epochs_after = 0;  // which of the ten candidates
    double ratio = 0.0;            // best over mean of the rest
};

struct L1cStatus {
    unsigned prn = 0;
    double cn0_dbhz = 0.0;  // pilot
    double doppler_hz = 0.0;
    bool phase_locked = false;
    bool frame_synced = false;
};

struct GpsL1cConfig {
    dsp::SampleRate sample_rate = 4'096'000;
    double offset_hz = 0.0;     // where 1575.42 MHz sits relative to DC
    std::vector<unsigned> prns;  // empty: 1 to 32
    FrontEndConfig ca;           // the C/A front end; sample_rate overwritten
    TrackingConfig l1c_tracking{2.0, 10.0, 1.0, 0.5, 20, 100, 24.0, 300};
    // C/A periods of phase lock before handing over.
    unsigned handover_after_periods = 500;
    double handover_threshold = 2.0;
    unsigned status_periods = 100;  // L1C periods between status lines
};

struct GpsL1cOutput {
    std::vector<AcquisitionEvent> acquisitions;  // C/A
    std::vector<L1cHandover> handovers;
    std::vector<L1cStatus> status;
    std::vector<L1cFrame> frames;
};

class GpsL1cDecoder {
public:
    [[nodiscard]] static Expected<GpsL1cDecoder> create(GpsL1cConfig config);
    void feed(std::span<const dsp::Complex32> samples, GpsL1cOutput& out);

private:
    GpsL1cDecoder() = default;

    struct Sat {
        unsigned prn = 0;
        std::vector<std::int8_t> pilot, data, overlay;
        // C/A epochs from the one handover starts at.
        struct Epoch {
            SampleIndex sample = 0;
            double code_phase_chips = 0.0;
            double doppler_hz = 0.0;
        };
        std::deque<Epoch> epochs;
        unsigned locked_periods = 0;
        bool handover_pending = false;
        bool handed_over = false;
        std::optional<TrackingChannel> channel;
        std::vector<Prompt> prompts;  // consecutive locked pilot prompts before sync
        bool synced = false;
        std::uint64_t frame_period = 0;  // channel period of a frame start
        double polarity = 1.0;
        std::vector<float> symbols;
        SampleIndex frame_sample = 0;
        std::uint64_t last_status = 0;
    };

    void try_handover(Sat& sat, GpsL1cOutput& out);
    void handle_l1c(Sat& sat, const Prompt& p, GpsL1cOutput& out);
    void decode_frame(Sat& sat, GpsL1cOutput& out);
    [[nodiscard]] double correlate(const Sat& sat, SampleIndex start, double code_phase_chips,
                                   double doppler_hz) const;

    GpsL1cConfig config_;
    std::optional<FrontEnd> ca_;
    std::map<unsigned, Sat> sats_;
    std::optional<LdpcDecoder> ldpc2_, ldpc3_;
    // Recent input, for starting an L1C channel at an epoch already passed.
    std::vector<dsp::Complex32> ring_;
    SampleIndex ring_start_ = 0;
    SampleIndex next_ = 0;
};

}  // namespace revenant::decode::gnss
