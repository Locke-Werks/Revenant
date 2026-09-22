// The client half of the wire. Links capnp and nothing else of ours.
//
// This header is compiled twice, into two different processes, against two
// different C runtimes: once /MT inside the main build so the test suite can
// drive a real server, and once /MD inside the Qt project. That is why it
// returns core/rpc/types.h structs and never an engine type, and why it must
// not grow an include of anything under core/engine or core/dsp. See
// core/rpc/types.h for the reasoning and core/rpc/CMakeLists.txt for the two
// builds.
//
// THREADING
//
// A Client owns a thread running the Cap'n Proto event loop. Every method
// here is synchronous from the caller's point of view: it hands work to that
// loop and waits. Calls are serialised, so two threads may call a Client
// without tearing, though they will queue.
//
// The one exception is the spectrum callback, which is invoked ON the event
// loop thread. It must not call back into the same Client, and it must
// return quickly: everything else this connection does is waiting behind it.
//
// A UI copies the frame and posts a WAKE-UP to its own thread. Not the
// frame. This sentence used to say "posts it to its own thread, which is
// what ui/ does with a queued signal", and a second consumer following that
// would rebuild the failure the whole backpressure design exists to prevent:
// a queued signal carrying a 256 KiB frame lets Qt's event queue grow
// without bound the moment the GUI thread falls behind, which at 305 frames
// a second is megabytes in well under a second.
//
// ui/models/engine_link.h is the worked example and does the opposite on
// purpose: cycling buffers, a latch so only one wake-up is in flight, and
// the newest frame wins. Copy that shape rather than this paragraph's
// earlier advice.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.h"
#include "core/rpc/token.h"
#include "core/rpc/types.h"

namespace revenant::rpc {

class Client {
public:
    // address is a host or literal address; the engine binds loopback by
    // default and a remote engine is an explicit choice, not an accident.
    //
    // token is the engine's pre-shared token, exactly kTokenBytes of it. See
    // core/rpc/token.h for where the file lives and how to read it; a caller
    // that wants the ordinary case, one operator with the engine and the UI
    // running as the same account, passes load_token(default_token_path()).
    //
    // THIS CALL NOW ROUND-TRIPS, WHICH IT DID NOT BEFORE
    //
    // The bootstrap capability is lazy in Cap'n Proto, so until 2026-09-20
    // connect() returned the moment the TCP connect did and nothing was
    // exchanged. It now sends Authenticator.login and waits for the answer,
    // so a wrong token is a connect failure carrying the engine's refusal
    // rather than a surprise on the first real call. Session calls are still
    // pipelined on the login result, so a good token costs no extra round
    // trip afterwards.
    //
    // A WRONG TOKEN IS PERMANENT AND THE FAILURE SAYS SO
    //
    // Error::category is ErrorCategory::Unauthenticated for a token this
    // engine will not take, for a token of the wrong length, and for a token
    // file that cannot be read or holds something that is not hexadecimal.
    // It is ErrorCategory::Unreachable when nothing answered at the address,
    // which includes the token file not existing yet, because the engine
    // mints that file on its first run and a client started first finds no
    // engine and no token for the same reason.
    //
    // The two are distinguished by HOW FAR THE HANDSHAKE GOT rather than by
    // the message, which is core/rpc/client.cpp's Phase, and that is what
    // makes the distinction survive a rewording.
    //
    // WHAT THIS PARAGRAPH USED TO SAY, UNDER THE HEADING "A WRONG TOKEN IS
    // PERMANENT AND THIS INTERFACE CANNOT SAY SO":
    //
    //   "core/error.h carries a message and an originating API code with no
    //   category, so a caller reconnecting in a loop cannot tell this refusal
    //   from a connection refused by a server that is not up yet, except by
    //   matching on the message text. ui/models/engine_link.cpp retries every
    //   failure identically and will spin against a wrong token. Widening
    //   Error with a category touches every user of Expected in the tree and
    //   is the right fix rather than this one; it is not on this branch."
    //
    // It was on the next branch. The one clause of it that stayed true is the
    // reach: a category is on Error itself, so it is on the error type every
    // Expected in the tree returns, and it defaults to Unclassified so that
    // the eleven hundred fail() sites that say enough already did not have to
    // be visited.
    [[nodiscard]] static Expected<std::unique_ptr<Client>> connect(
        std::string_view address, std::uint16_t port, std::span<const std::uint8_t> token);

