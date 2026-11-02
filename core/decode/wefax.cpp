#include "core/decode/wefax.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>

namespace revenant::decode {

namespace {

// The low-pass after the 1900 Hz mix passes 1200 Hz either side: the 400 Hz
// deviation and the first sidebands of the 675 Hz IOC signal.
constexpr double kPassHz = 1200.0;
constexpr double kTransitionHz = 900.0;
constexpr double kWorkRateTarget = 12'000.0;

constexpr double kBlockSeconds = 0.2;
constexpr int kStartBlocks = 10;          // two seconds of a start tone
constexpr double kToneShare = 0.5;        // a square wave puts 8 / pi^2 in its fundamental
constexpr double kPhasingSeconds = 12.0;  // read before the line rate is named
constexpr int kBins = 48;
constexpr double kPhasingMismatch = 0.2;
constexpr double kPhasingLimitSeconds = 90.0;
constexpr std::uint32_t kMaxRows = 12'000;

// WMO-No. 386 III 5.1.5.
constexpr std::array<int, 4> kRates = {240, 120, 90, 60};

}  // namespace

std::uint32_t wefax_width(int ioc) {
    return static_cast<std::uint32_t>(std::lround(std::numbers::pi * ioc));
}

Expected<WefaxDecoder> WefaxDecoder::create(const WefaxConfig& config) {
    if (config.rate < 8'000 || config.rate > 768'000) {
        return fail("wefax: the sample rate must be between 8000 and 768000 S/s");
    }
    if (config.lpm != 0 && std::ranges::find(kRates, config.lpm) == kRates.end()) {
        return fail(std::format("wefax: {} lines per minute is not one of 60, 90, 120 or 240",
                                config.lpm));
    }
    return WefaxDecoder(config);
}

WefaxDecoder::WefaxDecoder(const WefaxConfig& config) : config_(config) {
    const double rate = static_cast<double>(config.rate);
    decimation_ = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(rate / kWorkRateTarget));
    work_rate_ = rate / static_cast<double>(decimation_);

    // Hamming-windowed sinc, as core/decode/sstv.cpp builds its own.
    const auto half = static_cast<std::size_t>(1.65 * rate / kTransitionHz);
    const std::size_t count = 2 * half + 1;
    taps_.resize(count);
    double gain = 0.0;
    const double fc = kPassHz / rate;
    for (std::size_t k = 0; k < count; ++k) {
        const double t = static_cast<double>(k) - static_cast<double>(half);
        const double sinc = t == 0.0 ? 2.0 * fc
                                     : std::sin(2.0 * std::numbers::pi * fc * t) /
                                           (std::numbers::pi * t);
        const double window = 0.54 - 0.46 * std::cos(2.0 * std::numbers::pi *
                                                     static_cast<double>(k) /
                                                     static_cast<double>(count - 1));
        taps_[k] = static_cast<float>(sinc * window);
        gain += sinc * window;
    }
    for (float& t : taps_) {
        t = static_cast<float>(t / gain);
    }
    history_.assign(count, std::complex<float>{});
    step_ = std::polar(1.0, -2.0 * std::numbers::pi * kWefaxCentreHz / rate);
    sum_.push_back(0.0);
}

void WefaxDecoder::push(float x) {
    const std::complex<double> mixed = static_cast<double>(x) * phasor_;
    phasor_ *= step_;
    ++input_count_;
    if ((input_count_ & 0xFFFU) == 0) {
        phasor_ /= std::abs(phasor_);
    }
    history_[history_at_] = std::complex<float>(mixed);
    const std::size_t at = history_at_;
    history_at_ = (history_at_ + 1) % history_.size();
    if (input_count_ % decimation_ != 0) {
        return;
    }
    std::complex<float> z{};
    const std::size_t n = history_.size();
    std::size_t i = at;
    for (std::size_t k = 0; k < n; ++k) {
        z += taps_[k] * history_[i];
        i = (i == 0) ? n - 1 : i - 1;
    }
    const std::complex<float> turn = z * std::conj(previous_);
    previous_ = z;
    const double hz = kWefaxCentreHz + static_cast<double>(std::arg(turn)) * work_rate_ /
                                           (2.0 * std::numbers::pi);
    freq_.push_back(static_cast<float>(hz));
    sum_.push_back(sum_.back() + hz);
}

