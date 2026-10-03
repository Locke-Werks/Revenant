// Speech to text, the client's half, models/transcription.h: the choice's
// names and the engine's rule as the window explains it, which per-receiver
// choices still have to be sent, what the top bar says beside the switch, and
// what a transcript says in the decode log and on the hover card.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "core/rpc/types.h"
#include "models/decoded_log.h"
#include "models/transcription.h"

using revenant::rpc::TranscribeChoice;
using revenant::rpc::Transcript;
using revenant::rpc::TranscriptionModelState;
using revenant::ui::choice_takes_in;
using revenant::ui::decoded_line_text;
using revenant::ui::download_percent;
using revenant::ui::make_transcript_line;
using revenant::ui::exact_megahertz_text;
using revenant::ui::mode_makes_speech;
using revenant::ui::receiver_transcribe_note;
using revenant::ui::transcribe_choice_from_name;
using revenant::ui::transcribe_choice_name;
using revenant::ui::transcript_card_text;
using revenant::ui::TranscriptionFacts;
using revenant::ui::TranscriptionTone;
using revenant::ui::transcription_view;
using revenant::ui::VrxTranscribeSent;
using revenant::ui::VrxTranscribeWant;

namespace {

[[nodiscard]] TranscriptionFacts ready_facts()
{
    TranscriptionFacts facts;
    facts.connected = true;
    facts.known = true;
    facts.status.enabled = true;
    facts.status.model_state = TranscriptionModelState::Ready;
    facts.status.model_name = "ggml-large-v3-turbo";
    facts.status.backend = "Vulkan0: NVIDIA GeForce RTX 4090";
    facts.status.receivers = 3;
    return facts;
}

[[nodiscard]] Transcript p25_transcript()
{
    Transcript out;
    out.vrx = 7;
    out.sequence = 41;
    out.source_start = 20'000'000;
    out.source_end = 60'000'000;
    out.start_sample = 16'000;
    out.end_sample = 48'000;
    out.sample_rate = 8000;
    out.center_hz = 462'562'500;
    out.low_hz = 462'556'250;
    out.high_hz = 462'568'750;
    out.mode = "p25p1";
    out.text = "engine two on scene\r\nstaging north";
    out.confidence = 0.82F;
    out.no_speech_prob = 0.04F;
    out.latency_ms = 1900.0;
    out.fields.push_back(revenant::rpc::DecodedField{"talkgroup", std::int64_t{1201}});
    return out;
}

}  // namespace

// Rejects reading a name nobody meant as auto, which would take a receiver in
// or leave it out without anybody asking, and rejects a table that drifts
// from the enum's order.
TEST_CASE("the three choices round-trip by name and nothing else is one")
{
    for (const TranscribeChoice choice :
         {TranscribeChoice::Auto, TranscribeChoice::On, TranscribeChoice::Off}) {
        const auto back = transcribe_choice_from_name(transcribe_choice_name(choice));
        REQUIRE(back.has_value());
        CHECK(*back == choice);
    }
    CHECK(transcribe_choice_name(TranscribeChoice::Off) == "off");
    CHECK_FALSE(transcribe_choice_from_name("Auto").has_value());
    CHECK_FALSE(transcribe_choice_from_name("").has_value());
}

// Rejects a rule that leaves wfm in on auto, which captions song lyrics
// nobody sang, or that keeps it out when it was chosen in, and rejects
// taking in the modes with no speech whatever is asked.
TEST_CASE("auto leaves wfm out, on takes it in, and modes with no speech stay out")
{
    CHECK(choice_takes_in(TranscribeChoice::Auto, "nfm"));
    CHECK(choice_takes_in(TranscribeChoice::Auto, "p25p1"));
    CHECK(choice_takes_in(TranscribeChoice::Auto, "usb"));
    CHECK_FALSE(choice_takes_in(TranscribeChoice::Auto, "wfm"));
    CHECK(choice_takes_in(TranscribeChoice::On, "wfm"));
    CHECK_FALSE(choice_takes_in(TranscribeChoice::Off, "nfm"));
    for (const char* mode : {"raw", "tetra", "cw"}) {
        CHECK_FALSE(mode_makes_speech(mode));
        CHECK_FALSE(choice_takes_in(TranscribeChoice::On, mode));
    }
}

