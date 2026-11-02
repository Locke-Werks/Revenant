// NOAA APT decoder. See apt.h for the specification and its citations.

#include "core/decode/apt.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace revenant::decode {

namespace {

// Envelope low-pass. The video occupies up to half the word rate, 2080 Hz,
// either side of the subcarrier; the mixer's image sits at twice 2400 Hz.
// Engineering choices, not from KLM.
constexpr double kEnvelopeCutoffHz = 2200.0;
constexpr double kEnvelopeTransitionHz = 500.0;

// Combined Sync A and Sync B normalised correlation, halved, that a line has
// to reach to start an image and to stay tracked. Chosen from the round trip
// in tests/decode/test_apt.cpp, not from the document.
constexpr float kAcquireScore = 0.5F;
constexpr float kTrackScore = 0.3F;

// Search either side of the expected line start while tracked, in words.
constexpr double kTrackWindowWords = 6.0;

// Line period may wander this far from nominal: the satellite's clock and the
// receiver's sample clock together.
constexpr double kPeriodTolerance = 0.005;

std::vector<float> design_lowpass(double fs) {
    auto n = static_cast<std::size_t>(std::ceil(3.3 * fs / kEnvelopeTransitionHz));
    n |= 1U;
    std::vector<float> taps(n);
    const double fc = kEnvelopeCutoffHz / fs;
    const double mid = static_cast<double>(n - 1) / 2.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(i) - mid;
        const double sinc = x == 0.0 ? 2.0 * fc
                                     : std::sin(2.0 * std::numbers::pi * fc * x) /
                                           (std::numbers::pi * x);
        const double w = 0.54 - 0.46 * std::cos(2.0 * std::numbers::pi *
                                                static_cast<double>(i) /
                                                static_cast<double>(n - 1));
        taps[i] = static_cast<float>(sinc * w);
        sum += sinc * w;
    }
    for (float& t : taps) {
        t = static_cast<float>(t / sum);
    }
    return taps;
}

// A sync's words resampled to the input rate, zero mean, so a correlation
// against it ignores the envelope's level.
std::vector<float> sync_template(const std::array<std::uint8_t, kAptSyncWords>& words, double fs) {
    const auto n = static_cast<std::size_t>(
        std::floor(static_cast<double>(kAptSyncWords) * fs / kAptWordRate));
    std::vector<float> t(n);
    double mean = 0.0;
    for (std::size_t j = 0; j < n; ++j) {
        const auto w = std::min<std::size_t>(
            kAptSyncWords - 1,
            static_cast<std::size_t>(static_cast<double>(j) * kAptWordRate / fs));
        t[j] = words[w] == kAptSyncHigh ? 1.0F : -1.0F;
        mean += t[j];
    }
    mean /= static_cast<double>(n);
    for (float& v : t) {
        v = static_cast<float>(v - mean);
    }
    return t;
}

double parabolic(float left, float centre, float right) {
    const double den = static_cast<double>(left) - 2.0 * centre + right;
    if (std::abs(den) < 1e-12) {
        return 0.0;
    }
    return std::clamp(0.5 * (static_cast<double>(left) - right) / den, -0.5, 0.5);
}

}  // namespace

std::array<std::uint8_t, kAptSyncWords> apt_sync_a() {
    // Figure 4.2.3-3: low to word 4, seven cycles of two high and two low,
    // low from word 32 to the end of the 39.
    std::array<std::uint8_t, kAptSyncWords> s{};
    s.fill(kAptSyncLow);
    for (std::size_t c = 0; c < 7; ++c) {
        s[4 + 4 * c] = kAptSyncHigh;
        s[5 + 4 * c] = kAptSyncHigh;
    }
    return s;
}

std::array<std::uint8_t, kAptSyncWords> apt_sync_b() {
    // Figure 4.2.3-3: low to word 4, seven pulses of three high and two low.
    std::array<std::uint8_t, kAptSyncWords> s{};
    s.fill(kAptSyncLow);
    for (std::size_t c = 0; c < 7; ++c) {
        for (std::size_t k = 0; k < 3; ++k) {
            s[4 + 5 * c + k] = kAptSyncHigh;
        }
    }
    return s;
}

std::string apt_channel_name(int grey_wedge) {
    // 4.2.3: wedge 3 is channel 3A, wedge 6 is 3B, otherwise the same number.
    switch (grey_wedge) {
        case 1: return "1";
        case 2: return "2";
        case 3: return "3A";
        case 4: return "4";
        case 5: return "5";
        case 6: return "3B";
        default: return "";
    }
}

