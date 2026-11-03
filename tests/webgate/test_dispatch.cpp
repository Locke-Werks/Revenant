// Phone command dispatch against the in-process engine of
// tests/rpc/rpc_fixture.h. Each case asserts the engine-side effect, read back
// through a client the dispatcher did not use, not the reply text alone.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/error.h"
#include "core/rpc/client.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"
#include "tools/webgate/dispatch.h"
#include "tools/webgate/protocol.h"
#include "tools/webgate/session.h"

namespace {

using revenant::webgate::dispatch;
using revenant::webgate::handle_text;
using revenant::webgate::PhoneSession;

constexpr std::int64_t kCentre = 100'000'000;

[[nodiscard]] revenant::test::HarnessOptions options()
{
    revenant::test::HarnessOptions o;
    o.center_hz = kCentre;
    return o;
}

[[nodiscard]] std::unique_ptr<PhoneSession> phone(revenant::test::Harness& harness)
{
    auto client = harness.connect_another();
    INFO(revenant::test::message_of(client));
    REQUIRE(client.has_value());
    return std::make_unique<PhoneSession>(std::move(*client));
}

[[nodiscard]] std::string send(PhoneSession& session, std::string_view text)
{
    return handle_text(session, text);
}

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Pulls the number after "\"vrx\":" out of a reply.
[[nodiscard]] std::uint64_t vrx_of(const std::string& reply)
{
    const auto at = reply.find("\"vrx\":");
    REQUIRE(at != std::string::npos);
    return std::stoull(reply.substr(at + 6));
}

}  // namespace

