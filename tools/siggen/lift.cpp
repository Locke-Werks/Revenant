// tools/siggen/lift.h has what this does.

#include "tools/siggen/lift.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <random>

#include "core/dsp/synth/channel.h"

namespace revenant::siggen_lift {
namespace {

// Taps per polyphase branch of the interpolator and its Kaiser beta. The
// same figures tests/engine/test_engine_probe.cpp's interpolator uses for its
// OFDM burst.
constexpr std::size_t kTapsPerPhase = 24;
constexpr double kKaiserBeta = 8.0;

// Abramowitz and Stegun 9.6.12.
[[nodiscard]] double bessel_i0(double x) {
    const double quarter_square = 0.25 * x * x;
    double term = 1.0;
    double sum = 1.0;
    for (int k = 1; k < 256; ++k) {
        term *= quarter_square / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < 1e-18 * sum) {
            break;
        }
    }
    return sum;
}

struct Lift {
    const LiftedEmitter* emitter = nullptr;
    std::size_t factor = 1;
    std::vector<float> taps;  // kTapsPerPhase * factor, scaled so power is kept
    double gain = 1.0;
};

[[nodiscard]] Lift make_lift(const LiftedEmitter& emitter, dsp::SampleRate out_rate, double gain) {
    Lift lift;
    lift.emitter = &emitter;
    lift.factor = static_cast<std::size_t>(out_rate / emitter.rate);
    lift.gain = gain;
    const std::size_t length = kTapsPerPhase * lift.factor;
    const double centre = (static_cast<double>(length) - 1.0) / 2.0;
    const double cutoff = 0.45 * static_cast<double>(emitter.rate) / static_cast<double>(out_rate);
    const double i0 = bessel_i0(kKaiserBeta);
    std::vector<double> taps(length);
    double sum = 0.0;
    for (std::size_t i = 0; i < length; ++i) {
        const double position = static_cast<double>(i) - centre;
        const double ratio = 2.0 * static_cast<double>(i) / static_cast<double>(length - 1) - 1.0;
        const double window =
            bessel_i0(kKaiserBeta * std::sqrt(std::max(0.0, 1.0 - ratio * ratio))) / i0;
        const double x = 2.0 * cutoff * position;
        const double sinc = x == 0.0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
        taps[i] = 2.0 * cutoff * sinc * window;
        sum += taps[i];
    }
    const double scale = static_cast<double>(lift.factor) / sum;
    lift.taps.resize(length);
    for (std::size_t i = 0; i < length; ++i) {
        lift.taps[i] = static_cast<float>(taps[i] * scale);
    }
    return lift;
}

}  // namespace

void normalise(std::vector<dsp::Complex32>& samples) {
    double power = 0.0;
    for (const dsp::Complex32 sample : samples) {
        power += static_cast<double>(std::norm(sample));
    }
    power /= static_cast<double>(std::max<std::size_t>(samples.size(), 1));
    if (!(power > 0.0)) {
        return;
    }
    const auto scale = static_cast<float>(1.0 / std::sqrt(power));
    for (dsp::Complex32& sample : samples) {
        sample *= scale;
    }
}

Status render_lifted(std::vector<LiftedEmitter>& emitters, const LiftSpec& spec,
                     const BlockSink& sink) {
    if (spec.rate <= 0) {
        return fail("render_lifted: the capture needs a rate");
    }
    for (LiftedEmitter& emitter : emitters) {
        if (emitter.rate <= 0 || spec.rate % emitter.rate != 0 || emitter.samples.empty()) {
            return fail(std::format("render_lifted: the {} emitter at {} S/s does not divide {} "
                                    "S/s, or is empty",
                                    emitter.name, emitter.rate, spec.rate));
        }
        normalise(emitter.samples);
    }

    // Every emitter at its SNR in 2500 Hz against the noise's density.
    const double noise_power = std::pow(10.0, spec.noise_dbfs / 10.0);
    const double noise_in_reference = noise_power * 2500.0 / static_cast<double>(spec.rate);
    std::vector<Lift> lifts;
    for (const LiftedEmitter& emitter : emitters) {
        const double signal_power =
            noise_in_reference * std::pow(10.0, emitter.snr_2500_db / 10.0);
        lifts.push_back(make_lift(emitter, spec.rate, std::sqrt(signal_power)));
    }

    constexpr std::size_t kBlock = 1U << 16;
    std::vector<std::complex<float>> block(kBlock);
    std::mt19937_64 noise_engine(siggen::derive_seed(spec.seed, 0x4E015E));
    const double sigma = std::sqrt(noise_power / 2.0);

    for (std::uint64_t first = 0; first < spec.total_samples; first += kBlock) {
        const std::size_t count =
            static_cast<std::size_t>(std::min<std::uint64_t>(kBlock, spec.total_samples - first));
        for (std::size_t n = 0; n < count; n += 2) {
            const double u1 = (static_cast<double>(noise_engine() >> 11) + 1.0) / 9007199254740993.0;
            const double u2 = static_cast<double>(noise_engine() >> 11) / 9007199254740992.0;
            const double radius = sigma * std::sqrt(-2.0 * std::log(u1));
            block[n] = {static_cast<float>(radius * std::cos(2.0 * std::numbers::pi * u2)),
                        static_cast<float>(radius * std::sin(2.0 * std::numbers::pi * u2))};
            if (n + 1 < count) {
                const double u3 = (static_cast<double>(noise_engine() >> 11) + 1.0) / 9007199254740993.0;
                const double u4 = static_cast<double>(noise_engine() >> 11) / 9007199254740992.0;
                const double r2 = sigma * std::sqrt(-2.0 * std::log(u3));
                block[n + 1] = {static_cast<float>(r2 * std::cos(2.0 * std::numbers::pi * u4)),
                                static_cast<float>(r2 * std::sin(2.0 * std::numbers::pi * u4))};
            }
        }

        for (const Lift& lift : lifts) {
            const LiftedEmitter& emitter = *lift.emitter;
            const std::size_t loop = emitter.samples.size();
            const std::size_t length = lift.taps.size();
            for (std::size_t n = 0; n < count; ++n) {
                const std::uint64_t m = first + n;
                // y[m] = sum over input k of x[k] h[m - k L], h of `length`
                // taps, so k runs from ceil((m - length + 1) / L) to m / L.
                const std::uint64_t k_high = m / lift.factor;
                std::complex<float> acc(0.0F, 0.0F);
                for (std::uint64_t k = k_high + 1; k-- > 0;) {
                    const std::uint64_t offset = m - k * lift.factor;
                    if (offset >= length) {
                        break;
                    }
                    acc += lift.taps[offset] * emitter.samples[static_cast<std::size_t>(k % loop)];
                }
                // Integer reduction of the mixer's phase, so it does not
                // drift over a long capture.
                std::int64_t turns = (emitter.offset_hz * static_cast<std::int64_t>(m)) %
                                     static_cast<std::int64_t>(spec.rate);
                if (turns < 0) {
                    turns += spec.rate;
                }
                const double angle = 2.0 * std::numbers::pi * static_cast<double>(turns) /
                                     static_cast<double>(spec.rate);
                const std::complex<float> mixer(static_cast<float>(std::cos(angle)),
                                                static_cast<float>(std::sin(angle)));
                block[n] += static_cast<float>(lift.gain) * acc * mixer;
            }
        }

        if (auto sent = sink(std::span<const std::complex<float>>(block.data(), count)); !sent) {
            return sent;
        }
    }
    return {};
}

}  // namespace revenant::siggen_lift
