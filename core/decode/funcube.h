// FUNcube telemetry: 1200 bit/s BPSK channel symbols to 256-octet frames and
// their real-time telemetry.
//
// SPECIFICATION
//
// Two documents:
//
//   - "FUNcube-1 Downlink Data", Doc-ID FUNcube.downlink data.doc, Issue 1.0
//     Final as flown, 05/03/2015, AMSAT-UK, "Distribution: Unlimited public
//     release", from funcube.org.uk/wp-content/uploads/2010/11/
//     funcube-1-downlink-data-spec_ver1-as_flown.pdf, fetched 2026-11-02.
//     Cited as "FC" with a section. It fixes the frame (section 1: 2-bit
//     satellite id, 6-bit frame type, 440 bits of real-time telemetry, 1600
//     bits of payload), the field layout (section 4) and the schedule
//     (section 5), and says the coding is the AO-40 FEC telemetry model.
//   - Phil Karn KA9Q, "A Proposal for a Coded AO-40 Telemetry Format", v1.2,
//     7 January 2002, ka9q.net/papers/ao40tlm.html, fetched 2026-11-02.
//     Cited as "AO40". The coding: two (160,128) Reed-Solomon codewords,
//     even and odd octets, shortened from CCSDS (255,223) in the
//     conventional basis; the CCSDS 255-bit scrambler over all 320 octets;
//     the CCSDS K=7 rate 1/2 code with G2 inverted and six tail bits; an 80
//     by 65 bit block interleaver written by rows from the second row and
//     read by columns, whose first row is a 65-bit sync vector from
//     x^7 + x^3 + 1 seeded with ones. The page links a reference encoder in
//     C; it was not opened.
//
// The Reed-Solomon, scrambler and convolutional pieces are the ones in
// core/decode/ccsds.h, with CcsdsRsConfig::dual_basis false, depth 2 and 95
// symbols of virtual fill per codeword (190 over the block).
//
// DEVIATIONS AND CHOICES
//
//   - Differential encoding. AO40 lists a differential encoder in the
//     Phase 3 hardware chain before the modulator, and FC says only "1200bps
//     BPSK". This decoder takes differentially encoded symbols by default,
//     which also removes BPSK's phase ambiguity; `differential` false takes
//     coherent BPSK and resolves the ambiguity on the sync vector's sign.
//     Which one FUNcube-1 flies is not stated in either document, so the
//     default is a choice and labelled as one.
//   - AO40's Manchester encoding belongs to AO-40's 400 bit/s hardware.
//     FC's 5200 bits in about 4.3 s are 1200 symbols a second with no
//     Manchester stage, so none is applied here.
//   - FC section 5's schedule table lost its row alignment in text
//     extraction; the frame type to content map below is read from its
//     "Frame Id" column (WO1-12 are 1-12, HR1 13, FM1-3 14-16, HR2 17, FM4-6
//     18-20, HR3 21, FM7-9 22-24), which agrees with the 12 / 3 / 9 counts
//     the same section states.
//   - Fields are packed most significant bit first after the 8-bit header,
//     per FC section 3's "transmitted MSB first", in the order of FC section
//     4's table. Values are raw counts; FC publishes no calibrations.
//   - Frame sync: a soft correlation of the 65 sync positions, every 80th
//     symbol, against the vector, accepted above `sync_threshold` of the
//     ideal. The threshold is chosen, not cited.
//
// CLEAN ROOM
//
// No FUNcube, AO-40 or gr-satellites implementation was read; the AO-40
// page's linked encoder was not opened.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/decode/ccsds.h"
#include "core/decode/dv_codes.h"
#include "core/error.h"

