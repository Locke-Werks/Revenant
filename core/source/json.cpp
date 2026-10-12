// The JSON parser behind core/source/json.h. The header has why it exists.

#include "core/source/json.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace revenant::source {
namespace {

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    [[nodiscard]] Expected<JsonValue> parse_document()
    {
        skip_space();
        auto value = parse_value(0);
        if (!value) {
            return value;
        }
        skip_space();
        if (at_ != text_.size()) {
            return error("trailing text after the end of the document");
        }
        return value;
    }

private:
    // A sidecar is two levels of nesting. Anything past this is either
    // corrupt or hostile, and either way a recursive descent parser that
    // followed it would take the stack with it.
    static constexpr int kMaxDepth = 64;

    [[nodiscard]] std::unexpected<Error> error(std::string_view what) const
    {
        return fail(std::format("malformed JSON at byte {}: {}", at_, what));
    }

    void skip_space()
    {
        while (at_ < text_.size()) {
            const char c = text_[at_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++at_;
            } else {
                break;
            }
        }
    }

    [[nodiscard]] bool consume(char c)
    {
        if (at_ < text_.size() && text_[at_] == c) {
            ++at_;
            return true;
        }
        return false;
    }

    [[nodiscard]] Expected<JsonValue> parse_value(int depth);
    [[nodiscard]] Expected<std::string> parse_string();
    [[nodiscard]] Expected<JsonValue> parse_number();

    std::string_view text_;
    std::size_t at_ = 0;
};

Expected<std::string> JsonParser::parse_string()
{
    if (!consume('"')) {
        return error("expected a string");
    }

    std::string out;
    while (true) {
        if (at_ >= text_.size()) {
            return error("a string is not closed before the end of the document");
        }
        const char c = text_[at_++];
        if (c == '"') {
            return out;
        }
        if (static_cast<unsigned char>(c) < 0x20) {
            return error("a raw control character inside a string");
        }
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (at_ >= text_.size()) {
            return error("a string ends with a dangling escape");
        }
        const char esc = text_[at_++];
        switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (at_ + 4 > text_.size()) {
                    return error("a \\u escape is cut short");
                }
                std::uint32_t code = 0;
                for (int i = 0; i < 4; ++i) {
                    const char h = text_[at_ + static_cast<std::size_t>(i)];
                    int digit = -1;
                    if (h >= '0' && h <= '9') {
                        digit = h - '0';
                    } else if (h >= 'a' && h <= 'f') {
                        digit = h - 'a' + 10;
                    } else if (h >= 'A' && h <= 'F') {
                        digit = h - 'A' + 10;
                    }
                    if (digit < 0) {
                        return error("a \\u escape has a character that is not hexadecimal");
                    }
                    code = (code << 4) | static_cast<std::uint32_t>(digit);
                }
                at_ += 4;

                // A leading surrogate must be followed by its trailing half.
                // Emitting the halves separately would produce a string that
                // is not valid UTF-8, which then reaches a filesystem call as
                // a path nobody can open.
                if (code >= 0xD800 && code <= 0xDBFF) {
                    if (at_ + 6 > text_.size() || text_[at_] != '\\' || text_[at_ + 1] != 'u') {
                        return error("a leading surrogate is not followed by a trailing one");
                    }
                    std::uint32_t low = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = text_[at_ + 2 + static_cast<std::size_t>(i)];
                        int digit = -1;
                        if (h >= '0' && h <= '9') {
                            digit = h - '0';
                        } else if (h >= 'a' && h <= 'f') {
                            digit = h - 'a' + 10;
                        } else if (h >= 'A' && h <= 'F') {
                            digit = h - 'A' + 10;
                        }
                        if (digit < 0) {
                            return error("a trailing surrogate is not hexadecimal");
                        }
                        low = (low << 4) | static_cast<std::uint32_t>(digit);
                    }
                    if (low < 0xDC00 || low > 0xDFFF) {
                        return error("a leading surrogate is followed by something that is not "
                                     "a trailing surrogate");
                    }
                    at_ += 6;
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                } else if (code >= 0xDC00 && code <= 0xDFFF) {
                    return error("a trailing surrogate with no leading one before it");
                }

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
                break;
            }
            default:
                return error("an unknown escape sequence");
        }
    }
}

