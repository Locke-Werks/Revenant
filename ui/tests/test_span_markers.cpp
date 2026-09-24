// models/span_markers.h: the noise floor and the strongest signal marked on
// the span spectrum, and where their plates go.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows. The obvious peak, the largest bin of each frame, is
// right on every single frame and unreadable across them. The obvious floor,
// the auto-scale floor, is a colour-map end that parts from the noise
// whenever it is pinned or catching up. The obvious plate, centred on its
// marker, sits on the trace.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "models/span_markers.h"

using Catch::Matchers::WithinAbs;
using revenant::ui::kNoiseSmoothSeconds;
using revenant::ui::kPeakSmoothSeconds;
using revenant::ui::kPeakSwitchDb;
using revenant::ui::NoiseFloor;
using revenant::ui::PaneRoom;
using revenant::ui::percentile_of;
using revenant::ui::place_noise_plate;
using revenant::ui::place_peak_plate;
using revenant::ui::PlateBox;
using revenant::ui::SpanPeak;
using revenant::ui::track_noise_floor;
using revenant::ui::track_span_peak;

namespace {

// The engine's shipped span at 2.4 MS/s: 8192 bins, 36.6 frames a second.
constexpr std::size_t kBins = 8192;
constexpr double kFrameSeconds = 65536.0 / 2'400'000.0;

// Noise around -100 dBFS with the scatter of a single transform bin, which is
// several decibels, plus whatever carriers the case puts on it.
[[nodiscard]] std::vector<float> noise_frame(std::mt19937_64& rng)
{
    std::exponential_distribution<double> power(1.0);
    std::vector<float> bins(kBins);
    for (float& bin : bins) {
        bin = static_cast<float>(-100.0 + 10.0 * std::log10(power(rng)));
    }
    return bins;
}

}  // namespace

TEST_CASE("the first frame takes the largest bin as it stands", "[span_markers]")
{
    std::mt19937_64 rng(20260923);
    std::vector<float> bins = noise_frame(rng);
    bins[5000] = -31.5F;

    const SpanPeak peak = track_span_peak(SpanPeak{}, bins, kFrameSeconds);
    REQUIRE(peak.valid);
    CHECK(peak.bin == 5000.0);
    CHECK_THAT(peak.level_db, WithinAbs(-31.5, 1e-6));
    CHECK(peak.bins == kBins);
}

// Rejects searching only near the focused receiver, or only on screen: the
// whole frame is searched, so a carrier at the very last bin is found.
TEST_CASE("the search covers the whole frame, edge to edge", "[span_markers]")
{
    std::mt19937_64 rng(1);
    std::vector<float> bins = noise_frame(rng);
    bins[kBins - 1] = -20.0F;
    CHECK(track_span_peak(SpanPeak{}, bins, kFrameSeconds).bin == static_cast<double>(kBins - 1));

    bins[kBins - 1] = -100.0F;
    bins[0] = -20.0F;
    CHECK(track_span_peak(SpanPeak{}, bins, kFrameSeconds).bin == 0.0);
}

// Rejects the largest bin of each frame. Two carriers a decibel apart, each
// with a couple of decibels of fading, trade the maximum on most frames; the
// marker has to stay on one of them.
TEST_CASE("two near-equal carriers do not make the marker hop", "[span_markers]")
{
    std::mt19937_64 rng(20260923);
    std::normal_distribution<double> fade(0.0, 1.0);

    SpanPeak peak;
    int hops = 0;
    int naive_hops = 0;
    std::size_t naive_last = 0;
    for (int frame = 0; frame < 400; ++frame) {
        std::vector<float> bins = noise_frame(rng);
        bins[2000] = static_cast<float>(-40.0 + fade(rng));
        bins[6000] = static_cast<float>(-41.0 + fade(rng));

        const double before = peak.bin;
        peak = track_span_peak(peak, bins, kFrameSeconds);
        if (frame > 0 && std::abs(peak.bin - before) > 100.0) {
            ++hops;
        }

        const std::size_t naive = bins[2000] > bins[6000] ? 2000 : 6000;
        if (frame > 0 && naive != naive_last) {
            ++naive_hops;
        }
        naive_last = naive;
    }

    // The per-frame maximum really does trade places, which is the premise.
    CHECK(naive_hops > 50);

    // At most one move, and only onto the carrier that is louder on average,
    // where it then stays: starting on the quieter one is a coin the first
    // frame tosses, and staying there would be the hold defeating its own
    // purpose.
    CHECK(hops <= 1);
    CHECK_THAT(peak.bin, WithinAbs(2000.0, 50.0));
}

