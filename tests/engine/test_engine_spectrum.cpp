// The engine's full-span spectrum, end to end on a real device.
//
// test_engine.cpp covers the receiver path and stops there. The spectrum path
// is a second chain hanging off the same channel ring, and until this file
// existed nothing dispatched any of it: the frequency axis, the channel ring
// sized around the transform's window, the gate that refuses a window
// straddling a skip, the readback, the colour-map scale and the sink. All of
// it is host code with device code under it, and all of it fails quietly. A
// wrong axis draws a plausible spectrum with every label wrong, an undersized
// ring draws a band of energy that was never on the air, and a broken gate
// draws the ring's leftovers as signal.
//
// So the case that carries the most weight here is the carrier one: a scene
// with a single emitter at a frequency this file chose, transformed on the
// device, read back, and turned into a frequency through the frame's own
// geometry. That number is either the emitter's or it is not, and none of the
// failures above survive it. Everything else in the file is a contract check
// around that.
//
// WHY THE PEAK IS A VOTE AND NOT ONE FRAME'S ARGMAX
//
// docs/fft.md has the integrated Radeon corrupting isolated spectrum
// dispatches, with the correct answer either side of them, at a rate its own
// sweep puts at one in 1,900 and an eyeball count of the CLI's waterfall puts
// three orders of magnitude above that. A test that reads one frame on that
// device is a test that fails for a reason that is written down and is not
// the reason the test exists. Taking the bin that most frames agree on holds
// for as long as the bad frames are a minority, and says more than any single
// frame can: that the axis is right repeatedly. It also reports the agreement
// rate, so a device that has started corrupting frames shows up as a number
// rather than as a flake.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/engine/engine.h"
#include "core/engine/spectrum_scale.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;
using Catch::Approx;

namespace {

constexpr dsp::SampleRate kSourceRate = 2'400'000;
constexpr std::uint32_t kChannels = 64;
constexpr dsp::Hertz kChannelSpacing = kSourceRate / kChannels;

// Small enough that both devices in the conformance matrix hold it in one
// workgroup's shared memory with room to spare: the integrated part reports
// 32768 bytes, which is eight times what a 512-point complex transform needs.
// The engine clamps to what the device offers and reports the clamp, so the
// cases below read EngineInfo::spectrum rather than assuming this, but a
// clamp here would mean the number was chosen badly and is checked for.
constexpr std::uint32_t kTransform = 512;

// Two coarse blocks per dispatch short of the transform's window, on purpose.
// 8192 source samples is 256 channel blocks at D = 32, so the first dispatches
// cannot fill a 512-block window and the contiguity gate has to refuse them.
// At the 32768 the other engine cases use, the very first dispatch already has
// four times the window and the gate is never exercised at all.
constexpr std::size_t kBlockSamples = 8'192;

// One unmodulated carrier and a floor far below it, the same scene shape
// test_engine.cpp uses and for the same reasons: AM because it carries its
// carrier continuously, a 3.2 kHz span because a 3 kHz AM signal is refused
// anything narrower, and the noise floor left on because an emitter's level is
// an SNR and there is nothing to place it against without one.
//
// The scene puts the emitter somewhere inside the span rather than exactly at
// the offset, so the carrier is within 1600 Hz of what is asked for. Against
// the 146 Hz bins this frame has that is eleven bins, and against the ways the
// axis can be wrong, which start at half a channel spacing, it is nothing.
std::string tone_uri(dsp::Hertz offset_hz, dsp::SampleIndex samples) {
    return "synthetic:wideband?rate=" + std::to_string(kSourceRate) +
           "&emitters=1&modes=am&seed=424242&noise_dbfs=-120&snr_min=60&snr_max=60" +
           "&samples=" + std::to_string(samples) +
           "&span_low=" + std::to_string(offset_hz - 1600) +
           "&span_high=" + std::to_string(offset_hz + 1600);
}

engine::EngineConfig spectrum_config() {
    engine::EngineConfig config;
    config.channels = kChannels;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = kBlockSamples;
    config.spectrum_transform = kTransform;
    config.gpu_index = -1;  // honours REVENANT_GPU_INDEX
    return config;
}

// One frame, copied. SpectrumFrame::power_db is valid for the call and not
// after, which is the contract these cases are also checking by obeying it.
struct CapturedFrame {
    engine::SpectrumGeometry geometry;
    dsp::SampleIndex start = 0;
    dsp::SampleIndex count = 0;
    std::uint64_t sequence = 0;
    float floor_db = 0.0F;
    float ceiling_db = 0.0F;
    float percentile_low_db = 0.0F;
    float percentile_high_db = 0.0F;

