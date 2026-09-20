// The client half of the wire, implementing core/rpc/client.h.
//
// WHAT MAY BE INCLUDED HERE, WHICH IS A SHORT LIST ON PURPOSE
//
// capnp, kj, the generated schema, core/rpc/types.h and core/error.h. Nothing
// from core/engine, core/dsp or core/source, not even for a struct
// definition. This translation unit is compiled a second time inside the Qt
// project, against the dynamic CRT, where revenant_core is not linked at all.
// An engine include here builds fine in this tree and fails in that one, so
// the restriction has to be kept by hand. core/rpc/CMakeLists.txt records
// why the split exists.
//
// It is also why the readers below share no code with core/rpc/convert.cpp.
// That file converts between the schema and the engine; this one converts
// between the schema and core/rpc/types.h. The two cannot meet, and the
// duplication is the process boundary being paid for once rather than a
// missed refactor.
//
// THE THREAD MODEL
//
// One kj event loop thread per Client, started by ClientImpl::start and
// joined by the destructor. Every kj and capnp object this file creates lives
// in LoopState on that thread's stack and is destroyed there as the loop
// unwinds. None of them may be a member of ClientImpl: a member is destroyed
// on whichever thread deletes the Client, and a capability or a
// PromiseFulfiller touched from the wrong thread corrupts the promise system
// rather than racing on a field. server.h states the same rule for the other
// end of the connection.
//
// Caller threads reach the loop only through kj::Executor::executeSync, which
// runs a functor there and blocks until the promise it returns resolves. One
// mutex around that makes concurrent callers queue instead of interleaving,
// which is what client.h promises them.
//
// The spectrum callback runs on the loop thread inside the frame() call, and
// that call is not answered until the callback returns. That is what makes
// the engine's one-frame-in-flight rule press on a slow subscriber instead of
// filling a queue on its behalf.
//
// NOTHING THROWS OUT OF HERE
//
// kj is exception-based and this API is not. Every entry point that can reach
// kj catches kj::Exception and std::exception and turns them into an Error.
// The event loop thread swallows the same at its top level, because an
// exception escaping a std::thread is a terminate().

#include "core/rpc/client.h"

#include <atomic>
#include <cstdint>
#include <format>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <capnp/rpc-twoparty.h>
#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/common.h>
#include <kj/exception.h>
#include <kj/memory.h>

#include "core/error.h"
#include "core/rpc/revenant.capnp.h"
#include "core/rpc/types.h"

namespace revenant::rpc {
namespace {

// The cast in read_demod and write_demod, made a compile error to break.
// core/rpc/convert.h holds the same check between the schema and the engine;
// this is the other pair, and this file is the only one that sees both.
static_assert(static_cast<std::uint16_t>(schema::Demod::RAW) ==
              static_cast<std::uint16_t>(Demod::Raw));
static_assert(static_cast<std::uint16_t>(schema::Demod::AM) ==
              static_cast<std::uint16_t>(Demod::Am));
static_assert(static_cast<std::uint16_t>(schema::Demod::NFM) ==
              static_cast<std::uint16_t>(Demod::Nfm));
static_assert(static_cast<std::uint16_t>(schema::Demod::WFM) ==
              static_cast<std::uint16_t>(Demod::Wfm));
static_assert(static_cast<std::uint16_t>(schema::Demod::USB) ==
              static_cast<std::uint16_t>(Demod::Usb));
static_assert(static_cast<std::uint16_t>(schema::Demod::LSB) ==
              static_cast<std::uint16_t>(Demod::Lsb));
static_assert(static_cast<std::uint16_t>(schema::Demod::DSB) ==
              static_cast<std::uint16_t>(Demod::Dsb));
static_assert(static_cast<std::uint16_t>(schema::Demod::CW) ==
              static_cast<std::uint16_t>(Demod::Cw));

// Sets a field for the length of a scope and puts it back on the way out,
// including out of an exception. Both uses are loop-thread-only fields whose
// stale value would be read by code running after the scope: a dangling
// LoopState pointer, and a re-entrancy flag.
template <typename T>
class ScopedSet {
public:
    ScopedSet(T& slot, T active, T restored) : slot_(slot), restored_(restored) {
        slot_ = active;
    }
    ~ScopedSet() { slot_ = restored_; }

