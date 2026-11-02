#include "core/decode/asterix_cat021.h"

#include <array>
#include <format>

#include "core/decode/ais.h"

namespace revenant::decode {

namespace {

enum class Shape : std::uint8_t {
    Unused,
    Fixed,
    Variable,    // one-octet parts while the LSB, FX, is set
    Repetitive,  // REP octet then REP parts of `size`
    Explicit,    // length octet counting itself
    Met,         // I021/220
    Intent,      // I021/110
    Ages,        // I021/295
};

struct Format {
    unsigned item = 0;
    Shape shape = Shape::Unused;
    std::size_t size = 0;
};

// Part 12 Table 1 in FRN order, index 0 for FRN 1, with each item's format
// and length from its own clause in 5.2.
constexpr std::array<Format, kCat021LastFrn> kUap = {{
    {10, Shape::Fixed, 2},    {40, Shape::Variable, 0}, {161, Shape::Fixed, 2},
    {15, Shape::Fixed, 1},    {71, Shape::Fixed, 3},    {130, Shape::Fixed, 6},
    {131, Shape::Fixed, 8},   {72, Shape::Fixed, 3},    {150, Shape::Fixed, 2},
    {151, Shape::Fixed, 2},   {80, Shape::Fixed, 3},    {73, Shape::Fixed, 3},
    {74, Shape::Fixed, 4},    {75, Shape::Fixed, 3},    {76, Shape::Fixed, 4},
    {140, Shape::Fixed, 2},   {90, Shape::Variable, 0}, {210, Shape::Fixed, 1},
    {70, Shape::Fixed, 2},    {230, Shape::Fixed, 2},   {145, Shape::Fixed, 2},
    {152, Shape::Fixed, 2},   {200, Shape::Fixed, 1},   {155, Shape::Fixed, 2},
    {157, Shape::Fixed, 2},   {160, Shape::Fixed, 4},   {165, Shape::Fixed, 2},
    {77, Shape::Fixed, 3},    {170, Shape::Fixed, 6},   {20, Shape::Fixed, 1},
    {220, Shape::Met, 0},     {146, Shape::Fixed, 2},   {148, Shape::Fixed, 2},
    {110, Shape::Intent, 0},  {16, Shape::Fixed, 1},    {8, Shape::Fixed, 1},
    {271, Shape::Variable, 0}, {132, Shape::Fixed, 1},  {250, Shape::Repetitive, 8},
    {260, Shape::Fixed, 7},   {400, Shape::Fixed, 1},   {295, Shape::Ages, 0},
    {0, Shape::Unused, 0},    {0, Shape::Unused, 0},    {0, Shape::Unused, 0},
    {0, Shape::Unused, 0},    {0, Shape::Unused, 0},    {0, Shape::Explicit, 0},
    {0, Shape::Explicit, 0},
}};

// Octets from `at` to the first whose LSB is clear, inclusive.
std::optional<std::size_t> fx_run(std::span<const std::uint8_t> d, std::size_t at) {
    std::size_t n = 0;
    while (at + n < d.size()) {
        if ((d[at + n] & 1U) == 0U) {
            return n + 1;
        }
        ++n;
    }
    return std::nullopt;
}

// The length of the item starting at `at`, or nothing when it runs past.
std::optional<std::size_t> item_length(const Format& f, std::span<const std::uint8_t> d,
                                       std::size_t at) {
    const std::size_t left = at <= d.size() ? d.size() - at : 0;
    switch (f.shape) {
        case Shape::Unused: return std::nullopt;
        case Shape::Fixed: return f.size <= left ? std::optional(f.size) : std::nullopt;
        case Shape::Variable: return fx_run(d, at);
        case Shape::Repetitive: {
            if (left < 1) {
                return std::nullopt;
            }
            const std::size_t n = 1 + static_cast<std::size_t>(d[at]) * f.size;
            return n <= left ? std::optional(n) : std::nullopt;
        }
        case Shape::Explicit: {
            if (left < 1 || d[at] < 1 || d[at] > left) {
                return std::nullopt;
            }
            return static_cast<std::size_t>(d[at]);
        }
        case Shape::Met: {
            // 5.2.36: one primary octet, WS, WD, TMP of two octets and TRB
            // of one, bits 8 to 5.
            if (left < 1) {
                return std::nullopt;
            }
            const std::uint8_t p = d[at];
            std::size_t n = 1;
            n += (p & 0x80U) != 0U ? 2 : 0;
            n += (p & 0x40U) != 0U ? 2 : 0;
            n += (p & 0x20U) != 0U ? 2 : 0;
            n += (p & 0x10U) != 0U ? 1 : 0;
            if ((p & 1U) != 0U) {
                return std::nullopt;  // no extension is defined
            }
            return n <= left ? std::optional(n) : std::nullopt;
        }
        case Shape::Intent: {
            // 5.2.17: primary octet, TIS bit 8 and TI bit 7. TIS is itself
            // extended by FX; TI is repetitive, fifteen octets a point.
            if (left < 1) {
                return std::nullopt;
            }
            const std::uint8_t p = d[at];
            if ((p & 1U) != 0U) {
                return std::nullopt;
            }
            std::size_t n = 1;
            if ((p & 0x80U) != 0U) {
                const auto tis = fx_run(d, at + n);
                if (!tis) {
                    return std::nullopt;
                }
                n += *tis;
            }
            if ((p & 0x40U) != 0U) {
                if (at + n >= d.size()) {
                    return std::nullopt;
                }
                n += 1 + static_cast<std::size_t>(d[at + n]) * 15;
            }
            return n <= left ? std::optional(n) : std::nullopt;
        }
        case Shape::Ages: {
            // 5.2.41: a primary of up to four octets, FX-extended, whose
            // presence bits each bring a one-octet age.
            const auto primary = fx_run(d, at);
            if (!primary || *primary > 4) {
                return std::nullopt;
            }
            std::size_t n = *primary;
            for (std::size_t k = 0; k < *primary; ++k) {
                for (unsigned b = 7; b >= 1; --b) {
                    n += (d[at + k] >> b) & 1U;
                }
            }
            return n <= left ? std::optional(n) : std::nullopt;
        }
    }
    return std::nullopt;
}

std::uint32_t be(std::span<const std::uint8_t> o, std::size_t at, std::size_t n) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        v = (v << 8U) | o[at + i];
    }
    return v;
}

