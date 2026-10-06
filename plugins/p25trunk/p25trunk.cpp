// P25 Phase 1 trunk tracker, an engine plugin.
//
// It watches a P25 control channel somebody has already opened a receiver on,
// reads the group voice grants the control channel sends, and opens a p25p1
// receiver on each granted voice channel for as long as the call lasts. The
// engine's p25p1 receiver plays the IMBE voice itself, so following a call is
// opening a receiver at the right frequency and removing it at the right time.
//
// THIS FILE IS ALSO THE TEMPLATE
//
// docs/plugins.md points people who want to write their own plugin here, so
// the structure is laid out to be copied: the five exports at the bottom, one
// state struct, one dispatch function per event type, a try/catch at the only
// place the host calls in. Comments marked TEMPLATE are about the ABI and
// apply to any plugin; the rest are about trunking.
//
// HOW IT FINDS THE CONTROL CHANNEL
//
// It does not tune one. The operator opens a p25p1 receiver on the control
// channel, from the UI or any RPC client, and the plugin subscribes to the
// p25p1 decoder of every p25p1 receiver it did not open itself. The first one
// to deliver a Trunking Signaling Block with a good CRC becomes the control
// channel, and the others are unsubscribed. Removing that receiver, or
// changing its mode, removes every voice receiver the plugin opened, and the
// plugin goes back to listening to every p25p1 receiver still open.
//
// A p25p1 receiver that never sends a TSBK, one on a conventional channel
// say, stays subscribed while the plugin is looking, which keeps its decoder
// running. That costs one decoder's time on a decode lane and nothing else.
//
// A grant only resolves to a frequency once the control channel has sent the
// identifier update for the grant's channel identifier (TIA-102.AABC-B clause
// 2.3.9.2). The p25p1 decoder remembers those and adds channel_frequency_hz to
// the grant; a grant without it is skipped, and the next one, usually within
// a second or two, resolves.
//
// WHEN A CALL ENDS
//
// Three things end a call, whichever comes first:
//   - The voice channel sends a terminator, kind "tdu" or "tdulc". The
//     receiver stays kTduHoldSeconds in case the next transmission on the
//     same talkgroup follows at once, which is common, and goes after that
//     unless an explicit grant arrives first. Grant updates alone do not hold
//     it open, because a control channel keeps sending updates for a short
//     while after the call has ended.
//   - Nothing is heard about it for hang_seconds: no grant, no update, no
//     voice frame.
//   - Its receiver is removed by something else, a front-end retune that no
//     longer covers it for instance.
//   - Its channel goes to a talkgroup this plugin would not follow, by a
//     grant on the control channel or by the talkgroup the voice itself
//     carries.
//
// The second depends on the control channel to keep time, see TIME. If the
// control channel stops decoding, a fade or the operator moving its receiver
// somewhere else, no time passes and open calls stay open until it decodes
// again or its receiver is removed. A plugin thread with a timer would close
// that gap at the cost of locking every piece of state; this plugin does not.
//
// TIME
//
// TEMPLATE: a plugin has no timer. It is called when an event arrives and at
// no other time. A control channel sends a TSDU every few tens of
// milliseconds, so this plugin uses the control channel's own sample count as
// its clock: end_sample / sample_rate of each control channel message, in
// seconds, taken only as forward steps so a decoder that restarts its count
// cannot run the clock backwards. Voice receivers have their own sample
// counts that start at their own zero, so voice events are stamped with the
// control channel's latest time rather than their own.
//
// CONFIGURATION
//
// An optional text file beside the DLL with the same name and .ini in place
// of .dll, p25trunk.ini, read once at create. One key=value per line, # for a
// comment, unknown keys logged and ignored:
//   talkgroups=100,200,3501   follow only these; absent or empty follows all
//   follow_encrypted=0        1 to open receivers on encrypted calls too; the
//                             receiver stays silent for them either way
//   max_calls=6               voice receivers open at once
//   hang_seconds=2.5          see WHEN A CALL ENDS
//   control_frequency_hz=     a control channel for the plugin to open
//                             itself whenever a source is open and it is not,
//                             for an engine run with no client; it is a
//                             p25p1 receiver like any other and is found the
//                             same way
//
// WHAT IT DOES NOT DO
//
// It does not decrypt, it does not follow Phase 2 TDMA voice grants, it does
// not retune the front end to reach a voice channel outside the span, and it
// does not follow a control channel to an adjacent site. A grant outside the
// span is refused by the engine; the refusal is logged once and the frequency
// is not retried for kRefusalBackoffSeconds.
//
// THE EXPLICIT GRANT UPDATE
//
// Opcode $03 carries two channels. The p25p1 decoder publishes the first as
// channel and the second as channel_receive (core/rpc/decoders.h), and this
// follows channel. That reading is from the decoder's field names alone and
// has not been checked against a live system that sends $03.

