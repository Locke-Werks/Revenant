// One connected phone, as far as the engine is concerned.
//
// A PhoneSession owns the phone's own rpc::Client, which is its own engine
// Session, and remembers what the phone asked for so the socket layer can act
// on it.
//
// SINGLE USER. The engine serves one user at a time (docs/superpowers/specs/
// 2026-11-03-single-user-engine-design.md), so every receiver the engine has
// is this phone's to see and control, whoever opened it: another session, an
// engine plugin, the trunk follower. The session's receiver set is the
// engine's whole list as the receiver events last reported it, plus what the
// phone itself just added. Nothing is removed when a session ends; the engine
// decides receiver lifetime by the lease.
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
#include <set>
#include <string>
#include <vector>

#include "core/rpc/client.h"
#include "core/rpc/types.h"
#include "tools/webgate/spectrum_reducer.h"

namespace revenant::webgate {

class PhoneSession {
public:
    explicit PhoneSession(std::unique_ptr<rpc::Client> client);
    ~PhoneSession() = default;

    PhoneSession(const PhoneSession&) = delete;
    PhoneSession& operator=(const PhoneSession&) = delete;
    PhoneSession(PhoneSession&&) = delete;
    PhoneSession& operator=(PhoneSession&&) = delete;

    [[nodiscard]] rpc::Client& client() { return *client_; }
    // The same Client, shared, for a command run on a worker: the worker keeps
    // it alive if the session is destroyed while the command runs.
    [[nodiscard]] std::shared_ptr<rpc::Client> shared_client() const { return client_; }

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

    // ---- Every receiver the engine has ----
    // THE set of receivers the phone works on: streams, audio, HLS, decoders
    // and RDS all iterate this and nothing narrower. Restart recreation walks
    // it too and rebuilds the recreatable ones.
    [[nodiscard]] std::vector<std::uint64_t> receiver_ids() const;
    [[nodiscard]] bool has_receiver(std::uint64_t vrx) const { return receivers_.contains(vrx); }

    // The params last seen for a receiver, kept so one the engine dropped can
    // be recreated. Null when the session has not heard of the id.
    [[nodiscard]] const rpc::VrxParams* last_params(std::uint64_t vrx) const;

    // A receiver the engine reported, or one this session just added.
    // opened_here is whether this session opened it. Provenance only, never
    // a permission: it decides which receivers this session adds again after
    // an engine restart, since another session rebuilds its own and a plugin
    // reopens its own, and two copies of one receiver are no use to anyone.
    void learn(std::uint64_t vrx, const rpc::VrxParams& params, bool opened_here = true);
    // Whether the phone should add this receiver again after an engine restart.
    [[nodiscard]] bool recreatable(std::uint64_t vrx) const;
    void remember(std::uint64_t vrx, const rpc::VrxParams& params);
    void forget(std::uint64_t vrx);

    // The phone's per-receiver transcription choice, Auto unless it set one.
    // Kept beside the params so a recreated receiver gets it back.
    [[nodiscard]] rpc::TranscribeChoice transcribe_choice(std::uint64_t vrx) const;
    void set_transcribe_choice(std::uint64_t vrx, rpc::TranscribeChoice choice);

    // The decoders the phone wants on a receiver: its own choice once it made
    // one, else default_decoders. The choice is kept beside the params so a
    // receiver recreated after an engine restart decodes what it did before.
    [[nodiscard]] std::vector<std::string> decoders(std::uint64_t vrx) const;
    // THE default for a receiver of this mode: the engine's decoder named
    // after the mode when it has one (cw, p25p1, dstar, tetra, dmr, ...),
    // else none. A mode with no decoder of its own costs nothing undecoded.
    [[nodiscard]] std::vector<std::string> default_decoders(rpc::Demod demod) const;
    // False while the default cannot be told because the engine's decoder
    // list could not be read: an empty answer then means "unknown", and is
    // not to be applied as the receiver's set.
    [[nodiscard]] bool decoders_known(std::uint64_t vrx) const;
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

    // The phone's own sound switch, apart from which receiver leads: Auto DV
    // opens receivers the phone never selected, and those must be heard too.
    // audio_vrx() is only the lead of the mix this turns on.
    [[nodiscard]] bool audio_on() const { return audio_on_; }
    void set_audio_on(bool on) { audio_on_ = on; }

    [[nodiscard]] bool keep_listening() const { return keep_listening_; }
    void set_keep_listening(bool on) { keep_listening_ = on; }

