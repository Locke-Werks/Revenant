// Floating-point discipline first, for the reason given in sweep.cpp: the
// interpolated sensitivity is compared against a stored number, so the
// arithmetic that produces it must not depend on whether the host compiler
// chose to fuse a multiply-add.
#include "core/dsp/reference_fp.h"

#include "tools/bench/curve.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

namespace revenant::bench {

static_assert(dsp::kReferenceFpDisciplineApplied,
              "tools/bench/curve.cpp must include core/dsp/reference_fp.h first");

namespace {

constexpr double kQuietNan = std::numeric_limits<double>::quiet_NaN();

// Two curves are the same point when their SNR values agree this closely. The
// file carries six decimals, so anything at this scale is the same number that
// went in.
constexpr double kSnrMatchToleranceDb = 1e-4;

// -------------------------------------------------------------------------
// Emitting
// -------------------------------------------------------------------------

// Negative zero prints as "-0.000000" and positive zero as "0.000000", which
// would make two byte-identical sweeps produce different files if a step
// arrived at zero from below. Normalise it away.
double normalise_zero(double value) {
    if (value == 0.0) {
        return 0.0;
    }
    return value;
}

std::string format_db(double value) {
    return std::format("{:.6f}", normalise_zero(value));
}

// Scientific with a fixed mantissa width, because a BER curve spans six
// decades and a fixed-point rendering would quantise the interesting end of it
// to zero.
std::string format_ber(double value) {
    return std::format("{:.12e}", normalise_zero(value));
}

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (raw) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(byte));
                } else {
                    out += raw;
                }
                break;
        }
    }
    return out;
}

// -------------------------------------------------------------------------
// Parsing
//
// A hand-written parser rather than a dependency, per the assignment, and a
// general one rather than a positional reader of the exact bytes emitted
// above: a curve file gets hand-edited during an investigation, and a reader
// that only accepts its own output fails in a way nobody can diagnose.
// -------------------------------------------------------------------------

struct JsonMember;

struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;

    // String contents, or for a number the raw token, kept so an integer field
    // can be read exactly rather than through a double.
    std::string text;

    std::vector<JsonValue> elements;
    std::vector<JsonMember> members;

    [[nodiscard]] const JsonValue* member(std::string_view key) const;
};

struct JsonMember {
    std::string key;
    JsonValue value;
};

const JsonValue* JsonValue::member(std::string_view key) const {
    for (const JsonMember& entry : members) {
        if (entry.key == key) {
            return &entry.value;
        }
    }
    return nullptr;
}

void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point < 0x80) {
        out += static_cast<char>(code_point);
    } else if (code_point < 0x800) {
        out += static_cast<char>(0xC0u | (code_point >> 6));
        out += static_cast<char>(0x80u | (code_point & 0x3Fu));
    } else if (code_point < 0x10000) {
        out += static_cast<char>(0xE0u | (code_point >> 12));
        out += static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (code_point & 0x3Fu));
    } else {
        out += static_cast<char>(0xF0u | (code_point >> 18));
        out += static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu));
        out += static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (code_point & 0x3Fu));
    }
}

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    [[nodiscard]] Expected<JsonValue> parse_document() {
        Expected<JsonValue> root = parse_value(0);
        if (!root) {
            return root;
        }
        skip_space();
        if (pos_ != text_.size()) {
            return fail(message("trailing content after the top level value"));
        }
        return root;
    }

