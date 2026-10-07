// The call table's rules: when a call starts, carries on and ends on each of
// the digital voice protocols, what a grant nobody followed looks like, and
// what the cap does.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/rpc/types.h"
#include "models/call_log.h"

using revenant::rpc::DecodedField;
using revenant::rpc::DecodedMessage;
using revenant::rpc::DecodedValue;
using revenant::ui::call_frames;
using revenant::ui::CallFrame;
using revenant::ui::CallLog;
using revenant::ui::CallRow;
using revenant::ui::kCallLogCapacity;
using revenant::ui::kCallTimeoutMs;

namespace {

[[nodiscard]] DecodedField integer(std::string key, std::int64_t value)
{
    return DecodedField{std::move(key), DecodedValue{std::in_place_index<0>, value}};
}

[[nodiscard]] DecodedField flag(std::string key, bool value)
{
    return DecodedField{std::move(key), DecodedValue{std::in_place_index<2>, value}};
}

[[nodiscard]] DecodedField text(std::string key, std::string value)
{
    return DecodedField{std::move(key), DecodedValue{std::in_place_index<3>, std::move(value)}};
}

[[nodiscard]] DecodedMessage message(std::uint64_t vrx, std::string decoder, std::string kind,
                                     std::vector<DecodedField> fields)
{
    DecodedMessage out;
    out.vrx = vrx;
    out.decoder = std::move(decoder);
    out.kind = std::move(kind);
    out.fields = std::move(fields);
    return out;
}

void feed(CallLog& log, const DecodedMessage& m, std::int64_t at_ms, std::int64_t hz = 851000000,
          const std::string& owner = "user")
{
    for (const CallFrame& frame : call_frames(m, at_ms, hz, owner)) {
        log.apply(frame);
    }
}

[[nodiscard]] DecodedMessage p25(std::string kind, std::vector<DecodedField> extra = {},
                                 std::uint64_t vrx = 1)
{
    extra.push_back(integer("nac", 0x344));
    return message(vrx, "p25p1", std::move(kind), std::move(extra));
}

}  // namespace

TEST_CASE("a P25 header starts a call and its LDUs refresh the same row")
{
    // Rejects one row per data unit, which would be nine rows a second.
    CallLog log;
    feed(log, p25("hdu", {integer("talkgroup", 1201)}), 1000);
    feed(log, p25("ldu1", {integer("talkgroup", 1201), integer("source", 7001)}), 1180);
    feed(log, p25("ldu2"), 1360);

    REQUIRE(log.calls().size() == 1);
    const CallRow& call = log.calls().front();
    CHECK(call.protocol == "P25");
    CHECK(call.system == "NAC 344");
    CHECK(call.target == "1201");
    CHECK(call.source == "7001");
    CHECK(call.active);
    CHECK(call.first_ms == 1000);
    CHECK(call.last_ms == 1360);
    CHECK(call.frequency_hz == 851000000);
    CHECK(call.owner == "user");
}

TEST_CASE("a P25 terminator ends the call")
{
    // Rejects ending only on the timeout, which keeps a finished call lit for
    // a second and a half after the radio unkeyed.
    CallLog log;
    feed(log, p25("ldu1", {integer("talkgroup", 1201), integer("source", 7001)}), 1000);
    feed(log, p25("tdu"), 1100);
    REQUIRE(log.calls().size() == 1);
    CHECK_FALSE(log.calls().front().active);
    CHECK_FALSE(log.groups().front().active);

    // tdulc the same way.
    feed(log, p25("ldu1", {integer("talkgroup", 1201), integer("source", 7002)}), 2000);
    feed(log, p25("tdulc"), 2100);
    REQUIRE(log.calls().size() == 2);
    CHECK_FALSE(log.calls().front().active);
}

TEST_CASE("a P25 call with no frames for the timeout ends")
{
    // Rejects waiting forever for a terminator lost to a fade.
    CallLog log;
    feed(log, p25("ldu1", {integer("talkgroup", 1201)}), 1000);
    CHECK_FALSE(log.expire(1000 + kCallTimeoutMs));
    CHECK(log.calls().front().active);
    CHECK(log.expire(1001 + kCallTimeoutMs));
    CHECK_FALSE(log.calls().front().active);

    // And a frame after it is a new call, not the old one revived.
    feed(log, p25("ldu1", {integer("talkgroup", 1201)}), 5000);
    CHECK(log.calls().size() == 2);
    CHECK(log.groups().front().count == 2);
}

