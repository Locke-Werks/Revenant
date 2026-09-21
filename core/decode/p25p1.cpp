#include "core/decode/p25p1.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

#include "core/decode/dv_codes.h"

namespace revenant::decode {
namespace {

constexpr double kPi = std::numbers::pi;

// TIA-102.BAAA-A clause 9.6, the Integrate and Dump Filter the QPSK
// demodulator applies to the frequency detector's output:
//
//     |D(f)| = sin(pi*f/4800) / (pi*f/4800)
//
// WHY THE SPECIFIED BAND IS NOT TOUCHED
//
// The whole point of this filter is that it cancels the transmitter's shaping
// filter. Clause 9.4 gives |P(f)| = (pi*f/4800) / sin(pi*f/4800), which is
// exactly the reciprocal of this, so the cascade H(f)*P(f)*D(f) is H(f), the
// clause 9.3 raised cosine, which is Nyquist and therefore free of
// intersymbol interference at the symbol instants. Any deviation from the
// clause inside 2880 Hz breaks that cancellation.
//
// It was broken here first, with a raised-cosine taper applied from 1920 Hz
// up on the reasoning that the receive filter's transition should match the
// transmit filter's. It should not: the transmit filter's transition is
// already in H(f) and applying a second one squeezes the constellation, which
// showed up as the inner and outer symbol levels closing from 1 and 3 towards
// 1.2 and 2.8. The eye stays open, the frame sync still correlates, and the
// Network Identifier starts taking corrections it should not need.
//
// WHAT IS AN ENGINEERING CHOICE HERE
//
// Only the region above 2880 Hz. The clause leaves it unspecified because the
// transmit filter is zero there, and a receiver cannot rely on a transmitter
// for its own noise bandwidth. The taper below runs from the specified band
// edge out to 4320 Hz, which rejects out-of-band noise without touching a
// hertz the clause specifies.
double p25_receive_response(double hertz, const void* /*context*/) {
    constexpr double kTaperEndHz = 4320.0;
    if (hertz >= kTaperEndHz) {
        return 0.0;
    }
    const double x = kPi * hertz / kP25SymbolRate;
    const double sinc = (std::abs(x) < 1e-12) ? 1.0 : std::sin(x) / x;
    if (hertz <= kP25NyquistStopHz) {
        return sinc;
    }
    const double t = (hertz - kP25NyquistStopHz) / (kTaperEndHz - kP25NyquistStopHz);
    return sinc * 0.5 * (1.0 + std::cos(kPi * t));
}

// Nearest of the four Table 9-1 levels.
std::uint8_t slice_dibit(float value) {
    const double v = static_cast<double>(value);
    if (v >= 2.0) {
        return kP25SymbolToDibit[3];  // +3
    }
    if (v >= 0.0) {
        return kP25SymbolToDibit[2];  // +1
    }
    if (v >= -2.0) {
        return kP25SymbolToDibit[1];  // -1
    }
    return kP25SymbolToDibit[0];  // -3
}

}  // namespace

std::string_view p25_duid_name(std::uint8_t duid) {
    switch (static_cast<P25Duid>(duid)) {
        case P25Duid::HeaderDataUnit: return "hdu";
        case P25Duid::TerminatorWithoutLinkControl: return "tdu";
        case P25Duid::LogicalLinkDataUnit1: return "ldu1";
        case P25Duid::LogicalLinkDataUnit2: return "ldu2";
        case P25Duid::PacketDataUnit: return "pdu";
        case P25Duid::TerminatorWithLinkControl: return "tdulc";
    }
    // Table 8-4: "The remaining 10 values not given in Table 8-4 are reserved
    // for use in trunking or other systems."
    return "reserved";
}

std::size_t p25_data_unit_symbols(std::uint8_t duid) {
    switch (static_cast<P25Duid>(duid)) {
        case P25Duid::HeaderDataUnit: return kP25HduTotalSymbols;
        case P25Duid::TerminatorWithoutLinkControl: return kP25SimpleTerminatorSymbols;
        case P25Duid::LogicalLinkDataUnit1:
        case P25Duid::LogicalLinkDataUnit2: return kP25LduSymbols;
        case P25Duid::TerminatorWithLinkControl: return kP25TerminatorWithLcSymbols;

        // Clause 6 sizes a packet data unit from its own header, so there is
        // no constant for it and stepping past it needs the header decoded.
        // Zero here means the caller steps past the sync word instead, which
        // costs a redundant search and finds the next unit either way.
        case P25Duid::PacketDataUnit: return 0;
    }
    return 0;
}

std::array<float, kP25FrameSyncSymbols> p25_frame_sync_pattern() {
    std::array<float, kP25FrameSyncSymbols> pattern{};
    for (std::size_t i = 0; i < kP25FrameSyncSymbols; ++i) {
        const auto shift = static_cast<unsigned>(kP25FrameSyncBits - 2 - 2 * i);
        const auto dibit = static_cast<std::size_t>((kP25FrameSync >> shift) & 0x3ULL);
        pattern[i] = static_cast<float>(kP25DibitToSymbol[dibit]);
    }
    return pattern;
}

std::vector<float> p25_strip_status_symbols(std::span<const float> symbols) {
    std::vector<float> out;
    out.reserve(symbols.size());
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        if (i >= kP25FirstStatusSymbol &&
            (i - kP25FirstStatusSymbol) % kP25StatusSymbolInterval == 0) {
            continue;
        }
        out.push_back(symbols[i]);
    }
    return out;
}