#define RV_ENGINE_PLUGIN_BUILDING_PLUGIN
#include "core/plugin/engine_plugin_abi.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr const char* kPluginName = "p25trunk";
constexpr const char* kPluginVersion = "1.0";

// The p25p1 decoder's registry name, which is also the mode a voice receiver
// is opened in.
constexpr const char* kP25 = "p25p1";

// AABC-B Table 4.2-1.
constexpr std::int64_t kOpGroupVoiceGrant = 0x00;
constexpr std::int64_t kOpGroupVoiceGrantUpdate = 0x02;
constexpr std::int64_t kOpGroupVoiceGrantUpdateExplicit = 0x03;

// ENGINEERING CHOICES, not citations.
constexpr double kTduHoldSeconds = 0.5;
constexpr double kRefusalBackoffSeconds = 5.0;

struct Settings {
    std::set<std::int64_t> talkgroups;  // empty follows all
    bool follow_encrypted = false;
    std::size_t max_calls = 6;
    double hang_seconds = 2.5;
    std::optional<std::int64_t> control_frequency_hz;
};

// One followed call, keyed by its voice frequency. A voice channel carries one
// call at a time, so the frequency is the identity and the talkgroup is what
// is on it now.
struct Call {
    std::int64_t group = 0;
    std::int64_t frequency_hz = 0;
    std::uint32_t vrx = 0;          // 0 while add_vrx is outstanding
    std::uint32_t pending_tag = 0;  // that add's tag, so a stale result is not taken for it
    double last_heard = 0.0;
    bool terminated = false;  // a TDU was heard
    double terminated_at = 0.0;
};

// One decoded message's fields, looked up by key. TEMPLATE: fields arrive as
// an array of key, type and value, valid only during on_event, so read what
// you need while you are there and copy nothing you do not.
class Fields {
public:
    explicit Fields(const rv_engine_event& ev) : ev_(ev) {}

    [[nodiscard]] std::optional<std::int64_t> integer(std::string_view key) const {
        const rv_engine_field* f = find(key);
        if (f == nullptr || f->type != RV_ENGINE_FIELD_INT) {
            return std::nullopt;
        }
        return f->int_value;
    }

    [[nodiscard]] bool flag(std::string_view key) const {
        const rv_engine_field* f = find(key);
        return f != nullptr && f->type == RV_ENGINE_FIELD_BOOL && f->bool_value != 0;
    }

private:
    [[nodiscard]] const rv_engine_field* find(std::string_view key) const {
        for (std::uint32_t i = 0; i < ev_.field_count; ++i) {
            if (ev_.fields[i].key != nullptr && key == ev_.fields[i].key) {
                return &ev_.fields[i];
            }
        }
        return nullptr;
    }

    const rv_engine_event& ev_;
};

}  // namespace

// TEMPLATE: the opaque handle the ABI declares. The host only ever holds a
// pointer to it, so it may be any C++ type, and it is allocated and freed on
// this side of the line.
struct rv_engine_plugin {
    const rv_engine_host* host = nullptr;
    Settings settings;

    // Tags are this plugin's own; the host hands each back unread in the
    // COMMAND_RESULT for the request. Zero is never used.
    std::uint32_t next_tag = 1;
    std::map<std::uint32_t, std::int64_t> pending_adds;  // tag -> frequency