    virtual ~Client() = default;

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;

    [[nodiscard]] virtual Expected<EngineInfo> info() = 0;
    [[nodiscard]] virtual Expected<bool> running() = 0;
    [[nodiscard]] virtual Expected<std::vector<SourceDescriptor>> list_sources() = 0;
    [[nodiscard]] virtual Expected<SourceStats> source_stats() = 0;

    // Points the front end somewhere else, and answers with the centre the
    // device actually took, which a synthesiser with a tuning step will
    // round.
    //
    // A RECEIVER KEEPS ITS FREQUENCY, AND IS REMOVED IF THE FRONT END CANNOT
    // REACH IT ANY MORE.
    //
    // Until this call existed, changing band meant restarting the engine
    // process, which took the operator's receivers, the waterfall's history
    // and the audio with it. None of that happens; what does happen to a
    // receiver changed on 2026-09-21.
    //
    // WHAT THESE TWO PARAGRAPHS USED TO SAY, AND IT IS NOW FALSE. Under the
    // heading "THE RECEIVERS SURVIVE, AND SO DOES EVERYTHING ELSE" they read
    // "Nothing here is torn down: a receiver's centre is a baseband offset,
    // the grid does not know where the front end is pointed, and every
    // subscription keeps running", and then "WHICH IS ALSO THE TRAP. A
    // receiver stays where it is in baseband and is therefore now hearing a
    // different piece of spectrum. A client with receivers open across a
    // retune has to decide what it meant by each one and move it, and this
    // call will not do that for it: there is no reading of 'keep this
    // receiver on 145.1 MHz' that is right for every client, and one imposed
    // here would be wrong for a scanner."
    //
    // The engine does it now. A receiver's offset is rebased to hold the
    // absolute frequency it was tuned to, and one whose CENTRE falls outside
    // the new span is removed. The reason the old behaviour was indefensible
    // is that nothing announced it: an operator retuned from broadcast FM to
    // 435 MHz and their receiver carried on making noise at a frequency they
    // had never chosen, with its highlight sitting at the same place in the
    // span.
    //
    // The scanner objection was real and was overruled rather than missed. A
    // client that wants a receiver to follow the front end can add one at the
    // new centre, which is one call and is explicit; a client that wanted the
    // old behaviour could not tell it had happened. So the surprising case is
    // the one that now needs asking for.
    //
    // A REMOVED RECEIVER IS NOT REPORTED THROUGH THIS CALL. It answers with
    // the centre the device took, because the retune succeeded and a receiver
    // that could not come along is not a failed retune. A client finds out the
    // way it finds out about any receiver that has gone: vrx_status refuses
    // and the subscriptions end. Check vrx_ids after a retune rather than
    // assuming the set is unchanged.
    //
    // EVERY ABSOLUTE FREQUENCY THIS CLIENT IS HOLDING IS STALE WHEN THIS
    // RETURNS. Call info() again and use the new source_center rather than
    // adding the delta: the answer here is where the device landed, which
    // is not always what was asked for. A Detection's center_hz is
    // absolute and belongs to the band that was left; the detector is
    // rebuilt server-side, so poll detections again rather than diffing.
    // RDS decoders are cleared for the same reason.
    //
    // Refused, in the SOURCE's own words, on a source that cannot retune,
    // which is every file and every synthetic scene. The three backends say
    // three different things to do instead. Ask source_can_retune first and
    // grey the control out rather than offering one that always refuses.
    [[nodiscard]] virtual Expected<std::int64_t> set_source_center(std::int64_t center_hz) = 0;

