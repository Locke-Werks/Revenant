// A small JSON reader and writer with no Qt in it, for the frequency manager's
// file and for the two formats it imports that are JSON.
//
// WHY NOT QJsonDocument, which the client already links. The rules that read
// and write these files live in Qt-free headers so ui/tests can hold them, and
// that test binary links no Qt at all; ui/CMakeLists.txt says why at length.
// A memory file is the one thing in the client an operator will keep for
// years and carry between machines, so the code that reads it is the code
// that most needs a test.
//
// NUMBERS KEEP THEIR TEXT. A frequency is integer hertz everywhere in this
// tree (docs/conventions.md), and a JSON number is a decimal literal that a
// reader is free to turn into a double on the way in. Doing that here would
// be harmless for 146520000 and wrong for nothing today, and it is still the
// one conversion the conventions forbid. So a number is held as the literal
// it was written as, and as_integer reads it exactly: "98500000",
// "9.85e7" and "98500000.0", which SDR++ writes, are all the same integer and
// none of them passes through a double.
//
// What is not here: comments, trailing commas, NaN and the other extensions
// some writers emit. None of the three formats this reads produce them, and a
// file that has them is refused with the line it failed on rather than half
// read.

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/error.h"

namespace revenant::ui::json {

struct Member;

struct Value {
    enum class Kind : std::uint8_t {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    Kind kind = Kind::Null;
    bool boolean = false;

    // The line of the document the value starts on, counted from one, so an
    // importer can name the line it skipped. Zero for a value built here.
    std::size_t line = 0;

    // A number's literal exactly as written, or a string's decoded UTF-8.
    std::string text;

    std::vector<Value> items;

    // In the order written, which is the order a writer here puts them back
    // in: a file a person reads should not come back reshuffled.
    std::vector<Member> members;

    [[nodiscard]] bool is_null() const { return kind == Kind::Null; }
    [[nodiscard]] bool is_bool() const { return kind == Kind::Bool; }
    [[nodiscard]] bool is_number() const { return kind == Kind::Number; }
    [[nodiscard]] bool is_string() const { return kind == Kind::String; }
    [[nodiscard]] bool is_array() const { return kind == Kind::Array; }
    [[nodiscard]] bool is_object() const { return kind == Kind::Object; }

    // The member with this key, or nullptr. The first one when a writer
    // repeated a key, which JSON permits and says nothing about.
    [[nodiscard]] const Value* find(std::string_view key) const;

