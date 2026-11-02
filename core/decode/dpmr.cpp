#include "core/decode/dpmr.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <numbers>

#include "core/decode/crc.h"

namespace revenant::decode {
namespace {

constexpr Crc kCrc7{CrcParams{.width = 7, .polynomial = kDpmrCrc7Polynomial}};
constexpr Crc kCrc8{CrcParams{.width = 8, .polynomial = kDpmrCrc8Polynomial}};

void push(std::vector<std::uint8_t>& bits, std::uint64_t value, std::size_t count) {
    for (std::size_t i = count; i-- > 0;) {
        bits.push_back(static_cast<std::uint8_t>((value >> i) & 1U));
    }
}

std::uint32_t read(std::span<const std::uint8_t> bits, std::size_t& at, std::size_t count) {
    const auto value = static_cast<std::uint32_t>(pack_bits_msb(bits, at, count));
    at += count;
    return value;
}

// 490 7.6 to 7.8: content and CRC to bytes, each byte to a Hamming word, the
// words concatenated most significant bit first.
std::vector<std::uint8_t> hamming_blocks(std::span<const std::uint8_t> bits) {
    std::vector<std::uint8_t> out;
    out.reserve(bits.size() / 8 * 12);
    for (std::size_t i = 0; i + 8 <= bits.size(); i += 8) {
        const auto byte = static_cast<std::uint8_t>(pack_bits_msb(bits, i, 8));
        push(out, dpmr_hamming_encode(byte), 12);
    }
    return out;
}

struct Unblocked {
    std::vector<std::uint8_t> bits;
    std::uint32_t corrected = 0;
};

Unblocked hamming_unblock(std::span<const std::uint8_t> coded) {
    Unblocked out;
    for (std::size_t i = 0; i + 12 <= coded.size(); i += 12) {
        const auto word = static_cast<std::uint16_t>(pack_bits_msb(coded, i, 12));
        const DpmrHammingDecode d = dpmr_hamming_decode(word);
        if (d.corrected) {
            ++out.corrected;
        }
        push(out.bits, d.data, 8);
    }
    return out;
}

std::vector<std::uint8_t> with_crc(std::vector<std::uint8_t> bits, unsigned width) {
    const std::uint8_t crc = width == 7 ? dpmr_crc7(bits) : dpmr_crc8(bits);
    push(bits, crc, width);
    return bits;
}

bool crc_matches(std::span<const std::uint8_t> bits, std::size_t content, unsigned width) {
    const auto content_bits = bits.first(content);
    const std::uint8_t expected = width == 7 ? dpmr_crc7(content_bits) : dpmr_crc8(content_bits);
    return pack_bits_msb(bits, content, width) == expected;
}

}  // namespace

std::uint16_t dpmr_hamming_encode(std::uint8_t data) {
    std::uint8_t parity = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        if (((data >> (7U - i)) & 1U) != 0U) {
            parity ^= kDpmrHammingParity[i];
        }
    }
    return static_cast<std::uint16_t>((static_cast<unsigned>(data) << 4U) | parity);
}

DpmrHammingDecode dpmr_hamming_decode(std::uint16_t word) {
    DpmrHammingDecode out;
    auto data = static_cast<std::uint8_t>((word >> 4U) & 0xFFU);
    const auto syndrome =
        static_cast<std::uint8_t>((dpmr_hamming_encode(data) ^ word) & 0x0FU);
    out.data = data;
    if (syndrome == 0) {
        return out;
    }
    // A check bit alone: its own column of the identity. A data bit: its row
    // of Table 7.1. All twelve are distinct and nonzero, so one error is
    // always located.
    if (std::popcount(static_cast<unsigned>(syndrome)) == 1) {
        out.corrected = true;
        return out;
    }
    for (std::size_t i = 0; i < 8; ++i) {
        if (kDpmrHammingParity[i] == syndrome) {
            out.data = static_cast<std::uint8_t>(data ^ (1U << (7U - i)));
            out.corrected = true;
            return out;
        }
    }
    out.failed = true;
    return out;
}

std::uint8_t dpmr_crc7(std::span<const std::uint8_t> bits) {
    return static_cast<std::uint8_t>(kCrc7.finish(kCrc7.update_bits(kCrc7.start(), bits)));
}

