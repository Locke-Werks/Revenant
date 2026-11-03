// Every phone session the gateway holds, attached to a socket or not.
//
// A session outlives its WebSocket on purpose. iOS suspends a locked page and
// its socket goes with it, while the HLS <audio> element keeps playing; if
// the session died with the socket, its receiver and its HLS stream would
// die too and the phone would go silent. So a dropped socket detaches the
// session, and a new socket can take it back with the token the gateway sent
// in "hello".
//
// LIFETIME OF A DETACHED SESSION
//   no receivers and keep listening off:
//                       empty_linger after the drop. Every WebSocket opens an
//                       engine session, so a page that connected and left
//                       must not hold one any longer than that.
//   otherwise:          detached_linger after the drop.
//   keep listening on:  the same, and also until hls_idle after the last HLS
//                        request for one of its receivers, whichever is later.
// The base linger covers a socket dropped by a network blip or an app switch.
// The owner chose 30 s over the earlier 10 min on 2026-11-03: a phone that is
// put away without keep listening should not hold the radio's receivers for
// minutes. Keep listening is how a locked phone keeps its session.
// On expiry it is destroyed: its streams unsubscribe and its engine
// connection closes, which ends the phone's lease. The gateway removes no
// receivers; the engine removes the ones a client opened when the lease ends.
//
// At most max_residents sessions exist at once. A connection that would go
// past that evicts the detached session that dropped longest ago; when every
// session is attached the connection is refused (make_room says so).
//
// THE LEASE (docs/superpowers/specs/2026-11-03-single-user-engine-design.md).
// The engine serves one user. Each phone session logs in to the engine as
// "phone <ip> via webgate" (phone_holder); the engine joins every claim made
// under the holder's exact text to one lease, so a phone's resumes and its
// rebuilt sessions share its lease and a different phone does not.
//   held     the phone's claim was granted. "lease" {"state":"held","holder"}
//            is the first event on the connection, before "hello".
//   inUse    the claim was refused. The connection stays open with no
//            session: "lease" {"state":"inUse","holder","since","message"},
//            then every command but pong, resume and takeOver is answered
//            with the engine's refusal. "takeOver" logs in with takeOver,
//            evicting the holder, and the connection gets a session as if
//            it had just connected (hello, lease held, link), then the reply.
//   lost     someone else took the lease over. The session is destroyed and
//            the connection, if any, goes back to having none, with
//            "lease" {"state":"lost","holder":<the new holder>}. takeOver
//            from there takes it back.
// The lease is released when the session is destroyed (its linger ends): its
// engine connections close and the engine ends the lease.
//
// No kj here, so tests drive it with a fake socket, a manual strand and a
// fake clock. Strand thread only, like PhoneStreams.

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/rpc/client.h"
#include "tools/webgate/engine_link.h"
#include "tools/webgate/phone_streams.h"
#include "tools/webgate/protocol.h"
#include "tools/webgate/session.h"