TEST_CASE("addVrx creates a receiver the session owns", "[gpu][webgate][dispatch]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    REQUIRE(harness.open(options()).has_value());
    auto session = phone(harness);

    const auto reply =
        send(*session, R"({"id":1,"op":"addVrx","args":{"hz":100100000,"mode":"am"}})");
    INFO(reply);
    REQUIRE(contains(reply, R"("ok":true)"));
    const auto vrx = vrx_of(reply);

    CHECK(session->owns(vrx));
    const auto status = session->client().vrx_status(vrx);
    REQUIRE(status.has_value());
    CHECK(status->params.center == 100'000);
    CHECK(status->params.demod == revenant::rpc::Demod::Am);
    CHECK(status->owned_by_caller);
    CHECK(contains(reply, R"("hz":100100000)"));
    CHECK(contains(reply, R"("mode":"am")"));
}

TEST_CASE("tune moves the receiver in absolute hertz", "[gpu][webgate][dispatch]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    REQUIRE(harness.open(options()).has_value());
    auto session = phone(harness);

    const auto added = send(*session, R"({"id":1,"op":"addVrx","args":{"hz":100050000}})");
    const auto vrx = vrx_of(added);

    const auto reply = send(
        *session, R"({"id":2,"op":"tune","args":{"vrx":)" + std::to_string(vrx) +
                      R"(,"hz":100200000}})");
    INFO(reply);
    REQUIRE(contains(reply, R"("id":2)"));
    REQUIRE(contains(reply, R"("ok":true)"));

    const auto status = harness.client().vrx_status(vrx);
    REQUIRE(status.has_value());
    CHECK(status->params.center == 200'000);
}

TEST_CASE("setMode recreates the receiver in the new mode", "[gpu][webgate][dispatch]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    REQUIRE(harness.open(options()).has_value());
    auto session = phone(harness);

    const auto old_vrx = vrx_of(send(*session, R"({"id":1,"op":"addVrx","args":{"hz":100100000}})"));
    REQUIRE(contains(send(*session, R"({"id":2,"op":"audio","args":{"vrx":)" +
                                        std::to_string(old_vrx) + "}}"),
                     R"("ok":true)"));

    const auto reply = send(*session, R"({"id":3,"op":"setMode","args":{"vrx":)" +
                                          std::to_string(old_vrx) + R"(,"mode":"usb"}})");
    INFO(reply);
    REQUIRE(contains(reply, R"("ok":true)"));
    const auto fresh = vrx_of(reply);
    CHECK(fresh != old_vrx);
    CHECK(contains(reply, "\"replaced\":" + std::to_string(old_vrx)));

    // The engine's rule is that a demodulator is a stage: one receiver goes
    // and another comes, at the same frequency.
    CHECK_FALSE(session->owns(old_vrx));
    CHECK_FALSE(harness.client().vrx_status(old_vrx).has_value());
    CHECK(session->audio_vrx() == fresh);
    const auto status = harness.client().vrx_status(fresh);
    REQUIRE(status.has_value());
    CHECK(status->params.demod == revenant::rpc::Demod::Usb);
    CHECK(status->params.center == 100'000);

    // Same mode again changes nothing.
    const auto same = send(*session, R"({"id":4,"op":"setMode","args":{"vrx":)" +
                                         std::to_string(fresh) + R"(,"mode":"usb"}})");
    CHECK(vrx_of(same) == fresh);
    CHECK_FALSE(contains(same, "replaced"));
}

TEST_CASE("setBandwidth recreates the receiver and setSquelch changes it in place",
          "[gpu][webgate][dispatch]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    REQUIRE(harness.open(options()).has_value());
    auto session = phone(harness);

    const auto first = vrx_of(send(*session, R"({"id":1,"op":"addVrx","args":{"hz":100100000}})"));

    const auto bw = send(*session, R"({"id":3,"op":"setBandwidth","args":{"vrx":)" +
                                       std::to_string(first) + R"(,"hz":3000}})");
    INFO(bw);
    REQUIRE(contains(bw, R"("ok":true)"));
    const auto vrx = vrx_of(bw);
    CHECK(vrx != first);
    CHECK(contains(bw, "\"replaced\":" + std::to_string(first)));

    const auto sq = send(*session, R"({"id":4,"op":"setSquelch","args":{"vrx":)" +
                                       std::to_string(vrx) + R"(,"dbfs":-60}})");
    INFO(sq);
    REQUIRE(contains(sq, R"("ok":true)"));
    CHECK(vrx_of(sq) == vrx);

    const auto status = harness.client().vrx_status(vrx);
    REQUIRE(status.has_value());
    CHECK(status->params.bandwidth == 3000);
    CHECK(status->params.squelch_dbfs == -60.0);
    // Untouched by the two above.
    CHECK(status->params.demod == revenant::rpc::Demod::Nfm);
    CHECK(status->params.center == 100'000);
    CHECK_FALSE(harness.client().vrx_status(first).has_value());
}

TEST_CASE("removeVrx removes an owned receiver and refuses one that is not", "[gpu][webgate][dispatch]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    REQUIRE(harness.open(options()).has_value());
    auto session = phone(harness);

    const auto other = harness.client().add_vrx(revenant::rpc::VrxParams{.center = 50'000});
    REQUIRE(other.has_value());

    const auto mine = vrx_of(send(*session, R"({"id":1,"op":"addVrx","args":{"hz":100100000}})"));
    const auto removed =
        send(*session, R"({"id":2,"op":"removeVrx","args":{"vrx":)" + std::to_string(mine) + "}}");
    REQUIRE(contains(removed, R"("ok":true)"));
    CHECK_FALSE(session->owns(mine));
    CHECK_FALSE(harness.client().vrx_status(mine).has_value());

    const auto refused = send(
        *session, R"({"id":3,"op":"removeVrx","args":{"vrx":)" + std::to_string(*other) + "}}");
    INFO(refused);
    CHECK(contains(refused, R"("ok":false)"));
    CHECK(harness.client().vrx_status(*other).has_value());
}

TEST_CASE("a phone leaving removes its receivers and only its receivers",
          "[gpu][webgate][dispatch]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    REQUIRE(harness.open(options()).has_value());

    const auto other = harness.client().add_vrx(revenant::rpc::VrxParams{.center = 50'000});
    REQUIRE(other.has_value());

    std::uint64_t mine = 0;
    {
        auto first = phone(harness);
        auto second = phone(harness);
        mine = vrx_of(send(*first, R"({"id":1,"op":"addVrx","args":{"hz":100100000}})"));
        const auto theirs =
            vrx_of(send(*second, R"({"id":1,"op":"addVrx","args":{"hz":100150000}})"));
        CHECK(first->owns(mine));
        CHECK_FALSE(first->owns(theirs));
        // A phone cannot drive another phone's receiver.
        const auto reply = send(
            *first, R"({"id":2,"op":"tune","args":{"vrx":)" + std::to_string(theirs) +
                        R"(,"hz":100160000}})");
        CHECK(contains(reply, R"("ok":false)"));

        second.reset();
        const auto ids = harness.client().vrx_ids();
        REQUIRE(ids.has_value());
        CHECK(std::ranges::find(*ids, theirs) == ids->end());
        CHECK(std::ranges::find(*ids, mine) != ids->end());
    }

    const auto ids = harness.client().vrx_ids();
    REQUIRE(ids.has_value());
    CHECK(std::ranges::find(*ids, mine) == ids->end());
    CHECK(std::ranges::find(*ids, *other) != ids->end());
}

TEST_CASE("a refused command returns the engine's sentence", "[gpu][webgate][dispatch]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    REQUIRE(harness.open(options()).has_value());
    auto session = phone(harness);

    const auto vrx = vrx_of(send(*session, R"({"id":1,"op":"addVrx","args":{"hz":100100000}})"));

    // Far outside the half-rate span the source holds.
    const auto reply = send(
        *session, R"({"id":2,"op":"tune","args":{"vrx":)" + std::to_string(vrx) +
                      R"(,"hz":900000000}})");
    INFO(reply);
    REQUIRE(contains(reply, R"("ok":false)"));

    auto params = session->client().vrx_status(vrx)->params;
    params.center = 900'000'000 - kCentre;
    const auto direct = session->client().set_vrx_params(vrx, params);
    REQUIRE_FALSE(direct.has_value());
    CHECK(reply == revenant::webgate::error_reply(2, direct.error().message));
}

TEST_CASE("bad arguments are refused naming the argument", "[gpu][webgate][dispatch]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    REQUIRE(harness.open(options()).has_value());
    auto session = phone(harness);

    const auto missing = send(*session, R"({"id":5,"op":"addVrx","args":{}})");
    CHECK(contains(missing, R"("id":5)"));
    CHECK(contains(missing, R"("ok":false)"));
    CHECK(contains(missing, "hz"));

    const auto wrong = send(*session, R"({"id":6,"op":"tune","args":{"vrx":"x","hz":1}})");
    CHECK(contains(wrong, R"("ok":false)"));
    CHECK(contains(wrong, "vrx"));

    const auto mode = send(*session, R"({"id":7,"op":"addVrx","args":{"hz":100100000,"mode":"zz"}})");
    CHECK(contains(mode, R"("ok":false)"));
    CHECK(contains(mode, "mode"));

    // None of that reached the engine.
    CHECK(session->owned_ids().empty());
    const auto ids = harness.client().vrx_ids();
    REQUIRE(ids.has_value());
    CHECK(ids->empty());

    // A command parse_command refuses still gets a reply when it had an id.
    const auto unknown = send(*session, R"({"id":9,"op":"nope"})");
    CHECK(contains(unknown, R"("id":9)"));
    CHECK(contains(unknown, R"("ok":false)"));
    CHECK(send(*session, "not json").empty());
}

TEST_CASE("setCenter reports the centre the source took", "[gpu][webgate][dispatch]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    auto o = options();
    o.retunable = true;
    REQUIRE(harness.open(o).has_value());
    auto session = phone(harness);

    const auto reply = send(*session, R"({"id":1,"op":"setCenter","args":{"hz":100010000}})");
    INFO(reply);
    REQUIRE(contains(reply, R"("ok":true)"));
    CHECK(contains(reply, R"("grantedHz":100010000)"));
    const auto info = harness.client().info();
    REQUIRE(info.has_value());
    CHECK(info->source_center == 100'010'000);
}

TEST_CASE("setCenter on a source that cannot retune returns the source's words",
          "[gpu][webgate][dispatch]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    REQUIRE(harness.open(options()).has_value());
    auto session = phone(harness);

    const auto reply = send(*session, R"({"id":1,"op":"setCenter","args":{"hz":100010000}})");
    const auto direct = session->client().set_source_center(100'010'000);
    REQUIRE_FALSE(direct.has_value());
    CHECK(reply == revenant::webgate::error_reply(1, direct.error().message));
}

TEST_CASE("setGain and setGainAuto reach the source", "[gpu][webgate][dispatch]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    REQUIRE(harness.open(options()).has_value());
    auto session = phone(harness);

    // A synthetic scene has no gain stages, so the engine refuses; what is
    // checked is that its sentence comes through unchanged.
    const auto gain = send(*session, R"({"id":1,"op":"setGain","args":{"stage":"LNA","db":10}})");
    const auto direct = session->client().set_source_gain("LNA", 10.0);
    if (direct.has_value()) {
        CHECK(contains(gain, R"("ok":true)"));
    } else {
        CHECK(gain == revenant::webgate::error_reply(1, direct.error().message));
    }

    const auto autoed =
        send(*session, R"({"id":2,"op":"setGainAuto","args":{"stage":"LNA","on":true}})");
    CHECK(contains(autoed, R"("id":2)"));
    const auto bad = send(*session, R"({"id":3,"op":"setGainAuto","args":{"stage":"LNA"}})");
    CHECK(contains(bad, "on"));
    CHECK(contains(bad, R"("ok":false)"));
}

TEST_CASE("listSources lists what the engine lists", "[gpu][webgate][dispatch]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    REQUIRE(harness.open(options()).has_value());
    auto session = phone(harness);

    const auto reply = send(*session, R"({"id":1,"op":"listSources"})");
    INFO(reply);
    REQUIRE(contains(reply, R"("ok":true)"));
    CHECK(contains(reply, R"("sources":[)"));
    const auto direct = session->client().list_sources();
    REQUIRE(direct.has_value());
    for (const auto& source : *direct) {
        CHECK(contains(reply, revenant::webgate::json_string(source.uri)));
    }
}

TEST_CASE("openSource switches from an open source", "[gpu][webgate][dispatch]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    REQUIRE(harness.open(options()).has_value());
    auto session = phone(harness);

    // A source is already open: the phone switches radios in one command,
    // which closes the old one first.
    const std::string uri =
        "synthetic:wideband?rate=1200000&emitters=1&modes=am&seed=77&samples=400000";
    const auto reply =
        send(*session, R"({"id":1,"op":"openSource","args":{"uri":")" + uri + R"("}})");
    INFO(reply);
    REQUIRE(contains(reply, R"("ok":true)"));

    const auto descriptor = harness.client().source_descriptor();
    REQUIRE(descriptor.has_value());
    REQUIRE(descriptor->has_value());
    CHECK((*descriptor)->uri == uri);
    CHECK(contains(reply, revenant::webgate::json_string(uri)));
    const auto info = harness.client().info();
    REQUIRE(info.has_value());
    CHECK(info->source_rate == 1'200'000);
}

TEST_CASE("setTranscription flips the engine's switch", "[gpu][webgate][dispatch]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    REQUIRE(harness.open(options()).has_value());
    auto session = phone(harness);

    const auto reply =
        send(*session, R"({"id":1,"op":"setTranscription","args":{"on":true}})");
    INFO(reply);
    const auto status = harness.client().transcription_status();
    REQUIRE(status.has_value());
    if (status->enabled) {
        CHECK(contains(reply, R"("ok":true)"));
        CHECK(contains(reply, R"("enabled":true)"));
        const auto off = send(*session, R"({"id":2,"op":"setTranscription","args":{"on":false}})");
        CHECK(contains(off, R"("enabled":false)"));
        CHECK_FALSE(harness.client().transcription_status()->enabled);
    } else {
        // An engine without a recogniser says so in its own words.
        CHECK(contains(reply, R"("ok":false)"));
    }
}

TEST_CASE("setVrxTranscribe sets the receiver's choice", "[gpu][webgate][dispatch]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    REQUIRE(harness.open(options()).has_value());
    auto session = phone(harness);

    const auto vrx = vrx_of(send(*session, R"({"id":1,"op":"addVrx","args":{"hz":100100000}})"));
    const auto reply = send(*session, R"({"id":2,"op":"setVrxTranscribe","args":{"vrx":)" +
                                          std::to_string(vrx) + R"(,"choice":"off"}})");
    INFO(reply);
    REQUIRE(contains(reply, R"("ok":true)"));
    const auto status = harness.client().vrx_status(vrx);
    REQUIRE(status.has_value());
    CHECK(status->transcribe == revenant::rpc::TranscribeChoice::Off);
}

TEST_CASE("setView, audio and keepListening record state for the stream layer",
          "[gpu][webgate][dispatch]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    REQUIRE(harness.open(options()).has_value());
    auto session = phone(harness);

    REQUIRE(contains(
        send(*session,
             R"({"id":1,"op":"setView","args":{"centerHz":100000000,"spanHz":200000,"widthPx":600}})"),
        R"("ok":true)"));
    REQUIRE(session->view().has_value());
    CHECK(session->view()->center_hz == 100'000'000.0);
    CHECK(session->view()->span_hz == 200'000.0);
    CHECK(session->view()->width_px == 600);

    CHECK(contains(
        send(*session, R"({"id":2,"op":"setView","args":{"centerHz":1,"spanHz":0,"widthPx":600}})"),
        "spanHz"));
    CHECK(contains(
        send(*session, R"({"id":3,"op":"setView","args":{"centerHz":1,"spanHz":1,"widthPx":0}})"),
        "widthPx"));
    CHECK(session->view()->width_px == 600);

    const auto vrx = vrx_of(send(*session, R"({"id":4,"op":"addVrx","args":{"hz":100100000}})"));
    REQUIRE(contains(send(*session, R"({"id":5,"op":"audio","args":{"vrx":)" +
                                        std::to_string(vrx) + "}}"),
                     R"("ok":true)"));
    CHECK(session->audio_vrx() == vrx);
    REQUIRE(contains(send(*session, R"({"id":6,"op":"audio","args":{"vrx":)" +
                                        std::to_string(vrx) + R"(,"on":false}})"),
                     R"("ok":true)"));
    CHECK_FALSE(session->audio_vrx().has_value());
    // Not a receiver of this phone's.
    CHECK(contains(send(*session, R"({"id":7,"op":"audio","args":{"vrx":999999}})"),
                   R"("ok":false)"));

    REQUIRE(contains(send(*session, R"({"id":8,"op":"keepListening","args":{"on":true}})"),
                     R"("ok":true)"));
    CHECK(session->keep_listening());
    CHECK(contains(send(*session, R"({"id":9,"op":"keepListening","args":{}})"), R"("ok":false)"));
}

TEST_CASE("json_number formats like add_double and nulls what JSON cannot spell", "[webgate][dispatch]")
{
    CHECK(revenant::webgate::json_number(1.5) == "1.5");
    CHECK(revenant::webgate::json_number(-60.0) == "-60");
    CHECK(revenant::webgate::json_number(std::nan("")) == "null");
}

TEST_CASE("a recreate whose old receiver will not go is rolled back", "[gpu][webgate][dispatch]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    REQUIRE(harness.open(options()).has_value());
    auto session = phone(harness);

    const auto old_vrx = vrx_of(send(*session, R"({"id":1,"op":"addVrx","args":{"hz":100100000}})"));
    REQUIRE(contains(send(*session, R"({"id":2,"op":"audio","args":{"vrx":)" +
                                        std::to_string(old_vrx) + "}}"),
                     R"("ok":true)"));
    REQUIRE(contains(send(*session, R"({"id":3,"op":"keepListening","args":{"on":true}})"),
                     R"("ok":true)"));
    session->test_hooks().remove_vrx = [old_vrx](std::uint64_t id) -> revenant::Status {
        if (id == old_vrx) {
            return revenant::fail("injected remove failure");
        }
        return {};
    };

    const auto reply = send(*session, R"({"id":4,"op":"setBandwidth","args":{"vrx":)" +
                                          std::to_string(old_vrx) + R"(,"hz":3000}})");
    INFO(reply);
    CHECK(contains(reply, R"("ok":false)"));
    CHECK(contains(reply, "injected remove failure"));

    CHECK(session->owned_ids() == std::vector<std::uint64_t>{old_vrx});
    CHECK(session->audio_vrx() == old_vrx);
    CHECK(session->keep_listening());
    const auto ids = harness.client().vrx_ids();
    REQUIRE(ids.has_value());
    CHECK(*ids == std::vector<std::uint64_t>{old_vrx});
}

TEST_CASE("a recreate the engine refuses leaves the old receiver alone", "[gpu][webgate][dispatch]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    REQUIRE(harness.open(options()).has_value());
    auto session = phone(harness);

    const auto old_vrx = vrx_of(send(*session, R"({"id":1,"op":"addVrx","args":{"hz":100100000}})"));
    REQUIRE(contains(send(*session, R"({"id":2,"op":"audio","args":{"vrx":)" +
                                        std::to_string(old_vrx) + "}}"),
                     R"("ok":true)"));

    // Compared against the engine's own refusal of the same receiver.
    auto params = session->client().vrx_status(old_vrx)->params;
    params.bandwidth = -5;
    params.passband_low = 0;
    params.passband_high = 0;
    const auto direct = harness.client().add_vrx(params);
    REQUIRE_FALSE(direct.has_value());

    const auto reply = send(*session, R"({"id":3,"op":"setBandwidth","args":{"vrx":)" +
                                          std::to_string(old_vrx) + R"(,"hz":-5}})");
    INFO(reply);
    CHECK(reply == revenant::webgate::error_reply(3, direct.error().message));
    CHECK(session->owned_ids() == std::vector<std::uint64_t>{old_vrx});
    CHECK(session->audio_vrx() == old_vrx);
    const auto ids = harness.client().vrx_ids();
    REQUIRE(ids.has_value());
    CHECK(*ids == std::vector<std::uint64_t>{old_vrx});
}

TEST_CASE("a status read failing after the change still reports the new id",
          "[gpu][webgate][dispatch]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    REQUIRE(harness.open(options()).has_value());
    auto session = phone(harness);
    session->test_hooks().vrx_status = [](std::uint64_t) -> revenant::Expected<revenant::rpc::VrxStatus> {
        return revenant::fail("injected status failure");
    };

    const auto added = send(*session, R"({"id":1,"op":"addVrx","args":{"hz":100100000}})");
    INFO(added);
    REQUIRE(contains(added, R"("ok":true)"));
    CHECK(contains(added, R"("statusUnavailable":true)"));
    const auto first = vrx_of(added);
    CHECK(session->owns(first));

    const auto changed = send(*session, R"({"id":2,"op":"setBandwidth","args":{"vrx":)" +
                                            std::to_string(first) + R"(,"hz":3000}})");
    INFO(changed);
    REQUIRE(contains(changed, R"("ok":true)"));
    CHECK(contains(changed, "\"replaced\":" + std::to_string(first)));
    const auto fresh = vrx_of(changed);
    CHECK(fresh != first);
    CHECK(session->owns(fresh));
    CHECK(contains(changed, R"("bandwidthHz":3000)"));
}

TEST_CASE("a recreate carries the receiver's transcribe choice", "[gpu][webgate][dispatch]")
{
    REVENANT_NEEDS_GPU();
    revenant::test::Harness harness;
    REQUIRE(harness.open(options()).has_value());
    auto session = phone(harness);

    const auto first = vrx_of(send(*session, R"({"id":1,"op":"addVrx","args":{"hz":100100000}})"));
    REQUIRE(contains(send(*session, R"({"id":2,"op":"setVrxTranscribe","args":{"vrx":)" +
                                        std::to_string(first) + R"(,"choice":"off"}})"),
                     R"("ok":true)"));
    const auto changed = send(*session, R"({"id":3,"op":"setBandwidth","args":{"vrx":)" +
                                            std::to_string(first) + R"(,"hz":3000}})");
    INFO(changed);
    REQUIRE(contains(changed, R"("ok":true)"));
    const auto status = harness.client().vrx_status(vrx_of(changed));
    REQUIRE(status.has_value());
    CHECK(status->transcribe == revenant::rpc::TranscribeChoice::Off);
}
