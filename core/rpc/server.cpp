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
//
// THE DETECTOR LIVES HERE, NOT IN THE ENGINE, AND IT IS BUILT ON DEMAND
//
// core/detect/detector.h works on spectrum frames and nothing else, and this
// server already receives every one of them. There is no detector on
// engine::Engine and no Engine method that returns tracks, which
// docs/detection.md lists as one of the things click-to-tune is waiting for;
// tools/cli/main.cpp solves it by owning a Detector beside its own spectrum
// sink, and this is the same arrangement one process further out.
//
// It is built at the first detections or setDetectionThreshold call rather
// than at startup, the same way the listing thread is started at the first
// listSources. A detector is not free: core/detect/detector.h budgets its
// per-frame accumulation and its ten-a-second decision against 65536 bins,
// and a headless engine feeding a recorder should not pay either of them for
// a track list nobody has asked to see.
//
// WHAT THE LAZY BUILD DOES NOT DO IS GIVE THE COST BACK
//
// That saving lasts until the first client asks and no longer. detecting_ is
// set once in ensure_detector, and short of stop() the only thing that clears
// it is a consume() that refuses a frame, so from that first call the
// completion thread runs accumulation on every frame and a decision ten times
// a second for the life of the Server, whether or not anyone is still reading
// the list. The saving is for a server nobody has ever asked, not for one that
// is idle now. Measured on 2026-09-19 with revenant-cli over this project's
// 8192-bin test geometry, the pair cost 0.201 ms per frame, which the same run
// put at 0.7% of one core at that frame rate; detector.h has the figures at
// the shipped 65536.
//
// Deliberate, because the alternative resets state that is not the
// disconnecting client's to reset. detections is a poll rather than a
// subscription, so there is nothing to count: a client that asked once is
// indistinguishable from one polling ten times a second, and a dropped
// connection says nothing about whether detection is still wanted. Tearing
// the detector down at the last of them would throw away two things nobody
// asked to lose. Track ids are issued from one and never reused, which is what
// lets an id in a log name one signal for the life of the process, and a
// rebuilt detector numbers the same signals from one again. The detection
// threshold is engine-wide and the operator's, per docs/detection.md, and a
// rebuild silently puts it back to the default. A display that reconnects,
// which is the ordinary case rather than the exception, would do both.
//
// ONE LOCK AROUND THE WHOLE DETECTOR, AND WHY THAT IS THE CHEAP ANSWER
//
// The detector is written on the engine's completion thread, inside
// on_frame, and read on the event loop thread, inside a call. Its own header
// says one thread owns a Detector and there is no lock inside it, so the lock
// has to be here.
//
// It covers the whole of consume(), which is the expensive half, and that
// looks like the wrong shape until the other side is counted. The loop thread
// holds it only to copy a bounded vector of tracks out, or to set one double;
// neither allocates beyond that copy and neither does I/O. So the completion
// thread waits for a memcpy and the loop thread waits for at most one
// consume(). The alternatives are worse in ways that matter: a deferred
// threshold applied on the completion thread cannot report that the value was
// out of range, and a published snapshot copied per decision pays a copy on
// the sample path for every decision whether or not anyone polls.
//
// Nothing takes sink_lock_ while holding detect_lock_, which is what keeps
// ensure_detector's two locks from closing a cycle against stop().
//
// A DETECTOR FAILURE MUST NOT TAKE THE ENGINE DOWN
//
// The spectrum sink's Status is the engine's: core/engine/graph.cpp turns a
// failing sink into a failing dispatch, which ends the run. So a consume()
// that refuses a frame is recorded, detection is switched off, and the sink
// still returns success. The next detections call reports what happened. The
// only way consume() can refuse is a geometry that is not the one the
// detector was built against, which would mean two engines rather than one,
// and losing the track list is the right price for not losing the radio.

#include "core/rpc/server.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
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

#include "core/detect/detector.h"
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

// The same for one receiver's passband. A separate class and not a template
// over the two frame types, because the two structs share no base and the
// saving would be one small copy each.
class PassbandCopy {
public:
    PassbandCopy() = default;

    PassbandCopy(const PassbandCopy&) = delete;
    PassbandCopy& operator=(const PassbandCopy&) = delete;

    void assign(const engine::PassbandFrame& source) {
        bins_.assign(source.power_db.begin(), source.power_db.end());
        frame_ = source;
        frame_.power_db = std::span<const float>(bins_);
    }

