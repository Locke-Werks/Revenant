// The engine side of the wire.
//
// Holds a reference to a running (or not yet running) Engine and serves the
// Authenticator interface over a socket. Unlike core/rpc/client.h this one
// does link revenant_core, because its whole job is to be the engine's mouth.
//
// The bootstrap capability is an Authenticator and not a Session. A caller
// that has not passed ServerOptions::token holds one method and no radio, so
// there is no Session in its capability table to refuse. login mints a fresh
// Session per call, which costs a reference to this server and a login number.
// See core/rpc/revenant.capnp, which has the argument for the shape.
//
// A SESSION OWNS THE RECEIVERS IT CREATES, and removes them when it ends,
// unless addVrx was asked to keep one. That is the owner decision of
// 2026-09-22 and core/rpc/server.cpp's SessionImpl destructor is where it is
// enforced. This paragraph used to say that object was stateless and that its
// only member is a reference to this server; it now carries the number that
// VrxStatus::creatorSession reports.
//
// THREADING, WHICH IS THE ONLY HARD PART IN HERE
//
// Six kinds of thread matter and none of them is the same thread: the four
// below, and since 2026-09-23 the sigid lane and the decode lanes, which the
// two paragraphs after the listing worker describe.
//
// WHAT THE SENTENCE ABOVE USED TO SAY: "Four threads matter and none of them
// is the same thread."
//
//   The caller's. Constructs the Server, and later stops it.
//
//   The Cap'n Proto event loop, which Server owns. Every capability in the
//   generated schema may only be touched from here. This is not a
//   performance guideline; kj's promise machinery is not thread safe and
//   calling a capability from elsewhere corrupts it.
//
//   The engine's completion thread, which is where a SpectrumSink is
//   invoked. It is therefore the one thread that must never touch a
//   capability directly.
//
//   The listing worker, which exists because listSources opens hardware.
//   source::describe_sources reaches rtlsdr_open, a libusb open, claim and
//   reset, and running that on the loop thread stalled every other client's
//   calls, the spectrum fan-out, and the shutdown fulfiller that stop()
//   depends on. The work runs here; only the fulfiller crosses back, which
//   is the split kj supports. stop() closes the queue and joins this thread
//   before it fulfils the shutdown promise, so no listing is ever fulfilled
//   at an event loop that has gone.
//
//   The sigid lane, one thread below normal priority that runs the wideband
//   detector and tier two on frames the spectrum sink copies to it, and
//   publishes each decision for the loop thread to answer polls from. It
//   sheds frames it cannot take, under ServerOptions::detector_cpu_budget.
//
//   The decode lanes, ServerOptions::decode_lanes of them above normal
//   priority, core/rpc/decode_lane.h, which run every event decoder, every
//   P25 voice stream and every RDS decode on chunks their audio sinks copy
//   to them. Neither kind touches a capability; both reach the loop the way
//   the completion thread does.
//
//   core/thread_role.h has the priorities and docs/rpc.md, under Threading,
//   the measurement that put signal identification and the decoders on
//   threads of their own.
//
// The bridge between the last two is a cross-thread promise fulfiller. The
// sink copies the frame into a single-frame slot and fulfils a promise the
// loop thread is waiting on; the loop thread then does the capability work.
//
// This paragraph used to say "the sink calls kj::Executor::executeAsync, and
// that is the supported mechanism and the only one". Both halves were wrong,
// and the correction is recorded rather than quietly swapped because the
// wrong version is the one an experienced reader would expect to be right.
//
// executeAsync cannot be called from the engine's completion thread. kj's own
// header is explicit that the promise it returns "belongs to the requesting
// thread", and a kj promise requires an event loop on the thread that owns
// it. The completion thread has none, so the call throws. There is also no
// fire-and-forget form to fall back on: the same header states that
// destroying the returned promise blocks until the executor thread
// acknowledges cancellation, so dropping it cancels the work rather than
// detaching it.
//
// executeSync does work from a thread with no loop, and is still wrong here.
// It blocks the caller until the loop thread is done, and the caller is the
// thread retiring GPU readbacks. Parking it behind anything the loop is busy
// with would stall the signal path to
// serve the display.
//
// kj::newPromiseAndCrossThreadFulfiller is the primitive kj documents as
// safe to fire from any thread, which is exactly and only what the sink
// needs. The Executor is still held, with addRef, because the cross-thread
// promise keeps a bare reference to its creating thread's executor and kj
// destroys an unreferenced Executor along with its loop.
//
// BACKPRESSURE, DECIDED HERE RATHER THAN DISCOVERED LATER
//
// A slow client must not grow a queue. At the shipped geometry a frame is
// 256 KiB and the engine makes 305 a second, so a client that stalls for two
// seconds would be 150 MB behind if anything buffered on its behalf.
//
// So: at most one frame in flight per subscription. A frame arriving while
// the previous call has not resolved is dropped and counted, not queued. For
// a waterfall that is the right answer anyway, since the newest frame is the
// one worth drawing. The count is reported so that a display can say it is
// behind instead of silently lying about the band.
//
// AUDIO IS THE EXCEPTION, AND IT IS A DIFFERENT RULE RATHER THAN A LOOSER ONE
//
// The paragraph above is written about pictures and every word of it depends
// on that. A spectrum frame is a measurement of a band that is still there,
// so an older one is redundant and the newest is the one worth having. An
// audio chunk is the only copy of that instant: the newest is worth no more
// than the one before it, and skipping one is not a lower frame rate, it is
// a hole the listener hears.
//
// So an audio subscription QUEUES, up to the depth subscribeAudio granted
// it, and the one-chunk-in-flight limit on the wire is only the wire's own
// serialisation rather than the whole of the policy. When the queue is full
// the OLDEST chunk goes, not the newest, because late audio is worse than no
// audio when the point is to hear what the radio is doing now, and because
// front eviction is what makes AudioChunk::framesDroppedBefore exact: the
// frames it discards lie precisely between the last chunk sent and the next
// one, so a client can check
//
//   sampleIndex == previous.sampleIndex + previous frame count
//                  + framesDroppedBefore
//
// on every consecutive pair, and a gap larger than that was lost upstream in
// the engine rather than here.
//
// The counters are PER SUBSCRIPTION and are read through
// AudioSubscription.stats, not through frames_sent and frames_dropped below.
// Those two are server-wide and the spectrum's own comment admits they
// over-count across subscribers; a slow client's drops must never appear on
// a fast client's status line, and audio has no shared decimation to excuse
// it. Audio does not touch them at all.
//
// WHAT NEITHER RULE FIXES. A subscription's queue absorbs a slow socket and
// nothing else. The engine's completion thread still walks every subscriber
// on a receiver inline, per core/engine/engine.h's AudioFanout, so a sink
// that blocks rather than copying delays the radio. Everything this file
// installs copies and returns, and since 2026-09-23 that includes the
// decoders, the P25 voice stream and RDS, whose sinks copy the chunk to a
// decode lane (core/rpc/decode_lane.h) where the decoding runs.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/engine/engine.h"
#include "core/error.h"
#include "core/rpc/token.h"