private:
    // Deep enough for anything this format will hold and shallow enough that a
    // malformed file cannot exhaust the stack.
    static constexpr int kMaxDepth = 32;

    std::string_view text_;
    std::size_t pos_ = 0;

    [[nodiscard]] std::string message(std::string_view what) const {
        return std::format("{} at offset {}", what, pos_);
    }

    void skip_space() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    [[nodiscard]] bool consume(char expected) {
        if (pos_ < text_.size() && text_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    [[nodiscard]] Expected<JsonValue> parse_value(int depth) {
        if (depth > kMaxDepth) {
            return fail(message("nesting is too deep"));
        }
        skip_space();
        if (pos_ >= text_.size()) {
            return fail(message("unexpected end of input"));
        }

        switch (text_[pos_]) {
            case '{':
                return parse_object(depth);
            case '[':
                return parse_array(depth);
            case '"': {
                Expected<std::string> contents = parse_string();
                if (!contents) {
                    return std::unexpected(contents.error());
                }
                JsonValue value;
                value.kind = JsonValue::Kind::String;
                value.text = std::move(*contents);
                return value;
            }
            case 't':
            case 'f':
            case 'n':
                return parse_literal();
            default:
                return parse_number();
        }
    }

    [[nodiscard]] Expected<JsonValue> parse_object(int depth) {
        JsonValue value;
        value.kind = JsonValue::Kind::Object;
        ++pos_;  // '{'
        skip_space();
        if (consume('}')) {
            return value;
        }

        for (;;) {
            skip_space();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return fail(message("expected a member name"));
            }
            Expected<std::string> key = parse_string();
            if (!key) {
                return std::unexpected(key.error());
            }
            skip_space();
            if (!consume(':')) {
                return fail(message("expected ':' after a member name"));
            }
            Expected<JsonValue> member_value = parse_value(depth + 1);
            if (!member_value) {
                return std::unexpected(member_value.error());
            }
            value.members.push_back(JsonMember{std::move(*key), std::move(*member_value)});

            skip_space();
            if (consume(',')) {
                continue;
            }
            if (consume('}')) {
                return value;
            }
            return fail(message("expected ',' or '}' in an object"));
        }
    }

    [[nodiscard]] Expected<JsonValue> parse_array(int depth) {
        JsonValue value;
        value.kind = JsonValue::Kind::Array;
        ++pos_;  // '['
        skip_space();
        if (consume(']')) {
            return value;
        }

        for (;;) {
            Expected<JsonValue> element = parse_value(depth + 1);
            if (!element) {
                return std::unexpected(element.error());
            }
            value.elements.push_back(std::move(*element));

            skip_space();
            if (consume(',')) {
                continue;
            }
            if (consume(']')) {
                return value;
            }
            return fail(message("expected ',' or ']' in an array"));
        }
    }

    [[nodiscard]] Expected<JsonValue> parse_literal() {
        const auto starts_with = [this](std::string_view word) {
            return text_.substr(pos_).starts_with(word);
        };

        JsonValue value;
        if (starts_with("true")) {
            pos_ += 4;
            value.kind = JsonValue::Kind::Bool;
            value.boolean = true;
            return value;
        }
        if (starts_with("false")) {
            pos_ += 5;
            value.kind = JsonValue::Kind::Bool;
            value.boolean = false;
            return value;
        }
        if (starts_with("null")) {
            pos_ += 4;
            value.kind = JsonValue::Kind::Null;
            return value;
        }
        return fail(message("unrecognised literal"));
    }

    [[nodiscard]] Expected<JsonValue> parse_number() {
        const std::size_t start = pos_;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            const bool part_of_number = (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
                                        c == 'e' || c == 'E';
            if (!part_of_number) {
                break;
            }
            ++pos_;
        }
        if (pos_ == start) {
            return fail(message("expected a value"));
        }

        const std::string_view token = text_.substr(start, pos_ - start);
        double parsed = 0.0;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
            return fail(std::format("'{}' is not a number at offset {}", token, start));
        }

        JsonValue value;
        value.kind = JsonValue::Kind::Number;
        value.number = parsed;
        value.text = std::string(token);
        return value;
    }

    [[nodiscard]] Expected<std::string> parse_string() {
        ++pos_;  // opening quote
        std::string out;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return out;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) {
                break;
            }
            const char escape = text_[pos_++];
            switch (escape) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    Expected<std::uint32_t> unit = parse_hex4();
                    if (!unit) {
                        return std::unexpected(unit.error());
                    }
                    std::uint32_t code_point = *unit;
                    // A high surrogate is only half a character. Pair it with
                    // the low surrogate that must follow, or the file holds a
                    // lone surrogate and is not valid UTF-16 either.
                    if (code_point >= 0xD800 && code_point <= 0xDBFF && pos_ + 1 < text_.size() &&
                        text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                        const std::size_t rollback = pos_;
                        pos_ += 2;
                        Expected<std::uint32_t> low = parse_hex4();
                        if (!low) {
                            return std::unexpected(low.error());
                        }
                        if (*low >= 0xDC00 && *low <= 0xDFFF) {
                            code_point = 0x10000 + ((code_point - 0xD800) << 10) + (*low - 0xDC00);
                        } else {
                            pos_ = rollback;
                        }
                    }
                    append_utf8(out, code_point);
                    break;
                }
                default:
                    return fail(message("unrecognised string escape"));
            }
        }
        return fail(message("unterminated string"));
    }

    [[nodiscard]] Expected<std::uint32_t> parse_hex4() {
        if (pos_ + 4 > text_.size()) {
            return fail(message("truncated \\u escape"));
        }
        std::uint32_t value = 0;
        for (int digit = 0; digit < 4; ++digit) {
            const char c = text_[pos_++];
            std::uint32_t nibble = 0;
            if (c >= '0' && c <= '9') {
                nibble = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                nibble = static_cast<std::uint32_t>(c - 'a') + 10u;
            } else if (c >= 'A' && c <= 'F') {
                nibble = static_cast<std::uint32_t>(c - 'A') + 10u;
            } else {
                return fail(message("\\u escape needs four hex digits"));
            }
            value = (value << 4) | nibble;
        }
        return value;
    }
};

