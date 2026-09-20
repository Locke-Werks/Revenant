// The engine side of the wire.
//
// server.h states the threading and the backpressure contract and this file
// is what has to hold them up. Five decisions it had to make on its own, the
// first of them against what the header asks for, each one a place where the
// obvious implementation is wrong.
//
// THE SINK CANNOT USE kj::Executor::executeAsync, WHICH THE HEADER ASKS FOR
//
// kj::Executor::send() does `event.replyExecutor = getCurrentThreadExecutor()`
// for every asynchronous request, and getCurrentThreadExecutor() requires the
// calling thread to be running a kj event loop. The engine's completion
// thread is not, so executeAsync throws "No event loop is running on this
// thread" rather than delivering anything. The other direction is no better:
// the promise executeAsync hands back belongs to the requesting thread, and
// dropping it cancels the work instead of detaching it, so there is no
// fire-and-forget shape of that call even from a thread that does have a
// loop.
//
// executeSync does work from a loopless thread, and it is still the wrong
// answer. It blocks its caller until the loop thread reaches the work, and
// its caller here is the thread retiring GPU readbacks. The loop thread owes
// that thread nothing in return: it is serving every other client on the
// socket, so whatever the slowest of them asked for is what the completion
// thread would be parked behind.
//
// So the hand-off is a single-frame slot plus kj::newPromiseAndCrossThreadFulfiller,
// which is the kj primitive that is explicitly safe to fire from a thread
// with no event loop and which does not block the firing thread. The sink
// stores the frame and rings the bell; the loop thread drains and fans out.
// The executor is still captured at startup, because a cross-thread promise
// holds a bare reference to the executor of the thread that created it.
//
// THE ONE CALL THAT OPENS HARDWARE DOES NOT RUN ON THE LOOP THREAD
//
// Session::listSources calls source::describe_sources(), which
// core/source/registry.h says costs more than enumeration because some
// backends must briefly open a device to answer. For the RTL-SDR backend that
// reaches rtlsdr_open, a libusb open, claim and reset. Run on the loop thread
// it dispatched nothing for the whole of that: no other client's calls, no
// spectrum fan-out, and not the shutdown fulfiller stop() waits on. One
// dongle's enumeration held up everything the server does, which is this file
// arguing against executeSync directly above and then doing the same thing
// itself.
//
// It runs on a thread of this file's own and the listing comes back through
// kj::newPromiseAndCrossThreadFulfiller, the same primitive the sink uses.
// The alternative kj offers, AsyncIoProvider::newPipeThread, was not taken
// for three reasons. It hands back a byte pipe, so a
// std::vector<SourceCapabilities> would have to be serialised and parsed
// again to cross one thread boundary inside one process. It starts a thread
// per call, so two clients asking at once would open the same dongle twice
// and each would be told the other process has it. And kj::Thread's
// destructor joins, so dropping the returned PipeThread on the loop thread
// would block the loop on exactly the device open being moved off it.
//
// One worker and one queue, so the listings stay serialised the way the loop
// thread serialised them before. stop() closes the queue and joins that
// thread before it ends the loop, so no listing is ever fulfilled at an event
// loop that has gone.
//
// THE COPY IS LOAD-BEARING, NOT DEFENSIVE
//
// engine::SpectrumFrame::power_db is valid for the duration of the sink call
// and not after. With an asynchronous hand-off the loop thread reads the
// frame long after that call has returned, so the copy on the completion
// thread is what makes the whole arrangement legal rather than a precaution
// that could be optimised away.
//
// The allocation behind the copy is not load-bearing. It used to happen once
// per frame on that same completion thread, and the displaced frame's bins
// were freed inside the lock the loop thread waits on. The buffers are pooled
// instead. At most three exist: the engine has one completion thread
// (core/engine/scheduler.h), so one buffer is being filled, one is waiting in
// the slot and one is being fanned out. Not measured. Nothing in this tree
// counts allocations on that thread, so the change rests on the argument and
// not on a number.
//
// THE SINK OUTLIVES THE SERVER, ROUTINELY
//
// engine::Graph::set_spectrum_sink queues a control operation and a dispatch
// already in flight keeps a shared_ptr to the previous sink. Clearing the
// sink therefore does not mean the callable is dead. The callable reaches the
// server through a heap-allocated gate it co-owns, and stop() closes that
// gate under its lock, so a late call finds a null owner instead of freed
// memory.
//
// ONE SERVER PER ENGINE IS ENFORCED HERE BECAUSE NOTHING ELSE CAN
//
// server.h requires create() to reject a second server on one engine, and
// engine::Engine has no way to ask whether a spectrum sink is already
// installed: set_spectrum_sink replaces silently. The file-scope claim list
// below is the only place that question can be answered.

