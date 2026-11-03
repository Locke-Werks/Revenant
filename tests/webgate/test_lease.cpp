// The engine's single-user lease as the gateway carries it: the link observes,
// each phone claims under its own holder text, a second phone is refused and
// offered Take over, a phone taken over is told who by, the lease ends with
// the phone's linger, and an engine restart claims it back for the phone.
//
// The harness here has no client of its own: its usual one claims as
// "unknown client" and would hold the lease against every phone.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/rpc/client.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"
#include "tests/webgate/stream_fakes.h"
#include "tools/webgate/engine_link.h"
#include "tools/webgate/hls_playlist.h"
#include "tools/webgate/phone_hub.h"
#include "tools/webgate/protocol.h"

namespace {

namespace rpc = revenant::rpc;
using namespace std::chrono_literals;
using revenant::webgate::HlsRegistry;
using revenant::webgate::LinkState;
using revenant::webgate::OutgoingMessage;
using revenant::webgate::PhoneHub;
using revenant::webgate::phone_holder;
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

[[nodiscard]] revenant::test::HarnessOptions lease_options()
{
    revenant::test::HarnessOptions options;
    options.center_hz = kCenter;
    options.connect_client = false;
    return options;
}

const std::string kPhoneA = phone_holder("10.1.10.57");
const std::string kPhoneB = phone_holder("10.1.10.58");

struct LeaseRig {
    std::unique_ptr<revenant::test::Harness> harness;
    ManualStrand strand;
    HlsRegistry registry;
    Clock::time_point fake_now = Clock::now();
    std::unique_ptr<PhoneHub> hub;
    // Reads the lease without holding it.
    std::unique_ptr<rpc::Client> observer;

    LeaseRig()
    {
        open_engine();
        StreamsConfig config;
        config.hls = &registry;
        config.clock = [this] { return fake_now; };
        hub = std::make_unique<PhoneHub>(
            strand, config, revenant::webgate::HubTiming{},
            [this](rpc::LoginMode mode,
                   const std::string& holder) -> revenant::Expected<std::unique_ptr<rpc::Client>> {
                if (!harness) {
                    return std::unexpected(revenant::Error{
                        "nothing is listening", 0, revenant::ErrorCategory::Unreachable});
                }
                return harness->connect_as(mode, holder);
            });
        hub->link_state(LinkState::Up, "");
    }

    ~LeaseRig()
    {
        hub.reset();
        observer.reset();
    }

    void open_engine()
    {
        observer.reset();
        harness = std::make_unique<revenant::test::Harness>();
        const auto opened = harness->open(lease_options());
        INFO(revenant::test::message_of(opened));
        REQUIRE(opened.has_value());
        auto watching = harness->connect_as(rpc::LoginMode::Observe, "lease test");
        INFO(revenant::test::message_of(watching));
        REQUIRE(watching.has_value());
        observer = std::move(*watching);
    }

    [[nodiscard]] rpc::LeaseStatus lease()
    {
        const auto status = observer->lease_status();
        INFO(revenant::test::message_of(status));
        REQUIRE(status.has_value());
        return *status;
    }

    std::uint64_t admit(FakeSocket& socket, const std::string& holder)
    {
        const auto id = hub->admit(socket, holder);
        INFO(revenant::test::message_of(id));
        REQUIRE(id.has_value());
        strand.run_pending();
        return *id;
    }

    void text(std::uint64_t connection, const std::string& line)
    {
        hub->text(connection, line);
        strand.run_pending();
    }

    // Pumps the strand until done says so or the ceiling passes. Engine
    // notices arrive on the client's own thread, so this is a wait.
    bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds limit = 10s)
    {
        const auto until = Clock::now() + limit;
        while (Clock::now() < until) {
            strand.run_pending();
            if (done()) {
                return true;
            }
            std::this_thread::sleep_for(20ms);
        }
        strand.run_pending();
        return done();
    }
};

}  // namespace

