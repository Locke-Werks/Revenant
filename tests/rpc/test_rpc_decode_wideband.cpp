// A decoder on the source's own stream, over the wire.
//
// subscribeWidebandDecoded attaches a decoder to a wideband tap rather than
// to a receiver (core/engine/wideband_tap.h). What a decoder there sees is
// what this test pins: complex chunks at the granted rate, delivered through
// the same decode lane, message queue and receiver capability as
// subscribeDecoded, with a tap number in place of a receiver id. The probe
// decoder is test-only, so it never appears in the engine's list.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/decoders.h"
#include "core/rpc/types.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/decoded_log.h"
#include "tests/rpc/rpc_fixture.h"

using namespace revenant;
using test::Harness;
using test::HarnessOptions;

namespace {

constexpr dsp::SampleRate kProbeRate = 48'000;

// One message per chunk, naming the rate and channel count it was handed.
class WidebandProbe final : public rpc::ChunkDecoder {
public:
    static constexpr std::string_view kName = "test_wideband";

    static Expected<std::unique_ptr<rpc::ChunkDecoder>> make(const rpc::DecoderBuild&) {
        return std::unique_ptr<rpc::ChunkDecoder>(std::make_unique<WidebandProbe>());
    }

    Status consume(const rpc::DecoderChunk& chunk,
                   std::vector<rpc::DecodedMessage>& out) override {
        if (sent_ >= 4) {
            return {};
        }
        rpc::DecodedMessage message;
        message.decoder = std::string(kName);
        message.kind = "chunk";
        message.start_sample = chunk.start;
        message.end_sample = chunk.start + chunk.frames();
        message.sample_rate = static_cast<std::uint32_t>(chunk.rate);
        message.fields.push_back({"channels", static_cast<std::int64_t>(chunk.channels)});
        out.push_back(std::move(message));
        ++sent_;
        return {};
    }

    void flush(std::vector<rpc::DecodedMessage>&) override {}
    void reset() override {}

private:
    int sent_ = 0;
};

const rpc::DecoderSpec kProbeSpec{
    WidebandProbe::kName,
    rpc::DecoderInput::ComplexBaseband,
    "A probe on the wideband tap, for the wire test",
    &WidebandProbe::make,
    {},
    kProbeRate};

}  // namespace

TEST_CASE("a wideband decoder hears the source's stream over the wire",
          "[gpu][rpc][decode][wideband]") {
    REVENANT_NEEDS_GPU();
    rpc::register_test_decoder(kProbeSpec);

    Harness harness;
    const auto ready = harness.open(HarnessOptions{});
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    auto messages = std::make_shared<test::MessageLog>();
    auto subscribed = harness.client().subscribe_wideband_decoded(
        "test_wideband", 100'000, test::into(messages), test::ending(messages));
    INFO(test::message_of(subscribed));
    REQUIRE(subscribed.has_value());
    CHECK(subscribed->rate >= static_cast<std::uint64_t>(kProbeRate));
    CHECK(subscribed->tap >= 0xF000'0000ULL);

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(test::kDecodedWaitMs);
    std::vector<rpc::DecodedMessage> seen = messages->messages();
    while (seen.size() < 4 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        seen = messages->messages();
    }
    static_cast<void>(harness.stop_engine());

    REQUIRE(seen.size() >= 4);
    for (const rpc::DecodedMessage& message : seen) {
        CHECK(message.vrx == subscribed->tap);
        CHECK(message.decoder == "test_wideband");
        CHECK(message.sample_rate == subscribed->rate);
        REQUIRE(message.fields.size() == 1);
        REQUIRE(message.fields[0].integer() != nullptr);
        CHECK(*message.fields[0].integer() == 2);
    }
    // Consecutive chunks, nothing lost on an unthrottled synthetic source
    // the ring is sized for.
    for (std::size_t k = 1; k < seen.size(); ++k) {
        CHECK(seen[k].start_sample == seen[k - 1].end_sample);
    }

    harness.client().unsubscribe_decoded(subscribed->key, "test_wideband");
}

TEST_CASE("a wideband decoder is refused on a receiver, and a receiver decoder on a tap",
          "[gpu][rpc][decode][wideband]") {
    REVENANT_NEEDS_GPU();
    rpc::register_test_decoder(kProbeSpec);

    Harness harness;
    const auto ready = harness.open(HarnessOptions{});
    REQUIRE(ready.has_value());

    auto vrx = harness.client().add_vrx(
        rpc::VrxParams{.center = 131'072, .bandwidth = 12'000, .demod = rpc::Demod::Raw});
    REQUIRE(vrx.has_value());

    auto messages = std::make_shared<test::MessageLog>();
    auto on_receiver = harness.client().subscribe_decoded(
        *vrx, "test_wideband", test::into(messages), test::ending(messages));
    REQUIRE_FALSE(on_receiver.has_value());
    CHECK(test::message_of(on_receiver).find("subscribeWidebandDecoded") != std::string::npos);

    auto on_tap = harness.client().subscribe_wideband_decoded(
        "p25p1", 0, test::into(messages), test::ending(messages));
    REQUIRE_FALSE(on_tap.has_value());
    CHECK(test::message_of(on_tap).find("subscribeDecoded") != std::string::npos);
}