double WefaxDecoder::cumulative(double at) const {
    const double rel = at - static_cast<double>(base_);
    if (rel <= 0.0) {
        return sum_.front();
    }
    const auto i = static_cast<std::size_t>(rel);
    if (i >= freq_.size()) {
        return sum_.back();
    }
    return sum_[i] + (rel - static_cast<double>(i)) * static_cast<double>(freq_[i]);
}

double WefaxDecoder::mean(double from, double to) const {
    if (to <= from) {
        to = from + 1.0;
    }
    return (cumulative(to) - cumulative(from)) / (to - from);
}

// The share of the span's variance in one Goertzel bin: 1 for a sine at
// `hz`, 8 / pi^2 for a square wave, about 2 / N for noise.
double WefaxDecoder::tone_share(double from, double to, double hz) const {
    const auto a = static_cast<std::size_t>(std::max(0.0, std::ceil(from) - static_cast<double>(base_)));
    const auto b = std::min(freq_.size(),
                            static_cast<std::size_t>(std::max(0.0, std::ceil(to) - static_cast<double>(base_))));
    if (b <= a + 8) {
        return 0.0;
    }
    const double m = mean(static_cast<double>(base_ + a), static_cast<double>(base_ + b));
    const double w = 2.0 * std::numbers::pi * hz / work_rate_;
    double re = 0.0;
    double im = 0.0;
    double var = 0.0;
    for (std::size_t i = a; i < b; ++i) {
        const double d = static_cast<double>(freq_[i]) - m;
        const double ph = w * static_cast<double>(i - a);
        re += d * std::cos(ph);
        im -= d * std::sin(ph);
        var += d * d;
    }
    const auto n = static_cast<double>(b - a);
    return var > 0.0 ? 2.0 * (re * re + im * im) / (n * var) : 0.0;
}

double WefaxDecoder::white_share(double from, double to) const {
    const auto a = static_cast<std::size_t>(std::max(0.0, std::ceil(from) - static_cast<double>(base_)));
    const auto b = std::min(freq_.size(),
                            static_cast<std::size_t>(std::max(0.0, std::ceil(to) - static_cast<double>(base_))));
    if (b <= a) {
        const double f = mean(from, from + 1.0) - offset_hz_ - kWefaxCentreHz;
        return (f > 0.0) == config_.white_high ? 1.0 : 0.0;
    }
    std::size_t white = 0;
    for (std::size_t i = a; i < b; ++i) {
        const double f = static_cast<double>(freq_[i]) - offset_hz_ - kWefaxCentreHz;
        white += (f > 0.0) == config_.white_high ? 1 : 0;
    }
    return static_cast<double>(white) / static_cast<double>(b - a);
}

// The white leading edge nearest `around`: where the mean after, less the
// mean before, is greatest. Found to a sample by a scan and then to a
// fraction of one by a ternary search, which the tent the contrast makes
// around a clean edge allows.
double WefaxDecoder::edge(double around, double reach) const {
    const double span = seconds(0.003);
    const double sign = config_.white_high ? 1.0 : -1.0;
    const auto contrast = [&](double t) { return sign * (mean(t, t + span) - mean(t - span, t)); };
    double best = around;
    double best_value = -1.0e18;
    for (double q = std::floor(around - reach); q <= around + reach; q += 1.0) {
        const double v = contrast(q);
        if (v > best_value) {
            best_value = v;
            best = q;
        }
    }
    double lo = best - 1.0;
    double hi = best + 1.0;
    for (int i = 0; i < 30; ++i) {
        const double a = lo + (hi - lo) / 3.0;
        const double b = hi - (hi - lo) / 3.0;
        if (contrast(a) < contrast(b)) {
            lo = a;
        } else {
            hi = b;
        }
    }
    return 0.5 * (lo + hi);
}