    ScopedSet(const ScopedSet&) = delete;
    ScopedSet& operator=(const ScopedSet&) = delete;
    ScopedSet(ScopedSet&&) = delete;
    ScopedSet& operator=(ScopedSet&&) = delete;

private:
    T& slot_;
    T restored_;
};

template <typename T>
struct PromiseValue;

template <typename T>
struct PromiseValue<kj::Promise<T>> {
    using type = T;
};

// A kj exception, said in a sentence a person can act on.
//
// The Error carries no code. core/error.h reserves that field for the
// originating API's own number, and kj has a four-value Type rather than an
// errno, so putting the ordinal there would read in a log as though a driver
// had returned it.
[[nodiscard]] Error translate(std::string_view what, const kj::Exception& failure) {
    const kj::StringPtr description = failure.getDescription();
    std::string detail(description.begin(), description.end());
    if (detail.empty()) {
        detail = "the Cap'n Proto layer gave no detail";
    }

    switch (failure.getType()) {
        case kj::Exception::Type::DISCONNECTED:
            return Error{
                std::format("{}: the connection to the engine is gone: {}", what, detail)};
        case kj::Exception::Type::OVERLOADED:
            return Error{std::format("{}: the engine is out of resources: {}", what, detail)};
        case kj::Exception::Type::UNIMPLEMENTED:
            return Error{std::format(
                "{}: the engine does not implement this call, so it is older than this client: {}",
                what, detail)};
        case kj::Exception::Type::FAILED:
        default:
            return Error{std::format("{}: {}", what, detail)};
    }
}

[[nodiscard]] std::string read_text(capnp::Text::Reader in) {
    return std::string(in.begin(), in.end());
}

// Copied field for field, including a zero denominator. types.h::hertz()
// already decides what a zero denominator means, and a second policy here
// would make the same wire value mean two different things depending on which
// layer looked at it.
[[nodiscard]] Rational read_rational(schema::Rational::Reader in) {
    Rational out;
    out.numerator = in.getNumerator();
    out.denominator = in.getDenominator();
    return out;
}

// An ordinal with no enumerator is rejected rather than cast, for the reason
// core/rpc/convert.h gives in the other direction: a Cap'n Proto enum field
// may legally hold a value the reader's schema has never heard of, which is
// how a client reaches an engine built against a newer one. Mode is the
// parameter where being wrong is inaudible until a recording turns out to be
// unintelligible.
[[nodiscard]] Expected<Demod> read_demod(schema::Demod mode) {
    const auto ordinal = static_cast<std::uint16_t>(mode);
    if (ordinal > static_cast<std::uint16_t>(Demod::Cw)) {
        return fail(std::format(
            "demodulator ordinal {} is not one this client knows; the engine was built against "
            "a newer schema",
            ordinal));
    }
    return static_cast<Demod>(ordinal);
}

[[nodiscard]] schema::Demod write_demod(Demod mode) {
    return static_cast<schema::Demod>(static_cast<std::uint16_t>(mode));
}

[[nodiscard]] DeviceInfo read_device(schema::DeviceInfo::Reader in) {
    DeviceInfo out;
    out.index = in.getIndex();
    out.name = read_text(in.getName());
    out.vendor = read_text(in.getVendor());
    out.discrete = in.getDiscrete();
    out.api_version = in.getApiVersion();
    out.driver_version = in.getDriverVersion();
    return out;
}

[[nodiscard]] GridParams read_grid(schema::GridParams::Reader in) {
    GridParams out;
    out.channels = in.getChannels();
    out.taps_per_branch = in.getTapsPerBranch();
    out.decimation = in.getDecimation();
    return out;
}

[[nodiscard]] SpectrumGeometry read_geometry(schema::SpectrumGeometry::Reader in) {
    SpectrumGeometry out;
    out.transform = in.getTransform();
    out.bins_per_channel = in.getBinsPerChannel();
    out.channels = in.getChannels();
    out.bins = in.getBins();
    out.bin_width = read_rational(in.getBinWidth());
    out.bin_zero = read_rational(in.getBinZero());
    return out;
}

[[nodiscard]] EngineInfo read_engine_info(schema::EngineInfo::Reader in) {
    EngineInfo out;
    out.device = read_device(in.getDevice());
    out.grid = read_grid(in.getGrid());
    out.source_rate = in.getSourceRate();
    out.channel_rate = in.getChannelRate();
    out.channel_spacing = in.getChannelSpacing();
    out.spectrum = read_geometry(in.getSpectrum());
    out.source_center = in.getSourceCenter();
    out.ring_samples = in.getRingSamples();
    out.ring_seconds = in.getRingSeconds();

    // Not ring trivia, whatever the field names say. core/engine/engine.cpp
    // overloads the ring's clamp reason as the one field in EngineInfo that
    // can carry a sentence, so a clamped channel count or block size rides
    // out in it too. Dropping it here would hand a UI an engine that built
    // something other than what was asked for and no way to find out.
    out.ring_clamped = in.getRingClamped();
    out.ring_clamp_reason = read_text(in.getRingClampReason());
    return out;
}

[[nodiscard]] SourceDescriptor read_source_descriptor(schema::SourceDescriptor::Reader in) {
    SourceDescriptor out;
    out.uri = read_text(in.getUri());
    out.backend = read_text(in.getBackend());
    out.display_name = read_text(in.getDisplayName());
    out.unavailable = read_text(in.getUnavailable());
    return out;
}

[[nodiscard]] SourceStats read_source_stats(schema::SourceStats::Reader in) {
    SourceStats out;
    out.blocks_delivered = in.getBlocksDelivered();
    out.samples_delivered = in.getSamplesDelivered();
    out.overrun_events = in.getOverrunEvents();
    out.samples_lost = in.getSamplesLost();
    out.last_loss_index = in.getLastLossIndex();
    out.write_index = in.getWriteIndex();
    return out;
}

[[nodiscard]] Expected<VrxParams> read_vrx_params(schema::VrxParams::Reader in) {
    auto mode = read_demod(in.getDemod());
    if (!mode) {
        return std::unexpected(mode.error());
    }

    VrxParams out;
    out.center = in.getCenter();
    out.bandwidth = in.getBandwidth();
    out.demod = *mode;
    out.audio_rate = in.getAudioRate();
    out.squelch_dbfs = in.getSquelchDbfs();
    out.agc_attack_ms = in.getAgcAttackMs();
    out.agc_decay_ms = in.getAgcDecayMs();
    out.agc_enabled = in.getAgcEnabled();
    out.cw_pitch = in.getCwPitch();
    return out;
}

void write_vrx_params(schema::VrxParams::Builder out, const VrxParams& in) {
    out.setCenter(in.center);
    out.setBandwidth(in.bandwidth);
    out.setDemod(write_demod(in.demod));
    out.setAudioRate(in.audio_rate);
    out.setSquelchDbfs(in.squelch_dbfs);
    out.setAgcAttackMs(in.agc_attack_ms);
    out.setAgcDecayMs(in.agc_decay_ms);
    out.setAgcEnabled(in.agc_enabled);
    out.setCwPitch(in.cw_pitch);
}

[[nodiscard]] VrxPlacement read_vrx_placement(schema::VrxPlacement::Reader in) {
    VrxPlacement out;
    out.channel = in.getChannel();
    out.channel_centre = read_rational(in.getChannelCentre());
    out.residual = read_rational(in.getResidual());
    out.channel_rate = in.getChannelRate();
    out.bandwidth_clamped = in.getBandwidthClamped();
    return out;
}

[[nodiscard]] Expected<VrxStatus> read_vrx_status(schema::VrxStatus::Reader in) {
    auto params = read_vrx_params(in.getParams());
    if (!params) {
        return std::unexpected(params.error());
    }

    VrxStatus out;
    out.id = in.getId();
    out.params = std::move(*params);
    out.placement = read_vrx_placement(in.getPlacement());
    out.level_dbfs = in.getLevelDbfs();
    out.squelch_open = in.getSquelchOpen();
    out.audio_samples = in.getAudioSamples();
    out.audio_dropped = in.getAudioDropped();
    return out;
}

// Fills a caller-owned frame rather than returning one, so the bins buffer can
// be reused across frames. The callback holds a const reference for the length
// of the call and copies whatever it keeps, which client.h already requires of
// it, and the bin count does not change from frame to frame.
void read_spectrum_frame(SpectrumFrame& out, schema::SpectrumFrame::Reader in) {
    auto bins = in.getPowerDb();
    out.power_db.resize(bins.size());
    for (unsigned i = 0; i < bins.size(); ++i) {
        out.power_db[i] = bins[i];
    }
    out.geometry = read_geometry(in.getGeometry());
    out.start = in.getStart();
    out.count = in.getCount();
    out.sequence = in.getSequence();
    out.floor_db = in.getFloorDb();
    out.ceiling_db = in.getCeilingDb();
    out.percentile_low_db = in.getPercentileLowDb();
    out.percentile_high_db = in.getPercentileHighDb();
}

// Everything the event loop thread owns, in one place on its own stack.
//
// See the thread model at the top of the file. These three cannot be members
// of ClientImpl, because a Client is deleted on a caller's thread and all
// three must be destroyed on the loop's.
struct LoopState {
    schema::Session::Client session;

