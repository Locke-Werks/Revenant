// Bluetooth Low Energy advertising: complex baseband to advertising PDUs.
//
// SPECIFICATION
//
// "Bluetooth Core Specification", Version 6.1, version date 2025-04-29, the
// Bluetooth SIG, from files.bluetooth.com/download/core_v6-1/ (linked from
// bluetooth.com/specifications/specs/core-specification-6-1/), downloaded
// 2026-11-02. Cited below as "Core" with volume, part and section:
//
//   - Vol 6 Part A 3.1, the GFSK modulation of the LE 1M PHY;
//   - Vol 6 Part B 1.2, bit ordering; 2.1, 2.1.1 and 2.1.2, the uncoded
//     packet, its preamble and the advertising Access Address; 2.3 and
//     2.3.1 to 2.3.3, the advertising physical channel PDU and the payloads
//     of the legacy PDU types; 3.1.1, the CRC; 3.2, data whitening;
//   - Vol 3 Part C 11, the AD structures inside AdvData and ScanRspData.
//
// WHAT IS DECODED
//
// LE 1M packets on one advertising channel: preamble and Access Address
// correlation on one-bit differential phase, de-whitening seeded by the
// channel index, the CRC-24, and the PDU header. AdvA, TargetA, ScanA and
// InitA are pulled out of the legacy PDU types whose figures place them, and
// AdvData and ScanRspData are split into AD structures. The extended PDU
// types (ADV_EXT_IND and the AUX_ family, Core Vol 6 Part B 2.3.4) are
// reported with their header and raw payload only.
//
// WHERE IT STOPS
//
// No LE 2M, no LE Coded PHY, no data channels and so no connection
// following, no Constant Tone Extension. One decoder hears one channel: the
// whitening seed is the channel index, so a wideband feed covering all three
// primary channels runs three of these, one per tuned offset.
//
// DEVIATION
//
// The specification has the receiver check the Access Address and reject on
// any mismatch (Vol 6 Part B 3.1). This one admits up to
// BleConfig::max_access_address_errors wrong bits, default one, and leans on
// the CRC instead: at low signal to noise a single flipped Access Address bit
// throws away a packet whose CRC would have passed.
//
// CLEAN ROOM
//
// Written from the specification alone. No Bluetooth implementation was
// read: not btlejack, not ubertooth, not any controller stack.

#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/decode/dv_phy.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::SampleIndex;

// Core Vol 6 Part A 3.1 and Part B 2.1: the LE 1M PHY runs at 1 Msym/s.
inline constexpr double kBleSymbolRate = 1'000'000.0;

// Core Vol 6 Part A 3.1: modulation index 0.45 to 0.55. The nominal 0.5 is a
// peak deviation of 250 kHz at 1 Msym/s.
inline constexpr double kBleNominalDeviationHz = 250'000.0;

// Core Vol 6 Part B 2.1.2: the Access Address of every advertising physical
// channel packet other than the periodic and connection-request ones.
inline constexpr std::uint32_t kBleAdvertisingAccessAddress = 0x8E89BED6U;

// Core Vol 6 Part B 3.1.1: the CRC preset for those same packets.
inline constexpr std::uint32_t kBleAdvertisingCrcInit = 0x555555U;

// Core Vol 6 Part B 3.1.1: x^24 + x^10 + x^9 + x^6 + x^4 + x^3 + x + 1, the
// x^24 term dropped, bit i the coefficient of x^i.
inline constexpr std::uint32_t kBleCrcPolynomial = 0x00065BU;

// Core Vol 6 Part B 2.1: preamble 1 octet on LE 1M, Access Address 4, CRC 3,
// PDU 2 to 258 octets.
inline constexpr std::size_t kBlePreambleBits = 8;
inline constexpr std::size_t kBleAccessAddressBits = 32;
inline constexpr std::size_t kBleCrcBits = 24;
inline constexpr std::size_t kBleMaxPduOctets = 258;

// Core Vol 6 Part B Table 2.3, the PDU Type field.
enum class BlePduType : std::uint8_t {
    AdvInd = 0b0000,
    AdvDirectInd = 0b0001,
    AdvNonconnInd = 0b0010,
    ScanReq = 0b0011,
    ScanRsp = 0b0100,
    ConnectInd = 0b0101,
    AdvScanInd = 0b0110,
    AdvExtInd = 0b0111,
    AuxConnectRsp = 0b1000,
    AdvDecisionInd = 0b1001,
};

// Core Vol 6 Part B Table 1.3: advertising channel index to centre frequency.
// 37, 38 and 39 are 2402, 2426 and 2480 MHz. Fails for any other index.
[[nodiscard]] Expected<std::int64_t> ble_advertising_channel_hz(unsigned channel_index);

// Core Vol 6 Part B 3.1.1. `bits` are one bit per element in transmission
// order, LSB of each octet first per 1.2. The result holds position i of the
// shift register in bit i, so bit 23 is the first CRC bit on air.
[[nodiscard]] std::uint32_t ble_crc24_bits(std::span<const std::uint8_t> bits, std::uint32_t init);

