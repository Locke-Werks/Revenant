// CVSD per MIL-STD-188-113 section 5.2. See cvsd.h for the document, the
// clauses used and the choices the document leaves open.

#include "core/decode/cvsd.h"

#include <cmath>
#include <format>
#include <numbers>

namespace revenant::decode {

namespace {

bool supported(std::uint32_t rate) { return rate == kCvsdRate16k || rate == kCvsdRate32k; }

std::size_t factor(std::uint32_t rate) { return rate / kCvsdPcmRateHz; }

// Header note 3: a Hamming-windowed sinc lowpass at 3.4 kHz, eight 8 kHz
// sample periods long, at the bit rate. Unity gain at DC.
std::vector<double> lowpass_taps(std::uint32_t rate) {
    const std::size_t n = 8 * factor(rate) * 2 + 1;
    const double fc = 3400.0 / static_cast<double>(rate);
    std::vector<double> h(n);
    double sum = 0.0;
    const double mid = static_cast<double>(n - 1) / 2.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) - mid;
        const double sinc = t == 0.0 ? 2.0 * fc : std::sin(2.0 * std::numbers::pi * fc * t) / (std::numbers::pi * t);
        const double w = 0.54 - 0.46 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                                static_cast<double>(n - 1));
        h[i] = sinc * w;
        sum += h[i];
    }
    for (double& v : h) v /= sum;
    return h;
}

// Header note 2: V_min for unit V_min, measured by decoding the 30-percent
// pattern, then scaled so that pattern lands on kCvsdZeroDbm0Peak. The loop
// is linear in V_min once the bits are fixed, so one measurement is exact.
double calibrated_v_min(std::uint32_t rate);

}  // namespace

std::vector<std::uint8_t> cvsd_reference_pattern(std::uint32_t bit_rate, int run_of_threes) {
    // Table VI, hexadecimal, "the order of digital pattern bit input to the
    // decoder shall be from left to right".
    const char* hex = nullptr;
    if (bit_rate == kCvsdRate16k && run_of_threes == 0) hex = "DB492";
    if (bit_rate == kCvsdRate32k && run_of_threes == 0) hex = "DB54924AB6";
    if (bit_rate == kCvsdRate16k && run_of_threes == 30) hex = "FB412";
    if (bit_rate == kCvsdRate32k && run_of_threes == 30) hex = "FDAA10255E";
    std::vector<std::uint8_t> bits;
    if (hex == nullptr) return bits;
    for (const char* p = hex; *p != '\0'; ++p) {
        const int v = (*p >= 'A') ? (*p - 'A' + 10) : (*p - '0');
        for (int b = 3; b >= 0; --b) bits.push_back(static_cast<std::uint8_t>((v >> b) & 1));
    }
    return bits;
}

// ---------------------------------------------------------------------------
// The shared loop
// ---------------------------------------------------------------------------

CvsdLoop::CvsdLoop(std::uint32_t bit_rate) : bit_rate_(bit_rate) {
    const double fs = static_cast<double>(supported(bit_rate) ? bit_rate : kCvsdRate16k);
    // 5.2.3.6 and 5.2.3.7: exponential, so a one-pole decay per clock.
    syllabic_decay_ = std::exp(-1.0 / (fs * kCvsdSyllabicSeconds));
    integrator_decay_ = std::exp(-1.0 / (fs * kCvsdIntegratorSeconds));
    v_min_ = 1.0;
    // Header note 1: V_min + 0.30 (V_max - V_min) = 16 V_min.
    v_max_ = v_min_ * (1.0 + (kCvsdCompressionRatio - 1.0) / 0.30);
    if (supported(bit_rate)) {
        const double v = calibrated_v_min(bit_rate);
        v_max_ *= v;
        v_min_ = v;
    }
    reset();
}

void CvsdLoop::reset() {
    shift_ = 0;
    bits_seen_ = 0;
    overload_ = false;
    syllabic_ = v_min_;
    integrator_ = 0.0;
}

double CvsdLoop::clock(std::uint8_t bit) {
    // 5.2.3.4: a 3-bit shift register and XYZ + X'Y'Z', true for the clock
    // following three like bits.
    shift_ = static_cast<std::uint8_t>(((shift_ << 1U) | (bit & 1U)) & 0x7U);
    if (bits_seen_ < 3) ++bits_seen_;
    overload_ = bits_seen_ >= 3 && (shift_ == 0x7U || shift_ == 0x0U);

    // 5.2.3.6: charge toward V_max while the algorithm output is true,
    // discharge toward V_min while it is false.
    const double target = overload_ ? v_max_ : v_min_;
    syllabic_ = target + (syllabic_ - target) * syllabic_decay_;

    // Figure 2 and Appendix C 50.6, 50.7: the PAM takes its amplitude from
    // the syllabic filter and its sign from the newest bit, and feeds the
    // leaky reconstruction integrator.
    const double pam = (bit & 1U) ? syllabic_ : -syllabic_;
    integrator_ = integrator_ * integrator_decay_ + pam;
    return integrator_;
}

