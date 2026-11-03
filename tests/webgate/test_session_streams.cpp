// The stream layer against the in-process engine of tests/rpc/rpc_fixture.h,
// with a fake socket that records what it was handed and can be held busy.
//
// The test thread plays the gateway's event loop: it pumps a manual strand,
// and the engine's callbacks post to that strand from the client's thread
// exactly as they would post to the kj loop.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

// gateway.h carries kj-http, which has to precede anything that pulls in
// <windows.h>.
#include "tools/webgate/gateway.h"

#include "core/rpc/client.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"
#include "tools/webgate/hls_playlist.h"
#include "tools/webgate/phone_streams.h"
#include "tools/webgate/protocol.h"
#include "tools/webgate/session.h"
#include "tests/webgate/stream_fakes.h"

namespace {

using revenant::webgate::HlsRegistry;
using revenant::webgate::HlsSegmenter;
using revenant::webgate::OutgoingMessage;
using revenant::webgate::PhoneSession;
using revenant::webgate::PhoneSocket;
using revenant::webgate::PhoneStreams;
using revenant::webgate::Strand;
using revenant::webgate::StreamsConfig;
using Kind = OutgoingMessage::Kind;
using Clock = std::chrono::steady_clock;

using revenant::webgate::test::FakeSocket;
using revenant::webgate::test::ManualStrand;

struct Rig {
    revenant::test::Harness harness;
    ManualStrand strand;
    FakeSocket socket;
    HlsRegistry registry;
    Clock::time_point fake_now = Clock::now();
    bool use_fake_clock = false;
    std::size_t max_queued_text = 2000;
    // Stands in for the engine's speech status when set.
    std::function<revenant::Expected<revenant::rpc::TranscriptionStatus>()> speech;
    // Stands in for the engine's detections when set.
    std::function<revenant::Expected<revenant::rpc::DetectionList>(double, double)> detections;
    // Stands in for the desktop's registry when set.
    std::function<revenant::ui::DisplaySettings()> display;
    std::unique_ptr<PhoneSession> session;
    std::unique_ptr<PhoneStreams> streams;

    void open(const revenant::test::HarnessOptions& options)
    {
        const auto opened = harness.open(options);
        INFO(revenant::test::message_of(opened));
        REQUIRE(opened.has_value());
    }

    void attach()
    {
        auto client = harness.connect_another();
        INFO(revenant::test::message_of(client));
        REQUIRE(client.has_value());
        session = std::make_unique<PhoneSession>(std::move(*client));
        StreamsConfig config;
        config.hls = &registry;
        if (use_fake_clock) {
            config.clock = [this] { return fake_now; };
        }
        config.max_queued_text = max_queued_text;
        config.transcription_status = speech;
        config.detections = detections;
        config.display_settings = display;
        streams =std::make_unique<PhoneStreams>(*session, &socket, strand, config);
        streams->start();
        strand.run_pending();
    }

    void command(std::string_view text)
    {
        streams->on_text(text);
        strand.run_pending();
    }

