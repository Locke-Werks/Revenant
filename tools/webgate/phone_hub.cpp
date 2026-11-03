#include "tools/webgate/phone_hub.h"

#include <algorithm>
#include <cstdio>
#include <optional>
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

constexpr auto kProbeInterval = std::chrono::seconds(1);

// A call on a client whose connection has gone fails at once with
// Disconnected; anything else (an engine that answered with an error) means
// the connection works.
[[nodiscard]] bool connection_dead(rpc::Client& client)
{
    const auto info = client.info();
    return !info && info.error().category == ErrorCategory::Disconnected;
}

[[nodiscard]] std::string lost_entry(std::uint64_t vrx, const std::string& reason)
{
    return JsonObject().add_u64("vrx", vrx).add_string("reason", reason).str();
}

[[nodiscard]] std::string json_array(const std::vector<std::string>& items)
{
    std::string out = "[";
    for (const auto& item : items) {
        if (out.size() > 1) {
            out += ',';
        }
        out += item;
    }
    return out + "]";
}

}  // namespace

std::string link_event(LinkState state, const std::string& detail)
{
    return event("link",
                 JsonObject().add_string("state", state_name(state)).add_string("detail", detail));
}

PhoneHub::PhoneHub(Strand& strand, StreamsConfig base, HubTiming timing, NewSession new_session)
    : strand_(strand), base_(std::move(base)), timing_(timing), new_session_(std::move(new_session))
{
    // Every phone this hub makes copies base_, so they all share one cache.
    if (!base_.frame_levels) {
        base_.frame_levels = std::make_shared<FrameLevelsCache>();
    }
}

PhoneHub::~PhoneHub()
{
    clear();
}

std::chrono::steady_clock::time_point PhoneHub::now() const
{
    return base_.clock ? base_.clock() : std::chrono::steady_clock::now();
}

bool PhoneHub::make_room()
{
    while (residents_.size() >= timing_.max_residents) {
        auto oldest = residents_.end();
        for (auto it = residents_.begin(); it != residents_.end(); ++it) {
            if (!it->second->connection &&
                (oldest == residents_.end() ||
                 it->second->detached_at < oldest->second->detached_at)) {
                oldest = it;
            }
        }
        if (oldest == residents_.end()) {
            return false;
        }
        residents_.erase(oldest);
    }
    return true;
}