#include "core/rpc/server.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <capnp/message.h>
#include <capnp/rpc-twoparty.h>
#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/exception.h>
#include <kj/memory.h>

#include "core/rpc/convert.h"
#include "core/source/capabilities.h"
#include "core/source/registry.h"

namespace revenant::rpc {
namespace {

// Engines that already have a server. See the header note above: the engine
// cannot be asked, so the answer is kept here.
//
// Function-local statics rather than namespace-scope objects, because
// Server::create is a library entry point and nothing stops a caller reaching
// it from another translation unit's static initialiser.
std::mutex& claim_lock() {
    static std::mutex lock;
    return lock;
}

std::vector<const engine::Engine*>& claimed_engines() {
    static std::vector<const engine::Engine*> engines;
    return engines;
}

[[nodiscard]] Status claim_engine(const engine::Engine& engine) {
    std::scoped_lock held(claim_lock());
    auto& engines = claimed_engines();
    if (std::ranges::find(engines, &engine) != engines.end()) {
        return fail("this engine already has an RPC server. A second one would replace the "
                    "first's spectrum sink, and the first would then hold subscriptions that "
                    "never receive another frame");
    }
    engines.push_back(&engine);
    return {};
}

void release_engine(const engine::Engine& engine) {
    std::scoped_lock held(claim_lock());
    std::erase(claimed_engines(), &engine);
}

// An engine failure becomes a Cap'n Proto exception carrying the engine's own
// message. Defaulting the result instead would hand a client a zeroed struct
// and no way to tell it from a real answer.
[[nodiscard]] kj::Exception to_exception(const Error& error) {
    // error.h: a zero code means the failure was ours rather than a call's, so
    // printing it would attribute our own message to a driver.
    const std::string text = error.code == 0
                                 ? error.message
                                 : std::format("{} (code {})", error.message, error.code);
    return {kj::Exception::Type::FAILED, __FILE__, __LINE__,
            kj::heapString(text.data(), text.size())};
}

[[nodiscard]] std::string describe(const kj::Exception& exception) {
    const kj::StringPtr text = exception.getDescription();
    return {text.cStr(), text.size()};
}

// The schema carries a receiver id as UInt64 and engine::VrxId holds 32 bits.
// A value that does not fit cannot name a receiver this engine issued, and
// truncating it would address a different one.
[[nodiscard]] Expected<engine::VrxId> to_vrx_id(std::uint64_t wire) {
    if (wire > std::numeric_limits<std::uint32_t>::max()) {
        return fail(std::format("no receiver {} is registered", wire));
    }
    return engine::VrxId{static_cast<std::uint32_t>(wire)};
}

// A frame that owns its bins, so it can outlive the sink call.
//
// engine::SpectrumFrame is reused verbatim rather than mirrored, because
// convert.cpp's write_spectrum_frame takes one and this file is not allowed to
// write its own conversion.
//
// Filled in place rather than constructed per frame. See the note at the top:
// the copy has to happen, the allocation behind it does not, and the thread
// doing both is the one retiring GPU readbacks.
class FrameCopy {
public:
    FrameCopy() = default;

    FrameCopy(const FrameCopy&) = delete;
    FrameCopy& operator=(const FrameCopy&) = delete;

    // assign rather than a fresh vector: it keeps whatever capacity this
    // buffer already had, so a recycled one copies the bins without reaching
    // the allocator at all.
    void assign(const engine::SpectrumFrame& source) {
        bins_.assign(source.power_db.begin(), source.power_db.end());
        frame_ = source;
        frame_.power_db = std::span<const float>(bins_);
    }

    [[nodiscard]] const engine::SpectrumFrame& frame() const { return frame_; }

private:
    std::vector<float> bins_;
    engine::SpectrumFrame frame_;
};

// The most frame buffers that can exist at once, and so the pool's capacity:
// one being filled on the completion thread, one waiting in the slot, one
// being fanned out on the loop thread. core/engine/scheduler.h has exactly
// one completion thread, so no two frames are ever mid-copy.
constexpr std::size_t kFrameBuffers = 3;

// What listSources answers with, and how the worker thread hands one back.
using SourceListing = std::vector<source::SourceCapabilities>;
using ListingFulfiller = kj::Own<kj::CrossThreadPromiseFulfiller<SourceListing>>;

// One subscriber. Touched only on the event loop thread, so none of it is
// atomic and none of it is locked.
//
// enable_shared_from_this because a send's continuation has to find this node
// again later and must tolerate it having been dropped in the meantime: the
// client may cancel, or disconnect, while a frame is still on the wire.
struct Subscription : std::enable_shared_from_this<Subscription> {
    Subscription(schema::SpectrumReceiver::Client client, std::uint32_t nth)
        : receiver(kj::mv(client)), every_nth(nth) {}

