#include "core/decode/ccsds.h"

#include <algorithm>
#include <bit>
#include <format>
#include <utility>

#include "core/decode/crc.h"

namespace revenant::decode {

namespace {

// ---------------------------------------------------------------------------
// The conventional field of 131.0 4.3.3: F(x) = x^8 + x^7 + x^2 + x + 1.
// ---------------------------------------------------------------------------

constexpr unsigned kFieldPolynomial = 0x187;

constexpr std::uint8_t gf_multiply(std::uint8_t a, std::uint8_t b) {
    unsigned x = a;
    unsigned r = 0;
    while (b != 0U) {
        if ((b & 1U) != 0U) {
            r ^= x;
        }
        b = static_cast<std::uint8_t>(b >> 1U);
        x <<= 1U;
        if ((x & 0x100U) != 0U) {
            x ^= kFieldPolynomial;
        }
    }
    return static_cast<std::uint8_t>(r);
}

constexpr std::uint8_t gf_power(std::uint8_t a, unsigned e) {
    std::uint8_t r = 1;
    for (unsigned i = 0; i < e; ++i) {
        r = gf_multiply(r, a);
    }
    return r;
}

// 131.0 4.4.2: Tr(z) = sum of z^(2^k), k = 0..7; always 0 or 1.
constexpr std::uint8_t gf_trace(std::uint8_t z) {
    std::uint8_t s = 0;
    std::uint8_t x = z;
    for (int k = 0; k < 8; ++k) {
        s ^= x;
        x = gf_multiply(x, x);
    }
    return s;
}

struct DualTables {
    std::array<std::uint8_t, 256> to_dual{};
    std::array<std::uint8_t, 256> to_conventional{};
};

// 131.0 4.4.2: beta = alpha^117 and z_j = Tr(w beta^j), z_0 the first bit.
DualTables build_dual_tables() {
    DualTables t;
    std::array<std::uint8_t, 8> beta_powers{};
    const std::uint8_t beta = gf_power(2, 117);
    for (unsigned j = 0; j < 8; ++j) {
        beta_powers[j] = gf_power(beta, j);
    }
    for (unsigned w = 0; w < 256; ++w) {
        unsigned z = 0;
        for (unsigned j = 0; j < 8; ++j) {
            z |= static_cast<unsigned>(gf_trace(gf_multiply(static_cast<std::uint8_t>(w), beta_powers[j])))
                 << (7U - j);
        }
        t.to_dual[w] = static_cast<std::uint8_t>(z);
        t.to_conventional[z] = static_cast<std::uint8_t>(w);
    }
    return t;
}

const DualTables& dual_tables() {
    static const DualTables tables = build_dual_tables();
    return tables;
}

// The minimal polynomial of gamma = alpha^11: the degree-8 polynomial with
// gamma as a root. gamma is primitive, so it is unique and has degree 8.
unsigned gamma_polynomial() {
    const std::uint8_t gamma = gf_power(2, 11);
    for (unsigned p = 0x100; p < 0x200; ++p) {
        std::uint8_t sum = 0;
        std::uint8_t term = 1;
        for (unsigned i = 0; i <= 8; ++i) {
            if (((p >> i) & 1U) != 0U) {
                sum ^= term;
            }
            term = gf_multiply(term, gamma);
        }
        if (sum == 0) {
            return p;
        }
    }
    return 0;
}

// Conventional (alpha basis) to and from the codec's field, where gamma is
// written y. alpha = gamma^116 since 11 * 116 = 1276 = 5 * 255 + 1.
struct CodecMap {
    std::array<std::uint8_t, 256> dual_to_codec{};
    std::array<std::uint8_t, 256> codec_to_dual{};
    std::array<std::uint8_t, 256> conventional_to_codec{};
    std::array<std::uint8_t, 256> codec_to_conventional{};
};

CodecMap build_codec_map(const Rs8Codec& codec) {
    CodecMap m;
    std::array<std::uint8_t, 8> image{};
    for (unsigned i = 0; i < 8; ++i) {
        image[i] = codec.alpha_power((116U * i) % 255U);
    }
    const auto& dual = dual_tables();
    for (unsigned u = 0; u < 256; ++u) {
        std::uint8_t c = 0;
        for (unsigned i = 0; i < 8; ++i) {
            if (((u >> i) & 1U) != 0U) {
                c ^= image[i];
            }
        }
        const std::uint8_t z = dual.to_dual[u];
        m.dual_to_codec[z] = c;
        m.codec_to_dual[c] = z;
        m.conventional_to_codec[u] = c;
        m.codec_to_conventional[c] = static_cast<std::uint8_t>(u);
    }
    return m;
}

const CodecMap& codec_map(const Rs8Codec& codec) {
    // Every CCSDS codec shares the field polynomial, so one map serves both E.
    static const CodecMap map = build_codec_map(codec);
    return map;
}

// ---------------------------------------------------------------------------
// Pseudo-randomiser
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> pn_bits_uncached(CcsdsRandomizer randomizer, std::size_t bits) {
    std::vector<std::uint8_t> s;
    s.reserve(bits);
    if (randomizer == CcsdsRandomizer::Legacy255) {
        // 10.4.3: all ones; s[n+8] = s[n+7] + s[n+5] + s[n+3] + s[n] from h(x).
        for (std::size_t i = 0; i < std::min<std::size_t>(8, bits); ++i) {
            s.push_back(1);
        }
        while (s.size() < bits) {
            const std::size_t n = s.size() - 8;
            s.push_back(static_cast<std::uint8_t>(s[n + 7] ^ s[n + 5] ^ s[n + 3] ^ s[n]));
        }
    } else if (randomizer == CcsdsRandomizer::Long131071) {
        // 10.4.3: the seed 11000111000111000 reads X16 first in figure 10-2;
        // the sequence leaves from X0, so its first 17 bits are the seed
        // reversed, matching the 40 bits printed in note 2.
        constexpr char kSeed[] = "11000111000111000";
        for (std::size_t i = 0; i < std::min<std::size_t>(17, bits); ++i) {
            s.push_back(static_cast<std::uint8_t>(kSeed[16 - i] - '0'));
        }
        while (s.size() < bits) {
            const std::size_t n = s.size() - 17;
            s.push_back(static_cast<std::uint8_t>(s[n + 14] ^ s[n]));
        }
    } else {
        s.assign(bits, 0);
    }
    return s;
}

}  // namespace

std::vector<std::uint8_t> ccsds_pn_bits(CcsdsRandomizer randomizer, std::size_t bits) {
    return pn_bits_uncached(randomizer, bits);
}

void ccsds_randomize(std::span<std::uint8_t> octets, CcsdsRandomizer randomizer) {
    if (randomizer == CcsdsRandomizer::None) {
        return;
    }
    // Cache one period's worth of octets per randomiser; the 131071-bit one
    // is longer than any codeblock (8 * 255 * 8 bits) so no wrap is needed
    // for it, and the 255-bit one is extended to cover the longest block.
    static const std::vector<std::uint8_t> legacy = [] {
        const auto b = pn_bits_uncached(CcsdsRandomizer::Legacy255, 8 * 2040 * 8);
        std::vector<std::uint8_t> o(b.size() / 8);
        for (std::size_t i = 0; i < b.size(); ++i) {
            o[i / 8] = static_cast<std::uint8_t>(o[i / 8] | (b[i] << (7U - (i % 8U))));
        }
        return o;
    }();
    static const std::vector<std::uint8_t> long_seq = [] {
        const auto b = pn_bits_uncached(CcsdsRandomizer::Long131071, 131064);
        std::vector<std::uint8_t> o(b.size() / 8);
        for (std::size_t i = 0; i < b.size(); ++i) {
            o[i / 8] = static_cast<std::uint8_t>(o[i / 8] | (b[i] << (7U - (i % 8U))));
        }
        return o;
    }();
    const auto& seq = randomizer == CcsdsRandomizer::Legacy255 ? legacy : long_seq;
    if (octets.size() <= seq.size()) {
        for (std::size_t i = 0; i < octets.size(); ++i) {
            octets[i] ^= seq[i];
        }
        return;
    }
    const auto b = pn_bits_uncached(randomizer, octets.size() * 8);
    for (std::size_t i = 0; i < b.size(); ++i) {
        octets[i / 8] = static_cast<std::uint8_t>(octets[i / 8] ^ (b[i] << (7U - (i % 8U))));
    }
}

// ---------------------------------------------------------------------------
// Reed-Solomon
// ---------------------------------------------------------------------------

bool CcsdsRsResult::ok() const {
    return std::all_of(corrected.begin(), corrected.end(), [](const auto& c) { return c.has_value(); });
}

std::size_t CcsdsRsResult::total_corrected() const {
    std::size_t n = 0;
    for (const auto& c : corrected) {
        n += c.value_or(0);
    }
    return n;
}

std::uint8_t CcsdsReedSolomon::conventional_to_dual(std::uint8_t u) { return dual_tables().to_dual[u]; }

std::uint8_t CcsdsReedSolomon::dual_to_conventional(std::uint8_t z) {
    return dual_tables().to_conventional[z];
}

std::uint8_t CcsdsReedSolomon::alpha_power(unsigned exponent) { return gf_power(2, exponent % 255U); }

std::vector<std::uint8_t> CcsdsReedSolomon::generator(unsigned e) {
    // 131.0 4.3.4: product of (x - alpha^(11j)), j = 128 - E .. 127 + E.
    std::vector<std::uint8_t> g{1};
    for (unsigned j = 128 - e; j <= 127 + e; ++j) {
        const std::uint8_t root = gf_power(2, (11U * j) % 255U);
        std::vector<std::uint8_t> next(g.size() + 1, 0);
        for (std::size_t i = 0; i < g.size(); ++i) {
            next[i + 1] ^= g[i];
            next[i] ^= gf_multiply(g[i], root);
        }
        g = std::move(next);
    }
    return g;
}

Expected<CcsdsReedSolomon> CcsdsReedSolomon::create(const CcsdsRsConfig& config) {
    if (config.e != 16 && config.e != 8) {
        return fail(std::format("CCSDS Reed-Solomon: E must be 16 or 8 (131.0 4.3.1), not {}", config.e));
    }
    const unsigned i = config.interleave;
    if (!(i >= 1 && i <= 5) && i != 8) {
        return fail(std::format("CCSDS Reed-Solomon: interleave depth {} is not 1-5 or 8 (131.0 4.3.5.1)", i));
    }
    const std::size_t k = 255 - 2 * config.e;
    if (config.virtual_fill % i != 0 || config.virtual_fill >= k * i) {
        return fail("CCSDS Reed-Solomon: virtual fill must be a multiple of I and less than kI (131.0 4.3.7.3)");
    }
    const unsigned poly = gamma_polynomial();
    auto codec = Rs8Codec::create({poly, 128 - config.e, 2 * static_cast<std::size_t>(config.e)});
    if (!codec) {
        return std::unexpected(codec.error());
    }
    CcsdsReedSolomon rs;
    rs.config_ = config;
    rs.codec_ = std::move(*codec);
    (void)codec_map(*rs.codec_);
    return rs;
}

std::size_t CcsdsReedSolomon::data_octets() const {
    return (255 - 2 * config_.e) * config_.interleave - config_.virtual_fill;
}

std::size_t CcsdsReedSolomon::codeblock_octets() const {
    return 255 * config_.interleave - config_.virtual_fill;
}

std::vector<std::uint8_t> CcsdsReedSolomon::encode(std::span<const std::uint8_t> data) const {
    const std::size_t depth = config_.interleave;
    const std::size_t q = config_.virtual_fill / depth;  // fill per codeword
    const std::size_t k = 255 - 2 * config_.e;
    const std::size_t parity = 2 * config_.e;
    const auto& map = codec_map(*codec_);
    const auto& to_codec = config_.dual_basis ? map.dual_to_codec : map.conventional_to_codec;
    const auto& from_codec = config_.dual_basis ? map.codec_to_dual : map.codec_to_conventional;

    std::vector<std::uint8_t> out(codeblock_octets(), 0);
    std::copy(data.begin(), data.end(), out.begin());
    std::vector<std::uint8_t> info(k - q);
    std::vector<std::uint8_t> check(parity);
    for (std::size_t c = 0; c < depth; ++c) {
        for (std::size_t s = 0; s < k - q; ++s) {
            info[s] = to_codec[data[s * depth + c]];
        }
        codec_->encode(info, check);
        for (std::size_t s = 0; s < parity; ++s) {
            out[data.size() + s * depth + c] = from_codec[check[s]];
        }
    }
    return out;
}

CcsdsRsResult CcsdsReedSolomon::decode(std::span<std::uint8_t> codeblock) const {
    const std::size_t depth = config_.interleave;
    const std::size_t q = config_.virtual_fill / depth;
    const std::size_t len = 255 - q;
    const auto& map = codec_map(*codec_);
    const auto& to_codec = config_.dual_basis ? map.dual_to_codec : map.conventional_to_codec;
    const auto& from_codec = config_.dual_basis ? map.codec_to_dual : map.codec_to_conventional;

    CcsdsRsResult result;
    std::vector<std::uint8_t> word(len);
    for (std::size_t c = 0; c < depth; ++c) {
        for (std::size_t s = 0; s < len; ++s) {
            word[s] = to_codec[codeblock[s * depth + c]];
        }
        const auto fixed = codec_->decode(word);
        if (fixed && *fixed > 0) {
            for (std::size_t s = 0; s < len; ++s) {
                codeblock[s * depth + c] = from_codec[word[s]];
            }
        }
        result.corrected.push_back(fixed);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Frames and packets
// ---------------------------------------------------------------------------

std::uint16_t ccsds_fecf(std::span<const std::uint8_t> octets) {
    static constexpr Crc kCrc{crc_catalogue::kCrc16CcittFalse};
    return static_cast<std::uint16_t>(kCrc.compute(octets));
}

bool ccsds_fecf_ok(std::span<const std::uint8_t> frame) {
    if (frame.size() < 3) {
        return false;
    }
    const std::size_t n = frame.size() - 2;
    const auto want = static_cast<std::uint16_t>((frame[n] << 8U) | frame[n + 1]);
    return ccsds_fecf(frame.first(n)) == want;
}

std::optional<TmFrameHeader> parse_tm_frame_header(std::span<const std::uint8_t> f) {
    if (f.size() < kTmFrameHeaderOctets) {
        return std::nullopt;
    }
    TmFrameHeader h;
    h.version = static_cast<std::uint8_t>(f[0] >> 6U);
    h.spacecraft_id = static_cast<std::uint16_t>(((f[0] & 0x3FU) << 4U) | (f[1] >> 4U));
    h.virtual_channel = static_cast<std::uint8_t>((f[1] >> 1U) & 7U);
    h.ocf_present = (f[1] & 1U) != 0U;
    h.master_frame_count = f[2];
    h.vc_frame_count = f[3];
    h.secondary_header = (f[4] & 0x80U) != 0U;
    h.sync_flag = (f[4] & 0x40U) != 0U;
    h.packet_order = (f[4] & 0x20U) != 0U;
    h.segment_length_id = static_cast<std::uint8_t>((f[4] >> 3U) & 3U);
    h.first_header_pointer = static_cast<std::uint16_t>(((f[4] & 7U) << 8U) | f[5]);
    if (h.version != 0) {
        return std::nullopt;
    }
    return h;
}

void write_tm_frame_header(const TmFrameHeader& h, std::span<std::uint8_t> o) {
    o[0] = static_cast<std::uint8_t>(((h.version & 3U) << 6U) | ((h.spacecraft_id >> 4U) & 0x3FU));
    o[1] = static_cast<std::uint8_t>(((h.spacecraft_id & 0xFU) << 4U) | ((h.virtual_channel & 7U) << 1U) |
                                     (h.ocf_present ? 1U : 0U));
    o[2] = h.master_frame_count;
    o[3] = h.vc_frame_count;
    o[4] = static_cast<std::uint8_t>((h.secondary_header ? 0x80U : 0U) | (h.sync_flag ? 0x40U : 0U) |
                                     (h.packet_order ? 0x20U : 0U) | ((h.segment_length_id & 3U) << 3U) |
                                     ((h.first_header_pointer >> 8U) & 7U));
    o[5] = static_cast<std::uint8_t>(h.first_header_pointer & 0xFFU);
}

std::optional<AosFrameHeader> parse_aos_frame_header(std::span<const std::uint8_t> f) {
    if (f.size() < kAosFrameHeaderOctets) {
        return std::nullopt;
    }
    AosFrameHeader h;
    h.version = static_cast<std::uint8_t>(f[0] >> 6U);
    h.spacecraft_id = static_cast<std::uint8_t>(((f[0] & 0x3FU) << 2U) | (f[1] >> 6U));
    h.virtual_channel = static_cast<std::uint8_t>(f[1] & 0x3FU);
    h.vc_frame_count = (static_cast<std::uint32_t>(f[2]) << 16U) | (static_cast<std::uint32_t>(f[3]) << 8U) | f[4];
    h.replay = (f[5] & 0x80U) != 0U;
    h.vc_count_cycle_used = (f[5] & 0x40U) != 0U;
    h.vc_count_cycle = static_cast<std::uint8_t>(f[5] & 0x0FU);
    if (h.version != 1) {
        return std::nullopt;
    }
    return h;
}

void write_aos_frame_header(const AosFrameHeader& h, std::span<std::uint8_t> o) {
    o[0] = static_cast<std::uint8_t>(((h.version & 3U) << 6U) | (h.spacecraft_id >> 2U));
    o[1] = static_cast<std::uint8_t>(((h.spacecraft_id & 3U) << 6U) | (h.virtual_channel & 0x3FU));
    o[2] = static_cast<std::uint8_t>(h.vc_frame_count >> 16U);
    o[3] = static_cast<std::uint8_t>(h.vc_frame_count >> 8U);
    o[4] = static_cast<std::uint8_t>(h.vc_frame_count);
    o[5] = static_cast<std::uint8_t>((h.replay ? 0x80U : 0U) | (h.vc_count_cycle_used ? 0x40U : 0U) |
                                     (h.vc_count_cycle & 0x0FU));
}

std::optional<std::uint16_t> parse_mpdu_first_header_pointer(std::span<const std::uint8_t> m) {
    if (m.size() < 2) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(((m[0] & 7U) << 8U) | m[1]);
}

std::optional<SpacePacketHeader> parse_space_packet_header(std::span<const std::uint8_t> p) {
    if (p.size() < kSpacePacketHeaderOctets) {
        return std::nullopt;
    }
    SpacePacketHeader h;
    h.version = static_cast<std::uint8_t>(p[0] >> 5U);
    h.telecommand = (p[0] & 0x10U) != 0U;
    h.secondary_header = (p[0] & 0x08U) != 0U;
    h.apid = static_cast<std::uint16_t>(((p[0] & 7U) << 8U) | p[1]);
    h.sequence_flags = static_cast<std::uint8_t>(p[2] >> 6U);
    h.sequence_count = static_cast<std::uint16_t>(((p[2] & 0x3FU) << 8U) | p[3]);
    h.data_length = static_cast<std::uint16_t>((p[4] << 8U) | p[5]);
    if (h.version != 0) {
        return std::nullopt;
    }
    return h;
}

void write_space_packet_header(const SpacePacketHeader& h, std::span<std::uint8_t> o) {
    o[0] = static_cast<std::uint8_t>(((h.version & 7U) << 5U) | (h.telecommand ? 0x10U : 0U) |
                                     (h.secondary_header ? 0x08U : 0U) | ((h.apid >> 8U) & 7U));
    o[1] = static_cast<std::uint8_t>(h.apid & 0xFFU);
    o[2] = static_cast<std::uint8_t>(((h.sequence_flags & 3U) << 6U) | ((h.sequence_count >> 8U) & 0x3FU));
    o[3] = static_cast<std::uint8_t>(h.sequence_count & 0xFFU);
    o[4] = static_cast<std::uint8_t>(h.data_length >> 8U);
    o[5] = static_cast<std::uint8_t>(h.data_length & 0xFFU);
}

void SpacePacketExtractor::reset() {
    partial_.clear();
    in_sync_ = false;
}

void SpacePacketExtractor::consume(std::span<const std::uint8_t> bytes, std::vector<SpacePacket>& out) {
    std::size_t i = 0;
    while (i < bytes.size()) {
        if (partial_.size() < kSpacePacketHeaderOctets) {
            const std::size_t take = std::min(kSpacePacketHeaderOctets - partial_.size(), bytes.size() - i);
            partial_.insert(partial_.end(), bytes.begin() + static_cast<std::ptrdiff_t>(i),
                            bytes.begin() + static_cast<std::ptrdiff_t>(i + take));
            i += take;
            if (partial_.size() < kSpacePacketHeaderOctets) {
                return;
            }
            if (!parse_space_packet_header(partial_)) {
                // Not a version-0 header: sync is lost until the next pointer.
                partial_.clear();
                in_sync_ = false;
                ++dropped_;
                return;
            }
        }
        const auto header = *parse_space_packet_header(partial_);
        const std::size_t need = header.total_octets() - partial_.size();
        const std::size_t take = std::min(need, bytes.size() - i);
        partial_.insert(partial_.end(), bytes.begin() + static_cast<std::ptrdiff_t>(i),
                        bytes.begin() + static_cast<std::ptrdiff_t>(i + take));
        i += take;
        if (partial_.size() == header.total_octets()) {
            if (header.apid != kIdleApid) {
                out.push_back({header, std::move(partial_)});
            }
            partial_.clear();
        }
    }
}

void SpacePacketExtractor::push(std::span<const std::uint8_t> zone, std::uint16_t fhp, bool continuous,
                                std::vector<SpacePacket>& out) {
    if (fhp == kCcsdsOnlyIdleData) {
        return;
    }
    if (!continuous && (!partial_.empty() || in_sync_)) {
        if (!partial_.empty()) {
            ++dropped_;
        }
        partial_.clear();
        in_sync_ = false;
    }
    if (fhp == kCcsdsNoPacketStarts) {
        if (in_sync_) {
            consume(zone, out);
        }
        return;
    }
    if (fhp >= zone.size()) {
        reset();
        return;
    }
    if (in_sync_) {
        consume(zone.first(fhp), out);
        if (!partial_.empty()) {
            // The pointer says a packet starts here, but the one in progress
            // has not ended: one of the two is wrong, and the pointer wins.
            partial_.clear();
            ++dropped_;
        }
    }
    in_sync_ = true;
    consume(zone.subspan(fhp), out);
}

// ---------------------------------------------------------------------------
// The receiver
// ---------------------------------------------------------------------------

Expected<CcsdsReceiver> CcsdsReceiver::create(const CcsdsReceiverConfig& config) {
    CcsdsReceiver r;
    r.config_ = config;
    if (config.reed_solomon) {
        auto rs = CcsdsReedSolomon::create(config.rs);
        if (!rs) {
            return std::unexpected(rs.error());
        }
        r.rs_ = std::move(*rs);
        r.frame_octets_ = r.rs_->data_octets();
        r.codeblock_octets_ = r.rs_->codeblock_octets();
    } else {
        if (config.frame_octets == 0 || config.frame_octets > 65536) {
            return fail("CCSDS receiver: frame_octets must be 1 to 65536 without Reed-Solomon");
        }
        r.frame_octets_ = config.frame_octets;
        r.codeblock_octets_ = config.frame_octets;
    }
    if (config.asm_search_errors > config.asm_locked_errors || config.asm_locked_errors >= 16) {
        return fail("CCSDS receiver: marker thresholds must satisfy search <= locked < 16");
    }
    const std::size_t branches = config.convolutional ? 2 : 1;
    r.branches_.resize(branches);
    for (auto& b : r.branches_) {
        if (config.convolutional) {
            StreamingViterbiConfig vc;
            vc.memory = kCcsdsK7Memory;
            vc.generators = {kCcsdsK7G1, kCcsdsK7G2};
            vc.invert_mask = kCcsdsK7InvertMask;
            vc.traceback = config.traceback;
            auto v = StreamingViterbi::create(std::move(vc));
            if (!v) {
                return std::unexpected(v.error());
            }
            b.viterbi = std::move(*v);
        }
        b.block.assign(r.codeblock_octets_, 0);
    }
    return r;
}

void CcsdsReceiver::reset() {
    for (auto& b : branches_) {
        if (b.viterbi) {
            b.viterbi->reset();
        }
        const auto size = b.block.size();
        auto v = std::move(b.viterbi);
        b = Branch{};
        b.viterbi = std::move(v);
        b.block.assign(size, 0);
    }
    skew_done_ = false;
    active_ = -1;
}

bool CcsdsReceiver::locked() const {
    if (active_ < 0) {
        return false;
    }
    return branches_[static_cast<std::size_t>(active_)].state != Branch::State::Search;
}

void CcsdsReceiver::push(SoftSpan soft, std::vector<CcsdsFrame>& out) {
    if (!config_.convolutional) {
        scratch_.resize(soft.size());
        for (std::size_t i = 0; i < soft.size(); ++i) {
            scratch_[i] = soft[i] < 0.0F ? 1 : 0;
        }
        bits(0, scratch_, out);
        return;
    }
    for (std::size_t bi = 0; bi < branches_.size(); ++bi) {
        SoftSpan s = soft;
        if (bi == 1 && !skew_done_ && !s.empty()) {
            s = s.subspan(1);
            skew_done_ = true;
        }
        scratch_.clear();
        branches_[bi].viterbi->push(s, scratch_);
        bits(bi, scratch_, out);
    }
}

void CcsdsReceiver::flush(std::vector<CcsdsFrame>& out) {
    if (!config_.convolutional) {
        return;
    }
    for (std::size_t bi = 0; bi < branches_.size(); ++bi) {
        scratch_.clear();
        branches_[bi].viterbi->flush(false, scratch_);
        bits(bi, scratch_, out);
    }
}

void CcsdsReceiver::bits(std::size_t bi, std::span<const std::uint8_t> decided, std::vector<CcsdsFrame>& out) {
    Branch& b = branches_[bi];
    const std::size_t block_bits = codeblock_octets_ * 8;
    for (std::uint8_t raw : decided) {
        std::uint8_t bit = raw & 1U;
        if (config_.nrzm) {
            const std::uint8_t d = bit ^ b.nrzm_previous;
            b.nrzm_previous = bit;
            bit = d;
        }
        b.shift = (b.shift << 1U) | bit;
        ++b.bits_seen;

        switch (b.state) {
            case Branch::State::Search: {
                if (b.bits_seen < kCcsdsAsmBits) {
                    break;
                }
                // Another branch already holds lock: this one stays quiet.
                if (active_ >= 0 && static_cast<std::size_t>(active_) != bi) {
                    break;
                }
                const auto e_true = static_cast<unsigned>(std::popcount(b.shift ^ kCcsdsAsm));
                const auto e_inv = static_cast<unsigned>(std::popcount(b.shift ^ ~kCcsdsAsm));
                if (e_true <= config_.asm_search_errors || e_inv <= config_.asm_search_errors) {
                    b.inverted = e_inv < e_true;
                    b.asm_errors = std::min(e_true, e_inv);
                    b.state = Branch::State::Collect;
                    b.block_bits = 0;
                    b.block_start = b.bits_seen;
                    std::fill(b.block.begin(), b.block.end(), 0);
                }
                break;
            }
            case Branch::State::Collect: {
                const std::uint8_t v = bit ^ (b.inverted ? 1U : 0U);
                b.block[b.block_bits / 8] =
                    static_cast<std::uint8_t>(b.block[b.block_bits / 8] | (v << (7U - (b.block_bits % 8U))));
                ++b.block_bits;
                if (b.block_bits == block_bits) {
                    if (finish_block(bi, out)) {
                        b.state = Branch::State::Verify;
                        b.verify_bits = 0;
                    } else {
                        b.state = Branch::State::Search;
                    }
                }
                break;
            }
            case Branch::State::Verify: {
                if (++b.verify_bits < kCcsdsAsmBits) {
                    break;
                }
                const auto e_true = static_cast<unsigned>(std::popcount(b.shift ^ kCcsdsAsm));
                const auto e_inv = static_cast<unsigned>(std::popcount(b.shift ^ ~kCcsdsAsm));
                const unsigned e = std::min(e_true, e_inv);
                if (e <= config_.asm_locked_errors) {
                    // A complemented marker here is a phase slip; follow it.
                    b.inverted = e_inv < e_true;
                    b.asm_errors = e;
                    b.state = Branch::State::Collect;
                    b.block_bits = 0;
                    b.block_start = b.bits_seen;
                    std::fill(b.block.begin(), b.block.end(), 0);
                } else {
                    b.state = Branch::State::Search;
                    if (active_ == static_cast<int>(bi)) {
                        active_ = -1;
                    }
                }
                break;
            }
        }
    }
}

bool CcsdsReceiver::finish_block(std::size_t bi, std::vector<CcsdsFrame>& out) {
    Branch& b = branches_[bi];
    CcsdsFrame f;
    f.inverted = b.inverted;
    f.asm_errors = b.asm_errors;
    f.bit_position = b.block_start;
    std::vector<std::uint8_t> block = b.block;
    ccsds_randomize(block, config_.randomizer);
    if (rs_) {
        f.rs = rs_->decode(block);
        // A codeblock with no codeword decodable is noise that happened to
        // follow a marker-like pattern: the branch has not proven itself.
        const bool any_ok = std::any_of(f.rs.corrected.begin(), f.rs.corrected.end(),
                                        [](const auto& c) { return c.has_value(); });
        if (!any_ok && active_ != static_cast<int>(bi)) {
            return false;
        }
    }
    if (active_ < 0) {
        active_ = static_cast<int>(bi);
    }
    if (active_ != static_cast<int>(bi)) {
        return true;
    }
    block.resize(frame_octets_);
    f.frame = std::move(block);
    out.push_back(std::move(f));
    return true;
}

}  // namespace revenant::decode