    // Fulfilled from inside an executeSync, which is the only way to reach it:
    // kj requires a PromiseFulfiller be fulfilled on the thread that made it.
    kj::Own<kj::PromiseFulfiller<void>> shutdown;

    // Null when there is no subscription. kj::Maybe would say the same thing,
    // but the idiom for reading one differs between kj releases (this build
    // has KJ_IF_MAYBE and no kj::none) while a null kj::Own reads the same in
    // every version.
    kj::Own<schema::SpectrumSubscription::Client> subscription;
};

class ClientImpl;

// The capability the engine calls. Its whole job is to get onto ClientImpl,
// which owns the callback and the counters.
class SpectrumReceiverImpl final : public schema::SpectrumReceiver::Server {
public:
    explicit SpectrumReceiverImpl(ClientImpl& owner) : owner_(owner) {}

    kj::Promise<void> frame(FrameContext context) override;

private:
    // No lifetime problem to solve: the Client's destructor stops the loop and
    // joins it before any member of the Client is destroyed, and the loop
    // unwinding is what drops the last reference to this object.
    ClientImpl& owner_;
};

class ClientImpl final : public Client {
public:
    ClientImpl() = default;

    // kj::Executor's destructor is declared noexcept(false), which makes this
    // class's implicit destructor potentially throwing: a wider exception
    // specification than the Client destructor it overrides, and so a compile
    // error. Hence the explicit noexcept. The body catches everything it calls
    // that can fail, and the one throw left is joining the loop from the loop,
    // which is the misuse client.h already rules out.
    ~ClientImpl() noexcept override;

