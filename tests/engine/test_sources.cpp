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
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "core/dsp/types.h"
#include "core/source/registry.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;

namespace {

// A capture written to the system temp directory and removed afterwards.
class ScratchCapture {
public:
    explicit ScratchCapture(std::size_t samples) {
        path_ = std::filesystem::temp_directory_path() /
                ("revenant_test_capture_" + std::to_string(samples) + ".cf32");

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

// Drains a source into a flat vector, recording the indices it was given.
struct Drained {
    std::vector<dsp::Complex32> samples;
    std::vector<dsp::SampleIndex> block_starts;
    std::uint64_t dropped_total = 0;
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
