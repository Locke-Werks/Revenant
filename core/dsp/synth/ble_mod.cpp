#include "core/dsp/synth/ble_mod.h"

#include <cmath>
#include <numbers>

namespace revenant::siggen {

Expected<std::vector<std::uint8_t>> ble_advertising_pdu(decode::BlePduType type, bool tx_add,
                                                        bool rx_add,
                                                        std::span<const std::uint8_t> payload) {
    // Core Vol 6 Part B 2.3: Length 1 to 255.
    if (payload.empty() || payload.size() > 255) {
        return fail("BLE: advertising payload must be 1 to 255 octets");
    }
    std::vector<std::uint8_t> pdu;
    pdu.reserve(2 + payload.size());
    pdu.push_back(static_cast<std::uint8_t>(static_cast<unsigned>(type) | (tx_add ? 0x40U : 0U) |
                                            (rx_add ? 0x80U : 0U)));
    pdu.push_back(static_cast<std::uint8_t>(payload.size()));
    pdu.insert(pdu.end(), payload.begin(), payload.end());
    return pdu;
}

std::vector<std::uint8_t> ble_adv_payload(const decode::BleAddress& adv_address,
                                          std::span<const std::uint8_t> adv_data) {
    std::vector<std::uint8_t> p(adv_address.begin(), adv_address.end());
    p.insert(p.end(), adv_data.begin(), adv_data.end());
    return p;
}

Expected<std::vector<std::uint8_t>> ble_packet_bits(const BleModConfig& config,
                                                    std::span<const std::uint8_t> pdu) {
    if (pdu.size() < 2 || pdu.size() > decode::kBleMaxPduOctets) {
        return fail("BLE: PDU must be 2 to 258 octets");
    }
    std::vector<std::uint8_t> bits;
    const std::uint32_t aa = config.access_address;
    // Core Vol 6 Part B 2.1.1: alternating, first bit equal to the AA LSB.
    for (unsigned k = 0; k < 8; ++k) {
        bits.push_back(static_cast<std::uint8_t>((aa & 1U) ^ (k & 1U)));
    }
    for (unsigned k = 0; k < 32; ++k) {
        bits.push_back(static_cast<std::uint8_t>((aa >> k) & 1U));
    }
    std::vector<std::uint8_t> body;
    for (const std::uint8_t octet : pdu) {
        for (unsigned i = 0; i < 8; ++i) {
            body.push_back(static_cast<std::uint8_t>((octet >> i) & 1U));
        }
    }
    const std::uint32_t crc = decode::ble_crc24_bits(body, config.crc_init);
    for (unsigned i = 0; i < 24; ++i) {
        body.push_back(static_cast<std::uint8_t>((crc >> (23U - i)) & 1U));
    }
    // Core Vol 6 Part B 3.2: PDU and CRC are whitened, after the CRC.
    decode::ble_whiten(body, config.channel_index);
    bits.insert(bits.end(), body.begin(), body.end());
    return bits;
}

Expected<std::vector<dsp::Complex32>> ble_render(const BleModConfig& config,
                                                 std::span<const std::vector<std::uint8_t>> pdus) {
    if (config.sample_rate < 2'000'000) {
        return fail("BLE: sample rate must be at least 2 MS/s");
    }
    const auto rate = static_cast<double>(config.sample_rate);
    const double spb = rate / decode::kBleSymbolRate;

    // Core Vol 6 Part A 3.1: Gaussian, BT = 0.5, so a 3 dB bandwidth of
    // 500 kHz. The rectangular NRZ wave below supplies the other half of the
    // GFSK frequency pulse.
    const double bandwidth = 0.5 * decode::kBleSymbolRate;
    const double sigma = std::sqrt(std::log(2.0)) / (2.0 * std::numbers::pi * bandwidth) * rate;
    const auto half = static_cast<int>(std::ceil(3.0 * sigma));
    std::vector<double> g(static_cast<std::size_t>(2 * half + 1));
    double gsum = 0.0;
    for (int i = -half; i <= half; ++i) {
        const double v = std::exp(-0.5 * (i / sigma) * (i / sigma));
        g[static_cast<std::size_t>(i + half)] = v;
        gsum += v;
    }
    for (double& v : g) {
        v /= gsum;
    }

    // NRZ frequency wave, +1 for a one (Core Vol 6 Part A 3.1), 0 for silence.
    std::vector<double> nrz(config.lead_in_samples, 0.0);
    for (const auto& pdu : pdus) {
        auto bits = ble_packet_bits(config, pdu);
        if (!bits.has_value()) {
            return std::unexpected(bits.error());
        }
        const std::size_t n = static_cast<std::size_t>(std::ceil(static_cast<double>(bits->size()) * spb));
        const std::size_t origin = nrz.size();
        nrz.resize(origin + n, 0.0);
        for (std::size_t i = 0; i < n; ++i) {
            const auto k = static_cast<std::size_t>((static_cast<double>(i) + 0.5) / spb);
            if (k < bits->size()) {
                nrz[origin + i] = (*bits)[k] != 0U ? 1.0 : -1.0;
            }
        }
        nrz.resize(nrz.size() + config.gap_samples, 0.0);
    }

    const double deviation = config.modulation_index * decode::kBleSymbolRate / 2.0;
    std::vector<dsp::Complex32> out(nrz.size());
    double phase = 0.0;
    for (std::size_t n = 0; n < nrz.size(); ++n) {
        double f = 0.0;
        for (int i = -half; i <= half; ++i) {
            const auto j = static_cast<std::ptrdiff_t>(n) - i;
            if (j >= 0 && static_cast<std::size_t>(j) < nrz.size()) {
                f += g[static_cast<std::size_t>(i + half)] * nrz[static_cast<std::size_t>(j)];
            }
        }
        phase += 2.0 * std::numbers::pi * deviation * f / rate;
        phase = std::remainder(phase, 2.0 * std::numbers::pi);
        out[n] = dsp::Complex32(static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase)));
    }
    return out;
}

}  // namespace revenant::siggen
