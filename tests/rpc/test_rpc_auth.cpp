// The login bootstrap, over a real socket.
//
// WHY HALF OF THIS IS RAW CAP'N PROTO AND NOT rpc::Client
//
// rpc::Client sends exactly one login, with exactly kTokenBytes, and waits
// for it. That is the right shape for a client and it makes four of the
// reachable shapes below unreachable through it: a zero-length token, a token
// of the wrong length arriving at the SERVER rather than being refused
// locally, a second login on one connection, and a pipelined call made before
// login has come back. Those four go through capnp::EzRpcClient, which gives
// a bootstrap and a WaitScope on this thread and nothing else.
//
// The pipelined pair is the important one and it is the one a bar written
// from the login response alone would miss. It is the shape rpc::Client
// actually generates, and the property it proves is not "the call failed": it
// is that the engine's Session implementation was never entered. Asserting
// only the failure would pass even if the server had run the method and then
// failed for some unrelated reason, so the wrong-token case sends addVrx and
// then asks the ENGINE whether a receiver appeared. The good-token case sends
// the same call and asserts one did, which is what stops the first from being
// green against a server that answers nothing at all.
//
// THE SHAPES, ENUMERATED BEFORE THE BAR WAS WRITTEN
//
//   A1  Right token. A Session comes back and every existing method works
//       through it. Every other case in tests/rpc is this shape, which is
//       why it is the one a lazy bar would prove and the reason the list
//       below exists.
//   A2  Wrong token, right length. Refused, naming what to do about it. The
//       token differs in its LAST byte, so a compare over a prefix would
//       accept it.
//   A3  No token: the field unset, which arrives as zero bytes. Refused, and
//       a different input from thirty-two zero bytes, which is also refused.
//   A4  Wrong length, both directions, at both layers. 31 and 33 bytes are
//       refused by Client::connect before a socket is opened; 31 bytes sent
//       raw are refused by the server.
//   A5  Right token, pipelined. The Session call travels behind the login
//       and both resolve.
//   A6  Wrong token, pipelined. The Session call fails with login's own
//       exception and the engine never saw it.
//   A7  A second login on one connection. Succeeds, returning a second
//       independent Session. Pinned so it is a decision and not an accident:
//       the caller has already proved it holds the token, and refusing would
//       need per-connection state capnp 1.4.0's TwoPartyServer does not
//       offer.
//   A8  Dropping the first Session does not disturb the second.
//   A9  A Session outliving the Authenticator it came from. This is the
//       capability discipline the whole design rests on, and a bar that
//       never drops the root never touches it.
//   A10 Two clients, one token, at once. Independent sessions, and one
//       going away leaves the other's subscription running.
//   A11 Reconnecting. A fresh connection has to log in again, and the old
//       Session is dead.
//   A12 ServerOptions::token empty. create() fails, in words, and no port is
//       bound. This is the shape that would otherwise ship an engine anyone
//       on the machine can drive.
//   A13 ServerOptions::token the wrong length. Same, distinguishably.
//   A14 stop() with a Session held by a connected client. The server shuts
//       down cleanly and the client's next call fails.
//
// NOT HERE: the off-loopback warning revenant-engine prints, which is a
// property of that program's stderr rather than of this library, and would
// need another CTest entry rather than a C++ case.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <capnp/ez-rpc.h>
#include <kj/common.h>
#include <kj/exception.h>

#include "core/engine/engine.h"
#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/revenant.capnp.h"
#include "core/rpc/server.h"
#include "core/rpc/token.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"

using namespace revenant;
using test::Harness;
using test::HarnessOptions;
using test::test_token;
using test::wrong_test_token;

namespace {

void bring_up(Harness& harness, const HarnessOptions& options) {
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());
}

[[nodiscard]] capnp::Data::Reader as_data(const std::uint8_t* bytes, std::size_t count) {
    return {reinterpret_cast<const kj::byte*>(bytes), count};
}

