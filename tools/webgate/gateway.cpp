#include "tools/webgate/gateway.h"

#include <memory>
#include <string_view>
#include <utility>

#include "tools/webgate/protocol.h"

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
};

namespace {

[[nodiscard]] StreamsConfig streams_config(HlsRegistry& hls)
{
    StreamsConfig config;
    config.hls = &hls;
    // One reader for every phone: they all follow the same desktop.
    auto display = std::make_shared<DisplaySettingsSource>(registry_display_reader());
    config.display_settings = [display] { return display->current(); };
    return config;
}

}  // namespace

Gateway::Gateway(kj::AsyncIoContext& io, NewSession new_session, HlsRegistry& hls,
                 HubTiming timing)
    : io_(io),
      new_session_(std::move(new_session)),
      hls_(&hls),
      hub_(strand_, streams_config(hls), timing, new_session_)
{
    hls.on_fetch = [this](std::uint64_t vrx) { hub_.hls_fetched(vrx); };
}

Gateway::~Gateway()
{
    shutdown();
}

void Gateway::shutdown()
{
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
    // Blocks the loop for one loopback connect. A phone's session is its own
    // engine Session, claiming the lease as this phone, so it cannot share
    // the link's observing connection. A claim the lease refuses still
    // connects: the phone is shown who holds the engine and offered Take
    // over (phone_hub.h).
    const std::string holder = phone_holder(peer);
    auto client = new_session_(rpc::LoginMode::Claim, holder);
    const bool refused = !client && parse_lease_refusal(client.error().message).has_value();
    if (!client && !refused) {
        auto text = kj::heap<std::string>(link_event(last_state_, client.error().message));
        auto sending = socket->send(kj::ArrayPtr<const char>(text->data(), text->size()));
        auto& ws = *socket;
        return sending.attach(kj::mv(text))
            .then([&ws] { return ws.close(1013, "engine unavailable"); })
            .attach(kj::mv(socket))
            .catch_([](kj::Exception&&) {});
    }

    auto connection = kj::heap<Connection>(*this, kj::mv(socket));
    connection->id = refused
                         ? hub_.connect_refused(connection->adapter, holder, client.error().message)
                         : hub_.connect(connection->adapter, std::move(*client), holder);
    auto& ref = *connection;
    return receive(ref).catch_([](kj::Exception&&) {}).attach(kj::mv(connection));
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
                hub_.text(connection.id, std::string_view(text.cStr(), text.size()));
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
