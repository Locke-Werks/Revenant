// The programme type name's corrected mark, the Emergency Warning System and
// the Traffic Message Channel, as models/rds_services.h renders them.
//
// EVERY TEST HERE NAMES THE WRONG IMPLEMENTATION IT REJECTS, in its own
// comment, for the reason test_audio_ring.cpp gives.
//
// The wrong implementations on this path are a marked renderer that drifts
// from the unmarked one, so the same name reads differently depending on
// whether a block was corrected; an emergency warning left as a count on
// hover; and a TMC list that shows payloads heard once or names fields whose
// layout nobody here has read.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "core/rpc/types.h"
#include "models/rds_services.h"
#include "models/rds_text.h"

using revenant::rpc::RdsStation;
using revenant::rpc::RdsTmcMessage;
using revenant::ui::kTmcPayloadsListed;
using revenant::ui::make_ews_view;
using revenant::ui::make_ptyn_view;
using revenant::ui::make_tmc_view;
using revenant::ui::RdsRun;
using revenant::ui::render_rds_runs;
using revenant::ui::render_rds_text;

namespace {

[[nodiscard]] std::vector<std::uint8_t> bytes_of(const char* ascii)
{
    std::vector<std::uint8_t> out;
    for (const char* p = ascii; *p != '\0'; ++p) {
        out.push_back(static_cast<std::uint8_t>(*p));
    }
    return out;
}

[[nodiscard]] std::string joined(const std::vector<RdsRun>& runs)
{
    std::string out;
    for (const RdsRun& run : runs) {
        out += run.text;
    }
    return out;
}

// U+00B7, the placeholder, in UTF-8.
const std::string kDot = "\xC2\xB7";

[[nodiscard]] RdsTmcMessage payload(std::uint8_t x, std::uint16_t y, std::uint16_t z,
                                    std::uint32_t receptions, std::uint32_t corrected = 0)
{
    RdsTmcMessage m;
    m.x = x;
    m.y = y;
    m.z = z;
    m.receptions = receptions;
    m.corrected_receptions = corrected;
    return m;
}

}  // namespace

TEST_CASE("the marked runs read exactly as the unmarked text does", "[rds]")
{
    // REJECTS a second renderer that trims, places or transcodes differently
    // from render_rds_text, so that a corrected block changes what the name
    // says rather than only how it is drawn. Every received and corrected
    // mask over a PTYN with a trailing space, an inner space and an Annex E
    // byte, and over a RadioText in 2B segments.
    std::vector<std::uint8_t> ptyn = bytes_of("Ro k Ab ");
    ptyn[5] = 0x91;  // Annex E a-umlaut
    for (std::uint32_t received = 0; received < 4; ++received) {
        for (std::uint32_t corrected = 0; corrected < 4; ++corrected) {
            INFO("received " << received << " corrected " << corrected);
            CHECK(joined(render_rds_runs(ptyn, received, corrected, 4, 8)) ==
                  render_rds_text(ptyn, received, 4, 8).text);
        }
    }
    const std::vector<std::uint8_t> rt = bytes_of("NEWS AT SIX    ");
    for (std::uint32_t received = 0; received < 256; received += 7) {
        for (std::uint32_t corrected : {0U, 0x05U, 0xFFU}) {
            CHECK(joined(render_rds_runs(rt, received, corrected, 2, rt.size())) ==
                  render_rds_text(rt, received, 2, rt.size()).text);
        }
    }
}

TEST_CASE("a corrected segment is its own run and a placeholder is never marked", "[rds]")
{
    // REJECTS marking by whole field, which would underline "ROCK" because
    // "CLAS" was repaired, and marking a placeholder, which doubts a
    // character that never arrived.
    const std::vector<std::uint8_t> ptyn = bytes_of("CLASROCK");
    const auto runs = render_rds_runs(ptyn, 0x03, 0x01, 4, 8);
    REQUIRE(runs.size() == 2);
    CHECK(runs[0] == RdsRun{"CLAS", true});
    CHECK(runs[1] == RdsRun{"ROCK", false});

    // Second segment unreceived but flagged: four placeholders, unmarked.
    const auto half = render_rds_runs(ptyn, 0x01, 0x03, 4, 8);
    REQUIRE(half.size() == 2);
    CHECK(half[0] == RdsRun{"CLAS", true});
    CHECK(half[1] == RdsRun{kDot + kDot + kDot + kDot, false});
}

TEST_CASE("the programme type name counts what is corrected among what arrived", "[rds]")
{
    // REJECTS counting the corrected bits alone. A bit set on a segment that
    // has not arrived marks nothing on screen, so a hover saying a segment
    // was repaired would describe placeholders.
    RdsStation station;
    station.ptyn = bytes_of("JAZZ    ");
    station.ptyn_received = 0x01;
    station.ptyn_corrected = 0x02;
    auto view = make_ptyn_view(station);
    CHECK_FALSE(view.empty());
    CHECK(view.segments_received == 1);
    CHECK(view.segments_total == 2);
    CHECK(view.segments_corrected == 0);
    CHECK(view.note == "1/2 segments");
    CHECK(view.corrected_detail.empty());
    CHECK(joined(view.runs) == "JAZZ" + kDot + kDot + kDot + kDot);

    station.ptyn = bytes_of("JAZZ FM ");
    station.ptyn_received = 0x03;
    station.ptyn_corrected = 0x02;
    view = make_ptyn_view(station);
    CHECK(view.segments_corrected == 1);
    CHECK(view.note.empty());
    CHECK_FALSE(view.corrected_detail.empty());
    REQUIRE(view.runs.size() == 2);
    CHECK(view.runs[0] == RdsRun{"JAZZ", false});
    // The segment's leading space belongs to it and is marked with it; the
    // trailing padding is trimmed as render_rds_text trims it.
    CHECK(view.runs[1] == RdsRun{" FM", true});

    // Nothing received is empty, which is not the same as eight spaces.
    RdsStation none;
    CHECK(make_ptyn_view(none).empty());
    none.ptyn = bytes_of("        ");
    none.ptyn_received = 0x03;
    CHECK_FALSE(make_ptyn_view(none).empty());
    CHECK(make_ptyn_view(none).runs.empty());
}

