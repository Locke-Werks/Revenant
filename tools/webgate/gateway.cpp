#include "tools/webgate/gateway.h"

#include <atomic>
#include <deque>
#include <memory>
#include <thread>
#include <string_view>
#include <utility>

#include "tools/webgate/protocol.h"
#include "tools/webgate/voice_gain.h"

namespace revenant::webgate {

namespace {

constexpr auto kTickInterval = 250 * kj::MILLISECONDS;

// Hands one message at a time to the kj WebSocket. The bytes are owned by the
// promise until the send completes, because kj sends from the caller's buffer.
class KjPhoneSocket final : public PhoneSocket, private kj::TaskSet::ErrorHandler {
public:
    explicit KjPhoneSocket(kj::WebSocket& socket) : socket_(socket), tasks_(*this) {}
    // kj::TaskSet's destructor may throw; PhoneSocket's may not.
    ~KjPhoneSocket() noexcept override = default;
    KjPhoneSocket(const KjPhoneSocket&) = delete;
    KjPhoneSocket& operator=(const KjPhoneSocket&) = delete;

    void do_send(OutgoingMessage message, std::function<void()> done) override
    {
        if (broken_) {
            return;
        }
        auto held = kj::heap<OutgoingMessage>(std::move(message));
        kj::Promise<void> sending = nullptr;
        if (held->kind == OutgoingMessage::Kind::Text) {
            sending = socket_.send(kj::ArrayPtr<const char>(held->text.data(), held->text.size()));
        } else {
            sending = socket_.send(
                kj::ArrayPtr<const kj::byte>(held->binary.data(), held->binary.size()));
        }
        // A failed send means the phone is gone or the socket is wedged.
        // Aborting ends the receive side too, which detaches the session.
        tasks_.add(sending.attach(kj::mv(held))
                       .then([done = std::move(done)] { done(); },
                             [this](kj::Exception&&) { close(); }));
    }

    void close() override
    {
        if (!broken_) {
            broken_ = true;
            socket_.abort();
        }
    }

private:
    void taskFailed(kj::Exception&&) override { close(); }

    kj::WebSocket& socket_;
    kj::TaskSet tasks_;
    bool broken_ = false;
};

}  // namespace

// ---- KjStrand --------------------------------------------------------------

void KjStrand::post(std::function<void()> task)
{
    kj::Own<kj::CrossThreadPromiseFulfiller<void>> wake;
    {
        const std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(task));
        wake = kj::mv(waiting_);
    }
    if (wake.get() != nullptr) {
        wake->fulfill();
    }
}

void KjStrand::drain()
{
    for (;;) {
        std::deque<std::function<void()>> batch;
        {
            const std::lock_guard lock(mutex_);
            batch.swap(tasks_);
        }
        if (batch.empty()) {
            return;
        }
        for (auto& task : batch) {
            task();
        }
    }
}

kj::Promise<void> KjStrand::run()
{
    drain();
    auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
    {
        const std::lock_guard lock(mutex_);
        if (!tasks_.empty()) {
            paf.fulfiller->fulfill();
        } else {
            waiting_ = kj::mv(paf.fulfiller);
        }
    }
    return paf.promise.then([this] { return run(); });
}

// ---- Gateway ---------------------------------------------------------------

