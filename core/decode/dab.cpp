#include "core/decode/dab.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <format>
#include <numbers>
#include <string_view>
#include <utility>

#include "core/characterise/transform.h"
#include "core/decode/crc.h"
#include "core/decode/dv_codes.h"

namespace revenant::decode {

namespace {

using characterise::Complex64;
using dsp::Complex32;

constexpr auto kFrame = static_cast<std::int64_t>(kDabFrameSamples);
constexpr auto kNull = static_cast<std::int64_t>(kDabNullSamples);
constexpr auto kSymbol = static_cast<std::int64_t>(kDabSymbolSamples);
constexpr auto kFft = static_cast<std::int64_t>(kDabFftSize);
constexpr auto kGuard = static_cast<std::int64_t>(kDabGuardSamples);
constexpr int kHalfCarriers = static_cast<int>(kDabCarriers / 2);

// EN 300 401 table 23, transmission mode I: for each run of 32 carriers, the
// first carrier k' of the run, then i and n. Rows in the order printed, the
// left half of the table first.
struct PrsRun {
    int first;
    int i;
    int n;
};
constexpr std::array<PrsRun, 48> kPrsRuns{{
    {-768, 0, 1}, {-736, 1, 2}, {-704, 2, 0}, {-672, 3, 1}, {-640, 0, 3}, {-608, 1, 2},
    {-576, 2, 2}, {-544, 3, 3}, {-512, 0, 2}, {-480, 1, 1}, {-448, 2, 2}, {-416, 3, 3},
    {-384, 0, 1}, {-352, 1, 2}, {-320, 2, 3}, {-288, 3, 3}, {-256, 0, 2}, {-224, 1, 2},
    {-192, 2, 2}, {-160, 3, 1}, {-128, 0, 1}, {-96, 1, 3},  {-64, 2, 1},  {-32, 3, 2},
    {1, 0, 3},    {33, 3, 1},   {65, 2, 1},   {97, 1, 1},   {129, 0, 2},  {161, 3, 2},
    {193, 2, 1},  {225, 1, 0},  {257, 0, 2},  {289, 3, 2},  {321, 2, 3},  {353, 1, 3},
    {385, 0, 0},  {417, 3, 2},  {449, 2, 1},  {481, 1, 3},  {513, 0, 3},  {545, 3, 3},
    {577, 2, 3},  {609, 1, 0},  {641, 0, 3},  {673, 3, 0},  {705, 2, 1},  {737, 1, 1},
}};

// EN 300 401 table 24, h(i, j) for j = 0..15. The table prints j = 16..31 as
// well, identical to j = 0..15 row for row; the index is taken modulo 16.
constexpr std::array<std::array<std::uint8_t, 16>, 4> kPrsH{{
    {0, 2, 0, 0, 0, 0, 1, 1, 2, 0, 0, 0, 2, 2, 1, 1},
    {0, 3, 2, 3, 0, 1, 3, 0, 2, 1, 2, 3, 2, 3, 3, 0},
    {0, 0, 0, 2, 0, 2, 1, 3, 2, 2, 0, 2, 2, 0, 1, 3},
    {0, 1, 2, 1, 0, 3, 3, 2, 2, 3, 2, 1, 2, 1, 3, 2},
}};

// EN 300 401 table 13, v(PI, 0..31) as printed, PI = 1..24.
constexpr std::array<std::string_view, 24> kPunctureVectors{
    "11001000100010001000100010001000", "11001000100010001100100010001000",
    "11001000110010001100100010001000", "11001000110010001100100011001000",
    "11001100110010001100100011001000", "11001100110010001100110011001000",
    "11001100110011001100110011001000", "11001100110011001100110011001100",
    "11101100110011001100110011001100", "11101100110011001110110011001100",
    "11101100111011001110110011001100", "11101100111011001110110011101100",
    "11101110111011001110110011101100", "11101110111011001110111011101100",
    "11101110111011101110111011101100", "11101110111011101110111011101110",
    "11111110111011101110111011101110", "11111110111011101111111011101110",
    "11111110111111101111111011101110", "11111110111111101111111011111110",
    "11111111111111101111111011111110", "11111111111111101111111111111110",
    "11111111111111111111111111111110", "11111111111111111111111111111111",
};

// EN 300 401 clause 11.1.2: V_T, the 24 tail bits' puncturing.
constexpr std::string_view kTailVector = "110011001100110011001100";

// EN 300 401 clause 12 table 21: r' = r - delay for i mod 16 = 0..15.
constexpr std::array<unsigned, 16> kTimeDelay{0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};

// EN 300 401 table 6: packet lengths in bytes, by the 2-bit field.
constexpr std::array<std::size_t, 4> kPacketLength{24, 48, 72, 96};

// EN 300 401 clause 5.2.1 and annex E: G(x) = x^16 + x^12 + x^5 + 1, preset
// ones, complemented, MSb first, so neither end is reflected.
constexpr CrcParams kDabCrcParams{16, 0x1021, 0xFFFF, false, false, 0xFFFF};

std::size_t bin_of(int carrier) {
    const int n = static_cast<int>(kDabFftSize);
    return static_cast<std::size_t>(((carrier % n) + n) % n);
}

std::vector<std::uint8_t> bits_to_bytes(std::span<const std::uint8_t> bits) {
    std::vector<std::uint8_t> bytes(bits.size() / 8, 0);
    for (std::size_t i = 0; i < bytes.size() * 8; ++i) {
        bytes[i / 8] = static_cast<std::uint8_t>(bytes[i / 8] | ((bits[i] & 1U) << (7U - (i % 8))));
    }
    return bytes;
}

DabLabel make_label(unsigned charset, std::span<const std::uint8_t> chars, std::uint16_t flags) {
    DabLabel label;
    label.charset = charset;
    label.short_flags = flags;
    std::copy_n(chars.begin(), 16, label.bytes.begin());
    for (const std::uint8_t c : label.bytes) {
        label.text.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
    }
    while (!label.text.empty() && label.text.back() == ' ') {
        label.text.pop_back();
    }
    return label;
}

unsigned be16(std::span<const std::uint8_t> b, std::size_t at) {
    return (static_cast<unsigned>(b[at]) << 8U) | b[at + 1];
}

void parse_fig0(std::span<const std::uint8_t> d, DabEnsemble& ens, std::size_t& recognised) {
    if (d.empty()) {
        return;
    }
    // Clause 5.2.2.1 figure 7.
    const bool next = (d[0] & 0x80U) != 0;
    const bool pd = (d[0] & 0x20U) != 0;
    const unsigned ext = d[0] & 0x1FU;
    const auto f = d.subspan(1);
    if (ext == 0) {
        // Clause 6.4.1 figure 26: EId, change flags, Al, CIF count.
        if (f.size() < 4) {
            return;
        }
        ens.eid = static_cast<std::uint16_t>(be16(f, 0));
        ens.alarm = (f[2] & 0x20U) != 0;
        const unsigned high = f[2] & 0x1FU;
        const unsigned low = f[3];
        if (high < 20 && low < 250) {
            ens.cif_count = high * 250 + low;
        }
        ++recognised;
        return;
    }
    if (next) {
        return;  // only the current configuration is applied
    }
    if (ext == 1) {
        // Clause 6.2.1 figure 19.
        std::size_t at = 0;
        while (at + 3 <= f.size()) {
            DabSubchannel sc;
            const unsigned head = be16(f, at);
            sc.id = head >> 10U;
            sc.start_cu = head & 0x3FFU;
            if ((f[at + 2] & 0x80U) == 0) {
                sc.protection = DabProtection::Uep;
                sc.uep_index = f[at + 2] & 0x3FU;
                at += 3;
            } else {
                if (at + 4 > f.size()) {
                    return;
                }
                const unsigned word = be16(f, at + 2);
                const unsigned option = (word >> 12U) & 0x7U;
                sc.level = ((word >> 10U) & 0x3U) + 1;
                sc.size_cu = word & 0x3FFU;
                at += 4;
                if (option > 1) {
                    continue;  // reserved options
                }
                sc.protection = option == 0 ? DabProtection::EepA : DabProtection::EepB;
            }
            ens.subchannels[sc.id] = sc;
        }
        ++recognised;
        return;
    }
    if (ext == 2) {
        // Clause 6.3.1 figure 21.
        std::size_t at = 0;
        const std::size_t sid_bytes = pd ? 4 : 2;
        while (at + sid_bytes + 1 <= f.size()) {
            std::uint32_t sid = 0;
            for (std::size_t b = 0; b < sid_bytes; ++b) {
                sid = (sid << 8U) | f[at + b];
            }
            at += sid_bytes;
            const unsigned count = f[at] & 0x0FU;
            ++at;
            if (at + 2 * count > f.size()) {
                return;
            }
            DabService& service = ens.services[sid];
            service.sid = sid;
            service.data_sid = pd;
            service.components.clear();
            for (unsigned c = 0; c < count; ++c) {
                const unsigned word = be16(f, at);
                at += 2;
                DabComponent comp;
                comp.transport = word >> 14U;
                if (comp.transport == 3) {
                    comp.scid = (word >> 2U) & 0xFFFU;
                } else {
                    comp.type = (word >> 8U) & 0x3FU;
                    comp.subchannel = (word >> 2U) & 0x3FU;
                }
                comp.primary = (word & 0x2U) != 0;
                comp.ca = (word & 0x1U) != 0;
                service.components.push_back(comp);
            }
        }
        ++recognised;
        return;
    }
    if (ext == 3) {
        // Clause 6.3.2 figure 22.
        std::size_t at = 0;
        while (at + 5 <= f.size()) {
            DabPacketComponent pc;
            const unsigned head = be16(f, at);
            pc.scid = head >> 4U;
            const bool caorg = (head & 0x1U) != 0;
            pc.data_groups = (f[at + 2] & 0x80U) == 0;
            pc.dscty = f[at + 2] & 0x3FU;
            const unsigned tail = be16(f, at + 3);
            pc.subchannel = tail >> 10U;
            pc.address = tail & 0x3FFU;
            at += caorg ? 7 : 5;
            ens.packet_components[pc.scid] = pc;
        }
        ++recognised;
    }
}

void parse_fig1(std::span<const std::uint8_t> d, DabEnsemble& ens, std::size_t& recognised) {
    if (d.empty()) {
        return;
    }
    // Clause 5.2.2.2 figure 8.
    const unsigned charset = d[0] >> 4U;
    const unsigned ext = d[0] & 0x7U;
    const auto f = d.subspan(1);
    // Clauses 8.1.13 and 8.1.14: a 16-bit identifier for extensions 0 and 1,
    // 32-bit for extension 5, then 16 characters and the flag field.
    const std::size_t id_bytes = ext == 5 ? 4 : 2;
    if ((ext != 0 && ext != 1 && ext != 5) || f.size() < id_bytes + 18) {
        return;
    }
    std::uint32_t id = 0;
    for (std::size_t b = 0; b < id_bytes; ++b) {
        id = (id << 8U) | f[b];
    }
    const auto label = make_label(charset, f.subspan(id_bytes, 16),
                                  static_cast<std::uint16_t>(be16(f, id_bytes + 16)));
    if (ext == 0) {
        ens.label = label;
    } else {
        DabService& service = ens.services[id];
        service.sid = id;
        service.data_sid = ext == 5;
        service.label = label;
    }
    ++recognised;
}

}  // namespace

// ---------------------------------------------------------------------------
// Fixed sequences
// ---------------------------------------------------------------------------

Complex32 dab_prs_value(int carrier) {
    if (carrier == 0 || carrier < -kHalfCarriers || carrier > kHalfCarriers) {
        return {0.0F, 0.0F};
    }
    // Clause 14.3.2: phi_k = pi/2 * (h(i, k - k') + n).
    for (auto it = kPrsRuns.rbegin(); it != kPrsRuns.rend(); ++it) {
        if (carrier >= it->first) {
            const int j = carrier - it->first;
            const int q = (kPrsH[static_cast<std::size_t>(it->i)][static_cast<std::size_t>(j % 16)] +
                           it->n) % 4;
            constexpr std::array<Complex32, 4> kQuarter{
                Complex32{1.0F, 0.0F}, Complex32{0.0F, 1.0F}, Complex32{-1.0F, 0.0F},
                Complex32{0.0F, -1.0F}};
            return kQuarter[static_cast<std::size_t>(q)];
        }
    }
    return {0.0F, 0.0F};
}

const std::array<int, kDabCarriers>& dab_frequency_interleaver() {
    static const std::array<int, kDabCarriers> table = [] {
        // Clause 14.6.1: Pi(i) = 13 Pi(i-1) + 511 mod 2048, keeping the
        // values in [256, 1792] other than 1024, k = d_n - 1024.
        std::array<int, kDabCarriers> out{};
        std::size_t n = 0;
        unsigned pi = 0;
        for (unsigned i = 0; i < 2048; ++i) {
            if (i > 0) {
                pi = (13 * pi + 511) % 2048;
            }
            if (pi >= 256 && pi <= 1792 && pi != 1024 && n < out.size()) {
                out[n++] = static_cast<int>(pi) - 1024;
            }
        }
        return out;
    }();
    return table;
}

std::vector<std::uint8_t> dab_prbs(std::size_t length) {
    // Clause 10.1 figure 54: nine stages, all ones, feedback from stages 9
    // and 5; table 12 lists the first 16 outputs.
    std::vector<std::uint8_t> out(length);
    unsigned reg = 0x1FF;
    for (std::size_t i = 0; i < length; ++i) {
        const unsigned bit = ((reg >> 8U) ^ (reg >> 4U)) & 1U;
        out[i] = static_cast<std::uint8_t>(bit);
        reg = ((reg << 1U) | bit) & 0x1FFU;
    }
    return out;
}

std::array<std::uint8_t, 32> dab_puncture_vector(unsigned pi) {
    std::array<std::uint8_t, 32> v{};
    if (pi >= 1 && pi <= 24) {
        const auto s = kPunctureVectors[pi - 1];
        for (std::size_t i = 0; i < 32; ++i) {
            v[i] = s[i] == '1' ? 1 : 0;
        }
    }
    return v;
}

namespace {

// The mask of transmitted mother-code bits for I information bits under the
// given runs, clause 11.1.2.
Expected<std::vector<std::uint8_t>> puncture_mask(std::size_t information_bits,
                                                  std::span<const DabPunctureRun> runs) {
    std::size_t blocks = 0;
    for (const auto& run : runs) {
        if (run.pi < 1 || run.pi > 24) {
            return fail(std::format("dab: puncturing index {} is outside table 13", run.pi));
        }
        blocks += run.blocks;
    }
    if (blocks * 128 != information_bits * 4) {
        return fail(std::format("dab: {} puncturing blocks do not cover {} information bits",
                                blocks, information_bits));
    }
    std::vector<std::uint8_t> mask;
    mask.reserve(information_bits * 4 + 24);
    for (const auto& run : runs) {
        const auto v = dab_puncture_vector(run.pi);
        for (std::size_t b = 0; b < run.blocks * 4; ++b) {
            mask.insert(mask.end(), v.begin(), v.end());
        }
    }
    for (const char c : kTailVector) {
        mask.push_back(c == '1' ? 1 : 0);
    }
    return mask;
}

}  // namespace

Expected<std::vector<std::uint8_t>> dab_convolutional_encode(std::span<const std::uint8_t> bits,
                                                             std::span<const DabPunctureRun> runs) {
    auto mask = puncture_mask(bits.size(), runs);
    if (!mask) {
        return std::unexpected(mask.error());
    }
    std::vector<std::uint8_t> input(bits.begin(), bits.end());
    input.resize(bits.size() + kDabCodeMemory, 0);
    const ConvolutionalCode code{kDabCodeMemory, kDabCodeGenerators};
    auto mother = convolutional_encode(code, input);
    if (!mother) {
        return std::unexpected(mother.error());
    }
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < mother->size(); ++i) {
        if ((*mask)[i] != 0) {
            out.push_back((*mother)[i]);
        }
    }
    return out;
}

