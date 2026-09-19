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

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/dsp/types.h"
#include "core/source/registry.h"
#include "core/source/rtlsdr_source.h"

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

TEST_CASE("a serial that no dongle carries fails by name", "[source][rtlsdr]") {
    // The leading zeros are what make this a serial rather than an index.
    // That rule is the whole of the ambiguity in rtlsdr://<body>, so it is
    // checked rather than assumed.
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

TEST_CASE("an attached dongle describes itself honestly", "[source][rtlsdr][device]") {
    if (!a_dongle_is_attached()) {
        SKIP(kNoDongle);
    }

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

TEST_CASE("tuning reports where the tuner landed", "[source][rtlsdr][device]") {
    if (!a_dongle_is_attached()) {
        SKIP(kNoDongle);
    }

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

TEST_CASE("a manual gain snaps to a step the tuner has", "[source][rtlsdr][device]") {
    if (!a_dongle_is_attached()) {
        SKIP(kNoDongle);
    }

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

TEST_CASE("a dongle streams, stops cleanly and its counters add up",
          "[source][rtlsdr][device]") {
    if (!a_dongle_is_attached()) {
        SKIP(kNoDongle);
    }

    constexpr dsp::SampleRate kRate = 2'400'000;
    constexpr std::uint64_t kWanted = 300'000;

    auto opened = source::open_source("rtlsdr://0?rate=2400000&freq=100M&gain=auto");
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
          "[source][rtlsdr][device]") {
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
    if (!a_dongle_is_attached()) {
        SKIP(kNoDongle);
    }

    auto opened = source::open_source("rtlsdr://0?rate=2400000&freq=100M&gain=auto");
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
          "[source][rtlsdr][device]") {
    // The case that catches the failure every structural assertion misses.
    // A backend that opens, starts, hands over the right number of zero bytes
    // and stops passes everything above this.
    if (!a_dongle_is_attached()) {
        SKIP(kNoDongle);
    }

    auto opened = source::open_source("rtlsdr://0?rate=2400000&freq=100M&gain=auto");
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