    // The confidence bar for the detections event. Validated by dispatch
    // (the engine refuses 1 or more), so it is always safe to hand over.
    [[nodiscard]] double min_confidence() const { return min_confidence_; }
    void set_min_confidence(double value) { min_confidence_ = value; }

    // ---- The DV tab's switches (dv_feed.h) ----
    // Auto DV: on or off, the receivers it opened (so it hands back only its
    // own), and the frequencies the phone removed a receiver from while it
    // was on, which it leaves alone until it is turned on afresh.
    [[nodiscard]] bool auto_dv() const { return auto_dv_; }
    void set_auto_dv(bool on)
    {
        auto_dv_ = on;
        auto_dv_dismissed_.clear();
    }
    // Whether this session switched the engine's plugin receivers off for its
    // Auto DV. The switch is engine-wide, so only a hold this session set is
    // lifted by it, and the hold goes when the session does.
    [[nodiscard]] bool plugin_hold() const { return plugin_hold_; }
    void set_plugin_hold(bool held) { plugin_hold_ = held; }
    [[nodiscard]] std::set<std::uint64_t>& auto_dv_spawned() { return auto_dv_spawned_; }
    [[nodiscard]] const std::set<std::uint64_t>& auto_dv_spawned() const { return auto_dv_spawned_; }
    [[nodiscard]] std::vector<double>& auto_dv_dismissed() { return auto_dv_dismissed_; }
    [[nodiscard]] const std::vector<double>& auto_dv_dismissed() const { return auto_dv_dismissed_; }

    // Trunk following as the phone asked for it, kept so a session rebuilt
    // on a restarted engine asks again.
    struct TrunkFollow {
        std::uint64_t vrx = 0;
        std::uint32_t max_followers = 0;
    };
    [[nodiscard]] const std::optional<TrunkFollow>& trunk_follow() const { return trunk_follow_; }
    void set_trunk_follow(std::optional<TrunkFollow> follow) { trunk_follow_ = follow; }

    // The rules the phone set with setTrunkRules; without them the desktop's.
    [[nodiscard]] const std::optional<rpc::TrunkRuleSet>& trunk_rules() const { return trunk_rules_; }
    void set_trunk_rules(std::optional<rpc::TrunkRuleSet> rules) { trunk_rules_ = std::move(rules); }
    // The desktop's saved rules and follower count, refreshed by the streams.
    [[nodiscard]] const rpc::TrunkRuleSet& desktop_trunk_rules() const { return desktop_rules_; }
    void set_desktop_trunk_rules(rpc::TrunkRuleSet rules) { desktop_rules_ = std::move(rules); }
    [[nodiscard]] std::uint32_t desktop_max_followers() const { return desktop_max_followers_; }
    void set_desktop_max_followers(std::uint32_t n) { desktop_max_followers_ = n; }
    [[nodiscard]] const rpc::TrunkRuleSet& wanted_trunk_rules() const
    {
        return trunk_rules_ ? *trunk_rules_ : desktop_rules_;
    }

    // Whether the phone has the DV tab open, which is when trunk status is
    // polled without following on.
    [[nodiscard]] bool dv_watch() const { return dv_watch_; }
    void set_dv_watch(bool on) { dv_watch_ = on; }

private:
    bool auto_dv_ = false;
    bool plugin_hold_ = false;
    std::set<std::uint64_t> auto_dv_spawned_;
    std::vector<double> auto_dv_dismissed_;
    std::optional<TrunkFollow> trunk_follow_;
    std::optional<rpc::TrunkRuleSet> trunk_rules_;
    rpc::TrunkRuleSet desktop_rules_;
    std::uint32_t desktop_max_followers_ = 2;
    bool dv_watch_ = false;
    std::shared_ptr<rpc::Client> client_;
    struct Known {
        rpc::VrxParams params;
        bool opened_here = true;
    };
    std::map<std::uint64_t, Known> receivers_;
    std::map<std::uint64_t, rpc::TranscribeChoice> transcribe_;
    std::map<std::uint64_t, std::vector<std::string>> decoders_;
    // The engine's decoder names, read on first need.
    mutable std::optional<std::vector<std::string>> engine_decoders_;
    std::optional<View> view_;
    std::optional<std::uint64_t> audio_vrx_;
    bool keep_listening_ = false;
    bool audio_on_ = false;
    double min_confidence_ = 0.0;
    TestHooks hooks_;
};

}  // namespace revenant::webgate
