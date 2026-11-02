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

// Every registered wideband decoder builds at its floor and at a faster tap,
// refuses a slower one, and eats a chunk of each. DAB alone is held to its
// one rate. The server picks the tap's rate; this is what make() has to
// accept of whatever it picks.
TEST_CASE("every wideband decoder builds at the rates a tap can deliver", "[rpc][decode][wideband]") {
    std::size_t wideband = 0;
    for (const rpc::DecoderSpec& spec : rpc::decoder_registry()) {
        if (spec.wideband_rate == 0) {
            continue;
        }
        ++wideband;
        INFO(spec.name);
        CHECK(spec.input == rpc::DecoderInput::ComplexBaseband);
        const bool exact = spec.name == "dab";
        const dsp::SampleRate floor = spec.wideband_rate;
        const dsp::SampleRate faster = floor * 5 / 4;

        CHECK_FALSE(spec.make(rpc::DecoderBuild{floor / 2, "raw"}).has_value());
        CHECK(spec.make(rpc::DecoderBuild{faster, "raw"}).has_value() == !exact);

        for (const dsp::SampleRate rate : {floor, faster}) {
            auto built = spec.make(rpc::DecoderBuild{rate, "raw"});
            if (exact && rate != floor) {
                continue;
            }
            INFO(rate);
            REQUIRE(built.has_value());
            std::vector<float> iq(static_cast<std::size_t>(rate / 100) * 2, 0.0F);
            std::vector<rpc::DecodedMessage> out;
            CHECK((*built)->consume(rpc::DecoderChunk{iq, 2, rate, 0}, out).has_value());
            (*built)->flush(out);
            (*built)->reset();
            CHECK((*built)->consume(rpc::DecoderChunk{iq, 2, rate, 0}, out).has_value());
        }
    }
    CHECK(wideband >= 22);
}

TEST_CASE("the raw-tap decoders take the rates they say", "[rpc][decode]") {
    const auto builds = [](std::string_view name, dsp::SampleRate rate) {
        const rpc::DecoderSpec* spec = rpc::find_decoder(name);
        REQUIRE(spec != nullptr);
        return spec->make(rpc::DecoderBuild{rate, "raw"}).has_value();
    };
    CHECK(builds("drm", 48'000));
    CHECK_FALSE(builds("drm", 96'000));
    for (const std::string_view name : {"dpmr", "vdes", "funcube", "cospas_sarsat"}) {
        INFO(name);
        CHECK(builds(name, 48'000));
        CHECK_FALSE(builds(name, 384'000));
    }
    CHECK_FALSE(builds("vdes", 24'000));
    CHECK(builds("cospas_sarsat_sgb", 76'800));
    CHECK(builds("cospas_sarsat_sgb", 153'600));
    CHECK_FALSE(builds("cospas_sarsat_sgb", 48'000));
    CHECK_FALSE(builds("cospas_sarsat_sgb", 150'000));
    CHECK_FALSE(builds("cospas_sarsat_sgb", 307'200));
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