std::int32_t sbe(std::span<const std::uint8_t> o, std::size_t at, std::size_t n) {
    const std::uint32_t v = be(o, at, n);
    const unsigned bits = static_cast<unsigned>(n * 8);
    if (bits < 32 && (v >> (bits - 1U)) != 0U) {
        return static_cast<std::int32_t>(v) - static_cast<std::int32_t>(std::uint32_t{1} << bits);
    }
    return static_cast<std::int32_t>(v);
}

// Fifteen bits of two's complement under an RE bit, 5.2.28 and 5.2.29.
double rate15(std::span<const std::uint8_t> o) {
    std::int32_t v = static_cast<std::int32_t>(be(o, 0, 2) & 0x7FFFU);
    if ((v & 0x4000) != 0) {
        v -= 0x8000;
    }
    return 6.25 * v;
}

void decode_item(Cat021Record& r, unsigned item, std::span<const std::uint8_t> o) {
    switch (item) {
        case 10:
            r.sac = o[0];
            r.sic = o[1];
            break;
        case 161: r.track_number = static_cast<std::uint16_t>(be(o, 0, 2) & 0x0FFFU); break;
        case 15: r.service_id = o[0]; break;
        case 71: r.time_of_applicability_position = be(o, 0, 3) / 128.0; break;
        case 73: r.time_of_reception_position = be(o, 0, 3) / 128.0; break;
        case 130:
            if (!r.latitude) {
                r.latitude = sbe(o, 0, 3) * (180.0 / 8388608.0);
                r.longitude = sbe(o, 3, 3) * (180.0 / 8388608.0);
            }
            break;
        case 131:
            r.latitude = sbe(o, 0, 4) * (180.0 / 1073741824.0);
            r.longitude = sbe(o, 4, 4) * (180.0 / 1073741824.0);
            break;
        case 80: r.target_address = be(o, 0, 3); break;
        case 140: r.geometric_height_ft = 6.25 * sbe(o, 0, 2); break;
        case 145: r.flight_level = 0.25 * sbe(o, 0, 2); break;
        case 151: r.true_airspeed_kt = static_cast<std::uint16_t>(be(o, 0, 2) & 0x7FFFU); break;
        case 152: r.magnetic_heading_deg = be(o, 0, 2) * (360.0 / 65536.0); break;
        case 155: r.barometric_vertical_rate_fpm = rate15(o); break;
        case 157: r.geometric_vertical_rate_fpm = rate15(o); break;
        case 160:
            // 2^-14 NM/s is 3600 / 16384 knots.
            r.ground_speed_kt = (be(o, 0, 2) & 0x7FFFU) * (3600.0 / 16384.0);
            r.track_angle_deg = be(o, 2, 2) * (360.0 / 65536.0);
            break;
        case 70: r.mode_3a = static_cast<std::uint16_t>(be(o, 0, 2) & 0x0FFFU); break;
        case 170: {
            std::string id;
            for (unsigned i = 0; i < 8; ++i) {
                const unsigned shift = 42U - 6U * i;
                const std::uint64_t all =
                    (static_cast<std::uint64_t>(be(o, 0, 3)) << 24U) | be(o, 3, 3);
                id.push_back(ais_sixbit_char(static_cast<unsigned>((all >> shift) & 0x3FU)));
            }
            while (!id.empty() && (id.back() == ' ' || id.back() == '@')) {
                id.pop_back();
            }
            r.target_identification = id;
            break;
        }
        case 20: r.emitter_category = o[0]; break;
        case 132: r.message_amplitude_dbm = static_cast<std::int8_t>(o[0]); break;
        default: break;
    }
}

}  // namespace

