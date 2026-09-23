#include "core/decode/aprs.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace revenant::decode {

namespace {

using Bytes = std::span<const std::uint8_t>;

bool is_digit(std::uint8_t c) { return c >= '0' && c <= '9'; }

// Chapter 9, page 37: base-91 characters run from "!" to "{", value = code
// minus 33.
bool is_base91(std::uint8_t c) { return c >= 33 && c <= 33 + 90; }

std::string as_text(Bytes b) { return {b.begin(), b.end()}; }

// Chapter 6, page 22.
std::optional<AprsTimestamp> parse_timestamp(Bytes b) {
    if (b.size() < 7) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < 6; ++i) {
        if (!is_digit(b[i])) {
            return std::nullopt;
        }
    }
    const auto two = [&](std::size_t i) { return (b[i] - '0') * 10 + (b[i + 1] - '0'); };
    AprsTimestamp t;
    switch (b[6]) {
        case 'z':
        case '/':
            t.format = (b[6] == 'z') ? AprsTimeFormat::DayHourMinuteZulu
                                     : AprsTimeFormat::DayHourMinuteLocal;
            t.day = two(0);
            t.hour = two(2);
            t.minute = two(4);
            break;
        case 'h':
            t.format = AprsTimeFormat::HourMinuteSecond;
            t.hour = two(0);
            t.minute = two(2);
            t.second = two(4);
            break;
        default:
            return std::nullopt;
    }
    return t;
}

// Chapter 6, pages 23 to 24: "ddmm.hhN" and "dddmm.hhW", with page 24's
// position ambiguity. `blanked` is how many trailing digits the latitude
// sent as spaces; the longitude has the same digits zeroed whether or not it
// sent spaces, as page 24 says the latitude's ambiguity applies to both.
struct Coordinate {
    double degrees = 0.0;
    int blanked = 0;
};

Expected<Coordinate> parse_coordinate(Bytes b, std::size_t degree_digits, char positive,
                                      char negative, int force_blanked) {
    const std::size_t length = degree_digits + 6;
    if (b.size() < length) {
        return fail("an APRS coordinate is shorter than chapter 6 allows");
    }
    // Digit positions, most significant first: degrees, minutes, then the
    // two hundredths after the point.
    std::vector<std::size_t> digits;
    for (std::size_t i = 0; i < degree_digits + 2; ++i) {
        digits.push_back(i);
    }
    if (b[degree_digits + 2] != '.') {
        return fail("an APRS coordinate has no decimal point where chapter 6 puts it");
    }
    digits.push_back(degree_digits + 3);
    digits.push_back(degree_digits + 4);

    // Page 24: ambiguity replaces the minute and hundredth digits from the
    // right, up to four of them.
    int blanked = 0;
    for (std::size_t k = digits.size(); k-- > 0;) {
        if (b[digits[k]] == ' ') {
            ++blanked;
        } else {
            break;
        }
    }
    if (blanked > 4) {
        return fail("APRS position ambiguity reaches into the degrees");
    }
    const int zeroed = std::max(blanked, force_blanked);

    int values[16] = {};
    for (std::size_t k = 0; k < digits.size(); ++k) {
        const std::uint8_t c = b[digits[k]];
        const bool blank = static_cast<int>(digits.size() - k) <= zeroed;
        if (blank) {
            values[k] = 0;
        } else if (is_digit(c)) {
            values[k] = c - '0';
        } else {
            return fail("an APRS coordinate holds a character that is not a digit");
        }
    }
    int whole = 0;
    for (std::size_t k = 0; k < degree_digits; ++k) {
        whole = whole * 10 + values[k];
    }
    const double minutes = values[degree_digits] * 10 + values[degree_digits + 1] +
                           (values[degree_digits + 2] * 10 + values[degree_digits + 3]) / 100.0;
    if (minutes >= 60.0) {
        return fail("APRS coordinate minutes are 60 or more");
    }
    double degrees = static_cast<double>(whole) + minutes / 60.0;
    const char hemisphere = static_cast<char>(b[length - 1]);
    if (hemisphere == negative) {
        degrees = -degrees;
    } else if (hemisphere != positive) {
        return fail("an APRS coordinate has no hemisphere letter where chapter 6 puts it");
    }
    return Coordinate{degrees, blanked};
}

