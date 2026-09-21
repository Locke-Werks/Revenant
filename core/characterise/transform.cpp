#include "core/characterise/transform.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace revenant::characterise {
namespace {

// Smallest buffer local_baseline will take a median over. Below this the
// median is drawn from too few bins to be a percentile of anything.
constexpr std::size_t kMinBaselineBlock = 8;

// Percentile of the spectrum taken as the noise floor in occupied_band. Low
// enough to sit under a signal that fills a third of the span, high enough
// that it is not reading the single quietest bin.
constexpr double kNoiseFloorPercentile = 0.20;

[[nodiscard]] double log_power(double value) {
    // -300 dB, well below anything a normalised buffer produces, so that a
    // zero bin interpolates as very small rather than as negative infinity.
    constexpr double kFloor = 1.0e-30;
    return std::log10(std::max(value, kFloor));
}

// Parabolic interpolation through three log-power samples, returning the
// offset of the vertex from the centre sample in bins.
//
// Log rather than linear power because the peak being interpolated is a
// windowed spectral line, whose main lobe is close to a Gaussian in
// amplitude, and a Gaussian is exactly a parabola in the log. Interpolating
// the linear values biases the answer toward the centre bin.
[[nodiscard]] double parabolic_offset(double left, double centre, double right) {
    const double denominator = left - 2.0 * centre + right;
    if (denominator >= 0.0) {
        // Not a maximum. Happens on a bin that ties with a neighbour.
        return 0.0;
    }
    const double offset = 0.5 * (left - right) / denominator;
    return std::clamp(offset, -0.5, 0.5);
}

// The half-circle a radix-2 transform of this size needs, exp(-2i*pi*j/size)
// for j below size/2.
//
// Built once per transform and indexed per stage rather than computed inside
// the butterfly loop. A cosine and a sine per butterfly is size/2*log2(size)
// transcendental pairs, which at 32768 points is a quarter of a million of
// them per segment and thirty-one segments per spectrum; the table is
// size/2 of them for the whole transform. The other way round, advancing one
// twiddle by repeated complex multiplication, is cheaper still and
// accumulates error that grows with the stage length, which is exactly the
// regime this runs in.
[[nodiscard]] std::vector<Complex64> fft_twiddles(std::size_t size) {
    std::vector<Complex64> table(size / 2);
    const double scale = -2.0 * std::numbers::pi / static_cast<double>(size);
    for (std::size_t j = 0; j < table.size(); ++j) {
        const double theta = scale * static_cast<double>(j);
        table[j] = Complex64(std::cos(theta), std::sin(theta));
    }
    return table;
}

void fft_core(std::span<Complex64> data, std::span<const Complex64> twiddles) {
    const std::size_t size = data.size();
    if (size < 2) {
        return;
    }

    // Bit-reversed load. Oppenheim and Schafer chapter 9: the
    // decimation-in-time graph consumes its input in bit-reversed order and
    // produces its output in natural order.
    for (std::size_t i = 1, j = 0; i < size; ++i) {
        std::size_t bit = size >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }

    for (std::size_t length = 2; length <= size; length <<= 1) {
        const std::size_t stride = size / length;
        const std::size_t half = length / 2;
        for (std::size_t start = 0; start < size; start += length) {
            for (std::size_t k = 0; k < half; ++k) {
                const Complex64 even = data[start + k];
                const Complex64 odd = twiddles[k * stride] * data[start + k + half];
                data[start + k] = even + odd;
                data[start + k + half] = even - odd;
            }
        }
    }
}

}  // namespace

std::size_t floor_power_of_two(std::size_t value)
{
    if (value == 0) {
        return 0;
    }
    std::size_t result = 1;
    while (result <= value / 2) {
        result *= 2;
    }
    return result;
}

void fft_in_place(std::span<Complex64> data)
{
    fft_core(data, fft_twiddles(data.size()));
}

std::vector<double> hann_window(std::size_t length)
{
    std::vector<double> window(length, 0.0);
    if (length == 0) {
        return window;
    }
    const double scale = 2.0 * std::numbers::pi / static_cast<double>(length);
    for (std::size_t n = 0; n < length; ++n) {
        window[n] = 0.5 - 0.5 * std::cos(scale * static_cast<double>(n));
    }
    return window;
}

