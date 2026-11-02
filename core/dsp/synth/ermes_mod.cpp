#include "core/dsp/synth/ermes_mod.h"

#include <cmath>
#include <format>
#include <numbers>

#include "core/decode/dv_phy.h"

namespace revenant::siggen {

namespace {

using decode::ErmesCategory;

void push_bits(std::vector<std::uint8_t>& bits, std::uint32_t value, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        bits.push_back(static_cast<std::uint8_t>((value >> (count - 1 - i)) & 1U));
    }
}

std::vector<std::uint32_t> pack(const std::vector<std::uint8_t>& bits) {
    std::vector<std::uint32_t> words((bits.size() + 17) / 18, 0U);
    for (std::size_t i = 0; i < bits.size(); ++i) {
        words[i / 18] |= static_cast<std::uint32_t>(bits[i] & 1U) << (17 - i % 18);
    }
    return words;
}

std::uint8_t alpha_code(char c) {
    if (c == '\n') {
        return 0x0A;
    }
    if (c == '\r') {
        return 0x0D;
    }
    return static_cast<std::uint8_t>(c) & 0x7FU;
}

std::uint8_t numeric_code(char c) {
    // 133-2 Table B.1.
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint8_t>(c - '0');
    }
    switch (c) {
        case '/': return 0xA;
        case 'U': return 0xC;
        case '-': return 0xD;
        case '.': return 0xE;
        case '%': return 0xF;
        default: return 0xB;
    }
}

}  // namespace

Expected<std::vector<std::uint32_t>> ermes_message_words(const ErmesMessageSpec& m) {
    std::vector<std::uint8_t> header;
    const bool aii = m.link_counter.has_value();
    // 6.5.1: local address 22, message number 5, external bit, AII, VIF 7.
    push_bits(header, m.local_address & 0x3FFFFFU, 22);
    push_bits(header, m.message_number & 0x1FU, 5);
    push_bits(header, m.operator_identity ? 1U : 0U, 1);
    push_bits(header, aii ? 1U : 0U, 1);
    if (!aii) {
        // 6.5.1.1: RSVD, category, UMI, alert.
        push_bits(header, 0, 1);
        push_bits(header, static_cast<std::uint32_t>(m.category), 2);
        push_bits(header, m.urgent ? 1U : 0U, 1);
        push_bits(header, m.alert & 7U, 3);
    } else {
        // 6.5.1.2.1: AIT 001, AIN urgent bit then alert type.
        push_bits(header, 0b001, 3);
        push_bits(header, (m.urgent ? 8U : 0U) | (m.alert & 7U), 4);
    }
    std::vector<std::uint32_t> words = pack(header);

    std::vector<std::uint8_t> extra;
    if (m.operator_identity) {
        push_bits(extra, *m.operator_identity & 0x1FFFU, 13);
    }
    if (aii) {
        push_bits(extra, static_cast<std::uint32_t>(m.category), 2);
        push_bits(extra, *m.link_counter, 16);
    }
    for (std::uint32_t w : pack(extra)) {
        words.push_back(w);
    }

    // 6.5.3: data, then the terminator, then fill to a whole word.
    std::vector<std::uint8_t> data;
    switch (m.category) {
        case ErmesCategory::Tone:
            break;
        case ErmesCategory::Numeric:
            for (char c : m.text) {
                push_bits(data, numeric_code(c), 4);
            }
            while (data.size() % 18 != 0) {
                std::vector<std::uint8_t> space;
                push_bits(space, decode::kErmesNumericSpace, 4);
                for (std::size_t i = 0; i < 4 && data.size() % 18 != 0; ++i) {
                    data.push_back(space[i]);
                }
            }
            break;
        case ErmesCategory::Alphanumeric:
            for (char c : m.text) {
                push_bits(data, alpha_code(c), 7);
            }
            push_bits(data, decode::kErmesEom, 7);
            while (data.size() % 18 != 0) {
                std::vector<std::uint8_t> eom;
                push_bits(eom, decode::kErmesEom, 7);
                for (std::size_t i = 0; i < 7 && data.size() % 18 != 0; ++i) {
                    data.push_back(eom[i]);
                }
            }
            break;
        case ErmesCategory::Transparent:
            data = m.data;
            data.push_back(1);
            while (data.size() % 18 != 0) {
                data.push_back(0);
            }
            break;
    }
    for (std::uint32_t w : pack(data)) {
        words.push_back(w);
    }
    return words;
}

