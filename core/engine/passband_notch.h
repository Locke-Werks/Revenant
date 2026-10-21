// The manual notches as the passband display shows them.
//
// The notch runs on demodulated audio, and the passband display is taken
// from a tap ahead of the fine filter, so without this the detail view
// shows neither where a notch sits nor what it removes. Here a notch is
// mapped back into the passband by mode (core/dsp/notch_map.h) and its
// section's response (dsp::notch_section_db) is added to each bin, which is
// what the audio actually loses there. Display only: the audio path and the
// span view never see this.
//
// Pure host arithmetic over a frame already read back, so it is tested
// without a device in tests/engine/test_passband_notch.cpp.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/dsp/noise_reference.h"
#include "core/engine/engine.h"
#include "core/engine/vrx.h"

namespace revenant::engine {

// AM, SAM and DSB place each notch twice.
inline constexpr std::size_t kMaxPassbandNotchMarks = 2 * kMaxVrxNotches;

// What a receiver's display needs of its request, snapshotted per frame.
struct PassbandNotchRequest {
    Demod demod = Demod::Nfm;
    dsp::Hertz cw_pitch = 0;
    dsp::SampleRate audio_rate = 0;
    dsp::NotchSet notches{};
};

struct PassbandNotchMarks {
    std::array<PassbandNotchMark, kMaxPassbandNotchMarks> marks{};
    std::size_t count = 0;
    bool audio_only = false;

    [[nodiscard]] std::span<const PassbandNotchMark> view() const {
        return {marks.data(), count};
    }
};

// Where each enabled notch lands in the passband, in VrxParams' frame. On an
// audio-only mode each enabled notch is listed at its control offset with
// audio_only set, and nothing should be applied.
[[nodiscard]] PassbandNotchMarks map_passband_notches(const PassbandNotchRequest& request);

// Adds each mark's section response to power_db in place. bin_zero_hz is bin
// zero's offset from the receiver's centre. excluded, as long as power_db,
// is set to 1 for each bin inside a mark's width and 0 elsewhere. Returns how
// many bins were excluded. Does nothing on an audio-only mapping.
std::size_t shape_passband(std::span<float> power_db, double bin_width_hz, double bin_zero_hz,
                           const PassbandNotchRequest& request,
                           const PassbandNotchMarks& marks,
                           std::span<std::uint8_t> excluded);

// The low and high percentiles of power_db over the bins not excluded, by
// the same buckets the device kernel uses. scratch holds at least
// power_db.size() floats. False when every bin is excluded.
[[nodiscard]] bool measure_outside(std::span<const float> power_db,
                                   std::span<const std::uint8_t> excluded,
                                   std::span<float> scratch, std::array<float, 2>& levels);

}  // namespace revenant::engine
