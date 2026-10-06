// plugins/p25trunk against a host table of this file's own, with no engine
// behind it. Events are built by hand with the field keys core/rpc/decoders.h
// documents for the p25p1 decoder, and every host call the plugin makes is
// recorded, so each case reads as "these events in, these requests out".
//
// What this does not cover is the engine's half: whether a real control
// channel produces those fields. tests/decode/test_p25_tsbk.cpp and
// tests/rpc/test_rpc_decode.cpp cover that.

#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/plugin/engine_plugin_abi.h"

#include "p25trunk_path.h"

namespace {

struct Request {
    std::string what;  // "add", "remove", "subscribe", "unsubscribe"
    std::int64_t frequency_hz = 0;
    std::uint32_t vrx = 0;
    std::uint32_t tag = 0;
};

struct Recorder {
    std::vector<Request> requests;
    std::vector<std::string> log;
};

std::int32_t RV_ENGINE_PLUGIN_CALL add_vrx(void* ctx, std::int64_t hz, std::uint32_t,
                                           const char*, std::uint32_t tag)
{
    static_cast<Recorder*>(ctx)->requests.push_back({"add", hz, 0, tag});
    return RV_ENGINE_PLUGIN_OK;
}
std::int32_t RV_ENGINE_PLUGIN_CALL remove_vrx(void* ctx, std::uint32_t vrx, std::uint32_t tag)
{
    static_cast<Recorder*>(ctx)->requests.push_back({"remove", 0, vrx, tag});
    return RV_ENGINE_PLUGIN_OK;
}
std::int32_t RV_ENGINE_PLUGIN_CALL set_vrx_center(void*, std::uint32_t, std::int64_t,
                                                  std::uint32_t)
{
    return RV_ENGINE_PLUGIN_OK;
}
std::int32_t RV_ENGINE_PLUGIN_CALL set_source_center(void*, std::int64_t, std::uint32_t)
{
    return RV_ENGINE_PLUGIN_OK;
}
std::int32_t RV_ENGINE_PLUGIN_CALL subscribe(void* ctx, std::uint32_t vrx, const char*,
                                             std::uint32_t tag)
{
    static_cast<Recorder*>(ctx)->requests.push_back({"subscribe", 0, vrx, tag});
    return RV_ENGINE_PLUGIN_OK;
}
std::int32_t RV_ENGINE_PLUGIN_CALL unsubscribe(void* ctx, std::uint32_t vrx, const char*,
                                               std::uint32_t tag)
{
    static_cast<Recorder*>(ctx)->requests.push_back({"unsubscribe", 0, vrx, tag});
    return RV_ENGINE_PLUGIN_OK;
}
void RV_ENGINE_PLUGIN_CALL log_line(void* ctx, std::uint32_t, const char* message)
{
    static_cast<Recorder*>(ctx)->log.emplace_back(message);
}

constexpr std::uint32_t kControl = 7;
constexpr std::uint32_t kRate = 48'000;
constexpr std::int64_t kVoiceHz = 851'012'500;

class Tracker {
public:
    Tracker() {
        module_ = LoadLibraryA(kP25TrunkPlugin);
        REQUIRE(module_ != nullptr);
        create_ = reinterpret_cast<rv_engine_plugin_create_fn>(
            GetProcAddress(module_, "revenant_engine_plugin_create"));
        on_event_ = reinterpret_cast<rv_engine_plugin_on_event_fn>(
            GetProcAddress(module_, "revenant_engine_plugin_on_event"));
        destroy_ = reinterpret_cast<rv_engine_plugin_destroy_fn>(
            GetProcAddress(module_, "revenant_engine_plugin_destroy"));
        REQUIRE(create_ != nullptr);
        REQUIRE(on_event_ != nullptr);
        REQUIRE(destroy_ != nullptr);

        host_.struct_size = sizeof(rv_engine_host);
        host_.ctx = &rec;
        host_.add_vrx = add_vrx;
        host_.remove_vrx = remove_vrx;
        host_.set_vrx_center = set_vrx_center;
        host_.set_source_center = set_source_center;
        host_.subscribe_decoded = subscribe;
        host_.unsubscribe_decoded = unsubscribe;
        host_.log = log_line;
        plugin_ = create_(&host_);
        REQUIRE(plugin_ != nullptr);
    }
    ~Tracker() {
        destroy_(plugin_);
        FreeLibrary(module_);
    }
    Tracker(const Tracker&) = delete;
    Tracker& operator=(const Tracker&) = delete;