    // Every p25p1 receiver somebody else opened, so a lost control channel
    // can be looked for again among them, and the ones subscribed to while
    // looking.
    std::set<std::uint32_t> p25_receivers;
    std::set<std::uint32_t> candidates;
    std::optional<std::uint32_t> control;

    // The receiver opened for control_frequency_hz, or the tag of the add
    // that is opening it.
    std::optional<std::uint32_t> own_control;
    std::optional<std::uint32_t> own_control_tag;

    // The control channel's clock, seconds. Only ever moves forward: see
    // advance_clock.
    double now = 0.0;
    std::optional<double> last_stamp;

    std::map<std::int64_t, Call> calls;               // frequency -> call
    std::map<std::uint32_t, std::int64_t> voice_vrx;  // our receiver -> frequency
    std::map<std::int64_t, double> refused;           // frequency -> when
    std::set<std::int64_t> encrypted_groups;          // last grant said encrypted
    bool full_logged = false;
};

namespace {

// ---------------------------------------------------------------------------
// Talking to the host
// ---------------------------------------------------------------------------

// TEMPLATE: the host copies the string before log returns, so a temporary is
// fine.
void log(const rv_engine_plugin& self, std::uint32_t level, const std::string& line)
{
    self.host->log(self.host->ctx, level, line.c_str());
}

std::uint32_t take_tag(rv_engine_plugin& self)
{
    const std::uint32_t tag = self.next_tag++;
    if (self.next_tag == 0) {
        self.next_tag = 1;
    }
    return tag;
}

// TEMPLATE: a host function returning anything but OK refused the request
// before queueing it, and no COMMAND_RESULT will follow. Anything that waits
// on a result has to forget the request here.
[[nodiscard]] bool queued(rv_engine_plugin& self, std::int32_t code, const char* what)
{
    if (code == RV_ENGINE_PLUGIN_OK) {
        return true;
    }
    log(self, RV_ENGINE_LOG_WARN, std::string(what) + " was not queued, code " +
                                      std::to_string(code));
    return false;
}

void subscribe(rv_engine_plugin& self, std::uint32_t vrx)
{
    static_cast<void>(queued(
        self, self.host->subscribe_decoded(self.host->ctx, vrx, kP25, take_tag(self)),
        "subscribe"));
}

void unsubscribe(rv_engine_plugin& self, std::uint32_t vrx)
{
    static_cast<void>(queued(
        self, self.host->unsubscribe_decoded(self.host->ctx, vrx, kP25, take_tag(self)),
        "unsubscribe"));
}

std::string mhz(std::int64_t hz)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.5f MHz", static_cast<double>(hz) / 1e6);
    return text;
}

// ---------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------

void release(rv_engine_plugin& self, std::map<std::int64_t, Call>::iterator it,
             const char* why)
{
    const Call& call = it->second;
    log(self, RV_ENGINE_LOG_INFO, "release TG " + std::to_string(call.group) + " on " +
                                      mhz(call.frequency_hz) + ": " + why);
    if (call.vrx != 0) {
        static_cast<void>(queued(
            self, self.host->remove_vrx(self.host->ctx, call.vrx, take_tag(self)), "remove"));
        self.voice_vrx.erase(call.vrx);
    }
    // A call still waiting on add_vrx has no receiver yet. Its entry in
    // pending_adds stays, and on_command_result removes the receiver when it
    // arrives because the call is no longer in calls.
    self.calls.erase(it);
}

void release_all(rv_engine_plugin& self, const char* why)
{
    while (!self.calls.empty()) {
        release(self, self.calls.begin(), why);
    }
}

void expire(rv_engine_plugin& self)
{
    for (auto it = self.calls.begin(); it != self.calls.end();) {
        const Call& call = it->second;
        auto next = std::next(it);
        if (call.terminated && self.now - call.terminated_at >= kTduHoldSeconds) {
            release(self, it, "terminator");
        } else if (self.now - call.last_heard >= self.settings.hang_seconds) {
            release(self, it, "nothing heard");
        }
        it = next;
    }
    if (self.calls.size() < self.settings.max_calls) {
        self.full_logged = false;
    }
}

