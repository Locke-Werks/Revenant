#include "core/dsp/noise_profile.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace revenant::dsp {

namespace {

// Per-frame smoothing of each bin's power. 0.7 at a 10.7 ms hop is a time
// constant near 30 ms, short enough to follow a syllable's onset.
constexpr double kPowerSmooth = 0.7;

// The quiet average's time constant. Long, because it is noise.
constexpr double kQuietTauS = 0.5;

// Minimum statistics over this long, in this many sub-windows, so the minimum
// can let go of a level within a quarter second of the window passing it.
constexpr double kMinWindowS = 2.0;
constexpr std::uint32_t kMinSubWindows = 8;

// The minimum of a smoothed exponential sits below its mean. 1.5 was read off
// white noise through this window and smoothing; a steady tone has no spread
// and is over-estimated by the same factor, which only floors it harder.
constexpr double kMinBias = 1.5;

// Gain smoothing across frames. Without it a noise bin's gain flickers
// between the floor and one frame to frame, which is musical noise.
constexpr double kGainSmooth = 0.5;

constexpr double kLearnS = 1.0;

void fft(std::span<std::complex<double>> x, std::span<const std::complex<double>> twiddles) {
    const std::size_t n = x.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(x[i], x[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const std::size_t step = n / len;
        for (std::size_t i = 0; i < n; i += len) {
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> t = twiddles[k * step] * x[i + k + len / 2];
                x[i + k + len / 2] = x[i + k] - t;
                x[i + k] += t;
            }
        }
    }
}

}  // namespace

std::span<const double> NoiseProfile::profile() const {
    if (!have_noise_) {
        return {};
    }
    return noise_;
}

void NoiseProfile::rebuild(double rate) {
    rate_ = rate;
    size_ = rate >= 32'000.0 ? 1024u : (rate >= 16'000.0 ? 512u : 256u);
    hop_ = size_ / 2;
    window_.resize(size_);
    for (std::uint32_t n = 0; n < size_; ++n) {
        window_[n] = std::sqrt(0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * n / size_)));
    }
    twiddles_.resize(size_ / 2);
    for (std::uint32_t k = 0; k < size_ / 2; ++k) {
        twiddles_[k] = std::polar(1.0, -2.0 * std::numbers::pi * k / size_);
    }
    bins_.assign(size_, {});
    const std::uint32_t half = size_ / 2 + 1;
    learned_.assign(half, 0.0);
    learn_acc_.assign(half, 0.0);
    have_learned_ = false;
    learn_left_ = 0;
    clear_state();
}

void NoiseProfile::clear_state() {
    const std::uint32_t half = size_ / 2 + 1;
    in_.assign(size_, 0.0);
    ola_.assign(size_, 0.0);
    out_.assign(hop_, 0.0);
    in_fill_ = size_ - hop_;
    out_pos_ = 0;
    smoothed_.assign(half, 0.0);
    quiet_avg_.assign(half, 0.0);
    have_quiet_ = false;
    sub_min_.assign(half, std::numeric_limits<double>::infinity());
    mins_.assign(static_cast<std::size_t>(half) * kMinSubWindows,
                 std::numeric_limits<double>::infinity());
    sub_frames_ = 0;
    sub_index_ = 0;
    frames_seen_ = 0;
    gain_.assign(half, 1.0);
    noise_.assign(half, 0.0);
    have_noise_ = false;
}

void NoiseProfile::retune() {
    if (size_ == 0) {
        return;
    }
    clear_state();
}

void NoiseProfile::process(std::span<float> audio, double rate, bool quiet,
                           const NoiseProfileSettings& settings) {
    if (rate <= 0.0) {
        return;
    }
    if (rate != rate_) {
        rebuild(rate);
    }

    // The first settings seen are the baseline, so a receiver created with a
    // count already moved does not learn or reset on its first block.
    if (!counts_seen_) {
        counts_seen_ = true;
        learn_seen_ = settings.learn_count;
        reset_seen_ = settings.reset_count;
    }
    if (settings.reset_count != reset_seen_) {
        reset_seen_ = settings.reset_count;
        have_learned_ = false;
        learn_left_ = 0;
        clear_state();
    }
    if (settings.learn_count != learn_seen_) {
        learn_seen_ = settings.learn_count;
        std::fill(learn_acc_.begin(), learn_acc_.end(), 0.0);
        learn_total_ = std::max<std::uint32_t>(
            1, static_cast<std::uint32_t>(std::ceil(kLearnS * rate / hop_)));
        learn_left_ = learn_total_;
    }

    if (!settings.enabled) {
        // Off costs a compare. The stream restarts clean when it comes back,
        // so a stale half frame is not played out.
        if (enabled_last_) {
            enabled_last_ = false;
            clear_state();
        }
        return;
    }
    enabled_last_ = true;

    const double strength =
        std::clamp(settings.strength_db, 0.0, kNoiseProfileMaxStrengthDb);
    const double floor = std::pow(10.0, -strength / 20.0);

    for (float& sample : audio) {
        in_[in_fill_++] = static_cast<double>(sample);
        sample = static_cast<float>(out_[out_pos_++]);
        if (in_fill_ == size_) {
            run_frame(quiet, settings.auto_learn, floor);
            std::copy_n(ola_.begin(), hop_, out_.begin());
            std::copy(ola_.begin() + hop_, ola_.end(), ola_.begin());
            std::fill(ola_.begin() + (size_ - hop_), ola_.end(), 0.0);
            std::copy(in_.begin() + hop_, in_.end(), in_.begin());
            in_fill_ = size_ - hop_;
            out_pos_ = 0;
        }
    }
}