// Chapter 7, page 27: "ddd/sss", course and speed, or unknown as "000/000",
// ".../..." or spaces.
void parse_course_speed(Bytes b, AprsPosition& p, std::size_t& consumed) {
    consumed = 0;
    if (b.size() < 7 || b[3] != '/') {
        return;
    }
    const auto three = [&](std::size_t at) -> std::optional<int> {
        int v = 0;
        for (std::size_t i = at; i < at + 3; ++i) {
            if (!is_digit(b[i])) {
                return std::nullopt;
            }
            v = v * 10 + (b[i] - '0');
        }
        return v;
    };
    const auto unknown = [&](std::size_t at) {
        for (std::size_t i = at; i < at + 3; ++i) {
            if (b[i] != '.' && b[i] != ' ') {
                return false;
            }
        }
        return true;
    };
    const auto course = three(0);
    const auto speed = three(4);
    if (course && speed) {
        if (*course != 0 || *speed != 0) {
            p.course_deg = *course;
            p.speed_knots = static_cast<double>(*speed);
        }
        consumed = 7;
    } else if (unknown(0) && unknown(4)) {
        consumed = 7;
    }
}

// Chapter 6, page 26: "/A=aaaaaa" anywhere in the comment, in feet.
void parse_comment_altitude(const std::string& comment, AprsPosition& p) {
    const auto at = comment.find("/A=");
    if (at == std::string::npos || at + 9 > comment.size()) {
        return;
    }
    const std::string digits = comment.substr(at + 3, 6);
    const bool negative = digits[0] == '-';
    for (std::size_t i = negative ? 1 : 0; i < 6; ++i) {
        if (digits[i] < '0' || digits[i] > '9') {
            return;
        }
    }
    p.altitude_ft = std::strtod(digits.c_str(), nullptr);
}

double base91(Bytes b) {
    double v = 0.0;
    for (const std::uint8_t c : b) {
        v = v * 91.0 + static_cast<double>(c - 33);
    }
    return v;
}

// Chapter 9, pages 37 to 40: "/YYYYXXXX$csT".
Expected<AprsPosition> parse_compressed(Bytes b) {
    if (b.size() < 13) {
        return fail("an APRS compressed position is shorter than chapter 9's 13 characters");
    }
    for (std::size_t i = 1; i < 9; ++i) {
        if (!is_base91(b[i])) {
            return fail("an APRS compressed position holds a character outside base 91");
        }
    }
    AprsPosition p;
    p.compressed = true;
    p.symbol_table = static_cast<char>(b[0]);
    // Page 38: Lat = 90 - YYYY / 380926, Long = -180 + XXXX / 190463.
    p.latitude_deg = 90.0 - base91(b.subspan(1, 4)) / 380926.0;
    p.longitude_deg = -180.0 + base91(b.subspan(5, 4)) / 190463.0;
    p.symbol_code = static_cast<char>(b[9]);

    const std::uint8_t c = b[10];
    const std::uint8_t s = b[11];
    const std::uint8_t t = b[12];
    // Page 38: c as a space means no course, speed, range or altitude, and
    // page 39 says the T byte then means nothing either.
    if (c == ' ') {
        return p;
    }
    if (!is_base91(c) || !is_base91(s) || !is_base91(t)) {
        return fail("APRS compressed cs or T bytes lie outside base 91");
    }
    const auto type = static_cast<std::uint8_t>(t - 33);
    p.compression_type = type;
    // Page 40: bits 4 and 3 of T at 10 mean the source was GGA and cs is
    // altitude, 1.002^cs feet. Checked before the course and speed reading,
    // because page 41's GGA example "=/5L!!<*e7OS]S" has a c byte that
    // would otherwise read as a course.
    if (((type >> 3U) & 0x3U) == 0x2U) {
        p.altitude_ft = std::pow(1.002, static_cast<double>((c - 33) * 91 + (s - 33)));
    } else if (c == '{') {
        // Page 39: range = 2 * 1.08^s miles.
        p.range_miles = 2.0 * std::pow(1.08, static_cast<double>(s - 33));
    } else if (c <= 'z') {
        // Page 39: course = c * 4, speed = 1.08^s - 1.
        p.course_deg = (c - 33) * 4;
        p.speed_knots = std::pow(1.08, static_cast<double>(s - 33)) - 1.0;
    }
    return p;
}