TEST_CASE("an idle gateway's link observes and holds no lease", "[gpu][webgate][lease]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    const auto opened = harness.open(lease_options());
    INFO(revenant::test::message_of(opened));
    REQUIRE(opened.has_value());

    revenant::webgate::EngineLinkConfig config;
    config.port = harness.port();
    config.token = revenant::test::test_token();
    config.poll_interval = 100ms;
    revenant::webgate::EngineLink link(config);
    std::mutex mutex;
    bool up = false;
    link.on_state([&](LinkState state, const std::string&) {
        const std::lock_guard lock(mutex);
        up = up || state == LinkState::Up;
    });
    link.start();
    const auto deadline = Clock::now() + 30s;
    for (;;) {
        {
            const std::lock_guard lock(mutex);
            if (up) {
                break;
            }
        }
        REQUIRE(Clock::now() < deadline);
        std::this_thread::sleep_for(20ms);
    }

    // Several probes go by: the link's liveness check must be one an
    // observer is allowed, or the link would drop and reconnect each time.
    std::this_thread::sleep_for(500ms);
    CHECK(link.state() == LinkState::Up);

    auto watching = harness.connect_as(rpc::LoginMode::Observe, "lease test");
    REQUIRE(watching.has_value());
    const auto status = (*watching)->lease_status();
    REQUIRE(status.has_value());
    CHECK_FALSE(status->held);

    // So the desktop can start while the gateway runs.
    auto desktop = harness.connect_as(rpc::LoginMode::Claim, "Revenant desktop on TEST");
    INFO(revenant::test::message_of(desktop));
    CHECK(desktop.has_value());
}

TEST_CASE("the first phone claims as itself", "[gpu][webgate][lease]")
{
    REVENANT_NEEDS_GPU();
    LeaseRig rig;
    CHECK_FALSE(rig.lease().held);

    FakeSocket socket;
    const auto id = rig.admit(socket, kPhoneA);
    REQUIRE(rig.hub->session(id) != nullptr);
    REQUIRE_FALSE(socket.sent.empty());
    CHECK(contains(socket.sent.front().text, R"("event":"lease","state":"held")"));

    const auto lease = rig.lease();
    CHECK(lease.held);
    CHECK(lease.holder == "phone 10.1.10.57 via webgate");

    // The same phone again (a reload, a resume's fresh session) joins its
    // own lease rather than being refused.
    FakeSocket again;
    const auto second = rig.admit(again, kPhoneA);
    CHECK(rig.hub->session(second) != nullptr);
}