// Rejects a hold with no way out: a signal that is plainly louder, by more
// than the switch margin, takes the marker at once, without easing across the
// frequencies in between.
TEST_CASE("a plainly stronger signal takes the marker at once", "[span_markers]")
{
    std::mt19937_64 rng(7);
    SpanPeak peak;
    for (int frame = 0; frame < 20; ++frame) {
        std::vector<float> bins = noise_frame(rng);
        bins[1000] = -50.0F;
        peak = track_span_peak(peak, bins, kFrameSeconds);
    }
    REQUIRE_THAT(peak.bin, WithinAbs(1000.0, 1e-6));

    std::vector<float> bins = noise_frame(rng);
    bins[1000] = -50.0F;
    bins[7000] = static_cast<float>(-50.0 + kPeakSwitchDb + 0.5);
    peak = track_span_peak(peak, bins, kFrameSeconds);
    CHECK(peak.bin == 7000.0);
    CHECK_THAT(peak.level_db, WithinAbs(-50.0 + kPeakSwitchDb + 0.5, 1e-6));
}

// A signal that goes away leaves the marker on whatever is loudest next,
// rather than on a patch of noise where it used to be. Not on the first
// frame, because the marker's own level eases down first, and within a
// quarter second of source time.
TEST_CASE("a signal that stops hands the marker on", "[span_markers]")
{
    std::mt19937_64 rng(11);
    SpanPeak peak;
    for (int frame = 0; frame < 20; ++frame) {
        std::vector<float> bins = noise_frame(rng);
        bins[3000] = -30.0F;
        bins[500] = -60.0F;
        peak = track_span_peak(peak, bins, kFrameSeconds);
    }
    REQUIRE_THAT(peak.bin, WithinAbs(3000.0, 1e-6));

    int frames = 0;
    while (peak.bin != 500.0 && frames < 100) {
        std::vector<float> bins = noise_frame(rng);
        bins[500] = -60.0F;
        peak = track_span_peak(peak, bins, kFrameSeconds);
        ++frames;
    }
    CHECK(peak.bin == 500.0);
    CHECK_THAT(peak.level_db, WithinAbs(-60.0, 1e-6));
    CHECK(static_cast<double>(frames) * kFrameSeconds <= kPeakSmoothSeconds);
}

// Rejects no smoothing and too much. A carrier keyed up by 10 dB moves the
// plate most of the way within a second of source time, and the frame-to-
// frame scatter of its level is taken down.
TEST_CASE("the level eases over a quarter second of source time", "[span_markers]")
{
    std::mt19937_64 rng(3);
    SpanPeak peak;
    std::vector<float> bins = noise_frame(rng);
    bins[4000] = -45.0F;
    peak = track_span_peak(peak, bins, kFrameSeconds);

    bins[4000] = -35.0F;
    peak = track_span_peak(peak, bins, kFrameSeconds);

    // One frame, 27 ms, is about a tenth of the way.
    const double one_frame = 1.0 - std::exp(-kFrameSeconds / kPeakSmoothSeconds);
    CHECK_THAT(peak.level_db, WithinAbs(-45.0 + 10.0 * one_frame, 1e-3));

    // A second of source time is within a quarter of a decibel.
    for (int frame = 0; frame < 36; ++frame) {
        peak = track_span_peak(peak, bins, kFrameSeconds);
    }
    CHECK(peak.level_db > -35.25);

    // A carrier whose single-bin level scatters by a couple of decibels a
    // frame reads steady to a few tenths once eased.
    std::normal_distribution<double> scatter(0.0, 2.0);
    double lowest = 0.0;
    double highest = -200.0;
    for (int frame = 0; frame < 300; ++frame) {
        bins[4000] = static_cast<float>(-35.0 + scatter(rng));
        peak = track_span_peak(peak, bins, kFrameSeconds);
        if (frame > 50) {
            lowest = std::min(lowest, peak.level_db);
            highest = std::max(highest, peak.level_db);
        }
    }
    CHECK(highest - lowest < 2.5);
}