std::size_t analysis_segment(std::size_t sample_count)
{
    constexpr std::size_t kMinSegment = 512;
    constexpr std::size_t kMaxSegment = 32768;
    const std::size_t target = floor_power_of_two(sample_count / 16);
    return std::clamp(target, kMinSegment, kMaxSegment);
}

double PowerSpectrum::frequency_at(double bin) const
{
    if (one_sided) {
        return bin * bin_width_hz;
    }
    const double size = static_cast<double>(segment);
    const double folded = bin >= size / 2.0 ? bin - size : bin;
    return folded * bin_width_hz;
}

std::size_t PowerSpectrum::bin_at(double frequency_hz) const
{
    if (bin_width_hz <= 0.0 || segment == 0) {
        return bins.size();
    }
    const double exact = frequency_hz / bin_width_hz;
    const auto nearest = static_cast<long long>(std::llround(exact));
    if (one_sided) {
        if (nearest < 0 || static_cast<std::size_t>(nearest) >= bins.size()) {
            return bins.size();
        }
        return static_cast<std::size_t>(nearest);
    }
    const auto size = static_cast<long long>(segment);
    if (nearest <= -size / 2 || nearest >= size / 2) {
        return bins.size();
    }
    const long long wrapped = nearest < 0 ? nearest + size : nearest;
    return static_cast<std::size_t>(wrapped);
}

namespace {

// Shared body of the two Welch entry points. real_input selects the one-sided
// output and nothing else: a real signal is transformed as a complex one with
// a zero imaginary part, which costs a factor of two in work and removes an
// entire class of packing bug from a file whose job is to be trusted.
[[nodiscard]] Expected<PowerSpectrum> welch_common(std::span<const Complex64> signal,
                                                   SampleRate rate,
                                                   std::size_t segment,
                                                   std::size_t hop,
                                                   bool real_input)
{
    if (rate <= 0) {
        return fail(std::format("welch: sample rate must be positive, was {}", rate));
    }
    if (!is_power_of_two(segment)) {
        return fail(std::format("welch: segment length must be a power of two, was {}", segment));
    }
    if (segment > kMaxTransform) {
        return fail(std::format("welch: segment length {} is above the {} this analysis will run",
                                segment, kMaxTransform));
    }
    if (hop == 0) {
        hop = segment / 2;
    }
    if (hop == 0) {
        return fail("welch: hop resolved to zero, which would never advance");
    }
    if (signal.size() < segment) {
        return fail(std::format(
            "welch: {} samples is shorter than one {}-point segment, so the spectrum would be a "
            "single unaveraged periodogram whose largest excursion over its own median is 8 to "
            "10 dB, above every detection threshold in core/characterise. Supply at least {} "
            "samples, or ask for a shorter segment.",
            signal.size(), segment, segment));
    }

    const std::vector<double> window = hann_window(segment);
    double window_power = 0.0;
    for (const double w : window) {
        window_power += w * w;
    }

    PowerSpectrum out;
    out.segment = segment;
    out.rate = rate;
    out.bin_width_hz = static_cast<double>(rate) / static_cast<double>(segment);
    out.one_sided = real_input;
    out.bins.assign(real_input ? segment / 2 + 1 : segment, 0.0);

    const std::vector<Complex64> twiddles = fft_twiddles(segment);
    std::vector<Complex64> scratch(segment);
    std::size_t count = 0;
    for (std::size_t start = 0; start + segment <= signal.size(); start += hop) {
        for (std::size_t n = 0; n < segment; ++n) {
            scratch[n] = signal[start + n] * window[n];
        }
        fft_core(scratch, twiddles);
        for (std::size_t k = 0; k < out.bins.size(); ++k) {
            out.bins[k] += std::norm(scratch[k]);
        }
        ++count;
    }

    // Noise-equivalent normalisation: divide by sum(w^2), which is one of the
    // two Harris 1978 conventions and the one that makes a bin read the
    // per-sample variance of white noise whatever the window length. Every
    // threshold in this directory is a ratio of a line to the floor beside
    // it, so the absolute scale never reaches a decision; what matters is
    // that it does not move when the segment length does. A tone on a bin
    // centre reads sum(w)^2/sum(w^2) times its power, which for a Hann
    // window is 2*segment/3, and that factor cancels in every ratio here.
    const double scale = 1.0 / (window_power * static_cast<double>(count));
    for (double& bin : out.bins) {
        bin *= scale;
    }
    out.segments_averaged = count;
    return out;
}

}  // namespace