TEST_CASE("a different source on the same receiver is a new call")
{
    // Rejects folding two talkers into one row when the first's TDU was lost.
    CallLog log;
    feed(log, p25("ldu1", {integer("talkgroup", 1201), integer("source", 7001)}), 1000);
    feed(log, p25("ldu1", {integer("talkgroup", 1201), integer("source", 7002)}), 1200);
    REQUIRE(log.calls().size() == 2);
    CHECK(log.calls().front().source == "7002");
    CHECK_FALSE(log.calls().back().active);
    REQUIRE(log.groups().size() == 1);
    CHECK(log.groups().front().count == 2);
    CHECK(log.groups().front().source == "7002");
}

TEST_CASE("a grant nobody followed shows as granted and not followed")
{
    // Rejects dropping control channel grants, which is the only way to see a
    // talkgroup the tracker is not following.
    CallLog log;
    const auto grant = p25("tsbk", {integer("opcode", 0), integer("group", 2201),
                                    integer("source", 9001),
                                    integer("channel_frequency_hz", 852125000),
                                    flag("encrypted_call", false), flag("emergency", false)},
                           5);
    feed(log, grant, 1000, 853050100, "p25trunk");
    feed(log, grant, 1900, 853050100, "p25trunk");

    REQUIRE(log.calls().size() == 1);
    const CallRow& row = log.calls().front();
    CHECK(row.granted);
    CHECK(row.target == "2201");
    CHECK(row.source == "9001");
    CHECK(row.frequency_hz == 852125000);
    CHECK(row.active);

    // Still granted, and no longer live, once the control channel stops
    // repeating it.
    log.expire(1900 + 3001);
    CHECK_FALSE(log.calls().front().active);
    CHECK(log.calls().front().granted);
}

TEST_CASE("a followed grant becomes the voice call's row")
{
    // Rejects two rows, one granted and one voice, for one transmission.
    CallLog log;
    feed(log,
         p25("tsbk", {integer("group", 2201), integer("source", 9001),
                      integer("channel_frequency_hz", 852125000)},
             5),
         1000);
    feed(log, p25("hdu", {integer("talkgroup", 2201)}, 9), 1200, 0, "p25trunk");

    REQUIRE(log.calls().size() == 1);
    const CallRow& row = log.calls().front();
    CHECK_FALSE(row.granted);
    CHECK(row.vrx == 9);
    CHECK(row.owner == "p25trunk");
    CHECK(row.frequency_hz == 852125000);
    CHECK(log.groups().front().count == 1);

    // A grant repeated during the call adds nothing.
    feed(log, p25("tsbk", {integer("group", 2201)}, 5), 1500);
    CHECK(log.calls().size() == 1);
}

TEST_CASE("a grant update naming two talkgroups grants both")
{
    CallLog log;
    feed(log, p25("tsbk", {integer("group", 2201), integer("group_2", 2202)}, 5), 1000);
    REQUIRE(log.calls().size() == 2);
    CHECK(log.groups().size() == 2);
}

TEST_CASE("DMR keys calls per timeslot, so both slots talk at once")
{
    // Rejects keying on the receiver alone, which would make slot 2's header
    // end slot 1's call.
    CallLog log;
    const auto slot = [](int n, std::int64_t tg, std::int64_t src, std::string kind) {
        return message(3, "dmr", std::move(kind),
                       {integer("slot", n), integer("colour_code", 1), flag("group", true),
                        integer("talkgroup", tg), integer("source", src)});
    };
    feed(log, slot(1, 91, 3100001, "voice_header"), 1000);
    feed(log, slot(2, 3100, 3100002, "voice_header"), 1030);
    feed(log, slot(1, 91, 3100001, "embedded_lc"), 1400);

    REQUIRE(log.calls().size() == 2);
    CHECK(log.calls()[0].slot == 2);
    CHECK(log.calls()[0].system == "CC 1 TS 2");
    CHECK(log.calls()[1].slot == 1);
    CHECK(log.calls()[0].active);
    CHECK(log.calls()[1].active);

    feed(log, message(3, "dmr", "terminator", {integer("slot", 1)}), 1500);
    CHECK(log.calls()[0].active);
    CHECK_FALSE(log.calls()[1].active);
}