// The loudest bin inside one wide station wanders across it. Rejects a hold
// window so narrow that the marker keeps being handed to another bin of the
// same station: it follows, eased, and does not jump.
TEST_CASE("a wandering peak inside one station is followed, not jumped to", "[span_markers]")
{
    std::mt19937_64 rng(5);
    std::uniform_int_distribution<int> wander(-20, 20);
    SpanPeak peak;
    double widest_step = 0.0;
    for (int frame = 0; frame < 200; ++frame) {
        std::vector<float> bins = noise_frame(rng);
        const int at = 4096 + wander(rng);
        bins[static_cast<std::size_t>(at)] = -25.0F;
        const double before = peak.bin;
        peak = track_span_peak(peak, bins, kFrameSeconds);
        if (frame > 0) {
            widest_step = std::max(widest_step, std::abs(peak.bin - before));
        }
    }
    CHECK(widest_step < 10.0);
    CHECK_THAT(peak.bin, WithinAbs(4096.0, 20.0));
}

// Rejects easing across a change of axis. A frame of another width, or no
// time since the last one, is a new start, not a step to smooth.
TEST_CASE("a new axis or no elapsed time starts over", "[span_markers]")
{
    std::mt19937_64 rng(9);
    SpanPeak peak;
    std::vector<float> bins = noise_frame(rng);
    bins[100] = -30.0F;
    peak = track_span_peak(peak, bins, kFrameSeconds);

    // Same width, the carrier gone quieter but still the strongest by far:
    // with no elapsed time the reading is taken as it stands.
    bins[100] = -40.0F;
    CHECK_THAT(track_span_peak(peak, bins, 0.0).level_db, WithinAbs(-40.0, 1e-6));

    std::vector<float> narrower(4096, -100.0F);
    narrower[3000] = -45.0F;
    const SpanPeak moved = track_span_peak(peak, narrower, kFrameSeconds);
    CHECK(moved.bin == 3000.0);
    CHECK(moved.bins == 4096);

    CHECK_FALSE(track_span_peak(peak, std::vector<float>{}, kFrameSeconds).valid);
}

// ---------------------------------------------------------------------------
// The noise floor
// ---------------------------------------------------------------------------

namespace {

// A trace as the span draws it: each column the largest of five bins, which
// is 8192 bins across a 1578-pixel window, with a dip every channel boundary
// the way a channelised span has one.
[[nodiscard]] std::vector<float> noise_columns(std::mt19937_64& rng, std::size_t count = 1578)
{
    std::exponential_distribution<double> power(1.0);
    std::vector<float> columns(count);
    for (std::size_t c = 0; c < count; ++c) {
        double largest = 0.0;
        for (int b = 0; b < 5; ++b) {
            largest = std::max(largest, power(rng));
        }
        const bool dip = (c % 197) < 4;
        columns[c] = static_cast<float>(-100.0 + 10.0 * std::log10(largest) - (dip ? 14.0 : 0.0));
    }
    return columns;
}

[[nodiscard]] double share_below(const std::vector<float>& columns, double level)
{
    std::size_t below = 0;
    for (const float c : columns) {
        below += static_cast<double>(c) < level ? 1 : 0;
    }
    return static_cast<double>(below) / static_cast<double>(columns.size());
}

}  // namespace

