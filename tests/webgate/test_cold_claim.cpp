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
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <kj/async-io.h>
#include <kj/compat/http.h>

#include "core/rpc/client.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"
#include "tools/webgate/gateway.h"
#include "tools/webgate/hls_playlist.h"

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
