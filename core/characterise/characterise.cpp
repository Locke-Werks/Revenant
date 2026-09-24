#include "core/characterise/characterise.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <format>
#include <numbers>

namespace revenant::characterise {
namespace {

[[nodiscard]] std::string hertz(double value) {
    // Anything under a twentieth of a hertz is zero for this stage's
    // purposes and printing it as 1.137e-13 Hz, which a centred tone set
    // produces, reads as a measurement rather than as the rounding it is.
    if (std::abs(value) < 0.05) {
        return "0 Hz";
    }
    if (std::abs(value) >= 1000.0) {
        return std::format("{:.5g} kHz", value / 1000.0);
    }
    return std::format("{:.5g} Hz", value);
}

[[nodiscard]] std::string seconds(double value) {
    if (value < 1.0e-3) {
        return std::format("{:.3g} us", value * 1.0e6);
    }
    return std::format("{:.4g} ms", value * 1000.0);
}

// The clause that names what the numbers are consistent with, or says
// plainly that nothing in the catalogue matches. The second half is a
// finding rather than a gap, and phrasing it as one is the point: a clean
// measurement of a system nobody has a document for is a correct result.
[[nodiscard]] std::string candidate_clause(const std::vector<ProtocolCandidate>& candidates) {
    if (candidates.empty()) {
        return "; consistent with nothing in the catalogue, which is a result rather than a "
               "gap: docs/modes.md's out-of-scope section is full of systems with no obtainable "
               "document";
    }
    return "; consistent with " + summarise_candidates(candidates);
}

// The extract's own power at the power law's carrier, over the median power
// across the occupied band, taken at whichever of the carrier's M-fold
// aliases stands highest: the law reads a line at M times the carrier, so the
// carrier it reports is only known modulo rate/M. Three bins around each
// alias, so one bin's variance does not decide it. Negative when there is no
// band or no order to measure against.
[[nodiscard]] double carrier_level(const ModulationOrder& order, const OccupiedBand& band,
                                   const PowerSpectrum& spectrum)
{
    if (!order.found || order.order <= 0 || !band.found || spectrum.bins.empty()) {
        return -1.0;
    }
    std::vector<double> inside;
    for (std::size_t k = 0; k < spectrum.bins.size(); ++k) {
        const double f = spectrum.frequency_at(static_cast<double>(k));
        if (f >= band.low_hz && f <= band.high_hz) {
            inside.push_back(spectrum.bins[k]);
        }
    }
    if (inside.empty()) {
        return -1.0;
    }
    const std::size_t middle = inside.size() / 2;
    std::nth_element(inside.begin(), inside.begin() + static_cast<std::ptrdiff_t>(middle),
                     inside.end());
    const double typical = inside[middle];
    if (!(typical > 0.0)) {
        return -1.0;
    }

    const std::size_t count = spectrum.bins.size();
    const double alias = static_cast<double>(spectrum.rate) / static_cast<double>(order.order);
    double best = 0.0;
    for (int k = -order.order; k <= order.order; ++k) {
        const std::size_t at =
            spectrum.bin_at(order.carrier_offset_hz + static_cast<double>(k) * alias);
        if (at >= count) {
            continue;
        }
        double sum = 0.0;
        for (std::size_t d = 0; d < 3; ++d) {
            sum += spectrum.bins[(at + count - 1 + d) % count];
        }
        best = std::max(best, sum / 3.0);
    }
    return best / typical;
}

// The two strongest lines and the third, for the tone-pair rule on
// CharacteriseConfig::tone_pair_fraction. Each line is a three-bin window of
// excess power over the band's floor, wrapping on a two-sided spectrum, and
// each later window is taken at least four bins from every earlier one so a
// line's own skirt is not counted as its neighbour.
struct TonePair {
    double share = -1.0;
    double third = -1.0;
    double spacing_hz = 0.0;
};

[[nodiscard]] TonePair tone_pair(const PowerSpectrum& spectrum, const OccupiedBand& band)
{
    TonePair out;
    const std::size_t size = spectrum.bins.size();
    if (size < 16) {
        return out;
    }
    const double floor = band.found ? band.noise_floor : 0.0;
    std::vector<double> excess(size);
    double total = 0.0;
    for (std::size_t k = 0; k < size; ++k) {
        excess[k] = std::max(spectrum.bins[k] - floor, 0.0);
        total += excess[k];
    }
    if (!(total > 0.0)) {
        return out;
    }

    const auto window = [&](std::size_t k) {
        return excess[(k + size - 1) % size] + excess[k] + excess[(k + 1) % size];
    };
    const auto apart = [size](std::size_t a, std::size_t b) {
        const std::size_t d = a > b ? a - b : b - a;
        return std::min(d, size - d) >= 4;
    };

    std::size_t picked[3] = {size, size, size};
    double power[3] = {0.0, 0.0, 0.0};
    for (std::size_t line = 0; line < 3; ++line) {
        for (std::size_t k = 0; k < size; ++k) {
            bool clear = true;
            for (std::size_t earlier = 0; earlier < line; ++earlier) {
                clear = clear && apart(k, picked[earlier]);
            }
            if (!clear) {
                continue;
            }
            const double here = window(k);
            if (picked[line] == size || here > power[line]) {
                picked[line] = k;
                power[line] = here;
            }
        }
    }
    if (picked[1] == size) {
        return out;
    }
    out.share = (power[0] + power[1]) / total;
    out.third = power[1] > 0.0 ? power[2] / power[1] : 1.0;
    out.spacing_hz = std::abs(spectrum.frequency_at(static_cast<double>(picked[0])) -
                              spectrum.frequency_at(static_cast<double>(picked[1])));
    return out;
}

// The double-sideband reading on CharacteriseConfig::am_sideband_share. The
// carrier is the strongest three-bin window; each bin at least the minimum
// offset from it, out to the occupied band's farther edge, is paired with its
// mirror.
struct Sidebands {
    double share = -1.0;
    double symmetry = -1.0;
};

[[nodiscard]] Sidebands sidebands(const PowerSpectrum& spectrum, const OccupiedBand& band,
                                  double min_offset_hz)
{
    Sidebands out;
    const std::size_t size = spectrum.bins.size();
    if (size < 16 || !band.found || !(spectrum.bin_width_hz > 0.0)) {
        return out;
    }
    std::vector<double> excess(size);
    double total = 0.0;
    for (std::size_t k = 0; k < size; ++k) {
        excess[k] = std::max(spectrum.bins[k] - band.noise_floor, 0.0);
        total += excess[k];
    }
    if (!(total > 0.0)) {
        return out;
    }
    std::size_t carrier = 0;
    double loudest = -1.0;
    for (std::size_t k = 0; k < size; ++k) {
        const double here =
            excess[(k + size - 1) % size] + excess[k] + excess[(k + 1) % size];
        if (here > loudest) {
            loudest = here;
            carrier = k;
        }
    }
    const double carrier_hz = spectrum.frequency_at(static_cast<double>(carrier));
    const double reach_hz =
        std::max(std::abs(band.high_hz - carrier_hz), std::abs(carrier_hz - band.low_hz));
    const auto first = static_cast<std::size_t>(std::ceil(min_offset_hz / spectrum.bin_width_hz));
    const auto last = std::min(static_cast<std::size_t>(reach_hz / spectrum.bin_width_hz),
                               size / 2 - 1);
    double side = 0.0;
    double smaller = 0.0;
    double larger = 0.0;
    for (std::size_t d = first; d <= last; ++d) {
        const double upper = excess[(carrier + d) % size];
        const double lower = excess[(carrier + size - d) % size];
        side += upper + lower;
        smaller += std::min(upper, lower);
        larger += std::max(upper, lower);
    }
    out.share = side / total;
    out.symmetry = larger > 0.0 ? smaller / larger : 0.0;
    return out;
}

// The voice channel: 300 to 3000 Hz. ITU-T G.712 states 300 to 3400 Hz for
// telephony; 3000 is the top land mobile and amateur voice transmitters pass
// and the figure tools/siggen's voice scenes use.
constexpr double kVoiceLowHz = 300.0;
constexpr double kVoiceHighHz = 3000.0;

// The noise level inside the extract's passband. See
// Characterisation::inband_noise.
struct InbandNoise {
    double per_bin = 0.0;
    double total = 0.0;
};

[[nodiscard]] InbandNoise inband_noise(const PowerSpectrum& spectrum)
{
    InbandNoise out;
    const double half = static_cast<double>(spectrum.rate) / 4.0;
    std::vector<double> central;
    for (std::size_t k = 0; k < spectrum.bins.size(); ++k) {
        if (std::abs(spectrum.frequency_at(static_cast<double>(k))) <= half) {
            central.push_back(spectrum.bins[k]);
        }
    }
    if (central.empty()) {
        return out;
    }
    const auto at = static_cast<std::size_t>(0.2 * static_cast<double>(central.size()));
    std::nth_element(central.begin(), central.begin() + static_cast<std::ptrdiff_t>(at),
                     central.end());
    out.per_bin = central[at];
    double sum = 0.0;
    for (const double bin : spectrum.bins) {
        sum += std::min(bin, out.per_bin);
    }
    out.total = sum / static_cast<double>(spectrum.bins.size());
    return out;
}

// The variance a series of frame values carries between 2 and 10 Hz and
// between 10 and 40 Hz, and its mean, from a direct transform: a few hundred
// frames against a few dozen bins.
struct ModulationBands {
    double mean = 0.0;
    double syllabic = 0.0;
    double fast = 0.0;
};

[[nodiscard]] ModulationBands modulation_bands(const std::vector<double>& series, double frame_rate)
{
    ModulationBands out;
    const std::size_t frames = series.size();
    if (frames == 0) {
        return out;
    }
    for (const double value : series) {
        out.mean += value;
    }
    out.mean /= static_cast<double>(frames);
    const double resolution = frame_rate / static_cast<double>(frames);
    for (std::size_t j = 1; static_cast<double>(j) * resolution <= 40.0 && j < frames / 2; ++j) {
        const double hz = static_cast<double>(j) * resolution;
        std::complex<double> sum(0.0, 0.0);
        const double step =
            -2.0 * std::numbers::pi * static_cast<double>(j) / static_cast<double>(frames);
        for (std::size_t k = 0; k < frames; ++k) {
            sum += (series[k] - out.mean) * std::polar(1.0, step * static_cast<double>(k));
        }
        // Both halves of the two-sided transform, so each sum is the variance
        // the series carries in its band.
        const double variance =
            2.0 * std::norm(sum) / (static_cast<double>(frames) * static_cast<double>(frames));
        if (hz >= 2.0 && hz <= 10.0) {
            out.syllabic += variance;
        } else if (hz > 10.0) {
            out.fast += variance;
        }
    }
    return out;
}

// Ten milliseconds, the frame every syllabic measurement here is taken over:
// short against a syllable and long against the 300 Hz bottom of the voice
// channel.
constexpr double kSyllabicFrameSeconds = 0.01;

// Frames the syllabic measurements need, 0.64 s, so the 2 Hz end of the band
// holds at least one whole cycle.
constexpr std::size_t kMinSyllabicFrames = 64;

struct Syllabic {
    double depth = -1.0;
    double fast = -1.0;
};

// The power envelope's movement at a syllable's rate and above it. See
// Characterisation::syllabic_depth.
[[nodiscard]] Syllabic syllabic_envelope(const std::vector<Complex64>& samples,
                                         dsp::SampleRate rate, double noise_power)
{
    Syllabic out;
    const auto frame = std::max<std::size_t>(
        1, static_cast<std::size_t>(kSyllabicFrameSeconds * static_cast<double>(rate)));
    const std::size_t frames = samples.size() / frame;
    if (frames < kMinSyllabicFrames) {
        return out;
    }
    std::vector<double> power(frames, 0.0);
    for (std::size_t k = 0; k < frames; ++k) {
        double sum = 0.0;
        for (std::size_t n = k * frame; n < (k + 1) * frame; ++n) {
            sum += std::norm(samples[n]);
        }
        power[k] = sum / static_cast<double>(frame);
    }
    const ModulationBands bands =
        modulation_bands(power, static_cast<double>(rate) / static_cast<double>(frame));
    const double signal = bands.mean - noise_power;
    if (!(signal > 0.0)) {
        return out;
    }
    out.depth = std::sqrt(bands.syllabic) / signal;
    out.fast = std::sqrt(bands.fast) / signal;
    return out;
}

// The instantaneous frequency's deviation, frame by frame, moving at a
// syllable's rate. See Characterisation::frequency_syllabic_depth.
[[nodiscard]] double frequency_syllabic(const std::vector<Complex64>& samples,
                                        dsp::SampleRate rate, double mean_power)
{
    const auto frame = std::max<std::size_t>(
        1, static_cast<std::size_t>(kSyllabicFrameSeconds * static_cast<double>(rate)));
    const std::size_t frames = samples.size() / frame;
    if (frames < kMinSyllabicFrames || !(mean_power > 0.0)) {
        return -1.0;
    }
    // The same amplitude gate the tone histogram uses by default, a tenth of
    // the mean power, so a sample of phase noise across an envelope null
    // does not read as a deviation of the whole rate.
    const double gate = 0.1 * mean_power;
    const double to_hertz = static_cast<double>(rate) / (2.0 * std::numbers::pi);
    std::vector<double> frequency;
    frequency.reserve(samples.size());
    for (std::size_t n = 1; n < samples.size(); ++n) {
        if (std::norm(samples[n]) < gate || std::norm(samples[n - 1]) < gate) {
            frequency.push_back(std::nan(""));
            continue;
        }
        frequency.push_back(std::arg(samples[n] * std::conj(samples[n - 1])) * to_hertz);
    }
    std::vector<double> kept;
    for (const double f : frequency) {
        if (!std::isnan(f)) {
            kept.push_back(f);
        }
    }
    if (kept.size() < frequency.size() / 4) {
        return -1.0;
    }
    const std::size_t middle = kept.size() / 2;
    std::nth_element(kept.begin(), kept.begin() + static_cast<std::ptrdiff_t>(middle), kept.end());
    const double centre = kept[middle];

    std::vector<double> deviation(frames, 0.0);
    for (std::size_t k = 0; k < frames; ++k) {
        double sum = 0.0;
        std::size_t count = 0;
        for (std::size_t n = k * frame; n < (k + 1) * frame && n < frequency.size(); ++n) {
            if (!std::isnan(frequency[n])) {
                sum += (frequency[n] - centre) * (frequency[n] - centre);
                ++count;
            }
        }
        deviation[k] = count == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(count));
    }
    const ModulationBands bands =
        modulation_bands(deviation, static_cast<double>(rate) / static_cast<double>(frame));
    if (!(bands.mean > 0.0)) {
        return -1.0;
    }
    return std::sqrt(bands.syllabic) / bands.mean;
}

// Which side of the extract's centre its excess power sits, over the centre
// plus and minus `half_width`. See Characterisation::side_centroid.
[[nodiscard]] double side_centroid(const PowerSpectrum& spectrum, double floor, double half_width)
{
    if (!(half_width > 0.0)) {
        return 0.0;
    }
    double weight = 0.0;
    double moment = 0.0;
    for (std::size_t k = 0; k < spectrum.bins.size(); ++k) {
        const double f = spectrum.frequency_at(static_cast<double>(k));
        if (std::abs(f) > half_width) {
            continue;
        }
        const double excess = std::max(spectrum.bins[k] - floor, 0.0);
        weight += excess;
        moment += excess * f;
    }
    if (!(weight > 0.0)) {
        return 0.0;
    }
    return moment / (weight * half_width);
}

// The share of the passband's power over its median bin that sits within
// `half_width_hz` of its strongest bin. See Characterisation::line_share.
[[nodiscard]] double line_share(const PowerSpectrum& spectrum, double half_width_hz)
{
    const double passband = static_cast<double>(spectrum.rate) / 4.0;
    std::vector<double> central;
    std::size_t loudest = spectrum.bins.size();
    for (std::size_t k = 0; k < spectrum.bins.size(); ++k) {
        if (std::abs(spectrum.frequency_at(static_cast<double>(k))) > passband) {
            continue;
        }
        central.push_back(spectrum.bins[k]);
        if (loudest == spectrum.bins.size() || spectrum.bins[k] > spectrum.bins[loudest]) {
            loudest = k;
        }
    }
    if (central.empty()) {
        return -1.0;
    }
    // The median rather than inband_noise's 20th percentile, and each bin's
    // excess signed rather than clipped at zero: noise then sums to nothing
    // across the passband instead of adding the positive half of its own
    // fluctuation to the total, which on a wide bucket is most of the total
    // at a low SNR.
    const std::size_t middle = central.size() / 2;
    std::nth_element(central.begin(), central.begin() + static_cast<std::ptrdiff_t>(middle),
                     central.end());
    const double floor = central[middle];
    const double line_hz = spectrum.frequency_at(static_cast<double>(loudest));
    double total = 0.0;
    double near = 0.0;
    for (std::size_t k = 0; k < spectrum.bins.size(); ++k) {
        const double f = spectrum.frequency_at(static_cast<double>(k));
        if (std::abs(f) > passband) {
            continue;
        }
        const double excess = spectrum.bins[k] - floor;
        total += excess;
        if (std::abs(f - line_hz) <= half_width_hz) {
            near += excess;
        }
    }
    return total > 0.0 ? near / total : -1.0;
}

// The carrier's sidebands split into the part in phase with it and the part
// in quadrature. See Characterisation::carrier_iq_balance.
struct CarrierQuadrature {
    double balance = 0.0;
    double in_phase_excess = 0.0;
    bool measured = false;
};

[[nodiscard]] CarrierQuadrature carrier_quadrature(const std::vector<Complex64>& samples,
                                                   const PowerSpectrum& spectrum,
                                                   std::size_t segment)
{
    CarrierQuadrature out;
    const std::size_t size = spectrum.bins.size();
    const dsp::SampleRate rate = spectrum.rate;
    const double upper_hz = std::min(kVoiceHighHz, static_cast<double>(rate) / 4.0);
    if (size < 16 || rate <= 0 || !(upper_hz > kVoiceLowHz)) {
        return out;
    }

    // The carrier: the strongest three-bin window, refined to the power
    // centroid of those three bins.
    std::size_t carrier = 0;
    double loudest = -1.0;
    for (std::size_t k = 0; k < size; ++k) {
        const double here = spectrum.bins[(k + size - 1) % size] + spectrum.bins[k] +
                            spectrum.bins[(k + 1) % size];
        if (here > loudest) {
            loudest = here;
            carrier = k;
        }
    }
    double weight = 0.0;
    double moment = 0.0;
    for (int d = -1; d <= 1; ++d) {
        const std::size_t k = (carrier + size + static_cast<std::size_t>(d + 1) - 1) % size;
        const double p = spectrum.bins[k];
        weight += p;
        moment += p * (spectrum.frequency_at(static_cast<double>(carrier)) +
                       static_cast<double>(d) * spectrum.bin_width_hz);
    }
    const double carrier_hz = weight > 0.0 ? moment / weight
                                           : spectrum.frequency_at(static_cast<double>(carrier));

    // Mixed to the carrier, then its residual phase followed in 20 ms blocks,
    // which averages the voice channel's lowest tone over six cycles and
    // follows anything the bin left of the carrier's frequency up to 25 Hz.
    const std::size_t count = samples.size();
    const auto block = std::max<std::size_t>(1, static_cast<std::size_t>(rate / 50));
    const std::size_t blocks = count / block;
    if (blocks < 8) {
        return out;
    }
    std::vector<Complex64> mixed(count);
    const double step = -2.0 * std::numbers::pi * carrier_hz / static_cast<double>(rate);
    for (std::size_t n = 0; n < count; ++n) {
        mixed[n] = samples[n] * std::polar(1.0, std::fmod(step * static_cast<double>(n),
                                                          2.0 * std::numbers::pi));
    }
    std::vector<double> phase(blocks, 0.0);
    for (std::size_t b = 0; b < blocks; ++b) {
        Complex64 sum(0.0, 0.0);
        for (std::size_t n = b * block; n < (b + 1) * block; ++n) {
            sum += mixed[n];
        }
        phase[b] = std::arg(sum);
        if (b > 0) {
            while (phase[b] - phase[b - 1] > std::numbers::pi) {
                phase[b] -= 2.0 * std::numbers::pi;
            }
            while (phase[b] - phase[b - 1] < -std::numbers::pi) {
                phase[b] += 2.0 * std::numbers::pi;
            }
        }
    }
    std::vector<double> in_phase(count);
    std::vector<double> quadrature(count);
    double carrier_sum = 0.0;
    for (std::size_t n = 0; n < count; ++n) {
        const double position = (static_cast<double>(n) + 0.5) / static_cast<double>(block) - 0.5;
        double theta = 0.0;
        if (position <= 0.0) {
            theta = phase.front();
        } else if (position >= static_cast<double>(blocks - 1)) {
            theta = phase.back();
        } else {
            const auto low = static_cast<std::size_t>(position);
            const double t = position - static_cast<double>(low);
            theta = phase[low] + t * (phase[low + 1] - phase[low]);
        }
        const Complex64 y = mixed[n] * std::polar(1.0, -theta);
        in_phase[n] = y.real();
        quadrature[n] = y.imag();
        carrier_sum += y.real();
    }
    const double carrier_amplitude = carrier_sum / static_cast<double>(count);
    const double carrier_power = carrier_amplitude * carrier_amplitude;

    auto i_spectrum = welch_spectrum_real(in_phase, rate, segment);
    auto q_spectrum = welch_spectrum_real(quadrature, rate, segment);
    if (!i_spectrum.has_value() || !q_spectrum.has_value() || !(carrier_power > 0.0)) {
        return out;
    }
    // Band power on the noise-equivalent normalisation: a bin reads the
    // variance of white noise, so a band's share of it is its width over the
    // one-sided extent.
    double p_i = 0.0;
    double p_q = 0.0;
    const double scale = i_spectrum->bin_width_hz / (static_cast<double>(rate) / 2.0);
    for (std::size_t k = 0; k < i_spectrum->bins.size(); ++k) {
        const double f = i_spectrum->frequency_at(static_cast<double>(k));
        if (f >= kVoiceLowHz && f <= upper_hz) {
            p_i += i_spectrum->bins[k] * scale;
            p_q += q_spectrum->bins[k] * scale;
        }
    }
    if (!(p_i + p_q > 0.0)) {
        return out;
    }
    out.balance = (p_i - p_q) / (p_i + p_q);
    out.in_phase_excess = (p_i - p_q) / carrier_power;
    out.measured = true;
    return out;
}

}  // namespace