Expected<AptDecoder> AptDecoder::create(const AptConfig& config, AptEvents events) {
    // Below about 2.2 times the top of the subcarrier's band, 4480 Hz, the
    // mixer's image folds onto the video.
    if (!(config.sample_rate >= 9600.0) || !(config.sample_rate <= 384000.0)) {
        return fail("APT: sample rate must be between 9600 and 384000 Hz");
    }
    AptDecoder d;
    d.config_ = config;
    d.events_ = std::move(events);
    d.taps_ = design_lowpass(config.sample_rate);
    d.hist_i_.assign(2 * d.taps_.size(), 0.0F);
    d.hist_q_.assign(2 * d.taps_.size(), 0.0F);
    d.phase_step_ = kAptSubcarrierHz / config.sample_rate;
    d.sync_a_tmpl_ = sync_template(apt_sync_a(), config.sample_rate);
    d.sync_b_tmpl_ = sync_template(apt_sync_b(), config.sample_rate);
    // Table 4.2.2-1: 120 lines a minute.
    d.nominal_period_ = config.sample_rate / 2.0;
    d.period_ = d.nominal_period_;
    return d;
}

void AptDecoder::push(std::span<const float> audio) {
    const std::size_t n = taps_.size();
    const std::size_t delay = (n - 1) / 2;
    for (const float x : audio) {
        const double angle = 2.0 * std::numbers::pi * phase_;
        phase_ += phase_step_;
        phase_ -= std::floor(phase_);
        const auto i = static_cast<float>(x * std::cos(angle));
        const auto q = static_cast<float>(-x * std::sin(angle));
        hist_i_[hist_pos_] = i;
        hist_i_[hist_pos_ + n] = i;
        hist_q_[hist_pos_] = q;
        hist_q_[hist_pos_ + n] = q;
        hist_pos_ = (hist_pos_ + 1) % n;
        // hist_[hist_pos_ .. hist_pos_ + n) is oldest to newest.
        float si = 0.0F;
        float sq = 0.0F;
        for (std::size_t k = 0; k < n; ++k) {
            si += taps_[k] * hist_i_[hist_pos_ + k];
            sq += taps_[k] * hist_q_[hist_pos_ + k];
        }
        if (consumed_ >= delay) {
            env_.push_back(2.0F * std::sqrt(si * si + sq * sq));
        }
        ++consumed_;
    }
    process();
}

float AptDecoder::envelope_at(double sample) const {
    const double rel = sample - env_base_;
    if (rel < 0.0) {
        return 0.0F;
    }
    const auto k = static_cast<std::size_t>(rel);
    if (k + 1 >= env_.size()) {
        return env_.empty() ? 0.0F : env_.back();
    }
    const auto f = static_cast<float>(rel - static_cast<double>(k));
    return env_[k] + f * (env_[k + 1] - env_[k]);
}

float AptDecoder::correlate(const std::vector<float>& tmpl, double at) const {
    const auto base = static_cast<std::ptrdiff_t>(std::llround(at - env_base_));
    if (base < 0 || static_cast<std::size_t>(base) + tmpl.size() > env_.size()) {
        return 0.0F;
    }
    const float* e = env_.data() + base;
    double num = 0.0;
    double tt = 0.0;
    double sum = 0.0;
    double sq = 0.0;
    for (std::size_t j = 0; j < tmpl.size(); ++j) {
        num += static_cast<double>(tmpl[j]) * e[j];
        tt += static_cast<double>(tmpl[j]) * tmpl[j];
        sum += e[j];
        sq += static_cast<double>(e[j]) * e[j];
    }
    const double var = sq - sum * sum / static_cast<double>(tmpl.size());
    if (var <= 1e-20 || tt <= 0.0) {
        return 0.0F;
    }
    return static_cast<float>(num / std::sqrt(tt * var));
}

void AptDecoder::process() {
    while (try_line()) {
    }
    // Keep a line behind whatever the next decision looks at.
    const double keep = (locked_ ? last_start_ : search_from_) - nominal_period_;
    const double drop = std::floor(keep - env_base_);
    if (drop > 4.0 * nominal_period_) {
        env_.erase(env_.begin(), env_.begin() + static_cast<std::ptrdiff_t>(drop));
        env_base_ += drop;
    }
}