    [[nodiscard]] const engine::PassbandFrame& frame() const { return frame_; }

private:
    std::vector<float> bins_;
    engine::PassbandFrame frame_;
};

// The most frame buffers that can exist at once, and so the pool's capacity:
// one being filled on the completion thread, one waiting in the slot, one
// being fanned out on the loop thread. core/engine/scheduler.h has exactly
// one completion thread, so no two frames are ever mid-copy.
constexpr std::size_t kFrameBuffers = 3;

// What listSources answers with, and how the worker thread hands one back.
using SourceListing = std::vector<source::SourceCapabilities>;
using ListingFulfiller = kj::Own<kj::CrossThreadPromiseFulfiller<SourceListing>>;

// The bounds this file checks a threshold against before it reaches the
// detector, which checks its own as well.
//
// Duplicated on purpose and duplicated NARROWER. Detector::set_thresholds
// accepts -200 to 200 dB, which are the bounds of the arithmetic rather than
// of anything an operator means; tools/cli/main.cpp refuses outside -60 to
// 120 on the command line and this is the same interval said to a GUI. The
// wider check downstream still runs, so a value this accepts and the detector
// refuses is a mistake in this file rather than a client's, and the message
// will say so.
constexpr double kMinDetectionThresholdDb = -60.0;
constexpr double kMaxDetectionThresholdDb = 120.0;

// What one detections call takes off the detector, on the loop thread, under
// detect_lock_.
//
// The tracks are copied rather than spanned. Detector::tracks() is valid only
// until the next consume(), which the completion thread may reach the instant
// the lock is released, and the capnp message is written after that.
struct DetectionSnapshot {
    std::vector<detect::Track> tracks;
    std::uint64_t decisions = 0;
    std::uint64_t last_decision = 0;
    std::uint32_t total = 0;
    double threshold_db = 0.0;

    // Seconds of source time a track survives with no evidence, which a
    // display has to have and cannot derive: it is the detector's
    // configuration and the wire is the only place a client can read it.
    double hold_seconds = 0.0;
};

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

// One subscriber to one receiver's passband. Loop thread only, same as
// Subscription above and for the same reasons.
struct PassbandNode : std::enable_shared_from_this<PassbandNode> {
    PassbandNode(schema::PassbandReceiver::Client client, engine::VrxId which,
                 std::uint32_t nth)
        : receiver(kj::mv(client)), vrx(which), every_nth(nth) {}

    schema::PassbandReceiver::Client receiver;
    engine::VrxId vrx;
    std::uint32_t every_nth = 1;
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

    // Engine completion thread, for one receiver.
    [[nodiscard]] Status on_passband_frame(const engine::PassbandFrame& frame);

    // Event loop thread.
    void add_subscription(std::shared_ptr<Subscription> subscription);
    void end_subscription(const std::shared_ptr<Subscription>& subscription);

    // Event loop thread. Installs the engine's per-receiver passband sink on
    // the first subscriber to that receiver and takes it off with the last,
    // which is what makes a transform nobody is watching cost nothing.
    [[nodiscard]] Status add_passband(std::shared_ptr<PassbandNode> node);
    void end_passband(const std::shared_ptr<PassbandNode>& node);

    // Event loop thread. Queues a listing for the worker and hands back a
    // promise this loop resolves when the worker is done. See the note at the
    // top: this is the only engine-facing call that opens hardware, and the
    // loop must stay free while it runs.
    [[nodiscard]] kj::Promise<SourceListing> list_sources();

    // Event loop thread, all three. See the detector notes at the top of the
    // file for why the object is built on demand and locked as a whole.
    [[nodiscard]] Status ensure_detector();
    [[nodiscard]] Expected<DetectionSnapshot> detections(double min_confidence);
    [[nodiscard]] Status set_detection_threshold(double threshold_db);

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

    void drain_passbands();
    void fan_out_passband(const engine::PassbandFrame& frame);
    void deliver_passband(PassbandNode& node, const engine::PassbandFrame& frame);
    void detach_passband_sink(engine::VrxId vrx);

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

