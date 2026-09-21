// The egress layer's failure paths, which are the ones nothing else covers.
//
// AudioEgress had no test file at all until 2026-09-20. Its happy path is
// exercised indirectly every time revenant-cli records a WAV, and its failure
// paths were exercised by nothing: a backend that returns an error, and a
// backend that throws. The second of those ended the process.
//
// No GPU and no sound card. Everything here is a fake Push backend and the
// egress layer's own threading, which is the whole of what is under test.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/engine/audio_egress.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 48'000;
constexpr std::uint32_t kChannels = 1;

// How the fake backends fail, if they do.
enum class Misbehaviour : std::uint8_t {
    None,

    // The documented way: write() answers with an Error and the slot faults.
    ReturnError,

    // The undocumented way, which the interface does not permit and a
    // caller-supplied class can do anyway. Before 2026-09-20 this reached the
    // top of a std::thread and took the process with it.
    ThrowStdException,

    // The same, through something that is not a std::exception at all, so the
    // catch-all is exercised rather than only the typed catch.
    ThrowSomethingElse,
};

class FakeBackend final : public engine::AudioBackend {
public:
    explicit FakeBackend(Misbehaviour how, std::atomic<std::uint64_t>* calls)
        : how_(how), calls_(calls)
    {
    }

    [[nodiscard]] engine::AudioBackendKind kind() const override
    {
        return engine::AudioBackendKind::Push;
    }

    [[nodiscard]] Status open(const engine::AudioStreamInfo&) override { return {}; }
    [[nodiscard]] Status close() override { return {}; }

    [[nodiscard]] Status write(std::span<const float> samples) override
    {
        if (calls_ != nullptr) {
            calls_->fetch_add(1, std::memory_order_relaxed);
        }
        switch (how_) {
            case Misbehaviour::None: accepted_ += samples.size(); return {};
            case Misbehaviour::ReturnError: return fail("the disk is full");
            case Misbehaviour::ThrowStdException:
                throw std::runtime_error("the disk is full and the backend threw about it");
            case Misbehaviour::ThrowSomethingElse: throw 17;
        }
        return {};
    }

    [[nodiscard]] std::size_t accepted() const { return accepted_; }

private:
    Misbehaviour how_;
    std::atomic<std::uint64_t>* calls_;
    std::size_t accepted_ = 0;
};

[[nodiscard]] engine::AudioStreamInfo stream_info(engine::VrxId id)
{
    engine::AudioStreamInfo info;
    info.vrx = id;
    info.rate = kRate;
    info.channels = kChannels;
    info.label = "test";
    return info;
}

// Publishes one chunk of ordinary audio at the receiver's own rate.
[[nodiscard]] Status publish_block(engine::AudioEgress& egress, engine::VrxId id,
                                   dsp::SampleIndex start, std::size_t frames)
{
    const std::vector<float> samples(frames * kChannels, 0.25F);
    engine::AudioChunk chunk;
    chunk.vrx = id;
    chunk.start = start;
    chunk.rate = kRate;
    chunk.channels = kChannels;
    chunk.samples = std::span<const float>(samples);
    return egress.publish(chunk);
}

// The drain thread is on a poll rather than a clock, so a test waits for the
// outcome rather than for a duration. Bounded, because a fault that never
// arrives has to fail rather than hang the suite.
[[nodiscard]] bool wait_for_fault(engine::AudioEgress& egress, engine::VrxId id)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        auto stats = egress.stats(id);
        if (stats && stats->faulted) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

}  // namespace

TEST_CASE("a backend that returns an error faults its own receiver and no other",
          "[engine][audio][egress]") {
    auto created = engine::AudioEgress::create();
    REQUIRE(created.has_value());
    engine::AudioEgress& egress = **created;

    const engine::VrxId good{1};
    const engine::VrxId bad{2};

    REQUIRE(egress.add_receiver(stream_info(good),
                                std::make_unique<FakeBackend>(Misbehaviour::None, nullptr))
                .has_value());
    REQUIRE(egress.add_receiver(stream_info(bad),
                                std::make_unique<FakeBackend>(Misbehaviour::ReturnError, nullptr))
                .has_value());
    REQUIRE(egress.start().has_value());

    REQUIRE(publish_block(egress, good, 0, 4096).has_value());
    REQUIRE(publish_block(egress, bad, 0, 4096).has_value());

    REQUIRE(wait_for_fault(egress, bad));

    auto broken = egress.stats(bad);
    REQUIRE(broken.has_value());
    CHECK(broken->fault.find("disk is full") != std::string::npos);

    // The whole point of faulting per receiver: one disk filling up does not
    // stop the other forty-nine.
    auto healthy = egress.stats(good);
    REQUIRE(healthy.has_value());
    CHECK_FALSE(healthy->faulted);

    REQUIRE(egress.stop().has_value());
}

