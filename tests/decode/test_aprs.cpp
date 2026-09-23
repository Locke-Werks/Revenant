// APRS: the parser against the APRS Protocol Reference 1.0.1's own examples,
// then two packets through the whole chain, audio to fields.
//
// Every string in the first group is one the document prints, with its page.
// The PDF marks spaces with a visible glyph that text extraction renders as
// "V" or "VV"; each is a single space here, as the surrounding prose says
// (page 41: "Note the space character following the > Symbol Code").

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "core/decode/aprs.h"
#include "core/dsp/synth/fsk_mod.h"

using namespace revenant;
using Catch::Approx;

namespace {

std::vector<std::uint8_t> bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

decode::AprsPacket parse(std::string_view info, std::string_view destination = "APRS",
                         std::uint8_t ssid = 0) {
    auto packet = decode::aprs_parse(destination, ssid, bytes(info));
    INFO("parsing \"" << info << "\": "
                      << (packet.has_value() ? std::string("ok") : packet.error().message));
    REQUIRE(packet.has_value());
    return *packet;
}

decode::AprsPacket parse_bytes(const std::vector<std::uint8_t>& info, std::string_view destination,
                               std::uint8_t ssid = 0) {
    auto packet = decode::aprs_parse(destination, ssid, info);
    INFO((packet.has_value() ? std::string("ok") : packet.error().message));
    REQUIRE(packet.has_value());
    return *packet;
}

const decode::AprsPositionReport& position(const decode::AprsPacket& p) {
    REQUIRE(std::holds_alternative<decode::AprsPositionReport>(p.body));
    return std::get<decode::AprsPositionReport>(p.body);
}

// Page 23 and 24 values: 4903.50N and 07201.75W.
constexpr double kLat = 49.0 + 3.50 / 60.0;
constexpr double kLon = -(72.0 + 1.75 / 60.0);

// Page 53's Mic-E information field, with its double quote as a number so
// the literal carries no escape: `(_fn"Oj/
const std::vector<std::uint8_t> kMicEInfo = {'`', '(', '_', 'f', 'n', 34, 'O', 'j', '/'};

}  // namespace

TEST_CASE("uncompressed positions parse as chapter 8's examples say", "[decode][aprs]") {
    SECTION("page 32, no timestamp, with comment") {
        const auto p = parse("!4903.50N/07201.75W-Test 001234");
        const auto& r = position(p);
        CHECK(p.data_type == '!');
        CHECK_FALSE(r.messaging);
        CHECK_FALSE(r.timestamp.has_value());
        CHECK(r.position.latitude_deg == Approx(kLat).margin(1e-9));
        CHECK(r.position.longitude_deg == Approx(kLon).margin(1e-9));
        CHECK(r.position.symbol_table == '/');
        CHECK(r.position.symbol_code == '-');
        CHECK(r.comment == "Test 001234");
        CHECK_FALSE(r.position.altitude_ft.has_value());
    }
    SECTION("page 32, altitude in the comment") {
        const auto packet = parse("!4903.50N/07201.75W-Test /A=001234");
        const auto& r = position(packet);
        REQUIRE(r.position.altitude_ft.has_value());
        CHECK(*r.position.altitude_ft == Approx(1234.0));
    }
    SECTION("page 32, ambiguity to the nearest degree") {
        const auto packet = parse("!49  .  N/072  .  W-");
        const auto& r = position(packet);
        CHECK(r.position.ambiguity == 4);
        CHECK(r.position.latitude_deg == Approx(49.0));
        CHECK(r.position.longitude_deg == Approx(-72.0));
    }
    SECTION("page 24, ambiguity in the latitude applies to the longitude") {
        const auto packet = parse("!4903.  N/07201.75W-");
        const auto& r = position(packet);
        CHECK(r.position.ambiguity == 2);
        CHECK(r.position.latitude_deg == Approx(49.0 + 3.0 / 60.0));
        CHECK(r.position.longitude_deg == Approx(-(72.0 + 1.0 / 60.0)));
    }
    SECTION("page 18 and 32, an X1J header before the identifier") {
        const auto p = parse("TheNet X-1J4  (BFLD)!4903.50N/07201.75Wn");
        CHECK(p.data_type == '!');
        CHECK(position(p).position.symbol_code == 'n');
    }
    SECTION("page 32, timestamp in zulu") {
        const auto p = parse("/092345z4903.50N/07201.75W>Test1234");
        const auto& r = position(p);
        REQUIRE(r.timestamp.has_value());
        CHECK(r.timestamp->format == decode::AprsTimeFormat::DayHourMinuteZulu);
        CHECK(r.timestamp->day == 9);
        CHECK(r.timestamp->hour == 23);
        CHECK(r.timestamp->minute == 45);
        CHECK_FALSE(r.messaging);
        CHECK(r.comment == "Test1234");
    }
    SECTION("page 32, local time with messaging") {
        const auto packet = parse("@092345/4903.50N/07201.75W>Test1234");
        const auto& r = position(packet);
        REQUIRE(r.timestamp.has_value());
        CHECK(r.timestamp->format == decode::AprsTimeFormat::DayHourMinuteLocal);
        CHECK(r.messaging);
    }
    SECTION("page 33, course and speed") {
        const auto packet = parse("@092345/4903.50N/07201.75W>088/036");
        const auto& r = position(packet);
        REQUIRE(r.position.course_deg.has_value());
        CHECK(*r.position.course_deg == 88);
        CHECK(*r.position.speed_knots == Approx(36.0));
        CHECK(r.comment.empty());
    }
    SECTION("page 33, hours minutes seconds and PHG left in the comment") {
        const auto packet = parse("@234517h4903.50N/07201.75W>PHG5132");
        const auto& r = position(packet);
        REQUIRE(r.timestamp.has_value());
        CHECK(r.timestamp->format == decode::AprsTimeFormat::HourMinuteSecond);
        CHECK(r.timestamp->hour == 23);
        CHECK(r.timestamp->minute == 45);
        CHECK(r.timestamp->second == 17);
        CHECK(r.comment == "PHG5132");
        CHECK_FALSE(r.position.course_deg.has_value());
    }
}