TEST_CASE("D-STAR keys on the header's MY and UR, and every header is a new call")
{
    CallLog log;
    const auto header = [](std::string my, std::string ur) {
        return message(4, "dstar", "header",
                       {text("my", std::move(my)), text("my_suffix", "ID51"),
                        text("ur", std::move(ur)), text("rpt1", "KF4FIC B"),
                        integer("voice_frames", 21)});
    };
    feed(log, header("KF4FIC", "CQCQCQ"), 1000);
    feed(log,
         message(4, "dstar", "superframe",
                 {text("my", "KF4FIC"), text("ur", "CQCQCQ"), integer("voice_frames", 21)}),
         1420);
    REQUIRE(log.calls().size() == 1);
    CHECK(log.calls().front().source == "KF4FIC");
    CHECK(log.calls().front().target == "CQCQCQ");
    CHECK(log.calls().front().system == "KF4FIC B");

    // Rejects refreshing on a header: the same callsigns keying again is
    // still a second transmission.
    feed(log, header("KF4FIC", "CQCQCQ"), 1500);
    CHECK(log.calls().size() == 2);

    // A short superframe is the transmission closing.
    feed(log, message(4, "dstar", "superframe", {integer("voice_frames", 7)}), 1700);
    CHECK_FALSE(log.calls().front().active);

    // Stations, not UR, in the groups view.
    REQUIRE(log.groups().size() == 1);
    CHECK(log.groups().front().source == "KF4FIC");
    CHECK(log.groups().front().count == 2);
}

TEST_CASE("M17 starts on an LSF and ends on end of stream")
{
    CallLog log;
    feed(log,
         message(6, "m17", "lsf",
                 {text("source", "N0CALL"), text("destination", "ALL"),
                  flag("encrypted", false)}),
         1000);
    REQUIRE(log.calls().size() == 1);
    CHECK(log.calls().front().protocol == "M17");
    CHECK(log.calls().front().active);
    feed(log, message(6, "m17", "eot", {}), 1500);
    CHECK_FALSE(log.calls().front().active);
}

TEST_CASE("encrypted marks the row and stays marked for the call")
{
    // Rejects an LDU that says nothing about encryption clearing the mark
    // the header set.
    CallLog log;
    feed(log, p25("hdu", {integer("talkgroup", 1201), flag("encrypted", true)}), 1000);
    feed(log, p25("ldu1", {integer("talkgroup", 1201)}), 1180);
    feed(log, p25("ldu2", {flag("encrypted", false)}), 1360);
    CHECK(log.calls().front().encrypted);
    CHECK(log.groups().front().encrypted);

    // DMR privacy and a PI header mark it the same way.
    feed(log,
         message(3, "dmr", "voice_header",
                 {integer("slot", 1), integer("talkgroup", 91), flag("privacy", true)}),
         2000);
    CHECK(log.calls().front().encrypted);
    feed(log, message(3, "dmr", "pi_header", {integer("slot", 2), integer("talkgroup", 92)}), 2000);
    CHECK(log.calls().front().encrypted);
}

TEST_CASE("messages that are not calls say nothing")
{
    CallLog log;
    feed(log, p25("tsbk", {integer("opcode", 0x3A)}), 1000);
    feed(log, message(3, "dmr", "csbk", {integer("slot", 1)}), 1000);
    feed(log, message(1, "rtty", "line", {}), 1000);
    feed(log, p25("tdu"), 1000);
    CHECK(log.calls().empty());
    CHECK(log.groups().empty());
}

TEST_CASE("the call list holds the newest calls up to its cap")
{
    // Rejects an unbounded list on a busy trunk, and dropping the newest
    // rather than the oldest.
    CallLog log;
    for (std::size_t i = 0; i < kCallLogCapacity + 25; ++i) {
        const auto at = static_cast<std::int64_t>(i) * 10;
        feed(log,
             p25("ldu1", {integer("talkgroup", 1201),
                          integer("source", static_cast<std::int64_t>(7000 + i))}),
             at);
    }
    REQUIRE(log.calls().size() == kCallLogCapacity);
    CHECK(log.calls().front().source == std::to_string(7000 + kCallLogCapacity + 24));
    CHECK(log.calls().back().source == "7025");
    REQUIRE(log.groups().size() == 1);
    CHECK(log.groups().front().count == kCallLogCapacity + 25);
}