    // Where this frame's largest bin was, and how many bins of it were not a
    // finite decibel value. The whole frame is 16384 floats and there are
    // seventy of them in a run, so keeping the two numbers that matter costs
    // nothing where keeping the frames would cost megabytes.
    std::size_t peak_bin = 0;
    float peak_db = 0.0F;
    std::size_t non_finite = 0;
    std::size_t bins_seen = 0;
};

CapturedFrame reduce(const engine::SpectrumFrame& frame) {
    CapturedFrame out;
    out.geometry = frame.geometry;
    out.start = frame.start;
    out.count = frame.count;
    out.sequence = frame.sequence;
    out.floor_db = frame.floor_db;
    out.ceiling_db = frame.ceiling_db;
    out.percentile_low_db = frame.percentile_low_db;
    out.percentile_high_db = frame.percentile_high_db;
    out.bins_seen = frame.power_db.size();

    float best = -std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < frame.power_db.size(); ++i) {
        const float value = frame.power_db[i];
        if (!std::isfinite(value)) {
            ++out.non_finite;
            continue;
        }
        if (value > best) {
            best = value;
            out.peak_bin = i;
        }
    }
    out.peak_db = best;
    return out;
}

struct Capture {
    Status run_status;
    std::vector<CapturedFrame> frames;
    engine::EngineInfo info{};
};

// Runs one capture with a spectrum sink attached and hands back every frame
// it delivered, reduced. Everything in this file that needs a device needs
// exactly this.
//
// The mutex is declared before the engine so that it is destroyed after it.
// Locals are destroyed in reverse order and the sink holds a reference to
// this one, so the other order leaves a window where the engine is still
// being torn down with a dead mutex behind its callback.
Capture capture_spectrum(const engine::EngineConfig& config, const std::string& uri) {
    Capture out;
    std::mutex lock;

    auto created = engine::Engine::create(config);
    if (!created) {
        out.run_status = std::unexpected(created.error());
        return out;
    }
    auto& eng = **created;

    if (auto opened = eng.open_source(uri); !opened) {
        out.run_status = opened;
        return out;
    }
    out.info = eng.info();

    if (auto attached = eng.set_spectrum_sink([&](const engine::SpectrumFrame& frame) -> Status {
            const std::lock_guard<std::mutex> guard(lock);
            out.frames.push_back(reduce(frame));
            return {};
        });
        !attached) {
        out.run_status = attached;
        return out;
    }

    out.run_status = eng.run();
    return out;
}

}  // namespace

