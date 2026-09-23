// The RTL-SDR backend.
//
// Split in two on purpose. The URI layer is host logic and runs everywhere,
// including on a build machine with nothing plugged in, because a typo that
// falls back to a default is exactly the failure that never shows up as a
// crash. Everything that needs the radio skips itself with a reason when
// there is no radio, so a green CI run on a machine without a dongle means
// "those cases did not apply" and says so, rather than meaning nothing.
//
// What the device cases are actually checking is that the Paced contract
// holds against real hardware: the callback thread is joined before stop
// returns, the counters add up, and the samples are samples rather than a
// stream of nothing reported as success. That last one is the case that
// earns its keep. A source that opens, starts, delivers the right number of
// zero bytes and stops passes every structural assertion there is.
//
// EVERY CASE THAT OPENS THE DONGLE IS TAGGED [dongle] AND HOLDS THE LOCK.
// tests/support/dongle_lock.h's hold_the_dongle() takes the machine-wide lock
// the backend takes, for the whole case, and skips with the reason when another
// Revenant process has the radio. tests/engine/CMakeLists.txt registers the
// [dongle] cases under the ctest label dongle with a RESOURCE_LOCK, so ctest
// never runs two at once and `ctest -LE dongle` leaves them all out.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <rtl-sdr.h>

#include "core/dsp/types.h"
#include "core/source/device_lock.h"
#include "core/source/registry.h"
#include "core/source/rtlsdr_lock.h"
#include "core/source/rtlsdr_source.h"
#include "tests/support/dongle_lock.h"

using namespace revenant;

namespace {

// True when librtlsdr can see at least one dongle. Enumeration opens nothing,
// so asking is cheap and safe even while another test is streaming.
[[nodiscard]] bool a_dongle_is_attached() {
    auto attached = source::enumerate_rtlsdr_devices();
    return attached.has_value() && !attached->empty();
}

constexpr const char* kNoDongle = "no RTL-SDR is attached to this machine";

// Everything a run of the device collects, gathered under one lock because
// the sink is called on the source's delivery thread and the assertions run
// on the test's.
struct Collected {
    std::mutex lock;
    std::uint64_t blocks = 0;
    std::uint64_t samples = 0;
    std::uint64_t dropped_reported = 0;
    dsp::SampleIndex first_index = 0;
    dsp::SampleIndex next_expected = 0;
    bool indices_contiguous = true;
    bool format_was_cu8 = true;
    bool sizes_matched_bytes = true;

    // A histogram over the 256 byte values, which is how the "not all
    // identical and not all zero" check is made without keeping the samples.
    std::vector<std::uint64_t> byte_counts = std::vector<std::uint64_t>(256, 0);
};

[[nodiscard]] source::BlockSink collecting_sink(Collected& into) {
    return [&into](const source::SourceBlock& block) -> Status {
        std::scoped_lock guard(into.lock);

        if (block.format != source::SampleFormat::Cu8) {
            into.format_was_cu8 = false;
        }
        if (block.bytes.size() != block.sample_count * source::bytes_per_sample(block.format)) {
            into.sizes_matched_bytes = false;
        }

        if (into.blocks == 0) {
            into.first_index = block.stamp.start;
        } else if (block.stamp.start != into.next_expected + block.dropped_before) {
            into.indices_contiguous = false;
        }
        into.next_expected = block.stamp.start + block.sample_count;

        into.dropped_reported += block.dropped_before;
        ++into.blocks;
        into.samples += block.sample_count;

        // Sampled rather than exhaustive: at 2.4 MS/s a full histogram is
        // several million increments a second on the delivery thread, which
        // would turn the test itself into the overrun it is checking for.
        const std::size_t stride = std::max<std::size_t>(1, block.bytes.size() / 4096);
        for (std::size_t i = 0; i < block.bytes.size(); i += stride) {
            ++into.byte_counts[std::to_integer<std::size_t>(block.bytes[i])];
        }
        return {};
    };
}

}  // namespace

// ---------------------------------------------------------------------------
// The URI layer, which needs no hardware
// ---------------------------------------------------------------------------

TEST_CASE("an unknown rtlsdr query parameter is refused by name", "[source][rtlsdr]") {
    // The rule the whole registry is built around. A capture taken at the
    // default rate because somebody typed "rat=" has metadata that is wrong,
    // and every frequency derived from it afterwards is wrong by the same
    // ratio with nothing anywhere to catch it.
    auto opened = source::open_source("rtlsdr://0?freq=100000000&rat=2400000");
    REQUIRE_FALSE(opened.has_value());

    const std::string& message = opened.error().message;
    INFO(message);
    CHECK(message.find("rat") != std::string::npos);
    CHECK(message.find("rtlsdr") != std::string::npos);
}