// The socket half of a phone. Its destructor detaches the session before the
// adapter the session's streams point at is destroyed.
struct Gateway::Connection {
    Connection(Gateway& owner, kj::Own<kj::WebSocket> ws)
        : gateway(owner), socket(kj::mv(ws)), adapter(*socket)
    {
    }
    ~Connection()
    {
        if (id != 0) {
            gateway.hub_.disconnect(id);
        }
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Gateway& gateway;
    kj::Own<kj::WebSocket> socket;
    KjPhoneSocket adapter;
    std::uint64_t id = 0;

    // While the claim is on its worker thread there is no hub connection to
    // give the phone's commands to, so they wait here in order.
    bool claiming = true;
    std::deque<std::string> early;
};

// What a claim's worker hands back to the loop. Shared, so a phone that left
// during the start can drop its half while the worker still holds the other;
// the Client, if one came back, is destroyed by whichever half goes last,
// which releases the lease it claimed.
struct Gateway::ClaimSlot {
    Expected<std::unique_ptr<rpc::Client>> result{std::unexpected(Error{})};
};

namespace {

[[nodiscard]] StreamsConfig streams_config(HlsRegistry& hls)
{
    StreamsConfig config;
    config.hls = &hls;
    // One reader for every phone: they all follow the same desktop.
    auto display = std::make_shared<DisplaySettingsSource>(registry_display_reader());
    config.display_settings = [display] { return display->current(); };
    auto desktop = std::make_shared<DesktopSettingsSource>(registry_desktop_reader());
    config.desktop_settings = [desktop] { return desktop->current(); };
    auto voice_gain =std::make_shared<VoiceGainSource>(registry_audio_reader());
    config.voice_gain_db = [voice_gain] { return voice_gain->current(); };
    return config;
}

}  // namespace

Gateway::Gateway(kj::AsyncIoContext& io, NewSession new_session, HlsRegistry& hls,
                 HubTiming timing, NewSession session_now)
    : io_(io),
      new_session_(std::move(new_session)),
      hls_(&hls),
      hub_(strand_, streams_config(hls), timing,
           session_now ? std::move(session_now) : new_session_)
{
    hls.on_fetch = [this](std::uint64_t vrx) { hub_.hls_fetched(vrx); };
    hub_.set_claim_runner([this](rpc::LoginMode mode, std::string holder,
                                 std::function<void(Expected<std::unique_ptr<rpc::Client>>)> done) {
        prune_claims();
        claims_.push_back(Claim{});
        Claim& claim = claims_.back();
        claim.done = std::make_shared<std::atomic<bool>>(false);
        claim.thread = std::thread([this, new_session = new_session_, mode,
                                    holder = std::move(holder), done = std::move(done),
                                    finished = claim.done]() mutable {
            auto result = std::make_shared<Expected<std::unique_ptr<rpc::Client>>>(
                new_session(mode, holder));
            // Back on the loop, where the hub lives. A result nobody takes
            // (the gateway shut down first) is destroyed with the task.
            strand_.post([done = std::move(done), result] { done(std::move(*result)); });
            finished->store(true);
        });
    });
    // The slow phone commands, on the same kind of thread and joined the same
    // way, so their engine wait never holds the loop serving HTTP and pings.
    hub_.set_work_runner([this](std::function<std::function<void()>()> work) {
        prune_claims();
        claims_.push_back(Claim{});
        Claim& claim = claims_.back();
        claim.done = std::make_shared<std::atomic<bool>>(false);
        claim.thread = std::thread([this, work = std::move(work), finished = claim.done]() {
            std::function<void()> then = work();
            strand_.post(std::move(then));
            finished->store(true);
        });
    });
}

Gateway::~Gateway()
{
    shutdown();
}

void Gateway::shutdown()
{
    join_claims();
    hub_.clear();
    if (hls_ != nullptr) {
        hls_->on_fetch = nullptr;
        hls_ = nullptr;
    }
}

kj::Promise<void> Gateway::accept(kj::Own<kj::WebSocket> socket, std::string peer)
{
    if (!hub_.make_room()) {
        // Every session is a phone that is connected right now; evicting one
        // would cut off someone listening.
        auto& ws = *socket;
        return ws.close(1013, "too many phones connected to this gateway; try again later")
            .attach(kj::mv(socket))
            .catch_([](kj::Exception&&) {});
    }
    // A phone's session is its own engine Session, claiming the lease as this
    // phone, so it cannot share the link's observing connection. The claim
    // may wait for an engine to be started, seconds on a cold GPU, so it runs
    // on a worker and the loop goes on serving every other socket; this one
    // keeps being read, its commands held until the claim lands, so a phone
    // that leaves during the start is noticed and its claim dropped. A claim
    // the lease refuses still connects: the phone is shown who holds the
    // engine and offered Take over (phone_hub.h).
    const std::string holder = phone_holder(peer);
    auto connection = kj::heap<Connection>(*this, kj::mv(socket));
    auto& ref = *connection;
    if (last_state_ != LinkState::Up) {
        // The link is not up, so this claim is the one that brings an engine.
        ref.adapter.send(OutgoingMessage{.text = engine_starting_event()}, [] {});
    }

    auto slot = std::make_shared<ClaimSlot>();
    auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
    prune_claims();
    claims_.push_back(Claim{});
    Claim& claim = claims_.back();
    claim.done = std::make_shared<std::atomic<bool>>(false);
    claim.thread = std::thread(
        [new_session = new_session_, holder, slot, done = claim.done,
         fulfiller = kj::mv(paf.fulfiller)]() mutable {
            slot->result = new_session(rpc::LoginMode::Claim, holder);
            // Safe after the promise is gone: a cross-thread fulfiller whose
            // promise was cancelled ignores the call.
            fulfiller->fulfill();
            done->store(true);
        });

    auto seated = paf.promise.then([this, &ref, holder, slot]() -> kj::Promise<void> {
        auto client = std::move(slot->result);
        ref.claiming = false;
        const bool refused = !client && parse_lease_refusal(client.error().message).has_value();
        if (!client && !refused) {
            auto text = kj::heap<std::string>(link_event(last_state_, client.error().message));
            auto& ws = *ref.socket;
            return ws.send(kj::ArrayPtr<const char>(text->data(), text->size()))
                .attach(kj::mv(text))
                .then([&ws] { return ws.close(1013, "engine unavailable"); });
        }
        ref.id = refused ? hub_.connect_refused(ref.adapter, holder, client.error().message)
                         : hub_.connect(ref.adapter, std::move(*client), holder);
        while (!ref.early.empty()) {
            const std::string text = std::move(ref.early.front());
            ref.early.pop_front();
            hub_.text(ref.id, text);
        }
        // The receive loop below ends the connection from here on.
        return kj::NEVER_DONE;
    });

    return receive(ref)
        .exclusiveJoin(kj::mv(seated))
        .catch_([](kj::Exception&&) {})
        .attach(kj::mv(connection));
}

void Gateway::prune_claims()
{
    for (auto it = claims_.begin(); it != claims_.end();) {
        if (it->done->load()) {
            it->thread.join();
            it = claims_.erase(it);
        } else {
            ++it;
        }
    }
}

void Gateway::join_claims()
{
    for (auto& claim : claims_) {
        if (claim.thread.joinable()) {
            claim.thread.join();
        }
    }
    claims_.clear();
}


kj::Promise<void> Gateway::receive(Connection& connection)
{
    return connection.socket->receive(kMaxCommandBytes)
        .then([this, &connection](kj::WebSocket::Message&& message) -> kj::Promise<void> {
            if (message.is<kj::WebSocket::Close>()) {
                return kj::READY_NOW;
            }
            if (message.is<kj::String>()) {
                const auto& text = message.get<kj::String>();
                if (connection.claiming) {
                    connection.early.emplace_back(text.cStr(), text.size());
                } else {
                    hub_.text(connection.id, std::string_view(text.cStr(), text.size()));
                }
            }
            // Binary frames: the phone sends none, and one is ignored rather
            // than dropping the connection over it.
            return receive(connection);
        });
}

void Gateway::on_link_state(LinkState state, std::string detail)
{
    strand_.post([this, state, detail = std::move(detail)] {
        last_state_ = state;
        last_detail_ = detail;
        hub_.link_state(state, detail);
    });
}

kj::Promise<void> Gateway::tick_loop()
{
    return io_.provider->getTimer().afterDelay(kTickInterval).then([this] {
        hub_.tick();
        return tick_loop();
    });
}

kj::Promise<void> Gateway::run()
{
    return strand_.run().exclusiveJoin(tick_loop());
}

}  // namespace revenant::webgate
