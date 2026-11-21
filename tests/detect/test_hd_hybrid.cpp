// Hybrid digital sideband identification, core/detect/hd_hybrid.h.
//
// Two kinds of case. The synthetic ones build an averaged spectrum and a floor
// by hand, so each rule in the header is checked against a shape whose answer
// is known by construction. The recorded ones run real RTL-SDR captures of the
// local FM band through the detector itself and check the label a client
// would print: a hybrid station must read "FM + HD hybrid" and an analogue
// only station must not.
//
// THE RECORDINGS ARE NOT IN THE REPOSITORY (tests/corpus/README.md says why).
// Set REVENANT_FM_HD_DIR to the directory holding them; without it the
// recorded cases skip, loudly. Each is raw cu8 at 2.4 MS/s, two seconds,
// taken 2026-11-20 at 30 dB gain:
//
//   hd-102.7.cu8      centre 102.1 MHz, hybrid station at 102.7
//   hd-90.5.cu8       centre  91.1 MHz, hybrid station at 90.5
//   fmonly-98.1.cu8   centre  97.7 MHz, analogue-only station at 98.1

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

#include "core/detect/detector.h"
#include "core/detect/hd_hybrid.h"
#include "core/detect/label.h"
#include "core/engine/engine.h"

using namespace revenant;

namespace {

constexpr double kBinWidth = 1000.0;
constexpr std::size_t kBins = 1024;
constexpr double kBinZero = -512'000.0;

// A frame with a flat floor of 1, an analogue FM hump of the given density
// over +/-100 kHz falling off as a skirt, and optional blocks on each side.
struct Spectrum {
    std::vector<double> power = std::vector<double>(kBins, 1.0);
    std::vector<double> floor = std::vector<double>(kBins, 1.0);

    [[nodiscard]] std::size_t bin(double hz) const {
        return static_cast<std::size_t>(std::llround((hz - kBinZero) / kBinWidth));
    }
    void add(double low_hz, double high_hz, double density) {
        for (std::size_t i = bin(low_hz); i <= bin(high_hz); ++i) {
            power[i] += density;
        }
    }
    void analogue(double centre, double density) {
        for (std::size_t i = 0; i < kBins; ++i) {
            const double off = std::abs(kBinZero + static_cast<double>(i) * kBinWidth - centre);
            // Flat to 75 kHz, then 1 dB per kHz: a skirt, which tilts.
            power[i] += off <= 75'000.0 ? density : density * std::pow(10.0, -(off - 75'000.0) / 10'000.0);
        }
    }
    [[nodiscard]] detect::HdSidebandReading read(double centre) const {
        return detect::measure_hd_sidebands(power, floor, kBinZero, kBinWidth, centre);
    }
};

}  // namespace

TEST_CASE("symmetric blocks beside an FM signal read as hybrid at their level", "[hd]") {
    Spectrum s;
    s.analogue(0.0, 1000.0);
    s.add(-198'000.0, -129'000.0, 10.0);
    s.add(129'000.0, 198'000.0, 10.0);
    const auto r = s.read(0.0);
    REQUIRE(r.measured);
    CHECK(r.hybrid);
    // 10 per bin over 70 bins against about 1000 over the analogue's ~170.
    CHECK(r.lower_dbc < -20.0);
    CHECK(r.lower_dbc > -26.0);
    CHECK(std::abs(r.lower_dbc - r.upper_dbc) < 0.5);
}

TEST_CASE("an analogue FM signal alone is not hybrid", "[hd]") {
    Spectrum s;
    s.analogue(0.0, 1000.0);
    const auto r = s.read(0.0);
    REQUIRE(r.measured);
    CHECK_FALSE(r.hybrid);
}

TEST_CASE("one block alone is a neighbour, not a hybrid", "[hd]") {
    Spectrum s;
    s.analogue(0.0, 1000.0);
    s.add(129'000.0, 198'000.0, 10.0);
    CHECK_FALSE(s.read(0.0).hybrid);
}

TEST_CASE("blocks at the floor do not count", "[hd]") {
    Spectrum s;
    s.analogue(0.0, 1000.0);
    s.add(-198'000.0, -129'000.0, 0.5);
    s.add(129'000.0, 198'000.0, 0.5);
    CHECK_FALSE(s.read(0.0).hybrid);
}

