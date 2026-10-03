// Speech to text, the client's half of it: what the top bar says beside the
// switch, which receivers a choice takes in, what one transcript says in the
// decode log and on the hover card, and which per-receiver choices still have
// to reach the engine.
//
// Qt-free on the rule models/decoded_log.h follows, so ui/tests can hold it.
// models/transcribe_link.cpp is the wire half, render/caption_layout.h puts
// the text on the span waterfall, and the contract is the schema's "SPEECH TO
// TEXT" block in core/rpc/revenant.capnp.
//
// THE OWNER'S DECISIONS OF 2026-10-03, which this file encodes and does not
// re-decide. One global switch, independent of the rack's auto DV switch,
// which only opens receivers. While it is on the engine transcribes every
// receiver that makes speech: digital voice by call, analogue by squelch, wfm
// left out unless a receiver is chosen in. Per receiver the choice is auto
// (follow that rule), on or off. The recogniser runs in the engine, and its
// model is downloaded there the first time the switch goes on, so this window
// shows the download and any failure rather than doing either.
//
// THE ENGINE IS THE AUTHORITY on what is transcribed. VrxStatus.transcribing
// says what it is doing; the mode rule below is only ever used to explain a
// choice in words, never to decide what the window draws as transcribing.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/rpc/types.h"
#include "models/decoded_log.h"

namespace revenant::ui {

// ---------------------------------------------------------------------------
// The per-receiver choice
// ---------------------------------------------------------------------------

// In rpc::TranscribeChoice's ordinal order, which mirrors the schema's, so a
// name and an enumerator cannot drift apart. The segmented control in
// qml/ReceiverDetail.qml shows them in this order.
inline constexpr std::array<std::string_view, 3> kTranscribeChoiceNames = {"auto", "on", "off"};

static_assert(static_cast<std::size_t>(rpc::TranscribeChoice::Off) + 1 ==
                  kTranscribeChoiceNames.size(),
              "a choice was added to rpc::TranscribeChoice without a name here");

[[nodiscard]] constexpr std::string_view transcribe_choice_name(rpc::TranscribeChoice choice)
{
    const auto index = static_cast<std::size_t>(choice);
    return index < kTranscribeChoiceNames.size() ? kTranscribeChoiceNames[index]
                                                 : kTranscribeChoiceNames[0];
}

// Nothing rather than auto for a name that is not one of the three, on the
// argument demod_from_name makes: a choice nobody meant is a receiver taken in
// or left out without anybody asking.
[[nodiscard]] constexpr std::optional<rpc::TranscribeChoice> transcribe_choice_from_name(
    std::string_view name)
{
    for (std::size_t i = 0; i < kTranscribeChoiceNames.size(); ++i) {
        if (kTranscribeChoiceNames[i] == name) {
            return static_cast<rpc::TranscribeChoice>(i);
        }
    }
    return std::nullopt;
}

// Whether a receiver in `mode` produces anything the recogniser can hear.
// The schema: "Modes with no speech, raw, tetra and cw, are never
// transcribed." raw is complex baseband, a TETRA receiver makes no audio
// (docs/ui-spectrum.md, "Auto DV"), and Whisper writes words for Morse that
// nobody keyed.
[[nodiscard]] constexpr bool mode_makes_speech(std::string_view mode)
{
    return mode != "raw" && mode != "tetra" && mode != "cw";
}

// Whether the engine's rule takes a receiver in, as the schema states it:
// auto takes every mode that makes speech except wfm, because broadcast FM is
// mostly music and Whisper writes lyrics nobody sang; on takes a wfm talk
// station in as well; off leaves it out. Used to explain a choice in words,
// not to decide anything; see the note at the top.
[[nodiscard]] constexpr bool choice_takes_in(rpc::TranscribeChoice choice, std::string_view mode)
{
    if (!mode_makes_speech(mode)) {
        return false;
    }
    switch (choice) {
        case rpc::TranscribeChoice::Auto: return mode != "wfm";
        case rpc::TranscribeChoice::On: return true;
        case rpc::TranscribeChoice::Off: return false;
    }
    return false;
}

// The word or two beside the focused receiver's choice, and empty when the
// control already says everything. The engine's own flag outranks the rule:
// a receiver it reports as transcribing is "transcribing" whatever this file
// would have predicted.
[[nodiscard]] inline std::string receiver_transcribe_note(rpc::TranscribeChoice choice,
                                                          std::string_view mode,
                                                          bool switch_on, bool transcribing)
{
    if (transcribing) {
        return "transcribing";
    }
    if (!mode_makes_speech(mode)) {
        return std::format("no speech on {}", mode);
    }
    if (choice == rpc::TranscribeChoice::Off) {
        return {};
    }
    if (!switch_on) {
        // Said only of an explicit on, which is a request the operator will
        // wonder about. Auto under a switch that is off is every receiver in
        // the rack, and the top bar already says the switch is off once.
        return choice == rpc::TranscribeChoice::On ? "switch is off" : std::string();
    }
    if (choice == rpc::TranscribeChoice::Auto && mode == "wfm") {
        return "auto leaves wfm out";
    }
    // Taken in and not transcribing yet: an analogue receiver waits for its
    // squelch and a digital one for a call, so this is the ordinary quiet.
    return "waiting for speech";
}

// ---------------------------------------------------------------------------
// The per-receiver choices, reconciled against what this connection sent
// ---------------------------------------------------------------------------

struct VrxTranscribeWant {
    std::uint64_t vrx = 0;
    rpc::TranscribeChoice choice = rpc::TranscribeChoice::Auto;