bool AptDecoder::try_line() {
    const double available = env_base_ + static_cast<double>(env_.size());
    const auto score_at = [&](double p, float& a, float& b) {
        a = correlate(sync_a_tmpl_, p);
        b = correlate(sync_b_tmpl_, p + period_ / 2.0);
        return 0.5F * (a + b);
    };

    if (!locked_) {
        const double s = std::ceil(search_from_);
        if (available < s + 2.0 * nominal_period_ + 4.0) {
            return false;
        }
        double best_p = s;
        float best = -2.0F;
        float best_a = 0.0F;
        float best_b = 0.0F;
        for (double p = s; p < s + nominal_period_; p += 1.0) {
            float a = 0.0F;
            float b = 0.0F;
            const float sc = score_at(p, a, b);
            if (sc > best) {
                best = sc;
                best_p = p;
                best_a = a;
                best_b = b;
            }
        }
        if (best < kAcquireScore) {
            search_from_ = s + nominal_period_;
            return true;
        }
        const double refined =
            best_p + parabolic(correlate(sync_a_tmpl_, best_p - 1.0), best_a,
                               correlate(sync_a_tmpl_, best_p + 1.0));
        locked_ = true;
        missed_ = 0;
        period_ = nominal_period_;
        calibrated_ = false;
        image_id_ = next_image_id_++;
        rows_ = 0;
        telemetry_.clear();
        telemetry_ab_.clear();
        any_frame_ = false;
        if (events_.image.on_open) {
            events_.image.on_open(ImageOpen{image_id_, static_cast<std::uint32_t>(kAptLineWords),
                                            ImageRow::Gray8, "NOAA APT", 2.0});
        }
        last_start_ = refined;
        emit_line(refined, best_a, best_b, false);
        return true;
    }

    const double window = std::ceil(kTrackWindowWords * config_.sample_rate / kAptWordRate);
    const double expected = last_start_ + period_;
    if (available < expected + window + period_ + 4.0) {
        return false;
    }
    const double centre = std::round(expected);
    double best_p = centre;
    float best = -2.0F;
    float best_a = 0.0F;
    float best_b = 0.0F;
    for (double p = centre - window; p <= centre + window; p += 1.0) {
        float a = 0.0F;
        float b = 0.0F;
        const float sc = score_at(p, a, b);
        if (sc > best) {
            best = sc;
            best_p = p;
            best_a = a;
            best_b = b;
        }
    }
    if (best >= kTrackScore) {
        const double refined =
            best_p + parabolic(correlate(sync_a_tmpl_, best_p - 1.0), best_a,
                               correlate(sync_a_tmpl_, best_p + 1.0));
        const double measured = (refined - last_start_) / static_cast<double>(missed_ + 1);
        period_ += 0.1 * (measured - period_);
        period_ = std::clamp(period_, nominal_period_ * (1.0 - kPeriodTolerance),
                             nominal_period_ * (1.0 + kPeriodTolerance));
        missed_ = 0;
        last_start_ = refined;
        emit_line(refined, best_a, best_b, false);
        return true;
    }
    ++missed_;
    if (missed_ > config_.max_missed_lines) {
        close_image(false);
        locked_ = false;
        search_from_ = expected;
        return true;
    }
    last_start_ = expected;
    emit_line(expected, best_a, best_b, true);
    return true;
}

void AptDecoder::emit_line(double start, float a, float b, bool free_running) {
    const double step = period_ / static_cast<double>(kAptLineWords);
    std::array<float, kAptLineWords> raw{};
    for (std::size_t k = 0; k < kAptLineWords; ++k) {
        raw[k] = envelope_at(start + (static_cast<double>(k) + 0.5) * step);
    }
    if (!free_running) {
        // Sync A words 4..31 average to the midpoint of 11 and 244; words
        // 33..37 sit at 11. Figure 4.2.3-3.
        double mid = 0.0;
        for (std::size_t k = 4; k < 32; ++k) {
            mid += raw[k];
        }
        mid /= 28.0;
        double low = 0.0;
        for (std::size_t k = 33; k < 38; ++k) {
            low += raw[k];
        }
        low /= 5.0;
        if (!calibrated_) {
            cal_low_ = low;
            cal_mid_ = mid;
            calibrated_ = true;
        } else {
            cal_low_ += 0.1 * (low - cal_low_);
            cal_mid_ += 0.1 * (mid - cal_mid_);
        }
    }
    const double span = std::max(cal_mid_ - cal_low_, 1e-9);
    const double scale = (0.5 * (kAptSyncLow + kAptSyncHigh) - kAptSyncLow) / span;

    AptLine line;
    line.line = rows_;
    line.image_id = image_id_;
    line.start_sample = start;
    line.period_samples = period_;
    line.sync_a = a;
    line.sync_b = b;
    line.free_running = free_running;
    std::array<double, kAptLineWords> counts{};
    for (std::size_t k = 0; k < kAptLineWords; ++k) {
        counts[k] = kAptSyncLow + (raw[k] - cal_low_) * scale;
        line.words[k] = static_cast<std::uint8_t>(std::clamp(std::lround(counts[k]), 0L, 255L));
    }

    // Middle of each 45-word telemetry block, away from the edges the
    // low-pass smears.
    std::array<float, 2> tel{};
    for (std::size_t ch = 0; ch < 2; ++ch) {
        const std::size_t from = (ch == 0 ? kAptChannelATelemetry : kAptChannelBTelemetry) + 6;
        double s = 0.0;
        for (std::size_t k = from; k < from + 33; ++k) {
            s += counts[k];
        }
        tel[ch] = static_cast<float>(s / 33.0);
    }
    telemetry_ab_.push_back(tel);
    telemetry_.push_back(0.5F * (tel[0] + tel[1]));

    if (events_.on_line) {
        events_.on_line(line);
    }
    if (events_.image.on_row) {
        ImageRow row;
        row.image_id = image_id_;
        row.row = static_cast<std::uint32_t>(rows_);
        row.width = static_cast<std::uint32_t>(kAptLineWords);
        row.model = ImageRow::Gray8;
        row.bytes.assign(line.words.begin(), line.words.end());
        events_.image.on_row(row);
    }
    ++rows_;
    check_telemetry();
}