    void pump_for(std::chrono::milliseconds duration)
    {
        const auto until = Clock::now() + duration;
        while (Clock::now() < until) {
            strand.run_pending();
            streams->tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        strand.run_pending();
    }

    ~Rig()
    {
        // The streams and the session talk to the engine on the way out, so
        // they go before the harness, which the member order would not give.
        streams.reset();
        session.reset();
    }
};

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

[[nodiscard]] std::uint64_t vrx_of(const std::string& reply)
{
    const auto at = reply.find("\"vrx\":");
    REQUIRE(at != std::string::npos);
    return std::stoull(reply.substr(at + 6));
}

// The reply to command id, from the recorded text frames.
[[nodiscard]] std::string reply_to(const FakeSocket& socket, std::uint64_t id)
{
    const std::string prefix = "{\"id\":" + std::to_string(id) + ",";
    for (const auto& message : socket.sent) {
        if (message.kind == Kind::Text && message.text.rfind(prefix, 0) == 0) {
            return message.text;
        }
    }
    // Empty, which every caller's check on "ok" then reports.
    return {};
}

[[nodiscard]] std::uint32_t width_of(const OutgoingMessage& message)
{
    // tag, offset, scale, source_start, centre, span, then width_px.
    constexpr std::size_t at = 1 + 4 + 4 + 8 + 8 + 8;
    REQUIRE(message.binary.size() >= at + 4);
    std::uint32_t width = 0;
    std::memcpy(&width, message.binary.data() + at, sizeof width);
    return width;
}

[[nodiscard]] std::uint32_t seq_of(const OutgoingMessage& message)
{
    constexpr std::size_t at = 1 + 8;
    REQUIRE(message.binary.size() >= at + 4);
    std::uint32_t seq = 0;
    std::memcpy(&seq, message.binary.data() + at, sizeof seq);
    return seq;
}

// A frame the size the engine sends, centred on baseband DC.
[[nodiscard]] revenant::rpc::SpectrumFrame synthetic_frame(std::uint64_t start)
{
    revenant::rpc::SpectrumFrame frame;
    frame.geometry.bins = 4096;
    frame.geometry.bin_width = {1000, 1};
    frame.geometry.bin_zero = {-2'048'000, 1};
    frame.power_db.assign(4096, -90.0F);
    frame.power_db[2048] = -20.0F;
    frame.start = start;
    frame.count = 16384;
    frame.floor_db = -120.0F;
    frame.ceiling_db = 0.0F;
    return frame;
}

constexpr std::int64_t kCentre = 100'000'000;

[[nodiscard]] revenant::test::HarnessOptions still_engine()
{
    revenant::test::HarnessOptions o;
    o.center_hz = kCentre;
    return o;
}

}  // namespace

TEST_CASE("one spectrum frame in flight", "[gpu][webgate][streams]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    rig.open(still_engine());
    rig.attach();
    rig.command(R"({"id":1,"op":"setView","args":{"centerHz":100000000,"spanHz":2000000,"widthPx":800}})");

    rig.socket.hold();
    rig.streams->deliver_spectrum(synthetic_frame(0));
    rig.strand.run_pending();
    REQUIRE(rig.socket.count(Kind::Spectrum) == 1);
    const auto before = rig.streams->counters().spectrum_skipped;

    for (std::uint64_t i = 1; i <= 10; ++i) {
        rig.streams->deliver_spectrum(synthetic_frame(i * 16384));
        rig.strand.run_pending();
    }
    CHECK(rig.socket.count(Kind::Spectrum) == 1);

    rig.socket.release();
    rig.strand.run_pending();
    CHECK(rig.socket.count(Kind::Spectrum) == 2);
    CHECK(rig.streams->counters().spectrum_skipped - before == 9);
    CHECK(rig.streams->counters().spectrum_sent == 2);
}

TEST_CASE("rotation: next frame is cut for the new view", "[gpu][webgate][streams]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    rig.open(still_engine());
    rig.attach();

    // The only frame arrives while the view is 800 px wide and the socket is
    // busy. The view then changes and no other frame comes, so the frame sent
    // after the second reply is that one: it must have been cut when sent,
    // not when it arrived.
    rig.socket.hold();
    rig.command(R"({"id":1,"op":"setView","args":{"centerHz":100000000,"spanHz":2000000,"widthPx":800}})");
    rig.streams->deliver_spectrum(synthetic_frame(0));
    rig.strand.run_pending();
    rig.command(R"({"id":2,"op":"setView","args":{"centerHz":100000000,"spanHz":2000000,"widthPx":1500}})");
    rig.socket.release();
    rig.strand.run_pending();

    std::size_t second_reply = rig.socket.sent.size();
    for (std::size_t i = 0; i < rig.socket.sent.size(); ++i) {
        const auto& m = rig.socket.sent[i];
        if (m.kind == Kind::Text && m.text.rfind("{\"id\":2,", 0) == 0) {
            second_reply = i;
        }
    }
    REQUIRE(second_reply < rig.socket.sent.size());
    std::size_t after = 0;
    for (std::size_t i = second_reply + 1; i < rig.socket.sent.size(); ++i) {
        if (rig.socket.sent[i].kind == Kind::Spectrum) {
            ++after;
            CHECK(width_of(rig.socket.sent[i]) == 1500);
        }
    }
    CHECK(after == 1);
    CHECK(rig.socket.count(Kind::Spectrum) == 1);
}

TEST_CASE("no source open", "[gpu][webgate][streams]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    auto options = still_engine();
    options.spectrum_transform = revenant::test::kSpectrumTransform;
    rig.open(options);
    REQUIRE(rig.harness.client().close_source().has_value());
    rig.attach();
    rig.command(R"({"id":1,"op":"setView","args":{"centerHz":0,"spanHz":2000000,"widthPx":800}})");
    rig.pump_for(std::chrono::milliseconds(500));

    bool said_closed = false;
    for (const auto& m : rig.socket.sent) {
        if (m.kind == Kind::Text && contains(m.text, R"("event":"source")")) {
            said_closed = contains(m.text, R"("open":false)");
        }
    }
    CHECK(said_closed);
    CHECK(rig.socket.count(Kind::Spectrum) == 0);
    CHECK_FALSE(rig.streams->spectrum_subscribed());
}

TEST_CASE("openSource at a new rate recomputes everyNth", "[gpu][webgate][streams]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    auto options = still_engine();
    options.spectrum_transform = revenant::test::kSpectrumTransform;
    options.pace = 1.0;
    // The engine's own default dispatch. At the fixture's 16384 the
    // in-process engine runs 20 MS/s at about half realtime, which would
    // measure the test machine rather than every_nth.
    options.block_samples = 65'536;
    options.samples = 2'400'032LL * 30;
    options.source_uri = "synthetic:wideband?rate=2400000&emitters=2&seed=5&samples=72000000";
    rig.open(options);
    REQUIRE(rig.harness.start_engine().has_value());
    rig.attach();
    rig.command(R"({"id":1,"op":"setView","args":{"centerHz":0,"spanHz":2000000,"widthPx":600}})");

    const auto measure = [&rig] {
        rig.pump_for(std::chrono::milliseconds(1500));
        const auto from = rig.streams->counters().spectrum_sent;
        rig.pump_for(std::chrono::milliseconds(2000));
        return static_cast<double>(rig.streams->counters().spectrum_sent - from) / 2.0;
    };

    const double slow = measure();
    INFO("2.4 MS/s: " << slow << " fps, every_nth " << rig.streams->every_nth());
    CHECK(slow >= 15.0);
    CHECK(slow <= 25.0);

    // close_source stops run(), so the harness's runner is joined and started
    // again around the phone's openSource.
    REQUIRE(rig.harness.client().close_source().has_value());
    static_cast<void>(rig.harness.stop_engine());
    // emitters=0: synthesising emitters at 20 MS/s holds the in-process
    // source to about half realtime (measured 2026-11-02), which would test
    // the machine and not every_nth. Noise alone keeps realtime.
    rig.command(
        R"({"id":2,"op":"openSource","args":{"uri":"synthetic:wideband?rate=20000000&emitters=0&seed=6&samples=600000000"}})");
    INFO(reply_to(rig.socket, 2));
    REQUIRE(contains(reply_to(rig.socket, 2), R"("ok":true)"));
    REQUIRE(rig.harness.start_engine().has_value());

    const double fast = measure();
    const auto engine_info = rig.harness.client().info();
    INFO("realtime factor " << (engine_info ? engine_info->realtime_factor : -1.0));
    INFO("20 MS/s: " << fast << " fps, every_nth " << rig.streams->every_nth());
    CHECK(fast >= 15.0);
    CHECK(fast <= 25.0);
}

TEST_CASE("the source event carries the engine's realtime factor", "[gpu][webgate][streams]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    auto options = still_engine();
    options.pace = 1.0;
    options.samples = 2'400'032LL * 20;
    rig.open(options);
    rig.attach();

    // Before the run the engine has measured nothing, and says zero.
    std::string first;
    for (const auto& m : rig.socket.sent) {
        if (m.kind == Kind::Text && contains(m.text, R"("event":"source")")) {
            first = m.text;
        }
    }
    INFO(first);
    REQUIRE(contains(first, R"("realtimeFactor":0,)"));
    CHECK(contains(first, R"("pacedBy":)"));

    REQUIRE(rig.harness.start_engine().has_value());
    rig.pump_for(std::chrono::milliseconds(2500));

    // Once measured, the factor alone sends a new event without the centre
    // or the stream changing.
    std::string last;
    for (const auto& m : rig.socket.sent) {
        if (m.kind == Kind::Text && contains(m.text, R"("event":"source")")) {
            last = m.text;
        }
    }
    INFO(last);
    CHECK(last != first);
    CHECK(contains(last, R"("realtimeFactor":)"));
    CHECK_FALSE(contains(last, R"("realtimeFactor":0,)"));
}

[[nodiscard]] std::optional<double> number_field(const std::string& json, std::string_view key)
{
    const std::string needle = "\"" + std::string(key) + "\":";
    const auto at = json.find(needle);
    if (at == std::string::npos) {
        return std::nullopt;
    }
    return std::stod(json.substr(at + needle.size()));
}

TEST_CASE("the source event carries the spectrum's real edges", "[gpu][webgate][streams]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    auto options = still_engine();
    options.spectrum_transform = revenant::test::kSpectrumTransform;
    rig.open(options);
    rig.attach();

    std::string text;
    for (const auto& m : rig.socket.sent) {
        if (m.kind == Kind::Text && contains(m.text, R"("event":"source")")) {
            text = m.text;
        }
    }
    INFO(text);
    const auto info = rig.harness.client().info();
    REQUIRE(info.has_value());
    REQUIRE(info->spectrum.enabled());
    const double bin = info->spectrum.bin_width.hertz();
    const double first = static_cast<double>(info->source_center) + info->spectrum.bin_zero.hertz();
    const auto low = number_field(text, "dataLowHz");
    const auto high = number_field(text, "dataHighHz");
    REQUIRE(low.has_value());
    REQUIRE(high.has_value());
    CHECK(std::abs(*low - (first - bin / 2.0)) < 1e-3);
    CHECK(std::abs(*high - (first + (static_cast<double>(info->spectrum.bins) - 0.5) * bin)) < 1e-3);
    // The edges are where the bins lie, which is not the band: the low edge
    // sits below centre - rate/2 and the high one short of centre + rate/2.
    const double half_rate = static_cast<double>(info->source_rate) / 2.0;
    CHECK(*low <= static_cast<double>(info->source_center) - half_rate);
    CHECK(*high < static_cast<double>(info->source_center) + half_rate);
}

TEST_CASE("no spectrum, no edges in the source event", "[gpu][webgate][streams]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    rig.open(still_engine());
    rig.attach();
    std::string text;
    for (const auto& m : rig.socket.sent) {
        if (m.kind == Kind::Text && contains(m.text, R"("event":"source")")) {
            text = m.text;
        }
    }
    INFO(text);
    REQUIRE(contains(text, R"("open":true)"));
    CHECK_FALSE(contains(text, R"("dataLowHz")"));
}

[[nodiscard]] std::size_t count_events(const FakeSocket& socket, std::string_view name)
{
    const std::string needle = "\"event\":\"" + std::string(name) + "\"";
    return static_cast<std::size_t>(std::count_if(socket.sent.begin(), socket.sent.end(), [&](const auto& m) {
        return m.kind == Kind::Text && contains(m.text, needle);
    }));
}

TEST_CASE("transcriptionStatus goes out on connect, on change, and not otherwise",
          "[gpu][webgate][streams]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    rig.use_fake_clock = true;
    revenant::rpc::TranscriptionStatus speech;
    speech.enabled = true;
    speech.model_state = revenant::rpc::TranscriptionModelState::Downloading;
    speech.model_name = "base.en";
    speech.bytes_total = 1000;
    speech.bytes_done = 100;
    rig.speech = [&speech] { return revenant::Expected<revenant::rpc::TranscriptionStatus>(speech); };
    rig.open(still_engine());
    rig.attach();

    // Connecting sends the state once, with the download at 10%.
    REQUIRE(count_events(rig.socket, "transcriptionStatus") == 1);
    const auto first = std::find_if(rig.socket.sent.begin(), rig.socket.sent.end(), [](const auto& m) {
        return m.kind == Kind::Text && contains(m.text, R"("event":"transcriptionStatus")");
    });
    REQUIRE(first != rig.socket.sent.end());
    CHECK(contains(first->text, R"("modelState":"downloading")"));
    CHECK(contains(first->text, R"("bytesDone":100)"));

    auto poll = [&] {
        rig.fake_now += std::chrono::seconds(2);
        rig.streams->tick();
        rig.strand.run_pending();
    };

    SECTION("nothing changed sends nothing")
    {
        poll();
        poll();
        CHECK(count_events(rig.socket, "transcriptionStatus") == 1);
    }
    SECTION("counters and a sub-percent download move send nothing")
    {
        speech.bytes_done = 105;
        speech.transcribed = 40;
        speech.queued = 3;
        poll();
        CHECK(count_events(rig.socket, "transcriptionStatus") == 1);
    }
    SECTION("a whole percent, the state or a detail sentence sends one")
    {
        speech.bytes_done = 300;
        poll();
        CHECK(count_events(rig.socket, "transcriptionStatus") == 2);
        speech.model_state = revenant::rpc::TranscriptionModelState::Failed;
        speech.detail = "checksum did not match";
        poll();
        CHECK(count_events(rig.socket, "transcriptionStatus") == 3);
        poll();
        CHECK(count_events(rig.socket, "transcriptionStatus") == 3);
        CHECK(contains(rig.socket.sent.back().text, "checksum did not match"));
    }
    SECTION("a resumed socket is told the current state again")
    {
        rig.streams->announce_state();
        rig.strand.run_pending();
        CHECK(count_events(rig.socket, "transcriptionStatus") == 2);
    }
    SECTION("an engine that cannot transcribe sends nothing")
    {
        rig.speech = nullptr;
        rig.streams.reset();
        rig.session.reset();
        rig.socket.sent.clear();
        speech.enabled = false;
        rig.speech = [] {
            return revenant::Expected<revenant::rpc::TranscriptionStatus>(revenant::fail("no recogniser"));
        };
        rig.attach();
        CHECK(count_events(rig.socket, "transcriptionStatus") == 0);
    }
}

// Rejects a display event sent once at connect and never again, sent every
// tick, or missing from a resumed socket; and a frame without its trace row.
TEST_CASE("display goes out on connect, on change and on resume", "[gpu][webgate][streams]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    rig.use_fake_clock = true;
    revenant::ui::DisplaySettings desktop;
    rig.display = [&desktop] { return desktop; };
    rig.open(still_engine());
    rig.attach();
    REQUIRE(count_events(rig.socket, "display") == 1);

    auto poll = [&] {
        rig.fake_now += std::chrono::seconds(2);
        rig.streams->tick();
        rig.strand.run_pending();
    };
    poll();
    CHECK(count_events(rig.socket, "display") == 1);

    desktop.waterfall_theme = revenant::ui::colour_map::theme_by_id("amber");
    poll();
    CHECK(count_events(rig.socket, "display") == 2);
    poll();
    CHECK(count_events(rig.socket, "display") == 2);

    rig.streams->announce_state();
    rig.strand.run_pending();
    CHECK(count_events(rig.socket, "display") == 3);

    rig.command(R"({"id":1,"op":"setView","args":{"centerHz":100000000,"spanHz":2000000,"widthPx":800}})");
    rig.streams->deliver_spectrum(synthetic_frame(0));
    rig.strand.run_pending();
    REQUIRE(rig.socket.count(Kind::Spectrum) == 1);
    const auto frame = std::find_if(rig.socket.sent.begin(), rig.socket.sent.end(),
                                    [](const auto& m) { return m.kind == Kind::Spectrum; });
    CHECK(frame->binary.size() == revenant::webgate::kSpectrumHeaderSize + 800 +
                                      revenant::webgate::kSpectrumTraceHeaderSize + 800);
}

TEST_CASE("slow socket drops oldest Opus", "[gpu][webgate][streams]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    auto options = still_engine();
    options.pace = 1.0;
    options.samples = 2'400'032LL * 20;
    rig.open(options);
    REQUIRE(rig.harness.start_engine().has_value());
    rig.attach();

    rig.command(R"({"id":1,"op":"addVrx","args":{"hz":100100000,"mode":"am"}})");
    const auto added = reply_to(rig.socket, 1);
    INFO(added);
    REQUIRE(contains(added, R"("ok":true)"));
    const auto vrx = vrx_of(added);
    rig.command(R"({"id":2,"op":"audio","args":{"vrx":)" + std::to_string(vrx) + "}}");
    rig.pump_for(std::chrono::milliseconds(700));
    REQUIRE(rig.socket.count(Kind::Opus) > 0);

    rig.socket.hold();
    rig.pump_for(std::chrono::milliseconds(1000));
    const auto mid = rig.session->client().audio_stats(vrx);
    rig.pump_for(std::chrono::milliseconds(1000));
    const auto end = rig.session->client().audio_stats(vrx);
    INFO(revenant::test::message_of(end));
    REQUIRE(mid.has_value());
    REQUIRE(end.has_value());
    // The gateway kept reading the engine while the phone stalled, so the
    // engine's queue for this subscription neither grew nor overflowed.
    CHECK(end->frames_dropped == mid->frames_dropped);
    CHECK(end->backlog_frames <= std::max<std::uint64_t>(mid->backlog_frames, end->buffer_frames));

    // The last packet handed over before the release is the one the socket
    // has been holding all along.
    const auto released_at = rig.socket.sent.size();
    std::uint32_t held_seq = 0;
    for (std::size_t i = 0; i < released_at; ++i) {
        if (rig.socket.sent[i].kind == Kind::Opus) {
            held_seq = seq_of(rig.socket.sent[i]);
        }
    }
    const auto dropped_before_release = rig.streams->counters().opus_dropped;
    rig.socket.release();
    rig.pump_for(std::chrono::milliseconds(100));

    std::optional<std::uint32_t> first_after;
    for (std::size_t i = released_at; i < rig.socket.sent.size(); ++i) {
        if (rig.socket.sent[i].kind == Kind::Opus) {
            first_after = seq_of(rig.socket.sent[i]);
            break;
        }
    }
    REQUIRE(first_after.has_value());
    // The packets missing between the held one and the first after the
    // release are exactly the ones dropped from the front of the queue, so
    // what played next was the newest the queue held, not the oldest.
    CHECK(dropped_before_release > 0);
    CHECK(*first_after > held_seq + 1);
    CHECK(*first_after - held_seq - 1 == dropped_before_release);
}

TEST_CASE("HLS lingers 30 s after keepListening off", "[gpu][webgate][streams]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    rig.use_fake_clock = true;
    rig.open(still_engine());
    rig.attach();

    rig.command(R"({"id":1,"op":"addVrx","args":{"hz":100100000,"mode":"am"}})");
    const auto vrx = vrx_of(reply_to(rig.socket, 1));
    const auto id = std::to_string(vrx);
    rig.command(R"({"id":2,"op":"audio","args":{"vrx":)" + id + "}}");
    rig.command(R"({"id":3,"op":"keepListening","args":{"on":true}})");

    bool announced = false;
    for (const auto& m : rig.socket.sent) {
        announced = announced ||
                    (m.kind == Kind::Text && contains(m.text, R"("event":"hls")") &&
                     contains(m.text, "/hls/" + id + "/live.m3u8"));
    }
    CHECK(announced);
    HlsSegmenter* const first = rig.registry.find(vrx);
    REQUIRE(first != nullptr);

    SECTION("gone after the linger")
    {
        rig.command(R"({"id":4,"op":"keepListening","args":{"on":false}})");
        rig.fake_now += std::chrono::seconds(29);
        rig.streams->tick();
        CHECK(rig.registry.find(vrx) == first);
        rig.fake_now += std::chrono::seconds(2);
        rig.streams->tick();
        CHECK(rig.registry.find(vrx) == nullptr);
        CHECK(rig.streams->segmenter(vrx) == nullptr);
    }

    SECTION("on again inside the linger keeps the same segmenter")
    {
        rig.command(R"({"id":4,"op":"keepListening","args":{"on":false}})");
        rig.fake_now += std::chrono::seconds(10);
        rig.streams->tick();
        rig.command(R"({"id":5,"op":"keepListening","args":{"on":true}})");
        CHECK(rig.registry.find(vrx) == first);
        rig.fake_now += std::chrono::seconds(60);
        rig.streams->tick();
        CHECK(rig.registry.find(vrx) == first);
    }
}

TEST_CASE("a receiver removed elsewhere is reported to its phone", "[gpu][webgate][streams]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    rig.open(still_engine());
    rig.attach();

    rig.command(R"({"id":1,"op":"addVrx","args":{"hz":100100000,"mode":"am"}})");
    const auto vrx = vrx_of(reply_to(rig.socket, 1));
    rig.command(R"({"id":2,"op":"audio","args":{"vrx":)" + std::to_string(vrx) + "}}");

    // Someone else closes the source, which takes every receiver with it.
    REQUIRE(rig.harness.client().close_source().has_value());
    rig.pump_for(std::chrono::milliseconds(1500));

    bool reported = false;
    for (const auto& m : rig.socket.sent) {
        if (m.kind == Kind::Text && contains(m.text, R"("event":"vrxRemoved")") &&
            contains(m.text, "\"vrx\":" + std::to_string(vrx))) {
            reported = contains(m.text, R"("reason":)");
        }
    }
    CHECK(reported);
    CHECK_FALSE(rig.session->owns(vrx));
    CHECK_FALSE(rig.session->audio_vrx().has_value());
}

TEST_CASE("a replaced receiver is reported removed and audio follows the new id",
          "[gpu][webgate][streams]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    rig.open(still_engine());
    rig.attach();

    rig.command(R"({"id":1,"op":"addVrx","args":{"hz":100100000,"mode":"am"}})");
    const auto old_id = vrx_of(reply_to(rig.socket, 1));
    rig.command(R"({"id":2,"op":"audio","args":{"vrx":)" + std::to_string(old_id) + "}}");
    rig.command(R"({"id":3,"op":"setMode","args":{"vrx":)" + std::to_string(old_id) +
                R"(,"mode":"usb"}})");
    const auto replaced = reply_to(rig.socket, 3);
    INFO(replaced);
    REQUIRE(contains(replaced, R"("ok":true)"));
    const auto new_id = vrx_of(replaced);
    rig.pump_for(std::chrono::milliseconds(500));

    bool reported = false;
    for (const auto& m : rig.socket.sent) {
        reported = reported || (m.kind == Kind::Text &&
                                contains(m.text, R"("event":"vrxRemoved")") &&
                                contains(m.text, "\"vrx\":" + std::to_string(old_id)));
    }
    CHECK(reported);
    CHECK(rig.session->audio_vrx() == new_id);
}

TEST_CASE("link events reach the phone", "[gpu][webgate][streams]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    rig.open(still_engine());
    rig.attach();
    rig.streams->send_event(revenant::webgate::event(
        "link", revenant::webgate::JsonObject().add_string("state", "waiting")));
    rig.strand.run_pending();
    REQUIRE_FALSE(rig.socket.sent.empty());
    CHECK(contains(rig.socket.sent.back().text, R"("event":"link")"));
}

TEST_CASE("KjStrand runs a task posted from another thread on the loop thread",
          "[webgate][streams]")
{
    auto io = kj::setupAsyncIo();
    revenant::webgate::KjStrand strand;
    auto done = kj::newPromiseAndFulfiller<void>();
    std::thread::id ran_on;
    bool ran = false;

    std::thread poster([&] {
        strand.post([&] {
            ran = true;
            ran_on = std::this_thread::get_id();
            done.fulfiller->fulfill();
        });
    });

    bool timed_out = false;
    auto deadline = io.provider->getTimer().afterDelay(5 * kj::SECONDS).then([&] {
        timed_out = true;
    });
    kj::mv(done.promise)
        .exclusiveJoin(strand.run())
        .exclusiveJoin(kj::mv(deadline))
        .wait(io.waitScope);
    poster.join();

    CHECK_FALSE(timed_out);
    CHECK(ran);
    CHECK(ran_on == std::this_thread::get_id());
}

TEST_CASE("a socket that stops reading is closed rather than queued without bound",
          "[gpu][webgate][streams]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    rig.max_queued_text = 5;
    rig.open(still_engine());
    rig.attach();

    rig.socket.hold();
    for (int i = 0; i < 20; ++i) {
        rig.streams->send_event(revenant::webgate::event(
            "link", revenant::webgate::JsonObject().add_u64("n", static_cast<std::uint64_t>(i))));
    }
    CHECK(rig.socket.closed);
    CHECK_FALSE(rig.streams->attached());
    CHECK(rig.streams->counters().text_dropped > 0);
}

namespace {

[[nodiscard]] revenant::rpc::Detection track(std::uint64_t id, std::int64_t centre, double snr,
                                             revenant::rpc::TrackState state = revenant::rpc::TrackState::Live)
{
    revenant::rpc::Detection d;
    d.id = id;
    d.center_hz = centre;
    d.bandwidth_hz = 10'000;
    d.snr_2500_db = snr;
    d.confidence = 0.8;
    d.state = state;
    d.last_seen = 5000;
    return d;
}

// Last "detections" event the phone was sent, as text.
[[nodiscard]] std::string last_detections(const FakeSocket& socket)
{
    for (auto it = socket.sent.rbegin(); it != socket.sent.rend(); ++it) {
        if (it->kind == Kind::Text && contains(it->text, R"("event":"detections")")) {
            return it->text;
        }
    }
    return {};
}

struct DetectionRig {
    Rig rig;
    revenant::rpc::DetectionList list;
    bool fail = false;
    int polls = 0;
    double last_bar = -1.0;

