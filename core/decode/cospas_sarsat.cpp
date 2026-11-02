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

}  // namespace revenant::decode