    // The wideband detector, and everything about it.
    //
    // detecting_ is the completion thread's cheap way to skip the lock
    // entirely on a server nobody has asked for detections. It is set once,
    // after the detector exists, and cleared once: by a consume() that
    // refuses a frame, or by stop() with both threads that could be inside
    // the detector already gone. So a relaxed load is ordering enough, and
    // the worst a stale read does is feed or skip one frame at the edge.
    //
    // Nothing else clears it. A client disconnecting does not, which is the
    // decision the note at the top of this file argues.
    std::mutex detect_lock_;
    std::atomic<bool> detecting_{false};
    std::optional<detect::Detector> detector_;

    // Why detection stopped, empty while it has not. See the note at the top:
    // a detector that refuses a frame must not fail the sink, so the reason
    // is kept here and reported to the next caller instead.
    std::string detector_fault_;  // detect_lock_

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

    // One pending slot per receiver, under frame_lock_ and woken by the same
    // fulfiller, so a burst across several receivers rings the bell once and
    // pump() drains all of them.
    //
    // Per receiver and not one shared slot: two receivers produce frames on
    // the same completion thread one after the other, and a single slot would
    // make each one throw the other away rather than the previous frame of
    // its own. Newest wins WITHIN a receiver, which is the waterfall rule,
    // and never between receivers.
    std::map<std::uint32_t, std::unique_ptr<PassbandCopy>> passband_pending_;
    std::vector<std::unique_ptr<PassbandCopy>> passband_spare_;

    // Receivers this server has a passband sink installed on, so the last
    // subscriber leaving can take it off again and free the device buffers.
    // Loop thread only.
    std::map<std::uint32_t, std::uint32_t> passband_sinks_;

    // Loop thread only.
    std::vector<std::shared_ptr<Subscription>> subscriptions_;
    std::vector<std::shared_ptr<PassbandNode>> passband_nodes_;
    kj::TaskSet* sends_ = nullptr;
};

class PassbandSubscriptionImpl final : public schema::PassbandSubscription::Server {
public:
    PassbandSubscriptionImpl(ServerImpl& owner, std::shared_ptr<PassbandNode> node)
        : owner_(owner), node_(std::move(node)) {}

    PassbandSubscriptionImpl(const PassbandSubscriptionImpl&) = delete;
    PassbandSubscriptionImpl& operator=(const PassbandSubscriptionImpl&) = delete;

    // Not an override, for the reason SubscriptionImpl's destructor gives:
    // capnp::Capability::Server has no virtual destructor.
    ~PassbandSubscriptionImpl() { end(); }

    kj::Promise<void> cancel(CancelContext) override {
        end();
        return kj::READY_NOW;
    }

private:
    void end() {
        if (node_ == nullptr) {
            return;
        }
        owner_.end_passband(node_);
        node_.reset();
    }

    ServerImpl& owner_;
    std::shared_ptr<PassbandNode> node_;
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

    kj::Promise<void> subscribePassband(SubscribePassbandContext context) override {
        auto request = context.getParams();
        if (!request.hasReceiver()) {
            return to_exception(Error{"subscribePassband needs a receiver capability, and "
                                      "this request carried a null pointer in its place"});
        }

        auto id = to_vrx_id(request.getVrx());
        if (!id) {
            return to_exception(id.error());
        }

        auto node = std::make_shared<PassbandNode>(request.getReceiver(), *id,
                                                   std::max(request.getEveryNth(), 1U));

        // The sink goes on before the capability exists, so a refusal from
        // the engine (no passband stage, a raw tap, no such receiver) comes
        // back as the engine's own sentence rather than as a subscription
        // that can never produce a frame.
        if (auto installed = owner_.add_passband(node); !installed) {
            return to_exception(installed.error());
        }

        schema::PassbandSubscription::Client handle =
            kj::heap<PassbandSubscriptionImpl>(owner_, std::move(node));
        context.getResults().setSubscription(kj::mv(handle));
        return kj::READY_NOW;
    }

