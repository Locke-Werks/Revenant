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
//
// THE RDS DECODERS LIVE HERE TOO, AND THE DETECTOR'S PRECEDENT IS ONLY HALF
// AN ARGUMENT FOR IT
//
// The half that carries: a decoder is host-side work on host-side PCM.
// core/decode/rds_bits.h takes a span of real samples and core/decode/
// rds_groups.h takes bits, neither has a shader behind it, and this server
// is already the thing a client asks. So the shape is the detector's: built
// on the first call rather than at startup, run on the engine's completion
// thread inside a sink, one lock around the whole object because its own
// header says one thread owns it and there is no lock inside it.
//
// The half that does not, and which had to be decided rather than
// inherited: the detector had nowhere else it could go. engine::Engine has
// no detector and no method returning tracks, and the spectrum sink it
// needs is the one this server already owns. RDS had somewhere else.
// core/engine could have grown a per-receiver decode stage, and
// core/engine/vrx.h says in as many words that it deliberately did not.
// Three reasons it stays out here:
//
//   1. Engine::attach_audio_sink is the composition point and it exists.
//      core/engine/vrx.h retracted its "optional decoder chain" sentence on
//      exactly these terms: a second decoder seam invented inside the
//      engine would mean the one written first gets deleted.
//   2. A decoder in the engine would have to hand its state back through an
//      Engine method, which means core/engine depending on core/decode and
//      an engine-level mirror of decode::StationState. The schema is
//      already that mirror and two of them would drift.
//   3. Nothing in the engine knows that a client asked. The whole saving
//      here is that a receiver nobody has asked about runs no decoder, and
//      "somebody asked" is a fact about a session.
//
// FIVE DIFFERENCES FROM THE DETECTOR THAT CHANGE THE CODE
//
// It is PER RECEIVER, so there is a map of these rather than one optional,
// and each one dies with the receiver it was built on rather than living as
// long as the Server.
//
// It ATTACHES rather than sets. The sink goes on through
// Engine::attach_audio_sink, so a recording, a loudspeaker or a
// subscribeAudio already on that receiver keeps its audio. The schema's own
// note on rdsStation said this call would have to be refused on a receiver
// that already had a sink, which was true when it was written and is not
// true now; the retraction is in core/rpc/revenant.capnp.
//
// It TOUCHES NOTHING BUT ITS OWN ROUTE on the sample path. AudioRoute needs
// an owner pointer because on_audio_chunk has to reach the server's node
// list and its fulfiller; an RdsRoute's sink call reads and writes that one
// route and nothing else, so a late call arriving after the Server is gone
// is safe as long as the route is, and the callable co-owns it. There is no
// gate on this one and that is why.
//
// It CANNOT FAIL THE DISPATCH either, and for a narrower reason than the
// detector's. The decoder has no refusal at all: RdsBitSync::process and
// RdsDecoder::feed both return void. What can go wrong is the chunk not
// being what the receiver promised, a rate or a channel count that is not
// what the decoder was built for, and that is recorded in the route and
// reported to the next poll rather than returned to the graph.
//
// And a client CAN tear this one down early, by removing the receiver,
// which the detector has no equivalent of. Every poll asks vrx_status
// first, so a receiver removed by any path, including one this session
// never saw, is answered in the engine's own words and its decoder is
// dropped there.
//
// WHAT IT COSTS, MEASURED RATHER THAN ASSERTED
//
// 12.07 ms of one core per second of composite at 171000 S/s, measured on
// 2026-09-20 by tests/rpc/test_rpc_rds.cpp's own timing case: a locked
// station through RdsBitSync::process with RdsDecoder::feed on its bit
// sink, one second timed out of the middle of the recording, best of three.
// That is 1.2 percent of one core, PER DECODING RECEIVER rather than per
// engine, so eight of them cost eight times it.
//
// The figure to compare against is the detector's 0.201 ms per frame above,
// which the same kind of run put at 0.7 percent of one core. Per chunk they
// land in the same place. A chunk at this project's 16384-sample blocks and
// a 1368000 S/s source is 2048 composite samples, so the decoder spends
// 0.145 ms on it against the detector's 0.201 ms on a frame.
//
// THE CPU SHARE IS NOT THE NUMBER THAT MATTERS. This runs on the thread
// retiring GPU readbacks, so what it really costs is LATENCY added to every
// other sink behind it: 0.145 ms of work inside a 12 ms chunk interval,
// which is 1.2 percent of the budget before anything else on that thread
// has run. Eight decoders would be 1.2 ms of a 12 ms interval, still inside
// it and no longer negligible, and that is the point at which a decoder
// wants its own thread rather than the sink.
//
// AND IT COSTS MORE GPU THAN A LISTENING RECEIVER, NOT LESS
//
// docs/rpc.md carried "slightly LESS GPU than a listening receiver: the
// audio FIR runs at the output rate and needs 103 taps instead of 353", and
// the arithmetic in that same sentence disproves it. core/dsp/
// vrx_reference.cpp runs the audio decimation FIR at the OUTPUT rate, one
// detector evaluation per tap per output sample, and for a discriminator
// each of those is an atan2. So the count is output_rate * audio_taps:
//
//   listening   48000 x 353 = 16.9 M atan2/s
//   RDS        171000 x 103 = 17.6 M atan2/s
//
// Fewer taps, three and a half times the rate, and the product is 3.9
// percent higher. Four percent is still small and small was the point being
// made; "less" was the wrong word for it and is retracted here as well as
// there, because a reader sizing a device off this file should not have to
// find the doc to learn which way the inequality runs.

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
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <capnp/message.h>
#include <capnp/rpc-twoparty.h>
#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/exception.h>
#include <kj/memory.h>
#include <kj/string.h>

#include "core/detect/detector.h"
#include "core/detect/front_end.h"
#include "core/rpc/convert.h"
#include "core/rpc/decoders.h"
#include "core/rpc/listen.h"
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

// The four-value type Cap'n Proto carries, from the category we hold.
//
// THREE OF OUR SIX MAP AND THREE DO NOT, and the reason is the protocol rather
// than this function. capnp::rpc::Exception is a reason, a type and a trace;
// there is no detail blob on the wire, so nothing can be smuggled beside the
// type. Unreachable and Unauthenticated never arise on this side of a
// connection that exists, so their absence costs nothing: a client generates
// both locally, at the point where it failed to reach or failed to log in.
// Unclassified is the honest answer for everything else, which is what FAILED
// already means in rpc.capnp's own words: repeating the operation without a
// change in the state of the world would fail again.
//
// So a refusal a client has to ACT on does not belong here. rpc.capnp says it
// under its own Exception struct: exceptions should not be used to flag
// conditions a client is expected to handle in an application-specific way.
// Those are result fields in revenant.capnp.
[[nodiscard]] kj::Exception::Type to_exception_type(ErrorCategory category) {
    switch (category) {
        case ErrorCategory::Disconnected:
            return kj::Exception::Type::DISCONNECTED;
        case ErrorCategory::Overloaded:
            return kj::Exception::Type::OVERLOADED;
        case ErrorCategory::Unimplemented:
            return kj::Exception::Type::UNIMPLEMENTED;
        case ErrorCategory::Unreachable:
        case ErrorCategory::Unauthenticated:
        case ErrorCategory::Unclassified:
            break;
    }
    return kj::Exception::Type::FAILED;
}

