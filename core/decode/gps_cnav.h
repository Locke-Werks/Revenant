// GPS L2C and L5 CNAV: complex baseband at a tap centred on 1227.60 MHz (L2C)
// or 1176.45 MHz (L5) to CRC-checked CNAV messages.
//
// SPECIFICATION
//
// "IS-GPS-200 Revision N" (IS-GPS-200N), 1 August 2022, and "IS-GPS-705
// Revision J" (IS-GPS-705J), 1 August 2022, both free from gps.gov. US
// Government works; tables are transcribed below with citation.
//
// L2C, IS-GPS-200N:
//   - 3.3.2.4, Figure 3-13 and Table 3-IIa: L2 CM and L2 CL from one 27-stage
//     modular generator, polynomial 1112225171 octal, 511.5 kchip/s, CM reset
//     every 10230 chips (20 ms), CL every 767250 (1.5 s). The register
//     convention here reproduces Table 3-IIa's end states for all 37 PRNs,
//     which the tests check.
//   - 3.2.2 and 3.3.3.1: CM carries the 50 sps symbols, CL no data, and the
//     two are time multiplexed chip by chip at 1.023 MHz.
//   - 3.3.3.1.1 and Figure 3-14: rate 1/2, K = 7, G1 = 171 and G2 = 133
//     octal, G1 first, encoded continuously across message boundaries.
//   - Appendix III, 30.3.2, 30.3.3 and Figure 30-1: 300-bit 12 s messages,
//     preamble 10001011, 6-bit PRN, 6-bit type, 17-bit TOW count, alert
//     flag in bit 38, CRC-24Q (30.3.5, the same code as IS-GPS-800J 3.2.3.3)
//     in bits 277 to 300; message type 10's WN, top and toe.
//
// L5, IS-GPS-705J:
//   - 3.3.2.2, Figures 3-4 and 3-5 and Table 3-Ia: I5 and Q5, 10230 chips at
//     10.23 Mchip/s, XA (1 + x^9 + x^10 + x^12 + x^13, all ones, short
//     cycled at 8190) xor XB (1 + x + x^3 + x^4 + x^6 + x^7 + x^8 + x^12 +
//     x^13, from all ones advanced by the table's count). The advance
//     reproduces the table's initial XB state for all 74 codes, checked by
//     test.
//   - 3.3.3.1.2: the 10-bit Neuman-Hofman code 0000110101 on each 10 ms I5
//     symbol; 3.3.2.3: the 20-bit code on Q5.
//   - 3.3.3.1.1 and Figure 3-7: the same K = 7 code as L2C, 100 sps.
//   - Appendix II, 20.3.2 and 20.3.3: 300-bit 6 s messages with the same
//     header and CRC as L2C.
//
// RECEIVER
//
// Both decoders run the shared FrontEnd (gnss_acq.h). L2C correlates against
// CM alone, its chips in their slots and zero in the CL slots, which recovers
// half the L2C power: 3 dB less than a CM+CL receiver, and no 1.5 s CL search.
// Which of the two slots CM occupies does not matter to a receiver that
// searches code phase in 1.023 MHz half-slots. L5 tracks I5 at 1 ms and finds
// the 10 ms symbol edge by correlating the prompt signs against the NH10 code.
//
// The Viterbi decoder is the shared StreamingViterbi (viterbi_stream.h). The
// pairing of symbols into G1/G2 is unknown at the start, and a Costas loop
// leaves the sign unknown; both generators have odd weight, so an inverted
// symbol stream decodes to inverted bits. Two decoders run, one per pairing,
// and the framer accepts the preamble or its complement, then requires the
// CRC.
//
// NARROW L5 TAPS
//
// The L5 main lobe is 20.46 MHz wide. A tap narrower than that, such as the
// 2 to 4 MS/s a dongle delivers, still tracks, with the correlation loss of
// the band it cuts off; tests/decode/test_gps_cnav.cpp measures it by
// filtering a 20.46 MS/s synthesis down to 4.092 MS/s.
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
#include "core/decode/viterbi_stream.h"
#include "core/error.h"