Expected<const JsonValue*> require_member(const JsonValue& object, std::string_view key) {
    const JsonValue* found = object.member(key);
    if (found == nullptr) {
        return fail(std::format("missing required field '{}'", key));
    }
    return found;
}

Expected<std::string> member_string(const JsonValue& object, std::string_view key) {
    Expected<const JsonValue*> found = require_member(object, key);
    if (!found) {
        return std::unexpected(found.error());
    }
    if ((*found)->kind != JsonValue::Kind::String) {
        return fail(std::format("field '{}' must be a string", key));
    }
    return (*found)->text;
}

Expected<double> member_double(const JsonValue& object, std::string_view key) {
    Expected<const JsonValue*> found = require_member(object, key);
    if (!found) {
        return std::unexpected(found.error());
    }
    if ((*found)->kind != JsonValue::Kind::Number) {
        return fail(std::format("field '{}' must be a number", key));
    }
    return (*found)->number;
}

Expected<std::uint64_t> member_uint(const JsonValue& object, std::string_view key) {
    Expected<const JsonValue*> found = require_member(object, key);
    if (!found) {
        return std::unexpected(found.error());
    }
    if ((*found)->kind != JsonValue::Kind::Number) {
        return fail(std::format("field '{}' must be a number", key));
    }
    // Read the token rather than the double, so a count near 2^53 survives.
    const std::string& token = (*found)->text;
    std::uint64_t value = 0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return fail(std::format("field '{}' must be a non-negative integer, found '{}'", key, token));
    }
    return value;
}

// The reference BER a point represents for interpolation. A point that saw no
// errors has not proven a BER of zero, only that it is below the resolution of
// the measurement, so the half-an-error convention stands in. log10(0) is not
// a number anybody wants in a sensitivity figure.
double measurable_ber(const CurvePoint& point) {
    if (point.ber > 0.0) {
        return point.ber;
    }
    const auto bits = static_cast<double>(std::max<std::uint64_t>(point.bits_total, 1));
    return 0.5 / bits;
}

const CurvePoint* find_point(const std::vector<CurvePoint>& points, double snr_db) {
    for (const CurvePoint& point : points) {
        if (std::abs(point.snr_db - snr_db) <= kSnrMatchToleranceDb) {
            return &point;
        }
    }
    return nullptr;
}

}  // namespace

Curve make_curve(std::string mode,
                 std::string subject,
                 std::string commit,
                 const SweepConfig& config,
                 const std::vector<SweepPoint>& points) {
    Curve curve;
    curve.mode = std::move(mode);
    curve.subject = std::move(subject);
    curve.commit = std::move(commit);
    curve.config = config;
    curve.points.reserve(points.size());
    for (const SweepPoint& point : points) {
        CurvePoint stored;
        stored.snr_db = point.snr_db;
        stored.ber = point.ber();
        stored.trials = point.trials;
        stored.bits_total = point.bits_total;
        stored.bit_errors = point.bit_errors;
        stored.decoded_count = point.decoded_count;
        curve.points.push_back(stored);
    }
    return curve;
}

