#include "core/decode/cospas_sarsat.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <format>
#include <numbers>

namespace revenant::decode {

namespace {

// T.001 Table A3, modified-Baudot: letters, then figures.
struct BaudotEntry {
    char c;
    unsigned code;
};
constexpr BaudotEntry kBaudot[] = {
    {'A', 0b111000}, {'B', 0b110011}, {'C', 0b101110}, {'D', 0b110010}, {'E', 0b110000},
    {'F', 0b110110}, {'G', 0b101011}, {'H', 0b100101}, {'I', 0b101100}, {'J', 0b111010},
    {'K', 0b111110}, {'L', 0b101001}, {'M', 0b100111}, {'N', 0b100110}, {'O', 0b100011},
    {'P', 0b101101}, {'Q', 0b111101}, {'R', 0b101010}, {'S', 0b110100}, {'T', 0b100001},
    {'U', 0b111100}, {'V', 0b101111}, {'W', 0b111001}, {'X', 0b110111}, {'Y', 0b110101},
    {'Z', 0b110001}, {' ', 0b100100}, {'-', 0b011000}, {'/', 0b010111}, {'0', 0b001101},
    {'1', 0b011101}, {'2', 0b011001}, {'3', 0b010000}, {'4', 0b001010}, {'5', 0b000001},
    {'6', 0b010101}, {'7', 0b011100}, {'8', 0b001100}, {'9', 0b000011},
};

const BchCodec& bch1() {
    // Annex B1: m1(X) = X^7 + X^3 + 1, g = LCM(m1, m3, m5), t = 3.
    static const BchCodec c = BchCodec::create({7, 0x89, 7, {}}).value();
    return c;
}

const BchCodec& bch2() {
    // Annex B2: (1 + x + x^6)(1 + x + x^2 + x^4 + x^6), t = 2.
    static const BchCodec c = BchCodec::create({6, 0x43, 5, {}}).value();
    return c;
}

// Bits by their T.001 number, 1-based, most significant first.
struct Bits {
    std::span<const std::uint8_t> b;
    [[nodiscard]] unsigned at(std::size_t n) const { return b[n - 1] & 1U; }
    [[nodiscard]] std::uint32_t range(std::size_t first, std::size_t last) const {
        std::uint32_t v = 0;
        for (std::size_t i = first; i <= last; ++i) {
            v = (v << 1U) | at(i);
        }
        return v;
    }
};

std::string baudot_string(const Bits& b, std::size_t first, std::size_t chars, unsigned width = 6) {
    std::string s;
    for (std::size_t i = 0; i < chars; ++i) {
        unsigned code = b.range(first + i * width, first + i * width + width - 1);
        if (width == 5) {
            code |= 0x20U;  // A3.3.5.2 note 2: letters with the leading 1 deleted
        }
        s.push_back(cs_baudot_decode(code));
    }
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && s.back() == ' ') {
        s.pop_back();
    }
    while (!s.empty() && s.front() == ' ') {
        s.erase(s.begin());
    }
    return s;
}

CsProtocol user_protocol(unsigned code) {
    // Table A2-A, bits 37-39.
    switch (code) {
        case 0b010: return CsProtocol::Maritime;
        case 0b110: return CsProtocol::RadioCallSign;
        case 0b001: return CsProtocol::Aviation;
        case 0b011: return CsProtocol::Serial;
        case 0b111: return CsProtocol::Test;
        case 0b000: return CsProtocol::Orbitography;
        case 0b100: return CsProtocol::NationalUser;
        default: return CsProtocol::SecondGeneration;
    }
}

CsProtocol location_protocol(unsigned code) {
    // Table A2-B, bits 37-40.
    switch (code) {
        case 0b0010: return CsProtocol::StandardMmsi;
        case 0b0011: return CsProtocol::StandardAircraftAddress;
        case 0b0100: return CsProtocol::StandardEltSerial;
        case 0b0101: return CsProtocol::StandardEltOperator;
        case 0b0110: return CsProtocol::StandardEpirbSerial;
        case 0b0111: return CsProtocol::StandardPlbSerial;
        case 0b1100: return CsProtocol::ShipSecurity;
        case 0b1000: return CsProtocol::NationalElt;
        case 0b1010: return CsProtocol::NationalEpirb;
        case 0b1011: return CsProtocol::NationalPlb;
        case 0b1110: return CsProtocol::StandardTest;
        case 0b1111: return CsProtocol::NationalTest;
        case 0b1101: return CsProtocol::Rls;
        case 0b1001: return CsProtocol::EltDt;
        default: return CsProtocol::Spare;
    }
}

bool is_standard(CsProtocol p) {
    switch (p) {
        case CsProtocol::StandardMmsi:
        case CsProtocol::StandardAircraftAddress:
        case CsProtocol::StandardEltSerial:
        case CsProtocol::StandardEltOperator:
        case CsProtocol::StandardEpirbSerial:
        case CsProtocol::StandardPlbSerial:
        case CsProtocol::ShipSecurity:
        case CsProtocol::StandardTest: return true;
        default: return false;
    }
}

bool is_national(CsProtocol p) {
    return p == CsProtocol::NationalElt || p == CsProtocol::NationalEpirb || p == CsProtocol::NationalPlb ||
           p == CsProtocol::NationalTest;
}

// A PDF-2 offset: sign, minutes and seconds fields. Returns degrees, or
// nothing for the default (minutes zero, seconds 1111: A3.2 b and c).
std::optional<double> offset(const Bits& b, std::size_t sign_bit, unsigned minute_bits) {
    const std::size_t m0 = sign_bit + 1;
    const std::uint32_t minutes = b.range(m0, m0 + minute_bits - 1);
    const std::uint32_t seconds4 = b.range(m0 + minute_bits, m0 + minute_bits + 3);
    if (seconds4 == 15) {
        return std::nullopt;
    }
    const double v = minutes / 60.0 + seconds4 * 4.0 / 3600.0;
    return b.at(sign_bit) != 0U ? v : -v;
}

void apply_offsets(CsPosition& p, std::optional<double> dlat, std::optional<double> dlon) {
    // A3.3.1: offsets add to the magnitude in the flagged direction.
    const auto add = [](double coord, double d) {
        const double mag = std::abs(coord) + d;
        return coord < 0.0 ? -mag : mag;
    };
    if (dlat) {
        p.latitude = add(p.latitude, *dlat);
        p.refined = true;
    }
    if (dlon) {
        p.longitude = add(p.longitude, *dlon);
        p.refined = true;
    }
}

void decode_user_identity(const Bits& b, CsMessage& m) {
    switch (m.protocol) {
        case CsProtocol::Maritime: {
            // A2.2: six characters in 40-75, beacon number 76-81.
            const std::string id = trim(baudot_string(b, 40, 6));
            const bool digits = !id.empty() && std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; });
            if (digits && id.size() == 6) {
                m.mmsi = m.country_code * 1000000U + static_cast<std::uint32_t>(std::stoul(id));
                m.identity = std::format("MMSI {:09}", *m.mmsi);
            } else {
                m.identity = "call sign " + id;
            }
            m.identity += std::format(", beacon {}", cs_baudot_decode(b.range(76, 81)));
            break;
        }
        case CsProtocol::RadioCallSign: {
            // A2.3: four Baudot characters 40-63, three BCD digits 64-75.
            std::string id = baudot_string(b, 40, 4);
            for (std::size_t i = 0; i < 3; ++i) {
                const auto d = b.range(64 + 4 * i, 67 + 4 * i);
                id.push_back(d <= 9 ? static_cast<char>('0' + d) : ' ');
            }
            m.identity = "call sign " + trim(id) + std::format(", beacon {}", cs_baudot_decode(b.range(76, 81)));
            break;
        }
        case CsProtocol::Aviation:
            // A2.4: seven characters 40-81, ELT number 82-83.
            m.identity = "registration " + trim(baudot_string(b, 40, 7)) + std::format(", ELT {}", b.range(82, 83));
            break;
        case CsProtocol::Serial: {
            // A2.5: beacon type 40-42, certificate flag 43.
            const unsigned type = b.range(40, 42);
            const bool cert = b.at(43) != 0U;
            std::string kind;
            switch (type) {
                case 0b000: kind = "ELT serial"; break;
                case 0b010: kind = "float free EPIRB serial"; break;
                case 0b100: kind = "non float free EPIRB serial"; break;
                case 0b110: kind = "PLB serial"; break;
                case 0b011: kind = "ELT 24-bit address"; break;
                case 0b001: kind = "ELT operator"; break;
                default: kind = "spare serial type"; break;
            }
            if (type == 0b011) {
                m.aircraft_address = b.range(44, 67);
                m.identity = std::format("{} {:06X}, ELT {}", kind, *m.aircraft_address, b.range(68, 73));
            } else if (type == 0b001) {
                m.identity = std::format("{} {}, serial {}", kind, baudot_string(b, 44, 3), b.range(62, 73));
            } else {
                m.identity = std::format("{} {}", kind, b.range(44, 63));
            }
            if (cert) {
                m.identity += std::format(", C/S TAC {}", b.range(74, 83));
            }
            break;
        }
        case CsProtocol::Test: m.identity = "test"; break;
        case CsProtocol::Orbitography: m.identity = "orbitography"; break;
        case CsProtocol::NationalUser: m.identity = std::format("national user {:08X}", b.range(40, 71)); break;
        default: m.identity = "second generation"; break;
    }
}

}  // namespace

