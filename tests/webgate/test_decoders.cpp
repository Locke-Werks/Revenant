// listDecoders and setDecoders, through a hub on the in-process engine of
// tests/rpc/rpc_fixture.h: the reply, the session's record, the decoders the
// stream layer actually subscribed, and that the choice outlives a resume and
// an engine restart. The restart is the harness being destroyed and a second
// one opened, as in test_recreate.cpp.

#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

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
using Names = std::set<std::string>;

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

struct Rig {
    std::unique_ptr<revenant::test::Harness> harness;
    ManualStrand strand;
    HlsRegistry registry;
    std::unique_ptr<PhoneHub> hub;
    Clock::time_point fake_now = Clock::now();
    std::uint64_t next_id = 100;

    Rig()
    {
        open_engine();
        StreamsConfig config;
        config.hls = &registry;
        config.clock = [this] { return fake_now; };
        hub = std::make_unique<PhoneHub>(
            strand, config, revenant::webgate::HubTiming{},
            [this]() -> revenant::Expected<std::unique_ptr<rpc::Client>> {
                if (!harness) {
                    return std::unexpected(revenant::Error{
                        "nothing is listening", 0, revenant::ErrorCategory::Unreachable});
                }
                return harness->connect_another();
            });
        hub->link_state(LinkState::Up, "");
    }

    ~Rig() { hub.reset(); }

    void open_engine()
    {
        harness = std::make_unique<revenant::test::Harness>();
        revenant::test::HarnessOptions options;
        options.center_hz = kCenter;
        const auto opened = harness->open(options);
        INFO(revenant::test::message_of(opened));
        REQUIRE(opened.has_value());
    }

    void restart()
    {
        harness.reset();
        hub->link_state(LinkState::Waiting, "the engine stopped");
        strand.run_pending();
        open_engine();
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

    // Sends one command and answers with its reply.
    std::string send(std::uint64_t connection, FakeSocket& socket, const std::string& op,
                     const std::string& args)
    {
        const auto id = next_id++;
        hub->text(connection, R"({"id":)" + std::to_string(id) + R"(,"op":")" + op +
                                  R"(","args":)" + args + "}");
        strand.run_pending();
        return reply_to(socket, id);
    }

    std::uint64_t add(std::uint64_t connection, FakeSocket& socket, std::int64_t hz,
                      std::string_view mode)
    {
        const auto reply = send(connection, socket, "addVrx",
                                R"({"hz":)" + std::to_string(hz) + R"(,"mode":")" +
                                    std::string(mode) + R"("})");
        INFO(reply);
        REQUIRE(contains(reply, R"("ok":true)"));
        return vrx_of(reply);
    }

    std::string set(std::uint64_t connection, FakeSocket& socket, std::uint64_t vrx,
                    const std::string& names)
    {
        return send(connection, socket, "setDecoders",
                    R"({"vrx":)" + std::to_string(vrx) + R"(,"names":)" + names + "}");
    }
};

}  // namespace

TEST_CASE("listDecoders names each decoder, its input and its modes", "[gpu][webgate][decoders]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    FakeSocket socket;
    const auto id = rig.connect(socket);

    const auto reply = rig.send(id, socket, "listDecoders", "{}");
    INFO(reply);
    REQUIRE(contains(reply, R"("ok":true)"));
    CHECK(contains(reply, R"("name":"p25p1")"));
    CHECK(contains(reply, R"("name":"pocsag")"));
    CHECK(contains(reply, R"("modes":["p25p1","raw"])"));
    CHECK(contains(reply, R"("input":"complexBaseband")"));
    CHECK(contains(reply, R"("input":"realAudio")"));
    // P25's vocoder is built in, so its voice is always listed as playing.
    CHECK(contains(reply, R"("voiceModes":["p25p1")"));
}