    // The front end's gain, by the stage's own name, answering with what the
    // device took.
    //
    // The names, their ranges, their discrete steps and whether each one has
    // an AGC all come from SourceDescriptor::gain_stages, which list_sources
    // carries. Draw the controls the device says it has: an R820T reports one
    // stage with 29 steps, and a slider that showed the request rather than
    // this answer would show a gain the tuner never held.
    //
    // ON AN RTL-SDR THIS COSTS A THIRD OF A SECOND OF SAMPLES, because the
    // gain registers sit behind the same I2C repeater the tuner does and the
    // transfers have to stop for it. Reported the same way a retune's gap is,
    // and it is not a source change: the epoch does not move.
    [[nodiscard]] virtual Expected<double> set_source_gain(std::string_view stage,
                                                           double db) = 0;

    // Hands the stage to the device's own AGC, or takes it back. Only offer it
    // where GainStage::has_auto says the device will do it.
    [[nodiscard]] virtual Status set_source_gain_auto(std::string_view stage, bool on) = 0;

    // What the OPEN source can do, which is a different question from what
    // list_sources answers.
    //
    // list_sources describes candidates and opens every device index to do it.
    // This describes the one already open and touches no device, and it is the
    // only way to learn a running source's gain stages, its flow control or its
    // sample formats. A client that attached to an engine somebody else started
    // with a URI has never seen a descriptor otherwise, which is what stopped
    // it offering a gain control at all.
    //
    // Nothing when no source is open. That is a state to poll through and not
    // a failure, so it is an empty optional rather than an error.
    [[nodiscard]] virtual Expected<std::optional<SourceDescriptor>> source_descriptor() = 0;

    // Whether the call above will work, and over what range.
    //
    // The range is an ENVELOPE and not a promise: a device with a gap in
    // its coverage reports the outer bounds and still refuses a frequency
    // inside the gap. Outside it, do not bother asking.
    [[nodiscard]] virtual Expected<SourceTuning> source_can_retune() = 0;

    // Opens a source on an engine that has none, and closes the one it has.
    //
    // The URI is the grammar the command line takes, which is what
    // list_sources hands back in SourceDescriptor::uri. Build one by appending
    // the settings an operator chose to that string; do not parse it, because
    // each backend's grammar is its own.
    //
    // open_source IS REFUSED WHEN A SOURCE IS ALREADY OPEN. Close first. It is
    // not a replace, because a replace that failed on the new URI would have
    // destroyed the working one already.
    //
    // EVERYTHING THIS CLIENT WAS HOLDING IS GONE AFTER A CLOSE, which is more
    // than a retune costs and is worth listing rather than discovering:
    //
    //   Every receiver, with its audio, its passband and its RDS decoder.
    //   Their centres were offsets from a baseband whose meaning was the
    //   closed source's, so carrying one forward would place it at a plausible
    //   offset from the wrong centre.
    //
    //   Every subscription. An audio subscriber is told, through ended(); a
    //   spectrum or passband subscriber finds out by the frames stopping,
    //   which is the shape those two already have.
    //
    //   The detector, and any threshold set on it.
    //
    // AND THE SAMPLE INDICES START AGAIN. EngineInfo::source_epoch is what
    // separates the new stream's index zero from the old one's. Read it on
    // every poll rather than at connect: nothing else distinguishes two
    // honest indices into two different streams.
    //
    // close_source succeeds on an engine with nothing open, so a client that
    // closes before every open does not have to know which state it was in. It
    // BLOCKS while the stream stops, which is a device stop plus a GPU flush,
    // and is refused with nothing torn down if that takes longer than five
    // seconds.
    [[nodiscard]] virtual Status open_source(std::string_view uri) = 0;
    [[nodiscard]] virtual Status close_source() = 0;

    [[nodiscard]] virtual Expected<std::uint64_t> add_vrx(const VrxParams& params) = 0;
    [[nodiscard]] virtual Status remove_vrx(std::uint64_t id) = 0;
    [[nodiscard]] virtual Status set_vrx_params(std::uint64_t id, const VrxParams& params) = 0;
    [[nodiscard]] virtual Expected<VrxStatus> vrx_status(std::uint64_t id) = 0;
    [[nodiscard]] virtual Expected<std::vector<std::uint64_t>> vrx_ids() = 0;

