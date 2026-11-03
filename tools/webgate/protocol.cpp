#include "tools/webgate/protocol.h"

#include <kj/exception.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <exception>

namespace revenant::webgate {

namespace {

// ---- Little-endian writers --------------------------------------------------
//
// Byte by byte rather than memcpy of the native value, so the layout does not
// depend on the host.

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

void put_f32(std::vector<std::uint8_t>& out, float v) {
    put_u32(out, std::bit_cast<std::uint32_t>(v));
}

void put_f64(std::vector<std::uint8_t>& out, double v) {
    put_u64(out, std::bit_cast<std::uint64_t>(v));
}

// ---- JSON reading -----------------------------------------------------------

// Largest integer a double holds exactly, so also the largest a phone can send.
constexpr double kMaxExactInt = 9007199254740992.0;  // 2^53

using capnp::JsonValue;

std::string_view view_of(capnp::Text::Reader t) {
    return std::string_view(t.begin(), t.size());
}

// First member with this name. A duplicate key later in the object is ignored,
// which is deterministic and cannot be used to swap a value past a check made
// on the first.
std::optional<JsonValue::Reader> find_field(JsonValue::Reader obj, std::string_view key) {
    if (obj.which() != JsonValue::OBJECT) return std::nullopt;
    for (auto f : obj.getObject()) {
        if (view_of(f.getName()) == key) return f.getValue();
    }
    return std::nullopt;
}

// A non-negative whole number a JSON number can carry exactly.
std::optional<std::uint64_t> as_u64(JsonValue::Reader v) {
    if (v.which() != JsonValue::NUMBER) return std::nullopt;
    const double d = v.getNumber();
    if (!std::isfinite(d) || d < 0.0 || d > kMaxExactInt || d != std::floor(d)) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(d);
}

// Parses text into a fresh message. Throws kj::Exception on bad JSON; callers
// catch it.
std::shared_ptr<capnp::MallocMessageBuilder> decode(std::string_view text) {
    auto message = std::make_shared<capnp::MallocMessageBuilder>();
    capnp::JsonCodec codec;
    // The default depth is 64, which is plenty for a control message and bounds
    // the stack the parser uses on hostile input.
    codec.setMaxNestingDepth(16);
    auto root = message->initRoot<JsonValue>();
    codec.decodeRaw(kj::arrayPtr(text.data(), text.size()), root);
    return message;
}

std::string clip(std::string_view s, std::size_t n = 64) {
    if (s.size() <= n) return std::string(s);
    return std::string(s.substr(0, n)) + "...";
}

std::string describe(const std::exception& e) {
    return e.what();
}

Expected<JsonValue::Reader> arg(const Command& c, std::string_view key) {
    auto v = find_field(c.args.reader(), key);
    if (!v) return fail("missing argument: " + std::string(key));
    return *v;
}

}  // namespace

// ---- Binary frames ----------------------------------------------------------

std::vector<std::uint8_t> encode(const ReducedFrame& frame) {
    std::vector<std::uint8_t> out;
    out.reserve(kSpectrumHeaderSize + frame.px.size());
    out.push_back(static_cast<std::uint8_t>(Tag::Spectrum));
    put_f32(out, frame.offset_db);
    put_f32(out, frame.scale_db);
    put_u64(out, frame.source_start);
    put_f64(out, frame.view.center_hz);
    put_f64(out, frame.view.span_hz);
    put_u32(out, static_cast<std::uint32_t>(frame.px.size()));
    out.insert(out.end(), frame.px.begin(), frame.px.end());
    return out;
}

std::vector<std::uint8_t> encode_opus(std::uint64_t vrx, std::uint32_t seq,
                                      std::span<const std::uint8_t> packet) {
    std::vector<std::uint8_t> out;
    out.reserve(kOpusHeaderSize + packet.size());
    out.push_back(static_cast<std::uint8_t>(Tag::Opus));
    put_u64(out, vrx);
    put_u32(out, seq);
    out.insert(out.end(), packet.begin(), packet.end());
    return out;
}

// ---- Commands ---------------------------------------------------------------

bool is_known_op(std::string_view op) {
    return std::find(std::begin(kOps), std::end(kOps), op) != std::end(kOps);
}

JsonArgs::JsonArgs(std::shared_ptr<capnp::MallocMessageBuilder> message)
    : message_(std::move(message)) {}

JsonValue::Reader JsonArgs::reader() const {
    if (!message_) return JsonValue::Reader();
    return message_->getRoot<JsonValue>().asReader();
}

bool Command::has(std::string_view key) const {
    return find_field(args.reader(), key).has_value();
}

Expected<std::uint64_t> Command::get_u64(std::string_view key) const {
    auto v = arg(*this, key);
    if (!v) return std::unexpected(v.error());
    if (auto n = as_u64(*v)) return *n;
    return fail("argument " + std::string(key) + " must be a whole number from 0 to 2^53");
}

Expected<std::int64_t> Command::get_i64(std::string_view key) const {
    auto v = arg(*this, key);
    if (!v) return std::unexpected(v.error());
    if (v->which() == JsonValue::NUMBER) {
        const double d = v->getNumber();
        if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) <= kMaxExactInt) {
            return static_cast<std::int64_t>(d);
        }
    }
    return fail("argument " + std::string(key) + " must be a whole number within 2^53");
}

