// CPU twin of core/shaders/spectrum_levels.comp: two percentiles of a spectrum
// frame.
//
// This is a referee, not a library. It exists so CI can prove that the kernel
// which sets the display's colour map computes what it claims to, on every
// vendor's driver, and it is written to match the kernel's operation ORDER
// rather than merely its mathematical result. Read the two files side by side;
// every statement here has a counterpart there, in the same order, the way
// core/dsp/spectrum_reference.cpp reads against core/shaders/spectrum.comp.
//
// WHAT THE STAGE IS, AND WHY A HISTOGRAM
//
// docs/ui-spectrum.md asks for the colour map's ends to follow a low and a
// high percentile of the frame rather than its minimum and maximum, and asks
// for the measurement to happen on the device. An exact percentile is an order
// statistic and wants a sort; a sort of 65536 floats on the GPU is a hundred
// and twenty-eight passes and does not fit one workgroup's shared memory. A
// histogram over the decibel axis is one pass, and the decibel axis is already
// bounded: the spectrum kernel clamps power at 1e-20 before its logarithm, so
// nothing is below -200 dB, and nothing an SDR converter can produce is above
// +40 dBFS.
//
// The error this costs is two quantisations and no approximation:
//
//   The value is the CENTRE of the bucket the true percentile falls in, so it
//   is wrong by at most half a bucket, which is 0.1171875 dB.
//
//   The rank is floor(bins * permille / 1000), so the fraction reported
//   differs from the fraction asked for by under 1/bins, which is 0.0015
//   percent at 65536 bins.
//
// Both are far under what a colour map or a detector threshold can see. The
// case in tests/reference/test_spectrum_levels.cpp measures the first against
// an exact host sort rather than taking this paragraph's word for it, the same
// way the logarithm is measured against std::log2.
//
// WHY IT IS REFEREEABLE AT ALL
//
// The counting is integer addition, which commutes, so the histogram does not
// depend on the order the GPU's invocations arrive in. A floating-point
// reduction would, and could not be compared bit for bit on two devices with
// different scheduling. Everything else is a subtract, a multiply, two
// comparisons and a truncation.
//
// Written from published mathematics only, per docs/clean-room.md: the
// histogram-and-cumulative-count percentile is textbook order statistics. No
// GPL-licensed implementation was read.

#pragma once

#include <cstddef>
#include <cstdint>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::dsp {

// Buckets in the histogram, and the decibel range they cover.
//
// Every one of these is written with the same digits as its counterpart in
// core/shaders/spectrum_levels.comp, so both sides round to the same float.
// Duplicated rather than shared because GLSL has no way to read a C++ header.
//
// 1024 over 240 dB is 0.234375 dB per bucket, which is exact in binary and so
// gives the two sides nothing to disagree about. The bottom of the range is
// kSpectrumFloorDb exactly, so a frame full of silence quantises without
// clamping.
inline constexpr std::uint32_t kSpectrumLevelsBuckets = 1024;
inline constexpr float kSpectrumLevelsRangeFloorDb = -200.0F;
inline constexpr float kSpectrumLevelsRangeCeilingDb = 40.0F;
inline constexpr float kSpectrumLevelsBucketsPerDb = 4.26666666666666666F;
inline constexpr float kSpectrumLevelsDbPerBucket = 0.234375F;

// The two percentiles the engine measures, in parts per thousand.
//
// docs/ui-spectrum.md asks for "something like the fifth and the
// ninety-ninth", and the reason it is a range rather than a number is that
// neither end is a measurement of anything: the fifth is low enough to sit
// under the noise floor without being dragged down by a stuck bin, and the
// ninety-ninth is high enough to reach the body of a strong signal without
// being pulled up by one spur. Per mille rather than per cent because the
// interesting part of the high end is between 99 and 99.9 percent, which per
// cent cannot express.
//
// 999 was measured against 990 on both the FM broadcast band and the
// eight-emitter synthetic scene, on the reasoning that a span whose signals
// occupy under one percent of it leaves the high percentile sitting in the
// noise. On the synthetic scene it changed nothing: those emitters occupy
// about sixty-four bins of sixty-five thousand, which is a tenth of a
// percent, so no percentile short of the maximum reaches them and the
// minimum span is what keeps that display sane. That is the case
// docs/ui-spectrum.md accepts when it says percentiles rather than extremes,
// and it is why SpectrumScaleConfig::minimum_span_db is not decoration.
inline constexpr std::uint32_t kSpectrumLowPermille = 50;
inline constexpr std::uint32_t kSpectrumHighPermille = 990;

// Floats the kernel writes: the low percentile then the high one.
inline constexpr std::size_t kSpectrumLevelsOutputs = 2;

// The kernel's push constant block. It has no specialization constants beyond
// the workgroup size, which is deliberate: core/shaders/spectrum.comp builds a
// pipeline per grid shape and docs/fft.md records an integrated-device fault
// that tracks exactly that churn, so a kernel that needs none does not create
// any.
struct SpectrumLevelsParams {
    // Values in the frame to measure.
    std::uint32_t bins = 0;

    std::uint32_t low_permille = kSpectrumLowPermille;
    std::uint32_t high_permille = kSpectrumHighPermille;
};

// Rejects a parameter set the kernel cannot run.
[[nodiscard]] Status validate(const SpectrumLevelsParams& params);

// Which bucket a decibel value falls in, exactly as the kernel decides it.
//
// Exposed because the bucket boundary is where the two implementations would
// disagree if either fused its multiply-add, and a test that can call this
// directly can walk a value across a boundary instead of hoping a random frame
// lands on one.
[[nodiscard]] std::uint32_t spectrum_levels_bucket_of(float decibels);

// The decibel value the kernel reports for a bucket: its centre.
[[nodiscard]] float spectrum_levels_value_of(std::uint32_t bucket);

// Twin of core/shaders/spectrum_levels.comp, whole.
//
// frame is the decibel frame core/shaders/spectrum.comp produced, at least
// params.bins long. levels receives kSpectrumLevelsOutputs values: the low
// percentile then the high one.
[[nodiscard]] Status reference_spectrum_levels(const SpectrumLevelsParams& params,
                                               ConstRealSpan frame,
                                               RealSpan levels);

}  // namespace revenant::dsp