TEST_CASE("a second phone is refused with Take over, which evicts the first",
          "[gpu][webgate][lease]")
{
    REVENANT_NEEDS_GPU();
    LeaseRig rig;

    FakeSocket a;
    const auto a_id = rig.admit(a, kPhoneA);
    const auto a_token = rig.hub->token(a_id);
    rig.text(a_id, R"({"id":1,"op":"addVrx","args":{"hz":100100000,"mode":"am"}})");
    const auto vrx = vrx_of(reply_to(a, 1));

    FakeSocket b;
    const auto b_id = rig.admit(b, kPhoneB);
    CHECK(rig.hub->session(b_id) == nullptr);
    const auto in_use = texts_with(b, R"("event":"lease")");
    REQUIRE(in_use.size() == 1);
    INFO(in_use.front());
    CHECK(contains(in_use.front(), R"("state":"inUse")"));
    CHECK(contains(in_use.front(), R"("holder":"phone 10.1.10.57 via webgate")"));
    CHECK(contains(in_use.front(), R"("since":")"));
    CHECK(contains(in_use.front(), R"("message":"in use: phone 10.1.10.57 via webgate since )"));

    // Nothing but Take over runs while the engine is someone else's.
    rig.text(b_id, R"({"id":7,"op":"addVrx","args":{"hz":100200000}})");
    CHECK(contains(reply_to(b, 7), R"("ok":false)"));
    CHECK(contains(reply_to(b, 7), "in use: phone 10.1.10.57 via webgate"));

    rig.text(b_id, R"({"id":8,"op":"takeOver"})");
    const auto took = reply_to(b, 8);
    INFO(took);
    CHECK(contains(took, R"("ok":true)"));
    CHECK(contains(took, R"("held":true)"));
    REQUIRE(rig.hub->session(b_id) != nullptr);
    CHECK(rig.lease().holder == kPhoneB);
    CHECK(texts_with(b, R"("state":"held")").size() == 1);
    CHECK(texts_with(b, R"("event":"hello")").size() == 1);

    // The first phone hears who took it, and its session is gone.
    REQUIRE(rig.pump_until([&] { return !texts_with(a, R"("state":"lost")").empty(); }));
    const auto lost = texts_with(a, R"("state":"lost")");
    CHECK(contains(lost.front(), R"("holder":"phone 10.1.10.58 via webgate")"));
    CHECK(rig.hub->session(a_id) == nullptr);
    CHECK_FALSE(rig.hub->holds(a_token));
    rig.text(a_id, R"({"id":2,"op":"tune","args":{"vrx":1,"hz":100150000}})");
    CHECK(contains(reply_to(a, 2), "Taken over by phone 10.1.10.58 via webgate"));

    // The new holder inherits the radio as it was.
    REQUIRE(rig.pump_until([&] { return rig.hub->session(b_id)->has_receiver(vrx); }));

    // Take back.
    rig.text(a_id, R"({"id":3,"op":"takeOver"})");
    CHECK(contains(reply_to(a, 3), R"("ok":true)"));
    CHECK(rig.lease().holder == kPhoneA);
    REQUIRE(rig.pump_until([&] { return !texts_with(b, R"("state":"lost")").empty(); }));
    CHECK(rig.hub->session(b_id) == nullptr);
}

TEST_CASE("the lease is released when the phone's linger ends", "[gpu][webgate][lease]")
{
    REVENANT_NEEDS_GPU();
    LeaseRig rig;
    FakeSocket socket;
    const auto id = rig.admit(socket, kPhoneA);
    REQUIRE(rig.lease().held);

    // A dropped socket keeps the lease through the linger.
    rig.hub->disconnect(id);
    rig.fake_now += 5s;
    rig.hub->tick();
    rig.strand.run_pending();
    CHECK(rig.hub->sessions() == 1);
    CHECK(rig.lease().held);

    rig.fake_now += 60s;
    rig.hub->tick();
    rig.strand.run_pending();
    CHECK(rig.hub->sessions() == 0);
    CHECK(rig.pump_until([&] { return !rig.lease().held; }));
}

TEST_CASE("an engine restart claims the lease back for the attached phone",
          "[gpu][webgate][lease]")
{
    REVENANT_NEEDS_GPU();
    LeaseRig rig;
    FakeSocket socket;
    const auto id = rig.admit(socket, kPhoneA);
    rig.text(id, R"({"id":1,"op":"addVrx","args":{"hz":100100000,"mode":"am"}})");
    REQUIRE(contains(reply_to(socket, 1), R"("ok":true)"));

    rig.harness.reset();
    rig.hub->link_state(LinkState::Waiting, "the engine stopped");
    rig.strand.run_pending();
    rig.open_engine();
    CHECK_FALSE(rig.lease().held);
    rig.hub->link_state(LinkState::Up, "");
    rig.strand.run_pending();

    REQUIRE(rig.hub->session(id) != nullptr);
    const auto lease = rig.lease();
    CHECK(lease.held);
    CHECK(lease.holder == kPhoneA);
    CHECK(texts_with(socket, R"("recreated":[{"old":)").size() == 1);
}

TEST_CASE("a phone sees and controls a receiver it did not open", "[gpu][webgate][lease]")
{
    REVENANT_NEEDS_GPU();
    LeaseRig rig;
    FakeSocket socket;
    const auto id = rig.admit(socket, kPhoneA);

    // Another session in the phone's lease, standing in for whatever else
    // opens receivers for the one user: the desktop before a takeover, a
    // script, a second tab.
    auto other = rig.harness->connect_as(rpc::LoginMode::Claim, kPhoneA);
    INFO(revenant::test::message_of(other));
    REQUIRE(other.has_value());
    rpc::VrxParams params;
    params.center = 300'000;
    params.demod = rpc::Demod::Nfm;
    const auto made = (*other)->add_vrx(params);
    REQUIRE(made.has_value());

    const std::string listed = R"("event":"vrx","vrx":)" + std::to_string(*made) + ",";
    REQUIRE(rig.pump_until([&] { return !texts_with(socket, listed).empty(); }));
    const auto event = texts_with(socket, listed).front();
    INFO(event);
    CHECK(contains(event, R"("hz":100300000)"));
    CHECK(contains(event, R"("openedBy":"another client")"));

    rig.text(id, R"({"id":2,"op":"tune","args":{"vrx":)" + std::to_string(*made) +
                     R"(,"hz":100350000}})");
    const auto tuned = reply_to(socket, 2);
    INFO(tuned);
    CHECK(contains(tuned, R"("ok":true)"));
    const auto moved = (*other)->vrx_status(*made);
    REQUIRE(moved.has_value());
    CHECK(moved->params.center == 350'000);

    rig.text(id, R"({"id":3,"op":"audio","args":{"vrx":)" + std::to_string(*made) + "}}");
    CHECK(contains(reply_to(socket, 3), R"("ok":true)"));
    rig.text(id, R"({"id":4,"op":"removeVrx","args":{"vrx":)" + std::to_string(*made) + "}}");
    CHECK(contains(reply_to(socket, 4), R"("ok":true)"));
    CHECK_FALSE((*other)->vrx_status(*made).has_value());
}
