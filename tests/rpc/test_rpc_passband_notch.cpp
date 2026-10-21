// The notch list, the split flag and the passband frame's notch, carrier and
// raw fields across the wire.
//
// Two halves. The schema half needs no GPU: core/rpc/convert.cpp's reader
// against a message an older peer would send, with no notch list, and
// against one carrying all four. The session half runs a real engine behind
// a real server and reads back what the engine was given and what its
// passband frames carry.

#include <catch2/catch_test_macros.hpp>

#include <capnp/message.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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

TEST_CASE("a peer that sends no notch list has its single notch mirrored into entry 0",
          "[rpc][notch]") {
    capnp::MallocMessageBuilder message;
    auto out = message.initRoot<rpc::schema::VrxParams>();
    out.setDemod(rpc::schema::Demod::USB);
    out.setNotch(true);
    out.setNotchHz(1'250);
    out.setNotchDepthDb(30.0);
    out.setNotchWidthHz(80);

    const auto read = rpc::read_vrx_params(out.asReader());
    INFO(test::message_of(read));
    REQUIRE(read.has_value());
    CHECK(read->notch_enabled);
    CHECK(read->notches[0] == engine::VrxNotch{true, 1'250, 30.0, 80});
    for (std::size_t i = 1; i < engine::kMaxVrxNotches; ++i) {
        CHECK_FALSE(read->notches[i].enabled);
    }
    CHECK_FALSE(read->display_split);
}

TEST_CASE("all four notches and the split flag survive the engine's writer and reader",
          "[rpc][notch]") {
    engine::VrxParams sent;
    sent.demod = engine::Demod::Usb;
    sent.notch_enabled = true;
    sent.notch_hz = 500;
    sent.notch_depth_db = 20.0;
    sent.notch_width_hz = 50;
    sent.notches[1] = engine::VrxNotch{true, 900, 30.0, 60};
    sent.notches[2] = engine::VrxNotch{false, 1'300, 40.0, 70};
    sent.notches[3] = engine::VrxNotch{true, 2'100, 50.0, 90};
    sent.display_split = true;

    capnp::MallocMessageBuilder message;
    auto out = message.initRoot<rpc::schema::VrxParams>();
    rpc::write_vrx_params(out, sent);

    // Entry 0 on the wire is the single notch, for an older reader.
    REQUIRE(out.asReader().getNotches().size() == engine::kMaxVrxNotches);
    CHECK(out.asReader().getNotches()[0].getHz() == 500);

    const auto read = rpc::read_vrx_params(out.asReader());
    INFO(test::message_of(read));
    REQUIRE(read.has_value());
    CHECK(read->notches[0] == engine::VrxNotch{true, 500, 20.0, 50});
    for (std::size_t i = 1; i < engine::kMaxVrxNotches; ++i) {
        CHECK(read->notches[i] == sent.notches[i]);
    }
    CHECK(read->display_split);
}

TEST_CASE("more than four notches are refused by the reader", "[rpc][notch]") {
    capnp::MallocMessageBuilder message;
    auto out = message.initRoot<rpc::schema::VrxParams>();
    out.setDemod(rpc::schema::Demod::USB);
    out.initNotches(5);
    CHECK_FALSE(rpc::read_vrx_params(out.asReader()).has_value());
}

TEST_CASE("a passband frame's notch marks, carriers and raw row cross the wire",
          "[rpc][notch]") {
    std::vector<float> power{-90.0F, -80.0F, -70.0F};
    std::vector<float> raw{-91.0F, -81.0F, -71.0F};
    const std::vector<engine::PassbandNotchMark> marks{{-1'000.0, 100.0, 40.0, 0},
                                                       {1'000.0, 100.0, 40.0, 0}};
    const std::vector<engine::PassbandCarrier> carriers{{750.5, 12.0, 25.0F, 4.0F}};

    engine::PassbandFrame in;
    in.vrx = engine::VrxId{7};
    in.power_db = power;
    in.raw_power_db = raw;
    in.notch_marks = marks;
    in.notch_audio_only = true;
    in.carriers = carriers;

    capnp::MallocMessageBuilder message;
    auto out = message.initRoot<rpc::schema::PassbandFrame>();
    rpc::write_passband_frame(out, in);
    const auto reader = out.asReader();

    REQUIRE(reader.getNotchMarks().size() == 2);
    CHECK(reader.getNotchMarks()[0].getOffsetHz() == -1'000.0);
    CHECK(reader.getNotchMarks()[1].getDepthDb() == 40.0);
    CHECK(reader.getNotchAudioOnly());
    REQUIRE(reader.getCarriers().size() == 1);
    CHECK(reader.getCarriers()[0].getOffsetHz() == 750.5);
    CHECK(reader.getCarriers()[0].getAgeSeconds() == 4.0F);
    REQUIRE(reader.getRawPowerDb().size() == 3);
    CHECK(reader.getRawPowerDb()[2] == -71.0F);
}

TEST_CASE("notches and the split flag reach the engine and its frames over a session",
          "[gpu][rpc][notch]") {
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
    params.notches[2] = rpc::NotchEntry{true, 1'900, 40.0, 120};
    params.display_split = true;

    auto id = harness.client().add_vrx(params);
    INFO(test::message_of(id));
    REQUIRE(id.has_value());

    // What the engine holds, read back through convert.cpp's writer and the
    // client's reader.
    auto status = harness.client().vrx_status(*id);
    INFO(test::message_of(status));
    REQUIRE(status.has_value());
    CHECK(status->params.notch_hz == 800);
    CHECK(status->params.notches[0] == rpc::NotchEntry{true, 800, 30.0, 100});
    CHECK(status->params.notches[2] == params.notches[2]);
    CHECK_FALSE(status->params.notches[1].enabled);
    CHECK(status->params.display_split);

    struct Seen {
        std::mutex lock;
        std::vector<rpc::PassbandFrame> frames;
    };
    auto seen = std::make_shared<Seen>();
    const auto subscribed =
        harness.client().subscribe_passband(*id, 1, [seen](const rpc::PassbandFrame& frame) {
            std::scoped_lock held(seen->lock);
            if (seen->frames.size() < 16) {
                seen->frames.push_back(frame);
            }
        });
    INFO(test::message_of(subscribed));
    REQUIRE(subscribed.has_value());
    REQUIRE(harness.start_engine().has_value());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(8000);
    std::size_t got = 0;
    while (std::chrono::steady_clock::now() < deadline && got < 4) {
        {
            std::scoped_lock held(seen->lock);
            got = seen->frames.size();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    harness.client().unsubscribe_passband(*id);
    CHECK(harness.stop_engine().has_value());

    std::vector<rpc::PassbandFrame> frames;
    {
        std::scoped_lock held(seen->lock);
        frames = seen->frames;
    }
    REQUIRE(frames.size() >= 4);
    for (const rpc::PassbandFrame& frame : frames) {
        // USB places each notch once, at +f.
        REQUIRE(frame.notch_marks.size() == 2);
        CHECK(frame.notch_marks[0] == rpc::PassbandNotchMark{800.0, 100.0, 30.0, 0});
        CHECK(frame.notch_marks[1] == rpc::PassbandNotchMark{1'900.0, 120.0, 40.0, 2});
        CHECK_FALSE(frame.notch_audio_only);
        CHECK(frame.raw_power_db.size() == frame.power_db.size());
    }
}