Expected<Characterisation> characterise(dsp::ConstComplexSpan samples,
                                        const CharacteriseConfig& config)
{
    if (config.rate <= 0) {
        return fail(std::format(
            "characterise: CharacteriseConfig::rate must be positive, was {}. The stage has no "
            "way to turn a bin or a lag into hertz without it.",
            config.rate));
    }
    if (samples.size() < kMinCharacteriseSamples) {
        return fail(std::format(
            "characterise: {} samples is under the {} this stage needs, which at {} S/s is "
            "{:.3g} seconds. The tone histogram wants 4096 sample pairs past its amplitude "
            "gate, the autocorrelation divides by its own overlap, and the spectral estimate "
            "averages sixteen segments; none of the three produces a usable floor on a buffer "
            "this short. Capture a longer extract.",
            samples.size(), kMinCharacteriseSamples, config.rate,
            static_cast<double>(kMinCharacteriseSamples) / static_cast<double>(config.rate)));
    }

    Characterisation out;
    const std::vector<Complex64> widened = to_analysis(samples);

    out.envelope = envelope_stats(widened);
    if (!(out.envelope.mean_power > 0.0)) {
        return fail("characterise: the extract carries no power at all. Check the receiver "
                    "reached this call and that the buffer is the one that was filled.");
    }

    // The caller's transform length when it named one, so two extracts of
    // different lengths can be compared. See CharacteriseConfig::segment for
    // the measurement that made this worth offering.
    const std::size_t segment =
        config.segment != 0 ? config.segment : analysis_segment(widened.size());

    auto spectrum = welch_spectrum(widened, config.rate, segment);
    if (!spectrum.has_value()) {
        return std::unexpected(with_context(spectrum.error(), "characterise"));
    }
    out.spectral_concentration = spectral_concentration(*spectrum);
    out.band = occupied_band(*spectrum, 0.99);

    auto tone_result = estimate_tone_structure(widened, config.rate, config.tones);
    if (!tone_result.has_value()) {
        return std::unexpected(with_context(tone_result.error(), "characterise"));
    }
    out.tones = std::move(*tone_result);

    auto envelope_rate = estimate_symbol_rate(widened, config.rate,
                                              CyclicDetector::SquaredEnvelope, config.cyclic);
    if (!envelope_rate.has_value()) {
        return std::unexpected(with_context(envelope_rate.error(), "characterise"));
    }
    out.squared_envelope = std::move(*envelope_rate);

    auto transition_rate = estimate_symbol_rate(
        widened, config.rate, CyclicDetector::FrequencyTransition, config.cyclic);
    if (!transition_rate.has_value()) {
        return std::unexpected(with_context(transition_rate.error(), "characterise"));
    }
    out.frequency_transition = std::move(*transition_rate);

    auto order = estimate_modulation_order(widened, config.rate, config.cyclic.threshold_db);
    if (!order.has_value()) {
        return std::unexpected(with_context(order.error(), "characterise"));
    }
    out.order = std::move(*order);

    auto profile = autocorrelation(widened, config.rate);
    if (!profile.has_value()) {
        return std::unexpected(with_context(profile.error(), "characterise"));
    }
    out.ofdm = find_cyclic_prefix(*profile, config.ofdm);
    out.frame = find_frame_period(*profile, config.frame);

    // The voice measurements, each against the noise inside the passband
    // rather than the 20th percentile of the whole extract. See
    // Characterisation::inband_noise.
    {
        const InbandNoise noise = inband_noise(*spectrum);
        out.inband_noise = noise.total;
        const Syllabic syllabic = syllabic_envelope(widened, config.rate, noise.total);
        out.syllabic_depth = syllabic.depth;
        out.syllabic_fast = syllabic.fast;
        out.frequency_syllabic_depth =
            frequency_syllabic(widened, config.rate, out.envelope.mean_power);
        const double half_width = config.detection_bandwidth_hz > 0.0
                                      ? 0.5 * config.detection_bandwidth_hz
                                      : (out.band.found ? 0.5 * out.band.bandwidth_hz : 0.0);
        out.side_centroid = side_centroid(*spectrum, noise.per_bin, half_width);
        out.line_share = line_share(*spectrum, config.voice_line_half_width_hz);
        const CarrierQuadrature quadrature = carrier_quadrature(widened, *spectrum, segment);
        if (quadrature.measured) {
            out.carrier_iq_balance = quadrature.balance;
            out.carrier_in_phase_excess = quadrature.in_phase_excess;
        }
    }

    const bool constant_envelope =
        out.envelope.normalised_power_variance < config.constant_envelope_variance;

    // Whether the power law's carrier sits where the extract has no power of
    // its own, which is where band-limited noise puts one. See
    // CharacteriseConfig::psk_carrier_level_fraction for the measurement.
    out.psk_carrier_level = carrier_level(out.order, out.band, *spectrum);
    out.psk_carrier_outside_band =
        out.psk_carrier_level >= 0.0 && out.psk_carrier_level < config.psk_carrier_level_fraction;

    // Whether what the PSK branch would call a symbol clock is the gap between
    // two lines. See CharacteriseConfig::tone_pair_fraction. The rate is the
    // one the branch would report, so the test is against the claim it would
    // make rather than against either detector on its own.
    {
        const TonePair pair = tone_pair(*spectrum, out.band);
        out.tone_pair_share = pair.share;
        out.tone_pair_third = pair.third;
        out.tone_pair_spacing_hz = pair.spacing_hz;
        const SymbolRateEstimate& claimed =
            out.squared_envelope.found ? out.squared_envelope : out.frequency_transition;
        if (claimed.found && pair.share >= config.tone_pair_fraction &&
            pair.third < config.tone_pair_third_fraction) {
            const double tolerance =
                std::max(2.0 * spectrum->bin_width_hz,
                         config.tone_pair_rate_tolerance * claimed.symbol_rate_hz);
            out.psk_tone_pair = std::abs(pair.spacing_hz - claimed.symbol_rate_hz) <= tolerance;
        }
    }

    // The envelope against the noise inside the passband, for the voice
    // branches. See Characterisation::envelope_variance_inband.
    {
        const double noise = out.inband_noise;
        const double signal = std::max(out.envelope.mean_power - noise, 0.0);
        const double total = signal + noise;
        out.envelope_variance_inband =
            out.envelope.normalised_power_variance -
            (total > 0.0 ? (2.0 * signal * noise + noise * noise) / (total * total) : 0.0);
    }
    const bool constant_inband = out.envelope_variance_inband < config.constant_envelope_variance;
    const double voice_width = config.detection_bandwidth_hz > 0.0
                                   ? config.detection_bandwidth_hz
                                   : (out.band.found ? out.band.bandwidth_hz : 0.0);
    // A keyed carrier is one frequency and a talker is not. See
    // CharacteriseConfig::voice_max_line_share.
    const bool one_line = out.line_share >= config.voice_max_line_share;
    const bool ssb_voice = !constant_inband && voice_width >= config.voice_min_width_hz &&
                           !one_line && out.syllabic_depth >= config.voice_syllabic_depth &&
                           out.syllabic_depth >= config.voice_syllabic_ratio * out.syllabic_fast;
    const bool fm_voice =
        constant_inband && out.frequency_syllabic_depth >= config.fm_voice_frequency_syllabic;

    ProtocolQuery query;
    query.bandwidth_hz = out.band.found ? out.band.bandwidth_hz : 0.0;

    // The order of these branches is the whole classifier and each one is
    // ahead of the next for a reason rather than by convenience.
    // The carrier's sidebands against its own phase. See
    // CharacteriseConfig::carrier_in_phase_balance.
    const bool in_phase_sidebands =
        out.carrier_iq_balance >= config.carrier_in_phase_balance &&
        out.carrier_in_phase_excess >= config.carrier_sideband_excess;
    const bool quadrature_sidebands =
        out.carrier_iq_balance <= -config.carrier_in_phase_balance &&
        -out.carrier_in_phase_excess >= config.carrier_sideband_excess;
    const bool carrier_with_sidebands =
        out.spectral_concentration >= config.carrier_sideband_concentration && !ssb_voice &&
        (in_phase_sidebands || quadrature_sidebands);

    // A talker on single sideband whose extract a voiced sound's harmonics
    // pulled over the carrier bar; over a wide detection that is the talker,
    // and ssb_voice has already ruled out a keyed carrier by its line share.
    // Whatever the quadrature reading says about the harmonic it locked to:
    // an AM carrier holds its power through the talker's pauses, so its
    // envelope never moves like this. See CharacteriseConfig::
    // voice_carrier_width_hz.
    //
    // WHAT THE FIRST SENTENCE USED TO SAY after the semicolon: "over a wide
    // detection that is the talker and not a keyed carrier, which is never a
    // kilohertz wide." A coarse grid measures one 1311 Hz wide; see
    // CharacteriseConfig::voice_max_line_share.
    const bool voice_over_carrier = ssb_voice && voice_width >= config.voice_carrier_width_hz;

    // Two lines and nothing else of note, on a constant envelope: a pair of
    // tones keyed between, which is two-tone FSK. See "THE SAME TWO BARS ON A
    // CONSTANT ENVELOPE" under CharacteriseConfig::tone_pair_fraction.
    out.fsk_tone_pair = constant_inband && out.tone_pair_share >= config.tone_pair_fraction &&
                        out.tone_pair_third >= 0.0 &&
                        out.tone_pair_third < config.tone_pair_third_fraction;

    if (out.fsk_tone_pair) {
        out.family = ModulationFamily::Fsk;
        out.family_confidence = std::clamp(out.tone_pair_share, 0.0, 1.0);
        // The transition detector only. The squared envelope cannot read a
        // constant envelope at all, and on RTTY it named 2994 Hz, which the
        // symbol-rate rule then refused as wider than the 791 Hz detection.
        if (out.frequency_transition.found) {
            out.symbol_rate = out.frequency_transition;
        }
        query.family = ModulationFamily::Fsk;
        query.tone_count = 2;
        query.symbol_rate_hz = out.symbol_rate.found ? out.symbol_rate.symbol_rate_hz : 0.0;
        out.candidates = match_protocols(query);
        out.summary = std::format(
            "2-FSK at {}, read off the spectrum: its two strongest lines, {} apart, hold {:.1f} "
            "percent of the band's excess power, the next holds {:.3f} of the weaker, and the "
            "envelope is constant, which two steady tones' is not{}",
            out.symbol_rate.found ? hertz(out.symbol_rate.symbol_rate_hz)
                                  : std::string("an unmeasured symbol rate"),
            hertz(out.tone_pair_spacing_hz), 100.0 * out.tone_pair_share, out.tone_pair_third,
            candidate_clause(out.candidates));
    } else if ((out.spectral_concentration >= config.carrier_concentration ||
                carrier_with_sidebands) &&
               !voice_over_carrier) {
        // Nothing else can be read off a signal whose energy is one line,
        // or a carrier whose sidebands sit against it the way AM's or FM's
        // do. See CharacteriseConfig::carrier_sideband_concentration.
        out.family = ModulationFamily::Unmodulated;
        out.family_confidence = std::clamp(out.spectral_concentration, 0.0, 1.0);

        // The spectral sideband reading, reported and no longer deciding
        // anything. See CharacteriseConfig::am_sideband_share.
        const Sidebands sides = sidebands(*spectrum, out.band, config.am_sideband_min_offset_hz);
        out.sideband_share = sides.share;
        out.sideband_symmetry = sides.symmetry;
        // Where the sidebands sit against the carrier's own phase is what
        // separates AM from FM at a low index and both from a keyed or bare
        // carrier. See CharacteriseConfig::carrier_in_phase_balance.
        out.double_sideband = in_phase_sidebands;
        out.low_index_fm = quadrature_sidebands;

        // The envelope net of the noise, which the double-sideband reading
        // used to rest on and is still reported. See envelope_variance_net.
        const double noise = out.band.found ? out.band.noise_floor : 0.0;
        const double signal = std::max(out.envelope.mean_power - noise, 0.0);
        const double total = signal + noise;
        const double from_noise =
            total > 0.0 ? (2.0 * signal * noise + noise * noise) / (total * total) : 0.0;
        out.envelope_variance_net = out.envelope.normalised_power_variance - from_noise;
        out.summary = std::format(
            "an unmodulated carrier: {:.1f} percent of the extract's power is in three adjacent "
            "bins at {}, and its instantaneous frequency spans {}",
            100.0 * out.spectral_concentration, hertz(out.band.centre_hz),
            hertz(out.tones.frequency_spread_hz));
        if (out.double_sideband) {
            out.summary += std::format(
                ", with sidebands in the voice channel {:.2f} in phase with it against their "
                "quadrature and holding {:.1f} percent of its power beyond what the quadrature "
                "holds, which is double sideband amplitude modulation",
                out.carrier_iq_balance, 100.0 * out.carrier_in_phase_excess);
        }
        if (out.low_index_fm) {
            // Held to a half for the reason the AnalogueFm branch below is:
            // an elimination, and a continuous-phase digital mode at a low
            // index would read the same.
            out.family = ModulationFamily::AnalogueFm;
            out.family_confidence = 0.5;
            query.family = ModulationFamily::AnalogueFm;
            out.candidates = match_protocols(query);
            out.summary = std::format(
                "frequency modulation at a low index: {:.1f} percent of the extract's power is "
                "in the carrier at {}, and its sidebands in the voice channel sit in quadrature "
                "with it, {:.2f} against their in-phase part and {:.1f} percent of its power "
                "beyond it, which is phase modulation and not AM{}",
                100.0 * out.spectral_concentration, hertz(out.band.centre_hz),
                -out.carrier_iq_balance, -100.0 * out.carrier_in_phase_excess,
                candidate_clause(out.candidates));
        }
    } else if (out.ofdm.found) {
        // Before the tone test, because an OFDM waveform's instantaneous
        // frequency is a mess with no tones in it and would fall through
        // to the analogue branch. A cyclic prefix is a positive finding
        // and the others below are eliminations.
        out.family = ModulationFamily::Ofdm;
        out.family_confidence = out.ofdm.confidence;
        query.family = ModulationFamily::Ofdm;
        query.ofdm_symbol_seconds = out.ofdm.symbol_seconds;
        out.candidates = match_protocols(query);
        out.summary = std::format(
            "OFDM: a {} useful symbol, so {} between subcarriers, with a guard of about {} "
            "samples read off a correlation of {:.3f}; occupying {}{}",
            seconds(out.ofdm.symbol_seconds), hertz(out.ofdm.subcarrier_spacing_hz),
            out.ofdm.prefix_samples, out.ofdm.correlation, hertz(out.band.bandwidth_hz),
            candidate_clause(out.candidates));
    } else if (ssb_voice) {
        // Speech on a suppressed carrier. No family here names single
        // sideband, so this is a refusal, and it is taken before the tone
        // test and the PSK branch because a talker's pitch lights both: a
        // cyclic line at the fundamental, which the PSK branch would report
        // as a symbol rate. See CharacteriseConfig::voice_syllabic_depth.
        out.voice = true;
        if (out.side_centroid <= -config.voice_side_centroid) {
            out.voice_sideband = VoiceSideband::Upper;
        } else if (out.side_centroid >= config.voice_side_centroid) {
            out.voice_sideband = VoiceSideband::Lower;
        }
    } else if (fm_voice) {
        // A constant envelope whose deviation comes and goes at a syllable's
        // rate: a talker on FM. Held to a half like the other analogue FM
        // calls, because the syllabic measurement says voice and not which
        // index, and a slow data burst on FM could move the deviation the
        // same way. See CharacteriseConfig::fm_voice_frequency_syllabic.
        out.voice = true;
        out.family = ModulationFamily::AnalogueFm;
        out.family_confidence = 0.5;
        query.family = ModulationFamily::AnalogueFm;
        out.candidates = match_protocols(query);
        out.summary = std::format(
            "frequency modulation carrying speech: the envelope's normalised power variance is "
            "{:.3f} net of the noise inside the passband, and the instantaneous frequency's "
            "deviation moves by {:.2f} of its mean between 2 and 10 Hz, which is a talker's "
            "syllables{}",
            out.envelope_variance_inband, out.frequency_syllabic_depth,
            candidate_clause(out.candidates));
    } else if (out.tones.found) {
        out.family = ModulationFamily::Fsk;
        out.family_confidence = out.tones.confidence;
        // The transition detector first: a tone set is constant envelope
        // by construction, which is the case the squared envelope cannot
        // read at all.
        out.symbol_rate = out.frequency_transition.found ? out.frequency_transition
                                                         : out.squared_envelope;
        query.family = ModulationFamily::Fsk;
        query.tone_count = out.tones.tone_count;
        query.symbol_rate_hz = out.symbol_rate.found ? out.symbol_rate.symbol_rate_hz : 0.0;
        out.candidates = match_protocols(query);
        out.summary = std::format(
            "{}-FSK at {}, tones {} apart centred on {}, occupying {}{}", out.tones.tone_count,
            out.symbol_rate.found ? hertz(out.symbol_rate.symbol_rate_hz)
                                  : std::string("an unmeasured symbol rate"),
            hertz(out.tones.spacing_hz), hertz(out.tones.centre_offset_hz),
            hertz(out.band.bandwidth_hz), candidate_clause(out.candidates));
    } else if (constant_envelope && out.band.found &&
               out.tones.frequency_spread_hz >
                   config.analogue_fm_spread_fraction * out.band.bandwidth_hz) {
        // Constant envelope rules out every linear modulation, and the
        // tone test has already said the instantaneous frequency is a
        // continuum rather than a set of values. That is frequency
        // modulation by something continuous.
        out.family = ModulationFamily::AnalogueFm;
        // Half, and no more. This branch is an elimination rather than a
        // positive finding, and it cannot separate analogue FM from a
        // continuous-phase digital mode whose index is low enough to
        // merge its tones. Reporting it above half would claim a
        // discrimination that was not made.
        out.family_confidence = 0.5;
        query.family = ModulationFamily::AnalogueFm;
        out.candidates = match_protocols(query);
        // The cycle frequency is reported and not attributed, which is a
        // different thing from suppressing it. On a broadcast carrier the
        // frequency-transition feature carries lines at the composite's
        // own tones, and on a 4FSK signal too weak for its tones to
        // separate it carries one at the symbol rate. The two are the
        // same measurement and nothing here tells them apart, so the
        // number goes in the summary saying exactly that, rather than
        // being thrown away or being called a symbol rate.
        const std::string cycle =
            out.frequency_transition.found
                ? std::format(
                      ". A cycle frequency of {} stands {:.1f} dB up in the phase-curvature "
                      "feature. On an analogue carrier that is a modulation tone; on a digital "
                      "one it would be the symbol rate. Nothing here decides which, so it is "
                      "not reported as a symbol rate",
                      hertz(out.frequency_transition.symbol_rate_hz),
                      out.frequency_transition.margin_db)
                : std::string();
        out.summary = std::format(
            "constant envelope with a continuously distributed instantaneous frequency spanning "
            "{} inside {}: analogue FM, or a continuous-phase digital mode whose tones this "
            "extract cannot separate{}{}",
            hertz(out.tones.frequency_spread_hz), hertz(out.band.bandwidth_hz), cycle,
            candidate_clause(out.candidates));
    } else if (out.order.found && !constant_envelope && !out.psk_carrier_outside_band &&
               !out.psk_tone_pair) {
        out.family = ModulationFamily::Psk;
        out.family_confidence = out.order.confidence;
        out.symbol_rate =
            out.squared_envelope.found ? out.squared_envelope : out.frequency_transition;
        query.family = ModulationFamily::Psk;
        query.psk_order = out.order.order;
        query.symbol_rate_hz = out.symbol_rate.found ? out.symbol_rate.symbol_rate_hz : 0.0;
        out.candidates = match_protocols(query);
        out.summary = std::format(
            "{}-PSK at {}, carrier {} from the extract's own centre modulo {}, occupying {}{}",
            out.order.order,
            out.symbol_rate.found ? hertz(out.symbol_rate.symbol_rate_hz)
                                  : std::string("an unmeasured symbol rate"),
            hertz(out.order.carrier_offset_hz),
            hertz(static_cast<double>(config.rate) / static_cast<double>(out.order.order)),
            hertz(out.band.bandwidth_hz), candidate_clause(out.candidates));

        // Kept, flagged and capped rather than refused, and the reason is on
        // kPskWithoutRateConfidence: real PSK loses its rate before its order,
        // and a carrier in noise lights the same order line with no rate at
        // all, at confidences the two share.
        if (!out.symbol_rate.found) {
            out.psk_without_symbol_rate = true;
            out.family_confidence = std::min(out.family_confidence, kPskWithoutRateConfidence);
            out.summary += std::format(
                ". No symbol rate was measured, so this rests on the M-th power line alone, "
                "which a bare carrier in noise lights the same way: confidence held to {:.2f} "
                "and not to be used to drive detection",
                kPskWithoutRateConfidence);
        }
    }

    // A signal cannot be keyed faster than it is wide. See
    // CharacteriseConfig::detection_bandwidth_hz for the measurement. Applied
    // to whatever family carried a rate, which is PSK and FSK: the others
    // never report one.
    double refused_rate = 0.0;
    if (config.detection_bandwidth_hz > 0.0 && out.symbol_rate.found &&
        out.symbol_rate.symbol_rate_hz > config.detection_bandwidth_hz &&
        (out.family == ModulationFamily::Psk || out.family == ModulationFamily::Fsk)) {
        refused_rate = out.symbol_rate.symbol_rate_hz;
        out.symbol_rate_exceeds_detection = true;
        out.family = ModulationFamily::Unknown;
        out.family_confidence = 0.0;
        out.psk_without_symbol_rate = false;
        out.candidates.clear();
    }

    if (out.family == ModulationFamily::Unknown) {
        // Nothing carried a family. The frame period is the one finding
        // that can stand without one, because a repeat is a repeat
        // whatever is repeating, so it is reported rather than swallowed.
        out.refused = !out.frame.found;
        out.refusal = std::format(
            "no modulation family was established. The envelope's normalised power variance is "
            "{:.4f}, against {:.2f} for a constant envelope and exactly 1 for pure complex "
            "Gaussian noise. {:.2f} percent of the power is in three adjacent bins, against "
            "the {:.0f} percent an unmodulated carrier reaches. The tone histogram said: {} "
            "The squared envelope said: {} The frequency transition said: {} The cyclic "
            "prefix search said: {}",
            out.envelope.normalised_power_variance, config.constant_envelope_variance,
            100.0 * out.spectral_concentration, 100.0 * config.carrier_concentration,
            out.tones.refusal.empty() ? "it found a tone set." : out.tones.refusal,
            out.squared_envelope.refusal.empty() ? "it found a symbol rate."
                                                 : out.squared_envelope.refusal,
            out.frequency_transition.refusal.empty() ? "it found a symbol rate."
                                                     : out.frequency_transition.refusal,
            out.ofdm.refusal.empty() ? "it found a cyclic prefix." : out.ofdm.refusal);

        if (out.voice) {
            out.refusal += std::format(
                " The power envelope moves by {:.2f} of the signal's mean power between 2 and 10 "
                "Hz against {:.2f} from 10 to 40 Hz, across {}, and the envelope is not constant: "
                "that is a talker's syllables on a suppressed carrier, {}. No family here names "
                "single sideband, so it is refused rather than given the digital family its "
                "pitch would pass for.",
                out.syllabic_depth, out.syllabic_fast, hertz(voice_width),
                out.voice_sideband == VoiceSideband::Upper   ? "the upper sideband"
                : out.voice_sideband == VoiceSideband::Lower ? "the lower sideband"
                                                             : "on a side this could not read");
        }

        if (out.psk_carrier_outside_band) {
            out.refusal += std::format(
                " The M-th power law found an order-{} line, {:.1f} dB up, and put its carrier at "
                "{}, where the extract's own power is {:.2f} of the median across its occupied "
                "band, against the {:.2f} a carrier has to reach. A linear modulation's spectrum "
                "peaks at its carrier; a carrier where the band's power has fallen away is the "
                "corner band-limited noise leaves at its edge, not a line. So no PSK call was "
                "made from it.",
                out.order.order, out.order.margin_db, hertz(out.order.carrier_offset_hz),
                out.psk_carrier_level, config.psk_carrier_level_fraction);
        }

        if (out.psk_tone_pair) {
            out.refusal += std::format(
                " The M-th power law found an order-{} line and a symbol clock was read at {}, "
                "but {:.2f} of the band's excess power is in two lines {} apart, with the next "
                "line at {:.2f} of the weaker. Two tones square to a line at their difference, so "
                "that clock is the gap between two carriers, not a symbol rate, and no PSK call "
                "was made from it.",
                out.order.order,
                hertz(out.squared_envelope.found ? out.squared_envelope.symbol_rate_hz
                                                 : out.frequency_transition.symbol_rate_hz),
                out.tone_pair_share, hertz(out.tone_pair_spacing_hz), out.tone_pair_third);
        }

        if (out.symbol_rate_exceeds_detection) {
            out.refusal += std::format(
                " A family was found at a symbol rate of {}, wider than the {} the detection it "
                "was asked about occupies. A signal cannot be keyed faster than it is wide, so "
                "the rate is a modulating tone or the gap between lines, and the family was "
                "refused.",
                hertz(refused_rate), hertz(config.detection_bandwidth_hz));
        }

        if (out.frame.found) {
            out.summary = std::format(
                "unidentified, but something repeats every {} ({} samples) at a correlation of "
                "{:.3f}. A frame period with no modulation family under it is still worth "
                "recording: it is what a probe or a sync word in the clear produces, and it "
                "does not need the payload",
                seconds(out.frame.period_seconds), out.frame.period_samples,
                out.frame.repeat_fraction);
        } else {
            out.summary = "unidentified: " + out.refusal;
        }
    } else if (out.frame.found) {
        // Two cases where the repeat is already accounted for and saying
        // it again would read as a second, independent finding.
        //
        // On OFDM the guard interval IS the repeat, at the useful symbol
        // length, so the frame reader finds the same lag the cyclic
        // prefix reader did and the summary has already named it.
        //
        // On an analogue carrier the modulating waveform comes round
        // again, and a 1 kHz programme tone at 684 kS/s repeats every 684
        // samples at a correlation of one. That is the modulation's own
        // period and it is not framing; calling it a frame period would
        // put a digital word on an analogue measurement.
        const bool is_the_prefix = out.family == ModulationFamily::Ofdm &&
                                   out.frame.period_samples == out.ofdm.symbol_samples;
        if (out.family == ModulationFamily::AnalogueFm) {
            out.summary += std::format(
                ". The modulating waveform itself repeats every {}, which is its own period "
                "rather than a frame", seconds(out.frame.period_seconds));
        } else if (!is_the_prefix) {
            out.summary += std::format(". Something also repeats every {} ({} samples) at a "
                                       "correlation of {:.3f}",
                                       seconds(out.frame.period_seconds),
                                       out.frame.period_samples, out.frame.repeat_fraction);
        }
    }

    return out;
}

bool may_drive_detection(const Characterisation& result)
{
    return result.family != ModulationFamily::Unknown && !result.psk_without_symbol_rate;
}

}  // namespace revenant::characterise
