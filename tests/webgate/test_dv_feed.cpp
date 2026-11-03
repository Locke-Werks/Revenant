// The DV tab's gateway side: dv_feed.h's pure rules, then the stream layer
// running Auto DV, the trunk status poll and the call log against the
// in-process engine of tests/rpc/rpc_fixture.h.

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

// gateway.h carries kj-http, which has to precede anything that pulls in
// <windows.h>.
#include "tools/webgate/gateway.h"

#include "tests/reference/gpu_fixture.h"
#include "tests/rpc/rpc_fixture.h"
#include "tests/webgate/stream_fakes.h"
#include "tools/webgate/dispatch.h"
#include "tools/webgate/dv_feed.h"
#include "tools/webgate/phone_streams.h"
#include "tools/webgate/session.h"

namespace {

namespace rpc = revenant::rpc;
namespace wg = revenant::webgate;
using Kind = wg::OutgoingMessage::Kind;
using Clock = std::chrono::steady_clock;

[[nodiscard]] rpc::Detection dv(std::int64_t hz, const std::string& protocol = "P25",
                                double snr = 20.0)
{
    rpc::Detection d;
    d.center_hz = hz;
    d.bandwidth_hz = 9'000;
    d.snr_2500_db = snr;
    d.state = rpc::TrackState::Live;
    d.label.kind = rpc::LabelKind::Protocol;
    d.label.name = protocol;
    d.label.may_drive = true;
    return d;
}

[[nodiscard]] rpc::Detection control(std::int64_t hz)
{
    rpc::Detection d = dv(hz, "P25 control");
    d.label.path = {{"NFM", 0.9, true}, {"4FSK", 0.9, true}, {"P25", 0.9, true},
                    {"control", 0.8, true}};
    d.label.confirmed_depth = 4;
    return d;
}

[[nodiscard]] rpc::DecodedField integer(std::string key, std::int64_t value)
{
    return rpc::DecodedField{std::move(key), rpc::DecodedValue{std::in_place_index<0>, value}};
}

[[nodiscard]] rpc::DecodedMessage p25(std::uint64_t vrx, std::string kind,
                                      std::vector<rpc::DecodedField> fields = {})
{
    rpc::DecodedMessage m;
    m.vrx = vrx;
    m.decoder = "p25p1";
    m.kind = std::move(kind);
    fields.push_back(integer("nac", 0x344));
    m.fields = std::move(fields);
    return m;
}

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// ---- The pure rules ---------------------------------------------------------

TEST_CASE("auto DV plans a receiver for an uncovered DV detection only", "[webgate][dv]")
{
    const std::vector<rpc::Detection> seen{dv(851'000'000), dv(852'000'000, "DMR"),
                                           dv(853'000'000)};
    const std::vector<wg::PlacedReceiver> placed{{7, 853'000'200.0}};
    const auto pass = wg::plan_auto_dv(seen, placed, {}, {}, std::nullopt);
    REQUIRE(pass.open.size() == 2);
    CHECK(pass.retire.empty());
    for (const auto& spawn : pass.open) {
        // Not over the receiver already at 853 MHz, whoever opened it.
        CHECK(spawn.centre_hz != 853'000'000.0);
    }
}

TEST_CASE("auto DV never removes a receiver whose signal went quiet", "[webgate][dv]")
{
    const std::vector<wg::PlacedReceiver> placed{{3, 851'000'000.0}};
    const auto pass = wg::plan_auto_dv({}, placed, {3}, {}, std::nullopt);
    CHECK(pass.retire.empty());
    CHECK(pass.open.empty());
}

TEST_CASE("auto DV hands back only its own receiver on a control channel, not the one heard",
          "[webgate][dv]")
{
    const std::vector<rpc::Detection> seen{control(851'000'000), control(852'000'000),
                                           control(853'000'000)};
    const std::vector<wg::PlacedReceiver> placed{
        {1, 851'000'000.0}, {2, 852'000'000.0}, {3, 853'000'000.0}};
    // 1 is auto DV's, 2 the operator's, 3 auto DV's and being listened to.
    const auto pass = wg::plan_auto_dv(seen, placed, {1, 3}, {}, std::uint64_t{3});
    CHECK(pass.retire == std::vector<std::uint64_t>{1});
    CHECK(pass.open.empty());
}

TEST_CASE("auto DV leaves a dismissed frequency alone and stops at the rack limit",
          "[webgate][dv]")
{
    const std::vector<rpc::Detection> seen{dv(851'000'000), dv(852'000'000)};
    const std::vector<double> dismissed{851'000'000.0};
    const auto pass = wg::plan_auto_dv(seen, {}, {}, dismissed, std::nullopt);
    REQUIRE(pass.open.size() == 1);
    CHECK(pass.open.front().centre_hz == 852'000'000.0);

    const std::vector<wg::PlacedReceiver> full{{1, 100.0}, {2, 200.0}};
    CHECK(wg::plan_auto_dv(seen, full, {}, {}, std::nullopt, 2).open.empty());
}

TEST_CASE("call rows carry the alias, the state and newest first", "[webgate][dv]")
{
    revenant::ui::CallLog log;
    for (const auto& f : revenant::ui::call_frames(
             p25(1, "hdu", {integer("talkgroup", 1201), integer("source", 7001)}), 1000,
             851'000'000, "user")) {
        log.apply(f);
    }
    for (const auto& f :
         revenant::ui::call_frames(p25(2, "ldu1", {integer("talkgroup", 300)}), 1200, 0, "trunk")) {
        log.apply(f);
    }
    const wg::TrunkAliases aliases{{1201, "Fire Dispatch"}};
    const std::string calls = wg::calls_event(log, aliases);
    INFO(calls);
    CHECK(contains(calls, R"("event":"calls")"));
    CHECK(contains(calls, R"("alias":"Fire Dispatch")"));
    CHECK(contains(calls, R"("state":"live")"));
    CHECK(contains(calls, R"("owner":"trunk")"));
    CHECK(calls.find(R"("target":"300")") < calls.find(R"("target":"1201")"));

    log.expire(10'000);
    CHECK(contains(wg::calls_event(log, aliases), R"("state":"ended")"));
    const std::string groups = wg::talkgroups_event(log, aliases);
    CHECK(contains(groups, R"("event":"talkgroups")"));
    CHECK(contains(groups, R"("count":1)"));
}

TEST_CASE("a grant no receiver took reads as granted", "[webgate][dv]")
{
    revenant::ui::CallLog log;
    for (const auto& f : revenant::ui::call_frames(
             p25(4, "tsbk", {integer("group", 500), integer("channel_frequency_hz", 852'500'000)}),
             1000, 0, "user")) {
        log.apply(f);
    }
    CHECK(contains(wg::calls_event(log, {}), R"("state":"granted")"));
}

TEST_CASE("trunk rules read the desktop's text and compare order-blind", "[webgate][dv]")
{
    const auto parsed = wg::trunk_rules_from_text("100, 200", "300\nnope", "500=5");
    CHECK(parsed.rules.allow == std::vector<std::uint32_t>{100, 200});
    CHECK(parsed.rules.deny == std::vector<std::uint32_t>{300});
    REQUIRE(parsed.rules.priorities.size() == 1);
    CHECK(parsed.skipped == 1);
    auto shuffled = parsed.rules;
    std::swap(shuffled.allow[0], shuffled.allow[1]);
    CHECK(wg::same_trunk_rules(parsed.rules, shuffled));
    shuffled.deny.clear();
    CHECK_FALSE(wg::same_trunk_rules(parsed.rules, shuffled));
}

TEST_CASE("the trunk event carries calls, aliases and a refusal", "[webgate][dv]")
{
    rpc::TrunkStatusInfo status;
    status.enabled = true;
    status.control_vrx = 9;
    rpc::TrunkCallInfo call;
    call.talkgroup = 1201;
    call.followed = true;
    status.calls.push_back(call);
    const std::string json = wg::trunk_event(status, {{1201, "Fire"}});
    CHECK(contains(json, R"({"event":"trunk","enabled":true,"controlVrx":9)"));
    CHECK(contains(json, R"("alias":"Fire")"));
    CHECK(contains(wg::trunk_event({}, {}, "no trunking"), R"("fault":"no trunking")"));
}

TEST_CASE("the desktop's trunk settings are read from its trunk group", "[webgate][dv]")
{
    const auto s = wg::read_desktop_settings(
        [](std::string_view group, std::string_view name) -> std::optional<std::string> {
            if (group != "trunk") {
                return std::nullopt;
            }
            if (name == "aliases") return std::string("100 = Fire\n");
            if (name == "maxFollowers") return std::string("4");
            return std::nullopt;
        });
    CHECK(s.trunk_aliases == "100 = Fire\n");
    CHECK(s.trunk_max_followers == 4);
    CHECK_FALSE(s.trunk_allow.has_value());
}

// ---- The stream layer ---------------------------------------------------------

namespace {

constexpr std::int64_t kCentre = 851'000'000;

struct DvRig {
    revenant::test::Harness harness;
    wg::test::ManualStrand strand;
    wg::test::FakeSocket socket;
    Clock::time_point fake_now = Clock::now();
    std::int64_t wall = 1'700'000'000'000;
    rpc::DetectionList list;
    int trunk_polls = 0;
    rpc::TrunkStatusInfo trunk;
    wg::DesktopSettings desktop;
    // The decoded-message callbacks, by receiver, so a test can play frames.
    std::map<std::uint64_t, std::function<void(const rpc::DecodedMessage&)>> decoders;
    std::unique_ptr<wg::PhoneSession> session;
    std::unique_ptr<wg::PhoneStreams> streams;

    DvRig()
    {
        revenant::test::HarnessOptions o;
        o.center_hz = kCentre;
        const auto opened = harness.open(o);
        REQUIRE(opened.has_value());
        auto client = harness.connect_another();
        REQUIRE(client.has_value());
        session = std::make_unique<wg::PhoneSession>(std::move(*client));
        start();
    }

    void start()
    {
        wg::StreamsConfig config;
        config.clock = [this] { return fake_now; };
        config.wall_clock_ms = [this] { return wall; };
        config.detections = [this](double, double) -> revenant::Expected<rpc::DetectionList> {
            return list;
        };
        config.trunk_status = [this]() -> revenant::Expected<rpc::TrunkStatusInfo> {
            ++trunk_polls;
            return trunk;
        };
        config.desktop_settings = [this] { return desktop; };
        config.subscribe_decoded =
            [this](std::uint64_t vrx, std::string_view, std::function<void(const rpc::DecodedMessage&)> m,
                   std::function<void(const std::string&)>) -> revenant::Expected<std::string> {
            decoders[vrx] = std::move(m);
            return std::string("ok");
        };
        streams = std::make_unique<wg::PhoneStreams>(*session, &socket, strand, config);
        streams->start();
        strand.run_pending();
    }

    void command(std::string_view text)
    {
        streams->on_text(text);
        strand.run_pending();
    }

    void tick(std::chrono::milliseconds step = std::chrono::milliseconds(600))
    {
        fake_now += step;
        streams->tick();
        strand.run_pending();
    }

    [[nodiscard]] std::size_t count(std::string_view needle) const
    {
        std::size_t n = 0;
        for (const auto& m : socket.sent) {
            n += m.kind == Kind::Text && contains(m.text, needle) ? 1U : 0U;
        }
        return n;
    }

    [[nodiscard]] std::string last(std::string_view needle) const
    {
        for (auto it = socket.sent.rbegin(); it != socket.sent.rend(); ++it) {
            if (it->kind == Kind::Text && contains(it->text, needle)) {
                return it->text;
            }
        }
        return {};
    }

    ~DvRig()
    {
        streams.reset();
        session.reset();
    }
};

}  // namespace

TEST_CASE("auto DV opens one receiver per DV signal, never twice, never takes one away",
          "[gpu][webgate][dv]")
{
    REVENANT_NEEDS_GPU();
    DvRig t;
    t.list.detections = {dv(kCentre + 50'000), dv(kCentre + 50'300)};
    t.command(R"({"id":1,"op":"autoDv","args":{"on":true}})");
    CHECK(contains(t.last(R"("id":1,)"), R"("ok":true)"));
    CHECK(contains(t.last(R"("event":"dv")"), R"("autoDv":true)"));
    REQUIRE(t.session->receiver_ids().size() == 1);
    const auto spawned = t.session->receiver_ids().front();
    CHECK(t.session->last_params(spawned)->demod == rpc::Demod::P25p1);

    // A second pass over the same signals adds nothing.
    t.tick();
    CHECK(t.session->receiver_ids().size() == 1);

    // Gone from the air: the receiver stays.
    t.list.detections.clear();
    t.tick();
    CHECK(t.session->has_receiver(spawned));
}

TEST_CASE("a receiver auto DV opens is heard with no receiver selected", "[gpu][webgate][dv][mix]")
{
    REVENANT_NEEDS_GPU();
    DvRig t;
    t.command(R"({"id":1,"op":"audio","args":{"on":true}})");
    CHECK(contains(t.last(R"("id":1,)"), R"("ok":true)"));
    t.list.detections = {dv(kCentre + 50'000)};
    t.command(R"({"id":2,"op":"autoDv","args":{"on":true}})");
    REQUIRE(t.session->receiver_ids().size() == 1);
    const auto spawned = t.session->receiver_ids().front();
    CHECK_FALSE(t.session->audio_vrx().has_value());
    CHECK(t.streams->audio_subscriptions() == std::set<std::uint64_t>{spawned});
}

TEST_CASE("auto DV keeps opening receivers with no view and resumes on a rebuilt stream",
          "[gpu][webgate][dv]")
{
    REVENANT_NEEDS_GPU();
    DvRig t;
    t.command(R"({"id":1,"op":"autoDv","args":{"on":true}})");
    CHECK(t.session->receiver_ids().empty());
    // A locked phone sets no view; the pass runs anyway.
    t.list.detections = {dv(kCentre + 80'000, "DMR")};
    t.tick();
    CHECK(t.session->receiver_ids().size() == 1);

    // A stream rebuilt on the same session (an engine restart rebuilds the
    // session too, carrying the switch) says the switch is on and carries on.
    t.streams.reset();
    t.socket.sent.clear();
    t.start();
    CHECK(contains(t.last(R"("event":"dv")"), R"("autoDv":true)"));
    t.list.detections.push_back(dv(kCentre + 120'000, "D-STAR"));
    t.tick();
    CHECK(t.session->receiver_ids().size() == 2);
}

TEST_CASE("a receiver removed while auto DV is on is not opened again", "[gpu][webgate][dv]")
{
    REVENANT_NEEDS_GPU();
    DvRig t;
    t.list.detections = {dv(kCentre + 50'000)};
    t.command(R"({"id":1,"op":"autoDv","args":{"on":true}})");
    REQUIRE(t.session->receiver_ids().size() == 1);
    const auto vrx = t.session->receiver_ids().front();
    t.command(R"({"id":2,"op":"removeVrx","args":{"vrx":)" + std::to_string(vrx) + "}}");
    t.tick();
    CHECK(t.session->receiver_ids().empty());
}

TEST_CASE("trunk status is polled only while the DV tab is open, and sent on change",
          "[gpu][webgate][dv]")
{
    REVENANT_NEEDS_GPU();
    DvRig t;
    t.tick();
    CHECK(t.trunk_polls == 0);
    t.command(R"({"id":1,"op":"dvTab","args":{"visible":true}})");
    CHECK(t.trunk_polls == 1);
    CHECK(t.count(R"("event":"trunk")") == 1);
    t.tick(std::chrono::milliseconds(1100));
    CHECK(t.trunk_polls == 2);
    // Nothing changed, nothing resent.
    CHECK(t.count(R"("event":"trunk")") == 1);
    t.trunk.tsbks = 40;
    t.tick(std::chrono::milliseconds(1100));
    CHECK(t.count(R"("event":"trunk")") == 2);
    t.command(R"({"id":2,"op":"dvTab","args":{"visible":false}})");
    const int polls = t.trunk_polls;
    t.tick(std::chrono::milliseconds(1100));
    CHECK(t.trunk_polls == polls);
}

TEST_CASE("decoded P25 frames fold into calls and talkgroups with the desktop's aliases",
          "[gpu][webgate][dv]")
{
    REVENANT_NEEDS_GPU();
    DvRig t;
    t.desktop.trunk_aliases = "1201 = Fire Dispatch\n";
    t.command(R"({"id":1,"op":"addVrx","args":{"hz":851050000,"mode":"p25p1"}})");
    t.tick();
    const auto vrx = t.session->receiver_ids().front();
    REQUIRE(t.decoders.contains(vrx));
    t.decoders[vrx](p25(vrx, "hdu", {integer("talkgroup", 1201), integer("source", 7001)}));
    t.strand.run_pending();
    t.tick();
    const std::string calls = t.last(R"("event":"calls")");
    INFO(calls);
    CHECK(contains(calls, R"("alias":"Fire Dispatch")"));
    CHECK(contains(calls, R"("source":"7001")"));
    CHECK(contains(calls, R"("state":"live")"));
    CHECK(contains(calls, R"("frequencyHz":851050000)"));
    CHECK(contains(t.last(R"("event":"talkgroups")"), R"("target":"1201")"));

    // Unchanged, not resent; silence ends the call.
    const auto sent = t.count(R"("event":"calls")");
    t.tick();
    CHECK(t.count(R"("event":"calls")") == sent);
    t.wall += 5'000;
    t.tick();
    CHECK(contains(t.last(R"("event":"calls")"), R"("state":"ended")"));
}

TEST_CASE("the DV ops refuse bad arguments in their own words", "[gpu][webgate][dv]")
{
    REVENANT_NEEDS_GPU();
    DvRig t;
    t.command(R"({"id":1,"op":"autoDv","args":{}})");
    CHECK(contains(t.last(R"("id":1,)"), R"("ok":false)"));
    t.command(R"({"id":2,"op":"trunkFollow","args":{"vrx":1,"on":true,"maxFollowers":40}})");
    CHECK(contains(t.last(R"("id":2,)"), "maxFollowers"));
    t.command(R"({"id":3,"op":"setTrunkRules","args":{"allow":"100, 200","priority":"500=5"}})");
    const std::string reply = t.last(R"("id":3,)");
    INFO(reply);
    CHECK(contains(reply, R"("allow":[100,200])"));
    CHECK(t.session->wanted_trunk_rules().priorities.size() == 1);
}

TEST_CASE("auto DV off lifts only a plugin hold this phone set", "[gpu][webgate][dv]")
{
    REVENANT_NEEDS_GPU();
    DvRig t;
    // Off with nothing held: nothing to lift, so the desktop's hold stands.
    t.command(R"({"id":1,"op":"autoDv","args":{"on":false}})");
    CHECK_FALSE(t.session->plugin_hold());
    t.command(R"({"id":2,"op":"autoDv","args":{"on":true}})");
    CHECK(t.session->plugin_hold());
    t.command(R"({"id":3,"op":"autoDv","args":{"on":false}})");
    CHECK_FALSE(t.session->plugin_hold());
}

TEST_CASE("a session that ends lifts its hold and stops the following it started",
          "[gpu][webgate][dv]")
{
    REVENANT_NEEDS_GPU();
    DvRig t;
    t.command(R"({"id":1,"op":"addVrx","args":{"hz":851050000,"mode":"p25p1"}})");
    const auto vrx = t.session->receiver_ids().front();
    // The poll reads the rig's status, which has to agree it is following.
    t.trunk.enabled = true;
    t.command(R"({"id":2,"op":"trunkFollow","args":{"vrx":)" + std::to_string(vrx) +
              R"(,"on":true,"maxFollowers":3}})");
    const std::string reply = t.last(R"("id":2,)");
    INFO(reply);
    REQUIRE(contains(reply, R"("ok":true)"));
    auto& client = t.session->client();
    REQUIRE(client.trunk_status().has_value());
    CHECK(client.trunk_status()->enabled);
    t.command(R"({"id":3,"op":"autoDv","args":{"on":true}})");
    REQUIRE(t.session->plugin_hold());
    REQUIRE(t.session->trunk_follow().has_value());

    t.streams.reset();
    CHECK_FALSE(t.session->plugin_hold());
    CHECK_FALSE(client.trunk_status()->enabled);
    // The phone's intent outlives the streams, so a rebuilt one asks again.
    t.start();
    CHECK(t.session->plugin_hold());
    CHECK(client.trunk_status()->enabled);
}

TEST_CASE("following held off by auto DV is kept and comes back when it goes off",
          "[gpu][webgate][dv]")
{
    REVENANT_NEEDS_GPU();
    DvRig t;
    t.session->set_trunk_follow(wg::PhoneSession::TrunkFollow{42, 2});
    t.command(R"({"id":1,"op":"autoDv","args":{"on":true}})");
    REQUIRE(t.session->plugin_hold());
    t.trunk.enabled = false;
    t.command(R"({"id":2,"op":"dvTab","args":{"visible":true}})");
    CHECK(t.session->trunk_follow().has_value());

    // Without the hold, an engine that stopped following ends the switch.
    t.command(R"({"id":3,"op":"autoDv","args":{"on":false}})");
    t.tick(std::chrono::milliseconds(1100));
    CHECK_FALSE(t.session->trunk_follow().has_value());
}

TEST_CASE("a detached phone stops the trunk poll until it says dvTab again", "[gpu][webgate][dv]")
{
    REVENANT_NEEDS_GPU();
    DvRig t;
    t.command(R"({"id":1,"op":"dvTab","args":{"visible":true}})");
    t.streams->detach();
    const int polls = t.trunk_polls;
    t.tick(std::chrono::milliseconds(1100));
    CHECK(t.trunk_polls == polls);
}