TEST_CASE("compressed positions parse as chapter 9's examples say", "[decode][aprs]") {
    SECTION("page 38, the longitude worked example alone") {
        // "<*e7" is -72.75 degrees.
        const auto packet = parse("!/5L!!<*e7>7P[");
        const auto& r = position(packet);
        CHECK(r.position.longitude_deg == Approx(-72.75).margin(1e-6));
    }
    SECTION("pages 40 and 41, course and speed") {
        const auto packet = parse("=/5L!!<*e7>7P[");
        const auto& r = position(packet);
        CHECK(r.messaging);
        CHECK(r.position.compressed);
        // Page 40: latitude 49 degrees 30 minutes. "5L!!" is 15427503, which
        // is 380926 x 40.5 exactly.
        CHECK(r.position.latitude_deg == Approx(49.5).margin(1e-9));
        CHECK(r.position.longitude_deg == Approx(-72.75).margin(1e-6));
        CHECK(r.position.symbol_table == '/');
        CHECK(r.position.symbol_code == '>');
        // Page 39: "7P" is course 88 and speed 1.08^47 - 1 = 36.2 knots.
        REQUIRE(r.position.course_deg.has_value());
        CHECK(*r.position.course_deg == 88);
        CHECK(*r.position.speed_knots == Approx(36.2).margin(0.05));
        // Page 39: "[" is 58, current fix, RMC, APRSdos software.
        REQUIRE(r.position.compression_type.has_value());
        CHECK(*r.position.compression_type == 58);
    }
    SECTION("page 41, radio range") {
        const auto packet = parse("=/5L!!<*e7>{?!");
        const auto& r = position(packet);
        REQUIRE(r.position.range_miles.has_value());
        // Page 39: "{?" is 2 x 1.08^30, "~ 20 miles".
        CHECK(*r.position.range_miles == Approx(20.1).margin(0.05));
        CHECK_FALSE(r.position.course_deg.has_value());
    }
    SECTION("page 41, altitude from a GGA source") {
        const auto packet = parse("=/5L!!<*e7OS]S");
        const auto& r = position(packet);
        CHECK(r.position.symbol_code == 'O');
        REQUIRE(r.position.altitude_ft.has_value());
        // Page 40: "S]" is 4610, and 1.002^4610 is 10004 feet.
        CHECK(*r.position.altitude_ft == Approx(10004.0).margin(1.0));
        CHECK_FALSE(r.position.course_deg.has_value());
    }
    SECTION("page 41, no course, speed, range or altitude") {
        const auto packet = parse("=/5L!!<*e7> sTComment");
        const auto& r = position(packet);
        CHECK_FALSE(r.position.course_deg.has_value());
        CHECK_FALSE(r.position.range_miles.has_value());
        CHECK_FALSE(r.position.altitude_ft.has_value());
        CHECK(r.comment == "Comment");
    }
    SECTION("page 41, with a timestamp") {
        const auto packet = parse("@092345z/5L!!<*e7>{?!");
        const auto& r = position(packet);
        REQUIRE(r.timestamp.has_value());
        CHECK(r.timestamp->day == 9);
        CHECK(r.position.range_miles.has_value());
    }
}

