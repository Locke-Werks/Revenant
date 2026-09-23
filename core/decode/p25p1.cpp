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

// Hexbits, most significant bit first, into octets. Clauses 5.2, 5.4 and 5.5
// all say the same thing about the information: the fields are concatenated
// and then "separated into ... symbols of 6 bits each", so the bit order runs
// straight through the hexbit boundaries.
template <std::size_t Octets>
std::array<std::uint8_t, Octets> hexbits_to_octets(std::span<const std::uint8_t> hexbits) {
    std::array<std::uint8_t, Octets * 8> bits{};
    for (std::size_t i = 0; i < hexbits.size() && (i + 1) * 6 <= bits.size(); ++i) {
        for (std::size_t b = 0; b < 6; ++b) {
            bits[i * 6 + b] = static_cast<std::uint8_t>((hexbits[i] >> (5U - b)) & 1U);
        }
    }
    std::array<std::uint8_t, Octets> out{};
    for (std::size_t byte = 0; byte < Octets; ++byte) {
        out[byte] = static_cast<std::uint8_t>(pack_bits_msb(bits, byte * 8, 8));
    }
    return out;
}

// Reads `count` dibits from `dibits` starting at `first` as one integer, the
// first dibit most significant. Every code word in clauses 10.2 to 10.4 is
// transmitted most significant bit first, Bit 1 of a dibit before Bit 0.
std::uint32_t dibits_to_word(std::span<const std::uint8_t> dibits, std::size_t first,
                             std::size_t count) {
    std::uint32_t word = 0;
    for (std::size_t i = 0; i < count; ++i) {
        word = (word << 2U) | (dibits[first + i] & 0x3U);
    }
    return word;
}

}  // namespace

P25LinkControl p25_parse_link_control(const std::array<std::uint8_t, 9>& octets) {
    P25LinkControl lc;
    lc.octets = octets;
    lc.format = octets[0];
    lc.manufacturer_id = octets[1];
    lc.encrypted = (lc.format & kP25LcfEncryptedBit) != 0;
    if (lc.encrypted || lc.manufacturer_id != kP25MfidStandard) {
        return lc;
    }

    const auto source = static_cast<std::uint32_t>((octets[6] << 16U) | (octets[7] << 8U) |
                                                   octets[8]);
    // Figure 5-6, format $00: LCF, MFID, the emergency bit and 15 reserved
    // bits, TGID in octets 4 and 5, source in 6 to 8.
    if (lc.format == kP25LcfGroupVoice) {
        lc.emergency = (octets[2] & 0x80U) != 0;
        lc.talkgroup_id = static_cast<std::uint16_t>((octets[4] << 8U) | octets[5]);
        lc.source_id = source;
    }
    // Format $03: LCF, MFID, 8 reserved bits, destination in octets 3 to 5,
    // source in 6 to 8.
    if (lc.format == kP25LcfUnitToUnitVoice) {
        lc.destination_id =
            static_cast<std::uint32_t>((octets[3] << 16U) | (octets[4] << 8U) | octets[5]);
        lc.source_id = source;
    }
    return lc;
}