TEST_CASE("a backend that throws faults rather than ending the process",
          "[engine][audio][egress]") {
    // WITHOUT THE CATCH THIS CASE DOES NOT FAIL, IT TERMINATES. drain_loop
    // runs on a std::thread, so an exception leaving AudioBackend::write
    // reaches the top of that thread and is std::terminate: the test binary
    // dies with no assertion and no message. That is what the case is worth
    // running for, and it is why the outcome asserted below is an ordinary
    // recorded fault rather than anything exotic.
    auto created = engine::AudioEgress::create();
    REQUIRE(created.has_value());
    engine::AudioEgress& egress = **created;

    const engine::VrxId id{1};
    std::atomic<std::uint64_t> calls{0};
    REQUIRE(egress.add_receiver(
                    stream_info(id),
                    std::make_unique<FakeBackend>(Misbehaviour::ThrowStdException, &calls))
                .has_value());
    REQUIRE(egress.start().has_value());

    REQUIRE(publish_block(egress, id, 0, 4096).has_value());
    REQUIRE(wait_for_fault(egress, id));

    auto stats = egress.stats(id);
    REQUIRE(stats.has_value());
    CHECK(stats->fault.find("threw an exception") != std::string::npos);

    // The what() string is carried through, because a backend that throws is
    // as entitled to say why as one that returns an Error.
    CHECK(stats->fault.find("the disk is full and the backend threw about it") !=
          std::string::npos);

    // Once and not again. A faulted slot is never called back, so the throw
    // costs one allocation for the message and nothing after it.
    const std::uint64_t seen = calls.load(std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(calls.load(std::memory_order_relaxed) == seen);

    REQUIRE(egress.stop().has_value());
}

TEST_CASE("a backend that throws something which is not a std::exception still faults",
          "[engine][audio][egress]") {
    auto created = engine::AudioEgress::create();
    REQUIRE(created.has_value());
    engine::AudioEgress& egress = **created;

    const engine::VrxId id{1};
    REQUIRE(egress.add_receiver(
                    stream_info(id),
                    std::make_unique<FakeBackend>(Misbehaviour::ThrowSomethingElse, nullptr))
                .has_value());
    REQUIRE(egress.start().has_value());

    REQUIRE(publish_block(egress, id, 0, 4096).has_value());
    REQUIRE(wait_for_fault(egress, id));

    auto stats = egress.stats(id);
    REQUIRE(stats.has_value());

    // No what() to quote, so the message says that rather than inventing one.
    CHECK(stats->fault.find("not a std::exception") != std::string::npos);

    REQUIRE(egress.stop().has_value());
}

TEST_CASE("a backend that throws on the final drain is caught too",
          "[engine][audio][egress]") {
    // final_drain runs on the CONTROL thread, where a throw would unwind
    // through a Status-returning API rather than end the process. It is
    // caught anyway: a caller that has to handle a Status for a disk error
    // and an exception for a bug in the same backend has to handle both to
    // handle either.
    auto created = engine::AudioEgress::create();
    REQUIRE(created.has_value());
    engine::AudioEgress& egress = **created;

    const engine::VrxId id{1};
    REQUIRE(egress.add_receiver(
                    stream_info(id),
                    std::make_unique<FakeBackend>(Misbehaviour::ThrowStdException, nullptr))
                .has_value());

    // Never started, so no drain thread exists and the samples sit in the
    // ring until remove_receiver drains them on this thread.
    REQUIRE(publish_block(egress, id, 0, 4096).has_value());

    const Status removed = egress.remove_receiver(id);
    REQUIRE_FALSE(removed.has_value());
    CHECK(removed.error().message.find("threw an exception") != std::string::npos);
}
