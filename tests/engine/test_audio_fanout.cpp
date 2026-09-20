// The composition seam, on its own, with no GPU and no engine.
//
// AudioFanout is the only part of core/engine/engine.h that is pure host
// code: it holds a list of callables and calls them. Everything the wire
// cases in tests/rpc assert about two consumers on one receiver rests on the
// four properties below, and none of them needs a device to check, so they
// are checked here where a failure names the property rather than a
// subscription that went quiet.
//
// Engine::attach_audio_sink is NOT exercised here and cannot be. It calls
// set_audio_sink, which is pure virtual, so reaching it means a real Engine
// and therefore a real device; tests/rpc/test_rpc_audio.cpp is where that
// half is driven, against a running engine over a socket.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "core/engine/engine.h"
#include "core/error.h"

using namespace revenant;

namespace {

// A chunk with nothing in it. Every case here is about which sink was called
// and in what order, so the samples carry no signal and the span is empty.
[[nodiscard]] engine::AudioChunk empty_chunk() {
    engine::AudioChunk chunk;
    chunk.vrx = engine::VrxId{1};
    chunk.rate = 48'000;
    chunk.channels = 1;
    return chunk;
}

}  // namespace

TEST_CASE("a fan-out calls every sink, in attach order", "[engine][audio][fanout]") {
    engine::AudioFanout fanout;
    std::vector<int> called;

    const engine::AudioSinkId first =
        fanout.attach([&called](const engine::AudioChunk&) -> Status {
            called.push_back(1);
            return {};
        });
    const engine::AudioSinkId second =
        fanout.attach([&called](const engine::AudioChunk&) -> Status {
            called.push_back(2);
            return {};
        });

    // Unique and not reused, which is what makes a stale token detach
    // nothing rather than detaching whoever arrived next.
    CHECK(first != second);
    CHECK(fanout.size() == 2);

    const auto chunk = empty_chunk();
    REQUIRE(fanout.deliver(chunk).has_value());
    REQUIRE(fanout.deliver(chunk).has_value());

    CHECK(called == std::vector<int>{1, 2, 1, 2});
}

TEST_CASE("a sink that refuses does not rob the sinks behind it",
          "[engine][audio][fanout]") {
    engine::AudioFanout fanout;
    int good_calls = 0;
    int late_calls = 0;

    static_cast<void>(fanout.attach([](const engine::AudioChunk&) -> Status {
        return fail("the first consumer is miswired");
    }));
    static_cast<void>(fanout.attach([&good_calls](const engine::AudioChunk&) -> Status {
        good_calls += 1;
        return {};
    }));
    static_cast<void>(fanout.attach([&late_calls](const engine::AudioChunk&) -> Status {
        late_calls += 1;
        return fail("the third consumer is miswired too");
    }));

    const auto outcome = fanout.deliver(empty_chunk());

    // Every sink ran, including the two behind the failure. A fan-out that
    // returned at the first error would make one misconfigured consumer look
    // exactly like a dead radio to the other two.
    CHECK(good_calls == 1);
    CHECK(late_calls == 1);

    // And the FIRST error came back, not the last. The run is ending either
    // way and the first message names the consumer that started it.
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message.find("first consumer") != std::string::npos);
}

TEST_CASE("detaching one consumer leaves the others attached",
          "[engine][audio][fanout]") {
    engine::AudioFanout fanout;
    int kept = 0;
    int dropped = 0;

    const engine::AudioSinkId keeper =
        fanout.attach([&kept](const engine::AudioChunk&) -> Status {
            kept += 1;
            return {};
        });
    const engine::AudioSinkId leaver =
        fanout.attach([&dropped](const engine::AudioChunk&) -> Status {
            dropped += 1;
            return {};
        });

    REQUIRE(fanout.deliver(empty_chunk()).has_value());
    CHECK(kept == 1);
    CHECK(dropped == 1);

    CHECK(fanout.detach(leaver));
    REQUIRE(fanout.deliver(empty_chunk()).has_value());
    CHECK(kept == 2);

    // This is the whole point of the seam. The consumer that left took its
    // own audio with it and nobody else's.
    CHECK(dropped == 1);
    CHECK_FALSE(fanout.empty());

    CHECK(fanout.detach(keeper));
    CHECK(fanout.empty());
    REQUIRE(fanout.deliver(empty_chunk()).has_value());
    CHECK(kept == 2);
}

TEST_CASE("a token that is not attached detaches nothing", "[engine][audio][fanout]") {
    engine::AudioFanout fanout;
    int calls = 0;

    const engine::AudioSinkId only =
        fanout.attach([&calls](const engine::AudioChunk&) -> Status {
            calls += 1;
            return {};
        });

    // Never issued.
    CHECK_FALSE(fanout.detach(only + 1000));
    CHECK(fanout.size() == 1);

    CHECK(fanout.detach(only));

    // A second detach of the same token, which is the shape a client that
    // cancels and then drops its capability produces. It must not take the
    // next consumer's place: attach issues a fresh token rather than reusing
    // this one, so there is nothing for a stale detach to hit.
    const engine::AudioSinkId next =
        fanout.attach([&calls](const engine::AudioChunk&) -> Status {
            calls += 10;
            return {};
        });
    CHECK(next != only);
    CHECK_FALSE(fanout.detach(only));

    REQUIRE(fanout.deliver(empty_chunk()).has_value());
    CHECK(calls == 10);
}
