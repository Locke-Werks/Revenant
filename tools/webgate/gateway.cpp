#include "tools/webgate/gateway.h"

#include <memory>
#include <string_view>
#include <utility>

#include "tools/webgate/protocol.h"
#include "tools/webgate/session.h"

namespace revenant::webgate {

namespace {

constexpr auto kTickInterval = 250 * kj::MILLISECONDS;

[[nodiscard]] std::string_view state_name(LinkState state)
{
    switch (state) {
        case LinkState::Starting: return "starting";
        case LinkState::Waiting: return "waiting";
        case LinkState::Up: return "up";
        case LinkState::Refused: return "refused";
    }
    return "waiting";
}

// Hands one message at a time to the kj WebSocket. The bytes are owned by the
// promise until the send completes, because kj sends from the caller's buffer.
class KjPhoneSocket final : public PhoneSocket, private kj::TaskSet::ErrorHandler {
public:
    explicit KjPhoneSocket(kj::WebSocket& socket) : socket_(socket), tasks_(*this) {}
    // kj::TaskSet's destructor may throw; PhoneSocket's may not.
    ~KjPhoneSocket() noexcept override = default;
    KjPhoneSocket(const KjPhoneSocket&) = delete;
    KjPhoneSocket& operator=(const KjPhoneSocket&) = delete;

    void send(OutgoingMessage message, std::function<void()> done) override
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
        // A failed send means the phone is gone. The receive side ends the
        // connection; calling done here would only start the next doomed send.
        tasks_.add(sending.attach(kj::mv(held))
                       .then([done = std::move(done)] { done(); },
                             [this](kj::Exception&&) { broken_ = true; }));
    }

private:
    void taskFailed(kj::Exception&&) override { broken_ = true; }

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

std::string link_event(LinkState state, const std::string& detail)
{
    return event("link",
                 JsonObject().add_string("state", state_name(state)).add_string("detail", detail));
}

// Member order is teardown order, reversed: the streams unsubscribe through
// the session's client, the socket adapter cancels its sends before the
// streams it calls back into are gone, and the session removes the phone's
// receivers last.
struct Gateway::Phone {
    Phone(Gateway& owner, kj::Own<kj::WebSocket> ws, std::unique_ptr<rpc::Client> client)
        : gateway(owner),
          socket(kj::mv(ws)),
          session(std::move(client)),
          adapter(*socket)
    {
        StreamsConfig config;
        config.hls = &gateway.hls_;
        streams = std::make_unique<PhoneStreams>(session, adapter, gateway.strand_, config);
        gateway.phones_.insert(this);
    }

    ~Phone()
    {
        gateway.phones_.erase(this);
        streams.reset();
    }

    Phone(const Phone&) = delete;
    Phone& operator=(const Phone&) = delete;

    Gateway& gateway;
    kj::Own<kj::WebSocket> socket;
    PhoneSession session;
    KjPhoneSocket adapter;
    std::unique_ptr<PhoneStreams> streams;
};

Gateway::Gateway(kj::AsyncIoContext& io, EngineLink& link, HlsRegistry& hls)
    : io_(io), link_(link), hls_(hls)
{
}

Gateway::~Gateway() = default;

kj::Promise<void> Gateway::accept(kj::Own<kj::WebSocket> socket)
{
    // Blocks the loop for one loopback connect. A phone's session is its own
    // engine Session, so it cannot share the link's connection.
    auto client = link_.new_session();
    if (!client) {
        auto text = kj::heap<std::string>(link_event(link_.state(), client.error().message));
        auto sending = socket->send(kj::ArrayPtr<const char>(text->data(), text->size()));
        auto& ws = *socket;
        return sending.attach(kj::mv(text))
            .then([&ws] { return ws.close(1013, "engine unavailable"); })
            .attach(kj::mv(socket))
            .catch_([](kj::Exception&&) {});
    }

    auto phone = kj::heap<Phone>(*this, kj::mv(socket), std::move(*client));
    phone->streams->start();
    phone->streams->send_event(link_event(last_state_, last_detail_));
    auto& ref = *phone;
    return receive(ref).catch_([](kj::Exception&&) {}).attach(kj::mv(phone));
}

kj::Promise<void> Gateway::receive(Phone& phone)
{
    return phone.socket->receive(kMaxCommandBytes)
        .then([this, &phone](kj::WebSocket::Message&& message) -> kj::Promise<void> {
            if (message.is<kj::WebSocket::Close>()) {
                return kj::READY_NOW;
            }
            if (message.is<kj::String>()) {
                const auto& text = message.get<kj::String>();
                phone.streams->on_text(std::string_view(text.cStr(), text.size()));
            }
            // Binary frames: the phone sends none, and one is ignored rather
            // than dropping the connection over it.
            return receive(phone);
        });
}

void Gateway::on_link_state(LinkState state, std::string detail)
{
    strand_.post([this, state, detail = std::move(detail)] {
        last_state_ = state;
        last_detail_ = detail;
        const auto text = link_event(state, detail);
        for (Phone* phone : phones_) {
            phone->streams->send_event(text);
        }
    });
}

kj::Promise<void> Gateway::tick_loop()
{
    return io_.provider->getTimer().afterDelay(kTickInterval).then([this] {
        for (Phone* phone : phones_) {
            phone->streams->tick();
        }
        return tick_loop();
    });
}

kj::Promise<void> Gateway::run()
{
    return strand_.run().exclusiveJoin(tick_loop());
}

}  // namespace revenant::webgate