Status p25_decode_ldu(std::span<const std::uint8_t> information_dibits, std::uint8_t duid,
                      P25Frame& out) {
    const auto type = static_cast<P25Duid>(duid);
    if (type != P25Duid::LogicalLinkDataUnit1 && type != P25Duid::LogicalLinkDataUnit2) {
        return fail(std::format("p25_decode_ldu takes an LDU1 or LDU2; DUID {:#x} is {}", duid,
                                p25_duid_name(duid)));
    }
    if (information_dibits.size() < kP25LduInformationSymbols) {
        return fail(std::format(
            "an LDU carries {} information dibits once its {} status symbols are removed "
            "(TIA-102.BAAA-A clause 8.2.2); got {}",
            kP25LduInformationSymbols, kP25LduStatusSymbols, information_dibits.size()));
    }

    constexpr P25LduLayout kLayout = p25_ldu_layout();

    // Clause 5.3.1: each voice code word is 72 dibits in the Table 5-1 order,
    // Bit 1 then Bit 0, which is exactly the 144-bit order the vocoder's own
    // interleave, TIA-102.BABA Annex H, is written against.
    out.voice.assign(kP25VoiceFramesPerLdu, {});
    for (std::size_t frame = 0; frame < kP25VoiceFramesPerLdu; ++frame) {
        auto& bits = out.voice[frame];
        for (std::size_t s = 0; s < kP25VoiceFrameSymbols; ++s) {
            const std::uint8_t dibit = information_dibits[kLayout.voice[frame] + s];
            bits[2 * s] = static_cast<std::uint8_t>((dibit >> 1U) & 1U);
            bits[2 * s + 1] = static_cast<std::uint8_t>(dibit & 1U);
        }
    }

    // Clause 5.6: each low speed data octet is a (16,8,5) code word, the
    // octet then its parity, over eight dibits. Two errors are what d = 5
    // corrects, so a word further out than that is not reported.
    for (std::size_t octet = 0; octet < kP25LduLsdOctets; ++octet) {
        const std::uint32_t word = dibits_to_word(
            information_dibits, kLayout.low_speed_data + octet * kP25LsdWordSymbols,
            kP25LsdWordSymbols);
        const LsdDecode decoded = p25_lsd_decode(static_cast<std::uint16_t>(word));
        out.low_speed_data[octet].reset();
        if (decoded.distance <= 2) {
            out.low_speed_data[octet] = decoded.octet;
        }
    }

    // Clauses 5.4 and 5.5: 24 hexbits, each a (10,6,3) shortened Hamming
    // word, six information bits then four parity, over five dibits. The
    // annexes send them in code word order, the information hexbits first
    // and RS_parity_0 last.
    P25CodeReport report;
    std::array<std::uint8_t, kP25LduHammingWords> hexbits{};
    std::vector<std::size_t> erasures;
    for (std::size_t w = 0; w < kP25LduHammingWords; ++w) {
        const std::uint32_t word =
            dibits_to_word(information_dibits, kLayout.hamming[w], kP25HammingWordSymbols);
        const Hamming10Decode decoded = p25_hamming10_decode(static_cast<std::uint16_t>(word));
        hexbits[w] = decoded.information;
        if (decoded.detected) {
            erasures.push_back(w);
        } else if (decoded.distance != 0) {
            ++report.inner_words_corrected;
        }
        report.worst_inner_correction = std::max(report.worst_inner_correction, decoded.distance);
    }

    const P25ReedSolomon& code =
        type == P25Duid::LogicalLinkDataUnit1 ? kP25RsLinkControl : kP25RsEncryptionSync;
    auto decoded = p25_rs_decode(code, hexbits, erasures);
    if (!decoded) {
        return std::unexpected(with_context(decoded.error(), "decoding an LDU's Reed-Solomon word"));
    }
    out.link_control.reset();
    out.encryption_sync.reset();
    if (!decoded->decoded) {
        out.code_word_failed = true;
        return {};
    }
    report.rs_corrected = decoded->corrected;
    report.erasures = decoded->erasures;

    const std::span<const std::uint8_t> information(decoded->codeword.data(), code.k);
    if (type == P25Duid::LogicalLinkDataUnit1) {
        // Clause 5.5: 72 bits of Link Control in 12 hexbits.
        P25LinkControl lc = p25_parse_link_control(hexbits_to_octets<9>(information));
        lc.code = report;
        out.link_control = lc;
    } else {
        // Clause 5.4 and the clause 10.4 annex: MI(71..0), ALGID(7..0),
        // KID(15..0), 96 bits in 16 hexbits.
        const auto octets = hexbits_to_octets<12>(information);
        P25EncryptionSync es;
        std::copy(octets.begin(), octets.begin() + 9, es.message_indicator.begin());
        es.algorithm_id = octets[9];
        es.key_id = static_cast<std::uint16_t>((octets[10] << 8U) | octets[11]);
        es.encrypted = es.algorithm_id != kP25AlgidUnencrypted;
        es.code = report;
        out.encryption_sync = es;
    }
    return {};
}

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

