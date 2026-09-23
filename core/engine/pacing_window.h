// How fast capture arrived over the last few seconds, which is what
// SourcePacing::realtime_factor reports.
//
// WHY THE FACTOR IS WINDOWED, AND WHY A PAUSE THE ENGINE ASKED FOR IS EXCUSED
//
// Until 2026-09-23 the factor was every sample delivered since run() started
// over the seconds since then. An RTL-SDR stops its transfers around every
// control transfer, a retune or a gain change, for about 330 ms, and counts
// the samples the device produced in that time as lost rather than
// delivered, so the stream's index stays on the device's clock. Under a
// lifetime mean each of those pauses took a third of a second out of the
// numerator for good: after N retunes in T seconds the factor read
// (T - 0.33 N) / T, the deficit added up with every tune, and the client
// called the source behind after the first few while the audio was not
// behind at all. A lifetime mean also cannot recover from anything, which is
// the other half of the same fault.
//
// So the factor is taken over the last kPacingWindowSeconds, and a sample
// the source lost inside a control call the engine itself made is counted
// with the delivered ones. Those samples were not late: the device produced
// them on its own clock while the engine had its transfers stopped on
// purpose, and SourceStats::samples_lost still reports every one of them as
// lost. While such a call is still running, the reading holds where it was
// when the call began, so a poll that lands inside the pause does not see it.
// What the factor keeps is a loss nobody asked for, such as a consumer too
// slow to drain the device, and that leaves the window within
// kPacingWindowSeconds of ending.
//
// WHY THE ENDS OF THE WINDOW ARE ARRIVALS AND NOT CLOCK TICKS
//
// The delivered count moves a block at a time: 65536 samples, 27 ms at
// 2.4 MS/s and 262 ms at 250 kS/s. A window whose ends are wherever the clock
// happened to be is off by up to a block at each end, which at two seconds is
// 1.4% at the first rate and 13% at the second, and the client calls a source
// behind at 0.97 and caught up at 0.99. So the window runs between two
// moments a count was first seen to change, which run()'s loop sees within
// a pass of its poll, and a steady source reads the rate it delivers at
// however large its blocks are.
//
// That alone would read a source that has stopped at its old rate forever,
// since no new arrival comes to end the window at. So a second reading is
// taken up to the moment of the call, crediting the source with two more
// blocks that may be about to land, and the factor is the lower of the two.
// On a source that is delivering, the second is not the lower until an
// arrival is more than a block's time late; on one that has stopped it falls
// from then on, to two blocks over the window. Two and not one because one
// was measured too few: a synthetic scene paced at 2 MS/s through the engine
// in tests/engine/test_engine_pacing.cpp read as low as 0.979 while exactly
// on time with one block credited, an arrival late by a timer tick being
// enough to pull it under, and 0.999 to 1.001 with two.
//
// WHY IT HOLDS NO CLOCK. The engine reads steady_clock and the source's
// counters and hands them in, so tests/engine/test_pacing_window.cpp can
// drive a stall shaped like the RTL-SDR's through it without a radio, a GPU
// or a sleep.
//
// NOT THE SAMPLE PATH. The engine offers a sample from run()'s control loop
// and reads one on an info() call, under a lock of its own; the delivery
// thread never touches this.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace revenant::engine {

// The window the factor is measured over. Two seconds: long enough that the
// time a poll can place an arrival late is a fraction of a percent of it,
// short enough that a stall nobody asked for is off the figure two seconds
// after it ends. The client polls once a second, so a stall that pulls the
// figure down for two seconds is on at least one of its polls.
inline constexpr double kPacingWindowSeconds = 2.0;
inline constexpr std::int64_t kPacingWindowNs = 2'000'000'000;

// The fewest nanoseconds between two arrivals the window keeps, which is
// what bounds how much it holds. A reading covers the window plus up to this
// and one block's worth of time more.
inline constexpr std::int64_t kPacingSnapshotNs = 100'000'000;

// The source's delivered count at one instant.
struct PacingSample {
    // steady_clock nanoseconds.
    std::int64_t at_ns = 0;

    // SourceStats::samples_delivered.
    std::uint64_t delivered = 0;
};

struct PacingReading {
    // Capture seconds per wall second over window_seconds. ZERO IS NOT
    // MEASURED, as SourcePacing::realtime_factor documents: nothing has been
    // delivered since the run started.
    double factor = 0.0;

    // The wall seconds the factor covers.
    double window_seconds = 0.0;
};

class PacingWindow {
public:
    // A new run. Everything recorded before is about a stream that has ended.
    void start(const PacingSample& origin)
    {
        excused_ = 0;
        pause_depth_ = 0;
        pause_at_ns_ = 0;
        block_ = 0;
        origin_ = Point{.at_ns = origin.at_ns, .counted = origin.delivered,
                        .delivered = origin.delivered};
        latest_ = origin_;
        kept_[0] = origin_;
        head_ = 1;
        count_ = 1;
    }

    // What run()'s loop saw on one pass. Only a pass that sees the count
    // move is an arrival, and an arrival is kept when kPacingSnapshotNs has
    // passed since the last one kept.
    void offer(const PacingSample& now)
    {
        if (count_ == 0) {
            return;
        }
        const Point seen = point(now);
        if (seen.counted == latest_.counted) {
            return;
        }
        note_block(seen);
        latest_ = seen;
        if (seen.at_ns - newest_kept().at_ns >= kPacingSnapshotNs) {
            kept_[head_] = seen;
            head_ = (head_ + 1) % kCapacity;
            count_ = std::min(count_ + 1, kCapacity);
        }
    }