std::uint8_t dpmr_crc8(std::span<const std::uint8_t> bits) {
    return static_cast<std::uint8_t>(kCrc8.finish(kCrc8.update_bits(kCrc8.start(), bits)));
}

void dpmr_scramble(std::span<std::uint8_t> bits) {
    // 490 Figure 3: S9 to S1 shift towards S1, the output is S1 XOR data, and
    // S1 XOR S5 feeds S9. Bit 0 of the register is S1.
    unsigned state = kDpmrScramblerPreset;
    for (std::uint8_t& bit : bits) {
        const unsigned s1 = state & 1U;
        const unsigned s5 = (state >> 4U) & 1U;
        bit = static_cast<std::uint8_t>((bit ^ s1) & 1U);
        state = (state >> 1U) | ((s1 ^ s5) << 8U);
    }
}

std::vector<std::uint8_t> dpmr_interleave(std::span<const std::uint8_t> bits,
                                          std::size_t columns) {
    constexpr std::size_t rows = 12;
    std::vector<std::uint8_t> out(rows * columns, 0);
    if (bits.size() < out.size()) {
        return out;
    }
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < columns; ++c) {
            out[r * columns + c] = bits[c * rows + r];
        }
    }
    return out;
}

std::vector<std::uint8_t> dpmr_deinterleave(std::span<const std::uint8_t> bits,
                                            std::size_t columns) {
    constexpr std::size_t rows = 12;
    std::vector<std::uint8_t> out(rows * columns, 0);
    if (bits.size() < out.size()) {
        return out;
    }
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < columns; ++c) {
            out[c * rows + r] = bits[r * columns + c];
        }
    }
    return out;
}

std::uint32_t dpmr_channel_code_bits(std::uint16_t code) {
    std::uint32_t out = 0;
    for (unsigned i = 12; i-- > 0;) {
        out = (out << 2U) | (((code >> i) & 1U) != 0U ? 0b11U : 0b01U);
    }
    return out;
}

std::optional<std::uint16_t> dpmr_channel_code_value(std::uint32_t bits) {
    std::uint16_t out = 0;
    for (unsigned i = 12; i-- > 0;) {
        const unsigned dibit = (bits >> (2U * i)) & 0b11U;
        if (dibit != 0b01U && dibit != 0b11U) {
            return std::nullopt;
        }
        out = static_cast<std::uint16_t>((out << 1U) | (dibit == 0b11U ? 1U : 0U));
    }
    return out;
}

std::vector<float> dpmr_word_symbols(std::uint64_t word, std::size_t bits) {
    std::vector<float> out;
    out.reserve(bits / 2);
    for (std::size_t i = bits; i >= 2; i -= 2) {
        const auto dibit = static_cast<std::size_t>((word >> (i - 2)) & 0b11U);
        out.push_back(static_cast<float>(kDpmrDibitToSymbol[dibit]));
    }
    return out;
}

std::vector<std::uint8_t> dpmr_encode_cch(const DpmrCch& cch) {
    // 490 Table 5.1a and clause 9.1: FN, ID, M, V, F, reserved, SLD: 41 bits.
    std::vector<std::uint8_t> bits;
    push(bits, cch.frame_number, 2);
    push(bits, cch.id_part, 12);
    push(bits, cch.mode, 3);
    push(bits, cch.version, 2);
    push(bits, cch.format, 2);
    push(bits, cch.reserved, 2);
    push(bits, cch.slow_data, 18);
    auto coded = dpmr_interleave(hamming_blocks(with_crc(std::move(bits), 7)), 6);
    dpmr_scramble(coded);
    return coded;
}

DpmrCch dpmr_decode_cch(std::span<const std::uint8_t> bits) {
    DpmrCch out;
    if (bits.size() < kDpmrCchBits) {
        return out;
    }
    std::vector<std::uint8_t> work(bits.begin(), bits.begin() + kDpmrCchBits);
    dpmr_scramble(work);
    const Unblocked plain = hamming_unblock(dpmr_deinterleave(work, 6));
    std::size_t at = 0;
    out.frame_number = static_cast<std::uint8_t>(read(plain.bits, at, 2));
    out.id_part = static_cast<std::uint16_t>(read(plain.bits, at, 12));
    out.mode = static_cast<std::uint8_t>(read(plain.bits, at, 3));
    out.version = static_cast<std::uint8_t>(read(plain.bits, at, 2));
    out.format = static_cast<std::uint8_t>(read(plain.bits, at, 2));
    out.reserved = static_cast<std::uint8_t>(read(plain.bits, at, 2));
    out.slow_data = read(plain.bits, at, 18);
    out.crc_valid = crc_matches(plain.bits, 41, 7);
    out.corrected_words = plain.corrected;
    return out;
}