const char* cs_protocol_name(CsProtocol p) {
    switch (p) {
        case CsProtocol::Maritime: return "maritime user";
        case CsProtocol::RadioCallSign: return "radio call sign user";
        case CsProtocol::Aviation: return "aviation user";
        case CsProtocol::Serial: return "serial user";
        case CsProtocol::Test: return "test user";
        case CsProtocol::Orbitography: return "orbitography";
        case CsProtocol::NationalUser: return "national user";
        case CsProtocol::SecondGeneration: return "second generation";
        case CsProtocol::StandardMmsi: return "standard location, EPIRB MMSI";
        case CsProtocol::StandardAircraftAddress: return "standard location, ELT 24-bit address";
        case CsProtocol::StandardEltSerial: return "standard location, ELT serial";
        case CsProtocol::StandardEltOperator: return "standard location, ELT operator";
        case CsProtocol::StandardEpirbSerial: return "standard location, EPIRB serial";
        case CsProtocol::StandardPlbSerial: return "standard location, PLB serial";
        case CsProtocol::ShipSecurity: return "standard location, ship security";
        case CsProtocol::NationalElt: return "national location, ELT";
        case CsProtocol::NationalEpirb: return "national location, EPIRB";
        case CsProtocol::NationalPlb: return "national location, PLB";
        case CsProtocol::StandardTest: return "standard location, test";
        case CsProtocol::NationalTest: return "national location, test";
        case CsProtocol::Rls: return "RLS location";
        case CsProtocol::EltDt: return "ELT(DT) location";
        case CsProtocol::Spare: return "spare";
    }
    return "?";
}

std::optional<unsigned> cs_baudot_encode(char c) {
    for (const auto& e : kBaudot) {
        if (e.c == c) {
            return e.code;
        }
    }
    return std::nullopt;
}

char cs_baudot_decode(unsigned code) {
    for (const auto& e : kBaudot) {
        if (e.code == code) {
            return e.c;
        }
    }
    return '?';
}

void cs_fill_bch(std::span<std::uint8_t> bits) {
    std::array<std::uint8_t, 21> c1{};
    (void)bch1().encode(bits.subspan(24, 61), c1);
    std::copy(c1.begin(), c1.end(), bits.begin() + 85);
    if (bits.size() >= kCsLongBits) {
        std::array<std::uint8_t, 12> c2{};
        (void)bch2().encode(bits.subspan(106, 26), c2);
        std::copy(c2.begin(), c2.end(), bits.begin() + 132);
    }
}