    // What the engine's wideband detector is tracking, filtered to this
    // caller's confidence bar. Polled rather than subscribed: a track list is
    // a state that changes ten times a second and an older one is of no use,
    // where a spectrum frame is produced whether anyone looks or not.
    //
    // THE FIRST CALL BUILDS THE DETECTOR AND ANSWERS WITH NOTHING
    //
    // No detector runs until somebody asks for one, because it costs the
    // engine real CPU per frame. So the first call starts it and comes back
    // with an empty list and DetectionList::decisions at zero, which is the
    // one answer that cannot be confused with a quiet band. Poll again.
    //
    // min_confidence is from 0 up to but not including 1, and 1 is refused
    // rather than answered emptily: a track's confidence approaches 1 without
    // reaching it, so a bar of 1 would list nothing however strong the signal
    // is, and an empty list is what a dead band looks like too.
    //
    // Fails on an engine built with no spectrum stage, in that engine's own
    // words, because the detector works on spectrum frames.
    [[nodiscard]] virtual Expected<DetectionList> detections(double min_confidence) = 0;

    // The detector's other threshold, in dB of SNR in the 2500 Hz reference
    // bandwidth, and the one that changes what the detector FINDS rather than
    // what this caller is shown. Engine-wide, so two clients setting it
    // fight; DetectionList::detection_threshold_db reads back the winner.
    //
    // Builds the detector on first use, like detections above, so a client
    // can set its threshold before polling rather than polling once at the
    // wrong one.
    [[nodiscard]] virtual Status set_detection_threshold(double threshold_db) = 0;

    // Invoked on the event loop thread. See THREADING above.
    using FrameCallback = std::function<void(const SpectrumFrame&)>;

    // every_nth of 0 or 1 is every frame. The engine drops the rest before
    // copying them, so asking for fewer costs the engine less and not more.
    //
    // One subscription per Client. Subscribing again replaces the first,
    // because two waterfalls in one process is a reason to want two rates,
    // not two connections, and pretending otherwise would put the drop
    // policy somewhere it cannot be reasoned about.
    [[nodiscard]] virtual Status subscribe_spectrum(std::uint32_t every_nth,
                                                     FrameCallback callback) = 0;
    virtual void unsubscribe_spectrum() = 0;

    // One receiver's passband, on the same thread and the same terms.
    using PassbandCallback = std::function<void(const PassbandFrame&)>;

    // One subscription per receiver per Client, for the same reason
    // subscribe_spectrum allows one: subscribing to the same receiver again
    // replaces the first. Several receivers at once is ordinary and is what
    // a rack of them looks like.
    //
    // Fails on an engine built with no passband stage and for a raw tap,
    // which has no fine stage to transform, in the engine's own words.
    [[nodiscard]] virtual Status subscribe_passband(std::uint64_t vrx,
                                                     std::uint32_t every_nth,
                                                     PassbandCallback callback) = 0;
    virtual void unsubscribe_passband(std::uint64_t vrx) = 0;

    // One receiver's audio, as raw float32 PCM, on the same thread and the
    // same terms as the two above.
    using AudioCallback = std::function<void(const AudioChunk&)>;

    // No further chunk is coming, with the reason for why. Never called for
    // an unsubscribe_audio this client asked for. Optional: pass an empty
    // one and the stream simply stops, which is what a client that is
    // tearing down anyway wants.
    //
    // It exists because audio has no visible failure. A receiver removed out
    // from under a spectrum subscription freezes a picture and a frozen
    // picture is obvious; the same event here produces silence, and silence
    // is what a quiet channel with the squelch shut sounds like.
    //
    // THE REASON IS NOT ALWAYS THE ENGINE'S WORDS, and this line used to say
    // it was. Two things end a stream this client did not end: the receiver
    // being removed, which is the engine's sentence, and a chunk call THIS
    // PROCESS failed, which since 2026-09-20 comes back as the failure's own
    // description. The second was silent before then, so a callback that has
    // only ever seen removals is about to start seeing something else. A
    // client that matches on the text rather than treating it as prose was
    // already wrong and is now visibly so.
    using AudioEndedCallback = std::function<void(const std::string& reason)>;