std::vector<std::uint8_t> dpmr_encode_hi(const DpmrHeaderInfo& hi) {
    // 490 Table 5.2 and clause 9.5: HT, called ID, own ID, M, V, F, reserved,
    // CI: 72 bits.
    std::vector<std::uint8_t> bits;
    push(bits, hi.header_type, 4);
    push(bits, hi.called_id, 24);
    push(bits, hi.own_id, 24);
    push(bits, hi.mode, 3);
    push(bits, hi.version, 2);
    push(bits, hi.format, 2);
    push(bits, hi.reserved, 2);
    push(bits, hi.call_info, 11);
    auto coded = dpmr_interleave(hamming_blocks(with_crc(std::move(bits), 8)), 10);
    dpmr_scramble(coded);
    return coded;
}

DpmrHeaderInfo dpmr_decode_hi(std::span<const std::uint8_t> bits) {
    DpmrHeaderInfo out;
    if (bits.size() < kDpmrHiBits) {
        return out;
    }
    std::vector<std::uint8_t> work(bits.begin(), bits.begin() + kDpmrHiBits);
    dpmr_scramble(work);
    const Unblocked plain = hamming_unblock(dpmr_deinterleave(work, 10));
    std::size_t at = 0;
    out.header_type = static_cast<std::uint8_t>(read(plain.bits, at, 4));
    out.called_id = read(plain.bits, at, 24);
    out.own_id = read(plain.bits, at, 24);
    out.mode = static_cast<std::uint8_t>(read(plain.bits, at, 3));
    out.version = static_cast<std::uint8_t>(read(plain.bits, at, 2));
    out.format = static_cast<std::uint8_t>(read(plain.bits, at, 2));
    out.reserved = static_cast<std::uint8_t>(read(plain.bits, at, 2));
    out.call_info = static_cast<std::uint16_t>(read(plain.bits, at, 11));
    out.crc_valid = crc_matches(plain.bits, 72, 8);
    out.corrected_words = plain.corrected;
    return out;
}

std::vector<std::uint8_t> dpmr_encode_end(const DpmrEnd& end) {
    // 490 Table 5.3 and clause 9.6: ET, ARQ, WAIT, STAT, reserved: 17 bits.
    std::vector<std::uint8_t> bits;
    push(bits, end.end_type, 2);
    push(bits, end.arq, 2);
    push(bits, end.wait, 4);
    push(bits, end.status, 5);
    push(bits, end.reserved, 4);
    const auto block = hamming_blocks(with_crc(std::move(bits), 7));
    // 490 9.6 and Figure 11: the 36 bits twice, each copy scrambled from the
    // preset.
    std::vector<std::uint8_t> out;
    for (int copy = 0; copy < 2; ++copy) {
        std::vector<std::uint8_t> half(block);
        dpmr_scramble(half);
        out.insert(out.end(), half.begin(), half.end());
    }
    return out;
}

