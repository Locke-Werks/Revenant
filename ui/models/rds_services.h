// The parts of an RDS station that are marks, counts and raw bits rather than
// text to read: the programme type name with the segments a correction
// touched, the Traffic Message Channel, and the Emergency Warning System.
//
// DRAWN AS WHAT THEY ARE. docs/rpc.md says it for all three: tmc, ews and
// ptynCorrected are counts and raw bits, and a client must draw them as that.
// So nothing here names an event, a location or a warning's content. What it
// produces is the words and the hexadecimal the RDS section shows, and the
// rules for when each is shown.
//
// THE EMERGENCY WARNING IS THE LOUD ONE. core/rpc/revenant.capnp quotes EN
// 50067 clause 3.1.5.13: type 9A groups are sent "very infrequently, unless an
// emergency occurs or test transmissions are required". So ews.groups above
// zero is a fact an operator is told in the row itself, not in a count on
// hover, and the payload, which each country defines for itself, goes in the
// chip's detail as hexadecimal and nowhere else.
//
// TMC IS COUNTED, AND ONLY CONFIRMED PAYLOADS ARE LISTED. ISO 14819-1
// Introduction 0.3, as the schema quotes it, has a terminal use a group's data
// once a second identical group has verified it, and RdsTmcMessage::confirmed
// is that rule. A payload heard once is as likely to be a damaged copy as a
// message, so it is counted and not listed. The listed ones are bits in
// hexadecimal split at the block boundaries, with no event or location label,
// because ALERT-C's field positions were not in the part of ISO 14819-1 that
// was read and labelling sixteen of the bits a location would be inventing a
// layout.
//
// THE PTYN MARK. core/rpc/types.h: a set bit in ptyn_corrected means the four
// characters of that segment may not be the station's. The section draws such
// a segment in the warning ink, underlined, and says why on hover. The same
// mark exists in the decoder for PS and RadioText and is not on the wire for
// either, so PTYN is the one text this client can mark.
//
// WHY THIS HOLDS NO Qt. ui/tests links it, on the rule models/rds_view.h
// states. The runs, the counts, the ordering and every sentence are pure
// functions of the struct.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/rpc/types.h"
#include "models/rds_text.h"

namespace revenant::ui {

// core/decode/rds_groups.h: the programme type name is eight characters in
// two four-character segments.
inline constexpr std::size_t kPtynLength = 8;
inline constexpr std::size_t kPtynCharsPerSegment = 4;

// Confirmed TMC payloads listed before the rest are summarised in a count.
// NOT A SPECIFIED VALUE: a station carries a few dozen live messages at most,
// and a list longer than a screenful is not read, it is scrolled past.
inline constexpr std::size_t kTmcPayloadsListed = 24;

// A stretch of rendered text whose characters share one mark.
struct RdsRun {
    std::string text;
    bool corrected = false;

    bool operator==(const RdsRun&) const = default;
};

namespace detail {

// `value` in upper case hexadecimal, `digits` wide, with no prefix.
[[nodiscard]] inline std::string hex_digits(std::uint32_t value, int digits)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) {
        out += kDigits[(value >> shift) & 0xF];
    }
    return out;
}

[[nodiscard]] inline std::string group_name(std::uint8_t type, bool version_b)
{
    return std::to_string(static_cast<unsigned>(type)) + (version_b ? "B" : "A");
}

[[nodiscard]] inline std::string counted(std::uint64_t n, const char* one, const char* many)
{
    return std::to_string(n) + ' ' + (n == 1 ? one : many);
}

}  // namespace detail