Expected<double> Command::get_double(std::string_view key) const {
    auto v = arg(*this, key);
    if (!v) return std::unexpected(v.error());
    if (v->which() == JsonValue::NUMBER && std::isfinite(v->getNumber())) {
        return v->getNumber();
    }
    return fail("argument " + std::string(key) + " must be a number");
}

Expected<std::string> Command::get_string(std::string_view key) const {
    auto v = arg(*this, key);
    if (!v) return std::unexpected(v.error());
    if (v->which() == JsonValue::STRING) return std::string(view_of(v->getString()));
    return fail("argument " + std::string(key) + " must be a string");
}

Expected<bool> Command::get_bool(std::string_view key) const {
    auto v = arg(*this, key);
    if (!v) return std::unexpected(v.error());
    if (v->which() == JsonValue::BOOLEAN) return v->getBoolean();
    return fail("argument " + std::string(key) + " must be true or false");
}

Expected<Command> parse_command(std::string_view text) {
    if (text.size() > kMaxCommandBytes) {
        return fail("command too large: " + std::to_string(text.size()) + " bytes, limit " +
                    std::to_string(kMaxCommandBytes));
    }
    try {
        auto message = decode(text);
        const JsonValue::Reader root = message->getRoot<JsonValue>().asReader();
        if (root.which() != JsonValue::OBJECT) return fail("command must be a JSON object");

        const auto id_field = find_field(root, "id");
        if (!id_field) return fail("command has no id");
        const auto id = as_u64(*id_field);
        if (!id) return fail("command id must be a whole number from 0 to 2^53");

        const auto op_field = find_field(root, "op");
        if (!op_field || op_field->which() != JsonValue::STRING) {
            return fail("command has no op");
        }
        const std::string op(view_of(op_field->getString()));
        if (!is_known_op(op)) return fail("unknown op: " + clip(op));

        Command cmd;
        cmd.id = *id;
        cmd.op = op;

        // args gets a message of its own, copied out of the parsed command, so
        // the Command owns exactly what the dispatcher reads and nothing else.
        auto args_message = std::make_shared<capnp::MallocMessageBuilder>();
        auto args_root = args_message->initRoot<JsonValue>();
        if (const auto a = find_field(root, "args")) {
            if (a->which() != JsonValue::OBJECT) return fail("args must be an object");
            args_message->setRoot(*a);
        } else {
            args_root.initObject(0);
        }
        cmd.args = JsonArgs(std::move(args_message));
        return cmd;
    } catch (const kj::Exception& e) {
        return fail("invalid JSON: " + std::string(e.getDescription().cStr()));
    } catch (const std::exception& e) {
        return fail("invalid command: " + describe(e));
    } catch (...) {
        return fail("invalid command");
    }
}

