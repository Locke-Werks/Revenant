// A phone's receivers rebuilt after the engine restarts. The in-process
// engine is stopped by destroying its harness, which drops every phone's
// connection exactly as a dead engine process does, and a second harness
// stands in for the restarted engine; the hub reaches it through new_session.

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

namespace rpc = revenant::rpc;
using revenant::webgate::HlsRegistry;
using revenant::webgate::LinkState;
using revenant::webgate::OutgoingMessage;
using revenant::webgate::PhoneHub;
using revenant::webgate::StreamsConfig;
using revenant::webgate::test::FakeSocket;
using revenant::webgate::test::ManualStrand;
using Kind = OutgoingMessage::Kind;
using Clock = std::chrono::steady_clock;

constexpr std::int64_t kCenter = 100'000'000;

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

// The texts sent from index `from` on that contain needle.
[[nodiscard]] std::vector<std::string> texts_with(const FakeSocket& socket, std::string_view needle,
                                                  std::size_t from = 0)
{
    std::vector<std::string> out;
    for (std::size_t i = from; i < socket.sent.size(); ++i) {
        if (socket.sent[i].kind == Kind::Text && contains(socket.sent[i].text, needle)) {
            out.push_back(socket.sent[i].text);
        }
    }
    return out;
}

struct RestartRig {
    std::unique_ptr<revenant::test::Harness> harness;
    ManualStrand strand;
    HlsRegistry registry;
    std::unique_ptr<PhoneHub> hub;

    RestartRig()
    {
        open_engine(kCenter);
        StreamsConfig config;
        config.hls = &registry;
        hub = std::make_unique<PhoneHub>(strand, config, revenant::webgate::HubTiming{},
                                         [this] { return harness->connect_another(); });
        hub->link_state(LinkState::Up, "");
    }

    ~RestartRig() { hub.reset(); }

    void open_engine(std::int64_t center)
    {
        harness = std::make_unique<revenant::test::Harness>();
        revenant::test::HarnessOptions options;
        options.center_hz = center;
        const auto opened = harness->open(options);
        INFO(revenant::test::message_of(opened));
        REQUIRE(opened.has_value());
    }

    // The engine goes away and the link says so, then a new one comes up.
    void restart(std::int64_t center)
    {
        harness.reset();
        hub->link_state(LinkState::Waiting, "the engine stopped");
        strand.run_pending();
        open_engine(center);
        hub->link_state(LinkState::Up, "");
        strand.run_pending();
    }

    std::uint64_t connect(FakeSocket& socket)
    {
        auto client = harness->connect_another();
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

    std::uint64_t add(std::uint64_t connection, FakeSocket& socket, std::uint64_t id,
                      std::int64_t hz)
    {
        text(connection, R"({"id":)" + std::to_string(id) +
                             R"(,"op":"addVrx","args":{"hz":)" + std::to_string(hz) +
                             R"(,"mode":"am"}})");
        const auto reply = reply_to(socket, id);
        INFO(reply);
        REQUIRE(contains(reply, R"("ok":true)"));
        return vrx_of(reply);
    }

    [[nodiscard]] std::vector<std::uint64_t> engine_ids()
    {
        const auto ids = harness->client().vrx_ids();
        REQUIRE(ids.has_value());
        return *ids;
    }
};

}  // namespace

