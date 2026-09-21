#include "core/decode/dstar.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "core/decode/dv_codes.h"

namespace revenant::decode {
namespace {

// The 41 header bytes, as clause 4.1.2's frame diagram orders them.
constexpr std::size_t kHeaderBytes = 41;
constexpr std::size_t kFcsCoveredBytes = 39;

std::string trim_trailing_spaces(std::span<const std::uint8_t> field) {
    std::size_t length = field.size();
    while (length > 0 && (field[length - 1] == ' ' || field[length - 1] == 0)) {
        --length;
    }
    std::string out;
    out.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        out.push_back(static_cast<char>(field[i]));
    }
    return out;
}

void write_callsign(std::span<std::uint8_t> field, const std::string& text) {
    std::fill(field.begin(), field.end(), static_cast<std::uint8_t>(' '));
    const std::size_t length = std::min(field.size(), text.size());
    for (std::size_t i = 0; i < length; ++i) {
        field[i] = static_cast<std::uint8_t>(text[i]);
    }
}

// Ap1.4: each code's bit string is transmitted least significant bit first.
// Ap2.1 step 2 says the same of the bytes fed to the convolutional encoder.
void bytes_to_bits_lsb_first(std::span<const std::uint8_t> bytes,
                             std::vector<std::uint8_t>& out) {
    out.clear();
    out.reserve(bytes.size() * 8);
    for (const std::uint8_t byte : bytes) {
        for (unsigned bit = 0; bit < 8U; ++bit) {
            out.push_back(static_cast<std::uint8_t>((byte >> bit) & 1U));
        }
    }
}

void bits_to_bytes_lsb_first(std::span<const std::uint8_t> bits,
                             std::span<std::uint8_t> out) {
    std::fill(out.begin(), out.end(), static_cast<std::uint8_t>(0));
    for (std::size_t i = 0; i < bits.size() && i / 8 < out.size(); ++i) {
        out[i / 8] = static_cast<std::uint8_t>(out[i / 8] |
                                               ((bits[i] & 1U) << static_cast<unsigned>(i % 8)));
    }
}

const ConvolutionalCode& dstar_code() {
    static const ConvolutionalCode code{kDStarConvMemory, kDStarConvGenerators};
    return code;
}

}  // namespace

// ---------------------------------------------------------------------------
// The header codec
// ---------------------------------------------------------------------------

std::array<std::uint8_t, kHeaderBytes> dstar_header_bytes(const DStarHeader& header) {
    std::array<std::uint8_t, kHeaderBytes> bytes{};
    bytes[0] = header.flag1;
    bytes[1] = header.flag2;
    bytes[2] = header.flag3;
    write_callsign(std::span(bytes).subspan(3, 8), header.destination_repeater);
    write_callsign(std::span(bytes).subspan(11, 8), header.departure_repeater);
    write_callsign(std::span(bytes).subspan(19, 8), header.companion);
    write_callsign(std::span(bytes).subspan(27, 8), header.own_callsign);
    write_callsign(std::span(bytes).subspan(35, 4), header.own_suffix);

    const std::uint16_t fcs = dstar_header_fcs(std::span(bytes).subspan(0, kFcsCoveredBytes));

    // THE DOCUMENT DOES NOT SAY WHICH BYTE OF P_FCS GOES FIRST.
    //
    // Clause 4.1.1 k gives the polynomial and clause 4.1.2's diagram gives the
    // field's width as two bytes, and neither states the order. Least
    // significant byte first is what is written here, on the weak ground that
    // Ap1.4 makes the whole frame least significant first at the bit level.
    // A round trip against this project's own transmitter cannot settle it,
    // because both ends would make the same choice. Settling it needs a real
    // capture, and until one is recorded a receiver reporting fcs_valid false
    // on live traffic should suspect this line first.
    bytes[39] = static_cast<std::uint8_t>(fcs & 0xFFU);
    bytes[40] = static_cast<std::uint8_t>((fcs >> 8U) & 0xFFU);
    return bytes;
}

