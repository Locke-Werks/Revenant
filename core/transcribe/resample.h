// A receiver's audio, at whatever rate it runs, to the rate the recogniser
// takes.
//
// WHAT IT HAS TO TAKE. A digital voice receiver's decoded voice at 8000 S/s,
// and an analogue receiver's audio at the engine's audio rate, which is
// anything from 8000 to 192000 and need not divide 16000: 44100 is
// 160/441 of the way down and 171000 is 16/171. So the ratio is general,
// computed from the two integer rates, and kept as an integer position so a
// stream hours long does not drift by one sample.
//
// THE FILTER IS SIZED FOR SPEECH, NOT FOR HIGH FIDELITY. Whisper's log-mel
// front end works on a 16 kHz STFT and its filterbank stops at 8 kHz, half the
// rate, so nothing above 8 kHz reaches the model and nothing between 6.5 and
// 8 kHz carries much speech either. The passband is kept to 0.8125 of the
// lower Nyquist (6.5 kHz at 16000 out, 3.25 kHz from an 8000 S/s vocoder) and
// the stopband starts AT that Nyquist, so going down nothing folds back into
// the band at all, and going up the images of everything in the passband are
// in the stopband. A 1.5 kHz transition is what keeps the filter short: 644
// taps at 192000 in, 148 at 44100, 54 at 8000.
//
// KAISER-WINDOWED SINC, 80 dB. The window's beta and the length for a given
// attenuation and transition width are Kaiser's empirical formulas, as given
// in Oppenheim and Schafer, Discrete-Time Signal Processing, 3rd ed., section
// 7.6. 80 dB puts the strongest out-of-band tone a receiver's audio carries,
// an FM stereo pilot at 19 kHz some 20 dB under the programme, about 100 dB
// under the programme, below the 96 dB a 16-bit recording can represent.
//
// POLYPHASE, EXACT WHERE IT CAN BE. The output instants fall at L distinct
// fractional offsets between input samples, L = 16000 / gcd(rate, 16000):
// 2 for 8000, 1 for 48000, 160 for 44100, 16 for 171000. When L is at most
// kResampleMaxPhases every offset gets its own row of taps and nothing is
// interpolated. A rate with more offsets than that, 22050 or a rate that
// shares little with 16000, interpolates linearly between the two nearest of
// 256 rows, which keeps the table under 700 KB at the longest filter.
//
// MEASURED 2026-10-03 by the hidden [measure] case in
// tests/transcribe/test_resample.cpp, tones every 50 Hz across the passband
// and every 100 Hz across the stopband: passband ripple within 0.001 dB at
// every exact-phase rate from 8000 to 192000, 0.0021 dB at 11025 and
// 0.0009 dB at 22050, the two interpolated ones; nothing at 8 kHz or above
// comes out higher than -81.5 dB. Ten seconds at 192000 took 29 ms to
// resample in an optimised build, 644 multiply-adds per output sample.
//
// ZERO DELAY BY CONSTRUCTION. Output n is the input's value at time
// n * rate / 16000 input samples, exactly, so a stretch of input from sample
// a to b becomes output from a * 16000 / rate to b * 16000 / rate with no
// group delay to correct for. The price is that the last half-filter of
// output waits for input that has not arrived, which finish() supplies as
// zeros when the stretch ends.
//
// STATEFUL ACROSS CALLS, AND THE BLOCKING DOES NOT SHOW. The same samples go
// through the same dot product in the same order whatever pieces they
// arrived in, so a stream resampled in pieces is bit-identical to the stream
// resampled whole. tests/transcribe/test_resample.cpp holds it to that.
//
// NO ALLOCATION AFTER configure(). The work buffer is the filter length plus
// kResampleBlock, reserved once; process() appends to the caller's vector,
// which is the only thing that grows.
//
// EQUAL RATES PASS STRAIGHT THROUGH. Nothing folds at 16000 in, and the model
// reads up to 8 kHz anyway, so the copy is exact and free.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <span>
#include <vector>