Expected<std::vector<std::uint8_t>> dab_convolutional_decode(std::span<const float> soft,
                                                             std::size_t information_bits,
                                                             std::span<const DabPunctureRun> runs) {
    auto mask = puncture_mask(information_bits, runs);
    if (!mask) {
        return std::unexpected(mask.error());
    }
    std::vector<float> mother(mask->size(), 0.0F);
    std::size_t at = 0;
    for (std::size_t i = 0; i < mask->size(); ++i) {
        if ((*mask)[i] != 0) {
            if (at >= soft.size()) {
                return fail("dab: fewer soft bits than the puncturing pattern transmits");
            }
            mother[i] = soft[at++];
        }
    }
    const ConvolutionalCode code{kDabCodeMemory, kDabCodeGenerators};
    auto decoded = viterbi_decode(code, mother, true);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    decoded->resize(information_bits);
    return decoded;
}

std::vector<DabPunctureRun> dab_fic_runs() {
    // Clause 11.2.1.
    return {{21, 16}, {3, 15}};
}

unsigned dab_time_interleave_delay(std::size_t i) { return kTimeDelay[i % 16]; }

std::uint16_t dab_crc16(std::span<const std::uint8_t> bytes) {
    static const Crc crc{kDabCrcParams};
    return static_cast<std::uint16_t>(crc.compute(bytes));
}

