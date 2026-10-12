// detect_comb from core/dsp/comb_cancel.h, on spectra built bin by bin.
//
// Each spectrum is what a second of averaging produces: a floor whose bins are
// gamma distributed with shape K (K frames averaged, each bin exponential),
// lines drawn with a Hann window's response at a fractional bin position so
// the parabolic centre has something real to interpolate, and whatever else a
// case needs on top. The comb's c and d are chosen, so every assertion is
// against arithmetic rather than against another measurement.
//
// Randomness is seeded and the seed is printed with any failure.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <random>
#include <vector>

#include "core/dsp/comb_cancel.h"

using namespace revenant;
using dsp::CombEstimate;
using dsp::CombSpectrum;
using dsp::detect_comb;

namespace {

constexpr std::size_t kBins = 16384;
constexpr dsp::SampleRate kRate = 2'400'000;
constexpr double kHzPerBin = static_cast<double>(kRate) / static_cast<double>(kBins);

// A Hann window's power response at u bins from a line, 1 at u = 0.
[[nodiscard]] double hann_power(double u)
{
    const auto dirichlet = [](double v) {
        if (std::abs(v) < 1e-9) {
            return 1.0;
        }
        return std::sin(std::numbers::pi * v) / (std::numbers::pi * v);
    };
    const double w = 0.5 * dirichlet(u) + 0.25 * (dirichlet(u - 1.0) + dirichlet(u + 1.0));
    return (w * w) / 0.25;
}

class Scene {
public:
    explicit Scene(std::uint64_t seed, double frames = 140.0)
        : generator_(seed), frames_(frames), mean_(kBins, 1.0)
    {
    }

    // A line at `x` bins standing `height_db` above the floor's mean.
    void line(double x, double height_db)
    {
        const double a = std::pow(10.0, height_db / 10.0);
        const auto centre = static_cast<std::int64_t>(std::round(x));
        for (std::int64_t i = centre - 40; i <= centre + 40; ++i) {
            if (i >= 0 && i < static_cast<std::int64_t>(kBins)) {
                mean_[static_cast<std::size_t>(i)] += a * hann_power(static_cast<double>(i) - x);
            }
        }
    }

    // A wideband signal: a raised plateau with a gently rippled top.
    void plateau(std::size_t start, std::size_t width, double height_db)
    {
        std::uniform_real_distribution<double> ripple(-1.0, 1.0);
        for (std::size_t i = start; i < start + width && i < kBins; ++i) {
            const double db = height_db + ripple(generator_);
            mean_[i] += std::pow(10.0, db / 10.0);
        }
    }

    // A comb c + k d over the whole span, heights uniform in [lo_db, hi_db],
    // each line kept with probability `keep`. Returns the lines drawn.
    std::uint32_t comb(double c, double d, double lo_db, double hi_db, double keep = 1.0)
    {
        std::uniform_real_distribution<double> height(lo_db, hi_db);
        std::bernoulli_distribution present(keep);
        std::uint32_t drawn = 0;
        const auto k_lo = static_cast<std::int64_t>(std::ceil((2.0 - c) / d));
        const auto k_hi = static_cast<std::int64_t>(std::floor((kBins - 3.0 - c) / d));
        for (std::int64_t k = k_lo; k <= k_hi; ++k) {
            const double h = height(generator_);
            if (present(generator_)) {
                line(c + static_cast<double>(k) * d, h);
                ++drawn;
            }
        }
        return drawn;
    }

    std::mt19937_64& generator() { return generator_; }

