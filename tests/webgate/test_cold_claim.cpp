// A phone's first claim waiting for an engine to start, as the gateway serves
// it: off the event loop, with the phone told the engine is starting, and
// dropped cleanly when the phone leaves first.
//
// No <windows.h> here: gateway.h has to come before it (http.h says why).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <kj/async-io.h>
#include <kj/compat/http.h>

#include "core/rpc/client.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"
#include "tools/webgate/gateway.h"
#include "tools/webgate/hls_playlist.h"
#include "tools/webgate/phone_hub.h"
#include "tests/webgate/stream_fakes.h"

namespace {

namespace rpc = revenant::rpc;
using namespace std::chrono_literals;
using revenant::webgate::Gateway;
using revenant::webgate::HlsRegistry;

// A claim that does not return until released: an engine taking its time.
struct SlowStart {
    std::mutex mutex;
    std::condition_variable released_cv;
    bool released = false;
    std::atomic<bool> entered{false};
    std::function<revenant::Expected<std::unique_ptr<rpc::Client>>(rpc::LoginMode,
                                                                   const std::string&)>
        then;

    revenant::Expected<std::unique_ptr<rpc::Client>> claim(rpc::LoginMode mode,
                                                           const std::string& holder)
    {
        entered = true;
        std::unique_lock lock(mutex);
        released_cv.wait(lock, [this] { return released; });
        lock.unlock();
        return then(mode, holder);
    }

    void release()
    {
        {
            const std::lock_guard lock(mutex);
            released = true;
        }
        released_cv.notify_all();
    }
};

[[nodiscard]] std::string text_of(kj::WebSocket::Message& message)
{
    if (!message.is<kj::String>()) {
        return {};
    }
    const auto& text = message.get<kj::String>();
    return std::string(text.cStr(), text.size());
}

}  // namespace

TEST_CASE("the loop keeps serving while a phone's claim waits for the engine",
          "[webgate][coldstart]")
{
    auto io = kj::setupAsyncIo();
    HlsRegistry hls;
    SlowStart slow;
    slow.then = [](rpc::LoginMode, const std::string&)
        -> revenant::Expected<std::unique_ptr<rpc::Client>> {
        return std::unexpected(
            revenant::Error{"nothing is listening", 0, revenant::ErrorCategory::Unreachable});
    };
    Gateway gateway(
        io, [&slow](rpc::LoginMode mode, const std::string& holder) {
            return slow.claim(mode, holder);
        },
        hls);

    auto pipe = kj::newWebSocketPipe();
    auto phone = kj::mv(pipe.ends[1]);
    auto accepted = gateway.accept(kj::mv(pipe.ends[0]), "10.1.10.57").eagerlyEvaluate(nullptr);

    // Told at once, while the claim is still waiting.
    auto first = phone->receive().wait(io.waitScope);
    CHECK(text_of(first).find(R"("event":"engine")") != std::string::npos);
    CHECK(text_of(first).find(R"("state":"starting")") != std::string::npos);

    // Other work on the loop runs: a timer, and a second phone's socket.
    io.provider->getTimer().afterDelay(200 * kj::MILLISECONDS).wait(io.waitScope);
    auto other = kj::newWebSocketPipe();
    auto other_phone = kj::mv(other.ends[1]);
    auto other_accepted =
        gateway.accept(kj::mv(other.ends[0]), "10.1.10.58").eagerlyEvaluate(nullptr);
    auto other_first = other_phone->receive().wait(io.waitScope);
    CHECK(text_of(other_first).find(R"("event":"engine")") != std::string::npos);
    CHECK(slow.entered);

    // The phone's commands meanwhile are held, not lost or refused.
    phone->send(kj::StringPtr(R"({"id":1,"op":"pong"})")).wait(io.waitScope);

    // The engine never came: the phone is told so and closed.
    slow.release();
    auto second = phone->receive().wait(io.waitScope);
    CHECK(text_of(second).find(R"("event":"link")") != std::string::npos);
    CHECK(text_of(second).find("nothing is listening") != std::string::npos);
    auto third = phone->receive().wait(io.waitScope);
    CHECK(third.is<kj::WebSocket::Close>());
    accepted.wait(io.waitScope);

    static_cast<void>(other_phone->receive().wait(io.waitScope));
    other_phone = nullptr;
    other_accepted.wait(io.waitScope);
    gateway.shutdown();
}

TEST_CASE("a phone that leaves while the engine starts drops its claim",
          "[gpu][webgate][coldstart]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::HarnessOptions options;
    options.center_hz = 100'000'000;
    options.connect_client = false;
    revenant::test::Harness harness;
    const auto opened = harness.open(options);
    INFO(revenant::test::message_of(opened));
    REQUIRE(opened.has_value());

    auto io = kj::setupAsyncIo();
    HlsRegistry hls;
    SlowStart slow;
    slow.then = [&harness](rpc::LoginMode mode, const std::string& holder) {
        return harness.connect_as(mode, holder);
    };
    Gateway gateway(
        io, [&slow](rpc::LoginMode mode, const std::string& holder) {
            return slow.claim(mode, holder);
        },
        hls);

    auto pipe = kj::newWebSocketPipe();
    auto phone = kj::mv(pipe.ends[1]);
    auto accepted = gateway.accept(kj::mv(pipe.ends[0]), "10.1.10.57").eagerlyEvaluate(nullptr);
    static_cast<void>(phone->receive().wait(io.waitScope));

    // Gone before the engine answered: the connection ends without waiting
    // for the claim.
    phone = nullptr;
    accepted.wait(io.waitScope);

    // The engine then answers. The claim's connection is dropped, which ends
    // its lease, and no session is left behind for it.
    slow.release();
    gateway.join_claims();
    auto observer = harness.connect_as(rpc::LoginMode::Observe, "check");
    REQUIRE(observer.has_value());
    bool free = false;
    for (int i = 0; i < 100 && !free; ++i) {
        const auto status = (*observer)->lease_status();
        REQUIRE(status.has_value());
        free = !status->held;
        if (!free) {
            std::this_thread::sleep_for(50ms);
        }
    }
    CHECK(free);
    gateway.shutdown();
}