Expected<DabEepProfile> dab_eep_profile(const DabSubchannel& sc) {
    if (sc.protection == DabProtection::Uep) {
        return fail("dab: short form (UEP) sub-channels are not decoded");
    }
    if (sc.level < 1 || sc.level > 4) {
        return fail(std::format("dab: protection level {} is out of range", sc.level));
    }
    DabEepProfile p;
    const std::size_t level = sc.level - 1;
    if (sc.protection == DabProtection::EepA) {
        // Table 9: 12n, 8n, 6n, 4n CUs; table 17: I = 192n; table 18.
        constexpr std::array<unsigned, 4> kUnit{12, 8, 6, 4};
        if (sc.size_cu == 0 || sc.size_cu % kUnit[level] != 0) {
            return fail(std::format("dab: {} CUs is not a set A size at level {}", sc.size_cu,
                                    sc.level));
        }
        const std::size_t n = sc.size_cu / kUnit[level];
        p.information_bits = 192 * n;
        p.bitrate_kbps = static_cast<unsigned>(8 * n);
        switch (sc.level) {
        case 1: p.runs = {{6 * n - 3, 24}, {3, 23}}; break;
        case 2:
            p.runs = n == 1 ? std::vector<DabPunctureRun>{{5, 13}, {1, 12}}
                            : std::vector<DabPunctureRun>{{2 * n - 3, 14}, {4 * n + 3, 13}};
            break;
        case 3: p.runs = {{6 * n - 3, 8}, {3, 7}}; break;
        default: p.runs = {{4 * n - 3, 3}, {2 * n + 3, 2}}; break;
        }
    } else {
        // Table 10: 27n, 21n, 18n, 15n CUs; table 19: I = 768n; table 20.
        constexpr std::array<unsigned, 4> kUnit{27, 21, 18, 15};
        constexpr std::array<std::array<unsigned, 2>, 4> kPi{{{10, 9}, {6, 5}, {4, 3}, {2, 1}}};
        if (sc.size_cu == 0 || sc.size_cu % kUnit[level] != 0) {
            return fail(std::format("dab: {} CUs is not a set B size at level {}", sc.size_cu,
                                    sc.level));
        }
        const std::size_t n = sc.size_cu / kUnit[level];
        p.information_bits = 768 * n;
        p.bitrate_kbps = static_cast<unsigned>(32 * n);
        p.runs = {{24 * n - 3, kPi[level][0]}, {3, kPi[level][1]}};
    }
    if (sc.start_cu + sc.size_cu > kDabCusPerCif) {
        return fail("dab: sub-channel runs past the end of the CIF");
    }
    return p;
}