namespace revenant::rpc {

struct ServerOptions {
    // Loopback by default, and that has not changed now that there is a
    // token.
    //
    // WHAT THIS COMMENT USED TO SAY: "there is no authentication on this
    // interface yet, so the default must not be the one that exposes it".
    // There is authentication now, and the default still must not be the one
    // that exposes it, for a narrower reason. This wire is plaintext: TLS with
    // client certificates was refused as the wrong shape for one operator with
    // one radio, and the consequence is that a token crossing a routable
    // interface is readable and replayable by anything on the path. A
    // pre-shared token over cleartext on a LAN is not authentication.
    //
    // So what login changed is what a second process on THIS machine can do.
    // Off loopback still means a tunnel. Recorded rather than quietly
    // rewritten, because docs/rpc.md and the schema both said the absence was
    // the whole of the story and a reader who took the loopback default as the
    // control is owed the reason it survives.
    std::string bind_address = "127.0.0.1";

    // Zero binds an ephemeral port, which the test suite needs so that two
    // runs on one machine do not collide. Server::port() reports what was
    // actually bound.
    std::uint16_t port = 0;

    // The pre-shared token Authenticator.login compares against, exactly
    // kTokenBytes of it. See core/rpc/token.h for where the file lives and how
    // a host gets the bytes out of it.
    //
    // EMPTY IS A create() FAILURE AND NEVER "NO AUTHENTICATION"
    //
    // This is the single most dangerous default this design could have had. If
    // empty meant unauthenticated, a caller that forgot the field would get an
    // engine anyone on the machine can drive, out of code that reads as though
    // it were configured. There is no off switch and no --no-auth for the same
    // reason: an off switch is the thing that ends up on by accident.
    // Supplying 32 bytes in a test is one line.
    std::vector<std::uint8_t> token;