Expected<CsMessage> cs_parse(std::span<const std::uint8_t> in) {
    if (in.size() < kCsShortBits) {
        return fail("Cospas-Sarsat: fewer than 112 bits");
    }
    CsMessage m;
    m.bits.assign(in.begin(), in.end());
    for (auto& v : m.bits) {
        v &= 1U;
    }
    const Bits raw{m.bits};
    const std::uint32_t sync = raw.range(16, 24);
    if (sync == kCsFrameSyncTest) {
        m.self_test = true;
    } else if (sync != kCsFrameSync) {
        return fail("Cospas-Sarsat: frame sync is neither pattern of 2.2.8.2");
    }
    m.long_message = raw.at(25) != 0U;
    const std::size_t length = m.long_message ? kCsLongBits : kCsShortBits;
    if (m.bits.size() < length) {
        return fail("Cospas-Sarsat: long message truncated");
    }
    m.bits.resize(length);

    // Section 3.2: BCH-1 over bits 25-106, BCH-2 over 107-144.
    m.bch1 = bch1().decode(std::span(m.bits).subspan(24, 82));
    if (!m.bch1) {
        return fail("Cospas-Sarsat: BCH-1 refused");
    }
    if (m.long_message) {
        m.bch2 = bch2().decode(std::span(m.bits).subspan(106, 38));
    }
    const Bits b{m.bits};

    m.user_protocol = b.at(26) != 0U;
    m.country_code = b.range(27, 36);
    if (m.user_protocol) {
        m.protocol = user_protocol(b.range(37, 39));
    } else {
        m.protocol = m.long_message ? location_protocol(b.range(37, 40)) : CsProtocol::Spare;
    }

    // Section 3.3: the 15 Hex ID is bits 26-85 with PDF-1 position at its
    // A3.2 default.
    std::vector<std::uint8_t> id(m.bits.begin() + 25, m.bits.begin() + 85);  // bits 26..85
    const auto set_default = [&](std::size_t first, const char* pattern) {
        for (std::size_t i = 0; pattern[i] != '\0'; ++i) {
            id[first - 26 + i] = static_cast<std::uint8_t>(pattern[i] - '0');
        }
    };
    if (is_standard(m.protocol)) {
        set_default(65, "011111111101111111111");  // A3.3.5.3
    } else if (is_national(m.protocol)) {
        set_default(59, "011111110000001111111100000");  // A3.3.6.2
    } else if (m.protocol == CsProtocol::Rls || m.protocol == CsProtocol::EltDt) {
        set_default(67, "0111111110111111111");  // A3.3.7.1, A3.3.8.2
    }
    for (std::size_t i = 0; i < 60; i += 4) {
        const unsigned nib = (id[i] << 3U) | (id[i + 1] << 2U) | (id[i + 2] << 1U) | id[i + 3];
        m.hex_id.push_back("0123456789ABCDEF"[nib]);
    }

    const bool pdf2 = m.long_message && m.bch2.has_value();
    if (m.user_protocol) {
        decode_user_identity(b, m);
        if (m.protocol != CsProtocol::Orbitography) {
            m.homing = b.range(84, 85);
        }
        if (!m.long_message) {
            m.non_protected = b.range(107, 112);
        } else if (pdf2) {
            // A3.3.4.3: user-location position, 4-minute resolution.
            const std::uint32_t lat_deg = b.range(109, 115);
            const std::uint32_t lon_deg = b.range(121, 128);
            if (lat_deg <= 90 && lon_deg <= 180) {
                CsPosition p;
                p.latitude = lat_deg + b.range(116, 119) * 4.0 / 60.0;
                p.longitude = lon_deg + b.range(129, 132) * 4.0 / 60.0;
                if (b.at(108) != 0U) {
                    p.latitude = -p.latitude;
                }
                if (b.at(120) != 0U) {
                    p.longitude = -p.longitude;
                }
                p.refined = true;
                p.internal_source = b.at(107) != 0U;
                m.position = p;
            }
        }
        return m;
    }

    if (!m.long_message) {
        m.identity = "short location protocol (withdrawn, A3.1 note)";
        return m;
    }

    CsPosition p;
    bool have = false;
    if (is_standard(m.protocol)) {
        // A3.3.5.2 identification.
        switch (m.protocol) {
            case CsProtocol::StandardMmsi:
                m.mmsi = m.country_code * 1000000U + b.range(41, 60);
                m.identity = std::format("MMSI {:09}, beacon {}", *m.mmsi, b.range(61, 64));
                break;
            case CsProtocol::ShipSecurity:
                m.mmsi = m.country_code * 1000000U + b.range(41, 60);
                m.identity = std::format("ship security MMSI {:09}", *m.mmsi);
                break;
            case CsProtocol::StandardAircraftAddress:
                m.aircraft_address = b.range(41, 64);
                m.identity = std::format("aircraft address {:06X}", *m.aircraft_address);
                break;
            case CsProtocol::StandardEltOperator:
                m.identity = std::format("operator {}, serial {}", baudot_string(b, 41, 3, 5), b.range(56, 64));
                break;
            default:
                m.identity = std::format("C/S TAC {}, serial {}", b.range(41, 50), b.range(51, 64));
                break;
        }
        // A3.3.5.3: quarter degrees.
        const std::uint32_t lat = b.range(66, 74);
        const std::uint32_t lon = b.range(76, 85);
        if (lat != 0x1FF && lon != 0x3FF) {
            p.latitude = (b.at(65) != 0U ? -1.0 : 1.0) * lat * 0.25;
            p.longitude = (b.at(75) != 0U ? -1.0 : 1.0) * lon * 0.25;
            have = true;
            if (pdf2) {
                p.internal_source = b.at(111) != 0U;
                m.homing = b.at(112);
                apply_offsets(p, offset(b, 113, 5), offset(b, 123, 5));  // A3.3.5.4
            }
        }
    } else if (is_national(m.protocol)) {
        m.identity = std::format("national serial {}", b.range(41, 58));
        // A3.3.6.2: degrees and 2-minute steps.
        const std::uint32_t lat_deg = b.range(60, 66);
        const std::uint32_t lon_deg = b.range(73, 80);
        if (lat_deg != 0x7F && lon_deg != 0xFF) {
            p.latitude = (lat_deg + b.range(67, 71) * 2.0 / 60.0) * (b.at(59) != 0U ? -1.0 : 1.0);
            p.longitude = (lon_deg + b.range(81, 85) * 2.0 / 60.0) * (b.at(72) != 0U ? -1.0 : 1.0);
            have = true;
            if (pdf2) {
                p.internal_source = b.at(111) != 0U;
                m.homing = b.at(112);
                if (b.at(110) != 0U) {  // A3.3.6.3 b: offsets present
                    apply_offsets(p, offset(b, 113, 2), offset(b, 120, 2));
                }
            }
        }
    } else if (m.protocol == CsProtocol::Rls || m.protocol == CsProtocol::EltDt) {
        if (m.protocol == CsProtocol::Rls) {
            if (b.range(43, 46) == 0xF) {
                m.mmsi = m.country_code * 1000000U + b.range(47, 66);
                m.identity = std::format("RLS MMSI {:09}", *m.mmsi);
            } else {
                m.identity = std::format("RLS type {}, TAC {}, serial {}", b.range(41, 42), b.range(43, 52), b.range(53, 66));
            }
        } else {
            const unsigned kind = b.range(41, 42);
            if (kind == 0) {
                m.aircraft_address = b.range(43, 66);
                m.identity = std::format("ELT(DT) aircraft address {:06X}", *m.aircraft_address);
            } else {
                m.identity = std::format("ELT(DT) identity type {}, {:06X}", kind, b.range(43, 66));
            }
        }
        // A3.3.7.1 and A3.3.8.2: half degrees.
        const std::uint32_t lat = b.range(68, 75);
        const std::uint32_t lon = b.range(77, 85);
        if (lat != 0xFF && lon != 0x1FF) {
            p.latitude = (b.at(67) != 0U ? -1.0 : 1.0) * lat * 0.5;
            p.longitude = (b.at(76) != 0U ? -1.0 : 1.0) * lon * 0.5;
            have = true;
            if (pdf2) {
                if (m.protocol == CsProtocol::Rls) {
                    p.internal_source = b.at(107) != 0U;
                    m.homing = b.at(108);
                }
                apply_offsets(p, offset(b, 115, 4), offset(b, 124, 4));
            }
        }
    } else {
        m.identity = "spare protocol";
    }
    if (have) {
        m.position = p;
    }
    return m;
}

// ---------------------------------------------------------------------------
// The receiver
// ---------------------------------------------------------------------------

namespace {

// Textbook iterative radix-2 FFT, in place; n a power of two.
void fft(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1U;
        for (; (j & bit) != 0U; bit >>= 1U) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1U) {
        const auto w = std::polar(1.0, -2.0 * std::numbers::pi / static_cast<double>(len));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> wk(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * wk;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                wk *= w;
            }
        }
    }
}

}  // namespace