TEST_CASE("an engine restart recreates each phone's receivers", "[gpu][webgate][recreate]")
{
    REVENANT_NEEDS_GPU();
    RestartRig rig;

    FakeSocket socket;
    const auto id = rig.connect(socket);
    const auto first = rig.add(id, socket, 1, kCenter + 100'000);
    const auto second = rig.add(id, socket, 2, kCenter - 200'000);
    rig.text(id, R"({"id":3,"op":"audio","args":{"vrx":)" + std::to_string(first) + "}}");
    rig.text(id, R"({"id":4,"op":"keepListening","args":{"on":true}})");
    rig.text(id, R"({"id":5,"op":"setVrxTranscribe","args":{"vrx":)" + std::to_string(second) +
                     R"(,"choice":"off"}})");
    REQUIRE(contains(reply_to(socket, 5), R"("ok":true)"));
    REQUIRE(rig.registry.find(first) != nullptr);

    // A second phone, detached when the engine goes: it is rebuilt too.
    FakeSocket other;
    const auto other_id = rig.connect(other);
    const auto theirs = rig.add(other_id, other, 1, kCenter + 300'000);
    rig.hub->disconnect(other_id);

    const auto mark = socket.sent.size();
    rig.restart(kCenter);

    const auto ups = texts_with(socket, R"("state":"up")", mark);
    REQUIRE(ups.size() == 1);
    INFO(ups.front());
    CHECK(contains(ups.front(), R"("recreated":[{"old":)" + std::to_string(first)));
    CHECK(contains(ups.front(), R"("lost":[])"));
    CHECK(texts_with(socket, R"("state":"waiting")", mark).size() == 1);

    auto* session = rig.hub->session(id);
    REQUIRE(session != nullptr);
    const auto owned = session->owned_ids();
    REQUIRE(owned.size() == 2);
    REQUIRE(session->audio_vrx().has_value());
    const auto new_first = *session->audio_vrx();
    const auto new_second = owned.front() == new_first ? owned.back() : owned.front();
    CHECK(contains(ups.front(), R"({"old":)" + std::to_string(first) + R"(,"new":)" +
                                    std::to_string(new_first) + "}"));
    CHECK(contains(ups.front(), R"({"old":)" + std::to_string(second) + R"(,"new":)" +
                                    std::to_string(new_second) + "}"));

    // Same frequency, mode and transcription choice, on the new engine.
    auto& client = rig.harness->client();
    const auto a = client.vrx_status(new_first);
    REQUIRE(a.has_value());
    CHECK(a->params.center == 100'000);
    CHECK(a->params.demod == rpc::Demod::Am);
    const auto b = client.vrx_status(new_second);
    REQUIRE(b.has_value());
    CHECK(b->params.center == -200'000);
    CHECK(b->transcribe == rpc::TranscribeChoice::Off);

    // Keep listening followed the new id: a segmenter under it, and the phone
    // was told the new URL. (A fresh engine numbers from 1 again, so the new
    // id may well equal the old one.)
    CHECK(rig.registry.find(new_first) != nullptr);
    CHECK(texts_with(socket, "/hls/" + std::to_string(new_first) + "/live.m3u8", mark).size() ==
          1);

    // Both phones' receivers exist, and nothing else.
    auto ids = rig.engine_ids();
    std::ranges::sort(ids);
    CHECK(ids.size() == 3);
    CHECK(std::ranges::find(ids, new_first) != ids.end());
    CHECK(std::ranges::find(ids, new_second) != ids.end());
    static_cast<void>(theirs);

    // The phone's commands reach the new engine under the new id.
    rig.text(id, R"({"id":6,"op":"removeVrx","args":{"vrx":)" + std::to_string(new_second) + "}}");
    CHECK(contains(reply_to(socket, 6), R"("ok":true)"));
    CHECK(rig.engine_ids().size() == 2);
}

TEST_CASE("a receiver the new engine refuses is reported lost", "[gpu][webgate][recreate]")
{
    REVENANT_NEEDS_GPU();
    RestartRig rig;
    FakeSocket socket;
    const auto id = rig.connect(socket);
    const auto vrx = rig.add(id, socket, 1, kCenter + 100'000);

    // The restarted engine's source is 100 MHz away, so the frequency the
    // phone asked for is not in it.
    const auto mark = socket.sent.size();
    rig.restart(kCenter + 100'000'000);

    const auto ups = texts_with(socket, R"("state":"up")", mark);
    REQUIRE(ups.size() == 1);
    INFO(ups.front());
    CHECK(contains(ups.front(), R"("recreated":[])"));
    CHECK(contains(ups.front(), R"("lost":[{"vrx":)" + std::to_string(vrx) + R"(,"reason":")"));
    const auto removed = texts_with(socket, R"("event":"vrxRemoved")", mark);
    REQUIRE(removed.size() == 1);
    CHECK(contains(removed.front(), R"("vrx":)" + std::to_string(vrx)));
    CHECK(rig.hub->session(id)->owned_ids().empty());
    CHECK(rig.engine_ids().empty());
}

TEST_CASE("a link that comes back without a restart recreates nothing",
          "[gpu][webgate][recreate]")
{
    REVENANT_NEEDS_GPU();
    RestartRig rig;
    FakeSocket socket;
    const auto id = rig.connect(socket);
    const auto vrx = rig.add(id, socket, 1, kCenter + 100'000);

    const auto mark = socket.sent.size();
    rig.hub->link_state(LinkState::Waiting, "a missed probe");
    rig.hub->link_state(LinkState::Up, "");
    rig.strand.run_pending();

    const auto ups = texts_with(socket, R"("state":"up")", mark);
    REQUIRE(ups.size() == 1);
    CHECK_FALSE(contains(ups.front(), "recreated"));
    CHECK(rig.hub->session(id)->owns(vrx));
    CHECK(rig.engine_ids() == std::vector<std::uint64_t>{vrx});
}