Expected<std::vector<std::uint8_t>> dstar_encode_header(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kHeaderBytes) {
        return fail(std::format(
            "the D-STAR radio header is {} bytes per the clause 4.1.2 frame diagram; got {}",
            kHeaderBytes, bytes.size()));
    }

    std::vector<std::uint8_t> bits;
    bytes_to_bits_lsb_first(bytes, bits);
    // Ap2.1 step 3: two zero bits after all the header information including
    // P_FCS. Two is what a memory-2 register needs and is what makes the
    // encoded length 660; the figure's "four hangover bits" is unreachable
    // from anything else in the document. See core/decode/dv_codes.h.
    bits.push_back(0);
    bits.push_back(0);

    auto encoded = convolutional_encode(dstar_code(), bits);
    if (!encoded) {
        return std::unexpected(with_context(encoded.error(), "D-STAR header convolutional code"));
    }
    if (encoded->size() != kDStarHeaderEncodedBits) {
        return fail(std::format(
            "the D-STAR header should encode to {} bits per clause Ap2.2's interleave "
            "matrix; got {}",
            kDStarHeaderEncodedBits, encoded->size()));
    }

    const std::vector<std::uint16_t> map = dstar_interleave_map();
    std::vector<std::uint8_t> out(kDStarHeaderEncodedBits, 0);
    for (std::size_t position = 0; position < map.size(); ++position) {
        out[position] = (*encoded)[map[position]];
    }
    dstar_scramble(out);
    return out;
}

Expected<std::array<std::uint8_t, kHeaderBytes>> dstar_decode_header(std::span<const float> soft) {
    if (soft.size() != kDStarHeaderEncodedBits) {
        return fail(std::format("the D-STAR encoded header is {} bits; got {}",
                                kDStarHeaderEncodedBits, soft.size()));
    }

    // Descramble. The scrambler is an exclusive or with a fixed sequence, so on
    // soft values it is a sign flip wherever the sequence carries a one.
    std::vector<std::uint8_t> sequence(kDStarHeaderEncodedBits, 0);
    dstar_scramble(sequence);

    std::vector<float> descrambled(kDStarHeaderEncodedBits, 0.0F);
    for (std::size_t i = 0; i < kDStarHeaderEncodedBits; ++i) {
        descrambled[i] = sequence[i] ? -soft[i] : soft[i];
    }

    const std::vector<std::uint16_t> map = dstar_interleave_map();
    std::vector<float> deinterleaved(kDStarHeaderEncodedBits, 0.0F);
    for (std::size_t position = 0; position < map.size(); ++position) {
        deinterleaved[map[position]] = descrambled[position];
    }

    auto decoded = viterbi_decode(dstar_code(), deinterleaved, true);
    if (!decoded) {
        return std::unexpected(with_context(decoded.error(), "D-STAR header Viterbi decode"));
    }
    if (decoded->size() < kDStarHeaderBits) {
        return fail(std::format("the D-STAR header decode produced {} bits, short of {}",
                                decoded->size(), kDStarHeaderBits));
    }

    std::array<std::uint8_t, kHeaderBytes> bytes{};
    bits_to_bytes_lsb_first(std::span(*decoded).subspan(0, kDStarHeaderBits), bytes);
    return bytes;
}

DStarHeader dstar_parse_header(std::span<const std::uint8_t> bytes) {
    DStarHeader header;
    if (bytes.size() < kHeaderBytes) {
        return header;
    }

    header.flag1 = bytes[0];
    header.flag2 = bytes[1];
    header.flag3 = bytes[2];

    // Clause 4.1.1 c.
    header.flags.data = ((header.flag1 >> 7U) & 1U) != 0U;
    header.flags.via_repeater = ((header.flag1 >> 6U) & 1U) != 0U;
    header.flags.interrupted = ((header.flag1 >> 5U) & 1U) != 0U;
    header.flags.control = ((header.flag1 >> 4U) & 1U) != 0U;
    header.flags.emergency = ((header.flag1 >> 3U) & 1U) != 0U;
    header.flags.response = static_cast<std::uint8_t>(header.flag1 & 0x07U);

    header.destination_repeater = trim_trailing_spaces(bytes.subspan(3, 8));
    header.departure_repeater = trim_trailing_spaces(bytes.subspan(11, 8));
    header.companion = trim_trailing_spaces(bytes.subspan(19, 8));
    header.own_callsign = trim_trailing_spaces(bytes.subspan(27, 8));
    header.own_suffix = trim_trailing_spaces(bytes.subspan(35, 4));

    header.fcs = static_cast<std::uint16_t>(bytes[39] |
                                            (static_cast<std::uint16_t>(bytes[40]) << 8U));
    header.fcs_valid = header.fcs == dstar_header_fcs(bytes.subspan(0, kFcsCoveredBytes));
    return header;
}

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

