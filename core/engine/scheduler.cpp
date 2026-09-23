#include "core/engine/scheduler.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#include "core/engine/load_clock.h"
#include "core/engine/spsc_ring.h"
#include "core/thread_role.h"

namespace revenant::engine {
namespace {

// A chunk of a parallel_for, packed into one word so a deque slot is a single
// lock-free atomic load. begin in the high half, end in the low half; both fit
// in 32 bits because nothing here forks over more than four billion items and
// the graph's largest range is a receiver count.
[[nodiscard]] constexpr std::uint64_t pack_range(std::uint32_t begin, std::uint32_t end) {
    return (static_cast<std::uint64_t>(begin) << 32) | static_cast<std::uint64_t>(end);
}

[[nodiscard]] constexpr std::uint32_t range_begin(std::uint64_t packed) {
    return static_cast<std::uint32_t>(packed >> 32);
}

[[nodiscard]] constexpr std::uint32_t range_end(std::uint64_t packed) {
    return static_cast<std::uint32_t>(packed & 0xFFFFFFFFULL);
}

// Slots per deque. Recursive halving pushes at most log2(items) ranges from
// one worker at a time, so this is enormous headroom; a push that fails
// anyway is handled by running the range whole rather than by growing, which
// keeps the structure allocation-free once it is built.
constexpr std::size_t kDequeSlots = 1024;

// Chase-Lev: the owner pushes and pops at the bottom, thieves take from the
// top. Nothing here locks.
//
// The subtle part is pop() racing the last element against a steal(). Both
// sides resolve it with one compare-exchange on `top`, so exactly one of them
// wins and neither ever runs a chunk twice. Written from the published
// algorithm (Chase and Lev, "Dynamic Circular Work-Stealing Deque", SPAA
// 2005) with a fixed-size array in place of the growable one, because a deque
// that never grows cannot have the memory-reclamation problem the growable
// form carries.
class alignas(64) Deque {
public:
    Deque() : slots_(std::make_unique<std::atomic<std::uint64_t>[]>(kDequeSlots)) {}

    Deque(const Deque&) = delete;
    Deque& operator=(const Deque&) = delete;
    Deque(Deque&&) = delete;
    Deque& operator=(Deque&&) = delete;

    // Owner thread only.
    bool push(std::uint64_t value) {
        const auto bottom = bottom_.load(std::memory_order_relaxed);
        const auto top = top_.load(std::memory_order_acquire);
        if (bottom - top >= kDequeSlots) {
            return false;
        }
        slots_[bottom & kMask].store(value, std::memory_order_relaxed);

        // Release: the slot's value must be visible to a thief that later sees
        // the new bottom. Without it a steal can read an uninitialised slot.
        bottom_.store(bottom + 1, std::memory_order_release);
        return true;
    }

    // Owner thread only.
    bool pop(std::uint64_t& out) {
        auto bottom = bottom_.load(std::memory_order_relaxed);
        const auto top_snapshot = top_.load(std::memory_order_acquire);
        if (bottom == top_snapshot) {
            return false;
        }

        bottom -= 1;

        // seq_cst on both this store and the load below, and this is the one
        // place in the file where anything weaker is wrong. The owner must not
        // see a stale `top` after announcing the lower `bottom`, or the owner
        // and a thief both take the last element.
        bottom_.store(bottom, std::memory_order_seq_cst);
        auto top = top_.load(std::memory_order_seq_cst);

        if (top < bottom) {
            out = slots_[bottom & kMask].load(std::memory_order_relaxed);
            return true;
        }
        if (top == bottom) {
            const std::uint64_t value = slots_[bottom & kMask].load(std::memory_order_relaxed);
            const bool won = top_.compare_exchange_strong(top, top + 1, std::memory_order_seq_cst,
                                                          std::memory_order_relaxed);
            bottom_.store(bottom + 1, std::memory_order_relaxed);
            if (!won) {
                return false;
            }
            out = value;
            return true;
        }

        // A thief emptied the deque between the two loads.
        bottom_.store(bottom + 1, std::memory_order_relaxed);
        return false;
    }