// The same over octets, each sent LSB first.
[[nodiscard]] std::uint32_t ble_crc24(std::span<const std::uint8_t> octets, std::uint32_t init);

// Core Vol 6 Part B 3.2: XOR the whitening sequence for `channel_index` into
// `bits` in place. The operation is its own inverse.
void ble_whiten(std::span<std::uint8_t> bits, unsigned channel_index);

// Core Vol 6 Part B 3.2: the 7-bit register's initial state, position i in
// bit i, for a channel index. Exposed so the test can check the worked
// example for channel 23.
[[nodiscard]] std::uint8_t ble_whitening_seed(unsigned channel_index);

// Core Vol 3 Part C 11: one AD structure.
struct BleAdStructure {
    std::uint8_t type = 0;
    std::vector<std::uint8_t> data;
};

// Splits AdvData or ScanRspData into AD structures, stopping at the first
// zero length octet, which begins the non-significant part. Fails when a
// structure runs past the end of the field.
[[nodiscard]] Expected<std::vector<BleAdStructure>> ble_parse_ad(std::span<const std::uint8_t> data);

// A device address as it appears on air, octet 0 first, which is the least
// significant octet (Core Vol 6 Part B 1.2).
using BleAddress = std::array<std::uint8_t, 6>;

struct BlePacket {
    // Index of the first preamble sample in the input stream, at the input
    // sample rate.
    SampleIndex sample = 0;

    std::uint32_t access_address = 0;
    unsigned channel_index = 0;

    // Core Vol 6 Part B Figure 2.5.
    std::uint8_t pdu_type = 0;
    bool ch_sel = false;
    bool tx_add = false;
    bool rx_add = false;
    std::uint8_t length = 0;

    // Header and payload, de-whitened.
    std::vector<std::uint8_t> pdu;
    std::uint32_t crc = 0;

    // Present where the PDU type's payload figure places the field.
    std::optional<BleAddress> adv_address;     // AdvA
    std::optional<BleAddress> target_address;  // TargetA
    std::optional<BleAddress> scan_address;    // ScanA
    std::optional<BleAddress> init_address;    // InitA
    std::vector<BleAdStructure> ad;

    // Carrier offset from the mean phase advance over the sync word, and the
    // peak deviation seen there, both in hertz.
    double frequency_offset_hz = 0.0;
    double deviation_hz = 0.0;

    // Wrong Access Address bits accepted, see DEVIATION above.
    unsigned access_address_errors = 0;
};

// Fills the address and AD fields of `packet` from its pdu and pdu_type.
void ble_parse_pdu_fields(BlePacket& packet);

struct BleConfig {
    // Complex baseband rate. At least 2 MS/s so the 2 MHz channel fits.
    SampleRate sample_rate = 2'400'000;

    // Seeds the whitening (Core Vol 6 Part B 3.2). 37, 38 or 39 for the
    // primary advertising channels; any 0 to 39 is accepted.
    unsigned channel_index = 37;

    std::uint32_t access_address = kBleAdvertisingAccessAddress;
    std::uint32_t crc_init = kBleAdvertisingCrcInit;

    unsigned max_access_address_errors = 1;

    // Normalised correlation against the preamble and Access Address needed
    // to try a decode.
    double sync_threshold = 0.6;
};

class BleDecoder {
   public:
    BleDecoder() = default;

    [[nodiscard]] static Expected<BleDecoder> create(const BleConfig& config);

    // Feeds a block of samples. Packets that complete inside it, with a good
    // CRC, are appended to `out`. The output for a stream does not depend on
    // how it is split into blocks.
    [[nodiscard]] Status push(dsp::ConstComplexSpan samples, std::vector<BlePacket>& out);

    [[nodiscard]] const BleConfig& config() const { return config_; }

   private:
    [[nodiscard]] std::complex<double> value_at(double absolute) const;
    [[nodiscard]] double soft_bit(double centre) const;
    [[nodiscard]] double correlate(double start, double& amplitude, double& mean) const;
    enum class Attempt : std::uint8_t { NeedMore, Rejected, Decoded };
    [[nodiscard]] Attempt try_decode(double start, std::vector<BlePacket>& out,
                                     SampleIndex& end);

    BleConfig config_;
    double samples_per_bit_ = 0.0;
    ComplexFir channel_filter_;

    std::size_t filter_delay_ = 0;

    // Channel-filtered samples, with the
    // absolute input index of iq_[0] in base_.
    std::vector<Complex32> iq_;
    SampleIndex base_ = 0;
    SampleIndex search_ = 0;

    // Preamble and Access Address as +/-1, transmission order.
    std::array<float, kBlePreambleBits + kBleAccessAddressBits> sync_{};
    double sync_mean_ = 0.0;
    double sync_energy_ = 0.0;

    std::vector<Complex32> scratch_;
};

}  // namespace revenant::decode