// A straight line through the white leading edges against their line count:
// the start of each line and the sample clock against the transmitter's. One
// edge read off noise cannot tilt it; past six, outliers are dropped.
void WefaxDecoder::refit() {
    const auto fit = [&](const std::vector<std::pair<double, double>>& p) {
        double sk = 0, st = 0, skk = 0, skt = 0;
        for (const auto& [k, t] : p) {
            sk += k;
            st += t;
            skk += k * k;
            skt += k * t;
        }
        const auto n = static_cast<double>(p.size());
        const double den = n * skk - sk * sk;
        double period = p.size() >= 6 && den > 0.0 ? (n * skt - sk * st) / den : nominal_;
        period = std::clamp(period, nominal_ * 0.99, nominal_ * 1.01);
        origin_ = (st - period * sk) / n;
        period_ = period;
    };
    if (edges_.empty()) {
        return;
    }
    fit(edges_);
    std::vector<std::pair<double, double>> inliers;
    for (const auto& p : edges_) {
        if (std::abs(p.second - line_at(p.first)) < seconds(0.004)) {
            inliers.push_back(p);
        }
    }
    if (inliers.size() >= 3) {
        fit(inliers);
    }
    stats_.last_period_ratio = period_ / nominal_;
}

void WefaxDecoder::trim(double keep_from) {
    const double rel = std::floor(keep_from) - static_cast<double>(base_);
    if (rel < 65'536.0) {
        return;
    }
    const auto drop = std::min(static_cast<std::size_t>(rel), freq_.size());
    freq_.erase(freq_.begin(), freq_.begin() + static_cast<std::ptrdiff_t>(drop));
    sum_.erase(sum_.begin(), sum_.begin() + static_cast<std::ptrdiff_t>(drop));
    base_ += drop;
}

void WefaxDecoder::process(std::span<const float> samples, const ImageSink& sink) {
    freq_.reserve(freq_.size() + samples.size() / decimation_ + 1);
    sum_.reserve(sum_.size() + samples.size() / decimation_ + 1);
    for (const float x : samples) {
        push(x);
    }
    run(sink);
}

void WefaxDecoder::flush(const ImageSink& sink) {
    run(sink);
    close(sink, false);
}

void WefaxDecoder::reset(const ImageSink& sink) {
    close(sink, false);
    const WefaxStats kept = stats_;
    *this = WefaxDecoder(config_);
    stats_ = kept;
}

void WefaxDecoder::run(const ImageSink& sink) {
    for (;;) {
        bool moved = false;
        switch (state_) {
            case State::Idle:
                moved = idle_step();
                break;
            case State::Phasing:
                moved = phasing_step();
                break;
            case State::Image:
                moved = image_step(sink);
                break;
        }
        if (!moved) {
            return;
        }
    }
}

// WMO-No. 386 III 5.2.2: 300 Hz for IOC 576, 675 Hz for IOC 288, 5 to 10 s.
bool WefaxDecoder::idle_step() {
    const double block = std::round(seconds(kBlockSeconds));
    const double from = static_cast<double>(block_) * block;
    const double to = from + block;
    if (to > end()) {
        trim(from - seconds(1.0));
        return false;
    }
    int tone = 0;
    if (tone_share(from, to, kWefaxStart576Hz) > kToneShare) {
        tone = 576;
    } else if (tone_share(from, to, kWefaxStart288Hz) > kToneShare) {
        tone = 288;
    }
    ++block_;
    if (tone != 0 && (tone_blocks_ == 0 || tone == tone_ioc_)) {
        ++tone_blocks_;
        tone_ioc_ = tone;
        tone_sum_ += mean(from, to);
        return true;
    }
    if (tone_blocks_ >= kStartBlocks) {
        ++stats_.starts;
        // A square wave between black and white averages to the centre.
        offset_hz_ = tone_sum_ / tone_blocks_ - kWefaxCentreHz;
        ioc_ = tone_ioc_;
        phasing_from_ = from;
        state_ = State::Phasing;
        stats_.last_ioc = ioc_;
        stats_.last_offset_hz = offset_hz_;
    }
    tone_blocks_ = tone != 0 ? 1 : 0;
    tone_ioc_ = tone;
    tone_sum_ = tone != 0 ? mean(from, to) : 0.0;
    return true;
}

