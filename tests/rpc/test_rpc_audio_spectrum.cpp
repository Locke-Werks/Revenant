// The audio spectrum strip across the wire: the receiver's flag, and the
// passband frame's audio row, bin width and notch ticks.
//
// The schema half needs no GPU. The session half runs a real engine behind a
// real server, so it also proves the graph fills the row only when asked.

#include <catch2/catch_test_macros.hpp>

#include <capnp/message.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/engine/audio_spectrum.h"
#include "core/engine/engine.h"
#include "core/rpc/client.h"
#include "core/rpc/convert.h"
#include "core/rpc/revenant.capnp.h"
#include "core/rpc/types.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"

using namespace revenant;
using test::Harness;
using test::HarnessOptions;

TEST_CASE("the audio spectrum flag survives the engine's writer and reader",
          "[rpc][audio_spectrum]") {
    engine::VrxParams sent;
    sent.demod = engine::Demod::Nfm;
    sent.display_audio_spectrum = true;

    capnp::MallocMessageBuilder message;
    auto out = message.initRoot<rpc::schema::VrxParams>();
    rpc::write_vrx_params(out, sent);
    const auto read = rpc::read_vrx_params(out.asReader());
    INFO(test::message_of(read));
    REQUIRE(read.has_value());
    CHECK(read->display_audio_spectrum);

    // An older peer sends nothing and gets the strip off.
    capnp::MallocMessageBuilder bare;
    auto plain = bare.initRoot<rpc::schema::VrxParams>();
    plain.setDemod(rpc::schema::Demod::NFM);
    const auto off = rpc::read_vrx_params(plain.asReader());
    REQUIRE(off.has_value());
    CHECK_FALSE(off->display_audio_spectrum);
}

TEST_CASE("a passband frame's audio row and ticks cross the wire", "[rpc][audio_spectrum]") {
    std::vector<float> power{-90.0F, -80.0F};
    std::vector<float> audio{-100.0F, -60.0F, -30.0F, -61.0F};
    std::vector<double> ticks{1'000.0, 2'350.5};

    engine::PassbandFrame in;
    in.vrx = engine::VrxId{3};
    in.power_db = power;
    in.audio_power_db = audio;
    in.audio_bin_hz = 46.875;
    in.audio_notch_hz = ticks;

    capnp::MallocMessageBuilder message;
    auto out = message.initRoot<rpc::schema::PassbandFrame>();
    rpc::write_passband_frame(out, in);
    const auto reader = out.asReader();
    REQUIRE(reader.getAudioPowerDb().size() == 4);
    CHECK(reader.getAudioPowerDb()[2] == -30.0F);
    CHECK(reader.getAudioBinHz() == 46.875);
    REQUIRE(reader.getAudioNotchHz().size() == 2);
    CHECK(reader.getAudioNotchHz()[1] == 2'350.5);

    // Without the row nothing else of it is written.
    engine::PassbandFrame none;
    none.power_db = power;
    capnp::MallocMessageBuilder empty;
    auto blank = empty.initRoot<rpc::schema::PassbandFrame>();
    rpc::write_passband_frame(blank, none);
    CHECK(blank.asReader().getAudioPowerDb().size() == 0);
    CHECK(blank.asReader().getAudioBinHz() == 0.0);
}

TEST_CASE("the audio strip reaches a session's frames only when asked for",
          "[gpu][rpc][audio_spectrum]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    Harness harness;
    HarnessOptions options;
    options.passband_transform = 512;
    options.samples = 2'400'032;
    options.pace = 1.0;
    options.block_samples = 16'384;
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    rpc::VrxParams params;
    params.center = 187'500;
    params.demod = rpc::Demod::Usb;
    params.passband_low = 300;
    params.passband_high = 2'700;
    params.notch_enabled = true;
    params.notch_hz = 800;
    params.notch_depth_db = 30.0;
    params.notch_width_hz = 100;
    params.display_audio_spectrum = true;

    auto strip = harness.client().add_vrx(params);
    INFO(test::message_of(strip));
    REQUIRE(strip.has_value());
    params.display_audio_spectrum = false;
    auto plain = harness.client().add_vrx(params);
    REQUIRE(plain.has_value());

    auto status = harness.client().vrx_status(*strip);
    REQUIRE(status.has_value());
    CHECK(status->params.display_audio_spectrum);

    struct Seen {
        std::mutex lock;
        std::vector<rpc::PassbandFrame> strip;
        std::vector<rpc::PassbandFrame> plain;
    };
    auto seen = std::make_shared<Seen>();
    const auto keep = [seen](std::vector<rpc::PassbandFrame> Seen::*into) {
        return [seen, into](const rpc::PassbandFrame& frame) {
            std::scoped_lock held(seen->lock);
            if ((*seen.*into).size() < 16) {
                (*seen.*into).push_back(frame);
            }
        };
    };
    REQUIRE(harness.client().subscribe_passband(*strip, 1, keep(&Seen::strip)).has_value());
    REQUIRE(harness.client().subscribe_passband(*plain, 1, keep(&Seen::plain)).has_value());
    REQUIRE(harness.start_engine().has_value());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(8000);
    bool done = false;
    while (std::chrono::steady_clock::now() < deadline && !done) {
        {
            std::scoped_lock held(seen->lock);
            done = seen->strip.size() >= 8 && seen->plain.size() >= 4;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    harness.client().unsubscribe_passband(*strip);
    harness.client().unsubscribe_passband(*plain);
    CHECK(harness.stop_engine().has_value());

    std::scoped_lock held(seen->lock);
    REQUIRE(seen->strip.size() >= 8);
    REQUIRE(seen->plain.size() >= 4);

    // The first frames can arrive before a whole window of audio has; the
    // later ones must carry it.
    const rpc::PassbandFrame& last = seen->strip.back();
    REQUIRE(last.audio_power_db.size() == engine::kAudioSpectrumBins);
    CHECK(last.audio_bin_hz > 0.0);
    REQUIRE(last.audio_notch_hz.size() == 1);
    CHECK(last.audio_notch_hz[0] == 800.0);
    for (const rpc::PassbandFrame& frame : seen->plain) {
        CHECK(frame.audio_power_db.empty());
        CHECK(frame.audio_notch_hz.empty());
    }
}
