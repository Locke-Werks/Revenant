// The source abstraction.
//
// The properties worth committing a test for are the ones that fail silently:
// a URI whose typo is ignored, a seek that lands a sample early, a synthetic
// scene that is not reproducible from its seed, and a Demand source that turns
// out not to respect backpressure. Each of those produces plausible output and
// a wrong recording.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/dsp/types.h"
#include "core/source/capabilities.h"
#include "core/source/file_source.h"
#include "core/source/registry.h"
#include "tests/reference/reference_diff.h"
#include "tests/support/temp_path.h"

using namespace revenant;

namespace {

// A capture written to the system temp directory and removed afterwards.
class ScratchCapture {
public:
    explicit ScratchCapture(std::size_t samples) {
        path_ = test::unique_temp_path("revenant_test_capture_" + std::to_string(samples),
                                       ".cf32");

        std::vector<dsp::Complex32> data(samples);
        for (std::size_t i = 0; i < samples; ++i) {
            // A pattern in which every sample is distinguishable from every
            // other, so an off-by-one in a seek is visible rather than
            // hiding inside a smooth waveform.
            data[i] = dsp::Complex32{static_cast<float>(i), static_cast<float>(-static_cast<double>(i))};
        }
        expected_ = data;

        std::FILE* file = std::fopen(path_.string().c_str(), "wb");
        if (file != nullptr) {
            std::fwrite(data.data(), sizeof(dsp::Complex32), data.size(), file);
            std::fclose(file);
        }
    }

    ~ScratchCapture() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    ScratchCapture(const ScratchCapture&) = delete;
    ScratchCapture& operator=(const ScratchCapture&) = delete;

    [[nodiscard]] std::string uri(dsp::SampleRate rate) const {
        std::string generic = path_.generic_string();
        return "file:///" + generic + "?rate=" + std::to_string(rate) + "&format=cf32";
    }

    [[nodiscard]] const std::vector<dsp::Complex32>& expected() const { return expected_; }

private:
    std::filesystem::path path_;
    std::vector<dsp::Complex32> expected_;
};

// A directory of its own per test, so two of these running under ctest -j
// cannot collide on a filename and cannot delete each other's fixtures.
class ScratchDir {
public:
    explicit ScratchDir(std::string_view tag) {
        std::random_device entropy;
        root_ = std::filesystem::temp_directory_path() /
                std::format("revenant_{}_{:08x}{:08x}", tag, entropy(), entropy());
        std::error_code ignored;
        std::filesystem::create_directories(root_, ignored);
    }

    ~ScratchDir() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    [[nodiscard]] std::filesystem::path path(std::string_view name) const {
        return root_ / name;
    }

    [[nodiscard]] static std::string generic(const std::filesystem::path& path) {
        return path.generic_string();
    }

    // file:/// wants a leading slash before a drive letter, which is what the
    // rest of this file already does by hand.
    [[nodiscard]] static std::string uri(const std::filesystem::path& path,
                                         std::string_view query = {}) {
        std::string out = "file:///" + path.generic_string();
        if (!query.empty()) {
            out += '?';
            out += query;
        }
        return out;
    }

    static void write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
        std::FILE* file = std::fopen(path.string().c_str(), "wb");
        REQUIRE(file != nullptr);
        if (!bytes.empty()) {
            REQUIRE(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
        }
        REQUIRE(std::fclose(file) == 0);
    }

    static void write_text(const std::filesystem::path& path, std::string_view text) {
        write(path, std::as_bytes(std::span(text.data(), text.size())));
    }

private:
    std::filesystem::path root_;
};

// The pattern every fixture uses: each sample distinguishable from every
// other, so an off-by-one in a seek, a header skipped by the wrong number of
// bytes, or a capture segment resolved to the wrong offset is visible rather
// than hidden inside a smooth waveform.
[[nodiscard]] std::vector<dsp::Complex32> ramp_cf32(std::size_t samples, int seed) {
    std::vector<dsp::Complex32> out(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        out[i] = dsp::Complex32{static_cast<float>(static_cast<double>(i) + seed),
                                static_cast<float>(-static_cast<double>(i) - seed)};
    }
    return out;
}

[[nodiscard]] std::vector<std::byte> as_bytes_of(const std::vector<dsp::Complex32>& samples) {
    std::vector<std::byte> out(samples.size() * sizeof(dsp::Complex32));
    if (!samples.empty()) {
        std::memcpy(out.data(), samples.data(), out.size());
    }
    return out;
}

// Nanoseconds since the Unix epoch for a UTC calendar time, computed here so
// that a test's expected anchor does not come from the same code that
// produced it.
[[nodiscard]] std::int64_t epoch_ns_utc(int year, unsigned month, unsigned day, int hour,
                                        int minute, int second, std::int64_t sub_second_ns) {
    const std::chrono::year_month_day ymd{std::chrono::year{year}, std::chrono::month{month},
                                          std::chrono::day{day}};
    const auto days = std::chrono::sys_days(ymd).time_since_epoch().count();
    const std::int64_t seconds = static_cast<std::int64_t>(days) * 86'400 + hour * 3'600 +
                                 minute * 60 + second;
    return seconds * 1'000'000'000 + sub_second_ns;
}

// Little-endian byte assembly for the RIFF fixtures.
class ByteWriter {
public:
    void u8(std::uint8_t value) { out_.push_back(static_cast<std::byte>(value)); }

    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value & 0xFF));
        u8(static_cast<std::uint8_t>(value >> 8));
    }

    void u32(std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            u8(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
        }
    }

    void u64(std::uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            u8(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
        }
    }

    void tag(std::string_view four) {
        REQUIRE(four.size() == 4);
        for (char c : four) {
            u8(static_cast<std::uint8_t>(c));
        }
    }

    void bytes(std::span<const std::byte> data) {
        out_.insert(out_.end(), data.begin(), data.end());
    }

    [[nodiscard]] std::size_t size() const { return out_.size(); }
    [[nodiscard]] const std::vector<std::byte>& take() const { return out_; }

private:
    std::vector<std::byte> out_;
};

// What to put in a WAV, including the things a correct writer would never
// put there. Every refusal case below is one field of this being wrong.
struct WavPlan {
    std::string signature = "RIFF";
    std::uint16_t format_tag = 3;  // WAVE_FORMAT_IEEE_FLOAT
    std::uint16_t channels = 2;
    std::uint32_t rate = 2'000'000;
    std::uint16_t bits = 32;

    bool with_auxi = false;
    std::uint32_t auxi_center = 0;
    std::uint32_t auxi_rate = 0;
    bool auxi_time = false;

    bool with_ds64 = false;
    std::uint64_t ds64_data_bytes = 0;
    bool data_size_sentinel = false;

    bool override_data_size = false;
    std::uint32_t data_size = 0;

    // WAVE_FORMAT_EXTENSIBLE, a 40-byte fmt chunk with format_tag as the first
    // two bytes of the subformat GUID. What the 24-bit HF recorders write.
    bool extensible = false;

    std::vector<std::byte> payload;
};

