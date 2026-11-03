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
//                       must not hold one for ten minutes.
//   otherwise:          detached_linger after the drop.
//   keep listening on:  the same, and also until hls_idle after the last HLS
//                        request for one of its receivers, whichever is later.
// The base linger is long because an iPhone app sits backgrounded for minutes
// between glances, and coming back to its receivers beats starting over.
// On expiry it is destroyed exactly as a disconnect used to destroy it: its
// streams unsubscribe and its receivers are removed.
//
// At most max_residents sessions exist at once. A connection that would go
// past that evicts the detached session that dropped longest ago; when every
// session is attached the connection is refused (make_room says so).
//
// No kj here, so tests drive it with a fake socket, a manual strand and a
// fake clock. Strand thread only, like PhoneStreams.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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
    std::chrono::milliseconds detached_linger{600'000};
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

// Where a session gets a fresh engine connection when the engine restarted.
using NewSession = std::function<Expected<std::unique_ptr<rpc::Client>>()>;

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

    // A new connection with a fresh session on client. Calls make_room
    // first; the caller is expected to have checked it. Sends "hello" with the
    // session's resume token, then the current link state. Returns the
    // connection id the other calls take.
    std::uint64_t connect(PhoneSocket& socket, std::unique_ptr<rpc::Client> client);

    // One text frame. "resume" is handled here: allowed only as a
    // connection's first command, it swaps the fresh session for the one
    // holding the token, taking it from another connection if one still has
    // it (that socket is closed). "pong" only marks the phone as alive.
    // Everything else goes to the session's streams.
    void text(std::uint64_t connection, std::string_view text);

    // The socket is gone. Its session detaches and starts expiring.
    void disconnect(std::uint64_t connection);

    // An HLS request was answered for this receiver.
    void hls_fetched(std::uint64_t vrx);

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

private:
    struct Resident {
        std::string token;
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
    };

    [[nodiscard]] std::chrono::steady_clock::time_point now() const;
    void resume(std::uint64_t id, Connection& connection, const Command& command);
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
};

}  // namespace revenant::webgate