Expected<DStar> DStar::create(const DStarConfig& config) {
    if (config.rate <= 0) {
        return fail(std::format("DStar needs a positive sample rate; got {}", config.rate));
    }
    const double samples_per_bit = static_cast<double>(config.rate) / kDStarBitRate;
    if (samples_per_bit < 2.0) {
        return fail(std::format(
            "D-STAR DV is 4800 bit/s (JARL Ver 7.0 clause 4.1.2 b), so a rate of {} Hz "
            "gives {:.3f} samples per bit and the timing recovery has no midpoint to look "
            "at. 9600 Hz is the floor and 48000 is the usual choice",
            config.rate, samples_per_bit));
    }
    if (config.bandwidth_time <= 0.0) {
        return fail(std::format("DStar needs a positive bandwidth-time product; got {}",
                                config.bandwidth_time));
    }
    if (config.sync_threshold <= 0.0 || config.sync_threshold > 1.0) {
        return fail(
            std::format("DStar needs a sync threshold in (0, 1]; got {}", config.sync_threshold));
    }

    // The receive filter is a Gaussian matched to the transmitter's. The
    // standard specifies neither, since it specifies no BT at all, so this is
    // an engineering choice and is the reason bandwidth_time is configurable.
    auto taps = design_gaussian(config.rate, kDStarBitRate, config.bandwidth_time,
                                config.filter_taps);
    if (!taps) {
        return std::unexpected(with_context(taps.error(), "designing the D-STAR receive filter"));
    }

    SymbolSyncConfig sync_config;
    sync_config.rate = config.rate;
    sync_config.symbol_rate = kDStarBitRate;
    auto sync = SymbolSync::create(sync_config);
    if (!sync) {
        return std::unexpected(with_context(sync.error(), "creating the D-STAR bit timing"));
    }

    DStar decoder;
    decoder.config_ = config;
    decoder.filter_taps_ = std::move(*taps);
    decoder.sync_ = std::move(*sync);
    return decoder;
}

void DStar::reset() {
    sync_.reset();
    soft_bits_.clear();
    last_bits_.clear();
    consumed_ = 0;
}