// An engine failure becomes a Cap'n Proto exception carrying the engine's own
// message. Defaulting the result instead would hand a client a zeroed struct
// and no way to tell it from a real answer.
//
// WHAT THIS FUNCTION USED TO DO: every error became
// kj::Exception::Type::FAILED, whatever it was. An engine that was out of
// device memory and an engine that had been handed a frequency out of range
// were the same verdict to a client, so a client could not schedule a retry
// differently from a correction it would never make. The type now comes from
// Error::category, and error.h says what that carries and what it does not.
[[nodiscard]] kj::Exception to_exception(const Error& error) {
    // error.h: a zero code means the failure was ours rather than a call's, so
    // printing it would attribute our own message to a driver.
    const std::string text = error.code == 0
                                 ? error.message
                                 : std::format("{} (code {})", error.message, error.code);
    return {to_exception_type(error.category), __FILE__, __LINE__,
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

// The audio depth clamp, in milliseconds. Zero asks for the default.
//
// The ceiling is the schema's. The floor is 20 ms, which is under one chunk
// on every source this tree has run and is therefore not the binding
// constraint; the binding one is the two-chunk floor applied in frames when
// the first chunk arrives. It is here so that a client asking for 1 ms is
// told the number changed rather than being handed its own value back and a
// queue that behaves like something else.
constexpr std::uint32_t kDefaultAudioBufferMillis = 500;
constexpr std::uint32_t kMinAudioBufferMillis = 20;
constexpr std::uint32_t kMaxAudioBufferMillis = 5000;

// Chunk buffers a subscription keeps for reuse. Small on purpose: the queue
// itself is the buffer, and this is only what keeps the engine's completion
// thread out of the allocator in the steady state, where at most one buffer
// is being refilled while another is on its way out.
constexpr std::size_t kAudioSpareBuffers = 4;

[[nodiscard]] constexpr std::uint32_t grant_audio_buffer_millis(std::uint32_t asked) {
    const std::uint32_t wanted = asked == 0 ? kDefaultAudioBufferMillis : asked;
    return std::clamp(wanted, kMinAudioBufferMillis, kMaxAudioBufferMillis);
}

// One chunk, owning its samples so it can outlive the sink call.
//
// engine::AudioChunk::samples is valid for the duration of that call and not
// after, exactly like a spectrum frame's bins, and audio is queued rather
// than replaced, so the copy is what makes the queue legal at all.
struct AudioChunkBuffer {
    std::vector<float> samples;
    std::uint64_t sample_index = 0;
    std::uint32_t frames = 0;
    std::uint32_t rate = 0;
    std::uint16_t channels = 1;
    bool squelch_open = true;
};

// One subscriber to one receiver's audio.
//
// UNLIKE EVERY OTHER NODE IN THIS FILE, PART OF THIS ONE IS SHARED. The
// display subscriptions keep a single frame in a slot and are touched only
// on the loop thread. Audio is a queue, because a chunk is the only copy of
// that instant and the newest is worth no more than the one before it, and
// the engine's completion thread has to be able to push into that queue
// without waiting for the loop. So everything under `lock` is written by
// both threads and everything above it is the loop thread's alone.
struct AudioNode : std::enable_shared_from_this<AudioNode> {
    AudioNode(schema::AudioReceiver::Client client, engine::VrxId which,
              std::uint32_t millis)
        : receiver(kj::mv(client)), vrx(which), granted_millis(millis) {}

    // Set here and never written again, which is what makes it legal for
    // on_audio_chunk to read them on the engine's completion thread. They
    // were both filed under "Loop thread only" below until 2026-09-20, and
    // granted_millis is read by that other thread on every chunk: a label
    // saying a field belongs to one thread while another reads it is worse
    // than no label, because the next person to add a field copies the
    // neighbour that looks closest.
    //
    // The node is constructed and only then handed to add_audio, so the
    // completion thread cannot see either of these before the constructor
    // has finished with them.
    const engine::VrxId vrx;
    const std::uint32_t granted_millis = 0;

    // Loop thread only.
    schema::AudioReceiver::Client receiver;
    bool in_flight = false;
    bool cancelled = false;
    bool ended_sent = false;

    // Both threads, under `lock`.
    std::mutex lock;
    std::deque<AudioChunkBuffer> queue;
    std::vector<AudioChunkBuffer> spare;

    // The depth in force, and zero until the first chunk sets it. See the
    // schema's note on AudioStats::bufferFrames for why it cannot be
    // computed when the subscription is answered.
    std::uint64_t buffer_frames = 0;
    std::uint64_t queued_frames = 0;

    // Evicted since the last chunk this node was sent, which is exactly what
    // rides out on the next one as framesDroppedBefore.
    std::uint64_t dropped_before = 0;

    std::uint64_t frames_sent = 0;
    std::uint64_t frames_dropped = 0;
    std::uint64_t drop_events = 0;

    // True while the queue is evicting, so drop_events counts the edge
    // rather than the frames.
    bool dropping = false;
};

class ServerImpl;

// Everything one receiver's audio sink needs, co-owned by the sink callable.
//
// The same shape as SinkGate below and for the same reason: the callable
// outlives the Server whenever a dispatch was in flight when the sink came
// off, so `owner` is cleared under this lock and a late call finds null
// instead of freed memory. It carries the node list as well, because the
// completion thread is the one that has to walk it.
struct AudioRoute {
    std::mutex lock;
    ServerImpl* owner = nullptr;
    std::vector<std::shared_ptr<AudioNode>> nodes;
    engine::AudioSinkId sink = 0;
};

// ---------------------------------------------------------------------------
// RDS
// ---------------------------------------------------------------------------

// The top of the FM composite: the 57 kHz subcarrier plus the 2375 Hz of
// clause 1.7 shaping. Both the passband condition and the audio rate
// condition below are this number in different units.
constexpr dsp::Hertz kCompositeTopHz = 59'375;

// The audio rate a composite from a RECEIVER has to clear, as opposed to one
// from a file or a modulator.
//
// core/decode/rds_bits.h enforces decode::kMinimumRateHz, which is 125000,
// and says at length why it is not this number: a composite that went
// through no audio filter is intact at 125000 and refusing it would be the
// decoder declining a signal because of a stage that was not in the path. A
// composite from a receiver did go through one. dsp::design_audio_taps puts
// the decimation filter's passband edge at 0.4 of the audio rate, so 59375
// is inside the passband only from 59375/0.4 up, and this is that quotient
// rounded up to the hertz.
//
// It applies only when the decimation resolved above one. At a decimation of
// one the planner designs no audio filter at all and the decoder's own 125000
// is the true bound, which is why the refusal below distinguishes the two
// rather than quoting one number.
constexpr dsp::SampleRate kFilteredCompositeRateHz = 148'438;

// What one rdsStation call takes off a decoder, on the loop thread, under
// that route's lock.
//
// The station state is COPIED rather than referenced, for the reason
// DetectionSnapshot copies tracks: the completion thread may be inside the
// decoder the instant the lock is released, and the capnp message is written
// after that. The copy allocates, on the AF, ODA and EON vectors, and the
// completion thread waits for it. That is the same bargain the detector
// struck and it is a better one here, because the vectors are bounded at 16
// EON entries and a handful of frequencies where the detector's is bounded
// only by how busy the band is.
struct RdsSnapshot {
    decode::StationState state;
    decode::RdsBitsStatus bits;
    decode::Region region = decode::Region::kRds;

    // The block layer's counters, which are nine accessors on RdsDecoder
    // rather than a struct it hands out.
    decode::SyncState sync = decode::SyncState::kHunting;
    std::uint64_t bits_fed = 0;
    std::uint64_t groups_decoded = 0;
    std::uint64_t blocks_good = 0;
    std::uint64_t blocks_corrected = 0;
    std::uint64_t blocks_dropped = 0;
    std::uint64_t sync_acquisitions = 0;
    std::uint64_t sync_losses = 0;

    // The server's own six.
    std::uint32_t composite_rate = 0;
    std::uint64_t last_group_sample = 0;
    std::uint64_t ta_changed_at = 0;

    // The retune fence, as a state and a size. See RdsRoute::epoch_target.
    bool discarding = false;
    std::uint64_t chunks_discarded = 0;

    // Empty while the decoder is running. See RdsRoute::fault, and
    // rds_station for why a faulted decoder answers rather than refusing.
    std::string fault;
};

// One receiver's RDS decode.
//
// THE SINK CALLABLE CO-OWNS THIS AND REACHES NOTHING ELSE. See the note at
// the top of the file: unlike AudioRoute there is no owner pointer and no
// gate, because the completion thread's whole job here is to push samples
// into the two objects below and update three indices beside them.
struct RdsRoute {
    RdsRoute(engine::VrxId which, std::uint32_t rate, decode::Region what,
             decode::RdsBitSync sync)
        : vrx(which), composite_rate(rate), region(what), bits(std::move(sync)),
          groups(what) {}

    // Set before the route is handed to the completion thread and never
    // written again, so reading them there needs no lock.
    const engine::VrxId vrx;
    const std::uint32_t composite_rate;

    // Loop thread only: the token that detaches the sink again.
    engine::AudioSinkId sink = 0;

    // Everything below is written by the completion thread and read by the
    // loop thread, under this lock. setRdsRegion writes all of it from the
    // loop thread instead, which is the one place the ownership reverses and
    // is why the region is in here rather than beside composite_rate.
    std::mutex lock;

    decode::Region region;
    decode::RdsBitSync bits;
    decode::RdsDecoder groups;

    std::uint64_t last_group_sample = 0;

    // THE RETUNE FENCE. Engine::set_vrx_params only QUEUES a control op, so
    // reset_rds_for_vrx clearing this decoder on the loop thread leaves the
    // frames already recorded at the old tuning still on their way to the
    // sink. Without a fence they land in the decoder that was just cleared,
    // which reads as the NEW station's first samples and is worse than not
    // clearing at all.
    //
    // TWO READINGS OF ONE NUMBER, WHICH IS THE WHOLE ARRANGEMENT.
    // epoch_target is VrxStatus::tuning_epoch read on the loop thread after
    // the retune was queued: the epoch this receiver's chunks will carry
    // once the graph has applied everything asked of it. epoch_reached is
    // the highest AudioChunk::tuning_epoch the sample path has delivered. A
    // chunk below the target is the tuning the client just left, so it is
    // discarded; a chunk at or above it is the tuning the client asked for.
    //
    // It cannot stick. The graph moves the applied epoch for every retune
    // op, including one the stage refuses, and it never queues an op
    // without also moving the number vrx_status reports, so every target
    // this route can hold is one some chunk will carry. A target set for a
    // receiver that then stops producing chunks is not a stuck fence: there
    // is nothing to decode either way.
    //
    // WHAT THIS USED TO BE, BECAUSE IT FAILED SILENTLY AND PERMANENTLY.
    // Until this change the loop thread counted resets into retunes_pending
    // and the sample path took one off per OBSERVED CHANGE of chunk epoch.
    // Graph::drain_control applies the whole queued stack in one pass
    // before a single frame is recorded, so two retunes for one receiver
    // inside one block period take the epoch from 0 to 2 and produce ONE
    // stamped change. The count went to two, came down to one, and stayed
    // there: every later chunk was discarded, bits.process was never
    // reached again, and rdsStation answered with zeros and no fault, which
    // on the wire is indistinguishable from a receiver pointed at a quiet
    // channel. A dial drag reaches it, and so does any client that sets
    // centre and bandwidth in two calls. The same count was also lost
    // whenever an epoch landed only on a frame that produced no samples,
    // because such a frame is never delivered and its change never
    // observed. Comparing against a target has neither failure: it does not
    // care how many retunes share a drain, or whether any particular epoch
    // was ever carried by a chunk.
    //
    // A RETUNE BEFORE THE FIRST CHUNK IS NOW FENCED TOO, which the counting
    // version could not do and said so. There is no "first epoch" to
    // establish: the target is read off the receiver rather than inferred
    // from what has been seen, so a reset arriving before any chunk fences
    // exactly like one arriving after.
    std::uint64_t epoch_target = 0;
    std::uint64_t epoch_reached = 0;

    // Chunks the fence threw away, cumulative for the life of this decoder.
    //
    // ON THE WIRE, and that is the point of it rather than a diagnostic
    // afterthought. A decoder that is discarding looks exactly like a
    // decoder hearing nothing: same zeros, same empty fault, same frozen
    // counters. Reporting the state and its size is what lets a client say
    // which, and it is what would have made the counting bug above a
    // question somebody asked on the first poll instead of a quiet band
    // nobody doubted.
    std::uint64_t chunks_discarded = 0;

    // Whether the fence is up right now: the sample path has not yet
    // delivered a chunk from the tuning the client last asked for.
    [[nodiscard]] bool discarding() const { return epoch_reached < epoch_target; }

    // The index at which ta last CHANGED, which is not the index at which it
    // was first received. The first valid value is the state the station was
    // already in when this decoder started, and recording it here would tell
    // a client an announcement boundary happened where none did. So a
    // station tuned mid-announcement reads ta true with this at zero, which
    // is the honest answer.
    std::uint64_t ta_changed_at = 0;
    bool ta_seen = false;
    bool last_ta = false;

    // Why this decoder stopped, empty while it has not. The sink must not
    // fail the dispatch, so a chunk that is not what the receiver promised
    // is recorded here and reported to the next caller. Same arrangement as
    // detector_fault_ and for the same reason.
    //
    // TERMINAL, AND THAT IS CORRECT RATHER THAN UNFINISHED. The only two
    // ways to set it are a channel count and a rate that are not what the
    // decoder was built for, both of which are SHAPE, and
    // Graph::set_vrx_params refuses a retune that changes the shape rather
    // than applying it. So a receiver that delivered the wrong thing once
    // has no way to start delivering the right one, and a recovery path
    // would be a way to ask the same broken receiver again. Removing the
    // receiver and adding another is the recovery, and it is the only one
    // that changes anything.
    //
    // Neither reset clears it. set_rds_region does not, because a region is
    // not a shape; reset_rds_for_vrx does not, for the same reason. Both say
    // so where they do it.
    //
    // ON THE WIRE as RdsStation::fault since 2026-09-20, so a client can
    // tell a decoder that stopped from a receiver that went without matching
    // prose. rds_station has why that is a field rather than a refusal.
    std::string fault;
};

// ---------------------------------------------------------------------------
// Decoded messages
// ---------------------------------------------------------------------------

// How many messages one subscription holds before it evicts the oldest.
//
// A message is the only copy of an event, so a subscription queues, which is
// audio's rule rather than the display streams'. The depth is a count rather
// than a duration because a decoder's message rate is the transmitter's and
// not the engine's: P25 produces a data unit every 180 ms during a call and
// nothing between calls. 256 is 46 seconds of back-to-back P25 voice frames,
// which is far longer than any client stall this is meant to absorb, and at a
// few hundred bytes a message it is well under a megabyte a subscription.
constexpr std::size_t kDecodedQueueDepth = 256;

// One subscriber to one decoder on one receiver.
//
// Split the way AudioNode is: the loop thread alone touches the capability and
// the flight flags, and the queue under `lock` is written by the completion
// thread and drained by the loop.
struct DecodedNode : std::enable_shared_from_this<DecodedNode> {
    DecodedNode(schema::DecodedReceiver::Client client, engine::VrxId which,
                std::string_view name)
        : vrx(which), decoder(name), receiver(kj::mv(client)) {}

    // Set here and never written again.
    const engine::VrxId vrx;
    const std::string decoder;

    // Loop thread only.
    schema::DecodedReceiver::Client receiver;
    bool in_flight = false;
    bool cancelled = false;
    bool ended_sent = false;

    // Both threads, under `lock`.
    std::mutex lock;
    std::deque<DecodedMessage> queue;
    std::uint64_t dropped_before = 0;
    std::uint64_t messages_sent = 0;
    std::uint64_t messages_dropped = 0;
};

// One decoder attached to one receiver, shared by every subscriber to it.
//
// The shape of AudioRoute with a decoder inside it. The sink callable
// co-owns it, so a dispatch already in flight when the sink comes off still
// finds live memory, and `owner` is cleared under `lock` so that such a
// dispatch finds null rather than a server that has gone.
struct DecodeRoute {
    DecodeRoute(engine::VrxId which, const DecoderSpec& what, engine::Demod demod)
        : vrx(which), spec(what), mode(engine::demod_name(demod)) {}

    const engine::VrxId vrx;
    const DecoderSpec& spec;

    // The receiver's demodulator, which the engine never changes in place,
    // for DecoderBuild: the sideband decides an RTTY decoder's polarity.
    const std::string_view mode;

    // Loop thread only: the token that detaches the sink, and whether the
    // fault below has already been turned into ended() calls.
    engine::AudioSinkId sink = 0;
    bool fault_reported = false;

    // Everything below is under this lock. The completion thread writes the
    // decoder's state and the nodes' queues; the loop thread adds and removes
    // nodes, resets the decoder for a retune and reads the counters.
    std::mutex lock;
    ServerImpl* owner = nullptr;
    std::vector<std::shared_ptr<DecodedNode>> nodes;

    // Built on the first chunk, at that chunk's rate, because the rate a
    // complex tap delivers is the engine's business and is changing. See
    // core/rpc/decoders.h.
    std::unique_ptr<ChunkDecoder> decoder;
    std::uint32_t rate = 0;

    // The retune fence, on exactly RdsRoute::epoch_target's terms. A chunk
    // below the target is the tuning the client left and is discarded.
    std::uint64_t epoch_target = 0;
    std::uint64_t epoch_reached = 0;
    std::uint64_t chunks_discarded = 0;
    [[nodiscard]] bool discarding() const { return epoch_reached < epoch_target; }

    // Messages this decoder has produced, which is what DecodedMessage::
    // sequence counts.
    std::uint64_t sequence = 0;

    // Why this decoder stopped, empty while it has not. Terminal, on
    // RdsRoute::fault's argument: the only refusals are shape, and a retune
    // that changes shape is refused by the engine.
    std::string fault;

    // Scratch the decoder appends into, kept so a busy decoder does not
    // allocate a vector per chunk.
    std::vector<DecodedMessage> scratch;
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

// Whose a receiver is, for the lifetime rule the schema states on addVrx.
//
// session is the creating login's number, zero for a receiver this server did
// not create: one the host process added directly, or one that predates the
// server. keep is the flag addVrx was given. Loop thread only, like every map
// that holds one.
struct VrxOwner {
    std::uint64_t session = 0;
    bool keep = false;
};

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

    // Engine completion thread, with route.lock already held by the sink
    // callable that got here. Always answers success: a subscriber that
    // cannot keep up counts a drop of its own, and failing here would fail
    // the dispatch and end the run over one slow socket.
    [[nodiscard]] Status on_audio_chunk(AudioRoute& route, const engine::AudioChunk& chunk);

    // Event loop thread. Attaches the engine sink on the first subscriber to
    // a receiver and detaches it with the last, so a receiver nobody is
    // listening to costs nothing on the wire.
    [[nodiscard]] Status add_audio(std::shared_ptr<AudioNode> node);
    void end_audio(const std::shared_ptr<AudioNode>& node);

    // Event loop thread. Tells every subscriber on this receiver that no
    // further chunk is coming, then ends them. Called when a receiver is
    // removed through this session: silence is what a quiet channel sounds
    // like, so a stream that simply stops is indistinguishable from one
    // nobody is talking on.
    //
    // kj::StringPtr AND NOT std::string_view, WHICH IS NOT A STYLE CHOICE
    //
    // This took a string_view and handed it to kj::StringPtr(const char*,
    // size_t), whose contract is a NUL-terminated buffer: its constructor
    // reads text[size] to assert the terminator is there. A string_view
    // carries no such promise, so that read was one byte past the view. The
    // one call site passes a literal and the byte it read was the literal's
    // own NUL, which is why it never showed up. Taking kj::StringPtr moves
    // the requirement into the type, where a caller with a std::string
    // substring cannot satisfy it by accident.
    void end_audio_for_vrx(engine::VrxId vrx, kj::StringPtr reason);

    // Event loop thread. One ended() on one subscription, at most once for
    // the life of that subscription, best effort and never retried. Split
    // out because two paths end a stream the client did not ask to end and
    // both owe it the same call: a receiver removed, and a chunk call the
    // receiver itself failed.
    //
    // It does NOT check node->cancelled. The delivery-failure path sets
    // that flag before it gets here, because the node is finished either
    // way; a caller that must not speak to a client-cancelled subscription
    // checks the flag itself, which is what end_audio_for_vrx does.
    void send_audio_ended(const std::shared_ptr<AudioNode>& node, kj::StringPtr reason);

    // Event loop thread. Queues a listing for the worker and hands back a
    // promise this loop resolves when the worker is done. See the note at the
    // top: this is the only engine-facing call that opens hardware, and the
    // loop must stay free while it runs.
    [[nodiscard]] kj::Promise<SourceListing> list_sources();

    // Event loop thread. Opens and closes the engine's source, and takes this
    // server's own source-derived state down with it.
    //
    // WHY THE SERVER HAS TO DO ANYTHING AT ALL, given the engine tears its own
    // graph down. Everything this class holds that describes a stream outlives
    // that graph, and each piece fails differently if it is left:
    //
    //   sink_installed_ is sticky, so the next subscribeSpectrum would find it
    //   set, skip ensure_sink, and hand back a subscription nothing feeds.
    //
    //   The detector is sized against the old geometry, in bins. Fed the next
    //   source's frames it would either refuse them or, at a matching bin
    //   count and a different bin width, report tracks at frequencies that do
    //   not exist.
    //
    //   Audio routes, passband nodes and RDS decoders name receivers that went
    //   with the graph, and each holds an engine sink token for one. Left
    //   alone they are dropped one at a time by whoever happens to ask about
    //   that id again, which after a close is nobody.
    //
    //   The pending frame copies are the old stream's last frames, numbered in
    //   its sample indices, queued to be delivered after the close.
    //
    // ORDER: detach from the engine while the graph still exists, then close.
    // The reverse leaves the engine destroying sinks this server believes it
    // still owns.
    //
    // It runs on the loop thread, which is what makes the loop-thread-only
    // maps readable here at all. stop() does the same walk and has to join the
    // loop first; this is called from a Cap'n Proto method and is already
    // there.
    void release_source_state(kj::StringPtr reason);
    [[nodiscard]] Status open_source(std::string_view uri);
    [[nodiscard]] Status close_source();

    // Event loop thread, all three. See the detector notes at the top of the
    // file for why the object is built on demand and locked as a whole.
    [[nodiscard]] Status ensure_detector();
    [[nodiscard]] Expected<DetectionSnapshot> detections(double min_confidence,
                                                        double min_margin);
    [[nodiscard]] Status set_detection_threshold(double threshold_db);

    // Completion thread, with detect_lock_ already held and a detector that
    // has just taken a frame. Does nothing until the detector decides.
    void observe_front_end();

    // Event loop thread. What the monitor last said, or an Unmeasured
    // reading when nothing has asked for detections.
    [[nodiscard]] detect::FrontEndObservation front_end();

    // Event loop thread, all five.
    //
    // rds_station and set_rds_region both build the decoder if there is not
    // one, so the four conditions are checked on whichever call comes first
    // and a client presetting a region is told about an unsuitable receiver
    // then rather than at its first poll.
    [[nodiscard]] Expected<RdsSnapshot> rds_station(engine::VrxId vrx);
    [[nodiscard]] Status set_rds_region(engine::VrxId vrx, decode::Region region);

    // Detaches the sink and drops the decoder. Called when a receiver is
    // removed through this session, and again by any poll that finds the
    // receiver gone, so a removal this session never saw is still cleaned up
    // the first time anybody asks.
    void end_rds_for_vrx(engine::VrxId vrx);

    // Clears one decoder's accumulated state, keeping its region and its
    // sink. Called after a retune: PS, RadioText and the AF list belong to
    // the station that was tuned, and a receiver moved to another frequency
    // would otherwise assemble one station's text over another's.
    // epoch_target is VrxStatus::tuning_epoch read AFTER the retune was
    // queued. Taken as an argument rather than read here, so that a
    // receiver that vanished between the retune and this call is reported
    // to the caller who can still answer, instead of leaving a fence this
    // function could only guess at.
    void reset_rds_for_vrx(engine::VrxId vrx, std::uint64_t epoch_target);

    // Event loop thread. Throws away everything this server holds that was
    // measured against the front end's old centre, after a retune the
    // engine has already applied.
    //
    // TWO THINGS, AND NEITHER OF THEM IS SAMPLES. The wideband detector is
    // dropped, so the next detections call rebuilds it with the new
    // sourceCenter and no tracks: every track it held was a measurement of
    // a band that is not there any more, and its absolute centre was
    // computed by adding a constant that has changed. Every RDS decoder is
    // cleared and fenced, on exactly the terms setVrxParams clears one,
    // because each receiver is now pointed at a different transmitter.
    //
    // The decoders are fenced against the epoch the ENGINE moved.
    // Engine::set_source_center re-queues every receiver's own params so
    // the graph advances its tuning epoch, which is what makes the existing
    // per-receiver fence work for a change that is not per receiver. Read
    // here and after the tune, for the reason setVrxParams reads it after
    // its own: the target is the epoch this receiver's chunks will carry
    // once everything queued has been applied.
    void forget_across_retune();

    // Builds one, attaches its sink and records it. Split out because both
    // entry points above reach it and both have already asked the engine
    // for the receiver's status, which this needs and must not ask twice:
    // the receiver could be removed between the two calls.
    [[nodiscard]] Expected<std::shared_ptr<RdsRoute>> start_rds(
        const engine::VrxStatus& status, decode::Region region);

    // Event loop thread, all of them. The receiver lifetime rule.
    //
    // open_session hands a new login its number. record_vrx notes who created
    // a receiver and whether it asked to keep it; owner_of reads that back for
    // VrxStatus. end_session is called by a session's destructor and removes
    // every receiver that session created and did not keep.
    [[nodiscard]] std::uint64_t open_session() { return next_session_++; }
    void record_vrx(engine::VrxId vrx, std::uint64_t session, bool keep);
    [[nodiscard]] VrxOwner owner_of(engine::VrxId vrx) const;
    void end_session(std::uint64_t session);

    // Everything this server holds about a receiver the engine has just
    // removed, taken down in one place so that removeVrx and a session ending
    // cannot come to differ. Audio subscribers are told why; the RDS decoder
    // goes; the ownership record goes.
    //
    // The two display streams need nothing here, because a receiver removed
    // out from under one freezes a picture and a frozen picture is visible
    // from across the room. Audio goes quiet instead, and a quiet channel with
    // the squelch shut sounds identical, so the subscriber is told in words.
    void after_vrx_removed(engine::VrxId vrx, kj::StringPtr reason);

    // Decoded messages. Event loop thread unless marked.
    //
    // add_decoded attaches the decoder on the first subscriber to it on that
    // receiver and joins the node to it after that; `status` is the receiver
    // as the caller already read it, for the fence's starting epoch.
    // end_decoded takes one node off and the decoder with the last.
    // end_decoded_for_vrx tells every subscriber on a receiver why its stream
    // stopped and ends them, for a removal. reset_decoded_for_vrx clears and
    // fences every decoder on a receiver, for a retune.
    [[nodiscard]] Status add_decoded(std::shared_ptr<DecodedNode> node,
                                     const engine::VrxStatus& status, const DecoderSpec& spec);
    void end_decoded(const std::shared_ptr<DecodedNode>& node);
    void end_decoded_for_vrx(engine::VrxId vrx, kj::StringPtr reason);
    void reset_decoded_for_vrx(engine::VrxId vrx, std::uint64_t epoch_target);
    [[nodiscard]] Expected<DecodedStats> decoded_stats(DecodedNode& node);

    // Engine completion thread, with route.lock held by the sink callable.
    // Never fails the dispatch: a decoder that refuses records a fault and
    // the loop turns it into ended() calls, on the argument decode_rds_chunk
    // makes for RDS.
    void on_decoded_chunk(DecodeRoute& route, const engine::AudioChunk& chunk);

    // Any thread. Rings the bell the loop thread is waiting on, taken rather
    // than borrowed so a burst rings it once.
    void wake_loop();

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

    void drain_audio();
    void pump_audio(const std::shared_ptr<AudioNode>& node);

    void drain_decoded();
    void pump_decoded(const std::shared_ptr<DecodedNode>& node);
    void send_decoded_backlog(const std::shared_ptr<DecodedNode>& node);
    void send_decoded_ended(const std::shared_ptr<DecodedNode>& node, kj::StringPtr reason);
    void end_decode_route(const std::shared_ptr<DecodeRoute>& route, kj::StringPtr reason);

    // route.lock held. Stamps what the decoder left in route.scratch with the
    // receiver and the decoder's sequence and queues it on every node.
    void enqueue_decoded(DecodeRoute& route);

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
    //
    // IT GATES AN INSTALL AND NEVER A REMOVAL, and reading it as one flag
    // for both is how three separate sinks came to be left on the engine
    // for the rest of its life. stop() sets it while the loop thread is
    // still running and walks its sink maps only after joining that thread,
    // so a teardown in between erases its own entry and then finds itself
    // forbidden to detach, leaving stop()'s walk nothing to find.
    // end_rds_for_vrx, detach_passband_sink and end_audio each retract that
    // in place. A removal after the flag is set is always allowed: stop()
    // itself detaches after setting it.
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

    // The front end's verdict, and the decision it was last taken at.
    //
    // Under detect_lock_ with the detector because it reads the detector's
    // own arrays and has no state of its own that is worth a second lock. It
    // lives for the same span the detector does and is reset with it: after
    // a retune every segment is looking at a different piece of spectrum, so
    // the window's history is a measurement of somewhere else.
    //
    // last_front_end_decision_ is what keeps the monitor on the DECISION
    // rate rather than the frame rate. noise_floor() only moves when the
    // detector decides, so observing per frame would feed the regression
    // thirty copies of the same point and make its window a thirtieth as
    // long as it reads.
    detect::FrontEndMonitor front_end_;                    // detect_lock_
    dsp::SampleIndex last_front_end_decision_ = 0;         // detect_lock_
    bool have_front_end_decision_ = false;                 // detect_lock_

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

    // One route per receiver that has at least one audio subscriber. The map
    // is the loop thread's; each route's node list is shared with the
    // completion thread under that route's own lock.
    //
    // No spare-buffer pool here, unlike the two display streams. A chunk's
    // buffer belongs to the subscription that queued it, because two
    // subscribers on one receiver hold different numbers of chunks for
    // different lengths of time and a shared pool would be a queue with the
    // accounting hidden in it.
    std::map<std::uint32_t, std::shared_ptr<AudioRoute>> audio_routes_;

    // One decoder per receiver a client has asked about, keyed the same way
    // and owned the same way: the map is the loop thread's and each route's
    // contents are shared with the completion thread under that route's own
    // lock.
    //
    // Not pruned on its own. A receiver removed while a decoder was running
    // leaves its entry here until somebody polls it or removes it through
    // this session, which is the same residual Engine::attach_audio_sink
    // documents for its fan-out map and is bounded by the same thing: ids
    // are monotonic and never reused, so a stale entry can only be reached
    // by a caller naming an id it removed itself.
    std::map<std::uint32_t, std::shared_ptr<RdsRoute>> rds_routes_;

    // Loop thread only. Who created each receiver this server added, and the
    // counter the numbers come from. Counted from one so that zero can mean
    // "no session", and never reused for the life of the server, on the
    // ground receiver ids are never reused: a number that came back would
    // make a stale comparison look current.
    //
    // Pruned by after_vrx_removed and by release_source_state. A receiver the
    // engine removed on its own, which a front-end retune does to one it can
    // no longer reach, leaves its entry here until its session ends and the
    // removal is attempted, refused by the engine, and dropped. Ids are never
    // reused, so the stale entry cannot be mistaken for a live receiver.
    std::uint64_t next_session_ = 1;
    std::map<std::uint32_t, VrxOwner> vrx_owners_;

    // One route per decoder per receiver with at least one subscriber, keyed
    // by the receiver and the decoder's registry name. The map is the loop
    // thread's; each route's contents are shared with the completion thread
    // under that route's own lock, as audio_routes_ are.
    std::map<std::pair<std::uint32_t, std::string>, std::shared_ptr<DecodeRoute>>
        decode_routes_;

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

class AudioSubscriptionImpl final : public schema::AudioSubscription::Server {
public:
    AudioSubscriptionImpl(ServerImpl& owner, std::shared_ptr<AudioNode> node)
        : owner_(owner), node_(std::move(node)) {}

    AudioSubscriptionImpl(const AudioSubscriptionImpl&) = delete;
    AudioSubscriptionImpl& operator=(const AudioSubscriptionImpl&) = delete;

    // Not an override, for the reason SubscriptionImpl's destructor gives:
    // capnp::Capability::Server has no virtual destructor. This is what makes
    // a client that drops the capability without cancelling end the
    // subscription anyway, which is the case a client that crashed relies on.
    ~AudioSubscriptionImpl() { end(); }

    kj::Promise<void> cancel(CancelContext) override {
        end();
        return kj::READY_NOW;
    }

    kj::Promise<void> stats(StatsContext context) override {
        if (node_ == nullptr) {
            return to_exception(Error{"this audio subscription has been cancelled, so it has "
                                      "no counters left to report"});
        }

        // ENDED IS NOT CANCELLED AND THE NULL CHECK ABOVE DOES NOT COVER IT
        //
        // end() is what clears node_, and it runs for a cancel and for the
        // destructor. A subscription the SERVER ended leaves node_ set and
        // the node frozen at its last counts. Answering from it reports a
        // healthy subscription on a stream that stopped.
        //
        // THERE ARE TWO WAYS THE SERVER ENDS ONE, AND THIS USED TO NAME
        // ONE OF THEM. The paragraph read "which is the receiver being
        // removed out from under it" and the refusal below said "ended by
        // the engine ... AudioReceiver::ended carried the reason". The
        // other way is a chunk call this receiver failed, which pump_audio
        // cancels the node for, and until 2026-09-20 that path sent no
        // ended() at all: the client was told nothing, and then told by
        // this refusal that the engine had done it. The engine had no part
        // in it. pump_audio sends ended() with the call's own failure now,
        // and the wording here no longer attributes anything it cannot
        // know.
        //
        // core/rpc/client.h's client drops its capability the moment ended()
        // arrives, so it never reaches this line. The guard is here because
        // the schema is the contract and any client may hold a capability
        // across an ended.
        if (node_->cancelled) {
            return to_exception(
                Error{"this audio subscription was ended by the server rather than cancelled "
                      "by this client, so its counters are frozen at whatever the stream "
                      "stopped on. AudioReceiver::ended carried the reason, which is either "
                      "the receiver being removed or a chunk call this receiver failed"});
        }

        auto out = context.getResults().initStats();
        const std::scoped_lock held(node_->lock);
        out.setFramesSent(node_->frames_sent);
        out.setFramesDropped(node_->frames_dropped);
        out.setDropEvents(node_->drop_events);
        out.setBacklogFrames(node_->queued_frames);
        out.setBufferFrames(node_->buffer_frames);
        return kj::READY_NOW;
    }

private:
    void end() {
        if (node_ == nullptr) {
            return;
        }
        owner_.end_audio(node_);
        node_.reset();
    }

    ServerImpl& owner_;
    std::shared_ptr<AudioNode> node_;
};

class DecodedSubscriptionImpl final : public schema::DecodedSubscription::Server {
public:
    DecodedSubscriptionImpl(ServerImpl& owner, std::shared_ptr<DecodedNode> node)
        : owner_(owner), node_(std::move(node)) {}

    DecodedSubscriptionImpl(const DecodedSubscriptionImpl&) = delete;
    DecodedSubscriptionImpl& operator=(const DecodedSubscriptionImpl&) = delete;

    // Not an override, for the reason SubscriptionImpl's destructor gives.
    // This is what makes a client that drops the capability, or dies, end the
    // subscription and, with the last one, the decoder.
    ~DecodedSubscriptionImpl() { end(); }

    kj::Promise<void> cancel(CancelContext) override {
        end();
        return kj::READY_NOW;
    }

    kj::Promise<void> stats(StatsContext context) override {
        if (node_ == nullptr) {
            return to_exception(Error{"this decoded-message subscription has been cancelled, "
                                      "so it has no counters left to report"});
        }
        // Ended by the server and not cancelled, on AudioSubscriptionImpl's
        // argument: answering would report a healthy stream on one that has
        // stopped.
        if (node_->cancelled) {
            return to_exception(Error{
                "this decoded-message subscription was ended by the server rather than "
                "cancelled by this client, so its counters are frozen. DecodedReceiver::ended "
                "carried the reason"});
        }
        auto taken = owner_.decoded_stats(*node_);
        if (!taken) {
            return to_exception(taken.error());
        }
        auto out = context.getResults().initStats();
        out.setMessagesSent(taken->messages_sent);
        out.setMessagesDropped(taken->messages_dropped);
        out.setBacklog(taken->backlog);
        out.setChunksDiscarded(taken->chunks_discarded);
        out.setSampleRate(taken->sample_rate);
        return kj::READY_NOW;
    }

private:
    void end() {
        if (node_ == nullptr) {
            return;
        }
        owner_.end_decoded(node_);
        node_.reset();
    }

    ServerImpl& owner_;
    std::shared_ptr<DecodedNode> node_;
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
    explicit SessionImpl(ServerImpl& owner) : owner_(owner), id_(owner.open_session()) {}

    SessionImpl(const SessionImpl&) = delete;
    SessionImpl& operator=(const SessionImpl&) = delete;

    // THE RECEIVER LIFETIME RULE, and this destructor is the whole of how it
    // is enforced. Not an override, for the reason SubscriptionImpl's
    // destructor gives: capnp::Capability::Server has no virtual destructor,
    // and kj::heap disposes through this concrete type.
    //
    // It runs on the loop thread for every way a session can end: the client
    // dropping the capability, the connection closing cleanly, the connection
    // dying with the process at the other end, and the server itself stopping,
    // which tears down every connection it holds. ServerImpl::end_session has
    // what is removed and what is not.
    ~SessionImpl() { owner_.end_session(id_); }

    kj::Promise<void> info(InfoContext context) override {
        // Two reads and not one. info() is what the engine settled on when
        // it opened the source and is fixed for the run; source_pacing() is
        // measured now, and realtimeFactor is the whole reason this call is
        // worth polling more than once.
        write_engine_info(context.getResults().initInfo(), owner_.engine().info(),
                          owner_.engine().source_pacing());
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
        write_source_stats(context.getResults().initStats(), owner_.engine().source_stats(),
                           owner_.front_end(), owner_.engine().graph_conditions());
        return kj::READY_NOW;
    }

    kj::Promise<void> addVrx(AddVrxContext context) override {
        auto request = context.getParams();
        auto params = read_vrx_params(request.getParams());
        if (!params) {
            return to_exception(params.error());
        }
        auto id = owner_.engine().add_vrx(*params);
        if (!id) {
            return to_exception(id.error());
        }

        // Recorded before the answer goes out and on the same thread that
        // will run this session's destructor, so there is no interleaving in
        // which the receiver exists, the session ends, and nothing knew whose
        // it was.
        owner_.record_vrx(*id, id_, request.getKeep());
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

        // After the removal and not before: a removal that failed leaves the
        // receiver running and its subscribers listening to it.
        owner_.after_vrx_removed(*id, "the receiver was removed");
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

        // After the retune and only if it took. A receiver that moved is
        // pointed at a different transmitter, and PS, RadioText, the AF list
        // and the PI are that station's rather than this one's: keeping them
        // would assemble one station's text over another's, character by
        // character, with the A/B flag saying nothing changed.
        //
        // Every retune and not only one that moved the centre. The engine
        // takes a whole VrxParams and this server cannot tell "the same
        // frequency, a wider filter" from "a hundred kilohertz away" without
        // keeping its own copy of what was there, and a copy that went stale
        // would keep the wrong station's text at the one moment it matters.
        // A client that retunes without meaning to pays a reacquisition.
        //
        // The call above only QUEUED the retune, so this clears the decoder
        // AND fences the sample path against the frames the old tuning
        // already produced. reset_rds_for_vrx has why the clearing alone was
        // not enough.
        //
        // The status is read HERE and AFTER the retune, which is the whole
        // of the fence: VrxStatus::tuning_epoch is the epoch this
        // receiver's chunks will carry once the graph has applied
        // everything queued for it, so it is the number the decoder waits
        // to see. Reading it before the retune would fence against the
        // tuning being left.
        auto status = owner_.engine().vrx_status(*id);
        if (!status) {
            return to_exception(status.error());
        }
        owner_.reset_rds_for_vrx(*id, status->tuning_epoch);

        // Every event decoder on the receiver too, on the same fence and for
        // the same reason: a P25 header half from one transmitter and half
        // from another decodes to a talkgroup neither of them sent.
        owner_.reset_decoded_for_vrx(*id, status->tuning_epoch);
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
        auto out = context.getResults().initStatus();
        write_vrx_status(out, *status);

        // The server's half, which convert.cpp cannot write because the
        // engine does not know sessions exist.
        const VrxOwner owner = owner_.owner_of(*id);
        out.setCreatorSession(owner.session);
        out.setKept(owner.keep);
        out.setOwnedByCaller(owner.session != 0 && owner.session == id_);
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
        const double margin_bar = context.getParams().getMinMargin();

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

        // The same rule for the same reason. The margin map approaches one
        // without arriving, so a bar of one is a filter that can only be
        // empty.
        if (!std::isfinite(margin_bar) || margin_bar < 0.0 || margin_bar >= 1.0) {
            return to_exception(Error{std::format(
                "minMargin was {}, and it has to be from 0 up to but not including 1. The "
                "margin map is 1 - 0.5*exp(-(margin - threshold)/6), which approaches 1 "
                "without ever reaching it, so a bar of 1 lists nothing however strong the "
                "signal is",
                margin_bar)});
        }

        auto taken = owner_.detections(bar, margin_bar);
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

    kj::Promise<void> subscribeAudio(SubscribeAudioContext context) override {
        auto request = context.getParams();
        if (!request.hasReceiver()) {
            return to_exception(Error{"subscribeAudio needs a receiver capability, and this "
                                      "request carried a null pointer in its place"});
        }

        auto id = to_vrx_id(request.getVrx());
        if (!id) {
            return to_exception(id.error());
        }

        // Asked before the sink goes on, and its refusal is the engine's own
        // sentence: "no receiver N is registered" for an id that was never
        // issued or has been removed, and the source-not-open message when
        // there is no graph to ask. Both are what the operator needs to read.
        auto status = owner_.engine().vrx_status(*id);
        if (!status) {
            return to_exception(status.error());
        }

        // A complex tap, refused here rather than by the engine, because the
        // engine would happily install a sink on it. A client playing
        // interleaved I/Q as two-channel PCM plays noise at the wrong speed.
        //
        // BY MODE, because the four taps are not one path. The raw tap is
        // RawTapStage, one coarse channel at the channel rate, tens of times
        // what this design was costed at. The three digital voice modes went
        // through the fine stage on 2026-09-22 and come out mixed to DC and
        // resampled to the rate their decoder was built for, which
        // VrxStatus::demod_rate states; that stream has a reader, which is
        // subscribeDecoded. WHAT THIS REFUSAL USED TO SAY for all four: "is
        // a raw tap ... at the coarse channel rate", which named the wrong
        // path and the wrong rate for three of them.
        const engine::Demod mode = status->params.demod;
        if (!engine::produces_audio(mode)) {
            if (mode == engine::Demod::Raw) {
                return to_exception(Error{std::format(
                    "receiver {} is a raw tap, so there is no audio on it to subscribe to. The "
                    "raw tap is interleaved complex I/Q at the coarse channel rate, {} S/s, "
                    "rather than demodulated audio; an I/Q subscription is a separate method "
                    "that does not exist yet, and serving it through this one is the only "
                    "thing that would make it look like one",
                    id->value, status->placement.channel_rate)});
            }
            return to_exception(Error{std::format(
                "receiver {} is {}, which hands out complex baseband mixed to DC at {} S/s for "
                "a decoder rather than audio, so there is nothing on it to listen to. "
                "subscribeDecoded reads it: the {} decoder is attached to that receiver by "
                "passing an empty decoder name",
                id->value, engine::demod_name(mode), status->demod_rate,
                engine::demod_name(mode))});
        }

        const std::uint32_t granted = grant_audio_buffer_millis(request.getBufferMillis());
        auto node = std::make_shared<AudioNode>(request.getReceiver(), *id, granted);

        // The sink goes on before the capability exists, so a refusal comes
        // back as a sentence rather than as a subscription that can never
        // produce a chunk.
        if (auto installed = owner_.add_audio(node); !installed) {
            return to_exception(installed.error());
        }

        schema::AudioSubscription::Client handle =
            kj::heap<AudioSubscriptionImpl>(owner_, std::move(node));
        context.getResults().setSubscription(kj::mv(handle));
        context.getResults().setBufferMillisGranted(granted);
        return kj::READY_NOW;
    }

    // The two RDS surfaces.
    //
    // BOTH READ THEIR ARGUMENTS AND ANSWER ABOUT THEM, which is the
    // inversion this file used to describe in the other direction. Until
    // 2026-09-20 these were refused before the arguments were looked at, so
    // that a bad receiver id got "the surface is not wired" rather than "no
    // receiver 9 is registered": on a surface that does nothing, the second
    // reads as though a good id would have worked. On a surface that works
    // it is the true answer, and the same sentence that was misleading then
    // is the right one now. subscribeAudio made the same move for the same
    // reason.
    kj::Promise<void> rdsStation(RdsStationContext context) override {
        auto id = to_vrx_id(context.getParams().getVrx());
        if (!id) {
            return to_exception(id.error());
        }

        auto taken = owner_.rds_station(*id);
        if (!taken) {
            return to_exception(taken.error());
        }

        auto out = context.getResults().initStation();

        // The decode struct's own fields first, then the six that are not
        // its own. core/rpc/convert.h has the split and why it is there.
        write_rds_station(out, taken->state, taken->region);

        out.setVrx(id->value);
        out.setRegion(to_schema(taken->region));
        out.setCompositeRate(taken->composite_rate);
        out.setLastGroupSample(taken->last_group_sample);
        out.setTaChangedAt(taken->ta_changed_at);
        out.setFault(taken->fault);
        out.setDiscarding(taken->discarding);
        out.setDiscardedChunks(taken->chunks_discarded);

        auto health = out.initHealth();
        write_rds_bits_status(health, taken->bits);
        health.setSync(to_schema(taken->sync));
        health.setBitsFed(taken->bits_fed);
        health.setGroupsDecoded(taken->groups_decoded);
        health.setBlocksGood(taken->blocks_good);
        health.setBlocksCorrected(taken->blocks_corrected);
        health.setBlocksDropped(taken->blocks_dropped);
        health.setSyncAcquisitions(taken->sync_acquisitions);
        health.setSyncLosses(taken->sync_losses);
        return kj::READY_NOW;
    }

    kj::Promise<void> setSourceCenter(SetSourceCenterContext context) override {
        const std::int64_t wanted = context.getParams().getCenterHz();

        // The engine's call refuses in the SOURCE's own words on a source
        // that cannot retune, and each of the three backends says something
        // different about what to do instead. Nothing is composed here.
        auto landed = owner_.engine().set_source_center(wanted);
        if (!landed) {
            return to_exception(landed.error());
        }

        // Every receiver the engine removed gets the cleanup removeVrx gives
        // one, because it has been removed just as surely. Before 2026-09-23
        // this answer was discarded and none of that happened: an audio
        // subscriber went quiet with no ended(), which is what a shut
        // squelch sounds like, and the RDS route, the decoder routes and the
        // ownership record stayed behind. The reason names both frequencies,
        // since the operator knows the receiver by where it was and the
        // retune by where it went.
        auto removed = context.getResults().initRemoved(
            static_cast<unsigned int>(landed->removed.size()));
        for (unsigned int i = 0; i < removed.size(); ++i) {
            const engine::RetuneRemoval& gone = landed->removed[i];
            const std::string reason = std::format(
                "the front end was retuned to {} Hz, which leaves this receiver's centre at {} "
                "Hz outside the span, so the engine removed it",
                landed->center, gone.frequency);
            owner_.after_vrx_removed(gone.id, kj::StringPtr(reason.c_str()));

            removed[i].setVrx(gone.id.value);
            removed[i].setFrequencyHz(gone.frequency);
        }

        // After the tune and only if it took, exactly as setVrxParams
        // clears the decoders after its own. The engine has already moved
        // every receiver's tuning epoch; this is the half above the engine,
        // which is the detector's tracks and the decoders' accumulated
        // stations.
        owner_.forget_across_retune();

        context.getResults().setGrantedHz(landed->center);
        return kj::READY_NOW;
    }

    kj::Promise<void> sourceDescriptor(SourceDescriptorContext context) override {
        auto results = context.getResults();

        // No source is a state and not a failure: a client polls through the
        // window between engine start and the first openSource, and a refusal
        // there would have it reporting a fault for an engine that is fine.
        const bool open = owner_.engine().has_source();
        results.setOpen(open);
        if (!open) {
            return kj::READY_NOW;
        }

        // The same writer listSources uses, on the capabilities the engine
        // already holds for the source it opened. No device is touched: this is
        // a read of what open_source already learned, which is the whole
        // difference between this call and listSources.
        write_source_descriptor(results.initSource(), owner_.engine().source_capabilities());
        return kj::READY_NOW;
    }

    kj::Promise<void> setSourceGain(SetSourceGainContext context) override {
        auto params = context.getParams();
        const capnp::Text::Reader stage = params.getStage();

        // NOT forget_across_retune, which setSourceCenter calls. Gain changes
        // how loud the samples are and not what any frequency means, so the
        // detector's tracks and the decoders' accumulated stations are still
        // about the signals they were about. Dropping them here would throw
        // away an operator's RDS text every time they nudged a slider.
        auto landed = owner_.engine().set_source_gain(
            std::string_view(stage.begin(), stage.size()), params.getDb());
        if (!landed) {
            return to_exception(landed.error());
        }

        context.getResults().setGrantedDb(*landed);
        return kj::READY_NOW;
    }

    kj::Promise<void> setSourceGainAuto(SetSourceGainAutoContext context) override {
        auto params = context.getParams();
        const capnp::Text::Reader stage = params.getStage();

        if (auto applied = owner_.engine().set_source_gain_auto(
                std::string_view(stage.begin(), stage.size()), params.getOn());
            !applied) {
            return to_exception(applied.error());
        }
        return kj::READY_NOW;
    }

    kj::Promise<void> sourceCanRetune(SourceCanRetuneContext context) override {
        const engine::SourceTuning tuning = owner_.engine().source_tuning();
        auto results = context.getResults();
        results.setCanRetune(tuning.can_retune);
        results.setLowHz(tuning.low);
        results.setHighHz(tuning.high);
        return kj::READY_NOW;
    }

    kj::Promise<void> openSource(OpenSourceContext context) override {
        const capnp::Text::Reader uri = context.getParams().getUri();

        // Before the engine, because the engine's own refusal for an empty URI
        // is whatever source::open_source makes of an empty string, and that
        // reads as a parse failure rather than as a caller who sent nothing.
        if (uri.size() == 0) {
            return to_exception(
                Error{"openSource was given an empty URI. listSources hands back the string to "
                      "start from in SourceDescriptor::uri; append the settings an operator "
                      "chose to it rather than composing one from scratch"});
        }

        if (auto opened = owner_.open_source(std::string_view(uri.begin(), uri.size()));
            !opened) {
            return to_exception(opened.error());
        }
        return kj::READY_NOW;
    }

    kj::Promise<void> closeSource(CloseSourceContext context) override {
        // The context is unused and named anyway, because the generated
        // signature requires the parameter and a nameless one would read as
        // an oversight rather than as a call with no results.
        static_cast<void>(context);

        if (auto closed = owner_.close_source(); !closed) {
            return to_exception(closed.error());
        }
        return kj::READY_NOW;
    }

    kj::Promise<void> setRdsRegion(SetRdsRegionContext context) override {
        auto request = context.getParams();

        auto id = to_vrx_id(request.getVrx());
        if (!id) {
            return to_exception(id.error());
        }

        // The region before the receiver, which is the one place in this
        // pair the argument order matters. An ordinal this build cannot name
        // means the caller was built against a newer schema, and answering
        // that with a receiver's problem would send whoever read it to the
        // radio.
        auto region = from_schema(request.getRegion());
        if (!region) {
            return to_exception(region.error());
        }

        if (auto applied = owner_.set_rds_region(*id, *region); !applied) {
            return to_exception(applied.error());
        }
        return kj::READY_NOW;
    }

    kj::Promise<void> decoders(DecodersContext context) override {
        const std::span<const DecoderSpec> registry = decoder_registry();
        auto out = context.getResults().initDecoders(static_cast<unsigned>(registry.size()));
        for (unsigned i = 0; i < out.size(); ++i) {
            write_decoder_info(out[i], registry[i].name, registry[i].input,
                               registry[i].description);
        }
        return kj::READY_NOW;
    }

    kj::Promise<void> subscribeDecoded(SubscribeDecodedContext context) override {
        auto request = context.getParams();
        if (!request.hasReceiver()) {
            return to_exception(Error{"subscribeDecoded needs a receiver capability, and this "
                                      "request carried a null pointer in its place"});
        }

        auto id = to_vrx_id(request.getVrx());
        if (!id) {
            return to_exception(id.error());
        }

        // The engine's own words for a receiver that is not there, before the
        // decoder name is looked at: "no receiver 9" is the true answer
        // whatever was asked of it.
        auto status = owner_.engine().vrx_status(*id);
        if (!status) {
            return to_exception(status.error());
        }

        const capnp::Text::Reader asked_text = request.getDecoder();
        const std::string_view asked(asked_text.begin(), asked_text.size());
        const engine::Demod mode = status->params.demod;

        // Empty means the decoder named after the receiver's mode. Resolved
        // here rather than on the client, which does not know which modes
        // have a decoder, and answered back in decoderResolved so the client
        // can say which one ran.
        const DecoderSpec* spec =
            find_decoder(asked.empty() ? std::string_view(engine::demod_name(mode)) : asked);
        if (spec == nullptr) {
            if (asked.empty()) {
                // A usb or nfm receiver has no decoder named after its mode
                // and may well have several that read it, and naming those
                // is the answer the operator was looking for.
                const std::string readers = decoders_reading(engine::demod_name(mode));
                if (!readers.empty()) {
                    return to_exception(Error{std::format(
                        "receiver {} is {} and no decoder is named after that mode, so there is "
                        "nothing to attach by default: a {} receiver can carry more than one "
                        "protocol. Name one; the decoders that read {} audio are {}",
                        id->value, engine::demod_name(mode), engine::demod_name(mode),
                        engine::demod_name(mode), readers)});
                }
                return to_exception(Error{std::format(
                    "receiver {} is {} and no decoder is named after that mode, so there is "
                    "nothing to attach by default. Name one; this engine has {}",
                    id->value, engine::demod_name(mode), decoder_names())});
            }
            return to_exception(Error{std::format(
                "this engine has no decoder named '{}'. It has {}", asked, decoder_names())});
        }

        // The modes an audio decoder reads, asked before the input check
        // below because it is the more specific answer: RTTY on a wfm
        // receiver and RTTY on a raw tap are both refused here, naming the
        // sideband the decoder needs, rather than one of them being told only
        // that a complex tap has no audio.
        if (!decoder_accepts(*spec, engine::demod_name(mode))) {
            return to_exception(Error{std::format(
                "the {} decoder reads {} and receiver {} is {}. Add a receiver in {} on the "
                "signal",
                spec->name, decoder_needs_text(*spec), id->value, engine::demod_name(mode),
                decoder_modes_text(*spec))});
        }

        // The input the decoder reads against what the receiver gives. Checked
        // here, in words naming both, because the alternative is a decoder
        // fed the wrong shape and a stream that ends one chunk later with a
        // sentence about channel counts.
        const bool complex_tap = engine::is_complex_tap(mode);
        if (spec->input == DecoderInput::ComplexBaseband && !complex_tap) {
            return to_exception(Error{std::format(
                "the {} decoder reads complex baseband and receiver {} is {}, which produces "
                "audio. Add a receiver in a complex tap mode, raw or {}, on the signal",
                spec->name, id->value, engine::demod_name(mode), spec->name)});
        }
        if (spec->input == DecoderInput::RealAudio && complex_tap) {
            return to_exception(Error{std::format(
                "the {} decoder reads audio and receiver {} is {}, a complex tap that produces "
                "none",
                spec->name, id->value, engine::demod_name(mode))});
        }

        auto node = std::make_shared<DecodedNode>(request.getReceiver(), *id, spec->name);

        // The sink goes on before the capability exists, so a refusal comes
        // back as a sentence rather than as a subscription that never
        // delivers.
        if (auto added = owner_.add_decoded(node, *status, *spec); !added) {
            return to_exception(added.error());
        }

        schema::DecodedSubscription::Client handle =
            kj::heap<DecodedSubscriptionImpl>(owner_, std::move(node));
        auto results = context.getResults();
        results.setSubscription(kj::mv(handle));
        // Copied into a std::string for its terminator: capnp::Text::Reader
        // over a pointer and a length asserts a NUL at the end, which a
        // string_view does not promise. end_audio_for_vrx's note has the
        // same trap in kj::StringPtr.
        const std::string resolved(spec->name);
        results.setDecoderResolved(resolved.c_str());
        return kj::READY_NOW;
    }

private:
    ServerImpl& owner_;

    // This login's number, which is what VrxStatus::creatorSession names. Not
    // a capability and not a secret: a session proves itself by being held,
    // and the number only lets two receivers' creators be compared.
    const std::uint64_t id_;
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
// A SessionImpl holds a reference to the server and its own login number, so a
// second one costs nothing and two of them cannot disagree about the radio.
// That is what makes a second login on one connection an ordinary success
// rather than a case to defend against: the caller has already proved it holds
// the token, and a second capability grants it nothing it did not have. The
// Session's lifetime becomes the client's, and any still alive at shutdown die
// with the TwoPartyServer local, on this thread, exactly as the single
// bootstrap Session used to.
//
// The two sessions differ in one respect since 2026-09-22, which is what they
// own: each takes the receivers IT created with it when it ends, so dropping
// one of two sessions on a connection removes that one's receivers and leaves
// the other's. SessionImpl's destructor has the rule.
//
// WHAT THIS PARAGRAPH USED TO SAY: "SessionImpl's only member is ServerImpl&".
// It gained the login number with the receiver lifetime rule, and a reader who
// took the old sentence as licence to mint sessions freely inside the server
// would now be minting owners.
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

        // Not kj's own parseAddress and listen(), which set SO_REUSEADDR and
        // on Windows let a second server bind this port beside the first.
        // core/rpc/listen.h has the measurement.
        auto exclusive =
            listen_exclusive(*io.lowLevelProvider, options.bind_address, options.port);
        if (!exclusive) {
            announce(std::unexpected(with_context(exclusive.error(), "the RPC server")));
            return;
        }
        auto listener = kj::mv(*exclusive);

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

        // Audio subscriptions hold a capability made on this thread, so they
        // are dropped here rather than left to stop(), which runs on the
        // caller's. The routes themselves stay: stop() detaches their engine
        // sinks and closes them once this thread has been joined.
        for (const auto& entry : audio_routes_) {
            const std::scoped_lock owned(entry.second->lock);
            entry.second->nodes.clear();
        }

        // Decoded-message subscribers hold one too, on the same terms.
        for (const auto& entry : decode_routes_) {
            const std::scoped_lock owned(entry.second->lock);
            entry.second->nodes.clear();
        }

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

void ServerImpl::observe_front_end() {
    // detect_lock_ is held by the caller and detector_ has a value.
    const dsp::SampleIndex decided = detector_->last_decision();
    if (have_front_end_decision_ && decided == last_front_end_decision_) {
        return;
    }

    const dsp::SampleRate rate = detector_->config().source_rate;
    if (!have_front_end_decision_ || rate == 0 || decided <= last_front_end_decision_) {
        // The first decision has no interval behind it, and an index that
        // did not advance is the detector before it has decided at all.
        // Recording the position without observing is what makes the next
        // decision's interval the real one rather than a span reaching back
        // to sample zero.
        last_front_end_decision_ = decided;
        have_front_end_decision_ = true;
        return;
    }

    const double elapsed = static_cast<double>(decided - last_front_end_decision_) /
                           static_cast<double>(rate);
    last_front_end_decision_ = decided;

    // Source seconds and not wall seconds, so a capture replayed at forty
    // times realtime produces the slope it did live. Same rule detector.h
    // states for every interval on this path.
    //
    // A refusal is dropped rather than recorded. The monitor rejects a
    // geometry the detector cannot produce, so there is no reachable fault
    // here that is not already a detector fault, and this must not be the
    // thing that switches detection off.
    (void)front_end_.observe(detector_->averaged_power(), detector_->noise_floor(), elapsed);
}

detect::FrontEndObservation ServerImpl::front_end() {
    std::scoped_lock held(detect_lock_);

    // An Unmeasured reading and not the last one the monitor took, because
    // without a detector there is nothing feeding it and a stale verdict
    // would keep answering for a band nobody is watching any more.
    if (!detector_.has_value()) {
        return {};
    }
    return front_end_.observation();
}

Expected<DetectionSnapshot> ServerImpl::detections(double min_confidence, double min_margin) {
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
    //
    // BOTH BARS, AND A TRACK HAS TO CLEAR BOTH. They ask independent questions
    // and a caller wanting one passes zero for the other, so anding them is
    // what makes "persistent AND strong" expressible without a third call.
    out.tracks.reserve(tracks.size());
    for (const detect::Track& track : tracks) {
        if (track.confidence >= min_confidence && track.margin_confidence >= min_margin) {
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

// ---------------------------------------------------------------------------
// RDS
// ---------------------------------------------------------------------------

namespace {

// The four conditions core/rpc/revenant.capnp states, asked in order and
// answered one at a time.
//
// THE NATURAL BAR IS THE RATE ALONE AND THE RATE ALONE CERTIFIES ONE SHAPE
// OUT OF FOUR. A receiver on the right rate with an AM detector in front of
// it produces audio with no subcarrier anywhere; a WFM receiver at 171000
// whose passband was clamped to a channel too narrow to hold 59375 Hz
// produces a composite with the data band cut off. Neither decodes, neither
// faults, and both look exactly like a station that has no RDS.
[[nodiscard]] Status rds_receiver_is_suitable(const engine::VrxStatus& status) {
    const std::uint32_t id = status.id.value;
    const engine::Demod mode = status.params.demod;

    // 1. Only a discriminator produces a composite. NFM is admitted beside
    //    WFM: core/shaders/vrx_demod.comp reaches the same atan2 branch for
    //    both, only the gain differs, and the decoder is scale invariant.
    if (mode != engine::Demod::Nfm && mode != engine::Demod::Wfm) {
        return fail(std::format(
            "receiver {} is {} and RDS is decoded from an FM composite, which only a "
            "discriminator produces. An envelope detector and the four product detectors "
            "give audio with no 57 kHz subcarrier in it at any rate. Point a receiver with "
            "demod wfm at the station",
            id, engine::demod_name(mode)));
    }

    // 2. Real and mono. Today this is exactly what condition 1 already
    //    excluded, because the raw tap is the only thing in the tree that
    //    writes an interleaved complex pair and it is not a discriminator.
    //    There is nothing left to ask here, so nothing is asked; the guard
    //    that survives is in on_rds_chunk, which refuses a chunk whose
    //    channel count is not one rather than reading I and Q as
    //    consecutive samples of a signal that does not exist. Stated so
    //    that a reader counting four conditions against three checks knows
    //    which one moved and where it went.

    // 3. The audio rate, against the RECEIVER's bound rather than the
    //    decoder's.
    // EFFECTIVE AND NOT THE ECHO. VrxStatus::resolved_audio_rate is the rate
    // the receiver actually runs at, and vrx.h says of it in as many words:
    // "Everything below that asks a question OF the audio rate asks it here."
    // This guard asked the echo, which is zero whenever the request named no
    // rate, so it refused every receiver created the ordinary way.
    //
    // WHAT THIS REFUSAL USED TO SAY, and its premise was false. It read
    // "receiver {} took the engine's default audio rate, which is not a number
    // this server can read: VrxParams::audioRate comes back as the verbatim
    // zero that was sent and EngineInfo does not carry the default." The
    // server can read it. It is on the status this function already holds, put
    // there for exactly this reason after applied_deemphasis and
    // decoding_stereo were caught answering against the echo. Only this guard
    // was left asking the wrong field.
    //
    // The answer usually still refuses, because the default resolves to 48000
    // and 48000 cannot carry a 57 kHz subcarrier. What changes is that the
    // refusal below now names the rate and the bound, which is something an
    // operator can act on, rather than reporting that the number is unknowable.
    const dsp::SampleRate audio_rate = status.effective_audio_rate();
    if (audio_rate <= 0) {
        // A default-constructed status and not a receiver: resolved_audio_rate
        // is zero when nobody filled it in, and the echo is zero when the
        // request named no rate, so both being zero means there is nothing
        // here to ask about.
        return fail(std::format(
            "receiver {} reports no audio rate at all, neither the rate it was asked for nor "
            "the rate it resolved to. That is a status nothing filled in rather than a "
            "receiver running at an unusable rate, so the receiver is the thing to check",
            id));
    }

    const dsp::SampleRate demod_rate = status.demod_rate;
    const bool decimated = demod_rate > audio_rate;
    const dsp::SampleRate bound =
        decimated ? kFilteredCompositeRateHz : decode::kMinimumRateHz;
    if (audio_rate < bound) {
        if (decimated) {
            return fail(std::format(
                "receiver {} runs at {} S/s of audio and a composite from a receiver needs "
                "at least {}. It demodulates at {} and decimates by {}, so it has an audio "
                "filter whose passband edge is 0.4 of the audio rate, which is {} Hz: the "
                "composite reaches {} Hz and the top of the data band is already in the "
                "stopband. The decoder's own bound is {}, and it is lower because a "
                "composite from a file or a modulator went through no such filter. State "
                "171000 on the receiver: it is three times the 57 kHz subcarrier and 144 "
                "times the 1187.5 bit/s bit rate, both exact",
                id, audio_rate, kFilteredCompositeRateHz, demod_rate,
                demod_rate / std::max<dsp::SampleRate>(audio_rate, 1),
                (audio_rate * 2) / 5, kCompositeTopHz, decode::kMinimumRateHz));
        }
        return fail(std::format(
            "receiver {} runs at {} S/s of audio and the composite reaches {} Hz, so the "
            "decoder needs at least {}. Its decimation resolved to one, so the planner "
            "designed no audio filter and this is the decoder's own bound rather than the "
            "receiver's higher one. State 171000 on the receiver: it is three times the "
            "57 kHz subcarrier and 144 times the 1187.5 bit/s bit rate, both exact",
            id, audio_rate, kCompositeTopHz, decode::kMinimumRateHz));
    }

    // 4. The granted passband, read off the placement rather than the
    //    request, because each edge is fitted on its own and a request too
    //    wide on one side keeps the other edge where it was.
    //
    //    NECESSARY AND NOT SUFFICIENT, which is why this is a bar and not a
    //    prediction. Carson for a multiplex deviating 75 kHz and reaching
    //    59375 Hz is about 268750 Hz, so the ordinary 200 kHz broadcast
    //    passband already truncates the sidebands and a receiver at the
    //    bare minimum here will decode worse than one at 200000. The health
    //    counters are how a client finds that out.
    const engine::VrxPlacement& placement = status.placement;
    if (placement.granted_low > -kCompositeTopHz ||
        placement.granted_high < kCompositeTopHz) {
        return fail(std::format(
            "receiver {} was granted the passband {} to {} Hz about its centre and the FM "
            "composite reaches {} Hz either side, so the data band is outside the filter. "
            "{}Widen the receiver, or place it on a grid with wider channels: one channel is "
            "{} S/s here and a receiver cannot be given more than half of that either side "
            "of where it sits",
            id, placement.granted_low, placement.granted_high, kCompositeTopHz,
            placement.bandwidth_clamped
                ? "The request was clamped to what one grid channel can carry. "
                : "",
            placement.channel_rate));
    }

    return {};
}

// The engine's completion thread, with route.lock held by the callable that
// got here.
//
// A free function and not a ServerImpl member, which is the whole of the
// difference from on_audio_chunk and is worth the line it costs to say: it
// reaches nothing but the route it is handed, so there is no owner pointer
// to check and no gate to close. A sink call still running when the Server
// is destroyed writes into a route the callable itself keeps alive.
//
// Returns void because there is nothing it could usefully refuse. The
// decoder has no error channel of its own, and the graph turns a refusing
// sink into a failing dispatch, so the two things that can go wrong are
// recorded in the route and reported to the next poll instead.
void decode_rds_chunk(RdsRoute& route, const engine::AudioChunk& chunk) {
    if (!route.fault.empty()) {
        return;
    }

    // THE RETUNE FENCE, and it is asked BEFORE the two shape checks below.
    // A chunk recorded at the old tuning is still a chunk this receiver
    // produced at this receiver's shape, so faulting on it says nothing the
    // next chunk will not say; discarding it first keeps the fence one rule
    // rather than a rule with an exception in front of it.
    //
    // The epoch is recorded whether the chunk is decoded or discarded,
    // because it is what says the fence has been crossed. See
    // RdsRoute::epoch_target for the whole of the arrangement.
    //
    // A MAXIMUM AND NOT AN ASSIGNMENT. The graph delivers chunks in
    // submission order on one completion thread, so this is nondecreasing
    // already and the max costs a comparison; what it buys is that the
    // fence cannot be reopened by a chunk arriving out of order, which is
    // the failure that would be silent if the ordering guarantee ever
    // changed.
    route.epoch_reached = std::max(route.epoch_reached, chunk.tuning_epoch);
    if (route.discarding()) {
        // Recorded before the retune landed. Discarding it is the point:
        // the decoder was cleared for the new tuning the moment the client
        // asked, and these samples are the transmitter it left.
        ++route.chunks_discarded;
        return;
    }

    // The two things a chunk can be that the decoder was not built for. Both
    // are recorded rather than returned: a sink that refuses fails the
    // dispatch and ends the run, and losing the radio over a decoder is the
    // wrong trade for the same reason it is wrong for the detector.
    if (chunk.channels != 1) {
        route.fault = std::format(
            "receiver {} delivered {} interleaved channels and the decoder was built for a "
            "real mono composite. Reading an interleaved pair as consecutive samples "
            "decodes a signal that does not exist, so this decoder stopped",
            route.vrx.value, chunk.channels);
        return;
    }
    if (chunk.rate != route.composite_rate) {
        route.fault = std::format(
            "receiver {} delivered audio at {} S/s and the decoder was built for {}. Its "
            "loops are sized by the rate, so the ones running now are for a rate the "
            "receiver is not producing, and this decoder stopped rather than reporting a "
            "subcarrier offset that is an artefact of the mismatch",
            route.vrx.value, chunk.rate, route.composite_rate);
        return;
    }

    // A muted chunk is fed like any other. core/engine/graph.cpp writes
    // zeros into the readback buffer when the gate is shut, and those zeros
    // are what the receiver produced: skipping them would take the
    // composite timeline out of step with samplesConsumed, which is the unit
    // both sample indices on the wire are stated in. A gated receiver simply
    // loses lock, which is the truth about what reached it.

    // One std::function built per chunk and not per sample. It captures one
    // pointer, which MSVC's small-object buffer holds inline, so the
    // per-chunk cost is a construction and no allocation.
    route.bits.process(chunk.samples, [&route](bool bit) {
        const std::uint64_t before = route.groups.groups_decoded();
        route.groups.feed(bit);
        if (route.groups.groups_decoded() == before) {
            return;
        }

        // Read inside the sink rather than after the chunk, so the index is
        // the sample the group's last bit came out of rather than the end of
        // whatever buffer happened to carry it. A chunk is about 12 ms of
        // composite at the block sizes this tree runs and a group is 87.6
        // ms, so chunk resolution would have been visible.
        const std::uint64_t at = route.bits.status().samples_consumed;
        route.last_group_sample = at;

        // TA can only move when a group completes, so it is asked here
        // rather than per bit.
        const decode::StationState& state = route.groups.state();
        if (!state.ta_valid) {
            return;
        }
        if (!route.ta_seen) {
            route.ta_seen = true;
            route.last_ta = state.ta;
            return;
        }
        if (state.ta != route.last_ta) {
            route.last_ta = state.ta;
            route.ta_changed_at = at;
        }
    });
}

}  // namespace

Expected<RdsSnapshot> ServerImpl::rds_station(engine::VrxId vrx) {
    // The receiver first, on every poll and not only on the first. It is one
    // map lookup in the graph and it is what makes a decoder outliving its
    // receiver impossible to read: a removal by any path, including one this
    // session never saw, is answered in the engine's own words here and the
    // decoder is dropped on the way out.
    auto status = engine_.vrx_status(vrx);
    if (!status) {
        end_rds_for_vrx(vrx);
        return std::unexpected(status.error());
    }

    std::shared_ptr<RdsRoute> route;
    if (auto found = rds_routes_.find(vrx.value); found != rds_routes_.end()) {
        route = found->second;
    } else {
        // THE DEFAULT REGION IS STATED HERE AND NOWHERE ELSE, and it is RDS.
        // A client that has not called setRdsRegion gets EN 50067's reading
        // of the bitstream, reads it back off RdsStation::region, and can
        // change it. There is no inference from the tuned frequency and
        // there is deliberately none: core/decode/rds_groups.h has the
        // argument, and a server that guessed would be making a setting
        // look like a measurement.
        auto built = start_rds(*status, decode::Region::kRds);
        if (!built) {
            return std::unexpected(built.error());
        }
        route = std::move(*built);
    }

    const std::scoped_lock held(route->lock);

    // A FAULTED DECODER ANSWERS RATHER THAN REFUSING, and it is the one
    // place this departs from the detector's precedent.
    //
    // Until 2026-09-20 this returned fail(route->fault), which put the
    // sentence on the wire as an exception. Two things were wrong with that.
    // It threw away everything the decoder had accumulated before the bad
    // chunk, which is still true about the station and is the only record of
    // it there will ever be, since the fault is terminal. And it put "this
    // decoder stopped" on the same channel as "no receiver 9 is registered",
    // so a client telling them apart had to match prose.
    //
    // The detector keeps refusing and should: its fault means consume()
    // rejected a geometry, so the track list no longer describes anything.
    // An RDS fault leaves a station struct that was true when it was last
    // written, and freezing a true thing is not the same as holding a false
    // one. RdsStation::fault on the wire is what says which it is.
    RdsSnapshot out;
    out.fault = route->fault;
    out.state = route->groups.state();
    out.bits = route->bits.status();
    out.region = route->region;
    out.sync = route->groups.sync_state();
    out.bits_fed = route->groups.bits_fed();
    out.groups_decoded = route->groups.groups_decoded();
    out.blocks_good = route->groups.blocks_good();
    out.blocks_corrected = route->groups.blocks_corrected();
    out.blocks_dropped = route->groups.blocks_dropped();
    out.sync_acquisitions = route->groups.sync_acquisitions();
    out.sync_losses = route->groups.sync_losses();
    out.composite_rate = route->composite_rate;
    out.last_group_sample = route->last_group_sample;
    out.ta_changed_at = route->ta_changed_at;
    out.discarding = route->discarding();
    out.chunks_discarded = route->chunks_discarded;
    return out;
}

// ENGINE-WIDE THROUGH THE RECEIVER, which is a decision and not an oversight.
//
// rds_routes_ is keyed by receiver and not by session, so two clients polling
// one receiver share one decoder and this call from either clears what the
// other accumulated. set_detection_threshold is engine-wide and says so; this
// is the same choice made at the width of the thing it configures. There is
// one detector, so a threshold is per engine. There is one decoder per
// receiver, so a region is per receiver.
//
// The receiver is what makes that right. It is already shared state: two
// sessions on one share its centre, its filter and its squelch, and
// setVrxParams from either already clears the other's station through
// reset_rds_for_vrx. A per-session decoder would put one decode per client
// per receiver on the completion thread, which is the cost the whole
// build-on-first-ask arrangement exists to hold down, and it would let two
// clients disagree about a station that is one station. A client that wants
// its own region adds its own receiver, which it is already doing to reach a
// composite rate.
Status ServerImpl::set_rds_region(engine::VrxId vrx, decode::Region region) {
    auto status = engine_.vrx_status(vrx);
    if (!status) {
        end_rds_for_vrx(vrx);
        return std::unexpected(status.error());
    }

    auto found = rds_routes_.find(vrx.value);
    if (found == rds_routes_.end()) {
        // Built at the region asked for rather than built at the default and
        // then reset, so the client presetting a region pays nothing and
        // learns about an unsuitable receiver now.
        auto built = start_rds(*status, region);
        if (!built) {
            return std::unexpected(built.error());
        }
        return {};
    }

    const std::shared_ptr<RdsRoute>& route = found->second;
    const std::scoped_lock held(route->lock);

    route->region = region;

    // BOTH LAYERS, INCLUDING THE ONE THE REGION DOES NOT REACH.
    //
    // The physical layer knows nothing about regions, so resetting it costs
    // a reacquisition, about half a second, for no decoding benefit. It is
    // reset anyway because every counter in RdsHealth is documented as
    // cumulative from the moment the decoder was built, and a client
    // differences two polls to get a rate. Clearing the block layer's
    // counters and not the bit layer's would leave one struct with two
    // epochs in it, and a difference taken across the change would divide
    // one layer's delta by the other's elapsed samples.
    //
    // The same reset also puts samplesConsumed back to zero, which is the
    // unit lastGroupSample and taChangedAt are stated in, so those two go
    // with it.
    route->bits.reset();
    route->groups = decode::RdsDecoder(region);
    route->last_group_sample = 0;
    route->ta_changed_at = 0;
    route->ta_seen = false;
    route->last_ta = false;

    // AND NO FENCE, which is the one thing this does not share with
    // reset_rds_for_vrx. A region change does not move the receiver, so the
    // chunks in flight are the same transmitter this decoder was already
    // listening to and feeding them to the rebuilt decoder is right. What
    // changed is the reading of the bytes, and none of those bytes has
    // arrived yet.
    //
    // The fault is NOT cleared, on the same terms reset_rds_for_vrx states:
    // a fault here means the receiver delivered a chunk that was not what
    // the decoder was built for, the two things that can be wrong are both
    // shape, and a region is not a shape. Clearing it would let a receiver
    // that has already delivered the wrong thing look healthy for one poll.
    return {};
}

Expected<std::shared_ptr<RdsRoute>> ServerImpl::start_rds(const engine::VrxStatus& status,
                                                          decode::Region region) {
    if (auto suitable = rds_receiver_is_suitable(status); !suitable) {
        return std::unexpected(suitable.error());
    }

    decode::RdsBitsConfig config;
    config.rate = status.params.audio_rate;

    auto sync = decode::RdsBitSync::create(config);
    if (!sync) {
        return std::unexpected(with_context(
            sync.error(),
            std::format("building the RDS decoder for receiver {}", status.id.value)));
    }

    auto route = std::make_shared<RdsRoute>(
        status.id, static_cast<std::uint32_t>(status.params.audio_rate), region,
        std::move(*sync));

    // THE FENCE STARTS WHERE THE RECEIVER IS, not at zero. A decoder built
    // on a receiver that has been retuned nine times must not accept a
    // chunk stamped with an older epoch, and the attach cannot rule one out
    // on ordering alone: it is a control op like the retune, so reasoning
    // about which of the two the recording thread reaches first is exactly
    // the kind of argument that stops being true when one of them moves.
    // Reading the target off the receiver makes the question local.
    route->epoch_target = status.tuning_epoch;

    // sink_lock_ is held ACROSS the attach and not merely checked before it,
    // which is what add_audio does and for the same reason. stop() sets
    // sink_closed_ under this lock while the loop thread is still running,
    // so a check that released the lock first would leave a window in which
    // this installs a sink stop() had already decided to install no more of.
    // That window closes either way here, because stop() joins the loop
    // before it walks rds_routes_ and would find the late entry, but a
    // correctness argument that rests on the order of two unrelated
    // functions is the kind that stops being true when one of them moves.
    const std::scoped_lock held(sink_lock_);
    if (sink_closed_) {
        return fail("this server is stopping and will install no further sinks");
    }

    // attach rather than set, which is the seam core/engine/engine.h exists
    // for: a recording, a loudspeaker or an audio subscription already on
    // this receiver keeps its audio and this decoder joins beside it.
    auto attached = engine_.attach_audio_sink(
        status.id, [route](const engine::AudioChunk& chunk) -> Status {
            const std::scoped_lock owned(route->lock);
            decode_rds_chunk(*route, chunk);
            return {};
        });
    if (!attached) {
        return std::unexpected(attached.error());
    }
    route->sink = *attached;

    rds_routes_.emplace(status.id.value, route);
    return route;
}

void ServerImpl::end_rds_for_vrx(engine::VrxId vrx) {
    auto found = rds_routes_.find(vrx.value);
    if (found == rds_routes_.end()) {
        return;
    }

    auto route = found->second;
    rds_routes_.erase(found);

    // UNCONDITIONALLY, AND sink_lock_ IS NOT TAKEN FOR IT.
    //
    // This used to run under sink_lock_ and only when sink_closed_ was
    // clear, with a comment justifying the discarded return value and saying
    // nothing about the skip. There is no ordering that makes the skip safe,
    // and the one that looks like it does is the one that breaks it: stop()
    // sets sink_closed_ under sink_lock_ while the loop thread is STILL
    // RUNNING, and only walks rds_routes_ after joining it. This function is
    // loop-thread only and erases the route from that map BEFORE detaching.
    // So a removeVrx handled in the window between the flag and the join
    // erased the entry, skipped the detach, and left stop()'s walk nothing
    // to find: the engine kept the callable, the callable kept the route
    // alive through its own shared_ptr, and the decoder ran on for the life
    // of the engine, decoding a receiver nobody could reach.
    //
    // sink_closed_ answers a different question. It exists to stop a sink
    // being INSTALLED after stop() has decided which ones it will take off,
    // which is why start_rds checks it across the attach. Taking one off is
    // always allowed and always right: stop() itself detaches after setting
    // the flag.
    //
    // Discarded, though, for the reason it always was: the only failures are
    // a receiver the graph no longer knows, which is the ordinary teardown
    // order and the usual way this function is reached, and a token already
    // detached, which stop() would have done.
    static_cast<void>(engine_.detach_audio_sink(vrx, route->sink));

    // Taken and released, which is what waits for a sink call that was
    // already inside the decoder when the detach was queued. The detach is
    // asynchronous, so a dispatch recorded before it can still arrive; the
    // route outlives this scope in the callable's own shared_ptr and its
    // decode reaches nothing but itself, so a late call is harmless once
    // this has returned.
    const std::scoped_lock owned(route->lock);
}

// ---------------------------------------------------------------------------
// Receiver lifetime
// ---------------------------------------------------------------------------

void ServerImpl::record_vrx(engine::VrxId vrx, std::uint64_t session, bool keep) {
    vrx_owners_[vrx.value] = VrxOwner{.session = session, .keep = keep};
}

VrxOwner ServerImpl::owner_of(engine::VrxId vrx) const {
    const auto found = vrx_owners_.find(vrx.value);
    return found == vrx_owners_.end() ? VrxOwner{} : found->second;
}

void ServerImpl::after_vrx_removed(engine::VrxId vrx, kj::StringPtr reason) {
    end_audio_for_vrx(vrx, reason);

    // And the decoder, which needs no message: rdsStation is a poll, so the
    // next one answers with the engine's own "no receiver N is registered"
    // rather than with a station that stopped moving. This is what takes the
    // sink off promptly; a removal that went through neither removeVrx nor a
    // session ending is cleaned up by that next poll instead.
    end_rds_for_vrx(vrx);

    // And every event decoder, which does need a message: a decoded-message
    // stream that simply stops is what a quiet channel looks like, for the
    // reason audio's does.
    end_decoded_for_vrx(vrx, reason);

    vrx_owners_.erase(vrx.value);
}

// WHAT A SESSION ENDING TAKES WITH IT, which is its own receivers and nothing
// else.
//
// Not another session's, including one this session retuned or subscribed to:
// creation is the only thing that confers ownership, because it is the only
// thing a client does exactly once per receiver. Not a kept one, which is what
// keep is for. Not one the host process added, which has no creator here. And
// not the engine-wide state two sessions share, the detection threshold and
// the RDS region among them, for the reason the schema gives on
// setRdsRegion.
//
// The subscriptions this session held on OTHER receivers need nothing from
// here. Each is a capability that died with the session's connection, and its
// own destructor has already ended it by the time this runs, or will.
//
// WHY A DESTRUCTOR AND NOT A DISCONNECT HANDLER. capnp 1.4.0's TwoPartyServer
// offers no per-connection hook, as AuthenticatorImpl's note says. It does
// release every capability a connection exported when the connection goes,
// however it goes, so the Session's destructor is the one event that
// happens exactly once for every way a client can leave, a crash included.
void ServerImpl::end_session(std::uint64_t session) {
    std::vector<std::uint32_t> doomed;
    for (const auto& [id, owner] : vrx_owners_) {
        if (owner.session == session && !owner.keep) {
            doomed.push_back(id);
        }
    }

    for (const std::uint32_t id : doomed) {
        const engine::VrxId vrx{id};

        // Discarded, and the cleanup below runs either way. The engine refuses
        // only a receiver it no longer holds, which a front-end retune or a
        // closed source can have done already, and in that case the server's
        // own state is the only thing left to take down.
        static_cast<void>(engine_.remove_vrx(vrx));
        after_vrx_removed(vrx, "the session that created this receiver ended, and it was not "
                               "created with keep");
    }
}

void ServerImpl::forget_across_retune() {
    {
        std::scoped_lock held(detect_lock_);

        // The completion thread checks this before it takes the lock, so
        // clearing it first is what stops a frame from the new centre
        // reaching the old detector between here and the reset below.
        detecting_.store(false, std::memory_order_relaxed);
        detector_.reset();

        // Every segment now looks at a different piece of spectrum, so the
        // window's history is a measurement of somewhere else and a slope
        // fitted across the retune is a slope through two bands.
        front_end_.reset();
        have_front_end_decision_ = false;
        last_front_end_decision_ = 0;
    }

    // detector_fault_ is deliberately left alone. A detector that faulted
    // did so for a reason that has nothing to do with where the front end
    // is pointed, and clearing it here would rebuild the same fault on the
    // next poll while making it look like the retune had fixed something.

    for (const engine::VrxId id : engine_.vrx_ids()) {
        auto status = engine_.vrx_status(id);
        if (!status) {
            // Removed between the tune and this pass. reset_rds_for_vrx
            // would have nothing to fence against, and the next poll drops
            // the route anyway.
            continue;
        }
        reset_rds_for_vrx(id, status->tuning_epoch);
        reset_decoded_for_vrx(id, status->tuning_epoch);
    }
}

void ServerImpl::reset_rds_for_vrx(engine::VrxId vrx, std::uint64_t epoch_target) {
    auto found = rds_routes_.find(vrx.value);
    if (found == rds_routes_.end()) {
        return;
    }

    const std::shared_ptr<RdsRoute>& route = found->second;
    const std::scoped_lock held(route->lock);
    route->bits.reset();
    route->groups.reset();
    route->last_group_sample = 0;
    route->ta_changed_at = 0;
    route->ta_seen = false;
    route->last_ta = false;

    // AND THE FENCE, WHICH IS THE HALF THE CLEARING ABOVE DOES NOT DO.
    //
    // Engine::set_vrx_params queues a control op. The recording thread
    // applies it at the next block boundary and the frames already recorded
    // are still the old tuning and still in flight, so clearing here and
    // stopping would hand the old transmitter to a decoder that has just
    // been told it is on a new one. The result is not stale text, which a
    // client could at least distrust: it is the OLD station's PS and
    // RadioText assembled under the NEW tuning's first samples, which reads
    // as a successful decode of the station the client just tuned to.
    //
    // So the sample path is told to discard everything below the epoch the
    // caller read off the receiver after queueing the retune. That number
    // is VrxStatus::tuning_epoch, the graph's own count of retunes queued
    // for this receiver, and AudioChunk::tuning_epoch is the same count as
    // far as the recording thread has got, so the two are comparable by
    // construction. Two retunes in quick succession simply raise the target
    // twice and the tuning in between goes with the rest.
    //
    // NEVER LOWERED. Another session may have retuned this receiver in
    // between and read a higher number than this caller did; taking the
    // maximum keeps the fence at the latest request rather than reopening
    // it for a tuning somebody has already left.
    //
    // WHAT THIS FUNCTION USED TO BE, IN TWO STEPS. Until 2026-09-20 it was
    // the clearing alone, and the comment on setVrxParams said a retune
    // clears the decoder, which was true of the struct and false of the
    // stream. The fence that closed that COUNTED resets here and took one
    // off per observed change of chunk epoch in the sample path, which two
    // retunes in one control drain left permanently armed. RdsRoute::
    // epoch_target has that failure in full and why a comparison cannot
    // have it.
    route->epoch_target = std::max(route->epoch_target, epoch_target);

    // The fault is NOT cleared. A retune cannot change the audio rate or the
    // channel count: both are shape, and Graph::set_vrx_params refuses a
    // retune that changes the shape rather than applying it, so a receiver
    // that delivered the wrong thing once will deliver it again.
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
            } else {
                observe_front_end();
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
    // All three kinds through one pump and one bell. A receiver's audio, its
    // passband and the span are produced by the same completion thread in
    // the same dispatch, so three pumps would be three wakeups for one batch
    // of work.
    drain();
    drain_passbands();
    drain_audio();
    drain_decoded();

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
    // UNCONDITIONALLY, AND sink_lock_ IS NOT TAKEN FOR IT.
    //
    // WHAT THIS FUNCTION USED TO DO. It took sink_lock_, and when
    // sink_closed_ was set it returned without detaching and without
    // releasing the pending slot below, under a comment reading "stop() has
    // already taken every sink off". That is the one case in which stop()
    // has not. stop() sets sink_closed_ under sink_lock_ while the loop
    // thread is STILL RUNNING, and only walks passband_sinks_ after joining
    // it. end_passband is loop-thread only and erases this receiver's count
    // from that map BEFORE calling here. So a cancel, a dropped capability
    // or a removeVrx handled in the window between the flag and the join
    // erased the count, skipped the detach, and left stop()'s walk nothing
    // to find. The engine kept the sink for the rest of its own life with
    // no subscriber behind it, and a second server on the same engine saw
    // passband_frames climbing with nothing anywhere explaining it. An
    // ordinary shutdown with the Qt client's VFO pane open is that window.
    //
    // The identical defect was diagnosed and retracted in end_rds_for_vrx
    // four hundred lines above, in this file, and end_audio below carried
    // the third copy of it. All three detach unconditionally now.
    //
    // sink_closed_ answers a different question. It exists to stop a sink
    // being INSTALLED after stop() has decided which ones it will take off,
    // which is why add_passband checks it across the install. Taking one
    // off is always allowed and always right: stop() itself detaches after
    // setting the flag, and clearing a sink that is already clear is the
    // engine's own no-op.
    //
    // Asynchronous, so a frame recorded before this can still arrive. The
    // pending slot below is dropped for that reason rather than left to be
    // fanned out to a subscription nobody holds.
    static_cast<void>(engine_.set_passband_sink(vrx, {}));

    std::scoped_lock held(frame_lock_);
    auto slot = passband_pending_.find(vrx.value);
    if (slot != passband_pending_.end()) {
        if (slot->second != nullptr) {
            passband_spare_.push_back(std::move(slot->second));
        }
        passband_pending_.erase(slot);
    }
}

Status ServerImpl::on_audio_chunk(AudioRoute& route, const engine::AudioChunk& chunk) {
    // The engine's completion thread, with route.lock held by the callable
    // that got here. It touches no capability and never waits on the loop.
    if (chunk.channels == 0) {
        return {};
    }
    const auto frames = static_cast<std::uint32_t>(chunk.samples.size() / chunk.channels);
    if (frames == 0) {
        return {};
    }

    bool queued = false;
    for (const auto& node : route.nodes) {
        const std::scoped_lock held(node->lock);

        // The two-chunk floor, applied here because this is the first moment
        // the server knows what a chunk is. See the schema on subscribeAudio:
        // the length needs block_samples and the conversion from
        // milliseconds needs the receiver's audio rate, and EngineInfo
        // carries neither.
        //
        // WHY IT RUNS ON EVERY CHUNK AND NOT ONLY THE FIRST. This used to say
        // "because a retune is a remove and an add and the replacement can be
        // at another rate", which argues against itself: a remove ends this
        // subscription through end_audio_for_vrx and the node dies with it,
        // so no live AudioNode can ever see a second rate. Nothing else moves
        // one either, because Graph::set_vrx_params refuses a shape change in
        // place. The honest reason is that both inputs are already in hand on
        // this line and the arithmetic is two multiplies, so skipping it
        // would cost a "have I seen a chunk yet" flag to save nothing.
        // Recomputing an answer that cannot change is the cheap half of that
        // trade and it needs no state.
        const std::uint64_t depth =
            static_cast<std::uint64_t>(node->granted_millis) * chunk.rate / 1000U;
        const std::uint64_t floor = 2ULL * frames;
        node->buffer_frames = std::max(depth, floor);

        // From the FRONT, which is the oldest chunk not yet sent. Late audio
        // is worse than no audio when the point is to hear what the radio is
        // doing now, and front eviction is also what makes
        // framesDroppedBefore exact: the frames it discards lie precisely
        // between the last chunk this subscriber was sent and the next one.
        bool evicted = false;
        while (!node->queue.empty() &&
               node->queued_frames + frames > node->buffer_frames) {
            AudioChunkBuffer& oldest = node->queue.front();
            node->queued_frames -= oldest.frames;
            node->frames_dropped += oldest.frames;
            node->dropped_before += oldest.frames;
            evicted = true;
            if (node->spare.size() < kAudioSpareBuffers) {
                node->spare.push_back(std::move(oldest));
            }
            node->queue.pop_front();
        }

        // The edge rather than the frames. One two-second stall and four
        // hundred scattered hitches lose the same frames and sound nothing
        // alike.
        if (evicted && !node->dropping) {
            node->drop_events += 1;
        }
        node->dropping = evicted;

        AudioChunkBuffer buffer;
        if (!node->spare.empty()) {
            buffer = std::move(node->spare.back());
            node->spare.pop_back();
        }

        // assign rather than a fresh vector, so a recycled buffer copies the
        // samples without reaching the allocator.
        buffer.samples.assign(chunk.samples.begin(), chunk.samples.end());
        buffer.sample_index = chunk.start;
        buffer.frames = frames;
        buffer.rate = static_cast<std::uint32_t>(chunk.rate);
        buffer.channels = static_cast<std::uint16_t>(chunk.channels);
        buffer.squelch_open = chunk.squelch_open;

        node->queued_frames += frames;
        node->queue.push_back(std::move(buffer));
        queued = true;
    }

    if (!queued) {
        return {};
    }

    // The same bell the two display streams ring, taken rather than borrowed
    // so a burst across several receivers wakes the loop once.
    kj::Own<kj::CrossThreadPromiseFulfiller<void>> waker;
    {
        const std::scoped_lock held(frame_lock_);
        waker = kj::mv(wakeup_);
    }
    if (waker.get() != nullptr) {
        waker->fulfill();
    }
    return {};
}

void ServerImpl::drain_audio() {
    // Snapshotted under each route's lock and pumped outside it, so the
    // completion thread is not held off for the length of a fan-out.
    std::vector<std::shared_ptr<AudioNode>> ready;
    for (const auto& entry : audio_routes_) {
        const std::scoped_lock held(entry.second->lock);
        ready.insert(ready.end(), entry.second->nodes.begin(), entry.second->nodes.end());
    }

    std::vector<std::shared_ptr<AudioNode>> gone;
    for (const auto& node : ready) {
        if (node->cancelled) {
            gone.push_back(node);
            continue;
        }
        pump_audio(node);
    }

    // After the pass rather than during it, because end_audio mutates both
    // audio_routes_ and the node list this loop is walking.
    for (const auto& node : gone) {
        end_audio(node);
    }
}

void ServerImpl::pump_audio(const std::shared_ptr<AudioNode>& node) {
    // One chunk in flight per subscription, which is the same rule the
    // display streams use and means something different here. There it is
    // the whole of the backpressure and a frame arriving during one is
    // dropped. Here it is only the wire's own serialisation: a chunk that
    // arrives while this is true is QUEUED, and the depth is what decides
    // whether anything is lost.
    if (node->cancelled || node->in_flight || sends_ == nullptr) {
        return;
    }

    AudioChunkBuffer buffer;
    std::uint64_t dropped_before = 0;
    {
        const std::scoped_lock held(node->lock);
        if (node->queue.empty()) {
            return;
        }
        buffer = std::move(node->queue.front());
        node->queue.pop_front();
        node->queued_frames -= buffer.frames;
        dropped_before = node->dropped_before;
        node->dropped_before = 0;
        node->frames_sent += buffer.frames;
    }

    // WRITTEN HERE AND NOT IN core/rpc/convert.cpp, WHICH IS THE ONE PLACE
    // THIS FILE DEPARTS FROM THAT RULE
    //
    // The rule exists because a spectrum frame crosses as an
    // engine::SpectrumFrame and a conversion belongs where both sides are
    // visible. An audio chunk does not: engine::AudioChunk::samples is valid
    // only for the duration of the sink call, so by the time a chunk reaches
    // the wire it is an AudioChunkBuffer this file owns, queued and
    // reordered against other chunks, and framesDroppedBefore is this
    // subscription's own accounting rather than anything the engine said. A
    // convert.cpp function would take seven loose arguments and convert
    // nothing.
    //
    // Sized up front so the samples land in one segment. Two floats to a
    // word, plus room for the six scalars and the struct itself.
    const std::uint64_t words = (buffer.samples.size() + 1) / 2 + 32;
    auto request = node->receiver.chunkRequest(capnp::MessageSize{words, 0});
    auto out = request.initChunk();
    auto samples = out.initSamples(static_cast<unsigned>(buffer.samples.size()));
    for (unsigned i = 0; i < samples.size(); ++i) {
        samples.set(i, buffer.samples[i]);
    }
    out.setSampleRate(buffer.rate);
    out.setChannelCount(buffer.channels);
    out.setSampleIndex(buffer.sample_index);
    out.setFramesDroppedBefore(dropped_before);
    out.setSquelchOpen(buffer.squelch_open);

    node->in_flight = true;

    auto weak = node->weak_from_this();
    sends_->add(request.send().ignoreResult().then(
        [this, weak]() {
            // Straight on to the next one rather than waiting for the engine
            // to ring again. A subscriber that has just come back from a
            // stall has a backlog, and draining it at the production rate
            // would keep it exactly as far behind as the stall left it.
            if (auto live = weak.lock()) {
                live->in_flight = false;
                pump_audio(live);
            }
        },
        [this, weak](kj::Exception&& failure) {
            // A receiver that threw or went away is not coming back, so the
            // subscription ends here rather than being retried.
            //
            // WHAT THIS COMMENT USED TO SAY: "and with no ended() call: the
            // capability it would travel on is the one that failed." That
            // is true of one of the two failures and false of the other,
            // and the difference is the whole of whether the client ever
            // learns. DISCONNECTED is the connection going, and there is
            // nothing left to send on. Anything else is this RECEIVER
            // throwing on this one call, which says nothing about the
            // connection: the client is still there, still holding a
            // subscription that has silently stopped delivering, and
            // AudioSubscription::stats then refuses. The silence was the
            // heavier half of that, so the reason travels back on the same
            // capability in the case where it can arrive.
            auto live = weak.lock();
            if (live == nullptr) {
                return;
            }
            live->in_flight = false;
            live->cancelled = true;
            if (failure.getType() == kj::Exception::Type::DISCONNECTED) {
                return;
            }
            send_audio_ended(live,
                             kj::str("this receiver failed the chunk call, so the "
                                     "subscription was ended: ",
                                     failure.getDescription()));
        }));

    // Recycled only now, after the samples are in the outgoing message, so
    // nothing in flight still refers to this buffer.
    const std::scoped_lock held(node->lock);
    if (node->spare.size() < kAudioSpareBuffers) {
        node->spare.push_back(std::move(buffer));
    }
}

Status ServerImpl::add_audio(std::shared_ptr<AudioNode> node) {
    const std::uint32_t key = node->vrx.value;

    auto existing = audio_routes_.find(key);
    if (existing == audio_routes_.end()) {
        const std::scoped_lock held(sink_lock_);
        if (sink_closed_) {
            return fail("this server is stopping and will install no further sinks");
        }

        auto route = std::make_shared<AudioRoute>();
        route->owner = this;

        // attach rather than set, which is the whole point of the seam in
        // core/engine/engine.h: a recording or a loudspeaker already on this
        // receiver keeps its audio, and this server's sink joins it.
        auto attached = engine_.attach_audio_sink(
            node->vrx, [route](const engine::AudioChunk& chunk) -> Status {
                std::scoped_lock owned(route->lock);
                if (route->owner == nullptr) {
                    return {};
                }
                return route->owner->on_audio_chunk(*route, chunk);
            });
        if (!attached) {
            return std::unexpected(attached.error());
        }

        route->sink = *attached;
        existing = audio_routes_.emplace(key, std::move(route)).first;
    }

    const std::scoped_lock held(existing->second->lock);
    existing->second->nodes.push_back(std::move(node));
    return {};
}

void ServerImpl::end_audio(const std::shared_ptr<AudioNode>& node) {
    node->cancelled = true;

    auto found = audio_routes_.find(node->vrx.value);
    if (found == audio_routes_.end()) {
        return;
    }

    bool last = false;
    {
        const std::scoped_lock held(found->second->lock);
        const auto before = found->second->nodes.size();
        std::erase(found->second->nodes, node);
        if (found->second->nodes.size() == before) {
            // Already ended. Cancelling twice is ordinary: a client can call
            // cancel and then drop the capability.
            return;
        }
        last = found->second->nodes.empty();
    }
    if (!last) {
        return;
    }

    auto route = found->second;
    audio_routes_.erase(found);

    // UNCONDITIONALLY, AND sink_lock_ IS NOT TAKEN FOR IT. The third copy of
    // the shape end_rds_for_vrx retracts at length and detach_passband_sink
    // retracts again: the route is erased from audio_routes_ on the line
    // above, this is loop-thread only, and stop() sets sink_closed_ while
    // the loop is still running and walks audio_routes_ only after joining
    // it. A cancel in that window erased the route, skipped the detach and
    // left stop()'s walk nothing to find, so the engine carried a decoderless
    // audio sink for the rest of its life.
    //
    // WHAT THE COMMENT INSIDE THE BRANCH USED TO SAY: "the only failures are
    // a receiver the graph no longer knows, which is the ordinary teardown
    // order, and a token already detached, which stop() would have done."
    // The second half was false of the one window the branch covered. Inside
    // `!sink_closed_` stop() has not run, so it cannot have detached
    // anything, and the discard was being justified by a case the guard had
    // just excluded. Both halves are true now that the call is unconditional,
    // which is the reason the return value is still discarded.
    static_cast<void>(engine_.detach_audio_sink(node->vrx, route->sink));

    // Closed last, and it waits for a sink call that is already running. The
    // detach above is asynchronous, so a dispatch recorded before it can
    // still arrive; its node list is empty by then, so it queues nothing
    // either way.
    const std::scoped_lock owned(route->lock);
    route->owner = nullptr;
}

void ServerImpl::send_audio_ended(const std::shared_ptr<AudioNode>& node,
                                  kj::StringPtr reason) {
    // Best effort, which the schema says rather than promises. A server
    // whose loop has already stopped has no sends_ to put this on, and a
    // send that fails is dropped: the capability this would travel on is
    // the one the client would have to be holding for the answer to reach
    // it anyway.
    if (node->ended_sent || sends_ == nullptr) {
        return;
    }
    node->ended_sent = true;
    auto request = node->receiver.endedRequest();
    request.setReason(reason);
    sends_->add(request.send().ignoreResult().catch_([](kj::Exception&&) {}));
}

void ServerImpl::end_audio_for_vrx(engine::VrxId vrx, kj::StringPtr reason) {
    auto found = audio_routes_.find(vrx.value);
    if (found == audio_routes_.end()) {
        return;
    }

    std::vector<std::shared_ptr<AudioNode>> nodes;
    {
        const std::scoped_lock held(found->second->lock);
        nodes = found->second->nodes;
    }

    for (const auto& node : nodes) {
        // Never for a cancel the client asked for, which is the check
        // send_audio_ended deliberately does not make: this path reaches
        // nodes the client is still using, where the delivery-failure path
        // reaches one it has already lost.
        if (!node->cancelled) {
            send_audio_ended(node, reason);
        }
        end_audio(node);
    }
}

// ---------------------------------------------------------------------------
// Decoded messages
// ---------------------------------------------------------------------------

void ServerImpl::wake_loop() {
    kj::Own<kj::CrossThreadPromiseFulfiller<void>> waker;
    {
        const std::scoped_lock held(frame_lock_);
        waker = kj::mv(wakeup_);
    }
    if (waker.get() != nullptr) {
        waker->fulfill();
    }
}

void ServerImpl::on_decoded_chunk(DecodeRoute& route, const engine::AudioChunk& chunk) {
    if (!route.fault.empty()) {
        return;
    }

    // The retune fence, asked before anything else and recorded whether the
    // chunk is decoded or discarded, exactly as decode_rds_chunk does it.
    route.epoch_reached = std::max(route.epoch_reached, chunk.tuning_epoch);
    if (route.discarding()) {
        ++route.chunks_discarded;
        return;
    }

    const DecoderChunk in{
        .samples = chunk.samples,
        .channels = chunk.channels,
        .rate = chunk.rate,
        .start = chunk.start,
    };
    if (in.frames() == 0) {
        return;
    }

    if (route.decoder == nullptr) {
        auto made = route.spec.make(DecoderBuild{.rate = chunk.rate, .mode = route.mode});
        if (!made) {
            route.fault = made.error().message;
            wake_loop();
            return;
        }
        route.decoder = std::move(*made);
        route.rate = static_cast<std::uint32_t>(chunk.rate);
    }

    route.scratch.clear();
    if (auto consumed = route.decoder->consume(in, route.scratch); !consumed) {
        route.fault = consumed.error().message;
        wake_loop();
        return;
    }
    if (route.scratch.empty()) {
        return;
    }
    enqueue_decoded(route);
    wake_loop();
}

void ServerImpl::enqueue_decoded(DecodeRoute& route) {
    for (DecodedMessage& message : route.scratch) {
        message.vrx = route.vrx.value;
        message.sequence = route.sequence++;
        for (const auto& node : route.nodes) {
            const std::scoped_lock held(node->lock);
            // From the front, on audio's argument: the oldest message is the
            // one a subscriber that fell behind is least likely to still
            // want, and front eviction is what makes droppedBefore exact.
            while (node->queue.size() >= kDecodedQueueDepth) {
                node->queue.pop_front();
                ++node->dropped_before;
                ++node->messages_dropped;
            }
            node->queue.push_back(message);
        }
    }
}

Status ServerImpl::add_decoded(std::shared_ptr<DecodedNode> node,
                               const engine::VrxStatus& status, const DecoderSpec& spec) {
    auto key = std::make_pair(node->vrx.value, node->decoder);
    auto existing = decode_routes_.find(key);
    if (existing == decode_routes_.end()) {
        // Across the attach and not merely checked before it, for the reason
        // start_rds gives.
        const std::scoped_lock held(sink_lock_);
        if (sink_closed_) {
            return fail("this server is stopping and will install no further sinks");
        }

        auto route = std::make_shared<DecodeRoute>(node->vrx, spec, status.params.demod);
        route->owner = this;

        // The fence starts where the receiver is, on start_rds's argument: a
        // decoder attached to a receiver retuned nine times must not accept a
        // chunk stamped with an older tuning.
        route->epoch_target = status.tuning_epoch;

        auto attached = engine_.attach_audio_sink(
            node->vrx, [route](const engine::AudioChunk& chunk) -> Status {
                const std::scoped_lock owned(route->lock);
                if (route->owner != nullptr) {
                    route->owner->on_decoded_chunk(*route, chunk);
                }
                return {};
            });
        if (!attached) {
            return std::unexpected(attached.error());
        }
        route->sink = *attached;
        existing = decode_routes_.emplace(std::move(key), std::move(route)).first;
    }

    const std::scoped_lock held(existing->second->lock);
    existing->second->nodes.push_back(std::move(node));
    return {};
}

void ServerImpl::end_decoded(const std::shared_ptr<DecodedNode>& node) {
    node->cancelled = true;

    auto found = decode_routes_.find(std::make_pair(node->vrx.value, node->decoder));
    if (found == decode_routes_.end()) {
        return;
    }

    bool last = false;
    {
        const std::scoped_lock held(found->second->lock);
        const auto before = found->second->nodes.size();
        std::erase(found->second->nodes, node);
        if (found->second->nodes.size() == before) {
            // Already ended: a cancel followed by the capability being dropped.
            return;
        }
        last = found->second->nodes.empty();
    }
    if (!last) {
        return;
    }

    auto route = found->second;
    decode_routes_.erase(found);

    // Unconditionally and without sink_lock_, for the reason end_rds_for_vrx
    // gives at length: a removal is always allowed, and skipping it when
    // stop() has set its flag is the window three sinks were once left in.
    static_cast<void>(engine_.detach_audio_sink(route->vrx, route->sink));

    // Taken last, which waits for a dispatch already inside the decoder; the
    // callable keeps the route alive and finds owner null afterwards.
    const std::scoped_lock owned(route->lock);
    route->owner = nullptr;
}

void ServerImpl::send_decoded_ended(const std::shared_ptr<DecodedNode>& node,
                                    kj::StringPtr reason) {
    // Best effort, on send_audio_ended's terms.
    if (node->ended_sent || sends_ == nullptr) {
        return;
    }
    node->ended_sent = true;
    auto request = node->receiver.endedRequest();
    request.setReason(reason);
    sends_->add(request.send().ignoreResult().catch_([](kj::Exception&&) {}));
}

void ServerImpl::end_decode_route(const std::shared_ptr<DecodeRoute>& route,
                                  kj::StringPtr reason) {
    std::vector<std::shared_ptr<DecodedNode>> nodes;
    {
        const std::scoped_lock held(route->lock);

        // The receiver has gone, so its stream has ended, and a decoder
        // holding a message open says what it recovered of it. D-STAR is the
        // one that holds one: a transmission comes over a superframe at a
        // time, and the frames since the last boundary would otherwise go
        // with the decoder. Not after a fault, which stopped the decoder on
        // a chunk it refused.
        if (route->decoder != nullptr && route->fault.empty()) {
            route->scratch.clear();
            route->decoder->flush(route->scratch);
            enqueue_decoded(*route);
        }
        nodes = route->nodes;
    }
    for (const auto& node : nodes) {
        // Never for a cancel the client asked for.
        if (!node->cancelled) {
            send_decoded_backlog(node);
            send_decoded_ended(node, reason);
        }
        end_decoded(node);
    }
}

void ServerImpl::send_decoded_backlog(const std::shared_ptr<DecodedNode>& node) {
    if (sends_ == nullptr) {
        return;
    }

    // Everything queued goes now, ahead of ended() on the same capability,
    // which Cap'n Proto delivers in the order it was sent. pump_decoded's
    // one-in-flight limit is the wire's pacing for a live stream and there is
    // no stream left to pace; left to it, what was still queued when the
    // receiver went would be dropped with the node, which is what happened to
    // it before 2026-09-23.
    std::deque<DecodedMessage> backlog;
    std::uint64_t dropped_before = 0;
    {
        const std::scoped_lock held(node->lock);
        backlog.swap(node->queue);
        dropped_before = node->dropped_before;
        node->dropped_before = 0;
        node->messages_sent += backlog.size();
    }
    for (DecodedMessage& message : backlog) {
        message.dropped_before = dropped_before;
        dropped_before = 0;
        auto request = node->receiver.messageRequest();
        write_decoded_message(request.initMessage(), message);
        sends_->add(request.send().ignoreResult().catch_([](kj::Exception&&) {}));
    }
}

void ServerImpl::end_decoded_for_vrx(engine::VrxId vrx, kj::StringPtr reason) {
    std::vector<std::shared_ptr<DecodeRoute>> routes;
    for (const auto& [key, route] : decode_routes_) {
        if (key.first == vrx.value) {
            routes.push_back(route);
        }
    }
    for (const auto& route : routes) {
        end_decode_route(route, reason);
    }
}

void ServerImpl::reset_decoded_for_vrx(engine::VrxId vrx, std::uint64_t epoch_target) {
    for (const auto& [key, route] : decode_routes_) {
        if (key.first != vrx.value) {
            continue;
        }
        const std::scoped_lock held(route->lock);
        if (route->decoder != nullptr) {
            route->decoder->reset();
        }

        // Never lowered, on reset_rds_for_vrx's argument: another session may
        // have retuned in between and read a higher number.
        route->epoch_target = std::max(route->epoch_target, epoch_target);

        // Messages already queued are left alone. Each was completed from the
        // old tuning and says so by its sample index, and a client that has
        // not read them yet is owed them: an event that happened is not made
        // false by the receiver moving afterwards.
    }
}

Expected<DecodedStats> ServerImpl::decoded_stats(DecodedNode& node) {
    auto found = decode_routes_.find(std::make_pair(node.vrx.value, node.decoder));
    if (found == decode_routes_.end()) {
        return fail("this decoded-message subscription's decoder is no longer attached");
    }

    DecodedStats out;
    {
        const std::scoped_lock held(found->second->lock);
        out.chunks_discarded = found->second->chunks_discarded;
        out.sample_rate = found->second->rate;
    }
    {
        const std::scoped_lock held(node.lock);
        out.messages_sent = node.messages_sent;
        out.messages_dropped = node.messages_dropped;
        out.backlog = node.queue.size();
    }
    return out;
}

void ServerImpl::drain_decoded() {
    // Faults first. A decoder that refused a chunk has stopped for good, and
    // every subscriber to it is told why rather than left watching a stream
    // that went quiet.
    std::vector<std::pair<std::shared_ptr<DecodeRoute>, std::string>> faulted;
    std::vector<std::shared_ptr<DecodedNode>> ready;
    for (const auto& entry : decode_routes_) {
        const auto& route = entry.second;
        const std::scoped_lock held(route->lock);
        if (!route->fault.empty() && !route->fault_reported) {
            route->fault_reported = true;
            faulted.emplace_back(route, route->fault);
            continue;
        }
        ready.insert(ready.end(), route->nodes.begin(), route->nodes.end());
    }

    for (const auto& [route, fault] : faulted) {
        const std::string reason = std::format("the {} decoder on receiver {} stopped: {}",
                                               route->spec.name, route->vrx.value, fault);
        end_decode_route(route, kj::StringPtr(reason.c_str()));
    }

    std::vector<std::shared_ptr<DecodedNode>> gone;
    for (const auto& node : ready) {
        if (node->cancelled) {
            gone.push_back(node);
            continue;
        }
        pump_decoded(node);
    }
    for (const auto& node : gone) {
        end_decoded(node);
    }
}

void ServerImpl::pump_decoded(const std::shared_ptr<DecodedNode>& node) {
    // One message in flight per subscription, the wire's own serialisation;
    // anything arriving meanwhile is queued, as audio's chunks are.
    if (node->cancelled || node->in_flight || sends_ == nullptr) {
        return;
    }

    DecodedMessage message;
    std::uint64_t dropped_before = 0;
    {
        const std::scoped_lock held(node->lock);
        if (node->queue.empty()) {
            return;
        }
        message = std::move(node->queue.front());
        node->queue.pop_front();
        dropped_before = node->dropped_before;
        node->dropped_before = 0;
        ++node->messages_sent;
    }
    message.dropped_before = dropped_before;

    auto request = node->receiver.messageRequest();
    write_decoded_message(request.initMessage(), message);
    node->in_flight = true;

    auto weak = node->weak_from_this();
    sends_->add(request.send().ignoreResult().then(
        [this, weak]() {
            if (auto live = weak.lock()) {
                live->in_flight = false;
                pump_decoded(live);
            }
        },
        [this, weak](kj::Exception&& failure) {
            // pump_audio's two cases: a connection that went has nothing left
            // to tell, and a receiver that threw is still there to be told.
            auto live = weak.lock();
            if (live == nullptr) {
                return;
            }
            live->in_flight = false;
            live->cancelled = true;
            if (failure.getType() == kj::Exception::Type::DISCONNECTED) {
                return;
            }
            send_decoded_ended(live, kj::str("this receiver failed the message call, so the "
                                             "subscription was ended: ",
                                             failure.getDescription()));
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

void ServerImpl::release_source_state(kj::StringPtr reason) {
    // Audio first, because it is the only one of the four that tells the
    // client. A subscriber holding an AudioSubscription hears the stream end
    // with this reason in it; a spectrum or passband subscriber finds out by
    // the frames stopping, which is the shape those two already have and is
    // why neither grew an ended() call for this.
    //
    // A copy of the keys, because end_audio_for_vrx erases from audio_routes_
    // through end_audio and walking the map while it does is a dangling
    // iterator. The same for the two maps below it.
    std::vector<std::uint32_t> audio_ids;
    audio_ids.reserve(audio_routes_.size());
    for (const auto& entry : audio_routes_) {
        audio_ids.push_back(entry.first);
    }
    for (const std::uint32_t id : audio_ids) {
        end_audio_for_vrx(engine::VrxId{id}, reason);
    }

    std::vector<std::uint32_t> rds_ids;
    rds_ids.reserve(rds_routes_.size());
    for (const auto& entry : rds_routes_) {
        rds_ids.push_back(entry.first);
    }
    for (const std::uint32_t id : rds_ids) {
        end_rds_for_vrx(engine::VrxId{id});
    }

    // Every event decoder, telling its subscribers why, as audio's are told.
    std::vector<std::shared_ptr<DecodeRoute>> decode_routes;
    decode_routes.reserve(decode_routes_.size());
    for (const auto& entry : decode_routes_) {
        decode_routes.push_back(entry.second);
    }
    for (const auto& route : decode_routes) {
        end_decode_route(route, reason);
    }

    // Every receiver goes with the source, so nothing is left to own. A
    // session ending later would otherwise try to remove ids that belonged to
    // the old stream, which the engine refuses harmlessly and which would
    // still be work done against a graph that no longer exists.
    vrx_owners_.clear();

    // Every passband node, through end_passband so the per-receiver refcount
    // and the engine detach both happen exactly as they do for a cancel. A
    // copy again: end_passband erases from passband_nodes_.
    const std::vector<std::shared_ptr<PassbandNode>> nodes = passband_nodes_;
    for (const auto& node : nodes) {
        end_passband(node);
    }

    // Belt and braces after that walk. end_passband only detaches when the
    // count it decrements reaches zero, and a receiver whose nodes were all
    // already cancelled leaves a count behind with no node to drive it to
    // zero. Nothing should be left here; anything that is would be a sink on
    // a graph about to be destroyed.
    for (const auto& entry : passband_sinks_) {
        detach_passband_sink(engine::VrxId{entry.first});
    }
    passband_sinks_.clear();

    const std::vector<std::shared_ptr<Subscription>> spectrum = subscriptions_;
    for (const auto& subscription : spectrum) {
        end_subscription(subscription);
    }
    subscriptions_.clear();
    refresh_subscriber_summary();

    {
        std::scoped_lock held(sink_lock_);
        if (sink_installed_) {
            static_cast<void>(engine_.set_spectrum_sink({}));
            sink_installed_ = false;
        }
    }

    // The detector, and the front end monitor that reads its arrays. Under
    // detect_lock_ alone rather than after a join, unlike stop(): the two
    // threads that can be inside the detector are this one and the engine's
    // completion thread, this one is here, and the completion thread takes
    // this lock to feed it.
    //
    // detector_fault_ is cleared as well. A fault is a statement about the
    // stream that produced it, so carrying one across a close would refuse
    // every detections call on the next source for a reason that happened to
    // a different radio.
    {
        std::scoped_lock held(detect_lock_);
        detecting_.store(false, std::memory_order_relaxed);
        detector_.reset();
        detector_fault_.clear();
        front_end_.reset();
        have_front_end_decision_ = false;
        last_front_end_decision_ = 0;
    }

    // The queued copies, which are the old stream's frames numbered in its
    // sample indices. The spare pools are left: they are memory this server
    // reuses and carry no stream in them.
    {
        std::scoped_lock held(frame_lock_);
        if (pending_ != nullptr) {
            spare_.push_back(std::move(pending_));
            pending_.reset();
        }
        passband_pending_.clear();
    }
}

Status ServerImpl::open_source(std::string_view uri) {
    if (auto opened = engine_.open_source(uri); !opened) {
        return opened;
    }

    // Put back at once rather than left to the next subscribeSpectrum, for
    // the reason Server::create installs it in the first place: a recorder
    // with no subscribers still wants the detector and the front-end monitor
    // to have frames, and both are fed from this sink.
    //
    // DISCARDED, ON EXACTLY create's REASONING AND NOT OUT OF HASTE. The
    // source is open. Returning this refusal would answer a call that
    // succeeded with a failure, and a client reading that would undo an open
    // that worked or, worse, open again and be told it already has one. An
    // engine built with spectrum_transform at zero refuses here for the life
    // of the server and serves everything else perfectly well; subscribeSpectrum
    // makes the same call and reports the engine's own sentence to whoever
    // asks for a spectrum.
    static_cast<void>(ensure_sink());
    return {};
}

Status ServerImpl::close_source() {
    release_source_state("the engine's source was closed, so this stream has ended");
    return engine_.close_source();
}

// BOTH OF THOSE BLOCK THE EVENT LOOP, AND THAT IS A CHOICE RATHER THAN AN
// OVERSIGHT.
//
// The note at the top of this file says listSources is the only engine-facing
// call that opens hardware and that the loop must stay free while it runs. That
// rule is about listSources specifically and the reason is in its own shape: it
// opens EVERY device, including indices with nothing behind them, so it pays a
// libusb timeout per absent dongle and can take seconds on a machine with none.
//
// These two open or close ONE named device, which is what setSourceCenter
// already does on this loop: rtlsdr_set_center_freq is a USB control transfer
// and nobody moved it off. rtlsdr_open plus a claim and a reset, and on the way
// out rtlsdr_cancel_async plus the worker joining and the scheduler retiring
// what is already submitted, are tens to low hundreds of milliseconds on the
// hardware this has run on. On the loop that is a hitch in the other polls, not
// a freeze, and Engine::close_source's five second ceiling bounds the worst
// case rather than leaving it open.
//
// What would change this: a backend whose open is slow enough to matter, or a
// second client whose polls must not hitch while the first changes radios.
// list_sources is the worked pattern for moving it, and it costs a worker
// thread, a queue and a cross-thread fulfiller.

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

    // The same, for every receiver this server was carrying audio from. Each
    // route's owner is cleared under its own lock, which is what makes a
    // sink call already inside on_audio_chunk finish before this returns.
    for (const auto& entry : audio_routes_) {
        static_cast<void>(
            engine_.detach_audio_sink(engine::VrxId{entry.first}, entry.second->sink));
        const std::scoped_lock owned(entry.second->lock);
        entry.second->owner = nullptr;
        entry.second->nodes.clear();
    }
    audio_routes_.clear();

    // And every RDS decoder, on the same terms. Taking each route's lock is
    // what makes a sink call already inside the decoder finish before this
    // returns; there is no owner to clear, because the decode reaches
    // nothing outside its own route.
    for (const auto& entry : rds_routes_) {
        static_cast<void>(
            engine_.detach_audio_sink(engine::VrxId{entry.first}, entry.second->sink));
        const std::scoped_lock owned(entry.second->lock);
    }
    rds_routes_.clear();

    // And every event decoder. Clearing owner under the route's lock is what
    // makes a sink call already inside on_decoded_chunk finish before this
    // returns, and what makes a later one ring nothing.
    for (const auto& entry : decode_routes_) {
        static_cast<void>(engine_.detach_audio_sink(entry.second->vrx, entry.second->sink));
        const std::scoped_lock owned(entry.second->lock);
        entry.second->owner = nullptr;
        entry.second->nodes.clear();
    }
    decode_routes_.clear();

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
        front_end_.reset();
        have_front_end_decision_ = false;
        last_front_end_decision_ = 0;
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