    // Appends a member to an object. No check for a repeated key, because
    // the writers here build each object once from a struct.
    Value& set(std::string key, Value value);
};

struct Member {
    std::string key;
    Value value;
};

inline const Value* Value::find(std::string_view key) const
{
    for (const Member& member : members) {
        if (member.key == key) {
            return &member.value;
        }
    }
    return nullptr;
}

inline Value& Value::set(std::string key, Value value)
{
    members.push_back(Member{std::move(key), std::move(value)});
    return members.back().value;
}

// ---------------------------------------------------------------------------
// Building values
// ---------------------------------------------------------------------------

[[nodiscard]] inline Value null() { return {}; }

[[nodiscard]] inline Value boolean(bool on)
{
    Value out;
    out.kind = Value::Kind::Bool;
    out.boolean = on;
    return out;
}

[[nodiscard]] inline Value integer(std::int64_t number)
{
    Value out;
    out.kind = Value::Kind::Number;
    out.text = std::to_string(number);
    return out;
}

[[nodiscard]] inline Value string(std::string text)
{
    Value out;
    out.kind = Value::Kind::String;
    out.text = std::move(text);
    return out;
}

[[nodiscard]] inline Value array()
{
    Value out;
    out.kind = Value::Kind::Array;
    return out;
}

[[nodiscard]] inline Value object()
{
    Value out;
    out.kind = Value::Kind::Object;
    return out;
}

// ---------------------------------------------------------------------------
// Reading numbers exactly
// ---------------------------------------------------------------------------

// The number as an integer, if the literal names one exactly and it fits.
//
// Decimal arithmetic on the literal and not a double: "146.52e6" is 146520000
// exactly, "1.5" is not an integer and answers nothing, and "1e30" does not
// fit and answers nothing. rounding, when set, rounds a fraction half away
// from zero instead of refusing it, which is what a bandwidth written as
// 224298.0625 wants.
[[nodiscard]] inline std::optional<std::int64_t> literal_to_integer(std::string_view literal,
                                                                    bool rounding = false)
{
    std::size_t at = 0;
    bool negative = false;
    if (at < literal.size() && (literal[at] == '-' || literal[at] == '+')) {
        negative = literal[at] == '-';
        ++at;
    }

    std::string digits;
    std::int64_t point_shift = 0;
    bool any_digit = false;
    bool seen_point = false;
    for (; at < literal.size(); ++at) {
        const char c = literal[at];
        if (c >= '0' && c <= '9') {
            any_digit = true;
            if (!(digits.empty() && c == '0')) {
                digits.push_back(c);
            }
            if (seen_point) {
                --point_shift;
            }
            // A leading zero after the point still moves it.
            continue;
        }
        if (c == '.' && !seen_point) {
            seen_point = true;
            continue;
        }
        break;
    }
    if (!any_digit) {
        return std::nullopt;
    }

    std::int64_t exponent = 0;
    if (at < literal.size() && (literal[at] == 'e' || literal[at] == 'E')) {
        ++at;
        bool exp_negative = false;
        if (at < literal.size() && (literal[at] == '-' || literal[at] == '+')) {
            exp_negative = literal[at] == '-';
            ++at;
        }
        bool exp_digit = false;
        for (; at < literal.size() && literal[at] >= '0' && literal[at] <= '9'; ++at) {
            exp_digit = true;
            if (exponent > 1000) {
                return std::nullopt;
            }
            exponent = exponent * 10 + (literal[at] - '0');
        }
        if (!exp_digit) {
            return std::nullopt;
        }
        if (exp_negative) {
            exponent = -exponent;
        }
    }
    if (at != literal.size()) {
        return std::nullopt;
    }

    // digits * 10^scale is the magnitude. Leading zeros were dropped as they
    // were read, so a zero value leaves digits empty.
    const std::int64_t scale = exponent + point_shift;
    if (digits.empty()) {
        return 0;
    }

    std::string whole = digits;
    std::string fraction;
    if (scale >= 0) {
        if (scale > 19) {
            return std::nullopt;
        }
        whole.append(static_cast<std::size_t>(scale), '0');
    } else {
        const auto cut = static_cast<std::size_t>(-scale);
        if (cut >= whole.size()) {
            fraction = std::string(cut - whole.size(), '0') + whole;
            whole.clear();
        } else {
            fraction = whole.substr(whole.size() - cut);
            whole.resize(whole.size() - cut);
        }
    }

    const bool has_fraction = fraction.find_first_not_of('0') != std::string::npos;
    if (has_fraction && !rounding) {
        return std::nullopt;
    }

    std::uint64_t magnitude = 0;
    constexpr auto kLimit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    for (const char c : whole) {
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (magnitude > (kLimit - digit) / 10) {
            return std::nullopt;
        }
        magnitude = magnitude * 10 + digit;
    }
    if (has_fraction && fraction[0] >= '5') {
        if (magnitude == kLimit) {
            return std::nullopt;
        }
        ++magnitude;
    }

    const auto signed_magnitude = static_cast<std::int64_t>(magnitude);
    return negative ? -signed_magnitude : signed_magnitude;
}

[[nodiscard]] inline std::optional<std::int64_t> as_integer(const Value* value,
                                                            bool rounding = false)
{
    if (value == nullptr || !value->is_number()) {
        return std::nullopt;
    }
    return literal_to_integer(value->text, rounding);
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

namespace detail {

// Deep enough for any file these formats produce, which nest four levels at
// most, and shallow enough that a hostile file of ten thousand brackets is a
// refusal rather than a stack overflow in the reader.
inline constexpr int kMaxDepth = 64;

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Expected<Value> document()
    {
        // A UTF-8 byte order mark, which Windows editors put on a file saved
        // "as UTF-8" and which is not whitespace to JSON.
        if (text_.starts_with("\xEF\xBB\xBF")) {
            at_ = 3;
        }
        skip_space();
        auto value = parse_value(0);
        if (!value) {
            return value;
        }
        skip_space();
        if (at_ != text_.size()) {
            return failure("text after the end of the document");
        }
        return value;
    }

private:
    std::string_view text_;
    std::size_t at_ = 0;

    // Counted as whitespace is skipped, which is the only place a newline can
    // be: a string with a raw one in it is refused.
    std::size_t line_ = 1;

    [[nodiscard]] std::unexpected<Error> failure(std::string_view what) const
    {
        return fail("line " + std::to_string(line_) + ": " + std::string(what));
    }

    void skip_space()
    {
        while (at_ < text_.size()) {
            const char c = text_[at_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                return;
            }
            if (c == '\n') {
                ++line_;
            }
            ++at_;
        }
    }

    bool literal(std::string_view word)
    {
        if (text_.substr(at_).starts_with(word)) {
            at_ += word.size();
            return true;
        }
        return false;
    }

    Expected<Value> parse_value(int depth)
    {
        const std::size_t starts_on = line_;
        auto value = parse_bare_value(depth);
        if (value) {
            value->line = starts_on;
        }
        return value;
    }

    Expected<Value> parse_bare_value(int depth)
    {
        if (depth > kMaxDepth) {
            return failure("nested too deeply");
        }
        if (at_ >= text_.size()) {
            return failure("the document ends where a value was expected");
        }
        const char c = text_[at_];
        if (c == '{') {
            return parse_object(depth);
        }
        if (c == '[') {
            return parse_array(depth);
        }
        if (c == '"') {
            auto text = parse_string();
            if (!text) {
                return std::unexpected(text.error());
            }
            return string(std::move(*text));
        }
        if (literal("true")) {
            return boolean(true);
        }
        if (literal("false")) {
            return boolean(false);
        }
        if (literal("null")) {
            return null();
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parse_number();
        }
        return failure(std::string("unexpected character '") + c + "'");
    }

    Expected<Value> parse_number()
    {
        const std::size_t start = at_;
        if (text_[at_] == '-') {
            ++at_;
        }
        const auto digits = [&] {
            const std::size_t from = at_;
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
                ++at_;
            }
            return at_ - from;
        };
        if (digits() == 0) {
            return failure("a number with no digits");
        }
        if (at_ < text_.size() && text_[at_] == '.') {
            ++at_;
            if (digits() == 0) {
                return failure("a number with no digits after its point");
            }
        }
        if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
            ++at_;
            if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) {
                ++at_;
            }
            if (digits() == 0) {
                return failure("a number with no digits in its exponent");
            }
        }
        Value out;
        out.kind = Value::Kind::Number;
        out.text = std::string(text_.substr(start, at_ - start));
        return out;
    }

