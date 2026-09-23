// Resampler: accuracy in the band it passes, depth in the band it stops, and
// the copy it makes when there is nothing to do.
//
// Each case names the wrong implementation it rejects. The figures asserted
// are bounds with margin under what was measured on 2026-09-23, and each
// case prints what it measured so a regression reads as a number.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "audio/resampler.h"

using revenant::ui::Resampler;

namespace {

// A tone's samples at `rate`, from index 0.
[[nodiscard]] std::vector<float> tone(double hz, std::uint32_t rate, std::size_t frames,
                                      double amplitude = 0.5)
{
    std::vector<float> out(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        out[n] = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz *
                                                         static_cast<double>(n) /
                                                         static_cast<double>(rate)));
    }
    return out;
}

struct Measured {
    double worst_error = 0.0;
    double peak = 0.0;
};

// Resamples `input` and compares each output frame with the tone evaluated
// exactly at the position it was read from. Frames within a kernel of either
// end are left out, because the kernel reads zeros there.
[[nodiscard]] Measured against_tone(const Resampler& resampler, const std::vector<float>& input,
                                    double hz, std::uint32_t in_rate, double amplitude,
                                    bool expect_tone)
{
    Measured out;
    const double margin = static_cast<double>(resampler.half_width()) + 2.0;
    for (double p = margin; p < static_cast<double>(input.size()) - margin;
         p += resampler.step()) {
        float y = 0.0F;
        resampler.evaluate(input.data(), 0, input.size(), 1, p, &y);
        const double ideal =
            expect_tone ? amplitude * std::sin(2.0 * std::numbers::pi * hz * p /
                                               static_cast<double>(in_rate))
                        : 0.0;
        out.worst_error = std::max(out.worst_error, std::abs(static_cast<double>(y) - ideal));
        out.peak = std::max(out.peak, std::abs(static_cast<double>(y)));
    }
    return out;
}

}  // namespace

TEST_CASE("a tone in the passband survives 171000 to 48000 to 80 dB", "[audio][resampler]")
{
    // REJECTS: a linear interpolator, which is what a mix written in a hurry
    // reaches for. It droops at the top of the band and has no stopband, so
    // it folds the 57 kHz subcarrier straight into the audio.
    Resampler resampler;
    resampler.configure(171'000, 48'000);
    CHECK_FALSE(resampler.passthrough());
    CHECK(resampler.half_width() == 112);

    for (const double hz : {1'000.0, 15'000.0, 20'000.0}) {
        const auto input = tone(hz, 171'000, 20'000);
        const Measured measured = against_tone(resampler, input, hz, 171'000, 0.5, true);
        INFO(hz << " Hz: worst error " << measured.worst_error);
        CHECK(measured.worst_error < 2e-4);
    }
}

TEST_CASE("the stopband starts at the lower Nyquist and is 80 dB down", "[audio][resampler]")
{
    // REJECTS: a cutoff placed at the INPUT's Nyquist when going down, which
    // is the anti-image filter's placement mistaken for the anti-alias one.
    // 30 kHz at 171000 would then fold to 18 kHz at 48000 at full level.
    Resampler resampler;
    resampler.configure(171'000, 48'000);
    for (const double hz : {24'000.0, 30'000.0, 57'000.0}) {
        const auto input = tone(hz, 171'000, 20'000);
        const Measured measured = against_tone(resampler, input, hz, 171'000, 0.5, false);
        INFO(hz << " Hz: peak " << measured.peak << " against 0.5 in");
        CHECK(measured.peak < 0.5 * 1e-4 * 2.0);
    }
}

TEST_CASE("up from the vocoder's 8000 to 48000 is accurate and images are gone",
          "[audio][resampler]")
{
    // REJECTS: sample-and-hold, which repeats each P25 sample six times and
    // leaves images of the voice around 8, 16 and 24 kHz.
    Resampler resampler;
    resampler.configure(8'000, 48'000);
    CHECK(resampler.half_width() == 32);
    CHECK(resampler.step() == 1.0 / 6.0);

    const auto input = tone(1'000.0, 8'000, 4'000);
    const Measured measured = against_tone(resampler, input, 1'000.0, 8'000, 0.5, true);
    INFO("worst error " << measured.worst_error);
    CHECK(measured.worst_error < 2e-4);
}

TEST_CASE("equal rates copy the samples bit for bit", "[audio][resampler]")
{
    // REJECTS: running the kernel at a ratio of one, which rounds every
    // sample through a filter and takes the ordinary 48000 S/s receiver on a
    // 48000 S/s card off bit-exactness for nothing.
    Resampler resampler;
    resampler.configure(48'000, 48'000);
    CHECK(resampler.passthrough());
    CHECK(resampler.half_width() == 0);

    const std::vector<float> input = {0.1F, -0.25F, 1.5F, 3.0e-7F};
    for (std::size_t n = 0; n < input.size(); ++n) {
        float y = -1.0F;
        resampler.evaluate(input.data(), 100, input.size(), 1, 100.0 + static_cast<double>(n),
                           &y);
        CHECK(y == input[n]);
    }
}

TEST_CASE("a lower cutoff asked for is the one delivered", "[audio][resampler]")
{
    // REJECTS: ignoring the caller's cutoff for the default one, which on a
    // wfm multiplex lets the 19 kHz pilot through at its transmitted level.
    Resampler resampler;
    resampler.configure(171'000, 48'000, 17'000.0, 4'000.0);

    const auto pass = against_tone(resampler, tone(15'000.0, 171'000, 20'000), 15'000.0,
                                   171'000, 0.5, true);
    const auto pilot = against_tone(resampler, tone(19'000.0, 171'000, 20'000), 19'000.0,
                                    171'000, 0.5, false);
    INFO("15 kHz worst error " << pass.worst_error << ", 19 kHz peak " << pilot.peak);
    CHECK(pass.worst_error < 2e-4);
    CHECK(pilot.peak < 1e-4);
}

TEST_CASE("stereo frames are resampled channel by channel", "[audio][resampler]")
{
    // REJECTS: treating interleaved stereo as one stream at twice the rate,
    // which swaps a phase of L into R.
    Resampler resampler;
    resampler.configure(96'000, 48'000);
    const auto left = tone(1'000.0, 96'000, 4'000, 0.5);
    const auto right = tone(3'000.0, 96'000, 4'000, 0.25);
    std::vector<float> both(left.size() * 2);
    for (std::size_t n = 0; n < left.size(); ++n) {
        both[2 * n] = left[n];
        both[2 * n + 1] = right[n];
    }
    double worst = 0.0;
    const double margin = static_cast<double>(resampler.half_width()) + 2.0;
    for (double p = margin; p < static_cast<double>(left.size()) - margin; p += 2.0) {
        float y[2] = {};
        resampler.evaluate(both.data(), 0, left.size(), 2, p, y);
        const double l = 0.5 * std::sin(2.0 * std::numbers::pi * 1'000.0 * p / 96'000.0);
        const double r = 0.25 * std::sin(2.0 * std::numbers::pi * 3'000.0 * p / 96'000.0);
        worst = std::max({worst, std::abs(static_cast<double>(y[0]) - l),
                          std::abs(static_cast<double>(y[1]) - r)});
    }
    INFO("worst error " << worst);
    CHECK(worst < 2e-4);
}
