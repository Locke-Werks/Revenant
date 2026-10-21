#include "core/dsp/carrier_tracker.h"

#include <algorithm>
#include <cmath>

namespace revenant::dsp {

namespace {

// An unconfirmed peak missing for longer than this is dropped. Short, so a
// speech harmonic that comes back elsewhere starts a fresh track instead of
// inheriting an old one's age.
constexpr double kCandidateGraceSeconds = 0.25;

// Median of the bins around k, the peak and its guard excluded. A median and
// not a mean, so a second carrier inside the window does not lift the floor.
float local_floor(std::span<const float> p, std::size_t k, std::size_t half,
                  std::size_t guard, std::span<float> scratch) {
    const std::size_t n = p.size();
    std::size_t used = 0;
    const std::size_t lo = k > half ? k - half : 0;
    const std::size_t hi = std::min(n - 1, k + half);
    for (std::size_t i = lo; i <= hi && used < scratch.size(); ++i) {
        const std::size_t d = i > k ? i - k : k - i;
        if (d > guard) {
            scratch[used++] = p[i];
        }
    }
    if (used == 0) {
        return p[k];
    }
    auto mid = scratch.begin() + static_cast<std::ptrdiff_t>(used / 2);
    std::nth_element(scratch.begin(), mid, scratch.begin() + static_cast<std::ptrdiff_t>(used));
    return *mid;
}

// Width where the peak falls 20 dB, or to 3 dB above the floor when the peak
// is not 23 dB clear of it: below that the edge would be found in the noise.
// Linear interpolation between the bins either side of each crossing. A walk
// longer than limit bins stops there, which the caller reads as wide.
float width_bins(std::span<const float> p, std::size_t k, float floor_db, float limit) {
    const float edge = std::max(p[k] - 20.0f, floor_db + 3.0f);
    const auto steps = static_cast<std::size_t>(std::ceil(limit)) + 1;

    float left = 0.0f;
    {
        std::size_t i = k;
        std::size_t walked = 0;
        while (i > 0 && p[i - 1] >= edge && walked < steps) {
            --i;
            ++walked;
        }
        if (i == 0 || walked >= steps) {
            left = static_cast<float>(walked) + 1.0f;
        } else {
            const float a = p[i];
            const float b = p[i - 1];
            left = static_cast<float>(walked) + (a - edge) / (a - b);
        }
    }
    float right = 0.0f;
    {
        std::size_t i = k;
        std::size_t walked = 0;
        while (i + 1 < p.size() && p[i + 1] >= edge && walked < steps) {
            ++i;
            ++walked;
        }
        if (i + 1 >= p.size() || walked >= steps) {
            right = static_cast<float>(walked) + 1.0f;
        } else {
            const float a = p[i];
            const float b = p[i + 1];
            right = static_cast<float>(walked) + (a - edge) / (a - b);
        }
    }
    return left + right;
}

// Fractional position of a local maximum from the parabola through it and
// its neighbours, in dB.
float parabolic_bin(std::span<const float> p, std::size_t k) {
    if (k == 0 || k + 1 >= p.size()) {
        return static_cast<float>(k);
    }
    const float a = p[k - 1];
    const float b = p[k];
    const float c = p[k + 1];
    const float den = a - 2.0f * b + c;
    if (den >= 0.0f) {
        return static_cast<float>(k);
    }
    const float d = 0.5f * (a - c) / den;
    return static_cast<float>(k) + std::clamp(d, -0.5f, 0.5f);
}

bool is_local_max(std::span<const float> p, std::size_t k) {
    const bool left = k == 0 || p[k] > p[k - 1];
    const bool right = k + 1 >= p.size() || p[k] >= p[k + 1];
    return left && right;
}

}  // namespace

CarrierTracker::CarrierTracker(std::size_t max_bins, CarrierTrackerConfig config)
    : config_(config), max_bins_(max_bins), scratch_(2 * config.floor_half_window + 1) {}

void CarrierTracker::reset() {
    tracks_ = {};
    report_count_ = 0;
}

std::size_t CarrierTracker::find_peaks(std::span<const float> p) {
    std::size_t count = 0;
    for (std::size_t k = 0; k < p.size(); ++k) {
        if (!is_local_max(p, k)) {
            continue;
        }
        const float floor_db =
            local_floor(p, k, config_.floor_half_window, config_.guard_bins, scratch_);
        const float level = p[k] - floor_db;
        if (level < config_.threshold_db) {
            continue;
        }
        const float width = width_bins(p, k, floor_db, config_.max_width_bins);
        if (width > config_.max_width_bins) {
            continue;
        }
        const Peak peak{parabolic_bin(p, k), level, width};
        if (count < kMaxPeaks) {
            peaks_[count++] = peak;
        } else {
            // Full: the weakest gives way, so a crowded band keeps its worst.
            auto weakest = std::min_element(
                peaks_.begin(), peaks_.end(),
                [](const Peak& x, const Peak& y) { return x.level_db < y.level_db; });
            if (weakest->level_db < peak.level_db) {
                *weakest = peak;
            }
        }
    }
    return count;
}

void CarrierTracker::update(std::span<const float> power_db, double bin_hz, double bin_zero_hz,
                            double dt_seconds) {
    if (power_db.size() > max_bins_) {
        power_db = power_db.first(max_bins_);
    }
    const std::size_t found = power_db.empty() ? 0 : find_peaks(power_db);

    for (Track& t : tracks_) {
        t.seen = false;
    }

    const float match = config_.drift_bins + 0.5f;
    for (std::size_t i = 0; i < found; ++i) {
        const Peak& peak = peaks_[i];
        Track* best = nullptr;
        float best_d = match;
        for (Track& t : tracks_) {
            if (!t.live || t.seen) {
                continue;
            }
            const float d = std::fabs(peak.bin - t.bin);
            if (d <= best_d) {
                best_d = d;
                best = &t;
            }
        }
        if (best != nullptr) {
            if (std::fabs(peak.bin - best->anchor_bin) > config_.drift_bins) {
                // It moved: whatever it is, it is not a fixed carrier yet.
                best->anchor_bin = peak.bin;
                best->age = 0.0;
                best->confirmed = false;
            } else {
                best->age += dt_seconds;
            }
            best->bin = peak.bin;
            best->level_db = peak.level_db;
            best->width_bins = peak.width_bins;
            best->unseen = 0.0;
            best->seen = true;
            continue;
        }

        Track* slot = nullptr;
        for (Track& t : tracks_) {
            if (!t.live) {
                slot = &t;
                break;
            }
        }
        if (slot == nullptr) {
            // No free slot: replace the youngest unconfirmed track.
            for (Track& t : tracks_) {
                if (!t.confirmed && !t.seen && (slot == nullptr || t.age < slot->age)) {
                    slot = &t;
                }
            }
        }
        if (slot == nullptr) {
            continue;
        }
        *slot = Track{true, false, peak.bin, peak.bin, peak.level_db, peak.width_bins,
                      0.0,  0.0,   true};
    }

    for (Track& t : tracks_) {
        if (!t.live) {
            continue;
        }
        if (!t.seen) {
            t.unseen += dt_seconds;
            const double limit = t.confirmed ? config_.hold_seconds : kCandidateGraceSeconds;
            if (t.unseen > limit) {
                t = Track{};
                continue;
            }
            if (t.confirmed) {
                t.age += dt_seconds;
            }
        }
        if (t.age >= config_.persistence_seconds) {
            t.confirmed = true;
        }
    }

    std::array<CarrierReport, kMaxTracks> all{};
    std::size_t n = 0;
    for (const Track& t : tracks_) {
        if (t.live && t.confirmed) {
            all[n++] = CarrierReport{bin_zero_hz + static_cast<double>(t.bin) * bin_hz,
                                     static_cast<double>(t.width_bins) * bin_hz, t.level_db,
                                     static_cast<float>(t.age)};
        }
    }
    // Ties broken on offset so the order never depends on slot order.
    std::sort(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(n),
              [](const CarrierReport& a, const CarrierReport& b) {
                  if (a.level_db != b.level_db) {
                      return a.level_db > b.level_db;
                  }
                  return a.offset_hz < b.offset_hz;
              });
    report_count_ = std::min(n, kMaxReports);
    std::copy_n(all.begin(), report_count_, reports_.begin());
}

PeakFit find_nearest_peak(std::span<const float> power_db, double bin_hz, double bin_zero_hz,
                          double clicked_offset_hz, std::span<float> scratch,
                          std::size_t search_bins, float threshold_db,
                          std::size_t floor_half_window, std::size_t guard_bins) {
    PeakFit fit;
    if (power_db.empty() || bin_hz <= 0.0 || scratch.empty()) {
        return fit;
    }
    const double pos = (clicked_offset_hz - bin_zero_hz) / bin_hz;
    const auto last = static_cast<double>(power_db.size() - 1);
    const auto start = static_cast<std::size_t>(std::lround(std::clamp(pos, 0.0, last)));

    // The nearest local maximum by distance, the higher one on a tie, so a
    // click beside a carrier lands on it rather than on a noise ripple
    // further in: of the maxima within search_bins, take the strongest.
    const std::size_t lo = start > search_bins ? start - search_bins : 0;
    const std::size_t hi = std::min(power_db.size() - 1, start + search_bins);
    std::size_t k = start;
    bool any = false;
    for (std::size_t i = lo; i <= hi; ++i) {
        if (is_local_max(power_db, i) && (!any || power_db[i] > power_db[k])) {
            k = i;
            any = true;
        }
    }
    if (!any) {
        return fit;
    }
    const float floor_db =
        local_floor(power_db, k, floor_half_window, guard_bins, scratch);
    const float level = power_db[k] - floor_db;
    if (level < threshold_db) {
        return fit;
    }
    const float width =
        width_bins(power_db, k, floor_db, static_cast<float>(2 * floor_half_window));
    fit.found = true;
    fit.offset_hz = bin_zero_hz + static_cast<double>(parabolic_bin(power_db, k)) * bin_hz;
    fit.width_hz = static_cast<double>(width) * bin_hz;
    fit.level_db = level;
    return fit;
}

}  // namespace revenant::dsp