// Chapter 8: the position, which is compressed when the character after the
// Data Type Identifier and any timestamp is not a digit (chapter 9, page 37).
Expected<AprsPositionReport> parse_position(Bytes b, bool messaging, bool timestamped) {
    AprsPositionReport report;
    report.messaging = messaging;
    if (timestamped) {
        const auto t = parse_timestamp(b);
        if (!t) {
            return fail("an APRS position with a timestamp has none chapter 6 recognises");
        }
        report.timestamp = *t;
        b = b.subspan(7);
    }
    if (b.empty()) {
        return fail("an APRS position report ends before its position");
    }

    std::size_t used = 0;
    if (!is_digit(b[0]) && b[0] != ' ') {
        auto p = parse_compressed(b);
        if (!p) {
            return std::unexpected(p.error());
        }
        report.position = *p;
        used = 13;
    } else {
        // Chapter 8, page 32: Lat (8), table (1), Long (9), code (1).
        if (b.size() < 19) {
            return fail("an APRS position report is shorter than chapter 8's 19 characters");
        }
        auto lat = parse_coordinate(b.subspan(0, 8), 2, 'N', 'S', 0);
        if (!lat) {
            return std::unexpected(lat.error());
        }
        auto lon = parse_coordinate(b.subspan(9, 9), 3, 'E', 'W', lat->blanked);
        if (!lon) {
            return std::unexpected(lon.error());
        }
        report.position.latitude_deg = lat->degrees;
        report.position.longitude_deg = lon->degrees;
        report.position.ambiguity = lat->blanked;
        report.position.symbol_table = static_cast<char>(b[8]);
        report.position.symbol_code = static_cast<char>(b[18]);
        used = 19;
        std::size_t extension = 0;
        parse_course_speed(b.subspan(used), report.position, extension);
        used += extension;
    }
    report.comment = as_text(b.subspan(used));
    parse_comment_altitude(report.comment, report.position);
    return report;
}

// Chapter 14, pages 71 and 72.
Expected<AprsMessage> parse_message(Bytes b) {
    if (b.size() < 10 || b[9] != ':') {
        return fail("an APRS message has no nine-character addressee and colon");
    }
    AprsMessage m;
    m.addressee = as_text(b.subspan(0, 9));
    while (!m.addressee.empty() && m.addressee.back() == ' ') {
        m.addressee.pop_back();
    }
    std::string text = as_text(b.subspan(10));

    // Page 72: "ack" or "rej" then the message number.
    const auto numbered = [&](std::string_view prefix, AprsMessage::Kind kind) {
        if (text.size() > prefix.size() && text.size() <= prefix.size() + 5 &&
            text.compare(0, prefix.size(), prefix) == 0) {
            m.kind = kind;
            m.id = text.substr(prefix.size());
            return true;
        }
        return false;
    };
    if (numbered("ack", AprsMessage::Kind::Acknowledgement) ||
        numbered("rej", AprsMessage::Kind::Rejection)) {
        return m;
    }

    // Page 71: an optional "{" and up to five alphanumeric characters.
    const auto brace = text.rfind('{');
    if (brace != std::string::npos && text.size() - brace - 1 >= 1 && text.size() - brace - 1 <= 5) {
        bool alnum = true;
        for (std::size_t i = brace + 1; i < text.size(); ++i) {
            const char c = text[i];
            alnum = alnum && ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
        }
        if (alnum) {
            m.id = text.substr(brace + 1);
            text.resize(brace);
        }
    }
    m.text = std::move(text);
    return m;
}

