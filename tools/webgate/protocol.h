// The webgate wire format: what travels on the one WebSocket between the
// gateway and the phone's web app.
//
// Text frames carry JSON: commands from the phone, replies and events from
// here. Binary frames carry spectrum rows and Opus audio, little-endian, the
// first byte a Tag. web/src/protocol.ts is the other half of this file and the
// two change together.
//
// JSON goes through capnp's JsonValue and JsonCodec, already in the capnproto
// package, so the dependency list stays closed.

#pragma once

#include <capnp/compat/json.h>
#include <capnp/message.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/error.h"
#include "tools/webgate/spectrum_reducer.h"

namespace revenant::webgate {

// ---- Binary frames ---------------------------------------------------------

enum class Tag : std::uint8_t { Spectrum = 1, Opus = 2 };

// tag u8, offset_db f32, scale_db f32, source_start u64, center_hz f64,
// span_hz f64, width_px u32, px[width_px]
inline constexpr std::size_t kSpectrumHeaderSize = 1 + 4 + 4 + 8 + 8 + 8 + 4;

// tag u8, vrx u64, seq u32, packet[...]
inline constexpr std::size_t kOpusHeaderSize = 1 + 8 + 4;

// width_px on the wire is frame.px.size(), not frame.view.width_px: the pixels
// actually present are what the decoder must read, and the reducer caps both
// at kMaxWidthPx anyway. Only the view's centre and span are taken from it.
[[nodiscard]] std::vector<std::uint8_t> encode(const ReducedFrame& frame);

[[nodiscard]] std::vector<std::uint8_t> encode_opus(std::uint64_t vrx, std::uint32_t seq,
                                                    std::span<const std::uint8_t> packet);

// ---- Commands from the phone ----------------------------------------------

// Largest text frame parse_command will look at. A phone sends short control
// messages; anything bigger is a bug or an attack.
inline constexpr std::size_t kMaxCommandBytes = 64 * 1024;

// The gateway sends a "ping" event this often; the app treats a socket silent
// for two of these as dead. web/src/protocol.ts carries the same number.
inline constexpr std::int64_t kPingIntervalMs = 15'000;

// The operations a phone may request, exactly these.
inline constexpr std::string_view kOps[] = {
    "addVrx",       "removeVrx",     "tune",        "setMode",
    "setBandwidth", "setSquelch",    "setCenter",   "setGain",
    "setGainAuto",  "listSources",   "openSource",  "setView",
    "audio",        "keepListening", "setTranscription", "setVrxTranscribe",
    "resume",       "pong",
};

// The events the gateway sends, exactly these.
//
// "link" is {state, detail}. After an engine restart, the Up event of each
// session the gateway rebuilt also carries "recreated":[{"old":a,"new":b}]
// and "lost":[{"vrx":a,"reason":"..."}] (phone_hub.h); a lost id also gets a
// vrxRemoved. web/src/protocol.ts LinkEvent is the phone's copy.
//
// "source" is {open} and, while open, {rateHz, centerHz, epoch,
// realtimeFactor, pacedBy}: the engine's capture seconds per wall second over
// its recent window (0 is not measured yet) and the pace in force (0 is
// unthrottled). It goes out when the stream, the centre, or the factor by
// kRealtimeReportStep or more changes, so a source falling behind real time
// reaches the phone within the one second poll. SourceEvent in
// web/src/protocol.ts is the phone's copy.
inline constexpr std::string_view kEvents[] = {
    "vrxStatus", "decoded", "transcript", "transcriptionStatus",
    "source",    "link",    "vrxRemoved", "hls",
    "hello",     "ping",    "vrxEventsEnded",
};

[[nodiscard]] bool is_known_op(std::string_view op);

// A parsed JSON value that owns its storage. Copies share one immutable
// message (the reader points into heap segments, so moving or copying the
// holder never invalidates it). Default constructed means "no value".
class JsonArgs {
public:
    JsonArgs() = default;
    explicit JsonArgs(std::shared_ptr<capnp::MallocMessageBuilder> message);

    [[nodiscard]] bool empty() const { return message_ == nullptr; }

