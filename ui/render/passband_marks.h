// The detail spectrum's notch and carrier marks, as arithmetic: where an audio
// notch is drawn, what audio frequency a dragged band is asking for, which
// band a pointer is on, and which tracked carriers a "notch all" fills the
// four slots with.
//
// No Qt in here, so ui/tests can assert it without a window. PassbandItem
// owns the drawing and the gestures.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/rpc/types.h"

namespace revenant::ui {

// The engine runs four notch sections in cascade, so a "notch all" can fill no
// more than that.
inline constexpr std::size_t kNotchSlots = 4;

// The width a click-to-notch asks for, from the peak's -20 dB width. Below
// twenty hertz the notch is narrower than the passband display can show and
// than most carriers stay put to; above a kilohertz it is taking out speech,
// not a carrier.
inline constexpr double kClickNotchMinWidthHz = 20.0;
inline constexpr double kClickNotchMaxWidthHz = 1'000.0;

// A notch narrower than this on screen is still grabbable at this half width.
inline constexpr double kNotchGrabMinHalfPx = 5.0;

// Bins either side of the hovered one searched for the local peak, so the
// readout names the carrier under the pointer and not the slope beside it.
inline constexpr std::size_t kHoverPeakBins = 3;

struct MarkOffsets {
    std::array<double, 2> hz{};
    std::uint8_t count = 0;
    bool audio_only = false;
};

// Audio hertz to signed offsets from the receiver's centre. The same table as
// dsp::notch_offsets in core/dsp/notch_map.h, on the wire's Demod so the UI
// does not pull the engine headers in. A change to one is a change to both.
[[nodiscard]] constexpr MarkOffsets mark_offsets(rpc::Demod mode, double audio_hz,
                                                 double cw_pitch_hz)
{
    MarkOffsets out;
    switch (mode) {
        case rpc::Demod::Usb:
            out.hz[0] = audio_hz;
            out.count = 1;
            break;
        case rpc::Demod::Lsb:
            out.hz[0] = -audio_hz;
            out.count = 1;
            break;
        case rpc::Demod::Am:
        case rpc::Demod::Sam:
        case rpc::Demod::Dsb:
            out.hz[0] = -audio_hz;
            out.hz[1] = audio_hz;
            out.count = 2;
            break;
        case rpc::Demod::Cw:
            out.hz[0] = audio_hz - cw_pitch_hz;
            out.count = 1;
            break;
        case rpc::Demod::Nfm:
        case rpc::Demod::Wfm:
        case rpc::Demod::P25p1:
        case rpc::Demod::Dstar:
        case rpc::Demod::Tetra:
        case rpc::Demod::Dmr: out.audio_only = true; break;
        case rpc::Demod::Raw: break;
    }
    if (!(audio_hz > 0.0)) {
        out.count = 0;
    }
    return out;
}

// The inverse: the audio frequency an offset is heard at, or zero where it is
// heard at none (the discarded sideband, the carrier, an audio-only mode).
// Either AM sideband maps to the offset's magnitude.
[[nodiscard]] constexpr double audio_at_offset(rpc::Demod mode, double offset_hz,
                                               double cw_pitch_hz)
{
    double audio = 0.0;
    switch (mode) {
        case rpc::Demod::Usb: audio = offset_hz; break;
        case rpc::Demod::Lsb: audio = -offset_hz; break;
        case rpc::Demod::Am:
        case rpc::Demod::Sam:
        case rpc::Demod::Dsb: audio = offset_hz < 0.0 ? -offset_hz : offset_hz; break;
        case rpc::Demod::Cw: audio = offset_hz + cw_pitch_hz; break;
        case rpc::Demod::Nfm:
        case rpc::Demod::Wfm:
        case rpc::Demod::P25p1:
        case rpc::Demod::Dstar:
        case rpc::Demod::Tetra:
        case rpc::Demod::Dmr:
        case rpc::Demod::Raw: break;
    }
    return audio > 0.0 ? audio : 0.0;
}

[[nodiscard]] constexpr double click_notch_width(double peak_width_hz)
{
    return std::clamp(peak_width_hz, kClickNotchMinWidthHz, kClickNotchMaxWidthHz);
}

// One drawn notch band, in pixels, tagged with the notch it belongs to. An AM
// notch is two of these with the same index.
struct NotchBandPx {
    double centre_px = 0.0;
    double half_width_px = 0.0;
    int index = -1;
};

// The notch under x, or -1. The nearest centre wins where two overlap, so two
// close notches can both be grabbed.
[[nodiscard]] inline int notch_hit(std::span<const NotchBandPx> bands, double x_px)
{
    int best = -1;
    double best_distance = 0.0;
    for (const NotchBandPx& band : bands) {
        const double distance = std::abs(x_px - band.centre_px);
        if (distance > std::max(band.half_width_px, kNotchGrabMinHalfPx)) {
            continue;
        }
        if (best < 0 || distance < best_distance) {
            best = band.index;
            best_distance = distance;
        }
    }
    return best;
}

// The hovered bin's local peak: the strongest bin within kHoverPeakBins of
// the one under the pointer. found is false when the offset is off the frame.
[[nodiscard]] inline float hover_peak_db(std::span<const float> power_db, double bin_hz,
                                         double bin_zero_hz, double offset_hz, bool& found)
{
    found = false;
    if (power_db.empty() || !(bin_hz > 0.0)) {
        return 0.0F;
    }
    // Bin k is centred on bin_zero + k bins, as in find_nearest_peak.
    const double at = std::floor((offset_hz - bin_zero_hz) / bin_hz + 0.5);
    const auto last = static_cast<double>(power_db.size() - 1);
    if (at < 0.0 || at > last) {
        return 0.0F;
    }
    const auto centre = static_cast<std::size_t>(at);
    const std::size_t lo = centre > kHoverPeakBins ? centre - kHoverPeakBins : 0;
    const std::size_t hi = std::min(centre + kHoverPeakBins, power_db.size() - 1);
    float peak = power_db[lo];
    for (std::size_t bin = lo + 1; bin <= hi; ++bin) {
        peak = std::max(peak, power_db[bin]);
    }
    found = true;
    return peak;
}

struct CarrierMark {
    double offset_hz = 0.0;
    double width_hz = 0.0;
    float level_db = 0.0F;
};

struct NotchRequest {
    double audio_hz = 0.0;
    double width_hz = 0.0;
};

// What "notch all" asks for: the strongest carriers first, each at the audio
// frequency it is heard at, up to the four slots. A carrier heard at no audio
// frequency is skipped, and so is one landing on an audio frequency already
// taken, which is the other sideband of an AM carrier already chosen.
[[nodiscard]] inline std::vector<NotchRequest> notch_all_requests(
    std::span<const CarrierMark> carriers, rpc::Demod mode, double cw_pitch_hz)
{
    std::vector<NotchRequest> out;
    if (mark_offsets(mode, 1.0, cw_pitch_hz).count == 0) {
        return out;
    }
    std::vector<CarrierMark> order(carriers.begin(), carriers.end());
    std::stable_sort(order.begin(), order.end(), [](const CarrierMark& a, const CarrierMark& b) {
        return a.level_db > b.level_db;
    });
    for (const CarrierMark& carrier : order) {
        if (out.size() >= kNotchSlots) {
            break;
        }
        const double audio = audio_at_offset(mode, carrier.offset_hz, cw_pitch_hz);
        if (!(audio > 0.0)) {
            continue;
        }
        const double width = click_notch_width(carrier.width_hz);
        const bool taken = std::any_of(out.begin(), out.end(), [&](const NotchRequest& r) {
            return std::abs(r.audio_hz - audio) < std::max(r.width_hz, width) / 2.0;
        });
        if (!taken) {
            out.push_back(NotchRequest{audio, width});
        }
    }
    return out;
}

}  // namespace revenant::ui
