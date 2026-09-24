// tools/siggen/speech.h has what this makes and where each number comes from.

#include "tools/siggen/speech.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <random>

namespace revenant::siggen_speech {
namespace {

// Uniform on [0, 1), straight off the engine's top 53 bits.
[[nodiscard]] double uniform(std::mt19937_64& engine) {
    return static_cast<double>(engine() >> 11) * (1.0 / 9007199254740992.0);
}

[[nodiscard]] double uniform(std::mt19937_64& engine, double low, double high) {
    return low + (high - low) * uniform(engine);
}

// Box-Muller, one value per call.
[[nodiscard]] double gaussian(std::mt19937_64& engine) {
    const double u1 = (static_cast<double>(engine() >> 11) + 1.0) / 9007199254740993.0;
    const double u2 = uniform(engine);
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * std::numbers::pi * u2);
}

// The male averages of Peterson and Barney (1952), F1 to F3 in hertz, for
// heed, hid, head, had, hod, hawed, hood, who'd, hud and heard.
constexpr std::array<std::array<double, 3>, 10> kVowels = {{
    {270.0, 2290.0, 3010.0},
    {390.0, 1990.0, 2550.0},
    {530.0, 1840.0, 2480.0},
    {660.0, 1720.0, 2410.0},
    {730.0, 1090.0, 2440.0},
    {570.0, 840.0, 2410.0},
    {440.0, 1020.0, 2240.0},
    {300.0, 870.0, 2240.0},
    {640.0, 1190.0, 2390.0},
    {490.0, 1350.0, 1690.0},
}};

constexpr std::array<double, 3> kFormantBandwidths = {60.0, 90.0, 120.0};

// An unvoiced syllable's RMS against a voiced one's at the same level: 10 dB
// under the vowels. THIS FILE'S CHOICE, inside the range fricatives sit
// below vowels in running speech, from the sibilants a few decibels down to
// the weak fricatives twenty.
constexpr double kUnvoicedLevel = 0.316;

// Klatt's (1980) second-order digital resonator, unity gain at DC.
struct Resonator {
    double a = 1.0;
    double b = 0.0;
    double c = 0.0;
    double y1 = 0.0;
    double y2 = 0.0;

    void tune(double frequency_hz, double bandwidth_hz, double rate) {
        const double t = 1.0 / rate;
        c = -std::exp(-2.0 * std::numbers::pi * bandwidth_hz * t);
        b = 2.0 * std::exp(-std::numbers::pi * bandwidth_hz * t) *
            std::cos(2.0 * std::numbers::pi * frequency_hz * t);
        a = 1.0 - b - c;
    }

    [[nodiscard]] double step(double x) {
        const double y = a * x + b * y1 + c * y2;
        y2 = y1;
        y1 = y;
        return y;
    }
};

// A second-order Butterworth-family section in direct form I, from Robert
// Bristow-Johnson's "Cookbook formulae for audio EQ biquad filter
// coefficients". Two in cascade at Q of 0.5412 and 1.3066 are a fourth-order
// Butterworth response.
struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

    static Biquad make(bool high_pass, double corner_hz, double q, double rate) {
        const double w0 = 2.0 * std::numbers::pi * corner_hz / rate;
        const double alpha = std::sin(w0) / (2.0 * q);
        const double cw = std::cos(w0);
        const double a0 = 1.0 + alpha;
        Biquad out;
        if (high_pass) {
            out.b0 = (1.0 + cw) / 2.0 / a0;
            out.b1 = -(1.0 + cw) / a0;
            out.b2 = (1.0 + cw) / 2.0 / a0;
        } else {
            out.b0 = (1.0 - cw) / 2.0 / a0;
            out.b1 = (1.0 - cw) / a0;
            out.b2 = (1.0 - cw) / 2.0 / a0;
        }
        out.a1 = -2.0 * cw / a0;
        out.a2 = (1.0 - alpha) / a0;
        return out;
    }

    [[nodiscard]] double step(double x) {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};

}  // namespace

