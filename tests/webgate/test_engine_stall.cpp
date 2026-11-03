// The phone hub against an engine that stops answering: the 2026-11-03 hang,
// where a call made from the event loop waited for ever and took HTTP, the
// sockets and the pings down with it. tests/rpc/stall_engine.h stands in for
// the engine, so no GPU.

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "core/rpc/client.h"
#include "tests/rpc/stall_engine.h"
#include "tests/webgate/stream_fakes.h"
#include "tools/webgate/hls_playlist.h"
#include "tools/webgate/phone_hub.h"
#include "tools/webgate/protocol.h"

namespace {

using namespace std::chrono_literals;
using revenant::webgate::HlsRegistry;
using revenant::webgate::HubTiming;
using revenant::webgate::OutgoingMessage;
using revenant::webgate::PhoneHub;
using revenant::webgate::StreamsConfig;
using revenant::webgate::test::FakeSocket;
using revenant::webgate::test::ManualStrand;
using Clock = std::chrono::steady_clock;
namespace rpc = revenant::rpc;

[[nodiscard]] std::size_t count_containing(const FakeSocket& socket, std::string_view needle)
{
    std::size_t n = 0;
    for (const auto& message : socket.sent) {
        if (message.kind == OutgoingMessage::Kind::Text &&
            message.text.find(needle) != std::string::npos) {
            ++n;
        }
    }
    return n;
}

[[nodiscard]] std::string reply_to(const FakeSocket& socket, std::uint64_t id)
{
    const std::string prefix = "{\"id\":" + std::to_string(id) + ",";
    for (const auto& message : socket.sent) {
        if (message.kind == OutgoingMessage::Kind::Text && message.text.rfind(prefix, 0) == 0) {
            return message.text;
        }
    }
    return {};
}

[[nodiscard]] double seconds_since(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

struct StallRig {
    revenant::test::StallEngine engine;
    ManualStrand strand;
    HlsRegistry registry;
    Clock::time_point fake_now = Clock::now();
    std::unique_ptr<PhoneHub> hub;

    StallRig()
    {
        HubTiming timing;
        timing.poll_deadline = 300ms;
        timing.stalled_poll_interval = 2s;
        timing.ping_interval = 1s;
        timing.silence_limit = 1h;
        StreamsConfig config;
        config.hls = &registry;
        config.clock = [this] { return fake_now; };
        hub = std::make_unique<PhoneHub>(strand, config, timing);
    }

    ~StallRig() { hub.reset(); }

    std::uint64_t connect(FakeSocket& socket)
    {
        const std::array<std::uint8_t, rpc::kTokenBytes> token{};
        auto client = rpc::Client::connect("127.0.0.1", engine.port(), token);
        REQUIRE(client);
        const auto id = hub->connect(socket, std::move(*client));
        strand.run_pending();
        return id;
    }

    // One tick, with the fake clock moved on by `by`; returns the wall time
    // it took, which is the time the event loop would have been blocked.
    double tick(std::chrono::milliseconds by)
    {
        fake_now += by;
        const auto start = Clock::now();
        hub->tick();
        strand.run_pending();
        return seconds_since(start);
    }
};

}  // namespace

TEST_CASE("a stalled engine costs the loop one poll deadline and pings keep going",
          "[webgate][deadline]")
{
    StallRig rig;
    FakeSocket socket;
    const auto id = rig.connect(socket);
    static_cast<void>(id);
    static_cast<void>(rig.tick(250ms));
    REQUIRE_FALSE(rig.hub->engine_stalled());

    rig.engine.answer(false);

    // The first poll of the stall waits its deadline once, however many calls
    // the tick would have made, and the ping goes out regardless.
    const auto pings_before = count_containing(socket, R"("event":"ping")");
    const double first = rig.tick(1100ms);
    CHECK(first < 1.5);
    CHECK(rig.hub->engine_stalled());
    CHECK(rig.hub->polls_timed_out() == 1);
    CHECK(count_containing(socket, R"("event":"ping")") == pings_before + 1);

    // Inside the stalled interval the polls are skipped: no wait at all, and
    // pings still go out on time.
    const double skipped = rig.tick(1100ms);
    CHECK(skipped < 0.2);
    CHECK(rig.hub->polls_timed_out() == 1);
    CHECK(count_containing(socket, R"("event":"ping")") == pings_before + 2);

    // Past it, the poll is retried, and times out again.
    static_cast<void>(rig.tick(1100ms));
    CHECK(rig.hub->polls_timed_out() == 2);

    // The engine comes back: the next poll gets through and the streak ends.
    rig.engine.answer(true);
    static_cast<void>(rig.tick(2100ms));
    CHECK_FALSE(rig.hub->engine_stalled());
    CHECK(rig.hub->polls_timed_out() == 2);
}

TEST_CASE("a command the engine does not answer gets an error reply, and the session "
          "works afterwards",
          "[webgate][deadline]")
{
    StallRig rig;
    FakeSocket socket;
    const auto id = rig.connect(socket);

    rig.engine.answer(false);
    const auto start = Clock::now();
    rig.hub->text(id, R"({"id":41,"op":"setCenter","args":{"hz":100000000}})");
    rig.strand.run_pending();
    const double took = seconds_since(start);

    const std::string reply = reply_to(socket, 41);
    INFO(reply);
    CHECK(reply.find(R"("ok":false)") != std::string::npos);
    CHECK(reply.find("did not answer within 5 s") != std::string::npos);
    // One deadline for the command, not one per call it and its follow-up
    // refresh would have made.
    CHECK(took < static_cast<double>(rpc::kCallDeadline.count()) / 1000.0 + 1.5);

    // The same client, reused: the engine answers again and so does the
    // session. setCenter is not implemented by the stand-in, so the reply is
    // the engine's refusal rather than a timeout.
    rig.engine.answer(true);
    rig.hub->text(id, R"({"id":42,"op":"setCenter","args":{"hz":100000000}})");
    rig.strand.run_pending();
    const std::string again = reply_to(socket, 42);
    INFO(again);
    REQUIRE_FALSE(again.empty());
    CHECK(again.find("did not answer") == std::string::npos);
    CHECK(rig.hub->session(id)->client().info());
}
