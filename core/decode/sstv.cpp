#include "core/decode/sstv.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <string>

namespace revenant::decode {

namespace {

SstvSegment tone(double ms, double hz) { return SstvSegment{ms, hz, -1, 0}; }
SstvSegment scan(double ms, int channel, int line = 0) { return SstvSegment{ms, 0.0, channel, line}; }

constexpr int kR = 0;
constexpr int kG = 1;
constexpr int kB = 2;
constexpr int kY = 0;
constexpr int kRy = 1;
constexpr int kBy = 2;

// "MARTIN MODES": sync, porch, then green, blue and red each followed by a
// 0.572 ms 1500 Hz separator.
SstvMode martin(std::string_view name, int vis, double scan_ms) {
    SstvMode m;
    m.name = name;
    m.vis = vis;
    m.width = 320;
    m.height = 256;
    m.period = {tone(4.862, 1200), tone(0.572, 1500), scan(scan_ms, kG), tone(0.572, 1500),
                scan(scan_ms, kB),  tone(0.572, 1500), scan(scan_ms, kR), tone(0.572, 1500)};
    m.sync_offset_ms = 0.0;
    m.sync_ms = 4.862;
    return m;
}

// "SCOTTIE MODES": a 9.0 ms starting sync on the first line only, then
// separator, green, separator, blue, sync, porch, red.
SstvMode scottie(std::string_view name, int vis, double scan_ms) {
    SstvMode m;
    m.name = name;
    m.vis = vis;
    m.width = 320;
    m.height = 256;
    m.lead = {tone(9.0, 1200)};
    m.period = {tone(1.5, 1500), scan(scan_ms, kG), tone(1.5, 1500), scan(scan_ms, kB),
                tone(9.0, 1200), tone(1.5, 1500), scan(scan_ms, kR)};
    m.sync_offset_ms = 3.0 + 2.0 * scan_ms;
    m.sync_ms = 9.0;
    return m;
}

// "PASOKON P modes": sync, then a porch before each colour and after blue.
SstvMode pasokon(std::string_view name, int vis, double sync_ms, double porch_ms,
                 double scan_ms) {
    SstvMode m;
    m.name = name;
    m.vis = vis;
    m.width = 640;
    m.height = 496;
    m.period = {tone(sync_ms, 1200), tone(porch_ms, 1500), scan(scan_ms, kR),
                tone(porch_ms, 1500), scan(scan_ms, kG),   tone(porch_ms, 1500),
                scan(scan_ms, kB),    tone(porch_ms, 1500)};
    m.sync_ms = sync_ms;
    return m;
}

// "PD Modes": two lines per sync, Y odd, R-Y, B-Y, Y even.
SstvMode pd(std::string_view name, int vis, std::uint32_t width, std::uint32_t height,
            double scan_ms) {
    SstvMode m;
    m.name = name;
    m.vis = vis;
    m.width = width;
    m.height = height;
    m.lines_per_period = 2;
    m.colour = SstvColour::Ycc;
    m.period = {tone(20.0, 1200),   tone(2.080, 1500), scan(scan_ms, kY, 0),
                scan(scan_ms, kRy), scan(scan_ms, kBy), scan(scan_ms, kY, 1)};
    m.sync_ms = 20.0;
    return m;
}

std::vector<SstvMode> build_modes() {
    std::vector<SstvMode> modes;
    modes.push_back(martin("Martin 1", 44, 146.432));
    modes.push_back(martin("Martin 2", 40, 73.216));
    modes.push_back(scottie("Scottie 1", 60, 138.240));
    modes.push_back(scottie("Scottie 2", 56, 88.064));
    modes.push_back(scottie("Scottie DX", 76, 345.6));

    // "ROBOT 36 COLOR", two lines shown as the paper shows them.
    {
        SstvMode m;
        m.name = "Robot 36";
        m.vis = 8;
        m.width = 320;
        m.height = 240;
        m.lines_per_period = 2;
        m.colour = SstvColour::Ycc;
        m.period = {tone(9.0, 1200), tone(3.0, 1500), scan(88.0, kY, 0), tone(4.5, 1500),
                    tone(1.5, 1900), scan(44.0, kRy), tone(9.0, 1200),  tone(3.0, 1500),
                    scan(88.0, kY, 1), tone(4.5, 2300), tone(1.5, 1900), scan(44.0, kBy)};
        m.sync_ms = 9.0;
        modes.push_back(std::move(m));
    }
    // "ROBOT 72 COLOR".
    {
        SstvMode m;
        m.name = "Robot 72";
        m.vis = 12;
        m.width = 320;
        m.height = 240;
        m.colour = SstvColour::Ycc;
        m.period = {tone(9.0, 1200),   tone(3.0, 1500), scan(138.0, kY), tone(4.5, 1500),
                    tone(1.5, 1900),   scan(69.0, kRy), tone(4.5, 2300), tone(1.5, 1500),
                    scan(69.0, kBy)};
        m.sync_ms = 9.0;
        modes.push_back(std::move(m));
    }
    // "WRASSE SC2-180".
    {
        SstvMode m;
        m.name = "Wraase SC2-180";
        m.vis = 55;
        m.width = 320;
        m.height = 256;
        m.period = {tone(5.5225, 1200), tone(0.5, 1500), scan(235.0, kR), scan(235.0, kG),
                    scan(235.0, kB)};
        m.sync_ms = 5.5225;
        modes.push_back(std::move(m));
    }
    modes.push_back(pasokon("Pasokon P3", 113, 5.208, 1.042, 133.333));
    modes.push_back(pasokon("Pasokon P5", 114, 7.813, 1.563, 200.000));
    modes.push_back(pasokon("Pasokon P7", 115, 10.417, 2.083, 266.666));
    modes.push_back(pd("PD50", 93, 320, 256, 91.520));
    modes.push_back(pd("PD90", 99, 320, 256, 170.240));
    modes.push_back(pd("PD120", 95, 640, 496, 121.600));
    modes.push_back(pd("PD160", 98, 512, 400, 195.584));
    modes.push_back(pd("PD180", 96, 640, 496, 183.040));
    modes.push_back(pd("PD240", 97, 640, 496, 244.480));
    modes.push_back(pd("PD290", 94, 800, 616, 228.800));
    // "FAX480 mode".
    {
        SstvMode m;
        m.name = "FAX480";
        m.width = 512;
        m.height = 480;
        m.colour = SstvColour::Gray;
        m.period = {tone(5.12, 1200), scan(262.144, 0)};
        m.sync_ms = 5.12;
        m.skip_periods = 20;
        m.fax_start = true;
        modes.push_back(std::move(m));
    }
    return modes;
}

// The low-pass after the 1900 Hz mix: 1200 Hz either side passes every
// frequency the paper uses, 1100 to 2300 Hz, with a tuning error to spare.
constexpr double kPassHz = 1200.0;
constexpr double kTransitionHz = 900.0;
constexpr double kWorkRateTarget = 16'000.0;

std::uint8_t level(double hz) {
    const double v = (hz - kSstvBlackHz) / kSstvHzPerLevel;
    return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
}

std::uint8_t clamp_byte(double v) {
    return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
}

}  // namespace

double SstvMode::period_ms() const {
    double total = 0.0;
    for (const SstvSegment& s : period) {
        total += s.ms;
    }
    return total;
}

double SstvMode::lead_ms() const {
    double total = 0.0;
    for (const SstvSegment& s : lead) {
        total += s.ms;
    }
    return total;
}

std::span<const SstvMode> sstv_modes() {
    static const std::vector<SstvMode> modes = build_modes();
    return modes;
}

const SstvMode* sstv_mode_by_vis(int vis) {
    for (const SstvMode& m : sstv_modes()) {
        if (m.vis == vis) {
            return &m;
        }
    }
    return nullptr;
}

const SstvMode* sstv_mode_by_name(std::string_view name) {
    for (const SstvMode& m : sstv_modes()) {
        if (m.name == name) {
            return &m;
        }
    }
    return nullptr;
}

// Appendix B, coefficients as printed.
SstvYcc sstv_rgb_to_ycc(double r, double g, double b) {
    return SstvYcc{16.0 + 0.003906 * ((65.738 * r) + (129.057 * g) + (25.064 * b)),
                   128.0 + 0.003906 * ((112.439 * r) + (-94.154 * g) + (-18.285 * b)),
                   128.0 + 0.003906 * ((-37.945 * r) + (-74.494 * g) + (112.439 * b))};
}

SstvRgb sstv_ycc_to_rgb(double y, double ry, double by) {
    return SstvRgb{
        0.003906 * ((298.082 * (y - 16.0)) + (408.583 * (ry - 128.0))),
        0.003906 * ((298.082 * (y - 16.0)) + (-100.291 * (by - 128.0)) + (-208.12 * (ry - 128.0))),
        0.003906 * ((298.082 * (y - 16.0)) + (516.411 * (by - 128.0)))};
}

Expected<SstvDecoder> SstvDecoder::create(const SstvConfig& config) {
    if (config.rate < 8'000 || config.rate > 768'000) {
        return fail("sstv: the sample rate must be between 8000 and 768000 S/s");
    }
    return SstvDecoder(config);
}

SstvDecoder::SstvDecoder(const SstvConfig& config) : config_(config) {
    const double rate = static_cast<double>(config.rate);
    decimation_ = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(rate / kWorkRateTarget));
    work_rate_ = rate / static_cast<double>(decimation_);