Expected<P25Phase1> P25Phase1::create(const P25Config& config) {
    if (config.rate <= 0) {
        return fail(std::format("P25Phase1 needs a positive sample rate; got {}", config.rate));
    }
    const double samples_per_symbol = static_cast<double>(config.rate) / kP25SymbolRate;
    if (samples_per_symbol < 2.0) {
        return fail(std::format(
            "P25 Phase 1 is 4800 symbols per second (TIA-102.BAAA-A clause 9.2), so a "
            "rate of {} Hz gives {:.3f} samples per symbol and the timing recovery has no "
            "midpoint to look at. 9600 Hz is the floor and 48000 is the usual choice",
            config.rate, samples_per_symbol));
    }
    if (config.sync_threshold <= 0.0 || config.sync_threshold > 1.0) {
        return fail(std::format("P25Phase1 needs a sync threshold in (0, 1]; got {}",
                                config.sync_threshold));
    }

    auto taps = design_from_response(config.rate, config.filter_taps, p25_receive_response,
                                     nullptr);
    if (!taps) {
        return std::unexpected(with_context(taps.error(),
                                            "designing the clause 9.6 integrate and dump filter"));
    }

    SymbolSyncConfig sync_config;
    sync_config.rate = config.rate;
    sync_config.symbol_rate = kP25SymbolRate;
    auto sync = SymbolSync::create(sync_config);
    if (!sync) {
        return std::unexpected(
            with_context(sync.error(), "creating the P25 symbol timing recovery"));
    }

    P25Phase1 decoder;
    decoder.config_ = config;
    decoder.filter_taps_ = std::move(*taps);
    decoder.sync_ = std::move(*sync);
    return decoder;
}

void P25Phase1::reset() {
    sync_.reset();
    symbols_.clear();
    last_symbols_.clear();
    consumed_ = 0;
}

