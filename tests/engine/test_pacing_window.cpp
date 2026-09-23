// PacingWindow: the realtime factor over the last two seconds, with a pause
// the engine asked for excused.
//
// The RTL-SDR pause is simulated rather than run, because a synthetic or file
// source cannot pause the way the dongle does and the arithmetic is the part
// that was wrong. The shape is the backend's own, from
// core/source/rtlsdr_source.cpp: samples arrive in 65536-sample blocks at
// 2.4 MS/s; a control transfer stops the transfers for 330 ms, the figure
// measured on an R820T on 2026-09-21; the device's buffer is flushed, so the
// part-filled block is gone; and what the device produced meanwhile is added
// to samples_lost by note_dropped, never to samples_delivered. The engine's
// loop is modelled as offering the count every millisecond, where the real
// one offers it every 2 ms.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest of
// the tree's tests follow. No GPU, no radio and no sleep: the clock is a
// number the test advances.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>

#include "core/engine/pacing_window.h"

using revenant::engine::kPacingSnapshotNs;
using revenant::engine::kPacingWindowNs;
using revenant::engine::kPacingWindowSeconds;
using revenant::engine::PacingReading;
using revenant::engine::PacingSample;
using revenant::engine::PacingWindow;

namespace {

constexpr std::int64_t kStepNs = 1'000'000;
constexpr std::int64_t kPauseNs = 330'000'000;
constexpr std::uint64_t kBlock = 65'536;

// The client's two thresholds, models/source_pacing.h in ui/: a source is
// called behind below the first and caught up again above the second. Copied
// rather than included, because this binary links no ui code; the cases
// below are about whether the engine's figure crosses them.
constexpr double kClientBehindEnter = 0.97;
constexpr double kClientBehindLeave = 0.99;

// A dongle streaming into the engine, and the engine's window over it.
struct Dongle {
    explicit Dongle(std::int64_t rate = 2'400'000) : rate(rate) { window.start(sample()); }

    std::int64_t rate;
    std::int64_t now_ns = 1'000'000'000;

    // Produced and not yet handed over, in thousandths of a sample, so a rate
    // that is not a whole number of samples a millisecond keeps its fraction.
    std::uint64_t pending_milli = 0;
    std::uint64_t delivered = 0;
    std::uint64_t lost = 0;
    PacingWindow window;

    [[nodiscard]] PacingSample sample() const
    {
        return PacingSample{.at_ns = now_ns, .delivered = delivered};
    }

    [[nodiscard]] PacingReading read() const { return window.read(sample(), rate); }

    [[nodiscard]] std::uint64_t per_step_milli() const
    {
        return static_cast<std::uint64_t>(rate);
    }

    // Streams for `ns`, offering the window the count every millisecond.
    void stream(std::int64_t ns)
    {
        for (std::int64_t t = 0; t < ns; t += kStepNs) {
            now_ns += kStepNs;
            pending_milli += per_step_milli();
            while (pending_milli >= kBlock * 1000) {
                pending_milli -= kBlock * 1000;
                delivered += kBlock;
            }
            window.offer(sample());
        }
    }

    // with_transfers_paused: nothing delivered for kPauseNs, the flush throws
    // away the part-filled block, and what the device produced meanwhile is
    // counted lost. `declared` is whether the engine made the call, which is
    // what Engine::excusing_pause tells the window. Answers the lowest factor
    // read every 10 ms while it lasted.
    double pause(bool declared)
    {
        if (declared) {
            window.pause_begun(now_ns);
        }
        const std::uint64_t lost_before = lost;
        double lowest = 1e9;
        for (std::int64_t t = 0; t < kPauseNs; t += kStepNs) {
            now_ns += kStepNs;
            lost += per_step_milli() / 1000;
            window.offer(sample());
            if (t % 10'000'000 == 0) {
                lowest = std::min(lowest, read().factor);
            }
        }
        pending_milli = 0;
        if (declared) {
            window.pause_ended(lost - lost_before);
        }
        return lowest;
    }

    // The lowest and highest factor read every 100 ms over the next `ns`,
    // which is as often as anything polls it and more often than the client.
    struct Range {
        double low = 1e9;
        double high = 0.0;
    };
    [[nodiscard]] Range stream_and_watch(std::int64_t ns)
    {
        Range out;
        for (std::int64_t t = 0; t < ns; t += 100'000'000) {
            stream(100'000'000);
            const double factor = read().factor;
            out.low = std::min(out.low, factor);
            out.high = std::max(out.high, factor);
        }
        return out;
    }
};

}  // namespace

