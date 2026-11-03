// One connected phone, as far as the engine is concerned.
//
// A PhoneSession owns the phone's own rpc::Client, which is its own engine
// Session, and remembers what the phone asked for so the socket layer (Task
// 10) can act on it. The engine's vrx_ids() lists every session's receivers,
// so independence between phones is by ownership: this object tracks the ids
// it created itself and its destructor removes those and no others.
//
// THREADS. Not synchronised. A phone's commands arrive in order on its one
// socket and are dispatched one at a time; anything else touching a session
// (the stream pumps in Task 10) must go through the same strand or add its
// own lock around these accessors.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "core/rpc/client.h"
#include "core/rpc/types.h"
#include "tools/webgate/spectrum_reducer.h"

namespace revenant::webgate {

class PhoneSession {
public:
    explicit PhoneSession(std::unique_ptr<rpc::Client> client);

    // Removes the receivers this session created, ignoring failures: the
    // engine may already have dropped them (a retune, or the link going down),
    // and a destructor has nowhere to report to.
    ~PhoneSession();

    PhoneSession(const PhoneSession&) = delete;
    PhoneSession& operator=(const PhoneSession&) = delete;
    PhoneSession(PhoneSession&&) = delete;
    PhoneSession& operator=(PhoneSession&&) = delete;

    [[nodiscard]] rpc::Client& client() { return *client_; }

    // ---- Receivers this phone created ----
    [[nodiscard]] bool owns(std::uint64_t vrx) const { return owned_.contains(vrx); }
    [[nodiscard]] std::vector<std::uint64_t> owned_ids() const;

    // The params last sent for a receiver, kept so a receiver the engine
    // dropped can be recreated. Null when the id is not owned.
    [[nodiscard]] const rpc::VrxParams* last_params(std::uint64_t vrx) const;

    void adopt(std::uint64_t vrx, const rpc::VrxParams& params);
    void remember(std::uint64_t vrx, const rpc::VrxParams& params);
    void forget(std::uint64_t vrx);

    // ---- Requested stream state ----
    // Recorded by dispatch. Task 10 wires the actual spectrum and audio
    // streams to these; until then they are only state.
    [[nodiscard]] const std::optional<View>& view() const { return view_; }
    void set_view(const View& view) { view_ = view; }

    [[nodiscard]] std::optional<std::uint64_t> audio_vrx() const { return audio_vrx_; }
    void set_audio_vrx(std::optional<std::uint64_t> vrx) { audio_vrx_ = vrx; }

    [[nodiscard]] bool keep_listening() const { return keep_listening_; }
    void set_keep_listening(bool on) { keep_listening_ = on; }

private:
    std::unique_ptr<rpc::Client> client_;
    std::map<std::uint64_t, rpc::VrxParams> owned_;
    std::optional<View> view_;
    std::optional<std::uint64_t> audio_vrx_;
    bool keep_listening_ = false;
};

}  // namespace revenant::webgate