// Rejects a floor read off the bottom of the distribution, which is where a
// fifth percentile lands on a channelised span: in the dips at the channel
// boundaries. The line has to run through the noise itself.
TEST_CASE("the floor line runs through the noise the trace draws", "[span_markers]")
{
    std::mt19937_64 rng(20260923);
    const std::vector<float> columns = noise_columns(rng);

    const NoiseFloor floor = track_noise_floor(NoiseFloor{}, columns, kFrameSeconds);
    REQUIRE(floor.valid);

    const double share = share_below(columns, floor.level_db);
    CHECK(share > 0.20);
    CHECK(share < 0.30);

    // The dips at the channel boundaries, which are what a fifth percentile
    // lands in, do not move it: the same trace with the dips filled in reads
    // within a fraction of a decibel.
    std::mt19937_64 same(20260923);
    std::vector<float> filled = noise_columns(same);
    const float typical = static_cast<float>(percentile_of(filled, 500));
    for (std::size_t c = 0; c < filled.size(); c += 197) {
        for (std::size_t d = c; d < std::min(filled.size(), c + 4); ++d) {
            filled[d] = typical;
        }
    }
    const NoiseFloor without = track_noise_floor(NoiseFloor{}, filled, kFrameSeconds);
    CHECK_THAT(floor.level_db, WithinAbs(without.level_db, 0.5));

    // The plate's anchor is under the line.
    CHECK(floor.low_db < floor.level_db);
}

// Rejects the median. A broadcast band more than half full of stations puts
// the median on the stations, and the line has to stay on the noise.
TEST_CASE("a band more than half occupied still reads its noise", "[span_markers]")
{
    std::mt19937_64 rng(4);
    std::vector<float> columns = noise_columns(rng);
    for (std::size_t c = 0; c < columns.size(); ++c) {
        if ((c / 100) % 5 < 3) {
            columns[c] = -60.0F;
        }
    }
    REQUIRE(share_below(columns, -61.0) < 0.5);
    CHECK(percentile_of(columns, 500) > -61.0);

    const NoiseFloor floor = track_noise_floor(NoiseFloor{}, columns, kFrameSeconds);
    CHECK(floor.level_db < -90.0);
    CHECK(floor.level_db > -100.0);
}

// Rejects no easing: a floor that jumps with every frame's scatter makes the
// plate's figure unreadable, and one that follows a gain change instantly is
// indistinguishable from a signal arriving.
TEST_CASE("the floor eases over a second of source time", "[span_markers]")
{
    std::mt19937_64 rng(8);
    NoiseFloor floor = track_noise_floor(NoiseFloor{}, noise_columns(rng), kFrameSeconds);
    const double before = floor.level_db;

    std::vector<float> louder = noise_columns(rng);
    for (float& c : louder) {
        c += 10.0F;
    }
    const double target = percentile_of(louder, 250);
    floor = track_noise_floor(floor, louder, kFrameSeconds);
    const double one_frame = 1.0 - std::exp(-kFrameSeconds / kNoiseSmoothSeconds);
    CHECK_THAT(floor.level_db, WithinAbs(before + (target - before) * one_frame, 1e-6));

    // One second of frames is most of the way, not all of it.
    for (int frame = 1; frame < 37; ++frame) {
        floor = track_noise_floor(floor, louder, kFrameSeconds);
    }
    CHECK(floor.level_db > before + 5.0);
    CHECK(floor.level_db < target - 2.0);

    CHECK_FALSE(track_noise_floor(floor, std::vector<float>{}, kFrameSeconds).valid);
    CHECK_THAT(track_noise_floor(floor, louder, 0.0).level_db, WithinAbs(target, 1e-6));
}

// ---------------------------------------------------------------------------
// The plates
// ---------------------------------------------------------------------------

