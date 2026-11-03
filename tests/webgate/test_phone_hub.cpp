// Sessions outliving their sockets: detach on drop, expiry by the clock and
// by HLS requests, and resume onto a new socket. Same in-process engine and
// fakes as test_session_streams.cpp, with a fake clock throughout.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "core/rpc/client.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"
#include "tests/webgate/stream_fakes.h"
#include "tools/webgate/hls_playlist.h"
#include "tools/webgate/phone_hub.h"
#include "tools/webgate/protocol.h"

namespace {

using revenant::webgate::HlsRegistry;
using revenant::webgate::OutgoingMessage;
using revenant::webgate::PhoneHub;
using revenant::webgate::StreamsConfig;
using revenant::webgate::test::FakeSocket;
using revenant::webgate::test::ManualStrand;
using Kind = OutgoingMessage::Kind;
using Clock = std::chrono::steady_clock;
using std::chrono::seconds;

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

[[nodiscard]] std::string reply_to(const FakeSocket& socket, std::uint64_t id)
{
    const std::string prefix = "{\"id\":" + std::to_string(id) + ",";
    for (const auto& message : socket.sent) {
        if (message.kind == Kind::Text && message.text.rfind(prefix, 0) == 0) {
            return message.text;
        }
    }
    return {};
}

[[nodiscard]] std::uint64_t vrx_of(const std::string& reply)
{
    const auto at = reply.find("\"vrx\":");
    REQUIRE(at != std::string::npos);
    return std::stoull(reply.substr(at + 6));
}

[[nodiscard]] revenant::rpc::SpectrumFrame synthetic_frame()
{
    revenant::rpc::SpectrumFrame frame;
    frame.geometry.bins = 4096;
    frame.geometry.bin_width = {1000, 1};
    frame.geometry.bin_zero = {-2'048'000, 1};
    frame.power_db.assign(4096, -90.0F);
    frame.count = 16384;
    frame.floor_db = -120.0F;
    frame.ceiling_db = 0.0F;
    return frame;
}

[[nodiscard]] revenant::rpc::AudioChunk tone_chunk()
{
    revenant::rpc::AudioChunk chunk;
    chunk.sample_rate = 48'000;
    chunk.channel_count = 1;
    chunk.samples.assign(1920, 0.25F);
    return chunk;
}

struct HubRig {
    revenant::test::Harness harness;
    ManualStrand strand;
    HlsRegistry registry;
    Clock::time_point fake_now = Clock::now();
    std::unique_ptr<PhoneHub> hub;

    explicit HubRig(revenant::webgate::HubTiming timing = {})
    {
        revenant::test::HarnessOptions options;
        options.center_hz = 100'000'000;
        const auto opened = harness.open(options);
        INFO(revenant::test::message_of(opened));
        REQUIRE(opened.has_value());
        StreamsConfig config;
        config.hls = &registry;
        config.clock = [this] { return fake_now; };
        hub = std::make_unique<PhoneHub>(strand, config, timing);
        // What the Gateway wires: the HTTP side reports every HLS request.
        registry.on_fetch = [this](std::uint64_t vrx) { hub->hls_fetched(vrx); };
    }

    ~HubRig()
    {
        registry.on_fetch = nullptr;
        hub.reset();
    }

    std::uint64_t connect(FakeSocket& socket)
    {
        auto client = harness.connect_another();
        INFO(revenant::test::message_of(client));
        REQUIRE(client.has_value());
        const auto id = hub->connect(socket, std::move(*client));
        strand.run_pending();
        return id;
    }

    void text(std::uint64_t connection, const std::string& line)
    {
        hub->text(connection, line);
        strand.run_pending();
    }

    void advance(seconds by)
    {
        fake_now += by;
        hub->tick();
        strand.run_pending();
    }