// Rejects a window whose ends are clock ticks. The count moves 65536 samples
// at a time, which is 27 ms at 2.4 MS/s and 262 ms at 250 kS/s, so a window
// from one poll to another swings by up to a block at each end: at two
// seconds, 1.4% and 13%, and the client's thresholds are 1% and 3% below
// realtime. A source exactly on time would be called behind by its own block
// size.
// Named without the word for the device, which the engine suite's filter
// takes as a case that needs one attached.
TEST_CASE("a steady front end reads realtime at any block size", "[engine][pacing]")
{
    for (const std::int64_t rate : {2'400'000, 1'024'000, 250'000}) {
        Dongle dongle(rate);
        dongle.stream(3'000'000'000);
        const Dongle::Range range = dongle.stream_and_watch(10'000'000'000);
        INFO(rate << " S/s: factor from " << range.low << " to " << range.high);
        CHECK(range.low > kClientBehindLeave);
        CHECK(range.high < 1.01);
    }
}

// Rejects the lifetime mean this replaced, and rejects a window that charges
// the pause to the source. Under the first, twenty retunes in forty seconds
// read (40 - 20 x 0.33) / 40 = 0.835 for good and the client called the
// source behind after the first few; under the second it read behind for two
// seconds after every one. Rejects, too, a reading that follows the pause
// while it is under way: the client polls once a second and a sweep spends
// most of its time inside pauses. The retunes here come every two seconds,
// and none of them may cross the client's threshold, during or after.
TEST_CASE("a retune the engine asked for is not charged to the source", "[engine][pacing]")
{
    Dongle dongle;
    dongle.stream(3'000'000'000);

    double lowest = 1e9;
    for (int tune = 0; tune < 20; ++tune) {
        lowest = std::min(lowest, dongle.pause(true));
        const Dongle::Range range = dongle.stream_and_watch(2'000'000'000);
        lowest = std::min(lowest, range.low);
    }
    INFO("lowest factor across 20 retunes " << lowest << ", " << dongle.lost << " samples lost");

    // Every lost sample is still counted as lost. Only the factor excuses them.
    CHECK(dongle.lost == 20ULL * 330ULL * 2'400ULL);
    CHECK(lowest > kClientBehindEnter);

    // The part-filled block each flush throws away is gone without being
    // counted anywhere, as it is on the device, and is off the figure a
    // window after the last one.
    dongle.stream(kPacingWindowNs + kPacingSnapshotNs);
    CHECK(dongle.read().factor > kClientBehindLeave);
}

// The same pause, made by nobody the engine knows about: a USB hiccup, or a
// consumer too slow to drain the device. It is a real shortfall and has to
// show, and it has to stop showing once the window has passed it. Rejects the
// lifetime mean, which never recovers from it, and a window that is never
// advanced, which never recovers either.
TEST_CASE("a stall nobody asked for shows and leaves within the window", "[engine][pacing]")
{
    Dongle dongle;
    dongle.stream(3'000'000'000);

    const double inside = dongle.pause(false);
    INFO("lowest while stalled " << inside);
    CHECK(inside < kClientBehindEnter);

    dongle.stream(100'000'000);
    const PacingReading during = dongle.read();
    INFO("just after the stall " << during.factor << " over " << during.window_seconds << " s");
    CHECK(during.factor < kClientBehindEnter);

    // The stated window, plus the snapshot interval its start can lag by, from
    // the end of the stall. 100 ms of that has already passed above.
    dongle.stream(kPacingWindowNs + kPacingSnapshotNs - 100'000'000);
    const PacingReading after = dongle.read();
    INFO("a window after it " << after.factor << " over " << after.window_seconds << " s");
    CHECK(after.factor > kClientBehindLeave);
}

// Rejects a factor that goes on growing its deficit with every stall. Ten
// stalls nobody asked for, each followed by a window of streaming, all read
// the same afterwards, and the same as a dongle that never stalled.
TEST_CASE("stalls do not add up", "[engine][pacing]")
{
    Dongle dongle;
    dongle.stream(3'000'000'000);
    for (int stall = 0; stall < 10; ++stall) {
        static_cast<void>(dongle.pause(false));
        dongle.stream(kPacingWindowNs + kPacingSnapshotNs);
        INFO("after stall " << stall + 1 << ": " << dongle.read().factor);
        CHECK(dongle.read().factor > kClientBehindLeave);
    }
}

// Rejects stating the window as the whole run once the run is longer, and
// stating it as the constant while the run is shorter: a client says "over
// the last N seconds" from this number.
TEST_CASE("the window read is the one measured over", "[engine][pacing]")
{
    Dongle dongle;
    dongle.stream(500'000'000);
    const double young = dongle.read().window_seconds;
    CHECK(young > 0.45);
    CHECK(young <= 0.5);

    dongle.stream(9'500'000'000);
    const double window = dongle.read().window_seconds;
    CHECK(window >= kPacingWindowSeconds);
    CHECK(window <= kPacingWindowSeconds + 0.15);
}

// Rejects a window that ends at the last arrival and nowhere else, which
// reads a dongle that has stopped at the rate it had, and rejects zero for
// one, which is "not measured" on the wire: a client that read a dead source
// that way would say nothing at the one moment the line matters most.
TEST_CASE("a front end that has stopped reads as stopped", "[engine][pacing]")
{
    Dongle dongle;
    dongle.stream(3'000'000'000);

    // Half a second of nothing arriving is already behind.
    for (int i = 0; i < 500; ++i) {
        dongle.now_ns += kStepNs;
        dongle.window.offer(dongle.sample());
    }
    const double early = dongle.read().factor;
    INFO("factor half a second in " << early);
    CHECK(early < kClientBehindEnter);

    // Five seconds of it is two blocks' worth over the window.
    for (int i = 0; i < 4500; ++i) {
        dongle.now_ns += kStepNs;
        dongle.window.offer(dongle.sample());
    }
    const double factor = dongle.read().factor;
    INFO("factor five seconds in " << factor);
    CHECK(factor > 0.0);
    CHECK(factor < 0.05);
}

// The other side of the same line: before anything is delivered the answer
// is zero, not a small number, so a source still opening is not called dead.
TEST_CASE("nothing delivered yet is not measured", "[engine][pacing]")
{
    Dongle dongle;
    CHECK(dongle.read().factor == 0.0);
    dongle.now_ns += 20'000'000;
    dongle.window.offer(dongle.sample());
    CHECK(dongle.read().factor == 0.0);
}
