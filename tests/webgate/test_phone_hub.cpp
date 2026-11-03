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

    HubRig()
    {
        revenant::test::HarnessOptions options;
        options.center_hz = 100'000'000;
        const auto opened = harness.open(options);
        INFO(revenant::test::message_of(opened));
        REQUIRE(opened.has_value());
        StreamsConfig config;
        config.hls = &registry;
        config.clock = [this] { return fake_now; };
        hub = std::make_unique<PhoneHub>(strand, config);
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
    for (int i = 0; i < 180; ++i) {
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

TEST_CASE("resume of an expired token fails cleanly", "[gpu][webgate][hub]")
{
    REVENANT_NEEDS_GPU();
    HubRig rig;
    FakeSocket first;
    const auto id = rig.connect(first);
    const auto token = rig.hub->token(id);
    rig.hub->disconnect(id);
    rig.advance(seconds(31));
    REQUIRE_FALSE(rig.hub->holds(token));

    FakeSocket second;
    const auto again = rig.connect(second);
    rig.text(again, R"({"id":1,"op":"resume","args":{"token":")" + token + "\"}}");
    const auto reply = reply_to(second, 1);
    INFO(reply);
    CHECK(contains(reply, R"("ok":false)"));
    CHECK(contains(reply, "no detached session"));
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