Speech synthesise_speech(const SpeechSpec& spec) {
    Speech out;
    out.audio.assign(spec.samples, 0.0F);
    out.talking.assign(spec.samples, 0);
    if (spec.samples == 0 || spec.rate <= 0) {
        return out;
    }
    const auto rate = static_cast<double>(spec.rate);
    std::mt19937_64 engine(spec.seed);

    std::vector<double> voice(spec.samples, 0.0);

    // One-pole low pass on the pulse train: a corner near 230 Hz at 48 kS/s
    // and -6 dB per octave above it, which is the glottal slope less the
    // radiation gain. See the header.
    const double glottal_pole = std::exp(-2.0 * std::numbers::pi * 230.0 / rate);

    std::size_t n = 0;
    bool talking = uniform(engine) < 0.8;
    while (n < spec.samples) {
        if (!talking) {
            const auto pause = static_cast<std::size_t>(uniform(engine, 0.25, 0.9) * rate);
            n += pause;
            talking = true;
            continue;
        }

        const auto phrase = static_cast<std::size_t>(uniform(engine, 1.2, 3.0) * rate);
        const std::size_t phrase_end = std::min(spec.samples, n + phrase);
        const double f0_start = uniform(engine, 100.0, 140.0);
        const std::size_t phrase_start = n;

        std::array<Resonator, 3> formants{};
        Resonator hiss;
        hiss.tune(2500.0, 1000.0, rate);
        double glottal = 0.0;
        double next_pulse = static_cast<double>(n);

        while (n < phrase_end) {
            const auto syllable =
                static_cast<std::size_t>(uniform(engine, 0.150, 0.300) * rate);
            const std::size_t end = std::min(phrase_end, n + syllable);
            const double level = uniform(engine, 0.5, 1.0);
            const bool voiced = uniform(engine) < 0.8;
            const auto vowel =
                static_cast<std::size_t>(uniform(engine) * static_cast<double>(kVowels.size())) %
                kVowels.size();
            for (std::size_t f = 0; f < 3; ++f) {
                formants[f].tune(kVowels[vowel][f], kFormantBandwidths[f], rate);
            }
            // The syllable's body first, then scaled to its level: a pulse
            // train and white noise differ in power by two orders of
            // magnitude, so without this every unvoiced syllable would drown
            // the vowels around it.
            std::vector<double> body(end - n, 0.0);
            double body_power = 0.0;
            for (std::size_t k = n; k < end; ++k) {
                double sample = 0.0;
                if (voiced) {
                    double excitation = 0.0;
                    if (static_cast<double>(k) >= next_pulse) {
                        const double through =
                            static_cast<double>(k - phrase_start) /
                            static_cast<double>(std::max<std::size_t>(1, phrase_end - phrase_start));
                        const double f0 = f0_start * (1.0 - 0.2 * through);
                        const double period = rate / f0 * (1.0 + 0.01 * gaussian(engine));
                        next_pulse += std::max(period, 1.0);
                        excitation = 1.0;
                    }
                    glottal = excitation + glottal_pole * glottal;
                    sample = glottal;
                    for (Resonator& formant : formants) {
                        sample = formant.step(sample);
                    }
                } else {
                    // Keep the pulse clock moving so the next voiced syllable
                    // does not fire a burst of catch-up pulses.
                    next_pulse = std::max(next_pulse, static_cast<double>(k));
                    sample = hiss.step(gaussian(engine));
                }
                body[k - n] = sample;
                body_power += sample * sample;
            }
            const double body_rms = std::sqrt(body_power / static_cast<double>(body.size()));
            const double gain =
                body_rms > 0.0 ? level * (voiced ? 1.0 : kUnvoicedLevel) / body_rms : 0.0;
            const double length = static_cast<double>(end - n);
            for (std::size_t k = n; k < end; ++k) {
                const double t = static_cast<double>(k - n) / length;
                const double swell = std::sin(std::numbers::pi * t);
                voice[k] = gain * swell * swell * body[k - n];
                out.talking[k] = 1;
            }
            n = end;
        }
        talking = false;
    }

    // The voice channel, 300 to 3000 Hz, fourth order at each edge.
    std::array<Biquad, 4> channel = {
        Biquad::make(true, 300.0, 0.5412, rate), Biquad::make(true, 300.0, 1.3066, rate),
        Biquad::make(false, 3000.0, 0.5412, rate), Biquad::make(false, 3000.0, 1.3066, rate)};
    for (double& sample : voice) {
        for (Biquad& section : channel) {
            sample = section.step(sample);
        }
    }

    double power = 0.0;
    std::size_t talking_samples = 0;
    for (std::size_t k = 0; k < spec.samples; ++k) {
        if (out.talking[k] != 0) {
            power += voice[k] * voice[k];
            ++talking_samples;
        }
    }
    out.talking_fraction =
        static_cast<double>(talking_samples) / static_cast<double>(spec.samples);
    if (talking_samples == 0 || !(power > 0.0)) {
        return out;
    }
    const double scale =
        spec.talking_rms / std::sqrt(power / static_cast<double>(talking_samples));
    for (std::size_t k = 0; k < spec.samples; ++k) {
        out.audio[k] = static_cast<float>(std::clamp(voice[k] * scale, -1.0, 1.0));
    }
    return out;
}

}  // namespace revenant::siggen_speech