    static void append_utf8(std::string& out, std::uint32_t code)
    {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    std::optional<std::uint32_t> hex4()
    {
        if (at_ + 4 > text_.size()) {
            return std::nullopt;
        }
        std::uint32_t code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[at_++];
            code <<= 4;
            if (c >= '0' && c <= '9') {
                code |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                code |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                code |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return std::nullopt;
            }
        }
        return code;
    }

    Expected<std::string> parse_string()
    {
        ++at_;  // the opening quote
        std::string out;
        while (true) {
            if (at_ >= text_.size()) {
                return failure("a string that never closes");
            }
            const char c = text_[at_++];
            if (c == '"') {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return failure("a control character inside a string");
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (at_ >= text_.size()) {
                return failure("a string that never closes");
            }
            const char escape = text_[at_++];
            switch (escape) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    auto code = hex4();
                    if (!code) {
                        return failure("a \\u escape without four hex digits");
                    }
                    // A surrogate pair is one character written as two
                    // escapes; a lone half is replaced rather than refused,
                    // so a name is not lost over one bad character.
                    if (*code >= 0xD800 && *code <= 0xDBFF && text_.substr(at_).starts_with("\\u")) {
                        at_ += 2;
                        const auto low = hex4();
                        if (!low) {
                            return failure("a \\u escape without four hex digits");
                        }
                        if (*low >= 0xDC00 && *low <= 0xDFFF) {
                            *code = 0x10000 + ((*code - 0xD800) << 10) + (*low - 0xDC00);
                        } else {
                            *code = 0xFFFD;
                        }
                    } else if (*code >= 0xD800 && *code <= 0xDFFF) {
                        *code = 0xFFFD;
                    }
                    append_utf8(out, *code);
                    break;
                }
                default:
                    return failure(std::string("an unknown escape \\") + escape);
            }
        }
    }

    Expected<Value> parse_array(int depth)
    {
        ++at_;
        Value out = array();
        skip_space();
        if (at_ < text_.size() && text_[at_] == ']') {
            ++at_;
            return out;
        }
        while (true) {
            skip_space();
            auto item = parse_value(depth + 1);
            if (!item) {
                return item;
            }
            out.items.push_back(std::move(*item));
            skip_space();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == ']') {
                ++at_;
                return out;
            }
            return failure("expected ',' or ']' in an array");
        }
    }

    Expected<Value> parse_object(int depth)
    {
        ++at_;
        Value out = object();
        skip_space();
        if (at_ < text_.size() && text_[at_] == '}') {
            ++at_;
            return out;
        }
        while (true) {
            skip_space();
            if (at_ >= text_.size() || text_[at_] != '"') {
                return failure("expected a quoted key in an object");
            }
            auto key = parse_string();
            if (!key) {
                return std::unexpected(key.error());
            }
            skip_space();
            if (at_ >= text_.size() || text_[at_] != ':') {
                return failure("expected ':' after a key");
            }
            ++at_;
            skip_space();
            auto value = parse_value(depth + 1);
            if (!value) {
                return value;
            }
            out.set(std::move(*key), std::move(*value));
            skip_space();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == '}') {
                ++at_;
                return out;
            }
            return failure("expected ',' or '}' in an object");
        }
    }
};