std::optional<std::uint64_t> peek_id(std::string_view text) {
    if (text.size() > kMaxCommandBytes) return std::nullopt;
    try {
        auto message = decode(text);
        const JsonValue::Reader root = message->getRoot<JsonValue>().asReader();
        if (root.which() != JsonValue::OBJECT) return std::nullopt;
        const auto id_field = find_field(root, "id");
        if (!id_field) return std::nullopt;
        return as_u64(*id_field);
    } catch (...) {
        return std::nullopt;
    }
}

// ---- Output -----------------------------------------------------------------

std::string json_string(std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (c < 0x20 || c == 0x7F) {
                        out += "\\u00";
                        out.push_back(kHex[c >> 4]);
                        out.push_back(kHex[c & 0xF]);
                    } else {
                        out.push_back(static_cast<char>(c));
                    }
            }
            ++i;
            continue;
        }

        // Multi-byte sequence: validate length, continuation bytes, overlongs,
        // surrogates and the 0x10FFFF ceiling. Anything else is one U+FFFD per
        // offending lead byte, so the output is always valid UTF-8.
        std::size_t len = 0;
        std::uint32_t cp = 0;
        if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }

        bool valid = len != 0 && i + len <= n;
        for (std::size_t k = 1; valid && k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) valid = false;
            else cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (valid) {
            if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
                (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
                valid = false;
            }
        }
        if (valid) {
            out.append(reinterpret_cast<const char*>(p + i), len);
            i += len;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    out.push_back('"');
    return out;
}

JsonObject& JsonObject::add_raw(std::string_view key, std::string_view json) {
    // "event" names the message in event(); a field with that key would be a
    // second, conflicting name, and the last one wins in a browser's JSON.parse.
    if (key == "event") return *this;
    if (!body_.empty()) body_.push_back(',');
    body_ += json_string(key);
    body_.push_back(':');
    body_ += json;
    return *this;
}

JsonObject& JsonObject::add_null(std::string_view key) { return add_raw(key, "null"); }

JsonObject& JsonObject::add_bool(std::string_view key, bool value) {
    return add_raw(key, value ? "true" : "false");
}

JsonObject& JsonObject::add_u64(std::string_view key, std::uint64_t value) {
    return add_raw(key, std::to_string(value));
}

JsonObject& JsonObject::add_i64(std::string_view key, std::int64_t value) {
    return add_raw(key, std::to_string(value));
}

std::string json_number(double value) {
    if (!std::isfinite(value)) return "null";
    std::array<char, 40> buf{};
    // Shortest text that round-trips, so a frequency survives the trip exactly.
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    return std::string(buf.data(), static_cast<std::size_t>(r.ptr - buf.data()));
}

JsonObject& JsonObject::add_double(std::string_view key, double value) {
    return add_raw(key, json_number(value));
}

JsonObject& JsonObject::add_string(std::string_view key, std::string_view value) {
    return add_raw(key, json_string(value));
}

std::string JsonObject::str() const { return "{" + body_ + "}"; }

std::string ok_reply(std::uint64_t id, std::string_view result_json) {
    JsonObject o;
    o.add_u64("id", id).add_bool("ok", true).add_raw("result", result_json);
    return o.str();
}

std::string error_reply(std::uint64_t id, std::string_view text) {
    JsonObject o;
    o.add_u64("id", id).add_bool("ok", false).add_string("error", text);
    return o.str();
}

std::string event(std::string_view name, const JsonObject& fields) {
    std::string out = "{\"event\":" + json_string(name);
    const std::string body = fields.str();
    if (body.size() > 2) {
        out.push_back(',');
        out.append(body, 1, body.size() - 2);
    }
    out.push_back('}');
    return out;
}

}  // namespace revenant::webgate