TEST_CASE("a tracker centre off by one block is walked back to the carrier", "[hd]") {
    Spectrum s;
    s.analogue(0.0, 1000.0);
    s.add(-198'000.0, -129'000.0, 10.0);
    s.add(129'000.0, 198'000.0, 10.0);
    const auto r = s.read(60'000.0);
    REQUIRE(r.measured);
    CHECK(std::abs(r.centre_hz) < 5'000.0);
    CHECK(r.hybrid);
}

TEST_CASE("a band off the edge of the frame is not measured", "[hd]") {
    Spectrum s;
    s.analogue(-450'000.0, 1000.0);
    CHECK_FALSE(s.read(-450'000.0).measured);
}

TEST_CASE("a hybrid track's WFM label is renamed and carries its levels", "[hd]") {
    detect::Track track;
    track.classification = detect::Classification::AnalogueFm;
    track.classification_confidence = 0.5;
    track.bandwidth = 200'000;
    CHECK(detect::label_track(track).name == "WFM");
    track.hd_hybrid = true;
    track.hd_lower_dbc = -20.0;
    track.hd_upper_dbc = -21.0;
    const detect::TrackLabel label = detect::label_track(track);
    CHECK(label.name == "FM + HD hybrid");
    CHECK(label.hd_hybrid);
    CHECK(label.hd_lower_dbc == -20.0);
    // A narrow FM track is never hybrid, whatever the flag says.
    track.bandwidth = 16'000;
    CHECK(detect::label_track(track).name == "NFM");
}

// ---------------------------------------------------------------------------
// Recorded
// ---------------------------------------------------------------------------

namespace {

constexpr dsp::SampleRate kRate = 2'400'000;
constexpr std::uint32_t kChannels = 16;
constexpr std::uint32_t kTransform = 256;
constexpr std::size_t kGridBins = kChannels * (kTransform / 2);  // 2048
constexpr std::size_t kFrameStep = 8192;

[[nodiscard]] engine::SpectrumGeometry geometry() {
    engine::SpectrumGeometry g;
    g.transform = kTransform;
    g.bins_per_channel = kTransform / 2;
    g.channels = kChannels;
    g.bins = static_cast<std::uint32_t>(kGridBins);
    g.bin_width_numerator = kRate;
    g.bin_width_denominator = static_cast<std::int64_t>(kChannels / 2) * kTransform;
    g.bin_zero_numerator = -static_cast<std::int64_t>(kRate) * (kChannels + 1);
    g.bin_zero_denominator = 2 * static_cast<std::int64_t>(kChannels);
    return g;
}

void fft(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * std::numbers::pi / static_cast<double>(len);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

struct Run {
    bool found = false;
    detect::TrackLabel label;
    dsp::Hertz centre = 0;
};

// Plays a capture through the detector, one frame per kFrameStep samples,
// each frame the mean of four 2048-point periodograms, and returns the label
// of the widest track within 60 kHz of the station.
[[nodiscard]] Run run_capture(const std::filesystem::path& path, dsp::Hertz capture_centre,
                              dsp::Hertz station) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)), {});
    const std::size_t samples = raw.size() / 2;

    detect::DetectorConfig config;
    config.source_rate = kRate;
    config.source_center = capture_centre;
    config.grid_channels = kChannels;
    const engine::SpectrumGeometry g = geometry();
    auto made = detect::Detector::create(config, g);
    REQUIRE(made.has_value());
    detect::Detector& detector = *made;

    // Bin k of the grid is frequency bin_zero + k * width, which is FFT
    // index (k + offset) modulo the transform.
    const auto offset = static_cast<std::ptrdiff_t>(
        std::llround(-g.bin_zero_hz() / g.bin_width_hz()));
    std::vector<double> window(kGridBins);
    for (std::size_t i = 0; i < kGridBins; ++i) {
        window[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                          static_cast<double>(kGridBins));
    }
    std::vector<float> power_db(kGridBins);
    std::vector<double> accum(kGridBins);
    std::vector<std::complex<double>> buf(kGridBins);
    std::uint64_t sequence = 0;
    for (std::size_t start = 0; start + kFrameStep <= samples; start += kFrameStep) {
        std::fill(accum.begin(), accum.end(), 0.0);
        for (std::size_t seg = 0; seg < kFrameStep / kGridBins; ++seg) {
            for (std::size_t i = 0; i < kGridBins; ++i) {
                const std::size_t s = start + seg * kGridBins + i;
                buf[i] = {(raw[2 * s] - 127.5) * window[i] / 128.0,
                          (raw[2 * s + 1] - 127.5) * window[i] / 128.0};
            }
            fft(buf);
            for (std::size_t i = 0; i < kGridBins; ++i) {
                accum[i] += std::norm(buf[i]);
            }
        }
        for (std::size_t k = 0; k < kGridBins; ++k) {
            const auto idx = static_cast<std::size_t>(
                (static_cast<std::ptrdiff_t>(k) - offset + static_cast<std::ptrdiff_t>(kGridBins)) %
                static_cast<std::ptrdiff_t>(kGridBins));
            power_db[k] = static_cast<float>(10.0 * std::log10(accum[idx] / 4.0 + 1e-20));
        }
        engine::SpectrumFrame frame;
        frame.power_db = power_db;
        frame.geometry = g;
        frame.start = start;
        frame.count = kFrameStep;
        frame.sequence = sequence++;
        REQUIRE(detector.consume(frame).has_value());
    }

    {
        const auto r = detect::measure_hd_sidebands(
            detector.averaged_power(), detector.noise_floor(), g.bin_zero_hz(), g.bin_width_hz(),
            static_cast<double>(station - capture_centre));
        UNSCOPED_INFO("at the station: floor " << r.lower_over_floor_db << " / "
                      << r.upper_over_floor_db << " dB, gap " << r.lower_over_gap_db << " / "
                      << r.upper_over_gap_db << " dB, tilt " << r.lower_tilt_db << " / "
                      << r.upper_tilt_db << " dB, " << r.lower_dbc << " / " << r.upper_dbc
                      << " dBc");
    }
    Run out;
    for (const detect::Track& track : detector.tracks()) {
        if (std::abs(track.center - station) > 60'000) {
            continue;
        }
        // Stand in for the tier-two probe, which needs the GPU: the station
        // is wideband FM, so the label is what a probe naming it would give.
        detect::Track probed = track;
        probed.classification = detect::Classification::AnalogueFm;
        probed.classification_confidence = 0.5;
        const detect::TrackLabel label = detect::label_track(probed);
        if (!out.found || label.hd_hybrid) {
            out = Run{true, label, track.center};
        }
        UNSCOPED_INFO("track " << track.id << " at " << track.center << " width "
                               << track.bandwidth << " hd " << track.hd_hybrid << " "
                               << track.hd_lower_dbc << " / " << track.hd_upper_dbc << " dBc");
    }
    return out;
}

[[nodiscard]] std::filesystem::path recording(const char* name) {
    const char* dir = std::getenv("REVENANT_FM_HD_DIR");
    if (dir == nullptr) {
        return {};
    }
    std::filesystem::path p = std::filesystem::path(dir) / name;
    return std::filesystem::exists(p) ? p : std::filesystem::path{};
}

}  // namespace

TEST_CASE("recorded hybrid and analogue-only FM stations", "[hd][recorded]") {
    const auto hd1 = recording("hd-102.7.cu8");
    const auto fm = recording("fmonly-98.1.cu8");
    if (hd1.empty() || fm.empty()) {
        SKIP("REVENANT_FM_HD_DIR unset or recordings missing; real-recording check not run");
    }

    const Run a = run_capture(hd1, 102'100'000, 102'700'000);
    REQUIRE(a.found);
    CHECK(a.label.name == "FM + HD hybrid");
    std::printf("102.7: %s, sidebands %.1f / %.1f dBc\n", std::string(a.label.name).c_str(),
                a.label.hd_lower_dbc, a.label.hd_upper_dbc);

    // 90.5 is reported and not asserted. Against the detector's own floor,
    // which the crowded band raises, its upper block stood only 2.1 dB clear
    // against 10.2 dB for the lower on this antenna, so it fails the
    // over-floor and symmetry rules. The thresholds were not loosened to make
    // a weak, lopsided reception pass.
    if (const auto hd2 = recording("hd-90.5.cu8"); !hd2.empty()) {
        const Run b = run_capture(hd2, 91'100'000, 90'500'000);
        std::printf(" 90.5: %s (not asserted)\n", std::string(b.label.name).c_str());
    }

    const Run c = run_capture(fm, 97'700'000, 98'100'000);
    REQUIRE(c.found);
    CHECK(c.label.name == "WFM");
    std::printf(" 98.1: %s\n", std::string(c.label.name).c_str());
}
