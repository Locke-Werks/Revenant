#include "core/decode/tone_frontend.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace revenant::decode {
namespace {

constexpr double kPi = std::numbers::pi;

// The largest divisor of rate that leaves at least minimum samples per second.
std::size_t choose_decimation(SampleRate rate, SampleRate minimum) {
    std::size_t best = 1;
    for (SampleRate d = 1; d <= rate / minimum; ++d) {
        if (rate % d == 0) {
            best = static_cast<std::size_t>(d);
        }
    }
    return best;
}

Complex32 raise(Complex32 value, unsigned power) {
    std::complex<double> v{value.real(), value.imag()};
    std::complex<double> result{1.0, 0.0};
    for (unsigned i = 0; i < power; ++i) {
        result *= v;
    }
    return Complex32{static_cast<float>(result.real()), static_cast<float>(result.imag())};
}

double line_magnitude(const std::vector<Complex32>& raised, double hertz, double rate) {
    // A recurrence rather than a cos and sin per sample, renormalised every
    // step because a unit phasor multiplied a thousand times drifts off the
    // unit circle.
    const std::complex<double> step = std::polar(1.0, -2.0 * kPi * hertz / rate);
    std::complex<double> phasor{1.0, 0.0};
    std::complex<double> sum{0.0, 0.0};
    for (const Complex32 value : raised) {
        sum += std::complex<double>{value.real(), value.imag()} * phasor;
        phasor *= step;
        phasor /= std::abs(phasor);
    }
    return std::abs(sum);
}

}  // namespace

Expected<std::vector<float>> design_lowpass(SampleRate rate, double cutoff_hz, std::size_t taps) {
    if (rate <= 0) {
        return fail(std::format("design_lowpass needs a positive rate; got {}", rate));
    }
    if (taps == 0 || taps % 2 == 0) {
        return fail(std::format("design_lowpass needs an odd tap count; got {}", taps));
    }
    if (!(cutoff_hz > 0.0) || cutoff_hz >= 0.5 * static_cast<double>(rate)) {
        return fail(std::format("design_lowpass needs a cutoff in (0, {}) Hz; got {}",
                                0.5 * static_cast<double>(rate), cutoff_hz));
    }
    const double fc = cutoff_hz / static_cast<double>(rate);
    const auto half = static_cast<std::ptrdiff_t>((taps - 1) / 2);
    std::vector<float> out(taps, 0.0F);
    double sum = 0.0;
    for (std::size_t i = 0; i < taps; ++i) {
        const auto n = static_cast<double>(static_cast<std::ptrdiff_t>(i) - half);
        const double sinc = (n == 0.0) ? 2.0 * fc : std::sin(2.0 * kPi * fc * n) / (kPi * n);
        const double window =
            0.54 - 0.46 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                   static_cast<double>(taps - 1));
        out[i] = static_cast<float>(sinc * window);
        sum += sinc * window;
    }
    for (float& tap : out) {
        tap = static_cast<float>(static_cast<double>(tap) / sum);
    }
    return out;
}

Expected<ToneFrontEnd> ToneFrontEnd::create(const ToneFrontEndConfig& config) {
    if (config.rate <= 0 || config.minimum_output_rate <= 0) {
        return fail(std::format("ToneFrontEnd needs positive rates; got {} and {}", config.rate,
                                config.minimum_output_rate));
    }
    if (config.minimum_output_rate > config.rate) {
        return fail(std::format(
            "ToneFrontEnd cannot decimate {} Hz audio to at least {} Hz; the output rate "
            "has to be at or below the input rate",
            config.rate, config.minimum_output_rate));
    }
    if (!(config.passband_hz > 0.0)) {
        return fail(std::format("ToneFrontEnd needs a positive passband; got {}",
                                config.passband_hz));
    }
    const double nyquist = 0.5 * static_cast<double>(config.rate);
    if (static_cast<double>(config.centre_hz) - config.passband_hz <= 0.0 ||
        static_cast<double>(config.centre_hz) + config.passband_hz >= nyquist) {
        return fail(std::format(
            "ToneFrontEnd needs the tone and its {} Hz either side inside the audio band "
            "(0, {}) Hz; a centre of {} Hz puts part of it outside, where the real input "
            "folds it back over itself",
            config.passband_hz, nyquist, config.centre_hz));
    }

    ToneFrontEnd front;
    front.config_ = config;
    front.decimation_ = choose_decimation(config.rate, config.minimum_output_rate);
    front.output_rate_ = config.rate / static_cast<SampleRate>(front.decimation_);

    const double out_nyquist = 0.5 * static_cast<double>(front.output_rate_);
    if (config.passband_hz >= out_nyquist) {
        return fail(std::format(
            "ToneFrontEnd's {} Hz passband does not fit under the {} Hz Nyquist frequency "
            "of the {} Hz output; raise minimum_output_rate",
            config.passband_hz, out_nyquist, front.output_rate_));
    }

    // Stopband at the output Nyquist frequency, so nothing aliases into the
    // passband and the noise that does alias lands where the decoder's own
    // filter removes it. The cutoff sits halfway through that transition.
    //
    // Tap count from the Hamming window's rule of thumb, about 3.3 times the
    // rate over the transition width, rounded up to odd. At 48 kHz and a
    // 500 Hz output that is a filter of the order of a thousand taps, which
    // costs a thousand multiplies per output sample and five hundred outputs
    // a second.
    const double transition = out_nyquist - config.passband_hz;
    auto taps_count = static_cast<std::size_t>(
        std::ceil(3.3 * static_cast<double>(config.rate) / transition));
    taps_count |= 1U;
    const double cutoff = config.passband_hz + 0.5 * transition;

    auto taps = design_lowpass(config.rate, cutoff, taps_count);
    if (!taps) {
        return std::unexpected(with_context(taps.error(), "designing the tone front end"));
    }
    front.taps_ = std::move(*taps);
    return front;
}

