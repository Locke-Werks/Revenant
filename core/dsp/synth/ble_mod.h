// Bluetooth Low Energy advertising transmitter, LE 1M PHY.
//
// Written from the same clauses as core/decode/ble.h, Bluetooth Core
// Specification 6.1: Vol 6 Part A 3.1 for the GFSK, Vol 6 Part B 2.1, 2.3,
// 3.1.1 and 3.2 for the packet. Its job is the round trip in
// tests/decode/test_ble.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/ble.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

struct BleModConfig {
    dsp::SampleRate sample_rate = 2'400'000;
    unsigned channel_index = 37;
    std::uint32_t access_address = decode::kBleAdvertisingAccessAddress;
    std::uint32_t crc_init = decode::kBleAdvertisingCrcInit;

    // Core Vol 6 Part A 3.1: 0.45 to 0.55.
    double modulation_index = 0.5;

    // Unmodulated carrier before the first packet and between packets.
    std::size_t lead_in_samples = 200;
    std::size_t gap_samples = 400;
};

// Header plus payload for an advertising PDU: Core Vol 6 Part B Figure 2.5.
[[nodiscard]] Expected<std::vector<std::uint8_t>> ble_advertising_pdu(
    decode::BlePduType type, bool tx_add, bool rx_add, std::span<const std::uint8_t> payload);

// ADV_IND or ADV_NONCONN_IND payload: AdvA then AdvData.
[[nodiscard]] std::vector<std::uint8_t> ble_adv_payload(const decode::BleAddress& adv_address,
                                                        std::span<const std::uint8_t> adv_data);

// Preamble, Access Address, whitened PDU and CRC, one bit per element in
// transmission order.
[[nodiscard]] Expected<std::vector<std::uint8_t>> ble_packet_bits(const BleModConfig& config,
                                                                  std::span<const std::uint8_t> pdu);

// GFSK, BT 0.5, unit amplitude. Each PDU becomes one packet.
[[nodiscard]] Expected<std::vector<dsp::Complex32>> ble_render(
    const BleModConfig& config, std::span<const std::vector<std::uint8_t>> pdus);

}  // namespace revenant::siggen
