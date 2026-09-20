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
    // A WRONG TOKEN IS PERMANENT AND THIS INTERFACE CANNOT SAY SO
    //
    // core/error.h carries a message and an originating API code with no
    // category, so a caller reconnecting in a loop cannot tell this refusal
    // from a connection refused by a server that is not up yet, except by
    // matching on the message text. ui/models/engine_link.cpp retries every
    // failure identically and will spin against a wrong token. Widening
    // Error with a category touches every user of Expected in the tree and
    // is the right fix rather than this one; it is not on this branch.
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
