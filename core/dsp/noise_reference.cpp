// This include must come first. It carries the pragma that disables
// floating-point contraction for this translation unit, and a pragma that
// appears after a function definition does not apply to it.
#include "core/dsp/reference_fp.h"

#include "core/dsp/noise_reference.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numbers>
#include <string>

#include "core/dsp/denormal_mode.h"
#include "core/dsp/vrx_reference.h"

namespace revenant::dsp {
namespace {

static_assert(kReferenceFpDisciplineApplied,
              "core/dsp/reference_fp.h must be included before anything else in a reference "
              "implementation");

// Normalized LMS step and leakage, per audio sample.
//
// THIS IS A TRADE AND THE NUMBERS SAY WHICH WAY. Voiced speech is a harmonic
// series that holds for a syllable, so a predictor fast enough to learn a
// heterodyne in a few hundred milliseconds also learns a vowel, and removes
// some of it. Measured on the synthetic voice of tests/engine/
// test_engine_noise.cpp as audio at 48 kS/s, with the stride and a 5 ms delay:
// the voice alone keeps an output SNR of 16.1 dB at a step of 1e-4 with no
// leak and 18.0 dB with a leak of 3e-6, and with a 1300 Hz whistle at the
// voice's own level, -0.6 dB untouched, the output SNR is 13.6 and 13.1 dB. A
// step of 2.5e-4 cancels the whistle to 14.8 dB and keeps the voice alone at
// only 11.0; 5e-5 keeps the voice at 23.6 and leaves the whistle at 9.4; and
// 1e-3, the first value tried, with a 2 ms delay and a leak of 1e-5, kept the
// voice alone at 2.9. docs/noise.md has the figures through the engine. At
// 1e-4 a steady tone at half the audio's power is learned with a time
// constant near 0.8 s, and tests/reference/test_noise.cpp measures it 12.6 dB
// down over the second second.
constexpr double kLineMu = 1.0e-4;
constexpr double kLineLeak = 1.0 - 3.0e-6;

// Small against the reference power of any audio worth notching: 128 taps of
// a signal at -80 dBFS still sum to 1.3e-6.
constexpr double kLineEps = 1.0e-10;

// The predictor's delay, 5 ms, against 2 ms: 1.6 dB kinder to the voice
// alone at a step of 1e-3 and 0.5 dB better on the whistle. A voice is
// periodic over its pitch, so no delay decorrelates its harmonics; what the
// delay does decorrelate is a formant's ringing.
constexpr double kLineDelaySeconds = 5.0e-3;

// Fa / S stays at least this multiple of the passband's reach, so the stride's
// first image lands well clear of any audio the passband let through.
constexpr double kLineStrideMargin = 2.5;

// The spectral stage's fixed figures. Smoothing and gamma are per hop, which
// is 5.3 ms at 48 kS/s: 0.7 is an 18 ms smoothing time constant, and 0.998 is
// a floor that rises at most with a 2.7 s time constant, so a syllable cannot
// lift it and a band that gets noisier is followed within seconds.
constexpr double kSpectralSmoothing = 0.7;
constexpr double kSpectralGamma = 0.998;
constexpr double kSpectralBeta = 0.96;

// What a tracked minimum is multiplied by to estimate the mean noise power
// under it. Measured by tests/reference/test_noise.cpp on four seconds of
// white noise at 48 kS/s: the tracked minimum settles 3.8 dB under the true
// mean periodogram, which is a factor of 2.4, and that case asserts the
// estimate lands within 1.5 dB of the true floor.
constexpr double kSpectralBias = 2.4;

constexpr double kSpectralEps = 1.0e-12;

// The blanker's reference window, before the clamp.
constexpr double kBlankWindowSeconds = 0.5e-3;

// Channel samples blanked after the last and before the first flagged one.
// Measured by tests/engine/test_engine_noise.cpp's impulses, through a 17
// tap per branch grid at 144 kS/s, as the voice band's SNR after blanking
// with the threshold at 12 dB: 25.9 dB at 1, 30.4 at 2, 29.6 at 3, 26.8 at
// 6, 24.6 at 9 and 20.9 at 17, from -0.2 unblanked. The threshold already
// flags every sample of the channelizer's smear that stands clear of the
// background, so a longer hang only cuts programme, and in the voice's own
// band: a gap a fraction of a millisecond long has its spectrum right where
// the voice is.
constexpr std::uint32_t kBlankHang = 2;

// Rounded to float once, from the same decimal the kernels carry, so both
// sides hold the same bits. Not used by any table: the tables are designed
// in double and handed over.
[[nodiscard]] float to_float(double value) { return static_cast<float>(value); }

// The shared precondition of the three twins: a ring whose length is the
// mask plus one, a power of two.
[[nodiscard]] bool is_mask(std::uint32_t mask) {
    const std::uint64_t size = static_cast<std::uint64_t>(mask) + 1U;
    return std::has_single_bit(size);
}

}  // namespace

// ---------------------------------------------------------------------------
// The impulse blanker
// ---------------------------------------------------------------------------

BlankerConfig design_blanker(SampleRate channel_rate) {
    BlankerConfig config;
    const double window = std::round(kBlankWindowSeconds * static_cast<double>(channel_rate));
    config.window = static_cast<std::uint32_t>(std::clamp(
        window, static_cast<double>(kMinBlankWindow), static_cast<double>(kMaxBlankWindow)));
    config.hang = kBlankHang;
    config.lead = kBlankHang;
    config.guard = config.lead + 2U;
    return config;
}

float blanker_threshold(double threshold_db) {
    return to_float(std::pow(10.0, threshold_db / 10.0));
}

Status validate(const BlankerConfig& config, const BlankerParams& params) {
    if (config.pass != kBlankDetect && config.pass != kBlankApply) {
        return fail(std::format("noise blanker: pass {} is neither detect nor apply", config.pass));
    }
    if (config.window < kMinBlankWindow || config.window > kMaxBlankWindow) {
        return fail(std::format("noise blanker: a {} sample window is outside [{}, {}]",
                                config.window, kMinBlankWindow, kMaxBlankWindow));
    }
    if (!is_mask(params.chan_mask) || !is_mask(params.out_mask)) {
        return fail(std::format("noise blanker: masks {:#x} and {:#x} must each be a power of "
                                "two minus one",
                                params.chan_mask, params.out_mask));
    }
    // Everything one dispatch reads has to be live in each ring at once, or a
    // masked read wraps onto a sample this same dispatch is replacing.
    const std::uint64_t reach = static_cast<std::uint64_t>(blanker_reach_below(config)) +
                                config.lead + params.count;
    const std::uint64_t smaller =
        static_cast<std::uint64_t>(std::min(params.chan_mask, params.out_mask)) + 1U;
    if (reach >= smaller) {
        return fail(std::format(
            "noise blanker: {} samples plus a reach of {} below and {} above do not fit a {} "
            "sample ring",
            params.count, blanker_reach_below(config), config.lead, smaller));
    }
    if (!(params.threshold > 0.0F)) {
        return fail("noise blanker: the threshold must be a positive power ratio");
    }
    return {};
}

Status reference_blank_detect(const BlankerConfig& config, const BlankerParams& params,
                              ConstComplexSpan channel_ring, RealSpan flags) {
    if (auto valid = validate(config, params); !valid) {
        return valid;
    }
    const std::uint64_t out_ring = static_cast<std::uint64_t>(params.out_mask) + 1U;
    if (flags.size() < out_ring) {
        return fail(std::format("reference_blank_detect: {} flags for a {} sample ring",
                                flags.size(), out_ring));
    }
    if (channel_ring.size() <
        static_cast<std::uint64_t>(params.chan_base) + params.chan_mask + 1U) {
        return fail("reference_blank_detect: the channel ring is shorter than its base and mask");
    }

    const ScopedDenormalFlush flush_denormals;
    const auto window = static_cast<float>(config.window);

    for (std::uint32_t i = 0; i < params.count; ++i) {
        const std::uint32_t c = (params.chan_first + i) & params.chan_mask;
        const std::uint32_t o = (params.out_first + i) & params.out_mask;
        const Complex32 s = channel_ring[params.chan_base + c];
        const float p = s.real() * s.real() + s.imag() * s.imag();

        // Oldest first, the kernel's order.
        float sum = 0.0F;
        for (std::uint32_t m = 0; m < config.window; ++m) {
            const std::uint32_t at = (c - config.guard - config.window + m) & params.chan_mask;
            const Complex32 r = channel_ring[params.chan_base + at];
            const float q = r.real() * r.real() + r.imag() * r.imag();
            sum = sum + q;
        }
        const float lhs = p * window;
        const float rhs = params.threshold * sum;
        flags[o] = (sum > 0.0F && lhs > rhs) ? 1.0F : 0.0F;
    }
    return {};
}

Status reference_blank_apply(const BlankerConfig& config, const BlankerParams& params,
                             ConstComplexSpan channel_ring, ConstRealSpan flags,
                             ComplexSpan blanked) {
    if (auto valid = validate(config, params); !valid) {
        return valid;
    }
    const std::uint64_t out_ring = static_cast<std::uint64_t>(params.out_mask) + 1U;
    if (flags.size() < out_ring || blanked.size() < out_ring) {
        return fail("reference_blank_apply: the flag and blanked rings must each hold out_mask "
                    "+ 1 values");
    }
    if (channel_ring.size() <
        static_cast<std::uint64_t>(params.chan_base) + params.chan_mask + 1U) {
        return fail("reference_blank_apply: the channel ring is shorter than its base and mask");
    }

    for (std::uint32_t i = 0; i < params.count; ++i) {
        const std::uint32_t c = (params.chan_first + i) & params.chan_mask;
        const std::uint32_t o = (params.out_first + i) & params.out_mask;
        bool hit = false;
        for (std::uint32_t m = 0; m <= config.hang + config.lead; ++m) {
            if (flags[(o - config.hang + m) & params.out_mask] != 0.0F) {
                hit = true;
            }
        }
        blanked[o] = hit ? Complex32{0.0F, 0.0F} : channel_ring[params.chan_base + c];
    }
    return {};
}

// ---------------------------------------------------------------------------
// The line kernel
// ---------------------------------------------------------------------------

Status validate(const LineConfig& config, const LineParams& params) {
    if (config.lanes == 0 || config.taps == 0 || config.taps % config.lanes != 0) {
        return fail(std::format("noise line: {} taps do not divide over {} lanes", config.taps,
                                config.lanes));
    }
    if (!std::has_single_bit(config.history)) {
        return fail(std::format("noise line: a history of {} is not a power of two",
                                config.history));
    }
    if (params.stride == 0 || params.stride > kMaxLineStride) {
        return fail(std::format("noise line: stride {} is outside [1, {}]", params.stride,
                                kMaxLineStride));
    }
    if (params.delay == 0) {
        return fail("noise line: the predictor's delay must be at least one sample, or it "
                    "predicts the sample it is given");
    }
    const std::uint64_t reach = static_cast<std::uint64_t>(params.delay) +
                                static_cast<std::uint64_t>(config.taps - 1U) * params.stride;
    if (reach >= config.history) {
        return fail(std::format("noise line: a predictor reaching {} samples back does not fit "
                                "a {} sample history",
                                reach, config.history));
    }
    if (!(params.eps > 0.0F)) {
        return fail("noise line: the normalisation's eps must be positive, or silence divides "
                    "by zero");
    }
    return {};
}

Status reference_line(const LineConfig& config, const LineParams& params, RealSpan audio,
                      RealSpan state) {
    if (auto valid = validate(config, params); !valid) {
        return valid;
    }
    if (audio.size() < params.count) {
        return fail(std::format("reference_line: {} samples asked of a {} sample buffer",
                                params.count, audio.size()));
    }
    if (state.size() < line_state_size(config)) {
        return fail(std::format("reference_line: a {} float state where {} are needed",
                                state.size(), line_state_size(config)));
    }

    const ScopedDenormalFlush flush_denormals;

    const bool notch = (params.flags & kLineNotch) != 0U;
    const bool ale = (params.flags & kLineAle) != 0U;
    const std::uint32_t per_lane = config.taps / config.lanes;
    const std::uint32_t history_mask = config.history - 1U;
    float* const weights = state.data() + kLineStateWeights;
    float* const history = weights + config.taps;

    float x1 = state[0];
    float x2 = state[1];
    float y1 = state[2];
    float y2 = state[3];
    auto position = static_cast<std::uint32_t>(state[kLineStatePosition]);

    std::vector<float> reference(config.taps);
    std::vector<float> part_y(config.lanes);
    std::vector<float> part_p(config.lanes);

    for (std::uint32_t n = 0; n < params.count; ++n) {
        const float x = audio[n];
        float v = x;
        if (notch) {
            const float t0 = params.b0 * x;
            const float t1 = params.b1 * x1;
            const float t2 = params.b2 * x2;
            const float t3 = params.a1 * y1;
            const float t4 = params.a2 * y2;
            float acc = t0 + t1;
            acc = acc + t2;
            acc = acc - t3;
            acc = acc - t4;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = acc;
            v = acc;
        }

        float out = v;
        if (ale) {
            for (std::uint32_t lane = 0; lane < config.lanes; ++lane) {
                float py = 0.0F;
                float pp = 0.0F;
                for (std::uint32_t j = 0; j < per_lane; ++j) {
                    const std::uint32_t k = lane + j * config.lanes;
                    const std::uint32_t at =
                        (position - params.delay - k * params.stride) & history_mask;
                    const float r = history[at];
                    reference[k] = r;
                    const float wy = weights[k] * r;
                    py = py + wy;
                    const float rr = r * r;
                    pp = pp + rr;
                }
                part_y[lane] = py;
                part_p[lane] = pp;
            }
            float y = 0.0F;
            float p = 0.0F;
            for (std::uint32_t lane = 0; lane < config.lanes; ++lane) {
                y = y + part_y[lane];
                p = p + part_p[lane];
            }
            const float e = v - y;
            const float g = params.mu * det_recip(p + params.eps);
            const float ge = g * e;
            for (std::uint32_t k = 0; k < config.taps; ++k) {
                const float kept = params.leak * weights[k];
                const float step = ge * reference[k];
                weights[k] = kept + step;
            }
            history[position & history_mask] = v;
            out = e;
        }
        audio[n] = out;
        position = position + 1U;
    }

    state[0] = x1;
    state[1] = x2;
    state[2] = y1;
    state[3] = y2;
    state[kLineStatePosition] = static_cast<float>(position & history_mask);
    return {};
}

Expected<NotchDesign> design_notch(double audio_hz, double depth_db, double width_hz,
                                   SampleRate audio_rate) {
    if (audio_rate <= 0) {
        return fail("notch: no audio rate to design at");
    }
    const auto fa = static_cast<double>(audio_rate);
    if (!(audio_hz > 0.0) || !(audio_hz < 0.5 * fa)) {
        return fail(std::format("notch: {} Hz is not inside the audio band of a {} S/s stream",
                                audio_hz, audio_rate));
    }
    if (!(width_hz > 0.0) || !(width_hz < 0.25 * fa)) {
        return fail(std::format("notch: a {} Hz width does not fit a {} S/s stream", width_hz,
                                audio_rate));
    }
    const double omega = 2.0 * std::numbers::pi * audio_hz / fa;
    const double rp = 1.0 - std::numbers::pi * width_hz / fa;
    const double rz = 1.0 - (1.0 - rp) * std::pow(10.0, -depth_db / 20.0);
    const double c = std::cos(omega);

    NotchDesign design;
    design.b0 = 1.0;
    design.b1 = -2.0 * rz * c;
    design.b2 = rz * rz;
    design.a1 = -2.0 * rp * c;
    design.a2 = rp * rp;

    // Unit gain at whichever end of the band is further from the notch. z = 1
    // is DC and z = -1 is the Nyquist frequency.
    const double z = (omega < 0.5 * std::numbers::pi) ? -1.0 : 1.0;
    const double numerator = design.b0 + design.b1 * z + design.b2;
    const double denominator = 1.0 + design.a1 * z + design.a2;
    const double scale = denominator / numerator;
    design.b0 *= scale;
    design.b1 *= scale;
    design.b2 *= scale;
    return design;
}

// ---------------------------------------------------------------------------
// Spectral noise reduction
// ---------------------------------------------------------------------------

SpectralConfig spectral_config_for(SampleRate audio_rate) {
    const std::uint64_t wanted =
        audio_rate > 0 ? static_cast<std::uint64_t>(audio_rate) / 90U : kMinSpectralFrame;
    const std::uint64_t floor = std::bit_floor(std::max<std::uint64_t>(wanted, 1U));
    SpectralConfig config;
    config.frame = static_cast<std::uint32_t>(
        std::clamp<std::uint64_t>(floor, kMinSpectralFrame, kMaxSpectralFrame));
    return config;
}

std::vector<float> design_spectral_tables(const SpectralConfig& config) {
    const std::uint32_t n = config.frame;
    std::vector<float> tables(4U * static_cast<std::size_t>(n));
    const double two_pi_over_n = 2.0 * std::numbers::pi / static_cast<double>(n);
    for (std::uint32_t k = 0; k < n; ++k) {
        // The two quarter points and the half point are written exactly,
        // because cos(pi/2) in double is 6e-17 and not zero, and a table that
        // is not exactly odd about its half point leaks a bin into its image.
        double c = std::cos(two_pi_over_n * static_cast<double>(k));
        double s = std::sin(two_pi_over_n * static_cast<double>(k));
        if (k == 0) {
            c = 1.0;
            s = 0.0;
        } else if (4U * k == n) {
            c = 0.0;
            s = 1.0;
        } else if (2U * k == n) {
            c = -1.0;
            s = 0.0;
        } else if (4U * k == 3U * n) {
            c = 0.0;
            s = -1.0;
        }
        tables[k] = static_cast<float>(c);
        tables[n + k] = static_cast<float>(s);

        // Square-root Hann, offset half a sample so it is symmetric and has
        // no zero at either end. Its square at n and at n + N/2 are sin^2 and
        // cos^2 of the same angle, which is the 50 percent overlap-add
        // identity the reconstruction rests on.
        const double window =
            std::sin(std::numbers::pi * (static_cast<double>(k) + 0.5) / static_cast<double>(n));
        tables[2U * n + k] = static_cast<float>(window);
        tables[3U * n + k] = static_cast<float>(window / static_cast<double>(n));
    }
    return tables;
}

SpectralParams spectral_params(double strength) {
    const double s = std::clamp(strength, 0.0, 1.0);
    SpectralParams params;
    params.smoothing = to_float(kSpectralSmoothing);
    params.smoothing_complement = to_float(1.0 - kSpectralSmoothing);
    params.gamma = to_float(kSpectralGamma);
    params.beta = to_float(kSpectralBeta);
    params.rise = to_float((1.0 - kSpectralGamma) / (1.0 - kSpectralBeta));
    params.bias = to_float(kSpectralBias);
    params.alpha = to_float(1.0 + s);
    params.floor_gain = to_float(std::pow(10.0, -(6.0 + 12.0 * s) / 20.0));
    params.eps = to_float(kSpectralEps);
    return params;
}

Status validate(const SpectralConfig& config, const SpectralParams& params) {
    if (!std::has_single_bit(config.frame) || config.frame < kMinSpectralFrame ||
        config.frame > kMaxSpectralFrame) {
        return fail(std::format("noise reduction: a frame of {} is not a power of two in [{}, {}]",
                                config.frame, kMinSpectralFrame, kMaxSpectralFrame));
    }
    if (!(params.eps > 0.0F)) {
        return fail("noise reduction: eps must be positive, or a silent bin divides by zero");
    }
    return {};
}

Status reference_spectral(const SpectralConfig& config, const SpectralParams& params,
                          ConstRealSpan tables, RealSpan audio, RealSpan state) {
    if (auto valid = validate(config, params); !valid) {
        return valid;
    }
    const std::uint32_t n = config.frame;
    const std::uint32_t h = n / 2U;
    const std::uint32_t bins = h + 1U;
    if (tables.size() < 4U * static_cast<std::size_t>(n)) {
        return fail(std::format("reference_spectral: {} table entries where {} are needed",
                                tables.size(), 4U * n));
    }
    if (audio.size() < params.count) {
        return fail(std::format("reference_spectral: {} samples asked of a {} sample buffer",
                                params.count, audio.size()));
    }
    if (state.size() < spectral_state_size(config)) {
        return fail(std::format("reference_spectral: a {} float state where {} are needed",
                                state.size(), spectral_state_size(config)));
    }

    const ScopedDenormalFlush flush_denormals;

    const float* const cosine = tables.data();
    const float* const sine = cosine + n;
    const float* const analysis = sine + n;
    const float* const synthesis = analysis + n;

    float* const hist = state.data() + 2;
    float* const tail = hist + n;
    float* const queue = tail + h;
    float* const smoothed = queue + h;
    float* const minimum = smoothed + bins;

    auto phase = static_cast<std::uint32_t>(state[0]);
    float seen = state[1];
    const std::uint32_t mask = n - 1U;

    std::vector<float> work(n);
    std::vector<float> y_real(bins);
    std::vector<float> y_imag(bins);

    std::uint32_t s = 0;
    while (s < params.count) {
        const std::uint32_t length = std::min(h - phase, params.count - s);
        for (std::uint32_t i = 0; i < length; ++i) {
            const float x = audio[s + i];
            hist[h + phase + i] = x;
            audio[s + i] = queue[phase + i];
        }
        phase += length;
        s += length;
        if (phase != h) {
            continue;
        }

        // Analysis.
        for (std::uint32_t t = 0; t < n; ++t) {
            work[t] = hist[t] * analysis[t];
        }

        // Forward transform, gain, per bin.
        for (std::uint32_t k = 0; k < bins; ++k) {
            float re = 0.0F;
            float im = 0.0F;
            for (std::uint32_t t = 0; t < n; ++t) {
                const std::uint32_t at = (k * t) & mask;
                const float xc = work[t] * cosine[at];
                re = re + xc;
                const float xs = work[t] * sine[at];
                im = im - xs;
            }
            const float rr = re * re;
            const float ii = im * im;
            const float power = rr + ii;

            float level = power;
            float floor_power = power;
            if (seen != 0.0F) {
                const float previous = smoothed[k];
                const float kept = params.smoothing * previous;
                const float fresh = params.smoothing_complement * power;
                level = kept + fresh;
                const float held = minimum[k];
                if (held < level) {
                    const float decayed = params.gamma * held;
                    const float lagged = params.beta * previous;
                    const float lift = level - lagged;
                    const float risen = params.rise * lift;
                    floor_power = decayed + risen;
                } else {
                    floor_power = level;
                }
            }
            smoothed[k] = level;
            minimum[k] = floor_power;

            const float noise = params.bias * floor_power;
            const float ratio = noise * det_recip(level + params.eps);
            const float cut = params.alpha * ratio;
            float gain = 1.0F - cut;
            gain = std::max(gain, params.floor_gain);
            y_real[k] = gain * re;
            y_imag[k] = gain * im;
        }

        // Inverse transform and synthesis window, per sample.
        for (std::uint32_t t = 0; t < n; ++t) {
            float acc = 0.0F;
            for (std::uint32_t k = 1; k < h; ++k) {
                const std::uint32_t at = (k * t) & mask;
                const float a = y_real[k] * cosine[at];
                const float b = y_imag[k] * sine[at];
                const float term = a - b;
                acc = acc + term;
            }
            const float nyquist = ((t & 1U) != 0U) ? -y_real[h] : y_real[h];
            const float doubled = 2.0F * acc;
            float sum = y_real[0] + nyquist;
            sum = sum + doubled;
            work[t] = sum * synthesis[t];
        }

        // Overlap-add, and the older hop of history becomes the newer one's.
        for (std::uint32_t t = 0; t < h; ++t) {
            queue[t] = tail[t] + work[t];
            tail[t] = work[h + t];
            hist[t] = hist[h + t];
        }

        phase = 0;
        seen = std::min(seen + 1.0F, 2.0F);
    }

    state[0] = static_cast<float>(phase);
    state[1] = seen;
    return {};
}

// ---------------------------------------------------------------------------
// From a request to a plan
// ---------------------------------------------------------------------------

bool blanker_offered(engine::Demod mode) { return engine::produces_audio(mode); }

bool notch_offered(engine::Demod mode) {
    switch (mode) {
        case engine::Demod::Am:
        case engine::Demod::Usb:
        case engine::Demod::Lsb:
        case engine::Demod::Dsb:
        case engine::Demod::Cw: return true;
        case engine::Demod::Raw:
        case engine::Demod::Nfm:
        case engine::Demod::Wfm:
        case engine::Demod::P25p1:
        case engine::Demod::Dstar:
        case engine::Demod::Tetra:
        case engine::Demod::Dmr: return false;
    }
    return false;
}

bool auto_notch_offered(engine::Demod mode) {
    // CW is the one linear mode left out, and the reason is the whole point:
    // the tone the operator is copying is exactly what an adaptive line
    // enhancer learns and removes.
    return notch_offered(mode) && mode != engine::Demod::Cw;
}

bool noise_reduction_offered(engine::Demod mode) {
    // WFM is offered because a mono WFM receiver is one channel; the stage
    // declines a stereo one at plan time, see plan_noise.
    return engine::produces_audio(mode);
}

std::optional<double> notch_audio_hz(engine::Demod mode, Hertz passband_hz, Hertz cw_pitch) {
    const auto f = static_cast<double>(passband_hz);
    switch (mode) {
        case engine::Demod::Usb: return f;
        case engine::Demod::Lsb: return -f;
        case engine::Demod::Am:
        case engine::Demod::Dsb: return std::abs(f);
        case engine::Demod::Cw: return std::abs(f + static_cast<double>(cw_pitch));
        case engine::Demod::Raw:
        case engine::Demod::Nfm:
        case engine::Demod::Wfm:
        case engine::Demod::P25p1:
        case engine::Demod::Dstar:
        case engine::Demod::Tetra:
        case engine::Demod::Dmr: return std::nullopt;
    }
    return std::nullopt;
}

Status validate_noise_request(const engine::VrxParams& params) {
    const char* const mode = engine::demod_name(params.demod);
    if (params.nb_enabled && !blanker_offered(params.demod)) {
        return fail(std::format("the noise blanker is not offered on {}: it removes impulses "
                                "ahead of a demodulator, and {} hands out complex baseband",
                                mode, mode));
    }
    if (!(params.nb_threshold_db >= 3.0 && params.nb_threshold_db <= 40.0)) {
        return fail(std::format("a noise blanker threshold of {} dB is outside [3, 40]",
                                params.nb_threshold_db));
    }
    if (params.notch_enabled && !notch_offered(params.demod)) {
        return fail(std::format("the manual notch is not offered on {}: its audio frequency is "
                                "not a function of where a signal sits in the passband",
                                mode));
    }
    if (params.auto_notch_enabled && params.demod == engine::Demod::Cw) {
        return fail("the automatic notch is not offered on cw: it removes a steady tone, and "
                    "on cw the steady tone is the signal");
    }
    if (params.auto_notch_enabled && !auto_notch_offered(params.demod)) {
        return fail(std::format("the automatic notch is not offered on {}: it needs audio "
                                "band-limited by a linear mode's passband",
                                mode));
    }
    if (!(params.notch_depth_db >= 3.0 && params.notch_depth_db <= 80.0)) {
        return fail(std::format("a notch depth of {} dB is outside [3, 80]",
                                params.notch_depth_db));
    }
    if (params.notch_width_hz < 10 || params.notch_width_hz > 2000) {
        return fail(std::format("a notch width of {} Hz is outside [10, 2000]",
                                params.notch_width_hz));
    }
    if (params.notch_enabled) {
        const auto audio = notch_audio_hz(params.demod, params.notch_hz, params.cw_pitch);
        if (!audio || !(*audio > 0.0)) {
            return fail(std::format("a notch at {:+} Hz from the carrier lands on no audio "
                                    "frequency in {}: it is on the side the mode discards",
                                    params.notch_hz, mode));
        }
    }
    if (params.nr_enabled && !noise_reduction_offered(params.demod)) {
        return fail(std::format("noise reduction is not offered on {}, which hands out complex "
                                "baseband rather than audio",
                                mode));
    }
    if (!(params.nr_strength >= 0.0 && params.nr_strength <= 1.0)) {
        return fail(std::format("a noise reduction strength of {} is outside [0, 1]",
                                params.nr_strength));
    }
    return {};
}

Expected<NoisePlan> plan_noise(const engine::VrxParams& params, const VrxPlan& plan,
                               const LineConfig& line) {
    if (auto valid = validate_noise_request(params); !valid) {
        return std::unexpected(valid.error());
    }

    NoisePlan out;
    out.blank = params.nb_enabled;
    out.blank_threshold = blanker_threshold(params.nb_threshold_db);

    // The audio stages need one channel of audio. A stereo WFM receiver
    // declines them here rather than being refused: stereo is decided by the
    // station's pilot and the rate, not by anything the operator set, and a
    // receiver asked for noise reduction on a mono station that later turns
    // out to be stereo should not start failing.
    const bool mono = plan.demod.channels == 1U;
    const SampleRate fa = plan.output_rate;

    LineParams lp;
    lp.eps = to_float(kLineEps);
    lp.mu = to_float(kLineMu);
    lp.leak = to_float(kLineLeak);

    if (mono && params.notch_enabled) {
        // A notch outside the granted passband sits on audio the filter has
        // already removed, so it is not applied rather than being refused:
        // dragging an edge past it should not fail the drag.
        const Hertz at = params.notch_hz;
        const bool inside = at > plan.passband.low && at < plan.passband.high;
        const auto audio = notch_audio_hz(params.demod, at, params.cw_pitch);
        if (inside && audio && *audio > 0.0 && *audio < 0.45 * static_cast<double>(fa)) {
            auto design = design_notch(*audio, params.notch_depth_db,
                                       static_cast<double>(params.notch_width_hz), fa);
            if (!design) {
                return std::unexpected(with_context(design.error(), "receiver notch"));
            }
            lp.flags |= kLineNotch;
            lp.b0 = to_float(design->b0);
            lp.b1 = to_float(design->b1);
            lp.b2 = to_float(design->b2);
            lp.a1 = to_float(design->a1);
            lp.a2 = to_float(design->a2);
        }
    }

    if (mono && params.auto_notch_enabled) {
        const double reach = std::max(std::abs(static_cast<double>(plan.passband.low)),
                                      std::abs(static_cast<double>(plan.passband.high)));
        const double stride =
            reach > 0.0 ? std::floor(static_cast<double>(fa) / (kLineStrideMargin * reach)) : 1.0;
        lp.stride = static_cast<std::uint32_t>(
            std::clamp(stride, 1.0, static_cast<double>(kMaxLineStride)));
        lp.delay = static_cast<std::uint32_t>(
            std::max(1.0, std::round(kLineDelaySeconds * static_cast<double>(fa))));
        lp.flags |= kLineAle;
    }

    if (lp.flags != 0U) {
        if (auto valid = validate(line, lp); !valid) {
            return std::unexpected(with_context(valid.error(), "receiver notch"));
        }
        out.line = true;
        out.line_params = lp;
    }

    if (mono && params.nr_enabled) {
        out.spectral = true;
        out.spectral_params = spectral_params(params.nr_strength);
    }
    return out;
}

}  // namespace revenant::dsp