Status P25Phase1::decode_at(std::size_t offset, bool inverted, double score,
                            P25Frame& out) const {
    // A data unit is at most kP25HduTotalSymbols long, and the shortest one
    // that carries a NID is the sync word plus the NID plus the status symbol
    // that falls inside them.
    const std::size_t minimum = kP25FrameSyncSymbols + kP25NidSymbols + 1;
    if (offset + minimum > symbols_.size()) {
        return fail("not enough symbols for a Network Identifier");
    }

    const std::size_t available = std::min(kP25HduTotalSymbols, symbols_.size() - offset);
    std::vector<float> raw(symbols_.begin() + static_cast<std::ptrdiff_t>(offset),
                           symbols_.begin() + static_cast<std::ptrdiff_t>(offset + available));
    if (inverted) {
        for (float& value : raw) {
            value = -value;
        }
    }

    const std::vector<float> information = p25_strip_status_symbols(raw);
    if (information.size() < kP25FrameSyncSymbols + kP25NidSymbols) {
        return fail("not enough information symbols for a Network Identifier");
    }

    out.dibits.clear();
    out.dibits.reserve(information.size());
    for (const float value : information) {
        out.dibits.push_back(slice_dibit(value));
    }

    // Clause 8.5: the 64 NID bits follow the 48 sync bits.
    std::array<std::uint8_t, kP25NidBits> nid_bits{};
    for (std::size_t i = 0; i < kP25NidSymbols; ++i) {
        const std::uint8_t dibit = out.dibits[kP25FrameSyncSymbols + i];
        nid_bits[2 * i] = static_cast<std::uint8_t>((dibit >> 1U) & 1U);
        nid_bits[2 * i + 1] = static_cast<std::uint8_t>(dibit & 1U);
    }
    auto nid = p25_nid_decode(nid_bits);
    if (!nid) {
        return std::unexpected(with_context(nid.error(), "decoding the P25 Network Identifier"));
    }

    out.nid.network_access_code = nid->nac;
    out.nid.duid = nid->duid;
    out.nid.corrected_bits = nid->corrected_bits;
    out.first_symbol = offset;
    out.sync_score = score;
    out.inverted = inverted;
    out.header.reset();

    if (static_cast<P25Duid>(nid->duid) != P25Duid::HeaderDataUnit) {
        return {};
    }

    // Clause 10.2. After the sync word and the NID come 36 Golay (18,6,8)
    // code words, each carried as three symbols of information followed by six
    // of parity, then five null symbols.
    const std::size_t body = kP25FrameSyncSymbols + kP25NidSymbols;
    const std::size_t needed = body + kP25HduGolayWords * kP25HduSymbolsPerGolayWord;
    if (information.size() < needed) {
        // Not a failure of the frame: the NID decoded and is worth reporting.
        // Only the header is missing, which is what the optional is for.
        return {};
    }

    P25Header header;
    // 36 hexbits of 6 bits each, of which the first 20 are the information the
    // Reed-Solomon (36,20,17) code protects and the last 16 are its parity.
    std::array<std::uint8_t, kP25HduGolayWords> hexbits{};
    for (std::size_t word = 0; word < kP25HduGolayWords; ++word) {
        std::uint32_t code_word = 0;
        for (std::size_t s = 0; s < kP25HduSymbolsPerGolayWord; ++s) {
            const std::uint8_t dibit =
                out.dibits[body + word * kP25HduSymbolsPerGolayWord + s];
            code_word = (code_word << 2U) | dibit;
        }
        const Golay18Decode decoded = p25_golay18_decode(code_word);
        hexbits[word] = decoded.information;
        header.worst_golay_correction =
            std::max(header.worst_golay_correction, decoded.corrected_bits);
        if (decoded.corrected_bits != 0) {
            ++header.golay_words_corrected;
        }
    }

    // The Reed-Solomon code is systematic, so the first 20 hexbits are the
    // header fields as transmitted and the fields can be read without
    // decoding it. What that gives up is the RS code's correction: a hexbit
    // the Golay code got wrong stays wrong here, where RS(36,20,17) would have
    // fixed up to eight of them. Reported rather than hidden, through
    // worst_golay_correction above.
    std::array<std::uint8_t, 120> information_bits{};
    for (std::size_t i = 0; i < 20; ++i) {
        for (std::size_t b = 0; b < 6; ++b) {
            information_bits[i * 6 + b] =
                static_cast<std::uint8_t>((hexbits[i] >> (5U - b)) & 1U);
        }
    }

    // Clause 10.2's field order: MI(71..0), MFID(7..0), ALGID(7..0),
    // KID(15..0), TGID(15..0).
    for (std::size_t byte = 0; byte < 9; ++byte) {
        header.message_indicator[byte] =
            static_cast<std::uint8_t>(pack_bits_msb(information_bits, byte * 8, 8));
    }
    header.manufacturer_id = static_cast<std::uint8_t>(pack_bits_msb(information_bits, 72, 8));
    header.algorithm_id = static_cast<std::uint8_t>(pack_bits_msb(information_bits, 80, 8));
    header.key_id = static_cast<std::uint16_t>(pack_bits_msb(information_bits, 88, 16));
    header.talkgroup_id = static_cast<std::uint16_t>(pack_bits_msb(information_bits, 104, 16));
    header.encrypted = header.algorithm_id != kP25AlgidUnencrypted;

    out.header = header;
    return {};
}