TEST_CASE("messages parse as chapter 14's examples say", "[decode][aprs]") {
    using Kind = decode::AprsMessage::Kind;
    const auto message = [](const decode::AprsPacket& p) {
        REQUIRE(std::holds_alternative<decode::AprsMessage>(p.body));
        return std::get<decode::AprsMessage>(p.body);
    };
    // Page 71.
    auto m = message(parse(":WU2Z     :Testing"));
    CHECK(m.kind == Kind::Message);
    CHECK(m.addressee == "WU2Z");
    CHECK(m.text == "Testing");
    CHECK_FALSE(m.id.has_value());

    m = message(parse(":WU2Z     :Testing{003"));
    CHECK(m.text == "Testing");
    REQUIRE(m.id.has_value());
    CHECK(*m.id == "003");

    m = message(parse(":EMAIL    :msproul@ap.org Test email"));
    CHECK(m.addressee == "EMAIL");
    CHECK(m.text == "msproul@ap.org Test email");

    // Page 72.
    m = message(parse(":KB2ICI-14:ack003"));
    CHECK(m.kind == Kind::Acknowledgement);
    CHECK(m.addressee == "KB2ICI-14");
    CHECK(*m.id == "003");

    m = message(parse(":KB2ICI-14:rej003"));
    CHECK(m.kind == Kind::Rejection);
    CHECK(*m.id == "003");
}

TEST_CASE("status reports parse as chapter 16's examples say", "[decode][aprs]") {
    const auto status = [](const decode::AprsPacket& p) {
        REQUIRE(std::holds_alternative<decode::AprsStatus>(p.body));
        return std::get<decode::AprsStatus>(p.body);
    };
    // Page 80.
    auto s = status(parse(">Net Control Center"));
    CHECK(s.text == "Net Control Center");
    CHECK_FALSE(s.timestamp.has_value());

    s = status(parse(">092345zNet Control Center"));
    REQUIRE(s.timestamp.has_value());
    CHECK(s.timestamp->hour == 23);
    CHECK(s.text == "Net Control Center");

    // Page 82.
    s = status(parse(">IO91SX/G"));
    REQUIRE(s.maidenhead.has_value());
    CHECK(*s.maidenhead == "IO91SX");
    CHECK(*s.symbol_table == '/');
    CHECK(*s.symbol_code == 'G');
    CHECK(s.text.empty());

    s = status(parse(">IO91/G"));
    CHECK(*s.maidenhead == "IO91");

    s = status(parse(">IO91SX/- My house"));
    CHECK(*s.symbol_code == '-');
    CHECK(s.text == " My house");

    s = status(parse(">IO91SX/- ^B7"));
    CHECK(s.text == " ^B7");
}