// WMO-No. 386 III 5.2.3: phasing at the line rate, white then black, the
// white 50 % or 5 % of the line. Folded at the right period it is a square;
// folded at a period it is not, it smears toward its average.
bool WefaxDecoder::phasing_step() {
    const double a = phasing_from_ + seconds(0.5);
    const double b = phasing_from_ + seconds(kPhasingSeconds + 0.5);
    if (b + seconds(0.1) > end()) {
        return false;
    }
    struct Fold {
        int lpm = 0;
        double contrast = 0.0;
        std::vector<double> profile;
    };
    std::vector<Fold> folds;
    for (const int lpm : kRates) {
        if (config_.lpm != 0 && lpm != config_.lpm) {
            continue;
        }
        const double p = seconds(60.0 / lpm);
        Fold f{lpm, 0.0, std::vector<double>(kBins, 0.0)};
        int cycles = 0;
        for (double s = a; s + p <= b; s += p) {
            for (int i = 0; i < kBins; ++i) {
                f.profile[static_cast<std::size_t>(i)] +=
                    white_share(s + i * p / kBins, s + (i + 1) * p / kBins);
            }
            ++cycles;
        }
        for (double& v : f.profile) {
            v /= std::max(1, cycles);
        }
        const auto [lo, hi] = std::ranges::minmax_element(f.profile);
        f.contrast = *hi - *lo;
        folds.push_back(std::move(f));
    }
    double best = 0.0;
    for (const Fold& f : folds) {
        best = std::max(best, f.contrast);
    }
    const Fold* chosen = nullptr;
    for (const Fold& f : folds) {
        if (f.contrast >= 0.75 * best) {
            chosen = &f;
            break;
        }
    }
    if (chosen == nullptr || best < 0.5) {
        ++stats_.phasing_failed;
        state_ = State::Idle;
        block_ = static_cast<std::uint64_t>(std::ceil(a / std::round(seconds(kBlockSeconds))));
        return true;
    }
    lpm_ = chosen->lpm;
    nominal_ = seconds(60.0 / lpm_);
    width_ = wefax_width(ioc_);
    stats_.last_lpm = lpm_;

    // The white leading edge, the steepest rise of the folded line.
    int rise = 0;
    double steepest = -2.0;
    for (int i = 0; i < kBins; ++i) {
        const double d = chosen->profile[static_cast<std::size_t>(i)] -
                         chosen->profile[static_cast<std::size_t>((i + kBins - 1) % kBins)];
        if (d > steepest) {
            steepest = d;
            rise = i;
        }
    }
    template_.resize(kBins);
    for (int i = 0; i < kBins; ++i) {
        template_[static_cast<std::size_t>(i)] =
            chosen->profile[static_cast<std::size_t>((rise + i) % kBins)];
    }
    const double first = a + rise * nominal_ / kBins;
    edges_.clear();
    origin_ = first;
    period_ = nominal_;
    double k = 0.0;
    for (; line_at(k + 1.0) <= b; k += 1.0) {
        edges_.emplace_back(k, edge(line_at(k), nominal_ / 24.0));
        refit();
    }
    stats_.phasing_lines += edges_.size();
    next_line_ = k;
    stop_lines_ = 0;
    state_ = State::Image;
    return true;
}