Expected<PowerSpectrum> welch_spectrum(std::span<const Complex64> signal,
                                       SampleRate rate,
                                       std::size_t segment,
                                       std::size_t hop)
{
    return welch_common(signal, rate, segment, hop, false);
}

Expected<PowerSpectrum> welch_spectrum_real(std::span<const double> signal,
                                            SampleRate rate,
                                            std::size_t segment,
                                            std::size_t hop)
{
    std::vector<Complex64> widened(signal.size());
    for (std::size_t n = 0; n < signal.size(); ++n) {
        widened[n] = Complex64(signal[n], 0.0);
    }
    return welch_common(widened, rate, segment, hop, true);
}

std::vector<double> local_baseline(std::span<const double> spectrum, std::size_t block)
{
    std::vector<double> baseline(spectrum.size(), 0.0);
    if (spectrum.empty()) {
        return baseline;
    }
    block = std::max(block, kMinBaselineBlock);

    std::vector<double> scratch;
    scratch.reserve(block);
    for (std::size_t start = 0; start < spectrum.size(); start += block) {
        const std::size_t stop = std::min(start + block, spectrum.size());
        scratch.assign(spectrum.begin() + static_cast<std::ptrdiff_t>(start),
                       spectrum.begin() + static_cast<std::ptrdiff_t>(stop));
        const std::size_t middle = scratch.size() / 2;
        std::nth_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(middle),
                         scratch.end());
        const double median = scratch[middle];
        for (std::size_t k = start; k < stop; ++k) {
            baseline[k] = median;
        }
    }
    return baseline;
}

namespace {

// Builds a peak record for one bin, interpolating the position when the two
// neighbours are available.
[[nodiscard]] SpectralPeak make_peak(const PowerSpectrum& spectrum,
                                     std::span<const double> baseline,
                                     std::size_t bin)
{
    SpectralPeak peak;
    peak.found = true;
    peak.bin = bin;
    peak.power = spectrum.bins[bin];
    peak.baseline = baseline[bin];
    peak.margin_db = 10.0 * (log_power(peak.power) - log_power(peak.baseline));

    double offset = 0.0;
    if (bin > 0 && bin + 1 < spectrum.bins.size()) {
        offset = parabolic_offset(log_power(spectrum.bins[bin - 1]), log_power(peak.power),
                                  log_power(spectrum.bins[bin + 1]));
    }
    peak.frequency_hz = spectrum.frequency_at(static_cast<double>(bin) + offset);
    return peak;
}

}  // namespace

SpectralPeak strongest_peak(const PowerSpectrum& spectrum,
                            std::span<const double> baseline,
                            double low_hz,
                            double high_hz)
{
    SpectralPeak best;
    if (spectrum.bins.empty() || baseline.size() != spectrum.bins.size()) {
        return best;
    }

    double best_margin = 0.0;
    for (std::size_t k = 0; k < spectrum.bins.size(); ++k) {
        const double frequency = spectrum.frequency_at(static_cast<double>(k));
        if (frequency < low_hz || frequency > high_hz) {
            continue;
        }
        const double margin = 10.0 * (log_power(spectrum.bins[k]) - log_power(baseline[k]));
        if (!best.found || margin > best_margin) {
            best_margin = margin;
            best = make_peak(spectrum, baseline, k);
        }
    }
    return best;
}

SpectralPeak peak_near(const PowerSpectrum& spectrum,
                       std::span<const double> baseline,
                       double target_hz,
                       std::size_t tolerance_bins)
{
    SpectralPeak best;
    if (spectrum.bins.empty() || baseline.size() != spectrum.bins.size()) {
        return best;
    }
    const std::size_t centre = spectrum.bin_at(target_hz);
    if (centre >= spectrum.bins.size()) {
        return best;
    }

    const std::size_t low = centre > tolerance_bins ? centre - tolerance_bins : 0;
    const std::size_t high = std::min(centre + tolerance_bins, spectrum.bins.size() - 1);
    double best_margin = 0.0;
    for (std::size_t k = low; k <= high; ++k) {
        const double margin = 10.0 * (log_power(spectrum.bins[k]) - log_power(baseline[k]));
        if (!best.found || margin > best_margin) {
            best_margin = margin;
            best = make_peak(spectrum, baseline, k);
        }
    }
    return best;
}