    // A receiver with audio on, and keep listening if asked.
    std::uint64_t listen(std::uint64_t connection, FakeSocket& socket, bool keep)
    {
        text(connection, R"({"id":1,"op":"addVrx","args":{"hz":100100000,"mode":"am"}})");
        const auto reply = reply_to(socket, 1);
        INFO(reply);
        REQUIRE(contains(reply, R"("ok":true)"));
        const auto vrx = vrx_of(reply);
        text(connection, R"({"id":2,"op":"audio","args":{"vrx":)" + std::to_string(vrx) + "}}");
        if (keep) {
            text(connection, R"({"id":3,"op":"keepListening","args":{"on":true}})");
        }
        return vrx;
    }

    [[nodiscard]] bool engine_has(std::uint64_t vrx)
    {
        const auto ids = harness.client().vrx_ids();
        REQUIRE(ids.has_value());
        return std::ranges::find(*ids, vrx) != ids->end();
    }
};

}  // namespace

TEST_CASE("hello carries a resume token", "[gpu][webgate][hub]")
{
    REVENANT_NEEDS_GPU();
    HubRig rig;
    FakeSocket socket;
    const auto id = rig.connect(socket);
    REQUIRE_FALSE(socket.sent.empty());
    const auto token = rig.hub->token(id);
    CHECK(token.size() == 32);
    CHECK(contains(socket.sent.front().text,
                   R"({"event":"hello","resume":")" + token + "\"}"));
}

TEST_CASE("keep listening outlives a dropped socket while HLS is fetched",
          "[gpu][webgate][hub]")
{
    REVENANT_NEEDS_GPU();
    HubRig rig;
    FakeSocket socket;
    const auto id = rig.connect(socket);
    const auto token = rig.hub->token(id);
    const auto vrx = rig.listen(id, socket, true);
    REQUIRE(rig.registry.find(vrx) != nullptr);

    rig.hub->disconnect(id);
    // Well past the 30 s base linger, so only the fetches keep it.
    for (int i = 0; i < 660; ++i) {
        rig.registry.fetched(vrx);
        rig.advance(seconds(1));
    }
    CHECK(rig.hub->holds(token));
    CHECK(rig.registry.find(vrx) != nullptr);
    CHECK(rig.engine_has(vrx));

    rig.advance(seconds(59));
    CHECK(rig.hub->holds(token));
    rig.advance(seconds(2));
    CHECK_FALSE(rig.hub->holds(token));
    CHECK(rig.registry.find(vrx) == nullptr);
    CHECK_FALSE(rig.engine_has(vrx));
}

TEST_CASE("keep listening with no fetches lasts until hls_idle", "[gpu][webgate][hub]")
{
    REVENANT_NEEDS_GPU();
    HubRig rig;
    FakeSocket socket;
    const auto id = rig.connect(socket);
    const auto token = rig.hub->token(id);
    rig.listen(id, socket, true);

    // No HLS fetch at all: the later of the 30 s base linger and the 60 s
    // hls_idle decides, so the session is still there at 59 s.
    rig.hub->disconnect(id);
    rig.advance(seconds(29));
    CHECK(rig.hub->holds(token));
    rig.advance(seconds(30));
    CHECK(rig.hub->holds(token));
    rig.advance(seconds(3));
    CHECK_FALSE(rig.hub->holds(token));
}

TEST_CASE("a detached session without keep listening goes after 30 s", "[gpu][webgate][hub]")
{
    REVENANT_NEEDS_GPU();
    HubRig rig;
    FakeSocket socket;
    const auto id = rig.connect(socket);
    const auto token = rig.hub->token(id);
    const auto vrx = rig.listen(id, socket, false);

    rig.hub->disconnect(id);
    rig.advance(seconds(29));
    CHECK(rig.hub->holds(token));
    CHECK(rig.engine_has(vrx));
    rig.advance(seconds(2));
    CHECK_FALSE(rig.hub->holds(token));
    CHECK_FALSE(rig.engine_has(vrx));
}

