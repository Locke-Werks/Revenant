// The audio spectrum strip: a low-rate power spectrum of a receiver's
// demodulated audio, after the notch and the noise reduction.
//
// The passband display is taken ahead of the demodulator, so in FM it cannot
// show a notch at all and in no mode can it show the noise reduction. This
// reads the audio the completion thread already has on the host, which is
// the stage's output after core/engine/noise_stage.h ran, so what it draws is
// what the listener gets. Computed only on the frames that carry a passband,
// and only for a receiver with VrxParams::display_audio_spectrum set.
//
// Pure host arithmetic, tested without a device in
// tests/engine/test_audio_spectrum.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/dsp/types.h"
#include "core/engine/passband_notch.h"

namespace revenant::engine {

// Where each enabled manual notch sits in the audio, which is where its
// section runs: core/dsp/noise_reference.h's notch_audio_hz, as a magnitude.
// The strip draws these as ticks. Writes at most out.size() and returns how
// many; a mode with no mapping writes none.
std::size_t audio_notch_frequencies(const PassbandNotchRequest& request, std::span<double> out);

// 1024 points at 48 kHz is 47 Hz a bin, fine enough to see a 100 Hz notch.
inline constexpr std::size_t kAudioSpectrumPoints = 1024;
inline constexpr std::size_t kAudioSpectrumBins = kAudioSpectrumPoints / 2 + 1;

class AudioSpectrum {
public:
    // Smoothing per computed frame, on linear power. 0.3 settles in about
    // ten frames, a third of a second at the display rate.
    explicit AudioSpectrum(float smoothing = 0.3F);

    // Forgets the history: a retune, or a change of rate or channel count.
    void reset();

    // Appends audio to the window, downmixed to mono. Interleaved frames of
    // `channels` floats. A rate different from the last call restarts.
    void push(std::span<const float> audio, std::uint32_t channels, dsp::SampleRate rate);

    // Transforms the newest kAudioSpectrumPoints samples and folds them into
    // the average. False until a whole window has arrived. A full-scale sine
    // reads 0 dB at its bin.
    bool compute();

    // Bin k is k * bin_hz(), from DC to the audio Nyquist inclusive.
    [[nodiscard]] std::span<const float> power_db() const { return power_db_; }
    [[nodiscard]] double bin_hz() const;
    [[nodiscard]] dsp::SampleRate rate() const { return rate_; }

private:
    float smoothing_;
    dsp::SampleRate rate_ = 0;
    std::vector<float> ring_;
    std::size_t write_ = 0;
    std::size_t filled_ = 0;
    std::vector<float> window_;
    float scale_ = 1.0F;
    std::vector<float> re_;
    std::vector<float> im_;
    std::vector<float> cos_;
    std::vector<float> sin_;
    std::vector<std::uint32_t> reversed_;
    std::vector<float> average_;
    bool have_average_ = false;
    std::vector<float> power_db_;
};

}  // namespace revenant::engine
