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
// span_hz f64, width_px u32, px[width_px], then the trace: trace_offset_db
// f32, trace_scale_db f32, trace_px[width_px].
//
// px is the waterfall row, quantised to the desktop waterfall's ends, and
// trace_px the spectrum trace, smoothed and quantised to the desktop
// spectrum's ends (display_sync.h); each decodes as offset + px * scale. A
// frame that stops after px has no trace of its own and draws px.
inline constexpr std::size_t kSpectrumHeaderSize = 1 + 4 + 4 + 8 + 8 + 8 + 4;
inline constexpr std::size_t kSpectrumTraceHeaderSize = 4 + 4;

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

// Bounds on a string list argument (setDecoders names). The engine has a few
// dozen decoders with short names, so these only stop nonsense.
inline constexpr std::size_t kMaxListItems = 64;
inline constexpr std::size_t kMaxListItemBytes = 64;

// The gateway sends a "ping" event this often; the app treats a socket silent
// for two of these as dead. web/src/protocol.ts carries the same number.
inline constexpr std::int64_t kPingIntervalMs = 15'000;

// The operations a phone may request, exactly these.
inline constexpr std::string_view kOps[] = {
    "addVrx",       "removeVrx",     "tune",        "setMode",
    "setBandwidth", "setSquelch",    "setCenter",   "setGain",
    "setGainAuto",  "listSources",   "openSource",  "setView",
    "audio",        "keepListening", "setTranscription", "setVrxTranscribe",
    "setDetection", "resume",       "pong",        "listDecoders",
    "setDecoders",
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
// unthrottled). It goes out when the stream, the centre or the pace changes, or the factor by
// kRealtimeReportStep or more changes, so a source falling behind real time
// reaches the phone within the one second poll. SourceEvent in
// web/src/protocol.ts is the phone's copy.
//
// "transcriptionStatus" is the engine's speech state, the fields of the
// setTranscription result: {enabled, modelState, modelName, bytesDone,
// bytesTotal, detail, backend, receivers, queued, transcribed, dropped}. It is
// sent on connect and on resume, and when the switch, the model state, its
// name or the detail change or the download moves a whole percent (the one
// second poll, transcription_report_due in dispatch.h). Counters alone do not
// send one.
//
// "detections" is {items:[{id, centerHz, bandwidthHz, snrDb, confidence,
// state, label, labelConfidence, lastSeen}]}, the engine's live and held
// tracks near the phone's view, sent each poll while a view is set. id is the
// track id as a decimal string (a UInt64 does not survive a JSON number);
// state is "live" or "held"; label is the classifier's name for it or empty,
// and labelKind says what that name is: "modulation" (analog or digital
// family) or "protocol". A protocol label is kept because it is the more
// specific answer, and the phone shows whichever the engine settled on.
//
// "display" is the desktop's colours for the phone's waterfall and trace,
// {waterfall, spectrum, trace, background}; display_sync.h has the shape. It
// is sent on connect, on resume and when the desktop's settings change them.
inline constexpr std::string_view kEvents[] = {
    "vrxStatus", "decoded", "transcript", "transcriptionStatus",
    "source",    "link",    "vrxRemoved", "hls",
    "hello",     "ping",    "vrxEventsEnded", "detections",
    "display",
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
    // An array of strings, each at most kMaxListItemBytes, at most
    // kMaxListItems of them.
    [[nodiscard]] Expected<std::vector<std::string>> get_string_list(std::string_view key) const;
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

// Whether the pacing part of a "source" event has changed enough to send: the
// factor by realtime_report_due, or any change of pace, which alters how the
// factor reads (0.5 is a fault unpaced and the setting at pace 0.5).
[[nodiscard]] bool pacing_report_due(double sent_factor, double sent_pace, double factor, double pace);

}  // namespace revenant::webgate