// Rejects predicting over the engine: a receiver the engine says it is
// transcribing is "transcribing" even where the rule says otherwise, and the
// note says why a receiver is not, in the order an operator would look.
TEST_CASE("the note beside a receiver's choice follows the engine first")
{
    CHECK(receiver_transcribe_note(TranscribeChoice::Auto, "wfm", true, true) == "transcribing");
    CHECK(receiver_transcribe_note(TranscribeChoice::On, "cw", true, false) == "no speech on cw");
    CHECK(receiver_transcribe_note(TranscribeChoice::Off, "nfm", true, false).empty());
    CHECK(receiver_transcribe_note(TranscribeChoice::On, "nfm", false, false) == "switch is off");
    CHECK(receiver_transcribe_note(TranscribeChoice::Auto, "nfm", false, false).empty());
    CHECK(receiver_transcribe_note(TranscribeChoice::Auto, "wfm", true, false) ==
          "auto leaves wfm out");
    CHECK(receiver_transcribe_note(TranscribeChoice::Auto, "nfm", true, false) ==
          "waiting for speech");
}

// Rejects sending auto to every receiver on every connection, which would be
// a round trip each for nothing and would undo a choice another session made;
// rejects sending a choice twice; and rejects forgetting that a rebuilt
// receiver comes back under a new id the engine has told nothing.
TEST_CASE("only a choice that differs from what this connection sent is sent")
{
    VrxTranscribeSent sent;
    const std::vector<VrxTranscribeWant> fresh = {{11, TranscribeChoice::Auto},
                                                  {12, TranscribeChoice::On}};
    auto plan = sent.plan(fresh);
    REQUIRE(plan.size() == 1);
    CHECK(plan[0] == VrxTranscribeWant{12, TranscribeChoice::On});
    sent.note(plan[0]);
    CHECK(sent.plan(fresh).empty());

    // Back to auto is sent, since the engine was told on.
    const std::vector<VrxTranscribeWant> back = {{11, TranscribeChoice::Auto},
                                                 {12, TranscribeChoice::Auto}};
    plan = sent.plan(back);
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].vrx == 12);
    sent.note(plan[0]);

    // Rebuilt under id 13 with its choice on: the new id is told.
    const std::vector<VrxTranscribeWant> rebuilt = {{11, TranscribeChoice::Auto},
                                                    {13, TranscribeChoice::On}};
    sent.prune(rebuilt);
    CHECK(sent.sent_choice(12) == TranscribeChoice::Auto);
    plan = sent.plan(rebuilt);
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].vrx == 13);
    sent.note(plan[0]);

    // A new connection has been told nothing.
    sent.forget();
    plan = sent.plan(rebuilt);
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].vrx == 13);

    // A receiver with no engine id yet is not asked about.
    CHECK(sent.plan({{0, TranscribeChoice::Off}}).empty());
}

// Rejects an engine that does not transcribe reading as a fault or as off: it
// is "not offered", the switch cannot be used, and the engine's sentence is
// the reason on hover.
TEST_CASE("an engine that refuses the calls does not offer the switch")
{
    TranscriptionFacts facts;
    facts.connected = true;
    facts.unsupported = true;
    facts.unsupported_reason = "transcriptionStatus: the engine does not implement this call";
    facts.wanted = true;
    const auto view = transcription_view(facts);
    CHECK_FALSE(view.offered);
    CHECK_FALSE(view.on);
    CHECK(view.label == "not offered");
    CHECK(view.tone == TranscriptionTone::Off);
    CHECK(view.detail.find("does not implement") != std::string::npos);
}

// Rejects drawing the operator's switch over the engine's once the engine has
// said: the switch is engine-wide, and another session's on is on here too.
TEST_CASE("the switch is the operator's until the engine says, then the engine's")
{
    TranscriptionFacts facts;
    facts.connected = true;
    facts.wanted = true;
    CHECK(transcription_view(facts).on);
    CHECK(transcription_view(facts).offered);

    facts.known = true;
    facts.status.enabled = false;
    CHECK_FALSE(transcription_view(facts).on);

    facts.wanted = false;
    facts.status.enabled = true;
    CHECK(transcription_view(facts).on);
}

// Rejects a percentage that rounds up, which says "100%" of a download that
// has not finished, and one that divides by an unknown total.
TEST_CASE("the download reads as a floored percentage, or megabytes when the size is unknown")
{
    CHECK(download_percent(439, 1000) == 43);
    CHECK(download_percent(999, 1000) == 99);
    CHECK(download_percent(1000, 1000) == 100);
    CHECK(download_percent(5, 0) == 0);

    TranscriptionFacts facts = ready_facts();
    facts.status.model_state = TranscriptionModelState::Downloading;
    facts.status.bytes_done = 690'000'000;
    facts.status.bytes_total = 1'600'000'000;
    auto view = transcription_view(facts);
    CHECK(view.label == "model 43%");
    CHECK(view.tone == TranscriptionTone::Busy);
    CHECK(view.detail.find("690 MB of 1600 MB") != std::string::npos);

    facts.status.bytes_total = 0;
    view = transcription_view(facts);
    CHECK(view.label == "model 690 MB");
}