std::string curve_to_json(const Curve& curve) {
    // Keys are alphabetical at every level and the points keep the sweep's
    // ascending SNR order. Both are deliberate: the file is diffed by a human
    // in review, and a reordered key is a diff that means nothing.
    std::string out;
    out += "{\n";
    out += std::format("  \"commit\": \"{}\",\n", json_escape(curve.commit));
    out += "  \"config\": {\n";
    out += std::format("    \"base_seed\": {},\n", curve.config.base_seed);
    out += std::format("    \"min_bit_errors\": {},\n", curve.config.min_bit_errors);
    out += std::format("    \"payload_bytes\": {},\n", curve.config.payload_bytes);
    out += std::format("    \"snr_start_db\": {},\n", format_db(curve.config.snr_start_db));
    out += std::format("    \"snr_step_db\": {},\n", format_db(curve.config.snr_step_db));
    out += std::format("    \"snr_stop_db\": {},\n", format_db(curve.config.snr_stop_db));
    out += std::format("    \"trial_batch\": {},\n", curve.config.trial_batch);
    out += std::format("    \"trials_per_point\": {}\n", curve.config.trials_per_point);
    out += "  },\n";
    out += std::format("  \"mode\": \"{}\",\n", json_escape(curve.mode));
    out += "  \"points\": [\n";
    for (std::size_t index = 0; index < curve.points.size(); ++index) {
        const CurvePoint& point = curve.points[index];
        out += "    {\n";
        out += std::format("      \"ber\": {},\n", format_ber(point.ber));
        out += std::format("      \"bit_errors\": {},\n", point.bit_errors);
        out += std::format("      \"bits_total\": {},\n", point.bits_total);
        out += std::format("      \"decoded_count\": {},\n", point.decoded_count);
        out += std::format("      \"snr_db\": {},\n", format_db(point.snr_db));
        out += std::format("      \"trials\": {}\n", point.trials);
        out += index + 1 == curve.points.size() ? "    }\n" : "    },\n";
    }
    out += "  ],\n";
    out += std::format("  \"schema\": {},\n", kCurveSchemaVersion);
    out += std::format("  \"subject\": \"{}\"\n", json_escape(curve.subject));
    out += "}\n";
    return out;
}