namespace revenant::webgate {

// The text of a "link" event, shared so the one a phone gets on connect reads
// the same as later ones.
[[nodiscard]] std::string link_event(LinkState state, const std::string& detail);

struct HubTiming {
    std::chrono::milliseconds detached_linger{30'000};
    std::chrono::milliseconds empty_linger{30'000};
    std::size_t max_residents = 16;
    std::chrono::milliseconds hls_idle{60'000};

    // Liveness. kj does not expose WebSocket protocol pings, so the gateway
    // sends a "ping" event and the app answers with a "pong" command; any
    // message from the phone counts as hearing from it. A socket silent for
    // silence_limit is closed, which detaches its session: an iOS lock can
    // leave a TCP connection open with nobody on the other end.
    std::chrono::milliseconds ping_interval{kPingIntervalMs};
    std::chrono::milliseconds silence_limit{45'000};
};

// Where a session gets an engine connection: on connect, on take over and
// when the engine restarted. holder is the phone's lease holder text.
using NewSession = std::function<Expected<std::unique_ptr<rpc::Client>>(
    rpc::LoginMode mode, const std::string& holder)>;

// The holder text a phone's sessions log in with, from its address.
[[nodiscard]] std::string phone_holder(std::string_view peer);

// The engine's refusal of a claim, "in use: <holder> since <time>", taken
// apart. Null when message is not one.
struct LeaseRefusal {
    std::string holder;
    std::string since;
    std::string message;
};
[[nodiscard]] std::optional<LeaseRefusal> parse_lease_refusal(std::string_view message);

class PhoneHub {
public:
    // base.clock also times detached sessions. new_session may be empty, in
    // which case sessions are not recreated after an engine restart.
    PhoneHub(Strand& strand, StreamsConfig base, HubTiming timing = {},
             NewSession new_session = {});
    ~PhoneHub();
    PhoneHub(const PhoneHub&) = delete;
    PhoneHub& operator=(const PhoneHub&) = delete;

    // Room for one more session: true when there already is, or after
    // evicting the oldest detached session; false when every session is
    // attached to a socket, and the caller refuses the connection.
    [[nodiscard]] bool make_room();

    // A new connection with a fresh session on client, which logged in under
    // holder. Calls make_room first; the caller is expected to have checked
    // it. Sends "lease" held, "hello" with the session's resume token, then
    // the current link state. Returns the connection id the other calls take.
    std::uint64_t connect(PhoneSocket& socket, std::unique_ptr<rpc::Client> client,
                          std::string holder = {});

    // A new connection whose claim the engine refused with refusal (its
    // message). No session; sends "lease" inUse. See THE LEASE above.
    std::uint64_t connect_refused(PhoneSocket& socket, std::string holder,
                                  const std::string& refusal);

    // Logs in with claim under holder and connects, refused or not. Fails
    // only when the engine could not be reached at all.
    [[nodiscard]] Expected<std::uint64_t> admit(PhoneSocket& socket, const std::string& holder);

    // One text frame. "resume" is handled here: allowed only as a
    // connection's first command, it swaps the fresh session for the one
    // holding the token, taking it from another connection if one still has
    // it (that socket is closed). "takeOver" is handled here too. "pong"
    // only marks the phone as alive. Everything else goes to the session's
    // streams, or is refused while the connection has none.
    void text(std::uint64_t connection, std::string_view text);

    // The socket is gone. Its session detaches and starts expiring.
    void disconnect(std::uint64_t connection);

    // An HLS request was answered for this receiver.
    void hls_fetched(std::uint64_t hls_id);

    // Ticks every session's streams and destroys expired detached sessions.
    void tick();

    // Broadcasts the link state. On Up, and on later ticks while Up (see
    // recover_dead_sessions), every session whose engine connection died
    // (the engine restarted) is rebuilt on a new one: each
    // receiver it owned is added again from its last params at the same
    // absolute frequency, with its transcription choice, and its audio, HLS
    // and other subscriptions follow. That session's link event then carries
    //   "recreated": [{"old":id,"new":id}, ...]
    //   "lost":      [{"vrx":id,"reason":"<the engine's words>"}, ...]
    // and each lost id also gets a vrxRemoved with the same reason.
    void link_state(LinkState state, std::string detail);

    // Destroys every session. For shutdown, while the HLS registry they
    // borrow still exists.
    void clear();

    // For tests and the gateway.
    [[nodiscard]] std::size_t sessions() const { return residents_.size(); }
    [[nodiscard]] PhoneSession* session(std::uint64_t connection) const;
    [[nodiscard]] PhoneStreams* streams(std::uint64_t connection) const;
    [[nodiscard]] std::string token(std::uint64_t connection) const;
    [[nodiscard]] bool holds(const std::string& token) const { return residents_.contains(token); }
    // The cache every phone of this hub shares.
    [[nodiscard]] const FrameLevelsCache& frame_levels() const { return *base_.frame_levels; }

private:
    struct Resident {
        std::string token;
        // The lease holder text its engine connections log in with.
        std::string holder;
        // Declared before streams so the streams go first: they unsubscribe
        // through the session's client.
        std::unique_ptr<PhoneSession> session;
        std::unique_ptr<PhoneStreams> streams;
        std::optional<std::uint64_t> connection;
        std::chrono::steady_clock::time_point detached_at{};
        std::optional<std::chrono::steady_clock::time_point> last_hls;
        // When tick() last asked this session's client whether its engine
        // connection still works.
        std::chrono::steady_clock::time_point last_probe{};
    };
    struct Connection {
        PhoneSocket* socket = nullptr;
        Resident* resident = nullptr;
        bool commanded = false;
        std::chrono::steady_clock::time_point last_heard{};
        std::chrono::steady_clock::time_point last_ping{};
        // While the connection has no session (refused, or taken over): the
        // holder text a takeOver logs in with, the sentence every other
        // command is refused with, and JSON waiting for the socket, which
        // with no streams to send it has to be written here.
        std::string holder;
        std::string refusal;
        std::deque<std::string> outbox;
    };

    [[nodiscard]] std::chrono::steady_clock::time_point now() const;
    void resume(std::uint64_t id, Connection& connection, const Command& command);
    void take_over(std::uint64_t id, Connection& connection, const Command& command);
    // A session for client under holder, put on connection id: lease held,
    // hello, start, link event.
    Resident& seat(std::uint64_t id, std::unique_ptr<rpc::Client> client, std::string holder);
    // To the connection's session when it has one, else through its outbox.
    void send_bare(std::uint64_t id, std::string json);
    void flush(std::uint64_t id);
    // The engine's leaseLost for the session, posted to the strand.
    void watch_lease(Resident& resident);
    // The resident lost the lease to holder: destroyed, and its connection
    // told. Not inside a loop over residents_; see pending_losses_.
    void lose(const std::string& token, const std::string& holder);
    void settle_losses();
    [[nodiscard]] Resident* find_token(std::string_view token) const;
    // Rebuilds a resident on a fresh engine connection when its own one is
    // dead, and sends that session's link event with "recreated" and "lost"
    // in fields. False when nothing was rebuilt: the connection still works,
    // or no new connection could be had, in which case the session keeps
    // its receivers' params and tick() tries again.
    bool recreate(Resident& resident, JsonObject& fields);

    // Up and new_session set: each session's client is probed about once a
    // second, and one whose engine connection is gone is rebuilt. Catches a
    // restart the link never reported (back within one probe) and retries a
    // rebuild that could not get a connection.
    void recover_dead_sessions(std::chrono::steady_clock::time_point now);

    Strand& strand_;
    StreamsConfig base_;
    HubTiming timing_;
    NewSession new_session_;
    std::map<std::string, std::unique_ptr<Resident>> residents_;
    std::map<std::uint64_t, Connection> connections_;
    std::uint64_t next_connection_ = 1;
    LinkState link_state_ = LinkState::Starting;
    std::string link_detail_;
    // Tokens and new holders of residents found to have lost the lease while
    // residents_ was being walked.
    std::vector<std::pair<std::string, std::string>> pending_losses_;
    // Cleared by the destructor, so a leaseLost posted after it does nothing.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace revenant::webgate