// A decoder's sample count is its own and starts at its own zero, so a new
// control channel, or the same one after its decoder restarts, can stamp a
// time behind the last. Only forward steps are taken, which keeps every time
// stored against `now` meaningful whatever the stamps do.
void advance_clock(rv_engine_plugin& self, const rv_engine_event& ev)
{
    if (ev.sample_rate == 0) {
        return;
    }
    const double stamp =
        static_cast<double>(ev.end_sample) / static_cast<double>(ev.sample_rate);
    if (self.last_stamp && stamp > *self.last_stamp) {
        self.now += stamp - *self.last_stamp;
    }
    self.last_stamp = stamp;
}

bool wanted(const rv_engine_plugin& self, std::int64_t group, bool encrypted)
{
    if (encrypted && !self.settings.follow_encrypted) {
        return false;
    }
    return self.settings.talkgroups.empty() || self.settings.talkgroups.contains(group);
}

// One talkgroup granted one frequency. `explicit_grant` is opcode $00, the
// grant itself, as against an update repeating a call already in progress.
void on_grant(rv_engine_plugin& self, std::int64_t group, std::int64_t frequency_hz,
              bool explicit_grant, bool encrypted)
{
    if (!wanted(self, group, encrypted)) {
        // A channel this plugin holds, handed to a talkgroup it would not
        // follow. Without this the receiver plays the new call, and that
        // call's voice keeps it open.
        if (auto it = self.calls.find(frequency_hz);
            it != self.calls.end() && it->second.group != group) {
            release(self, it, "channel reassigned");
        }
        return;
    }

    if (auto it = self.calls.find(frequency_hz); it != self.calls.end()) {
        Call& call = it->second;
        if (call.group != group) {
            // The channel was reassigned before this plugin noticed the last
            // call end. Same receiver, new talkgroup.
            log(self, RV_ENGINE_LOG_INFO, "TG " + std::to_string(group) + " takes " +
                                              mhz(frequency_hz) + " from TG " +
                                              std::to_string(call.group));
            call.group = group;
            call.terminated = false;
        } else if (call.terminated && !explicit_grant) {
            return;  // updates do not hold a terminated call open
        }
        call.terminated = false;
        call.last_heard = self.now;
        return;
    }

    if (auto refused = self.refused.find(frequency_hz); refused != self.refused.end()) {
        if (self.now - refused->second < kRefusalBackoffSeconds) {
            return;
        }
        self.refused.erase(refused);
    }

    if (self.calls.size() >= self.settings.max_calls) {
        if (!self.full_logged) {
            log(self, RV_ENGINE_LOG_INFO, "max_calls reached, TG " + std::to_string(group) +
                                              " on " + mhz(frequency_hz) + " not followed");
            self.full_logged = true;
        }
        return;
    }

    const std::uint32_t tag = take_tag(self);
    if (!queued(self, self.host->add_vrx(self.host->ctx, frequency_hz, 0, kP25, tag), "add")) {
        return;
    }
    self.pending_adds[tag] = frequency_hz;
    self.calls[frequency_hz] = Call{group, frequency_hz, 0, tag, self.now, false, 0.0};
    log(self, RV_ENGINE_LOG_INFO,
        "follow TG " + std::to_string(group) + " on " + mhz(frequency_hz));
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void on_tsbk(rv_engine_plugin& self, const rv_engine_event& ev, const Fields& f)
{
    if (!f.flag("crc_ok") || f.flag("protected")) {
        return;
    }
    if (f.integer("manufacturer_id").value_or(-1) != 0) {
        return;
    }

    if (!self.control) {
        self.control = ev.vrx;
        log(self, RV_ENGINE_LOG_INFO, "control channel on receiver " + std::to_string(ev.vrx));
        for (std::uint32_t other : self.candidates) {
            if (other != ev.vrx) {
                unsubscribe(self, other);
            }
        }
        self.candidates.clear();
    }
    if (*self.control != ev.vrx) {
        return;
    }

    advance_clock(self, ev);

    const std::int64_t opcode = f.integer("opcode").value_or(-1);
    const bool encrypted = f.flag("encrypted_call");
    if (opcode == kOpGroupVoiceGrant || opcode == kOpGroupVoiceGrantUpdateExplicit) {
        const auto group = f.integer("group");
        const auto hz = f.integer("channel_frequency_hz");
        if (group && !self.settings.follow_encrypted) {
            if (encrypted) {
                self.encrypted_groups.insert(*group);
            } else {
                self.encrypted_groups.erase(*group);
            }
        }
        if (group && hz) {
            on_grant(self, *group, *hz, opcode == kOpGroupVoiceGrant, encrypted);
        }
    } else if (opcode == kOpGroupVoiceGrantUpdate) {
        // Two talkgroups per block, and no service options, so an update
        // cannot say whether its call is encrypted. A talkgroup whose grant
        // said so is remembered and skipped; one this plugin joins mid-call
        // is opened, and released by on_voice if its voice says encrypted.
        const std::pair<const char*, const char*> pairs[] = {
            {"group", "channel_frequency_hz"}, {"group_2", "channel_2_frequency_hz"}};
        for (const auto& [group_key, hz_key] : pairs) {
            const auto group = f.integer(group_key);
            const auto hz = f.integer(hz_key);
            if (group && hz && !self.encrypted_groups.contains(*group)) {
                on_grant(self, *group, *hz, false, false);
            }
        }
    }

    expire(self);
}

void on_voice(rv_engine_plugin& self, const rv_engine_event& ev, const Fields& f)
{
    auto vrx = self.voice_vrx.find(ev.vrx);
    if (vrx == self.voice_vrx.end()) {
        return;
    }
    auto it = self.calls.find(vrx->second);
    if (it == self.calls.end()) {
        return;
    }
    Call& call = it->second;
    const std::string_view kind = ev.kind;
    if (kind == "tdu" || kind == "tdulc") {
        if (!call.terminated) {
            call.terminated = true;
            call.terminated_at = self.now;
        }
    } else if (kind == "hdu" || kind == "ldu1" || kind == "ldu2") {
        // The voice says who is talking, and it can disagree with the grant
        // this plugin last saw for the channel.
        if (const auto group = f.integer("talkgroup"); group && *group != call.group) {
            if (!wanted(self, *group, false)) {
                release(self, it, "voice is another talkgroup");
                return;
            }
            call.group = *group;
        }
        // Encrypted voice is the header's or LDU2's flag, from the ALGID.
        // LDU1's flag of the same name is the Link Control's, which can be
        // encrypted over clear voice, so it is not read here.
        if (kind != "ldu1" && f.flag("encrypted") && !self.settings.follow_encrypted) {
            self.encrypted_groups.insert(call.group);
            release(self, it, "encrypted");
            return;
        }
        call.last_heard = self.now;
        call.terminated = false;
    }
}

void on_decoded(rv_engine_plugin& self, const rv_engine_event& ev)
{
    const Fields fields(ev);
    if (std::string_view(ev.kind) == "tsbk") {
        on_tsbk(self, ev, fields);
    } else {
        on_voice(self, ev, fields);
    }
}

void look_for_control(rv_engine_plugin& self)
{
    for (std::uint32_t vrx : self.p25_receivers) {
        if (self.candidates.insert(vrx).second) {
            subscribe(self, vrx);
        }
    }
}

void open_own_control(rv_engine_plugin& self)
{
    const auto hz = self.settings.control_frequency_hz;
    if (!hz || self.own_control || self.own_control_tag) {
        return;
    }
    const std::uint32_t tag = take_tag(self);
    if (queued(self, self.host->add_vrx(self.host->ctx, *hz, 0, kP25, tag), "control add")) {
        self.own_control_tag = tag;
    }
}

// The control channel is gone: release every call, and look for a control
// channel again among the p25p1 receivers still open. The caller has already
// dropped the lost receiver from p25_receivers if it is no longer one.
void lose_control(rv_engine_plugin& self, const char* why)
{
    if (!self.control) {
        return;
    }
    log(self, RV_ENGINE_LOG_INFO, std::string("control channel lost: ") + why);
    self.control.reset();
    self.last_stamp.reset();
    release_all(self, "control channel lost");
    look_for_control(self);
}

void on_vrx_added(rv_engine_plugin& self, const rv_engine_event& ev)
{
    // TEMPLATE: owner is relative to the plugin hearing the event. Our own
    // voice receivers arrive here as THIS_PLUGIN and are handled through
    // their COMMAND_RESULT instead.
    if (ev.owner == RV_ENGINE_OWNER_THIS_PLUGIN || std::string_view(ev.demod) != kP25) {
        return;
    }
    self.p25_receivers.insert(ev.vrx);
    if (!self.control) {
        look_for_control(self);
    }
}

void on_vrx_changed(rv_engine_plugin& self, const rv_engine_event& ev)
{
    if (ev.owner == RV_ENGINE_OWNER_THIS_PLUGIN) {
        return;
    }
    if (std::string_view(ev.demod) == kP25) {
        // A move keeps the subscription. A control channel receiver moved
        // somewhere with no control channel stops sending TSBKs, and see
        // WHEN A CALL ENDS for what that does.
        on_vrx_added(self, ev);
        return;
    }
    self.p25_receivers.erase(ev.vrx);
    if (self.candidates.erase(ev.vrx) != 0) {
        unsubscribe(self, ev.vrx);
    }
    if (self.control && *self.control == ev.vrx) {
        unsubscribe(self, ev.vrx);
        lose_control(self, "its receiver left p25p1");
    }
}

void on_vrx_removed(rv_engine_plugin& self, const rv_engine_event& ev)
{
    self.p25_receivers.erase(ev.vrx);
    self.candidates.erase(ev.vrx);
    if (self.own_control && *self.own_control == ev.vrx) {
        // Reopened at the next source event, when the span may fit it.
        self.own_control.reset();
    }
    if (self.control && *self.control == ev.vrx) {
        lose_control(self, ev.text);
        return;
    }
    if (auto vrx = self.voice_vrx.find(ev.vrx); vrx != self.voice_vrx.end()) {
        const std::int64_t hz = vrx->second;
        self.voice_vrx.erase(vrx);
        if (auto it = self.calls.find(hz); it != self.calls.end()) {
            it->second.vrx = 0;  // already gone; release must not remove it again
            release(self, it, ev.text);
        }
    }
}

void on_command_result(rv_engine_plugin& self, const rv_engine_event& ev)
{
    if (self.own_control_tag && *self.own_control_tag == ev.request_tag) {
        self.own_control_tag.reset();
        if (ev.result_code != RV_ENGINE_PLUGIN_OK) {
            log(self, RV_ENGINE_LOG_WARN,
                "cannot open the control channel at " +
                    mhz(*self.settings.control_frequency_hz) + ": " + ev.text);
            return;
        }
        self.own_control = ev.vrx;
        self.p25_receivers.insert(ev.vrx);
        if (!self.control) {
            look_for_control(self);
        }
        return;
    }

    auto pending = self.pending_adds.find(ev.request_tag);
    if (pending == self.pending_adds.end()) {
        // Subscriptions and removals. A failed one is worth a line and
        // nothing more: the receiver it named has gone, or is going.
        if (ev.result_code != RV_ENGINE_PLUGIN_OK) {
            log(self, RV_ENGINE_LOG_DEBUG, std::string("request refused: ") + ev.text);
        }
        return;
    }
    const std::int64_t hz = pending->second;
    self.pending_adds.erase(pending);
    auto it = self.calls.find(hz);
    // A call released while its add was in flight may have been replaced by
    // another on the same frequency with an add of its own, and this result
    // is not that one's.
    if (it != self.calls.end() && it->second.pending_tag != ev.request_tag) {
        it = self.calls.end();
    }

    if (ev.result_code != RV_ENGINE_PLUGIN_OK) {
        log(self, RV_ENGINE_LOG_WARN, "cannot open " + mhz(hz) + ": " + ev.text);
        self.refused[hz] = self.now;
        if (it != self.calls.end()) {
            self.calls.erase(it);
        }
        return;
    }

    if (it == self.calls.end()) {
        // Released while the add was in flight.
        static_cast<void>(queued(
            self, self.host->remove_vrx(self.host->ctx, ev.vrx, take_tag(self)), "remove"));
        return;
    }
    it->second.vrx = ev.vrx;
    it->second.pending_tag = 0;
    self.voice_vrx[ev.vrx] = hz;
    subscribe(self, ev.vrx);
}

void dispatch(rv_engine_plugin& self, const rv_engine_event& ev)
{
    if (ev.events_dropped_before != 0) {
        // TEMPLATE: the queue overflowed and these are gone for good. This
        // plugin's state heals itself from the next grants and the hang
        // timer, so a line in the log is enough.
        log(self, RV_ENGINE_LOG_WARN,
            std::to_string(ev.events_dropped_before) + " events dropped");
    }

    switch (ev.type) {
    case RV_ENGINE_EVENT_VRX_ADDED: on_vrx_added(self, ev); break;
    case RV_ENGINE_EVENT_VRX_CHANGED: on_vrx_changed(self, ev); break;
    case RV_ENGINE_EVENT_VRX_REMOVED: on_vrx_removed(self, ev); break;
    case RV_ENGINE_EVENT_DECODED: on_decoded(self, ev); break;
    case RV_ENGINE_EVENT_COMMAND_RESULT: on_command_result(self, ev); break;
    case RV_ENGINE_EVENT_DECODED_ENDED:
        // The subscription is gone and is not renewed: the decoder stopped
        // for a reason, and a receiver that becomes p25p1 again arrives as
        // VRX_CHANGED.
        self.p25_receivers.erase(ev.vrx);
        self.candidates.erase(ev.vrx);
        if (self.control && *self.control == ev.vrx) {
            lose_control(self, ev.text);
        }
        break;
    case RV_ENGINE_EVENT_SOURCE_OPENED: open_own_control(self); break;
    case RV_ENGINE_EVENT_SOURCE_RETUNED:
        // A different span: frequencies refused before may fit now, the
        // configured control channel among them.
        self.refused.clear();
        open_own_control(self);
        break;
    case RV_ENGINE_EVENT_SOURCE_CLOSED:
        self.own_control.reset();
        lose_control(self, "the source closed");
        break;
    default: break;  // TEMPLATE: ignore types you do not know; later versions may add some
    }
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

// TEMPLATE: the DLL's own path, found from the address of something inside
// it. A plugin has no other way to learn where it was loaded from.
std::string settings_path()
{
#ifdef _WIN32
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&settings_path), &module) == 0) {
        return {};
    }
    wchar_t wide[MAX_PATH];
    const DWORD length = GetModuleFileNameW(module, wide, MAX_PATH);
    if (length == 0 || length == MAX_PATH) {
        return {};
    }
    std::wstring path(wide, length);
    if (const auto dot = path.find_last_of(L'.'); dot != std::wstring::npos) {
        path.resize(dot);
    }
    path += L".ini";
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr,
                                          nullptr);
    std::string utf8(static_cast<std::size_t>(bytes > 0 ? bytes - 1 : 0), '\0');
    if (bytes > 1) {
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8.data(), bytes, nullptr, nullptr);
    }
    return utf8;