double spectral_concentration(const PowerSpectrum& spectrum)
{
    if (spectrum.bins.size() < 3) {
        return 0.0;
    }
    double total = 0.0;
    for (const double bin : spectrum.bins) {
        total += bin;
    }
    if (total <= 0.0) {
        return 0.0;
    }

    // The window wraps for a two-sided spectrum, because a carrier sitting on
    // the Nyquist edge is a real case and splitting its three bins across the
    // ends of the array would halve the reading for a signal that is no less
    // concentrated than any other.
    const std::size_t size = spectrum.bins.size();
    double best = 0.0;
    for (std::size_t k = 0; k < size; ++k) {
        double sum = spectrum.bins[k];
        if (spectrum.one_sided) {
            if (k == 0 || k + 1 >= size) {
                continue;
            }
            sum += spectrum.bins[k - 1] + spectrum.bins[k + 1];
        } else {
            sum += spectrum.bins[(k + size - 1) % size] + spectrum.bins[(k + 1) % size];
        }
        best = std::max(best, sum);
    }
    return best / total;
}

OccupiedBand occupied_band(const PowerSpectrum& spectrum, double fraction)
{
    OccupiedBand band;
    const std::size_t size = spectrum.bins.size();
    if (size < 4 || fraction <= 0.0 || fraction > 1.0) {
        return band;
    }

    std::vector<double> sorted(spectrum.bins.begin(), spectrum.bins.end());
    const auto floor_index =
        static_cast<std::size_t>(kNoiseFloorPercentile * static_cast<double>(size));
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(floor_index),
                     sorted.end());
    band.noise_floor = sorted[floor_index];

    // Frequency order, so the run below is contiguous in frequency rather
    // than in transform order.
    std::vector<double> excess(size, 0.0);
    std::vector<double> frequency(size, 0.0);
    for (std::size_t k = 0; k < size; ++k) {
        const std::size_t source = spectrum.one_sided ? k : (k + size / 2) % size;
        excess[k] = std::max(spectrum.bins[source] - band.noise_floor, 0.0);
        frequency[k] = spectrum.frequency_at(static_cast<double>(source));
    }

    double total = 0.0;
    for (const double value : excess) {
        total += value;
    }
    if (total <= 0.0) {
        return band;
    }

    // Shortest run holding the requested share. Two pointers: grow the right
    // edge until the run is heavy enough, then pull the left edge in as far
    // as it will go before recording the width.
    const double target = fraction * total;
    std::size_t best_low = 0;
    std::size_t best_high = size - 1;
    std::size_t best_width = size;
    double running = 0.0;
    std::size_t low = 0;
    for (std::size_t high = 0; high < size; ++high) {
        running += excess[high];
        while (running - excess[low] >= target && low < high) {
            running -= excess[low];
            ++low;
        }
        if (running >= target && high - low + 1 < best_width) {
            best_width = high - low + 1;
            best_low = low;
            best_high = high;
        }
    }
    if (best_width > size) {
        return band;
    }

    band.found = true;
    band.low_hz = frequency[best_low] - 0.5 * spectrum.bin_width_hz;
    band.high_hz = frequency[best_high] + 0.5 * spectrum.bin_width_hz;
    band.centre_hz = 0.5 * (band.low_hz + band.high_hz);
    band.bandwidth_hz = band.high_hz - band.low_hz;
    return band;
}

std::vector<Complex64> to_analysis(ConstComplexSpan samples)
{
    std::vector<Complex64> out(samples.size());
    for (std::size_t n = 0; n < samples.size(); ++n) {
        out[n] = Complex64(static_cast<double>(samples[n].real()),
                           static_cast<double>(samples[n].imag()));
    }
    return out;
}

}  // namespace revenant::characterise
