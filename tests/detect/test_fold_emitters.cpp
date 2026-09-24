// detect::fold_emitters, against a track list built by hand.
//
// What the server publishes where tier two runs: one detection per emitter
// and every other track as it was. Every centre, id, state and figure here is
// chosen, so each case asserts the rule in core/detect/tier_two.h rather than
// a measurement. The measurement, one detection per talker on the voice
// scene over the wire, is tests/rpc/test_rpc_detect_voice.cpp's.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/detect/detector.h"
#include "core/detect/tier_two.h"

using namespace revenant;

namespace {

[[nodiscard]] detect::Track line(std::uint64_t id, dsp::Hertz center, dsp::Hertz bandwidth,
                                 double snr_db,
                                 detect::TrackState state = detect::TrackState::Live)
{
    detect::Track track;
    track.id = id;
    track.state = state;
    track.center = center;
    track.bandwidth = bandwidth;
    track.snr_2500_db = snr_db;
    track.margin_confidence = 0.5 + snr_db / 100.0;
    track.confidence = 0.5;
    track.first_seen = 1'000 * id;
    track.last_seen = 50'000;
    track.last_detected = 50'000;
    return track;
}

[[nodiscard]] const detect::Track* find(const std::vector<detect::Track>& tracks,
                                        std::uint64_t id)
{
    const auto found = std::ranges::find_if(
        tracks, [id](const detect::Track& track) { return track.id == id; });
    return found == tracks.end() ? nullptr : &*found;
}

}  // namespace

// REJECTS: a talker published as its lines, a folded detection that takes the
// anchor's id and so changes id when the anchor moves, a band that is not the
// emitter's extent, a label or an SNR that is not the strongest line's, and a
// merged row left pointing at a line that is no longer on the list.
TEST_CASE("an emitter's lines publish as one detection over its extent", "[detect][tier-two]")
{
    constexpr dsp::Hertz kCarrier = 146'000'000;

    // An AM talker: a carrier and four stretches of sideband, the carrier the
    // strongest. The sideband born first has the lowest id, 3.
    std::vector<detect::Track> tracks = {
        line(3, kCarrier - 2'400, 600, 18.0),
        line(5, kCarrier - 1'200, 500, 16.0, detect::TrackState::Held),
        line(7, kCarrier, 40, 31.0),
        line(8, kCarrier + 1'200, 500, 15.0),
        line(9, kCarrier + 2'400, 600, 17.0),
        // A carrier of its own 20 kHz up, in no group.
        line(4, kCarrier + 20'000, 40, 25.0),
        // Swallowed by the upper sideband stretch.
        line(12, kCarrier + 2'450, 100, 9.0, detect::TrackState::Merged),
    };
    tracks[2].classification = detect::Classification::Unmodulated;
    tracks[2].classification_double_sideband = true;
    tracks[2].classification_confidence = 0.96;
    tracks[2].probes = 2;
    tracks[2].confidence = 0.4;
    tracks[0].confidence = 0.9;
    tracks[0].first_seen = 100;
    tracks[6].merged_into = 9;
    std::ranges::sort(tracks, {}, &detect::Track::center);

    detect::TierTwoEmitter emitter;
    emitter.group = 1;
    emitter.anchor = 7;
    emitter.low_edge = kCarrier - 2'700;
    emitter.high_edge = kCarrier + 2'700;
    emitter.tracks = {3, 5, 7, 8, 9};

    const std::vector<detect::Track> out =
        detect::fold_emitters(tracks, std::vector<detect::TierTwoEmitter>{emitter});

    REQUIRE(out.size() == 3);
    CHECK(std::ranges::is_sorted(out, {}, &detect::Track::center));

    const detect::Track* talker = find(out, 3);
    REQUIRE(talker != nullptr);
    CHECK(talker->center == kCarrier);
    CHECK(talker->bandwidth == 5'400);
    CHECK(talker->state == detect::TrackState::Live);
    CHECK(talker->snr_2500_db == 31.0);
    CHECK(talker->margin_confidence == tracks[2].margin_confidence);
    CHECK(talker->classification == detect::Classification::Unmodulated);
    CHECK(talker->classification_double_sideband);
    CHECK(talker->classification_confidence == 0.96);
    CHECK(talker->probes == 2);
    CHECK(talker->confidence == 0.9);
    CHECK(talker->first_seen == 100);
    CHECK(talker->merged_into == 0);
    for (const std::uint64_t gone : std::vector<std::uint64_t>{5, 7, 8, 9}) {
        CHECK(find(out, gone) == nullptr);
    }

    const detect::Track* other = find(out, 4);
    REQUIRE(other != nullptr);
    CHECK(other->center == kCarrier + 20'000);
    CHECK(other->bandwidth == 40);

    const detect::Track* merged = find(out, 12);
    REQUIRE(merged != nullptr);
    CHECK(merged->merged_into == 3);
}

// REJECTS: a fold that invents an extent for one line, a fold that drops a
// Merged line into the emitter, and an emitter that reads Live when every one
// of its lines is holding.
TEST_CASE("fold_emitters folds only what is on the list, and a holding emitter holds",
          "[detect][tier-two]")
{
    constexpr dsp::Hertz kCarrier = 7'100'000;

    std::vector<detect::Track> tracks = {
        line(20, kCarrier - 900, 400, 12.0, detect::TrackState::Held),
        line(21, kCarrier, 40, 20.0, detect::TrackState::Held),
        line(30, kCarrier + 30'000, 40, 20.0),
        line(31, kCarrier + 30'600, 400, 12.0, detect::TrackState::Merged),
    };

    detect::TierTwoEmitter holding;
    holding.anchor = 21;
    holding.low_edge = kCarrier - 1'100;
    holding.high_edge = kCarrier + 20;
    holding.tracks = {20, 21};

    // Its second line is not on the list at all, and the one other member
    // is Merged: one line is not an emitter to publish.
    detect::TierTwoEmitter thin;
    thin.anchor = 30;
    thin.low_edge = kCarrier + 29'980;
    thin.high_edge = kCarrier + 30'800;
    thin.tracks = {30, 31, 99};

    const std::vector<detect::Track> out = detect::fold_emitters(
        tracks, std::vector<detect::TierTwoEmitter>{holding, thin});

    REQUIRE(out.size() == 3);
    const detect::Track* held = find(out, 20);
    REQUIRE(held != nullptr);
    CHECK(held->state == detect::TrackState::Held);
    CHECK(held->bandwidth == 1'120);
    CHECK(held->snr_2500_db == 20.0);
    CHECK(find(out, 21) == nullptr);

    const detect::Track* alone = find(out, 30);
    REQUIRE(alone != nullptr);
    CHECK(alone->bandwidth == 40);
    CHECK(alone->center == kCarrier + 30'000);
    CHECK(find(out, 31) != nullptr);

    // And with no emitters at all the list is the list.
    const std::vector<detect::Track> same = detect::fold_emitters(tracks, {});
    REQUIRE(same.size() == tracks.size());
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        CHECK(same[i].id == tracks[i].id);
    }
}