    kj::Promise<void> detections(DetectionsContext context) override {
        const double bar = context.getParams().getMinConfidence();

        // Refused rather than answered with an empty list, because an empty
        // list is also what a quiet band looks like and the caller would have
        // no way to tell. The schema says why one is unreachable.
        if (!std::isfinite(bar) || bar < 0.0 || bar >= 1.0) {
            return to_exception(Error{std::format(
                "minConfidence was {}, and it has to be from 0 up to but not including 1. A "
                "track's confidence approaches 1 without ever reaching it, so a bar of 1 lists "
                "nothing however strong the signal is",
                bar)});
        }

        auto taken = owner_.detections(bar);
        if (!taken) {
            return to_exception(taken.error());
        }

        auto out = context.getResults().initDetections();
        auto rows = out.initDetections(static_cast<unsigned>(taken->tracks.size()));
        for (unsigned i = 0; i < rows.size(); ++i) {
            write_detection(rows[i], taken->tracks[i]);
        }
        out.setDecisions(taken->decisions);
        out.setLastDecision(taken->last_decision);
        out.setTotal(taken->total);
        out.setDetectionThresholdDb(taken->threshold_db);
        out.setDetectorHoldSeconds(taken->hold_seconds);
        return kj::READY_NOW;
    }

    kj::Promise<void> setDetectionThreshold(SetDetectionThresholdContext context) override {
        if (auto applied = owner_.set_detection_threshold(
                context.getParams().getThresholdDb());
            !applied) {
            return to_exception(applied.error());
        }
        return kj::READY_NOW;
    }

private:
    ServerImpl& owner_;
};

// The bootstrap capability, and the only thing an unauthenticated connection
// holds.
//
// ONE OF THESE IS SHARED BY EVERY CONNECTION, WHICH IS SAFE BECAUSE IT HAS NO
// STATE
//
// capnp 1.4.0's TwoPartyServer takes exactly one Capability::Client and has no
// BootstrapFactory overload; the factory exists on RpcSystem alone. Getting a
// bootstrap per connection would mean replacing rpc.listen with a hand-written
// accept loop inside serve(), which is already the hardest function in this
// file. It is not needed: this object holds a reference and 32 bytes, both set
// on the loop thread before the listener exists, so login needs no lock and
// adds no thread.
//
// LOGIN MINTS A FRESH SESSION PER CALL
//
// SessionImpl's only member is ServerImpl&, so a second one costs nothing and
// two of them cannot disagree. That is what makes a second login on one
// connection an ordinary success rather than a case to defend against: the
// caller has already proved it holds the token, and a second capability grants
// it nothing it did not have. The Session's lifetime becomes the client's, and
// any still alive at shutdown die with the TwoPartyServer local, on this
// thread, exactly as the single bootstrap Session used to.
class AuthenticatorImpl final : public schema::Authenticator::Server {
public:
    AuthenticatorImpl(ServerImpl& owner, Token token) : owner_(owner), token_(token) {}

    kj::Promise<void> login(LoginContext context) override {
        const capnp::Data::Reader offered = context.getParams().getToken();
        const std::span<const std::uint8_t> bytes(
            static_cast<const std::uint8_t*>(offered.begin()), offered.size());

        if (!tokens_equal(bytes, std::span<const std::uint8_t>(token_))) {
            // The rejection says nothing about the token, and not because the
            // length is a secret: it is in the schema, in docs/rpc.md and in
            // the size of the file. There is simply nothing else true to say,
            // and "too short" would invite a caller to treat the length as
            // the thing to get right.
            return to_exception(Error{
                "login was refused: that is not this engine's token. The engine keeps it in "
                "the file it named at startup; pass those bytes, or --token-file, to the "
                "client"});
        }

        context.getResults().setSession(kj::heap<SessionImpl>(owner_));
        return kj::READY_NOW;
    }

private:
    ServerImpl& owner_;
    Token token_;
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

