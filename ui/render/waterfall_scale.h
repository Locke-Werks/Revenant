// The span waterfall's own scale: rows stored as levels, coloured at draw
// time against one pair of ends for the whole history, with those ends
// measured from the history itself.
//
// WHAT THE OWNER ASKED FOR, 2026-10-07
//
// "Autoscale over time as a whole: not with each new row per se, but all
// together as signals come into the waterfall." Until then each row was
// coloured against the ends of the moment it arrived and kept as RGB, so
// whenever the scale moved the history became a patchwork of rows drawn to
// different scales, and an old row could not be redrawn because its levels
// were gone.
//
// HOW IT WORKS
//
// A row is stored as one 16-bit level code per column, encode_level below, and
// coloured from the codes through the shared palette. The colours are a cache
// of the codes: when the ends move, the tiles coloured against the old ends
// are recoloured from their codes, so every row on screen converges on the
// same pair. LevelHistogram counts every code the history holds, which makes
// the history's percentiles a scan of a couple of thousand counters rather
// than a sort of every pixel, and HistoryLevels smooths them over time.
//
// WHY 16 BITS AND NOT 8. Eight bits over a range wide enough for any tuner
// (about 160 dB) is 0.6 dB a step, which on a 40 dB map is 64 levels and
// shows as contour banding in a smooth fade. Sixteen at a sixty-fourth of a
// decibel is finer than the 1024-entry palette can show on any map, and the
// ring at 1578 by 1123 device pixels costs 3.5 MB, beside the 7 MB of colour.
//
// WHY ON THE CPU AND NOT IN A SHADER. A palette lookup in a fragment shader
// would recolour for free, but the offscreen platform the frame budget is
// measured on renders with Qt's software scene graph, which runs no custom
// material at all, and the client has no shader build step. Recolouring a
// tile from its codes is a multiply and a table read per pixel, and is spread
// over frames; see WaterfallItem::recolourStale.
//
// This header holds no Qt; ui/tests links it.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "render/colour_map.h"
#include "render/spectrum_scale.h"

namespace revenant::ui {

// ---------------------------------------------------------------------------
// Level codes
// ---------------------------------------------------------------------------

// Code 0 is a pixel never written, or slid in from outside the span by a
// retune: it draws the background and the histogram does not count it, so a
// half-filled ring is measured on its rows and not on its empty ones.
inline constexpr std::uint16_t kNoLevel = 0;

// The bottom of the code range and its resolution. -250 dBFS is below
// anything a frame carries other than kSpectrumFloorDb's -200 sentinel, and
// 65535 steps of a sixty-fourth reach +773 dBFS, which nothing does.
inline constexpr float kLevelCodeFloorDb = -250.0F;
inline constexpr float kLevelCodesPerDb = 64.0F;

[[nodiscard]] inline std::uint16_t encode_level(float db)
{
    if (!(db == db)) {
        return 1;
    }
    const float code = std::round((db - kLevelCodeFloorDb) * kLevelCodesPerDb) + 1.0F;
    return static_cast<std::uint16_t>(std::clamp(code, 1.0F, 65535.0F));
}

[[nodiscard]] inline float decode_level(std::uint16_t code)
{
    return kLevelCodeFloorDb + static_cast<float>(code - 1) / kLevelCodesPerDb;
}

// ---------------------------------------------------------------------------
// Colour from a code
// ---------------------------------------------------------------------------

// The palette packed the way a Format_RGBX8888 scanline holds it on a little
// endian machine: red in the low byte.
[[nodiscard]] inline std::uint32_t pack_rgbx(colour_map::Srgb8 c)
{
    return 0xFF000000U | (static_cast<std::uint32_t>(c.b) << 16U) |
           (static_cast<std::uint32_t>(c.g) << 8U) | static_cast<std::uint32_t>(c.r);
}

using PackedPalette = std::array<std::uint32_t, colour_map::kTableSize>;

[[nodiscard]] inline const PackedPalette& packed_palette()
{
    static const PackedPalette built = [] {
        PackedPalette packed{};
        const colour_map::Table& table = colour_map::table();
        for (std::size_t i = 0; i < packed.size(); ++i) {
            packed[i] = pack_rgbx(table[i]);
        }
        return packed;
    }();
    return built;
}

// Codes to palette indices for one pair of ends, set up once per pair so the
// per-pixel work is a subtract, a multiply and a clamp.
class CodeColourer {
public:
    explicit CodeColourer(const MapEnds& ends)
        : palette_(packed_palette()),
          background_(pack_rgbx(colour_map::kBottom))
    {
        const float span = std::max(ends.span_db(), 1e-3F);
        const auto top = static_cast<float>(colour_map::kTableSize - 1);
        floor_code_ = (ends.floor_db - kLevelCodeFloorDb) * kLevelCodesPerDb + 1.0F;
        scale_ = top / (span * kLevelCodesPerDb);
        top_ = top;
    }

