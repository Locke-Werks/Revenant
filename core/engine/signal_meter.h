// The per-receiver signal meter's arithmetic.
//
// Lifted out of core/engine/graph.cpp because it was wrong for two years'
// worth of a day and nothing could have caught it there: the meter sits
// inside on_block, behind a Vulkan readback, and the only way to assert on
// it was to run a receiver on a device and believe the number that came out.
// What can actually be wrong is which divisor a buffer of interleaved floats
// takes, and that is four lines of arithmetic over a span.
//
// WHY THE DIVISOR IS THE WHOLE OF IT. A stage hands the graph one buffer of
// floats and a channel count, and two different things arrive with a channel
// count of two. The raw tap's pair is one complex sample, so its power is
// re*re + im*im per FRAME and the mean is taken over frames. A stereo WFM
// receiver's pair is two real audio channels of one programme, so its power
// is the mean over SAMPLES, the same as mono. Taking the complex divisor to a
// stereo pair reads 10*log10(2) = 3.01 dB high, which moved every stereo
// receiver's meter and, because the squelch compares against the same number,
// opened gates the operator had set shut.

#pragma once

#include <cmath>
#include <cstdint>
#include <span>

namespace revenant::engine {

// Where the meter bottoms out. A receiver with a silent buffer has no
// logarithm, and a float's worth of denormal noise should not read as a
// signal 700 dB down either.
inline constexpr double kSilenceFloorDbfs = -200.0;

[[nodiscard]] inline double dbfs_of(double rms) {
    if (!(rms > 0.0)) {
        return kSilenceFloorDbfs;
    }
    const double value = 20.0 * std::log10(rms);
    return value < kSilenceFloorDbfs ? kSilenceFloorDbfs : value;
}

// dBFS of one dispatch's audio.
//
// complex_iq says what a pair of floats IS, and it is the only thing that
// distinguishes the two callers: StageOutput::channels reads 2 for the raw
// complex tap and for a WFM receiver decoding stereo, and those two want
// different divisors. It is deliberately not inferred from the channel count
// here, because inferring it is exactly the bug.
//
// A station with no pilot is what makes this checkable: the stereo kernel
// gates the difference channel by multiplying it by zero, so L and R come
// back bit-identical and the meter has to read what the same programme reads
// as mono. tests/engine/test_signal_meter.cpp asserts that equality.
[[nodiscard]] inline double meter_dbfs(std::span<const float> audio, std::uint32_t frames,
                                       bool complex_iq) {
    if (audio.empty() || frames == 0) {
        return kSilenceFloorDbfs;
    }

    double sum = 0.0;
    for (const float value : audio) {
        const double v = static_cast<double>(value);
        sum += v * v;
    }

    const double divisor = complex_iq ? static_cast<double>(frames)
                                      : static_cast<double>(audio.size());
    return dbfs_of(std::sqrt(sum / divisor));
}

}  // namespace revenant::engine