// Rejects a phone built from a config that missed the hub's cache: two
// phones zoomed into the same frame must measure its whole-frame levels once
// between them, and a new frame once more.
TEST_CASE("two phones on one hub measure a zoomed frame once", "[gpu][webgate][hub]")
{
    REVENANT_NEEDS_GPU();
    HubRig rig;
    FakeSocket first;
    FakeSocket second;
    const auto a = rig.connect(first);
    const auto b = rig.connect(second);
    // A quarter of the frame's 4.096 MHz, at different widths: the share is
    // the whole frame's and does not depend on either view.
    rig.text(a, R"({"id":9,"op":"setView","args":{"centerHz":100000000,"spanHz":1000000,"widthPx":400}})");
    rig.text(b, R"({"id":9,"op":"setView","args":{"centerHz":100200000,"spanHz":1000000,"widthPx":300}})");
    const auto before = rig.hub->frame_levels().measured();

    auto frame = synthetic_frame();
    frame.sequence = 7;
    frame.start = 7 * 16384;
    rig.hub->streams(a)->deliver_spectrum(frame);
    rig.hub->streams(b)->deliver_spectrum(frame);
    rig.strand.run_pending();
    REQUIRE(first.count(Kind::Spectrum) == 1);
    REQUIRE(second.count(Kind::Spectrum) == 1);
    CHECK(rig.hub->frame_levels().measured() - before == 1);

    frame.sequence = 8;
    frame.start = 8 * 16384;
    rig.hub->streams(a)->deliver_spectrum(frame);
    rig.hub->streams(b)->deliver_spectrum(frame);
    rig.strand.run_pending();
    CHECK(rig.hub->frame_levels().measured() - before == 2);
}

TEST_CASE("resume reattaches and the next frames go to the new socket", "[gpu][webgate][hub]")
{
    REVENANT_NEEDS_GPU();
    HubRig rig;
    FakeSocket first;
    const auto id = rig.connect(first);
    const auto token = rig.hub->token(id);
    rig.text(id, R"({"id":9,"op":"setView","args":{"centerHz":100000000,"spanHz":2000000,"widthPx":400}})");
    const auto vrx = rig.listen(id, first, true);
    auto* const streams = rig.hub->streams(id);

    rig.hub->disconnect(id);
    const auto sent_before = first.sent.size();
    // While detached nothing is queued for a phone that is not there.
    streams->deliver_spectrum(synthetic_frame());
    streams->deliver_audio(vrx, tone_chunk());
    rig.strand.run_pending();
    CHECK(first.sent.size() == sent_before);

    FakeSocket second;
    const auto again = rig.connect(second);
    CHECK(rig.hub->sessions() == 2);
    rig.text(again, R"({"id":1,"op":"resume","args":{"token":")" + token + "\"}}");
    const auto reply = reply_to(second, 1);
    INFO(reply);
    CHECK(contains(reply, R"("ok":true)"));
    CHECK(rig.hub->sessions() == 1);
    CHECK(rig.hub->streams(again) == streams);
    CHECK(rig.hub->session(again)->owns(vrx));
    CHECK(rig.registry.find(vrx) != nullptr);

    const auto opus_before = second.count(Kind::Opus);
    streams->deliver_spectrum(synthetic_frame());
    streams->deliver_audio(vrx, tone_chunk());
    rig.strand.run_pending();
    CHECK(second.count(Kind::Spectrum) == 1);
    CHECK(second.count(Kind::Opus) > opus_before);
    CHECK(first.sent.size() == sent_before);
}