// ---------------------------------------------------------------------------
// FIC content
// ---------------------------------------------------------------------------

std::size_t dab_parse_fib(std::span<const std::uint8_t> fib, DabEnsemble& ensemble) {
    std::size_t recognised = 0;
    std::size_t at = 0;
    const std::size_t end = std::min<std::size_t>(fib.size(), 30);
    while (at < end) {
        // Clause 5.2.2 and figure 6: 3-bit type, 5-bit length; 0xFF ends.
        const std::uint8_t header = fib[at];
        if (header == 0xFF) {
            break;
        }
        const unsigned type = header >> 5U;
        const std::size_t length = header & 0x1FU;
        ++at;
        if (length == 0 || length > 29 || at + length > end) {
            break;
        }
        const auto data = fib.subspan(at, length);
        if (type == 0) {
            parse_fig0(data, ensemble, recognised);
        } else if (type == 1) {
            parse_fig1(data, ensemble, recognised);
        }
        at += length;
    }
    return recognised;
}

// ---------------------------------------------------------------------------
// Packet mode and MOT
// ---------------------------------------------------------------------------

std::optional<DabDataGroup> dab_parse_data_group(std::span<const std::uint8_t> b,
                                                 unsigned subchannel, unsigned address) {
    // Clause 5.3.3 figure 12.
    if (b.size() < 2) {
        return std::nullopt;
    }
    DabDataGroup g;
    g.subchannel = subchannel;
    g.address = address;
    const bool extension = (b[0] & 0x80U) != 0;
    const bool crc = (b[0] & 0x40U) != 0;
    g.has_segment = (b[0] & 0x20U) != 0;
    const bool user_access = (b[0] & 0x10U) != 0;
    g.type = b[0] & 0x0FU;
    g.continuity = b[1] >> 4U;
    g.repetition = b[1] & 0x0FU;
    std::size_t end = b.size();
    if (crc) {
        if (end < 4) {
            return std::nullopt;
        }
        end -= 2;
        if (dab_crc16(b.first(end)) != be16(b, end)) {
            return std::nullopt;
        }
    }
    std::size_t at = 2 + (extension ? 2 : 0);
    if (g.has_segment) {
        if (at + 2 > end) {
            return std::nullopt;
        }
        const unsigned word = be16(b, at);
        g.last = (word & 0x8000U) != 0;
        g.segment = word & 0x7FFFU;
        at += 2;
    }
    if (user_access) {
        if (at + 1 > end) {
            return std::nullopt;
        }
        const bool tid = (b[at] & 0x10U) != 0;
        const std::size_t n = b[at] & 0x0FU;
        ++at;
        if (at + n > end || (tid && n < 2)) {
            return std::nullopt;
        }
        if (tid) {
            g.transport_id = static_cast<std::uint16_t>(be16(b, at));
            g.end_user_address.assign(b.begin() + static_cast<std::ptrdiff_t>(at + 2),
                                      b.begin() + static_cast<std::ptrdiff_t>(at + n));
        } else {
            g.end_user_address.assign(b.begin() + static_cast<std::ptrdiff_t>(at),
                                      b.begin() + static_cast<std::ptrdiff_t>(at + n));
        }
        at += n;
    }
    if (at > end) {
        return std::nullopt;
    }
    g.data.assign(b.begin() + static_cast<std::ptrdiff_t>(at),
                  b.begin() + static_cast<std::ptrdiff_t>(end));
    return g;
}