Status P25Phase1::process(ConstComplexSpan samples, std::vector<P25Frame>& out) {
    if (samples.empty()) {
        return {};
    }

    discriminated_.assign(samples.size(), 0.0F);
    if (auto status = fm_discriminate(samples, discriminated_, config_.rate); !status) {
        return std::unexpected(with_context(status.error(), "P25 frequency discrimination"));
    }

    filtered_.assign(discriminated_.size(), 0.0F);
    if (auto status = filter_real(discriminated_, filter_taps_, filtered_); !status) {
        return std::unexpected(with_context(status.error(), "P25 receive filtering"));
    }

    // A carrier offset appears as a constant in the discriminator output, so
    // removing the block's mean is the automatic frequency control every C4FM
    // receiver needs. Over a whole data unit the four symbol levels are close
    // to balanced, which is what makes the mean an estimate of the offset
    // rather than of the data.
    double mean = 0.0;
    for (const float value : filtered_) {
        mean += static_cast<double>(value);
    }
    mean /= static_cast<double>(filtered_.size());

    // Scale so a Table 9-1 symbol comes out at its symbol-column value.
    const auto scale = static_cast<float>(1.0 / kP25DeviationPerSymbolUnitHz);
    std::vector<Complex32> shaped(filtered_.size());
    for (std::size_t i = 0; i < filtered_.size(); ++i) {
        shaped[i] = Complex32{(filtered_[i] - static_cast<float>(mean)) * scale, 0.0F};
    }

    recovered_.clear();
    sync_.process(shaped, recovered_);
    for (const RecoveredSymbol& symbol : recovered_) {
        symbols_.push_back(symbol.value.real());
    }
    last_symbols_.assign(symbols_.begin(), symbols_.end());

    const std::array<float, kP25FrameSyncSymbols> pattern = p25_frame_sync_pattern();
    const std::size_t minimum = kP25FrameSyncSymbols + kP25NidSymbols + 1;

    std::size_t position = consumed_;
    while (position + minimum <= symbols_.size()) {
        const double score = correlation_at(symbols_, pattern, position);
        if (std::abs(score) < config_.sync_threshold) {
            ++position;
            continue;
        }

        P25Frame frame;
        if (auto status = decode_at(position, score < 0.0, score, frame); status) {
            // Step past the whole data unit rather than past the sync word, so
            // a sync pattern appearing inside a payload cannot start a second
            // overlapping frame.
            const std::size_t length = p25_data_unit_symbols(frame.nid.duid);
            out.push_back(std::move(frame));
            position += (length != 0) ? length : kP25FrameSyncSymbols;
        } else {
            // Short of a NID. Leave the position where it is so the next call,
            // with more symbols behind it, tries again from the same place.
            break;
        }
    }

    consumed_ = position;

    // Trim what can never start a frame again, so a long capture does not grow
    // the buffer without bound. Anything before the last possible sync start
    // is finished with.
    const std::size_t keep = kP25HduTotalSymbols + kP25FrameSyncSymbols;
    if (consumed_ > keep) {
        const std::size_t drop = consumed_ - keep;
        symbols_.erase(symbols_.begin(), symbols_.begin() + static_cast<std::ptrdiff_t>(drop));
        consumed_ -= drop;
    }
    return {};
}

}  // namespace revenant::decode
