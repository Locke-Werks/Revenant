// One connected phone, as far as the engine is concerned.
//
// A PhoneSession owns the phone's own rpc::Client, which is its own engine
// Session, and remembers what the phone asked for so the socket layer (Task
// 10) can act on it. The engine's vrx_ids() lists every session's receivers,
// so independence between phones is by ownership: this object tracks the ids
// it created itself and its destructor removes those and no others.
//
// THREADS. Not synchronised, and does not need to be: every access is on one
// strand, which in the gateway is the kj event loop thread.
//
//   Commands are dispatched there (Gateway::receive -> PhoneStreams::on_text).
//   PhoneStreams reads and changes the session there.
//   The rpc::Client's callbacks arrive on the client's own event loop thread
//   and never touch a session: PhoneStreams' callbacks copy into a locked
//   inbox and post a wake-up to the strand, and the work happens when the
//   strand runs it. phone_streams.h has the whole model, including what the
//   socket, the HLS registry and the segmenters may be touched from (the
//   strand, and nothing else).
//
// The client's blocking calls are made from the strand, so a slow engine
// call holds up every phone's sends for its duration; on loopback that is
// accepted.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
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

    // The two engine calls dispatch makes after it has already changed engine
    // state, routed here so a test can make them fail. Production never sets
    // the hooks.
    struct TestHooks {
        std::function<Status(std::uint64_t)> remove_vrx;
        std::function<Expected<rpc::VrxStatus>(std::uint64_t)> vrx_status;
    };
    [[nodiscard]] TestHooks& test_hooks() { return hooks_; }
    [[nodiscard]] Status remove_receiver(std::uint64_t vrx);
    [[nodiscard]] Expected<rpc::VrxStatus> receiver_status(std::uint64_t vrx);

    // ---- Receivers this phone created ----
    [[nodiscard]] bool owns(std::uint64_t vrx) const { return owned_.contains(vrx); }
    [[nodiscard]] std::vector<std::uint64_t> owned_ids() const;

    // The params last sent for a receiver, kept so a receiver the engine
    // dropped can be recreated. Null when the id is not owned.
    [[nodiscard]] const rpc::VrxParams* last_params(std::uint64_t vrx) const;

    void adopt(std::uint64_t vrx, const rpc::VrxParams& params);
    void remember(std::uint64_t vrx, const rpc::VrxParams& params);
    void forget(std::uint64_t vrx);

    // The phone's per-receiver transcription choice, Auto unless it set one.
    // Kept beside the params so a recreated receiver gets it back.
    [[nodiscard]] rpc::TranscribeChoice transcribe_choice(std::uint64_t vrx) const;
    void set_transcribe_choice(std::uint64_t vrx, rpc::TranscribeChoice choice);

    // The decoders the phone wants on a receiver. Until it picks, a digital
    // voice receiver gets its mode's own decoder, which is what carries its
    // talkgroup and encryption flags, and an analog one gets none, because a
    // decoder costs engine CPU on every chunk and most analog listening has
    // nothing to decode. The choice is kept beside the params so a receiver
    // recreated after an engine restart decodes what it did before.
    [[nodiscard]] std::vector<std::string> decoders(std::uint64_t vrx) const;
    // Null while the phone has made no choice for the receiver.
    [[nodiscard]] const std::vector<std::string>* decoder_choice(std::uint64_t vrx) const;
    void set_decoders(std::uint64_t vrx, std::vector<std::string> names);

    // ---- Requested stream state ----
    // Recorded by dispatch. Task 10 wires the actual spectrum and audio
    // streams to these; until then they are only state.
    [[nodiscard]] const std::optional<View>& view() const { return view_; }
    void set_view(const View& view) { view_ = view; }

    [[nodiscard]] std::optional<std::uint64_t> audio_vrx() const { return audio_vrx_; }
    void set_audio_vrx(std::optional<std::uint64_t> vrx) { audio_vrx_ = vrx; }

    [[nodiscard]] bool keep_listening() const { return keep_listening_; }
    void set_keep_listening(bool on) { keep_listening_ = on; }

    // The confidence bar for the detections event. Validated by dispatch
    // (the engine refuses 1 or more), so it is always safe to hand over.
    [[nodiscard]] double min_confidence() const { return min_confidence_; }
    void set_min_confidence(double value) { min_confidence_ = value; }

private:
    std::unique_ptr<rpc::Client> client_;
    std::map<std::uint64_t, rpc::VrxParams> owned_;
    std::map<std::uint64_t, rpc::TranscribeChoice> transcribe_;
    std::map<std::uint64_t, std::vector<std::string>> decoders_;
    std::optional<View> view_;
    std::optional<std::uint64_t> audio_vrx_;
    bool keep_listening_ = false;
    double min_confidence_ = 0.0;
    TestHooks hooks_;
};

}  // namespace revenant::webgate
