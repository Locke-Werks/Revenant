#include "core/dsp/synth/dv_mod.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <random>

#include "core/decode/dv_codes.h"
#include "core/decode/dv_phy.h"

namespace revenant::siggen {
namespace {

using decode::design_from_response;
using decode::design_gaussian;
using decode::design_rrc;

constexpr double kPi = std::numbers::pi;

// TIA-102.BAAA-A clause 9.3 cascaded with clause 9.4: the Nyquist raised
// cosine filter and the shaping filter the C4FM modulator puts between the
// symbol impulses and the frequency modulator.
//
//   |H(f)| = 1                            for |f| < 1920 Hz
//   |H(f)| = 0.5 + 0.5 cos(2*pi*f/1920)   for 1920 < |f| < 2880 Hz
//   |H(f)| = 0                            for |f| > 2880 Hz
//
//   |P(f)| = (pi*f/4800) / sin(pi*f/4800) for |f| < 2880 Hz
//
// Clause 9.4 says P(f) above 2880 Hz "is not specified because the filter
// H(f) should cut off above 2880 Hz", so the product is zero there and the
// unspecified region never arises.
double p25_transmit_response(double hertz, const void* /*context*/) {
    if (hertz > decode::kP25NyquistStopHz) {
        return 0.0;
    }
    double nyquist = 1.0;
    if (hertz > decode::kP25NyquistFlatEdgeHz) {
        nyquist = 0.5 + 0.5 * std::cos(2.0 * kPi * hertz / decode::kP25NyquistFlatEdgeHz);
    }
    const double x = kPi * hertz / decode::kP25SymbolRate;
    const double shaping = (std::abs(x) < 1e-12) ? 1.0 : x / std::sin(x);
    return nyquist * shaping;
}

// Normalises a filter so that a symbol impulse train scaled by the samples per
// symbol comes out at the amplitude the symbol asked for.
//
// An impulse train carrying A every N samples through a filter whose taps sum
// to one averages A/N, not A, because N-1 of every N inputs are zero. So the
// impulses go in at A*N and the filter keeps its unit sum, which puts both the
// steady state and, for a Nyquist pulse, the symbol instants at A.
//
// Getting this wrong is quiet rather than loud, and it was got wrong here
// first: at ten samples per symbol the deviation came out a tenth of what
// Table 9-1 states, the frame sync still correlated at 0.94 because a
// normalised correlation does not see a scale factor, and every symbol landed
// in the inner two levels so the Network Identifier decoded to noise. A
// scaling error that survives the sync search and dies at the slicer looks
// like a framing bug from the outside.
void normalise_unit_sum(std::vector<float>& taps) {
    double sum = 0.0;
    for (const float tap : taps) {
        sum += static_cast<double>(tap);
    }
    if (std::abs(sum) < 1e-12) {
        return;
    }
    const auto scale = static_cast<float>(1.0 / sum);
    for (float& tap : taps) {
        tap *= scale;
    }
}

// Frequency modulates an instantaneous-frequency stream in hertz.
std::vector<Complex32> frequency_modulate(std::span<const float> hertz, SampleRate rate,
                                          double amplitude) {
    std::vector<Complex32> out(hertz.size());
    // Phase is accumulated in double and wrapped every sample, so an hour of
    // output does not lose precision the way a float accumulator would.
    double phase = 0.0;
    const double step = 2.0 * kPi / static_cast<double>(rate);
    for (std::size_t i = 0; i < hertz.size(); ++i) {
        phase += step * static_cast<double>(hertz[i]);
        if (phase > kPi) {
            phase -= 2.0 * kPi;
        } else if (phase < -kPi) {
            phase += 2.0 * kPi;
        }
        out[i] = Complex32{static_cast<float>(amplitude * std::cos(phase)),
                           static_cast<float>(amplitude * std::sin(phase))};
    }
    return out;
}

// Places one impulse per symbol at an integer number of samples apart. The
// integer requirement is what keeps the symbol instants exactly on samples,
// which is what lets the round trip be checked bit for bit rather than
// approximately.
Expected<std::size_t> samples_per_symbol(SampleRate rate, double symbol_rate) {
    const double exact = static_cast<double>(rate) / symbol_rate;
    const auto rounded = static_cast<std::size_t>(std::lround(exact));
    if (rounded < 2 || std::abs(exact - static_cast<double>(rounded)) > 1e-9) {
        return fail(std::format(
            "this transmitter needs the sample rate to be a whole multiple of the symbol "
            "rate, at least two; {} Hz over {} symbols per second is {:.6f}",
            rate, symbol_rate, exact));
    }
    return rounded;
}

// Interleaves the clause 8.4 status symbols into a run of information dibits,
// producing a data unit of exactly `total` symbols.
//
// Clause 8.4 and the clause 10.2 annex put them at absolute symbol positions
// 35, 71, 107 and so on. Table 8-2 gives 10 as "unknown, use for inbound or
// outbound", which is what a transmitter with nothing to say about the
// inbound channel sends.
Expected<std::vector<std::uint8_t>> place_status_symbols(
    std::span<const std::uint8_t> information, std::size_t total) {
    constexpr std::uint8_t kStatusUnknown = 0b10;
    std::vector<std::uint8_t> out;
    out.reserve(total);
    std::size_t taken = 0;
    for (std::size_t position = 0; position < total; ++position) {
        if (position >= decode::kP25FirstStatusSymbol &&
            (position - decode::kP25FirstStatusSymbol) % decode::kP25StatusSymbolInterval == 0) {
            out.push_back(kStatusUnknown);
            continue;
        }
        if (taken >= information.size()) {
            return fail(std::format(
                "a P25 data unit of {} symbols ran out of information at position {}; it "
                "was given {}",
                total, position, information.size()));
        }
        out.push_back(information[taken++]);
    }
    if (taken != information.size()) {
        return fail(std::format(
            "a P25 data unit of {} symbols left {} information symbols unplaced, so the "
            "layout in this file does not add up",
            total, information.size() - taken));
    }
    return out;
}

// Table 8-1, the frame sync word, then the clause 8.5 Network Identifier.
Status append_sync_and_nid(std::vector<std::uint8_t>& information, std::uint16_t nac,
                           decode::P25Duid duid) {
    for (std::size_t i = 0; i < decode::kP25FrameSyncSymbols; ++i) {
        const auto shift = static_cast<unsigned>(decode::kP25FrameSyncBits - 2 - 2 * i);
        information.push_back(static_cast<std::uint8_t>((decode::kP25FrameSync >> shift) & 0x3ULL));
    }
    std::array<std::uint8_t, decode::kP25NidBits> nid{};
    if (auto status = decode::p25_nid_encode(nac, static_cast<std::uint8_t>(duid), nid); !status) {
        return std::unexpected(with_context(status.error(), "encoding the P25 Network Identifier"));
    }
    for (std::size_t i = 0; i < decode::kP25NidSymbols; ++i) {
        information.push_back(static_cast<std::uint8_t>((nid[2 * i] << 1U) | nid[2 * i + 1]));
    }
    return {};
}

// A code word of `symbols` dibits, most significant first, which is how every
// code word in the clause 10 annexes goes out.
void append_word(std::vector<std::uint8_t>& information, std::uint32_t word, std::size_t symbols) {
    for (std::size_t s = 0; s < symbols; ++s) {
        const auto shift = static_cast<unsigned>(2 * (symbols - 1 - s));
        information.push_back(static_cast<std::uint8_t>((word >> shift) & 0x3U));
    }
}

// Clauses 5.2, 5.4 and 5.5: the fields concatenated most significant bit
// first and cut into 6-bit hexbits.
std::vector<std::uint8_t> octets_to_hexbits(std::span<const std::uint8_t> octets) {
    std::vector<std::uint8_t> hexbits(octets.size() * 8 / 6, 0);
    for (std::size_t bit = 0; bit < hexbits.size() * 6; ++bit) {
        const std::uint8_t value =
            static_cast<std::uint8_t>((octets[bit / 8] >> (7U - bit % 8)) & 1U);
        hexbits[bit / 6] = static_cast<std::uint8_t>((hexbits[bit / 6] << 1U) | value);
    }
    return hexbits;
}

// The clause 10.2 Header Data Unit, status symbols included.
Expected<std::vector<std::uint8_t>> header_data_unit(std::uint16_t nac,
                                                    const decode::P25Header& header) {
    std::vector<std::uint8_t> information;
    information.reserve(decode::kP25HduTotalSymbols);
    if (auto status = append_sync_and_nid(information, nac, decode::P25Duid::HeaderDataUnit);
        !status) {
        return std::unexpected(status.error());
    }

    // Clause 5.2: MI, MFID, ALGID, KID and TGID, 120 bits in 20 hexbits,
    // through the (36,20,17) Reed-Solomon code and then each of the 36
    // hexbits through the (18,6,8) shortened Golay code.
    std::array<std::uint8_t, 15> fields{};
    std::copy(header.message_indicator.begin(), header.message_indicator.end(), fields.begin());
    fields[9] = header.manufacturer_id;
    fields[10] = header.algorithm_id;
    fields[11] = static_cast<std::uint8_t>(header.key_id >> 8U);
    fields[12] = static_cast<std::uint8_t>(header.key_id & 0xFFU);
    fields[13] = static_cast<std::uint8_t>(header.talkgroup_id >> 8U);
    fields[14] = static_cast<std::uint8_t>(header.talkgroup_id & 0xFFU);

    auto codeword = decode::p25_rs_encode(decode::kP25RsHeader, octets_to_hexbits(fields));
    if (!codeword) {
        return std::unexpected(with_context(codeword.error(), "encoding the P25 header word"));
    }
    for (const std::uint8_t hexbit : *codeword) {
        append_word(information, decode::p25_golay18_encode(hexbit),
                    decode::kP25HduSymbolsPerGolayWord);
    }

    // The five null symbols the clause 10.2 annex ends with, at symbols 390
    // through 394, each the dibit 00.
    information.insert(information.end(), decode::kP25HduNullSymbols,
                       static_cast<std::uint8_t>(0b00));

    auto unit = place_status_symbols(information, decode::kP25HduTotalSymbols);
    if (!unit) {
        return std::unexpected(with_context(unit.error(), "laying out the P25 header data unit"));
    }
    return unit;
}

// Table 8-4 gives %0011 as the terminator without subsequent Link Control,
// and the clause 10.5 annex makes it the frame sync, the Network Identifier,
// fourteen nulls and two status symbols, 72 symbols in all.
Expected<std::vector<std::uint8_t>> simple_terminator(std::uint16_t nac) {
    std::vector<std::uint8_t> information;
    information.reserve(decode::kP25SimpleTerminatorSymbols);
    if (auto status =
            append_sync_and_nid(information, nac, decode::P25Duid::TerminatorWithoutLinkControl);
        !status) {
        return std::unexpected(status.error());
    }
    information.insert(information.end(), decode::kP25SimpleTerminatorNullSymbols,
                       static_cast<std::uint8_t>(0b00));
    auto unit = place_status_symbols(information, decode::kP25SimpleTerminatorSymbols);
    if (!unit) {
        return std::unexpected(with_context(unit.error(), "laying out the P25 simple terminator"));
    }
    return unit;
}

// One clause 10.3 or 10.4 Logical Link Data Unit. `words` is the 24-hexbit
// Reed-Solomon code word the unit carries, Link Control in an LDU1 and
// encryption sync in an LDU2; the layout is the same for both.
Expected<std::vector<std::uint8_t>> logical_link_data_unit(
    std::uint16_t nac, decode::P25Duid duid,
    std::span<const std::array<std::uint8_t, decode::kP25VoiceFrameBits>> voice,
    std::span<const std::uint8_t> words, std::array<std::uint8_t, 2> low_speed_data) {
    constexpr decode::P25LduLayout kLayout = decode::p25_ldu_layout();

    std::vector<std::uint8_t> information;
    information.reserve(decode::kP25LduInformationSymbols);
    if (auto status = append_sync_and_nid(information, nac, duid); !status) {
        return std::unexpected(status.error());
    }

    std::size_t word = 0;
    std::size_t frame = 0;
    // Walk the unit in transmission order, emitting whichever field the
    // layout puts at the current position.
    while (information.size() < decode::kP25LduInformationSymbols) {
        const std::size_t at = information.size();
        if (frame < voice.size() && at == kLayout.voice[frame]) {
            // Clause 5.3.1, Table 5-1: Bit 1 then Bit 0 of each dibit.
            for (std::size_t s = 0; s < decode::kP25VoiceFrameSymbols; ++s) {
                information.push_back(static_cast<std::uint8_t>(
                    ((voice[frame][2 * s] & 1U) << 1U) | (voice[frame][2 * s + 1] & 1U)));
            }
            ++frame;
        } else if (word < words.size() && at == kLayout.hamming[word]) {
            // Clause 5.8: each hexbit through the (10,6,3) shortened Hamming
            // code.
            append_word(information, decode::p25_hamming10_encode(words[word]),
                        decode::kP25HammingWordSymbols);
            ++word;
        } else if (at == kLayout.low_speed_data) {
            // Clause 5.6: each octet through the (16,8,5) cyclic code.
            for (const std::uint8_t octet : low_speed_data) {
                append_word(information, decode::p25_lsd_encode(octet),
                            decode::kP25LsdWordSymbols);
            }
        } else {
            return fail(std::format(
                "the LDU layout names no field at information symbol {}, so "
                "core/decode/p25p1.h's p25_ldu_layout does not tile the unit",
                at));
        }
    }

    auto unit = place_status_symbols(information, decode::kP25LduSymbols);
    if (!unit) {
        return std::unexpected(with_context(unit.error(), "laying out a P25 LDU"));
    }
    return unit;
}

std::vector<std::uint8_t> pseudorandom_bits(std::size_t count, std::uint64_t seed) {
    std::mt19937_64 engine(seed);
    std::vector<std::uint8_t> out(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = static_cast<std::uint8_t>(engine() & 1ULL);
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// P25 Phase 1
// ---------------------------------------------------------------------------

Expected<std::vector<std::uint8_t>> p25_header_message_dibits(const P25HeaderMessage& message) {
    auto unit = header_data_unit(message.network_access_code, message.header);
    if (!unit) {
        return std::unexpected(unit.error());
    }

    // A simple terminator after the header, so the transmission ends the way
    // clause 8 says a message does rather than stopping mid-symbol.
    auto terminator = simple_terminator(message.network_access_code);
    if (!terminator) {
        return std::unexpected(terminator.error());
    }
    unit->insert(unit->end(), terminator->begin(), terminator->end());
    return *unit;
}

Expected<std::vector<std::uint8_t>> p25_voice_message_dibits(const P25VoiceMessage& message) {
    if (message.voice.empty() || message.voice.size() % decode::kP25VoiceFramesPerLdu != 0) {
        return fail(std::format(
            "a P25 voice message carries its voice nine frames to an LDU (TIA-102.BAAA-A "
            "clause 8.2.2); got {} frames. This transmitter has no IMBE encoder to fill the "
            "last LDU with the silence clause 8.2.3 asks for, so hand it whole LDUs",
            message.voice.size()));
    }
    for (std::size_t f = 0; f < message.voice.size(); ++f) {
        for (const std::uint8_t bit : message.voice[f]) {
            if (bit > 1) {
                return fail(std::format(
                    "voice frame {} holds the value {}; the frames are one bit per byte",
                    f, static_cast<unsigned>(bit)));
            }
        }
    }

    std::vector<std::uint8_t> out;
    if (message.header) {
        auto unit = header_data_unit(message.network_access_code, *message.header);
        if (!unit) {
            return std::unexpected(unit.error());
        }
        out.insert(out.end(), unit->begin(), unit->end());
    }

    // Clause 5.5: the 72 Link Control bits in 12 hexbits, through the
    // (24,12,13) code.
    auto link_control = decode::p25_rs_encode(decode::kP25RsLinkControl,
                                              octets_to_hexbits(message.link_control));
    if (!link_control) {
        return std::unexpected(with_context(link_control.error(), "encoding the Link Control word"));
    }

    // Clause 5.4: MI, ALGID and KID, 96 bits in 16 hexbits, through the
    // (24,16,9) code.
    std::array<std::uint8_t, 12> sync_fields{};
    std::copy(message.message_indicator.begin(), message.message_indicator.end(),
              sync_fields.begin());
    sync_fields[9] = message.algorithm_id;
    sync_fields[10] = static_cast<std::uint8_t>(message.key_id >> 8U);
    sync_fields[11] = static_cast<std::uint8_t>(message.key_id & 0xFFU);
    auto encryption_sync =
        decode::p25_rs_encode(decode::kP25RsEncryptionSync, octets_to_hexbits(sync_fields));
    if (!encryption_sync) {
        return std::unexpected(
            with_context(encryption_sync.error(), "encoding the encryption sync word"));
    }

    const std::size_t units = message.voice.size() / decode::kP25VoiceFramesPerLdu;
    for (std::size_t u = 0; u < units; ++u) {
        // Clause 8.2.2: LDU1 and LDU2 alternate, LDU1 first.
        const bool first = u % 2 == 0;
        std::array<std::uint8_t, 2> lsd{};
        for (std::size_t i = 0; i < lsd.size(); ++i) {
            const std::size_t index = u * lsd.size() + i;
            lsd[i] = index < message.low_speed_data.size() ? message.low_speed_data[index] : 0;
        }
        auto unit = logical_link_data_unit(
            message.network_access_code,
            first ? decode::P25Duid::LogicalLinkDataUnit1 : decode::P25Duid::LogicalLinkDataUnit2,
            std::span(message.voice).subspan(u * decode::kP25VoiceFramesPerLdu,
                                             decode::kP25VoiceFramesPerLdu),
            first ? *link_control : *encryption_sync, lsd);
        if (!unit) {
            return std::unexpected(unit.error());
        }
        out.insert(out.end(), unit->begin(), unit->end());
    }

    auto terminator = simple_terminator(message.network_access_code);
    if (!terminator) {
        return std::unexpected(terminator.error());
    }
    out.insert(out.end(), terminator->begin(), terminator->end());
    return out;
}

Expected<std::vector<Complex32>> p25_render_dibits(const P25ModConfig& config,
                                                   std::span<const std::uint8_t> dibits) {
    auto sps = samples_per_symbol(config.rate, decode::kP25SymbolRate);
    if (!sps) {
        return std::unexpected(sps.error());
    }
    auto taps = design_from_response(config.rate, config.filter_taps, p25_transmit_response,
                                     nullptr);
    if (!taps) {
        return std::unexpected(
            with_context(taps.error(), "designing the clause 9.3 and 9.4 transmit filters"));
    }
    normalise_unit_sum(*taps);

    // Table 9-1. The impulses carry the deviation in hertz, so the filter
    // output is an instantaneous frequency and the modulator below turns it
    // into phase.
    const auto scale = static_cast<double>(*sps);
    std::vector<float> impulses(dibits.size() * *sps + config.filter_taps, 0.0F);
    for (std::size_t i = 0; i < dibits.size(); ++i) {
        const std::size_t index = static_cast<std::size_t>(dibits[i] & 0x3U);
        impulses[i * *sps] = static_cast<float>(decode::kP25DibitToSymbol[index] *
                                                decode::kP25DeviationPerSymbolUnitHz * scale);
    }

    std::vector<float> shaped(impulses.size(), 0.0F);
    if (auto status = decode::filter_real(impulses, *taps, shaped); !status) {
        return std::unexpected(with_context(status.error(), "P25 transmit filtering"));
    }
    return frequency_modulate(shaped, config.rate, config.amplitude);
}

Expected<std::vector<Complex32>> p25_render_header_message(const P25ModConfig& config,
                                                           const P25HeaderMessage& message) {
    auto dibits = p25_header_message_dibits(message);
    if (!dibits) {
        return std::unexpected(dibits.error());
    }
    return p25_render_dibits(config, *dibits);
}

// ---------------------------------------------------------------------------
// D-STAR
// ---------------------------------------------------------------------------

Expected<std::vector<std::uint8_t>> dstar_message_bits(const DStarMessage& message) {
    std::vector<std::uint8_t> bits;

    // Clause 4.1.1 a: the bit sync is "1010" repeated, 64 bits for GMSK.
    for (std::size_t i = 0; i < decode::kDStarBitSyncBits; ++i) {
        bits.push_back(static_cast<std::uint8_t>((i % 2 == 0) ? 1 : 0));
    }

    // Clause 4.1.1 b: the 15 bit frame sync, leftmost bit first.
    bits.insert(bits.end(), decode::kDStarFrameSync.begin(), decode::kDStarFrameSync.end());

    const std::array<std::uint8_t, 41> header_bytes = decode::dstar_header_bytes(message.header);
    auto encoded = decode::dstar_encode_header(header_bytes);
    if (!encoded) {
        return std::unexpected(with_context(encoded.error(), "encoding the D-STAR radio header"));
    }
    bits.insert(bits.end(), encoded->begin(), encoded->end());

    for (std::size_t frame = 0; frame < message.voice_frames.size(); ++frame) {
        bits.insert(bits.end(), message.voice_frames[frame].begin(),
                    message.voice_frames[frame].end());

        // Clause 4.1.2 c: the first data frame and every 21st after it carry
        // the resynchronisation signal in place of data.
        if (frame % decode::kDStarResyncInterval == 0) {
            bits.insert(bits.end(), decode::kDStarResync.begin(), decode::kDStarResync.end());
            continue;
        }
        if (frame < message.data_frames.size()) {
            bits.insert(bits.end(), message.data_frames[frame].begin(),
                        message.data_frames[frame].end());
        } else {
            bits.insert(bits.end(), decode::kDStarDataBits, static_cast<std::uint8_t>(0));
        }
    }

    if (message.terminate) {
        // Clause 4.1.2 h: 32 bits of the repeated sync pattern, the 15 bits
        // "000100110101111", then a single "0".
        for (std::size_t i = 0; i < 32; ++i) {
            bits.push_back(static_cast<std::uint8_t>((i % 2 == 0) ? 1 : 0));
        }
        bits.insert(bits.end(), decode::kDStarLastFrameTail.begin(),
                    decode::kDStarLastFrameTail.end());
    }
    return bits;
}

Expected<std::vector<Complex32>> dstar_render_bits(const DStarModConfig& config,
                                                   std::span<const std::uint8_t> bits) {
    auto sps = samples_per_symbol(config.rate, decode::kDStarBitRate);
    if (!sps) {
        return std::unexpected(sps.error());
    }
    auto taps = design_gaussian(config.rate, decode::kDStarBitRate, config.bandwidth_time,
                                config.filter_taps);
    if (!taps) {
        return std::unexpected(
            with_context(taps.error(), "designing the D-STAR premodulation filter"));
    }

    // Modulation index one half, which is what the M in GMSK stands for: the
    // phase advances by a quarter turn per bit, so the peak deviation is a
    // quarter of the bit rate. The JARL standard states neither the index nor
    // the bandwidth-time product, so this is the definition of GMSK rather
    // than a citation, and clause Ap1.5 fixes only the sign: a bit value of
    // one makes the deviation positive.
    const double peak_deviation = decode::kDStarBitRate / 4.0;

    // Scaled by the samples per symbol for the reason normalise_unit_sum
    // above gives: design_gaussian normalises to unit area, so an impulse
    // train needs the factor back.
    const double amplitude = peak_deviation * static_cast<double>(*sps);
    std::vector<float> impulses(bits.size() * *sps + config.filter_taps, 0.0F);
    for (std::size_t i = 0; i < bits.size(); ++i) {
        impulses[i * *sps] = static_cast<float>((bits[i] & 1U) ? amplitude : -amplitude);
    }

    std::vector<float> shaped(impulses.size(), 0.0F);
    if (auto status = decode::filter_real(impulses, *taps, shaped); !status) {
        return std::unexpected(with_context(status.error(), "D-STAR premodulation filtering"));
    }
    return frequency_modulate(shaped, config.rate, config.amplitude);
}

Expected<std::vector<Complex32>> dstar_render(const DStarModConfig& config,
                                              const DStarMessage& message) {
    auto bits = dstar_message_bits(message);
    if (!bits) {
        return std::unexpected(bits.error());
    }
    return dstar_render_bits(config, *bits);
}

// ---------------------------------------------------------------------------
// TETRA
// ---------------------------------------------------------------------------

Expected<std::vector<std::uint8_t>> tetra_sync_burst_bits(const decode::TetraSyncPdu& pdu,
                                                          std::uint64_t filler_seed) {
    const std::array<std::uint8_t, decode::kTetraBschInformationBits> information =
        decode::tetra_sync_pdu_bits(pdu);
    auto block1 = decode::tetra_bsch_encode(information);
    if (!block1) {
        return std::unexpected(with_context(block1.error(), "encoding the TETRA BSCH"));
    }

    std::vector<std::uint8_t> bits(decode::kTetraBurstBits, 0);

    // Table 9.9. Normal training sequence 3 is spread over two bursts, with
    // q11 through q22 at the head of this one and q1 through q10 at the tail.
    for (std::size_t i = 0; i < decode::kTetraSyncBurstTrainingHead.length; ++i) {
        bits[decode::kTetraSyncBurstTrainingHead.offset + i] =
            decode::kTetraNormalTrainingSequence3[10 + i];
    }

    // Clause 9.4.4.3.6 leaves the phase adjustment bits to the transmitter's
    // own arithmetic, since their job is to hold a known phase relationship
    // between the training sequences whatever the blocks contain. Nothing in
    // this tree uses them, so they go out as zero and the burst is still
    // demodulable; a receiver that equalised against them would notice.
    bits[decode::kTetraSyncBurstPhaseHead.offset] = 0;
    bits[decode::kTetraSyncBurstPhaseHead.offset + 1] = 0;

    // Clause 9.4.4.3.1, equations 9.1 to 9.3: eight ones, sixty-four zeros,
    // eight ones.
    for (std::size_t i = 0; i < decode::kTetraSyncBurstFrequencyCorrection.length; ++i) {
        const bool one = (i < 8) || (i >= 72);
        bits[decode::kTetraSyncBurstFrequencyCorrection.offset + i] =
            static_cast<std::uint8_t>(one ? 1 : 0);
    }

    for (std::size_t i = 0; i < decode::kTetraSyncBurstBlock1.length; ++i) {
        bits[decode::kTetraSyncBurstBlock1.offset + i] = (*block1)[i];
    }

    for (std::size_t i = 0; i < decode::kTetraSyncBurstTraining.length; ++i) {
        bits[decode::kTetraSyncBurstTraining.offset + i] = decode::kTetraSyncTrainingSequence[i];
    }

    // The broadcast bits and block 2 are scrambled with the cell's real colour
    // code and carry channels this project does not decode. Filled with a
    // pseudorandom pattern so the burst has realistic statistics rather than a
    // run of zeros the timing loop would coast through.
    const std::vector<std::uint8_t> filler = pseudorandom_bits(
        decode::kTetraSyncBurstBroadcast.length + decode::kTetraSyncBurstBlock2.length,
        filler_seed);
    for (std::size_t i = 0; i < decode::kTetraSyncBurstBroadcast.length; ++i) {
        bits[decode::kTetraSyncBurstBroadcast.offset + i] = filler[i];
    }
    for (std::size_t i = 0; i < decode::kTetraSyncBurstBlock2.length; ++i) {
        bits[decode::kTetraSyncBurstBlock2.offset + i] =
            filler[decode::kTetraSyncBurstBroadcast.length + i];
    }

    bits[decode::kTetraSyncBurstPhaseTail.offset] = 0;
    bits[decode::kTetraSyncBurstPhaseTail.offset + 1] = 0;

    for (std::size_t i = 0; i < decode::kTetraSyncBurstTrainingTail.length; ++i) {
        bits[decode::kTetraSyncBurstTrainingTail.offset + i] =
            decode::kTetraNormalTrainingSequence3[i];
    }
    return bits;
}

Expected<std::vector<Complex32>> tetra_render_bits(const TetraModConfig& config,
                                                   std::span<const std::uint8_t> bits) {
    if (bits.size() % 2 != 0) {
        return fail(std::format(
            "TETRA pi/4-DQPSK takes two bits per symbol (EN 300 392-2 clause 5.4); got {} "
            "bits, which is not a whole number of symbols",
            bits.size()));
    }
    auto sps = samples_per_symbol(config.rate, decode::kTetraSymbolRate);
    if (!sps) {
        return std::unexpected(sps.error());
    }
    auto taps = design_rrc(config.rate, decode::kTetraSymbolRate, decode::kTetraRollOff,
                           config.filter_taps);
    if (!taps) {
        return std::unexpected(
            with_context(taps.error(), "designing the clause 5.5 transmit filter"));
    }

    // Clause 5.4, equation 5.1: S(k) = S(k-1) * exp(j*D(k)) with S(0) = 1, the
    // phase reference transmitted before the first symbol of the first burst.
    const std::size_t symbols = bits.size() / 2;
    std::vector<Complex32> impulses(symbols * *sps + config.filter_taps, Complex32{});
    double phase = 0.0;
    for (std::size_t k = 0; k < symbols; ++k) {
        phase += decode::tetra_phase_transition(bits[2 * k], bits[2 * k + 1]);
        if (phase > kPi) {
            phase -= 2.0 * kPi;
        } else if (phase < -kPi) {
            phase += 2.0 * kPi;
        }
        impulses[k * *sps] = Complex32{static_cast<float>(config.amplitude * std::cos(phase)),
                                       static_cast<float>(config.amplitude * std::sin(phase))};
    }

    std::vector<Complex32> out(impulses.size(), Complex32{});
    if (auto status = decode::filter_complex(impulses, *taps, out); !status) {
        return std::unexpected(with_context(status.error(), "TETRA transmit filtering"));
    }
    return out;
}

Expected<std::vector<Complex32>> tetra_render_sync_burst(const TetraModConfig& config,
                                                         const decode::TetraSyncPdu& pdu,
                                                         std::uint64_t filler_seed) {
    auto bits = tetra_sync_burst_bits(pdu, filler_seed);
    if (!bits) {
        return std::unexpected(bits.error());
    }
    return tetra_render_bits(config, *bits);
}

}  // namespace revenant::siggen