[[nodiscard]] std::vector<std::byte> render_wav(const WavPlan& plan) {
    ByteWriter body;

    if (plan.with_ds64) {
        body.tag("ds64");
        body.u32(28);
        body.u64(0xFFFFFFFFFFFFFFFFull);       // riff size, not read here
        body.u64(plan.ds64_data_bytes);        // data size
        body.u64(plan.payload.size() / 8);     // sample count, not read here
        body.u32(0);                           // table length
    }

    body.tag("fmt ");
    body.u32(plan.extensible ? 40 : 16);
    body.u16(plan.extensible ? std::uint16_t{0xFFFE} : plan.format_tag);
    body.u16(plan.channels);
    body.u32(plan.rate);
    body.u32(plan.rate * plan.channels * (plan.bits / 8u));
    body.u16(static_cast<std::uint16_t>(plan.channels * (plan.bits / 8u)));
    body.u16(plan.bits);
    if (plan.extensible) {
        body.u16(22);         // cbSize
        body.u16(plan.bits);  // valid bits per sample
        body.u32(0x3);        // channel mask, front left and right
        // KSDATAFORMAT_SUBTYPE_PCM or _IEEE_FLOAT: the tag, then the fixed
        // tail 0000-0010-8000-00AA00389B71.
        body.u32(plan.format_tag);
        body.u16(0x0000);
        body.u16(0x0010);
        const std::uint8_t tail[8] = {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        for (std::uint8_t byte : tail) {
            body.u8(byte);
        }
    }

    if (plan.with_auxi) {
        body.tag("auxi");
        body.u32(60);
        // StartTime, a Win32 SYSTEMTIME: year, month, day of week, day, hour,
        // minute, second, millisecond.
        const std::uint16_t start[8] = {2026, 9, 1, 21, 14, 3, 7, 250};
        for (std::uint16_t field : start) {
            body.u16(plan.auxi_time ? field : 0);
        }
        for (int i = 0; i < 8; ++i) {
            body.u16(0);  // StopTime, not read
        }
        body.u32(plan.auxi_center);
        body.u32(plan.auxi_rate);
        body.u32(0);  // IFFrequency
        body.u32(0);  // Bandwidth
        body.u32(0);  // IQOffset
        body.u32(0);  // DBOffset
        body.u32(0);  // MaxVal
    }

    body.tag("data");
    if (plan.data_size_sentinel) {
        body.u32(0xFFFFFFFFu);
    } else if (plan.override_data_size) {
        body.u32(plan.data_size);
    } else {
        body.u32(static_cast<std::uint32_t>(plan.payload.size()));
    }
    body.bytes(plan.payload);

    ByteWriter file;
    file.tag(plan.signature);
    file.u32(plan.signature == "RIFF" ? static_cast<std::uint32_t>(4 + body.size())
                                      : 0xFFFFFFFFu);
    file.tag("WAVE");
    file.bytes(body.take());
    return file.take();
}

// Drains a source into a flat vector, recording the indices it was given.
struct Drained {
    std::vector<dsp::Complex32> samples;
    std::vector<dsp::SampleIndex> block_starts;
    std::uint64_t dropped_total = 0;

    // From the first block's stamp, which is where a container's anchor and
    // rate actually reach a consumer.
    std::int64_t anchor_ns = 0;
    dsp::SampleRate rate = 0;
};

// Runs a bounded source to its natural end and collects what it produced.
//
// start() spawns the source's own thread and returns immediately, so stopping
// straight away truncates the stream before a single block arrives. A bounded
// source ends by itself, and running() going false is how that is observed;
// stop() then joins and hands back whatever error ended it.
Drained drain(source::Source& src, const source::StreamOptions& options, std::size_t limit) {
    Drained out;
    const auto started = src.start(options, [&](const source::SourceBlock& block) -> Status {
        if (out.block_starts.empty()) {
            out.anchor_ns = block.stamp.epoch_anchor_ns;
            out.rate = block.stamp.rate;
        }
        out.block_starts.push_back(block.stamp.start);
        out.dropped_total += block.dropped_before;
        const auto* first = reinterpret_cast<const dsp::Complex32*>(block.bytes.data());
        for (std::size_t i = 0; i < block.sample_count && out.samples.size() < limit; ++i) {
            out.samples.push_back(first[i]);
        }
        return {};
    });
    REQUIRE(started.has_value());

    // Bounded by wall clock so a source that never finishes fails the test
    // rather than hanging the suite.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (src.running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_FALSE(src.running());

    const auto stopped = src.stop();
    INFO(test::message_of(stopped));
    CHECK(stopped.has_value());
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The URI layer
// ---------------------------------------------------------------------------

TEST_CASE("an unknown URI parameter is refused rather than ignored", "[source][m1]") {
    // The whole reason this is an error. A typo in a rate that silently falls
    // back to a default produces a capture whose metadata is wrong, and
    // nothing downstream ever says so: the samples are real, the header lies,
    // and every frequency computed from it is off by the ratio of the two
    // rates.
    const auto opened = source::open_source("synthetic:wideband?rate=2400000&emiters=8");
    REQUIRE_FALSE(opened.has_value());
    INFO(opened.error().message);

    // And the message names the offending key, so the fix is obvious.
    CHECK(opened.error().message.find("emiters") != std::string::npos);
}

TEST_CASE("a synthetic URI opens and reports its capabilities", "[source][m1]") {
    const auto opened = source::open_source("synthetic:wideband?rate=2400000&emitters=4&seed=7");
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    const auto& caps = (*opened)->capabilities();
    CHECK(caps.backend == "synthetic");
    CHECK(caps.flow == source::FlowControl::Demand);
    CHECK(caps.seekable);
    CHECK((*opened)->sample_rate() == 2'400'000);
}

TEST_CASE("an unparseable URI fails with a message rather than a crash", "[source][m1]") {
    for (const char* bad : {"", "nonsense", "://missing-scheme", "unknownscheme:whatever"}) {
        const auto opened = source::open_source(bad);
        INFO("uri '" << bad << "'");
        CHECK_FALSE(opened.has_value());
    }
}

// ---------------------------------------------------------------------------
// The file source
// ---------------------------------------------------------------------------

TEST_CASE("a capture reads back exactly, including a short final block", "[source][m1]") {
    constexpr std::size_t kSamples = 100'003;  // prime, so no block size divides it
    const ScratchCapture capture(kSamples);

    auto opened = source::open_source(capture.uri(2'400'000));
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    source::StreamOptions options;
    options.block_samples = 7919;  // also prime
    const auto got = drain(**opened, options, kSamples);

    REQUIRE(got.samples.size() == kSamples);
    CHECK(got.dropped_total == 0);

    // Byte-identical, not close. A file source that resamples or rounds is not
    // a file source.
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < kSamples; ++i) {
        if (got.samples[i] != capture.expected()[i]) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);

    // Indices are contiguous from zero, which is what every downstream stage
    // relies on to detect a gap arithmetically.
    for (std::size_t b = 0; b < got.block_starts.size(); ++b) {
        INFO("block " << b);
        CHECK(got.block_starts[b] == static_cast<dsp::SampleIndex>(b) * options.block_samples);
    }
}

TEST_CASE("a start index lands exactly on the sample asked for", "[source][m1]") {
    constexpr std::size_t kSamples = 60'000;
    constexpr dsp::SampleIndex kStart = 41'337;
    const ScratchCapture capture(kSamples);

    auto opened = source::open_source(capture.uri(2'400'000));
    REQUIRE(opened.has_value());

    source::StreamOptions options;
    options.block_samples = 4096;
    options.start_index = kStart;
    const auto got = drain(**opened, options, 5000);

    REQUIRE(got.samples.size() >= 5000);
    REQUIRE_FALSE(got.block_starts.empty());

    // Exactly, not approximately. An off-by-one here shifts every timestamp
    // downstream and is invisible in the audio.
    CHECK(got.block_starts.front() == kStart);
    for (std::size_t i = 0; i < 5000; ++i) {
        if (got.samples[i] != capture.expected()[kStart + i]) {
            FAIL("sample " << i << " after a start at " << kStart << " does not match");
        }
    }
}

// ---------------------------------------------------------------------------
// The synthetic source
// ---------------------------------------------------------------------------

TEST_CASE("a synthetic scene is reproducible from its seed", "[source][m1]") {
    // The property the whole offline test strategy rests on. A scene that is
    // not byte-identical from the same seed cannot referee a decoder, cannot
    // reproduce a BER point, and cannot be used to reproduce a failure from a
    // CI log.
    const char* uri = "synthetic:wideband?rate=2400000&emitters=6&seed=20260918&samples=40000";

    auto run = [&]() {
        auto opened = source::open_source(uri);
        REQUIRE(opened.has_value());
        source::StreamOptions options;
        options.block_samples = 8192;
        return drain(**opened, options, 40000);
    };

    const auto first = run();
    const auto second = run();

    REQUIRE(first.samples.size() == second.samples.size());
    REQUIRE_FALSE(first.samples.empty());

    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < first.samples.size(); ++i) {
        if (first.samples[i] != second.samples[i]) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);

    // And a different seed gives a different scene, or the seed is not being
    // used at all.
    auto other = source::open_source(
        "synthetic:wideband?rate=2400000&emitters=6&seed=20260919&samples=40000");
    REQUIRE(other.has_value());
    source::StreamOptions options;
    options.block_samples = 8192;
    const auto third = drain(**other, options, 40000);
    CHECK(third.samples != first.samples);
}

