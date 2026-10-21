#include "core/engine/audio_spectrum.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace revenant::engine {

namespace {

constexpr std::size_t kPoints = kAudioSpectrumPoints;

constexpr std::uint32_t log2_of(std::size_t n) {
    std::uint32_t bits = 0;
    while ((std::size_t{1} << bits) < n) {
        ++bits;
    }
    return bits;
}

constexpr std::uint32_t kStages = log2_of(kPoints);
static_assert((std::size_t{1} << kStages) == kPoints, "the transform is radix 2");

// Far below anything a float audio path can carry, so silence reads as a
// floor rather than as minus infinity.
constexpr float kFloorPower = 1e-14F;

}  // namespace

std::size_t audio_notch_frequencies(const PassbandNotchRequest& request, std::span<double> out) {
    std::size_t count = 0;
    for (const dsp::NotchSpec& notch : request.notches) {
        if (!notch.enabled || count == out.size()) {
            continue;
        }
        const auto hz = dsp::notch_audio_hz(request.demod, notch.hz, request.cw_pitch);
        if (!hz.has_value()) {
            continue;
        }
        out[count++] = std::abs(*hz);
    }
    return count;
}

AudioSpectrum::AudioSpectrum(float smoothing)
    : smoothing_(std::clamp(smoothing, 0.01F, 1.0F)),
      ring_(kPoints, 0.0F),
      window_(kPoints),
      re_(kPoints),
      im_(kPoints),
      cos_(kPoints / 2),
      sin_(kPoints / 2),
      reversed_(kPoints),
      average_(kAudioSpectrumBins, 0.0F),
      power_db_(kAudioSpectrumBins, 10.0F * std::log10(kFloorPower)) {
    double sum = 0.0;
    for (std::size_t i = 0; i < kPoints; ++i) {
        const double w =
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                 static_cast<double>(kPoints));
        window_[i] = static_cast<float>(w);
        sum += w;
    }
    // A sine of amplitude A leaves A * sum / 2 in its bin, so this reads A.
    scale_ = static_cast<float>(2.0 / sum);

    for (std::size_t k = 0; k < kPoints / 2; ++k) {
        const double angle =
            -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(kPoints);
        cos_[k] = static_cast<float>(std::cos(angle));
        sin_[k] = static_cast<float>(std::sin(angle));
    }
    for (std::uint32_t i = 0; i < kPoints; ++i) {
        std::uint32_t r = 0;
        for (std::uint32_t b = 0; b < kStages; ++b) {
            r |= ((i >> b) & 1U) << (kStages - 1U - b);
        }
        reversed_[i] = r;
    }
}

void AudioSpectrum::reset() {
    std::ranges::fill(ring_, 0.0F);
    write_ = 0;
    filled_ = 0;
    have_average_ = false;
}

void AudioSpectrum::push(std::span<const float> audio, std::uint32_t channels,
                         dsp::SampleRate rate) {
    if (channels == 0 || rate == 0) {
        return;
    }
    if (rate != rate_) {
        reset();
        rate_ = rate;
    }
    const std::size_t frames = audio.size() / channels;
    const float share = 1.0F / static_cast<float>(channels);
    for (std::size_t f = 0; f < frames; ++f) {
        float sum = 0.0F;
        for (std::uint32_t c = 0; c < channels; ++c) {
            sum += audio[f * channels + c];
        }
        ring_[write_] = sum * share;
        write_ = (write_ + 1) % kPoints;
    }
    filled_ = std::min(kPoints, filled_ + frames);
}

bool AudioSpectrum::compute() {
    if (filled_ < kPoints) {
        return false;
    }
    // Oldest sample first, loaded in bit-reversed order for the in-place
    // decimation-in-time butterflies below.
    for (std::size_t i = 0; i < kPoints; ++i) {
        const float sample = ring_[(write_ + i) % kPoints] * window_[i];
        re_[reversed_[i]] = sample;
        im_[reversed_[i]] = 0.0F;
    }
    for (std::uint32_t stage = 0; stage < kStages; ++stage) {
        const std::size_t half = std::size_t{1} << stage;
        const std::size_t step = kPoints / (2 * half);
        for (std::size_t start = 0; start < kPoints; start += 2 * half) {
            for (std::size_t j = 0; j < half; ++j) {
                const float wr = cos_[j * step];
                const float wi = sin_[j * step];
                const std::size_t a = start + j;
                const std::size_t b = a + half;
                const float tr = re_[b] * wr - im_[b] * wi;
                const float ti = re_[b] * wi + im_[b] * wr;
                re_[b] = re_[a] - tr;
                im_[b] = im_[a] - ti;
                re_[a] += tr;
                im_[a] += ti;
            }
        }
    }
    const float alpha = have_average_ ? smoothing_ : 1.0F;
    for (std::size_t k = 0; k < kAudioSpectrumBins; ++k) {
        const float mr = re_[k] * scale_;
        const float mi = im_[k] * scale_;
        const float power = mr * mr + mi * mi;
        average_[k] += alpha * (power - average_[k]);
        power_db_[k] = 10.0F * std::log10(std::max(average_[k], kFloorPower));
    }
    have_average_ = true;
    return true;
}

double AudioSpectrum::bin_hz() const {
    return static_cast<double>(rate_) / static_cast<double>(kPoints);
}

}  // namespace revenant::engine