TEST_CASE("an analog receiver decodes nothing and a digital voice one its own mode",
          "[gpu][webgate][decoders]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    FakeSocket socket;
    const auto id = rig.connect(socket);

    const auto analog = rig.add(id, socket, kCenter + 100'000, "nfm");
    const auto dv = rig.add(id, socket, kCenter + 200'000, "p25p1");
    auto* session = rig.hub->session(id);
    auto* streams = rig.hub->streams(id);
    REQUIRE(session != nullptr);
    REQUIRE(streams != nullptr);

    CHECK(session->decoders(analog).empty());
    CHECK(streams->decoding(analog).empty());
    CHECK(session->decoders(dv) == std::vector<std::string>{"p25p1"});
    CHECK(session->decoder_choice(dv) == nullptr);
    CHECK(streams->decoding(dv) == Names{"p25p1"});
}

TEST_CASE("setDecoders replaces a receiver's set and refuses what its mode cannot feed",
          "[gpu][webgate][decoders]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    FakeSocket socket;
    const auto id = rig.connect(socket);
    const auto vrx = rig.add(id, socket, kCenter + 100'000, "nfm");
    auto* session = rig.hub->session(id);
    auto* streams = rig.hub->streams(id);

    const auto ok = rig.set(id, socket, vrx, R"(["pocsag","pocsag"])");
    INFO(ok);
    REQUIRE(contains(ok, R"("ok":true)"));
    CHECK(contains(ok, R"("decoders":["pocsag"])"));
    CHECK(session->decoders(vrx) == std::vector<std::string>{"pocsag"});
    CHECK(streams->decoding(vrx) == Names{"pocsag"});

    for (const std::string names : {R"(["nonesuch"])", R"(["p25p1"])", R"([""])", R"("pocsag")",
                                    R"([1])"}) {
        INFO(names);
        CHECK(contains(rig.set(id, socket, vrx, names), R"("ok":false)"));
        CHECK(session->decoders(vrx) == std::vector<std::string>{"pocsag"});
    }
    CHECK(contains(rig.set(id, socket, vrx, R"(["p25p1"])"), "does not read a nfm receiver"));
    CHECK(contains(rig.set(id, socket, vrx, R"(["nonesuch"])"), "no decoder called"));
    CHECK(contains(rig.set(id, socket, 999'999, "[]"), "not one of yours"));
    CHECK(contains(rig.send(id, socket, "setDecoders", R"({"vrx":)" + std::to_string(vrx) + "}"),
                   "names"));

    // The engine's p25p1 receiver can feed m17 as well as its own decoder.
    const auto dv = rig.add(id, socket, kCenter + 200'000, "p25p1");
    REQUIRE(contains(rig.set(id, socket, dv, R"(["p25p1","m17"])"), R"("ok":true)"));
    CHECK(streams->decoding(dv) == Names{"p25p1", "m17"});

    // An empty set is a choice too, and turns off even a mode's default.
    REQUIRE(contains(rig.set(id, socket, dv, "[]"), R"("ok":true)"));
    CHECK(session->decoders(dv).empty());
    CHECK(streams->decoding(dv).empty());
}

TEST_CASE("a decoder choice survives a new width and not a new mode", "[gpu][webgate][decoders]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    FakeSocket socket;
    const auto id = rig.connect(socket);
    const auto vrx = rig.add(id, socket, kCenter + 100'000, "nfm");
    REQUIRE(contains(rig.set(id, socket, vrx, R"(["pocsag"])"), R"("ok":true)"));

    const auto wider = rig.send(id, socket, "setBandwidth",
                                R"({"vrx":)" + std::to_string(vrx) + R"(,"hz":25000})");
    INFO(wider);
    REQUIRE(contains(wider, R"("ok":true)"));
    CHECK(contains(wider, R"("decoders":["pocsag"])"));
    CHECK(rig.hub->streams(id)->decoding(vrx_of(wider)) == Names{"pocsag"});

    const auto moded = rig.send(id, socket, "setMode",
                                R"({"vrx":)" + std::to_string(vrx_of(wider)) + R"(,"mode":"dmr"})");
    INFO(moded);
    REQUIRE(contains(moded, R"("ok":true)"));
    CHECK(contains(moded, R"("decoders":["dmr"])"));
    CHECK(rig.hub->streams(id)->decoding(vrx_of(moded)) == Names{"dmr"});
}

TEST_CASE("a decoder choice outlives a resume", "[gpu][webgate][decoders]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    FakeSocket first;
    const auto id = rig.connect(first);
    const auto token = rig.hub->token(id);
    const auto vrx = rig.add(id, first, kCenter + 100'000, "nfm");
    REQUIRE(contains(rig.set(id, first, vrx, R"(["pocsag"])"), R"("ok":true)"));
    rig.hub->disconnect(id);

    FakeSocket second;
    const auto again = rig.connect(second);
    rig.hub->text(again, R"({"id":1,"op":"resume","args":{"token":")" + token + "\"}}");
    rig.strand.run_pending();
    REQUIRE(contains(reply_to(second, 1), R"("ok":true)"));
    CHECK(rig.hub->session(again)->decoders(vrx) == std::vector<std::string>{"pocsag"});
    CHECK(rig.hub->streams(again)->decoding(vrx) == Names{"pocsag"});
}

TEST_CASE("an engine restart re-applies each receiver's decoders", "[gpu][webgate][decoders]")
{
    REVENANT_NEEDS_GPU();
    Rig rig;
    FakeSocket socket;
    const auto id = rig.connect(socket);
    const auto chosen = rig.add(id, socket, kCenter + 100'000, "nfm");
    const auto defaulted = rig.add(id, socket, kCenter + 200'000, "p25p1");
    const auto plain = rig.add(id, socket, kCenter + 300'000, "am");
    REQUIRE(contains(rig.set(id, socket, chosen, R"(["pocsag"])"), R"("ok":true)"));

    rig.restart();

    auto* session = rig.hub->session(id);
    auto* streams = rig.hub->streams(id);
    REQUIRE(session != nullptr);
    REQUIRE(streams != nullptr);
    REQUIRE(session->owned_ids().size() == 3);
    auto& client = rig.harness->client();
    int seen = 0;
    for (const auto vrx : session->owned_ids()) {
        const auto status = client.vrx_status(vrx);
        REQUIRE(status.has_value());
        INFO(status->params.center);
        if (status->params.center == 100'000) {
            CHECK(streams->decoding(vrx) == Names{"pocsag"});
            ++seen;
        } else if (status->params.center == 200'000) {
            CHECK(streams->decoding(vrx) == Names{"p25p1"});
            ++seen;
        } else if (status->params.center == 300'000) {
            CHECK(streams->decoding(vrx).empty());
            ++seen;
        }
    }
    CHECK(seen == 3);
    static_cast<void>(defaulted);
    static_cast<void>(plain);
}