// Bytes, the segment mask and a corrected mask, rendered as runs.
//
// THE SAME TEXT render_rds_text PRODUCES, cut where the mark changes: the
// runs concatenated are its text, placeholders and trailing trim included,
// which ui/tests/test_rds_services.cpp holds. A placeholder is never marked,
// because there is nothing received under it to doubt.
[[nodiscard]] inline std::vector<RdsRun> render_rds_runs(std::span<const std::uint8_t> bytes,
                                                         std::uint32_t received_mask,
                                                         std::uint32_t corrected_mask,
                                                         std::size_t chars_per_segment,
                                                         std::size_t length)
{
    std::vector<RdsRun> out;
    if (chars_per_segment == 0) {
        return out;
    }
    if (length > bytes.size()) {
        length = bytes.size();
    }

    struct Glyph {
        char32_t code_point;
        bool corrected;
    };
    std::vector<Glyph> glyphs;
    std::size_t kept = 0;  // glyphs up to the last one that is not a space
    for (std::size_t i = 0; i < length; ++i) {
        const auto segment = static_cast<unsigned>(i / chars_per_segment);
        const bool received = segment < 32 && (received_mask & (1U << segment)) != 0;
        if (!received) {
            glyphs.push_back({kRdsPlaceholder, false});
            kept = glyphs.size();
            continue;
        }
        const char32_t code_point = annex_e_code_point(bytes[i]);
        if (code_point == 0) {
            continue;
        }
        const bool corrected = (corrected_mask & (1U << segment)) != 0;
        glyphs.push_back({code_point, corrected});
        if (code_point != U' ') {
            kept = glyphs.size();
        }
    }
    glyphs.resize(kept);

    for (const Glyph& glyph : glyphs) {
        if (out.empty() || out.back().corrected != glyph.corrected) {
            out.push_back(RdsRun{{}, glyph.corrected});
        }
        append_utf8(out.back().text, glyph.code_point);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Programme type name
// ---------------------------------------------------------------------------

struct RdsPtynView {
    std::vector<RdsRun> runs;
    int segments_received = 0;
    int segments_total = 0;

    // Received segments whose corrected bit is set.
    int segments_corrected = 0;

    // "1/2 segments" while one is missing, empty when both are in.
    std::string note;

    // The hover sentence for the mark, empty when nothing is marked.
    std::string corrected_detail;

    [[nodiscard]] bool empty() const { return segments_received == 0; }
};

[[nodiscard]] inline RdsPtynView make_ptyn_view(const rpc::RdsStation& station)
{
    RdsPtynView out;
    const RdsText text =
        render_rds_text(station.ptyn, station.ptyn_received, kPtynCharsPerSegment, kPtynLength);
    out.segments_received = text.segments_received;
    out.segments_total = text.segments_total;
    out.runs = render_rds_runs(station.ptyn, station.ptyn_received, station.ptyn_corrected,
                               kPtynCharsPerSegment, kPtynLength);

    for (int segment = 0; segment < out.segments_total; ++segment) {
        const std::uint32_t bit = 1U << segment;
        if ((station.ptyn_received & bit) != 0 && (station.ptyn_corrected & bit) != 0) {
            ++out.segments_corrected;
        }
    }
    if (out.segments_received < out.segments_total) {
        out.note = std::to_string(out.segments_received) + "/" +
                   std::to_string(out.segments_total) + " segments";
    }
    if (out.segments_corrected > 0) {
        out.corrected_detail =
            "The underlined characters came through a block the error corrector repaired, "
            "so they may not be the station's. The mark clears when the segment next "
            "arrives clean.";
    }
    return out;
}

// ---------------------------------------------------------------------------
// Emergency Warning System
// ---------------------------------------------------------------------------

struct RdsEwsView {
    // ews.groups above zero: the station has sent a 9A group, or a test of one.
    bool sent = false;

    // The chip, and the sentence with the payload in hexadecimal on hover.
    std::string label;
    std::string detail;
};

[[nodiscard]] inline RdsEwsView make_ews_view(const rpc::RdsStation& station)
{
    RdsEwsView out;
    const rpc::RdsRawGroup& ews = station.ews;
    if (ews.groups == 0) {
        return out;
    }
    out.sent = true;
    out.label = "emergency warning";

    std::string sentence = "This station has sent " +
                           detail::counted(ews.groups, "type 9A group", "type 9A groups") +
                           ", the Emergency Warning System, which EN 50067 has sent very "
                           "infrequently unless there is an emergency or a test of one. Each "
                           "country defines the payload, "
                           "so it is shown raw and not decoded. Latest: block 2 bits " +
                           detail::hex_digits(ews.block2_low & 0x1FU, 2) + ", block 3 ";
    sentence += ews.block3_valid ? detail::hex_digits(ews.block3, 4) : std::string("lost");
    sentence += ", block 4 ";
    sentence += ews.block4_valid ? detail::hex_digits(ews.block4, 4) : std::string("lost");
    sentence += ews.corrected ? ", through a corrected block." : ".";
    if (station.ews_channel_identification_valid) {
        sentence += " EWS channel identification " +
                    detail::hex_digits(station.ews_channel_identification & 0xFFFU, 3) + ".";
    }
    out.detail = sentence;
    return out;
}

// ---------------------------------------------------------------------------
// Traffic Message Channel
// ---------------------------------------------------------------------------

struct RdsTmcView {
    // Groups attributed to TMC have arrived.
    bool on_air = false;

    // A 3A group announced a TMC application, whether or not a group has
    // followed.
    bool announced = false;

    // on_air or announced: the section shows TMC at all.
    bool shown = false;

    std::string label;

    // Groups, confirmed payloads and payloads heard once, separated by middle
    // dots, for a fixed-width readout.
    std::string counts;

    std::string detail;

    // Confirmed payloads, most received first: "1F 1234 5678  3x", then
    // ", 1 corrected" when any reception came through a repaired block.
    std::vector<std::string> payloads;

    // Confirmed payloads past kTmcPayloadsListed, not listed.
    std::size_t payloads_not_listed = 0;
};

// One payload as the list prints it: block 2's five bits, block 3 and block 4,
// in hexadecimal and in that order, which is the order they arrive.
[[nodiscard]] inline std::string tmc_payload_text(const rpc::RdsTmcMessage& message)
{
    std::string out = detail::hex_digits(message.x & 0x1FU, 2) + ' ' +
                      detail::hex_digits(message.y, 4) + ' ' + detail::hex_digits(message.z, 4) +
                      "  " + std::to_string(message.receptions) + "x";
    if (message.corrected_receptions > 0) {
        out += ", " + std::to_string(message.corrected_receptions) + " corrected";
    }
    return out;
}

[[nodiscard]] inline RdsTmcView make_tmc_view(const rpc::RdsStation& station)
{
    RdsTmcView out;
    const rpc::RdsTmc& tmc = station.tmc;
    out.on_air = tmc.groups > 0;
    out.announced = tmc.announced;
    out.shown = out.on_air || out.announced;
    if (!out.shown) {
        return out;
    }
    out.label = out.on_air ? "TMC" : "TMC announced";

    std::vector<const rpc::RdsTmcMessage*> confirmed;
    std::uint64_t once = 0;
    for (const rpc::RdsTmcMessage& message : tmc.messages) {
        if (message.confirmed()) {
            confirmed.push_back(&message);
        } else {
            ++once;
        }
    }

    // Most received first, then by value, so two polls of an unchanged table
    // list it in the same order although the wire promises none.
    std::ranges::sort(confirmed, [](const rpc::RdsTmcMessage* a, const rpc::RdsTmcMessage* b) {
        if (a->receptions != b->receptions) {
            return a->receptions > b->receptions;
        }
        if (a->x != b->x) {
            return a->x < b->x;
        }
        if (a->y != b->y) {
            return a->y < b->y;
        }
        return a->z < b->z;
    });
    const std::size_t listed = std::min(confirmed.size(), kTmcPayloadsListed);
    for (std::size_t i = 0; i < listed; ++i) {
        out.payloads.push_back(tmc_payload_text(*confirmed[i]));
    }
    out.payloads_not_listed = confirmed.size() - listed;

    out.counts = detail::counted(tmc.groups, "group", "groups") + "  \xC2\xB7  " +
                 std::to_string(confirmed.size()) + " confirmed  \xC2\xB7  " +
                 std::to_string(once) + " heard once";

    std::string sentence = "Traffic Message Channel. ";
    if (out.announced) {
        sentence += "Announced in a 3A group with AID " + detail::hex_digits(tmc.aid, 4) +
                    " on group " + detail::group_name(tmc.group_type, tmc.version_b) +
                    ", message bits " + detail::hex_digits(tmc.oda_message, 4) + ". ";
    } else {
        sentence += "No 3A announcement has arrived; the groups came in on type 8A's default "
                    "use. ";
    }
    if (tmc.identification_valid) {
        sentence +=
            "Identification " + detail::hex_digits(tmc.identification & 0xFFFU, 3) + ". ";
    }
    sentence += detail::counted(tmc.groups, "group", "groups") + ", " +
                std::to_string(tmc.oda_groups) + " through the announcement; " +
                std::to_string(tmc.incomplete) + " lost block 3 or 4 and joined no message; " +
                std::to_string(tmc.evicted) +
                " evicted from the decoder's table. A payload counts as confirmed once it has "
                "arrived twice, and only those are listed, in hexadecimal: the layout of "
                "events and locations is not implemented, so no field is named.";
    out.detail = sentence;
    return out;
}

}  // namespace revenant::ui
