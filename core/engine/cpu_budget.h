// A share of one core, held as a token bucket charged in CPU time.
//
// Signal identification's two threads, the server's detector lane and the
// probe pool's worker, hold themselves to one of these as well as running
// below normal priority. The priority is what keeps them off a core the
// receivers need; this is what bounds what they take on a machine where
// nothing else wants the core, so that the worst case is a fraction of one
// core however busy the band is. docs/rpc.md, under Threading, has the
// numbers it was sized from.
//
// THE BUCKET. Credit accrues at `share` nanoseconds of CPU per nanosecond of
// wall time and is capped at `burst`; a job may start while there is credit,
// and the CPU it took is charged afterwards, which may take the credit below
// zero. So an idle stretch buys at most `burst` of work at once, and over any
// long run the thread averages no more than `share`.
//
// CHARGED IN CPU TIME, NOT WALL TIME. A below-normal thread on a busy machine
// can take a second of wall time to do ten milliseconds of work, and a budget
// charged in wall time would make it wait again for the wait it was already
// made to do. core/thread_role.h's this_thread_cpu_ns is the meter; its tick
// is coarse and its sum is right, and a bucket only ever looks at sums.
//
// One thread only. Nothing here is atomic.

#pragma once

#include <algorithm>
#include <cstdint>

namespace revenant::engine {

class CpuBudget {
public:
    // `share` of one core, above zero and at most one; `burst_ns` of CPU the
    // bucket holds at most.
    CpuBudget(double share, std::uint64_t burst_ns) : share_(share), burst_(burst_ns) {}

    // Whether a job may start at wall time `now_ns`.
    [[nodiscard]] bool may_start(std::uint64_t now_ns)
    {
        refill(now_ns);
        return credit_ > 0.0;
    }

    // What the job that just ran took, `cpu_ns` of this thread's CPU, ending
    // at wall time `now_ns`.
    void charge(std::uint64_t cpu_ns, std::uint64_t now_ns)
    {
        refill(now_ns);
        credit_ -= static_cast<double>(cpu_ns);
    }

    [[nodiscard]] double share() const { return share_; }

private:
    void refill(std::uint64_t now_ns)
    {
        if (!started_) {
            started_ = true;
            last_ns_ = now_ns;
            credit_ = static_cast<double>(burst_);
            return;
        }
        if (now_ns > last_ns_) {
            credit_ = std::min(static_cast<double>(burst_),
                               credit_ + static_cast<double>(now_ns - last_ns_) * share_);
            last_ns_ = now_ns;
        }
    }

    double share_;
    std::uint64_t burst_;
    bool started_ = false;
    std::uint64_t last_ns_ = 0;
    double credit_ = 0.0;
};

}  // namespace revenant::engine