#include "core/transcribe/utterance.h"

namespace revenant::transcribe {

// Stopband attenuation the filter is designed for, in dB.
inline constexpr double kResampleStopbandDb = 80.0;

// The passband edge as a fraction of the lower of the two Nyquist
// frequencies. The stopband edge is that Nyquist itself.
inline constexpr double kResamplePassbandFraction = 0.8125;

// The most rows the phase table gets before it interpolates between them.
inline constexpr std::uint32_t kResampleMaxPhases = 256;

// Input samples taken into the work buffer per pass.
inline constexpr std::size_t kResampleBlock = 4096;

class Resampler {
public:
    Resampler() = default;

    explicit Resampler(std::uint32_t in_rate, std::uint32_t out_rate = kRecogniserRateHz)
    {
        configure(in_rate, out_rate);
    }

    // Designs the filter for these rates and resets. Allocates; everything
    // after it does not. A rate of zero leaves the resampler producing
    // nothing.
    void configure(std::uint32_t in_rate, std::uint32_t out_rate = kRecogniserRateHz)
    {
        in_rate_ = in_rate;
        out_rate_ = out_rate;
        table_.clear();
        buf_.clear();
        if (in_rate == 0 || out_rate == 0) {
            taps_ = 0;
            half_ = 0;
            reset();
            return;
        }

        step_int_ = in_rate / out_rate;
        step_rem_ = in_rate % out_rate;

        if (in_rate == out_rate) {
            pass_hz_ = in_rate / 2.0;
            stop_hz_ = pass_hz_;
            taps_ = 0;
            half_ = 0;
            phases_ = 1;
            exact_ = true;
            reset();
            return;
        }

        const std::uint32_t common = std::gcd(in_rate, out_rate);
        const std::uint32_t distinct = out_rate / common;
        exact_ = distinct <= kResampleMaxPhases;
        phases_ = exact_ ? distinct : kResampleMaxPhases;

        const double nyquist = std::min(in_rate, out_rate) / 2.0;
        stop_hz_ = nyquist;
        pass_hz_ = kResamplePassbandFraction * nyquist;

        // Kaiser's length estimate, in input samples: the transition width
        // in radians per input sample sets it, not the output rate.
        const double transition = 2.0 * std::numbers::pi * (stop_hz_ - pass_hz_) / in_rate;
        const double order = (kResampleStopbandDb - 7.95) / (2.285 * transition);
        half_ = std::max<std::size_t>(2, static_cast<std::size_t>(std::ceil(order / 2.0)));
        taps_ = 2 * half_;

        const double beta = 0.1102 * (kResampleStopbandDb - 8.7);
        const double i0_beta = bessel_i0(beta);

        // Cut-off in the middle of the transition, in cycles per input
        // sample, so the windowed sinc's own transition is centred on it.
        const double cutoff = (pass_hz_ + stop_hz_) / 2.0 / in_rate;

        // One row per fractional offset f in [0, 1], the last being 1 so the
        // interpolating path can always read row k + 1. Tap j of a row
        // multiplies input sample (i - half + 1 + j) for an output at i + f,
        // which sits d = j - half + 1 - f samples from it.
        table_.assign(static_cast<std::size_t>(phases_ + 1) * taps_, 0.0F);
        const double half = static_cast<double>(half_);
        std::vector<double> exact(taps_);
        for (std::uint32_t p = 0; p <= phases_; ++p) {
            const double f = static_cast<double>(p) / phases_;
            float* row = table_.data() + static_cast<std::size_t>(p) * taps_;
            double sum = 0.0;
            for (std::size_t j = 0; j < taps_; ++j) {
                const double d = static_cast<double>(j) - half + 1.0 - f;
                const double x = d / half;
                const double window =
                    std::abs(x) >= 1.0 ? 1.0 / i0_beta : bessel_i0(beta * std::sqrt(1.0 - x * x)) / i0_beta;
                const double arg = 2.0 * cutoff * d;
                const double sinc =
                    arg == 0.0 ? 1.0 : std::sin(std::numbers::pi * arg) / (std::numbers::pi * arg);
                exact[j] = 2.0 * cutoff * sinc * window;
                sum += exact[j];
            }
            // Every row to unity gain at DC. Left alone the rows differ in
            // the fifth decimal, and that difference is a ripple at the
            // pattern's own period on anything with a DC term.
            for (std::size_t j = 0; j < taps_; ++j) {
                row[j] = static_cast<float>(exact[j] / sum);
            }
        }

        buf_.reserve(taps_ + kResampleBlock);
        reset();
    }