void DabPacketAssembler::push(std::span<const std::uint8_t> frame, std::vector<DabDataGroup>& out) {
    std::size_t at = 0;
    while (at + 3 <= frame.size()) {
        // Clause 5.3.2.1 figure 11 and table 6.
        const std::size_t length = kPacketLength[frame[at] >> 6U];
        if (at + length > frame.size()) {
            break;
        }
        const auto packet = frame.subspan(at, length);
        at += length;
        if (dab_crc16(packet.first(length - 2)) != be16(packet, length - 2)) {
            ++packets_bad_;
            // A lost packet breaks every open series: its address is unknown.
            for (auto& [address, s] : series_) {
                s.open = false;
            }
            continue;
        }
        ++packets_ok_;
        const unsigned head = be16(packet, 0);
        const unsigned continuity = (head >> 12U) & 0x3U;
        const bool first = (head & 0x800U) != 0;
        const bool last = (head & 0x400U) != 0;
        const unsigned address = head & 0x3FFU;
        const std::size_t useful = packet[2] & 0x7FU;
        if (address == 0 || useful > length - 5) {
            continue;  // padding packet, or a length the packet cannot hold
        }
        const auto data = packet.subspan(3, useful);
        Series& s = series_[address];
        if (first) {
            s.bytes.assign(data.begin(), data.end());
            s.open = true;
        } else if (s.open && continuity == s.next_continuity) {
            s.bytes.insert(s.bytes.end(), data.begin(), data.end());
        } else {
            s.open = false;
        }
        s.next_continuity = (continuity + 1) % 4;
        if (last && s.open) {
            s.open = false;
            if (auto g = dab_parse_data_group(s.bytes, subchannel_, address)) {
                out.push_back(std::move(*g));
            } else {
                ++groups_bad_;
            }
        }
    }
}

Expected<MotObject> mot_parse_header(std::span<const std::uint8_t> h) {
    // EN 301 234 clause 6.1 figure 20: BodySize 28, HeaderSize 13,
    // ContentType 6, ContentSubType 9.
    if (h.size() < 7) {
        return fail("mot: header shorter than its 7-byte core");
    }
    std::uint64_t core = 0;
    for (std::size_t i = 0; i < 7; ++i) {
        core = (core << 8U) | h[i];
    }
    MotObject o;
    const auto body_size = static_cast<std::size_t>(core >> 28U);
    const auto header_size = static_cast<std::size_t>((core >> 15U) & 0x1FFFU);
    o.content_type = static_cast<unsigned>((core >> 9U) & 0x3FU);
    o.content_subtype = static_cast<unsigned>(core & 0x1FFU);
    if (header_size < 7 || header_size > h.size()) {
        return fail("mot: HeaderSize disagrees with the header received");
    }
    o.body_size = body_size;
    o.header.assign(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(header_size));
    // Clause 6.2 figure 22: the extension parameters.
    std::size_t at = 7;
    while (at < header_size) {
        const unsigned pli = h[at] >> 6U;
        const unsigned id = h[at] & 0x3FU;
        ++at;
        std::size_t len = 0;
        if (pli == 1) {
            len = 1;
        } else if (pli == 2) {
            len = 4;
        } else if (pli == 3) {
            if (at >= header_size) {
                break;
            }
            if ((h[at] & 0x80U) != 0) {
                if (at + 2 > header_size) {
                    break;
                }
                len = be16(h, at) & 0x7FFFU;
                at += 2;
            } else {
                len = h[at] & 0x7FU;
                ++at;
            }
        }
        if (at + len > header_size) {
            break;
        }
        // Table 2: ContentName is ParamId 0x0C; clause 6.2.2.1.1 figure 24 puts
        // a charset byte before the name.
        if (id == 0x0C && len >= 1) {
            o.content_name.assign(h.begin() + static_cast<std::ptrdiff_t>(at + 1),
                                  h.begin() + static_cast<std::ptrdiff_t>(at + len));
        }
        at += len;
    }
    return o;
}