#else
    return {};
#endif
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

template <typename T>
bool parse_number(std::string_view text, T& out)
{
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc() && end == text.data() + text.size();
}

void load_settings(rv_engine_plugin& self, const std::string& path)
{
    std::ifstream file(path);
    if (!file) {
        log(self, RV_ENGINE_LOG_INFO, "no settings file, defaults in use");
        return;
    }
    std::string raw;
    int line_number = 0;
    while (std::getline(file, raw)) {
        ++line_number;
        std::string_view line = trim(raw);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto eq = line.find('=');
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = eq == std::string_view::npos ? "" : trim(line.substr(eq + 1));
        Settings& s = self.settings;
        bool ok = true;
        if (eq == std::string_view::npos) {
            ok = false;
        } else if (key == "talkgroups") {
            std::string_view rest = value;
            while (ok && !rest.empty()) {
                const auto comma = rest.find(',');
                std::int64_t group = 0;
                ok = parse_number(trim(rest.substr(0, comma)), group);
                s.talkgroups.insert(group);
                rest = comma == std::string_view::npos ? "" : rest.substr(comma + 1);
            }
        } else if (key == "follow_encrypted") {
            ok = value == "0" || value == "1";
            s.follow_encrypted = value == "1";
        } else if (key == "max_calls") {
            ok = parse_number(value, s.max_calls) && s.max_calls > 0;
        } else if (key == "control_frequency_hz") {
            std::int64_t hz = 0;
            ok = value.empty() || (parse_number(value, hz) && hz > 0);
            if (ok && !value.empty()) {
                s.control_frequency_hz = hz;
            }
        } else if (key == "hang_seconds") {
            ok = parse_number(value, s.hang_seconds) && s.hang_seconds > 0.0;
        } else {
            log(self, RV_ENGINE_LOG_WARN,
                "settings line " + std::to_string(line_number) + ": unknown key ignored");
            continue;
        }
        if (!ok) {
            log(self, RV_ENGINE_LOG_WARN,
                "settings line " + std::to_string(line_number) + " not understood");
        }
    }
    log(self, RV_ENGINE_LOG_INFO,
        "settings: " +
            (self.settings.talkgroups.empty()
                 ? std::string("all talkgroups")
                 : std::to_string(self.settings.talkgroups.size()) + " talkgroups") +
            ", encrypted " + (self.settings.follow_encrypted ? "followed" : "skipped") +
            ", max_calls " + std::to_string(self.settings.max_calls));
}

}  // namespace

