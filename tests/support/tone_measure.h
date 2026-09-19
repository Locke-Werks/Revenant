// Measuring a tone out of a buffer, for the cases that check what a stage
// produced rather than whether two implementations agree.
//
// A bit-exact diff answers "does the device compute what the twin computes".
// It cannot answer "is that the right answer", and a kernel that is bit-exact
// against a twin implementing the wrong convention passes every diff in the
// suite. These three estimators are what the behavioural cases use instead,
// and they are here rather than in one test file because both the reference
// suite and the engine suite need them and a second copy would drift.
//
// All three are exact for a pure tone and need no transform. The complex one
// reads the frequency out of the mean phase advance; the real ones project
// onto a cosine and a sine at a frequency the caller names, which is a
// one-bin Goertzel written out.

#pragma once

#include <cmath>
#include <complex>
#include <cstddef>
#include <initializer_list>
#include <span>

#include "core/dsp/types.h"

namespace revenant::test {

inline constexpr double kTwoPi = 6.283185307179586476925286766559;

struct ToneFit {
    // Root-mean-square magnitude, which for a constant or a pure tone is its
    // amplitude.
    double magnitude = 0.0;

    double frequency_hz = 0.0;
};

inline ToneFit measure_complex_tone(std::span<const dsp::Complex32> samples, double rate) {
    ToneFit fit;
    if (samples.size() < 2) {
        return fit;
    }

    std::complex<double> product{0.0, 0.0};
    double power = 0.0;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        const std::complex<double> current(samples[i]);
        const std::complex<double> previous(samples[i - 1]);
        product += current * std::conj(previous);
        power += std::norm(current);
    }

    fit.magnitude = std::sqrt(power / static_cast<double>(samples.size() - 1));
    fit.frequency_hz = std::arg(product) * rate / kTwoPi;
    return fit;
}

struct AudioFit {
    // Peak amplitude of the component at the frequency asked about.
    double amplitude = 0.0;

    // That component's share of the total power. A detector that recovered
    // the right amplitude at the right frequency and also produced a pile of
    // harmonics would pass an amplitude check alone; this is what notices.
    double purity = 0.0;
};

// The component at one known frequency. `remove_mean` subtracts the buffer's
// average first, which is what a mistuned FM receiver puts there and what
// would otherwise count against every share below.
inline AudioFit measure_audio_tone(std::span<const float> audio, double rate,
                                   double frequency_hz, bool remove_mean = false) {
    AudioFit fit;
    if (audio.empty()) {
        return fit;
    }

    const auto n = static_cast<double>(audio.size());

    double mean = 0.0;
    if (remove_mean) {
        for (const float value : audio) {
            mean += static_cast<double>(value);
        }
        mean /= n;
    }

    double cosine = 0.0;
    double sine = 0.0;
    double power = 0.0;
    for (std::size_t i = 0; i < audio.size(); ++i) {
        const double value = static_cast<double>(audio[i]) - mean;
        // Reduced before it reaches the transcendentals, so a long buffer
        // does not lose precision to a large argument.
        const double turns = std::fmod(frequency_hz * static_cast<double>(i) / rate, 1.0);
        const double angle = kTwoPi * turns;
        cosine += value * std::cos(angle);
        sine += value * std::sin(angle);
        power += value * value;
    }

    fit.amplitude = 2.0 * std::sqrt(cosine * cosine + sine * sine) / n;
    const double mean_power = power / n;
    fit.purity = (mean_power > 0.0) ? (0.5 * fit.amplitude * fit.amplitude) / mean_power : 0.0;
    return fit;
}

struct DominantTone {
    double frequency_hz = 0.0;
    double amplitude = 0.0;
    double share = 0.0;
};

// Whichever of the candidate frequencies holds the most power, with the mean
// removed. For a case that knows the signal is one of a small set of tones
// but not which, which is what a randomised scene produces.
inline DominantTone dominant_tone(std::span<const float> audio, double rate,
                                  std::initializer_list<double> candidates) {
    DominantTone best;
    for (const double candidate : candidates) {
        const AudioFit fit = measure_audio_tone(audio, rate, candidate, true);
        if (fit.purity > best.share) {
            best.frequency_hz = candidate;
            best.amplitude = fit.amplitude;
            best.share = fit.purity;
        }
    }
    return best;
}

}  // namespace revenant::test
