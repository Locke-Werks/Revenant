// An engine stand-in that can stop answering, for the call deadline tests.
//
// It serves the real schema on loopback: login takes any token and hands out a
// Session that answers info, running and leaseStatus at once while answering
// is on, and never answers them while it is off. Every other method is capnp's
// default UNIMPLEMENTED. No engine, no GPU: what is under test is the client's
// waiting, and a real engine cannot be made to stall on cue.
//
// A call that is never answered is held as a promise the stand-in can see
// cancelled, which is how a test checks that a timed-out call is given up on
// at the engine's end too, not just abandoned by the caller.

#pragma once

#include <atomic>
#include <cstdint>
#include <future>
#include <thread>

#include <capnp/ez-rpc.h>
#include <kj/async.h>

#include "core/rpc/revenant.capnp.h"

namespace revenant::test {

class StallEngine {
public:
    StallEngine() {
        std::promise<std::uint16_t> bound;
        auto port = bound.get_future();
        thread_ = std::thread([this, bound = std::move(bound)]() mutable {
            capnp::EzRpcServer server(kj::heap<AuthImpl>(*this), "127.0.0.1", 0);
            auto& scope = server.getWaitScope();
            // After the server, which is what makes this thread's event loop.
            auto stop = kj::newPromiseAndCrossThreadFulfiller<void>();
            stop_ = kj::mv(stop.fulfiller);
            bound.set_value(static_cast<std::uint16_t>(server.getPort().wait(scope)));
            stop.promise.wait(scope);
        });
        port_ = port.get();
    }

    ~StallEngine() {
        stop_->fulfill();
        thread_.join();
    }

    StallEngine(const StallEngine&) = delete;
    StallEngine& operator=(const StallEngine&) = delete;
    StallEngine(StallEngine&&) = delete;
    StallEngine& operator=(StallEngine&&) = delete;

    [[nodiscard]] std::uint16_t port() const { return port_; }

    // Off: every call this stand-in implements waits forever.
    void answer(bool on) { answering_.store(on); }

    // Calls answered, and calls held unanswered then cancelled by the client.
    [[nodiscard]] std::uint64_t answered() const { return answered_.load(); }
    [[nodiscard]] std::uint64_t cancelled() const { return cancelled_.load(); }

private:
    // Counts its own destruction while still pending, which is capnp
    // delivering the client's Finish for a call it gave up on.
    struct Held {
        explicit Held(StallEngine& owner) : owner_(owner) {}
        ~Held() { owner_.cancelled_.fetch_add(1); }
        Held(const Held&) = delete;
        Held& operator=(const Held&) = delete;
        Held(Held&&) = delete;
        Held& operator=(Held&&) = delete;
        StallEngine& owner_;
    };

    [[nodiscard]] kj::Promise<void> reply() {
        answered_.fetch_add(1);
        return kj::READY_NOW;
    }

    class SessionImpl final : public rpc::schema::Session::Server {
    public:
        explicit SessionImpl(StallEngine& owner) : owner_(owner) {}

        // Every method stalls while answering is off, the unimplemented ones
        // included, as an engine whose RPC thread is stuck does. Cancellation
        // is allowed explicitly: capnp's default holds a call to completion
        // even after the caller's Finish, and the test wants to see it go.
        DispatchCallResult dispatchCall(
            std::uint64_t interface_id, std::uint16_t method_id,
            capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer> context) override {
            if (!owner_.answering_.load()) {
                return {kj::Promise<void>(kj::NEVER_DONE).attach(kj::heap<Held>(owner_)), false,
                        true};
            }
            return rpc::schema::Session::Server::dispatchCall(interface_id, method_id, context);
        }

        kj::Promise<void> info(InfoContext context) override {
            static_cast<void>(context.getResults().initInfo());
            return owner_.reply();
        }
        kj::Promise<void> running(RunningContext context) override {
            context.getResults().setRunning(true);
            return owner_.reply();
        }
        kj::Promise<void> leaseStatus(LeaseStatusContext context) override {
            context.getResults().setHeld(true);
            return owner_.reply();
        }

    private:
        StallEngine& owner_;
    };

    class AuthImpl final : public rpc::schema::Authenticator::Server {
    public:
        explicit AuthImpl(StallEngine& owner) : owner_(owner) {}

        kj::Promise<void> login(LoginContext context) override {
            context.getResults().setSession(kj::heap<SessionImpl>(owner_));
            return kj::READY_NOW;
        }

    private:
        StallEngine& owner_;
    };

    std::thread thread_;
    std::uint16_t port_ = 0;
    kj::Own<kj::CrossThreadPromiseFulfiller<void>> stop_;
    std::atomic<bool> answering_{true};
    std::atomic<std::uint64_t> answered_{0};
    std::atomic<std::uint64_t> cancelled_{0};
};

}  // namespace revenant::test