bool maidenhead_at(Bytes b, std::size_t length) {
    const auto upper = [](std::uint8_t c) {
        return static_cast<std::uint8_t>((c >= 'a' && c <= 'z') ? c - 32 : c);
    };
    if (b.size() < length) {
        return false;
    }
    for (std::size_t i = 0; i < length; ++i) {
        const std::uint8_t c = upper(b[i]);
        const bool digit_pair = (i == 2 || i == 3);
        if (digit_pair ? !is_digit(c) : !(c >= 'A' && c <= 'X')) {
            return false;
        }
    }
    // The first pair is fields, A to R.
    return upper(b[0]) <= 'R' && upper(b[1]) <= 'R';
}

// Chapter 16, pages 80 to 82.
AprsStatus parse_status(Bytes b) {
    AprsStatus s;
    // Page 81: a locator must immediately follow the identifier, be followed
    // by a symbol table and code, and then either nothing or a space.
    for (const std::size_t length : {std::size_t{6}, std::size_t{4}}) {
        if (maidenhead_at(b, length) && b.size() >= length + 2 &&
            (b.size() == length + 2 || b[length + 2] == ' ')) {
            std::string grid = as_text(b.subspan(0, length));
            for (char& c : grid) {
                if (c >= 'a' && c <= 'z') {
                    c = static_cast<char>(c - 32);
                }
            }
            s.maidenhead = grid;
            s.symbol_table = static_cast<char>(b[length]);
            s.symbol_code = static_cast<char>(b[length + 1]);
            s.text = as_text(b.subspan(length + 2));
            return s;
        }
    }
    // Page 80: an optional DHM zulu timestamp, which is the only format a
    // status report may carry.
    const auto t = parse_timestamp(b);
    if (t && t->format == AprsTimeFormat::DayHourMinuteZulu) {
        s.timestamp = *t;
        b = b.subspan(7);
    }
    s.text = as_text(b);
    return s;
}

// Chapter 10, page 44: one destination address character.
struct MicEDigit {
    int digit = 0;       // 0-9, or -1 for a space (ambiguity)
    int message = 0;     // 0, 1 for a standard one, 2 for a custom one
    bool high = false;   // North, +100, West for bytes 4 to 6
};

std::optional<MicEDigit> mic_e_digit(char c) {
    if (c >= '0' && c <= '9') {
        return MicEDigit{c - '0', 0, false};
    }
    if (c >= 'A' && c <= 'J') {
        return MicEDigit{c - 'A', 2, false};
    }
    if (c == 'K') {
        return MicEDigit{-1, 2, false};
    }
    if (c == 'L') {
        return MicEDigit{-1, 0, false};
    }
    if (c >= 'P' && c <= 'Y') {
        return MicEDigit{c - 'P', 1, true};
    }
    if (c == 'Z') {
        return MicEDigit{-1, 1, true};
    }
    return std::nullopt;
}