TEST_CASE("an engine built with no spectrum stage refuses a sink and says why",
          "[gpu][engine][spectrum][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The stage is built with the coarse chain when the source is opened,
    // because adding one to a running graph would mean rebuilding it. That
    // makes "I forgot to set spectrum_transform" a mistake a caller makes
    // once, at a point where nothing has gone wrong yet, so the refusal has
    // to name the field rather than the symptom.
    auto config = spectrum_config();
    config.spectrum_transform = 0;

    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;

    // Before a source, because there is no grid yet to size a transform
    // against.
    const auto early = eng.set_spectrum_sink([](const engine::SpectrumFrame&) -> Status {
        return {};
    });
    REQUIRE_FALSE(early.has_value());
    INFO(early.error().message);
    CHECK(early.error().message.find("source") != std::string::npos);

    REQUIRE(eng.open_source(tone_uri(0, 100'000)).has_value());

    CHECK_FALSE(eng.info().spectrum.enabled());
    CHECK(eng.info().spectrum.bins == 0);

    const auto refused = eng.set_spectrum_sink([](const engine::SpectrumFrame&) -> Status {
        return {};
    });
    REQUIRE_FALSE(refused.has_value());
    INFO(refused.error().message);
    CHECK(refused.error().message.find("spectrum_transform") != std::string::npos);
}

TEST_CASE("the spectrum's frequency axis is derived from the grid", "[gpu][engine][spectrum][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // No run: the geometry is settled when the source is opened, and it is
    // the one part of this path whose correct answer can be written down in
    // closed form. Everything the axis claims is checked against the
    // arithmetic in core/engine/engine.h rather than against a golden number,
    // so a grid or a rate this case does not use is covered by the same
    // relations.
    auto created = engine::Engine::create(spectrum_config());
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;

    REQUIRE(eng.open_source(tone_uri(0, 100'000)).has_value());

    const auto& info = eng.info();
    const auto& geometry = info.spectrum;
    INFO(std::format("transform {}, {} bins per channel, {} channels, {} bins, {} Hz per bin, "
                     "bin zero at {} Hz",
                     geometry.transform, geometry.bins_per_channel, geometry.channels,
                     geometry.bins, geometry.bin_width_hz(), geometry.bin_zero_hz()));

    REQUIRE(geometry.enabled());

    // A clamp here is not a failure of the engine, it is this file having
    // asked for a transform the device cannot hold. Both devices in the
    // matrix hold 512 with room to spare, so a clamp means the number above
    // was chosen for a machine that is no longer the one being tested.
    CHECK(geometry.transform == kTransform);

    CHECK(geometry.channels == kChannels);
    CHECK(geometry.bins_per_channel == geometry.transform / 2);
    CHECK(geometry.bins == geometry.channels * geometry.bins_per_channel);

    // rate / (D * N) hertz per bin, exactly, where D is the decimation the
    // grid settled on rather than a number this case assumes.
    const double expected_width = static_cast<double>(kSourceRate) /
                                  (static_cast<double>(info.grid.decimation) *
                                   static_cast<double>(geometry.transform));
    CHECK(geometry.bin_width_hz() == Approx(expected_width));

    // Bin zero is half a channel spacing below the negative edge of the band,
    // because the lowest coarse channel is centred on Nyquist and owns the
    // half channel either side of it.
    const double expected_zero = -0.5 * static_cast<double>(kSourceRate) -
                                 0.5 * static_cast<double>(kChannelSpacing);
    CHECK(geometry.bin_zero_hz() == Approx(expected_zero));

    // And the frame tiles the span exactly once. This is the property the
    // central-half selection exists for and the one that silently breaks on
    // any grid that is not 2x oversampled: the pieces then overlap or leave
    // holes and every label in the frame is wrong by a growing amount.
    const double span = static_cast<double>(geometry.bins) * geometry.bin_width_hz();
    INFO(std::format("{} bins of {} Hz covers {} Hz against a source rate of {}", geometry.bins,
                     geometry.bin_width_hz(), span, kSourceRate));
    CHECK(span == Approx(static_cast<double>(kSourceRate)));

    // The exact rationals are the point of carrying a numerator and a
    // denominator rather than a double: at this rate and this transform the
    // bin width is 146.484375 Hz, which is not a whole number of hertz, and
    // rounding it would put an unsourceable offset into every frequency a
    // person clicks on.
    CHECK(geometry.bin_width_denominator != 0);
    CHECK(geometry.bin_zero_denominator != 0);
}

TEST_CASE("a carrier lands in the bin the frame's own axis says it should",
          "[gpu][engine][spectrum][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The case the rest of the file is scaffolding for. A chain that
    // transformed the wrong channel, ordered the frame by channel index
    // rather than by frequency, dropped the branch reversal, sized the ring
    // too small for the window or spliced two eras together gets this wrong,
    // and none of those is visible in a spectrum that looks plausible.
    constexpr dsp::Hertz kToneOffset = kChannelSpacing * 5;
    constexpr dsp::SampleIndex kSamples = 600'000;

    const auto captured = capture_spectrum(spectrum_config(), tone_uri(kToneOffset, kSamples));
    INFO(test::message_of(captured.run_status));
    REQUIRE(captured.run_status.has_value());
    REQUIRE(captured.frames.size() > 16);

    const auto& geometry = captured.info.spectrum;

    // Which bin the frames agree on. See the note at the top of this file on
    // why this is a vote.
    std::unordered_map<std::size_t, std::size_t> votes;
    for (const auto& frame : captured.frames) {
        ++votes[frame.peak_bin];
    }
    const auto winner = std::max_element(
        votes.begin(), votes.end(),
        [](const auto& left, const auto& right) { return left.second < right.second; });
    REQUIRE(winner != votes.end());

    const std::size_t peak_bin = winner->first;
    const double peak_hz =
        geometry.bin_zero_hz() + static_cast<double>(peak_bin) * geometry.bin_width_hz();
    const double agreement =
        static_cast<double>(winner->second) / static_cast<double>(captured.frames.size());

    INFO(std::format("{} of {} frames peaked in bin {} of {}, which the axis puts at {} Hz "
                     "against an emitter asked for at {} Hz; {} distinct peak bins across the "
                     "run",
                     winner->second, captured.frames.size(), peak_bin, geometry.bins, peak_hz,
                     kToneOffset, votes.size()));

    // The emitter is somewhere within 1600 Hz of the offset, because the scene
    // places it randomly inside the span it was given. Two kilohertz is that
    // plus a couple of bins. The failures this is aimed at are not close: a
    // mirrored frame puts the peak 375 kHz away and a half-channel error puts
    // it 18.75 kHz away.
    CHECK(std::abs(peak_hz - static_cast<double>(kToneOffset)) < 2000.0);

    // Most frames agree. This is deliberately not "all of them": docs/fft.md
    // records the integrated device corrupting isolated spectrum dispatches,
    // and a bare majority there still proves the axis while an all-frames
    // assertion would report a driver fault as an axis regression.
    CHECK(agreement > 0.5);

    // Nothing came back that a colour map cannot divide by. A NaN or an
    // infinity in power_db propagates into every consumer at once, and the
    // kernel has a floor at -200 dB specifically so that the logarithm of a
    // silent bin is a number.
    std::size_t non_finite = 0;
    for (const auto& frame : captured.frames) {
        non_finite += frame.non_finite;
    }
    INFO(std::format("{} non-finite bins across {} frames of {} bins", non_finite,
                     captured.frames.size(), geometry.bins));
    CHECK(non_finite == 0);

    // The carrier is 60 dB above a floor at -120 dBFS, so the peak is a long
    // way above the frame's own high percentile. A frame of noise, which is
    // what a readback of an unwritten buffer or a stale ring looks like, has
    // no such peak.
    //
    // Counted over the frames that agreed on the bin rather than over one
    // frame, for the reason at the top of this file. Reading one frame at a
    // fixed index would put the case at the mercy of whether that index
    // landed on a corrupted dispatch.
    std::size_t weak = 0;
    float thinnest = std::numeric_limits<float>::infinity();
    for (const auto& frame : captured.frames) {
        if (frame.peak_bin != peak_bin) {
            continue;
        }
        const float margin = frame.peak_db - frame.percentile_high_db;
        thinnest = std::min(thinnest, margin);
        if (margin <= 0.0F || frame.peak_db < frame.percentile_low_db + 30.0F) {
            ++weak;
        }
    }
    INFO(std::format("of the {} frames that peaked in bin {}, {} had the carrier at or below "
                     "the frame's own high percentile; the thinnest margin was {} dB",
                     winner->second, peak_bin, weak, thinnest));
    CHECK(weak == 0);
}

TEST_CASE("every frame carries a window inside the stream and a sequence that counts",
          "[gpu][engine][spectrum][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The contiguity gate and the sink contract, which are the two halves of
    // "a waterfall that skipped a row knows it skipped a row".
    //
    // The gate refuses a dispatch whose window reaches back over a skip or
    // over the start of the stream. It is also, jointly with the floor that
    // pushes the first output block past the filter's support, what keeps the
    // window's start index from underflowing: the start is the window's first
    // channel block times the decimation, minus the prototype's group delay,
    // and that subtraction is in an unsigned type, so a window allowed to
    // begin at block zero would report a start near 2^64. It never is,
    // because the support is the whole prototype and a linear-phase
    // prototype's group delay is half of it, but nothing in the spectrum path
    // says so. Measured at the shipped geometry: the support is 1087 samples
    // and the group delay is 544, so the earliest start any configuration can
    // reach is 544 rather than a negative number. The out_of_stream count
    // below is what would catch it if that ever stopped being true.
    constexpr dsp::SampleIndex kSamples = 400'000;

    const auto captured = capture_spectrum(spectrum_config(), tone_uri(0, kSamples));
    INFO(test::message_of(captured.run_status));
    REQUIRE(captured.run_status.has_value());
    REQUIRE(captured.frames.size() > 8);

    const auto& geometry = captured.info.spectrum;
    const auto expected_count = static_cast<dsp::SampleIndex>(geometry.transform) *
                                static_cast<dsp::SampleIndex>(captured.info.grid.decimation);

    const auto& first = captured.frames.front();
    const auto& last = captured.frames.back();
    INFO(std::format("{} frames from a {} sample capture; first window [{}, {}), last window "
                     "[{}, {})",
                     captured.frames.size(), kSamples, first.start, first.start + first.count,
                     last.start, last.start + last.count));

    // The dispatches the gate refused. With 256 channel blocks per dispatch
    // and a 512-block window, at least the first two dispatches cannot have a
    // window behind them, so fewer frames arrive than the source delivered
    // blocks. A gate that passed them would draw the channel ring's
    // leftovers as signal.
    const std::size_t dispatches = (kSamples + kBlockSamples - 1) / kBlockSamples;
    INFO(std::format("{} source blocks produced {} frames", dispatches,
                     captured.frames.size()));
    CHECK(captured.frames.size() < dispatches);

    std::size_t wrong_count = 0;
    std::size_t out_of_stream = 0;
    std::size_t out_of_order = 0;
    std::size_t wrong_geometry = 0;
    std::size_t wrong_width = 0;

    for (std::size_t i = 0; i < captured.frames.size(); ++i) {
        const auto& frame = captured.frames[i];
        if (frame.count != expected_count) {
            ++wrong_count;
        }
        // The wrap described above lands here: a start near 2^64 fails the
        // first half of this, and a window running past the end of the
        // capture fails the second.
        if (frame.start >= kSamples || frame.start + frame.count > kSamples) {
            ++out_of_stream;
        }
        if (i > 0 && frame.start <= captured.frames[i - 1].start) {
            ++out_of_order;
        }
        if (frame.sequence != i) {
            ++out_of_order;
        }
        if (frame.geometry.bins != geometry.bins ||
            frame.geometry.transform != geometry.transform) {
            ++wrong_geometry;
        }
        if (frame.bins_seen != geometry.bins) {
            ++wrong_width;
        }
    }

    INFO(std::format("{} frames with the wrong window length, {} outside the stream, {} out of "
                     "order, {} with the wrong geometry, {} with the wrong bin count",
                     wrong_count, out_of_stream, out_of_order, wrong_geometry, wrong_width));

    // The window is the transform's, in source samples, on every frame.
    CHECK(wrong_count == 0);
    CHECK(out_of_stream == 0);
    CHECK(out_of_order == 0);
    CHECK(wrong_geometry == 0);
    CHECK(wrong_width == 0);

    // Sequence starts at zero for a sink attached before the run, which is
    // what stops a display drawing rows it never missed.
    CHECK(first.sequence == 0);
    CHECK(last.sequence == captured.frames.size() - 1);

    // And the first window begins after the filter's support rather than at
    // the start of the stream, which is the gate refusing to transform blocks
    // the channelizer had not filled yet.
    INFO(std::format("first window starts at sample {}", first.start));
    CHECK(first.start > 0);
}

TEST_CASE("the colour map's ends arrive on every frame beside the percentiles",
          "[gpu][engine][spectrum][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The scale is on the completion thread and its output is four floats on
    // the frame. What this checks is the wiring, not the smoothing:
    // test_spectrum_scale.cpp owns the recurrence and can step thirty seconds
    // in a microsecond, which a run on a device cannot.
    //
    // The invariant every consumer depends on is the one worth dispatching a
    // GPU for, because it is the one that is a property of the whole path
    // rather than of the class: the ends are at least the minimum span apart
    // and the frame's own body is between them.
    constexpr dsp::SampleIndex kSamples = 400'000;

    const auto captured = capture_spectrum(spectrum_config(), tone_uri(0, kSamples));
    INFO(test::message_of(captured.run_status));
    REQUIRE(captured.run_status.has_value());
    REQUIRE(captured.frames.size() > 8);

    std::size_t narrow = 0;
    std::size_t inverted_percentiles = 0;
    std::size_t non_finite_ends = 0;
    std::vector<float> lows;
    std::vector<float> highs;

    for (const auto& frame : captured.frames) {
        if (frame.ceiling_db - frame.floor_db < engine::kSpectrumMinimumSpanDb - 0.01F) {
            ++narrow;
        }
        if (frame.percentile_high_db < frame.percentile_low_db) {
            ++inverted_percentiles;
        }
        if (!std::isfinite(frame.floor_db) || !std::isfinite(frame.ceiling_db) ||
            !std::isfinite(frame.percentile_low_db) ||
            !std::isfinite(frame.percentile_high_db)) {
            ++non_finite_ends;
        }
        lows.push_back(frame.percentile_low_db);
        highs.push_back(frame.percentile_high_db);
    }

    const auto& sample = captured.frames[captured.frames.size() / 2];
    INFO(std::format("mid-run frame: map {} to {} dB, percentiles {} to {} dB", sample.floor_db,
                     sample.ceiling_db, sample.percentile_low_db, sample.percentile_high_db));
    INFO(std::format("{} frames narrower than the minimum span, {} with inverted percentiles, "
                     "{} with a non-finite end",
                     narrow, inverted_percentiles, non_finite_ends));

    CHECK(narrow == 0);
    CHECK(inverted_percentiles == 0);
    CHECK(non_finite_ends == 0);

    // The band here is static, so the smoothed ends should sit on the
    // measurement they are smoothing. Against the MEDIAN of the run's
    // percentiles rather than frame by frame, for two reasons that both
    // matter: a percentile of a 16384-bin frame moves several decibels frame
    // to frame even on a band that is not changing, which is the reason
    // nothing draws against it directly, and the scale now rejects an
    // isolated outlier outright, so a corrupted frame on the integrated
    // device legitimately sits outside the map it did not move. A per-frame
    // bracket would be asserting the behaviour the outlier guard was written
    // to remove.
    const auto middle = lows.size() / 2;
    std::nth_element(lows.begin(), lows.begin() + static_cast<std::ptrdiff_t>(middle),
                     lows.end());
    std::nth_element(highs.begin(), highs.begin() + static_cast<std::ptrdiff_t>(middle),
                     highs.end());
    const float median_low = lows[middle];
    const float median_high = highs[middle];

    INFO(std::format("median percentiles across the run are {} to {} dB against a mid-run map "
                     "of {} to {} dB",
                     median_low, median_high, sample.floor_db, sample.ceiling_db));
    CHECK(sample.floor_db == Approx(median_low).margin(6.0));
    CHECK(sample.ceiling_db == Approx(median_high).margin(6.0));

    // The percentiles are the device's own measurement and are carried
    // alongside the smoothed ends rather than instead of them, so a detector
    // that wants this frame's noise floor has it. They are not the same
    // numbers, and a path that quietly copied one pair into the other would
    // pass every check above.
    std::size_t identical = 0;
    for (const auto& frame : captured.frames) {
        if (frame.percentile_low_db == frame.floor_db &&
            frame.percentile_high_db == frame.ceiling_db) {
            ++identical;
        }
    }
    INFO(std::format("{} of {} frames had the smoothed ends exactly equal to the percentiles",
                     identical, captured.frames.size()));
    CHECK(identical < captured.frames.size());
}

TEST_CASE("a pinned end arrives exactly where it was pinned", "[gpu][engine][spectrum][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // docs/ui-spectrum.md: "A pinned end is drawn exactly where it was
    // pinned... when something has to give, it comes out of the end that is
    // still automatic." The document records that the CLI broke this first
    // and drew a pinned ceiling 21.4 dB above where it was asked for, which
    // is why the check is an equality on the pinned end rather than a range.
    //
    // Checked through the engine rather than through SpectrumScale because
    // the pin travels EngineConfig to GraphConfig to the scale, and a pin
    // dropped anywhere along that is invisible to the scale's own tests.
    //
    // The pin has to be chosen against the frame's decibels and not against
    // the scene's. A bin of this frame is one of 16384 across the span, so
    // the scene's -120 dBFS wideband noise arrives as a per-bin level far
    // below that: measured on this scene it reads -172.7 to -152.5 dB
    // between the two percentiles. A pin at -118, which is where the scene's
    // own number points, sits above the whole frame, and the answer is then
    // correct and uninteresting: the free ceiling is pushed to exactly the
    // minimum span above the pin, -106, and the case proves nothing about
    // tracking. -190 sits below the frame's body and above the kernel's -200
    // silence floor, which is where an operator pinning a known noise floor
    // would actually put it.
    constexpr float kPinnedFloor = -190.0F;
    constexpr dsp::SampleIndex kSamples = 300'000;

    auto config = spectrum_config();
    config.spectrum_floor_db = kPinnedFloor;

    const auto captured = capture_spectrum(config, tone_uri(0, kSamples));
    INFO(test::message_of(captured.run_status));
    REQUIRE(captured.run_status.has_value());
    REQUIRE(captured.frames.size() > 8);

    std::size_t moved = 0;
    float worst = 0.0F;
    for (const auto& frame : captured.frames) {
        const float error = std::abs(frame.floor_db - kPinnedFloor);
        worst = std::max(worst, error);
        if (error > 0.001F) {
            ++moved;
        }
    }

    const auto& sample = captured.frames[captured.frames.size() / 2];
    INFO(std::format("{} of {} frames moved the pinned floor, worst by {} dB; mid-run map is {} "
                     "to {} dB",
                     moved, captured.frames.size(), worst, sample.floor_db, sample.ceiling_db));

    CHECK(moved == 0);

    // And the other end is still automatic, which is the case a fixed noise
    // floor with live traffic wants. Two separate things: the ceiling is not
    // sitting at the minimum span above the pin, which is where it lands if
    // the pin has dragged the map with it, and it is tracking the frame's own
    // high percentile, which is the only evidence that it is following the
    // signal rather than following the pin.
    CHECK(sample.ceiling_db > kPinnedFloor + engine::kSpectrumMinimumSpanDb + 20.0F);
    CHECK(sample.ceiling_db == Approx(sample.percentile_high_db).margin(6.0));
}

TEST_CASE("a spectrum sink that fails stops the run and names itself",
          "[gpu][engine][spectrum][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The sink runs on the completion thread and a caller's failure there has
    // nowhere local to go. It has to come back out of run(), or a display
    // that lost its window keeps a capture running with nothing reading it.
    constexpr dsp::SampleIndex kSamples = 300'000;

    // Declared before the engine so that it outlives it: the sink holds a
    // reference and locals are destroyed in reverse order.
    std::atomic<std::uint64_t> calls{0};

    auto created = engine::Engine::create(spectrum_config());
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;

    REQUIRE(eng.open_source(tone_uri(0, kSamples)).has_value());

    REQUIRE(eng.set_spectrum_sink([&](const engine::SpectrumFrame&) -> Status {
                 calls.fetch_add(1, std::memory_order_relaxed);
                 return fail("the display went away");
             }).has_value());

    const auto ran = eng.run();
    INFO(std::format("the sink was called {} times", calls.load()));
    REQUIRE(calls.load() > 0);

    REQUIRE_FALSE(ran.has_value());
    INFO(ran.error().message);
    CHECK(ran.error().message.find("the display went away") != std::string::npos);
    CHECK(ran.error().message.find("spectrum sink") != std::string::npos);
}

TEST_CASE("replacing the spectrum sink does not restart the sequence",
          "[gpu][engine][spectrum][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // One sink for the engine, because there is one span, and setting a
    // second replaces the first. What matters beyond the replacement landing
    // is that the sequence keeps counting across it: those rows were
    // delivered, and a second sink that saw the counter restart would draw a
    // gap where the handover was.
    constexpr dsp::SampleIndex kSamples = 400'000;

    // All declared before the engine so that they outlive it: both sinks hold
    // references and locals are destroyed in reverse order.
    std::mutex lock;
    std::vector<std::uint64_t> first_sink;
    std::vector<std::uint64_t> second_sink;
    Status swap_status;
    bool swapped = false;

    engine::SpectrumSink second = [&](const engine::SpectrumFrame& frame) -> Status {
        const std::lock_guard<std::mutex> guard(lock);
        second_sink.push_back(frame.sequence);
        return {};
    };

    auto created = engine::Engine::create(spectrum_config());
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;

    REQUIRE(eng.open_source(tone_uri(0, kSamples)).has_value());

    REQUIRE(eng.set_spectrum_sink([&](const engine::SpectrumFrame& frame) -> Status {
                 const std::lock_guard<std::mutex> guard(lock);
                 first_sink.push_back(frame.sequence);
                 if (!swapped && first_sink.size() >= 4) {
                     swapped = true;
                     swap_status = eng.set_spectrum_sink(second);
                 }
                 return {};
             }).has_value());

    const auto ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    INFO(test::message_of(swap_status));
    CHECK(swap_status.has_value());

    INFO(std::format("{} frames to the first sink, {} to the second", first_sink.size(),
                     second_sink.size()));
    REQUIRE(first_sink.size() >= 4);
    REQUIRE(!second_sink.empty());

    // The handover is not instant: the sink is swapped through the control
    // queue the recording thread drains, so a few frames already recorded
    // still reach the first one. What must hold is that no frame went to both
    // and none was skipped between them.
    INFO(std::format("the first sink's last sequence is {}, the second sink's first is {}",
                     first_sink.back(), second_sink.front()));
    CHECK(second_sink.front() == first_sink.back() + 1);
    CHECK(second_sink.back() == second_sink.front() + second_sink.size() - 1);
}

TEST_CASE("a slow source gets the display rows it was asked for from overlapping windows",
          "[gpu][engine][spectrum][m2]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // EngineConfig::spectrum_rows_per_second, on the case the owner reported
    // after the playtest of 2026-09-23: a recording at a low rate played at
    // realtime drew about two rows a second, because a row comes per block
    // and 65536 samples of a 96 kS/s file is 0.68 s of it. At 30 rows a
    // second asked for, the block becomes 3200 samples, the largest multiple
    // of the 16-channel grid's decimation of 8 at or under 96000 / 30, and
    // every row's window is still the full transform, so consecutive rows
    // overlap rather than any row being made of less.
    //
    // REJECTS: a row rate that still follows the block, a smaller block that
    // shortens the window with it, rows that are not evenly spaced in the
    // stream, and a setting that changes a source already fast enough.
    constexpr dsp::SampleRate kSlowRate = 96'000;
    constexpr dsp::SampleIndex kSamples = 480'000;
    const std::string uri = "synthetic:wideband?rate=" + std::to_string(kSlowRate) +
                            "&emitters=1&modes=am&seed=424242&noise_dbfs=-120&snr_min=60"
                            "&snr_max=60&samples=" +
                            std::to_string(kSamples) + "&span_low=-3000&span_high=3000";

    engine::EngineConfig config = spectrum_config();
    config.channels = 16;
    config.block_samples = 65'536;
    config.spectrum_rows_per_second = 30.0;

    const auto asked = capture_spectrum(config, uri);
    INFO(test::message_of(asked.run_status));
    REQUIRE(asked.run_status.has_value());

    const dsp::SampleIndex window = static_cast<dsp::SampleIndex>(kTransform) *
                                    static_cast<dsp::SampleIndex>(asked.info.grid.decimation);
    INFO(std::format("block {}, {} frames in flight, window {} samples, {} frames",
                     asked.info.block_samples, asked.info.frames_in_flight, window,
                     asked.frames.size()));
    REQUIRE(asked.info.grid.decimation == 8);
    CHECK(asked.info.block_samples == 3'200);

    // Three frames' worth of the old block's time kept in hand, capped at
    // the graph's eight.
    CHECK(asked.info.frames_in_flight == 8);

    // Every row after the first is exactly one block on from the one before
    // and covers the whole transform, so the window overlaps its neighbour
    // by what the block shrank.
    REQUIRE(asked.frames.size() > 2);
    std::size_t uneven = 0;
    std::size_t short_window = 0;
    for (std::size_t i = 0; i < asked.frames.size(); ++i) {
        if (asked.frames[i].count != window) {
            ++short_window;
        }
        if (i > 0 && asked.frames[i].start - asked.frames[i - 1].start != 3'200) {
            ++uneven;
        }
    }
    CHECK(uneven == 0);
    CHECK(short_window == 0);
    CHECK(window > 3'200);

    // Rows per second of source: every block but the few whose window was
    // not yet whole, which is 30 a second.
    const double seconds = static_cast<double>(kSamples) / static_cast<double>(kSlowRate);
    const double rows = static_cast<double>(asked.frames.size()) / seconds;
    INFO(std::format("{:.2f} rows a second of source", rows));
    CHECK(rows >= 29.0);
    CHECK(rows <= 30.0);

    // Asked for none, the same source keeps its block and draws one row per
    // 0.68 s of it, which is what the owner saw.
    config.spectrum_rows_per_second = 0.0;
    const auto plain = capture_spectrum(config, uri);
    REQUIRE(plain.run_status.has_value());
    CHECK(plain.info.block_samples == 65'536);
    CHECK(plain.info.frames_in_flight == 3);
    CHECK(plain.frames.size() <= (kSamples + 65'535) / 65'536);

    // And a source already fast enough keeps its block whatever is asked.
    engine::EngineConfig fast = spectrum_config();
    fast.spectrum_rows_per_second = 30.0;
    const auto quick = capture_spectrum(fast, tone_uri(0, 400'000));
    REQUIRE(quick.run_status.has_value());
    CHECK(quick.info.block_samples == kBlockSamples);
    CHECK(quick.info.frames_in_flight == 3);
}