    friend bool operator==(const VrxTranscribeWant&, const VrxTranscribeWant&) = default;
};

// What this connection has told the engine about each receiver id.
//
// RECONCILED, NOT COMMANDED, the way the audio and the decoders are and for
// their reason: a receiver the window rebuilds for a mode change or a
// reconnect comes back under a new id, and the engine forgets a choice when
// the receiver it was set on goes (the schema's setVrxTranscribe). So the
// window keeps each choice against its rack entry, hands the supervisor the
// whole list on every change, and this decides what has to be sent.
//
// AUTO IS NEVER SENT TO A RECEIVER THAT WAS NEVER TOLD ANYTHING ELSE. It is
// what the engine assumes, so sending it would be a round trip per receiver
// per connection for nothing, and on a receiver another session chose for it
// would quietly undo that session's choice without this operator asking.
class VrxTranscribeSent {
public:
    // The wants that differ from what was sent, in the order given.
    [[nodiscard]] std::vector<VrxTranscribeWant> plan(
        const std::vector<VrxTranscribeWant>& wants) const
    {
        std::vector<VrxTranscribeWant> out;
        for (const VrxTranscribeWant& want : wants) {
            if (want.vrx == 0) {
                continue;
            }
            if (sent_choice(want.vrx) != want.choice) {
                out.push_back(want);
            }
        }
        return out;
    }

    // Recorded whether the engine took it or refused it. A refusal is about a
    // receiver that does not exist, which the next rebuild replaces under a
    // new id; asking the same id again every pass would only be refused again.
    void note(const VrxTranscribeWant& sent)
    {
        for (VrxTranscribeWant& known : sent_) {
            if (known.vrx == sent.vrx) {
                known.choice = sent.choice;
                return;
            }
        }
        sent_.push_back(sent);
    }

    // Drops the ids no longer wanted, so the list is as long as the rack and
    // not as long as every id the session ever held.
    void prune(const std::vector<VrxTranscribeWant>& wants)
    {
        std::erase_if(sent_, [&wants](const VrxTranscribeWant& known) {
            return std::ranges::none_of(
                wants, [&known](const VrxTranscribeWant& want) { return want.vrx == known.vrx; });
        });
    }

    // A new connection: the engine behind it has been told nothing.
    void forget() { sent_.clear(); }

    [[nodiscard]] rpc::TranscribeChoice sent_choice(std::uint64_t vrx) const
    {
        for (const VrxTranscribeWant& known : sent_) {
            if (known.vrx == vrx) {
                return known.choice;
            }
        }
        return rpc::TranscribeChoice::Auto;
    }

private:
    std::vector<VrxTranscribeWant> sent_;
};

// ---------------------------------------------------------------------------
// What the top bar says beside the switch
// ---------------------------------------------------------------------------

// How loudly the label is drawn. Quiet is dim ink, Busy the accent while the
// model is being fetched or loaded, Bad a failure, Off an engine that does
// not transcribe at all.
enum class TranscriptionTone : std::uint8_t { Quiet, Busy, Bad, Off };

[[nodiscard]] constexpr std::string_view transcription_tone_name(TranscriptionTone tone)
{
    switch (tone) {
        case TranscriptionTone::Quiet: return "quiet";
        case TranscriptionTone::Busy: return "busy";
        case TranscriptionTone::Bad: return "bad";
        case TranscriptionTone::Off: return "off";
    }
    return "quiet";
}

// Everything the label is made from, gathered by the supervisor.
struct TranscriptionFacts {
    bool connected = false;

