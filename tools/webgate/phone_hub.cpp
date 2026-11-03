#include "tools/webgate/phone_hub.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <utility>
#include <vector>

#include "tools/webgate/protocol.h"

namespace revenant::webgate {

namespace {

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

// 128 bits, hex. std::random_device on MSVC draws from the OS generator
// (rand_s), so the token cannot be guessed from earlier ones; it is what
// stands between another LAN client and someone's detached session.
[[nodiscard]] std::string new_token()
{
    std::random_device device;
    std::string out;
    constexpr char kHex[] = "0123456789abcdef";
    for (int word = 0; word < 4; ++word) {
        auto value = static_cast<std::uint32_t>(device());
        for (int nibble = 0; nibble < 8; ++nibble) {
            out.push_back(kHex[value & 0xFU]);
            value >>= 4U;
        }
    }
    return out;
}

}  // namespace

std::string link_event(LinkState state, const std::string& detail)
{
    return event("link",
                 JsonObject().add_string("state", state_name(state)).add_string("detail", detail));
}

PhoneHub::PhoneHub(Strand& strand, StreamsConfig base, HubTiming timing)
    : strand_(strand), base_(std::move(base)), timing_(timing)
{
}

PhoneHub::~PhoneHub()
{
    clear();
}

std::chrono::steady_clock::time_point PhoneHub::now() const
{
    return base_.clock ? base_.clock() : std::chrono::steady_clock::now();
}

std::uint64_t PhoneHub::connect(PhoneSocket& socket, std::unique_ptr<rpc::Client> client)
{
    auto resident = std::make_unique<Resident>();
    resident->token = new_token();
    resident->session = std::make_unique<PhoneSession>(std::move(client));
    resident->streams =
        std::make_unique<PhoneStreams>(*resident->session, &socket, strand_, base_);

    const auto id = next_connection_++;
    resident->connection = id;
    Resident* raw = resident.get();
    residents_.emplace(resident->token, std::move(resident));
    const auto t = now();
    connections_[id] = Connection{&socket, raw, false, t, t};

    raw->streams->send_event(event("hello", JsonObject().add_string("resume", raw->token)));
    raw->streams->start();
    raw->streams->send_event(link_event(link_state_, link_detail_));
    return id;
}

void PhoneHub::text(std::uint64_t id, std::string_view text)
{
    const auto it = connections_.find(id);
    if (it == connections_.end() || it->second.resident == nullptr) {
        return;
    }
    auto& connection = it->second;
    connection.last_heard = now();
    auto command = parse_command(text);
    if (command && command->op == "pong") {
        return;
    }
    const bool first = !connection.commanded;
    connection.commanded = true;

    if (command && command->op == "resume") {
        if (!first) {
            connection.resident->streams->send_event(
                error_reply(command->id, "resume: only as the first command on a connection"));
            return;
        }
        resume(id, connection, *command);
        return;
    }
    connection.resident->streams->on_text(text);
}

void PhoneHub::resume(std::uint64_t id, Connection& connection, const Command& command)
{
    Resident* fresh = connection.resident;
    const auto token = command.get_string("token");
    if (!token) {
        fresh->streams->send_event(error_reply(command.id, token.error().message));
        return;
    }
    Resident* old = find_token(*token);
    if (old == nullptr || old == fresh) {
        // Expired or never issued. The phone keeps the fresh session it was
        // greeted with and rebuilds from there.
        fresh->streams->send_event(error_reply(
            command.id, "resume: no session with that token; this is a new session"));
        return;
    }
    if (old->connection) {
        // Still attached to a socket nobody reads: the usual state after an
        // iOS lock, where the TCP connection lingers without a FIN. The token
        // is proof of ownership, so the old socket is closed and the session
        // comes here.
        const auto previous = *old->connection;
        if (const auto prev = connections_.find(previous); prev != connections_.end()) {
            prev->second.socket->close();
        }
        disconnect(previous);
    }

    residents_.erase(fresh->token);  // nothing of the fresh one is wanted
    connection.resident = old;
    old->connection = id;
    old->last_hls.reset();
    old->streams->attach(*connection.socket);
    old->streams->send_event(
        ok_reply(command.id, JsonObject().add_bool("resumed", true).str()));
    old->streams->send_event(link_event(link_state_, link_detail_));
}

PhoneHub::Resident* PhoneHub::find_token(std::string_view token) const
{
    // Every entry is compared in full, with no early exit, so the time taken
    // says nothing about how close a guess was.
    Resident* match = nullptr;
    for (const auto& [key, resident] : residents_) {
        unsigned diff = key.size() == token.size() ? 0U : 1U;
        const std::size_t n = std::min(key.size(), token.size());
        for (std::size_t i = 0; i < n; ++i) {
            diff |= static_cast<unsigned>(static_cast<unsigned char>(key[i]) ^
                                          static_cast<unsigned char>(token[i]));
        }
        if (diff == 0U) {
            match = resident.get();
        }
    }
    return match;
}

void PhoneHub::disconnect(std::uint64_t id)
{
    const auto it = connections_.find(id);
    if (it == connections_.end()) {
        return;
    }
    if (Resident* resident = it->second.resident) {
        resident->connection.reset();
        resident->detached_at = now();
        resident->last_hls.reset();
        resident->streams->detach();
    }
    connections_.erase(it);
}

void PhoneHub::hls_fetched(std::uint64_t vrx)
{
    for (auto& [token, resident] : residents_) {
        if (resident->streams->segmenter(vrx) != nullptr) {
            resident->last_hls = now();
        }
    }
}

void PhoneHub::tick()
{
    const auto t = now();

    std::vector<std::uint64_t> silent;
    for (auto& [id, connection] : connections_) {
        if (t - connection.last_heard > timing_.silence_limit) {
            silent.push_back(id);
        } else if (t - connection.last_ping >= timing_.ping_interval &&
                   connection.resident != nullptr) {
            connection.last_ping = t;
            connection.resident->streams->send_event(event("ping"));
        }
    }
    for (const auto id : silent) {
        connections_.at(id).socket->close();
        disconnect(id);
    }

    std::vector<std::string> expired;
    for (auto& [token, resident] : residents_) {
        resident->streams->tick();
        if (resident->connection) {
            continue;
        }
        const bool listening = resident->session->keep_listening();
        const auto since = listening ? std::max(resident->detached_at,
                                                resident->last_hls.value_or(resident->detached_at))
                                     : resident->detached_at;
        const auto limit = listening ? timing_.hls_idle : timing_.detached_linger;
        if (t - since > limit) {
            expired.push_back(token);
        }
    }
    for (const auto& token : expired) {
        residents_.erase(token);
    }
}

void PhoneHub::link_state(LinkState state, std::string detail)
{
    link_state_ = state;
    link_detail_ = std::move(detail);
    const auto text = link_event(link_state_, link_detail_);
    for (auto& [token, resident] : residents_) {
        resident->streams->send_event(text);
    }
}

void PhoneHub::clear()
{
    for (auto& [id, connection] : connections_) {
        connection.resident = nullptr;
    }
    residents_.clear();
}

PhoneSession* PhoneHub::session(std::uint64_t id) const
{
    const auto it = connections_.find(id);
    return it == connections_.end() || it->second.resident == nullptr
               ? nullptr
               : it->second.resident->session.get();
}

PhoneStreams* PhoneHub::streams(std::uint64_t id) const
{
    const auto it = connections_.find(id);
    return it == connections_.end() || it->second.resident == nullptr
               ? nullptr
               : it->second.resident->streams.get();
}

std::string PhoneHub::token(std::uint64_t id) const
{
    const auto it = connections_.find(id);
    return it == connections_.end() || it->second.resident == nullptr
               ? std::string()
               : it->second.resident->token;
}

}  // namespace revenant::webgate