Expected<CsDecoder> CsDecoder::create(const CsDecoderConfig& config) {
    if (config.rate < 4000) {
        return fail("Cospas-Sarsat decoder: the sample rate must be at least 4 kHz");
    }
    if (!(config.trigger_ratio > 1.0)) {
        return fail("Cospas-Sarsat decoder: trigger_ratio must exceed 1");
    }
    CsDecoder d;
    d.config_ = config;
    // The largest power of two no longer than 20 ms.
    d.window_ = 1;
    while (d.window_ * 2 <= static_cast<std::size_t>(0.020 * config.rate)) {
        d.window_ *= 2;
    }
    d.capture_len_ = static_cast<std::size_t>(0.62 * config.rate);
    d.history_.reserve(d.window_);
    return d;
}

void CsDecoder::reset() {
    history_.clear();
    samples_ = 0;
    capturing_ = false;
    capture_.clear();
    holdoff_until_ = 0;
}

void CsDecoder::push(dsp::ConstComplexSpan in, std::vector<CsBurst>& out) {
    for (const auto& x : in) {
        ++samples_;
        if (capturing_) {
            capture_.push_back(x);
            if (capture_.size() >= capture_len_) {
                process_capture(out);
                capturing_ = false;
                capture_.clear();
                holdoff_until_ = samples_;
                history_.clear();
            }
            continue;
        }
        history_.push_back(x);
        if (history_.size() < window_) {
            continue;
        }
        // The carrier of 2.2.6 is one spectral line: trigger on the peak
        // bin of this block against the mean bin.
        std::vector<std::complex<double>> a(window_);
        for (std::size_t i = 0; i < window_; ++i) {
            a[i] = {history_[i].real(), history_[i].imag()};
        }
        fft(a);
        double peak = 0.0;
        double total = 0.0;
        for (const auto& v : a) {
            const double p = std::norm(v);
            peak = std::max(peak, p);
            total += p;
        }
        const double mean = total / static_cast<double>(window_);
        if (samples_ >= holdoff_until_ && mean > 0.0 && peak > config_.trigger_ratio * mean) {
            capturing_ = true;
            capture_start_ = samples_ - window_;
            capture_.assign(history_.begin(), history_.end());
        }
        history_.clear();
    }
}

void CsDecoder::process_capture(std::vector<CsBurst>& out) {
    using C = std::complex<double>;
    const double fs = static_cast<double>(config_.rate);
    const double tb = fs / kCsBitRate;
    const std::size_t n = capture_.size();
    // The capture starts with the block that triggered, so the carrier rose
    // no earlier than its first sample and no later than its last.
    const std::size_t rise = 0;
    // T.001 2.2.6: 160 ms of carrier. Frequency from the peak of a
    // zero-padded spectrum over 15 to 135 ms after the rise, which is
    // coherent over the whole span; phase from the derotated mean over it.
    const std::size_t c0 = rise + static_cast<std::size_t>(0.015 * fs);
    const std::size_t c1 = std::min(n, rise + static_cast<std::size_t>(0.135 * fs));
    if (c1 <= c0 + 16) {
        ++failed_;
        return;
    }
    // Doppler moves during a burst (tens of hertz a second on a LEOSAR pass),
    // so the frequency is measured on each half of the span and the carrier
    // modelled as a quadratic phase about the span's centre.
    const auto frequency = [&](std::size_t a, std::size_t b) {
        std::size_t size = 1;
        while (size < 8 * (b - a)) {
            size *= 2;
        }
        std::vector<C> spec(size);
        for (std::size_t i = a; i < b; ++i) {
            spec[i - a] = C(capture_[i].real(), capture_[i].imag());
        }
        fft(spec);
        std::size_t k = 0;
        for (std::size_t i = 1; i < size; ++i) {
            if (std::norm(spec[i]) > std::norm(spec[k])) {
                k = i;
            }
        }
        // Parabolic interpolation on the magnitudes either side.
        const double ym = std::abs(spec[(k + size - 1) % size]);
        const double y0 = std::abs(spec[k]);
        const double yp = std::abs(spec[(k + 1) % size]);
        const double den = ym - 2.0 * y0 + yp;
        const double frac = den != 0.0 ? 0.5 * (ym - yp) / den : 0.0;
        double bin = static_cast<double>(k) + frac;
        if (bin > static_cast<double>(size) / 2.0) {
            bin -= static_cast<double>(size);
        }
        return 2.0 * std::numbers::pi * bin / static_cast<double>(size);  // radians per sample
    };
    const std::size_t cm = (c0 + c1) / 2;
    const double omega = frequency(c0, c1);
    const double wa = frequency(c0, cm);
    const double wb = frequency(cm, c1);
    // Clamped to 200 Hz/s, beyond any orbit's Doppler rate at 406 MHz.
    const double alpha_limit = 2.0 * std::numbers::pi * 200.0 / (fs * fs);
    const double alpha = std::clamp((wb - wa) / (static_cast<double>(c1 - c0) / 2.0), -alpha_limit, alpha_limit);
    const double centre = static_cast<double>(cm);
    const auto carrier_model = [&](double i) {
        const double d = i - centre;
        return omega * d + 0.5 * alpha * d * d;
    };
    C ref{};
    for (std::size_t i = c0; i < c1; ++i) {
        ref += C(capture_[i].real(), capture_[i].imag()) * std::polar(1.0, -carrier_model(static_cast<double>(i)));
    }
    double phase = std::arg(ref);
    const double amplitude = std::abs(ref) / static_cast<double>(c1 - c0);
    if (amplitude <= 0.0) {
        ++failed_;
        return;
    }

    // The quadrature component carries +-sin(1.1) of the amplitude.
    std::vector<double> q(n);
    for (std::size_t i = 0; i < n; ++i) {
        const C z = C(capture_[i].real(), capture_[i].imag()) * std::polar(1.0, -(carrier_model(static_cast<double>(i)) + phase));
        q[i] = z.imag();
    }

    // Frame sync: correlate 24 bits of biphase waveform (2.2.8.1, 2.2.8.2)
    // from 100 to 260 ms after the rise.
    std::vector<std::uint8_t> head(24, 1);
    for (int i = 0; i < 9; ++i) {
        head[15 + i] = static_cast<std::uint8_t>((kCsFrameSync >> (8 - i)) & 1U);
    }
    const std::size_t s0 = rise + static_cast<std::size_t>(0.100 * fs);
    const std::size_t s1 = std::min(n, rise + static_cast<std::size_t>(0.260 * fs));
    const auto template_len = static_cast<std::size_t>(24 * tb);
    double best = 0.0;
    std::size_t best_at = 0;
    for (std::size_t start = s0; start < s1 && start + template_len < n; ++start) {
        double c = 0.0;
        for (std::size_t k = 0; k < template_len; ++k) {
            const double t = static_cast<double>(k) / tb;
            const auto bit = static_cast<std::size_t>(t);
            const bool first_half = (t - static_cast<double>(bit)) < 0.5;
            const double sign = (head[bit] != 0U) == first_half ? 1.0 : -1.0;
            c += sign * q[start + k];
        }
        if (std::abs(c) > std::abs(best)) {
            best = c;
            best_at = start;
        }
    }
    if (best == 0.0) {
        ++failed_;
        return;
    }
    // A negative peak is the phase sense reversed, as a spectrum inverted
    // somewhere upstream gives: turn the reference round.
    if (best < 0.0) {
        phase += std::numbers::pi;
    }

    // Bits, with carrier and timing loops.
    std::vector<std::uint8_t> bits;
    double t0 = static_cast<double>(best_at);  // start of the current bit
    double period = tb;
    double dphase = 0.0;
    double dfreq = 0.0;
    for (std::size_t k = 0; k < kCsLongBits; ++k) {
        const auto a = static_cast<std::size_t>(std::lround(t0));
        const auto mid = static_cast<std::size_t>(std::lround(t0 + period / 2.0));
        const auto e = static_cast<std::size_t>(std::lround(t0 + period));
        if (e + static_cast<std::size_t>(period / 4) >= n) {
            break;
        }
        double h1 = 0.0;
        double h2 = 0.0;
        C carrier{};
        for (std::size_t i = a; i < e; ++i) {
            const double ph = carrier_model(static_cast<double>(i)) + phase + dphase + dfreq * (static_cast<double>(i) - t0);
            const C z = C(capture_[i].real(), capture_[i].imag()) * std::polar(1.0, -ph);
            (i < mid ? h1 : h2) += z.imag();
            carrier += z;
        }
        const double s = h1 - h2;
        bits.push_back(s > 0.0 ? 1 : 0);
        // Carrier: the bit's mean is the residual carrier, real when locked.
        const double perr = std::arg(carrier);
        dphase += dfreq * period + 0.1 * perr;
        dfreq += 0.003 * perr / period;
        // Timing: around mid-bit the halves cancel when the estimate is right.
        const auto quarter = static_cast<std::size_t>(period / 4.0);
        double around = 0.0;
        for (std::size_t i = mid - quarter; i < mid + quarter; ++i) {
            const double ph = carrier_model(static_cast<double>(i)) + phase + dphase + dfreq * (static_cast<double>(i) - t0);
            around += (C(capture_[i].real(), capture_[i].imag()) * std::polar(1.0, -ph)).imag();
        }
        const double terr = std::abs(s) > 0.0 ? (s > 0.0 ? around : -around) / std::abs(s) : 0.0;
        // terr is -2d/T for a mid-bit d samples earlier than estimated.
        const double adjust = terr * period / 2.0;
        t0 += period + 0.1 * adjust;
        period += 0.003 * adjust;
    }
    auto msg = cs_parse(bits);
    if (!msg) {
        ++failed_;
        return;
    }
    CsBurst burst;
    burst.message = std::move(*msg);
    burst.sample_position = capture_start_ + rise;
    burst.frequency_offset_hz = (omega + alpha * (t0 - centre) + dfreq) * fs / (2.0 * std::numbers::pi);
    burst.bit_rate = fs / period;
    out.push_back(std::move(burst));
}