    void send(rv_engine_event ev, std::vector<rv_engine_field> fields = {}) {
        ev.struct_size = sizeof(rv_engine_event);
        ev.fields = fields.data();
        ev.field_count = static_cast<std::uint32_t>(fields.size());
        on_event_(plugin_, &ev);
    }

    void client_receiver(std::uint32_t vrx) {
        rv_engine_event ev = blank(RV_ENGINE_EVENT_VRX_ADDED);
        ev.vrx = vrx;
        ev.owner = RV_ENGINE_OWNER_CLIENT;
        ev.demod = "p25p1";
        send(ev);
    }

    // A control channel TSBK at `seconds` on the control channel's clock.
    void tsbk(double seconds, std::int64_t opcode, std::vector<rv_engine_field> extra = {}) {
        rv_engine_event ev = blank(RV_ENGINE_EVENT_DECODED);
        ev.vrx = kControl;
        ev.decoder = "p25p1";
        ev.kind = "tsbk";
        ev.sample_rate = kRate;
        ev.end_sample = static_cast<std::uint64_t>(seconds * kRate);
        extra.push_back(flag("crc_ok", true));
        extra.push_back(integer("manufacturer_id", 0));
        extra.push_back(integer("opcode", opcode));
        send(ev, std::move(extra));
    }

    void grant(double seconds, std::int64_t group, bool encrypted = false) {
        tsbk(seconds, 0x00,
             {integer("group", group), integer("channel_frequency_hz", kVoiceHz),
              flag("encrypted_call", encrypted)});
    }

    void idle(double seconds) { tsbk(seconds, 0x3A); }  // RFSS status

    void result(std::uint32_t tag, std::uint32_t vrx, std::int32_t code = RV_ENGINE_PLUGIN_OK) {
        rv_engine_event ev = blank(RV_ENGINE_EVENT_COMMAND_RESULT);
        ev.request_tag = tag;
        ev.vrx = vrx;
        ev.result_code = code;
        ev.text = code == RV_ENGINE_PLUGIN_OK ? "" : "outside the span";
        send(ev);
    }

    void voice(std::uint32_t vrx, const char* kind, bool encrypted = false) {
        rv_engine_event ev = blank(RV_ENGINE_EVENT_DECODED);
        ev.vrx = vrx;
        ev.decoder = "p25p1";
        ev.kind = kind;
        send(ev, {flag("encrypted", encrypted)});
    }

    void removed(std::uint32_t vrx) {
        rv_engine_event ev = blank(RV_ENGINE_EVENT_VRX_REMOVED);
        ev.vrx = vrx;
        ev.owner = RV_ENGINE_OWNER_CLIENT;
        ev.text = "closed by the operator";
        send(ev);
    }

    [[nodiscard]] std::vector<Request> take() {
        std::vector<Request> out;
        out.swap(rec.requests);
        return out;
    }

    // Opens the control channel and the first call on it, the receiver given
    // id `voice_vrx`, and clears the record.
    void start_call(std::uint32_t voice_vrx, std::int64_t group = 100) {
        client_receiver(kControl);
        idle(1.0);
        grant(1.1, group);
        const auto requests = take();
        REQUIRE(requests.size() == 2);
        REQUIRE(requests[1].what == "add");
        result(requests[1].tag, voice_vrx);
        static_cast<void>(take());
    }

    static rv_engine_field integer(const char* key, std::int64_t value) {
        rv_engine_field f{};
        f.key = key;
        f.type = RV_ENGINE_FIELD_INT;
        f.int_value = value;
        return f;
    }
    static rv_engine_field flag(const char* key, bool value) {
        rv_engine_field f{};
        f.key = key;
        f.type = RV_ENGINE_FIELD_BOOL;
        f.bool_value = value ? 1U : 0U;
        return f;
    }

    Recorder rec;

private:
    static rv_engine_event blank(std::uint32_t type) {
        rv_engine_event ev{};
        ev.type = type;
        ev.demod = "";
        ev.decoder = "";
        ev.kind = "";
        ev.text = "";
        return ev;
    }

    HMODULE module_ = nullptr;
    rv_engine_plugin_create_fn create_ = nullptr;
    rv_engine_plugin_on_event_fn on_event_ = nullptr;
    rv_engine_plugin_destroy_fn destroy_ = nullptr;
    rv_engine_host host_{};
    rv_engine_plugin* plugin_ = nullptr;
};

}  // namespace

TEST_CASE("p25trunk subscribes to a client's p25p1 receiver", "[plugin][p25trunk]") {
    Tracker t;
    t.client_receiver(kControl);
    const auto requests = t.take();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].what == "subscribe");
    CHECK(requests[0].vrx == kControl);
}