namespace {

// The span pane at an ordinary height: the detection strip along the top,
// the scale's labels up the right, the ceiling's pin plate in the corner.
[[nodiscard]] PaneRoom span_room()
{
    PaneRoom room;
    room.width = 1578.0;
    room.height = 300.0;
    room.top_px = 26.0;
    room.right_px = 44.0;
    room.keep_clear = PlateBox{4.0, 28.0, 150.0, 18.0};
    return room;
}

constexpr double kPlateW = 170.0;
constexpr double kPlateH = 17.0;

}  // namespace

// Rejects a plate centred on the peak, which sits on the signal it names.
// Over the peak the pane is empty, because every other column is lower.
TEST_CASE("the peak's plate sits over the peak, clear of the trace", "[span_markers]")
{
    const PaneRoom room = span_room();
    const auto placed = place_peak_plate(800.0, 150.0, kPlateW, kPlateH, room);

    CHECK(placed.tick);
    CHECK_FALSE(placed.above_scale);
    CHECK(placed.plate.bottom() <= placed.tick_top + 1e-9);
    CHECK(placed.tick_bottom < 150.0);
    CHECK(placed.tick_x == 800.0);
    CHECK_THAT(placed.plate.x + kPlateW / 2.0, WithinAbs(800.0, 1e-9));
    CHECK(placed.plate.y >= room.top_px);
}

// Rejects a plate pushed up into the detection labels when the peak is near
// the top, and one pointing at the top edge for a peak above the scale.
TEST_CASE("a peak near or above the top puts the plate beside it", "[span_markers]")
{
    const PaneRoom room = span_room();

    const auto near_top = place_peak_plate(800.0, 30.0, kPlateW, kPlateH, room);
    CHECK_FALSE(near_top.tick);
    CHECK(near_top.plate.y == room.top_px);
    CHECK(near_top.plate.x > 800.0);

    const auto above = place_peak_plate(800.0, -40.0, kPlateW, kPlateH, room);
    CHECK(above.above_scale);
    CHECK_FALSE(above.tick);
    CHECK(above.plate.y == room.top_px);

    // At the right edge there is no room to its right, so it goes left, and
    // stays clear of the scale's labels.
    const auto right_edge = place_peak_plate(1560.0, -10.0, kPlateW, kPlateH, room);
    CHECK(right_edge.plate.right() < 1560.0);
    CHECK(right_edge.plate.right() <= room.width - room.right_px);
}

// Every position across the pane and every height, above the scale included:
// the plate is inside the pane, out of the detection strip, off the scale's
// labels and off the pin plate, and while it is over the peak it ends above it.
TEST_CASE("no peak position puts its plate on the labels or the pin", "[span_markers]")
{
    const PaneRoom room = span_room();
    for (double x = 0.0; x <= room.width; x += 11.0) {
        for (double y = -60.0; y <= room.height; y += 7.0) {
            const auto placed = place_peak_plate(x, y, kPlateW, kPlateH, room);
            CHECK(placed.plate.x >= 0.0);
            CHECK(placed.plate.right() <= room.width - room.right_px);
            CHECK(placed.plate.y >= room.top_px);
            CHECK(placed.plate.bottom() <= room.height);
            CHECK_FALSE(placed.plate.overlaps(room.keep_clear));
            if (placed.tick) {
                CHECK(placed.plate.bottom() <= y - 1.0);
            }
        }
    }
}

// Rejects a plate centred on the noise line, which covers the noise the line
// is drawn through; it goes under the bottom of the noise, and onto the
// bottom edge only when the noise is already there.
TEST_CASE("the noise plate goes under the noise", "[span_markers]")
{
    const PaneRoom room = span_room();

    const PlateBox under = place_noise_plate(200.0, 215.0, 90.0, kPlateH, room);
    CHECK(under.y >= 215.0);
    CHECK(under.bottom() <= room.height);
    CHECK_THAT(under.right(), WithinAbs(room.width - 4.0, 1e-9));

    const PlateBox bottom = place_noise_plate(290.0, 298.0, 90.0, kPlateH, room);
    CHECK(bottom.bottom() <= room.height);
    CHECK(bottom.y >= room.top_px);
}