    DetectionRig()
    {
        rig.use_fake_clock = true;
        rig.detections = [this](double bar, double) -> revenant::Expected<revenant::rpc::DetectionList> {
            polls += 1;
            last_bar = bar;
            if (fail) {
                return revenant::fail("detector not ready");
            }
            return list;
        };
        rig.open(still_engine());
        rig.attach();
    }

    void view()
    {
        rig.command(R"({"id":1,"op":"setView","args":{"centerHz":100000000,"spanHz":200000,"widthPx":600}})");
    }

    void poll()
    {
        rig.fake_now += std::chrono::milliseconds(600);
        rig.streams->tick();
        rig.strand.run_pending();
    }
};

}  // namespace

TEST_CASE("detections event shape, labels and ids", "[gpu][webgate][streams]")
{
    REVENANT_NEEDS_GPU();
    DetectionRig t;
    auto a = track(1ULL << 60, kCentre + 1000, 12.5);
    a.label.kind = revenant::rpc::LabelKind::AnalogModulation;
    a.label.name = "NFM";
    a.label.confidence = 0.9;
    auto b = track(7, kCentre + 2000, 9.0, revenant::rpc::TrackState::Held);
    b.label.kind = revenant::rpc::LabelKind::Protocol;
    b.label.name = "P25";
    t.list.detections = {a, b};
    t.view();
    t.poll();

    const auto text = last_detections(t.rig.socket);
    INFO(text);
    CHECK(contains(text, R"("id":"1152921504606846976")"));
    CHECK(contains(text, R"("centerHz":100001000)"));
    CHECK(contains(text, R"("bandwidthHz":10000)"));
    CHECK(contains(text, R"("snrDb":12.5)"));
    CHECK(contains(text, R"("confidence":0.8)"));
    CHECK(contains(text, R"("state":"live")"));
    CHECK(contains(text, R"("label":"NFM")"));
    CHECK(contains(text, R"("labelConfidence":0.9)"));
    CHECK(contains(text, R"("lastSeen":5000)"));
    // A held track, and a protocol label is not a modulation.
    CHECK(contains(text, R"("id":"7")"));
    CHECK(contains(text, R"("state":"held")"));
    CHECK_FALSE(contains(text, "P25"));
    CHECK(contains(text, R"("label":"")"));
}

TEST_CASE("detections drop merged and pending tracks and those far from the view",
          "[gpu][webgate][streams]")
{
    REVENANT_NEEDS_GPU();
    DetectionRig t;
    // The view spans 200 kHz, so the window is 1.5 spans each side: 300 kHz.
    t.list.detections = {track(1, kCentre, 10.0),
                         track(2, kCentre, 10.0, revenant::rpc::TrackState::Merged),
                         track(3, kCentre, 10.0, revenant::rpc::TrackState::Pending),
                         track(4, kCentre + 299'000, 10.0),
                         track(5, kCentre + 400'000, 10.0),
                         track(6, kCentre - 400'000, 10.0)};
    t.view();
    t.poll();
    const auto text = last_detections(t.rig.socket);
    INFO(text);
    CHECK(contains(text, R"("id":"1")"));
    CHECK_FALSE(contains(text, R"("id":"2")"));
    CHECK_FALSE(contains(text, R"("id":"3")"));
    CHECK(contains(text, R"("id":"4")"));
    CHECK_FALSE(contains(text, R"("id":"5")"));
    CHECK_FALSE(contains(text, R"("id":"6")"));
}

TEST_CASE("detections keep the 200 strongest in view", "[gpu][webgate][streams]")
{
    REVENANT_NEEDS_GPU();
    DetectionRig t;
    for (std::uint64_t i = 0; i < 250; ++i) {
        // Id i has SNR i, so the strongest are ids 50 to 249.
        t.list.detections.push_back(track(i, kCentre - 100'000 + static_cast<std::int64_t>(i) * 100,
                                          static_cast<double>(i)));
    }
    t.view();
    t.poll();
    const auto text = last_detections(t.rig.socket);
    std::size_t count = 0;
    for (auto at = text.find(R"("id":")"); at != std::string::npos; at = text.find(R"("id":")", at + 1)) {
        count += 1;
    }
    CHECK(count == 200);
    CHECK_FALSE(contains(text, R"("id":"49")"));
    CHECK(contains(text, R"("id":"50")"));
    CHECK(contains(text, R"("id":"249")"));
}

TEST_CASE("detections poll only with a view, at their own interval, with the session bar",
          "[gpu][webgate][streams]")
{
    REVENANT_NEEDS_GPU();
    DetectionRig t;
    t.list.detections = {track(1, kCentre, 10.0)};

    t.poll();
    t.poll();
    CHECK(t.polls == 0);
    CHECK(count_events(t.rig.socket, "detections") == 0);

    t.view();
    t.poll();
    CHECK(t.polls == 1);
    // Inside the interval nothing is asked.
    t.rig.fake_now += std::chrono::milliseconds(100);
    t.rig.streams->tick();
    CHECK(t.polls == 1);
    t.poll();
    CHECK(t.polls == 2);
    CHECK(t.last_bar == 0.0);

    t.rig.command(R"({"id":2,"op":"setDetection","args":{"minConfidence":0.4}})");
    CHECK(contains(reply_to(t.rig.socket, 2), R"("minConfidence":0.4)"));
    t.poll();
    CHECK(t.last_bar == 0.4);

    // The bar belongs to the session, so a socket that resumes it keeps it.
    t.rig.streams->detach();
    t.rig.streams->attach(t.rig.socket);
    t.poll();
    CHECK(t.last_bar == 0.4);
}

TEST_CASE("a failed detections poll sends nothing", "[gpu][webgate][streams]")
{
    REVENANT_NEEDS_GPU();
    DetectionRig t;
    t.fail = true;
    t.view();
    const auto before = t.rig.socket.sent.size();
    t.poll();
    CHECK(t.polls == 1);
    CHECK(t.rig.socket.sent.size() == before);
}
