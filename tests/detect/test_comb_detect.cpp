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
#include <cstdio>
#include <cstdlib>
#include <format>
#include <numbers>
#include <optional>
#include <random>
#include <string>
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
    CHECK_FALSE(dsp::detect_combs(CombSpectrum{bad, kRate}).has_value());
}

namespace {

[[nodiscard]] std::vector<CombEstimate> run_all(const std::vector<float>& power)
{
    auto result = dsp::detect_combs(CombSpectrum{power, kRate});
    REQUIRE(result.has_value());
    return *result;
}

// Whether `found` lies on the comb c + k d bins with spacing within `hz`.
[[nodiscard]] bool on_comb(const CombEstimate& found, double c, double d, double hz)
{
    const double spacing = d * kHzPerBin;
    const double dc = static_cast<double>(kBins / 2);
    const double offset = (c - dc) * kHzPerBin;
    const double k = std::round((found.offset_hz - offset) / spacing);
    return std::abs(found.spacing_hz - spacing) < hz &&
           std::abs(found.offset_hz - (offset + k * spacing)) < 0.2 * kHzPerBin;
}

}  // namespace

// The owner's HF comb, 2026-10-12: two switchers near 31.58 kHz, a few hertz
// apart in spacing, whose lines interleave to look like one comb at 15.79 kHz.
// Each is reported as its own comb, at its own spacing.
TEST_CASE("comb detections separate two interleaved switchers", "[comb][detect]")
{
    for (std::uint64_t seed = 300; seed < 308; ++seed) {
        INFO("seed " << seed);
        Scene scene(seed);
        const double d_a = 31'574.0 / kHzPerBin;
        const double d_b = 31'579.5 / kHzPerBin;
        const double c_a = 41.3 + 0.07 * static_cast<double>(seed - 300);
        const double c_b = c_a + 15'790.0 / kHzPerBin;
        scene.comb(c_a, d_a, 9.0, 14.0);
        scene.comb(c_b, d_b, 5.0, 9.0);
        const std::vector<CombEstimate> found = run_all(scene.render());
        std::string seen;
        for (const CombEstimate& f : found) {
            seen += std::format("[offset {:.1f} spacing {:.2f} k {}..{} lines {}] ", f.offset_hz,
                                f.spacing_hz, f.k_lo, f.k_hi, f.lines_found);
        }
        INFO("found " << seen);
        REQUIRE(found.size() >= 2);
        // Strongest first.
        CHECK(on_comb(found[0], c_a, d_a, 2.0));
        CHECK(on_comb(found[1], c_b, d_b, 2.0));
        CHECK(found[0].lines_found >= 50);
        CHECK(found[1].lines_found >= 50);
    }
}

// A comb with both halves populated is held as the two halves, each at twice
// the spacing: an exact model of every other line, and two families that sit
// on each other's half grid for a while each get a tracker of their own.
TEST_CASE("comb detections hold a single comb as its two exact halves", "[comb][detect]")
{
    for (std::uint64_t seed = 400; seed < 408; ++seed) {
        INFO("seed " << seed);
        Scene scene(seed);
        const double d = 14'000.0 / kHzPerBin;
        const double c = 37.31 + 0.11 * static_cast<double>(seed - 400);
        const std::uint32_t drawn = scene.comb(c, d, 6.0, 15.0);
        const std::vector<CombEstimate> found = run_all(scene.render());
        REQUIRE(found.size() == 2);
        const bool first_even = on_comb(found[0], c, 2.0 * d, 1.0);
        const CombEstimate& even = first_even ? found[0] : found[1];
        const CombEstimate& odd = first_even ? found[1] : found[0];
        CHECK(on_comb(even, c, 2.0 * d, 1.0));
        CHECK(on_comb(odd, c + d, 2.0 * d, 1.0));
        CHECK(even.lines_found + odd.lines_found >= drawn * 8 / 10);
    }
}

// Every line carrying a ladder of ripple sidebands, as on the owner's 80 m
// comb: the comb comes back with the ripple and the orders to cancel, and the
// sidebands do not come back as combs of their own.
TEST_CASE("comb detections report a comb's ripple sidebands with it", "[comb][detect]")
{
    for (std::uint64_t seed = 800; seed < 804; ++seed) {
        INFO("seed " << seed);
        Scene scene(seed);
        const double d = 31'578.9 / kHzPerBin;
        const double c = 45.2 + 0.13 * static_cast<double>(seed - 800);
        const double ripple = 3.4;  // bins: 498 Hz at 146.5 Hz a bin
        const double levels[] = {-12.0, -16.0};
        const auto k_lo = static_cast<std::int64_t>(std::ceil((20.0 - c) / d));
        const auto k_hi = static_cast<std::int64_t>(std::floor((kBins - 20.0 - c) / d));
        for (std::int64_t k = k_lo; k <= k_hi; ++k) {
            const double x = c + static_cast<double>(k) * d;
            scene.line(x, 22.0);
            for (int m = 1; m <= 2; ++m) {
                scene.line(x + m * ripple, 22.0 + levels[m - 1]);
                scene.line(x - m * ripple, 22.0 + levels[m - 1]);
            }
        }
        const std::vector<CombEstimate> found = run_all(scene.render());
        std::string seen;
        for (const CombEstimate& f : found) {
            seen += std::format("[offset {:.1f} spacing {:.2f} ripple {:.1f} orders {}] ", f.offset_hz,
                                f.spacing_hz, f.ripple_hz, f.sideband_orders);
        }
        INFO("found " << seen);
        // The comb, as one grid or its halves, every estimate on it.
        REQUIRE_FALSE(found.empty());
        for (const CombEstimate& f : found) {
            CHECK((on_comb(f, c, d, 2.0) || on_comb(f, c, 2.0 * d, 2.0) ||
                   on_comb(f, c + d, 2.0 * d, 2.0)));
            CHECK(std::abs(f.ripple_hz - ripple * kHzPerBin) < 0.3 * kHzPerBin);
            CHECK(f.sideband_orders > 0);
        }
    }
}