    // Forgets every sample seen, as though the stream started now. Keeps the
    // filter.
    void reset()
    {
        pos_int_ = 0;
        pos_rem_ = 0;
        consumed_ = 0;
        produced_ = 0;
        skip_ = 0;
        if (taps_ == 0) {
            buf_.clear();
            base_ = 0;
            return;
        }
        // Before the first sample the stream is silence, so the first output
        // reads half a filter of zeros on its left.
        buf_.assign(half_ - 1, 0.0F);
        base_ = -static_cast<std::int64_t>(half_ - 1);
    }

    // Appends to `out` every output sample whose input is all here.
    void process(std::span<const float> in, std::vector<float>& out)
    {
        if (in_rate_ == 0 || out_rate_ == 0) {
            return;
        }
        consumed_ += in.size();
        if (taps_ == 0) {
            out.insert(out.end(), in.begin(), in.end());
            produced_ += in.size();
            return;
        }
        while (!in.empty()) {
            const std::size_t taken = take(in.data(), in.size());
            in = in.subspan(taken);
            produce(out, UINT64_MAX);
            compact();
        }
    }

    // Appends the outputs still owed for the input so far, reading zeros
    // past its end, so that N inputs since the last reset have made exactly
    // ceil(N * out_rate / in_rate) outputs. Then resets.
    void finish(std::vector<float>& out)
    {
        if (in_rate_ == 0 || out_rate_ == 0) {
            return;
        }
        if (taps_ != 0) {
            const std::uint64_t owed = (consumed_ * out_rate_ + in_rate_ - 1) / in_rate_;
            while (produced_ < owed) {
                static_cast<void>(take(nullptr, half_ + 1));
                produce(out, owed);
                compact();
            }
        }
        reset();
    }

    [[nodiscard]] std::uint32_t in_rate() const { return in_rate_; }
    [[nodiscard]] std::uint32_t out_rate() const { return out_rate_; }

    // Taps per output sample, zero when passing straight through.
    [[nodiscard]] std::size_t taps() const { return taps_; }

    // Rows in the phase table, and whether every output instant has its own.
    [[nodiscard]] std::uint32_t phases() const { return phases_; }
    [[nodiscard]] bool exact_phases() const { return exact_; }

    [[nodiscard]] double passband_hz() const { return pass_hz_; }
    [[nodiscard]] double stopband_hz() const { return stop_hz_; }

    // Inputs taken and outputs made since the last reset.
    [[nodiscard]] std::uint64_t consumed() const { return consumed_; }
    [[nodiscard]] std::uint64_t produced() const { return produced_; }

private:
    static double bessel_i0(double x)
    {
        // The power series; at beta near 8 it has converged to double
        // precision in about thirty terms.
        double sum = 1.0;
        double term = 1.0;
        const double quarter = x * x / 4.0;
        for (int k = 1; k < 200; ++k) {
            term *= quarter / (static_cast<double>(k) * k);
            sum += term;
            if (term < sum * 1e-17) {
                break;
            }
        }
        return sum;
    }