    [[nodiscard]] std::vector<float> render()
    {
        std::gamma_distribution<double> gamma(frames_, 1.0 / frames_);
        std::vector<float> power(kBins);
        for (std::size_t i = 0; i < kBins; ++i) {
            power[i] = static_cast<float>(mean_[i] * gamma(generator_));
        }
        return power;
    }

private:
    std::mt19937_64 generator_;
    double frames_;
    std::vector<double> mean_;
};

[[nodiscard]] std::optional<CombEstimate> run(const std::vector<float>& power)
{
    auto result = detect_comb(CombSpectrum{power, kRate});
    REQUIRE(result.has_value());
    return *result;
}

// The comb's line nearest DC, in hertz, for a comb drawn at c + k d bins.
[[nodiscard]] double line_nearest_dc(double c, double d)
{
    const double dc = static_cast<double>(kBins / 2);
    const double k = std::round((dc - c) / d);
    return (c + k * d - dc) * kHzPerBin;
}

// Spacing to 1 Hz and the k = 0 line to a tenth of a bin, 15 Hz. With 0.1 bin
// centres over a hundred-odd lines the least-squares spacing is good to a few
// thousandths of a bin; 1 Hz is a hundredth.
void check(const CombEstimate& found, double c, double d)
{
    INFO("found offset " << found.offset_hz << " Hz spacing " << found.spacing_hz << " Hz, k "
                         << found.k_lo << ".." << found.k_hi << ", " << found.lines_found
                         << " lines, median " << found.median_height_db << " dB");
    CHECK(std::abs(found.spacing_hz - d * kHzPerBin) < 1.0);
    CHECK(std::abs(found.offset_hz - line_nearest_dc(c, d)) < 0.1 * kHzPerBin);
    CHECK(std::abs(found.offset_hz) <= 0.5 * found.spacing_hz + 1.0);
    CHECK(found.k_lo < 0);
    CHECK(found.k_hi > 0);
}

}  // namespace

TEST_CASE("comb detection recovers c and d of an HF switching comb", "[comb][detect]")
{
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        INFO("seed " << seed);
        Scene scene(seed);
        // 14 kHz is 95.57 bins; c at a fraction of a bin.
        const double d = 14'000.0 / kHzPerBin;
        const double c = 37.31 + 0.11 * static_cast<double>(seed);
        const std::uint32_t drawn = scene.comb(c, d, 4.0, 15.0);
        const auto found = run(scene.render());
        REQUIRE(found.has_value());
        check(*found, c, d);
        CHECK(found->lines_found >= drawn * 8 / 10);
        CHECK(found->line_count() <= drawn);
        CHECK(found->median_height_db > 6.0F);
        CHECK(found->median_height_db < 13.0F);
    }
}

TEST_CASE("comb detection keeps one comb across gaps", "[comb][detect]")
{
    for (std::uint64_t seed = 100; seed < 108; ++seed) {
        INFO("seed " << seed);
        Scene scene(seed);
        const double d = 61.37;
        const double c = 12.6;
        scene.comb(c, d, 6.0, 15.0, 0.6);
        const auto found = run(scene.render());
        REQUIRE(found.has_value());
        check(*found, c, d);
        CHECK(found->lines_found < found->line_count());
    }
}

TEST_CASE("comb detection is not fooled by carriers and wideband signals", "[comb][detect]")
{
    for (std::uint64_t seed = 200; seed < 208; ++seed) {
        INFO("seed " << seed);
        Scene scene(seed);
        const double d = 14'000.0 / kHzPerBin;
        const double c = 50.42;
        scene.comb(c, d, 4.0, 15.0, 0.9);

        // Broadcast carriers, much stronger than the comb, at random places,
        // and four sitting exactly on the grid.
        std::uniform_real_distribution<double> where(10.0, kBins - 10.0);
        std::uniform_real_distribution<double> strength(20.0, 35.0);
        for (int i = 0; i < 30; ++i) {
            scene.line(where(scene.generator()), strength(scene.generator()));
        }
        for (const int k : {3, 40, 77, 150}) {
            scene.line(c + k * d, 30.0);
        }
        // Stations 10 kHz wide, 10 to 25 dB up.
        std::uniform_int_distribution<std::size_t> start(0, kBins - 80);
        for (int i = 0; i < 8; ++i) {
            scene.plateau(start(scene.generator()), 68, 10.0 + 2.0 * i);
        }

        const auto found = run(scene.render());
        REQUIRE(found.has_value());
        check(*found, c, d);
    }
}