    [[nodiscard]] Status start(std::string address, std::uint16_t port);

    [[nodiscard]] Expected<EngineInfo> info() override;
    [[nodiscard]] Expected<bool> running() override;
    [[nodiscard]] Expected<std::vector<SourceDescriptor>> list_sources() override;
    [[nodiscard]] Expected<SourceStats> source_stats() override;

    [[nodiscard]] Expected<std::uint64_t> add_vrx(const VrxParams& params) override;
    [[nodiscard]] Status remove_vrx(std::uint64_t id) override;
    [[nodiscard]] Status set_vrx_params(std::uint64_t id, const VrxParams& params) override;
    [[nodiscard]] Expected<VrxStatus> vrx_status(std::uint64_t id) override;
    [[nodiscard]] Expected<std::vector<std::uint64_t>> vrx_ids() override;

    [[nodiscard]] Status subscribe_spectrum(std::uint32_t every_nth,
                                            FrameCallback callback) override;
    void unsubscribe_spectrum() override;

    [[nodiscard]] std::uint64_t frames_received() const override;
    [[nodiscard]] std::uint64_t frames_dropped() const override;

    // Loop thread only, called by SpectrumReceiverImpl.
    void deliver(schema::SpectrumFrame::Reader in);

private:
    void run(const std::string& address, std::uint16_t port, std::promise<Status>& ready);