TEST_CASE("resume re-sends source, link and hls to the new socket", "[gpu][webgate][hub]")
{
    REVENANT_NEEDS_GPU();
    HubRig rig;
    FakeSocket first;
    const auto id = rig.connect(first);
    const auto token = rig.hub->token(id);
    const auto vrx = rig.listen(id, first, true);
    rig.advance(seconds(2));
    const auto saw = [](const FakeSocket& socket, std::string_view needle) {
        return std::ranges::any_of(socket.sent, [&](const auto& m) {
            return m.kind == Kind::Text && contains(m.text, needle);
        });
    };
    REQUIRE(saw(first, R"({"event":"source","open":true)"));
    REQUIRE(saw(first, R"({"event":"hls")"));
    rig.hub->disconnect(id);

    // A page launched after those events went out: it has seen none of them.
    FakeSocket second;
    const auto again = rig.connect(second);
    second.sent.clear();
    rig.text(again, R"({"id":1,"op":"resume","args":{"token":")" + token + "\"}}");
    REQUIRE(contains(reply_to(second, 1), R"("resumed":true)"));
    CHECK(saw(second, R"({"event":"link")"));
    CHECK(saw(second, R"({"event":"source","open":true,"rateHz":)"));
    CHECK(saw(second, R"("centerHz":100000000)"));
    CHECK(saw(second, R"({"event":"hls","vrx":)" + std::to_string(vrx)));
}

TEST_CASE("resume of an expired token fails cleanly", "[gpu][webgate][hub]")
{
    REVENANT_NEEDS_GPU();
    HubRig rig;
    FakeSocket first;
    const auto id = rig.connect(first);
    const auto token = rig.hub->token(id);
    rig.hub->disconnect(id);
    rig.advance(seconds(601));
    REQUIRE_FALSE(rig.hub->holds(token));

    FakeSocket second;
    const auto again = rig.connect(second);
    rig.text(again, R"({"id":1,"op":"resume","args":{"token":")" + token + "\"}}");
    const auto reply = reply_to(second, 1);
    INFO(reply);
    CHECK(contains(reply, R"("ok":false)"));
    CHECK(contains(reply, "no session with that token"));
    CHECK(rig.hub->sessions() == 1);

    // The fresh session it was greeted with works.
    rig.text(again, R"({"id":2,"op":"setView","args":{"centerHz":0,"spanHz":1000,"widthPx":10}})");
    CHECK(contains(reply_to(second, 2), R"("ok":true)"));
}

TEST_CASE("resume is refused after the first command", "[gpu][webgate][hub]")
{
    REVENANT_NEEDS_GPU();
    HubRig rig;
    FakeSocket socket;
    const auto id = rig.connect(socket);
    rig.text(id, R"({"id":1,"op":"setView","args":{"centerHz":0,"spanHz":1000,"widthPx":10}})");
    rig.text(id, R"({"id":2,"op":"resume","args":{"token":"00"}})");
    CHECK(contains(reply_to(socket, 2), "only as the first command"));
}

TEST_CASE("resume waits for a send already in flight on the socket", "[gpu][webgate][hub]")
{
    REVENANT_NEEDS_GPU();
    HubRig rig;
    FakeSocket first;
    const auto id = rig.connect(first);
    const auto token = rig.hub->token(id);
    rig.hub->disconnect(id);

    // The fresh session's hello is still going out when resume arrives.
    FakeSocket second;
    second.hold();
    const auto again = rig.connect(second);
    REQUIRE(second.busy());
    rig.text(again, R"({"id":1,"op":"resume","args":{"token":")" + token + "\"}}");
    CHECK(second.concurrent_sends == 0);
    CHECK(reply_to(second, 1).empty());

    second.release();
    rig.strand.run_pending();
    CHECK(second.concurrent_sends == 0);
    CHECK(contains(reply_to(second, 1), R"("resumed":true)"));
}

TEST_CASE("resume takes a session from a socket that is still attached", "[gpu][webgate][hub]")
{
    REVENANT_NEEDS_GPU();
    HubRig rig;
    FakeSocket first;
    const auto id = rig.connect(first);
    const auto token = rig.hub->token(id);
    const auto vrx = rig.listen(id, first, true);

    FakeSocket second;
    const auto again = rig.connect(second);
    rig.text(again, R"({"id":1,"op":"resume","args":{"token":")" + token + "\"}}");
    CHECK(contains(reply_to(second, 1), R"("ok":true)"));
    CHECK(first.closed);
    CHECK(rig.hub->sessions() == 1);
    CHECK(rig.hub->session(id) == nullptr);
    CHECK(rig.hub->session(again)->owns(vrx));
}