    // Any thread.
    bool steal(std::uint64_t& out) {
        auto top = top_.load(std::memory_order_acquire);

        // The fence orders this thread's load of top against its load of
        // bottom, which is what makes the emptiness test below meaningful
        // against a concurrent pop.
        std::atomic_thread_fence(std::memory_order_seq_cst);

        const auto bottom = bottom_.load(std::memory_order_acquire);
        if (top >= bottom) {
            return false;
        }

        const std::uint64_t value = slots_[top & kMask].load(std::memory_order_relaxed);
        if (!top_.compare_exchange_strong(top, top + 1, std::memory_order_seq_cst,
                                          std::memory_order_relaxed)) {
            return false;
        }
        out = value;
        return true;
    }

private:
    static_assert(std::has_single_bit(kDequeSlots), "the deque masks its index, so the slot count "
                                                    "must be a power of two");
    static constexpr std::uint64_t kMask = kDequeSlots - 1;

    // Separate cache lines: the owner writes bottom on every push and pop and
    // every thief reads top, so sharing a line would put the two in permanent
    // contention. The padding is spelled out rather than left to the alignment
    // specifier so the layout is visible and the compiler inserts none of its
    // own.
    std::unique_ptr<std::atomic<std::uint64_t>[]> slots_;
    std::uint64_t pad_after_slots_[7]{};

    std::atomic<std::uint64_t> bottom_{0};
    std::uint64_t pad_after_bottom_[7]{};

    std::atomic<std::uint64_t> top_{0};
    std::uint64_t pad_after_top_[7]{};
};

static_assert(sizeof(Deque) == 192, "the deque's three fields each own a cache line, with no "
                                    "padding the compiler had to invent");

}  // namespace

std::size_t default_worker_count(std::size_t reserved_threads) {
    const unsigned hardware = std::thread::hardware_concurrency();
    if (hardware == 0) {
        return 1;
    }
    const std::size_t total = static_cast<std::size_t>(hardware);
    return total > reserved_threads ? total - reserved_threads : std::size_t{1};
}

// ---------------------------------------------------------------------------
// WorkStealingPool
// ---------------------------------------------------------------------------

struct WorkStealingPool::Impl {
    // One deque per worker plus one the submitting thread owns. The submitter
    // is not a worker: it is the recording thread or the completion thread,
    // and giving it a deque of its own is what lets it seed the job and then
    // participate on equal terms instead of blocking.
    std::vector<std::unique_ptr<Deque>> deques;
    std::vector<std::thread> workers;

    const RangeBody* body = nullptr;
    std::uint32_t grain = 1;

    // Items not yet executed. The submitter leaves parallel_for when this
    // reaches zero, which is also the proof that every deque is empty and no
    // worker still holds a pointer to `body`.
    std::atomic<std::size_t> remaining{0};

    // Bumped whenever work appears, and what an idle worker parks on. A
    // counter rather than a flag so a wake that lands between the check and
    // the wait is not lost.
    std::atomic<std::uint64_t> epoch{0};

    std::atomic<bool> job_active{false};
    std::atomic<bool> stopping{false};

    std::atomic<std::uint64_t> chunks_run{0};
    std::atomic<std::uint64_t> steals{0};
    std::atomic<std::uint64_t> inline_runs{0};

    [[nodiscard]] std::size_t submitter_index() const { return deques.size() - 1; }

    void wake_all() {
        epoch.fetch_add(1, std::memory_order_release);
        epoch.notify_all();
    }

    // Splits the range down to the grain, pushing the far half onto this
    // thread's own deque so a thief can take it, then runs what is left.
    void execute(std::size_t owner, std::uint64_t packed) {
        std::uint32_t begin = range_begin(packed);
        std::uint32_t end = range_end(packed);

        for (;;) {
            const std::uint32_t span = end - begin;
            if (span <= grain) {
                break;
            }
            const std::uint32_t mid = begin + span / 2;
            if (!deques[owner]->push(pack_range(mid, end))) {
                break;  // full: run the whole range rather than losing it
            }
            end = mid;

            // A worker that was asleep when the job started has to be told
            // there is something to steal now.
            wake_all();
        }

        (*body)(static_cast<std::size_t>(begin), static_cast<std::size_t>(end));
        chunks_run.fetch_add(1, std::memory_order_relaxed);

        const auto done = static_cast<std::size_t>(end - begin);
        if (remaining.fetch_sub(done, std::memory_order_acq_rel) == done) {
            remaining.notify_all();
        }
    }