TEST_CASE("p25trunk opens a receiver on a grant and subscribes to it", "[plugin][p25trunk]") {
    Tracker t;
    t.client_receiver(kControl);
    t.idle(1.0);
    t.grant(1.1, 100);
    auto requests = t.take();
    REQUIRE(requests.size() == 2);
    CHECK(requests[1].what == "add");
    CHECK(requests[1].frequency_hz == kVoiceHz);

    t.result(requests[1].tag, 20);
    requests = t.take();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].what == "subscribe");
    CHECK(requests[0].vrx == 20);

    // A repeated grant for the same call opens nothing more.
    t.grant(1.5, 100);
    CHECK(t.take().empty());
}

TEST_CASE("p25trunk releases a call nothing is heard about", "[plugin][p25trunk]") {
    Tracker t;
    t.start_call(20);
    t.idle(2.0);
    CHECK(t.take().empty());
    t.idle(4.0);
    const auto requests = t.take();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].what == "remove");
    CHECK(requests[0].vrx == 20);
}

TEST_CASE("p25trunk voice frames keep a call open", "[plugin][p25trunk]") {
    Tracker t;
    t.start_call(20);
    for (double s = 1.5; s < 6.0; s += 0.5) {
        t.voice(20, "ldu1");
        t.idle(s);
    }
    CHECK(t.take().empty());
}

TEST_CASE("p25trunk releases on a terminator after the hold", "[plugin][p25trunk]") {
    Tracker t;
    t.start_call(20);
    t.voice(20, "tdulc");
    t.idle(1.2);
    // Updates do not hold a terminated call open.
    t.tsbk(1.3, 0x02, {Tracker::integer("group", 100),
                       Tracker::integer("channel_frequency_hz", kVoiceHz)});
    CHECK(t.take().empty());
    t.idle(1.7);
    const auto requests = t.take();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].what == "remove");
}

TEST_CASE("p25trunk a new grant after a terminator keeps the receiver", "[plugin][p25trunk]") {
    Tracker t;
    t.start_call(20);
    t.voice(20, "tdu");
    t.grant(1.3, 100);
    t.idle(1.9);
    CHECK(t.take().empty());
}

TEST_CASE("p25trunk skips encrypted calls by default", "[plugin][p25trunk]") {
    Tracker t;
    t.client_receiver(kControl);
    t.grant(1.0, 300, true);
    // An update for the same talkgroup, which carries no service options.
    t.tsbk(1.5, 0x02, {Tracker::integer("group", 300),
                       Tracker::integer("channel_frequency_hz", kVoiceHz)});
    const auto requests = t.take();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].what == "subscribe");
}

TEST_CASE("p25trunk releases a call joined mid-way when its voice is encrypted",
          "[plugin][p25trunk]") {
    Tracker t;
    t.client_receiver(kControl);
    t.idle(1.0);
    t.tsbk(1.1, 0x02, {Tracker::integer("group", 300),
                       Tracker::integer("channel_frequency_hz", kVoiceHz)});
    auto requests = t.take();
    REQUIRE(requests.size() == 2);
    t.result(requests[1].tag, 20);
    static_cast<void>(t.take());
    t.voice(20, "ldu2", true);
    requests = t.take();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].what == "remove");
}

TEST_CASE("p25trunk does not retry a refused frequency at once", "[plugin][p25trunk]") {
    Tracker t;
    t.client_receiver(kControl);
    t.grant(1.0, 100);
    auto requests = t.take();
    REQUIRE(requests.size() == 2);
    t.result(requests[1].tag, 0, RV_ENGINE_PLUGIN_ERR_REFUSED);
    t.grant(2.0, 100);
    CHECK(t.take().empty());
    t.grant(7.0, 100);
    requests = t.take();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].what == "add");
}

TEST_CASE("p25trunk removes its receivers when the control channel goes", "[plugin][p25trunk]") {
    Tracker t;
    t.start_call(20);
    t.removed(kControl);
    const auto requests = t.take();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].what == "remove");
    CHECK(requests[0].vrx == 20);
}

TEST_CASE("p25trunk removes a receiver whose add finished after the release",
          "[plugin][p25trunk]") {
    Tracker t;
    t.client_receiver(kControl);
    t.grant(1.0, 100);
    auto requests = t.take();
    REQUIRE(requests.size() == 2);
    t.idle(4.0);  // hangs before the add's result arrives
    t.result(requests[1].tag, 20);
    requests = t.take();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].what == "remove");
    CHECK(requests[0].vrx == 20);
}