    schema::SpectrumReceiver::Client receiver;
    std::uint32_t every_nth = 1;

    // The whole of the backpressure rule: true from the moment a frame is
    // handed to this receiver until its call resolves. A frame arriving while
    // it is true is dropped and counted.
    bool in_flight = false;

    bool cancelled = false;
};

class ServerImpl;

// What the engine's sink reaches the server through.
//
// Heap-allocated and co-owned by the sink callable, because the callable
// outlives the Server whenever a dispatch was in flight when the sink was
// cleared. Closing the gate under its own lock is what makes that safe rather
// than merely unlikely.
struct SinkGate {
    std::mutex lock;
    ServerImpl* owner = nullptr;
};

class ServerImpl final : public Server, private kj::TaskSet::ErrorHandler {
public:
    explicit ServerImpl(engine::Engine& engine)
        : engine_(engine), gate_(std::make_shared<SinkGate>()) {
        gate_->owner = this;

        // Reserved once so that returning a frame buffer to the pool, which
        // happens under frame_lock_, cannot grow a vector there.
        spare_.reserve(kFrameBuffers);
    }

    // noexcept because Server's destructor is, and kj::Own's is not: a member
    // Own throwing on the way out would terminate rather than propagate. The
    // alternative is changing a frozen header.
    ~ServerImpl() noexcept override { stop(); }

    [[nodiscard]] Status start(const ServerOptions& options);

    // Installs the spectrum sink, or reports why the engine refused.
    //
    // Called from create() and again from subscribeSpectrum. The retry is not
    // belt and braces: Engine::set_spectrum_sink fails before a source is
    // open, and a server constructed in that window would otherwise stay
    // spectrum-deaf for the rest of its life even once the source arrived.
    [[nodiscard]] Status ensure_sink();