    [[nodiscard]] std::uint32_t operator()(std::uint16_t code) const
    {
        if (code == kNoLevel) {
            return background_;
        }
        const float at = std::clamp((static_cast<float>(code) - floor_code_) * scale_ + 0.5F,
                                    0.0F, top_);
        return palette_[static_cast<std::size_t>(at)];
    }

    void row(const std::uint16_t* codes, std::uint32_t* pixels, int count) const
    {
        for (int i = 0; i < count; ++i) {
            pixels[i] = (*this)(codes[i]);
        }
    }

private:
    const PackedPalette& palette_;
    std::uint32_t background_;
    float floor_code_ = 0.0F;
    float scale_ = 0.0F;
    float top_ = 0.0F;
};

// ---------------------------------------------------------------------------
// The history's percentiles
// ---------------------------------------------------------------------------

// Every level code the history holds, in half-decibel bins. Half a decibel is
// as fine as a percentile needs to be once it is smoothed over seconds, and
// keeps a percentile to a scan of 2048 counters.
class LevelHistogram {
public:
    static constexpr int kCodesPerBin = 32;
    static constexpr std::size_t kBins = 65536 / kCodesPerBin;

    LevelHistogram() : counts_(kBins, 0) {}

    void add(std::uint16_t code)
    {
        if (code != kNoLevel) {
            counts_[code / kCodesPerBin] += 1;
            total_ += 1;
        }
    }

    void remove(std::uint16_t code)
    {
        if (code == kNoLevel) {
            return;
        }
        std::uint32_t& count = counts_[code / kCodesPerBin];
        if (count > 0) {
            count -= 1;
            total_ -= 1;
        }
    }

    void add_row(const std::uint16_t* codes, int count)
    {
        for (int i = 0; i < count; ++i) {
            add(codes[i]);
        }
    }

    void remove_row(const std::uint16_t* codes, int count)
    {
        for (int i = 0; i < count; ++i) {
            remove(codes[i]);
        }
    }

    void clear()
    {
        std::fill(counts_.begin(), counts_.end(), 0U);
        total_ = 0;
    }

    [[nodiscard]] std::uint64_t total() const { return total_; }

    // The level `permille` thousandths of the counted codes sit below, at the
    // middle of its bin. Empty answers the code range's floor; callers check
    // total() first.
    [[nodiscard]] float percentile(std::uint32_t permille) const
    {
        if (total_ == 0) {
            return kLevelCodeFloorDb;
        }
        const std::uint64_t rank =
            std::min<std::uint64_t>(total_ - 1, total_ * std::min(permille, 1000U) / 1000U);
        std::uint64_t seen = 0;
        for (std::size_t bin = 0; bin < kBins; ++bin) {
            seen += counts_[bin];
            if (seen > rank) {
                const auto middle = static_cast<std::uint16_t>(
                    std::max<std::size_t>(bin * kCodesPerBin + kCodesPerBin / 2, 1));
                return decode_level(middle);
            }
        }
        return decode_level(65535);
    }

private:
    std::vector<std::uint32_t> counts_;
    std::uint64_t total_ = 0;
};

// Where in the history the noise and the strong signals are measured.
//
// The noise is the level a quarter of the history's pixels sit below, the same
// quarter the span's noise estimate uses and for its reason: a broadcast band
// can be more than half stations, and a median there is a station. The pixels
// are columns already reduced to their largest bin, so this measures the noise
// as it is drawn and needs no headroom correction.
//
// The strong level is the 999th thousandth: one pixel in a thousand above it.
// A lone one-column carrier is less than that on a wide display and goes above
// the top of the map, where it saturates to the brightest colour, which is
// still the right answer for the strongest thing on screen; a higher
// percentile would let one such carrier set the scale for everything else.
inline constexpr std::uint32_t kHistoryNoisePermille = 250;
inline constexpr std::uint32_t kHistoryStrongPermille = 999;

// The weak level, the bottom of the contrast slider's tight end: one pixel in a
// hundred below it. Not the minimum, because the drawn noise is a log of an
// exponential power, whose deepest pixel among a million and a half falls tens
// of dB under the rest and would spend most of the map on single null pixels.
// The hundredth sits a little under the bulk of the noise, so at full contrast
// the noise floor's own texture spans the map rather than one stray pixel
// setting black. The top keeps the strong level for the same reason in mirror:
// the maximum is one pixel, the thousandth is the strongest signal on screen.
inline constexpr std::uint32_t kHistoryWeakPermille = 10;

// How fast the waterfall's ends follow the history. Expanding, the noise
// falling or the strong level rising, takes a few seconds, so a signal that
// arrives brightens the whole picture's contrast over the time it takes to
// notice rather than jumping it in one row. Contracting takes thirty, the
// spectrum's own decay, so a signal that leaves does not hand its contrast
// straight back to the noise. docs/ui-spectrum.md, "Auto-scaling, both ends".
inline constexpr double kHistoryExpandSeconds = 3.0;
inline constexpr double kHistoryContractSeconds = 30.0;

// The noise and strong levels, each following its percentile with the attack
// and decay above.
class HistoryLevels {
public:
    void update(float noise_db, float strong_db, double seconds)
    {
        update(noise_db, strong_db, seconds, noise_db);
    }