    // One unit of work from anywhere, own deque first. Returns false when
    // there was nothing to take.
    bool run_one(std::size_t owner) {
        std::uint64_t packed = 0;
        if (deques[owner]->pop(packed)) {
            execute(owner, packed);
            return true;
        }

        const std::size_t count = deques.size();
        for (std::size_t offset = 1; offset < count; ++offset) {
            const std::size_t victim = (owner + offset) % count;
            if (deques[victim]->steal(packed)) {
                steals.fetch_add(1, std::memory_order_relaxed);
                execute(owner, packed);
                return true;
            }
        }
        return false;
    }

    void worker_loop(std::size_t index) {
        describe_this_thread(std::format(L"revenant pool {}", index), ThreadClass::Listening);
        while (!stopping.load(std::memory_order_acquire)) {
            const auto observed = epoch.load(std::memory_order_acquire);
            if (run_one(index)) {
                continue;
            }
            if (stopping.load(std::memory_order_acquire)) {
                return;
            }
            epoch.wait(observed, std::memory_order_acquire);
        }
    }
};

WorkStealingPool::WorkStealingPool() : impl_(std::make_unique<Impl>()) {}

WorkStealingPool::~WorkStealingPool() {
    if (impl_ == nullptr) {
        return;
    }
    impl_->stopping.store(true, std::memory_order_release);
    impl_->wake_all();
    for (auto& thread : impl_->workers) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

Expected<std::unique_ptr<WorkStealingPool>> WorkStealingPool::create(std::size_t workers) {
    if (workers == 0) {
        return fail("WorkStealingPool::create with zero workers: a pool with no workers is a "
                    "serial executor wearing the wrong name, so it is rejected rather than "
                    "silently accepted");
    }
    if (workers > 1024) {
        return fail(std::format("WorkStealingPool::create asked for {} workers, which is more "
                                "threads than any machine this runs on has",
                                workers));
    }

    std::unique_ptr<WorkStealingPool> pool(new (std::nothrow) WorkStealingPool());
    if (pool == nullptr || pool->impl_ == nullptr) {
        return fail("WorkStealingPool::create could not allocate the pool");
    }

    auto& impl = *pool->impl_;
    impl.deques.reserve(workers + 1);
    for (std::size_t i = 0; i <= workers; ++i) {
        impl.deques.push_back(std::make_unique<Deque>());
    }

    impl.workers.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        try {
            impl.workers.emplace_back([impl_ptr = &impl, i] { impl_ptr->worker_loop(i); });
        } catch (const std::system_error& error) {
            return fail(std::format("could not start pool worker {} of {}: {}", i, workers,
                                    error.what()),
                        error.code().value());
        }
    }
    return pool;
}

std::size_t WorkStealingPool::worker_count() const { return impl_->workers.size(); }

std::uint64_t WorkStealingPool::chunks_run() const {
    return impl_->chunks_run.load(std::memory_order_relaxed);
}

std::uint64_t WorkStealingPool::steals() const {
    return impl_->steals.load(std::memory_order_relaxed);
}

std::uint64_t WorkStealingPool::inline_runs() const {
    return impl_->inline_runs.load(std::memory_order_relaxed);
}

void WorkStealingPool::parallel_for(std::size_t items, std::size_t grain, const RangeBody& body) {
    if (items == 0 || !body) {
        return;
    }

    auto& impl = *impl_;

    // One live job at a time. A second caller runs inline, which is correct
    // and merely serial; see the header for why that trade is the right one
    // here.
    bool expected = false;
    if (items > 0xFFFFFFFFULL ||
        !impl.job_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
        impl.inline_runs.fetch_add(1, std::memory_order_relaxed);
        body(0, items);
        return;
    }

    impl.body = &body;
    impl.grain = static_cast<std::uint32_t>(std::max<std::size_t>(grain, 1));
    impl.remaining.store(items, std::memory_order_release);

    const std::size_t submitter = impl.submitter_index();
    const std::uint64_t whole = pack_range(0, static_cast<std::uint32_t>(items));
    if (!impl.deques[submitter]->push(whole)) {
        // Cannot happen with an empty deque and a capacity of kDequeSlots, but
        // failing loudly beats deadlocking on a counter nobody will decrement.
        impl.remaining.store(0, std::memory_order_release);
        impl.body = nullptr;
        impl.job_active.store(false, std::memory_order_release);
        impl.inline_runs.fetch_add(1, std::memory_order_relaxed);
        body(0, items);
        return;
    }
    impl.wake_all();

    // The submitter participates rather than blocking. When there is nothing
    // left to take but work is still outstanding, a worker is holding the last
    // chunk and the only thing to do is let it finish.
    while (impl.remaining.load(std::memory_order_acquire) != 0) {
        if (!impl.run_one(submitter)) {
            std::this_thread::yield();
        }
    }

    impl.body = nullptr;
    impl.job_active.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------

namespace {

// What crosses the SpscRing to the completion thread. Trivially copyable, as
// the ring requires, so the handler resolves the ticket rather than the queue
// carrying a callable.
struct Completion {
    std::uint64_t timeline_value;
    std::uint32_t ticket;
    std::uint32_t reserved;
};

static_assert(sizeof(Completion) == 16, "the completion record is copied through an SpscRing and "
                                        "its size is part of that structure's cost");

}  // namespace

struct Scheduler::Impl {
    const gpu::Context* context = nullptr;
    std::unique_ptr<WorkStealingPool> pool;
    std::unique_ptr<SpscRing<Completion>> queue;
    std::uint64_t wait_timeout_ns = 0;

    VkSemaphore timeline = VK_NULL_HANDLE;
    CompletionHandler handler;
    std::thread thread;

    std::atomic<std::uint64_t> posted{0};
    std::atomic<std::uint64_t> finished{0};
    std::atomic<std::uint64_t> host_waits{0};
    std::atomic<std::uint64_t> wait_ns{0};
    std::atomic<std::uint64_t> handler_ns{0};
    std::atomic<std::uint64_t> handler_max_ns{0};

    // Separate from `posted` on purpose. `posted` is a count that drain()
    // compares against `finished`, and shutdown has to wake the completion
    // thread without inventing a frame that will never finish.
    std::atomic<std::uint64_t> signal{0};
    std::atomic<bool> stopping{false};
    std::atomic<bool> has_error{false};

    // Control plane only: the completion thread writes the first error once
    // and a control thread reads it. Never touched from the sample path.
    mutable std::mutex error_lock;
    Error first_error;

    // Formats where a second failure cannot escape. Everything here
    // allocates and this runs inside a catch block on a thread that
    // terminates the process if an exception leaves it, so an Error with no
    // message is the floor rather than the aim.
    [[nodiscard]] static Status handler_threw(std::uint32_t ticket, const char* detail) noexcept {
        try {
            return fail(detail != nullptr
                            ? std::format("the completion handler for frame {} threw an "
                                          "exception: {}",
                                          ticket, detail)
                            : std::format("the completion handler for frame {} threw an "
                                          "exception that is not a std::exception",
                                          ticket));
        } catch (...) {
        }
        return std::unexpected(Error{});
    }

    void record_error(Error error) {
        std::scoped_lock lock(error_lock);
        if (!has_error.load(std::memory_order_relaxed)) {
            first_error = std::move(error);
            has_error.store(true, std::memory_order_release);
        }
    }

    void loop() {
        describe_this_thread(L"revenant completion", ThreadClass::Listening);
        for (;;) {
            Completion item{};
            const std::size_t got = queue->read(std::span<Completion>(&item, 1));
            if (got == 0) {
                // The snapshot comes FIRST, before either test that could
                // make parking the wrong answer, and that order is what
                // makes the park safe rather than merely usual.
                //
                // stop() stores `stopping` and then bumps `signal`, both
                // release. Either this acquire load already includes that
                // bump, in which case it synchronises with the fetch_add and
                // the load of `stopping` below is guaranteed to read true, or
                // it does not, in which case the bump lands after the
                // snapshot and wait() returns at once because the value has
                // moved. post() is the same argument with the queue write in
                // place of the flag.
                //
                // WHAT THIS USED TO DO. Until 2026-09-20 `stopping` was read
                // first and `signal` second, with no re-read of the flag
                // afterwards. A stop() that ran entirely between the two
                // reads left this thread waiting on the value stop() had just
                // written, with the one notify already spent on a waiter that
                // did not exist yet and nothing left to move the counter
                // again: Scheduler::stop() then blocked in join() for the
                // rest of the process's life. WorkStealingPool::worker_loop
                // in this file and RingConsumer::reserve_blocking in
                // ring_consumer.h both already read their counter before
                // testing their flag, the second with the reason written out.
                const auto observed = signal.load(std::memory_order_acquire);
                if (stopping.load(std::memory_order_acquire)) {
                    return;
                }
                if (queue->readable() != 0) {
                    continue;
                }
                signal.wait(observed, std::memory_order_acquire);
                continue;
            }

            // The one host wait per block. Everything above this line ran
            // without the host looking at the device.
            VkSemaphoreWaitInfo wait{};
            wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            wait.semaphoreCount = 1;
            wait.pSemaphores = &timeline;
            wait.pValues = &item.timeline_value;

            VkResult result = VK_SUCCESS;
            {
                const LoadTimer timed(wait_ns);
                result = vkWaitSemaphores(context->device(), &wait, wait_timeout_ns);
            }
            host_waits.fetch_add(1, std::memory_order_relaxed);

            if (result == VK_TIMEOUT) {
                record_error(Error{std::format(
                    "the graph's frame {} did not reach timeline value {} within {} ns",
                    item.ticket, item.timeline_value, wait_timeout_ns)});
            } else if (result != VK_SUCCESS) {
                record_error(Error{std::format("vkWaitSemaphores failed ({})",
                                               gpu::result_name(result)),
                                   result});
            }

            // The handler runs whatever the frame owes: readback, sinks, and
            // the ring retirement. It runs even after a failed wait, and that
            // is deliberate. Skipping it leaves the ring's retire cursor
            // behind and the frame slot taken, so a producer parked on either
            // never wakes and the process cannot be stopped. A hung GPU should
            // end with the error below reported, not with a hang in the
            // shutdown path as well.
            //
            // THE CATCH IS THE BACKSTOP AND NOT THE MECHANISM. This thread
            // was created with std::thread, so an exception leaving the
            // handler is std::terminate: the process ends with whatever the
            // runtime prints and nothing of the promise on Scheduler::error
            // is kept. The handler reaches every sink an integrator
            // attached, none of them declared noexcept and none of them in
            // this tree's control.
            //
            // The sinks are caught one level down, in
            // core/engine/graph.cpp's call_sink, because a throw caught only
            // here leaves that frame's bookkeeping half done. This exists
            // for everything else a handler can do, so that the worst case
            // is a recorded error rather than no error and no process.
            if (handler) {
                const LoadTimer timed(handler_ns, &handler_max_ns);
                Status status;
                try {
                    status = handler(item.timeline_value, item.ticket);
                } catch (const std::exception& thrown) {
                    status = handler_threw(item.ticket, thrown.what());
                } catch (...) {
                    status = handler_threw(item.ticket, nullptr);
                }
                if (!status) {
                    record_error(status.error());
                }
            }

            finished.fetch_add(1, std::memory_order_release);
            finished.notify_all();
        }
    }
};

Scheduler::Scheduler() : impl_(std::make_unique<Impl>()) {}

Scheduler::~Scheduler() { stop(); }

Expected<std::unique_ptr<Scheduler>> Scheduler::create(const gpu::Context& context,
                                                       const SchedulerConfig& config) {
    if (!context.valid()) {
        return fail("Scheduler::create called with an invalid gpu::Context");
    }

    std::unique_ptr<Scheduler> scheduler(new (std::nothrow) Scheduler());
    if (scheduler == nullptr || scheduler->impl_ == nullptr) {
        return fail("Scheduler::create could not allocate the scheduler");
    }

    const std::size_t workers = config.worker_threads != 0
                                    ? config.worker_threads
                                    : default_worker_count(config.reserved_threads);

    auto pool = WorkStealingPool::create(workers);
    if (!pool) {
        return std::unexpected(with_context(pool.error(), "Scheduler::create"));
    }

    auto queue = SpscRing<Completion>::create(config.completion_capacity);
    if (!queue) {
        return std::unexpected(with_context(queue.error(), "Scheduler::create completion queue"));
    }

    scheduler->impl_->context = &context;
    scheduler->impl_->pool = std::move(*pool);
    scheduler->impl_->queue = std::move(*queue);
    scheduler->impl_->wait_timeout_ns = config.wait_timeout_ns;
    return scheduler;
}

WorkStealingPool& Scheduler::pool() { return *impl_->pool; }

Status Scheduler::start(VkSemaphore timeline, CompletionHandler handler) {
    if (timeline == VK_NULL_HANDLE) {
        return fail("Scheduler::start without a timeline semaphore");
    }
    if (impl_->thread.joinable()) {
        return fail("Scheduler::start called twice");
    }
    impl_->timeline = timeline;
    impl_->handler = std::move(handler);
    impl_->stopping.store(false, std::memory_order_release);

    try {
        impl_->thread = std::thread([impl = impl_.get()] { impl->loop(); });
    } catch (const std::system_error& error) {
        return fail(std::format("could not start the completion thread: {}", error.what()),
                    error.code().value());
    }
    return {};
}

Status Scheduler::post(std::uint64_t timeline_value, std::uint32_t ticket) {
    if (!impl_->thread.joinable()) {
        return fail("Scheduler::post before start");
    }

    const Completion item{timeline_value, ticket, 0};
    for (;;) {
        if (impl_->queue->write(std::span<const Completion>(&item, 1)) == 1) {
            break;
        }
        // Full means the GPU is completion_capacity frames behind. There is
        // nothing useful for the recording thread to do with the next block
        // until one retires, so waiting here is the backpressure rather than a
        // stall to be engineered away.
        const auto observed = impl_->finished.load(std::memory_order_acquire);
        if (impl_->queue->writable() != 0) {
            continue;
        }
        impl_->finished.wait(observed, std::memory_order_acquire);
    }

    impl_->posted.fetch_add(1, std::memory_order_release);
    impl_->signal.fetch_add(1, std::memory_order_release);
    impl_->signal.notify_all();
    return {};
}

Status Scheduler::drain() {
    if (!impl_->thread.joinable()) {
        return {};
    }
    for (;;) {
        const auto posted = impl_->posted.load(std::memory_order_acquire);
        const auto finished = impl_->finished.load(std::memory_order_acquire);
        if (finished >= posted) {
            break;
        }
        impl_->finished.wait(finished, std::memory_order_acquire);
    }
    if (impl_->has_error.load(std::memory_order_acquire)) {
        return std::unexpected(error());
    }
    return {};
}

void Scheduler::stop() {
    if (impl_ == nullptr) {
        return;
    }
    impl_->stopping.store(true, std::memory_order_release);
    impl_->signal.fetch_add(1, std::memory_order_release);
    impl_->signal.notify_all();
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

SchedulerStats Scheduler::stats() const {
    SchedulerStats out;
    out.completions_posted = impl_->posted.load(std::memory_order_relaxed);
    out.completions_finished = impl_->finished.load(std::memory_order_relaxed);
    out.host_waits = impl_->host_waits.load(std::memory_order_relaxed);
    out.wait_ns = impl_->wait_ns.load(std::memory_order_relaxed);
    out.handler_ns = impl_->handler_ns.load(std::memory_order_relaxed);
    out.handler_max_ns = impl_->handler_max_ns.load(std::memory_order_relaxed);
    if (impl_->pool != nullptr) {
        out.chunks_run = impl_->pool->chunks_run();
        out.steals = impl_->pool->steals();
        out.inline_runs = impl_->pool->inline_runs();
    }
    return out;
}

bool Scheduler::failed() const { return impl_->has_error.load(std::memory_order_acquire); }

Error Scheduler::error() const {
    std::scoped_lock lock(impl_->error_lock);
    return impl_->first_error;
}

}  // namespace revenant::engine