    // Loop thread only.
    [[nodiscard]] kj::Promise<void> end_subscription(LoopState& state);

    // Runs body on the event loop thread and waits for the promise it returns,
    // translating whatever comes back into an Expected.
    //
    // Defined here rather than out of line because the return type is deduced
    // from the promise body hands back, and spelling that twice is worse than
    // having a template body in a class definition.
    template <typename Func>
    auto on_loop(std::string_view what, Func&& body)
        -> Expected<typename PromiseValue<std::invoke_result_t<Func&, LoopState&>>::type> {
        using Value = typename PromiseValue<std::invoke_result_t<Func&, LoopState&>>::type;

        // Checked before the mutex, not after. A callback that called back
        // into its own Client would otherwise block on a lock the thread
        // waiting for it is holding, and the hang would say nothing about
        // which rule was broken.
        if (std::this_thread::get_id() == loop_id_) {
            return fail(std::format(
                "{}: called from the spectrum callback. client.h forbids it, because the "
                "callback already runs on the event loop thread and this call waits for that "
                "thread",
                what));
        }

        if (executor_.get() == nullptr) {
            return fail(std::format("{}: this client is not connected", what));
        }

        const std::lock_guard<std::mutex> serialise(calls_);

        try {
            auto deliver_to_loop = [this, &body]() {
                LoopState* state = state_;
                if (state == nullptr) {
                    kj::throwFatalException(
                        KJ_EXCEPTION(DISCONNECTED, "the client event loop is shutting down"));
                }
                return body(*state);
            };

            if constexpr (std::is_void_v<Value>) {
                executor_->executeSync(deliver_to_loop);
                return {};
            } else {
                return executor_->executeSync(deliver_to_loop);
            }
        } catch (const kj::Exception& failure) {
            return std::unexpected(translate(what, failure));
        } catch (const std::exception& failure) {
            return std::unexpected(Error{std::format("{}: {}", what, failure.what())});
        }
    }

    std::thread loop_;

    // Written on the loop thread before the start handshake completes and not
    // again, so a caller that has seen start() return sees a settled value.
    // The reference is counted, so holding it is safe even if the loop exits
    // under us; kj destroys an unreferenced Executor with its event loop.
    kj::Own<const kj::Executor> executor_;
    std::thread::id loop_id_{};

    // Loop thread only, all four. state_ is cleared before the loop's stack
    // unwinds, so a functor delivered during teardown sees null rather than a
    // dead pointer.
    LoopState* state_ = nullptr;
    FrameCallback callback_;
    bool callback_active_ = false;
    SpectrumFrame scratch_;

    // client.h says calls queue. This is what makes them.
    std::mutex calls_;