namespace {

using revenant::webgate::PhoneHub;
using revenant::webgate::test::FakeSocket;
using revenant::webgate::test::ManualStrand;
using ClaimDone = std::function<void(revenant::Expected<std::unique_ptr<rpc::Client>>)>;

[[nodiscard]] bool sent_containing(const FakeSocket& socket, std::string_view needle)
{
    for (const auto& message : socket.sent) {
        if (message.kind == revenant::webgate::OutgoingMessage::Kind::Text &&
            message.text.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// A refused phone on a hub whose takeOver claims are held by the case, which
// plays the worker thread: the hub's calls return while the claim is pending.
struct TakeOverRig {
    revenant::test::Harness harness;
    ManualStrand strand;
    HlsRegistry registry;
    std::unique_ptr<PhoneHub> hub;
    std::unique_ptr<rpc::Client> desktop;
    std::vector<std::pair<std::string, ClaimDone>> pending;

    TakeOverRig()
    {
        revenant::test::HarnessOptions options;
        options.center_hz = 100'000'000;
        options.connect_client = false;
        const auto opened = harness.open(options);
        INFO(revenant::test::message_of(opened));
        REQUIRE(opened.has_value());
        revenant::webgate::StreamsConfig config;
        config.hls = &registry;
        hub = std::make_unique<PhoneHub>(
            strand, config, revenant::webgate::HubTiming{},
            [this](rpc::LoginMode mode, const std::string& holder) {
                return harness.connect_as(mode, holder);
            });
        hub->set_claim_runner(
            [this](rpc::LoginMode, std::string holder, ClaimDone done) {
                pending.emplace_back(std::move(holder), std::move(done));
            });
        auto held = harness.connect_as(rpc::LoginMode::Claim, "Revenant desktop on TEST");
        REQUIRE(held.has_value());
        desktop = std::move(*held);
    }

    ~TakeOverRig()
    {
        hub.reset();
        desktop.reset();
    }

    void land()
    {
        auto [holder, done] = std::move(pending.front());
        pending.erase(pending.begin());
        done(harness.connect_as(rpc::LoginMode::TakeOver, holder));
        strand.run_pending();
    }
};

}  // namespace

TEST_CASE("a takeOver waiting for the engine does not hold up the hub",
          "[gpu][webgate][coldstart]")
{
    REVENANT_NEEDS_GPU();
    TakeOverRig rig;
    FakeSocket phone;
    const auto id = rig.hub->admit(phone, "phone 10.1.10.57 via webgate");
    REQUIRE(id.has_value());
    rig.strand.run_pending();
    REQUIRE(sent_containing(phone, R"("state":"inUse")"));

    rig.hub->text(*id, R"({"id":2,"op":"takeOver"})");
    // Returned with the claim still out: told the engine is starting, no
    // reply yet, and a command meanwhile is held rather than refused.
    REQUIRE(rig.pending.size() == 1);
    CHECK(sent_containing(phone, R"("event":"engine")"));
    CHECK_FALSE(sent_containing(phone, R"({"id":2,)"));
    rig.hub->text(*id, R"({"id":3,"op":"noSuchOp"})");
    CHECK_FALSE(sent_containing(phone, R"({"id":3,)"));

    // Other phones are served meanwhile.
    FakeSocket other;
    const auto other_id = rig.hub->admit(other, "phone 10.1.10.58 via webgate");
    CHECK(other_id.has_value());

    rig.land();
    CHECK(sent_containing(phone, R"({"id":2,"ok":true)"));
    CHECK(sent_containing(phone, R"({"id":3,)"));
    CHECK(rig.hub->session(*id) != nullptr);
}

TEST_CASE("a phone gone before its takeOver lands leaves no session and no lease",
          "[gpu][webgate][coldstart]")
{
    REVENANT_NEEDS_GPU();
    TakeOverRig rig;
    FakeSocket phone;
    const auto id = rig.hub->admit(phone, "phone 10.1.10.57 via webgate");
    REQUIRE(id.has_value());
    rig.strand.run_pending();
    rig.hub->text(*id, R"({"id":2,"op":"takeOver"})");
    REQUIRE(rig.pending.size() == 1);

    rig.hub->disconnect(*id);
    rig.land();
    CHECK(rig.hub->sessions() == 0);

    // The takeOver took the lease from the desktop and its Client was dropped
    // with nobody to give it to, so the lease is free.
    auto observer = rig.harness.connect_as(rpc::LoginMode::Observe, "check");
    REQUIRE(observer.has_value());
    bool free = false;
    for (int i = 0; i < 100 && !free; ++i) {
        const auto status = (*observer)->lease_status();
        REQUIRE(status.has_value());
        free = !status->held;
        if (!free) {
            std::this_thread::sleep_for(50ms);
        }
    }
    CHECK(free);
}