    // For callers that want to walk the value themselves. A default constructed
    // JsonArgs yields a null value.
    [[nodiscard]] capnp::JsonValue::Reader reader() const;

private:
    std::shared_ptr<capnp::MallocMessageBuilder> message_;
};

// Wire shape: {"id": <non-negative integer>, "op": "<name>", "args": {...}}.
// "args" is optional and defaults to an empty object; when present it must be
// an object. Every other member is ignored.
//
// The getters read args by key and return an error naming the key when it is
// absent or the wrong type, which the dispatcher can send back as the reply.
// get_u64 accepts only whole numbers from 0 to 2^53, the largest range a JSON
// number carries exactly.
struct Command {
    std::uint64_t id = 0;
    std::string op;
    JsonArgs args;

    [[nodiscard]] bool has(std::string_view key) const;
    [[nodiscard]] Expected<std::uint64_t> get_u64(std::string_view key) const;
    [[nodiscard]] Expected<std::int64_t> get_i64(std::string_view key) const;
    [[nodiscard]] Expected<double> get_double(std::string_view key) const;
    [[nodiscard]] Expected<std::string> get_string(std::string_view key) const;
    [[nodiscard]] Expected<bool> get_bool(std::string_view key) const;
};

// Never throws. Refuses text over kMaxCommandBytes, malformed JSON, a value
// that is not an object, a missing or non-integer id, a missing or non-string
// op, an unknown op (the error names it), and a non-object args.
[[nodiscard]] Expected<Command> parse_command(std::string_view text);

// The id of a command that failed parse_command, when it had a usable one, so
// the refusal can be sent as a reply the phone can match. Same limits as
// parse_command; nullopt on anything it would refuse for the id itself.
[[nodiscard]] std::optional<std::uint64_t> peek_id(std::string_view text);

// ---- Replies and events from the gateway ----------------------------------

// A JSON object under construction. Values are appended as finished JSON
// text; the typed adders escape and format for you. add_raw takes text that is
// already valid JSON (a result built elsewhere) and does not check it.
// Non-finite doubles are written as null, since JSON has no spelling for them.
// The key "event" is reserved for event() and silently dropped.
class JsonObject {
public:
    JsonObject& add_null(std::string_view key);
    JsonObject& add_bool(std::string_view key, bool value);
    JsonObject& add_u64(std::string_view key, std::uint64_t value);
    JsonObject& add_i64(std::string_view key, std::int64_t value);
    JsonObject& add_double(std::string_view key, double value);
    JsonObject& add_string(std::string_view key, std::string_view value);
    JsonObject& add_raw(std::string_view key, std::string_view json);

    [[nodiscard]] std::string str() const;

private:
    std::string body_;
};

// A JSON string literal with quotes. Control characters are escaped and bytes
// that are not valid UTF-8 become U+FFFD, because a WebSocket text frame must
// be UTF-8 and a browser drops the connection on one bad byte.
[[nodiscard]] std::string json_string(std::string_view text);

// A JSON number for a double: shortest text that round-trips, "null" when not
// finite (JSON has no spelling for NaN or infinity).
[[nodiscard]] std::string json_number(double value);

// {"id":n,"ok":true,"result":<result_json>}. result_json must be valid JSON.
[[nodiscard]] std::string ok_reply(std::uint64_t id, std::string_view result_json = "null");

// {"id":n,"ok":false,"error":"<text>"}. The engine's text goes through
// unchanged apart from JSON escaping.
[[nodiscard]] std::string error_reply(std::uint64_t id, std::string_view text);

// {"event":"<name>", ...fields}.
[[nodiscard]] std::string event(std::string_view name, const JsonObject& fields = {});

// The smallest move in the realtime factor that sends a new "source" event.
// Finer than the phone's 0.98 threshold needs, coarse enough that a factor
// wandering in its third decimal does not send an event every second.
inline constexpr double kRealtimeReportStep = 0.01;

// Whether a factor of now is worth an event after last_sent went out. A
// change between measured and not measured (zero) always is.
[[nodiscard]] bool realtime_report_due(double last_sent, double now);

}  // namespace revenant::webgate