    // The weak level eases on the same terms as the noise: falling is
    // expanding the contrast and takes seconds, rising takes thirty.
    void update(float noise_db, float strong_db, double seconds, float weak_db)
    {
        if (!valid_) {
            noise_db_ = noise_db;
            strong_db_ = strong_db;
            weak_db_ = weak_db;
            valid_ = true;
            return;
        }
        const double dt = std::max(seconds, 0.0);
        weak_db_ = follow(weak_db_, weak_db, dt, weak_db < weak_db_);
        noise_db_ = follow(noise_db_, noise_db, dt, noise_db < noise_db_);
        strong_db_ = follow(strong_db_, strong_db, dt, strong_db > strong_db_);
    }

    void reset() { valid_ = false; }

    [[nodiscard]] bool valid() const { return valid_; }
    [[nodiscard]] float noise_db() const { return noise_db_; }
    [[nodiscard]] float strong_db() const { return strong_db_; }

    // The ends the waterfall draws against: place_ends, the spectrum's rule,
    // on the smoothed levels, so the noise sits at the same height on both.
    [[nodiscard]] float weak_db() const { return weak_db_; }

    // Contrast 0 is place_ends as it always was; 1 puts the weak level on the
    // map's bottom and the strong level on its top. See blend_ends.
    //
    // 1 to 2 goes on to the hot ends: the noise on the bottom edge and the top
    // halfway from the noise to the strong level. Past 1 is the owner's
    // request of 2026-10-08 for more: 0 to 1 alone barely moved the picture,
    // because the weak level sits only a few dB under the noise, which is
    // about where place_ends already puts the floor, so the only change was
    // the ceiling. The hot ends black out the noise and saturate the strongest
    // signals, which is what lets a weak one stand out.
    [[nodiscard]] MapEnds ends(const ScalePins& pins, float contrast = 0.0F) const
    {
        const MapEnds padded = place_ends(noise_db_, strong_db_, pins);
        if (!(contrast > 0.0F)) {
            return padded;
        }
        const MapEnds tight = tight_ends(weak_db_, strong_db_, kWaterfallTightGuardDb, pins);
        if (!(contrast > 1.0F)) {
            return blend_ends(padded, tight, contrast);
        }
        const float hot_top = noise_db_ + kHotTopFraction * std::max(strong_db_ - noise_db_, 0.0F);
        return blend_ends(tight, tight_ends(noise_db_, hot_top, kWaterfallTightGuardDb, pins),
                          contrast - 1.0F);
    }

    static constexpr float kHotTopFraction = 0.5F;

private:
    [[nodiscard]] static float follow(float now, float target, double dt, bool expanding)
    {
        const double tau = expanding ? kHistoryExpandSeconds : kHistoryContractSeconds;
        const double step = 1.0 - std::exp(-dt / tau);
        return static_cast<float>(now + (target - now) * step);
    }

    bool valid_ = false;
    float noise_db_ = 0.0F;
    float strong_db_ = 0.0F;
    float weak_db_ = 0.0F;
};

// ---------------------------------------------------------------------------
// When a tile has to be recoloured
// ---------------------------------------------------------------------------

// A tile coloured against ends within this of the current pair at both ends is
// left alone. A quarter of a decibel on the 40 dB minimum map is 0.6% of it,
// about six of the palette's 1024 entries and under one 8-bit step of
// lightness, so the mismatch cannot be seen; recolouring on any change at all
// would recolour every tile on every frame while the ends are easing.
inline constexpr float kRecolourToleranceDb = 0.25F;

// How far a tile's ends are from the current pair, the worse of the two.
[[nodiscard]] inline float ends_distance_db(const MapEnds& a, const MapEnds& b)
{
    return std::max(std::abs(a.floor_db - b.floor_db), std::abs(a.ceiling_db - b.ceiling_db));
}

}  // namespace revenant::ui