    // Answers with the buffer depth actually granted, in milliseconds. Zero
    // asks for the default and comes back as the default rather than as
    // zero, and the value is clamped rather than applied silently, because a
    // depth that quietly changed is a dropout nobody can trace.
    //
    // THAT NUMBER IS NOT THE WHOLE CLAMP. A two-chunk floor is applied in
    // frames when the first chunk arrives, because the server cannot convert
    // milliseconds to frames until it knows the receiver's audio rate and
    // how long a chunk is, and it learns both from the first chunk.
    // AudioStats::buffer_frames is the depth being enforced.
    //
    // THERE IS NO every_nth, which is where this departs from the two
    // subscriptions above. Dropping every other spectrum frame halves an
    // update rate and loses nothing anyone wanted; dropping every other
    // audio chunk is a 50 percent duty cycle of silence. A client that wants
    // less audio subscribes to fewer receivers.
    //
    // One subscription per receiver per Client; subscribing to the same
    // receiver again replaces the first. Fails for a receiver that does not
    // exist, before the source is open, and for a raw tap, in the engine's
    // own words for the first two and in the server's for the last.
    [[nodiscard]] virtual Expected<std::uint32_t> subscribe_audio(
        std::uint64_t vrx, std::uint32_t buffer_millis, AudioCallback on_chunk,
        AudioEndedCallback on_ended) = 0;
    virtual void unsubscribe_audio(std::uint64_t vrx) = 0;

    // This subscription's own counters, read off the engine. Per
    // subscription rather than per client and per server, so a second client
    // falling behind on the same receiver does not appear here.
    //
    // Refused once the subscription is over, however it ended: an
    // unsubscribe_audio, a subscribe_audio that replaced it, or an ended
    // callback. All three leave this client holding no subscription on that
    // receiver and all three answer in those words.
    //
    // THE ENDED CASE USED TO ANSWER Ok. Until 2026-09-20 the ended path
    // cleared the callbacks and kept the subscription capability, so a
    // caller that had just been told the receiver was removed could poll
    // this and be handed the dead stream's last counts. A UI polling a
    // status line saw a healthy subscription on a receiver that did not
    // exist.
    [[nodiscard]] virtual Expected<AudioStats> audio_stats(std::uint64_t vrx) = 0;

    // What the RDS decoder on one receiver has accumulated.
    //
    // A POLL AND NOT A SUBSCRIPTION, on the same argument detections is:
    // this is a STATE. PI, PS, RadioText, PTY and the AF list accumulate and
    // are retained, a group completes at most every 87.6 ms, an older
    // snapshot is of no use, and a display redraws on its own timer.
    //
    // THE FIRST CALL IS WHAT STARTS THE DECODER, the same way the first
    // detections call builds the detector. It joins that receiver's audio
    // fan-out, builds the physical and group layers, and answers with an
    // unlocked decoder and zero groups, because it cannot answer otherwise.
    // A caller polls again.
    //
    // Refused, in the engine's own words, for a receiver that does not
    // exist, and in the server's for a receiver that cannot carry a
    // composite. There are FOUR conditions and not the one the audio rate
    // suggests: the demodulator has to be nfm or wfm, the audio has to be
    // real and mono, the audio rate has to clear the receiver's own bound,
    // and the granted passband has to reach 59375 Hz either side of the mix
    // centre. core/rpc/revenant.capnp has all four in full, and the refusal
    // names which one failed rather than saying the receiver is unsuitable.
    //
    // THE RECEIVER THIS NAMES IS NOT THE ONE BEING LISTENED TO, in practice
    // and not by rule. A listening receiver runs at 48 kHz, where the audio
    // decimation filter has already destroyed the 57 kHz subcarrier, so it
    // fails the rate condition. A caller points a DEDICATED receiver at the
    // station: demod wfm, audio_rate 171000, the station's centre.
    //
    // subscribe_audio and this pair were the three unserved surfaces. All
    // three are served now, and this paragraph used to say that the branch
    // serving RDS would change the return type and break every caller. It
    // did.
    //
    // A DECODER THAT STOPPED ANSWERS RATHER THAN REFUSING, so check
    // RdsStation::fault before drawing anything else. It is non-empty only
    // when the receiver delivered a chunk the decoder was not built for, an
    // interleaved pair or a rate its loops are not sized for, and it is
    // terminal for the life of that receiver: both faults are shape, and a
    // retune that changes the shape is refused rather than applied. The
    // fields beside it are frozen at the last chunk the decoder accepted and
    // are still true about the station, which is why they are served instead
    // of thrown away. What this refuses is a receiver that does not exist,
    // in the engine's words, and a receiver that cannot carry a composite,
    // in the server's.
    [[nodiscard]] virtual Expected<RdsStation> rds_station(std::uint64_t vrx) = 0;