void ToneFrontEnd::reset() {
    buffer_.clear();
    buffer_start_ = 0;
    next_input_ = 0;
    next_output_ = 0;
}

void ToneFrontEnd::process(ConstRealSpan audio, std::vector<Complex32>& out) {
    if (taps_.empty()) {
        return;
    }
    const SampleRate rate = config_.rate;
    const auto centre = static_cast<std::uint64_t>(config_.centre_hz);

    // The phase of exp(-j*2*pi*f*n/rate) with f integer repeats every rate
    // samples, so reducing n*f modulo rate in integers keeps the argument
    // exact however long the stream runs.
    for (const float sample : audio) {
        const std::uint64_t cycle = (next_input_ % static_cast<std::uint64_t>(rate)) * centre %
                                    static_cast<std::uint64_t>(rate);
        const double angle =
            -2.0 * kPi * static_cast<double>(cycle) / static_cast<double>(rate);
        buffer_.push_back(Complex32{static_cast<float>(sample * std::cos(angle)),
                                    static_cast<float>(sample * std::sin(angle))});
        ++next_input_;
    }

    const std::size_t length = taps_.size();
    while (next_output_ < next_input_) {
        // Output at input index k needs inputs k-length+1 .. k. Before the
        // stream start those are zero.
        double real = 0.0;
        double imag = 0.0;
        for (std::size_t i = 0; i < length; ++i) {
            if (next_output_ < i) {
                break;
            }
            const SampleIndex index = next_output_ - i;
            if (index < buffer_start_) {
                break;
            }
            const Complex32 value = buffer_[static_cast<std::size_t>(index - buffer_start_)];
            real += static_cast<double>(taps_[i]) * value.real();
            imag += static_cast<double>(taps_[i]) * value.imag();
        }
        // The mixer halves the tone's amplitude, since a real cosine is two
        // phasors and only one of them is kept. Doubling here puts a tone of
        // amplitude A out at magnitude A, which is what every threshold
        // downstream is written against.
        out.push_back(Complex32{static_cast<float>(2.0 * real), static_cast<float>(2.0 * imag)});
        next_output_ += decimation_;
    }

    // Keep what the next output reaches back to.
    const SampleIndex keep_from = (next_output_ + 1 > length) ? next_output_ + 1 - length : 0;
    if (keep_from > buffer_start_) {
        const auto drop = static_cast<std::size_t>(
            std::min<SampleIndex>(keep_from - buffer_start_, buffer_.size()));
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(drop));
        buffer_start_ += drop;
    }
}

