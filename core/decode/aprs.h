// APRS: the information field of an AX.25 UI frame, parsed into fields.
//
// SPECIFICATION
//
// "APRS Protocol Reference, Protocol Version 1.0", document version 1.0.1,
// 29 August 2000, the APRS Working Group, read from the web.archive.org copy
// of aprs.org/doc/APRS101.PDF (aprs.org itself answers with a challenge page
// rather than the document). Chapter and page numbers below are that
// document's own.
//
// Chapters used: 5 (the Data Type Identifier table, page 17), 6 (time,
// latitude, longitude and position ambiguity formats, pages 22 to 26), 7
// (the course and speed extension, page 27), 8 (uncompressed position
// reports, pages 32 and 33), 9 (compressed position reports, pages 36 to
// 41), 10 (Mic-E, pages 42 to 56), 14 (messages, acknowledgements and
// rejections, pages 71 and 72) and 16 (status reports, pages 80 to 82).
//
// WHERE THIS STOPS
//
// Position, status, message and Mic-E, which is what docs/modes.md asks of
// this decoder and what most of the channel carries. Objects and items
// (chapter 11), weather (chapter 12), telemetry (chapter 13), queries
// (chapter 15), third-party traffic (chapter 17) and the user-defined format
// are recognised by their Data Type Identifier and reported as not parsed,
// with the information field still available from the frame. Data
// extensions other than course and speed (PHG, RNG, DFS, area objects) stay
// in the comment text rather than being decoded.
//
// The WB2OSZ consolidated 1.2 revision that docs/modes.md also names was not
// read for this file. Nothing here depends on it, and where 1.2 extends 1.0.1
// (base-91 comment telemetry, the !DAO! precision extension, new symbols)
// the extension arrives here as comment text.
//
// CLEAN ROOM
//
// No APRS implementation was read. The tests check this parser against the
// document's own worked examples, which is the check a round trip through
// this project's own transmitter cannot give.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/decode/ax25.h"
#include "core/error.h"

namespace revenant::decode {

// ---------------------------------------------------------------------------
// What comes out
// ---------------------------------------------------------------------------

// Chapter 6, page 22: three timestamp formats. MDHM is used only by
// positionless weather reports, which are not parsed here.
enum class AprsTimeFormat : std::uint8_t {
    DayHourMinuteZulu,   // "092345z"
    DayHourMinuteLocal,  // "092345/"
    HourMinuteSecond,    // "234517h", zulu
};

struct AprsTimestamp {
    AprsTimeFormat format = AprsTimeFormat::DayHourMinuteZulu;
    int day = 0;  // zero for HourMinuteSecond
    int hour = 0;
    int minute = 0;
    int second = 0;  // zero unless HourMinuteSecond
};

struct AprsPosition {
    // Degrees, north and east positive.
    double latitude_deg = 0.0;
    double longitude_deg = 0.0;

    // Chapter 6, page 24: how many trailing digits of the latitude were sent
    // as spaces, 0 to 4, which the chapter says applies to the longitude as
    // well. The blanked digits are read as zero, so the position is the
    // corner of the ambiguity box nearest the equator and the prime meridian
    // on each axis, and the box is this many digits wide.
    int ambiguity = 0;

    // Chapter 6, page 24 and chapter 20: the symbol table identifier and the
    // symbol code.
    char symbol_table = '/';
    char symbol_code = ' ';

    // Chapter 9.
    bool compressed = false;

    // Chapter 7 course and speed extension, the compressed cs bytes, or
    // Mic-E. Course in degrees, 1 to 360 with 0 meaning unknown; speed in
    // knots.
    std::optional<int> course_deg;
    std::optional<double> speed_knots;

    // Feet. From "/A=" in a comment (chapter 6, page 26), or from compressed
    // cs bytes when the T byte says the source was GGA (chapter 9, page 40).
    std::optional<double> altitude_ft;

    // Chapter 9, page 39: compressed pre-calculated radio range, in miles.
    std::optional<double> range_miles;

    // Chapter 9, page 39: the compression type byte, less 33.
    std::optional<std::uint8_t> compression_type;
};

// Chapter 8, and chapter 9 for the compressed form.
struct AprsPositionReport {
    // Chapter 6, page 23: "=" and "@" say the station runs APRS messaging.
    bool messaging = false;
    std::optional<AprsTimestamp> timestamp;
    AprsPosition position;
    std::string comment;
};

// Chapter 14.
struct AprsMessage {
    enum class Kind : std::uint8_t { Message, Acknowledgement, Rejection };
    Kind kind = Kind::Message;

    // The nine-character addressee with its padding removed.
    std::string addressee;

    // The text of a message. Empty for an acknowledgement or rejection.
    std::string text;

    // The message number, which a message asks to be acknowledged and an
    // acknowledgement or rejection names.
    std::optional<std::string> id;
};

// Chapter 16.
struct AprsStatus {
    std::optional<AprsTimestamp> timestamp;
    std::string text;

    // Page 81: a four or six character locator immediately after the Data
    // Type Identifier, followed by a symbol table and code.
    std::optional<std::string> maidenhead;
    std::optional<char> symbol_table;
    std::optional<char> symbol_code;
};

// Chapter 10, page 45: the Mic-E message types.
enum class MicEMessage : std::uint8_t {
    OffDuty,     // M0
    EnRoute,     // M1
    InService,   // M2
    Returning,   // M3
    Committed,   // M4
    Special,     // M5
    Priority,    // M6
    Custom0,
    Custom1,
    Custom2,
    Custom3,
    Custom4,
    Custom5,
    Custom6,
    Emergency,
    // Page 45 note: a mix of standard and custom ones is "unknown".
    Unknown,
};

[[nodiscard]] std::string_view mic_e_message_name(MicEMessage message);

// Chapter 10.
struct AprsMicE {
    AprsPosition position;
    MicEMessage message = MicEMessage::Unknown;

    // Page 46: "`" is current GPS data and "'" old, except that a TM-D700
    // sends "'" for current data, which page 55 says cannot be told apart
    // with certainty. Reported as sent.
    bool current = false;

    // The destination SSID, which page 46 says selects a generic digipeater
    // path.
    std::uint8_t path_code = 0;

    // Page 54: two or five channels of telemetry, when the byte after the
    // symbol table is a telemetry flag.
    std::vector<std::uint8_t> telemetry;

    // Page 54: everything after the symbol table when it is not telemetry.
    std::string status_text;

    // Page 55: metres, from a "xxx}" base-91 group at the start of the status
    // text, relative to 10 km below sea level.
    std::optional<double> altitude_m;
};

struct AprsPacket {
    // Chapter 5, page 17: the first character of the information field, or
    // "!" where page 18's exception found it later in the field.
    char data_type = 0;

    std::variant<AprsPositionReport, AprsMessage, AprsStatus, AprsMicE> body;
};

// Parses the information field, with the destination call sign supplied for
// Mic-E, which carries half its position there. Fails with a message naming
// the Data Type Identifier when the type is one this file does not parse,
// and with a message naming the field when a parsed type is malformed.
[[nodiscard]] Expected<AprsPacket> aprs_parse(std::string_view destination_callsign,
                                              std::uint8_t destination_ssid,
                                              std::span<const std::uint8_t> information);

// The same, taking both from a frame. Fails on a frame that is not UI, since
// chapter 3 says APRS uses UI frames exclusively.
[[nodiscard]] Expected<AprsPacket> aprs_parse(const Ax25Frame& frame);

}  // namespace revenant::decode