Status DStar::process(ConstComplexSpan samples, std::vector<DStarTransmission>& out) {
    if (samples.empty()) {
        return {};
    }

    discriminated_.assign(samples.size(), 0.0F);
    if (auto status = fm_discriminate(samples, discriminated_, config_.rate); !status) {
        return std::unexpected(with_context(status.error(), "D-STAR frequency discrimination"));
    }

    filtered_.assign(discriminated_.size(), 0.0F);
    if (auto status = filter_real(discriminated_, filter_taps_, filtered_); !status) {
        return std::unexpected(with_context(status.error(), "D-STAR receive filtering"));
    }

    // Remove the carrier offset the same way the P25 path does. The bit sync
    // is an alternating pattern and the scrambled body is balanced, so the
    // block mean estimates the offset rather than the data.
    double mean = 0.0;
    for (const float value : filtered_) {
        mean += static_cast<double>(value);
    }
    mean /= static_cast<double>(filtered_.size());

    double peak = 1e-9;
    for (const float value : filtered_) {
        peak = std::max(peak, std::abs(static_cast<double>(value) - mean));
    }

    std::vector<Complex32> shaped(filtered_.size());
    for (std::size_t i = 0; i < filtered_.size(); ++i) {
        shaped[i] = Complex32{static_cast<float>((static_cast<double>(filtered_[i]) - mean) / peak),
                              0.0F};
    }

    recovered_.clear();
    sync_.process(shaped, recovered_);
    for (const RecoveredSymbol& symbol : recovered_) {
        soft_bits_.push_back(symbol.value.real());
    }

    last_bits_.clear();
    last_bits_.reserve(soft_bits_.size());
    for (const float value : soft_bits_) {
        // Ap1.5: a bit value of one makes the frequency deviation positive.
        last_bits_.push_back(static_cast<std::uint8_t>(value > 0.0F ? 1 : 0));
    }

    // The frame sync as soft values, +1 for a one and -1 for a zero, matching
    // the Ap1.5 polarity.
    std::array<float, kDStarFrameSync.size()> pattern{};
    for (std::size_t i = 0; i < kDStarFrameSync.size(); ++i) {
        pattern[i] = kDStarFrameSync[i] ? 1.0F : -1.0F;
    }

    const std::size_t needed = kDStarFrameSync.size() + kDStarHeaderEncodedBits;
    std::size_t position = consumed_;
    while (position + needed <= soft_bits_.size()) {
        const double score = correlation_at(soft_bits_, pattern, position);
        if (std::abs(score) < config_.sync_threshold) {
            ++position;
            continue;
        }

        const bool inverted = score < 0.0;
        std::vector<float> body(kDStarHeaderEncodedBits, 0.0F);
        for (std::size_t i = 0; i < kDStarHeaderEncodedBits; ++i) {
            const float value = soft_bits_[position + kDStarFrameSync.size() + i];
            // NEGATED, because the two ends of this handoff use opposite sign
            // conventions and both are right for their own side.
            //
            // Everything above here is in the standard's polarity: clause
            // Ap1.5 says a bit value of one makes the frequency deviation
            // positive, so a positive soft value leans towards a one, and the
            // bit sync, frame sync and payload slicers all read it that way.
            // core/decode/dv_codes.h's SoftSpan is the opposite, positive
            // leaning towards zero, because that makes the hard decision
            // (value < 0) and matches every slicer in that file.
            //
            // Getting this wrong produces a demodulator that recovers every
            // bit correctly, finds the frame sync, and then fails the header
            // frame check sequence on every single transmission, which reads
            // as a broken CRC rather than as a sign.
            body[i] = inverted ? value : -value;
        }

        auto header = decode_header(body);
        if (!header) {
            ++position;
            continue;
        }

        DStarTransmission transmission;
        transmission.header = *header;
        transmission.first_bit = position;
        transmission.sync_score = score;
        transmission.inverted = inverted;

        // Voice frames run from the end of the header until the last-frame
        // pattern or the end of what has arrived.
        std::size_t cursor = position + kDStarFrameSync.size() + kDStarHeaderEncodedBits;
        std::size_t frame_index = 0;
        while (cursor + kDStarFrameBits <= soft_bits_.size()) {
            DStarVoiceFrame frame;
            for (std::size_t i = 0; i < kDStarVoiceBits; ++i) {
                const float value = soft_bits_[cursor + i];
                frame.voice[i] =
                    static_cast<std::uint8_t>((inverted ? -value : value) > 0.0F ? 1 : 0);
            }
            for (std::size_t i = 0; i < kDStarDataBits; ++i) {
                const float value = soft_bits_[cursor + kDStarVoiceBits + i];
                frame.data[i] =
                    static_cast<std::uint8_t>((inverted ? -value : value) > 0.0F ? 1 : 0);
            }

            // Clause 4.1.2 c and d: the data slot carries the resynchronisation
            // signal on the first frame and every 21st after it. Checked
            // against the pattern rather than against the count, because a
            // receiver that joined mid-transmission does not know the count
            // and the pattern is what clause 4.1.2 c says the signal is for.
            std::size_t matches = 0;
            for (std::size_t i = 0; i < kDStarDataBits; ++i) {
                matches += (frame.data[i] == kDStarResync[i]) ? 1U : 0U;
            }
            frame.carried_resync = matches >= kDStarDataBits - 2;

            // Clause 4.1.2 h, the last frame. Its tail follows 32 bits of the
            // repeated sync pattern, which lands in the voice slot, so the
            // check is against the tail sitting where the data slot starts.
            std::size_t tail_matches = 0;
            for (std::size_t i = 0; i < kDStarLastFrameTail.size(); ++i) {
                tail_matches += (frame.data[i] == kDStarLastFrameTail[i]) ? 1U : 0U;
            }
            if (tail_matches >= kDStarLastFrameTail.size() - 1) {
                transmission.ended = true;
                cursor += kDStarFrameBits;
                break;
            }

            transmission.frames.push_back(frame);
            cursor += kDStarFrameBits;
            ++frame_index;
        }
        (void)frame_index;

        out.push_back(std::move(transmission));
        position = cursor;
    }

    consumed_ = position;

    const std::size_t keep = needed + kDStarFrameBits * kDStarResyncInterval;
    if (consumed_ > keep) {
        const std::size_t drop = consumed_ - keep;
        soft_bits_.erase(soft_bits_.begin(),
                         soft_bits_.begin() + static_cast<std::ptrdiff_t>(drop));
        consumed_ -= drop;
    }
    return {};
}

Expected<DStarHeader> DStar::decode_header(std::span<const float> soft) const {
    auto bytes = dstar_decode_header(soft);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    const DStarHeader header = dstar_parse_header(*bytes);
    if (!header.fcs_valid) {
        return fail("the D-STAR header frame check sequence did not verify");
    }
    return header;
}

}  // namespace revenant::decode