    // A status has been read on this connection.
    bool known = false;
    rpc::TranscriptionStatus status;

    // The engine refused a speech to text call as unimplemented, which is an
    // engine older than the schema's 2026-10-03 block. Not a fault: the
    // switch says the engine does not transcribe and nothing else happens.
    bool unsupported = false;
    std::string unsupported_reason;

    // The last call failed for any other reason, in the engine's words, and
    // the transcript stream's end when the engine ended it. Empty when not.
    std::string fault;
    std::string stream_ended;

    // The operator's switch, which is what the window shows until the engine
    // has said what it holds.
    bool wanted = false;

    // Field by field, because rpc::TranscriptionStatus is a mirror of the
    // schema and declares no comparison of its own, and core/rpc is not this
    // lane's to change. A field added there and not here would make two
    // statuses that differ in it compare equal, so the supervisor would not
    // hand the new one over; every field the view reads is listed.
    friend bool operator==(const TranscriptionFacts& a, const TranscriptionFacts& b)
    {
        const rpc::TranscriptionStatus& s = a.status;
        const rpc::TranscriptionStatus& t = b.status;
        return a.connected == b.connected && a.known == b.known &&
               a.unsupported == b.unsupported && a.unsupported_reason == b.unsupported_reason &&
               a.fault == b.fault && a.stream_ended == b.stream_ended && a.wanted == b.wanted &&
               s.enabled == t.enabled && s.model_state == t.model_state &&
               s.model_name == t.model_name && s.bytes_done == t.bytes_done &&
               s.bytes_total == t.bytes_total && s.detail == t.detail &&
               s.backend == t.backend && s.receivers == t.receivers && s.queued == t.queued &&
               s.transcribed == t.transcribed && s.dropped == t.dropped &&
               s.rejected == t.rejected && s.last_latency_ms == t.last_latency_ms;
    }
};

struct TranscriptionView {
    // Whether the switch is shown checked: the engine's switch once it has
    // said, the operator's until then. The switch is engine-wide, so another
    // session turning it on is drawn as on here.
    bool on = false;

    // Whether the switch can be used at all.
    bool offered = false;

    // The word or two beside the switch, empty when there is nothing to say,
    // and the sentence behind it.
    std::string label;
    std::string detail;
    TranscriptionTone tone = TranscriptionTone::Quiet;

