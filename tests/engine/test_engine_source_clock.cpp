// AudioChunk::source_end through a real engine: a receiver's audio mapped
// back onto the source's sample index, the index the waterfall and the
// detector are drawn on.
//
// THE CAPTURE is complex baseband rendered here and played through the
// engine's file source, so the instant a sound starts is known to the sample.
// It is silence and then a tone keyed on hard at kOnset, 1 kHz above a USB
// receiver's carrier, so the receiver's audio is silence and then a 1 kHz
// sine. Every filter between the two is linear phase, so the half-amplitude
// point of the audio's envelope is the onset delayed by exactly the filters'
// group delay, and mapping that point back with source_end has to land that
// far after kOnset and no further.
//
// WHAT IS MEASURED, and printed so the figure on AudioChunk::source_end can
// be checked against it:
//
//   lag       mapped onset minus true onset, the receiver's own filter delay
//             that source_end deliberately does not take off
//   predicted the same delay from the receiver's plan, the fine filter's and
//             the audio filter's group delays added
//   spread    how far source_end wanders against chunk.start over the run,
//             which is the per-chunk placement error a consumer sees
//
// The bounds are loose where a filter design could move them, for the reason
// tests/decode/CMakeLists.txt gives: a figure asserted to a decimal fails the
// day somebody improves the filter.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <mutex>
#include <numbers>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "core/dsp/vrx_reference.h"
#include "core/engine/engine.h"
#include "core/engine/vrx.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/support/temp_path.h"

using namespace revenant;

namespace {

// The AGC cases' rate and grid, which are known to carry a USB receiver: a
// 72 kHz channel spacing on 16 channels.
constexpr dsp::SampleRate kRate = 1'152'000;
constexpr std::uint32_t kChannels = 16;

// Two coarse channels above the centre, away from DC where the front-end
// correction works, and on a channel centre so the receiver reads one
// channel and not the edge between two.
constexpr dsp::Hertz kCarrier = 144'000;
constexpr dsp::Hertz kToneOffset = 1'000;

// Just over a second in, and deliberately not a multiple of any block,
// decimation or audio period, so a mapping that is right only on a block
// boundary cannot pass by landing on one.
constexpr dsp::SampleIndex kOnset = 1'152'000 + 4'321;
constexpr double kSeconds = 1.6;

// -20 dBFS: far above anything the empty band produces, far below clipping.
constexpr double kAmplitude = 0.1;

// Small, so a run has a few hundred chunks and the cross-chunk checks have
// something to bite on; the spectrum stage's own case uses the same size.
constexpr std::size_t kBlockSamples = 8'192;
constexpr std::uint32_t kTransform = 512;

[[nodiscard]] std::vector<dsp::Complex32> render() {
    const auto count = static_cast<std::size_t>(kSeconds * static_cast<double>(kRate));
    std::vector<dsp::Complex32> out(count, dsp::Complex32{0.0F, 0.0F});
    const dsp::Hertz tone = kCarrier + kToneOffset;
    for (std::size_t n = static_cast<std::size_t>(kOnset); n < count; ++n) {
        // The phase reduced exactly in integers, as the AGC cases do, so the
        // tone does not drift in phase over a second and a half of float
        // time.
        const std::int64_t turns = (tone * static_cast<std::int64_t>(n)) % kRate;
        const double angle =
            2.0 * std::numbers::pi * static_cast<double>(turns) / static_cast<double>(kRate);
        out[n] = dsp::Complex32{static_cast<float>(kAmplitude * std::cos(angle)),
                                static_cast<float>(kAmplitude * std::sin(angle))};
    }
    return out;
}

// What one chunk said about itself. The samples are kept separately.
struct ChunkRecord {
    dsp::SampleIndex start = 0;
    std::size_t frames = 0;
    dsp::SampleRate rate = 0;
    dsp::SampleIndex source_end = 0;
};

}  // namespace