// A receiver the harness engine will actually accept, written onto a raw
// request. The AGC and squelch values are the engine's own defaults rather
// than the struct's zeros: an unset Float64 arrives as 0.0, and a zero attack
// time is not a receiver anybody asked for.
void write_plain_vrx(rpc::schema::VrxParams::Builder out) {
    out.setCenter(262'144);
    out.setBandwidth(12'000);
    out.setDemod(rpc::schema::Demod::NFM);
    out.setSquelchDbfs(-200.0);
    out.setAgcAttackMs(5.0);
    out.setAgcDecayMs(200.0);
    out.setAgcEnabled(false);
}

}  // namespace

TEST_CASE("the right token gets a session and the wrong one gets a refusal",
          "[gpu][rpc][auth]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    // A1. The harness connected with the right token, so this is only worth
    // asserting because every refusal below has to be measured against a
    // connection that does work.
    auto info = harness.client().info();
    INFO(test::message_of(info));
    CHECK(info.has_value());

    // A2. One bit, in the last byte.
    const rpc::Token wrong = wrong_test_token();
    auto refused = harness.connect_with(wrong);
    REQUIRE_FALSE(refused.has_value());
    INFO(refused.error().message);
    CHECK(refused.error().message.find("not this engine's token") != std::string::npos);

    // And the refusal says nothing about the token itself. The length is
    // published, so hiding it would be theatre; there is simply nothing else
    // true to say, and "too short" would invite a caller to treat the length
    // as the thing to get right.
    CHECK(refused.error().message.find("32") == std::string::npos);
}

TEST_CASE("a token of the wrong length is refused before a socket is opened",
          "[rpc][auth]") {
    // A4, client side. No GPU and no server: these are argument errors
    // Client::connect answers on its own, and answering them before a thread
    // is started is the point.
    const rpc::Token full = test_token();

    const std::vector<std::uint8_t> short_one(full.begin(), full.end() - 1);
    auto too_short = rpc::Client::connect("127.0.0.1", 47'000, short_one);
    REQUIRE_FALSE(too_short.has_value());
    INFO(too_short.error().message);
    CHECK(too_short.error().message.find("31 bytes") != std::string::npos);

    std::vector<std::uint8_t> long_one(full.begin(), full.end());
    long_one.push_back(0);
    auto too_long = rpc::Client::connect("127.0.0.1", 47'000, long_one);
    REQUIRE_FALSE(too_long.has_value());
    INFO(too_long.error().message);
    CHECK(too_long.error().message.find("33 bytes") != std::string::npos);

    auto none = rpc::Client::connect("127.0.0.1", 47'000, {});
    REQUIRE_FALSE(none.has_value());
    INFO(none.error().message);
    CHECK(none.error().message.find("0 bytes") != std::string::npos);
}

TEST_CASE("the server refuses the token shapes a client cannot send", "[gpu][rpc][auth]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    capnp::EzRpcClient raw("127.0.0.1", harness.port());
    kj::WaitScope& scope = raw.getWaitScope();
    auto authenticator = raw.getMain<rpc::schema::Authenticator>();

    SECTION("the token field unset, which arrives as zero bytes") {
        // A3.
        auto pending = authenticator.loginRequest().send();
        CHECK_THROWS_AS(pending.wait(scope), kj::Exception);
    }

    SECTION("thirty-two zero bytes, which is a different input") {
        // A3's other half. A zeroed array is what an uninitialised buffer
        // holds, so it must not be the one value that happens to work.
        const rpc::Token zeros{};
        auto request = authenticator.loginRequest();
        request.setToken(as_data(zeros.data(), zeros.size()));
        CHECK_THROWS_AS(request.send().wait(scope), kj::Exception);
    }

    SECTION("thirty-one bytes of the right token") {
        // A4, server side. The client layer would have refused this without
        // ever opening a socket, so the server's own length check is only
        // reachable from here.
        const rpc::Token full = test_token();
        auto request = authenticator.loginRequest();
        request.setToken(as_data(full.data(), full.size() - 1));
        CHECK_THROWS_AS(request.send().wait(scope), kj::Exception);
    }

    SECTION("and the right token still works on this same connection") {
        // Without this the three above are satisfied by a server that
        // refuses everything, including a server whose token comparison is
        // inverted.
        const rpc::Token full = test_token();
        auto request = authenticator.loginRequest();
        request.setToken(as_data(full.data(), full.size()));
        auto response = request.send().wait(scope);
        CHECK(response.hasSession());
    }
}

TEST_CASE("a pipelined call behind a refused login never reaches the engine",
          "[gpu][rpc][auth]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});
    REQUIRE(harness.engine().vrx_ids().empty());

    capnp::EzRpcClient raw("127.0.0.1", harness.port());
    kj::WaitScope& scope = raw.getWaitScope();
    auto authenticator = raw.getMain<rpc::schema::Authenticator>();

    // A6. The addVrx goes out behind the login, without waiting for it, which
    // is what Cap'n Proto is for and what core/rpc/client.cpp does.
    const rpc::Token wrong = wrong_test_token();
    auto login = authenticator.loginRequest();
    login.setToken(as_data(wrong.data(), wrong.size()));
    auto pending = login.send();

    auto add = pending.getSession().addVrxRequest();
    write_plain_vrx(add.initParams());
    auto added = add.send();

    CHECK_THROWS_AS(added.wait(scope), kj::Exception);
    CHECK_THROWS_AS(pending.wait(scope), kj::Exception);

    // THE ASSERTION THIS CASE EXISTS FOR. The call failing is not enough: it
    // would fail just as loudly if the server had run the method body and
    // then hit something else. No receiver means SessionImpl::addVrx was
    // never entered.
    CHECK(harness.engine().vrx_ids().empty());
}

TEST_CASE("a pipelined call behind an accepted login reaches the engine",
          "[gpu][rpc][auth]") {
    REVENANT_NEEDS_GPU();

    // A5, and the control for the case above. Without it, a server that
    // answered nothing at all would pass the refusal case.
    Harness harness;
    bring_up(harness, HarnessOptions{});
    REQUIRE(harness.engine().vrx_ids().empty());

    capnp::EzRpcClient raw("127.0.0.1", harness.port());
    kj::WaitScope& scope = raw.getWaitScope();
    auto authenticator = raw.getMain<rpc::schema::Authenticator>();

    const rpc::Token good = test_token();
    auto login = authenticator.loginRequest();
    login.setToken(as_data(good.data(), good.size()));
    auto pending = login.send();

    auto add = pending.getSession().addVrxRequest();
    write_plain_vrx(add.initParams());
    const std::uint64_t id = add.send().wait(scope).getId();

    CHECK(id != 0);
    CHECK(harness.engine().vrx_ids().size() == 1);
}

TEST_CASE("a second login on one connection is another independent session",
          "[gpu][rpc][auth]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    capnp::EzRpcClient raw("127.0.0.1", harness.port());
    kj::WaitScope& scope = raw.getWaitScope();
    auto authenticator = raw.getMain<rpc::schema::Authenticator>();
    const rpc::Token good = test_token();

    const auto log_in = [&]() {
        auto request = authenticator.loginRequest();
        request.setToken(as_data(good.data(), good.size()));
        return request.send().wait(scope).getSession();
    };

    // A7. Allowed on purpose rather than by oversight: the caller has already
    // proved it holds the token, so a second capability grants it nothing it
    // did not have, and refusing would need per-connection state that
    // capnp 1.4.0's TwoPartyServer does not offer.
    auto first = log_in();
    auto second = log_in();

    CHECK(first.infoRequest().send().wait(scope).hasInfo());
    CHECK(second.infoRequest().send().wait(scope).hasInfo());

    // A8. Dropping one must leave the other alone, which is what says these
    // are two objects and not two references to one.
    {
        auto doomed = kj::mv(first);
        static_cast<void>(kj::mv(doomed));
    }
    CHECK(second.infoRequest().send().wait(scope).hasInfo());

    // A9. And the Session outlives the root it came from. A caller may drop
    // the Authenticator the moment it has served its purpose, which is the
    // capability discipline the whole bootstrap rests on.
    {
        auto doomed = kj::mv(authenticator);
        static_cast<void>(kj::mv(doomed));
    }
    CHECK(second.infoRequest().send().wait(scope).hasInfo());
}

TEST_CASE("two clients hold one token and neither disturbs the other", "[gpu][rpc][auth]") {
    REVENANT_NEEDS_GPU();

    // A10 and A11.
    Harness harness;
    bring_up(harness, HarnessOptions{});

    auto second = harness.connect_another();
    INFO(test::message_of(second));
    REQUIRE(second.has_value());

    auto from_first = harness.client().info();
    auto from_second = (*second)->info();
    INFO(test::message_of(from_first));
    INFO(test::message_of(from_second));
    REQUIRE(from_first.has_value());
    REQUIRE(from_second.has_value());
    CHECK(from_first->source_rate == from_second->source_rate);

    // One receiver each, so the sessions are provably separate capabilities
    // rather than one shared object with two handles.
    auto one = harness.client().add_vrx(rpc::VrxParams{.center = 131'072,
                                                       .bandwidth = 12'000,
                                                       .demod = rpc::Demod::Nfm});
    INFO(test::message_of(one));
    REQUIRE(one.has_value());

    second->reset();

    // The first client is untouched by the second going away, which is what
    // a shared SessionImpl torn down with one connection would break.
    auto after = harness.client().vrx_ids();
    INFO(test::message_of(after));
    REQUIRE(after.has_value());
    CHECK(after->size() == 1);

    // A11. A fresh connection logs in again and sees the same engine.
    auto third = harness.connect_another();
    INFO(test::message_of(third));
    REQUIRE(third.has_value());
    auto seen = (*third)->vrx_ids();
    REQUIRE(seen.has_value());
    CHECK(seen->size() == 1);
}

TEST_CASE("a server with no token refuses to start", "[gpu][rpc][auth]") {
    REVENANT_NEEDS_GPU();

    // A12 and A13. No source and no harness: Server::create has to answer
    // these before it claims the engine, so nothing else has to be built.
    engine::EngineConfig config;
    config.gpu_index = -1;
    config.channels = test::kChannels;
    config.ring_seconds = 0.25;

    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;

    // A12. The one line in this design where a plausible convenience, empty
    // meaning unauthenticated, produces a total silent failure: an engine
    // anyone on the machine can drive, out of code that reads as configured.
    auto none = rpc::Server::create(eng, rpc::ServerOptions{});
    REQUIRE_FALSE(none.has_value());
    INFO(none.error().message);
    CHECK(none.error().message.find("no unauthenticated mode") != std::string::npos);

    // A13, distinguishably. A caller that supplied SOMETHING and got the
    // length wrong needs to be told that rather than told it supplied
    // nothing.
    const rpc::Token full = test_token();
    rpc::ServerOptions truncated;
    truncated.token.assign(full.begin(), full.end() - 1);
    auto wrong_size = rpc::Server::create(eng, truncated);
    REQUIRE_FALSE(wrong_size.has_value());
    INFO(wrong_size.error().message);
    CHECK(wrong_size.error().message.find("31-byte token") != std::string::npos);

    // And the engine was not claimed by either refusal, so a caller that
    // fixes the argument can still serve it.
    rpc::ServerOptions good;
    good.token.assign(full.begin(), full.end());
    auto served = rpc::Server::create(eng, good);
    INFO(test::message_of(served));
    REQUIRE(served.has_value());
    CHECK((*served)->port() != 0);
}

TEST_CASE("stopping the server with a live session ends it cleanly", "[gpu][rpc][auth]") {
    REVENANT_NEEDS_GPU();

    // A14. Sessions handed out by login are new state in the teardown path:
    // before this branch the one bootstrap Session was a member of the
    // TwoPartyServer local and died with it, and now there can be several,
    // each owned by a client.
    Harness harness;
    bring_up(harness, HarnessOptions{});

    auto before = harness.client().info();
    INFO(test::message_of(before));
    REQUIRE(before.has_value());

    harness.server().stop();

    // The client cannot get an ended notification, because the loop it would
    // arrive on is gone. A failed call is the signal, and it has to be a
    // failure rather than a hang.
    auto after = harness.client().info();
    CHECK_FALSE(after.has_value());
}
