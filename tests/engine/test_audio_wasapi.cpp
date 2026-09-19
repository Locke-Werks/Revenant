// The WASAPI render backend, tested without a sound device and without making
// a sound.
//
// Almost everything that goes wrong in a device backend goes wrong where no
// device is involved: the option validation, the failure message for an id
// that does not exist, and the close() contract. That last one is the reason
// this file exists at all. The egress layer frees a receiver's ring the moment
// close() returns, so a close that does not join the render thread is a
// use-after-free on a lock-free ring, which reproduces as an intermittent
// crash under load weeks later and never in a debugger.
//
// The one case that does touch hardware feeds an empty ring, which
// read_or_fill turns into silence, and then multiplies it by a gain of zero.
// Two independent reasons for the speakers to stay quiet, because a test that
// makes noise on somebody's machine is a test they turn off.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/engine/audio_egress.h"
#include "core/engine/audio_wasapi.h"
#include "core/engine/spsc_ring.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kAudioRate = 48'000;

// Well formed for an endpoint id and belonging to nothing. The shape matters:
// an id the enumerator rejects as malformed and an id it simply does not hold
// take different paths through IMMDeviceEnumerator::GetDevice, and the one
// worth testing is the second.
constexpr const char* kAbsentDeviceId =
    "{0.0.0.00000000}.{deadbeef-dead-beef-dead-beefdeadbeef}";

engine::AudioStreamInfo mono_stream()
{
    engine::AudioStreamInfo info;
    info.vrx = engine::VrxId{1};
    info.rate = kAudioRate;
    info.channels = 1;
    info.center = 162'550'000;
    info.demod = engine::Demod::Nfm;
    info.label = "wasapi test";
    info.created = "2026-09-18T00:00:00Z";
    return info;
}

}  // namespace

TEST_CASE("enumerating the render endpoints answers rather than throwing", "[engine][audio][wasapi]")
{
    // A machine with no sound card is not a failure of this call, and neither
    // is a machine where opening a device would fail: the whole point of
    // enumerating without activating anything is that the answer survives
    // both. What is not acceptable is a silent empty result that was really
    // an error, so the two are distinguished here.
    const auto devices = engine::enumerate_audio_devices();

    if (!devices) {
        INFO("enumerate_audio_devices: " << devices.error().message);
        REQUIRE_FALSE(devices.error().message.empty());
        return;
    }

    int defaults = 0;
    for (const auto& device : *devices) {
        INFO("device '" << device.name << "' id '" << device.id << "' mixing at "
                        << device.mix_rate << " Hz in " << device.mix_channels << " channels");

        // The id is what goes on a command line or in a session file. An entry
        // without one offers the operator something they cannot then ask for.
        REQUIRE_FALSE(device.id.empty());
        REQUIRE_FALSE(device.name.empty());

        if (device.is_default) {
            ++defaults;
        }
    }

    REQUIRE(defaults <= 1);
    if (!devices->empty()) {
        REQUIRE(defaults == 1);
    }
}

#ifdef _WIN32

TEST_CASE("the WASAPI backend is a Pull backend and has no write()", "[engine][audio][wasapi]")
{
    auto backend = engine::make_wasapi_backend();
    REQUIRE(backend.has_value());
    REQUIRE(*backend != nullptr);

    REQUIRE((*backend)->kind() == engine::AudioBackendKind::Pull);

    // A Push backend is driven by the egress drain thread calling write(). A
    // device reads from you instead, so write() here is a wiring mistake and
    // has to say so rather than accepting samples nothing will ever play.
    const float sample = 0.0F;
    const auto written = (*backend)->write(std::span<const float>(&sample, 1));
    REQUIRE_FALSE(written.has_value());
    INFO("write: " << written.error().message);
    REQUIRE_FALSE(written.error().message.empty());
}

TEST_CASE("opening a device id that does not exist names the id", "[engine][audio][wasapi]")
{
    engine::WasapiOptions options;
    options.device_id = kAbsentDeviceId;

    // Not following the default: the option only applies to the default
    // endpoint, and a named device that is absent has not been replaced by
    // anything.
    options.follow_default = false;

    auto backend = engine::make_wasapi_backend(options);
    REQUIRE(backend.has_value());

    const auto opened = (*backend)->open(mono_stream());
    REQUIRE_FALSE(opened.has_value());

    INFO("open: " << opened.error().message);

    // The operator's next move is to check the id they typed against the list,
    // so the id has to be in the message. An HRESULT on its own sends them to
    // a search engine.
    REQUIRE(opened.error().message.find(kAbsentDeviceId) != std::string::npos);

    // The failure path still owns a thread and four handles until close()
    // runs, and calling it twice is part of the contract.
    REQUIRE((*backend)->close().has_value());
    REQUIRE((*backend)->close().has_value());
}