    // The most of one core the wideband detector and tier two average on a
    // source running on a clock, a fraction above zero and at most one. Past
    // it the detector's lane sheds frames rather than keep up. A quarter by
    // default: measured under "Threading" in docs/rpc.md, the lane took 1.0
    // to 2.3 percent of one core on a 300-emitter scene at 33 frames a
    // second, so the budget is a ceiling for a band far busier than that and
    // not a setting the ordinary case reaches.
    double detector_cpu_budget = 0.25;

    // Threads that run the decoders, the P25 voice streams and RDS, each
    // route pinned to one. Two, so one receiver's decoder that has fallen
    // behind does not hold up every other receiver's.
    std::uint32_t decode_lanes = 2;
};

// Where this server's own work runs and how long it takes, cumulative from
// create, for measuring contention. Durations are nanoseconds of steady
// clock, core/engine/load_clock.h. docs/rpc.md, under Threading, has the
// measurement these were added for and what they showed.
struct ServerLoad {
    // The wideband detector: frames it consumed, and the time spent in
    // consume and in tier two's step. detector_frames_shed counts frames the
    // detector was offered and did not take.
    std::uint64_t detector_frames = 0;
    std::uint64_t detector_frames_shed = 0;
    std::uint64_t detector_ns = 0;
    std::uint64_t detector_max_ns = 0;
    std::uint64_t tier_two_ns = 0;

    // The loop thread's wait for the detector's lock, which a detections
    // poll takes. Time the loop spends here is time every other client's
    // audio and spectrum sit undelivered.
    std::uint64_t detect_poll_wait_ns = 0;
    std::uint64_t detect_polls = 0;

    // The receiver-side work: event decoders, a P25 receiver's voice stream,
    // RDS, and plain audio queued for a subscriber. Chunks and time.
    std::uint64_t decode_chunks = 0;
    std::uint64_t decode_ns = 0;
    std::uint64_t voice_chunks = 0;
    std::uint64_t voice_ns = 0;
    std::uint64_t rds_ns = 0;
    std::uint64_t audio_ns = 0;

    // The decode lanes, summed: chunks handed to one, chunks a full lane
    // dropped, times the completion thread waited for room (an unthrottled
    // replay only), time the lanes spent working, and the time a chunk
    // spent queued before its work began. core/rpc/decode_lane.h.
    std::uint64_t lane_posted = 0;
    std::uint64_t lane_dropped = 0;
    std::uint64_t lane_waits = 0;
    std::uint64_t lane_busy_ns = 0;
    std::uint64_t lane_latency_ns = 0;
    std::uint64_t lane_latency_max_ns = 0;
};

class Server {
public:
    // The engine must outlive the server. The server installs a spectrum
    // sink on it and removes that sink on destruction, so constructing two
    // servers on one engine is not supported and is rejected rather than
    // silently letting the second replace the first's sink.
    //
    // EVERY SINK COMES OFF, not only the engine-wide one this paragraph
    // names. A server also installs a per-receiver passband sink for each
    // receiver something is watching and a per-receiver audio sink for each
    // one something is listening to, decoding RDS from, or running an event
    // decoder from core/rpc/decoders.h on, and stop() takes
    // all of them off whether the subscriptions were still open or had
    // already gone. Said here because until 2026-09-20 it was not true: a
    // subscription torn down after stop() had set its flag but before the
    // event loop was joined erased its own map entry and then skipped the
    // detach, so the sink outlived the server and only the engine's own
    // destruction took it off. A second server on the same engine then saw
    // frames being produced for nobody. core/rpc/server.cpp's sink_closed_
    // has the mechanism.
    [[nodiscard]] static Expected<std::unique_ptr<Server>> create(engine::Engine& engine,
                                                                   const ServerOptions& options);

    virtual ~Server() = default;

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    // The port actually bound, which is what ServerOptions::port asked for
    // unless that was zero. Valid as soon as create returns.
    [[nodiscard]] virtual std::uint16_t port() const = 0;

    // Frames handed to subscribers, and frames dropped because a subscriber
    // had not finished with the previous one. See BACKPRESSURE above.
    [[nodiscard]] virtual std::uint64_t frames_sent() const = 0;
    [[nodiscard]] virtual std::uint64_t frames_dropped() const = 0;

    // See ServerLoad.
    [[nodiscard]] virtual ServerLoad load() const = 0;

    // Stops the event loop and joins its thread. Idempotent, and also run by
    // the destructor, because a server torn down while a client is mid-call
    // is the ordinary case rather than the exception.
    virtual void stop() = 0;

protected:
    Server() = default;
};

}  // namespace revenant::rpc