std::optional<std::vector<std::uint8_t>> MotAssembler::complete(const Entity& e) {
    if (!e.last || e.segments.size() != *e.last + 1) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> out;
    for (const auto& [index, bytes] : e.segments) {
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

void MotAssembler::push(const DabDataGroup& g, std::vector<MotObject>& out) {
    // EN 301 234 clause 5.1: type 3 header, 4 unscrambled body, keyed by
    // TransportId; each segment starts with the 2-byte segmentation header
    // of clause 5.1.1.
    if ((g.type != 3 && g.type != 4) || !g.transport_id || g.data.size() < 2) {
        return;
    }
    const std::size_t size = be16(g.data, 0) & 0x1FFFU;
    if (size + 2 > g.data.size()) {
        return;
    }
    Pending& p = pending_[*g.transport_id];
    if (p.emitted) {
        return;
    }
    Entity& e = g.type == 3 ? p.header : p.body;
    e.segments[g.segment].assign(g.data.begin() + 2,
                                 g.data.begin() + 2 + static_cast<std::ptrdiff_t>(size));
    if (g.last) {
        e.last = g.segment;
    }
    const auto header = complete(p.header);
    const auto body = complete(p.body);
    if (!header || !body) {
        return;
    }
    auto object = mot_parse_header(*header);
    if (!object) {
        pending_.erase(*g.transport_id);
        return;
    }
    // Clause 6.1: a body whose length disagrees with BodySize is discarded.
    if (body->size() != object->body_size) {
        pending_.erase(*g.transport_id);
        return;
    }
    object->transport_id = *g.transport_id;
    object->body = *body;
    p.emitted = true;
    p.header = {};
    p.body = {};
    out.push_back(std::move(*object));
}

// ---------------------------------------------------------------------------
// The receiver
// ---------------------------------------------------------------------------

DabReceiver::DabReceiver(DabReceiverConfig config, DabCallbacks callbacks)
    : config_(std::move(config)), callbacks_(std::move(callbacks)) {}

Expected<DabReceiver> DabReceiver::create(DabReceiverConfig config, DabCallbacks callbacks) {
    if (config.max_integer_cfo < 0 || config.max_integer_cfo > 200) {
        return fail("dab: max_integer_cfo must be 0 to 200 carriers");
    }
    if (config.window_backoff >= kDabGuardSamples) {
        return fail("dab: window_backoff must be shorter than the guard interval");
    }
    return DabReceiver(std::move(config), std::move(callbacks));
}

void DabReceiver::reset() {
    sync_ = {};
    ensemble_ = {};
    buffer_.clear();
    buffer_start_ = total_;
    next_attempt_ = total_;
    missed_nulls_ = 0;
    cif_ = 0;
    subchannel_state_.clear();
}

Complex32 DabReceiver::sample(std::int64_t index) const {
    const std::int64_t at = index - buffer_start_;
    if (at < 0 || at >= static_cast<std::int64_t>(buffer_.size())) {
        return {0.0F, 0.0F};
    }
    return buffer_[static_cast<std::size_t>(at)];
}

void DabReceiver::push(dsp::ConstComplexSpan samples) {
    buffer_.insert(buffer_.end(), samples.begin(), samples.end());
    total_ += static_cast<std::int64_t>(samples.size());
    for (;;) {
        if (!sync_.locked) {
            if (total_ < next_attempt_ + 2 * kFrame + 4096) {
                break;
            }
            if (!try_acquire()) {
                // Past the window that was taken for a null and was not one,
                // which is still a function of absolute position alone.
                next_attempt_ = std::max(next_attempt_ + kNull, sync_.frame_start + kNull);
            }
        } else {
            if (total_ < sync_.frame_start + kFrame + 1024) {
                break;
            }
            process_frame(false);
        }
        trim();
    }
    trim();
}

void DabReceiver::trim() {
    const std::int64_t keep = sync_.locked ? sync_.frame_start - 4096 : next_attempt_ - 4096;
    const std::int64_t drop = keep - buffer_start_;
    if (drop > 0 && drop <= static_cast<std::int64_t>(buffer_.size())) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + drop);
        buffer_start_ = keep;
    }
}

double DabReceiver::null_ratio(std::int64_t s) const {
    double null_energy = 0.0;
    for (std::int64_t n = s + 64; n < s + kNull - 64; ++n) {
        null_energy += std::norm(sample(n));
    }
    double frame_energy = 0.0;
    for (std::int64_t n = s + kNull; n < s + kFrame; n += 7) {
        frame_energy += std::norm(sample(n));
    }
    const double null_mean = null_energy / static_cast<double>(kNull - 128);
    const double frame_mean = frame_energy / static_cast<double>((kFrame - kNull) / 7);
    return frame_mean > 0.0 ? null_mean / frame_mean : 1.0;
}

bool DabReceiver::try_acquire() {
    // Step 1: the null symbol is the least-energy T_NULL window.
    const std::int64_t a = next_attempt_;
    const std::size_t span = static_cast<std::size_t>(kFrame + kNull + 64);
    std::vector<double> prefix(span + 1, 0.0);
    for (std::size_t i = 0; i < span; ++i) {
        prefix[i + 1] = prefix[i] + std::norm(sample(a + static_cast<std::int64_t>(i)));
    }
    auto window = [&](std::size_t p) { return prefix[p + kDabNullSamples] - prefix[p]; };
    std::size_t best = 0;
    double best_energy = window(0);
    for (std::size_t p = 0; p < static_cast<std::size_t>(kFrame); p += 16) {
        if (window(p) < best_energy) {
            best_energy = window(p);
            best = p;
        }
    }
    const std::size_t lo = best >= 16 ? best - 16 : 0;
    for (std::size_t p = lo; p < best + 16 && p < static_cast<std::size_t>(kFrame); ++p) {
        if (window(p) < best_energy) {
            best_energy = window(p);
            best = p;
        }
    }
    sync_.frame_start = a + static_cast<std::int64_t>(best);
    sync_.cfo_carriers = 0.0;
    if (!process_frame(true)) {
        sync_.locked = false;
        return false;
    }
    return true;
}

void DabReceiver::fft_symbol(std::int64_t start, std::vector<Complex32>& bins) const {
    std::vector<Complex64> x(kDabFftSize);
    const double step = sync_.cfo_carriers / static_cast<double>(kDabFftSize);
    for (std::size_t i = 0; i < kDabFftSize; ++i) {
        const std::int64_t n = start + static_cast<std::int64_t>(i);
        // Rotation from the absolute index, so it does not depend on blocking.
        const double cycles = step * static_cast<double>(n);
        const double frac = cycles - std::floor(cycles);
        const double phase = -2.0 * std::numbers::pi * frac;
        x[i] = Complex64(sample(n)) * Complex64(std::cos(phase), std::sin(phase));
    }
    characterise::fft_in_place(x);
    bins.resize(kDabFftSize);
    const double scale = 1.0 / std::sqrt(static_cast<double>(kDabFftSize));
    for (std::size_t i = 0; i < kDabFftSize; ++i) {
        bins[i] = Complex32(x[i] * scale);
    }
}