namespace revenant::decode::gnss {

// ---------------------------------------------------------------------------
// Codes, +1/-1 with logic 0 as +1. Empty outside PRN 1 to 37.

[[nodiscard]] std::vector<std::int8_t> l2cm_code(unsigned prn);
[[nodiscard]] std::vector<std::int8_t> l2cl_code(unsigned prn);
// The register state after `chips` - 1 shifts, for checking Table 3-IIa.
[[nodiscard]] std::uint32_t l2c_register_end(std::uint32_t initial, std::size_t chips);
[[nodiscard]] std::uint32_t l2c_initial_state(unsigned prn, bool cl);
[[nodiscard]] std::uint32_t l2c_table_end_state(unsigned prn, bool cl);

// CM in its 1.023 MHz slots with zeros between: the receiver's replica.
[[nodiscard]] std::vector<std::int8_t> l2cm_replica(unsigned prn);

[[nodiscard]] std::vector<std::int8_t> l5_code(unsigned prn, bool q5);
// Table 3-Ia's initial XB state for the code, as printed.
[[nodiscard]] unsigned l5_table_xb_state(unsigned prn, bool q5);
[[nodiscard]] std::vector<std::int8_t> l5_nh10();
[[nodiscard]] std::vector<std::int8_t> l5_nh20();

// The K = 7 code of IS-GPS-200N Figure 3-14 in viterbi_stream.h's convention.
inline constexpr std::uint32_t kCnavG1 = 0x4F;  // 171 octal, bit-reversed
inline constexpr std::uint32_t kCnavG2 = 0x6D;  // 133 octal, bit-reversed

// ---------------------------------------------------------------------------

struct CnavMessage {
    unsigned prn = 0;   // the tracked PRN
    unsigned msg_prn = 0;  // bits 9-14
    unsigned type = 0;     // bits 15-20
    unsigned tow = 0;      // bits 21-37, 6 s units
    bool alert = false;    // bit 38
    std::array<std::uint8_t, 300> bits{};
    SampleIndex sample = 0;  // first sample of the message's first bit
    // Message type 10 (Figure 30-1 / 20-1), when type == 10.
    unsigned wn = 0, top = 0, toe = 0;

    // Document bit numbering, 1 to 300.
    [[nodiscard]] unsigned field(unsigned first, unsigned count) const;
};

// Soft symbols in, messages out, for one satellite.
class CnavFramer {
public:
    explicit CnavFramer(unsigned prn);
    // `soft` positive for 0; `sample` the first sample of the symbol.
    void push(float soft, SampleIndex sample, std::vector<CnavMessage>& out);

private:
    struct Phase {
        std::optional<StreamingViterbi> viterbi;
        std::vector<std::uint8_t> bits;
        std::vector<SampleIndex> samples;  // per bit
        std::size_t decided = 0;           // bits released so far
    };
    void scan(Phase& ph, std::vector<CnavMessage>& out);

    unsigned prn_;
    std::array<Phase, 2> phases_;
    std::vector<SampleIndex> symbol_samples_;
    std::size_t symbols_ = 0;
    double scale_ = 0.0;
    SampleIndex last_message_ = 0;  // TOW and type of the last reported
    bool have_last_ = false;
};

struct CnavStatus {
    unsigned prn = 0;
    double cn0_dbhz = 0.0;
    double doppler_hz = 0.0;
    bool phase_locked = false;
    bool symbol_synced = false;
};

enum class CnavBand { L2C, L5 };

struct GpsCnavConfig {
    CnavBand band = CnavBand::L2C;
    dsp::SampleRate sample_rate = 2'048'000;
    double offset_hz = 0.0;     // where the band centre sits relative to DC
    std::vector<unsigned> prns;  // empty: 1 to 32
    // Overwritten from the band by create() unless `keep_front_end`.
    FrontEndConfig front_end;
    bool keep_front_end = false;
    unsigned status_periods = 0;  // 0: one a second
};

struct GpsCnavOutput {
    std::vector<AcquisitionEvent> acquisitions;
    std::vector<CnavStatus> status;
    std::vector<CnavMessage> messages;
};

// The front-end settings create() uses for a band: L2C searches 20 ms periods
// at 25 Hz steps, one period coherent, and its loops run at 20 ms; L5 is
// searched and tracked like C/A at 1 ms.
[[nodiscard]] FrontEndConfig cnav_front_end(CnavBand band, dsp::SampleRate rate);

class GpsCnavDecoder {
public:
    [[nodiscard]] static Expected<GpsCnavDecoder> create(GpsCnavConfig config);
    void feed(std::span<const dsp::Complex32> samples, GpsCnavOutput& out);

private:
    GpsCnavDecoder() = default;

    struct Sat {
        std::optional<CnavFramer> framer;
        // L5 symbol sync over NH10.
        std::vector<double> re;
        std::vector<SampleIndex> samples;
        std::uint64_t first_period = 0;
        bool synced = false;
        std::uint64_t sym_period = 0;  // channel period of a symbol start
        double acc = 0.0;
        SampleIndex acc_sample = 0;
        std::uint64_t last_status = 0;
        bool started = false;
    };
    void handle(const Prompt& p, GpsCnavOutput& out);

    GpsCnavConfig config_;
    std::optional<FrontEnd> fe_;
    std::map<unsigned, Sat> sats_;
    std::vector<std::int8_t> nh10_;
};

}  // namespace revenant::decode::gnss