// ===========================================================================
// Second-generation beacons, T.018
// ===========================================================================

namespace {

using cd = std::complex<double>;

// T.018 Table 2.2, registers 22 down to 0 as printed, one character each.
constexpr std::array<const char*, 4> kSgbPresets{
    "00000000000000000000001",  // normal I
    "00110101100000111111100",  // normal Q
    "10100101100100111110000",  // self-test I
    "01111001110100100101000",  // self-test Q
};

// Appendix B.1: g(X), highest power first.
constexpr const char* kSgbGenerator = "1110001111110101110000101110111110011110010010111";

const BchCodec& sgb_bch() {
    static const BchCodec c = [] {
        BchParams p;
        // Appendix B.1: m1(X) = X^8 + X^4 + X^3 + X^2 + 1 defines the field.
        p.m = 8;
        p.field_polynomial = 0x11D;
        for (const char* q = kSgbGenerator; *q != '\0'; ++q) {
            p.generator.push_back(static_cast<std::uint8_t>(*q - '0'));
        }
        return BchCodec::create(p).value();
    }();
    return c;
}

// The preamble's half-chip template: the one-chip matched filter's output,
// sampled at the end of each I chip (even k) and each Q chip (odd k), with Q
// half a chip behind I, 2.3.3; signal level 1 - 2 * chip, Table 2.3.
std::vector<cd> sgb_preamble_samples(const std::vector<std::uint8_t>& pi,
                                     const std::vector<std::uint8_t>& pq) {
    auto lv = [](std::uint8_t c) { return 1.0 - 2.0 * c; };
    std::vector<cd> t(2 * kSgbPreambleChips - 1);
    for (std::size_t c = 0; c < kSgbPreambleChips; ++c) {
        const double q_prev = c > 0 ? lv(pq[c - 1]) : 0.0;
        t[2 * c] = {lv(pi[c]), 0.5 * (q_prev + lv(pq[c]))};
        if (2 * c + 1 < t.size()) {
            t[2 * c + 1] = {0.5 * (lv(pi[c]) + lv(pi[c + 1])), lv(pq[c])};
        }
    }
    return t;
}

std::vector<cd> sgb_differential(const std::vector<cd>& t) {
    std::vector<cd> d(t.size() - 2);
    for (std::size_t k = 0; k < d.size(); ++k) {
        d[k] = t[k + 2] * std::conj(t[k]);
    }
    return d;
}

constexpr std::size_t kSgbFft = 32768;
constexpr std::size_t kSgbTemplate = 2 * kSgbPreambleChips - 3;
// Grid points a peak must hold to be taken. A template whose end overlaps
// only the start of a burst normalises to about 1/sqrt(overlap), which can
// cross the threshold before the true peak, so the search runs for a whole
// template length past the first crossing and keeps the largest.
constexpr std::int64_t kSgbConfirm = static_cast<std::int64_t>(2 * kSgbPreambleChips);
constexpr std::size_t kSgbBits = kSgbChipsPerArm / kSgbChipsPerBit;  // 150 per arm
constexpr std::size_t kSgbPreambleBits = kSgbPreambleChips / kSgbChipsPerBit;  // 25

void ifft(std::vector<cd>& a) {
    for (auto& v : a) {
        v = std::conj(v);
    }
    fft(a);
    const double n = static_cast<double>(a.size());
    for (auto& v : a) {
        v = std::conj(v) / n;
    }
}

unsigned sgb_range(std::span<const std::uint8_t> b, std::size_t first, std::size_t last) {
    unsigned v = 0;
    for (std::size_t i = first; i <= last; ++i) {
        v = (v << 1U) | b[i - 1];
    }
    return v;
}

std::string sgb_baudot(std::span<const std::uint8_t> b, std::size_t first, std::size_t chars,
                       unsigned width) {
    std::string s;
    for (std::size_t i = 0; i < chars; ++i) {
        const std::size_t at = first + i * width;
        unsigned code = sgb_range(b, at, at + width - 1);
        if (width == 5) {
            // Table 3.1 note: the 3-letter designator drops the leading 1
            // every letter's six-bit code has.
            code |= 0b100000U;
        }
        s.push_back(cs_baudot_decode(code));
    }
    while (!s.empty() && s.back() == ' ') {
        s.pop_back();
    }
    while (!s.empty() && s.front() == ' ') {
        s.erase(s.begin());
    }
    return s;
}

}  // namespace

