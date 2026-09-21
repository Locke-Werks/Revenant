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
          "[source][rtlsdr][device]") {
    // The case that catches the failure every structural assertion misses.
    // A backend that opens, starts, hands over the right number of zero bytes
    // and stops passes everything above this.
    if (!a_dongle_is_attached()) {
        SKIP(kNoDongle);
    }

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
          "[source][rtlsdr][device]") {
    if (!a_dongle_is_attached()) {
        SKIP(kNoDongle);
    }

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

TEST_CASE("a streaming dongle can still be tuned", "[source][rtlsdr][device]") {
    if (!a_dongle_is_attached()) {
        SKIP(kNoDongle);
    }

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
    constexpr dsp::Hertz kOpenAt = 98'100'000;
    constexpr dsp::Hertz kWanted = 95'100'000;

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

    // Streaming for real before the tune, so this is a retune against
    // transfers in flight rather than against a handle that has merely been
    // started. Without the wait a fast machine can reach the tune before the
    // first transfer completes, which is the arrangement that already works.
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

    REQUIRE(radio.stop().has_value());
}

TEST_CASE("describing every device while one is streaming does not wedge its tuner",
          "[source][rtlsdr][device]") {
    if (!a_dongle_is_attached()) {
        SKIP(kNoDongle);
    }

    // THE SEQUENCE THAT BROKE TUNING IN THE WINDOW, and the only one of the
    // four candidate shapes that is not already covered above.
    //
    // A picker refreshes its list whenever the panel is opened, and the panel
    // gets opened while a source is already running: that is what changing radio
    // twice looks like. describe_sources() opens EVERY device to ask it what it
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

TEST_CASE("a dongle opened at the bottom of its range can still be tuned away",
          "[source][rtlsdr][device]") {
    if (!a_dongle_is_attached()) {
        SKIP(kNoDongle);
    }

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