TEST_CASE("a silent socket is pinged, then closed after 45 s", "[gpu][webgate][hub]")
{
    REVENANT_NEEDS_GPU();
    HubRig rig;
    FakeSocket socket;
    const auto id = rig.connect(socket);
    const auto token = rig.hub->token(id);

    rig.advance(seconds(16));
    bool pinged = false;
    for (const auto& m : socket.sent) {
        pinged = pinged || contains(m.text, R"({"event":"ping"})");
    }
    CHECK(pinged);

    // A pong keeps it alive.
    rig.text(id, R"({"id":5,"op":"pong"})");
    rig.advance(seconds(40));
    CHECK_FALSE(socket.closed);

    rig.advance(seconds(6));
    CHECK(socket.closed);
    CHECK(rig.hub->session(id) == nullptr);
    CHECK(rig.hub->holds(token));
}

TEST_CASE("a detached session with no receivers goes after 30 s", "[gpu][webgate][hub]")
{
    REVENANT_NEEDS_GPU();
    HubRig rig;
    FakeSocket socket;
    const auto id = rig.connect(socket);
    const auto token = rig.hub->token(id);

    rig.hub->disconnect(id);
    rig.advance(seconds(29));
    CHECK(rig.hub->holds(token));
    rig.advance(seconds(2));
    CHECK_FALSE(rig.hub->holds(token));
}

TEST_CASE("keep listening holds an empty detached session for the full linger",
          "[gpu][webgate][hub]")
{
    REVENANT_NEEDS_GPU();
    HubRig rig;
    FakeSocket socket;
    const auto id = rig.connect(socket);
    const auto token = rig.hub->token(id);
    rig.text(id, R"({"id":3,"op":"keepListening","args":{"on":true}})");
    REQUIRE(contains(reply_to(socket, 3), R"("ok":true)"));

    rig.hub->disconnect(id);
    rig.advance(seconds(31));
    CHECK(rig.hub->holds(token));
    rig.advance(seconds(570));
    CHECK_FALSE(rig.hub->holds(token));
}

TEST_CASE("a connection past the cap evicts the oldest detached session",
          "[gpu][webgate][hub]")
{
    REVENANT_NEEDS_GPU();
    revenant::webgate::HubTiming timing;
    timing.max_residents = 3;
    HubRig rig(timing);
    FakeSocket a;
    FakeSocket b;
    FakeSocket c;
    const auto ia = rig.connect(a);
    const auto ib = rig.connect(b);
    const auto ic = rig.connect(c);
    const auto ta = rig.hub->token(ia);
    const auto tb = rig.hub->token(ib);
    const auto tc = rig.hub->token(ic);

    // b drops first, then a: b is the oldest detached session.
    rig.hub->disconnect(ib);
    rig.advance(seconds(1));
    rig.hub->disconnect(ia);
    rig.advance(seconds(1));

    REQUIRE(rig.hub->make_room());
    FakeSocket d;
    rig.connect(d);
    CHECK(rig.hub->sessions() == 3);
    CHECK_FALSE(rig.hub->holds(tb));
    CHECK(rig.hub->holds(ta));
    CHECK(rig.hub->holds(tc));
}

TEST_CASE("a connection past the cap is refused when every session is attached",
          "[gpu][webgate][hub]")
{
    REVENANT_NEEDS_GPU();
    revenant::webgate::HubTiming timing;
    timing.max_residents = 2;
    HubRig rig(timing);
    FakeSocket a;
    FakeSocket b;
    const auto ia = rig.connect(a);
    rig.connect(b);
    const auto ta = rig.hub->token(ia);

    CHECK_FALSE(rig.hub->make_room());
    CHECK(rig.hub->sessions() == 2);
    CHECK(rig.hub->holds(ta));
    CHECK_FALSE(a.closed);

    rig.hub->disconnect(ia);
    CHECK(rig.hub->make_room());
    CHECK(rig.hub->sessions() == 1);
}