    friend bool operator==(const TranscriptionView&, const TranscriptionView&) = default;
};

// Whole megabytes, for a download whose size the engine has not said.
[[nodiscard]] inline std::string megabytes_text(std::uint64_t bytes)
{
    return std::format("{} MB", bytes / 1'000'000);
}

// The download as a percentage, floored, so "100%" is only ever said of a
// download that has finished. Zero for an unknown total.
[[nodiscard]] constexpr int download_percent(std::uint64_t done, std::uint64_t total)
{
    if (total == 0) {
        return 0;
    }
    const std::uint64_t capped = std::min(done, total);
    return static_cast<int>(capped * 100 / total);
}

namespace transcription_detail {

[[nodiscard]] inline std::string model_name_or(const rpc::TranscriptionStatus& status)
{
    return status.model_name.empty() ? std::string("the speech model") : status.model_name;
}

// The counters, for the tooltip. Each is named for what it counts, because
// "dropped" and "rejected" are two different losses with two different fixes.
[[nodiscard]] inline std::string counters_text(const rpc::TranscriptionStatus& status)
{
    std::string out = std::format(
        "{} receiver{} being transcribed, {} utterance{} waiting for the recogniser. Since the "
        "engine started: {} transcribed, {} dropped because the queue was full, {} heard and "
        "judged not to be speech.",
        status.receivers, status.receivers == 1 ? "" : "s", status.queued,
        status.queued == 1 ? "" : "s", status.transcribed, status.dropped, status.rejected);
    if (status.last_latency_ms > 0.0) {
        out += std::format(" The last was ready {:.1f} s after the speech ended.",
                           status.last_latency_ms / 1000.0);
    }
    return out;
}

}  // namespace transcription_detail

[[nodiscard]] inline TranscriptionView transcription_view(const TranscriptionFacts& facts)
{
    using rpc::TranscriptionModelState;
    namespace detail = transcription_detail;

    TranscriptionView view;
    if (!facts.connected) {
        // The bar hides the switch with no engine; what it would show is the
        // operator's own setting, which is applied on the next connection.
        view.on = facts.wanted;
        return view;
    }
    if (facts.unsupported) {
        view.label = "not offered";
        view.tone = TranscriptionTone::Off;
        view.detail = "This engine does not transcribe";
        if (!facts.unsupported_reason.empty()) {
            view.detail += ": " + facts.unsupported_reason;
        } else {
            view.detail += ".";
        }
        return view;
    }

    view.offered = true;
    if (!facts.known) {
        view.on = facts.wanted;
        if (!facts.fault.empty()) {
            view.label = "no status";
            view.tone = TranscriptionTone::Bad;
            view.detail = facts.fault;
        }
        return view;
    }

    const rpc::TranscriptionStatus& status = facts.status;
    view.on = status.enabled;
    const std::string model = detail::model_name_or(status);

    switch (status.model_state) {
        case TranscriptionModelState::Downloading:
            // Said whether or not the switch is still on: a download the
            // engine is carrying on with is a gigabyte and a half on the
            // operator's line either way.
            view.tone = TranscriptionTone::Busy;
            if (status.bytes_total > 0) {
                view.label = std::format(
                    "model {}%", download_percent(status.bytes_done, status.bytes_total));
                view.detail = std::format(
                    "Downloading {}: {} of {}. The engine fetches it once, the first time the "
                    "switch goes on, and keeps it.",
                    model, megabytes_text(status.bytes_done), megabytes_text(status.bytes_total));
            } else {
                view.label = "model " + megabytes_text(status.bytes_done);
                view.detail = std::format(
                    "Downloading {}: {} so far, of a size the engine has not said.", model,
                    megabytes_text(status.bytes_done));
            }
            break;
        case TranscriptionModelState::Verifying:
            view.tone = TranscriptionTone::Busy;
            view.label = "checking model";
            view.detail = std::format("Verifying {} now that it is downloaded.", model);
            break;
        case TranscriptionModelState::Loading:
            view.tone = TranscriptionTone::Busy;
            view.label = "loading model";
            view.detail = std::format("Loading {} into the recogniser.", model);
            break;
        case TranscriptionModelState::Failed:
            view.tone = TranscriptionTone::Bad;
            view.label = "model failed";
            view.detail = status.detail.empty()
                              ? std::string("The engine could not get its speech model ready "
                                            "and gave no reason.")
                              : status.detail;
            view.detail += " Turning the switch off and on again retries.";
            break;
        case TranscriptionModelState::Absent:
            if (status.enabled) {
                view.tone = TranscriptionTone::Busy;
                view.label = "starting";
                view.detail = "The switch is on and the engine has not started on its model yet.";
            }
            break;
        case TranscriptionModelState::Ready:
            if (status.enabled) {
                // The queue depth only when there is one, because a number
                // that climbs is a recogniser falling behind the band, and a
                // zero beside the switch all day says nothing.
                view.label = status.queued > 0 ? std::format("{} queued", status.queued)
                                               : std::string("ready");
                view.detail = std::format("{} is loaded{}. ", model,
                                          status.backend.empty() ? std::string()
                                                                 : " on " + status.backend) +
                              detail::counters_text(status);
            }
            break;
    }

    // The engine's own sentence about what it is waiting on, after ours,
    // when it has one and it is not already the whole of a failure.
    if (!status.detail.empty() && status.model_state != TranscriptionModelState::Failed &&
        !view.detail.empty()) {
        view.detail += " " + status.detail;
    }
    if (!facts.stream_ended.empty() && status.enabled) {
        view.tone = TranscriptionTone::Bad;
        view.label = "stream ended";
        view.detail = "The engine ended the transcript stream: " + facts.stream_ended +
                      " Turning the switch off and on again asks for it again.";
    }
    return view;
}

// ---------------------------------------------------------------------------
// A transcript as a line of the decode log, and as the hover card
// ---------------------------------------------------------------------------

// The decode log's decoder column for a transcript. Six characters, inside
// kDecoderColumn, so the text lines up with the decoders' lines around it.
inline constexpr std::string_view kTranscriptDecoder = "speech";

// Below this the line is drawn dim and carries a chip saying the recogniser
// doubted it. The schema leaves the line to the client ("A client may dim a
// line it doubts"); a half is this window's choice, and the number beside the
// chip in the expansion is the engine's.
inline constexpr float kDoubtfulConfidence = 0.5F;

// Hertz as megahertz to the hertz, six places, the way the rack strip reads.
// Integer arithmetic, so the text is exact at any frequency and a test can
// compare it; a negative frequency is not one a receiver can be on.
[[nodiscard]] inline std::string exact_megahertz_text(std::int64_t hz)
{
    const auto magnitude = static_cast<std::uint64_t>(std::max<std::int64_t>(hz, 0));
    return std::format("{}.{:06} MHz", magnitude / 1'000'000, magnitude % 1'000'000);
}

// One transcript as a line of the log. The time column is the receiver's own
// stream, end_sample / sample_rate, which is the clock every other line in
// the log reads in (models/decoded_log.h); the source clock that puts the
// text on the waterfall is in the expansion. `slot` is the receiver's rack
// colour, or -1 when it has left the rack.
[[nodiscard]] inline DecodedLine make_transcript_line(const rpc::Transcript& transcript,
                                                      std::uint64_t serial,
                                                      std::string_view arrived, int slot)
{
    DecodedLine line;
    line.serial = serial;
    line.vrx = transcript.vrx;
    line.slot = slot;
    line.time = stream_time_text(transcript.end_sample, transcript.sample_rate);
    line.decoder = std::string(kTranscriptDecoder);
    line.kind = "transcript";
    line.text = one_line(transcript.text);
    if (transcript.confidence < kDoubtfulConfidence) {
        line.chip = "unsure";
    }
    line.dropped_before = transcript.dropped_before;

    line.fields.push_back({"kind", line.kind});
    line.fields.push_back({"receiver", std::to_string(transcript.vrx)});
    line.fields.push_back({"mode", one_line(transcript.mode)});
    line.fields.push_back(
        {"passband", std::format("{} to {}", exact_megahertz_text(transcript.low_hz),
                                 exact_megahertz_text(transcript.high_hz))});
    line.fields.push_back({"samples", std::format("{} to {} at {} S/s", transcript.start_sample,
                                                  transcript.end_sample,
                                                  transcript.sample_rate)});
    line.fields.push_back({"source samples", std::format("{} to {}", transcript.source_start,
                                                         transcript.source_end)});
    line.fields.push_back({"confidence", std::format("{:.2f}", transcript.confidence)});
    line.fields.push_back({"no speech", std::format("{:.2f}", transcript.no_speech_prob)});
    line.fields.push_back({"latency", std::format("{:.0f} ms", transcript.latency_ms)});
    line.fields.push_back({"sequence", std::to_string(transcript.sequence)});
    if (!arrived.empty()) {
        line.fields.push_back({"arrived", std::string(arrived)});
    }
    for (const rpc::DecodedField& field : transcript.fields) {
        line.fields.push_back({one_line(field.key), field_value_text(field.key, field.value)});
    }
    return line;
}

// What a call's protocol said, as "talkgroup 1201, source 0x1234": the
// identifiers an operator reads a caption by, from the fields the receiver's
// decoder publishes. Empty for analogue, which has none.
[[nodiscard]] inline std::string transcript_call_text(const rpc::Transcript& transcript)
{
    std::string out;
    for (const rpc::DecodedField& field : transcript.fields) {
        if (field.key != "talkgroup" && field.key != "source" && field.key != "destination") {
            continue;
        }
        if (!out.empty()) {
            out += ", ";
        }
        out += one_line(field.key) + " " + field_value_text(field.key, field.value);
    }
    return out;
}

// The hover card over a caption: who and where on the first line, the whole
// text, then how sure and how late. `receiver_label` is the rack's name for
// the receiver, "RX 3", or empty when it has left the rack.
[[nodiscard]] inline std::string transcript_card_text(const rpc::Transcript& transcript,
                                                      std::string_view receiver_label)
{
    std::string head = receiver_label.empty() ? std::format("receiver {}", transcript.vrx)
                                              : std::string(receiver_label);
    head += "  ·  " + one_line(transcript.mode);
    head += "  ·  " + exact_megahertz_text(transcript.center_hz);
    if (const std::string call = transcript_call_text(transcript); !call.empty()) {
        head += "  ·  " + call;
    }
    std::string out = head + "\n" + one_line(transcript.text) + "\n";
    out += std::format("confidence {:.2f}  ·  ready {:.1f} s after the speech ended",
                       transcript.confidence, transcript.latency_ms / 1000.0);
    return out;
}

}  // namespace revenant::ui
