// Stationary narrow carriers in a receiver's passband power, tracked over time.
//
// The interference tracker (#9) wants the carriers that sit still: a birdie, a
// switching supply's line, a neighbour's unmodulated carrier. Those are what a
// notch removes for good. Speech harmonics and a drifting oscillator are narrow
// too, but they move, so a carrier is only reported once its peak has stayed
// within a bin of where it was first seen for the persistence time.
//
// The input is a PassbandFrame's power_db with its geometry reduced to two
// numbers: the bin width and the offset of bin zero, both in hertz relative to
// whatever the fine stage mixed to DC. Offsets come out in the same frame.
//
// Everything is sized at construction. update() reads the frame, writes into
// fixed arrays and allocates nothing, so it can run on the display path.
//
// Written from published mathematics only, per docs/clean-room.md: local
// median floors and parabolic peak interpolation are textbook spectral
// estimation.

#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace revenant::dsp {

struct CarrierTrackerConfig {
    // How far a peak must stand above the local floor to count, in dB.
    float threshold_db = 12.0f;

    // Widest -20 dB width, in bins, still called narrow.
    float max_width_bins = 4.0f;

    // How far the peak may wander from where it was first seen, in bins,
    // before it is treated as a new peak and its age restarts.
    float drift_bins = 1.0f;

    // Seconds a peak must stay put before it is reported.
    float persistence_seconds = 3.0f;

    // Seconds a reported carrier survives without being seen.
    float hold_seconds = 2.0f;

    // Bins either side of a peak used for its local floor. The peak itself and
    // guard_bins either side of it are left out of the median.
    std::size_t floor_half_window = 32;
    std::size_t guard_bins = 3;
};

struct CarrierReport {
    double offset_hz = 0.0;  // relative to the passband centre (fine stage DC)
    double width_hz = 0.0;   // at -20 dB, or at the floor if that is higher
    float level_db = 0.0f;   // peak above local floor
    float age_seconds = 0.0f;
};

struct PeakFit {
    bool found = false;
    double offset_hz = 0.0;
    double width_hz = 0.0;
    float level_db = 0.0f;  // peak above local floor
};

class CarrierTracker {
public:
    static constexpr std::size_t kMaxReports = 8;

    // max_bins bounds the frames update() accepts; larger frames are cut.
    explicit CarrierTracker(std::size_t max_bins, CarrierTrackerConfig config = {});

    // One frame. dt_seconds is the time since the previous frame.
    void update(std::span<const float> power_db, double bin_hz, double bin_zero_hz,
                double dt_seconds);

    // Confirmed carriers, strongest first. Valid until the next update().
    [[nodiscard]] std::span<const CarrierReport> carriers() const {
        return {reports_.data(), report_count_};
    }

    void reset();

    [[nodiscard]] const CarrierTrackerConfig& config() const { return config_; }

private:
    struct Peak {
        float bin = 0.0f;  // fractional, parabolic
        float level_db = 0.0f;
        float width_bins = 0.0f;
    };

    struct Track {
        bool live = false;
        bool confirmed = false;
        float anchor_bin = 0.0f;
        float bin = 0.0f;
        float level_db = 0.0f;
        float width_bins = 0.0f;
        double age = 0.0;
        double unseen = 0.0;
        bool seen = false;
    };

    static constexpr std::size_t kMaxPeaks = 32;
    static constexpr std::size_t kMaxTracks = 24;

    std::size_t find_peaks(std::span<const float> power_db);

    CarrierTrackerConfig config_;
    std::size_t max_bins_;
    std::vector<float> scratch_;
    std::array<Peak, kMaxPeaks> peaks_{};
    std::array<Track, kMaxTracks> tracks_{};
    std::array<CarrierReport, kMaxReports> reports_{};
    std::size_t report_count_ = 0;
};

// The peak nearest clicked_offset_hz in one frame, and its -20 dB width, for
// click-to-notch (#1). The search climbs from the clicked bin to the local
// maximum within search_bins; found is false if nothing there stands
// threshold_db above the local floor. scratch must hold at least
// 2 * floor_half_window bins and is overwritten; passing it keeps the call
// allocation-free.
[[nodiscard]] PeakFit find_nearest_peak(std::span<const float> power_db, double bin_hz,
                                        double bin_zero_hz, double clicked_offset_hz,
                                        std::span<float> scratch,
                                        std::size_t search_bins = 8,
                                        float threshold_db = 6.0f,
                                        std::size_t floor_half_window = 32,
                                        std::size_t guard_bins = 3);

}  // namespace revenant::dsp