DpmrEnd dpmr_decode_end(std::span<const std::uint8_t> bits) {
    DpmrEnd out;
    if (bits.size() < 72) {
        return out;
    }
    for (std::size_t copy = 0; copy < 2; ++copy) {
        std::vector<std::uint8_t> half(bits.begin() + static_cast<std::ptrdiff_t>(copy * 36),
                                       bits.begin() + static_cast<std::ptrdiff_t>(copy * 36 + 36));
        dpmr_scramble(half);
        const Unblocked plain = hamming_unblock(half);
        DpmrEnd candidate;
        std::size_t at = 0;
        candidate.end_type = static_cast<std::uint8_t>(read(plain.bits, at, 2));
        candidate.arq = static_cast<std::uint8_t>(read(plain.bits, at, 2));
        candidate.wait = static_cast<std::uint8_t>(read(plain.bits, at, 4));
        candidate.status = static_cast<std::uint8_t>(read(plain.bits, at, 5));
        candidate.reserved = static_cast<std::uint8_t>(read(plain.bits, at, 4));
        candidate.crc_valid = crc_matches(plain.bits, 17, 7);
        candidate.corrected_words = plain.corrected;
        if (copy == 0 || candidate.crc_valid) {
            out = candidate;
        }
        if (out.crc_valid) {
            break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

Expected<Dpmr> Dpmr::create(const DpmrConfig& config) {
    if (config.rate <= 0) {
        return fail(std::format("dPMR needs a positive sample rate; got {}", config.rate));
    }
    const double sps = static_cast<double>(config.rate) / kDpmrSymbolRate;
    if (sps < 2.0) {
        return fail(std::format(
            "dPMR is 2400 symbols per second (TS 102 490 12.2.1), so {} Hz gives {:.3f} "
            "samples per symbol, below the two the timing recovery needs",
            config.rate, sps));
    }
    const std::size_t count =
        static_cast<std::size_t>(std::ceil(static_cast<double>(kDpmrRrcSpanSymbols) * sps)) | 1U;
    auto taps = design_rrc(config.rate, kDpmrSymbolRate, kDpmrRrcRollOff, count);
    if (!taps) {
        return std::unexpected(with_context(taps.error(), "designing the dPMR 12.2.2.2 filter"));
    }
    double tap_sum = 0.0;
    for (const float tap : *taps) {
        tap_sum += static_cast<double>(tap);
    }
    auto discriminator = FmDiscriminator::create(config.rate);
    if (!discriminator) {
        return std::unexpected(discriminator.error());
    }
    struct Edges {
        double pass;
        double stop;
    };
    static constexpr Edges kEdges{kDpmrChannelPassHz, kDpmrChannelStopHz};
    const auto channel_response = [](double hertz, const void* context) {
        const auto* edges = static_cast<const Edges*>(context);
        const double f = std::abs(hertz);
        if (f <= edges->pass) {
            return 1.0;
        }
        if (f >= edges->stop) {
            return 0.0;
        }
        return 0.5 * (1.0 + std::cos(std::numbers::pi * (f - edges->pass) /
                                     (edges->stop - edges->pass)));
    };
    const std::size_t channel_count =
        static_cast<std::size_t>(std::ceil(static_cast<double>(kDpmrChannelSpanSymbols) * sps)) |
        1U;
    auto channel_taps =
        design_from_response(config.rate, channel_count, channel_response, &kEdges);
    if (!channel_taps) {
        return std::unexpected(with_context(channel_taps.error(), "designing the dPMR channel filter"));
    }
    auto channel = ComplexFir::create(std::move(*channel_taps));
    if (!channel) {
        return std::unexpected(channel.error());
    }
    Dpmr decoder;
    decoder.config_ = config;
    decoder.channel_ = std::move(*channel);
    decoder.discriminator_ = *discriminator;
    // The transmit filter has unit gain at zero hertz, so a run of symbol s
    // deviates s * 350 Hz; through design_rrc's receive taps the cascade puts
    // it at s * 350 * sps / tap_sum, as core/decode/m17.cpp works through for
    // the same filter design. Dividing that out puts the levels at the
    // integers of 490 Table 12.1.
    decoder.scale_ = tap_sum / (kDpmrDeviationPerUnitHz * sps);
    decoder.delay_ = static_cast<double>(taps->size() - 1 + channel_count - 1) / 2.0;
    auto filter = RealFir::create(std::move(*taps));
    if (!filter) {
        return std::unexpected(filter.error());
    }
    decoder.filter_ = std::move(*filter);
    SymbolSyncConfig timing;
    timing.rate = config.rate;
    timing.symbol_rate = kDpmrSymbolRate;
    auto sync = SymbolSync::create(timing);
    if (!sync) {
        return std::unexpected(with_context(sync.error(), "creating the dPMR timing"));
    }
    decoder.sync_ = std::move(*sync);
    return decoder;
}

void Dpmr::reset() {
    channel_.reset();
    discriminator_.reset();
    filter_.reset();
    sync_.reset();
    symbols_.clear();
    positions_.clear();
    cursor_ = 0;
    in_transmission_ = false;
    inverted_ = false;
    next_frame_ = 0;
    missed_ = 0;
    channel_code_.reset();
    header_voice_ = false;
    id_parts_ = {};
    last_symbols_.clear();
}

Status Dpmr::process(ConstComplexSpan samples, std::vector<DpmrFrame>& out) {
    last_symbols_.clear();
    if (samples.empty()) {
        return {};
    }
    channelled_.resize(samples.size());
    if (auto status = channel_.process(samples, channelled_); !status) {
        return status;
    }
    scratch_.resize(samples.size());
    if (auto status = discriminator_.process(channelled_, scratch_); !status) {
        return status;
    }
    std::vector<float> filtered(samples.size());
    if (auto status = filter_.process(scratch_, filtered); !status) {
        return status;
    }
    shaped_.resize(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        shaped_[i] = Complex32{static_cast<float>(filtered[i] * scale_), 0.0F};
    }
    recovered_.clear();
    sync_.process(shaped_, recovered_);
    for (const RecoveredSymbol& symbol : recovered_) {
        symbols_.push_back(symbol.value.real());
        const double centre = symbol.position - delay_;
        positions_.push_back(centre > 0.0 ? static_cast<SampleIndex>(std::llround(centre)) : 0);
        last_symbols_.push_back(symbol.value.real());
    }

    search(out);

    if (cursor_ > 0) {
        symbols_.erase(symbols_.begin(), symbols_.begin() + static_cast<std::ptrdiff_t>(cursor_));
        positions_.erase(positions_.begin(),
                         positions_.begin() + static_cast<std::ptrdiff_t>(cursor_));
        cursor_ = 0;
    }
    return {};
}

std::vector<std::uint8_t> Dpmr::slice(std::size_t offset, std::size_t count,
                                      std::span<const float> known, std::size_t known_offset,
                                      float sign) const {
    // Least squares on the known symbols takes out the deviation scale and the
    // carrier offset; `sign` is the receive polarity, and the fit is made
    // against the pattern as it arrives so the gain comes out positive.
    std::vector<float> arriving(known.begin(), known.end());
    for (float& value : arriving) {
        value *= sign;
    }
    LevelFit fit;
    if (auto measured = fit_levels(symbols_, arriving, known_offset);
        measured && measured->gain > 0.2) {
        fit = *measured;
    }
    std::vector<std::uint8_t> bits;
    bits.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        const double value =
            static_cast<double>(sign) * (static_cast<double>(symbols_[offset + i]) - fit.level) /
            fit.gain;
        // 490 Table 12.1, nearest level.
        unsigned dibit = 0;
        if (value > 2.0) {
            dibit = 0b01;
        } else if (value > 0.0) {
            dibit = 0b00;
        } else if (value > -2.0) {
            dibit = 0b10;
        } else {
            dibit = 0b11;
        }
        bits.push_back(static_cast<std::uint8_t>(dibit >> 1U));
        bits.push_back(static_cast<std::uint8_t>(dibit & 1U));
    }
    return bits;
}

bool Dpmr::payload_frame(std::size_t offset, std::span<const float> sync, double score,
                         DpmrFrame& frame) {
    const float sign = inverted_ ? -1.0F : 1.0F;
    frame.kind = DpmrFrameKind::Payload;
    frame.first_sample = positions_[offset];
    frame.sync_score = score;
    frame.inverted = inverted_;

    // Frames 2 and 4 carry the channel code, all outer symbols, so their own
    // signs make a usable calibration pattern when the code is not yet known.
    std::vector<float> known(sync.begin(), sync.end());
    if (known.empty()) {
        for (std::size_t i = 0; i < kDpmrSyncSymbols; ++i) {
            known.push_back(sign * symbols_[offset + i] >= 0.0F ? 3.0F : -3.0F);
        }
    }
    const auto cch_bits = slice(offset + kDpmrSyncSymbols, kDpmrCchBits / 2, known, offset, sign);
    DpmrCch cch = dpmr_decode_cch(cch_bits);
    const auto tch_bits = slice(offset + kDpmrSyncSymbols + kDpmrCchBits / 2,
                                kDpmrTchBlocks * kDpmrTchBits / 2, known, offset, sign);
    for (std::size_t b = 0; b < kDpmrTchBlocks; ++b) {
        for (std::size_t i = 0; i < kDpmrTchBits / 8; ++i) {
            frame.tch[b][i] =
                static_cast<std::uint8_t>(pack_bits_msb(tch_bits, b * kDpmrTchBits + i * 8, 8));
        }
    }
    if ((next_frame_ % 2) == 1) {
        const auto code_bits = slice(offset, kDpmrSyncSymbols, known, offset, sign);
        if (auto code = dpmr_channel_code_value(
                static_cast<std::uint32_t>(pack_bits_msb(code_bits, 0, 24)))) {
            frame.channel_code = *code;
            if (cch.crc_valid) {
                channel_code_ = *code;
            }
        }
    }
    frame.voice = cch.crc_valid ? cch.mode <= 1 : header_voice_;
    if (cch.crc_valid) {
        next_frame_ = cch.frame_number;
        id_parts_[cch.frame_number] = cch.id_part;
        if (cch.frame_number == 3) {
            if (id_parts_[0] && id_parts_[1] && id_parts_[2] && id_parts_[3]) {
                // 490 9.1: FN 00 and 01 the called ID, 10 and 11 the own ID.
                frame.called_id = (static_cast<std::uint32_t>(*id_parts_[0]) << 12U) |
                                  *id_parts_[1];
                frame.own_id = (static_cast<std::uint32_t>(*id_parts_[2]) << 12U) | *id_parts_[3];
            }
            id_parts_ = {};
        }
    }
    const bool good = cch.crc_valid;
    frame.cch = cch;
    return good;
}

bool Dpmr::try_header(std::size_t i, double score, std::vector<DpmrFrame>& out) {
    // FS4 is FS1 with every symbol negated (658 6.1 note), so a hit is FS1 at
    // the polarity the score says or FS4 at the other. The HI CRC decides.
    const auto fs1 = dpmr_word_symbols(kDpmrFs1, 48);
    const auto fs4 = dpmr_word_symbols(kDpmrFs4, 48);
    const float hit_sign = score >= 0.0 ? 1.0F : -1.0F;
    for (int candidate = 0; candidate < 2; ++candidate) {
        const bool packet = candidate == 1;
        const float sign = packet ? -hit_sign : hit_sign;
        const auto& pattern = packet ? fs4 : fs1;
        const std::size_t hi0 = i + kDpmrLongSyncSymbols;
        const std::size_t cc = hi0 + kDpmrHiBits / 2;
        const std::size_t hi1 = cc + kDpmrSyncSymbols;
        DpmrHeaderInfo info = dpmr_decode_hi(slice(hi0, kDpmrHiBits / 2, pattern, i, sign));
        // HI1 is calibrated on the channel code beside it, whose symbols are
        // all outer levels, rather than on FS1 a hundred symbols back. FS1
        // follows the 490 6.1.6 preamble, whose +3 +3 -3 -3 period carries
        // little of the symbol-rate line the timing recovery estimates from,
        // so FS1 and HI0 are the worst-timed symbols of the header; at 20 dB
        // on 2026-11-02 both failed together when calibrated on FS1.
        std::vector<float> code_signs;
        for (std::size_t k = 0; k < kDpmrSyncSymbols; ++k) {
            code_signs.push_back(sign * symbols_[cc + k] >= 0.0F ? 3.0F : -3.0F);
        }
        if (!info.crc_valid) {
            info = dpmr_decode_hi(slice(hi1, kDpmrHiBits / 2, code_signs, cc, sign));
        }
        if (!info.crc_valid) {
            continue;
        }
        DpmrFrame frame;
        frame.kind = packet ? DpmrFrameKind::PacketHeader : DpmrFrameKind::Header;
        frame.first_sample = positions_[i];
        frame.sync_score = std::abs(score);
        frame.inverted = sign < 0.0F;
        frame.header = info;
        const auto code_bits = slice(cc, kDpmrSyncSymbols, pattern, i, sign);
        frame.channel_code = dpmr_channel_code_value(
            static_cast<std::uint32_t>(pack_bits_msb(code_bits, 0, 24)));
        frame.voice = info.mode <= 1;
        out.push_back(frame);

        in_transmission_ = true;
        inverted_ = sign < 0.0F;
        next_frame_ = 0;
        missed_ = 0;
        channel_code_ = frame.channel_code;
        header_voice_ = frame.voice;
        id_parts_ = {};
        cursor_ = i + kDpmrHeaderSymbols;
        return true;
    }
    return false;
}

bool Dpmr::try_late_entry(std::size_t i, double score, std::vector<DpmrFrame>& out) {
    const auto fs2 = dpmr_word_symbols(kDpmrFs2, 24);
    const std::size_t confirm = i + 2 * kDpmrFrameSymbols;
    const float hit_sign = score < 0.0 ? -1.0F : 1.0F;
    if (hit_sign * centred_correlation_at(symbols_, fs2, confirm) <
        config_.late_entry_threshold) {
        return false;
    }
    const DpmrCch second = dpmr_decode_cch(
        slice(confirm + kDpmrSyncSymbols, kDpmrCchBits / 2, fs2, confirm, hit_sign));
    if (!second.crc_valid) {
        return false;
    }
    const bool saved_inverted = inverted_;
    const std::size_t saved_next = next_frame_;
    inverted_ = score < 0.0;
    next_frame_ = 0;
    DpmrFrame frame;
    std::vector<float> sync(fs2.begin(), fs2.end());
    if (!payload_frame(i, sync, std::abs(score), frame) || (frame.cch->frame_number % 2) != 0 ||
        second.frame_number != (frame.cch->frame_number + 2) % 4) {
        inverted_ = saved_inverted;
        next_frame_ = saved_next;
        id_parts_ = {};
        return false;
    }
    out.push_back(frame);
    in_transmission_ = true;
    missed_ = 0;
    header_voice_ = frame.voice;
    next_frame_ = (next_frame_ + 1) % 4;
    cursor_ = i + kDpmrFrameSymbols;
    return true;
}

void Dpmr::search(std::vector<DpmrFrame>& out) {
    const auto fs1 = dpmr_word_symbols(kDpmrFs1, 48);
    const auto fs2 = dpmr_word_symbols(kDpmrFs2, 24);
    const auto fs3 = dpmr_word_symbols(kDpmrFs3, 24);
    while (true) {
        if (in_transmission_) {
            const float sign = inverted_ ? -1.0F : 1.0F;
            if (cursor_ + kDpmrEndSymbols > symbols_.size()) {
                return;
            }
            // 490 4.2.3: a call is whole superframes then the End frame, and
            // a header may be followed by the End directly.
            if (next_frame_ == 0) {
                const double end_score =
                    sign * centred_correlation_at(symbols_, fs3, cursor_);
                if (end_score >= config_.tracking_threshold) {
                    DpmrFrame frame;
                    frame.kind = DpmrFrameKind::End;
                    frame.first_sample = positions_[cursor_];
                    frame.sync_score = end_score;
                    frame.inverted = inverted_;
                    frame.end = dpmr_decode_end(
                        slice(cursor_ + kDpmrSyncSymbols, 36, fs3, cursor_, sign));
                    out.push_back(frame);
                    in_transmission_ = false;
                    cursor_ += kDpmrEndSymbols;
                    continue;
                }
            }
            if (cursor_ + kDpmrFrameSymbols > symbols_.size()) {
                return;
            }
            std::vector<float> sync;
            if ((next_frame_ % 2) == 0) {
                sync = fs2;
            } else if (channel_code_) {
                sync = dpmr_word_symbols(dpmr_channel_code_bits(*channel_code_), 24);
            }
            const double score =
                sync.empty() ? 0.0 : sign * centred_correlation_at(symbols_, sync, cursor_);
            DpmrFrame frame;
            const bool good = payload_frame(cursor_, sync, score, frame);
            if (good || score >= config_.tracking_threshold) {
                out.push_back(frame);
                missed_ = 0;
            } else if (++missed_ >= 2) {
                in_transmission_ = false;
                id_parts_ = {};
                // Search again from just after the first frame that was
                // missed, in case it was a new transmission's header.
                cursor_ = cursor_ + 1 >= kDpmrFrameSymbols ? cursor_ + 1 - kDpmrFrameSymbols : 0;
                continue;
            }
            next_frame_ = (next_frame_ + 1) % 4;
            cursor_ += kDpmrFrameSymbols;
            continue;
        }

        if (cursor_ + kDpmrFrameSymbols > symbols_.size()) {
            return;
        }
        const double header = centred_correlation_at(symbols_, fs1, cursor_);
        if (std::abs(header) >= config_.header_threshold && try_header(cursor_, header, out)) {
            continue;
        }
        if (cursor_ + 3 * kDpmrFrameSymbols > symbols_.size()) {
            return;
        }
        const double late = centred_correlation_at(symbols_, fs2, cursor_);
        if (std::abs(late) >= config_.late_entry_threshold &&
            try_late_entry(cursor_, late, out)) {
            continue;
        }
        ++cursor_;
    }
}

}  // namespace revenant::decode