Expected<std::vector<std::uint32_t>> ermes_batch_codewords(const ErmesBatchSpec& b) {
    if (b.initial_addresses.size() > 139) {
        return fail("ERMES allows at most 139 initial addresses in a batch (133-4 5.4)");
    }
    const auto& s = b.system;
    std::vector<std::uint32_t> words = {decode::kErmesPreamble, decode::kErmesSyncWord};
    // 6.3.1, 36 bits over two codewords.
    std::uint64_t si = 0;
    si = (si << 7U) | (s.country & 0x7FU);
    si = (si << 3U) | (s.op & 7U);
    si = (si << 6U) | (s.paging_area & 0x3FU);
    si = (si << 1U) | (s.eti ? 1U : 0U);
    si = (si << 1U) | (s.bai ? 1U : 0U);
    si = (si << 5U) | (s.fsi & 0x1FU);
    si = (si << 6U) | (s.cycle & 0x3FU);
    si = (si << 3U) | (s.ssn & 7U);
    si = (si << 4U) | (s.batch & 0xFU);
    words.push_back(decode::ermes_encode(static_cast<std::uint32_t>(si >> 18U)));
    words.push_back(decode::ermes_encode(static_cast<std::uint32_t>(si & 0x3FFFFU)));
    words.push_back(decode::ermes_encode(((b.ssit & 0xFU) << 14U) | (b.ssif & 0x3FFFU)));
    for (std::uint32_t ia : b.initial_addresses) {
        words.push_back(decode::ermes_encode(ia));
    }
    const std::size_t start = decode::ermes_message_partition_start(b.initial_addresses.size());
    while (words.size() < start) {
        words.push_back(decode::kErmesApt);
    }

    std::vector<std::uint32_t> partition;
    for (const auto& m : b.messages) {
        partition.push_back(decode::kErmesDelimiter);
        auto mw = ermes_message_words(m);
        if (!mw) {
            return std::unexpected(mw.error());
        }
        for (std::uint32_t w : *mw) {
            partition.push_back(decode::ermes_encode(w));
        }
    }
    partition.push_back(decode::kErmesDelimiter);
    const std::size_t length =
        s.batch == 15 ? decode::kErmesLastBatchCodewords : decode::kErmesBatchCodewords;
    if (start + partition.size() > length) {
        return fail(std::format("ERMES batch overflows: {} message codewords after {} others",
                                partition.size(), start));
    }
    std::size_t total = partition.size();
    total = (total + 8) / 9 * 9;
    if (b.fill) {
        total = length - start;
    }
    while (partition.size() < total) {
        partition.push_back(decode::kErmesDelimiter);
    }
    words.insert(words.end(), partition.begin(), partition.end());
    return words;
}

Expected<std::vector<int>> ermes_batch_levels(const ErmesBatchSpec& b) {
    auto words = ermes_batch_codewords(b);
    if (!words) {
        return std::unexpected(words.error());
    }
    const std::size_t start = decode::ermes_message_partition_start(b.initial_addresses.size());
    std::vector<int> levels;
    auto push_word = [&](std::uint32_t w) {
        for (int s = 14; s >= 0; --s) {
            levels.push_back(decode::kErmesDibitToLevel[(w >> (2 * s)) & 3U]);
        }
    };
    for (std::size_t i = 0; i < start; ++i) {
        push_word((*words)[i]);
    }
    for (std::size_t i = start; i + 9 <= words->size(); i += 9) {
        const auto bits = decode::ermes_interleave(std::span<const std::uint32_t, 9>(&(*words)[i], 9));
        for (std::size_t k = 0; k < 270; k += 2) {
            levels.push_back(decode::kErmesDibitToLevel[(bits[k] << 1U) | bits[k + 1]]);
        }
    }
    return levels;
}

Expected<std::vector<Complex32>> ermes_render_levels(const ErmesModConfig& config,
                                                     std::span<const int> levels) {
    const double sps = static_cast<double>(config.rate) / decode::kErmesSymbolRate;
    if (sps < 4.0) {
        return fail("ermes_render_levels needs at least four samples a symbol");
    }
    // Premodulation filter; see the header for the deviation.
    const std::size_t taps = static_cast<std::size_t>(std::lround(4.0 * sps)) | 1U;
    auto response = [](double hz, const void*) -> double {
        const double x = hz / 3900.0;
        return std::exp(-0.5 * std::log(2.0) * x * x);
    };
    auto designed = decode::design_from_response(config.rate, taps, response, nullptr);
    if (!designed) {
        return std::unexpected(designed.error());
    }
    double sum = 0.0;
    for (float t : *designed) {
        sum += t;
    }
    for (float& t : *designed) {
        t = static_cast<float>(t / sum);
    }
    // NRZ hold at fractional symbol boundaries, then the filter. The filter's
    // delay is removed so sample n is the signal for stream time n.
    const auto n = static_cast<std::size_t>(std::ceil(static_cast<double>(levels.size()) * sps));
    const std::size_t delay = (taps - 1) / 2;
    std::vector<float> nrz(n + delay, 0.0F);
    for (std::size_t i = 0; i < n; ++i) {
        // Symbol k occupies [k*sps - sps/2, k*sps + sps/2), so its centre is at k*sps.
        const auto k = static_cast<std::size_t>(std::floor(static_cast<double>(i) / sps + 0.5));
        const int level = k < levels.size() ? levels[k] : 0;
        nrz[i] = static_cast<float>(level * decode::kErmesDeviationPerUnitHz);
    }
    std::vector<float> hz(nrz.size());
    if (auto st = decode::filter_real(nrz, *designed, hz); !st) {
        return std::unexpected(st.error());
    }
    std::vector<Complex32> out(n);
    double phase = 0.0;
    const double step = 2.0 * std::numbers::pi / static_cast<double>(config.rate);
    for (std::size_t i = 0; i < n; ++i) {
        phase = std::remainder(phase + step * hz[i + delay], 2.0 * std::numbers::pi);
        out[i] = Complex32{static_cast<float>(config.amplitude * std::cos(phase)),
                           static_cast<float>(config.amplitude * std::sin(phase))};
    }
    return out;
}

Expected<std::vector<Complex32>> ermes_render_batches(const ErmesModConfig& config,
                                                      std::span<const ErmesBatchSpec> batches) {
    std::vector<int> levels;
    for (const auto& b : batches) {
        auto l = ermes_batch_levels(b);
        if (!l) {
            return std::unexpected(l.error());
        }
        levels.insert(levels.end(), l->begin(), l->end());
    }
    return ermes_render_levels(config, levels);
}

}  // namespace revenant::siggen
