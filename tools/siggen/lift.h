// Emitters rendered at their own rate, lifted onto one wideband capture.
//
// Shared by `siggen labelled` and `siggen voice`, and by the voice survey in
// tests/detect, which renders the same scene rather than a copy of it. Each
// emitter is one loop of complex baseband at a rate that divides the
// wideband rate; it is interpolated by the whole factor through a Kaiser
// windowed sinc cut at 0.45 of its own rate, mixed to its offset with an
// integer-reduced phase so a long capture does not drift, set to its SNR in
// 2500 Hz against the noise's density, and summed with complex Gaussian
// noise.

#pragma once

#include <complex>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen_lift {

struct LiftedEmitter {
    std::string name;
    dsp::Hertz offset_hz = 0;
    dsp::SampleRate rate = 0;

    // One loop of it. Normalised to unit mean power by render_lifted.
    std::vector<dsp::Complex32> samples;

    // Its mean power over the loop, in 2500 Hz, over the noise in 2500 Hz.
    double snr_2500_db = 25.0;
};

struct LiftSpec {
    dsp::SampleRate rate = 0;
    std::uint64_t total_samples = 0;
    std::uint64_t seed = 1;
    double noise_dbfs = -60.0;
};

// Called with each block of the capture in order.
using BlockSink = std::function<Status(std::span<const std::complex<float>>)>;

// Every emitter's rate has to divide spec.rate.
[[nodiscard]] Status render_lifted(std::vector<LiftedEmitter>& emitters, const LiftSpec& spec,
                                   const BlockSink& sink);

// Unit mean power, in place.
void normalise(std::vector<dsp::Complex32>& samples);

}  // namespace revenant::siggen_lift
