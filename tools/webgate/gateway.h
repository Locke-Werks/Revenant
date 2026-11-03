// The kj half of the phone path: each accepted WebSocket becomes a
// connection in a PhoneHub (phone_hub.h), which owns the sessions and lets
// them outlive their sockets; a timer ticks the hub.
//
// THREADING. Everything here runs on the kj event loop thread except
// KjStrand::post and Gateway::on_link_state, which any thread may call; they
// only queue work for the loop. See phone_streams.h for the rest of the
// model.
//
// Do not include <windows.h> before this header (http.h explains why).

#pragma once

#include <atomic>
#include <deque>
#include <list>
#include <memory>
#include <thread>
#include <functional>
#include <mutex>
#include <string>

#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/compat/http.h>

#include "tools/webgate/engine_link.h"
#include "tools/webgate/hls_playlist.h"
#include "tools/webgate/phone_hub.h"
#include "tools/webgate/phone_streams.h"

namespace revenant::webgate {

// A Strand whose tasks run on the kj event loop that called run(). post() may
// be called from any thread; it wakes the loop through a cross-thread
// fulfiller, the one kj primitive that is safe to complete off-thread.
class KjStrand final : public Strand {
public:
    KjStrand() = default;
    // kj's Own has a throwing destructor; this one must not, to match Strand.
    ~KjStrand() noexcept override = default;
    KjStrand(const KjStrand&) = delete;
    KjStrand& operator=(const KjStrand&) = delete;

    void post(std::function<void()> task) override;

    // Runs posted tasks until the promise is cancelled. Never resolves.
    [[nodiscard]] kj::Promise<void> run();

private:
    void drain();

    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;                 // guarded by mutex_
    kj::Own<kj::CrossThreadPromiseFulfiller<void>> waiting_;  // guarded by mutex_
};

class Gateway {
public:
    // hls is the HttpServer's registry; this gateway's sessions fill it and
    // hear about every request it answers.
    // new_session also rebuilds each phone's session after an engine restart.
    Gateway(kj::AsyncIoContext& io, NewSession new_session, HlsRegistry& hls,
            HubTiming timing = {});

    // Calls shutdown().
    ~Gateway();
    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    // HttpServer::on_websocket. The socket lives until the promise settles;
    // its session detaches then rather than dying (phone_hub.h). peer is the
    // phone's address, which names it in the engine's lease.
    [[nodiscard]] kj::Promise<void> accept(kj::Own<kj::WebSocket> socket, std::string peer);

    // EngineLink::on_state. Any thread.
    void on_link_state(LinkState state, std::string detail);

    // The strand and the tick timer. Never resolves; join it with the
    // server's run().
    [[nodiscard]] kj::Promise<void> run();

    // Destroys every session and unhooks the HLS registry, so the server and
    // its registry can go after this in any order. Idempotent.
    void shutdown();

    // Waits for every claim still on its worker thread. Call after
    // EngineLink::cancel_waits, or a claim waiting for an engine holds this
    // up to EngineLinkConfig::demand_wait. Loop thread.
    void join_claims();

private:
    struct Connection;
    struct ClaimSlot;
    struct Claim {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    [[nodiscard]] kj::Promise<void> receive(Connection& connection);
    [[nodiscard]] kj::Promise<void> tick_loop();
    void prune_claims();

    kj::AsyncIoContext& io_;
    NewSession new_session_;
    HlsRegistry* hls_;
    KjStrand strand_;
    PhoneHub hub_;
    LinkState last_state_ = LinkState::Starting;
    std::string last_detail_;
    std::list<Claim> claims_;
};

// {"event":"engine","state":"starting"}: sent to a phone whose claim is
// waiting for an engine to be started for it, ahead of hello. The app shows
// "Starting the engine..." until the link reports up.
[[nodiscard]] std::string engine_starting_event();

}  // namespace revenant::webgate