    // Hamming-windowed sinc; 3.3 / N of the rate is the window's transition.
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
    step_ = std::polar(1.0, -2.0 * std::numbers::pi * kSstvLeaderHz / rate);
    sum_.push_back(0.0);
    dev_sum_.push_back(0.0);
    hunt_at_ = ms(kVisLeaderMs);
}

void SstvDecoder::push(float x) {
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
    const double hz = kSstvLeaderHz + static_cast<double>(std::arg(turn)) * work_rate_ /
                                          (2.0 * std::numbers::pi);
    // Against the frequency half a FAX480 header cycle earlier: strongly
    // negative through the header, near zero in noise and in a leader.
    const auto lag = static_cast<std::size_t>(std::lround(ms(2.05)));
    const double earlier = freq_.size() >= lag
                               ? static_cast<double>(freq_[freq_.size() - lag])
                               : kSstvLeaderHz;
    freq_.push_back(static_cast<float>(hz));
    sum_.push_back(sum_.back() + hz);
    dev_sum_.push_back(dev_sum_.back() + (hz - kSstvLeaderHz) * (earlier - kSstvLeaderHz));
}

double SstvDecoder::cumulative(double at) const {
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

double SstvDecoder::mean(double from, double to) const {
    if (to <= from) {
        to = from + 1.0;
    }
    return (cumulative(to) - cumulative(from)) / (to - from);
}

void SstvDecoder::trim(double keep_from) {
    const double rel = std::floor(keep_from) - static_cast<double>(base_);
    if (rel < 65'536.0) {
        return;
    }
    const auto drop = std::min(static_cast<std::size_t>(rel), freq_.size());
    freq_.erase(freq_.begin(), freq_.begin() + static_cast<std::ptrdiff_t>(drop));
    sum_.erase(sum_.begin(), sum_.begin() + static_cast<std::ptrdiff_t>(drop));
    dev_sum_.erase(dev_sum_.begin(), dev_sum_.begin() + static_cast<std::ptrdiff_t>(drop));
    base_ += drop;
}

void SstvDecoder::process(std::span<const float> samples, ImageSink& sink) {
    freq_.reserve(freq_.size() + samples.size() / decimation_ + 1);
    sum_.reserve(sum_.size() + samples.size() / decimation_ + 1);
    dev_sum_.reserve(sum_.capacity());
    for (const float x : samples) {
        push(x);
    }
    run(sink, false);
}

void SstvDecoder::flush(ImageSink& sink) {
    run(sink, true);
    if (image_) {
        finish(sink, false);
    }
}

void SstvDecoder::reset(ImageSink& sink) {
    if (image_) {
        finish(sink, false);
    }
    const SstvStats kept = stats_;
    *this = SstvDecoder(config_);
    stats_ = kept;
}

void SstvDecoder::run(ImageSink& sink, bool at_end) {
    for (;;) {
        if (image_) {
            if (!step_image(sink, at_end)) {
                return;
            }
        } else if (!hunt(sink)) {
            return;
        }
    }
}

// "VIS Code and Robot calibration header". The candidate is the start of the
// start bit: the second leader before it, 1200 Hz after it.
bool SstvDecoder::hunt(ImageSink& sink) {
    for (;;) {
        double c = hunt_at_;
        if (c + ms(kVisBitMs * 10.0 + 20.0) > end()) {
            trim(c - ms(kVisLeaderMs + 50.0));
            return false;
        }
        hunt_at_ = c + ms(1.0);
        if (fax_header(c)) {
            const SstvMode* fax = sstv_mode_by_name("FAX480");
            Image image;
            image.mode = fax;
            image.start = c;
            image.nominal = ms(fax->period_ms());
            image.offset_hz = mean(c - ms(1000.0), c) - kSstvLeaderHz;
            image.fit_start = image.start;
            image.fit_period = image.nominal;
            image_ = std::move(image);
            scans_.assign(3, std::vector<std::vector<std::uint8_t>>(
                                 1, std::vector<std::uint8_t>(fax->width, 0)));
            ++stats_.images;
            stats_.last_offset_hz = image_->offset_hz;
            const double delay = static_cast<double>(taps_.size() / 2);
            const double input = std::max(0.0, c * decimation_ - delay);
            sink.open(static_cast<std::uint64_t>(input), fax->width, ImagePixels::Gray,
                      fax->line_rate_hz(), fax->name);
            return true;
        }
        const double lead = mean(c - ms(260.0), c - ms(20.0));
        if (std::abs(lead - kSstvLeaderHz) > 150.0) {
            continue;
        }
        const double start = mean(c + ms(5.0), c + ms(25.0));
        const double drop = lead - start;
        if (drop < 550.0 || drop > 850.0) {
            continue;
        }
        // The edge: where the leader gives way to the start bit.
        double s = c;
        double best = -1.0e9;
        for (double q = c - ms(15.0); q <= c + ms(15.0); q += 1.0) {
            const double d = mean(q - ms(5.0), q) - mean(q, q + ms(5.0));
            if (d > best) {
                best = d;
                s = q;
            }
        }
        const double offset = mean(s - ms(250.0), s - ms(10.0)) - kSstvLeaderHz;
        int code = 0;
        int ones = 0;
        bool shaped = true;
        for (int k = 0; k < 8; ++k) {
            const double a = s + ms(kVisBitMs * (k + 1) + 5.0);
            const double hz = mean(a, a + ms(kVisBitMs - 10.0)) - offset;
            shaped = shaped && hz > 1000.0 && hz < 1400.0;
            const int bit = hz < 1200.0 ? 1 : 0;
            ones += bit;
            if (k < 7) {
                code |= bit << k;
            }
        }
        const double stop = mean(s + ms(kVisBitMs * 9 + 5.0), s + ms(kVisBitMs * 10 - 5.0)) - offset;
        shaped = shaped && std::abs(stop - kSstvSyncHz) < 150.0;
        // Even parity over the seven bits and the parity bit together.
        if (!shaped || (ones & 1) != 0) {
            continue;
        }
        ++stats_.vis_codes;
        stats_.last_vis = code;
        stats_.last_offset_hz = offset;
        const SstvMode* mode = sstv_mode_by_vis(code);
        const double after = s + ms(kVisBitMs * 10.0);
        if (mode == nullptr) {
            ++stats_.unknown_vis;
            hunt_at_ = after;
            continue;
        }
        Image image;
        image.mode = mode;
        image.start = after + ms(mode->lead_ms());
        image.nominal = ms(mode->period_ms());
        image.offset_hz = offset;
        image.fit_start = image.start;
        image.fit_period = image.nominal;
        image_ = std::move(image);
        scans_.assign(3, std::vector<std::vector<std::uint8_t>>(
                             static_cast<std::size_t>(mode->lines_per_period),
                             std::vector<std::uint8_t>(mode->width, 0)));
        ++stats_.images;
        const double delay = static_cast<double>(taps_.size() / 2);
        const double input = std::max(0.0, after * decimation_ - delay);
        sink.open(static_cast<std::uint64_t>(input), mode->width,
                  mode->colour == SstvColour::Gray ? ImagePixels::Gray : ImagePixels::Rgb,
                  mode->line_rate_hz(), mode->name);
        return true;
    }
}

double SstvDecoder::mean_deviation(double from, double to) const {
    const auto index = [&](double at) {
        const double rel = std::clamp(at - static_cast<double>(base_), 0.0,
                                      static_cast<double>(freq_.size()));
        return static_cast<std::size_t>(rel);
    };
    const std::size_t a = index(from);
    const std::size_t b = std::max(a + 1, index(to));
    if (b >= dev_sum_.size()) {
        return 0.0;
    }
    return (dev_sum_[b] - dev_sum_[a]) / static_cast<double>(b - a);
}

// "FAX480 mode", HEADER: 2.05 ms of 2300 Hz and 2.05 ms of 1500 Hz, 1220
// times. `at` is taken as the start of the first phasing sync when the second
// before it is that header, centred on 1900 Hz and spread about 400 Hz either
// side, and the 5 ms after it sits at 1200 Hz.
bool SstvDecoder::fax_header(double at) {
    if (at - ms(1000.0) < static_cast<double>(base_)) {
        return false;
    }
    const double centre = mean(at - ms(1000.0), at);
    if (std::abs(centre - kSstvLeaderHz) > 150.0) {
        return false;
    }
    // An ideal header gives -(400 Hz)^2; the filter's edges take some off.
    if (mean_deviation(at - ms(1000.0), at) > -60'000.0) {
        return false;
    }
    const double offset = centre - kSstvLeaderHz;
    return mean(at + ms(0.5), at + ms(4.5)) - offset < kSstvSyncHz + 150.0 &&
           mean(at - ms(4.5), at - ms(0.5)) - offset > kSstvSyncHz + 150.0;
}

double SstvDecoder::fit(double period) const {
    return image_->fit_start + period * image_->fit_period;
}

// A straight line through the syncs found. Early on, the nominal period and
// the median offset, so one false sync cannot tilt the line; once enough are
// in, least squares with outliers dropped, the slope held within 1 % of
// nominal. The slope is the slant correction.
void SstvDecoder::refit() {
    Image& im = *image_;
    const auto& pts = im.syncs;
    if (pts.empty()) {
        return;
    }
    const auto line_fit = [](const std::vector<std::pair<double, double>>& p, double nominal,
                             double& start, double& period) {
        double sk = 0, st = 0, skk = 0, skt = 0;
        for (const auto& [k, t] : p) {
            sk += k;
            st += t;
            skk += k * k;
            skt += k * t;
        }
        const double n = static_cast<double>(p.size());
        const double den = n * skk - sk * sk;
        period = den > 0.0 ? (n * skt - sk * st) / den : nominal;
        period = std::clamp(period, nominal * 0.99, nominal * 1.01);
        start = (st - period * sk) / n;
    };
    const double span = pts.back().first - pts.front().first;
    if (pts.size() < 6 || span < 6.0) {
        std::vector<double> offsets;
        for (const auto& [k, t] : pts) {
            offsets.push_back(t - k * im.nominal);
        }
        std::ranges::sort(offsets);
        const double median = offsets[offsets.size() / 2];
        double total = 0.0;
        int used = 0;
        for (const double o : offsets) {
            if (std::abs(o - median) < ms(3.0)) {
                total += o;
                ++used;
            }
        }
        im.fit_period = im.nominal;
        im.fit_start = total / used;
        return;
    }
    double start = 0.0;
    double period = 0.0;
    line_fit(pts, im.nominal, start, period);
    std::vector<std::pair<double, double>> inliers;
    for (const auto& p : pts) {
        if (std::abs(p.second - (start + p.first * period)) < ms(2.0)) {
            inliers.push_back(p);
        }
    }
    if (inliers.size() >= 6) {
        line_fit(inliers, im.nominal, start, period);
    }
    im.fit_start = start;
    im.fit_period = period;
    stats_.last_period_ratio = period / im.nominal;
}

bool SstvDecoder::step_image(ImageSink& sink, bool at_end) {
    Image& im = *image_;
    const SstvMode& mode = *im.mode;
    const std::uint32_t periods = mode.periods();
    const double window = std::max(ms(4.0), 0.01 * im.nominal);
    const double length = ms(mode.sync_ms);

    // A period goes out as soon as the sync after it has been looked for, so
    // the fit it is read with already knows where it ends, and so the fit
    // used for each period is the same however the audio arrived.
    const auto emit_ready = [&] {
        while (im.next_emit < periods) {
            const bool searched_next =
                at_end || im.next_emit + 1 < im.next_search || im.next_search == periods;
            const bool have_data = fit(im.next_emit + 1) + 1.0 <= end();
            if (!searched_next || !have_data) {
                break;
            }
            emit_period(im.next_emit, sink);
            ++im.next_emit;
        }
    };

    for (;;) {
        emit_ready();
        if (im.next_search >= periods) {
            break;
        }
        const double p = fit(im.next_search) + ms(mode.sync_offset_ms);
        if (p + window + 1.25 * length + 2.0 > end()) {
            break;
        }
        double best = -1.0e9;
        double at = p;
        for (double q = std::floor(p - window); q <= p + window; q += 1.0) {
            const double around =
                0.5 * (mean(q - 0.25 * length, q) + mean(q + length, q + 1.25 * length));
            const double contrast = around - mean(q, q + length);
            if (contrast > best) {
                best = contrast;
                at = q;
            }
        }
        const double inside = mean(at, at + length) - im.offset_hz;
        if (best > 120.0 && inside < kSstvSyncHz + 200.0) {
            im.syncs.emplace_back(static_cast<double>(im.next_search),
                                  at - ms(mode.sync_offset_ms));
            ++stats_.syncs_found;
            im.misses_in_row = 0;
            refit();
        } else {
            ++stats_.syncs_missed;
            ++im.misses_in_row;
        }
        ++im.next_search;
        if (im.misses_in_row >= 16) {
            // The transmission has gone; the periods before the silence are
            // already out.
            hunt_at_ = fit(im.next_search);
            finish(sink, false);
            return true;
        }
    }
    emit_ready();
    if (im.next_emit == periods) {
        hunt_at_ = fit(periods);
        finish(sink, true);
        return true;
    }
    trim(fit(im.next_emit) - ms(50.0));
    return false;
}

void SstvDecoder::emit_period(std::uint32_t period, ImageSink& sink) {
    const Image& im = *image_;
    const SstvMode& mode = *im.mode;
    if (period < static_cast<std::uint32_t>(mode.skip_periods)) {
        return;
    }
    const double base = fit(period);
    const double scale = im.fit_period / im.nominal;
    double offset_ms = 0.0;
    for (const SstvSegment& seg : mode.period) {
        if (seg.scan()) {
            const bool chroma = mode.colour == SstvColour::Ycc && seg.channel != kY;
            auto& line = scans_[static_cast<std::size_t>(seg.channel)]
                               [static_cast<std::size_t>(chroma ? 0 : seg.line)];
            const double pixel_ms = seg.ms / mode.width;
            for (std::uint32_t i = 0; i < mode.width; ++i) {
                const double a = base + scale * ms(offset_ms + i * pixel_ms);
                const double b = base + scale * ms(offset_ms + (i + 1) * pixel_ms);
                line[i] = level(mean(a, b) - im.offset_hz);
            }
        }
        offset_ms += seg.ms;
    }
    const std::uint32_t lpp = static_cast<std::uint32_t>(mode.lines_per_period);
    const std::uint32_t first = (period - static_cast<std::uint32_t>(mode.skip_periods)) * lpp;
    for (std::uint32_t l = 0; l < lpp; ++l) {
        const std::uint32_t row = first + l;
        if (row >= mode.height) {
            break;
        }
        if (mode.colour == SstvColour::Gray) {
            sink.row(row, scans_[0][l]);
            continue;
        }
        row_.resize(static_cast<std::size_t>(mode.width) * 3);
        for (std::uint32_t i = 0; i < mode.width; ++i) {
            if (mode.colour == SstvColour::Rgb) {
                row_[3 * i] = scans_[kR][0][i];
                row_[3 * i + 1] = scans_[kG][0][i];
                row_[3 * i + 2] = scans_[kB][0][i];
            } else {
                const SstvRgb c = sstv_ycc_to_rgb(scans_[kY][l][i], scans_[kRy][0][i],
                                                  scans_[kBy][0][i]);
                row_[3 * i] = clamp_byte(c.r);
                row_[3 * i + 1] = clamp_byte(c.g);
                row_[3 * i + 2] = clamp_byte(c.b);
            }
        }
        sink.row(row, row_);
    }
}

void SstvDecoder::finish(ImageSink& sink, bool complete) {
    sink.close(complete);
    if (complete) {
        ++stats_.images_complete;
    }
    image_.reset();
}

}  // namespace revenant::decode