Expected<Curve> curve_from_json(std::string_view text) {
    JsonParser parser(text);
    Expected<JsonValue> document = parser.parse_document();
    if (!document) {
        return std::unexpected(with_context(document.error(), "curve json"));
    }
    if (document->kind != JsonValue::Kind::Object) {
        return fail("curve json: the top level value must be an object");
    }

    Curve curve;

    if (const JsonValue* schema = document->member("schema"); schema != nullptr) {
        if (schema->kind != JsonValue::Kind::Number) {
            return fail("curve json: 'schema' must be a number");
        }
        const int version = static_cast<int>(schema->number);
        if (version > kCurveSchemaVersion) {
            return fail(std::format("curve json: schema version {} is newer than this build understands ({})",
                                    version, kCurveSchemaVersion));
        }
    }

    const auto take_string = [&](std::string_view key, std::string& into) -> Status {
        Expected<std::string> value = member_string(*document, key);
        if (!value) {
            return std::unexpected(with_context(value.error(), "curve json"));
        }
        into = std::move(*value);
        return {};
    };

    if (const Status ok = take_string("commit", curve.commit); !ok) {
        return std::unexpected(ok.error());
    }
    if (const Status ok = take_string("mode", curve.mode); !ok) {
        return std::unexpected(ok.error());
    }
    if (const Status ok = take_string("subject", curve.subject); !ok) {
        return std::unexpected(ok.error());
    }

    Expected<const JsonValue*> config_field = require_member(*document, "config");
    if (!config_field) {
        return std::unexpected(with_context(config_field.error(), "curve json"));
    }
    const JsonValue& config = **config_field;
    if (config.kind != JsonValue::Kind::Object) {
        return fail("curve json: 'config' must be an object");
    }

    struct UintField {
        std::string_view key;
        std::uint64_t* target;
    };
    struct DoubleField {
        std::string_view key;
        double* target;
    };

    std::uint64_t payload_bytes = 0;
    const UintField uint_fields[] = {
        {"base_seed", &curve.config.base_seed},
        {"min_bit_errors", &curve.config.min_bit_errors},
        {"payload_bytes", &payload_bytes},
        {"trial_batch", &curve.config.trial_batch},
        {"trials_per_point", &curve.config.trials_per_point},
    };
    for (const UintField& field : uint_fields) {
        Expected<std::uint64_t> value = member_uint(config, field.key);
        if (!value) {
            return std::unexpected(with_context(value.error(), "curve json config"));
        }
        *field.target = *value;
    }
    curve.config.payload_bytes = static_cast<std::size_t>(payload_bytes);

    const DoubleField double_fields[] = {
        {"snr_start_db", &curve.config.snr_start_db},
        {"snr_step_db", &curve.config.snr_step_db},
        {"snr_stop_db", &curve.config.snr_stop_db},
    };
    for (const DoubleField& field : double_fields) {
        Expected<double> value = member_double(config, field.key);
        if (!value) {
            return std::unexpected(with_context(value.error(), "curve json config"));
        }
        *field.target = *value;
    }

    Expected<const JsonValue*> points_field = require_member(*document, "points");
    if (!points_field) {
        return std::unexpected(with_context(points_field.error(), "curve json"));
    }
    if ((*points_field)->kind != JsonValue::Kind::Array) {
        return fail("curve json: 'points' must be an array");
    }

    for (const JsonValue& entry : (*points_field)->elements) {
        if (entry.kind != JsonValue::Kind::Object) {
            return fail("curve json: every point must be an object");
        }
        CurvePoint point;

        Expected<double> snr = member_double(entry, "snr_db");
        if (!snr) {
            return std::unexpected(with_context(snr.error(), "curve json point"));
        }
        point.snr_db = *snr;

        Expected<double> ber = member_double(entry, "ber");
        if (!ber) {
            return std::unexpected(with_context(ber.error(), "curve json point"));
        }
        point.ber = *ber;

        const UintField point_fields[] = {
            {"bit_errors", &point.bit_errors},
            {"bits_total", &point.bits_total},
            {"decoded_count", &point.decoded_count},
            {"trials", &point.trials},
        };
        for (const UintField& field : point_fields) {
            Expected<std::uint64_t> value = member_uint(entry, field.key);
            if (!value) {
                return std::unexpected(with_context(value.error(), "curve json point"));
            }
            *field.target = *value;
        }

        curve.points.push_back(point);
    }

    return curve;
}

Status write_curve_file(std::string_view path, const Curve& curve) {
    const std::string filename(path);
    std::ofstream out(filename, std::ios::binary | std::ios::trunc);
    if (!out) {
        return fail(std::format("cannot open '{}' for writing", filename));
    }
    const std::string text = curve_to_json(curve);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out) {
        return fail(std::format("failed while writing '{}'", filename));
    }
    return {};
}

Expected<Curve> read_curve_file(std::string_view path) {
    const std::string filename(path);
    std::ifstream in(filename, std::ios::binary);
    if (!in) {
        return fail(std::format("cannot open '{}' for reading", filename));
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad()) {
        return fail(std::format("failed while reading '{}'", filename));
    }
    Expected<Curve> curve = curve_from_json(text);
    if (!curve) {
        return std::unexpected(with_context(curve.error(), filename));
    }
    return curve;
}

double sensitivity_db(const std::vector<CurvePoint>& points, double ber_threshold) {
    if (points.size() < 2 || !(ber_threshold > 0.0)) {
        return kQuietNan;
    }

    std::vector<CurvePoint> sorted = points;
    std::sort(sorted.begin(), sorted.end(),
              [](const CurvePoint& lhs, const CurvePoint& rhs) { return lhs.snr_db < rhs.snr_db; });

    for (std::size_t index = 0; index + 1 < sorted.size(); ++index) {
        const CurvePoint& low = sorted[index];
        const CurvePoint& high = sorted[index + 1];
        if (!(low.ber >= ber_threshold) || !(high.ber < ber_threshold)) {
            continue;
        }

        const double y_low = std::log10(measurable_ber(low));
        const double y_high = std::log10(measurable_ber(high));
        const double y_target = std::log10(ber_threshold);
        if (!(y_low > y_high)) {
            // Flat or inverted across the crossing: there is nothing to
            // interpolate, so report the first point that met the threshold.
            return high.snr_db;
        }
        const double fraction = (y_low - y_target) / (y_low - y_high);
        return low.snr_db + fraction * (high.snr_db - low.snr_db);
    }

    return kQuietNan;
}