    // The engine is about to make a control call that may stop the source.
    // Until the matching pause_ended, a reading is taken as of this moment.
    void pause_begun(std::int64_t at_ns)
    {
        if (pause_depth_++ == 0) {
            pause_at_ns_ = at_ns;
        }
    }

    // The call returned, and the source counted `lost` samples as lost while
    // it ran. They count as delivered here from now on.
    void pause_ended(std::uint64_t lost)
    {
        excused_ += lost;
        if (pause_depth_ > 0) {
            --pause_depth_;
        }
    }

    // The factor at `now`, which the caller reads from the same counters.
    [[nodiscard]] PacingReading read(const PacingSample& now, std::int64_t rate) const
    {
        PacingReading out;
        if (count_ == 0 || rate <= 0) {
            return out;
        }

        // Inside a pause the engine made, as of when it began, and never
        // before an arrival already seen.
        std::int64_t at_ns = now.at_ns;
        if (pause_depth_ > 0) {
            at_ns = std::max(std::min(at_ns, pause_at_ns_), latest_.at_ns);
        }

        // An arrival the loop has not offered yet ends the window here.
        Point end = latest_;
        std::uint64_t block = block_;
        const Point seen = point(PacingSample{.at_ns = at_ns, .delivered = now.delivered});
        if (seen.counted != latest_.counted) {
            if (seen.delivered > latest_.delivered) {
                block = std::max(block, seen.delivered - latest_.delivered);
            }
            end = seen;
        }

        if (now.delivered <= origin_.delivered) {
            // Nothing yet this run, which is not measured.
            out.window_seconds = seconds(at_ns - origin_.at_ns);
            return out;
        }

        const double sample_rate = static_cast<double>(rate);

        // Between two arrivals: the rate the source is delivering at.
        const Point& from_arrival = base_for(end.at_ns);
        double factor = 0.0;
        double window = 0.0;
        if (end.at_ns > from_arrival.at_ns) {
            window = seconds(end.at_ns - from_arrival.at_ns);
            factor = static_cast<double>(end.counted - from_arrival.counted) / sample_rate / window;
        }

        // Up to now, with two blocks credited: how far it can have got by
        // now, which is lower only once an arrival is more than a block late.
        const Point& from_now = base_for(at_ns);
        if (at_ns > from_now.at_ns) {
            const double window_now = seconds(at_ns - from_now.at_ns);
            const std::uint64_t counted = end.counted - from_now.counted + 2 * block;
            const double factor_now = static_cast<double>(counted) / sample_rate / window_now;
            if (window == 0.0 || factor_now < factor) {
                factor = factor_now;
                window = window_now;
            }
        }

        // Never zero once something has been delivered, since zero says it
        // was never measured. The blocks credited above keep it off zero
        // already; this is for a window no arrival has given a block size.
        out.factor = std::max(factor, 1.0 / sample_rate / std::max(window, 1e-9));
        out.window_seconds = window;
        return out;
    }

private:
    struct Point {
        std::int64_t at_ns = 0;

        // Delivered plus excused, which is what the factor counts.
        std::uint64_t counted = 0;

        std::uint64_t delivered = 0;
    };

    [[nodiscard]] static double seconds(std::int64_t ns) { return static_cast<double>(ns) / 1e9; }

    [[nodiscard]] Point point(const PacingSample& sample) const
    {
        return Point{.at_ns = sample.at_ns, .counted = sample.delivered + excused_,
                     .delivered = sample.delivered};
    }

    // The largest one arrival has carried, which is the block credited twice
    // to a source that may be about to deliver. The largest rather than the last,
    // because a backend can hand over the tail of a transfer as a short one.
    void note_block(const Point& seen)
    {
        if (seen.delivered > latest_.delivered) {
            block_ = std::max(block_, seen.delivered - latest_.delivered);
        }
    }

    [[nodiscard]] const Point& newest_kept() const
    {
        return kept_[(head_ + kCapacity - 1) % kCapacity];
    }

    // The newest arrival kept at least a window before `at_ns`, or the oldest
    // held when the run is younger than a window.
    [[nodiscard]] const Point& base_for(std::int64_t at_ns) const
    {
        const std::int64_t cutoff = at_ns - kPacingWindowNs;
        for (std::size_t i = 0; i < count_; ++i) {
            const Point& held = kept_[(head_ + kCapacity - 1 - i) % kCapacity];
            if (held.at_ns <= cutoff) {
                return held;
            }
        }
        return kept_[(head_ + kCapacity - count_) % kCapacity];
    }

    // Enough to hold a window of arrivals kept kPacingSnapshotNs apart with
    // room over, so the one a window starts at is never overwritten.
    static constexpr std::size_t kCapacity = 64;
    static_assert(static_cast<std::int64_t>(kCapacity - 1) * kPacingSnapshotNs >
                  2 * (kPacingWindowNs + kPacingSnapshotNs));

    std::array<Point, kCapacity> kept_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    Point origin_{};
    Point latest_{};
    std::uint64_t block_ = 0;
    std::uint64_t excused_ = 0;
    int pause_depth_ = 0;
    std::int64_t pause_at_ns_ = 0;
};

}  // namespace revenant::engine
