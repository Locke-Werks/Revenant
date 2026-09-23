// Runs an engine to the end of its source with a spectrum sink attached and
// keeps the average of its last frames, for the calibration cases.
//
// The average is of linear power, bin by bin, over the frames whose window
// starts after `settle_samples`, so the start-up of anything that converges is
// left out. It is the reading a person would take off a waterfall that has
// been left to settle, with the frame-to-frame noise averaged down.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "core/dsp/types.h"
#include "core/engine/engine.h"
#include "core/error.h"

namespace revenant::test {

struct AveragedSpectrum {
    Status run_status;
    engine::SpectrumGeometry geometry{};
    std::vector<double> power;  // linear, per bin
    std::size_t frames = 0;

    // The bin whose centre is nearest `offset_hz` from baseband DC.
    [[nodiscard]] std::size_t bin_of(double offset_hz) const {
        const double at = (offset_hz - geometry.bin_zero_hz()) / geometry.bin_width_hz();
        const double rounded = std::round(at);
        if (rounded < 0.0) {
            return 0;
        }
        return std::min(static_cast<std::size_t>(rounded), power.size() - 1);
    }

    [[nodiscard]] double offset_of(std::size_t bin) const {
        return geometry.bin_zero_hz() + static_cast<double>(bin) * geometry.bin_width_hz();
    }

    [[nodiscard]] double db_at(std::size_t bin) const {
        return 10.0 * std::log10(std::max(power[bin], 1.0e-30));
    }

    // The strongest bin within `half_width` bins of `bin`, which is where a
    // tone that does not sit on a bin centre puts its power.
    [[nodiscard]] double peak_db_near(std::size_t bin, std::size_t half_width) const {
        const std::size_t low = bin > half_width ? bin - half_width : 0;
        const std::size_t high = std::min(bin + half_width, power.size() - 1);
        double best = 0.0;
        for (std::size_t k = low; k <= high; ++k) {
            best = std::max(best, power[k]);
        }
        return 10.0 * std::log10(std::max(best, 1.0e-30));
    }

    [[nodiscard]] std::size_t peak_bin() const {
        std::size_t best = 0;
        for (std::size_t k = 1; k < power.size(); ++k) {
            if (power[k] > power[best]) {
                best = k;
            }
        }
        return best;
    }
};

// `engine` has a source open. The mutex is declared before the result it
// guards and the engine is the caller's, which outlives this call, so the sink
// is detached before anything it touches goes away.
[[nodiscard]] inline AveragedSpectrum average_spectrum(engine::Engine& engine,
                                                       dsp::SampleIndex settle_samples) {
    AveragedSpectrum out;
    std::mutex lock;
    out.run_status = engine.set_spectrum_sink([&](const engine::SpectrumFrame& frame) -> Status {
        const std::scoped_lock held(lock);
        if (frame.start < settle_samples) {
            return {};
        }
        if (out.power.empty()) {
            out.geometry = frame.geometry;
            out.power.assign(frame.power_db.size(), 0.0);
        }
        if (frame.power_db.size() != out.power.size()) {
            return {};
        }
        for (std::size_t k = 0; k < frame.power_db.size(); ++k) {
            out.power[k] += std::pow(10.0, static_cast<double>(frame.power_db[k]) / 10.0);
        }
        ++out.frames;
        return {};
    });
    if (!out.run_status) {
        return out;
    }
    out.run_status = engine.run();
    (void)engine.set_spectrum_sink(nullptr);

    const std::scoped_lock held(lock);
    if (out.frames > 0) {
        for (double& value : out.power) {
            value /= static_cast<double>(out.frames);
        }
    }
    return out;
}

}  // namespace revenant::test