TEST_CASE("the backend refuses a stream it cannot carry", "[engine][audio][wasapi]")
{
    SECTION("a rate no device can be asked for")
    {
        auto backend = engine::make_wasapi_backend();
        REQUIRE(backend.has_value());

        auto info = mono_stream();
        info.rate = 0;
        REQUIRE_FALSE((*backend)->open(info).has_value());
    }

    SECTION("a backlog cap under the buffer, which trims what the device is about to ask for")
    {
        engine::WasapiOptions options;
        options.buffer_ms = 50.0;
        options.max_backlog_ms = 10.0;

        auto backend = engine::make_wasapi_backend(options);
        REQUIRE(backend.has_value());

        const auto opened = (*backend)->open(mono_stream());
        REQUIRE_FALSE(opened.has_value());
        INFO("open: " << opened.error().message);
        REQUIRE(opened.error().message.find("backlog") != std::string::npos);
    }

    SECTION("a tap before there is a device")
    {
        auto backend = engine::make_wasapi_backend();
        REQUIRE(backend.has_value());
        REQUIRE_FALSE((*backend)->attach(engine::AudioTap{}).has_value());
    }
}

TEST_CASE("closing a backend that was opened and never attached returns", "[engine][audio][wasapi]")
{
    const auto devices = engine::enumerate_audio_devices();
    if (!devices || devices->empty()) {
        SKIP("no active audio render endpoint on this machine");
    }

    // add_receiver opens the backend and then fails somewhere before attach(),
    // which leaves a render thread parked waiting for a tap that is never
    // coming. If close() does not also unblock that wait, this test hangs
    // rather than failing, which is the point of having it: the same hang in
    // the engine takes the whole process down at shutdown with no output.
    auto backend = engine::make_wasapi_backend();
    REQUIRE(backend.has_value());

    const auto opened = (*backend)->open(mono_stream());
    INFO("open: " << (opened.has_value() ? std::string{"ok"} : opened.error().message));
    REQUIRE(opened.has_value());

    REQUIRE((*backend)->close().has_value());
}

TEST_CASE("a full cycle against the default endpoint joins its render thread",
          "[engine][audio][wasapi]")
{
    const auto devices = engine::enumerate_audio_devices();
    if (!devices) {
        SKIP("the audio endpoints could not be enumerated: " + devices.error().message);
    }
    if (devices->empty()) {
        SKIP("no active audio render endpoint on this machine");
    }

    // One second of mono audio, primed with a quarter second of digital
    // silence. Silence rather than a tone because a test that makes a noise
    // on somebody's machine is a test they disable, and primed rather than
    // empty because the backend waits for one device buffer before it starts
    // the stream: an empty ring would leave the render thread in that wait
    // for the whole of this case and prove nothing about it.
    auto ring = engine::SpscRing<float>::create(static_cast<std::size_t>(kAudioRate));
    REQUIRE(ring.has_value());

    const std::vector<float> silence(static_cast<std::size_t>(kAudioRate) / 4, 0.0F);
    REQUIRE(ring->get()->write(std::span<const float>(silence)) == silence.size());

    engine::WasapiOptions options;
    options.buffer_ms = 30.0;
    options.max_backlog_ms = 100.0;

    // Belt and braces against making a noise: the ring is already empty, and
    // zero gain exercises the multiply while guaranteeing silence even if it
    // were not.
    options.gain = 0.0F;

    auto backend = engine::make_wasapi_backend(options);
    REQUIRE(backend.has_value());

    const auto opened = (*backend)->open(mono_stream());
    INFO("open: " << (opened.has_value() ? std::string{"ok"} : opened.error().message));
    REQUIRE(opened.has_value());

    engine::AudioTap tap(ring->get(), kAudioRate, 1);
    REQUIRE((*backend)->attach(tap).has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // The device asked for buffers and got them. If nothing was read the
    // render thread never ran, and everything below would pass for the wrong
    // reason.
    const std::uint64_t consumed = ring->get()->total_read();
    INFO("samples the device consumed while running: " << consumed);
    INFO("underrun events while running: " << tap.underrun_events());
    REQUIRE(consumed > 0);

    REQUIRE((*backend)->close().has_value());

    // close() returning is supposed to mean no thread of the backend's can
    // touch the tap again, which is what lets the egress layer free the ring
    // on the next line. A thread that outlived the close would keep reading,
    // and a quarter second of audio is still in the ring for it to read.
    const std::uint64_t after_close = ring->get()->total_read();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(ring->get()->total_read() == after_close);

    // Idempotent, and this is the call that would deadlock or double-join if
    // the first one had left state behind.
    REQUIRE((*backend)->close().has_value());

    ring->reset();
}

#else  // _WIN32

TEST_CASE("the WASAPI backend says it is Windows only rather than failing to link",
          "[engine][audio][wasapi]")
{
    const auto backend = engine::make_wasapi_backend();
    REQUIRE_FALSE(backend.has_value());
    REQUIRE(backend.error().message.find("platform") != std::string::npos);
}

#endif  // _WIN32
