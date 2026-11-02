// Winlink B2F: the forwarding exchange and message structure carried over
// any Winlink bearer, read from a decoded byte stream.
//
// SPECIFICATION
//
// "Open B2F: Winlink Message Structure and B2 Forwarding Protocol", Winlink
// Development Team, last revised February 2018 (with a 5/27/2020 amendment
// to the File: name length), read at https://winlink.org/B2F on 2026-11-02.
// It defers everything it does not change to the FBB forwarding protocol,
// so the second document is "FBB forwarding protocol", F6FBB, read at
// https://www.f6fbb.org/protocole.html on 2026-11-02. Clauses used:
//
//   B2F  Message Structure: header, blank line, body of exactly Body:
//        characters then CR LF, attachments of exactly File: bytes each
//        then CR LF, in File: order. Mid: on the first line. Header is
//        case insensitive; unknown fields are ignored.
//   B2F  Specific B2 Expansion: the FC proposal, FC type id usize csize,
//        type CM or EM; ";" lines are comments.
//   FBB  Ascii Basic Protocol: FB proposal of seven fields, F> ends a
//        block, FS answers one sign per proposal, FF no more, FQ quit,
//        every command line starts with F in column one and ends in CR.
//   FBB  Binary Compressed Forward Version 0: SOH, header length, title,
//        NUL, ASCII offset, NUL; STX, count (0 means 256), data; EOT and a
//        checksum that makes the data bytes sum to zero modulo 256.
//   FBB  Binary Compressed Forward Version 1: the FS answer letters
//        Y N L H R E A and their sign equivalents, A/! carrying an offset;
//        the first data block opens with the CRC16 of the file (2 bytes)
//        and its uncompressed size (4 bytes), little-endian.
//
// WHERE THIS STOPS
//
// At the compressed payload. Both documents say the payload is LZHUF and
// neither describes it: B2F names the program and points at its source, and
// the LZH format page it cites gives the -lh1- method one line (4 KiB
// window, matches up to 60 bytes, dynamic Huffman, fixed position code)
// with none of the symbol alphabet, the initial tree, the rebuild rule, the
// position code table or the bit order. The FBB page likewise names the
// CRC16 of the six-byte prefix without its polynomial, initial value or
// reflection. So the payload is handed to a decompressor the caller
// supplies, and with none the item is reported as received and verified by
// its checksum but not expanded. The CRC16 is reported as read and not
// checked. parse_message() takes the expanded bytes from whatever produced
// them.
//
// CLEAN ROOM
//
// No Winlink, pat, wl2k-go, FBB or LZHUF source was read.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace revenant::decode::b2f {

// FBB Version 0: the framing bytes.
inline constexpr std::uint8_t kNul = 0x00;
inline constexpr std::uint8_t kSoh = 0x01;
inline constexpr std::uint8_t kStx = 0x02;
inline constexpr std::uint8_t kEot = 0x04;

// FBB Version 1: CRC16 then uncompressed size, ahead of the compressed data.
inline constexpr std::size_t kPrefixBytes = 6;

// ---------------------------------------------------------------------------
// Command lines
// ---------------------------------------------------------------------------

enum class LineKind : std::uint8_t {
    Sid,          // [NAME-VERSION-FLAGS$]
    Proposal,     // FA, FB or FC
    ProposalEnd,  // F>
    Answer,       // FS
    NoMore,       // FF
    Quit,         // FQ
    Comment,      // ; ...
    Text,         // anything else: prompts, banners, malformed commands
};

struct Proposal {
    char code = 0;       // 'A', 'B' or 'C' for FA, FB, FC
    std::string type;    // FA/FB: P or B. FC: CM or EM.
    std::string from;    // FA/FB only
    std::string at;      // FA/FB only: BBS of recipient
    std::string to;      // FA/FB only
    std::string mid;     // BID or MID, at most 12 characters
    std::uint32_t size = 0;             // FA/FB: size. FC: uncompressed size
    std::uint32_t compressed_size = 0;  // FC only
};

enum class AnswerKind : std::uint8_t { Accept, Reject, Later, Hold, Refuse, Error, AcceptFrom };

struct Answer {
    AnswerKind kind = AnswerKind::Accept;
    std::uint32_t offset = 0;  // AcceptFrom only
};