Expected<CurveComparison> compare_curves(const Curve& reference,
                                         const Curve& candidate,
                                         const RegressionOptions& options) {
    if (!(options.ber_threshold > 0.0) || !(options.ber_threshold < 1.0)) {
        return fail("ber_threshold must lie strictly between zero and one");
    }
    if (!(options.ber_ratio_tolerance >= 1.0)) {
        return fail("ber_ratio_tolerance must be at least one");
    }
    if (!(options.sensitivity_tolerance_db >= 0.0)) {
        return fail("sensitivity_tolerance_db must not be negative");
    }

    CurveComparison result;

    if (reference.mode != candidate.mode) {
        result.notes.push_back(
            std::format("mode differs: reference '{}', candidate '{}'", reference.mode, candidate.mode));
    }
    if (reference.subject != candidate.subject) {
        result.notes.push_back(std::format("subject differs: reference '{}', candidate '{}'",
                                           reference.subject, candidate.subject));
    }
    if (reference.config.base_seed != candidate.config.base_seed) {
        result.notes.push_back(std::format("base seed differs: reference {}, candidate {}",
                                           reference.config.base_seed, candidate.config.base_seed));
    }

    for (const CurvePoint& reference_point : reference.points) {
        const CurvePoint* candidate_point = find_point(candidate.points, reference_point.snr_db);
        if (candidate_point == nullptr) {
            result.notes.push_back(
                std::format("{:.3f} dB is in the reference and absent from the candidate", reference_point.snr_db));
            continue;
        }

        PointComparison comparison;
        comparison.snr_db = reference_point.snr_db;
        comparison.reference_ber = reference_point.ber;
        comparison.candidate_ber = candidate_point->ber;
        comparison.reference_bit_errors = reference_point.bit_errors;
        comparison.candidate_bit_errors = candidate_point->bit_errors;

        // Three errors' worth of counting noise on top of the ratio. A point
        // where the reference saw none must not fail on one unlucky error.
        const auto measured_bits = static_cast<double>(std::max<std::uint64_t>(candidate_point->bits_total, 1));
        const double noise_floor = 3.0 / measured_bits;
        comparison.worsened =
            comparison.candidate_ber > comparison.reference_ber * options.ber_ratio_tolerance + noise_floor;
        if (comparison.worsened) {
            ++result.worsened_points;
        }
        result.points.push_back(comparison);
    }

    for (const CurvePoint& candidate_point : candidate.points) {
        if (find_point(reference.points, candidate_point.snr_db) == nullptr) {
            result.notes.push_back(
                std::format("{:.3f} dB is in the candidate and absent from the reference", candidate_point.snr_db));
        }
    }

    result.reference_sensitivity_db = sensitivity_db(reference.points, options.ber_threshold);
    result.candidate_sensitivity_db = sensitivity_db(candidate.points, options.ber_threshold);
    const bool reference_crosses = !std::isnan(result.reference_sensitivity_db);
    const bool candidate_crosses = !std::isnan(result.candidate_sensitivity_db);

    result.regressed = result.worsened_points > 0;

    if (reference_crosses && candidate_crosses) {
        result.sensitivity_delta_db = result.candidate_sensitivity_db - result.reference_sensitivity_db;
        result.has_sensitivity_delta = true;
        if (result.sensitivity_delta_db > options.sensitivity_tolerance_db) {
            result.regressed = true;
        }
    } else if (reference_crosses && !candidate_crosses) {
        result.notes.push_back(std::format("the candidate never reaches a BER of {:g} inside its swept range",
                                           options.ber_threshold));
        result.regressed = true;
    } else if (!reference_crosses && candidate_crosses) {
        result.notes.push_back(std::format(
            "the reference never reached a BER of {:g}, so there is no sensitivity to compare against",
            options.ber_threshold));
    } else {
        result.notes.push_back(
            std::format("neither curve crosses a BER of {:g}, so the swept range is the wrong one for this threshold",
                        options.ber_threshold));
    }

    return result;
}

}  // namespace revenant::bench