Expected<AprsMicE> parse_mic_e(std::string_view destination, std::uint8_t ssid, Bytes b) {
    // Page 43: six latitude characters.
    if (destination.size() != 6) {
        return fail("a Mic-E destination address is not six characters");
    }
    // Page 47: "if the Information field appears to be less than 9 bytes
    // long, the packet must be ignored".
    if (b.size() < 8) {
        return fail("a Mic-E information field is shorter than page 47 allows");
    }

    AprsMicE e;
    MicEDigit d[6];
    for (std::size_t i = 0; i < 6; ++i) {
        const auto parsed = mic_e_digit(destination[i]);
        if (!parsed) {
            return fail("a Mic-E destination holds a character page 44 does not define");
        }
        // Page 44 note: A to K are not used in bytes 4 to 6.
        if (i >= 3 && parsed->message == 2) {
            return fail("a Mic-E destination uses A to K in bytes 4 to 6");
        }
        d[i] = *parsed;
    }

    // Latitude: degrees from bytes 1-2, minutes 3-4, hundredths 5-6, with
    // spaces as ambiguity from the right (page 53).
    int blanked = 0;
    for (int i = 5; i >= 0 && d[i].digit < 0; --i) {
        ++blanked;
    }
    int digits[6];
    for (int i = 0; i < 6; ++i) {
        if (d[i].digit < 0 && i < 6 - blanked) {
            return fail("a Mic-E latitude has a space that is not trailing");
        }
        digits[i] = d[i].digit < 0 ? 0 : d[i].digit;
    }
    if (blanked > 4) {
        return fail("Mic-E position ambiguity reaches into the degrees");
    }
    double latitude = digits[0] * 10 + digits[1] +
                      (digits[2] * 10 + digits[3] + (digits[4] * 10 + digits[5]) / 100.0) / 60.0;
    const bool north = d[3].high;
    const bool plus_100 = d[4].high;
    const bool west = d[5].high;
    e.position.latitude_deg = north ? latitude : -latitude;
    e.position.ambiguity = blanked;

    // Page 45: message bits A, B, C from bytes 1 to 3.
    const int a = d[0].message;
    const int bb = d[1].message;
    const int c = d[2].message;
    const bool any_standard = a == 1 || bb == 1 || c == 1;
    const bool any_custom = a == 2 || bb == 2 || c == 2;
    if (!any_standard && !any_custom) {
        e.message = MicEMessage::Emergency;
    } else if (any_standard && any_custom) {
        e.message = MicEMessage::Unknown;
    } else {
        const int abc = ((a != 0) ? 4 : 0) | ((bb != 0) ? 2 : 0) | ((c != 0) ? 1 : 0);
        const int n = 7 - abc;  // 111 is M0, 001 is M6
        e.message = static_cast<MicEMessage>((any_standard ? 0 : 7) + n);
    }
    e.path_code = ssid;

    // Page 46: the Data Type Identifier, then d+28, m+28, h+28, SP+28,
    // DC+28, SE+28, symbol code, symbol table.
    e.current = (b[0] == '`' || b[0] == 0x1C);
    // Page 48: longitude degrees.
    int deg = b[1] - 28;
    if (plus_100) {
        deg += 100;
    }
    if (deg >= 180 && deg <= 189) {
        deg -= 80;
    } else if (deg >= 190 && deg <= 199) {
        deg -= 190;
    }
    // Page 49: minutes and hundredths.
    int minutes = b[2] - 28;
    if (minutes >= 60) {
        minutes -= 60;
    }
    int hundredths = b[3] - 28;
    if (deg < 0 || deg > 179 || minutes < 0 || minutes > 59 || hundredths < 0 || hundredths > 99) {
        return fail("a Mic-E longitude decodes out of range");
    }
    // Page 54: the latitude's ambiguity applies to the longitude digits.
    if (blanked >= 1) {
        hundredths -= hundredths % 10;
    }
    if (blanked >= 2) {
        hundredths = 0;
    }
    if (blanked >= 3) {
        minutes -= minutes % 10;
    }
    if (blanked >= 4) {
        minutes = 0;
    }
    const double longitude = deg + (minutes + hundredths / 100.0) / 60.0;
    e.position.longitude_deg = west ? -longitude : longitude;

    // Page 52: speed and course.
    const int sp = b[4] - 28;
    const int dc = b[5] - 28;
    const int se = b[6] - 28;
    int speed = sp * 10 + dc / 10;
    int course = (dc % 10) * 100 + se;
    if (speed >= 800) {
        speed -= 800;
    }
    if (course >= 400) {
        course -= 400;
    }
    e.position.speed_knots = static_cast<double>(speed);
    // Page 49: a course of 0 is unknown.
    if (course != 0) {
        e.position.course_deg = course;
    }
    e.position.symbol_code = static_cast<char>(b[7]);
    e.position.symbol_table = b.size() > 8 ? static_cast<char>(b[8]) : '/';

    if (b.size() <= 9) {
        return e;
    }
    const Bytes rest = b.subspan(9);
    // Page 54: telemetry flags.
    if (rest[0] == '`' || rest[0] == '\'') {
        const std::size_t channels = (rest[0] == '`') ? 2 : 5;
        for (std::size_t k = 0; k < channels && 2 * k + 3 <= rest.size(); ++k) {
            const std::string hex = as_text(rest.subspan(1 + 2 * k, 2));
            char* end = nullptr;
            const long v = std::strtol(hex.c_str(), &end, 16);
            if (end != hex.c_str() + 2) {
                break;
            }
            e.telemetry.push_back(static_cast<std::uint8_t>(v));
        }
        return e;
    }
    if (rest[0] == 0x1D) {
        for (std::size_t k = 1; k < rest.size() && k <= 5; ++k) {
            e.telemetry.push_back(rest[k]);
        }
        return e;
    }

    e.status_text = as_text(rest);
    // Page 55: "xxx}" altitude, possibly after a Kenwood type code of ">"
    // or "]".
    Bytes alt = rest;
    if (!alt.empty() && (alt[0] == '>' || alt[0] == ']')) {
        alt = alt.subspan(1);
    }
    if (alt.size() >= 4 && alt[3] == '}' && is_base91(alt[0]) && is_base91(alt[1]) &&
        is_base91(alt[2])) {
        e.altitude_m = base91(alt.subspan(0, 3)) - 10000.0;
    }
    return e;
}

}  // namespace