// ---------------------------------------------------------------------------
// Flow control, which is the claim the offline path rests on
// ---------------------------------------------------------------------------

TEST_CASE("a Demand source runs at its consumer's rate", "[source][m1]") {
    // This is the mechanism behind faster than realtime, tested from the other
    // direction: if the sink is slow, the source must be slow, because nothing
    // is buffering on its behalf. A source that raced ahead here would be
    // dropping samples or growing an unbounded queue, and both are worse than
    // being slow.
    constexpr std::size_t kSamples = 200'000;
    const ScratchCapture capture(kSamples);

    auto opened = source::open_source(capture.uri(2'400'000));
    REQUIRE(opened.has_value());

    source::StreamOptions options;
    options.block_samples = 16384;

    std::atomic<int> blocks{0};
    const auto start = std::chrono::steady_clock::now();
    const auto started = (*opened)->start(options, [&](const source::SourceBlock&) -> Status {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        blocks.fetch_add(1, std::memory_order_relaxed);
        return {};
    });
    REQUIRE(started.has_value());
    while ((*opened)->running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE((*opened)->stop().has_value());
    const auto elapsed = std::chrono::steady_clock::now() - start;

    const int delivered = blocks.load(std::memory_order_relaxed);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    INFO(delivered << " blocks, " << millis << " ms wall");

    REQUIRE(delivered > 2);

    // The file itself delivers this in a millisecond or two. The elapsed time
    // must be dominated by the sink's own sleeping, which is what proves the
    // source waited for it.
    CHECK(millis >= (delivered - 1) * 8);

    CHECK((*opened)->stats().overrun_events == 0);
    CHECK((*opened)->stats().samples_lost == 0);
}

TEST_CASE("a sink error stops the stream and is returned from stop", "[source][m1]") {
    constexpr std::size_t kSamples = 100'000;
    const ScratchCapture capture(kSamples);

    auto opened = source::open_source(capture.uri(2'400'000));
    REQUIRE(opened.has_value());

    source::StreamOptions options;
    options.block_samples = 4096;

    int seen = 0;
    const auto started = (*opened)->start(options, [&](const source::SourceBlock&) -> Status {
        if (++seen == 3) {
            return fail("the consumer gave up on purpose");
        }
        return {};
    });
    REQUIRE(started.has_value());

    // The sink's error ends the stream by itself; wait for the thread to
    // notice rather than racing it with a stop.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while ((*opened)->running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto stopped = (*opened)->stop();

    // The error travels back rather than being swallowed. A stream that ends
    // early and reports success is a truncated recording nobody notices.
    REQUIRE_FALSE(stopped.has_value());
    CHECK(stopped.error().message.find("on purpose") != std::string::npos);
    CHECK(seen == 3);
}

// ---------------------------------------------------------------------------
// A file's own pace
// ---------------------------------------------------------------------------

TEST_CASE("a file's pace= is its own and a bad one is refused", "[source]") {
    const ScratchCapture capture(10'000);
    const std::string base = capture.uri(2'400'000);

    // None stated takes the caller's StreamOptions::pace, which is what every
    // URI written before pace= existed still gets.
    auto plain = source::open_source(base);
    REQUIRE(plain.has_value());
    CHECK_FALSE((*plain)->own_pace().has_value());

    auto four = source::open_source(base + "&pace=4");
    INFO(test::message_of(four));
    REQUIRE(four.has_value());
    CHECK((*four)->own_pace() == 4.0);

    // max is what a person typing the URI means by zero.
    for (const char* spelling : {"max", "MAX", "0"}) {
        auto flat_out = source::open_source(base + "&pace=" + spelling);
        INFO(spelling);
        REQUIRE(flat_out.has_value());
        CHECK((*flat_out)->own_pace() == 0.0);
    }

    for (const char* bad : {"-1", "fast", "inf", ""}) {
        auto refused = source::open_source(base + "&pace=" + bad);
        INFO("pace=" << bad);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message.find("pace") != std::string::npos);
    }
}

TEST_CASE("a file plays at its pace and a change restarts the stopwatch", "[source]") {
    // Two seconds of capture at 100 kS/s, opened at ten times realtime, so
    // 50000 samples are out 50 ms in. The pace then drops to realtime.
    //
    // THE STOPWATCH IS WHAT THIS IS ABOUT. Timed from the stream's start at
    // the new pace, sample 50000 is not due until 0.5 s, so the stream would
    // go silent for 0.45 s while the clock caught up with where it already
    // was. Restarted at the change, it goes on at realtime at once: 20000
    // more samples in the next 200 ms, where the old origin gives none.
    constexpr dsp::SampleRate kRate = 100'000;
    const ScratchCapture capture(static_cast<std::size_t>(2 * kRate));

    auto opened = source::open_source(capture.uri(kRate) + "&pace=10");
    REQUIRE(opened.has_value());

    source::StreamOptions options;
    options.block_samples = 1000;

    // Unthrottled in the options, which the URI's pace overrides.
    options.pace = 0.0;

    std::atomic<std::uint64_t> delivered{0};
    const auto start = std::chrono::steady_clock::now();
    REQUIRE((*opened)
                ->start(options,
                        [&](const source::SourceBlock& block) -> Status {
                            delivered.fetch_add(block.sample_count, std::memory_order_relaxed);
                            return {};
                        })
                .has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE((*opened)->set_pace(1.0).has_value());
    const std::uint64_t at_change = delivered.load(std::memory_order_relaxed);
    CHECK((*opened)->own_pace() == 1.0);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const std::uint64_t after = delivered.load(std::memory_order_relaxed);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    // Stopped here rather than played out, which at realtime would be most of
    // two seconds for nothing more to check.
    REQUIRE((*opened)->stop().has_value());
    INFO(at_change << " samples at the change, " << after << " 200 ms later, " << seconds
                   << " s in");

    // Ten times realtime before the change, and neither realtime nor flat out:
    // 50000 samples, with room either way for the sleep's granularity.
    CHECK(at_change >= 30'000);
    CHECK(at_change <= 100'000);

    // Realtime from the change on: 20000 in 200 ms, where timing from the
    // old origin would have delivered nothing.
    CHECK(after - at_change >= 12'000);
    CHECK(after - at_change <= 28'000);

    // And a negative pace is refused rather than read as flat out.
    CHECK_FALSE((*opened)->set_pace(-1.0).has_value());
}

TEST_CASE("a client's open gets pace=1 on a file and nothing else", "[source]") {
    using source::with_default_file_pace;

    CHECK(with_default_file_pace("file:///C:/x.cf32?rate=2400000", "1") ==
          "file:///C:/x.cf32?rate=2400000&pace=1");
    CHECK(with_default_file_pace("file:///C:/x.wav", "1") == "file:///C:/x.wav?pace=1");
    CHECK(with_default_file_pace("file:///C:/x.wav?", "1") == "file:///C:/x.wav?pace=1");
    CHECK(with_default_file_pace("FILE:///C:/x.wav", "1") == "FILE:///C:/x.wav?pace=1");

    // A pace already stated is the caller's, including one asking for flat out.
    CHECK(with_default_file_pace("file:///C:/x.wav?pace=max", "1") ==
          "file:///C:/x.wav?pace=max");
    CHECK(with_default_file_pace("file:///C:/x.wav?pace=4&center=7100000", "1") ==
          "file:///C:/x.wav?pace=4&center=7100000");

    // Not a file: a synthetic scene and a radio are the host's to pace.
    CHECK(with_default_file_pace("synthetic:wideband?rate=2400000", "1") ==
          "synthetic:wideband?rate=2400000");
    CHECK(with_default_file_pace("rtlsdr://0?freq=98.1M", "1") == "rtlsdr://0?freq=98.1M");

    // Unreadable goes back as it came, for the open to refuse in its own words.
    CHECK(with_default_file_pace("file:///C:/x.wav?rate", "1") == "file:///C:/x.wav?rate");
    CHECK(with_default_file_pace("nonsense", "1") == "nonsense");
}

// ---------------------------------------------------------------------------
// Containers: SigMF
// ---------------------------------------------------------------------------

namespace {

struct SigmfCapture {
    std::uint64_t sample_start = 0;
    bool has_frequency = false;
    std::int64_t frequency = 0;
    std::string datetime;
};

[[nodiscard]] std::string sigmf_json(std::string_view datatype, std::int64_t rate,
                                     const std::vector<SigmfCapture>& captures,
                                     std::string_view extra_global = {}) {
    std::string out = R"({"global":{"core:version":"1.0.0","core:datatype":")";
    out += datatype;
    out += R"(")";
    if (rate > 0) {
        out += R"(,"core:sample_rate":)";
        out += std::to_string(rate);
    }
    if (!extra_global.empty()) {
        out += ',';
        out += extra_global;
    }
    out += R"(},"captures":[)";
    for (std::size_t i = 0; i < captures.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        out += R"({"core:sample_start":)";
        out += std::to_string(captures[i].sample_start);
        if (captures[i].has_frequency) {
            out += R"(,"core:frequency":)";
            out += std::to_string(captures[i].frequency);
        }
        if (!captures[i].datetime.empty()) {
            out += R"(,"core:datetime":")";
            out += captures[i].datetime;
            out += R"(")";
        }
        out += '}';
    }
    out += R"(],"annotations":[]})";
    return out;
}

[[nodiscard]] bool a_note_mentions(const source::SourceCapabilities& caps,
                                   std::string_view needle) {
    for (const std::string& note : caps.notes) {
        if (note.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("a SigMF pair supplies the rate, the format, the centre and the anchor",
          "[source][sigmf][hf]") {
    // REJECTS: the reader this backend had before containers, which takes
    // rate=, format= and center= from the URI and reads the dataset raw. Given
    // this URI, which states none of the three, that reader fails to open; a
    // reader that read global but stopped before the captures array opens at
    // 0 Hz with no anchor.
    const ScratchDir scratch("sigmf_roundtrip");
    const auto data = scratch.path("capture.sigmf-data");
    const auto meta = scratch.path("capture.sigmf-meta");

    constexpr std::size_t kSamples = 20'000;
    const auto samples = ramp_cf32(kSamples, 0);
    ScratchDir::write(data, as_bytes_of(samples));

    SigmfCapture capture;
    capture.sample_start = 0;
    capture.has_frequency = true;
    capture.frequency = 7'074'000;
    capture.datetime = "2026-09-21T14:03:07.250Z";
    ScratchDir::write_text(meta, sigmf_json("cf32_le", 2'000'000, {capture}));

    auto opened = source::open_source(ScratchDir::uri(data));
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    CHECK((*opened)->sample_rate() == 2'000'000);
    CHECK((*opened)->center() == 7'074'000);

    const auto& caps = (*opened)->capabilities();
    CHECK(caps.native_format == source::SampleFormat::Cf32);
    CHECK(caps.length_samples == kSamples);

    source::StreamOptions options;
    options.block_samples = 4096;
    const auto got = drain(**opened, options, kSamples);

    REQUIRE(got.samples.size() == kSamples);
    CHECK(got.rate == 2'000'000);
    CHECK(got.anchor_ns == epoch_ns_utc(2026, 9, 21, 14, 3, 7, 250'000'000));

    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < kSamples; ++i) {
        if (got.samples[i] != samples[i]) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("a SigMF dataset with no sidecar names the file it went looking for",
          "[source][sigmf]") {
    // REJECTS: a reader whose container sniffing falls back to raw when the
    // sidecar is absent. That reader reports a missing format= or an
    // unrecognised .sigmf-data extension, which sends whoever reads it to fix
    // the URI rather than to find the file that is actually missing.
    const ScratchDir scratch("sigmf_nosidecar");
    const auto data = scratch.path("orphan.sigmf-data");
    ScratchDir::write(data, as_bytes_of(ramp_cf32(1000, 0)));

    const auto opened = source::open_source(ScratchDir::uri(data));
    REQUIRE_FALSE(opened.has_value());
    INFO(opened.error().message);

    CHECK(opened.error().message.find("orphan.sigmf-meta") != std::string::npos);
    CHECK(opened.error().message.find("container=raw") != std::string::npos);
}

TEST_CASE("a SigMF sidecar that outruns its dataset is refused with both lengths",
          "[source][sigmf]") {
    // A capture interrupted by a full disk leaves exactly this: a sidecar
    // describing the recording somebody meant to make.
    //
    // REJECTS: a reader that clamps a capture segment to what the file holds.
    // That one opens, plays a shorter stream than the metadata describes, and
    // says nothing, so the missing minutes are discovered by whoever wonders
    // where the transmission went.
    const ScratchDir scratch("sigmf_short");
    const auto data = scratch.path("cut.sigmf-data");
    const auto meta = scratch.path("cut.sigmf-meta");

    ScratchDir::write(data, as_bytes_of(ramp_cf32(5'000, 0)));

    SigmfCapture first;
    first.has_frequency = true;
    first.frequency = 14'074'000;
    SigmfCapture second;
    second.sample_start = 900'000;  // far past the 5000 samples on disk
    second.has_frequency = true;
    second.frequency = 14'074'000;
    ScratchDir::write_text(meta, sigmf_json("cf32_le", 2'000'000, {first, second}));

    const auto opened = source::open_source(ScratchDir::uri(data));
    REQUIRE_FALSE(opened.has_value());
    INFO(opened.error().message);

    CHECK(opened.error().message.find("900000") != std::string::npos);
    CHECK(opened.error().message.find("5000") != std::string::npos);
    CHECK(opened.error().message.find("disagree") != std::string::npos);
}

TEST_CASE("a rate in the URI that contradicts the sidecar is refused rather than ranked",
          "[source][sigmf]") {
    // REJECTS: any precedence rule. A reader that lets the URI win, and a
    // reader that lets the sidecar win, both open this and both play the
    // samples at a rate one of the two authors believed was wrong. Every
    // frequency derived from it is off by 2400000/2000000, which is a
    // spectrum that looks entirely ordinary.
    const ScratchDir scratch("sigmf_ratefight");
    const auto data = scratch.path("clash.sigmf-data");
    const auto meta = scratch.path("clash.sigmf-meta");

    ScratchDir::write(data, as_bytes_of(ramp_cf32(4'000, 0)));
    SigmfCapture capture;
    capture.has_frequency = true;
    capture.frequency = 10'000'000;
    ScratchDir::write_text(meta, sigmf_json("cf32_le", 2'000'000, {capture}));

    const auto opened = source::open_source(ScratchDir::uri(data, "rate=2400000"));
    REQUIRE_FALSE(opened.has_value());
    INFO(opened.error().message);

    CHECK(opened.error().message.find("2000000") != std::string::npos);
    CHECK(opened.error().message.find("2400000") != std::string::npos);
}

TEST_CASE("a SigMF recording that retunes is refused until a segment is named",
          "[source][sigmf][hf]") {
    // THE SILENT FAILURE THIS WHOLE BRANCH EXISTS FOR.
    //
    // REJECTS: the obvious reader, which takes captures[0], reports its
    // frequency, and plays the dataset through. That reader opens this file
    // and reports 7074000 Hz for all 30000 samples, so the 20000 samples
    // recorded at 14074000 Hz are displayed 7 MHz away from where they were
    // transmitted, with a spectrum that looks like a spectrum the whole time.
    const ScratchDir scratch("sigmf_retune");
    const auto data = scratch.path("hop.sigmf-data");
    const auto meta = scratch.path("hop.sigmf-meta");

    constexpr std::size_t kTotal = 30'000;
    constexpr std::uint64_t kSecondStart = 10'000;
    const auto samples = ramp_cf32(kTotal, 0);
    ScratchDir::write(data, as_bytes_of(samples));

    SigmfCapture first;
    first.sample_start = 0;
    first.has_frequency = true;
    first.frequency = 7'074'000;
    SigmfCapture second;
    second.sample_start = kSecondStart;
    second.has_frequency = true;
    second.frequency = 14'074'000;
    ScratchDir::write_text(meta, sigmf_json("cf32_le", 2'000'000, {first, second}));

    SECTION("with no segment named, it refuses and lists what to choose from") {
        const auto opened = source::open_source(ScratchDir::uri(data));
        REQUIRE_FALSE(opened.has_value());
        INFO(opened.error().message);

        CHECK(opened.error().message.find("retunes") != std::string::npos);
        CHECK(opened.error().message.find("segment=0") != std::string::npos);
        CHECK(opened.error().message.find("segment=1") != std::string::npos);
        CHECK(opened.error().message.find("7074000") != std::string::npos);
        CHECK(opened.error().message.find("14074000") != std::string::npos);
    }

    SECTION("the named segment plays its own samples at its own frequency") {
        auto opened = source::open_source(ScratchDir::uri(data, "segment=1"));
        INFO(test::message_of(opened));
        REQUIRE(opened.has_value());

        CHECK((*opened)->center() == 14'074'000);
        CHECK((*opened)->capabilities().length_samples == kTotal - kSecondStart);

        source::StreamOptions options;
        options.block_samples = 4096;
        const auto got = drain(**opened, options, kTotal);

        REQUIRE(got.samples.size() == kTotal - kSecondStart);

        // The byte offset is the part a reader gets wrong by a whole segment.
        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < got.samples.size(); ++i) {
            if (got.samples[i] != samples[kSecondStart + i]) {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0);
    }

    SECTION("a segment index past the end names how many there are") {
        const auto opened = source::open_source(ScratchDir::uri(data, "segment=7"));
        REQUIRE_FALSE(opened.has_value());
        INFO(opened.error().message);
        CHECK(opened.error().message.find("has 2") != std::string::npos);
    }
}

TEST_CASE("several capture segments at one frequency play as one stream", "[source][sigmf]") {
    // REJECTS: a reader that refuses every multi-capture file, which is what
    // the previous case alone would let somebody write. SigMF splits captures
    // for reasons other than retuning, a gap in the recording among them, and
    // refusing those would make the corpus format of record unreadable for
    // half the files it produces.
    const ScratchDir scratch("sigmf_uniform");
    const auto data = scratch.path("split.sigmf-data");
    const auto meta = scratch.path("split.sigmf-meta");

    constexpr std::size_t kTotal = 8'000;
    const auto samples = ramp_cf32(kTotal, 3);
    ScratchDir::write(data, as_bytes_of(samples));

    SigmfCapture first;
    first.has_frequency = true;
    first.frequency = 3'573'000;
    SigmfCapture second;
    second.sample_start = 4'000;
    second.has_frequency = true;
    second.frequency = 3'573'000;
    ScratchDir::write_text(meta, sigmf_json("cf32_le", 384'000, {first, second}));

    auto opened = source::open_source(ScratchDir::uri(data));
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    CHECK((*opened)->center() == 3'573'000);
    CHECK((*opened)->capabilities().length_samples == kTotal);
    CHECK(a_note_mentions((*opened)->capabilities(), "2 capture segments"));

    source::StreamOptions options;
    options.block_samples = 1024;
    const auto got = drain(**opened, options, kTotal);
    REQUIRE(got.samples.size() == kTotal);
    CHECK(got.samples.back() == samples.back());
}

TEST_CASE("a datatype this reader cannot honour is refused rather than reinterpreted",
          "[source][sigmf]") {
    // REJECTS: the two-line datatype reader that strips a "_le" or "_be"
    // suffix and switches on what is left. It accepts cf32_be and reads it
    // little-endian, which byte-swaps every sample into another value in the
    // same range, so nothing anywhere in the file looks wrong. It accepts
    // rf32_le the same way and reads one real stream as interleaved I and Q,
    // which halves the rate and mirrors the spectrum.
    const ScratchDir scratch("sigmf_datatype");
    const auto data = scratch.path("odd.sigmf-data");
    const auto meta = scratch.path("odd.sigmf-meta");
    ScratchDir::write(data, as_bytes_of(ramp_cf32(1'000, 0)));

    SigmfCapture capture;
    capture.has_frequency = true;
    capture.frequency = 5'000'000;

    SECTION("big-endian") {
        ScratchDir::write_text(meta, sigmf_json("cf32_be", 1'000'000, {capture}));
        const auto read = source::read_sigmf_metadata(ScratchDir::generic(meta));
        REQUIRE_FALSE(read.has_value());
        INFO(read.error().message);
        CHECK(read.error().message.find("cf32_be") != std::string::npos);
        CHECK(read.error().message.find("big-endian") != std::string::npos);
    }

    SECTION("real rather than complex") {
        ScratchDir::write_text(meta, sigmf_json("rf32_le", 1'000'000, {capture}));
        const auto read = source::read_sigmf_metadata(ScratchDir::generic(meta));
        REQUIRE_FALSE(read.has_value());
        INFO(read.error().message);
        CHECK(read.error().message.find("rf32_le") != std::string::npos);
        CHECK(read.error().message.find("real") != std::string::npos);
    }

    SECTION("a width the upload kernels do not convert") {
        ScratchDir::write_text(meta, sigmf_json("ci32_le", 1'000'000, {capture}));
        const auto read = source::read_sigmf_metadata(ScratchDir::generic(meta));
        REQUIRE_FALSE(read.has_value());
        INFO(read.error().message);
        CHECK(read.error().message.find("ci32_le") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Containers: RIFF WAV, RF64 and BW64
// ---------------------------------------------------------------------------

TEST_CASE("a WAV with an auxi chunk supplies the rate, the centre and the start time",
          "[source][wav][hf]") {
    // REJECTS: two readers. One takes the rate and the format from the fmt
    // chunk and ignores auxi, so it opens at 0 Hz and every frequency on the
    // display is baseband. The other starts reading samples at byte zero
    // rather than at the data chunk, so its first samples are the RIFF header
    // read as floats, which is noise at the front of an otherwise correct
    // recording.
    const ScratchDir scratch("wav_auxi");
    const auto path = scratch.path("HDSDR_20260921_140307Z_7100kHz_RF.wav");

    constexpr std::size_t kSamples = 12'000;
    const auto samples = ramp_cf32(kSamples, 11);

    WavPlan plan;
    plan.rate = 1'000'000;
    plan.with_auxi = true;
    plan.auxi_center = 7'100'000;
    plan.auxi_rate = 1'000'000;
    plan.auxi_time = true;
    plan.payload = as_bytes_of(samples);
    ScratchDir::write(path, render_wav(plan));

    auto opened = source::open_source(ScratchDir::uri(path));
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    CHECK((*opened)->sample_rate() == 1'000'000);
    CHECK((*opened)->center() == 7'100'000);
    CHECK((*opened)->capabilities().native_format == source::SampleFormat::Cf32);
    CHECK((*opened)->capabilities().length_samples == kSamples);

    source::StreamOptions options;
    options.block_samples = 2048;
    const auto got = drain(**opened, options, kSamples);

    REQUIRE(got.samples.size() == kSamples);
    CHECK(got.anchor_ns == epoch_ns_utc(2026, 9, 21, 14, 3, 7, 250'000'000));

    // The first sample is the tell. A reader that skipped the wrong number of
    // header bytes has everything here shifted.
    CHECK(got.samples.front() == samples.front());
    CHECK(got.samples.back() == samples.back());
}

TEST_CASE("a WAV data chunk claiming more than the file holds is refused", "[source][wav]") {
    // REJECTS: the reader that takes min(declared, what is left). It opens a
    // truncated recording, plays what survived, and reports a clean end of
    // stream, so a capture that lost its last four minutes to a full disk is
    // indistinguishable from one that ended when the operator stopped it.
    const ScratchDir scratch("wav_overclaim");
    const auto path = scratch.path("cut.wav");

    WavPlan plan;
    plan.rate = 2'000'000;
    plan.payload = as_bytes_of(ramp_cf32(1'000, 0));
    plan.override_data_size = true;
    plan.data_size = static_cast<std::uint32_t>(plan.payload.size() * 4);
    ScratchDir::write(path, render_wav(plan));

    const auto opened = source::open_source(ScratchDir::uri(path));
    REQUIRE_FALSE(opened.has_value());
    INFO(opened.error().message);

    CHECK(opened.error().message.find(std::to_string(plan.data_size)) != std::string::npos);
    CHECK(opened.error().message.find("data chunk") != std::string::npos);
}

TEST_CASE("RF64 takes its data size from ds64 and still checks it against the file",
          "[source][wav][hf]") {
    // The 4 GB boundary, synthesised at the header rather than by writing four
    // gigabytes. A 32-bit RIFF size field cannot count past 0xFFFFFFFF, so
    // EBU Tech 3306 puts the sentinel in the chunk header and the real size in
    // ds64; at 2 MS/s cs16 that arrives about eight and a half minutes into a
    // capture, which is most HF recordings.
    //
    // NOT COVERED HERE: an actual file over 4 GB. Writing one per test run is
    // minutes of disk, so what is exercised is the sentinel path, the 64-bit
    // arithmetic on a declared size past the boundary, and the check against
    // the file. A real one would additionally exercise the platform's 64-bit
    // seek, which seek_absolute already uses _fseeki64 for.
    const ScratchDir scratch("wav_rf64");

    SECTION("the sentinel and a ds64 size that matches read correctly") {
        // REJECTS: a reader that takes the 32-bit field at face value. It
        // computes a data chunk of 4294967295 bytes, which is larger than the
        // file, so it either refuses a perfectly good RF64 recording or reads
        // 4 GB of whatever follows.
        const auto path = scratch.path("long.wav");
        constexpr std::size_t kSamples = 6'000;
        const auto samples = ramp_cf32(kSamples, 5);

        WavPlan plan;
        plan.signature = "RF64";
        plan.rate = 2'000'000;
        plan.with_ds64 = true;
        plan.data_size_sentinel = true;
        plan.payload = as_bytes_of(samples);
        plan.ds64_data_bytes = plan.payload.size();
        ScratchDir::write(path, render_wav(plan));

        auto opened = source::open_source(ScratchDir::uri(path, "center=10000000"));
        INFO(test::message_of(opened));
        REQUIRE(opened.has_value());
        CHECK((*opened)->capabilities().length_samples == kSamples);

        source::StreamOptions options;
        options.block_samples = 1024;
        const auto got = drain(**opened, options, kSamples);
        REQUIRE(got.samples.size() == kSamples);
        CHECK(got.samples.front() == samples.front());
        CHECK(got.samples.back() == samples.back());
    }

    SECTION("a ds64 size past the boundary that the file cannot hold is refused") {
        // REJECTS: a reader that truncates the 64-bit size into 32 bits. Five
        // gigabytes wraps to 0xC0000000, which is still larger than this file
        // and so still refused, but it would report the wrong number; more to
        // the point, a size of exactly 4 GiB wraps to zero and that reader
        // opens a recording with no samples in it and calls it valid.
        const auto path = scratch.path("liar.wav");
        WavPlan plan;
        plan.signature = "BW64";
        plan.rate = 2'000'000;
        plan.with_ds64 = true;
        plan.data_size_sentinel = true;
        plan.payload = as_bytes_of(ramp_cf32(500, 0));
        plan.ds64_data_bytes = 5'000'000'000ull;
        ScratchDir::write(path, render_wav(plan));

        const auto opened = source::open_source(ScratchDir::uri(path, "center=10000000"));
        REQUIRE_FALSE(opened.has_value());
        INFO(opened.error().message);
        CHECK(opened.error().message.find("5000000000") != std::string::npos);
    }

    SECTION("a plain RIFF carrying the sentinel with no ds64 is refused") {
        // REJECTS: a reader that treats 0xFFFFFFFF as "to the end of the
        // file". The sentinel means "look in ds64", and a RIFF file has no
        // ds64 to look in, so the size is simply unknown.
        const auto path = scratch.path("sentinel.wav");
        WavPlan plan;
        plan.rate = 2'000'000;
        plan.data_size_sentinel = true;
        plan.payload = as_bytes_of(ramp_cf32(500, 0));
        ScratchDir::write(path, render_wav(plan));

        const auto opened = source::open_source(ScratchDir::uri(path, "center=10000000"));
        REQUIRE_FALSE(opened.has_value());
        INFO(opened.error().message);
        CHECK(opened.error().message.find("4294967295") != std::string::npos);
    }
}

TEST_CASE("a RIFX file is refused by name rather than read byte-swapped", "[source][wav]") {
    // REJECTS: the reader that checks bytes 8 to 11 for "WAVE" and goes on.
    // RIFX is the big-endian RIFF variant, so its chunk sizes read as absurd
    // numbers and its samples read as different samples in the same range.
    // Some of those files even parse, because a big-endian 16 in the fmt size
    // field is 0x10000000 and the walk simply falls off the end, which reports
    // a missing data chunk instead of the byte order.
    const ScratchDir scratch("wav_rifx");
    const auto path = scratch.path("swapped.wav");

    WavPlan plan;
    plan.signature = "RIFX";
    plan.rate = 2'000'000;
    plan.payload = as_bytes_of(ramp_cf32(500, 0));
    ScratchDir::write(path, render_wav(plan));

    const auto opened = source::open_source(ScratchDir::uri(path, "center=10000000"));
    REQUIRE_FALSE(opened.has_value());
    INFO(opened.error().message);
    CHECK(opened.error().message.find("RIFX") != std::string::npos);
    CHECK(opened.error().message.find("byte-swapped") != std::string::npos);
}

TEST_CASE("a WAV with no centre frequency says so rather than presenting DC",
          "[source][wav][hf]") {
    // Perseus and the other recorders that write a private chunk land here.
    //
    // REJECTS: a reader that leaves the centre at zero and returns success.
    // That is the failure this project spent a night on from the other
    // direction: the recording opens, the waterfall draws, every frequency
    // label is baseband, and nothing anywhere says the frequency was never
    // supplied.
    const ScratchDir scratch("wav_nocentre");
    const auto path = scratch.path("perseus_like.wav");

    WavPlan plan;
    plan.rate = 2'000'000;
    plan.payload = as_bytes_of(ramp_cf32(2'000, 0));
    ScratchDir::write(path, render_wav(plan));

    auto opened = source::open_source(ScratchDir::uri(path));
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    CHECK((*opened)->center() == 0);
    CHECK((*opened)->sample_rate() == 2'000'000);
    CHECK(a_note_mentions((*opened)->capabilities(), "no auxi chunk"));
    CHECK(a_note_mentions((*opened)->capabilities(), "center="));
}

TEST_CASE("an auxi rate that contradicts the fmt chunk is refused", "[source][wav]") {
    // REJECTS: a reader that prefers one of the two. The auxi chunk has no
    // specification behind it, so neither field is authoritative and nothing
    // in the file says which writer was confused. Preferring either one opens
    // a recording whose every derived frequency may be off by the ratio.
    const ScratchDir scratch("wav_ratefight");
    const auto path = scratch.path("clash.wav");

    WavPlan plan;
    plan.rate = 2'000'000;
    plan.with_auxi = true;
    plan.auxi_center = 7'100'000;
    plan.auxi_rate = 192'000;
    plan.payload = as_bytes_of(ramp_cf32(1'000, 0));
    ScratchDir::write(path, render_wav(plan));

    const auto opened = source::open_source(ScratchDir::uri(path));
    REQUIRE_FALSE(opened.has_value());
    INFO(opened.error().message);
    CHECK(opened.error().message.find("2000000") != std::string::npos);
    CHECK(opened.error().message.find("192000") != std::string::npos);
}

TEST_CASE("a channel count that is not two is refused", "[source][wav]") {
    // REJECTS: a reader that multiplies bits by channels for the stride and
    // otherwise does not look. A mono WAV is an audio recording, and read as
    // interleaved IQ it produces a mirrored spectrum at half the rate, which
    // is the same failure as a real SigMF datatype and looks just as ordinary.
    const ScratchDir scratch("wav_mono");
    const auto path = scratch.path("mono.wav");

    WavPlan plan;
    plan.channels = 1;
    plan.rate = 48'000;
    plan.payload = as_bytes_of(ramp_cf32(1'000, 0));
    ScratchDir::write(path, render_wav(plan));

    const auto opened = source::open_source(ScratchDir::uri(path, "center=7100000"));
    REQUIRE_FALSE(opened.has_value());
    INFO(opened.error().message);
    CHECK(opened.error().message.find("1 channel") != std::string::npos);
}

TEST_CASE("a 24-bit extensible WAV opens as cs24 and delivers its bytes untouched",
          "[source][wav][hf]") {
    // The layout of the KF4FIC HF recordings in docs/recordings.md: a 40-byte
    // WAVE_FORMAT_EXTENSIBLE fmt chunk, PCM subformat, two channels at 24
    // bits, no auxi, 96 kS/s. Those files were refused until the cs24 kernel
    // existed, because the only other way to read them was a host pass.
    //
    // REJECTS: a reader that converts on the host, which would deliver
    // eight-byte floats where the block says six-byte codes; one that reads
    // 24-bit as 32 and walks off the frame stride; and one that drops the odd
    // sample count's final frame.
    const ScratchDir scratch("wav_cs24");
    const auto path = scratch.path("hf24.wav");

    constexpr std::size_t kSamples = 1'001;
    std::vector<std::byte> payload(kSamples * 6);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        // Every byte distinguishable from its neighbours, and a high bit set on
        // many of them, so a reordered or sign-mangled byte cannot hide.
        payload[i] = static_cast<std::byte>(static_cast<std::uint8_t>((i * 151U + 7U) & 0xFFU));
    }

    WavPlan plan;
    plan.extensible = true;
    plan.format_tag = 1;
    plan.bits = 24;
    plan.rate = 96'000;
    plan.payload = payload;
    ScratchDir::write(path, render_wav(plan));

    auto opened = source::open_source(ScratchDir::uri(path, "center=7150000"));
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    const auto& caps = (*opened)->capabilities();
    CHECK(caps.native_format == source::SampleFormat::Cs24);
    CHECK(caps.bits_per_component == 24);
    CHECK(source::bytes_per_sample(caps.native_format) == 6);
    CHECK(caps.length_samples == kSamples);
    CHECK((*opened)->center() == 7'150'000);

    // The file has no centre of its own, so without center= the HF request
    // could not be stated. With it, the span reaches HF and says so.
    CHECK(caps.resolution.stated());
    CHECK(caps.resolution.narrowest_signal_hz == 31);

    std::vector<std::byte> delivered;
    bool every_block_cs24 = true;
    source::StreamOptions options;
    options.block_samples = 97;  // prime, so the final block is short and odd
    const auto started =
        (*opened)->start(options, [&](const source::SourceBlock& block) -> Status {
            every_block_cs24 = every_block_cs24 && block.format == source::SampleFormat::Cs24 &&
                               block.bytes.size() == block.sample_count * 6;
            delivered.insert(delivered.end(), block.bytes.begin(), block.bytes.end());
            return {};
        });
    REQUIRE(started.has_value());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while ((*opened)->running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto stopped = (*opened)->stop();
    INFO(test::message_of(stopped));
    CHECK(stopped.has_value());

    CHECK(every_block_cs24);
    REQUIRE(delivered.size() == payload.size());
    CHECK(delivered == payload);
}

TEST_CASE("a 24-bit WAV that is not a whole number of frames is refused", "[source][wav]") {
    // Three stray bytes is half a frame at six bytes a sample. The generic
    // whole-sample check has to hold at a width that is not a power of two.
    const ScratchDir scratch("wav_cs24_ragged");
    const auto path = scratch.path("ragged24.wav");

    WavPlan plan;
    plan.extensible = true;
    plan.format_tag = 1;
    plan.bits = 24;
    plan.rate = 96'000;
    plan.payload.assign(100 * 6 + 3, std::byte{0x11});
    ScratchDir::write(path, render_wav(plan));

    const auto opened = source::open_source(ScratchDir::uri(path, "center=7150000"));
    REQUIRE_FALSE(opened.has_value());
    INFO(opened.error().message);
    CHECK(opened.error().message.find("whole number of cs24 samples") != std::string::npos);
}

TEST_CASE("a raw cs24 file is named by its extension or by format=", "[source]") {
    const ScratchDir scratch("raw_cs24");
    const auto path = scratch.path("plain.cs24");
    ScratchDir::write(path, std::vector<std::byte>(600, std::byte{0x40}));

    auto by_extension = source::open_source(ScratchDir::uri(path, "rate=96000"));
    INFO(test::message_of(by_extension));
    REQUIRE(by_extension.has_value());
    CHECK((*by_extension)->capabilities().native_format == source::SampleFormat::Cs24);
    CHECK((*by_extension)->capabilities().length_samples == 100);

    const auto other = scratch.path("plain.iq");
    ScratchDir::write(other, std::vector<std::byte>(600, std::byte{0x40}));
    auto by_name = source::open_source(ScratchDir::uri(other, "rate=96000&format=cs24"));
    INFO(test::message_of(by_name));
    REQUIRE(by_name.has_value());
    CHECK((*by_name)->capabilities().native_format == source::SampleFormat::Cs24);
}

// ---------------------------------------------------------------------------
// Raw, which the containers must not have changed
// ---------------------------------------------------------------------------

TEST_CASE("a raw file with a partial trailing sample is refused", "[source]") {
    // REJECTS: the one-liner, length = size / bytes_per_sample. It floors, so
    // it opens a file whose last sample is half there, plays the whole
    // samples, and reports a clean end of stream. The interrupted writer and
    // the wrong declared format both arrive looking like that.
    const ScratchDir scratch("raw_partial");
    const auto path = scratch.path("ragged.cf32");

    auto bytes = as_bytes_of(ramp_cf32(1'000, 0));
    bytes.resize(bytes.size() + 3);
    ScratchDir::write(path, bytes);

    const auto opened = source::open_source(ScratchDir::uri(path, "rate=2000000"));
    REQUIRE_FALSE(opened.has_value());
    INFO(opened.error().message);
    CHECK(opened.error().message.find("whole number of cf32 samples") != std::string::npos);
    CHECK(opened.error().message.find(std::to_string(bytes.size())) != std::string::npos);
}

TEST_CASE("a raw file still needs rate= and still infers its format from the extension",
          "[source]") {
    // The containers moved the extension inference out of the URI layer and
    // into the backend, which runs it only after the container has had its
    // say. This is the case that would have gone quiet if the move had
    // dropped it.
    //
    // REJECTS: a backend that only ever takes the format from the container,
    // which fails on a raw file with no format= even though the extension
    // says cf32; and one that defaults a missing rate to something plausible.
    const ScratchDir scratch("raw_still_works");
    const auto path = scratch.path("plain.cf32");
    ScratchDir::write(path, as_bytes_of(ramp_cf32(1'000, 0)));

    auto opened = source::open_source(ScratchDir::uri(path, "rate=2400000"));
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());
    CHECK((*opened)->capabilities().native_format == source::SampleFormat::Cf32);

    const auto no_rate = source::open_source(ScratchDir::uri(path));
    REQUIRE_FALSE(no_rate.has_value());
    INFO(no_rate.error().message);
    CHECK(no_rate.error().message.find("rate=") != std::string::npos);
}

// ---------------------------------------------------------------------------
// The grid: what a source states about the scale of what it carries
// ---------------------------------------------------------------------------

TEST_CASE("the shipped geometry does not meet what an HF span asks for", "[source][hf]") {
    // The arithmetic the operator hit from the other direction. A bin is
    // 2 * rate / (channels * transform) hertz, so the shipped 2.4 MS/s over
    // 64 channels with a 2048-point transform is 4800000/131072 = 36.62 Hz.
    // FT8 at 50 Hz is 1.4 bins of that and PSK31 at 31 Hz is under one.
    //
    // REJECTS: a met_by that rounds the bin width to integer hertz before
    // comparing. The exactly-met case below is 31/4 against 31/4, and a
    // rounding implementation sees 8 Hz against a 7.75 Hz ceiling and answers
    // no, which would send a caller to a transform twice as large as it needs
    // for every HF recording.
    const auto request = source::resolution_for_span(7'100'000, 2'000'000);
    REQUIRE(request.stated());
    CHECK(request.narrowest_signal_hz == 31);
    CHECK(request.bins_across_narrowest == 4);
    INFO(request.basis);

    // The shipped geometry, as a rational: 2 * 2400000 over 64 * 2048.
    CHECK_FALSE(request.met_by(2 * 2'400'000, 64 * 2048));

    // A 16384-point transform on a 1 MS/s HF recording over the same 64
    // channels: 2000000/1048576, which is 1.907 Hz.
    CHECK(request.met_by(2 * 1'000'000, 64 * 16384));

    // Exactly at the ceiling counts as met, and a hair past it does not.
    CHECK(request.met_by(31, 4));
    CHECK_FALSE(request.met_by(7'751, 1'000));
}

TEST_CASE("the resolution comparison survives a product that would overflow", "[source]") {
    // REJECTS: the obvious cross-multiply, narrowest * denominator against
    // bins * numerator. Here that is 200000 * 10^14, which is 2 * 10^19 and
    // wraps a signed 64-bit integer to a negative number, so the comparison
    // answers backwards: a bin width of a hundredth of a femtohertz is
    // reported as too coarse for a 200 kHz signal.
    source::ResolutionRequest request;
    request.narrowest_signal_hz = 200'000;
    request.bins_across_narrowest = 4;

    CHECK(request.met_by(1, 100'000'000'000'000LL));
    CHECK_FALSE(request.met_by(100'000'000'000'000LL, 1));
}

TEST_CASE("a span reaching below 30 MHz asks for the HF grid", "[source][hf]") {
    // REJECTS: an implementation that tests the centre frequency rather than
    // the span's low edge. A 4 MS/s capture centred at 31 MHz covers 29 to
    // 33 MHz, so it has the top of the 10 metre band in it and the narrow
    // modes with it, while its centre sits above the boundary.
    const auto straddling = source::resolution_for_span(31'000'000, 4'000'000);
    REQUIRE(straddling.stated());
    CHECK(straddling.narrowest_signal_hz == 31);

    const auto vhf = source::resolution_for_span(145'000'000, 2'400'000);
    REQUIRE(vhf.stated());
    CHECK(vhf.narrowest_signal_hz == 12'500);

    // And the shipped geometry meets the VHF figure comfortably, which is why
    // nothing about a VHF session changes when the request is honoured.
    CHECK(vhf.met_by(2 * 2'400'000, 64 * 2048));

    // A recording at DC has not said where it was taken, and guessing HF from
    // that would put the project's finest grid onto every baseband scene.
    const auto baseband = source::resolution_for_span(0, 20'000'000);
    CHECK_FALSE(baseband.stated());
    CHECK(baseband.met_by(2 * 20'000'000, 64 * 2048));
}

TEST_CASE("a recording states the grid it needs and a dongle does not", "[source][hf]") {
    // REJECTS: a backend that fills this in from whatever it was tuned to at
    // open. A radio's centre moves under tune() while its capability
    // description does not, so a stated request there is right once and
    // quietly wrong one retune later, which is the class of failure the whole
    // capability exists to avoid.
    const ScratchDir scratch("resolution_source");
    const auto path = scratch.path("hf.cf32");
    ScratchDir::write(path, as_bytes_of(ramp_cf32(4'000, 0)));

    auto hf = source::open_source(ScratchDir::uri(path, "rate=1000000&center=7100000"));
    INFO(test::message_of(hf));
    REQUIRE(hf.has_value());
    REQUIRE((*hf)->capabilities().resolution.stated());
    CHECK((*hf)->capabilities().resolution.narrowest_signal_hz == 31);

    auto synthetic = source::open_source("synthetic:wideband?rate=2400000&emitters=2&seed=1");
    REQUIRE(synthetic.has_value());
    CHECK_FALSE((*synthetic)->capabilities().resolution.stated());
}