Expected<ToneEstimate> estimate_tone_offset(ConstComplexSpan samples, SampleRate rate,
                                            unsigned power, double max_offset_hz) {
    if (samples.size() < 16) {
        return fail(std::format("estimate_tone_offset needs at least 16 samples; got {}",
                                samples.size()));
    }
    if (rate <= 0 || power == 0 || !(max_offset_hz > 0.0)) {
        return fail(std::format(
            "estimate_tone_offset needs a positive rate, power and range; got {}, {}, {}", rate,
            power, max_offset_hz));
    }
    const double rate_d = static_cast<double>(rate);
    const double reach = max_offset_hz * static_cast<double>(power);
    if (reach >= 0.5 * rate_d) {
        return fail(std::format(
            "estimate_tone_offset cannot search +/- {} Hz at power {}: the line would sit at "
            "up to {} Hz, past the {} Hz Nyquist frequency, and alias",
            max_offset_hz, power, reach, 0.5 * rate_d));
    }

    std::vector<Complex32> raised(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        raised[i] = raise(samples[i], power);
    }

    const double bin = rate_d / static_cast<double>(samples.size());
    const double step = 0.25 * bin;
    const auto count = static_cast<std::size_t>(std::ceil(2.0 * reach / step)) + 1;
    std::vector<double> magnitude(count, 0.0);
    double total = 0.0;
    std::size_t best = 0;
    for (std::size_t k = 0; k < count; ++k) {
        const double hertz = -reach + step * static_cast<double>(k);
        magnitude[k] = line_magnitude(raised, hertz, rate_d);
        total += magnitude[k];
        if (magnitude[k] > magnitude[best]) {
            best = k;
        }
    }

    double refined = -reach + step * static_cast<double>(best);
    if (best > 0 && best + 1 < count) {
        const double a = magnitude[best - 1];
        const double b = magnitude[best];
        const double c = magnitude[best + 1];
        const double denominator = a - 2.0 * b + c;
        if (std::abs(denominator) > 0.0) {
            refined += step * 0.5 * (a - c) / denominator;
        }
    }

    ToneEstimate estimate;
    estimate.offset_hz = refined / static_cast<double>(power);
    const double mean = total / static_cast<double>(count);
    estimate.line_to_mean = (mean > 0.0) ? magnitude[best] / mean : 0.0;
    return estimate;
}

Expected<ToneEstimate> estimate_tone_pair(ConstComplexSpan samples, SampleRate rate,
                                          double half_spacing_hz, double max_offset_hz) {
    if (samples.size() < 16) {
        return fail(std::format("estimate_tone_pair needs at least 16 samples; got {}",
                                samples.size()));
    }
    if (rate <= 0 || !(half_spacing_hz > 0.0) || !(max_offset_hz > 0.0)) {
        return fail(std::format(
            "estimate_tone_pair needs a positive rate, spacing and range; got {}, {}, {}", rate,
            half_spacing_hz, max_offset_hz));
    }
    const double rate_d = static_cast<double>(rate);
    const double reach = max_offset_hz + half_spacing_hz;
    if (reach >= 0.5 * rate_d) {
        return fail(std::format(
            "estimate_tone_pair cannot search +/- {} Hz for lines {} Hz either side: they would "
            "sit at up to {} Hz, past the {} Hz Nyquist frequency, and alias",
            max_offset_hz, half_spacing_hz, reach, 0.5 * rate_d));
    }

    const std::vector<Complex32> plain(samples.begin(), samples.end());

    // A quarter of a bin, or a little less, so that the spacing is a whole
    // number of steps and each pair is read off one scan.
    const double bin = rate_d / static_cast<double>(samples.size());
    const double spacing = 2.0 * half_spacing_hz;
    const double apart = std::max(1.0, std::ceil(spacing / (0.25 * bin)));
    const double step = spacing / apart;
    const auto offset = static_cast<std::size_t>(apart);
    const auto count = static_cast<std::size_t>(std::ceil(2.0 * reach / step)) + 1;
    std::vector<double> magnitude(count, 0.0);
    double total = 0.0;
    for (std::size_t k = 0; k < count; ++k) {
        magnitude[k] = line_magnitude(plain, -reach + step * static_cast<double>(k), rate_d);
        total += magnitude[k];
    }

    // Pair k is the lines at scan points k and k + offset, centred halfway.
    const std::size_t pairs = count > offset ? count - offset : 0;
    if (pairs == 0) {
        return fail("estimate_tone_pair found no centre to search");
    }
    std::size_t best = 0;
    double best_weaker = -1.0;
    for (std::size_t k = 0; k < pairs; ++k) {
        const double weaker = std::min(magnitude[k], magnitude[k + offset]);
        if (weaker > best_weaker) {
            best_weaker = weaker;
            best = k;
        }
    }
    const auto sum = [&](std::size_t k) { return magnitude[k] + magnitude[k + offset]; };
    double refined = -reach + step * static_cast<double>(best) + half_spacing_hz;
    if (best > 0 && best + 1 < pairs) {
        const double a = sum(best - 1);
        const double b = sum(best);
        const double c = sum(best + 1);
        const double denominator = a - 2.0 * b + c;
        if (std::abs(denominator) > 0.0) {
            refined += step * 0.5 * (a - c) / denominator;
        }
    }

    ToneEstimate estimate;
    estimate.offset_hz = refined;
    const double mean = total / static_cast<double>(count);
    estimate.line_to_mean = (mean > 0.0) ? best_weaker / mean : 0.0;
    return estimate;
}

}  // namespace revenant::decode