std::vector<std::uint8_t> sgb_prn(SgbMode mode, bool q_arm) {
    const char* preset = kSgbPresets[(mode == SgbMode::SelfTest ? 2U : 0U) + (q_arm ? 1U : 0U)];
    // Figure 2-2: output X0; X0 xor X18 into X22 after the shift.
    std::uint32_t reg = 0;
    for (unsigned i = 0; i < 23; ++i) {
        if (preset[i] == '1') {
            reg |= 1U << (22U - i);
        }
    }
    std::vector<std::uint8_t> out(kSgbChipsPerArm);
    for (auto& chip : out) {
        const std::uint32_t x0 = reg & 1U;
        chip = static_cast<std::uint8_t>(x0);
        const std::uint32_t temp = x0 ^ ((reg >> 18U) & 1U);
        reg = (reg >> 1U) | (temp << 22U);
    }
    return out;
}

void sgb_fill_bch(std::span<std::uint8_t> bits) {
    if (bits.size() < kSgbMessageBits) {
        return;
    }
    (void)sgb_bch().encode(bits.first(kSgbInformationBits),
                           bits.subspan(kSgbInformationBits, kSgbMessageBits - kSgbInformationBits));
}

const char* sgb_beacon_type_name(unsigned type) {
    // Table 3.1, bits 138-140.
    switch (type) {
        case 0b000: return "ELT";
        case 0b001: return "EPIRB";
        case 0b010: return "PLB";
        case 0b011: return "ELT(DT)";
        case 0b111: return "system beacon";
        default: return "spare";
    }
}

Expected<SgbMessage> sgb_parse(std::span<const std::uint8_t> bits, SgbMode mode) {
    if (bits.size() != kSgbMessageBits) {
        return fail(std::format("Cospas-Sarsat SGB: {} bits, not 250", bits.size()));
    }
    SgbMessage m;
    m.bits.assign(bits.begin(), bits.end());
    const auto corrected = sgb_bch().decode(m.bits);
    if (!corrected) {
        return fail("Cospas-Sarsat SGB: BCH(250,202) refused the message");
    }
    m.bch_corrected = *corrected;
    m.self_test = mode == SgbMode::SelfTest;
    const std::span<const std::uint8_t> b(m.bits);

    // Table 3.1.
    m.tac = sgb_range(b, 1, 16);
    m.serial = sgb_range(b, 17, 30);
    m.country_code = sgb_range(b, 31, 40);
    m.homing = b[40] != 0;
    m.rls = b[41] != 0;
    m.test_protocol = b[42] != 0;
    // Encoded GNSS location, and the defaults of Table 3.1 and Appendix C.3.
    // Table 3.1 gives the default with the hemisphere flag 0 and, for a
    // beacon without location capability, 1; C.3 gives 0. Either is read as
    // no location: only degrees and decimal parts are compared.
    const unsigned lat_all = sgb_range(b, 45, 66);
    const unsigned lon_all = sgb_range(b, 68, 90);
    const bool lat_default = lat_all == ((0b1111111U << 15U) | 0b000001111100000U);
    const bool lon_default = lon_all == ((0b11111111U << 15U) | 0b111110000011111U);
    if (!lat_default && !lon_default) {
        const double lat = sgb_range(b, 45, 51) + sgb_range(b, 52, 66) / 32768.0;
        const double lon = sgb_range(b, 68, 75) + sgb_range(b, 76, 90) / 32768.0;
        m.latitude = b[43] != 0 ? -lat : lat;
        m.longitude = b[66] != 0 ? -lon : lon;
    }
    m.vessel_id_type = sgb_range(b, 91, 93);
    switch (m.vessel_id_type) {
        case 0b000:
            m.identity = "no aircraft or maritime identity";
            break;
        case 0b001: {
            const unsigned mmsi = sgb_range(b, 94, 123);
            const unsigned ais = sgb_range(b, 124, 137);
            m.identity = mmsi == 111111U ? "no MMSI" : std::format("MMSI {:09}", mmsi);
            if (mmsi != 111111U) {
                m.mmsi = mmsi;
            }
            if (ais != 10922U) {
                m.identity += std::format(", EPIRB-AIS ...{:04}", ais);
            }
            break;
        }
        case 0b010:
            m.identity = "call sign " + sgb_baudot(b, 94, 7, 6);
            break;
        case 0b011:
            m.identity = "registration " + sgb_baudot(b, 94, 7, 6);
            break;
        case 0b100: {
            const std::uint32_t address = sgb_range(b, 94, 117);
            m.aircraft_address = address;
            m.identity = std::format("aircraft address {:06X}", address);
            const std::string op = sgb_baudot(b, 118, 3, 5);
            if (sgb_range(b, 118, 132) != 0U) {
                m.identity += ", operator " + op;
            }
            break;
        }
        case 0b101:
            m.identity = std::format("operator {} serial {}", sgb_baudot(b, 94, 3, 5),
                                     sgb_range(b, 109, 120));
            break;
        case 0b111:
            m.identity = "system testing";
            break;
        default:
            m.identity = "spare vessel ID type";
            break;
    }
    m.beacon_type = sgb_range(b, 138, 140);
    // 3.3: the rotating field identifier.
    m.rotating_field = sgb_range(b, 155, 158);
    m.cancellation = m.rotating_field == 15U;

    // 3.6, Table 3.10: the 23 Hex ID.
    std::vector<std::uint8_t> id;
    auto put = [&](unsigned v, unsigned width) {
        for (unsigned i = 0; i < width; ++i) {
            id.push_back(static_cast<std::uint8_t>((v >> (width - 1U - i)) & 1U));
        }
    };
    put(1, 1);
    put(m.country_code, 10);
    put(0b101, 3);
    put(m.tac, 16);
    put(m.serial, 14);
    put(m.test_protocol ? 1U : 0U, 1);
    put(m.vessel_id_type, 3);
    id.insert(id.end(), b.begin() + 93, b.begin() + 137);
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (std::size_t i = 0; i < id.size(); i += 4) {
        m.hex_id.push_back(kHex[(id[i] << 3U) | (id[i + 1] << 2U) | (id[i + 2] << 1U) | id[i + 3]]);
    }
    return m;
}