TEST_CASE("comb detection picks d, not d / 2 or 2 d", "[comb][detect]")
{
    SECTION("alternate lines weak picks d, not 2 d")
    {
        for (std::uint64_t seed = 300; seed < 306; ++seed) {
            INFO("seed " << seed);
            Scene scene(seed);
            const double d = 40.25;
            const double c = 7.7;
            for (int k = -300; k < 300; ++k) {
                const double x = c + k * d + 8192.0;
                if (x > 3.0 && x < kBins - 3.0) {
                    scene.line(x, (k % 2 == 0) ? 15.0 : 6.0);
                }
            }
            const auto found = run(scene.render());
            REQUIRE(found.has_value());
            check(*found, c + 8192.0, d);
        }
    }
    SECTION("strays at the midpoints pick d, not d / 2")
    {
        for (std::uint64_t seed = 310; seed < 316; ++seed) {
            INFO("seed " << seed);
            Scene scene(seed);
            const double d = 80.6;
            const double c = 31.2;
            scene.comb(c, d, 6.0, 15.0);
            for (const int k : {5, 30, 61, 90, 120, 150, 180}) {
                scene.line(c + (k + 0.5) * d, 10.0);
            }
            const auto found = run(scene.render());
            REQUIRE(found.has_value());
            check(*found, c, d);
        }
    }
}

TEST_CASE("comb detection resolves a comb four bins apart and refuses one closer",
          "[comb][detect]")
{
    for (std::uint64_t seed = 400; seed < 404; ++seed) {
        INFO("seed " << seed);
        {
            Scene scene(seed);
            const double d = 4.6;
            const double c = 1.3;
            scene.comb(c, d, 8.0, 15.0);
            const auto found = run(scene.render());
            REQUIRE(found.has_value());
            check(*found, c, d);
        }
        {
            Scene scene(seed);
            scene.comb(0.9, 2.5, 8.0, 15.0);
            const auto found = run(scene.render());
            CHECK_FALSE(found.has_value());
        }
    }
}

TEST_CASE("comb detection finds nothing where there is no comb", "[comb][detect]")
{
    SECTION("carriers and stations, no comb")
    {
        for (std::uint64_t seed = 500; seed < 520; ++seed) {
            INFO("seed " << seed);
            Scene scene(seed);
            std::uniform_real_distribution<double> where(10.0, kBins - 10.0);
            std::uniform_real_distribution<double> strength(5.0, 35.0);
            for (int i = 0; i < 40; ++i) {
                scene.line(where(scene.generator()), strength(scene.generator()));
            }
            std::uniform_int_distribution<std::size_t> start(0, kBins - 80);
            for (int i = 0; i < 10; ++i) {
                scene.plateau(start(scene.generator()), 68, 8.0 + 2.0 * i);
            }
            CHECK_FALSE(run(scene.render()).has_value());
        }
    }
    SECTION("averaged noise alone, many seeds")
    {
        for (std::uint64_t seed = 1000; seed < 1200; ++seed) {
            INFO("seed " << seed);
            Scene scene(seed);
            CHECK_FALSE(run(scene.render()).has_value());
        }
    }
    SECTION("a single unaveraged frame of noise")
    {
        for (std::uint64_t seed = 2000; seed < 2050; ++seed) {
            INFO("seed " << seed);
            Scene scene(seed, 1.0);
            CHECK_FALSE(run(scene.render()).has_value());
        }
    }
    SECTION("an all-zero spectrum")
    {
        const std::vector<float> zero(kBins, 0.0F);
        CHECK_FALSE(run(zero).has_value());
    }
}

TEST_CASE("comb detection refuses a spectrum it cannot read", "[comb][detect]")
{
    const std::vector<float> ok(kBins, 1.0F);
    CHECK_FALSE(detect_comb(CombSpectrum{{}, kRate}).has_value());
    CHECK_FALSE(detect_comb(CombSpectrum{ok, 0}).has_value());
    CHECK_FALSE(detect_comb(CombSpectrum{ok, -5}).has_value());
    const std::vector<float> short_one(32, 1.0F);
    CHECK_FALSE(detect_comb(CombSpectrum{short_one, kRate}).has_value());
    std::vector<float> bad = ok;
    bad[17] = std::nanf("");
    CHECK_FALSE(detect_comb(CombSpectrum{bad, kRate}).has_value());
    bad[17] = -1.0F;
    CHECK_FALSE(detect_comb(CombSpectrum{bad, kRate}).has_value());
    bad[17] = INFINITY;
    CHECK_FALSE(detect_comb(CombSpectrum{bad, kRate}).has_value());
}