TEST_CASE("an emergency warning group is a chip in the row, with the payload on hover",
          "[rds]")
{
    // REJECTS an EWS surfaced only as a count in a tooltip somewhere, and a
    // payload rendered as anything but hexadecimal. No groups is nothing.
    RdsStation station;
    CHECK_FALSE(make_ews_view(station).sent);
    CHECK(make_ews_view(station).label.empty());

    station.ews.groups = 3;
    station.ews.group_type = 9;
    station.ews.block2_low = 0x1A;
    station.ews.block3 = 0xBEEF;
    station.ews.block3_valid = true;
    station.ews.block4 = 0x0123;
    station.ews.block4_valid = false;
    station.ews.corrected = true;
    station.ews_channel_identification = 0x5A5;
    station.ews_channel_identification_valid = true;
    const auto view = make_ews_view(station);
    CHECK(view.sent);
    CHECK(view.label == "emergency warning");
    CHECK(view.detail.find("3 type 9A groups") != std::string::npos);
    CHECK(view.detail.find("block 2 bits 1A") != std::string::npos);
    CHECK(view.detail.find("block 3 BEEF") != std::string::npos);
    // Block 4 did not arrive, so its stale bits are not shown as if it had.
    CHECK(view.detail.find("block 4 lost") != std::string::npos);
    CHECK(view.detail.find("0123") == std::string::npos);
    CHECK(view.detail.find("corrected block") != std::string::npos);
    CHECK(view.detail.find("identification 5A5") != std::string::npos);

    station.ews.groups = 1;
    CHECK(make_ews_view(station).detail.find("1 type 9A group,") != std::string::npos);
}

TEST_CASE("TMC shows whether a service is on air and lists only confirmed payloads",
          "[rds]")
{
    // REJECTS listing a payload heard once, which ISO 14819-1 says not to
    // use, and any label on the bits. Nothing and no announcement is not
    // shown at all.
    RdsStation station;
    CHECK_FALSE(make_tmc_view(station).shown);

    station.tmc.announced = true;
    station.tmc.aid = 0xCD46;
    station.tmc.group_type = 8;
    auto view = make_tmc_view(station);
    CHECK(view.shown);
    CHECK_FALSE(view.on_air);
    CHECK(view.label == "TMC announced");
    CHECK(view.detail.find("AID CD46 on group 8A") != std::string::npos);

    station.tmc.groups = 40;
    station.tmc.oda_groups = 40;
    station.tmc.incomplete = 2;
    station.tmc.identification = 0x0A5;
    station.tmc.identification_valid = true;
    station.tmc.messages = {
        payload(0x01, 0x1111, 0x2222, 1),
        payload(0x1F, 0xABCD, 0x0001, 5, 1),
        payload(0x02, 0x0000, 0xFFFF, 2),
        payload(0x02, 0x0000, 0x0001, 2),
    };
    view = make_tmc_view(station);
    CHECK(view.on_air);
    CHECK(view.label == "TMC");
    CHECK(view.counts == "40 groups  \xC2\xB7  3 confirmed  \xC2\xB7  1 heard once");
    REQUIRE(view.payloads.size() == 3);
    // Most received first, then by value: stable across polls although the
    // wire gives the table in no order.
    CHECK(view.payloads[0] == "1F ABCD 0001  5x, 1 corrected");
    CHECK(view.payloads[1] == "02 0000 0001  2x");
    CHECK(view.payloads[2] == "02 0000 FFFF  2x");
    CHECK(view.payloads_not_listed == 0);
    for (const std::string& line : view.payloads) {
        CHECK(line.find("1111") == std::string::npos);
        CHECK(line.find("location") == std::string::npos);
        CHECK(line.find("event") == std::string::npos);
    }
    CHECK(view.detail.find("Identification 0A5") != std::string::npos);
    CHECK(view.detail.find("2 lost block 3 or 4") != std::string::npos);
}

TEST_CASE("a long TMC table is listed to its cap and the rest counted", "[rds]")
{
    // REJECTS an unbounded list, which on a busy service is a column longer
    // than the window and pushes the rest of the section off it.
    RdsStation station;
    station.tmc.groups = 500;
    for (std::uint16_t i = 0; i < kTmcPayloadsListed + 5; ++i) {
        station.tmc.messages.push_back(payload(0, i, 0, 2));
    }
    const auto view = make_tmc_view(station);
    CHECK(view.payloads.size() == kTmcPayloadsListed);
    CHECK(view.payloads_not_listed == 5);
    CHECK(view.detail.find("No 3A announcement") != std::string::npos);
}