namespace {

double calibrated_v_min(std::uint32_t rate) {
    // The same arithmetic as CvsdLoop::clock() with unit V_min. Constructing a
    // CvsdLoop here would recurse into this calibration.
    const double fs = static_cast<double>(rate);
    const double sd = std::exp(-1.0 / (fs * kCvsdSyllabicSeconds));
    const double id = std::exp(-1.0 / (fs * kCvsdIntegratorSeconds));
    const double v_min = 1.0;
    const double v_max = v_min * (1.0 + (kCvsdCompressionRatio - 1.0) / 0.30);

    const auto pattern = cvsd_reference_pattern(rate, 30);
    const auto taps = lowpass_taps(rate);
    std::uint8_t shift = 0;
    double syl = v_min;
    double integ = 0.0;
    std::vector<double> y;
    const std::size_t total = static_cast<std::size_t>(fs * 0.4);
    y.reserve(total);
    for (std::size_t i = 0; i < total; ++i) {
        const std::uint8_t bit = pattern[i % pattern.size()];
        shift = static_cast<std::uint8_t>(((shift << 1U) | bit) & 0x7U);
        const bool ov = i >= 2 && (shift == 0x7U || shift == 0x0U);
        const double target = ov ? v_max : v_min;
        syl = target + (syl - target) * sd;
        integ = integ * id + (bit ? syl : -syl);
        y.push_back(integ);
    }
    // RMS of the lowpassed output over the last 0.2 s, against the sine.
    double acc = 0.0;
    std::size_t count = 0;
    for (std::size_t i = total / 2; i < total; ++i) {
        double v = 0.0;
        for (std::size_t t = 0; t < taps.size(); ++t) v += taps[t] * y[i - t];
        acc += v * v;
        ++count;
    }
    const double rms = std::sqrt(acc / static_cast<double>(count));
    return (kCvsdZeroDbm0Peak / std::numbers::sqrt2) / rms;
}

}  // namespace

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

CvsdDecoder::CvsdDecoder(std::uint32_t bit_rate)
    : bit_rate_(bit_rate), loop_(bit_rate) {
    if (supported(bit_rate)) taps_ = lowpass_taps(bit_rate);
    reset();
}

void CvsdDecoder::reset() {
    loop_.reset();
    history_.assign(taps_.size(), 0.0);
    head_ = 0;
    phase_ = 0;
    duty_ = 0.0;
}

Status CvsdDecoder::decode(std::span<const std::uint8_t> bits, std::vector<float>& out) {
    if (!supported(bit_rate_)) {
        return fail(std::format("CVSD runs at 16000 or 32000 bit/s (MIL-STD-188-113 5.2.3.2), "
                                "not {}",
                                bit_rate_));
    }
    const std::size_t f = factor(bit_rate_);
    std::size_t overloads = 0;
    for (std::uint8_t b : bits) {
        const double y = loop_.clock(b);
        overloads += loop_.overload() ? 1U : 0U;
        history_[head_] = y;
        head_ = (head_ + 1) % history_.size();
        if (++phase_ == f) {
            phase_ = 0;
            double acc = 0.0;
            std::size_t idx = head_;
            for (std::size_t t = taps_.size(); t-- > 0;) {
                acc += taps_[t] * history_[idx];
                idx = (idx + 1) % history_.size();
            }
            out.push_back(static_cast<float>(acc));
        }
    }
    duty_ = bits.empty() ? 0.0 : static_cast<double>(overloads) / static_cast<double>(bits.size());
    return {};
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

CvsdEncoder::CvsdEncoder(std::uint32_t bit_rate) : bit_rate_(bit_rate), loop_(bit_rate) {
    if (supported(bit_rate)) taps_ = lowpass_taps(bit_rate);
    reset();
}

void CvsdEncoder::reset() {
    loop_.reset();
    history_.assign(taps_.size(), 0.0);
    head_ = 0;
    duty_ = 0.0;
}

Status CvsdEncoder::encode(std::span<const float> pcm, std::vector<std::uint8_t>& bits) {
    if (!supported(bit_rate_)) {
        return fail(std::format("CVSD runs at 16000 or 32000 bit/s (MIL-STD-188-113 5.2.3.2), "
                                "not {}",
                                bit_rate_));
    }
    const std::size_t f = factor(bit_rate_);
    std::size_t overloads = 0;
    std::size_t clocks = 0;
    for (float sample : pcm) {
        for (std::size_t k = 0; k < f; ++k) {
            // Zero-stuffed interpolation to the bit rate (header note 3).
            history_[head_] = k == 0 ? static_cast<double>(sample) * static_cast<double>(f) : 0.0;
            head_ = (head_ + 1) % history_.size();
            double x = 0.0;
            std::size_t idx = head_;
            for (std::size_t t = taps_.size(); t-- > 0;) {
                x += taps_[t] * history_[idx];
                idx = (idx + 1) % history_.size();
            }
            // Figure 2A: the comparator against the integrator's output.
            const std::uint8_t bit = x > loop_.integrator() ? 1U : 0U;
            loop_.clock(bit);
            overloads += loop_.overload() ? 1U : 0U;
            ++clocks;
            bits.push_back(bit);
        }
    }
    duty_ = clocks == 0 ? 0.0 : static_cast<double>(overloads) / static_cast<double>(clocks);
    return {};
}

}  // namespace revenant::decode