double DabReceiver::fractional_cfo(std::int64_t s) const {
    // Guard against tail, inside the guard so a timing error of a few dozen
    // samples either way still compares a sample with its own copy.
    Complex64 acc{0.0, 0.0};
    const double step = sync_.cfo_carriers / static_cast<double>(kDabFftSize);
    for (std::size_t l = 0; l < kDabSymbols; ++l) {
        const std::int64_t st = s + kNull + static_cast<std::int64_t>(l) * kSymbol;
        for (std::int64_t j = 64; j < kGuard - 64; ++j) {
            const std::int64_t n = st + j;
            const Complex64 a(sample(n));
            const Complex64 b(sample(n + kFft));
            acc += a * std::conj(b);
        }
    }
    // The raw samples carry the whole offset, which turns their product by
    // exp(-j 2 pi cfo); put back the part already being corrected so what is
    // left is the residual.
    const double applied = 2.0 * std::numbers::pi * step * static_cast<double>(kFft);
    acc *= Complex64(std::cos(applied), std::sin(applied));
    return -std::arg(acc) / (2.0 * std::numbers::pi);
}

int DabReceiver::integer_cfo(std::int64_t s, double& metric) const {
    std::vector<Complex32> y;
    const std::int64_t start = s + kNull + kGuard - static_cast<std::int64_t>(config_.window_backoff);
    fft_symbol(start, y);
    int best = 0;
    metric = 0.0;
    for (int m = -config_.max_integer_cfo; m <= config_.max_integer_cfo; ++m) {
        std::complex<double> acc{0.0, 0.0};
        double norm = 0.0;
        for (int k = -kHalfCarriers; k < kHalfCarriers; ++k) {
            if (k == 0 || k + 1 == 0) {
                continue;
            }
            const auto ref = std::complex<double>(dab_prs_value(k + 1) * std::conj(dab_prs_value(k)));
            const auto got = std::complex<double>(y[bin_of(k + 1 + m)] * std::conj(y[bin_of(k + m)]));
            acc += got * std::conj(ref);
            norm += std::abs(got);
        }
        const double value = norm > 0.0 ? std::abs(acc) / norm : 0.0;
        if (value > metric) {
            metric = value;
            best = m;
        }
    }
    return best;
}

int DabReceiver::timing_offset(std::int64_t s) const {
    std::vector<Complex32> y;
    const std::int64_t start = s + kNull + kGuard - static_cast<std::int64_t>(config_.window_backoff);
    fft_symbol(start, y);
    std::vector<Complex64> h(kDabFftSize, Complex64{0.0, 0.0});
    for (int k = -kHalfCarriers; k <= kHalfCarriers; ++k) {
        if (k != 0) {
            // Inverse transform through the forward one: conjugate in and out.
            h[bin_of(k)] = std::conj(Complex64(y[bin_of(k)] * std::conj(dab_prs_value(k))));
        }
    }
    characterise::fft_in_place(h);
    const int quarter = static_cast<int>(kDabFftSize / 4);
    double peak = 0.0;
    for (int t = -quarter; t < quarter; ++t) {
        peak = std::max(peak, std::norm(h[bin_of(t)]));
    }
    for (int t = -quarter; t < quarter; ++t) {
        if (std::norm(h[bin_of(t)]) >= 0.2 * peak) {
            return t;  // the first strong path
        }
    }
    return static_cast<int>(config_.window_backoff);
}

void DabReceiver::demodulate(std::int64_t s, std::vector<float>& soft) {
    const auto& interleaver = dab_frequency_interleaver();
    soft.assign((kDabSymbols - 1) * kDabBitsPerSymbol, 0.0F);
    std::vector<Complex32> previous;
    std::vector<Complex32> current;
    const std::int64_t backoff = static_cast<std::int64_t>(config_.window_backoff);
    fft_symbol(s + kNull + kGuard - backoff, previous);
    std::vector<Complex32> v(kDabCarriers);
    for (std::size_t l = 1; l < kDabSymbols; ++l) {
        fft_symbol(s + kNull + static_cast<std::int64_t>(l) * kSymbol + kGuard - backoff, current);
        // Clause 14.7: z(l,k) = z(l-1,k) y(l,k), so y = z(l) conj(z(l-1)).
        double mean = 0.0;
        for (std::size_t n = 0; n < kDabCarriers; ++n) {
            const std::size_t bin = bin_of(interleaver[n]);
            v[n] = current[bin] * std::conj(previous[bin]);
            mean += std::abs(v[n]);
        }
        mean /= static_cast<double>(kDabCarriers);
        const float scale = mean > 0.0 ? static_cast<float>(1.0 / mean) : 0.0F;
        // Clause 14.5: q(n) = ((1 - 2p(n)) + j(1 - 2p(n+K))) / sqrt 2.
        float* out = soft.data() + (l - 1) * kDabBitsPerSymbol;
        for (std::size_t n = 0; n < kDabCarriers; ++n) {
            out[n] = v[n].real() * scale;
            out[n + kDabCarriers] = v[n].imag() * scale;
        }
        std::swap(previous, current);
    }
}