Expected<P25Phase1::Outcome> P25Phase1::decode_at(std::size_t offset, bool inverted,
                                                  double score, P25Frame& out) const {
    // The shortest data unit that carries a NID is the sync word plus the NID
    // plus the status symbol that falls inside them.
    const std::size_t minimum = kP25FrameSyncSymbols + kP25NidSymbols + 1;
    if (offset + minimum > symbols_.size()) {
        return fail("not enough symbols for a Network Identifier");
    }

    const auto read_dibits = [&](std::size_t count) {
        std::vector<float> raw(symbols_.begin() + static_cast<std::ptrdiff_t>(offset),
                               symbols_.begin() + static_cast<std::ptrdiff_t>(offset + count));
        if (inverted) {
            for (float& value : raw) {
                value = -value;
            }
        }
        const std::vector<float> information = p25_strip_status_symbols(raw);
        std::vector<std::uint8_t> dibits;
        dibits.reserve(information.size());
        for (const float value : information) {
            dibits.push_back(slice_dibit(value));
        }
        return dibits;
    };

    std::vector<std::uint8_t> head = read_dibits(minimum);

    // Clause 8.5: the 64 NID bits follow the 48 sync bits.
    std::array<std::uint8_t, kP25NidBits> nid_bits{};
    for (std::size_t i = 0; i < kP25NidSymbols; ++i) {
        const std::uint8_t dibit = head[kP25FrameSyncSymbols + i];
        nid_bits[2 * i] = static_cast<std::uint8_t>((dibit >> 1U) & 1U);
        nid_bits[2 * i + 1] = static_cast<std::uint8_t>(dibit & 1U);
    }
    auto nid = p25_nid_decode(nid_bits);
    if (!nid) {
        return std::unexpected(with_context(nid.error(), "decoding the P25 Network Identifier"));
    }

    // Hold a data unit until all of it is in, so its payload is decoded once
    // and whole rather than reported short and then stepped over.
    const std::size_t unit = p25_data_unit_symbols(nid->duid);
    if (unit != 0 && offset + unit > symbols_.size()) {
        return Outcome::NeedMore;
    }
    // A packet data unit sizes itself from a header this file does not
    // decode, so it gets what a header data unit would, as it always has.
    const std::size_t take =
        (unit != 0) ? unit : std::min(kP25HduTotalSymbols, symbols_.size() - offset);

    out = P25Frame{};
    out.dibits = read_dibits(take);
    out.nid.network_access_code = nid->nac;
    out.nid.duid = nid->duid;
    out.nid.corrected_bits = nid->corrected_bits;
    out.first_symbol = offset;
    out.sync_score = score;
    out.inverted = inverted;

    const auto type = static_cast<P25Duid>(nid->duid);
    if (type == P25Duid::LogicalLinkDataUnit1 || type == P25Duid::LogicalLinkDataUnit2) {
        if (auto status = p25_decode_ldu(out.dibits, nid->duid, out); !status) {
            return std::unexpected(status.error());
        }
        return Outcome::Complete;
    }
    if (type != P25Duid::HeaderDataUnit) {
        return Outcome::Complete;
    }

    // Clause 10.2. After the sync word and the NID come 36 Golay (18,6,8)
    // code words, each carried as three symbols of information followed by six
    // of parity, then five null symbols.
    const std::size_t body = kP25FrameSyncSymbols + kP25NidSymbols;

    P25Header header;
    // 36 hexbits of 6 bits each, of which the first 20 are the information the
    // Reed-Solomon (36,20,17) code protects and the last 16 are its parity.
    std::array<std::uint8_t, kP25HduGolayWords> hexbits{};
    std::vector<std::size_t> erasures;
    for (std::size_t word = 0; word < kP25HduGolayWords; ++word) {
        const std::uint32_t code_word = dibits_to_word(
            out.dibits, body + word * kP25HduSymbolsPerGolayWord, kP25HduSymbolsPerGolayWord);
        const Golay18Decode decoded = p25_golay18_decode(code_word);
        hexbits[word] = decoded.information;
        header.worst_golay_correction =
            std::max(header.worst_golay_correction, decoded.corrected_bits);
        if (decoded.corrected_bits != 0) {
            ++header.golay_words_corrected;
        }
        // Distance 8 corrects three. A word four or more bits from the
        // nearest code word is a detected error, and an erasure is worth
        // twice what a wrong hexbit costs the Reed-Solomon code.
        if (decoded.corrected_bits >= 4) {
            erasures.push_back(word);
        }
    }
    header.code.inner_words_corrected = header.golay_words_corrected;
    header.code.worst_inner_correction = header.worst_golay_correction;

    auto decoded = p25_rs_decode(kP25RsHeader, hexbits, erasures);
    if (!decoded) {
        return std::unexpected(
            with_context(decoded.error(), "decoding the header's Reed-Solomon word"));
    }
    if (!decoded->decoded) {
        out.code_word_failed = true;
        return Outcome::Complete;
    }
    header.code.rs_corrected = decoded->corrected;
    header.code.erasures = decoded->erasures;

    // Clause 5.2 and the clause 10.2 annex: MI(71..0), MFID(7..0),
    // ALGID(7..0), KID(15..0), TGID(15..0), 120 bits in the first 20 hexbits.
    const auto octets = hexbits_to_octets<15>(
        std::span<const std::uint8_t>(decoded->codeword.data(), kP25RsHeader.k));
    std::copy(octets.begin(), octets.begin() + 9, header.message_indicator.begin());
    header.manufacturer_id = octets[9];
    header.algorithm_id = octets[10];
    header.key_id = static_cast<std::uint16_t>((octets[11] << 8U) | octets[12]);
    header.talkgroup_id = static_cast<std::uint16_t>((octets[13] << 8U) | octets[14]);
    header.encrypted = header.algorithm_id != kP25AlgidUnencrypted;

    out.header = header;
    return Outcome::Complete;
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
        auto outcome = decode_at(position, score < 0.0, score, frame);
        if (!outcome) {
            // The loop condition already guarantees a NID's worth of symbols,
            // so this is a decode that cannot succeed here however long it
            // waits. Stepping past the sync word keeps it from stalling the
            // stream on one position.
            position += kP25FrameSyncSymbols;
            continue;
        }
        if (*outcome == Outcome::NeedMore) {
            // Short of the data unit the NID named. Leave the position where
            // it is so the next call, with more symbols behind it, tries again
            // from the same place.
            break;
        }
        // Step past the whole data unit rather than past the sync word, so a
        // sync pattern appearing inside a payload cannot start a second
        // overlapping frame.
        const std::size_t length = p25_data_unit_symbols(frame.nid.duid);
        out.push_back(std::move(frame));
        position += (length != 0) ? length : kP25FrameSyncSymbols;
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

// ---------------------------------------------------------------------------
// Voice
// ---------------------------------------------------------------------------

void P25Voice::reset() {
    imbe_.reset();
    call_ = P25CallState{};
    pending_.clear();
}

void P25Voice::begin_call(std::uint16_t nac) {
    // A new call starts the vocoder from its TIA-102.BABA Annex A state, so
    // one call's pitch and phase history does not leak into the next.
    imbe_.reset();
    call_ = P25CallState{};
    call_.active = true;
    call_.network_access_code = nac;
    pending_.clear();
}

void P25Voice::end_call() {
    call_.frames_dropped += pending_.size();
    pending_.clear();
    call_.active = false;
}

Status P25Voice::decode_frames(
    std::span<const std::array<std::uint8_t, kP25VoiceFrameBits>> frames,
    std::vector<float>& pcm) {
    std::array<float, ImbeDecoder::kPcmFrames> block{};
    for (const auto& frame : frames) {
        if (auto status = imbe_.decode(frame, block); !status) {
            return std::unexpected(with_context(status.error(), "decoding a P25 voice frame"));
        }
        pcm.insert(pcm.end(), block.begin(), block.end());
        ++call_.frames_decoded;
        const ImbeFrameState state = imbe_.last_frame().state;
        call_.frames_repeated += state == ImbeFrameState::Repeated ? 1U : 0U;
        call_.frames_muted += state == ImbeFrameState::Muted ? 1U : 0U;
    }
    return {};
}

Status P25Voice::push(const P25Frame& frame, std::vector<float>& pcm) {
    const auto type = static_cast<P25Duid>(frame.nid.duid);
    const std::uint16_t nac = frame.nid.network_access_code;

    switch (type) {
        case P25Duid::HeaderDataUnit: {
            // Clause 5.1: the header begins a voice message.
            begin_call(nac);
            if (frame.header) {
                call_.talkgroup_id = frame.header->talkgroup_id;
                call_.algorithm_id = frame.header->algorithm_id;
                call_.key_id = frame.header->key_id;
                call_.encryption = frame.header->encrypted ? P25CallEncryption::Encrypted
                                                           : P25CallEncryption::Clear;
            }
            return {};
        }

        case P25Duid::TerminatorWithoutLinkControl:
        case P25Duid::TerminatorWithLinkControl:
            // Clause 8.2.3: the terminator marks the end of the message.
            end_call();
            return {};

        case P25Duid::LogicalLinkDataUnit1:
        case P25Duid::LogicalLinkDataUnit2:
            break;

        default:
            return {};
    }

    // An LDU with no header before it, or on a different network, is a call
    // joined in the middle.
    if (!call_.active || call_.network_access_code != nac) {
        begin_call(nac);
    }

    if (frame.link_control) {
        const P25LinkControl& lc = *frame.link_control;
        if (lc.encrypted) {
            call_.encryption = P25CallEncryption::Encrypted;
        }
        if (lc.talkgroup_id) {
            call_.talkgroup_id = lc.talkgroup_id;
        }
        if (lc.source_id) {
            call_.source_id = lc.source_id;
        }
    }

    if (frame.encryption_sync) {
        const P25EncryptionSync& es = *frame.encryption_sync;
        call_.algorithm_id = es.algorithm_id;
        call_.key_id = es.key_id;
        // Encrypted Link Control is evidence the encryption sync cannot
        // overrule: a call that has once said it is encrypted is treated as
        // encrypted to its end.
        if (es.encrypted) {
            call_.encryption = P25CallEncryption::Encrypted;
        } else if (call_.encryption != P25CallEncryption::Encrypted) {
            call_.encryption = P25CallEncryption::Clear;
        }
    }

    // Frames held from an LDU1 that arrived before anything said whether the
    // call was encrypted. They are earlier than this unit's, so they go first.
    if (!pending_.empty() && call_.encryption != P25CallEncryption::Unknown) {
        if (call_.encryption == P25CallEncryption::Clear) {
            if (auto status = decode_frames(pending_, pcm); !status) {
                return status;
            }
        } else {
            call_.frames_withheld += pending_.size();
        }
        pending_.clear();
    }

    switch (call_.encryption) {
        case P25CallEncryption::Clear:
            return decode_frames(frame.voice, pcm);
        case P25CallEncryption::Encrypted:
            call_.frames_withheld += frame.voice.size();
            return {};
        case P25CallEncryption::Unknown:
            // Hold one LDU's worth. A second unit arriving still unknown means
            // an encryption sync word failed its code, and the older frames
            // are too stale to play late.
            call_.frames_dropped += pending_.size();
            pending_.assign(frame.voice.begin(), frame.voice.end());
            return {};
    }
    return {};
}

}  // namespace revenant::decode