namespace revenant::decode {

// FC section 1 and AO40.
inline constexpr double kFuncubeSymbolRate = 1200.0;
inline constexpr std::size_t kFuncubeFrameOctets = 256;
inline constexpr std::size_t kFuncubeRows = 80;
inline constexpr std::size_t kFuncubeColumns = 65;
inline constexpr std::size_t kFuncubeBlockSymbols = kFuncubeRows * kFuncubeColumns;  // 5200
inline constexpr std::size_t kFuncubeCodedSymbols = (2048 + 512 + 6) * 2;               // 5132

// AO40 "Block Interleaving and Sync": the 65 bits of x^7 + x^3 + 1 from ones.
[[nodiscard]] std::array<std::uint8_t, kFuncubeColumns> funcube_sync_vector();

// The RS, scrambler, convolutional code and interleaver of AO40, in both
// directions, for the transmitter and the tests. `frame` is 256 octets;
// the result is 5200 bits in transmission order, before differential
// encoding.
[[nodiscard]] std::vector<std::uint8_t> funcube_encode_block(std::span<const std::uint8_t> frame);

enum class FuncubeContent : std::uint8_t { WholeOrbit, HighResolution, FitterMessage, Unknown };

// FC section 4, real-time telemetry, raw counts.
struct FuncubeRealTime {
    std::array<std::uint16_t, 3> photo_voltage{};
    std::uint16_t total_photo_current = 0;
    std::uint16_t battery_voltage = 0;
    std::uint16_t total_system_current = 0;
    std::uint16_t reboot_count = 0;
    std::uint16_t eps_software_errors = 0;
    std::array<std::uint8_t, 3> boost_converter_temp{};
    std::uint8_t battery_temp = 0;
    std::uint8_t latch_up_5v1 = 0;
    std::uint8_t latch_up_3v3_1 = 0;
    std::uint8_t reset_cause = 0;
    std::uint8_t power_point_tracking_mode = 0;
    // BOB: sun sensors X+, Y+, Z+; panel temps X+, X-, Y+, Y-; 3.3 V bus
    // voltage and current; 5.0 V bus voltage.
    std::array<std::uint16_t, 10> bob{};
    // RF: Doppler, RSSI, temperature, receive current, transmit current on
    // the 3.3 V and 5.0 V buses.
    std::array<std::uint8_t, 6> rf{};
    // PA: reverse power, forward power, board temperature, board current.
    std::array<std::uint8_t, 4> pa{};
    std::array<std::uint8_t, 2> antenna_temp{};
    std::array<bool, 4> antenna_deployed{};
    std::uint32_t sequence_number = 0;
    std::uint8_t dtmf_command_count = 0;
    std::uint8_t dtmf_last_command = 0;
    bool dtmf_command_success = false;
    // ASIB, EPS, PA, RF, MSE, ANTS bus B, ANTS bus A.
    std::array<bool, 7> data_valid{};
    bool in_eclipse = false;
    bool in_safe_mode = false;
    bool hardware_abf = false;
    bool software_abf = false;
    bool deployment_wait = false;
};

struct FuncubeFrame {
    std::array<std::uint8_t, kFuncubeFrameOctets> octets{};
    std::uint8_t satellite_id = 0;  // FC section 2
    std::uint8_t frame_type = 0;    // FC section 3, 1-24
    FuncubeContent content = FuncubeContent::Unknown;
    FuncubeRealTime real_time;
    // The 200 payload octets; for a fitter message, its text.
    std::vector<std::uint8_t> payload;
    // Reed-Solomon symbols corrected in each codeword.
    std::array<std::size_t, 2> corrected{};
    // Sync correlation as a fraction of the ideal, and the absolute symbol
    // index of the block's first symbol.
    double sync_quality = 0.0;
    std::uint64_t symbol_position = 0;
};

[[nodiscard]] FuncubeContent funcube_content(std::uint8_t frame_type);
// Parses the header, the real-time block and the payload of 256 octets.
[[nodiscard]] FuncubeFrame funcube_parse(std::span<const std::uint8_t, kFuncubeFrameOctets> octets);

struct FuncubeDecoderConfig {
    bool differential = true;
    double sync_threshold = 0.55;
};

class FuncubeDecoder {
public:
    [[nodiscard]] static Expected<FuncubeDecoder> create(const FuncubeDecoderConfig& config = {});

    // Soft symbols, one per channel symbol, in the SoftSpan convention.
    // Chunks may split anywhere.
    void push(SoftSpan soft, std::vector<FuncubeFrame>& out);
    void reset();

    // Blocks whose sync matched but whose Reed-Solomon decode failed.
    [[nodiscard]] std::size_t failed() const { return failed_; }

private:
    FuncubeDecoder() = default;
    void try_block(std::vector<FuncubeFrame>& out);

    FuncubeDecoderConfig config_;
    std::optional<CcsdsReedSolomon> rs_;
    std::array<float, kFuncubeColumns> sync_{};
    std::vector<float> ring_;  // the last 5200 symbols
    std::size_t head_ = 0;
    std::uint64_t count_ = 0;
    float previous_ = 0.0F;
    std::uint64_t hold_until_ = 0;
    std::size_t failed_ = 0;
};

}  // namespace revenant::decode