// Two sources at one spacing, the second offset a hundred hertz from half of
// it, as on the owner's HF dumps: the joint grid at half the spacing misplaces
// every other line by that much, which is the test that splits it.
TEST_CASE("comb detections separate interleaved combs offset from half a spacing",
          "[comb][detect]")
{
    for (std::uint64_t seed = 700; seed < 706; ++seed) {
        INFO("seed " << seed);
        Scene scene(seed);
        const double d = 31'578.9 / kHzPerBin;
        const double c_a = 45.2 + 0.13 * static_cast<double>(seed - 700);
        const double c_b = c_a + (15'789.45 + 100.0) / kHzPerBin;
        scene.comb(c_a, d, 9.0, 14.0);
        scene.comb(c_b, d, 8.0, 13.0);
        const std::vector<CombEstimate> found = run_all(scene.render());
        bool a = false;
        bool b = false;
        for (const CombEstimate& f : found) {
            a = a || on_comb(f, c_a, d, 2.0);
            b = b || on_comb(f, c_b, d, 2.0);
        }
        CHECK(a);
        CHECK(b);
    }
}

TEST_CASE("comb detections find an unrelated comb beside the first", "[comb][detect]")
{
    for (std::uint64_t seed = 500; seed < 504; ++seed) {
        INFO("seed " << seed);
        Scene scene(seed);
        const double d_a = 14'000.0 / kHzPerBin;
        const double d_b = 22'930.0 / kHzPerBin;
        scene.comb(37.31, d_a, 10.0, 15.0);
        scene.comb(80.9, d_b, 6.0, 9.0);
        const std::vector<CombEstimate> found = run_all(scene.render());
        // Each comb, whole or as its two halves.
        auto held = [&](double c, double d) {
            bool whole = false;
            bool even = false;
            bool odd = false;
            for (const CombEstimate& f : found) {
                whole = whole || on_comb(f, c, d, 1.0);
                even = even || on_comb(f, c, 2.0 * d, 1.0);
                odd = odd || on_comb(f, c + d, 2.0 * d, 1.0);
            }
            return whole || (even && odd);
        };
        CHECK(held(37.31, d_a));
        CHECK(held(80.9, d_b));
        CHECK(found.size() <= dsp::kCombMaxCombs);
    }
}

// Spectra dumped by the engine under REVENANT_COMB_DUMP (core/engine/graph.cpp),
// named <prefix>.<n>.<rate>.f32, replayed through detect_combs and printed.
// REVENANT_COMB_SPECTRA lists the files, separated by semicolons. Unset, the
// case does nothing: it is for looking at a real capture, not a regression.
TEST_CASE("comb detections on dumped spectra", "[comb][detect]")
{
    const char* list = std::getenv("REVENANT_COMB_SPECTRA");
    if (list == nullptr) {
        return;
    }
    std::string rest = list;
    while (!rest.empty()) {
        const std::size_t cut = rest.find(';');
        const std::string path = rest.substr(0, cut);
        rest = cut == std::string::npos ? std::string{} : rest.substr(cut + 1);
        if (path.empty()) {
            continue;
        }
        // The rate is the last dotted field before the extension.
        const std::size_t ext = path.rfind('.');
        const std::size_t dot = path.rfind('.', ext - 1);
        REQUIRE(ext != std::string::npos);
        REQUIRE(dot != std::string::npos);
        const auto rate = static_cast<dsp::SampleRate>(std::stoll(path.substr(dot + 1, ext - dot - 1)));
        std::FILE* file = std::fopen(path.c_str(), "rb");
        REQUIRE(file != nullptr);
        std::vector<float> power;
        float value = 0.0F;
        while (std::fread(&value, sizeof value, 1, file) == 1) {
            power.push_back(value);
        }
        std::fclose(file);
        auto found = dsp::detect_combs(CombSpectrum{power, rate});
        REQUIRE(found.has_value());
        std::printf("%s: %zu bins, %zu combs\n", path.c_str(), power.size(), found->size());
        for (const CombEstimate& f : *found) {
            std::printf("  offset %10.1f Hz  spacing %9.2f Hz  k %4d..%-4d  %3u lines  median %.1f dB\n",
                        f.offset_hz, f.spacing_hz, f.k_lo, f.k_hi, f.lines_found,
                        static_cast<double>(f.median_height_db));
        }
    }
}

TEST_CASE("comb detections find nothing where there is no comb", "[comb][detect]")
{
    Scene scene(600);
    CHECK(run_all(scene.render()).empty());
    const std::vector<float> zero(kBins, 0.0F);
    CHECK(run_all(zero).empty());
}