TEST_CASE("a rate outside the hardware's windows is refused and the windows are named",
          "[source][rtlsdr]") {
    // 500 kS/s sits in the hole between the RTL2832U's two windows, which is
    // the case a single min/max pair cannot express and the one a caller is
    // most likely to reach for.
    auto opened = source::open_source("rtlsdr://0?freq=100000000&rate=500000");
    REQUIRE_FALSE(opened.has_value());

    const std::string& message = opened.error().message;
    INFO(message);
    CHECK(message.find("225001") != std::string::npos);
    CHECK(message.find("300000") != std::string::npos);
    CHECK(message.find("900001") != std::string::npos);
    CHECK(message.find("3200000") != std::string::npos);

    // And the windows themselves are honoured at both edges rather than only
    // in the middle.
    CHECK(source::rtlsdr_rate_supported(225'001));
    CHECK(source::rtlsdr_rate_supported(300'000));
    CHECK(source::rtlsdr_rate_supported(900'001));
    CHECK(source::rtlsdr_rate_supported(3'200'000));
    CHECK_FALSE(source::rtlsdr_rate_supported(225'000));
    CHECK_FALSE(source::rtlsdr_rate_supported(300'001));
    CHECK_FALSE(source::rtlsdr_rate_supported(900'000));
    CHECK_FALSE(source::rtlsdr_rate_supported(3'200'001));
}

TEST_CASE("an rtlsdr URI takes the project's frequency spellings", "[source][rtlsdr]") {
    // One grammar for a frequency across the whole project. 100.1M in a URI
    // has to mean what 100.1M means to --vrx, or a number depends on where it
    // was typed.
    auto direct = source::parse_frequency("100.1M", "the test");
    REQUIRE(direct.has_value());
    CHECK(*direct == 100'100'000);

    // Through the URI, where a bad spelling has to be refused rather than
    // rounded.
    auto bad = source::open_source("rtlsdr://0?freq=100.1MHzish");
    REQUIRE_FALSE(bad.has_value());
    INFO(bad.error().message);
    CHECK(bad.error().message.find("freq") != std::string::npos);
}

TEST_CASE("an rtlsdr URI needs a device and names both forms", "[source][rtlsdr]") {
    auto opened = source::open_source("rtlsdr://");
    REQUIRE_FALSE(opened.has_value());
    INFO(opened.error().message);
    CHECK(opened.error().message.find("rtlsdr://0") != std::string::npos);
}

TEST_CASE("a URI that says nothing about gain gets a number and not the AGC",
          "[source][rtlsdr]") {
    // The default was gain=auto until 2026-09-21. Measured on air the day
    // before, an RTL-SDR v3 at 95.1 MHz in a suburban FM environment: with
    // auto the wideband detector reported three intermodulation products as
    // real tracks at confidence 1.00, and gain=20 improved KKFM's measured
    // SNR by 5.7 dB and removed all three. Every example in the tree used
    // auto, so the default path was the one that manufactured signals.
    //
    // Asserted on the config struct and not through a URI, because parsing
    // one means opening a device. registry.cpp leaves the struct's own
    // defaults alone when the key is absent, deliberately, so this is the
    // value a URI without the key produces and it is also what a caller
    // building the config by hand gets.
    const source::RtlSdrSourceConfig fresh;
    CHECK_FALSE(fresh.gain_auto);
    CHECK(fresh.gain_db == source::kRtlSdrDefaultGainDb);

    // A starting point rather than a right answer, and the refusal says so
    // rather than leaving an operator to find the number in a header.
    auto opened = source::open_source("rtlsdr://0?gain=loud");
    REQUIRE_FALSE(opened.has_value());
    INFO(opened.error().message);
    CHECK(opened.error().message.find("gain='loud'") != std::string::npos);
    CHECK(opened.error().message.find("20") != std::string::npos);
}

TEST_CASE("a bad direct sampling mode is refused", "[source][rtlsdr]") {
    auto opened = source::open_source("rtlsdr://0?direct=yes");
    REQUIRE_FALSE(opened.has_value());
    INFO(opened.error().message);
    CHECK(opened.error().message.find("direct") != std::string::npos);
}

TEST_CASE("opening an index that is not there fails with a clear message",
          "[source][rtlsdr]") {
    // Deliberately not skipped when nothing is attached: the message differs
    // between "no device at all" and "not that many", and both are worth
    // being clear.
    auto opened = source::open_source("rtlsdr://97?freq=100000000");
    REQUIRE_FALSE(opened.has_value());

    const std::string& message = opened.error().message;
    INFO(message);
    CHECK(message.find("97") != std::string::npos);

    if (a_dongle_is_attached()) {
        CHECK(message.find("index") != std::string::npos);
    } else {
        CHECK(message.find("no RTL-SDR is attached") != std::string::npos);
    }
}

TEST_CASE("a serial that no dongle carries fails by name", "[source][rtlsdr][dongle]") {
    // The leading zeros are what make this a serial rather than an index.
    // That rule is the whole of the ambiguity in rtlsdr://<body>, so it is
    // checked rather than assumed.
    //
    // A [dongle] case although it opens nothing it keeps: looking a serial up
    // opens every attached dongle to read its string descriptors. With none
    // attached it still runs, because "nothing is attached" is a message worth
    // checking too.
    std::optional<source::DeviceLock> radio_lock;
    if (a_dongle_is_attached()) {
        radio_lock = test::hold_the_dongle();
    }
    auto opened = source::open_source("rtlsdr://0000deadbeef");
    REQUIRE_FALSE(opened.has_value());
    INFO(opened.error().message);
    CHECK(opened.error().message.find("0000deadbeef") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

TEST_CASE("every enumerated source has a usable uri and a name", "[source][rtlsdr]") {
    auto listed = source::enumerate_sources();
    REQUIRE(listed.has_value());
    REQUIRE_FALSE(listed->empty());

    for (const source::SourceDescriptor& descriptor : *listed) {
        INFO(descriptor.uri);
        CHECK_FALSE(descriptor.uri.empty());
        CHECK_FALSE(descriptor.display_name.empty());
        CHECK_FALSE(descriptor.backend.empty());
    }

    const bool has_rtlsdr =
        std::any_of(listed->begin(), listed->end(), [](const source::SourceDescriptor& d) {
            return d.backend == "rtlsdr";
        });

    if (!a_dongle_is_attached()) {
        // The other direction matters as much: a device list that invents
        // entries is worse than a short one.
        CHECK_FALSE(has_rtlsdr);
        SKIP(kNoDongle);
    }

    CHECK(has_rtlsdr);
}

// ---------------------------------------------------------------------------
// The device
// ---------------------------------------------------------------------------

TEST_CASE("an attached dongle describes itself honestly", "[source][rtlsdr][device][dongle]") {
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    auto opened = source::open_source("rtlsdr://0?rate=2400000&freq=100M");
    if (!opened) {
        SKIP("the dongle could not be opened: " + opened.error().message);
    }
    const source::SourceCapabilities& caps = (*opened)->capabilities();

    CHECK(caps.backend == "rtlsdr");
    CHECK_FALSE(caps.display_name.empty());

    // Paced, not Demand. Everything else about this backend follows from it.
    CHECK(caps.flow == source::FlowControl::Paced);
    CHECK_FALSE(caps.seekable);
    CHECK(caps.length_samples == 0);

    // Native bytes. A backend reporting Cf32 here would be converting on the
    // host and quadrupling what crosses the bus.
    CHECK(caps.native_format == source::SampleFormat::Cu8);
    CHECK(caps.bits_per_component == 8);
    CHECK(source::bytes_per_sample(caps.native_format) == 2);

    // An undisciplined crystal. Zero here would be a claim nothing can back.
    CHECK(caps.timestamp_accuracy_ns > 0);
    CHECK(caps.clock_sources.size() == 1);
    CHECK(caps.clock_sources.front() == source::ClockSource::Internal);

    const source::ClockQuality clock = (*opened)->clock();
    CHECK(clock.source == source::ClockSource::Internal);
    CHECK_FALSE(clock.disciplined);
    CHECK(clock.accuracy_ns > 0);

    CHECK(caps.preferred_block_samples > 0);
    CHECK(caps.min_rate == source::kRtlSdrLowRateMin);
    CHECK(caps.max_rate == source::kRtlSdrHighRateMax);

    // Seeking is refused by capability rather than by symptom.
    auto sought = (*opened)->seek(1000);
    REQUIRE_FALSE(sought.has_value());
    INFO(sought.error().message);
    CHECK(sought.error().message.find("Paced") != std::string::npos);
}

TEST_CASE("tuning reports where the tuner landed", "[source][rtlsdr][device][dongle]") {
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    // Opened somewhere else, so tune() below is a real retune rather than a
    // readback of where the URI already put it. Opened with a frequency at
    // all because librtlsdr re-tunes the handle's current frequency as a side
    // effect of setting the rate, and an R820T asked to lock DC prints "PLL
    // not locked" on its way to failing.
    auto opened = source::open_source("rtlsdr://0?rate=2400000&freq=88M");
    if (!opened) {
        SKIP("the dongle could not be opened: " + opened.error().message);
    }

    constexpr dsp::Hertz kWanted = 100'000'000;
    auto landed = (*opened)->tune(kWanted);
    REQUIRE(landed.has_value());

    // The difference between asked and got is exact because both are integer
    // hertz, which is the whole reason Hertz is not a double. The tolerance
    // is one part in ten thousand of the request, which is far wider than any
    // R820T PLL step at VHF and still narrow enough to catch a backend that
    // tuned somewhere else entirely or echoed a stale value.
    const dsp::Hertz offset = *landed - kWanted;
    INFO("asked " << kWanted << " Hz, landed " << *landed << " Hz, offset " << offset << " Hz");
    CHECK(std::abs(offset) <= kWanted / 10'000);

    // center() agrees with what tune() returned. A backend where these two
    // disagree has a capture whose declared centre is not where it was
    // listening.
    CHECK((*opened)->center() == *landed);

    // Out of range is refused by name rather than attempted.
    auto impossible = (*opened)->tune(9'000'000'000);
    REQUIRE_FALSE(impossible.has_value());
    INFO(impossible.error().message);
}

TEST_CASE("a manual gain snaps to a step the tuner has", "[source][rtlsdr][device][dongle]") {
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    auto opened = source::open_source("rtlsdr://0?rate=2400000&freq=100M");
    if (!opened) {
        SKIP("the dongle could not be opened: " + opened.error().message);
    }
    const source::SourceCapabilities& caps = (*opened)->capabilities();

    if (caps.gain_stages.empty()) {
        SKIP("this dongle reports no tuner gain table");
    }
    const source::GainStage& stage = caps.gain_stages.front();
    REQUIRE_FALSE(stage.steps_db.empty());
    CHECK(stage.has_auto);

    // A value deliberately between two steps. What comes back has to be one
    // of the steps, not the request.
    const double between = (stage.steps_db.front() + stage.steps_db.back()) / 2.0 + 0.37;
    auto achieved = (*opened)->set_gain(stage.name, between);
    REQUIRE(achieved.has_value());

    const bool on_a_step =
        std::any_of(stage.steps_db.begin(), stage.steps_db.end(),
                    [&achieved](double step) { return std::abs(step - *achieved) < 0.05; });
    INFO("asked " << between << " dB, got " << *achieved << " dB");
    CHECK(on_a_step);

    REQUIRE((*opened)->set_gain_auto(stage.name, true).has_value());

    // A stage the device does not have is named rather than ignored.
    auto missing = (*opened)->set_gain("lna", 20.0);
    REQUIRE_FALSE(missing.has_value());
    INFO(missing.error().message);
    CHECK(missing.error().message.find("lna") != std::string::npos);
}

TEST_CASE("a streaming dongle takes a gain change and the automatic mode",
          "[source][rtlsdr][device][dongle]") {
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    // THE CASE ABOVE SETS GAIN ON A DONGLE THAT IS NOT STREAMING, which is the
    // same blind spot the retune had: rtlsdr_set_tuner_gain_mode and
    // rtlsdr_set_tuner_gain are vendor control transfers through the same I2C
    // repeater rtlsdr_set_center_freq uses, and the platform stalls all of them
    // once rtlsdr_read_async has been running for about half a second.
    //
    // Reported by an operator on 2026-09-21: broadcast FM sounded bad, they
    // reached for the dongle's automatic gain from the window, and the window
    // locked up. Only tune paused the transfers at that point, so the gain call
    // went straight at a streaming dongle and took the stall.
    //
    // Waits past the boundary for the reason the streaming retune case gives at
    // length: a gain change in the first fraction of a second would go through
    // on its own and prove nothing.
    constexpr auto kPastTheBoundary = std::chrono::milliseconds(1500);

    auto opened = source::open_source("rtlsdr://0?rate=2400000&freq=98.1M&gain=20");
    if (!opened) {
        SKIP("the dongle could not be opened: " + opened.error().message);
    }
    source::Source& radio = **opened;

    const source::SourceCapabilities& caps = radio.capabilities();
    if (caps.gain_stages.empty()) {
        SKIP("this dongle reports no tuner gain table");
    }
    const source::GainStage& stage = caps.gain_stages.front();
    REQUIRE_FALSE(stage.steps_db.empty());

    Collected collected;
    source::StreamOptions options;
    options.block_samples = 32'768;
    REQUIRE(radio.start(options, collecting_sink(collected)).has_value());

    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        std::uint64_t so_far = 0;
        {
            std::scoped_lock guard(collected.lock);
            so_far = collected.samples;
        }
        if ((so_far >= 300'000 && now - started >= kPastTheBoundary) ||
            now - started >= std::chrono::seconds(10)) {
            REQUIRE(so_far >= 300'000);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(radio.running());

    // A step the tuner has, and not the one it is already on, so a call that
    // quietly did nothing would show up as the readback not moving.
    const double wanted = stage.steps_db.size() > 1
                              ? stage.steps_db[stage.steps_db.size() / 2]
                              : stage.steps_db.front();
    auto achieved = radio.set_gain(stage.name, wanted);
    if (!achieved) {
        INFO(achieved.error().message);
    }
    REQUIRE(achieved.has_value());
    INFO("asked " << wanted << " dB while streaming, got " << *achieved << " dB");
    CHECK(std::abs(*achieved - wanted) < 0.05);

    // And the stream is still running, which is the half a successful gain
    // change could still get wrong.
    std::uint64_t at_change = 0;
    {
        std::scoped_lock guard(collected.lock);
        at_change = collected.samples;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    {
        std::scoped_lock guard(collected.lock);
        INFO("samples at the gain change " << at_change << ", after " << collected.samples);
        CHECK(collected.samples > at_change);
    }

    // The automatic mode, which is the call that froze. Whether the AGC is a
    // good idea is a separate question and README.md has the measurement; this
    // is only about the call completing against a live stream.
    if (stage.has_auto) {
        auto automatic = radio.set_gain_auto(stage.name, true);
        if (!automatic) {
            INFO(automatic.error().message);
        }
        REQUIRE(automatic.has_value());
        CHECK(radio.running());

        auto back = radio.set_gain_auto(stage.name, false);
        if (!back) {
            INFO(back.error().message);
        }
        REQUIRE(back.has_value());
    }

    REQUIRE(radio.stop().has_value());
}

TEST_CASE("a dongle streams, stops cleanly and its counters add up",
          "[source][rtlsdr][device][dongle]") {
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    constexpr dsp::SampleRate kRate = 2'400'000;
    constexpr std::uint64_t kWanted = 300'000;

    auto opened = source::open_source("rtlsdr://0?rate=2400000&freq=100M&gain=20");
    if (!opened) {
        SKIP("the dongle could not be opened: " + opened.error().message);
    }
    source::Source& radio = **opened;

    // The achieved rate, not the requested one. The RTL2832U resamples with a
    // fractional divider and lands nearby when a request is not exactly
    // representable, so this is a tolerance rather than an equality.
    INFO("asked " << kRate << " S/s, got " << radio.sample_rate() << " S/s");
    CHECK(std::llabs(radio.sample_rate() - kRate) <= kRate / 1'000);

    Collected collected;
    source::StreamOptions options;
    options.block_samples = 32'768;

    REQUIRE(radio.start(options, collecting_sink(collected)).has_value());
    CHECK(radio.running());

    // A tenth of a second of samples at 2.4 MS/s, with a wide margin for a
    // dongle that takes a moment to start delivering.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::scoped_lock guard(collected.lock);
            if (collected.samples >= kWanted) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(radio.stop().has_value());
    CHECK_FALSE(radio.running());

    // THE JOIN. If the callback thread were still alive, this sleep is long
    // enough for several more transfers to land, and the sink writes into a
    // Collected that is about to go out of scope. A count that does not move
    // is the observable form of "the thread is joined"; a count that moves is
    // a use-after-free that happened to be survivable this time.
    std::uint64_t blocks_at_stop = 0;
    std::uint64_t samples_at_stop = 0;
    {
        std::scoped_lock guard(collected.lock);
        blocks_at_stop = collected.blocks;
        samples_at_stop = collected.samples;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        std::scoped_lock guard(collected.lock);
        CHECK(collected.blocks == blocks_at_stop);
        CHECK(collected.samples == samples_at_stop);
    }

    INFO(samples_at_stop << " samples in " << blocks_at_stop << " blocks");
    REQUIRE(samples_at_stop >= kWanted);

    // Every block was what it claimed to be.
    CHECK(collected.format_was_cu8);
    CHECK(collected.sizes_matched_bytes);
    CHECK(collected.first_index == 0);

    // No block asked for more than the caller said it could take.
    CHECK(blocks_at_stop * options.block_samples >= samples_at_stop);

    // The index accounts for every sample, delivered or lost. A gap the
    // source did not report in dropped_before would show up here as a jump.
    CHECK(collected.indices_contiguous);

    const source::SourceStats stats = radio.stats();
    INFO("delivered " << stats.samples_delivered << ", lost " << stats.samples_lost << " in "
                      << stats.overrun_events << " overruns, write index "
                      << stats.write_index);

    CHECK(stats.samples_delivered == samples_at_stop);
    CHECK(stats.blocks_delivered == blocks_at_stop);
    CHECK(stats.write_index == collected.next_expected);

    // Every sample the source lost is either in the index as a gap or has
    // not been reported yet because nothing was delivered after it. Not an
    // equality, for that tail: a drop that happens after the last block the
    // sink saw is counted but has no later block to be attached to.
    CHECK(stats.samples_lost >= collected.dropped_reported);
    CHECK(stats.write_index == stats.samples_delivered + collected.dropped_reported);
    if (stats.samples_lost == 0) {
        CHECK(stats.overrun_events == 0);
        CHECK(stats.write_index == stats.samples_delivered);
    }

    // The anchor was placed and is getting worse at the crystal's rate,
    // which is what an undisciplined clock does and what it should say.
    const source::ClockQuality clock = radio.clock();
    CHECK_FALSE(clock.disciplined);
    CHECK(clock.accuracy_ns > 0);

    // A second stop is a no-op rather than a hang or a second error.
    CHECK(radio.stop().has_value());
}

TEST_CASE("a consumer that cannot keep up loses samples and is told exactly which",
          "[source][rtlsdr][device][dongle]") {
    // The Paced contract, exercised rather than reasoned about. Every other
    // device case here runs with a sink that returns instantly and therefore
    // never touches the loss path at all, which means the loss path is the
    // one piece of this backend a green suite would otherwise say nothing
    // about.
    //
    // Not flaky: the device produces 2.4 million samples a second and the
    // sink below retires roughly ten blocks a second, so the queue is full
    // within about a tenth of a second and stays full. What is being checked
    // is not that samples were lost, it is that the ones that survived are
    // still correctly placed in the stream afterwards.
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    auto opened = source::open_source("rtlsdr://0?rate=2400000&freq=100M&gain=20");
    if (!opened) {
        SKIP("the dongle could not be opened: " + opened.error().message);
    }
    source::Source& radio = **opened;

    Collected collected;
    source::StreamOptions options;
    options.block_samples = 32'768;

    source::BlockSink inner = collecting_sink(collected);
    const auto slow = [&inner](const source::SourceBlock& block) -> Status {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return inner(block);
    };

    REQUIRE(radio.start(options, slow).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    REQUIRE(radio.stop().has_value());

    const source::SourceStats stats = radio.stats();
    std::scoped_lock guard(collected.lock);

    INFO("delivered " << stats.samples_delivered << ", lost " << stats.samples_lost << " in "
                      << stats.overrun_events << " overruns, last loss at "
                      << stats.last_loss_index << ", gaps reported to the sink "
                      << collected.dropped_reported);

    // Lost, not buffered. A source that grew a queue instead would show zero
    // here and a memory graph that only goes up.
    CHECK(stats.overrun_events > 0);
    CHECK(stats.samples_lost > 0);
    CHECK(stats.last_loss_index > 0);
    CHECK(stats.last_loss_index < stats.samples_delivered + stats.samples_lost);

    // And the survivors are still in the right place. This is the assertion
    // the whole gap-carrying mechanism exists for: after a hole, the next
    // block's stamp.start is the true index of its first sample and
    // dropped_before says how big the hole was, so a recording with a gap in
    // it has the gap where the gap was.
    CHECK(collected.indices_contiguous);
    CHECK(collected.dropped_reported > 0);
    CHECK(stats.samples_lost >= collected.dropped_reported);
    CHECK(stats.write_index == stats.samples_delivered + collected.dropped_reported);
    CHECK(collected.first_index == 0);
}

TEST_CASE("a dongle delivers signal rather than a stream of nothing",
          "[source][rtlsdr][device][dongle]") {
    // The case that catches the failure every structural assertion misses.
    // A backend that opens, starts, hands over the right number of zero bytes
    // and stops passes everything above this.
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    auto opened = source::open_source("rtlsdr://0?rate=2400000&freq=100M&gain=20");
    if (!opened) {
        SKIP("the dongle could not be opened: " + opened.error().message);
    }
    source::Source& radio = **opened;

    Collected collected;
    source::StreamOptions options;
    options.block_samples = 32'768;

    REQUIRE(radio.start(options, collecting_sink(collected)).has_value());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::scoped_lock guard(collected.lock);
            if (collected.samples >= 300'000) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(radio.stop().has_value());

    std::scoped_lock guard(collected.lock);
    REQUIRE(collected.samples >= 300'000);

    std::uint64_t observed = 0;
    std::size_t distinct = 0;
    for (const std::uint64_t count : collected.byte_counts) {
        observed += count;
        if (count > 0) {
            ++distinct;
        }
    }
    REQUIRE(observed > 1000);

    // Not all zero. An unsigned 8-bit I/Q stream idles at 127 or 128, so a
    // run of literal zeros is a dead USB path rather than a quiet band.
    const std::uint64_t zeros = collected.byte_counts[0];
    INFO(zeros << " zero bytes of " << observed << " sampled, " << distinct
               << " distinct values");
    CHECK(zeros * 2 < observed);

    // Not all identical. Even a receiver pointed at an empty band has thermal
    // noise wobbling the low bits, so a handful of distinct byte values is
    // the floor. A stuck ADC or a buffer that is never written gives one.
    CHECK(distinct >= 8);
}

TEST_CASE("describing every device first does not stop the dongle tuning after",
          "[source][rtlsdr][device][dongle]") {
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    // THE SEQUENCE A DEVICE PICKER PRODUCES, IN ONE PROCESS, which is what
    // separates this from every other case in this file.
    //
    // describe_sources() opens every device to ask it what it can do, and a
    // client that lists before it opens therefore opens the same dongle twice
    // in one process: once to describe it, once to stream from it. libusb state
    // is per process, so that is a different thing from the two-process case
    // `revenant-engine --list` followed by `revenant-engine <uri>` exercises,
    // and it is the one the Qt picker does every time somebody opens the panel.
    //
    // Observed on 2026-09-21 driving the picker by hand: the dongle opened and
    // streamed, frames flowed, and every retune failed inside the tuner with
    // "r82xx_set_freq: failed=-9", which is LIBUSB_ERROR_PIPE on the i2c write.
    // Three opens of the dongle were logged. "tuning reports where the tuner
    // landed" above passes on a fresh open, so the tune path and the hardware
    // are both fine; what this case pins is whether describing the device first
    // is what breaks them.
    auto described = source::describe_sources();
    REQUIRE(described.has_value());

    std::string dongle;
    for (const source::SourceCapabilities& caps : *described) {
        if (caps.backend == "rtlsdr" && caps.available()) {
            dongle = caps.uri;
            break;
        }
    }
    if (dongle.empty()) {
        SKIP("no rtlsdr backend described itself as available");
    }

    // A frequency in the URI for the reason the case above gives: librtlsdr
    // re-tunes the handle's current frequency as a side effect of setting the
    // rate, and an R820T asked to lock DC prints "PLL not locked".
    auto opened = source::open_source(dongle + "?rate=2400000&freq=88M");
    if (!opened) {
        SKIP("the dongle could not be opened after being described: " +
             opened.error().message);
    }

    constexpr dsp::Hertz kWanted = 100'000'000;
    auto landed = (*opened)->tune(kWanted);
    if (!landed) {
        INFO(landed.error().message);
    }
    REQUIRE(landed.has_value());

    const dsp::Hertz offset = *landed - kWanted;
    INFO("asked " << kWanted << " Hz, landed " << *landed << " Hz, offset " << offset << " Hz");
    CHECK(std::abs(offset) <= kWanted / 10'000);
    CHECK((*opened)->center() == *landed);
}

TEST_CASE("a streaming dongle can still be tuned", "[source][rtlsdr][device][dongle]") {
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    // THE ONE ARRANGEMENT NOTHING IN THIS SUITE EVER EXERCISED, and the one
    // every retune from a client actually takes.
    //
    // "tuning reports where the tuner landed" above tunes a dongle that is
    // OPEN AND NOT STREAMING. Every other retune case in the tree is against a
    // synthetic source or a file, both of which refuse in their own words, so
    // the success path of Engine::set_source_center has only ever been checked
    // where it could not run. An operator retuning from the window is always
    // retuning a dongle mid-stream: rtlsdr_set_center_freq is a control
    // transfer issued while rtlsdr_read_async has bulk transfers in flight.
    //
    // Observed on 2026-09-21 driving the Qt picker by hand against a live
    // R820T: samples flowed and frames reached the client, and every retune
    // failed inside the tuner with "r82xx_set_freq: failed=-9", which is
    // LIBUSB_ERROR_PIPE on the i2c write. The stopped-dongle case passes on the
    // same hardware minutes either side of it, so the difference is the
    // streaming.
    //
    // A SECOND AND A HALF BEFORE THE RETUNE, AND THAT NUMBER IS THE TEST.
    //
    // This case used to wait only for 300,000 samples, an eighth of a second at
    // 2.4 MS/s, and it passed against the defect it was written for: a retune
    // that early is inside the window where the control transfer still goes
    // through, and the boundary is at about 500 ms. The probes at the bottom of
    // this file pin it. So waiting long enough is what makes the case mean
    // anything, and shortening this wait to make the suite faster puts the
    // original bug back with a green test over it.
    //
    // THIS CASE PRINTS librtlsdr's OWN FAILURE LINES WHILE PASSING, one set per
    // retune:
    //
    //   rtlsdr_demod_write_reg failed with -9
    //   r82xx_write: i2c wr failed=-9 reg=1a len=1
    //   r82xx_set_freq: failed=-9
    //
    // That is the first attempt after the stream is paused, which is expected to
    // fail and is retried; see kRetunePipeRetries. librtlsdr prints from inside
    // every register accessor, so the only way to remove those lines would be to
    // swallow the library's stderr, which would hide real faults with them.
    constexpr dsp::Hertz kOpenAt = 98'100'000;
    constexpr dsp::Hertz kWanted = 95'100'000;
    constexpr auto kPastTheBoundary = std::chrono::milliseconds(1500);

    auto opened = source::open_source("rtlsdr://0?rate=2400000&freq=98.1M&gain=20");
    if (!opened) {
        SKIP("the dongle could not be opened: " + opened.error().message);
    }
    source::Source& radio = **opened;
    REQUIRE(radio.center() == kOpenAt);

    Collected collected;
    source::StreamOptions options;
    options.block_samples = 32'768;
    REQUIRE(radio.start(options, collecting_sink(collected)).has_value());

    // Streaming for real, and for long enough, before the tune. Both halves
    // matter: samples prove the transfers are running and the clock proves they
    // have been running past the point where the control transfer starts being
    // refused.
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(10);
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        std::uint64_t so_far = 0;
        {
            std::scoped_lock guard(collected.lock);
            so_far = collected.samples;
        }
        if ((so_far >= 300'000 && now - started >= kPastTheBoundary) || now >= deadline) {
            INFO("samples before the tune: " << so_far);
            REQUIRE(so_far >= 300'000);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(radio.running());

    const std::uint64_t lost_before = radio.stats().samples_lost;

    auto landed = radio.tune(kWanted);
    if (!landed) {
        INFO(landed.error().message);
    }
    REQUIRE(landed.has_value());

    const dsp::Hertz offset = *landed - kWanted;
    INFO("asked " << kWanted << " Hz, landed " << *landed << " Hz, offset " << offset << " Hz");
    CHECK(std::abs(offset) <= kWanted / 10'000);
    CHECK(radio.center() == *landed);

    // AND THE STREAM SURVIVES IT. A tune that reports success and kills the
    // transfer loop is worse than one that refuses: the window would show a new
    // centre under a waterfall that has stopped.
    std::uint64_t at_tune = 0;
    {
        std::scoped_lock guard(collected.lock);
        at_tune = collected.samples;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    {
        std::scoped_lock guard(collected.lock);
        INFO("samples at the tune " << at_tune << ", after " << collected.samples);
        CHECK(collected.samples > at_tune);
    }

    // AND THE PAUSE IS DECLARED RATHER THAN SMOOTHED OVER. Retuning a streaming
    // dongle costs about a third of a second of samples, because the transfers
    // have to stop for the control transfer to go through at all. Those samples
    // are counted as lost and the index skips past them, which is what keeps
    // every timestamp after the retune from being early by the length of the
    // pause for the rest of the stream. A retune that reported no loss would
    // mean the skip had not happened.
    const source::SourceStats after = radio.stats();
    INFO("samples lost before the retune " << lost_before << ", after " << after.samples_lost);
    CHECK(after.samples_lost > lost_before);
    CHECK(after.overrun_events >= 1);
    {
        std::scoped_lock guard(collected.lock);
        INFO("dropped_before reported to the sink: " << collected.dropped_reported);
        CHECK(collected.dropped_reported > 0);
        CHECK(collected.indices_contiguous);
    }

    // A SECOND RETUNE, because the first one leaves the device in a state the
    // first one did not start from. Every device case in this file did exactly
    // one, which is how a sequence that wedges the tuner after its first use
    // would have passed.
    auto again = radio.tune(kOpenAt);
    if (!again) {
        INFO(again.error().message);
    }
    REQUIRE(again.has_value());
    CHECK(std::abs(*again - kOpenAt) <= kOpenAt / 10'000);

    REQUIRE(radio.stop().has_value());
}

TEST_CASE("describing every device while one is streaming does not wedge its tuner",
          "[source][rtlsdr][device][dongle]") {
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    // THE SEQUENCE THAT BROKE TUNING IN THE WINDOW, and the only one of the
    // four candidate shapes that is not already covered above.
    //
    // A picker refreshes its list whenever the panel is opened, and the panel
    // gets opened while a source is already running: that is what changing radio
    // twice looks like. Since EngineLink::note_source_epoch started asking for a
    // listing of its own, the client also does this WITHOUT anybody clicking,
    // once per source change, so this case covers an automatic path rather than
    // only an operator's. describe_sources() opens EVERY device to ask it what it
    // can do, and docs/rpc.md is explicit about what that costs on this backend:
    // "for the RTL-SDR backend that is rtlsdr_open, a libusb open, claim and
    // reset." A reset issued against a dongle another handle is streaming from
    // is the mechanism this case exists to catch.
    //
    // The symptom observed by hand on 2026-09-21 fits it exactly: samples kept
    // flowing and frames kept reaching the client, so the streaming handle
    // survived, while every retune failed inside the tuner with
    // "r82xx_set_freq: failed=-9", LIBUSB_ERROR_PIPE on the i2c write. The
    // stopped-dongle tune and the streaming tune both pass on this hardware, and
    // describing before opening passes too; this is what is left.
    constexpr dsp::Hertz kWanted = 95'100'000;

    auto opened = source::open_source("rtlsdr://0?rate=2400000&freq=98.1M&gain=20");
    if (!opened) {
        SKIP("the dongle could not be opened: " + opened.error().message);
    }
    source::Source& radio = **opened;

    Collected collected;
    source::StreamOptions options;
    options.block_samples = 32'768;
    REQUIRE(radio.start(options, collecting_sink(collected)).has_value());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::scoped_lock guard(collected.lock);
            if (collected.samples >= 300'000) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(radio.running());

    // THE LISTING, AGAINST A DONGLE THIS PROCESS IS ALREADY STREAMING FROM.
    // It is expected to report the dongle as unavailable, because the claim
    // cannot succeed while this handle holds it, and that is fine: what must
    // NOT happen is the attempt disturbing the handle that does hold it.
    auto described = source::describe_sources();
    REQUIRE(described.has_value());
    for (const source::SourceCapabilities& caps : *described) {
        if (caps.backend == "rtlsdr") {
            INFO("the listing said: " << (caps.available() ? "available" : caps.unavailable));
        }
    }

    // And the stream is still running, which it was before the listing.
    CHECK(radio.running());

    auto landed = radio.tune(kWanted);
    if (!landed) {
        INFO(landed.error().message);
    }
    REQUIRE(landed.has_value());

    const dsp::Hertz offset = *landed - kWanted;
    INFO("asked " << kWanted << " Hz, landed " << *landed << " Hz, offset " << offset << " Hz");
    CHECK(std::abs(offset) <= kWanted / 10'000);

    REQUIRE(radio.stop().has_value());
}

TEST_CASE("a dongle described, then opened, then streamed can still be tuned",
          "[source][rtlsdr][device][dongle]") {
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    // THE ENGINE'S ACTUAL ORDER, which none of the three cases above is.
    //
    // "describing every device first does not stop the dongle tuning after"
    // describes, opens and tunes WITHOUT STREAMING. "a streaming dongle can
    // still be tuned" streams and tunes WITHOUT DESCRIBING FIRST. "describing
    // every device while one is streaming" describes after the stream started.
    // A client with a picker does all four steps in this order and no other:
    // it lists when the panel opens, opens what was picked, the engine starts
    // the graph, and only then does anybody retune.
    //
    // Measured by driving the picker through UI automation on 2026-09-21: the
    // dongle opened at 98.1 MHz, the frequency axis moved to 96.75-99.15 MHz so
    // the hardware was genuinely there, frames kept arriving, and a retune to
    // 96.5 MHz came back "the tuner refused 96500000 Hz: librtlsdr returned -9".
    // The three cases above pass on that same hardware in the same run, so what
    // is left is the combination.
    constexpr dsp::Hertz kOpenAt = 98'100'000;
    constexpr dsp::Hertz kWanted = 96'500'000;

    auto described = source::describe_sources();
    REQUIRE(described.has_value());

    std::string dongle;
    for (const source::SourceCapabilities& caps : *described) {
        if (caps.backend == "rtlsdr" && caps.available()) {
            dongle = caps.uri;
            break;
        }
    }
    if (dongle.empty()) {
        SKIP("no rtlsdr backend described itself as available");
    }

    // Exactly what the picker composes with a centre typed and the other two
    // boxes left empty, which is the arrangement that failed by hand. The rate
    // and the gain are the backend's own defaults either way, so spelling them
    // out here would be testing a different URI from the one that broke.
    auto opened = source::open_source(dongle + "?freq=98100000");
    if (!opened) {
        SKIP("the dongle could not be opened after being described: " +
             opened.error().message);
    }
    source::Source& radio = **opened;
    REQUIRE(radio.center() == kOpenAt);

    Collected collected;
    source::StreamOptions options;
    options.block_samples = 32'768;
    REQUIRE(radio.start(options, collecting_sink(collected)).has_value());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::scoped_lock guard(collected.lock);
            if (collected.samples >= 300'000) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    {
        std::scoped_lock guard(collected.lock);
        INFO("samples before the tune: " << collected.samples);
        REQUIRE(collected.samples >= 300'000);
    }
    REQUIRE(radio.running());

    auto landed = radio.tune(kWanted);
    if (!landed) {
        INFO(landed.error().message);
    }
    REQUIRE(landed.has_value());

    const dsp::Hertz offset = *landed - kWanted;
    INFO("asked " << kWanted << " Hz, landed " << *landed << " Hz, offset " << offset << " Hz");
    CHECK(std::abs(offset) <= kWanted / 10'000);
    CHECK(radio.center() == *landed);

    REQUIRE(radio.stop().has_value());
}

TEST_CASE("a dongle opened at the bottom of its range can still be tuned away",
          "[source][rtlsdr][device][dongle]") {
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    // THE STATE THE PICKER PUT THE RADIO IN, read off the window on 2026-09-21:
    // the composed URI was "rtlsdr://0?freq=24000000&gain=0" and a tune to
    // 435 MHz came back "the tuner refused 435000000 Hz: librtlsdr returned -9".
    //
    // Neither value was typed. An empty centre box parsed to zero and
    // compose_source_uri clamped zero into the tune envelope, whose low edge on
    // an R820T is 24 MHz exactly; an empty gain box parsed to zero and snapped
    // to the lowest step in the tuner's table. So the radio was opened at the
    // very bottom edge of its tuning range with no gain, which is not a state
    // any other case in this file reaches.
    //
    // This pins whether that state is what breaks the tune. If it passes, the
    // empty-box defaults are still wrong and still worth fixing, and the refusal
    // has another cause.
    constexpr dsp::Hertz kWanted = 435'000'000;

    auto opened = source::open_source("rtlsdr://0?rate=2400000&freq=24000000&gain=0");
    if (!opened) {
        SKIP("the dongle could not be opened at 24 MHz: " + opened.error().message);
    }
    source::Source& radio = **opened;
    INFO("opened at " << radio.center() << " Hz");

    Collected collected;
    source::StreamOptions options;
    options.block_samples = 32'768;
    REQUIRE(radio.start(options, collecting_sink(collected)).has_value());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::scoped_lock guard(collected.lock);
            if (collected.samples >= 300'000) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(radio.running());

    auto landed = radio.tune(kWanted);
    if (!landed) {
        INFO("the tuner said: " << landed.error().message);
    }
    REQUIRE(landed.has_value());

    const dsp::Hertz offset = *landed - kWanted;
    INFO("asked " << kWanted << " Hz, landed " << *landed << " Hz, offset " << offset);
    CHECK(std::abs(offset) <= kWanted / 10'000);

    REQUIRE(radio.stop().has_value());
}

TEST_CASE("a dongle with no centre frequency is refused rather than left at DC",
          "[source][rtlsdr][device][dongle]") {
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    // THE ONLY CASE IN THIS FILE THAT OPENS AN rtlsdr FOR REAL WITHOUT freq=.
    //
    // Nothing in this tree had ever done it. Every documented example and every
    // other case here supplies a frequency, so the path was reachable only from
    // a GUI that omits a key whose box is empty. What it used to do was skip
    // rtlsdr_set_center_freq entirely and leave the tuner where rtlsdr_open put
    // it, at 0 Hz. That is not a quiet no-op: setting the sample rate re-tunes
    // the handle's current frequency as a side effect, so an R820T is asked to
    // lock DC and its PLL does not, leaving a dongle that opened and streamed
    // while pointed at nothing anybody asked for.
    //
    // THIS IS NOT WHY A RETUNE GETS REFUSED, and the comment here used to say it
    // was. A retune is refused on a dongle opened correctly at 98.1 MHz too, for
    // the reason retune_streaming_locked documents, so the two are independent
    // faults that happened to be found in the same hour. What this case pins is
    // only the open.
    auto opened = source::open_source("rtlsdr://0?rate=2400000");

    // A dongle whose tuner can reach DC is a different question and is not this
    // case: tune_ranges_for reports {0, xtal/2} under direct sampling, and an
    // open with no centre is legitimate there. Every tuner this has run on
    // starts above DC, so a success here means the hardware changed and the case
    // needs its own arm rather than a louder assertion.
    if (opened.has_value()) {
        INFO("this dongle opened with no centre, so its tuner reaches DC");
        CHECK((*opened)->center() == 0);
        return;
    }

    INFO(opened.error().message);
    CHECK(opened.error().message.find("needs a centre frequency") != std::string::npos);

    // AND IT NAMES THE RANGE, so the refusal is actionable rather than a
    // complaint. An operator who did not know what to type is exactly who
    // reaches this.
    //
    // Checked as "reaches" plus a bound rather than as a unit: tune_ranges_text
    // writes plain hertz, and asserting "MHz" was this case's own first mistake.
    CHECK(opened.error().message.find("It reaches") != std::string::npos);
    CHECK(opened.error().message.find("1766000000") != std::string::npos);

    // THE DONGLE IS STILL USABLE AFTERWARDS, which is the half that matters and
    // the half a refusal could get wrong. Refusing after having already set the
    // rate would leave the tuner in the same failed state the refusal exists to
    // prevent, so the next open has to work.
    auto again = source::open_source("rtlsdr://0?rate=2400000&freq=98.1M&gain=20");
    if (!again) {
        INFO(again.error().message);
    }
    REQUIRE(again.has_value());

    auto landed = (*again)->tune(95'100'000);
    if (!landed) {
        INFO(landed.error().message);
    }
    REQUIRE(landed.has_value());
    CHECK(std::abs(*landed - 95'100'000) <= 95'100'000 / 10'000);
}

TEST_CASE("a dongle already held is refused by name and its holder keeps streaming",
          "[source][rtlsdr][device][dongle]") {
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    // THE CONTENTION CI WAS SUSPECTED OF, from a second handle in this process.
    // Holders in one process share the machine-wide lock, so a second open
    // here still reaches rtlsdr_open and is refused there, and what this pins
    // is that the refusal arrives as an Error naming the index and the call,
    // not as a crash, and that the holder is untouched by it.
    //
    // A SECOND PROCESS NO LONGER GETS THE SAME ANSWER. It waits kRtlSdrOpenWait
    // for the lock and is refused naming the lock, and never reaches
    // rtlsdr_open. WHAT THIS PARAGRAPH USED TO SAY: "A second process gets the
    // same answer: a revenant-cli started on rtlsdr://0 while this suite held
    // it printed "usb_open error -3" and was refused with LIBUSB_ERROR_ACCESS".
    // True of that run, before the lock existed.
    auto holder = source::open_source("rtlsdr://0?rate=2400000&freq=98.1M&gain=20");
    if (!holder) {
        SKIP("the dongle could not be opened, so something else holds it: " +
             holder.error().message);
    }
    source::Source& radio = **holder;

    Collected collected;
    REQUIRE(radio.start(source::StreamOptions{}, collecting_sink(collected)).has_value());
    const auto wait_for = [&collected](std::uint64_t samples) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::scoped_lock guard(collected.lock);
                if (collected.samples >= samples) {
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    REQUIRE(wait_for(300'000));

    // A different centre and gain, so an open that got through would have
    // retuned and re-gained the device the holder is streaming from.
    auto second = source::open_source("rtlsdr://0?rate=2400000&freq=96.5M&gain=10");
    REQUIRE_FALSE(second.has_value());
    INFO(second.error().message);
    CHECK(second.error().message.find("could not open the RTL-SDR at index 0") !=
          std::string::npos);
    CHECK(second.error().message.find("rtlsdr_open returned") != std::string::npos);
    CHECK(second.error().code != 0);

    // Describing is the picker's path, and it opens the device too.
    source::RtlSdrSourceConfig config;
    config.uri = "rtlsdr://0";
    config.index = 0;
    auto described = source::describe_rtlsdr_source(config);
    REQUIRE_FALSE(described.has_value());
    CHECK(described.error().message.find("rtlsdr_open returned") != std::string::npos);

    // THE HOLDER IS UNTOUCHED: still streaming, still tunable, stops cleanly.
    const std::uint64_t before = [&collected] {
        std::scoped_lock guard(collected.lock);
        return collected.samples;
    }();
    REQUIRE(wait_for(before + 300'000));
    REQUIRE(radio.running());

    auto landed = radio.tune(96'500'000);
    INFO((landed ? std::string("tuned") : landed.error().message));
    REQUIRE(landed.has_value());

    const auto stopped = radio.stop();
    INFO((stopped ? std::string("stopped") : stopped.error().message));
    REQUIRE(stopped.has_value());
}

TEST_CASE("a dongle streaming for minutes can still be tuned",
          "[.probe][source][rtlsdr][device][dongle]") {
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    // HIDDEN, AND NAMED TO RUN, because what it measures is a threshold rather
    // than a yes or no and REVENANT_PROBE_DELAY_MS is what moves it. Every
    // device case above tunes as soon as it has enough samples to prove the
    // stream is live, which at 2.4 MS/s is a fifth of a second, and all of them
    // pass; the retune that failed in the window came minutes in. So this does
    // ONE tune, at a delay the caller sets, which is the only way to tell "the
    // first retune is the one that works" from "a retune this late is the one
    // that fails".
    const char* const delay_env = std::getenv("REVENANT_PROBE_DELAY_MS");
    const auto delay = std::chrono::milliseconds(delay_env == nullptr ? 2000
                                                                     : std::atoi(delay_env));

    // The block sizes the URB length, so this is how the probe moves the number
    // of transfers the stream has recycled by the time it tunes.
    const char* const block_env = std::getenv("REVENANT_PROBE_BLOCK");
    const auto block = static_cast<std::size_t>(
        block_env == nullptr ? 32'768 : std::max(1, std::atoi(block_env)));

    auto opened = source::open_source("rtlsdr://0?freq=98100000");
    if (!opened) {
        SKIP("the dongle could not be opened: " + opened.error().message);
    }
    source::Source& radio = **opened;

    Collected collected;
    source::StreamOptions options;
    options.block_samples = block;
    REQUIRE(radio.start(options, collecting_sink(collected)).has_value());

    const auto started = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - started < delay) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::uint64_t samples = 0;
    {
        std::scoped_lock guard(collected.lock);
        samples = collected.samples;
    }
    REQUIRE(radio.running());

    auto landed = radio.tune(96'500'000);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    if (!landed) {
        WARN("ONE tune at " << elapsed << " ms and " << samples
                            << " samples FAILED: " << landed.error().message);
    } else {
        WARN("ONE tune at " << elapsed << " ms and " << samples << " samples landed at "
                            << *landed << " Hz");
    }
    CHECK(landed.has_value());

    REQUIRE(radio.stop().has_value());
}

TEST_CASE("librtlsdr on its own retunes a streaming dongle",
          "[.probe][source][rtlsdr][device][dongle]") {
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    // REVENANT IS NOT IN THIS ONE. Nothing here goes through RtlSdrSource: it is
    // rtlsdr_open, set the centre, set the rate, reset the buffer, read_async on
    // a thread, then rtlsdr_set_center_freq from this one, which is what
    // rtl_tcp does and what the backend does with several hundred lines in
    // between.
    //
    // The probe above establishes that a retune at 250 ms lands and one at 500 ms
    // is refused with -9, and that the URB length does not move that boundary.
    // This says whether that is ours or librtlsdr's on this dongle, and the
    // answer changes what the fix is: a bug in the backend gets fixed, and a
    // limit of the driver gets worked around by stopping the stream around the
    // retune. Nothing else can tell those two apart.
    const char* const delay_env = std::getenv("REVENANT_PROBE_DELAY_MS");
    const auto delay_ms = delay_env == nullptr ? 2000 : std::atoi(delay_env);

    rtlsdr_dev_t* device = nullptr;
    REQUIRE(rtlsdr_open(&device, 0) == 0);
    REQUIRE(device != nullptr);

    // The same order and the same values the backend applies, so a difference in
    // the result is a difference in the code between here and there.
    CHECK(rtlsdr_set_center_freq(device, 98'100'000) == 0);
    CHECK(rtlsdr_set_sample_rate(device, 2'400'000) == 0);
    CHECK(rtlsdr_set_tuner_gain_mode(device, 1) == 0);
    CHECK(rtlsdr_set_agc_mode(device, 0) == 0);
    REQUIRE(rtlsdr_reset_buffer(device) == 0);

    std::atomic<std::uint64_t> bytes{0};
    std::thread usb([device, &bytes] {
        rtlsdr_read_async(
            device,
            [](unsigned char*, std::uint32_t length, void* ctx) {
                static_cast<std::atomic<std::uint64_t>*>(ctx)->fetch_add(length,
                                                                        std::memory_order_relaxed);
            },
            &bytes, 16, 65'536);
    });

    const auto started = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - started < std::chrono::milliseconds(delay_ms)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const std::uint64_t seen = bytes.load(std::memory_order_relaxed);
    const int rc = rtlsdr_set_center_freq(device, 96'500'000);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    // REPORTED, NOT ASSERTED. This case exists to measure the platform, and on
    // the platform it was written against the answer is a failure. Asserting
    // success would make the suite red for a defect that is not Revenant's;
    // asserting failure would make it red on a fixed librtlsdr or a dongle
    // without the defect, which is the outcome to hope for. So it prints the
    // number and a reader compares it against what the backend assumes.
    WARN("librtlsdr alone: set_center_freq at " << elapsed << " ms and " << seen
                                               << " bytes returned " << rc
                                               << " (0 is a working retune, -9 is the stall the "
                                                  "backend pauses the stream to avoid)");

    static_cast<void>(rtlsdr_cancel_async(device));
    usb.join();
    rtlsdr_close(device);
}

TEST_CASE("librtlsdr retunes a dongle whose stream is paused",
          "[.probe][source][rtlsdr][device][dongle]") {
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    // THE WORKAROUND, PROVED BEFORE IT IS BUILT. Cancel the async read, join,
    // retune, reset the buffer, read again, several times over, because the
    // sequence has to survive being done repeatedly rather than once.
    //
    // The retune is retried, and that is not a blind retry. join_locked in the
    // backend already documents the reason: on Windows the first control
    // transfer issued after the bulk transfers have been cancelled sometimes
    // comes back LIBUSB_ERROR_PIPE, on about two runs in three. A retune that
    // takes the first slot after a cancel is standing exactly where that lands.
    //
    // It also times the pause, because a retune the operator can feel is a
    // different product decision from one they cannot.
    rtlsdr_dev_t* device = nullptr;
    REQUIRE(rtlsdr_open(&device, 0) == 0);
    REQUIRE(device != nullptr);

    CHECK(rtlsdr_set_center_freq(device, 98'100'000) == 0);
    CHECK(rtlsdr_set_sample_rate(device, 2'400'000) == 0);
    CHECK(rtlsdr_set_tuner_gain_mode(device, 1) == 0);
    CHECK(rtlsdr_set_agc_mode(device, 0) == 0);

    std::atomic<std::uint64_t> bytes{0};
    const auto reader = [device, &bytes] {
        rtlsdr_read_async(
            device,
            [](unsigned char*, std::uint32_t length, void* ctx) {
                static_cast<std::atomic<std::uint64_t>*>(ctx)->fetch_add(length,
                                                                        std::memory_order_relaxed);
            },
            &bytes, 16, 65'536);
    };

    constexpr int kRounds = 6;
    const std::array<std::uint32_t, 2> targets{96'500'000, 99'700'000};
    for (int round = 0; round < kRounds; ++round) {
        REQUIRE(rtlsdr_reset_buffer(device) == 0);
        std::thread usb(reader);

        // Well past the boundary the probes above found, so every round is a
        // retune that would be refused without the pause.
        const std::uint64_t before = bytes.load(std::memory_order_relaxed);
        const auto settled = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (std::chrono::steady_clock::now() < settled ||
               bytes.load(std::memory_order_relaxed) == before) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        const auto paused_at = std::chrono::steady_clock::now();
        while (rtlsdr_cancel_async(device) != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        usb.join();

        int rc = rtlsdr_set_center_freq(device, targets[static_cast<std::size_t>(round) % 2]);
        int tries = 1;
        while (rc != 0 && tries < 4) {
            ++tries;
            rc = rtlsdr_set_center_freq(device, targets[static_cast<std::size_t>(round) % 2]);
        }
        const auto pause_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - paused_at)
                                  .count();

        WARN("round " << round << ": retune returned " << rc << " after " << tries
                      << " tries, the stream was down for " << pause_ms << " ms");
        CHECK(rc == 0);
        CHECK(static_cast<std::uint32_t>(rtlsdr_get_center_freq(device)) ==
              targets[static_cast<std::size_t>(round) % 2]);
    }

    rtlsdr_close(device);
}

TEST_CASE("librtlsdr retunes from inside its own callback",
          "[.probe][source][rtlsdr][device][dongle]") {
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    // WHICH FIX IS AVAILABLE, and the difference between the two is large enough
    // to be worth one probe.
    //
    // The case above shows the control transfer stalling from a thread that is
    // not the one inside rtlsdr_read_async. If issuing it from the callback
    // instead works, a retune costs nothing: park the request and let the USB
    // thread apply it between transfers. If it stalls there too, the only thing
    // left is cancelling the stream, retuning and restarting it, which is a real
    // gap in the samples that every consumer then has to be told about.
    rtlsdr_dev_t* device = nullptr;
    REQUIRE(rtlsdr_open(&device, 0) == 0);
    REQUIRE(device != nullptr);

    CHECK(rtlsdr_set_center_freq(device, 98'100'000) == 0);
    CHECK(rtlsdr_set_sample_rate(device, 2'400'000) == 0);
    CHECK(rtlsdr_set_tuner_gain_mode(device, 1) == 0);
    CHECK(rtlsdr_set_agc_mode(device, 0) == 0);
    REQUIRE(rtlsdr_reset_buffer(device) == 0);

    struct Shared {
        rtlsdr_dev_t* device = nullptr;
        std::atomic<std::uint64_t> bytes{0};
        std::atomic<bool> please_tune{false};
        std::atomic<int> result{1};
        std::atomic<bool> answered{false};
    };
    Shared shared;
    shared.device = device;

    std::thread usb([&shared] {
        rtlsdr_read_async(
            shared.device,
            [](unsigned char*, std::uint32_t length, void* ctx) {
                auto& state = *static_cast<Shared*>(ctx);
                state.bytes.fetch_add(length, std::memory_order_relaxed);
                if (state.please_tune.exchange(false, std::memory_order_acq_rel)) {
                    state.result.store(rtlsdr_set_center_freq(state.device, 96'500'000),
                                       std::memory_order_relaxed);
                    state.answered.store(true, std::memory_order_release);
                }
            },
            &shared, 16, 65'536);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    shared.please_tune.store(true, std::memory_order_release);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!shared.answered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(shared.answered.load(std::memory_order_acquire));
    const int rc = shared.result.load(std::memory_order_relaxed);
    // Reported and not asserted, for the reason the case above gives. -6 is
    // LIBUSB_ERROR_BUSY, which is what closes this route off.
    WARN("from inside the callback, two seconds in and "
         << shared.bytes.load(std::memory_order_relaxed) << " bytes: set_center_freq returned "
         << rc << " (0 would mean a retune could be parked for the USB thread to apply, -6 means "
                  "it cannot)");

    static_cast<void>(rtlsdr_cancel_async(device));
    usb.join();
    rtlsdr_close(device);
}

TEST_CASE("librtlsdr survives a second cancel", "[.probe][source][rtlsdr][device][dongle]") {
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    // WHETHER A SECOND rtlsdr_cancel_async IS DANGEROUS, which the backend's
    // comments used to assert. Measured 2026-09-23: 0 then -2 in four rounds
    // out of four, read_async returning 0 each time, and the retune after it
    // failing -9 exactly as it does after a single cancel. So on the librtlsdr
    // this tree links a second call does nothing.

    rtlsdr_dev_t* device = nullptr;
    REQUIRE(rtlsdr_open(&device, 0) == 0);
    REQUIRE(device != nullptr);
    CHECK(rtlsdr_set_center_freq(device, 98'100'000) == 0);
    CHECK(rtlsdr_set_sample_rate(device, 2'400'000) == 0);

    std::atomic<std::uint64_t> bytes{0};
    for (int round = 0; round < 4; ++round) {
        REQUIRE(rtlsdr_reset_buffer(device) == 0);
        std::atomic<int> read_rc{99};
        std::thread usb([device, &bytes, &read_rc] {
            read_rc = rtlsdr_read_async(
                device,
                [](unsigned char*, std::uint32_t length, void* ctx) {
                    static_cast<std::atomic<std::uint64_t>*>(ctx)->fetch_add(
                        length, std::memory_order_relaxed);
                },
                &bytes, 16, 65'536);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        const int first = rtlsdr_cancel_async(device);
        const int second = rtlsdr_cancel_async(device);
        usb.join();
        WARN("round " << round << ": cancel returned " << first << " then " << second
                      << ", read_async returned " << read_rc.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const int tuned = rtlsdr_set_center_freq(device, round % 2 == 0 ? 96'500'000 : 98'100'000);
        WARN("round " << round << ": retune after it returned " << tuned);
    }
    rtlsdr_close(device);
}

TEST_CASE("librtlsdr keeps up with synchronous reads", "[.probe][source][rtlsdr][device][dongle]") {
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    // WHETHER rtlsdr_read_sync COULD REPLACE read_async, which would take the
    // cancel out of the picture and the teardown fault with it. It cannot. The
    // delivered rate is measured between two completions, against nominal:
    //
    //   read_async, 16 x 64 KiB, 20 s, twice         -17 and -21 ppm
    //   read_sync, 256 KiB, 20 s, twice              -17 and -13 ppm
    //   read_sync, 256 KiB, 10 s                     -143 ppm
    //   read_sync with 1 ms slept between reads      -126,535 ppm
    //
    // The crystal's own offset is about -17. One read in flight leaves the
    // dongle's own buffer as the only slack, a millisecond away is enough to
    // lose an eighth of the stream, and none of it is counted anywhere: the
    // device drops it before the host sees it. That is the loss this engine
    // refuses to have, so the backend stays on read_async.
    //
    // REVENANT_PROBE_MODE is sync or async, REVENANT_PROBE_LEN the read length,
    // REVENANT_PROBE_GAP_US a pause between sync reads, REVENANT_PROBE_SECONDS
    // the window.
    const char* const seconds_env = std::getenv("REVENANT_PROBE_SECONDS");
    const int seconds = seconds_env == nullptr ? 20 : std::atoi(seconds_env);
    const char* const mode_env = std::getenv("REVENANT_PROBE_MODE");
    const std::string mode = mode_env == nullptr ? "sync" : mode_env;
    const char* const len_env = std::getenv("REVENANT_PROBE_LEN");
    const int length = len_env == nullptr ? 262'144 : std::atoi(len_env);
    const char* const gap_env = std::getenv("REVENANT_PROBE_GAP_US");
    const int gap_us = gap_env == nullptr ? 0 : std::atoi(gap_env);

    rtlsdr_dev_t* device = nullptr;
    REQUIRE(rtlsdr_open(&device, 0) == 0);
    CHECK(rtlsdr_set_center_freq(device, 98'100'000) == 0);
    CHECK(rtlsdr_set_sample_rate(device, 2'400'000) == 0);
    REQUIRE(rtlsdr_reset_buffer(device) == 0);

    std::atomic<std::uint64_t> bytes{0};
    std::atomic<bool> stop{false};
    std::atomic<int> last_rc{0};
    std::vector<unsigned char> buffer(static_cast<std::size_t>(length));
    std::thread usb([&] {
        if (mode == "sync") {
            while (!stop.load()) {
                int got = 0;
                const int rc = rtlsdr_read_sync(device, buffer.data(), length, &got);
                if (rc != 0) {
                    last_rc = rc;
                    break;
                }
                bytes.fetch_add(static_cast<std::uint64_t>(got));
                if (gap_us > 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(gap_us));
                }
            }
        } else {
            last_rc = rtlsdr_read_async(
                device,
                [](unsigned char*, std::uint32_t n, void* ctx) {
                    static_cast<std::atomic<std::uint64_t>*>(ctx)->fetch_add(n);
                },
                &bytes, 16, 65'536);
        }
    });
    // Both ends of the window are the moment a read completed, so the count
    // is not quantised by the read length.
    const auto wait_for_change = [&bytes] {
        const std::uint64_t seen = bytes.load();
        while (bytes.load() == seen) {
        }
        return std::pair{bytes.load(), std::chrono::steady_clock::now()};
    };
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const auto [at_start, window] = wait_for_change();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    const auto [at_stop, window_end] = wait_for_change();
    const std::uint64_t at_end = at_stop - at_start;
    const double elapsed = std::chrono::duration<double>(window_end - window).count();
    stop = true;
    if (mode != "sync") {
        static_cast<void>(rtlsdr_cancel_async(device));
    }
    usb.join();
    const double rate = static_cast<double>(at_end) / 2.0 / elapsed;
    WARN(mode << " reads of " << length << " bytes, " << gap_us << " us between: " << at_end / 2
              << " samples in " << elapsed << " s, " << rate << " S/s, "
              << (rate / 2'400'000.0 - 1.0) * 1e6 << " ppm against nominal; last rc "
              << last_rc.load());
    rtlsdr_close(device);
}

TEST_CASE("a streaming dongle takes control calls back to back",
          "[.probe][source][rtlsdr][device][dongle]") {
    const source::DeviceLock radio_lock = test::hold_the_dongle();

    // THE REPRODUCER FOR THE CI SEGFAULTS OF 2026-09-23. Every control call on
    // a streaming dongle cancels the transfers and restarts them, and about
    // one cancel in three hundred comes back from rtlsdr_read_async as -5 with
    // a transfer still in flight; see run_usb in core/source/rtlsdr_source.cpp.
    // At the default thirty rounds this rarely shows it. Two hundred rounds,
    // run five times, showed it in one to three runs of the five, and with full
    // page heap on the binary (gflags /p /enable <exe> /full) the access to
    // the freed transfer faults at once instead of corrupting the heap.
    //
    // REVENANT_PROBE_ROUNDS sets the rounds, REVENANT_PROBE_STEP_MS the spacing
    // (round % 7 steps of it), REVENANT_PROBE_BLOCK the block, which sizes the
    // transfers.
    const char* const rounds_env = std::getenv("REVENANT_PROBE_ROUNDS");
    const int rounds = rounds_env == nullptr ? 30 : std::atoi(rounds_env);

    auto opened = source::open_source("rtlsdr://0?rate=2400000&freq=98.1M&gain=20");
    if (!opened) {
        SKIP("the dongle could not be opened: " + opened.error().message);
    }
    source::Source& radio = **opened;
    Collected collected;
    source::StreamOptions options;
    const char* const block_env = std::getenv("REVENANT_PROBE_BLOCK");
    options.block_samples =
        static_cast<std::size_t>(block_env == nullptr ? 32'768 : std::atoi(block_env));
    REQUIRE(radio.start(options, collecting_sink(collected)).has_value());

    const char* const step_env = std::getenv("REVENANT_PROBE_STEP_MS");
    const int step_ms = step_env == nullptr ? 20 : std::atoi(step_env);

    int failures = 0;
    for (int round = 0; round < rounds; ++round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(step_ms * (round % 7)));
        Status outcome;
        switch (round % 3) {
            case 0: {
                auto landed = radio.tune(round % 2 == 0 ? 96'500'000 : 98'100'000);
                if (!landed) {
                    outcome = std::unexpected(landed.error());
                }
                break;
            }
            case 1: {
                auto gain = radio.set_gain("tuner", round % 2 == 0 ? 10.0 : 30.0);
                if (!gain) {
                    outcome = std::unexpected(gain.error());
                }
                break;
            }
            default: outcome = radio.set_gain_auto("tuner", round % 2 == 0); break;
        }
        if (!outcome) {
            ++failures;
            WARN("round " << round << ": " << outcome.error().message);
        }
    }
    const bool still_running = radio.running();
    const auto stopped = radio.stop();
    WARN(rounds << " control calls, " << failures << " refused; running at the end " << still_running
                << "; stop " << (stopped ? std::string("succeeded") : stopped.error().message)
                << "; " << collected.samples << " samples");
}

TEST_CASE("a second process contends for the dongle",
          "[.contender][source][rtlsdr][device][dongle]") {
    // Not hold_the_dongle(): this case is the other process, and what it does
    // about the lock is the thing being watched.
    if (!a_dongle_is_attached()) {
        SKIP(kNoDongle);
    }

    // NOT A TEST OF ANYTHING ON ITS OWN. Run from a second process while the
    // first runs a dongle case, it stands in for the other program that holds
    // or wants the radio: a window's engine asking every device to describe
    // itself, or a CLI started on the same index. REVENANT_CONTEND_MODE picks
    // which:
    //
    //   poke   enumerate, then ask for the machine-wide lock without waiting
    //          and open and close the dongle only when it was granted, as fast
    //          as possible, for the duration. That is what a device picker's
    //          describe does since the lock arrived. Every attempt is expected
    //          to be refused by the lock while the other process holds the
    //          device, before librtlsdr is reached at all.
    //   hold   open, configure and stream for the duration, then close.
    //
    // REVENANT_CONTEND_SECONDS sets the duration, ten by default.
    const char* const mode_env = std::getenv("REVENANT_CONTEND_MODE");
    const std::string mode = mode_env == nullptr ? "poke" : mode_env;
    const char* const seconds_env = std::getenv("REVENANT_CONTEND_SECONDS");
    const int seconds = seconds_env == nullptr ? 10 : std::atoi(seconds_env);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);

    if (mode == "hold") {
        auto opened = source::open_source("rtlsdr://0?rate=2400000&freq=98.1M&gain=20");
        if (!opened) {
            SKIP("could not take the dongle to hold it: " + opened.error().message);
        }
        Collected collected;
        REQUIRE((*opened)->start(source::StreamOptions{}, collecting_sink(collected)).has_value());
        while (std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const auto stopped = (*opened)->stop();
        WARN("held the dongle for " << seconds << " s and collected " << collected.samples
                                    << " samples; stop "
                                    << (stopped ? std::string("succeeded")
                                                : stopped.error().message));
        return;
    }

    int opens = 0;
    int locked_out = 0;
    int refused = 0;
    int granted = 0;
    while (std::chrono::steady_clock::now() < until) {
        static_cast<void>(source::enumerate_rtlsdr_devices());
        ++opens;
        auto lock = source::lock_for_probe(source::rtlsdr_lock_policy());
        if (!lock) {
            ++locked_out;
            continue;
        }
        rtlsdr_dev_t* device = nullptr;
        const int rc = rtlsdr_open(&device, 0);
        if (rc == 0 && device != nullptr) {
            ++granted;
            rtlsdr_close(device);
        } else {
            ++refused;
        }
    }
    WARN("poked the dongle " << opens << " times: " << locked_out
                             << " refused by the lock before librtlsdr, " << refused
                             << " refused by rtlsdr_open, " << granted << " granted");
}