TEST_CASE("source_end places a receiver's audio on the source clock",
          "[gpu][engine][source-clock]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    const std::vector<dsp::Complex32> capture = render();
    const std::filesystem::path path =
        test::unique_temp_path("revenant_test_source_clock", ".cf32");
    {
        std::FILE* file = std::fopen(path.string().c_str(), "wb");
        REQUIRE(file != nullptr);
        const std::size_t wrote =
            std::fwrite(capture.data(), sizeof(dsp::Complex32), capture.size(), file);
        std::fclose(file);
        REQUIRE(wrote == capture.size());
    }
    struct Remove {
        std::filesystem::path path;
        ~Remove() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } remove{path};

    engine::EngineConfig config;
    config.channels = kChannels;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = kBlockSamples;
    config.spectrum_transform = kTransform;
    config.gpu_index = -1;  // honours REVENANT_GPU_INDEX

    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;
    const std::string uri = "file:///" + path.generic_string() + "?rate=" +
                            std::to_string(kRate) + "&format=cf32&center=7100000";
    const auto opened = eng.open_source(uri);
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());
    const engine::EngineInfo info = eng.info();
    REQUIRE(info.source_rate == kRate);

    engine::VrxParams params;
    params.center = kCarrier;
    params.demod = engine::Demod::Usb;
    params.bandwidth = 0;
    const auto id = eng.add_vrx(params);
    INFO(test::message_of(id));
    REQUIRE(id.has_value());

    std::mutex lock;
    std::vector<ChunkRecord> chunks;
    std::vector<float> audio;
    std::uint32_t channels = 1;
    bool gap = false;
    REQUIRE(eng.attach_audio_sink(*id, [&](const engine::AudioChunk& chunk) -> Status {
                   const std::lock_guard<std::mutex> guard(lock);
                   channels = std::max<std::uint32_t>(chunk.channels, 1);
                   const std::size_t frames = chunk.samples.size() / channels;
                   if (!chunks.empty() &&
                       chunk.start != chunks.back().start + chunks.back().frames) {
                       gap = true;
                   }
                   chunks.push_back({chunk.start, frames, chunk.rate, chunk.source_end});
                   for (std::size_t i = 0; i < frames; ++i) {
                       audio.push_back(chunk.samples[i * channels]);
                   }
                   return {};
               })
                .has_value());

    // The end of every waterfall row, which source_end claims to share an
    // index with.
    std::vector<dsp::SampleIndex> spectrum_ends;
    REQUIRE(eng.set_spectrum_sink([&](const engine::SpectrumFrame& frame) -> Status {
                   const std::lock_guard<std::mutex> guard(lock);
                   spectrum_ends.push_back(frame.start + frame.count);
                   return {};
               })
                .has_value());

    const auto ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    const std::lock_guard<std::mutex> guard(lock);
    REQUIRE(chunks.size() > 100);
    REQUIRE_FALSE(gap);
    REQUIRE(channels == 1);
    const dsp::SampleRate audio_rate = chunks.front().rate;
    REQUIRE(audio_rate > 0);

    // Source samples per audio frame. Exact for this receiver, 24, and kept
    // as a double so the mapping below is the one the field's comment gives
    // a consumer rather than one that only works when the ratio is whole.
    const double ratio = static_cast<double>(kRate) / static_cast<double>(audio_rate);

    // Every chunk carries a source end, strictly increasing, in step with its start.
    {
        double low = 0.0;
        double high = 0.0;
        bool increasing = true;
        bool known = true;
        for (std::size_t k = 0; k < chunks.size(); ++k) {
            const ChunkRecord& c = chunks[k];
            known = known && c.source_end != 0;
            if (k > 0 && c.source_end <= chunks[k - 1].source_end) {
                increasing = false;
            }
            // Where the chunk's end sits on the source clock, less where its
            // end sits on the receiver's clock carried over at the rate
            // ratio. Constant if the two clocks are in step; the spread is
            // what one chunk's placement can be off by against another's.
            const double offset =
                static_cast<double>(c.source_end) -
                static_cast<double>(c.start + c.frames) * ratio;
            if (k == 0) {
                low = high = offset;
            }
            low = std::min(low, offset);
            high = std::max(high, offset);
        }
        WARN(std::format("{} chunks at {} Hz; source_end less (start + frames) * {:.3f} spans "
                         "{:.1f} source samples ({:.1f} us)",
                         chunks.size(), audio_rate, ratio, high - low,
                         (high - low) * 1e6 / static_cast<double>(kRate)));
        CHECK(known);
        CHECK(increasing);

        // The receiver writes whole fine and audio samples, so the last
        // frame of a dispatch falls up to one audio period short of the
        // block that ended it, and how far short changes from dispatch to
        // dispatch. Two audio periods is that with room for the fine
        // stage's own rounding; anything beyond it is a chunk placed
        // against the wrong dispatch.
        CHECK(high - low <= 2.0 * ratio);
    }

    // A waterfall row ends on the same index as the audio of its dispatch.
    {
        const std::set<dsp::SampleIndex> ends = [&] {
            std::set<dsp::SampleIndex> out;
            for (const ChunkRecord& c : chunks) {
                out.insert(c.source_end);
            }
            return out;
        }();
        REQUIRE_FALSE(spectrum_ends.empty());
        std::size_t compared = 0;
        std::size_t matched = 0;
        for (const dsp::SampleIndex end : spectrum_ends) {
            if (end < chunks.front().source_end) {
                continue;
            }
            ++compared;
            if (ends.contains(end)) {
                ++matched;
            }
        }
        INFO(compared << " rows compared, " << matched << " on a chunk's source_end");
        CHECK(compared > 50);
        CHECK(matched == compared);
    }

    // A keyed tone maps back to where it was keyed, late by the receiver's filters.
    {
        // A quadrature envelope: a quarter period of a 1 kHz tone apart, the
        // squares of a sine sum to its amplitude squared whatever its phase,
        // so the envelope is flat in steady state and rises once at the
        // onset rather than following the cycle.
        REQUIRE(audio_rate % 4'000 == 0);
        const std::size_t quarter = static_cast<std::size_t>(audio_rate / 4'000);
        std::vector<double> envelope(audio.size(), 0.0);
        for (std::size_t n = quarter; n < audio.size(); ++n) {
            const double a = audio[n];
            const double b = audio[n - quarter];
            envelope[n] = std::sqrt(a * a + b * b);
        }

        // The settled level from the last third of the tone, which is long
        // after every filter has filled.
        const std::size_t total = audio.size();
        REQUIRE(total > 3 * quarter);
        std::vector<double> settled(envelope.begin() + static_cast<std::ptrdiff_t>(total * 5 / 6),
                                    envelope.end());
        std::nth_element(settled.begin(), settled.begin() + settled.size() / 2, settled.end());
        const double level = settled[settled.size() / 2];
        INFO("settled envelope " << level);
        REQUIRE(level > 0.0);

        std::size_t crossed = 0;
        for (std::size_t n = quarter + 1; n < total; ++n) {
            if (envelope[n] >= 0.5 * level) {
                crossed = n;
                break;
            }
        }
        REQUIRE(crossed > quarter + 1);

        // Interpolated between the two frames either side of the half
        // level, then taken back by half the estimator's own span: it reads
        // a frame and the frame a quarter period earlier, so on a rising
        // edge it reports the level of an instant between the two.
        const double below = envelope[crossed - 1];
        const double above = envelope[crossed];
        const double fraction = above > below ? (0.5 * level - below) / (above - below) : 0.0;
        const double onset_frame = static_cast<double>(crossed - 1) + fraction -
                                   0.5 * static_cast<double>(quarter);

        // The chunk that frame was delivered in, and the field's own mapping
        // applied to it.
        const auto holding = std::find_if(chunks.begin(), chunks.end(), [&](const ChunkRecord& c) {
            return onset_frame < static_cast<double>(c.start + c.frames);
        });
        REQUIRE(holding != chunks.end());
        const double i = onset_frame - static_cast<double>(holding->start);
        const double mapped = static_cast<double>(holding->source_end) -
                              (static_cast<double>(holding->frames) - i) * ratio;
        const double lag_ms = (mapped - static_cast<double>(kOnset)) * 1e3 /
                              static_cast<double>(kRate);

        // The same delay from the receiver's plan: the fine filter's group
        // delay at the channel rate and the audio filter's at the
        // demodulation rate, which is everything between a channel sample
        // and an audio frame for a USB receiver.
        auto placement = engine::place(info.grid, kRate, params);
        INFO(test::message_of(placement));
        REQUIRE(placement.has_value());
        auto plan = dsp::plan_vrx(info.grid, kRate, params, *placement);
        INFO(test::message_of(plan));
        REQUIRE(plan.has_value());
        const double predicted_ms =
            1e3 * (plan->fine_group_delay_channel_samples /
                       static_cast<double>(plan->channel_rate) +
                   plan->audio_group_delay_demod_samples / static_cast<double>(plan->demod_rate));

        WARN(std::format("onset keyed at source sample {}, heard at audio frame {:.2f}, mapped "
                         "to source sample {:.1f}: {:.3f} ms late; the plan's fine and audio "
                         "group delays add to {:.3f} ms",
                         kOnset, onset_frame, mapped, lag_ms, predicted_ms));

        // Never early: nothing in a causal receiver hears a sound before it
        // is on the air, so a mapping that lands before kOnset by more than
        // the measurement's own resolution has placed the chunk wrongly.
        // A fortieth of a millisecond is one audio period.
        CHECK(lag_ms > -0.05);

        // Late by the filters and not by a block: a dispatch here is 8192
        // source samples, 7.1 ms, so a source_end one dispatch out would
        // move this by that much and fail the agreement with the plan.
        CHECK(lag_ms < 10.0);
        CHECK(std::abs(lag_ms - predicted_ms) < 0.5);
    }
}