    static float dot(const float* a, const float* b, std::size_t n)
    {
        // Four running sums so the multiplies are not one serial chain. The
        // order is fixed, which is what makes the blocking invisible.
        float s0 = 0.0F;
        float s1 = 0.0F;
        float s2 = 0.0F;
        float s3 = 0.0F;
        std::size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            s0 += a[i] * b[i];
            s1 += a[i + 1] * b[i + 1];
            s2 += a[i + 2] * b[i + 2];
            s3 += a[i + 3] * b[i + 3];
        }
        for (; i < n; ++i) {
            s0 += a[i] * b[i];
        }
        return (s0 + s1) + (s2 + s3);
    }

    // Moves up to `count` samples into the buffer, zeros when `source` is
    // null, first discarding any the next output has already passed. Returns
    // how many it took, skipped ones included.
    std::size_t take(const float* source, std::size_t count)
    {
        const auto skipped = static_cast<std::size_t>(std::min<std::uint64_t>(skip_, count));
        skip_ -= skipped;
        const std::size_t room = buf_.capacity() - buf_.size();
        const std::size_t moved = std::min(count - skipped, room);
        if (source == nullptr) {
            buf_.insert(buf_.end(), moved, 0.0F);
        } else {
            buf_.insert(buf_.end(), source + skipped, source + skipped + moved);
        }
        return skipped + moved;
    }

    // Makes every output whose last tap is in the buffer, up to `limit`
    // outputs since the reset.
    void produce(std::vector<float>& out, std::uint64_t limit)
    {
        const std::int64_t available = base_ + static_cast<std::int64_t>(buf_.size());
        const auto half = static_cast<std::int64_t>(half_);
        while (produced_ < limit && pos_int_ + half < available) {
            const std::int64_t first = pos_int_ - half + 1;
            const float* x = buf_.data() + (first - base_);
            const std::uint64_t scaled = pos_rem_ * phases_;
            const std::uint64_t row = scaled / out_rate_;
            const std::uint64_t frac = scaled % out_rate_;
            const float* taps = table_.data() + row * taps_;
            float y = dot(x, taps, taps_);
            if (frac != 0) {
                const float next = dot(x, taps + taps_, taps_);
                const float mu = static_cast<float>(static_cast<double>(frac) / out_rate_);
                y += mu * (next - y);
            }
            out.push_back(y);
            ++produced_;
            pos_int_ += static_cast<std::int64_t>(step_int_);
            pos_rem_ += step_rem_;
            if (pos_rem_ >= out_rate_) {
                pos_rem_ -= out_rate_;
                ++pos_int_;
            }
        }
    }

    // Drops the samples no future output reads. When the next output starts
    // past the end of what is buffered, the gap is skipped as it arrives.
    void compact()
    {
        const std::int64_t keep_from = pos_int_ - static_cast<std::int64_t>(half_) + 1;
        const std::int64_t drop = keep_from - base_;
        if (drop <= 0) {
            return;
        }
        const auto held = static_cast<std::int64_t>(buf_.size());
        if (drop >= held) {
            skip_ += static_cast<std::uint64_t>(drop - held);
            buf_.clear();
        } else {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(drop));
        }
        base_ += drop;
    }

    std::uint32_t in_rate_ = 0;
    std::uint32_t out_rate_ = 0;
    std::uint32_t step_int_ = 0;
    std::uint32_t step_rem_ = 0;
    std::uint32_t phases_ = 1;
    bool exact_ = true;
    double pass_hz_ = 0.0;
    double stop_hz_ = 0.0;
    std::size_t half_ = 0;
    std::size_t taps_ = 0;
    std::vector<float> table_;

    // Input samples [base_, base_ + buf_.size()) of the stream since reset,
    // negative indices being the silence before it.
    std::vector<float> buf_;
    std::int64_t base_ = 0;
    std::uint64_t skip_ = 0;

    // The next output's instant, pos_int_ + pos_rem_ / out_rate_ in input
    // samples.
    std::int64_t pos_int_ = 0;
    std::uint64_t pos_rem_ = 0;

    std::uint64_t consumed_ = 0;
    std::uint64_t produced_ = 0;
};

}  // namespace revenant::transcribe
