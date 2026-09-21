// The composition seam, on its own, with no GPU and no engine.
//
// AudioFanout is the only part of core/engine/engine.h that is pure host
// code: it holds a list of callables and calls them. Everything the wire
// cases in tests/rpc assert about two consumers on one receiver rests on the
// properties below, and none of them needs a device to check, so they are
// checked here where a failure names the property rather than a subscription
// that went quiet.
//
// This sentence used to say "the four properties below" and there are five.
// The count was right once. Counting is the wrong shape for it, the same
// mistake engine.h's own AudioChunk comment records, so the number is gone
// rather than corrected.
//
// Engine::attach_audio_sink is NOT exercised here and cannot be. It calls
// set_audio_sink, which is pure virtual, so reaching it means a real Engine
// and therefore a real device; tests/rpc/test_rpc_audio.cpp is where that
// half is driven, against a running engine over a socket.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
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

TEST_CASE("a sink that throws is that sink's failure and not the process's",
          "[engine][audio][fanout]") {
    // Run against the fan-out as it was before 2026-09-20 this reports
    // "unexpected exception with message: the consumer's own bug" and the
    // sink behind the thrower is never called. Catch2 wraps each case in its
    // own try, which is why it is a failure here and not a dead binary; in
    // the engine deliver runs on the completion thread, which
    // core/engine/scheduler.cpp creates with std::thread, and an exception
    // leaving a std::thread's callable is std::terminate with no catch
    // anywhere between. So this case checks two things the engine cannot
    // check for itself: that the throw becomes an ordinary error, and that
    // it does not rob the sinks behind it the way it did.
    engine::AudioFanout fanout;
    int before = 0;
    int after = 0;

    const engine::AudioSinkId first =
        fanout.attach([&before](const engine::AudioChunk&) -> Status {
            before += 1;
            return {};
        });
    const engine::AudioSinkId thrower =
        fanout.attach([](const engine::AudioChunk&) -> Status {
            throw std::runtime_error("the consumer's own bug");
        });
    const engine::AudioSinkId last =
        fanout.attach([&after](const engine::AudioChunk&) -> Status {
            after += 1;
            return {};
        });
    CHECK(first != thrower);
    CHECK(thrower != last);

    const Status outcome = fanout.deliver(empty_chunk());
    REQUIRE_FALSE(outcome.has_value());

    // The throw is reported as what it was, with the consumer's own text.
    CHECK(outcome.error().message.find("threw") != std::string::npos);
    CHECK(outcome.error().message.find("the consumer's own bug") != std::string::npos);

    // And it took nobody with it.
    CHECK(before == 1);
    CHECK(after == 1);
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

TEST_CASE("an empty sink is skipped rather than called", "[engine][audio][fanout]") {
    engine::AudioFanout fanout;
    int before = 0;
    int after = 0;

    const engine::AudioSinkId first =
        fanout.attach([&before](const engine::AudioChunk&) -> Status {
            before += 1;
            return {};
        });

    // Engine::attach_audio_sink refuses this and says why, so the guard in
    // deliver is unreachable through the seam and reachable through the
    // class: attach returns a token and has no error channel to refuse with.
    // A caller that lost its callable gets a fan-out that skips it, and the
    // alternative is calling an empty std::function on the thread retiring
    // GPU readbacks.
    const engine::AudioSinkId hollow = fanout.attach(engine::AudioSink{});
    const engine::AudioSinkId last =
        fanout.attach([&after](const engine::AudioChunk&) -> Status {
            after += 1;
            return {};
        });

    // It occupies a place and holds a token of its own, so the ones behind
    // it are not renumbered by its presence.
    CHECK(fanout.size() == 3);
    CHECK(hollow != first);
    CHECK(hollow != last);

    const auto outcome = fanout.deliver(empty_chunk());

    // Skipped, and the sink behind it still ran. A guard that returned
    // instead of continuing would leave `after` at zero.
    REQUIRE(outcome.has_value());
    CHECK(before == 1);
    CHECK(after == 1);

    // And it is an ordinary member otherwise: its token detaches it and the
    // fan-out shrinks.
    CHECK(fanout.detach(hollow));
    CHECK(fanout.size() == 2);
    REQUIRE(fanout.deliver(empty_chunk()).has_value());
    CHECK(before == 2);
    CHECK(after == 2);
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

TEST_CASE("two fan-outs never issue the same token", "[engine][audio][fanout]") {
    // The property AudioSinkId's comment states and detach_audio_sink's
    // refusal rests on. Engine keeps one fan-out per receiver, so a counter
    // living in the fan-out would hand the first consumer of every receiver
    // the same number, and detach_audio_sink called with the right token and
    // the wrong VrxId would then detach a stranger instead of refusing.
    engine::AudioFanout first;
    engine::AudioFanout second;

    const engine::AudioSinkId a = first.attach([](const engine::AudioChunk&) -> Status {
        return {};
    });
    const engine::AudioSinkId b = second.attach([](const engine::AudioChunk&) -> Status {
        return {};
    });
    CHECK(a != b);

    // And the token space survives the fan-out itself. Engine erases a
    // fan-out the moment its last consumer detaches, so a counter that was a
    // member died with it and the next attach on that receiver reissued a
    // token already handed out once.
    engine::AudioSinkId reborn = 0;
    {
        engine::AudioFanout transient;
        const engine::AudioSinkId leaving =
            transient.attach([](const engine::AudioChunk&) -> Status { return {}; });
        CHECK(transient.detach(leaving));
        CHECK(transient.empty());
    }
    {
        engine::AudioFanout replacement;
        reborn = replacement.attach([](const engine::AudioChunk&) -> Status { return {}; });
    }
    CHECK(reborn != a);
    CHECK(reborn != b);
}