std::string_view mic_e_message_name(MicEMessage message) {
    switch (message) {
        case MicEMessage::OffDuty: return "M0: Off Duty";
        case MicEMessage::EnRoute: return "M1: En Route";
        case MicEMessage::InService: return "M2: In Service";
        case MicEMessage::Returning: return "M3: Returning";
        case MicEMessage::Committed: return "M4: Committed";
        case MicEMessage::Special: return "M5: Special";
        case MicEMessage::Priority: return "M6: Priority";
        case MicEMessage::Custom0: return "C0: Custom-0";
        case MicEMessage::Custom1: return "C1: Custom-1";
        case MicEMessage::Custom2: return "C2: Custom-2";
        case MicEMessage::Custom3: return "C3: Custom-3";
        case MicEMessage::Custom4: return "C4: Custom-4";
        case MicEMessage::Custom5: return "C5: Custom-5";
        case MicEMessage::Custom6: return "C6: Custom-6";
        case MicEMessage::Emergency: return "Emergency";
        case MicEMessage::Unknown: return "Unknown";
    }
    return "Unknown";
}

Expected<AprsPacket> aprs_parse(std::string_view destination_callsign,
                                std::uint8_t destination_ssid,
                                std::span<const std::uint8_t> information) {
    if (information.empty()) {
        return fail("an APRS information field is empty");
    }
    AprsPacket packet;
    packet.data_type = static_cast<char>(information[0]);
    const Bytes rest = information.subspan(1);

    switch (information[0]) {
        case '!':
        case '=': {
            auto r = parse_position(rest, information[0] == '=', false);
            if (!r) {
                return std::unexpected(r.error());
            }
            packet.body = std::move(*r);
            return packet;
        }
        case '/':
        case '@': {
            auto r = parse_position(rest, information[0] == '@', true);
            if (!r) {
                return std::unexpected(r.error());
            }
            packet.body = std::move(*r);
            return packet;
        }
        case ':': {
            auto m = parse_message(rest);
            if (!m) {
                return std::unexpected(m.error());
            }
            packet.body = std::move(*m);
            return packet;
        }
        case '>':
            packet.body = parse_status(rest);
            return packet;
        case '`':
        case '\'':
        case 0x1C:
        case 0x1D: {
            auto e = parse_mic_e(destination_callsign, destination_ssid, information);
            if (!e) {
                return std::unexpected(e.error());
            }
            packet.body = std::move(*e);
            return packet;
        }
        default:
            break;
    }

    // Page 18: "!" may appear anywhere up to the 40th character, after an
    // unmodifiable beacon header.
    for (std::size_t i = 1; i < information.size() && i < 40; ++i) {
        if (information[i] == '!') {
            auto r = parse_position(information.subspan(i + 1), false, false);
            if (r) {
                packet.data_type = '!';
                packet.body = std::move(*r);
                return packet;
            }
            break;
        }
    }

    std::string message = "APRS data type '";
    message.push_back(static_cast<char>(information[0]));
    message += "' is not one this decoder parses";
    return fail(message);
}

Expected<AprsPacket> aprs_parse(const Ax25Frame& frame) {
    if (!frame.is_ui) {
        return fail("APRS travels in UI frames only, per chapter 3");
    }
    return aprs_parse(frame.destination.callsign, frame.destination.ssid, frame.information);
}

}  // namespace revenant::decode