void NoiseProfile::run_frame(bool quiet, bool auto_learn, double floor) {
    const std::uint32_t half = size_ / 2 + 1;
    for (std::uint32_t n = 0; n < size_; ++n) {
        bins_[n] = {in_[n] * window_[n], 0.0};
    }
    fft(bins_, twiddles_);

    const bool first = frames_seen_ == 0 && sub_frames_ == 0;
    const double quiet_keep = std::exp(-static_cast<double>(hop_) / (rate_ * kQuietTauS));
    for (std::uint32_t k = 0; k < half; ++k) {
        const double p = std::norm(bins_[k]);
        smoothed_[k] = first ? p : kPowerSmooth * smoothed_[k] + (1.0 - kPowerSmooth) * p;
        if (learn_left_ > 0) {
            learn_acc_[k] += p;
        }
        if (auto_learn && quiet) {
            quiet_avg_[k] = have_quiet_ ? quiet_keep * quiet_avg_[k] + (1.0 - quiet_keep) * p
                                        : smoothed_[k];
        }
        sub_min_[k] = std::min(sub_min_[k], smoothed_[k]);
    }
    if (auto_learn && quiet) {
        have_quiet_ = true;
    }

    if (learn_left_ > 0 && --learn_left_ == 0) {
        for (std::uint32_t k = 0; k < half; ++k) {
            learned_[k] = learn_acc_[k] / learn_total_;
        }
        have_learned_ = true;
    }

    const std::uint32_t per_sub = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(
               std::lround(kMinWindowS * rate_ / hop_ / static_cast<double>(kMinSubWindows))));
    if (++sub_frames_ >= per_sub) {
        std::copy(sub_min_.begin(), sub_min_.end(),
                  mins_.begin() + static_cast<std::ptrdiff_t>(sub_index_) * half);
        std::fill(sub_min_.begin(), sub_min_.end(), std::numeric_limits<double>::infinity());
        sub_index_ = (sub_index_ + 1) % kMinSubWindows;
        sub_frames_ = 0;
        ++frames_seen_;
    }

    // Which estimate, in the order the header gives.
    have_noise_ = false;
    if (have_learned_) {
        std::copy(learned_.begin(), learned_.end(), noise_.begin());
        have_noise_ = true;
    } else if (auto_learn && have_quiet_) {
        // Once the squelch has shut, its average is kept through the open
        // passages: minimum statistics would take a steady carrier for noise.
        std::copy(quiet_avg_.begin(), quiet_avg_.end(), noise_.begin());
        have_noise_ = true;
    } else if (auto_learn && frames_seen_ > 0) {
        for (std::uint32_t k = 0; k < half; ++k) {
            double m = sub_min_[k];
            for (std::uint32_t u = 0; u < kMinSubWindows; ++u) {
                m = std::min(m, mins_[static_cast<std::size_t>(u) * half + k]);
            }
            noise_[k] = kMinBias * m;
        }
        have_noise_ = true;
    }

    std::vector<double>& g = gain_;
    for (std::uint32_t k = 0; k < half; ++k) {
        double target = 1.0;
        if (have_noise_ && noise_[k] > 0.0) {
            const double snr = std::max(smoothed_[k] / noise_[k] - 1.0, 0.0);
            target = std::max(snr / (1.0 + snr), floor);
        }
        g[k] = kGainSmooth * g[k] + (1.0 - kGainSmooth) * target;
    }

    // Across bins, [1 2 1] / 4, so a lone bin cannot pop out of the floor.
    // The smallest of the smoothed and the raw gain is kept at a bin that is
    // only noise, so the smoothing never lets more hum through than the
    // floor allows.
    double prev = g[0];
    for (std::uint32_t k = 0; k < half; ++k) {
        const double here = g[k];
        const double next = k + 1 < half ? g[k + 1] : here;
        const double smooth = 0.25 * prev + 0.5 * here + 0.25 * next;
        prev = here;
        double applied = here <= floor * 1.0001 ? here : smooth;
        applied = std::clamp(applied, floor, 1.0);
        bins_[k] *= applied;
        if (k != 0 && k != size_ / 2) {
            bins_[size_ - k] *= applied;
        }
    }

    // Inverse by conjugation, then the synthesis window.
    for (auto& b : bins_) {
        b = std::conj(b);
    }
    fft(bins_, twiddles_);
    const double scale = 1.0 / static_cast<double>(size_);
    for (std::uint32_t n = 0; n < size_; ++n) {
        ola_[n] += bins_[n].real() * scale * window_[n];
    }
}

}  // namespace revenant::dsp
