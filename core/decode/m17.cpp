#include "core/decode/m17.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <format>
#include <numbers>

#include "core/decode/dv_codes.h"

namespace revenant::decode {
namespace {

constexpr double kPi = std::numbers::pi;

// M17 2.7 and 2.8.1: four zero flush bits after the content bits, so the
// encoder ends in state zero and the Viterbi decoder may start its traceback
// there.
constexpr std::size_t kFlushBits = 4;

constexpr std::size_t kLsfBits = kM17LsfBytes * 8;             // 240
constexpr std::size_t kLsfEncodedBits = (kLsfBits + kFlushBits) * 2;  // 488
constexpr std::size_t kStreamContentBits = 16 + 128;                  // 144, Table 2.10
constexpr std::size_t kStreamEncodedBits = (kStreamContentBits + kFlushBits) * 2;  // 296
constexpr std::size_t kStreamPuncturedBits = 272;                     // 2.8.1
constexpr std::size_t kLichEncodedBits = 96;                          // 2.8.1

// Symbols of the LSF preamble read alongside its sync burst to calibrate the
// level and offset of the first frame. Any number up to the 192 of 1.4.1
// would do; 32 is enough that the calibration's own noise is well under the
// channel's at the signal to noise ratios this decodes at.
constexpr std::size_t kPreambleCalibrationSymbols = 32;

// Share of each frame's calibration taken into the running one.
constexpr double kCalibrationLeak = 0.25;

// Soft values are clipped to this magnitude before the Viterbi decoder. NOT A
// SPECIFIED VALUE, and a measured one: unclipped, the stream frame error rate
// over 100 frames was 0.23 at 12 dB in 9 kHz and 0.85 at 10 dB; clipped at
// 1.0 it is 0.01 and 0.18, at 1.5 it is 0.01 and 0.29, and at 0.7 it is 0.01
// and 0.17. A frequency discriminator's noise is not Gaussian: below
// threshold it clicks, and a click makes a symbol wrong with all the
// confidence of an outer level, which is exactly what an unclipped
// correlation metric trusts most. One unit is the distance from an inner
// level to a decision boundary.
constexpr double kSoftClip = 1.0;

ConvolutionalCode conv_code() {
    ConvolutionalCode code;
    code.memory = kM17ConvMemory;
    code.generators = kM17ConvGenerators;
    return code;
}

void push_bits(std::vector<std::uint8_t>& bits, std::uint64_t value, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        bits.push_back(static_cast<std::uint8_t>((value >> (count - 1 - i)) & 1U));
    }
}

void push_bytes(std::vector<std::uint8_t>& bits, std::span<const std::uint8_t> bytes) {
    for (const std::uint8_t byte : bytes) {
        push_bits(bits, byte, 8);
    }
}

std::vector<std::uint8_t> puncture(std::span<const std::uint8_t> bits,
                                   std::span<const std::uint8_t> pattern) {
    std::vector<std::uint8_t> out;
    out.reserve(bits.size());
    for (std::size_t i = 0; i < bits.size(); ++i) {
        if (pattern[i % pattern.size()] != 0U) {
            out.push_back(bits[i]);
        }
    }
    return out;
}

// Puts the punctured soft values back in their places with zero, no
// information, in the gaps, which is what the Viterbi decoder's correlation
// metric wants for a bit that was never sent.
std::vector<float> depuncture(std::span<const float> soft, std::span<const std::uint8_t> pattern,
                              std::size_t full) {
    std::vector<float> out(full, 0.0F);
    std::size_t next = 0;
    for (std::size_t i = 0; i < full && next < soft.size(); ++i) {
        if (pattern[i % pattern.size()] != 0U) {
            out[i] = soft[next++];
        }
    }
    return out;
}

std::uint64_t read_be(std::span<const std::uint8_t> bytes, std::size_t first, std::size_t count) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        value = (value << 8U) | bytes[first + i];
    }
    return value;
}