TEST_CASE("Mic-E parses as chapter 10's examples say", "[decode][aprs]") {
    const auto mic_e = [](const decode::AprsPacket& p) {
        REQUIRE(std::holds_alternative<decode::AprsMicE>(p.body));
        return std::get<decode::AprsMicE>(p.body);
    };

    SECTION("page 44, the destination worked example") {
        // 33 25.64 north, west, +0, standard message bits 1/0/0.
        const auto e = mic_e(parse_bytes(kMicEInfo, "S32U6T"));
        CHECK(e.position.latitude_deg == Approx(33.0 + 25.64 / 60.0).margin(1e-9));
        CHECK(e.message == decode::MicEMessage::Returning);
        CHECK(e.position.longitude_deg < 0.0);
        // +0 offset: "(" is 12 degrees.
        CHECK(e.position.longitude_deg == Approx(-(12.0 + 7.74 / 60.0)).margin(1e-9));
    }
    SECTION("page 46, message type examples") {
        CHECK(mic_e(parse_bytes(kMicEInfo, "S32U6T")).message == decode::MicEMessage::Returning);
        CHECK(mic_e(parse_bytes(kMicEInfo, "F2D5VT")).message == decode::MicEMessage::Custom2);
        CHECK(mic_e(parse_bytes(kMicEInfo, "234U6T")).message == decode::MicEMessage::Emergency);
    }
    SECTION("page 53, the information field worked example") {
        // The same latitude with a +100 longitude offset in byte 5.
        const auto e = mic_e(parse_bytes(kMicEInfo, "S32UVT", 3));
        CHECK(e.current);
        CHECK(e.path_code == 3);
        // 112 degrees 7.74 minutes west.
        CHECK(e.position.longitude_deg == Approx(-(112.0 + 7.74 / 60.0)).margin(1e-9));
        CHECK(*e.position.speed_knots == Approx(20.0));
        REQUIRE(e.position.course_deg.has_value());
        CHECK(*e.position.course_deg == 251);
        // "j/" is the jeep from the primary table.
        CHECK(e.position.symbol_code == 'j');
        CHECK(e.position.symbol_table == '/');
    }
    SECTION("page 52, speed and course example, both encodings") {
        // 86 knots, 194 degrees: SP "t" or "$", DC "]" or "Y", SE "z".
        for (const std::uint8_t sp : {std::uint8_t{'t'}, std::uint8_t{'$'}}) {
            for (const std::uint8_t dc : {std::uint8_t{']'}, std::uint8_t{'Y'}}) {
                std::vector<std::uint8_t> info = kMicEInfo;
                info[4] = sp;
                info[5] = dc;
                info[6] = 'z';
                const auto e = mic_e(parse_bytes(info, "S32UVT"));
                CHECK(*e.position.speed_knots == Approx(86.0));
                CHECK(*e.position.course_deg == 194);
            }
        }
    }
    SECTION("pages 53 and 54, ambiguity carried to the longitude") {
        // "T4SQZZ": the last two latitude digits blanked, so the longitude
        // from "(_f" is 112 degrees 7 minutes with the hundredths ignored.
        const auto e = mic_e(parse_bytes(kMicEInfo, "T4SQZZ"));
        CHECK(e.position.ambiguity == 2);
        CHECK(e.position.latitude_deg == Approx(44.0 + 31.0 / 60.0).margin(1e-9));
        CHECK(e.position.longitude_deg == Approx(-(112.0 + 7.0 / 60.0)).margin(1e-9));
    }
    SECTION("page 54, five channels of hex telemetry") {
        std::vector<std::uint8_t> info = kMicEInfo;
        for (const char c : std::string_view("'7200007100")) {
            info.push_back(static_cast<std::uint8_t>(c));
        }
        const auto e = mic_e(parse_bytes(info, "S32UVT"));
        CHECK(e.telemetry == std::vector<std::uint8_t>{0x72, 0x00, 0x00, 0x71, 0x00});
    }
    SECTION("pages 55 and 56, altitude in the status text") {
        // "4T} is 200 feet, 61 metres; also after the two Kenwood type codes.
        for (const std::uint8_t prefix : {std::uint8_t{0}, std::uint8_t{'>'}, std::uint8_t{']'}}) {
            std::vector<std::uint8_t> info = kMicEInfo;
            if (prefix != 0) {
                info.push_back(prefix);
            }
            info.insert(info.end(), {34, '4', 'T', '}'});
            const auto e = mic_e(parse_bytes(info, "S32UVT"));
            REQUIRE(e.altitude_m.has_value());
            CHECK(*e.altitude_m == Approx(61.0));
        }
    }
}

TEST_CASE("types this parser does not reach are refused by name", "[decode][aprs]") {
    auto packet = decode::aprs_parse("APRS", 0, bytes(";LEADER   *092345z4903.50N/07201.75W>"));
    REQUIRE_FALSE(packet.has_value());
    CHECK(packet.error().message.find("';'") != std::string::npos);
}

TEST_CASE("APRS packets survive the whole chain from audio", "[decode][aprs]") {
    // A position report and a Mic-E report as UI frames, through Bell 202
    // audio, the AX.25 decoder, and the parser.
    decode::Ax25Address source;
    source.callsign = "N7LEM";
    decode::Ax25Address aprs;
    aprs.callsign = "APRS";
    aprs.command_or_repeated = true;
    decode::Ax25Address mic_e;
    mic_e.callsign = "S32UVT";
    mic_e.command_or_repeated = true;

    siggen::Ax25FrameSpec first;
    first.destination = aprs;
    first.source = source;
    first.information = bytes("@092345/4903.50N/07201.75W>088/036");
    siggen::Ax25FrameSpec second;
    second.destination = mic_e;
    second.source = source;
    second.information = kMicEInfo;

    std::vector<std::vector<std::uint8_t>> frames;
    for (const auto* spec : {&first, &second}) {
        auto octets = siggen::ax25_frame_octets(*spec);
        REQUIRE(octets.has_value());
        frames.push_back(*octets);
    }
    auto audio = siggen::ax25_render(siggen::Ax25ModConfig{}, frames);
    REQUIRE(audio.has_value());

    auto decoder = decode::Ax25Decoder::create(decode::Ax25Config{});
    REQUIRE(decoder.has_value());
    std::vector<decode::Ax25Frame> got;
    decoder->process(*audio, got);
    REQUIRE(got.size() == 2);

    auto a = decode::aprs_parse(got[0]);
    REQUIRE(a.has_value());
    const auto& r = std::get<decode::AprsPositionReport>(a->body);
    CHECK(r.position.latitude_deg == Approx(kLat).margin(1e-9));
    CHECK(*r.position.course_deg == 88);

    auto b = decode::aprs_parse(got[1]);
    REQUIRE(b.has_value());
    const auto& e = std::get<decode::AprsMicE>(b->body);
    CHECK(e.message == decode::MicEMessage::Returning);
    CHECK(*e.position.course_deg == 251);
}