bool DabReceiver::process_frame(bool acquiring) {
    std::int64_t s = sync_.frame_start;
    const double ratio = null_ratio(s);
    sync_.null_ratio = ratio;
    if (ratio > config_.null_threshold) {
        if (acquiring) {
            return false;
        }
        if (++missed_nulls_ >= 3) {
            sync_.locked = false;
            missed_nulls_ = 0;
            subchannel_state_.clear();
            next_attempt_ = s + kFrame;
            return false;
        }
    } else {
        missed_nulls_ = 0;
    }
    // Step 2, fractional offset; step 3, integer offset on acquisition.
    sync_.cfo_carriers += fractional_cfo(s);
    if (acquiring) {
        double metric = 0.0;
        const int m = integer_cfo(s, metric);
        sync_.prs_metric = metric;
        if (metric < 0.3) {
            return false;
        }
        sync_.cfo_carriers += m;
    }
    // Step 4, timing from the first strong path.
    const int tau = timing_offset(s);
    const std::int64_t correction = tau - static_cast<std::int64_t>(config_.window_backoff);
    if (correction != 0 && std::abs(correction) < kGuard) {
        s += correction;
    }
    sync_.cfo_hz = sync_.cfo_carriers * 1000.0;
    sync_.frame_start = s;
    if (acquiring) {
        sync_.locked = true;
        cif_ = 0;
        subchannel_state_.clear();
    }
    demodulate(s, soft_);
    decode_fic(std::span<const float>(soft_).first(kDabFicSymbols * kDabBitsPerSymbol));
    decode_msc(std::span<const float>(soft_).subspan(kDabFicSymbols * kDabBitsPerSymbol));
    ++sync_.frames;
    sync_.frame_start = s + kFrame;
    return true;
}

void DabReceiver::decode_fic(std::span<const float> soft) {
    // Clause 14.4.1.1: four codewords of 2304 bits back to back; clause 11.2.1
    // codes 768 bits each; clause 10.2 scrambles them; clause 5.2.1 makes
    // three FIBs of 256 bits.
    const auto runs = dab_fic_runs();
    const auto prbs = dab_prbs(768);
    for (std::size_t r = 0; r < 4; ++r) {
        auto bits = dab_convolutional_decode(soft.subspan(r * kDabFicCodewordBits, kDabFicCodewordBits),
                                             768, runs);
        if (!bits) {
            continue;
        }
        for (std::size_t i = 0; i < 768; ++i) {
            (*bits)[i] ^= prbs[i];
        }
        const auto bytes = bits_to_bytes(*bits);
        for (std::size_t f = 0; f < 3; ++f) {
            const auto fib = std::span<const std::uint8_t>(bytes).subspan(f * kDabFibBytes, kDabFibBytes);
            const bool ok = dab_crc16(fib.first(30)) == be16(fib, 30);
            if (ok) {
                ++sync_.fibs_ok;
                dab_parse_fib(fib.first(30), ensemble_);
            } else {
                ++sync_.fibs_bad;
            }
            if (callbacks_.on_fib) {
                callbacks_.on_fib(fib, ok);
            }
        }
    }
    sync_subchannels();
}

void DabReceiver::sync_subchannels() {
    for (const auto& [id, sc] : ensemble_.subchannels) {
        if (sc.protection == DabProtection::Uep) {
            continue;
        }
        if (!config_.subchannels.empty() &&
            std::find(config_.subchannels.begin(), config_.subchannels.end(), id) ==
                config_.subchannels.end()) {
            continue;
        }
        auto it = subchannel_state_.find(id);
        if (it != subchannel_state_.end() && it->second.config.start_cu == sc.start_cu &&
            it->second.config.size_cu == sc.size_cu && it->second.config.level == sc.level &&
            it->second.config.protection == sc.protection) {
            continue;
        }
        auto profile = dab_eep_profile(sc);
        if (!profile) {
            continue;
        }
        SubchannelState state;
        state.config = sc;
        state.profile = std::move(*profile);
        state.history.assign(16, std::vector<float>(sc.size_cu * kDabCuBits, 0.0F));
        subchannel_state_.insert_or_assign(id, std::move(state));
    }
}

void DabReceiver::decode_msc(std::span<const float> soft) {
    // Clause 14.4.2.1: four CIFs of 55296 bits back to back.
    for (std::size_t j = 0; j < kDabCifsPerFrame; ++j) {
        const auto cif = soft.subspan(j * kDabCifBits, kDabCifBits);
        for (auto& [id, st] : subchannel_state_) {
            const std::size_t bits = st.config.size_cu * kDabCuBits;
            auto& slot = st.history[cif_ % 16];
            const auto src = cif.subspan(st.config.start_cu * kDabCuBits, bits);
            std::copy(src.begin(), src.end(), slot.begin());
            ++st.filled;
            if (st.filled < 16) {
                continue;
            }
            // Clause 12: c(r, i) = b(r - delay(i), i), so codeword s = r - 15
            // takes bit i from CIF s + delay(i).
            const std::uint64_t first = cif_ - 15;
            std::vector<float> codeword(bits);
            for (std::size_t i = 0; i < bits; ++i) {
                codeword[i] = st.history[(first + dab_time_interleave_delay(i)) % 16][i];
            }
            auto decoded =
                dab_convolutional_decode(codeword, st.profile.information_bits, st.profile.runs);
            if (!decoded) {
                continue;
            }
            // Clause 10.3: each logical frame scrambled from PRBS index 0.
            const auto prbs = dab_prbs(decoded->size());
            for (std::size_t i = 0; i < decoded->size(); ++i) {
                (*decoded)[i] ^= prbs[i];
            }
            DabLogicalFrame frame;
            frame.subchannel = id;
            frame.cif = first;
            frame.bytes = bits_to_bytes(*decoded);
            if (callbacks_.on_frame) {
                callbacks_.on_frame(frame);
            }
            bool packet = false;
            for (const auto& [scid, pc] : ensemble_.packet_components) {
                packet = packet || pc.subchannel == id;
            }
            if (!packet) {
                continue;
            }
            if (!st.packets) {
                st.packets.emplace(id);
            }
            std::vector<DabDataGroup> groups;
            st.packets->push(frame.bytes, groups);
            for (const auto& g : groups) {
                if (callbacks_.on_data_group) {
                    callbacks_.on_data_group(g);
                }
                std::vector<MotObject> objects;
                st.mot.push(g, objects);
                for (const auto& o : objects) {
                    if (callbacks_.on_mot_object) {
                        callbacks_.on_mot_object(o);
                    }
                }
            }
        }
        ++cif_;
    }
}

}  // namespace revenant::decode