bool WefaxDecoder::image_step(const ImageSink& sink) {
    const double k = next_line_;
    const double s = line_at(k);
    const double e = line_at(k + 1.0);
    if (e + seconds(0.02) > end()) {
        trim(s - seconds(1.0));
        return false;
    }
    const double line_seconds = 60.0 / lpm_;

    // WMO-No. 386 III 5.2.5 the stop, or a start tone for the next chart.
    const bool stop = tone_share(s, e, kWefaxStopHz) > kToneShare;
    const bool start = tone_share(s, e, kWefaxStart576Hz) > kToneShare ||
                       tone_share(s, e, kWefaxStart288Hz) > kToneShare;
    if (stop || start) {
        ++stop_lines_;
        next_line_ += 1.0;
        if (stop_lines_ * line_seconds >= 1.0) {
            close(sink, stop && open_);
            // Back to the blocks from where the signal began, so a start tone
            // is heard from its beginning.
            const double from = line_at(k + 1.0 - stop_lines_);
            block_ = static_cast<std::uint64_t>(std::floor(from / std::round(seconds(kBlockSeconds))));
            tone_blocks_ = 0;
        }
        return true;
    }
    stop_lines_ = 0;

    if (!open_) {
        // Still phasing? The line folded into the template's bins, compared.
        double mismatch = 0.0;
        for (int i = 0; i < kBins; ++i) {
            const double w = white_share(s + i * period_ / kBins, s + (i + 1) * period_ / kBins);
            mismatch += std::abs(w - template_[static_cast<std::size_t>(i)]);
        }
        mismatch /= kBins;
        if (mismatch < kPhasingMismatch) {
            edges_.emplace_back(k, edge(s, nominal_ / 24.0));
            refit();
            ++stats_.phasing_lines;
            next_line_ += 1.0;
            if (k * line_seconds > kPhasingLimitSeconds) {
                ++stats_.phasing_failed;
                close(sink, false);
                block_ = static_cast<std::uint64_t>(std::floor(e / std::round(seconds(kBlockSeconds))));
            }
            return true;
        }
        open_ = true;
        ++image_id_;
        ++stats_.images;
        rows_ = 0;
        first_row_line_ = k;
        if (sink.on_open) {
            sink.on_open(ImageOpen{image_id_, width_, ImageRow::Gray8,
                                   std::format("WEFAX {}/{}", lpm_, ioc_), lpm_ / 60.0});
        }
    }

    row_.resize(width_);
    const double px = (e - s) / width_;
    for (std::uint32_t i = 0; i < width_; ++i) {
        const double f = mean(s + i * px, s + (i + 1) * px) - offset_hz_;
        double v = (f - (kWefaxCentreHz - kWefaxDeviationHz)) / (2.0 * kWefaxDeviationHz);
        if (!config_.white_high) {
            v = 1.0 - v;
        }
        row_[i] = static_cast<std::uint8_t>(std::clamp(std::lround(v * 255.0), 0L, 255L));
    }
    if (sink.on_row) {
        sink.on_row(ImageRow{image_id_, rows_, width_, ImageRow::Gray8, row_});
    }
    ++rows_;
    next_line_ += 1.0;
    if (rows_ >= kMaxRows) {
        close(sink, false);
        block_ = static_cast<std::uint64_t>(std::floor(e / std::round(seconds(kBlockSeconds))));
    }
    return true;
}

void WefaxDecoder::close(const ImageSink& sink, bool complete) {
    if (open_) {
        if (sink.on_close) {
            sink.on_close(ImageClose{image_id_, rows_, complete});
        }
        if (complete) {
            ++stats_.images_complete;
        }
    }
    open_ = false;
    state_ = State::Idle;
    edges_.clear();
    tone_blocks_ = 0;
}

}  // namespace revenant::decode