        // The bootstrap is an Authenticator, not a Session. Server::create
        // has already refused a token that is not exactly kTokenBytes, so
        // this copy cannot be short, and it is a copy because `options` is a
        // by-value parameter whose lifetime ends with this function while the
        // capability's does not.
        Token token{};
        std::copy_n(options.token.begin(), kTokenBytes, token.begin());
        capnp::TwoPartyServer rpc(kj::heap<AuthenticatorImpl>(*this, token));

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

Status ServerImpl::ensure_detector() {
    // The sink first, and its error rather than the detector's. An engine
    // built with no spectrum stage refuses here in its own words, which names
    // the config field that has to change; a detector built against an empty
    // geometry would refuse in terms of bin counts instead.
    if (auto ready = ensure_sink(); !ready) {
        return ready;
    }

    std::scoped_lock held(detect_lock_);
    if (!detector_fault_.empty()) {
        return fail(detector_fault_);
    }
    if (detector_.has_value()) {
        return {};
    }

    const engine::EngineInfo& info = engine_.info();

    detect::DetectorConfig config;
    config.source_rate = info.source_rate;
    config.source_center = info.source_center;
    config.grid_channels = info.grid.channels;

    // Left at its default, and inert. Nothing inside the detector drops a
    // track for failing it, and every caller of Session::detections passes
    // its own bar, which is what docs/detection.md means by the confidence
    // threshold belonging to the display. Setting it from the first caller
    // would make one client's preference the server's.
    config.confidence_threshold = detect::DetectorConfig{}.confidence_threshold;

    auto made = detect::Detector::create(config, info.spectrum);
    if (!made) {
        return std::unexpected(with_context(made.error(), "building the wideband detector"));
    }
    detector_ = std::move(*made);

    // Published last, so the completion thread cannot find a detector that is
    // still being constructed.
    detecting_.store(true, std::memory_order_relaxed);
    return {};
}

Expected<DetectionSnapshot> ServerImpl::detections(double min_confidence) {
    if (auto ready = ensure_detector(); !ready) {
        return std::unexpected(ready.error());
    }

    std::scoped_lock held(detect_lock_);
    if (!detector_fault_.empty()) {
        return fail(detector_fault_);
    }
    if (!detector_.has_value()) {
        return fail("the wideband detector is not running");
    }

    const std::span<const detect::Track> tracks = detector_->tracks();

    DetectionSnapshot out;
    out.decisions = detector_->stats().decisions;
    out.last_decision = detector_->last_decision();
    out.total = static_cast<std::uint32_t>(tracks.size());
    out.threshold_db = detector_->config().detection_threshold_db;

    // Read off the live config beside the threshold rather than off
    // DetectorConfig{}, so a detector built with a hold other than the
    // default reports the one it is running. ensure_detector above builds it
    // from defaults today, and a client that assumed that would be wrong the
    // first time it stops being true.
    out.hold_seconds = detector_->config().bootstrap_hold_seconds;

    // Filtered here rather than on the client so that a busy band does not
    // put five hundred rows on the wire for a display that asked for the
    // handful above its bar. tracks() is already ascending in frequency and a
    // filter preserves that.
    out.tracks.reserve(tracks.size());
    for (const detect::Track& track : tracks) {
        if (track.confidence >= min_confidence) {
            out.tracks.push_back(track);
        }
    }
    return out;
}

Status ServerImpl::set_detection_threshold(double threshold_db) {
    if (!std::isfinite(threshold_db) || threshold_db < kMinDetectionThresholdDb ||
        threshold_db > kMaxDetectionThresholdDb) {
        return fail(std::format(
            "the detection threshold was {} and it has to be between {} and {} dB of SNR in "
            "the 2500 Hz reference bandwidth",
            threshold_db, kMinDetectionThresholdDb, kMaxDetectionThresholdDb));
    }

    if (auto ready = ensure_detector(); !ready) {
        return ready;
    }

    std::scoped_lock held(detect_lock_);
    if (!detector_fault_.empty()) {
        return fail(detector_fault_);
    }
    if (!detector_.has_value()) {
        return fail("the wideband detector is not running");
    }

    // The confidence threshold is passed back unchanged. set_thresholds takes
    // both because the detector validates them as a pair, and this call is
    // about the other one.
    return detector_->set_thresholds(threshold_db, detector_->config().confidence_threshold);
}

Status ServerImpl::on_frame(const engine::SpectrumFrame& frame) {
    // The engine's completion thread. It touches no capability and it never
    // waits on the loop thread, which is serving every other client and owes
    // this thread nothing.

    // Before every early return below, because the detector integrates over
    // about a second and a frame skipped here is energy it never sees. What
    // the subscribers wanted and what the detector needs are different
    // questions: a client can ask for every tenth frame and still expect the
    // track list to be built out of all of them.
    if (detecting_.load(std::memory_order_relaxed)) {
        std::scoped_lock held(detect_lock_);
        if (detector_.has_value()) {
            if (auto fed = detector_->consume(frame); !fed) {
                // Recorded and switched off rather than returned. See the
                // note at the top: this Status is the engine's, and failing
                // it here would end the run over a track list.
                detecting_.store(false, std::memory_order_relaxed);
                detector_fault_ = fed.error().message;
            }
        }
    }

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
    //
    // Both kinds through one pump and one bell. A receiver's passband and the
    // span are produced by the same completion thread in the same dispatch,
    // so two pumps would be two wakeups for one batch of work.
    drain();
    drain_passbands();

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

Status ServerImpl::on_passband_frame(const engine::PassbandFrame& frame) {
    // The engine's completion thread, the same one on_frame runs on. It
    // touches no capability and never waits on the loop thread.
    const std::uint32_t key = frame.vrx.value;

    std::unique_ptr<PassbandCopy> buffer;
    {
        std::scoped_lock held(frame_lock_);
        if (!passband_spare_.empty()) {
            buffer = std::move(passband_spare_.back());
            passband_spare_.pop_back();
        }
    }
    if (buffer == nullptr) {
        buffer = std::make_unique<PassbandCopy>();
    }
    buffer->assign(frame);

    kj::Own<kj::CrossThreadPromiseFulfiller<void>> waker;
    {
        std::scoped_lock held(frame_lock_);
        auto slot = passband_pending_.find(key);
        if (slot != passband_pending_.end() && slot->second != nullptr) {
            // Newest wins, within this receiver only. Charged as a drop for
            // the same reason the span's is: the client asked for a rate and
            // is not getting it.
            frames_dropped_.fetch_add(1, std::memory_order_relaxed);
            passband_spare_.push_back(std::move(slot->second));
        }
        passband_pending_[key] = std::move(buffer);
        waker = kj::mv(wakeup_);
    }

    if (waker.get() != nullptr) {
        waker->fulfill();
    }
    return {};
}

void ServerImpl::drain_passbands() {
    // Moved out whole, so the loop thread fans out without holding the lock
    // the completion thread wants for its next copy.
    std::map<std::uint32_t, std::unique_ptr<PassbandCopy>> ready;
    {
        std::scoped_lock held(frame_lock_);
        ready.swap(passband_pending_);
    }
    if (ready.empty()) {
        return;
    }

    for (auto& entry : ready) {
        if (entry.second != nullptr) {
            fan_out_passband(entry.second->frame());
        }
    }

    // Returned rather than dropped, so the completion thread's next copy has
    // storage waiting for it. Safe here because deliver_passband writes the
    // bins into the outgoing message before it issues the send.
    std::scoped_lock held(frame_lock_);
    for (auto& entry : ready) {
        if (entry.second != nullptr) {
            passband_spare_.push_back(std::move(entry.second));
        }
    }
}

void ServerImpl::fan_out_passband(const engine::PassbandFrame& frame) {
    bool any_cancelled = false;
    for (const auto& node : passband_nodes_) {
        if (node->cancelled) {
            any_cancelled = true;
            continue;
        }
        if (node->vrx != frame.vrx) {
            continue;
        }
        if (node->every_nth > 1 && (frame.sequence % node->every_nth) != 0) {
            // What this subscription asked for, so not a drop.
            continue;
        }
        if (node->in_flight) {
            frames_dropped_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        deliver_passband(*node, frame);
    }

    if (any_cancelled) {
        std::vector<std::shared_ptr<PassbandNode>> gone;
        for (const auto& node : passband_nodes_) {
            if (node->cancelled) {
                gone.push_back(node);
            }
        }
        for (const auto& node : gone) {
            end_passband(node);
        }
    }
}

void ServerImpl::deliver_passband(PassbandNode& node, const engine::PassbandFrame& frame) {
    const std::uint64_t words = (frame.power_db.size() + 1) / 2 + 32;
    auto request = node.receiver.frameRequest(capnp::MessageSize{words, 0});
    write_passband_frame(request.initFrame(), frame);

    node.in_flight = true;
    frames_sent_.fetch_add(1, std::memory_order_relaxed);

    auto weak = node.weak_from_this();
    sends_->add(request.send().ignoreResult().then(
        [weak]() {
            if (auto live = weak.lock()) {
                live->in_flight = false;
            }
        },
        [weak](kj::Exception&&) {
            if (auto live = weak.lock()) {
                live->in_flight = false;
                live->cancelled = true;
            }
        }));
}

Status ServerImpl::add_passband(std::shared_ptr<PassbandNode> node) {
    const std::uint32_t key = node->vrx.value;

    // The engine is asked once per receiver, however many clients are
    // watching it. A second set_passband_sink would replace the first, which
    // would be silent and would take the other client's frames with it.
    auto existing = passband_sinks_.find(key);
    if (existing == passband_sinks_.end()) {
        std::scoped_lock held(sink_lock_);
        if (sink_closed_) {
            return fail("this server is stopping and will install no further sinks");
        }

        auto gate = gate_;
        const engine::VrxId which = node->vrx;
        Status installed = engine_.set_passband_sink(
            which, [gate, which](const engine::PassbandFrame& frame) -> Status {
                // The same gate the spectrum sink uses. A sink call already
                // running when stop() closes the gate finds the owner null
                // and returns rather than touching a dead server.
                std::scoped_lock owned(gate->lock);
                if (gate->owner == nullptr) {
                    return {};
                }
                return gate->owner->on_passband_frame(frame);
            });
        if (!installed) {
            return installed;
        }
        passband_sinks_[key] = 0;
    }

    passband_sinks_[key] += 1;
    passband_nodes_.push_back(std::move(node));
    return {};
}

void ServerImpl::end_passband(const std::shared_ptr<PassbandNode>& node) {
    const std::uint32_t key = node->vrx.value;
    node->cancelled = true;

    const auto before = passband_nodes_.size();
    std::erase(passband_nodes_, node);
    if (passband_nodes_.size() == before) {
        // Already ended. Cancelling twice is ordinary: a client can call
        // cancel and then drop the capability.
        return;
    }

    auto counted = passband_sinks_.find(key);
    if (counted == passband_sinks_.end()) {
        return;
    }
    if (counted->second > 1) {
        counted->second -= 1;
        return;
    }

    passband_sinks_.erase(counted);
    detach_passband_sink(node->vrx);
}

void ServerImpl::detach_passband_sink(engine::VrxId vrx) {
    {
        std::scoped_lock held(sink_lock_);
        if (sink_closed_) {
            // stop() has already taken every sink off.
            return;
        }
        // Asynchronous, so a frame recorded before this can still arrive.
        // The pending slot below is dropped for that reason rather than
        // left to be fanned out to a subscription nobody holds.
        static_cast<void>(engine_.set_passband_sink(vrx, {}));
    }

    std::scoped_lock held(frame_lock_);
    auto slot = passband_pending_.find(vrx.value);
    if (slot != passband_pending_.end()) {
        if (slot->second != nullptr) {
            passband_spare_.push_back(std::move(slot->second));
        }
        passband_pending_.erase(slot);
    }
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
    // Every receiver this server was watching, taken off only now: the map
    // is loop-thread state and the loop has just been joined, so this is the
    // first moment it can be read from here. sink_closed_ above is what
    // stopped a subscribePassband adding to it in the meantime.
    for (const auto& entry : passband_sinks_) {
        static_cast<void>(engine_.set_passband_sink(engine::VrxId{entry.first}, {}));
    }
    passband_sinks_.clear();
    passband_nodes_.clear();

    {
        std::scoped_lock held(frame_lock_);
        pending_.reset();
        spare_.clear();
        passband_pending_.clear();
        passband_spare_.clear();
    }

    // Released here rather than left to the destructor, for the same reason
    // the frame buffers are: the detector holds several arrays of one double
    // per bin, and a Server kept alive after stop() should not be holding a
    // frame's worth of them. Safe only now, with the gate closed and the loop
    // joined, because those are the two threads that could be inside it.
    {
        std::scoped_lock held(detect_lock_);
        detecting_.store(false, std::memory_order_relaxed);
        detector_.reset();
    }

    release_engine(engine_);
}

}  // namespace

Expected<std::unique_ptr<Server>> Server::create(engine::Engine& engine,
                                                 const ServerOptions& options) {
    // Before the engine is claimed and before a port is bound, so a caller
    // that got this wrong has no half-built server to tidy up.
    //
    // There is no unauthenticated path and this is what makes that true. An
    // empty token meaning "serve anybody" would be a one-line convenience
    // that ships an engine the whole machine can drive, out of code that
    // reads as configured. See ServerOptions::token.
    if (options.token.empty()) {
        return fail(std::format(
            "the RPC server was given no token, and there is no unauthenticated mode. Put {} "
            "bytes in ServerOptions::token; core/rpc/token.h mints and loads a file holding "
            "them",
            kTokenBytes));
    }
    if (options.token.size() != kTokenBytes) {
        return fail(std::format(
            "the RPC server was given a {}-byte token and it has to be exactly {}",
            options.token.size(), kTokenBytes));
    }

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