    // The region the decoder for this receiver uses, which defaults to
    // RdsRegion::Rds.
    //
    // Settable before the first rds_station call, so a client does not have
    // to poll once at the wrong region and then correct it. It builds the
    // decoder if there is not one yet, and checks the same four conditions,
    // so a receiver that cannot carry a composite is refused here too rather
    // than at the first poll.
    //
    // Called on a receiver whose decoder already exists, it REBUILDS IT AND
    // CLEARS THE ACCUMULATED STATE. The PTY table and the call sign
    // derivation are both region dependent, so keeping the old state would
    // mix two readings of the same bytes in one struct. A caller that sets
    // the region it already had pays that reset anyway, which is why a
    // client seeding this from the tuned frequency should do it once.
    //
    // Per receiver where set_detection_threshold is per engine. Both are as
    // wide as the thing they configure: there is one detector, and there is
    // one decoder per receiver. Two receivers can sit on two stations and
    // there is no reason in the standard why they share a continent.
    //
    // AND PER RECEIVER MEANS SHARED BY EVERY SESSION ON THAT RECEIVER.
    // Two clients polling one receiver hold one decoder between them, so
    // this call from either clears what the other had accumulated, silently.
    // That is the same choice set_detection_threshold makes and for the same
    // reason: a receiver is engine-wide state, two sessions on one already
    // share its tuning and its squelch, and the decoder hangs off the
    // receiver. A client that needs a region of its own creates a receiver
    // of its own, which it is already doing to reach 171000.
    [[nodiscard]] virtual Status set_rds_region(std::uint64_t vrx, RdsRegion region) = 0;

    // Frames this client was sent.
    [[nodiscard]] virtual std::uint64_t frames_received() const = 0;

    // Frames dropped BY THIS CLIENT, which in the current implementation is
    // always zero, and a consumer should not build a display on it.
    //
    // It is kept because the contract is real even though the count is not:
    // the callback runs inline on the event loop thread, and that thread
    // cannot dispatch the next frame until the current one returns, so there
    // is no interleaving in which a frame arrives while the callback is
    // still running. tests/rpc/test_rpc_spectrum.cpp pins it at zero
    // deliberately, including in the slow-subscriber case.
    //
    // This comment used to say the counter told a choppy waterfall which
    // layer skipped. It cannot, and a UI wiring its status line here ships
    // "0 dropped" whatever happens. The frames that genuinely go missing are
    // dropped in the other two places: the engine drops under backpressure
    // when this client has not answered yet, which core/rpc/server.h's
    // BACKPRESSURE section describes and Server::frames_dropped counts, and
    // a UI drops when it keeps only the newest frame for its render thread,
    // which it must count itself. ui/models/engine_link.h separates all
    // three and is the worked example.
    [[nodiscard]] virtual std::uint64_t frames_dropped() const = 0;

protected:
    Client() = default;
};

}  // namespace revenant::rpc