struct Sid {
    std::string text;      // between the brackets
    std::string flags;     // last dash-separated field, without the $
    bool fbb = false;      // F
    bool b1 = false;       // B1
    bool b2 = false;       // B2
};

struct Line {
    LineKind kind = LineKind::Text;
    std::string text;               // the line without its terminator
    std::optional<Proposal> proposal;
    std::vector<Answer> answers;
    std::optional<Sid> sid;
};

// One line, without its CR. Never fails: a command that does not parse is
// reported as Text so the caller can still show it.
Line parse_line(std::string_view line);

// FS body ("+-=", "YLA3350RH"); nullopt if any character is not an answer.
std::optional<std::vector<Answer>> parse_answers(std::string_view body);

// ---------------------------------------------------------------------------
// Binary items
// ---------------------------------------------------------------------------

struct Item {
    std::string title;          // title or file name, sent in clear
    std::uint32_t offset = 0;   // ASCII offset from the SOH header
    std::vector<std::uint8_t> data;  // every data byte, prefix included
    bool checksum_ok = false;
};

// The Version 1 prefix of an item's data.
struct Prefix {
    std::uint16_t crc16 = 0;          // as read, not verified
    std::uint32_t uncompressed_size = 0;
    std::span<const std::uint8_t> compressed;
};
std::optional<Prefix> split_prefix(std::span<const std::uint8_t> data);

// Turns compressed bytes into the expected number of uncompressed bytes, or
// nullopt. Supplied by the caller; see WHERE THIS STOPS.
using Decompressor = std::function<std::optional<std::vector<std::uint8_t>>(
    std::span<const std::uint8_t> compressed, std::uint32_t uncompressed_size)>;

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

struct Attachment {
    std::string name;
    std::vector<std::uint8_t> data;
};

struct Message {
    std::string mid;
    std::string date;     // YYYY/MM/DD HH:MM, UTC
    std::string type;
    std::string from;
    std::vector<std::string> to;
    std::vector<std::string> cc;
    std::string subject;
    std::string mbo;
    std::string body;
    std::vector<Attachment> attachments;
};

enum class MessageError : std::uint8_t {
    NoHeaderEnd,        // no blank line
    MidNotFirst,        // Mid: is not the first line
    NoBodyField,        // Body: missing or not a number
    BadFileField,       // File: without a size and a name
    Truncated,          // body or an attachment runs past the end
    MissingTerminator,  // body or attachment not followed by CR LF
    NoRecipient,        // no To: field
};

struct MessageResult {
    std::optional<Message> message;
    std::optional<MessageError> error;
    std::size_t consumed = 0;  // bytes of input this message used
};

// One message from the uncompressed bytes. Up to five may be combined into
// one compressed file; call again from `consumed`.
MessageResult parse_message(std::span<const std::uint8_t> bytes);

// ---------------------------------------------------------------------------
// The exchange
// ---------------------------------------------------------------------------

struct Event {
    enum class Kind : std::uint8_t { Line, Item, Message, ItemNotExpanded, ItemBad };
    Kind kind = Kind::Line;
    std::optional<Line> line;
    std::optional<Item> item;
    std::optional<Prefix> prefix;  // Item and later kinds, when the data held one
    std::optional<Message> message;
    std::optional<MessageError> message_error;
};

// A monitor over both directions of one link, interleaved in the order they
// were heard. Lines are split on CR (LF ignored). After an FS answer, as
// many binary items as it accepted are expected and read in binary.
class Monitor {
public:
    explicit Monitor(Decompressor decompress = {}) : decompress_(std::move(decompress)) {}

    std::vector<Event> feed(std::span<const std::uint8_t> bytes);

    // Last proposals seen, so an item can be matched to its FC line.
    const std::vector<Proposal>& proposals() const { return block_; }

private:
    enum class State : std::uint8_t { Lines, Header, HeaderBody, BlockLen, BlockData, Checksum };

    void finish_line(std::vector<Event>& out);
    void finish_item(std::uint8_t checksum, std::vector<Event>& out);

    Decompressor decompress_;
    State state_ = State::Lines;
    std::string line_;
    std::vector<Proposal> block_;
    std::vector<Proposal> pending_;  // proposals awaiting an answer
    int items_expected_ = 0;
    std::size_t header_left_ = 0;
    std::vector<std::uint8_t> header_;
    std::size_t block_left_ = 0;
    Item item_;
};

}  // namespace revenant::decode::b2f