// ---------------------------------------------------------------------------
// The five exports
// ---------------------------------------------------------------------------
//
// TEMPLATE: these are the whole surface the host sees. Every one is extern
// "C" and __cdecl through the ABI's macros, and none lets an exception out.

extern "C" {

RV_ENGINE_PLUGIN_EXPORT std::uint32_t RV_ENGINE_PLUGIN_CALL revenant_engine_plugin_abi_version(void)
{
    return RV_ENGINE_PLUGIN_ABI_VERSION;
}

RV_ENGINE_PLUGIN_EXPORT std::int32_t RV_ENGINE_PLUGIN_CALL
revenant_engine_plugin_describe(rv_engine_plugin_desc* out_desc)
{
    if (out_desc == nullptr || out_desc->struct_size != sizeof(rv_engine_plugin_desc)) {
        return RV_ENGINE_PLUGIN_ERR_ARGUMENT;
    }
    // Source events are wanted for SOURCE_CLOSED and SOURCE_RETUNED.
    out_desc->interests =
        RV_ENGINE_INTEREST_SOURCE | RV_ENGINE_INTEREST_VRX | RV_ENGINE_INTEREST_DECODED;
    std::snprintf(out_desc->name, sizeof(out_desc->name), "%s", kPluginName);
    std::snprintf(out_desc->version, sizeof(out_desc->version), "%s", kPluginVersion);
    return RV_ENGINE_PLUGIN_OK;
}

RV_ENGINE_PLUGIN_EXPORT rv_engine_plugin* RV_ENGINE_PLUGIN_CALL
revenant_engine_plugin_create(const rv_engine_host* host)
{
    if (host == nullptr || host->struct_size != sizeof(rv_engine_host)) {
        return nullptr;
    }
    try {
        auto* self = new rv_engine_plugin;
        self->host = host;
        load_settings(*self, settings_path());
        return self;
    } catch (...) {
        return nullptr;  // reported by the host as loaded and declined
    }
}

RV_ENGINE_PLUGIN_EXPORT void RV_ENGINE_PLUGIN_CALL
revenant_engine_plugin_on_event(rv_engine_plugin* self, const rv_engine_event* ev)
{
    if (self == nullptr || ev == nullptr || ev->struct_size != sizeof(rv_engine_event)) {
        return;
    }
    try {
        dispatch(*self, *ev);
    } catch (...) {
        // TEMPLATE: an exception reaching the host ends the process. The
        // likeliest one here is bad_alloc, and losing one event to it is the
        // better outcome.
    }
}

RV_ENGINE_PLUGIN_EXPORT void RV_ENGINE_PLUGIN_CALL revenant_engine_plugin_destroy(rv_engine_plugin* self)
{
    // TEMPLATE: no need to remove receivers here. The host removes every
    // receiver a plugin opened when it stops the plugin, and the host table
    // must not be called after this returns anyway.
    delete self;
}

}  // extern "C"