std::uint64_t PhoneHub::connect(PhoneSocket& socket, std::unique_ptr<rpc::Client> client)
{
    static_cast<void>(make_room());
    auto resident = std::make_unique<Resident>();
    resident->token = new_token();
    resident->session = std::make_unique<PhoneSession>(std::move(client));
    // The detection bar starts where the desktop's is. A phone that has moved
    // its slider sends its own on connecting, which replaces this.
    if (base_.desktop_settings) {
        if (const auto bar = base_.desktop_settings().confidence_bar) {
            resident->session->set_min_confidence(*bar);
        }
    }
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
    old->streams->announce_state();
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

void PhoneHub::hls_fetched(std::uint64_t hls_id)
{
    for (auto& [token, resident] : residents_) {
        if (resident->streams->segmenter(hls_id) != nullptr) {
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
        // Keep listening extends the base linger while HLS is being fetched;
        // it never shortens it. A session with nothing to keep goes sooner.
        const bool empty = !resident->session->keep_listening() &&
                           resident->session->owned_ids().empty();
        bool expire = t - resident->detached_at >
                      (empty ? timing_.empty_linger : timing_.detached_linger);
        if (expire && resident->session->keep_listening()) {
            const auto since = std::max(resident->detached_at,
                                        resident->last_hls.value_or(resident->detached_at));
            expire = t - since > timing_.hls_idle;
        }
        if (expire) {
            expired.push_back(token);
        }
    }
    for (const auto& token : expired) {
        residents_.erase(token);
    }

    recover_dead_sessions(t);
}

void PhoneHub::recover_dead_sessions(std::chrono::steady_clock::time_point t)
{
    if (link_state_ != LinkState::Up || !new_session_) {
        return;
    }
    for (auto& [token, resident] : residents_) {
        if (t - resident->last_probe < kProbeInterval) {
            continue;
        }
        resident->last_probe = t;
        JsonObject fields;
        fields.add_string("state", state_name(link_state_)).add_string("detail", link_detail_);
        static_cast<void>(recreate(*resident, fields));
    }
}

void PhoneHub::link_state(LinkState state, std::string detail)
{
    const bool rising = state == LinkState::Up && link_state_ != LinkState::Up;
    link_state_ = state;
    link_detail_ = std::move(detail);
    const auto text = link_event(link_state_, link_detail_);
    for (auto& [token, resident] : residents_) {
        JsonObject fields;
        fields.add_string("state", state_name(link_state_)).add_string("detail", link_detail_);
        if (!(rising && new_session_ && recreate(*resident, fields))) {
            resident->streams->send_event(text);
        }
        resident->last_probe = now();
    }
}

bool PhoneHub::recreate(Resident& resident, JsonObject& fields)
{
    // A link that dropped and came back without the engine going away (a
    // missed probe) leaves this session's connection working, and its
    // receivers still exist: nothing to rebuild.
    auto& old_session = *resident.session;
    if (!connection_dead(old_session.client())) {
        return false;
    }

    auto client = new_session_();
    if (!client) {
        // Nothing to rebuild on yet. The session keeps its receivers' params
        // and tick() tries again a second later; the phone hears about its
        // receivers when they are back.
        std::fprintf(stderr, "revenant-webgate: rebuilding a phone session: %s\n",
                     client.error().message.c_str());
        return false;
    }

    auto fresh = std::make_unique<PhoneSession>(std::move(*client));
    if (old_session.view()) {
        fresh->set_view(*old_session.view());
    }
    fresh->set_keep_listening(old_session.keep_listening());
    fresh->set_min_confidence(old_session.min_confidence());

    // Params hold the receiver's offset from the source centre. The phone
    // asked for an absolute frequency, so that is what is kept if the new
    // engine's source sits somewhere else.
    const auto old_center = resident.streams->source_center();
    std::optional<std::int64_t> new_center;
    if (const auto info = fresh->client().info(); info && info->source_rate != 0) {
        new_center = info->source_center;
    }

    std::vector<std::string> recreated;
    std::vector<std::string> lost;
    std::vector<std::pair<std::uint64_t, std::string>> removed;
    for (const auto old_id : old_session.owned_ids()) {
        const auto* last = old_session.last_params(old_id);
        if (last == nullptr) {
            // owned_ids and last_params read the same map, so this cannot
            // happen; if it ever does, the phone hears it was lost rather
            // than the gateway dereferencing nothing.
            const std::string reason = "the gateway kept no parameters for this receiver";
            lost.push_back(lost_entry(old_id, reason));
            removed.emplace_back(old_id, reason);
            continue;
        }
        rpc::VrxParams params = *last;
        if (old_center && new_center) {
            params.center = *old_center + params.center - *new_center;
        }
        const auto choice = old_session.transcribe_choice(old_id);
        auto added = fresh->client().add_vrx(params);
        if (added && choice != rpc::TranscribeChoice::Auto) {
            if (auto set = fresh->client().set_vrx_transcribe(*added, choice); !set) {
                // A receiver without the choice the phone made is not the one
                // it had; take it back out and report the engine's reason.
                (void)fresh->client().remove_vrx(*added);
                added = std::unexpected(set.error());
            }
        }
        if (!added) {
            lost.push_back(lost_entry(old_id, added.error().message));
            removed.emplace_back(old_id, added.error().message);
            continue;
        }
        fresh->adopt(*added, params);
        fresh->set_transcribe_choice(*added, choice);
        if (const auto* chosen = old_session.decoder_choice(old_id)) {
            fresh->set_decoders(*added, *chosen);
        }
        if (old_session.audio_vrx() == old_id) {
            fresh->set_audio_vrx(*added);
        }
        recreated.push_back(JsonObject().add_u64("old", old_id).add_u64("new", *added).str());
    }
    fields.add_raw("recreated", json_array(recreated)).add_raw("lost", json_array(lost));

    // Unsent JSON (a "waiting" link event, say) goes out ahead of anything
    // the new streams say. The old streams go before the old session: they
    // unsubscribe through its client, which fails at once on a dead
    // connection, as does the session's own removal of its old ids.
    auto pending = resident.streams->take_texts();
    auto streams = std::make_unique<PhoneStreams>(*fresh, nullptr, strand_, base_);
    for (auto& text : pending) {
        streams->send_event(std::move(text));
    }
    // The link event first, so the phone has the id map before the source,
    // hls and status events of the new receivers.
    streams->send_event(event("link", fields));
    for (const auto& [vrx, reason] : removed) {
        streams->send_event(
            event("vrxRemoved", JsonObject().add_u64("vrx", vrx).add_string("reason", reason)));
    }
    resident.streams.reset();
    resident.session = std::move(fresh);
    resident.streams = std::move(streams);
    if (resident.connection) {
        if (const auto it = connections_.find(*resident.connection); it != connections_.end()) {
            resident.streams->attach(*it->second.socket);
        }
    }
    resident.streams->start();
    resident.streams->resync();
    return true;
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