void write_be(std::span<std::uint8_t> bytes, std::size_t first, std::size_t count,
              std::uint64_t value) {
    for (std::size_t i = 0; i < count; ++i) {
        bytes[first + count - 1 - i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

// Table A.1, value for a character.
unsigned alphabet_value(char c) {
    const auto u = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(c)));
    if (u >= 'A' && u <= 'Z') {
        return 1U + (u - 'A');
    }
    if (u >= '0' && u <= '9') {
        return 27U + (u - '0');
    }
    if (u == '-') {
        return 37U;
    }
    if (u == '/') {
        return 38U;
    }
    if (u == '.') {
        return 39U;
    }
    return 0U;  // space, and "any invalid character"
}

// Table A.1, character for a value. The table's ASCII column prints 0x3F
// and 0x3E beside the slash and the dot, which are '?' and '>'; the
// characters and their names in the same rows are the slash and the dot, and
// those are what this uses. A transcription slip in the table, recorded so
// nobody "fixes" this to match it.
char alphabet_char(unsigned value) {
    if (value == 0U) {
        return ' ';
    }
    if (value <= 26U) {
        return static_cast<char>('A' + (value - 1U));
    }
    if (value <= 36U) {
        return static_cast<char>('0' + (value - 27U));
    }
    if (value == 37U) {
        return '-';
    }
    if (value == 38U) {
        return '/';
    }
    return '.';
}

// Table A.2: 40^9, the first address the base-40 scheme cannot reach.
constexpr std::uint64_t kFirstExtendedAddress = 0xEE6B28000000ULL;
constexpr std::uint64_t kBroadcastAddress = 0xFFFFFFFFFFFFULL;

const std::array<std::uint32_t, 4096>& golay_table() {
    static const std::array<std::uint32_t, 4096> table = [] {
        std::array<std::uint32_t, 4096> words{};
        for (std::uint32_t data = 0; data < 4096; ++data) {
            words[data] = m17_golay_encode(static_cast<std::uint16_t>(data));
        }
        return words;
    }();
    return table;
}

// The alternating preamble of 1.4.1 that precedes an LSF sync burst, the
// last `count` symbols of it: it alternates +3, -3 and its last symbol is the
// opposite of the burst's first, which for LSF is +3 (Table 2.3).
std::vector<float> lsf_preamble_tail(std::size_t count) {
    std::vector<float> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        // The symbol just before the burst is -3, the one before it +3.
        const std::size_t back = count - i;
        out[i] = (back % 2 == 1) ? -3.0F : 3.0F;
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Codes
// ---------------------------------------------------------------------------

std::array<std::uint8_t, 61> m17_puncture_p1() {
    std::array<std::uint8_t, 61> pattern{};
    pattern[0] = 1;
    constexpr std::array<std::uint8_t, 4> kM = {1, 0, 1, 1};  // Appendix E, (E.1)
    for (std::size_t i = 0; i < 60; ++i) {
        pattern[1 + i] = kM[i % 4];
    }
    return pattern;
}

std::uint32_t m17_interleave(std::uint32_t index) {
    const std::uint64_t x = index;
    return static_cast<std::uint32_t>((45ULL * x + 92ULL * x * x) % kM17PayloadBits);
}

std::uint16_t m17_crc(std::span<const std::uint8_t> bytes) {
    std::uint16_t crc = kM17CrcInitial;
    for (const std::uint8_t byte : bytes) {
        crc = static_cast<std::uint16_t>(crc ^ (static_cast<std::uint16_t>(byte) << 8U));
        for (int bit = 0; bit < 8; ++bit) {
            const bool top = (crc & 0x8000U) != 0U;
            crc = static_cast<std::uint16_t>(crc << 1U);
            if (top) {
                crc = static_cast<std::uint16_t>(crc ^ kM17CrcPolynomial);
            }
        }
    }
    return crc;
}

std::uint32_t m17_golay_encode(std::uint16_t data) {
    // Appendix D: systematic, the 11 check bits the remainder of the data
    // times x^11 divided by g(x), then one parity bit. The parity is even
    // over all 24 bits, which is what the matrix printed as (D.1) shows: every
    // one of its twelve rows has even weight.
    std::uint32_t remainder = static_cast<std::uint32_t>(data & 0xFFFU) << 11U;
    for (int bit = 22; bit >= 11; --bit) {
        if ((remainder >> bit) & 1U) {
            remainder ^= kM17GolayGenerator << (bit - 11);
        }
    }
    const std::uint32_t word23 = (static_cast<std::uint32_t>(data & 0xFFFU) << 11U) | remainder;
    const auto parity = static_cast<std::uint32_t>(std::popcount(word23) & 1);
    return (word23 << 1U) | parity;
}

M17GolayDecode m17_golay_decode(std::span<const float> soft) {
    M17GolayDecode best;
    if (soft.size() != 24) {
        return best;
    }
    std::uint32_t hard = 0;
    double total = 0.0;
    for (std::size_t i = 0; i < 24; ++i) {
        hard = (hard << 1U) | (soft[i] < 0.0F ? 1U : 0U);
        total += soft[i];
    }
    // metric(c) = sum over bits of soft * (+1 for a 0 in c, -1 for a 1),
    // which is the total less twice the soft values at c's ones.
    double best_metric = -1e300;
    const auto& table = golay_table();
    for (std::uint32_t data = 0; data < 4096; ++data) {
        const std::uint32_t word = table[data];
        double ones = 0.0;
        for (std::size_t i = 0; i < 24; ++i) {
            if ((word >> (23 - i)) & 1U) {
                ones += soft[i];
            }
        }
        const double metric = total - 2.0 * ones;
        if (metric > best_metric) {
            best_metric = metric;
            best.data = static_cast<std::uint16_t>(data);
            best.corrected_bits = static_cast<std::uint32_t>(std::popcount(word ^ hard));
        }
    }
    return best;
}

Expected<std::uint64_t> m17_encode_callsign(std::string_view callsign) {
    if (callsign.size() > 9) {
        return fail(std::format(
            "M17 Appendix A encodes at most nine characters into a 48-bit address; '{}' has {}",
            callsign, callsign.size()));
    }
    // A.2: the first character goes in the least significant digit, so the
    // Horner form runs from the last character to the first.
    std::uint64_t value = 0;
    for (std::size_t i = callsign.size(); i-- > 0;) {
        value = value * 40U + alphabet_value(callsign[i]);
    }
    return value;
}

M17Address m17_decode_address(std::uint64_t value) {
    M17Address address;
    address.value = value;
    if (value == 0U) {
        address.kind = M17AddressKind::Reserved;
        return address;
    }
    if (value == kBroadcastAddress) {
        address.kind = M17AddressKind::Broadcast;
        return address;
    }
    if (value >= kFirstExtendedAddress) {
        address.kind = M17AddressKind::Extended;
        return address;
    }
    address.kind = M17AddressKind::Standard;
    std::uint64_t rest = value;
    while (rest != 0U) {
        address.callsign.push_back(alphabet_char(static_cast<unsigned>(rest % 40U)));
        rest /= 40U;
    }
    while (!address.callsign.empty() && address.callsign.back() == ' ') {
        address.callsign.pop_back();
    }
    return address;
}

M17Type m17_parse_type(std::uint16_t raw) {
    M17Type type;
    type.raw = raw;
    type.stream = (raw & 0x1U) != 0U;
    type.data_type = static_cast<std::uint8_t>((raw >> 1U) & 0x3U);
    type.encryption_type = static_cast<std::uint8_t>((raw >> 3U) & 0x3U);
    type.encryption_subtype = static_cast<std::uint8_t>((raw >> 5U) & 0x3U);
    type.channel_access_number = static_cast<std::uint8_t>((raw >> 7U) & 0xFU);
    type.signed_stream = ((raw >> 11U) & 0x1U) != 0U;
    return type;
}

M17Lsf m17_parse_lsf(std::span<const std::uint8_t> bytes) {
    M17Lsf lsf;
    if (bytes.size() < kM17LsfBytes) {
        return lsf;
    }
    // Table 2.4 and 2.5, big endian per the note under 2.3.
    lsf.destination = m17_decode_address(read_be(bytes, 0, 6));
    lsf.source = m17_decode_address(read_be(bytes, 6, 6));
    lsf.type = m17_parse_type(static_cast<std::uint16_t>(read_be(bytes, 12, 2)));
    std::copy(bytes.begin() + 14, bytes.begin() + 28, lsf.meta.begin());
    lsf.crc = static_cast<std::uint16_t>(read_be(bytes, 28, 2));
    lsf.crc_valid = m17_crc(bytes.first(28)) == lsf.crc;
    return lsf;
}

std::array<std::uint8_t, kM17LsfBytes> m17_lsf_bytes(std::uint64_t destination,
                                                      std::uint64_t source, std::uint16_t type,
                                                      std::span<const std::uint8_t> meta) {
    std::array<std::uint8_t, kM17LsfBytes> bytes{};
    write_be(bytes, 0, 6, destination);
    write_be(bytes, 6, 6, source);
    write_be(bytes, 12, 2, type);
    std::copy_n(meta.begin(), std::min<std::size_t>(meta.size(), 14), bytes.begin() + 14);
    write_be(bytes, 28, 2, m17_crc(std::span<const std::uint8_t>(bytes).first(28)));
    return bytes;
}

void m17_randomize(std::span<std::uint8_t> bits) {
    // 1.4.4: payload bit i is XORed with bit (7 - i mod 8) of byte (i / 8),
    // restarting at byte 0 after byte 45.
    for (std::size_t i = 0; i < bits.size(); ++i) {
        const std::uint8_t byte = kM17Randomizer[(i / 8) % kM17Randomizer.size()];
        bits[i] = static_cast<std::uint8_t>(bits[i] ^ ((byte >> (7 - i % 8)) & 1U));
    }
}

std::array<float, kM17SyncSymbols> m17_word_symbols(std::uint16_t word) {
    std::array<float, kM17SyncSymbols> symbols{};
    for (std::size_t i = 0; i < kM17SyncSymbols; ++i) {
        const auto dibit = static_cast<std::size_t>((word >> (14 - 2 * i)) & 0x3U);
        symbols[i] = static_cast<float>(kM17DibitToSymbol[dibit]);
    }
    return symbols;
}

namespace {

std::vector<std::uint8_t> interleave(std::span<const std::uint8_t> type3) {
    std::vector<std::uint8_t> type4(kM17PayloadBits, 0);
    for (std::uint32_t i = 0; i < kM17PayloadBits; ++i) {
        type4[m17_interleave(i)] = type3[i];
    }
    return type4;
}

std::vector<float> deinterleave(std::span<const float> type4) {
    std::vector<float> type3(kM17PayloadBits, 0.0F);
    for (std::uint32_t i = 0; i < kM17PayloadBits; ++i) {
        type3[i] = type4[m17_interleave(i)];
    }
    return type3;
}

}  // namespace

Expected<std::vector<std::uint8_t>> m17_encode_lsf(std::span<const std::uint8_t> lsf) {
    if (lsf.size() != kM17LsfBytes) {
        return fail(std::format("an M17 LSF is {} bytes (2.5.2); got {}", kM17LsfBytes,
                                lsf.size()));
    }
    std::vector<std::uint8_t> bits;
    push_bytes(bits, lsf);
    bits.insert(bits.end(), kFlushBits, static_cast<std::uint8_t>(0));
    auto encoded = convolutional_encode(conv_code(), bits);
    if (!encoded) {
        return std::unexpected(with_context(encoded.error(), "encoding the M17 LSF"));
    }
    const auto p1 = m17_puncture_p1();
    const std::vector<std::uint8_t> type3 = puncture(*encoded, p1);
    if (type3.size() != kM17PayloadBits) {
        return fail(std::format("P1 puncturing left {} bits, not 368", type3.size()));
    }
    return interleave(type3);
}

Expected<std::array<std::uint8_t, kM17LsfBytes>> m17_decode_lsf(std::span<const float> soft) {
    if (soft.size() != kM17PayloadBits) {
        return fail(std::format("m17_decode_lsf needs 368 soft bits; got {}", soft.size()));
    }
    const std::vector<float> type3 = deinterleave(soft);
    const auto p1 = m17_puncture_p1();
    const std::vector<float> type2 = depuncture(type3, p1, kLsfEncodedBits);
    auto decoded = viterbi_decode(conv_code(), type2, true);
    if (!decoded) {
        return std::unexpected(with_context(decoded.error(), "decoding the M17 LSF"));
    }
    std::array<std::uint8_t, kM17LsfBytes> bytes{};
    for (std::size_t i = 0; i < kLsfBits; ++i) {
        bytes[i / 8] = static_cast<std::uint8_t>((bytes[i / 8] << 1U) | (*decoded)[i]);
    }
    return bytes;
}

Expected<std::vector<std::uint8_t>> m17_encode_stream_frame(
    std::span<const std::uint8_t> lich_chunk, std::uint8_t lich_count, std::uint16_t frame_number,
    std::span<const std::uint8_t> payload) {
    if (lich_chunk.size() != kM17LichChunkBytes || payload.size() != kM17StreamPayloadBytes ||
        lich_count >= kM17LichChunks) {
        return fail(std::format(
            "an M17 stream frame needs a 5-byte LICH chunk, a counter below 6 and 16 payload "
            "bytes (2.8.1); got {}, {} and {}",
            lich_chunk.size(), lich_count, payload.size()));
    }
    // Table 2.8: the 40-bit chunk, then LICH_CNT in the top three bits of
    // byte 5 and five reserved zero bits.
    std::vector<std::uint8_t> lich;
    push_bytes(lich, lich_chunk);
    push_bits(lich, static_cast<std::uint64_t>(lich_count) << 5U, 8);

    std::vector<std::uint8_t> combined;
    for (std::size_t part = 0; part < 4; ++part) {
        std::uint16_t data = 0;
        for (std::size_t b = 0; b < 12; ++b) {
            data = static_cast<std::uint16_t>((data << 1U) | lich[part * 12 + b]);
        }
        push_bits(combined, m17_golay_encode(data), 24);
    }

    std::vector<std::uint8_t> content;
    push_bits(content, frame_number, 16);
    push_bytes(content, payload);
    content.insert(content.end(), kFlushBits, static_cast<std::uint8_t>(0));
    auto encoded = convolutional_encode(conv_code(), content);
    if (!encoded) {
        return std::unexpected(with_context(encoded.error(), "encoding an M17 stream frame"));
    }
    const std::vector<std::uint8_t> punctured = puncture(*encoded, kM17PunctureP2);
    if (punctured.size() != kStreamPuncturedBits) {
        return fail(std::format("P2 puncturing left {} bits, not 272", punctured.size()));
    }
    combined.insert(combined.end(), punctured.begin(), punctured.end());
    return interleave(combined);
}

Expected<M17StreamFrame> m17_decode_stream_frame(std::span<const float> soft) {
    if (soft.size() != kM17PayloadBits) {
        return fail(std::format("m17_decode_stream_frame needs 368 soft bits; got {}",
                                soft.size()));
    }
    const std::vector<float> combined = deinterleave(soft);

    M17StreamFrame frame;
    std::array<std::uint8_t, 6> lich{};
    std::uint64_t lich_bits = 0;
    for (std::size_t part = 0; part < 4; ++part) {
        const M17GolayDecode word = m17_golay_decode(
            std::span<const float>(combined).subspan(part * 24, 24));
        frame.lich_worst_correction = std::max(frame.lich_worst_correction, word.corrected_bits);
        lich_bits = (lich_bits << 12U) | word.data;
    }
    for (std::size_t i = 0; i < 6; ++i) {
        lich[i] = static_cast<std::uint8_t>(lich_bits >> (8 * (5 - i)));
    }
    std::copy_n(lich.begin(), kM17LichChunkBytes, frame.lich_chunk.begin());
    frame.lich_count = static_cast<std::uint8_t>(lich[5] >> 5U);

    const std::vector<float> type2 =
        depuncture(std::span<const float>(combined).subspan(kLichEncodedBits),
                   kM17PunctureP2, kStreamEncodedBits);
    auto decoded = viterbi_decode(conv_code(), type2, true);
    if (!decoded) {
        return std::unexpected(with_context(decoded.error(), "decoding an M17 stream frame"));
    }
    std::uint16_t number = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        number = static_cast<std::uint16_t>((number << 1U) | (*decoded)[i]);
    }
    // 2.8.1: "The most significant bit in the FN is used for transmission end
    // signaling."
    frame.last = (number & 0x8000U) != 0U;
    frame.frame_number = static_cast<std::uint16_t>(number & 0x7FFFU);
    for (std::size_t i = 0; i < 128; ++i) {
        auto& byte = frame.payload[i / 8];
        byte = static_cast<std::uint8_t>((byte << 1U) | (*decoded)[16 + i]);
    }
    return frame;
}

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

Expected<M17> M17::create(const M17Config& config) {
    if (config.rate <= 0) {
        return fail(std::format("M17 needs a positive sample rate; got {}", config.rate));
    }
    const double samples_per_symbol = static_cast<double>(config.rate) / kM17SymbolRate;
    if (samples_per_symbol < 2.0) {
        return fail(std::format(
            "M17 is 4800 symbols per second (M17 1.1), so {} Hz gives {:.3f} samples per "
            "symbol, below the two the timing recovery needs",
            config.rate, samples_per_symbol));
    }
    auto taps_count =
        static_cast<std::size_t>(std::ceil(kM17RrcSpanSymbols * samples_per_symbol)) | 1U;
    auto taps = design_rrc(config.rate, kM17SymbolRate, kM17RrcRollOff, taps_count);
    if (!taps) {
        return std::unexpected(with_context(taps.error(), "designing the M17 1.3 filter"));
    }
    SymbolSyncConfig sync;
    sync.rate = config.rate;
    sync.symbol_rate = kM17SymbolRate;
    auto timing = SymbolSync::create(sync);
    if (!timing) {
        return std::unexpected(with_context(timing.error(), "creating the M17 timing"));
    }

    M17 decoder;
    decoder.config_ = config;
    decoder.taps_ = std::move(*taps);
    decoder.sync_ = std::move(*timing);
    return decoder;
}

void M17::reset() {
    sync_.reset();
    previous_ = Complex32{1.0F, 0.0F};
    history_.clear();
    samples_in_ = 0;
    symbols_.clear();
    positions_.clear();
    cursor_ = 0;
    in_transmission_ = false;
    inverted_ = false;
    have_lsf_ = false;
    calibrated_ = false;
    gain_ = 1.0;
    level_offset_ = 0.0;
    chunk_mask_ = 0;
    last_symbols_.clear();
}

Status M17::process(ConstComplexSpan samples, std::vector<M17Frame>& out) {
    last_symbols_.clear();
    if (samples.empty()) {
        return {};
    }

    const double rate = static_cast<double>(config_.rate);
    const std::size_t taps = taps_.size();

    // The receive filter's gain for a transmitter whose own shaping filter
    // has unit gain at zero hertz, which is what makes a run of +3 symbols
    // deviate by the 2.4 kHz of Table 1.1: the cascade then puts a symbol s at
    // s * 800 * (samples per symbol) / (sum of the receive taps). Dividing it
    // out puts the levels near +/-1 and +/-3; the per-frame calibration in
    // soft_payload removes whatever scale and offset remain.
    double tap_sum = 0.0;
    for (const float tap : taps_) {
        tap_sum += static_cast<double>(tap);
    }
    const double scale =
        tap_sum / (kM17DeviationPerUnitHz * (rate / kM17SymbolRate));

    std::vector<Complex32> shaped;
    shaped.reserve(samples.size());
    for (const Complex32 sample : samples) {
        const Complex32 product = sample * std::conj(previous_);
        previous_ = sample;
        const double hertz =
            std::atan2(static_cast<double>(product.imag()), static_cast<double>(product.real())) *
            rate / (2.0 * kPi);
        history_.push_back(static_cast<float>(hertz));
        if (history_.size() > taps) {
            history_.erase(history_.begin());
        }
        double sum = 0.0;
        const std::size_t have = history_.size();
        for (std::size_t i = 0; i < have; ++i) {
            sum += static_cast<double>(taps_[i]) * history_[have - 1 - i];
        }
        shaped.push_back(Complex32{static_cast<float>(sum * scale), 0.0F});
    }
    samples_in_ += samples.size();

    recovered_.clear();
    sync_.process(shaped, recovered_);
    const double delay = static_cast<double>(taps - 1) / 2.0;
    for (const RecoveredSymbol& symbol : recovered_) {
        symbols_.push_back(symbol.value.real());
        const double centre = symbol.position - delay;
        positions_.push_back(centre > 0.0 ? static_cast<SampleIndex>(std::llround(centre)) : 0);
        last_symbols_.push_back(symbol.value.real());
    }

    search(out);

    // Keep the calibration window behind the cursor and nothing older.
    const std::size_t keep_back = kPreambleCalibrationSymbols + kM17SyncSymbols + 2;
    if (cursor_ > keep_back) {
        const std::size_t drop = cursor_ - keep_back;
        symbols_.erase(symbols_.begin(), symbols_.begin() + static_cast<std::ptrdiff_t>(drop));
        positions_.erase(positions_.begin(),
                         positions_.begin() + static_cast<std::ptrdiff_t>(drop));
        cursor_ -= drop;
    }
    return {};
}

double M17::score_at(std::size_t offset, std::uint16_t word) const {
    const auto pattern = m17_word_symbols(word);
    return correlation_at(symbols_, pattern, offset);
}

std::vector<float> M17::soft_payload(std::size_t offset, bool inverted, std::uint16_t word) {
    // Least squares fit of y = a*p + b over the symbols whose values are
    // known: the sync burst, and for an LSF the end of the preamble before
    // it. a absorbs the deviation scale and b the carrier offset, which a
    // frequency discriminator turns into a constant.
    std::vector<float> known;
    std::vector<float> seen;
    const float sign = inverted ? -1.0F : 1.0F;
    if (word == kM17SyncLsf && offset >= kPreambleCalibrationSymbols) {
        const std::vector<float> tail = lsf_preamble_tail(kPreambleCalibrationSymbols);
        for (std::size_t i = 0; i < tail.size(); ++i) {
            known.push_back(tail[i]);
            seen.push_back(sign * symbols_[offset - kPreambleCalibrationSymbols + i]);
        }
    }
    const auto pattern = m17_word_symbols(word);
    for (std::size_t i = 0; i < kM17SyncSymbols; ++i) {
        known.push_back(pattern[i]);
        seen.push_back(sign * symbols_[offset + i]);
    }
    double mean_p = 0.0;
    double mean_y = 0.0;
    for (std::size_t i = 0; i < known.size(); ++i) {
        mean_p += known[i];
        mean_y += seen[i];
    }
    mean_p /= static_cast<double>(known.size());
    mean_y /= static_cast<double>(known.size());
    double cov = 0.0;
    double var = 0.0;
    for (std::size_t i = 0; i < known.size(); ++i) {
        cov += (known[i] - mean_p) * (seen[i] - mean_y);
        var += (known[i] - mean_p) * (known[i] - mean_p);
    }
    double gain = (var > 0.0) ? cov / var : 1.0;
    double offset_level = mean_y - gain * mean_p;
    if (!(gain > 0.05)) {
        gain = 1.0;
        offset_level = 0.0;
    }
    if (calibrated_) {
        gain = (1.0 - kCalibrationLeak) * gain_ + kCalibrationLeak * gain;
        offset_level = (1.0 - kCalibrationLeak) * level_offset_ + kCalibrationLeak * offset_level;
    }
    calibrated_ = true;
    gain_ = gain;
    level_offset_ = offset_level;

    // Table 1.1: the first bit of a dibit is its sign, 0 for the positive
    // pair; the second is 1 for the outer levels. In the dv_codes convention
    // a positive soft value leans to 0.
    std::vector<float> soft;
    soft.reserve(kM17PayloadBits);
    for (std::size_t k = 0; k < kM17PayloadBits / 2; ++k) {
        const double z =
            (sign * symbols_[offset + kM17SyncSymbols + k] - offset_level) / gain;
        soft.push_back(static_cast<float>(std::clamp(z, -kSoftClip, kSoftClip)));
        soft.push_back(static_cast<float>(std::clamp(2.0 - std::abs(z), -kSoftClip, kSoftClip)));
    }
    // 1.4.4: undo the randomiser. A 1 in the sequence flips the bit, so it
    // flips the sign of the soft value.
    for (std::size_t i = 0; i < soft.size(); ++i) {
        const std::uint8_t byte = kM17Randomizer[(i / 8) % kM17Randomizer.size()];
        if ((byte >> (7 - i % 8)) & 1U) {
            soft[i] = -soft[i];
        }
    }
    return soft;
}

void M17::search(std::vector<M17Frame>& out) {
    const double threshold = config_.sync_threshold;
    while (true) {
        if (in_transmission_) {
            // A frame is due at the cursor. Look one symbol either side for
            // the timing to have slipped, and read which burst is there.
            if (cursor_ + kM17FrameSymbols + 2 > symbols_.size()) {
                return;
            }
            const std::uint16_t words[] = {kM17SyncStream, kM17SyncPacket, kM17SyncBert,
                                           kM17EotWord};
            double best = -1.0;
            std::size_t best_offset = cursor_;
            std::uint16_t best_word = kM17SyncStream;
            const std::size_t first = cursor_ > 0 ? cursor_ - 1 : cursor_;
            for (std::size_t offset = first; offset <= cursor_ + 1; ++offset) {
                for (const std::uint16_t word : words) {
                    const double score = score_at(offset, word) * (inverted_ ? -1.0 : 1.0);
                    if (score > best) {
                        best = score;
                        best_offset = offset;
                        best_word = word;
                    }
                }
            }
            if (best < config_.tracking_threshold) {
                // Lost it. Go back to searching from here.
                in_transmission_ = false;
                continue;
            }
            M17Frame frame;
            frame.first_sample = positions_[best_offset];
            frame.sync_score = best;
            frame.inverted = inverted_;
            if (best_word == kM17EotWord) {
                frame.kind = M17FrameKind::EndOfTransmission;
                out.push_back(std::move(frame));
                in_transmission_ = false;
                cursor_ = best_offset + kM17SyncSymbols;
                continue;
            }
            if (best_word == kM17SyncStream) {
                decode_stream(best_offset, frame);
            } else {
                frame.kind = (best_word == kM17SyncPacket) ? M17FrameKind::Packet
                                                           : M17FrameKind::Bert;
            }
            out.push_back(std::move(frame));
            cursor_ = best_offset + kM17FrameSymbols;
            continue;
        }

        // Not in a transmission: search for one starting.
        bool started = false;
        while (cursor_ + 2 * kM17FrameSymbols + kM17SyncSymbols <= symbols_.size()) {
            const std::size_t p = cursor_;
            const double lsf = score_at(p, kM17SyncLsf);
            if (std::abs(lsf) >= threshold && p >= kM17SyncSymbols) {
                // 1.4.1: the preamble's last symbols alternate and end
                // opposite the burst's first. Checking eight of them is what
                // makes an eight-symbol burst safe to accept on its own.
                const std::vector<float> tail = lsf_preamble_tail(kM17SyncSymbols);
                const double preamble = correlation_at(symbols_, tail, p - kM17SyncSymbols);
                if (preamble * lsf >= threshold * threshold) {
                    inverted_ = lsf < 0.0;
                    calibrated_ = false;
                    M17Frame frame;
                    frame.kind = M17FrameKind::LinkSetup;
                    frame.first_sample = positions_[p];
                    frame.sync_score = std::abs(lsf);
                    frame.inverted = inverted_;
                    const std::vector<float> soft = soft_payload(p, inverted_, kM17SyncLsf);
                    if (auto bytes = m17_decode_lsf(soft); bytes) {
                        frame.lsf = m17_parse_lsf(*bytes);
                        have_lsf_ = frame.lsf->crc_valid;
                    }
                    chunk_mask_ = 0;
                    out.push_back(std::move(frame));
                    in_transmission_ = true;
                    cursor_ = p + kM17FrameSymbols;
                    started = true;
                    break;
                }
            }
            const double stream = score_at(p, kM17SyncStream);
            if (std::abs(stream) >= threshold) {
                // A late join: no LSF, so the evidence is a second burst a
                // frame later, a stream burst or the end marker.
                const double sign = stream < 0.0 ? -1.0 : 1.0;
                const double next = std::max(sign * score_at(p + kM17FrameSymbols, kM17SyncStream),
                                             sign * score_at(p + kM17FrameSymbols, kM17EotWord));
                if (next >= threshold) {
                    inverted_ = stream < 0.0;
                    calibrated_ = false;
                    have_lsf_ = false;
                    chunk_mask_ = 0;
                    in_transmission_ = true;
                    started = true;
                    break;
                }
            }
            ++cursor_;
        }
        if (!started) {
            return;
        }
    }
}

void M17::decode_stream(std::size_t offset, M17Frame& frame) {
    frame.kind = M17FrameKind::Stream;
    const std::vector<float> soft = soft_payload(offset, inverted_, kM17SyncStream);
    auto decoded = m17_decode_stream_frame(soft);
    if (!decoded) {
        return;
    }
    // Table 2.9: collect the six chunks. A chunk whose Golay words needed
    // more than the three corrections the code guarantees is not trusted.
    if (decoded->lich_count < kM17LichChunks && decoded->lich_worst_correction <= 3) {
        chunks_[decoded->lich_count] = decoded->lich_chunk;
        chunk_mask_ = static_cast<std::uint8_t>(chunk_mask_ | (1U << decoded->lich_count));
    }
    if (!have_lsf_ && chunk_mask_ == 0x3FU) {
        std::array<std::uint8_t, kM17LsfBytes> bytes{};
        for (std::size_t chunk = 0; chunk < kM17LichChunks; ++chunk) {
            std::copy(chunks_[chunk].begin(), chunks_[chunk].end(),
                      bytes.begin() + static_cast<std::ptrdiff_t>(chunk * kM17LichChunkBytes));
        }
        const M17Lsf lsf = m17_parse_lsf(bytes);
        if (lsf.crc_valid) {
            frame.lsf = lsf;
            frame.lsf_from_lich = true;
            have_lsf_ = true;
        }
    }
    frame.stream = *decoded;
}

}  // namespace revenant::decode
