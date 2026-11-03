// The kj half of the phone path: each accepted WebSocket becomes a Phone
// (its own engine session, its streams, and a socket adapter), every phone
// hears the engine link's state, and a timer ticks the streams.
//
// THREADING. Everything here runs on the kj event loop thread except
// KjStrand::post and Gateway::on_link_state, which any thread may call; they
// only queue work for the loop. See phone_streams.h for the rest of the
// model.
//
// Do not include <windows.h> before this header (http.h explains why).

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>

#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/compat/http.h>

#include "tools/webgate/engine_link.h"
#include "tools/webgate/hls_playlist.h"
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
    std::deque<std::function<void()>> tasks_;          // guarded by mutex_
    kj::Own<kj::CrossThreadPromiseFulfiller<void>> waiting_;  // guarded by mutex_
};

// The text for a "link" event, shared so the first event a phone gets on
// connect reads the same as later ones.
[[nodiscard]] std::string link_event(LinkState state, const std::string& detail);

class Gateway {
public:
    // hls is the HttpServer's registry, which this gateway's phones fill.
    Gateway(kj::AsyncIoContext& io, EngineLink& link, HlsRegistry& hls);
    ~Gateway();
    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    // HttpServer::on_websocket. The connection lives until the promise
    // settles; its receivers go with it.
    [[nodiscard]] kj::Promise<void> accept(kj::Own<kj::WebSocket> socket);

    // EngineLink::on_state. Any thread.
    void on_link_state(LinkState state, std::string detail);

    // The strand and the tick timer. Never resolves; join it with the
    // server's run().
    [[nodiscard]] kj::Promise<void> run();

    // How many phones are connected. Loop thread.
    [[nodiscard]] std::size_t phones() const { return phones_.size(); }

private:
    struct Phone;
    [[nodiscard]] kj::Promise<void> receive(Phone& phone);
    [[nodiscard]] kj::Promise<void> tick_loop();

    kj::AsyncIoContext& io_;
    EngineLink& link_;
    HlsRegistry& hls_;
    KjStrand strand_;
    std::set<Phone*> phones_;
    LinkState last_state_ = LinkState::Starting;
    std::string last_detail_;
};

}  // namespace revenant::webgate