Expected<JsonValue> JsonParser::parse_number()
{
    const std::size_t start = at_;
    if (at_ < text_.size() && (text_[at_] == '-' || text_[at_] == '+')) {
        ++at_;
    }
    bool has_fraction = false;
    while (at_ < text_.size()) {
        const char c = text_[at_];
        if ((c >= '0' && c <= '9')) {
            ++at_;
        } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
            has_fraction = true;
            ++at_;
        } else {
            break;
        }
    }
    if (at_ == start) {
        return error("expected a number");
    }

    const std::string_view token = text_.substr(start, at_ - start);
    JsonValue value;
    value.kind = JsonValue::Kind::Number;

    const char* first = token.data();
    const char* last = token.data() + token.size();
    const auto parsed = std::from_chars(first, last, value.number);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return fail(std::format("malformed JSON at byte {}: '{}' is not a number", start, token));
    }

    if (!has_fraction) {
        std::int64_t as_integer = 0;
        const auto exact = std::from_chars(first, last, as_integer);
        if (exact.ec == std::errc{} && exact.ptr == last) {
            value.integral = true;
            value.integer = as_integer;
        }
        std::uint64_t as_unsigned = 0;
        const auto exact_unsigned = std::from_chars(first, last, as_unsigned);
        if (exact_unsigned.ec == std::errc{} && exact_unsigned.ptr == last) {
            value.unsigned_integral = true;
            value.uinteger = as_unsigned;
        }
    }
    return value;
}

Expected<JsonValue> JsonParser::parse_value(int depth)
{
    if (depth > kMaxDepth) {
        return error(std::format("nesting deeper than {} levels", kMaxDepth));
    }
    if (at_ >= text_.size()) {
        return error("the document ends where a value was expected");
    }

    const char c = text_[at_];
    if (c == '{') {
        ++at_;
        JsonValue object;
        object.kind = JsonValue::Kind::Object;
        skip_space();
        if (consume('}')) {
            return object;
        }
        while (true) {
            skip_space();
            auto key = parse_string();
            if (!key) {
                return std::unexpected(key.error());
            }
            skip_space();
            if (!consume(':')) {
                return error("expected ':' after an object key");
            }
            skip_space();
            auto value = parse_value(depth + 1);
            if (!value) {
                return value;
            }
            object.members.emplace_back(std::move(*key), std::move(*value));
            skip_space();
            if (consume(',')) {
                continue;
            }
            if (consume('}')) {
                return object;
            }
            return error("expected ',' or '}' in an object");
        }
    }

    if (c == '[') {
        ++at_;
        JsonValue array;
        array.kind = JsonValue::Kind::Array;
        skip_space();
        if (consume(']')) {
            return array;
        }
        while (true) {
            skip_space();
            auto value = parse_value(depth + 1);
            if (!value) {
                return value;
            }
            array.elements.push_back(std::move(*value));
            skip_space();
            if (consume(',')) {
                continue;
            }
            if (consume(']')) {
                return array;
            }
            return error("expected ',' or ']' in an array");
        }
    }

    if (c == '"') {
        auto text = parse_string();
        if (!text) {
            return std::unexpected(text.error());
        }
        JsonValue value;
        value.kind = JsonValue::Kind::String;
        value.text = std::move(*text);
        return value;
    }

    if (text_.compare(at_, 4, "true") == 0) {
        at_ += 4;
        JsonValue value;
        value.kind = JsonValue::Kind::Bool;
        value.boolean = true;
        return value;
    }
    if (text_.compare(at_, 5, "false") == 0) {
        at_ += 5;
        JsonValue value;
        value.kind = JsonValue::Kind::Bool;
        value.boolean = false;
        return value;
    }
    if (text_.compare(at_, 4, "null") == 0) {
        at_ += 4;
        return JsonValue{};
    }

    return parse_number();
}

}  // namespace

Expected<JsonValue> parse_json(std::string_view text)
{
    JsonParser parser(text);
    return parser.parse_document();
}

}  // namespace revenant::source