Expected<std::vector<Cat021Record>> asterix_cat021_parse(std::span<const std::uint8_t> block) {
    if (block.size() < 3) {
        return fail("an ASTERIX data block is at least CAT and LEN, three octets");
    }
    if (block[0] != kAsterixCat021) {
        return fail(std::format("ASTERIX category {} is not 021", block[0]));
    }
    const std::size_t len = be(block, 1, 2);
    if (len != block.size()) {
        return fail(std::format("ASTERIX LEN says {} octets and the block has {}", len,
                                block.size()));
    }
    std::vector<Cat021Record> records;
    std::size_t at = 3;
    while (at < block.size()) {
        const auto fspec_len = fx_run(block, at);
        if (!fspec_len) {
            return fail(std::format("the FSPEC at octet {} runs past the block", at));
        }
        Cat021Record r;
        const std::size_t fspec = at;
        at += *fspec_len;
        for (std::size_t k = 0; k < *fspec_len; ++k) {
            for (unsigned b = 0; b < 7; ++b) {
                if (((block[fspec + k] >> (7U - b)) & 1U) == 0U) {
                    continue;
                }
                const auto frn = static_cast<unsigned>(k * 7 + b + 1);
                if (frn > kCat021LastFrn || kUap[frn - 1].shape == Shape::Unused) {
                    return fail(std::format("the FSPEC names FRN {}, which Table 1 does not use",
                                            frn));
                }
                const Format& f = kUap[frn - 1];
                const auto n = item_length(f, block, at);
                if (!n) {
                    return fail(std::format("FRN {} at octet {} runs past the block", frn, at));
                }
                Cat021Item item{frn, f.item, {block.begin() + static_cast<std::ptrdiff_t>(at),
                                              block.begin() + static_cast<std::ptrdiff_t>(at + *n)}};
                decode_item(r, f.item, item.octets);
                r.items.push_back(std::move(item));
                at += *n;
            }
        }
        records.push_back(std::move(r));
    }
    return records;
}

}  // namespace revenant::decode