// Rejects a failure that hides the engine's sentence or does not say how to
// retry, which the schema states: turning the switch off and on again.
TEST_CASE("a failed model says so, in the engine's words, with the retry")
{
    TranscriptionFacts facts = ready_facts();
    facts.status.model_state = TranscriptionModelState::Failed;
    facts.status.detail = "the download stopped at 412 MB: connection reset";
    const auto view = transcription_view(facts);
    CHECK(view.label == "model failed");
    CHECK(view.tone == TranscriptionTone::Bad);
    CHECK(view.detail.starts_with("the download stopped at 412 MB"));
    CHECK(view.detail.find("off and on again") != std::string::npos);
}

// Rejects a queue depth of zero printed all day, and a status that does not
// say which device the recogniser runs on.
TEST_CASE("ready says ready, and the queue only when there is one")
{
    TranscriptionFacts facts = ready_facts();
    auto view = transcription_view(facts);
    CHECK(view.label == "ready");
    CHECK(view.tone == TranscriptionTone::Quiet);
    CHECK(view.detail.find("RTX 4090") != std::string::npos);
    CHECK(view.detail.find("3 receivers being transcribed") != std::string::npos);

    facts.status.queued = 4;
    view = transcription_view(facts);
    CHECK(view.label == "4 queued");

    // Off, with the model loaded from earlier: nothing beside the switch.
    facts.status.enabled = false;
    CHECK(transcription_view(facts).label.empty());
}

// Rejects the transcript stream ending in silence: the switch is on and
// nothing will arrive, which the label says until the switch goes round.
TEST_CASE("a stream the engine ended is said beside the switch")
{
    TranscriptionFacts facts = ready_facts();
    facts.stream_ended = "the recogniser stopped.";
    const auto view = transcription_view(facts);
    CHECK(view.label == "stream ended");
    CHECK(view.tone == TranscriptionTone::Bad);
    CHECK(view.detail.find("the recogniser stopped.") != std::string::npos);
}

// Rejects stamping a transcript's log line with the source clock, which is
// not the clock any other line in the log reads in, and rejects losing the
// talkgroup a call carried or letting a line break into the log.
TEST_CASE("a transcript reads in the log like a decoder line")
{
    const Transcript transcript = p25_transcript();
    const auto line = make_transcript_line(transcript, 5, "10:42:01.250", 2);
    CHECK(line.time == "00:00:06.0");
    CHECK(line.decoder == "speech");
    CHECK(line.slot == 2);
    CHECK(line.text == "engine two on scene  staging north");
    CHECK(line.chip.empty());
    CHECK(decoded_line_text(line) == "00:00:06.0  speech   engine two on scene  staging north");

    bool talkgroup = false;
    bool source = false;
    for (const auto& field : line.fields) {
        talkgroup = talkgroup || (field.key == "talkgroup" && field.value == "1201");
        source = source || (field.key == "source samples" && field.value == "20000000 to 60000000");
    }
    CHECK(talkgroup);
    CHECK(source);
}

// Rejects a doubtful transcript drawn like a sure one. The schema leaves it
// to the client to dim a line it doubts; this window does below a half.
TEST_CASE("a transcript the recogniser doubted carries a chip")
{
    Transcript transcript = p25_transcript();
    transcript.confidence = 0.31F;
    CHECK(make_transcript_line(transcript, 1, "", -1).chip == "unsure");
}

// Rejects a hover card that drops the receiver, the call or the text, which
// is the whole of what the plate could not carry.
TEST_CASE("the hover card names the receiver, the call and the whole text")
{
    const std::string card = transcript_card_text(p25_transcript(), "RX 3");
    CHECK(card.starts_with("RX 3  ·  p25p1  ·  462.562500 MHz  ·  talkgroup 1201\n"));
    CHECK(card.find("engine two on scene  staging north\n") != std::string::npos);
    CHECK(card.find("confidence 0.82") != std::string::npos);
    CHECK(card.find("1.9 s") != std::string::npos);

    // A receiver that has left the rack is named by its engine id.
    CHECK(transcript_card_text(p25_transcript(), "").starts_with("receiver 7  ·  "));
}

// Rejects a frequency printed through a double, which is the convention
// docs/conventions.md exists to stop.
TEST_CASE("frequencies read to the hertz")
{
    CHECK(exact_megahertz_text(462'562'500) == "462.562500 MHz");
    CHECK(exact_megahertz_text(7'000'001) == "7.000001 MHz");
    CHECK(exact_megahertz_text(0) == "0.000000 MHz");
}