    [[nodiscard]] std::uint16_t port() const override {
        return port_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t frames_sent() const override {
        return frames_sent_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t frames_dropped() const override {
        return frames_dropped_.load(std::memory_order_relaxed);
    }

    void stop() override;

    [[nodiscard]] engine::Engine& engine() { return engine_; }

    // Engine completion thread.
    [[nodiscard]] Status on_frame(const engine::SpectrumFrame& frame);

    // Event loop thread.
    void add_subscription(std::shared_ptr<Subscription> subscription);
    void end_subscription(const std::shared_ptr<Subscription>& subscription);

    // Event loop thread. Queues a listing for the worker and hands back a
    // promise this loop resolves when the worker is done. See the note at the
    // top: this is the only engine-facing call that opens hardware, and the
    // loop must stay free while it runs.
    [[nodiscard]] kj::Promise<SourceListing> list_sources();

private:
    void serve(ServerOptions options);
    void announce(Status status);

    void run_listings();
    void close_listings();

    [[nodiscard]] kj::Promise<void> pump();
    void drain();
    void fan_out(const engine::SpectrumFrame& frame);
    void deliver(Subscription& subscription, const engine::SpectrumFrame& frame);
    void drop_cancelled();
    void refresh_subscriber_summary();

    void taskFailed(kj::Exception&&) override {
        // Every send already carries its own error handler, which ends the
        // subscription. Anything reaching here is not attributable to one
        // client, and tearing the loop down over it would take every other
        // client with it.
    }

    engine::Engine& engine_;
    std::shared_ptr<SinkGate> gate_;

    std::thread loop_;
    std::promise<Status> ready_;
    bool announced_ = false;  // loop thread only

    std::mutex stop_lock_;
    bool stopped_ = false;

    std::mutex sink_lock_;
    bool sink_installed_ = false;

    // Set by stop() under sink_lock_, which is the whole point of it. A
    // subscribeSpectrum waiting on that lock would otherwise install a sink
    // on the engine after stop() had taken one off, and stop() is past that
    // section by then, so nothing would ever remove the replacement.
    bool sink_closed_ = false;

    // The listing worker. Started on the first listSources rather than at
    // startup, because a server nobody asks never needs it, and joined by
    // stop() before the event loop is torn down.
    std::thread listings_thread_;
    std::mutex listing_lock_;
    std::condition_variable listing_wake_;
    std::deque<ListingFulfiller> listings_;
    bool listings_closed_ = false;

    std::atomic<std::uint16_t> port_{0};
    std::atomic<std::uint64_t> frames_sent_{0};
    std::atomic<std::uint64_t> frames_dropped_{0};

    // What the completion thread needs to know about the loop thread's
    // subscriptions without taking a lock to find it out.
    //
    // stride is the gcd of every subscription's everyNth. A frame whose
    // sequence it does not divide is one no subscription wanted, so the sink
    // can drop it before paying for the copy, which is the point of everyNth.
    // A frame it does divide may still be skipped per subscription later.
    std::atomic<std::uint32_t> subscribers_{0};
    std::atomic<std::uint32_t> stride_{1};

    // Set on the loop thread before create() returns and read afterwards. The
    // future handshake in start() is the happens-before for both.
    kj::Own<const kj::Executor> executor_;
    kj::Own<kj::CrossThreadPromiseFulfiller<void>> shutdown_;

    std::mutex frame_lock_;
    std::unique_ptr<FrameCopy> pending_;
    std::vector<std::unique_ptr<FrameCopy>> spare_;
    kj::Own<kj::CrossThreadPromiseFulfiller<void>> wakeup_;

    // Loop thread only.
    std::vector<std::shared_ptr<Subscription>> subscriptions_;
    kj::TaskSet* sends_ = nullptr;
};

class SubscriptionImpl final : public schema::SpectrumSubscription::Server {
public:
    SubscriptionImpl(ServerImpl& owner, std::shared_ptr<Subscription> node)
        : owner_(owner), node_(std::move(node)) {}

    SubscriptionImpl(const SubscriptionImpl&) = delete;
    SubscriptionImpl& operator=(const SubscriptionImpl&) = delete;

    // Dropping the capability ends the subscription, which is what the schema
    // promises and what a client that crashed relies on. Not an override:
    // capnp::Capability::Server has no virtual destructor, because kj::Own
    // disposes through the concrete type it was created with.
    ~SubscriptionImpl() { end(); }

    kj::Promise<void> cancel(CancelContext) override {
        end();
        return kj::READY_NOW;
    }

private:
    void end() {
        if (node_ == nullptr) {
            return;
        }
        owner_.end_subscription(node_);
        node_.reset();
    }

    ServerImpl& owner_;
    std::shared_ptr<Subscription> node_;
};

class SessionImpl final : public schema::Session::Server {
public:
    explicit SessionImpl(ServerImpl& owner) : owner_(owner) {}

    kj::Promise<void> info(InfoContext context) override {
        write_engine_info(context.getResults().initInfo(), owner_.engine().info());
        return kj::READY_NOW;
    }

    kj::Promise<void> running(RunningContext context) override {
        context.getResults().setRunning(owner_.engine().running());
        return kj::READY_NOW;
    }

    kj::Promise<void> listSources(ListSourcesContext context) override {
        // The listing is computed on the worker thread and written here. The
        // continuation runs on the loop thread, which is the only thread
        // allowed to touch this context. See the note at the top of the file
        // for why the enumeration itself is not allowed to run on it.
        return owner_.list_sources().then([context](SourceListing&& described) mutable {
            // A backend that could not be opened reports itself in
            // `unavailable` rather than failing the listing, and that has to
            // survive to the wire: a dongle held by another process must not
            // be able to hide the file and synthetic backends, which are what
            // somebody reaches for when the radio is busy.
            auto sources =
                context.getResults().initSources(static_cast<unsigned>(described.size()));
            for (unsigned i = 0; i < sources.size(); ++i) {
                write_source_descriptor(sources[i], described[i]);
            }
        });
    }

    kj::Promise<void> sourceStats(SourceStatsContext context) override {
        write_source_stats(context.getResults().initStats(), owner_.engine().source_stats());
        return kj::READY_NOW;
    }

    kj::Promise<void> addVrx(AddVrxContext context) override {
        auto params = read_vrx_params(context.getParams().getParams());
        if (!params) {
            return to_exception(params.error());
        }
        auto id = owner_.engine().add_vrx(*params);
        if (!id) {
            return to_exception(id.error());
        }
        context.getResults().setId(id->value);
        return kj::READY_NOW;
    }

    kj::Promise<void> removeVrx(RemoveVrxContext context) override {
        auto id = to_vrx_id(context.getParams().getId());
        if (!id) {
            return to_exception(id.error());
        }
        if (auto removed = owner_.engine().remove_vrx(*id); !removed) {
            return to_exception(removed.error());
        }
        return kj::READY_NOW;
    }

    kj::Promise<void> setVrxParams(SetVrxParamsContext context) override {
        auto request = context.getParams();
        auto id = to_vrx_id(request.getId());
        if (!id) {
            return to_exception(id.error());
        }
        auto params = read_vrx_params(request.getParams());
        if (!params) {
            return to_exception(params.error());
        }
        if (auto applied = owner_.engine().set_vrx_params(*id, *params); !applied) {
            return to_exception(applied.error());
        }
        return kj::READY_NOW;
    }

    kj::Promise<void> vrxStatus(VrxStatusContext context) override {
        auto id = to_vrx_id(context.getParams().getId());
        if (!id) {
            return to_exception(id.error());
        }
        auto status = owner_.engine().vrx_status(*id);
        if (!status) {
            return to_exception(status.error());
        }
        write_vrx_status(context.getResults().initStatus(), *status);
        return kj::READY_NOW;
    }

    kj::Promise<void> vrxIds(VrxIdsContext context) override {
        const auto ids = owner_.engine().vrx_ids();
        auto out = context.getResults().initIds(static_cast<unsigned>(ids.size()));
        for (unsigned i = 0; i < out.size(); ++i) {
            out.set(i, ids[i].value);
        }
        return kj::READY_NOW;
    }

    kj::Promise<void> subscribeSpectrum(SubscribeSpectrumContext context) override {
        auto request = context.getParams();
        if (!request.hasReceiver()) {
            return to_exception(Error{"subscribeSpectrum needs a receiver capability, and this "
                                      "request carried a null pointer in its place"});
        }

        // Refused with the engine's own words rather than by handing back a
        // subscription that can never produce a frame. An engine built with
        // EngineConfig::spectrum_transform at zero has no spectrum stage at
        // all, and that is the message the operator needs to see.
        if (auto ready = owner_.ensure_sink(); !ready) {
            return to_exception(ready.error());
        }

        auto node = std::make_shared<Subscription>(request.getReceiver(),
                                                   std::max(request.getEveryNth(), 1U));

        // The capability is built before the registry entry, so a throw on
        // the way to the client cannot leave a subscription nothing can end.
        schema::SpectrumSubscription::Client handle =
            kj::heap<SubscriptionImpl>(owner_, node);
        owner_.add_subscription(std::move(node));
        context.getResults().setSubscription(kj::mv(handle));
        return kj::READY_NOW;
    }

private:
    ServerImpl& owner_;
};

Status ServerImpl::start(const ServerOptions& options) {
    auto ready = ready_.get_future();
    try {
        loop_ = std::thread([this, options] { serve(options); });
    } catch (const std::exception& error) {
        return fail(std::format("the RPC server could not start its event loop thread: {}",
                                error.what()));
    }

    // The header requires port() to be valid the moment create() returns, so
    // the caller waits here for the bind rather than for the first connection.
    auto result = ready.get();
    if (!result) {
        loop_.join();
    }
    return result;
}

void ServerImpl::announce(Status status) {
    if (announced_) {
        return;
    }
    announced_ = true;
    ready_.set_value(std::move(status));
}

void ServerImpl::serve(ServerOptions options) {
    try {
        // Declared first so it is destroyed last. Everything below holds
        // promises or capabilities belonging to this loop, and kj aborts the
        // process if a cross-thread promise is still waiting when its event
        // loop goes, so the ordering here is not style.
        auto io = kj::setupAsyncIo();

        auto address = io.provider->getNetwork()
                           .parseAddress(options.bind_address.c_str(), options.port)
                           .wait(io.waitScope);
        auto listener = address->listen();

        // Ephemeral when ServerOptions::port was zero, which is what the test
        // suite needs and why this is read back rather than echoed.
        port_.store(static_cast<std::uint16_t>(listener->getPort()),
                    std::memory_order_release);

        // Kept for the life of the Server, not merely of this loop. A
        // cross-thread promise holds a bare reference to the executor of the
        // thread that created it, and the sink thread can be inside a fulfil
        // while this thread is on its way out.
        executor_ = kj::getCurrentThreadExecutor().addRef();

        auto shutdown = kj::newPromiseAndCrossThreadFulfiller<void>();
        shutdown_ = kj::mv(shutdown.fulfiller);

        capnp::TwoPartyServer rpc(kj::heap<SessionImpl>(*this));

        // After the RPC system so that outstanding sends are cancelled before
        // the system they were issued through is torn down.
        kj::TaskSet sends(*this);
        sends_ = &sends;

        try {
            auto serving = rpc.listen(*listener)
                               .exclusiveJoin(pump())
                               .exclusiveJoin(kj::mv(shutdown.promise));

            // Nothing between here and the wait may throw, or the caller is
            // told the server started and then it does not.
            announce(Status{});
            serving.wait(io.waitScope);
        } catch (const kj::Exception&) {
            // create() has already returned, so there is nobody to tell. A
            // client sees the connection close, which is what it sees for any
            // other end of this server.
        }

        // These capabilities were made on this thread and have to die on it,
        // ahead of the RPC system and the loop. Leaving them to ~ServerImpl
        // would destroy them on the caller's thread.
        sends.clear();
        sends_ = nullptr;
        subscriptions_.clear();
        refresh_subscriber_summary();
    } catch (const kj::Exception& error) {
        announce(fail(std::format("the RPC server could not bind {}:{}: {}",
                                  options.bind_address, options.port, describe(error))));
    } catch (const std::exception& error) {
        announce(fail(std::format("the RPC server could not bind {}:{}: {}",
                                  options.bind_address, options.port, error.what())));
    } catch (...) {
        announce(fail(std::format("the RPC server could not bind {}:{}", options.bind_address,
                                  options.port)));
    }

    // A no-op on every path that got as far as announcing. It is here for the
    // ones that did not, such as a throw while the joined promise was being
    // assembled: start() waiting forever for a future nobody sets is worse
    // than any message this can produce.
    announce(fail("the RPC server's event loop ended before it reported a port"));
}

Status ServerImpl::ensure_sink() {
    std::scoped_lock held(sink_lock_);

    // Checked under the same lock stop() sets it under. A subscribeSpectrum
    // that was waiting here while stop() removed the sink would otherwise put
    // a fresh one back on the engine with nothing left to take it off, and
    // server.h promises the server removes its sink on destruction.
    if (sink_closed_) {
        return fail("this RPC server is stopping, so it will not install a spectrum sink: "
                    "nothing would remove it from the engine afterwards");
    }
    if (sink_installed_) {
        return {};
    }

    auto installed = engine_.set_spectrum_sink(
        [gate = gate_](const engine::SpectrumFrame& frame) -> Status {
            std::scoped_lock owned(gate->lock);
            return gate->owner == nullptr ? Status{} : gate->owner->on_frame(frame);
        });
    if (!installed) {
        return installed;
    }

    sink_installed_ = true;
    return {};
}

Status ServerImpl::on_frame(const engine::SpectrumFrame& frame) {
    // The engine's completion thread. It touches no capability and it never
    // waits on the loop thread, which is serving every other client and owes
    // this thread nothing.
    const auto subscribers = subscribers_.load(std::memory_order_relaxed);
    if (subscribers == 0) {
        return {};
    }

    const auto stride = stride_.load(std::memory_order_relaxed);
    if (stride > 1 && (frame.sequence % stride) != 0) {
        return {};
    }

    // Taken from the pool, and filled outside the lock. In the steady state
    // the buffer is one the loop thread finished with, so the bins are copied
    // into storage that already exists.
    std::unique_ptr<FrameCopy> buffer;
    {
        std::scoped_lock held(frame_lock_);
        if (!spare_.empty()) {
            buffer = std::move(spare_.back());
            spare_.pop_back();
        }
    }
    if (buffer == nullptr) {
        buffer = std::make_unique<FrameCopy>();
    }
    buffer->assign(frame);

    kj::Own<kj::CrossThreadPromiseFulfiller<void>> waker;
    {
        std::scoped_lock held(frame_lock_);
        if (pending_ != nullptr) {
            // The loop thread has not drained the previous frame, so the
            // whole fan-out is behind. Newest wins: for a waterfall the
            // freshest row is the one worth drawing, and queueing would put
            // the engine's whole frame rate behind a client that stalled.
            // server.h does that arithmetic.
            //
            // Charged once per live subscription, which is the count the loop
            // thread last published. It over-counts only when subscriptions
            // ask for different rates, since the everyNth filter that ran
            // above was the gcd of all of them rather than each one's own.
            frames_dropped_.fetch_add(subscribers, std::memory_order_relaxed);

            // Recycled, not released. Assigning over it would run the bins'
            // deallocation inside the lock the loop thread is waiting on.
            spare_.push_back(std::move(pending_));
        }
        pending_ = std::move(buffer);

        // Taken rather than borrowed, so a burst of frames rings the bell
        // once. The loop thread arms a fresh one each time round.
        waker = kj::mv(wakeup_);
    }

    if (waker.get() != nullptr) {
        waker->fulfill();
    }
    return {};
}

kj::Promise<void> ServerImpl::pump() {
    auto armed = kj::newPromiseAndCrossThreadFulfiller<void>();
    {
        std::scoped_lock held(frame_lock_);
        wakeup_ = kj::mv(armed.fulfiller);
    }

    // Arm, then drain, then wait. A frame handed over while no fulfiller was
    // armed sets the slot and rings nothing, so draining before arming would
    // leave it sitting there until the frame behind it arrived.
    drain();

    return armed.promise.then([this]() { return pump(); });
}

void ServerImpl::drain() {
    std::unique_ptr<FrameCopy> frame;
    {
        std::scoped_lock held(frame_lock_);
        frame = std::exchange(pending_, nullptr);
    }
    if (frame == nullptr) {
        return;
    }

    fan_out(frame->frame());

    // Returned rather than dropped, so the completion thread's next copy has
    // storage waiting for it. Safe here because deliver() writes the bins
    // into the outgoing message before it issues the send, so nothing in
    // flight still refers to this buffer.
    std::scoped_lock held(frame_lock_);
    spare_.push_back(std::move(frame));
}

void ServerImpl::fan_out(const engine::SpectrumFrame& frame) {
    bool any_cancelled = false;
    for (const auto& subscription : subscriptions_) {
        if (subscription->cancelled) {
            any_cancelled = true;
            continue;
        }
        if (subscription->every_nth > 1 && (frame.sequence % subscription->every_nth) != 0) {
            // What this subscription asked for, so not a drop.
            continue;
        }
        if (subscription->in_flight) {
            frames_dropped_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        deliver(*subscription, frame);
    }

    if (any_cancelled) {
        drop_cancelled();
    }
}

void ServerImpl::deliver(Subscription& subscription, const engine::SpectrumFrame& frame) {
    // Sized up front so the frame lands in one segment. Left to discover the
    // list length itself, capnp grows the message a segment at a time and the
    // list is very nearly the whole message.
    const std::uint64_t words = (frame.power_db.size() + 1) / 2 + 32;
    auto request = subscription.receiver.frameRequest(capnp::MessageSize{words, 0});
    write_spectrum_frame(request.initFrame(), frame);

    subscription.in_flight = true;
    frames_sent_.fetch_add(1, std::memory_order_relaxed);

    auto node = subscription.weak_from_this();
    sends_->add(request.send().ignoreResult().then(
        [node]() {
            if (auto live = node.lock()) {
                live->in_flight = false;
            }
        },
        [node](kj::Exception&&) {
            // A receiver that threw or went away is not coming back, so the
            // subscription ends here rather than being retried every frame
            // for as long as the engine runs.
            if (auto live = node.lock()) {
                live->in_flight = false;
                live->cancelled = true;
            }
        }));
}

void ServerImpl::add_subscription(std::shared_ptr<Subscription> subscription) {
    subscriptions_.push_back(std::move(subscription));
    refresh_subscriber_summary();
}

void ServerImpl::end_subscription(const std::shared_ptr<Subscription>& subscription) {
    subscription->cancelled = true;
    std::erase(subscriptions_, subscription);
    refresh_subscriber_summary();
}

void ServerImpl::drop_cancelled() {
    std::erase_if(subscriptions_, [](const std::shared_ptr<Subscription>& subscription) {
        return subscription->cancelled;
    });
    refresh_subscriber_summary();
}

void ServerImpl::refresh_subscriber_summary() {
    std::uint32_t count = 0;
    std::uint32_t stride = 0;
    for (const auto& subscription : subscriptions_) {
        if (subscription->cancelled) {
            continue;
        }
        ++count;
        stride = stride == 0 ? subscription->every_nth
                             : std::gcd(stride, subscription->every_nth);
    }

    subscribers_.store(count, std::memory_order_relaxed);
    stride_.store(stride == 0 ? 1U : stride, std::memory_order_relaxed);
}

kj::Promise<SourceListing> ServerImpl::list_sources() {
    // Created on the loop thread on purpose: kj ties the promise to the
    // calling thread's event loop and only the fulfiller may cross. The
    // continuation in SessionImpl::listSources therefore runs here, which is
    // the only thread allowed to write the call's results.
    auto listing = kj::newPromiseAndCrossThreadFulfiller<SourceListing>();

    {
        std::scoped_lock held(listing_lock_);
        if (listings_closed_) {
            return to_exception(Error{"this RPC server is stopping and will not open a device "
                                      "to enumerate sources"});
        }

        if (!listings_thread_.joinable()) {
            try {
                listings_thread_ = std::thread([this] { run_listings(); });
            } catch (const std::exception& error) {
                return to_exception(
                    Error{std::format("the RPC server could not start the thread that "
                                      "enumerates sources: {}",
                                      error.what())});
            }
        }

        listings_.push_back(kj::mv(listing.fulfiller));
    }

    listing_wake_.notify_one();
    return kj::mv(listing.promise);
}

void ServerImpl::run_listings() {
    for (;;) {
        ListingFulfiller job;
        {
            std::unique_lock held(listing_lock_);
            listing_wake_.wait(held,
                               [this] { return listings_closed_ || !listings_.empty(); });
            if (listings_closed_) {
                break;
            }
            job = kj::mv(listings_.front());
            listings_.pop_front();
        }

        // The device open, off the loop thread, one at a time. Serialised
        // because two concurrent opens of one dongle would have each report
        // the other's process as holding it.
        auto described = source::describe_sources();
        if (described) {
            job->fulfill(std::move(*described));
        } else {
            job->reject(to_exception(described.error()));
        }
    }

    // Whatever was still queued when stop() came through. Rejected rather
    // than dropped: dropping the fulfiller resolves the promise too, but with
    // kj's own "fulfiller destroyed" text, which tells the client nothing
    // about what happened to the server.
    std::deque<ListingFulfiller> abandoned;
    {
        std::scoped_lock held(listing_lock_);
        abandoned.swap(listings_);
    }
    for (auto& job : abandoned) {
        job->reject(to_exception(Error{"the RPC server stopped before this listing ran"}));
    }
}

void ServerImpl::close_listings() {
    {
        std::scoped_lock held(listing_lock_);
        listings_closed_ = true;
    }
    listing_wake_.notify_all();

    if (listings_thread_.joinable()) {
        // Bounded by one source::describe_sources() already in progress,
        // which is a device open. That cost has not gone anywhere; it is now
        // paid by whoever stops the server rather than by every client on the
        // socket.
        listings_thread_.join();
    }
}

void ServerImpl::stop() {
    // A mutex rather than an exchanged flag, because the second caller has to
    // wait for the first to finish rather than race the destructor that
    // follows it.
    std::scoped_lock stopping(stop_lock_);
    if (stopped_) {
        return;
    }
    stopped_ = true;

    // First, and before the loop is told to end. A listing's result comes
    // back through a fulfiller that arms this loop, so the thread that can
    // fire one has to be joined while the loop is still there to be armed.
    close_listings();

    {
        std::scoped_lock held(sink_lock_);

        // Set under the lock that installs a sink, which is the whole point
        // of it. A subscribeSpectrum waiting here would otherwise put a fresh
        // sink on the engine after the removal below, and stop() is past this
        // section by then, so nothing would ever take the replacement off.
        sink_closed_ = true;

        if (sink_installed_) {
            // Asynchronous, so this does not stop delivery; it only stops the
            // engine reaching for the sink on dispatches recorded after it.
            // The gate below is what actually ends the calls.
            static_cast<void>(engine_.set_spectrum_sink({}));
            sink_installed_ = false;
        }
    }

    if (shutdown_.get() != nullptr) {
        shutdown_->fulfill();
    }
    if (loop_.joinable()) {
        loop_.join();
    }

    // Only now, with the loop joined, can this wait for a sink call that is
    // already running: a sink call blocked on the loop thread would deadlock
    // against a stop() that had not yet released it. After this the callable
    // is inert whether or not the engine still holds it.
    {
        std::scoped_lock owned(gate_->lock);
        gate_->owner = nullptr;
    }
    {
        std::scoped_lock held(frame_lock_);
        pending_.reset();
        spare_.clear();
    }

    release_engine(engine_);
}

}  // namespace

Expected<std::unique_ptr<Server>> Server::create(engine::Engine& engine,
                                                 const ServerOptions& options) {
    if (auto claim = claim_engine(engine); !claim) {
        return std::unexpected(claim.error());
    }

    // The claim is released by ~ServerImpl through stop(), including on the
    // failure below, so a bind that lost a race to another process does not
    // leave the engine looking taken.
    auto server = std::make_unique<ServerImpl>(engine);
    if (auto started = server->start(options); !started) {
        return std::unexpected(started.error());
    }

    // Installed here and not lazily, because server.h says the server owns one
    // sink for its whole life and fans out from it. A refusal is kept rather
    // than fatal: an engine with no spectrum stage, or one whose source is not
    // open yet, still serves info, the receivers and the counters, and
    // subscribeSpectrum retries the install and reports the engine's reason if
    // it still cannot.
    static_cast<void>(server->ensure_sink());

    return std::unique_ptr<Server>(std::move(server));
}

}  // namespace revenant::rpc