void AptDecoder::check_telemetry() {
    // Wedges 1..14 are the same on both channels (4.2.3), so their average is
    // used to find the frame. Wedges 1..8 rise evenly and 9 is zero
    // modulation, Figure 4.2.2-1; the frame ends at wedge 16.
    constexpr std::size_t kLines = kAptFrameLines;
    constexpr std::size_t kSettle = 5;  // lines past the frame to place it
    const std::size_t n = telemetry_.size();
    if (n < kLines + kSettle) {
        return;
    }
    const std::size_t s = n - kLines - kSettle;
    if (any_frame_ && s < last_frame_line_ + kLines - 4) {
        return;
    }
    const auto wedge = [&](std::size_t start, std::size_t w) {
        double v = 0.0;
        for (std::size_t l = 2; l < 6; ++l) {
            v += telemetry_[start + 8 * w + l];
        }
        return v / 4.0;
    };
    // Fit wedges 1..8 to a line.
    std::array<double, 8> g{};
    for (std::size_t w = 0; w < 8; ++w) {
        g[w] = wedge(s, w);
    }
    double mx = 3.5;
    double my = 0.0;
    for (double v : g) {
        my += v;
    }
    my /= 8.0;
    double sxy = 0.0;
    double sxx = 0.0;
    for (std::size_t w = 0; w < 8; ++w) {
        sxy += (static_cast<double>(w) - mx) * (g[w] - my);
        sxx += (static_cast<double>(w) - mx) * (static_cast<double>(w) - mx);
    }
    const double slope = sxy / sxx;
    double resid = 0.0;
    for (std::size_t w = 0; w < 8; ++w) {
        const double e = g[w] - (my + slope * (static_cast<double>(w) - mx));
        resid += e * e;
    }
    resid = std::sqrt(resid / 8.0);
    if (!(slope > 8.0) || resid > 0.15 * slope || !(wedge(s, 8) < g[0] - 0.5 * slope)) {
        return;
    }
    // The candidate can sit a line or two early; the sharpest drop from
    // wedge 8 to wedge 9 places it exactly.
    std::size_t start = s;
    double sharpest = -1e30;
    for (std::size_t d = 0; d <= kSettle; ++d) {
        const double drop = telemetry_[s + d + 63] - telemetry_[s + d + 64];
        if (drop > sharpest) {
            sharpest = drop;
            start = s + d;
        }
    }
    AptTelemetry t;
    t.image_id = image_id_;
    t.first_line = start;
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (std::size_t w = 0; w < 16; ++w) {
            double v = 0.0;
            for (std::size_t l = 1; l < 7; ++l) {
                v += telemetry_ab_[start + 8 * w + l][ch];
            }
            t.wedges[ch][w] = static_cast<float>(v / 6.0);
        }
        int closest = 0;
        float err = 1e30F;
        for (int w = 0; w < 6; ++w) {
            const float e = std::abs(t.wedges[ch][15] - t.wedges[ch][static_cast<std::size_t>(w)]);
            if (e < err) {
                err = e;
                closest = w + 1;
            }
        }
        t.channel[ch] = apt_channel_name(closest);
    }
    any_frame_ = true;
    last_frame_line_ = start;
    if (events_.on_telemetry) {
        events_.on_telemetry(t);
    }
}

void AptDecoder::close_image(bool complete) {
    if (image_id_ != 0 && events_.image.on_close) {
        events_.image.on_close(ImageClose{image_id_, static_cast<std::uint32_t>(rows_), complete});
    }
    image_id_ = 0;
}

void AptDecoder::finish() {
    if (locked_) {
        close_image(true);
        locked_ = false;
    }
}

}  // namespace revenant::decode