Expected<SgbDecoder> SgbDecoder::create(const SgbDecoderConfig& config) {
    if (config.rate <= 0 || config.rate % 76800 != 0) {
        return fail("Cospas-Sarsat SGB decoder needs a whole, even number of samples per chip");
    }
    if (!(config.threshold > 0.0 && config.threshold < 1.0)) {
        return fail("Cospas-Sarsat SGB decoder: threshold must be between 0 and 1");
    }
    SgbDecoder d;
    d.config_ = config;
    d.sps_ = static_cast<std::size_t>(config.rate / 38400);
    for (std::size_t mode = 0; mode < 2; ++mode) {
        const auto m = mode == 0 ? SgbMode::Normal : SgbMode::SelfTest;
        d.prn_i_[mode] = sgb_prn(m, false);
        d.prn_q_[mode] = sgb_prn(m, true);
        d.template_[mode] =
            sgb_differential(sgb_preamble_samples(d.prn_i_[mode], d.prn_q_[mode]));
        std::vector<cd> f(kSgbFft, cd{});
        std::copy(d.template_[mode].begin(), d.template_[mode].end(), f.begin());
        fft(f);
        for (auto& v : f) {
            v = std::conj(v);
        }
        d.template_fft_[mode] = std::move(f);
    }
    for (const cd& v : d.template_[0]) {
        d.template_energy_ += std::norm(v);
    }
    d.reset();
    return d;
}

void SgbDecoder::reset() {
    prefix_.assign(1, cd{});
    raw_base_ = 0;
    total_ = 0;
    running_ = {};
    diff_.clear();
    d_base_ = 0;
    next_block_ = 0;
    have_candidate_ = false;
    holdoff_until_ = 0;
    pending_.clear();
}

std::complex<double> SgbDecoder::filtered(double sample) const {
    // The one-chip moving sum ending at a sample, linearly interpolated.
    const auto s = static_cast<std::int64_t>(sps_);
    auto at = [&](std::int64_t n) {
        auto p = [&](std::int64_t k) {
            k = std::max<std::int64_t>(k, 0);
            const std::int64_t i = k - raw_base_;
            if (i < 0 || i >= static_cast<std::int64_t>(prefix_.size())) {
                return cd{};
            }
            return prefix_[static_cast<std::size_t>(i)];
        };
        return p(n + 1) - p(n + 1 - s);
    };
    const double fl = std::floor(sample);
    const auto n = static_cast<std::int64_t>(fl);
    const double frac = sample - fl;
    return at(n) * (1.0 - frac) + at(n + 1) * frac;
}

void SgbDecoder::push(dsp::ConstComplexSpan in, std::vector<SgbBurst>& out) {
    const auto h = static_cast<std::int64_t>(sps_ / 2);
    for (const auto& x : in) {
        running_ += cd(x.real(), x.imag());
        prefix_.push_back(running_);
        ++total_;
        // Differential products on the half-chip grid, g at sample g * h.
        const std::int64_t g = d_base_ + static_cast<std::int64_t>(diff_.size());
        if ((g + 2) * h < total_) {
            const cd y0 = filtered(static_cast<double>(g * h));
            const cd y2 = filtered(static_cast<double>((g + 2) * h));
            diff_.push_back(y2 * std::conj(y0));
        }
        if (static_cast<std::int64_t>(diff_.size()) + d_base_ >=
            next_block_ + static_cast<std::int64_t>(kSgbFft)) {
            correlate_block(out);
        }
        if (!pending_.empty()) {
            finish_ready(out);
        }
    }
    // Keep the raw input the pending bursts and the grid still need.
    std::int64_t keep = d_base_ * h - static_cast<std::int64_t>(4 * sps_);
    for (const Pending& p : pending_) {
        keep = std::min(keep, p.start - static_cast<std::int64_t>(4 * sps_));
    }
    if (have_candidate_) {
        keep = std::min(keep, candidate_ * h - static_cast<std::int64_t>(4 * sps_));
    }
    if (keep - raw_base_ > (1 << 20)) {
        const auto drop = static_cast<std::size_t>(keep - raw_base_);
        prefix_.erase(prefix_.begin(), prefix_.begin() + static_cast<std::ptrdiff_t>(drop));
        raw_base_ += static_cast<std::int64_t>(drop);
    }
}

void SgbDecoder::correlate_block(std::vector<SgbBurst>& /*out*/) {
    const auto off = static_cast<std::size_t>(next_block_ - d_base_);
    std::vector<cd> block(diff_.begin() + static_cast<std::ptrdiff_t>(off),
                          diff_.begin() + static_cast<std::ptrdiff_t>(off + kSgbFft));
    // Sliding energy of the products under the template.
    std::vector<double> energy(kSgbFft + 1, 0.0);
    for (std::size_t i = 0; i < kSgbFft; ++i) {
        energy[i + 1] = energy[i] + std::norm(block[i]);
    }
    fft(block);
    std::array<std::vector<cd>, 2> corr;
    for (std::size_t mode = 0; mode < 2; ++mode) {
        corr[mode].resize(kSgbFft);
        for (std::size_t i = 0; i < kSgbFft; ++i) {
            corr[mode][i] = block[i] * template_fft_[mode][i];
        }
        ifft(corr[mode]);
    }
    const auto h = static_cast<std::int64_t>(sps_ / 2);
    const std::size_t valid = kSgbFft - kSgbTemplate + 1;
    for (std::size_t i = 0; i < valid; ++i) {
        const std::int64_t g = next_block_ + static_cast<std::int64_t>(i);
        const double e = energy[i + kSgbTemplate] - energy[i];
        double metric = 0.0;
        SgbMode mode = SgbMode::Normal;
        if (e > 0.0) {
            for (std::size_t md = 0; md < 2; ++md) {
                const double v = std::norm(corr[md][i]) / (e * template_energy_);
                if (v > metric) {
                    metric = v;
                    mode = md == 0 ? SgbMode::Normal : SgbMode::SelfTest;
                }
            }
            metric = std::sqrt(metric);
        }
        if (have_candidate_) {
            if (metric > candidate_metric_) {
                candidate_ = g;
                candidate_metric_ = metric;
                candidate_mode_ = mode;
            } else if (g > candidate_ + kSgbConfirm) {
                // The grid point ends the first I chip's matched filter.
                pending_.push_back({candidate_ * h - static_cast<std::int64_t>(sps_) + 1,
                                    candidate_mode_, candidate_metric_});
                holdoff_until_ =
                    candidate_ + static_cast<std::int64_t>(2 * kSgbChipsPerArm * 9 / 10);
                have_candidate_ = false;
            }
        } else if (g >= holdoff_until_ && metric > config_.threshold) {
            have_candidate_ = true;
            candidate_ = g;
            candidate_metric_ = metric;
            candidate_mode_ = mode;
        }
    }
    next_block_ += static_cast<std::int64_t>(valid);
    const auto drop = static_cast<std::size_t>(next_block_ - d_base_);
    diff_.erase(diff_.begin(), diff_.begin() + static_cast<std::ptrdiff_t>(drop));
    d_base_ = next_block_;
}

void SgbDecoder::finish_ready(std::vector<SgbBurst>& out) {
    const auto need = static_cast<std::int64_t>((kSgbChipsPerArm + 32) * sps_);
    while (!pending_.empty() && pending_.front().start + need < total_) {
        const Pending p = pending_.front();
        pending_.erase(pending_.begin());
        decode_burst(p.start, p.mode, p.metric, out);
    }
}