inline void write_string(std::string& out, std::string_view text)
{
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    constexpr char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 0xF]);
                    out.push_back(kHex[static_cast<unsigned char>(c) & 0xF]);
                } else {
                    // UTF-8 passes through as it is: the file is UTF-8 and
                    // a name in Cyrillic should read as Cyrillic in an editor.
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

// An array of scalars stays on one line, which is what a list of tags or of
// scan list members reads best as. Anything holding a container is spread out.
[[nodiscard]] inline bool flat(const Value& value)
{
    for (const Value& item : value.items) {
        if (item.is_array() || item.is_object()) {
            return false;
        }
    }
    return true;
}

inline void write_value(std::string& out, const Value& value, int indent, int level)
{
    const auto newline = [&](int at) {
        out.push_back('\n');
        out.append(static_cast<std::size_t>(indent * at), ' ');
    };
    switch (value.kind) {
        case Value::Kind::Null: out += "null"; return;
        case Value::Kind::Bool: out += value.boolean ? "true" : "false"; return;
        case Value::Kind::Number: out += value.text; return;
        case Value::Kind::String: write_string(out, value.text); return;
        case Value::Kind::Array: {
            if (value.items.empty()) {
                out += "[]";
                return;
            }
            const bool one_line = indent == 0 || flat(value);
            out.push_back('[');
            for (std::size_t i = 0; i < value.items.size(); ++i) {
                if (i > 0) {
                    out += one_line && indent > 0 ? ", " : ",";
                }
                if (!one_line) {
                    newline(level + 1);
                }
                write_value(out, value.items[i], indent, level + 1);
            }
            if (!one_line) {
                newline(level);
            }
            out.push_back(']');
            return;
        }
        case Value::Kind::Object: {
            if (value.members.empty()) {
                out += "{}";
                return;
            }
            out.push_back('{');
            for (std::size_t i = 0; i < value.members.size(); ++i) {
                if (i > 0) {
                    out.push_back(',');
                }
                if (indent > 0) {
                    newline(level + 1);
                }
                write_string(out, value.members[i].key);
                out += indent > 0 ? ": " : ":";
                write_value(out, value.members[i].value, indent, level + 1);
            }
            if (indent > 0) {
                newline(level);
            }
            out.push_back('}');
            return;
        }
    }
}

}  // namespace detail

// The document in text, or an error naming the line it failed on.
[[nodiscard]] inline Expected<Value> parse(std::string_view text)
{
    return detail::Parser(text).document();
}

// The value as text. indent zero is one line; anything else is that many
// spaces a level with a newline at the end, which is what a file wants.
[[nodiscard]] inline std::string write(const Value& value, int indent = 2)
{
    std::string out;
    detail::write_value(out, value, indent, 0);
    if (indent > 0) {
        out.push_back('\n');
    }
    return out;
}

}  // namespace revenant::ui::json