    // Read from any thread and ordered against nothing: they are counters for
    // a display, not a handshake.
    //
    // frames_dropped_ counts re-entrant delivery and nothing else, and
    // deliver() explains why that leaves it at zero for the life of any
    // client this file can produce.
    std::atomic<std::uint64_t> frames_received_{0};
    std::atomic<std::uint64_t> frames_dropped_{0};
};

kj::Promise<void> SpectrumReceiverImpl::frame(FrameContext context) {
    owner_.deliver(context.getParams().getFrame());

    // Returning only now, after the callback has run, is the backpressure.
    // server.h allows one frame in flight per subscription, so the engine
    // cannot start another until this return reaches it, and a slow callback
    // makes the engine drop frames rather than queue them here.
    return kj::READY_NOW;
}

ClientImpl::~ClientImpl() noexcept {
    {
        const std::lock_guard<std::mutex> serialise(calls_);

        if (executor_.get() != nullptr && std::this_thread::get_id() != loop_id_) {
            try {
                executor_->executeSync([this]() {
                    if (state_ != nullptr) {
                        state_->shutdown->fulfill();
                    }
                });
            } catch (const kj::Exception&) {
                // The loop is already gone. The join below is all that is left
                // to do, and there is nobody to report this to from here.
            } catch (const std::exception&) {
            }
        }
    }

    // Outside the lock: a frame may still be in the callback, and the callback
    // runs on the thread being joined. Every member it touches is still alive,
    // because members are destroyed after this body returns.
    if (loop_.joinable()) {
        loop_.join();
    }
}

Status ClientImpl::start(std::string address, std::uint16_t port) {
    std::promise<Status> ready;
    std::future<Status> settled = ready.get_future();

    try {
        // The promise is moved into the thread rather than captured by
        // reference: start() returns as soon as the handshake lands, and its
        // frame goes with it while the loop thread runs on.
        loop_ = std::thread(
            [this, host = std::move(address), port, ready = std::move(ready)]() mutable {
                run(host, port, ready);
            });
    } catch (const std::system_error& failure) {
        return fail(std::format("could not start the client event loop thread: {}",
                                failure.what()));
    }

    Status result;
    try {
        result = settled.get();
    } catch (const std::exception& failure) {
        result = fail(std::format("the client event loop thread reported nothing: {}",
                                  failure.what()));
    }

    if (!result) {
        loop_.join();
        return result;
    }
    return {};
}

void ClientImpl::run(const std::string& address, std::uint16_t port,
                     std::promise<Status>& ready) {
    const std::string context = std::format("connecting to {}:{}", address, port);

    bool announced = false;
    const auto announce = [&](Status result) {
        if (!announced) {
            announced = true;
            ready.set_value(std::move(result));
        }
    };

    try {
        kj::AsyncIoContext io = kj::setupAsyncIo();

        auto resolved = io.provider->getNetwork()
                            .parseAddress(kj::StringPtr(address.c_str()), port)
                            .wait(io.waitScope);
        auto stream = resolved->connect().wait(io.waitScope);

        capnp::TwoPartyClient rpc(*stream);
        auto shutdown = kj::newPromiseAndFulfiller<void>();

        // The empty brace is a null kj::Own: its constructor from nullptr is
        // explicit, so it cannot be spelled here.
        LoopState state{
            rpc.bootstrap().castAs<schema::Session>(),
            kj::mv(shutdown.fulfiller),
            {},
        };

        // Declared after `state` so it clears the pointer before `state` is
        // destroyed. The event loop outlives LoopState by the length of this
        // unwinding, and an executeSync delivered in that window would
        // otherwise dereference a dead pointer.
        const ScopedSet<LoopState*> published{state_, &state, nullptr};

        loop_id_ = std::this_thread::get_id();
        executor_ = kj::getCurrentThreadExecutor().addRef();

        announce(Status{});

        // Runs until the destructor fulfills this from inside an executeSync.
        // A lost connection does not end it: the session goes on failing
        // calls, and it is the owner who decides when to stop.
        shutdown.promise.wait(io.waitScope);
    } catch (const kj::Exception& failure) {
        announce(std::unexpected(translate(context, failure)));
    } catch (const std::exception& failure) {
        announce(fail(std::format("{}: {}", context, failure.what())));
    } catch (...) {
        announce(fail(std::format("{}: the client event loop failed in a way it could not "
                                  "describe",
                                  context)));
    }

    // Reached with announced already true on every ordinary path. It is here
    // so that a failure between the handshake and the wait cannot leave
    // start() blocked on a future nobody will ever set.
    announce(fail("the client event loop stopped without saying why"));
}

Expected<EngineInfo> ClientImpl::info() {
    return on_loop("info", [](LoopState& state) {
        return state.session.infoRequest().send().then(
            [](auto&& response) { return read_engine_info(response.getInfo()); });
    });
}

Expected<bool> ClientImpl::running() {
    return on_loop("running", [](LoopState& state) {
        return state.session.runningRequest().send().then(
            [](auto&& response) { return response.getRunning(); });
    });
}

Expected<std::vector<SourceDescriptor>> ClientImpl::list_sources() {
    return on_loop("list_sources", [](LoopState& state) {
        return state.session.listSourcesRequest().send().then([](auto&& response) {
            auto sources = response.getSources();
            std::vector<SourceDescriptor> out;
            out.reserve(sources.size());
            for (auto source : sources) {
                out.push_back(read_source_descriptor(source));
            }
            return out;
        });
    });
}

Expected<SourceStats> ClientImpl::source_stats() {
    return on_loop("source_stats", [](LoopState& state) {
        return state.session.sourceStatsRequest().send().then(
            [](auto&& response) { return read_source_stats(response.getStats()); });
    });
}

Expected<std::uint64_t> ClientImpl::add_vrx(const VrxParams& params) {
    return on_loop("add_vrx", [&params](LoopState& state) {
        auto request = state.session.addVrxRequest();
        write_vrx_params(request.initParams(), params);
        return request.send().then([](auto&& response) { return response.getId(); });
    });
}

Status ClientImpl::remove_vrx(std::uint64_t id) {
    return on_loop("remove_vrx", [id](LoopState& state) {
        auto request = state.session.removeVrxRequest();
        request.setId(id);
        return request.send().ignoreResult();
    });
}

Status ClientImpl::set_vrx_params(std::uint64_t id, const VrxParams& params) {
    return on_loop("set_vrx_params", [id, &params](LoopState& state) {
        auto request = state.session.setVrxParamsRequest();
        request.setId(id);
        write_vrx_params(request.initParams(), params);
        return request.send().ignoreResult();
    });
}

Expected<VrxStatus> ClientImpl::vrx_status(std::uint64_t id) {
    // Two Expecteds deep, and they mean different things. The outer one is
    // whether the call happened; the inner one is whether what came back is
    // something this client can name, which is read_demod's problem. Flattened
    // here rather than in on_loop, because this is the only call that carries
    // a field the schema can legally hold and this build cannot interpret.
    auto response = on_loop("vrx_status", [id](LoopState& state) {
        auto request = state.session.vrxStatusRequest();
        request.setId(id);
        return request.send().then(
            [](auto&& reply) { return read_vrx_status(reply.getStatus()); });
    });

    if (!response) {
        return std::unexpected(response.error());
    }
    return std::move(*response);
}

Expected<std::vector<std::uint64_t>> ClientImpl::vrx_ids() {
    return on_loop("vrx_ids", [](LoopState& state) {
        return state.session.vrxIdsRequest().send().then([](auto&& response) {
            auto ids = response.getIds();
            std::vector<std::uint64_t> out;
            out.reserve(ids.size());
            for (auto id : ids) {
                out.push_back(id);
            }
            return out;
        });
    });
}

kj::Promise<void> ClientImpl::end_subscription(LoopState& state) {
    if (state.subscription.get() == nullptr) {
        return kj::READY_NOW;
    }

    // Cancel and then drop. Dropping alone ends the subscription, as the
    // schema says, but waiting for the cancel to return is what makes the stop
    // ordered: Cap'n Proto delivers calls and returns on one connection in
    // order, so any frame() the engine sent before answering this has already
    // been dispatched by the time the answer arrives. A caller that
    // unsubscribes and then tears down what its callback touches is therefore
    // not racing a frame already on the wire.
    auto cancelled = state.subscription->cancelRequest().send().ignoreResult();
    state.subscription = nullptr;
    callback_ = nullptr;

    // A cancel that failed because the connection died has ended the
    // subscription just as thoroughly.
    return cancelled.catch_([](kj::Exception&&) {});
}

Status ClientImpl::subscribe_spectrum(std::uint32_t every_nth, FrameCallback callback) {
    if (!callback) {
        return fail("subscribe_spectrum: the callback is empty. unsubscribe_spectrum is how a "
                    "subscription ends");
    }

    return on_loop("subscribe_spectrum", [this, every_nth, &callback](LoopState& state) {
        // Replacing, per client.h, and the old one is ended first so that a
        // frame from it cannot arrive at the new callback.
        return end_subscription(state).then([this, &state, every_nth, &callback]() {
            // Set before the request goes out, not after it returns. The
            // engine may call frame() on the receiver before it answers the
            // subscribe, and a callback installed afterwards would miss it.
            callback_ = std::move(callback);

            auto request = state.session.subscribeSpectrumRequest();
            request.setReceiver(
                schema::SpectrumReceiver::Client(kj::heap<SpectrumReceiverImpl>(*this)));
            request.setEveryNth(every_nth);

            return request.send()
                .then([&state](auto&& response) {
                    state.subscription = kj::heap<schema::SpectrumSubscription::Client>(
                        response.getSubscription());
                })
                .catch_([this](kj::Exception&& failure) -> kj::Promise<void> {
                    // A subscription that never took has nothing to call back
                    // into, so do not hold the caller's closure alive on the
                    // strength of it.
                    callback_ = nullptr;
                    return kj::Promise<void>(kj::mv(failure));
                });
        });
    });
}

void ClientImpl::unsubscribe_spectrum() {
    // No error channel here, and none is wanted. The only failure this could
    // report is a connection that has already died, which has already ended
    // the subscription, so there would be nothing for a caller to do with it.
    static_cast<void>(
        on_loop("unsubscribe_spectrum", [this](LoopState& state) {
            return end_subscription(state);
        }));
}

std::uint64_t ClientImpl::frames_received() const {
    return frames_received_.load(std::memory_order_relaxed);
}

std::uint64_t ClientImpl::frames_dropped() const {
    return frames_dropped_.load(std::memory_order_relaxed);
}

void ClientImpl::deliver(schema::SpectrumFrame::Reader in) {
    frames_received_.fetch_add(1, std::memory_order_relaxed);

    if (!callback_) {
        // Sent, but there is nobody to hand it to: an unsubscribe that crossed
        // a frame already on the wire. Not a drop in the sense client.h
        // counts, which is this client failing to keep up.
        return;
    }

    if (callback_active_) {
        // Unreachable, and kept as a guard rather than as a statistic.
        //
        // The callback runs inline on this thread and frame() is not answered
        // until it returns, so the engine cannot deliver a second frame while
        // the first is in the callback, and the loop thread is the only
        // thread that dispatches frame() at all. Nothing re-enters here short
        // of a callback driving the event loop itself, which it is given no
        // handle to do. tests/rpc/test_rpc_spectrum.cpp asserts
        // frames_dropped() reads zero even against a subscriber slow enough
        // to make the engine drop, and that zero is the assertion's point: a
        // client that grew a queue would make it move.
        //
        // So this is not the number a display wants, and client.h's summary
        // of it as frames the callback could not keep up with is the shape of
        // the intent rather than of the mechanism. A slow subscriber does
        // cause drops, but the engine executes them under the backpressure
        // core/rpc/server.h describes and counts them on its own side. The
        // only trace that reaches a client is SpectrumFrame::sequence, the
        // engine's own frame counter, jumping by more than the every_nth this
        // subscription asked for. ui/models/engine_link.cpp measures it from
        // there, because Client exposes no accessor to carry it and client.h
        // is a contract between two separate builds rather than somewhere to
        // grow one casually.
        frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const ScopedSet<bool> active{callback_active_, true, false};

    read_spectrum_frame(scratch_, in);
    callback_(scratch_);
}

}  // namespace

Expected<std::unique_ptr<Client>> Client::connect(std::string_view address, std::uint16_t port) {
    if (address.empty()) {
        return fail("cannot connect: no address was given");
    }
    if (port == 0) {
        // ServerOptions::port defaults to zero meaning "bind whatever is
        // free", so a caller that forwards its own options here asks the OS to
        // connect to port zero and gets a message about nothing listening.
        // Server::port() reports the port actually bound.
        return fail("cannot connect: zero is not a port to connect to. A server started on an "
                    "ephemeral port reports the real one through Server::port()");
    }

    auto client = std::make_unique<ClientImpl>();
    if (auto started = client->start(std::string(address), port); !started) {
        return std::unexpected(started.error());
    }
    return std::unique_ptr<Client>(std::move(client));
}

}  // namespace revenant::rpc