void SgbDecoder::decode_burst(std::int64_t start, SgbMode mode, double metric,
                              std::vector<SgbBurst>& out) {
    const std::size_t md = mode == SgbMode::Normal ? 0 : 1;
    const auto& pi = prn_i_[md];
    const auto& pq = prn_q_[md];
    const auto& tmpl = template_[md];
    const auto s = static_cast<double>(sps_);
    const double h = s / 2.0;
    auto lv = [](std::uint8_t c) { return 1.0 - 2.0 * c; };

    // Fine timing: the differential preamble correlation at each sample
    // within a chip of the detection, then a parabola through the peak.
    const auto span = static_cast<std::int64_t>(sps_);
    std::vector<double> score(static_cast<std::size_t>(2 * span + 1));
    std::vector<cd> y(kSgbTemplate + 2);
    for (std::int64_t off = -span; off <= span; ++off) {
        const double t0 = static_cast<double>(start + off) + s - 1.0;
        for (std::size_t k = 0; k < y.size(); ++k) {
            y[k] = filtered(t0 + static_cast<double>(k) * h);
        }
        cd c{};
        for (std::size_t k = 0; k < kSgbTemplate; ++k) {
            c += y[k + 2] * std::conj(y[k]) * std::conj(tmpl[k]);
        }
        score[static_cast<std::size_t>(off + span)] = std::abs(c);
    }
    const auto best = static_cast<std::size_t>(
        std::max_element(score.begin(), score.end()) - score.begin());
    double frac = 0.0;
    if (best > 0 && best + 1 < score.size()) {
        const double a = score[best - 1];
        const double b = score[best];
        const double c = score[best + 1];
        const double den = a - 2.0 * b + c;
        if (den < 0.0) {
            frac = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
        }
    }
    const double n0 = static_cast<double>(start) + static_cast<double>(best) -
                      static_cast<double>(span) + frac;

    // Carrier: the preamble despread chip by chip, both arms, then the peak
    // of a zero-padded FFT, refined by a parabola on its magnitude.
    constexpr std::size_t kCfoFft = 8192;
    std::vector<cd> w(kCfoFft, cd{});
    for (std::size_t c = 0; c < kSgbPreambleChips; ++c) {
        const double t = n0 + s - 1.0 + static_cast<double>(c) * s;
        w[c] = filtered(t) * lv(pi[c]) + filtered(t + h) * cd(0.0, -1.0) * lv(pq[c]);
    }
    std::vector<cd> spec = w;
    fft(spec);
    std::size_t peak = 0;
    for (std::size_t i = 1; i < kCfoFft; ++i) {
        if (std::norm(spec[i]) > std::norm(spec[peak])) {
            peak = i;
        }
    }
    double pk = static_cast<double>(peak);
    {
        const double a = std::abs(spec[(peak + kCfoFft - 1) % kCfoFft]);
        const double b = std::abs(spec[peak]);
        const double c = std::abs(spec[(peak + 1) % kCfoFft]);
        const double den = a - 2.0 * b + c;
        if (den < 0.0) {
            pk += std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
        }
    }
    if (pk > static_cast<double>(kCfoFft) / 2.0) {
        pk -= static_cast<double>(kCfoFft);
    }
    double omega = 2.0 * std::numbers::pi * pk / static_cast<double>(kCfoFft);  // rad per chip
    cd acc{};
    for (std::size_t c = 0; c < kSgbPreambleChips; ++c) {
        acc += w[c] * std::polar(1.0, -omega * static_cast<double>(c));
    }
    double phase = std::arg(acc);

    // Despreading, bit period by bit period on both arms, with a second
    // order phase loop and an early-late timing loop. Loop gains are
    // engineering choices for 256-chip periods at the SNR the acquisition
    // needs.
    constexpr double kPhaseP = 0.3;
    constexpr double kPhaseI = 0.03;
    constexpr double kTimeP = 0.3;
    constexpr double kTimeI = 0.02;
    const double initial_omega = omega;
    double tc = s;                    // samples per chip
    double tau = n0 + s - 1.0;        // matched filter peak of the period's first I chip
    std::vector<std::uint8_t> bits(kSgbMessageBits, 0);
    for (std::size_t p = 0; p < kSgbBits; ++p) {
        cd xi{}, xq{}, ei{}, eq{}, li{}, lq{};
        for (std::size_t j = 0; j < kSgbChipsPerBit; ++j) {
            const std::size_t c = p * kSgbChipsPerBit + j;
            const double ti = tau + static_cast<double>(j) * tc;
            const double tq = ti + 0.5 * tc;
            const cd rot_i = std::polar(1.0, -(phase + omega * static_cast<double>(j)));
            const cd rot_q = std::polar(1.0, -(phase + omega * (static_cast<double>(j) + 0.5)));
            const double si = lv(pi[c]);
            const double sq = lv(pq[c]);
            const cd ri = rot_i * si;
            const cd rq = rot_q * cd(0.0, -1.0) * sq;
            xi += filtered(ti) * ri;
            xq += filtered(tq) * rq;
            ei += filtered(ti - 0.5 * tc) * ri;
            eq += filtered(tq - 0.5 * tc) * rq;
            li += filtered(ti + 0.5 * tc) * ri;
            lq += filtered(tq + 0.5 * tc) * rq;
        }
        double di = 1.0;
        double dq = 1.0;
        if (p >= kSgbPreambleBits) {
            const std::size_t k = p - kSgbPreambleBits;
            const bool bi = xi.real() < 0.0;
            const bool bq = xq.real() < 0.0;
            bits[2 * k] = bi ? 1 : 0;      // bit 2k + 1, odd, on I
            bits[2 * k + 1] = bq ? 1 : 0;  // bit 2k + 2, even, on Q
            di = bi ? -1.0 : 1.0;
            dq = bq ? -1.0 : 1.0;
        }
        const double err = std::arg(xi * di + xq * dq);
        const double early = std::abs(ei) + std::abs(eq);
        const double late = std::abs(li) + std::abs(lq);
        const double delta = (late + early) > 0.0 ? 0.5 * (late - early) / (late + early) : 0.0;

        phase += omega * static_cast<double>(kSgbChipsPerBit) + kPhaseP * err;
        omega += kPhaseI * err / static_cast<double>(kSgbChipsPerBit);
        tau += static_cast<double>(kSgbChipsPerBit) * tc + kTimeP * delta * tc;
        tc += kTimeI * delta * tc / static_cast<double>(kSgbChipsPerBit);
    }

    auto parsed = sgb_parse(bits, mode);
    if (!parsed) {
        ++failed_;
        return;
    }
    SgbBurst burst;
    burst.message = std::move(*parsed);
    burst.sample_position = static_cast<std::uint64_t>(std::max(0.0, std::round(n0)));
    burst.frequency_offset_hz = initial_omega * kSgbChipRate / (2.0 * std::numbers::pi);
    // The mean over the burst: chips tracked over the samples they took.
    burst.chip_rate = static_cast<double>(config_.rate) * static_cast<double>(kSgbChipsPerArm) /
                      (tau - (n0 + s - 1.0));
    burst.sync_metric = metric;
    out.push_back(std::move(burst));
}

}  // namespace revenant::decode
