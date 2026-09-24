// Tracks followed as a set, against arithmetic and against the family scene.
//
// The first half hands core/detect/groups.h a track list built by hand, so
// every centre, birth and state is chosen and each case asserts against the
// rule rather than against a measurement.
//
// The second half runs the family scene test_front_end.cpp surveys, one
// emitter of each of eight modulations, through the channelizer and spectrum
// twins and the detector, and feeds every decision's track list to a grouper.
// docs/detection.md measured the lines of that scene as a set by hand, off
// candidates, and found the textbook answer for every analogue family. This
// is the same question asked of the surface that now exists, with an identity
// that has to survive from one decision to the next.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <print>
#include <random>
#include <string>
#include <vector>

#include "core/detect/detector.h"
#include "core/detect/groups.h"
#include "core/dsp/synth/modulators.h"
#include "core/dsp/synth/wideband.h"
#include "scene_frames.h"

using namespace revenant;

namespace {

// ---------------------------------------------------------------------------
// The arithmetic half
// ---------------------------------------------------------------------------

constexpr dsp::SampleRate kRate = 96'000;

[[nodiscard]] dsp::SampleIndex at_seconds(double seconds)
{
    return static_cast<dsp::SampleIndex>(std::llround(seconds * static_cast<double>(kRate)));
}

[[nodiscard]] detect::Track line(std::uint64_t id, dsp::Hertz center, double snr_db,
                                 double born_seconds,
                                 detect::TrackState state = detect::TrackState::Live)
{
    detect::Track track;
    track.id = id;
    track.state = state;
    track.center = center;
    track.bandwidth = 10;
    track.snr_2500_db = snr_db;
    track.first_seen = at_seconds(born_seconds);
    return track;
}

[[nodiscard]] detect::LineGrouper grouper_at(dsp::Hertz gap)
{
    auto made = detect::LineGrouper::create(detect::LineGroupConfig{.gap_hz = gap});
    REQUIRE(made.has_value());
    return std::move(*made);
}

[[nodiscard]] std::vector<std::uint64_t> member_ids(const detect::LineGrouper& grouper,
                                                    const detect::LineGroup& group)
{
    std::vector<std::uint64_t> out;
    for (const detect::GroupMember& member : grouper.members(group)) {
        out.push_back(member.track);
    }
    return out;
}

// ---------------------------------------------------------------------------
// The scene half
// ---------------------------------------------------------------------------

// The same scene test_front_end.cpp's family survey runs, built the same way:
// 600 kS/s over 64 channels with a 512-point second stage, which is the
// shipped 36.6 Hz a bin, and eight emitters 70 kHz apart at 30 dB in their
// own occupied bandwidth.
constexpr dsp::SampleRate kSceneRate = 600'000;
constexpr std::uint32_t kSceneChannels = 64;
constexpr std::uint32_t kSceneTransform = 512;
constexpr dsp::Hertz kSceneCenter = 98'100'000;

constexpr dsp::Hertz kFamilySpacing = 70'000;
constexpr dsp::Hertz kFamilyFirst = -245'000;
constexpr double kFamilySnrDb = 30.0;
constexpr double kFamilySceneSeconds = 6.0;

struct Family {
    const char* name;
    siggen::Modulation kind;
};

constexpr Family kFamilies[] = {
    {"cw", siggen::Modulation::Cw},     {"am", siggen::Modulation::Am},
    {"nfm", siggen::Modulation::Nfm},   {"usb", siggen::Modulation::Usb},
    {"lsb", siggen::Modulation::Lsb},   {"fsk2", siggen::Modulation::Fsk2},
    {"bpsk", siggen::Modulation::Bpsk}, {"qpsk", siggen::Modulation::Qpsk},
};
constexpr std::size_t kCw = 0;
constexpr std::size_t kAm = 1;
constexpr std::size_t kNfm = 2;
constexpr std::size_t kUsb = 3;
constexpr std::size_t kLsb = 4;
constexpr std::size_t kBpsk = 6;
constexpr std::size_t kQpsk = 7;

[[nodiscard]] siggen::SceneSpec family_scene()
{
    siggen::SceneSpec spec;
    spec.rate = kSceneRate;
    spec.center_hz = kSceneCenter;
    spec.duration_samples = static_cast<dsp::SampleIndex>(
        std::llround(kFamilySceneSeconds * static_cast<double>(kSceneRate)));
    spec.seed = 1234;
    spec.noise_power_full_band_dbfs = -60.0;

    std::uint64_t seed = 900;
    for (std::size_t i = 0; i < std::size(kFamilies); ++i) {
        siggen::ModulatorSpec modulator;
        modulator.kind = kFamilies[i].kind;
        modulator.common.rate = kSceneRate;
        modulator.common.carrier_offset =
            kFamilyFirst + static_cast<dsp::Hertz>(i) * kFamilySpacing;
        modulator.common.seed = seed++;

        // Two-tone SSB for the reason test_front_end.cpp gives: a single-tone
        // USB on the streaming path is one exponential, which is a carrier.
        modulator.ssb.tone_hz = 700;
        modulator.ssb.tone2_hz = 1900;

        siggen::EmitterPlacement placement;
        placement.modulator = modulator;
        placement.use_snr = true;
        placement.snr_in_occupied_bandwidth_db = kFamilySnrDb;
        placement.start_sample = 0;
        placement.end_sample = spec.duration_samples;
        spec.emitters.push_back(placement);
    }
    return spec;
}

// What one group looked like at one decision, copied out because the
// grouper's spans are overwritten by the next one.
struct GroupSnapshot {
    std::uint64_t id = 0;
    std::size_t family = 0;
    dsp::SampleIndex decided = 0;
    dsp::SampleIndex together_since = 0;
    dsp::SampleIndex birth_spread = 0;
    std::vector<detect::GroupMember> members;
};

struct GroupedRun {
    std::size_t decisions = 0;
    dsp::SampleIndex last_decision = 0;
    std::vector<GroupSnapshot> history;  // every group at every decision
    std::vector<GroupSnapshot> last;     // the final decision's groups
    detect::LineGroupStats stats{};
};

// The emitter slot a centre belongs to, by nearest slot.
[[nodiscard]] std::size_t family_of(dsp::Hertz absolute)
{
    const double offset = static_cast<double>(absolute - kSceneCenter - kFamilyFirst);
    const long slot = std::lround(offset / static_cast<double>(kFamilySpacing));
    const long highest = static_cast<long>(std::size(kFamilies)) - 1;
    return static_cast<std::size_t>(std::clamp<long>(slot, 0, highest));
}

[[nodiscard]] GroupedRun run_grouped(dsp::Hertz gap)
{
    test::SceneGeometry geometry;
    geometry.rate = kSceneRate;
    geometry.channels = kSceneChannels;
    geometry.transform = kSceneTransform;

    auto frames = test::SceneFrames::create(geometry, family_scene(), test::FrontEndModel{});
    if (!frames) {
        FAIL("scene frames: " << frames.error().message);
    }

    detect::DetectorConfig config;
    config.source_rate = geometry.rate;
    config.source_center = kSceneCenter;
    config.grid_channels = geometry.channels;
    auto detector = detect::Detector::create(config, frames->spectrum());
    if (!detector) {
        FAIL("detector: " << detector.error().message);
    }
    detect::LineGrouper grouper = grouper_at(gap);

    GroupedRun run;
    dsp::SampleIndex previous = 0;
    const std::uint64_t total = frames->frames_available();
    for (std::uint64_t i = 0; i < total; ++i) {
        auto frame = frames->next();
        if (!frame) {
            FAIL("frame " << i << ": " << frame.error().message);
        }
        const Status fed = detector->consume(*frame);
        if (!fed) {
            FAIL("consume: " << fed.error().message);
        }
        const dsp::SampleIndex decided = detector->last_decision();
        if (decided == 0 || decided == previous) {
            continue;
        }
        previous = decided;
        ++run.decisions;

        const Status grouped = grouper.observe(detector->tracks(), decided);
        REQUIRE(grouped.has_value());

        run.last.clear();
        for (const detect::LineGroup& group : grouper.groups()) {
            GroupSnapshot snapshot;
            snapshot.id = group.id;
            snapshot.family = family_of(group.anchor_center);
            snapshot.decided = decided;
            snapshot.together_since = group.together_since;
            snapshot.birth_spread = group.birth_spread;
            const auto members = grouper.members(group);
            snapshot.members.assign(members.begin(), members.end());
            run.history.push_back(snapshot);
            run.last.push_back(std::move(snapshot));
        }
    }
    run.last_decision = previous;
    run.stats = grouper.stats();
    return run;
}

[[nodiscard]] std::string describe(const GroupSnapshot& group)
{
    std::string out = std::format("{} group #{}: {} lines, together {:.1f}s, births {:.1f}s apart:",
                                  kFamilies[group.family].name, group.id, group.members.size(),
                                  static_cast<double>(group.decided - group.together_since) /
                                      static_cast<double>(kSceneRate),
                                  static_cast<double>(group.birth_spread) /
                                      static_cast<double>(kSceneRate));
    for (const detect::GroupMember& member : group.members) {
        out += std::format(" {:+}", member.offset);
    }
    return out;
}

[[nodiscard]] std::vector<const GroupSnapshot*> groups_of(const GroupedRun& run,
                                                          std::size_t family)
{
    std::vector<const GroupSnapshot*> out;
    for (const GroupSnapshot& group : run.last) {
        if (group.family == family) {
            out.push_back(&group);
        }
    }
    return out;
}

// Within this of a stated offset. One bin at 36.6 Hz, which is what a
// power-weighted centre over five bins can be trusted to.
constexpr dsp::Hertz kLineTolerance = 37;

[[nodiscard]] bool near(dsp::Hertz value, dsp::Hertz wanted)
{
    return std::abs(value - wanted) <= kLineTolerance;
}

}  // namespace

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------

// REJECTS: a grouping that compares every pair against the gap rather than
// neighbours, which would split a comb whose outer lines sit further apart
// than the gap; and one that reports a lone line as a group of one, which
// makes every carrier on the band look like structure.
TEST_CASE("lines within the gap of a neighbour are one group, and a lone line is none",
          "[detect][groups]")
{
    detect::LineGrouper grouper = grouper_at(1'000);
    const std::vector<detect::Track> tracks = {
        line(1, 7'001'000, 10.0, 0.0),
        line(2, 7'002'000, 20.0, 0.0),
        line(3, 7'003'000, 10.0, 0.0),
        line(4, 7'010'000, 30.0, 0.0),
    };
    REQUIRE(grouper.observe(tracks, at_seconds(5.0)).has_value());

    REQUIRE(grouper.groups().size() == 1);
    const detect::LineGroup& group = grouper.groups().front();
    CHECK(member_ids(grouper, group) == std::vector<std::uint64_t>{1, 2, 3});

    // The strongest is the anchor, and the offsets and spacings are the
    // carrier-with-a-matched-pair pattern stated from it.
    CHECK(group.anchor == 2);
    CHECK(group.anchor_center == 7'002'000);
    CHECK(group.span == 2'000);
    const auto members = grouper.members(group);
    CHECK(members[0].offset == -1'000);
    CHECK(members[1].offset == 0);
    CHECK(members[2].offset == 1'000);
    CHECK(members[0].spacing == 0);
    CHECK(members[1].spacing == 1'000);
    CHECK(members[2].spacing == 1'000);
}

// REJECTS: an id per decision. An identity that does not survive a decision
// is a snapshot, and a snapshot is what the CLI already had and what the HF
// corpus showed moving between two runs of the same file.
//
// And together_since has to be the LATEST arrival, not the group's founding:
// a group whose third line arrived a second ago has had that set of lines for
// a second.
TEST_CASE("a group keeps its id as lines join, and says since when the set is whole",
          "[detect][groups]")
{
    detect::LineGrouper grouper = grouper_at(500);

    std::vector<detect::Track> tracks = {line(1, 14'000'000, 20.0, 0.0),
                                         line(2, 14'000'300, 10.0, 0.0)};
    REQUIRE(grouper.observe(tracks, at_seconds(1.0)).has_value());
    REQUIRE(grouper.groups().size() == 1);
    const std::uint64_t id = grouper.groups().front().id;
    CHECK(grouper.groups().front().formed == at_seconds(1.0));
    CHECK(grouper.groups().front().together_since == at_seconds(1.0));

    tracks.push_back(line(3, 14'000'600, 10.0, 1.5));
    REQUIRE(grouper.observe(tracks, at_seconds(2.0)).has_value());
    REQUIRE(grouper.groups().size() == 1);
    const detect::LineGroup& grown = grouper.groups().front();
    CHECK(grown.id == id);
    CHECK(grown.formed == at_seconds(1.0));
    CHECK(grown.together_since == at_seconds(2.0));
    CHECK(grouper.members(grown)[0].joined == at_seconds(1.0));
    CHECK(grouper.members(grown)[2].joined == at_seconds(2.0));

    REQUIRE(grouper.observe(tracks, at_seconds(9.0)).has_value());
    CHECK(grouper.groups().front().id == id);
    CHECK(grouper.groups().front().together_since == at_seconds(2.0));

    CHECK(grouper.stats().groups_formed == 1);
    CHECK(grouper.stats().joins == 1);
    CHECK(grouper.stats().leaves == 0);

    // Seven seconds whole, from the third line's arrival to the last
    // decision, and not the eight since the group formed.
    CHECK(grouper.stats().longest_together == at_seconds(7.0));
    CHECK(grouper.stats().longest_together_group == id);
}

// REJECTS: grouping off the Live state alone, which drops a line from its
// group the moment it fades and re-admits it with a new join time when it
// returns. The tracker already decided how long a fade may last; this
// follows it.
//
// And grouping a Merged track, which counts one line twice: the track that
// swallowed it is in the list carrying the same energy.
TEST_CASE("a held line stays in its group and a merged one is not counted",
          "[detect][groups]")
{
    detect::LineGrouper grouper = grouper_at(500);

    std::vector<detect::Track> tracks = {line(1, 7'100'000, 20.0, 0.0),
                                         line(2, 7'100'200, 10.0, 0.0)};
    REQUIRE(grouper.observe(tracks, at_seconds(1.0)).has_value());
    const std::uint64_t id = grouper.groups().front().id;

    tracks[1].state = detect::TrackState::Held;
    REQUIRE(grouper.observe(tracks, at_seconds(2.0)).has_value());
    REQUIRE(grouper.groups().size() == 1);
    CHECK(grouper.groups().front().id == id);
    CHECK(grouper.members(grouper.groups().front())[1].state == detect::TrackState::Held);
    CHECK(grouper.members(grouper.groups().front())[1].joined == at_seconds(1.0));

    tracks[1].state = detect::TrackState::Merged;
    REQUIRE(grouper.observe(tracks, at_seconds(3.0)).has_value());
    CHECK(grouper.groups().empty());
    CHECK(grouper.stats().groups_ended == 1);
}

// REJECTS: a split that hands the old id to whichever half happens to come
// first in frequency. The half carrying more of the old group's lines is the
// continuation; the other is new and its lines joined it now.
TEST_CASE("a group that splits keeps its id on the larger half", "[detect][groups]")
{
    detect::LineGrouper grouper = grouper_at(1'000);

    std::vector<detect::Track> tracks = {
        line(1, 10'000, 10.0, 0.0), line(2, 11'000, 10.0, 0.0), line(3, 12'000, 20.0, 0.0),
        line(4, 13'000, 10.0, 0.0), line(5, 14'000, 10.0, 0.0),
    };
    REQUIRE(grouper.observe(tracks, at_seconds(1.0)).has_value());
    REQUIRE(grouper.groups().size() == 1);
    const std::uint64_t id = grouper.groups().front().id;

    // The middle line moves up, which opens a 2 kHz hole between 11 and 13
    // kHz and puts it in the upper half: two lines below, three above.
    tracks[2].center = 13'500;
    REQUIRE(grouper.observe(tracks, at_seconds(2.0)).has_value());
    REQUIRE(grouper.groups().size() == 2);

    const detect::LineGroup& lower = grouper.groups()[0];
    const detect::LineGroup& upper = grouper.groups()[1];
    CHECK(member_ids(grouper, lower) == std::vector<std::uint64_t>{1, 2});
    CHECK(member_ids(grouper, upper) == std::vector<std::uint64_t>{4, 3, 5});
    CHECK(upper.id == id);
    CHECK(upper.together_since == at_seconds(1.0));
    CHECK(lower.id != id);
    CHECK(lower.formed == at_seconds(2.0));
    CHECK(lower.together_since == at_seconds(2.0));

    CHECK(grouper.stats().groups_formed == 2);
    CHECK(grouper.stats().leaves == 2);
}

// REJECTS: a group that reports only its own age. The HF case in
// docs/detection.md is a carrier 41.6 seconds old with a neighbour 6.8
// seconds old; a real pair would have been born together. The spread of the
// births is what says which of those a group is.
TEST_CASE("the spread of the births tells lines born together from a coincidence",
          "[detect][groups]")
{
    detect::LineGrouper grouper = grouper_at(300);
    const std::vector<detect::Track> together = {line(1, 7'013'238, 11.7, 10.0),
                                                 line(2, 7'013'249, 22.3, 10.2)};
    REQUIRE(grouper.observe(together, at_seconds(50.0)).has_value());
    CHECK(grouper.groups().front().birth_spread == at_seconds(0.2));

    detect::LineGrouper other = grouper_at(300);
    const std::vector<detect::Track> apart = {line(45, 7'013'238, 11.7, 43.2),
                                              line(1, 7'013'249, 22.3, 8.4)};
    REQUIRE(other.observe(apart, at_seconds(50.0)).has_value());
    CHECK(other.groups().front().birth_spread == at_seconds(34.8));
    CHECK(other.groups().front().anchor == 1);
    CHECK(other.members(other.groups().front())[0].offset == -11);
}

// REJECTS: an answer that depends on the order the list arrived in.
// Detector::tracks() is ascending in frequency today, and nothing here should
// rest on that.
TEST_CASE("grouping is a function of the track list and not of its order",
          "[detect][groups]")
{
    std::vector<detect::Track> tracks;
    for (std::uint64_t i = 1; i <= 24; ++i) {
        // Six clusters of four, 400 Hz inside, 5 kHz between.
        const auto cluster = static_cast<dsp::Hertz>((i - 1) / 4);
        const auto within = static_cast<dsp::Hertz>((i - 1) % 4);
        tracks.push_back(line(i, 20'000 + 5'000 * cluster + 400 * within,
                              static_cast<double>(i % 5), 0.1 * static_cast<double>(i)));
    }

    detect::LineGrouper ordered = grouper_at(500);
    REQUIRE(ordered.observe(tracks, at_seconds(10.0)).has_value());
    REQUIRE(ordered.groups().size() == 6);

    constexpr std::uint64_t kSeed = 20260922;
    std::println("test_groups order: seed {}", kSeed);
    std::mt19937_64 shuffle(kSeed);
    for (int trial = 0; trial < 8; ++trial) {
        std::shuffle(tracks.begin(), tracks.end(), shuffle);
        detect::LineGrouper shuffled = grouper_at(500);
        REQUIRE(shuffled.observe(tracks, at_seconds(10.0)).has_value());
        REQUIRE(shuffled.groups().size() == ordered.groups().size());
        for (std::size_t g = 0; g < ordered.groups().size(); ++g) {
            const detect::LineGroup& a = ordered.groups()[g];
            const detect::LineGroup& b = shuffled.groups()[g];
            CHECK(a.id == b.id);
            CHECK(a.anchor == b.anchor);
            CHECK(member_ids(ordered, a) == member_ids(shuffled, b));
        }
    }
}

// REJECTS: an edge rule that compares neighbours' centres, which would leave a
// carrier's line outside the wide sideband band it sits beside; one that
// compares only the previous line's edge, which a narrow line inside a wide
// band's extent would cut; and an extent that forgets a member's width.
//
// The layout is an AM talker as the detector reported one at 30 dB in the
// voice survey: a wide lower sideband stretch, a narrow line inside its
// reach, the carrier 315 Hz clear of the inner stretch, and the upper side.
TEST_CASE("an edge gap chains the pieces of one filled emitter and not a neighbour",
          "[detect][groups]")
{
    const auto piece = [](std::uint64_t id, dsp::Hertz center, dsp::Hertz width, double snr) {
        detect::Track track = line(id, center, snr, 0.0);
        track.bandwidth = width;
        return track;
    };
    const std::vector<detect::Track> tracks = {
        piece(1, 146'000'000 - 2'243, 2'450, 10.0),
        piece(2, 146'000'000 - 1'500, 60, 3.0),
        piece(3, 146'000'000 - 640, 467, 8.0),
        piece(4, 146'000'000, 183, 30.0),
        piece(5, 146'000'000 + 643, 471, 8.0),
        piece(6, 146'000'000 + 2'322, 2'554, 10.0),
        piece(7, 146'000'000 + 5'000, 183, 20.0),
    };
    auto made = detect::LineGrouper::create(detect::LineGroupConfig{.edge_gap_hz = 400});
    REQUIRE(made.has_value());
    detect::LineGrouper grouper = std::move(*made);
    REQUIRE(grouper.observe(tracks, at_seconds(5.0)).has_value());

    REQUIRE(grouper.groups().size() == 1);
    const detect::LineGroup& group = grouper.groups().front();
    CHECK(member_ids(grouper, group) == std::vector<std::uint64_t>{1, 2, 3, 4, 5, 6});
    CHECK(group.anchor == 4);
    CHECK(group.low_edge == 146'000'000 - 2'243 - 1'225);
    CHECK(group.high_edge == 146'000'000 + 2'322 + 1'277);

    // The same lines at a 200 Hz edge gap: the carrier stands 315 Hz clear of
    // both inner stretches and is on its own, and each side is its own group.
    auto narrow = detect::LineGrouper::create(detect::LineGroupConfig{.edge_gap_hz = 200});
    REQUIRE(narrow.has_value());
    REQUIRE(narrow->observe(tracks, at_seconds(5.0)).has_value());
    REQUIRE(narrow->groups().size() == 2);
    CHECK(member_ids(*narrow, narrow->groups()[0]) == std::vector<std::uint64_t>{1, 2, 3});
    CHECK(member_ids(*narrow, narrow->groups()[1]) == std::vector<std::uint64_t>{5, 6});
}

// REJECTS: a grouper that accepts a gap it cannot use, and one that takes a
// decision from before the last one as though the capture had continued.
TEST_CASE("a grouper refuses a gap of nothing and time running backward", "[detect][groups]")
{
    CHECK_FALSE(detect::LineGrouper::create(detect::LineGroupConfig{.gap_hz = 0}).has_value());
    CHECK_FALSE(detect::LineGrouper::create(detect::LineGroupConfig{.gap_hz = -5}).has_value());

    detect::LineGrouper grouper = grouper_at(500);
    const std::vector<detect::Track> tracks = {line(1, 1'000, 1.0, 0.0),
                                               line(2, 1'200, 2.0, 0.0)};
    REQUIRE(grouper.observe(tracks, at_seconds(2.0)).has_value());
    const Status backward = grouper.observe(tracks, at_seconds(1.0));
    REQUIRE_FALSE(backward.has_value());
    CHECK(backward.error().message.find("reset()") != std::string::npos);

    grouper.reset();
    REQUIRE(grouper.observe(tracks, at_seconds(1.0)).has_value());
    CHECK(grouper.groups().size() == 1);
}

// ---------------------------------------------------------------------------
// The family scene
// ---------------------------------------------------------------------------

// REJECTS: a surface that cannot carry what the hand survey in
// docs/detection.md found between the lines. That survey read candidates off
// one decision at a 9.16 Hz grid; this is the tracker's output at the shipped
// 36.6 Hz, grouped as it runs, with a gap of 3 kHz. Three kilohertz is wider
// than every spacing an analogue family here produces and a twentieth of the
// slot spacing, so the gap is not what is being tested.
//
// What each family has to come out as, at the last decision:
//
//   cw, bpsk, qpsk   no group. One line or one filled band.
//   am               three lines, the carrier strongest, the others at plus
//                    and minus the 1 kHz tone.
//   usb, lsb         two lines 1200 Hz apart, which is 1900 minus 700.
//   nfm              a comb, every spacing a whole number of kilohertz.
//
// And the am group has to hold ONE id from the moment it is whole to the end
// of the scene, which is the property a snapshot did not have.
//
// WHAT IT MEASURED, 2026-09-22, offsets from each group's strongest line:
//
//   am     3 lines   -989 *0 +1017
//   nfm   15 lines   -11004 ... +3003, every spacing 989 to 1025
//   usb    2 lines   -1191 *0
//   lsb    2 lines   *0 +1191
//   fsk2   9 lines   -1547 to +6371, no pattern; its tones wander
//   cw, bpsk, qpsk   no group
//
// 46 decisions, five groups formed and none ended, every one whole for the
// last 4.7 seconds with its lines born in the same decision. Underneath the
// stable ids there were 17 joins and 17 leaves over the run.
//
// usb and lsb DO come out mirrored here, and that is the scene rather than
// the rule: the 1900 Hz tone measures louder than the 700 Hz one in both, so
// the anchor is the outer line on each side. Two tones of equal level would
// leave the sign to measurement noise. groups.h says so.
TEST_CASE("the family scene's lines group into the pattern each family makes",
          "[detect][groups][scene]")
{
    const GroupedRun run = run_grouped(3'000);
    REQUIRE(run.decisions > 0);

    for (const GroupSnapshot& group : run.last) {
        std::println("  {}", describe(group));
    }
    std::println("  {} decisions, {} groups formed, {} ended, {} joins, {} leaves",
                 run.decisions, run.stats.groups_formed, run.stats.groups_ended,
                 run.stats.joins, run.stats.leaves);

    CHECK(groups_of(run, kCw).empty());
    CHECK(groups_of(run, kBpsk).empty());
    CHECK(groups_of(run, kQpsk).empty());

    const auto am = groups_of(run, kAm);
    REQUIRE(am.size() == 1);
    REQUIRE(am.front()->members.size() == 3);
    CHECK(near(am.front()->members[0].offset, -1'000));
    CHECK(am.front()->members[1].offset == 0);
    CHECK(near(am.front()->members[2].offset, 1'000));

    for (const std::size_t sideband : {kUsb, kLsb}) {
        const auto ssb = groups_of(run, sideband);
        CAPTURE(kFamilies[sideband].name);
        REQUIRE(ssb.size() == 1);
        REQUIRE(ssb.front()->members.size() == 2);
        CHECK(near(ssb.front()->members[1].spacing, 1'200));
    }

    const auto nfm = groups_of(run, kNfm);
    REQUIRE(nfm.size() == 1);
    CHECK(nfm.front()->members.size() >= 5);
    for (std::size_t m = 1; m < nfm.front()->members.size(); ++m) {
        const dsp::Hertz spacing = nfm.front()->members[m].spacing;
        CAPTURE(m, spacing);
        CHECK(near(spacing, 1'000 * std::max<dsp::Hertz>(1, (spacing + 500) / 1'000)));
    }

    // One id for the am group from the first decision it had all three
    // lines to the last.
    std::uint64_t am_id = 0;
    std::size_t whole = 0;
    for (const GroupSnapshot& group : run.history) {
        if (group.family != kAm || group.members.size() != 3) {
            continue;
        }
        if (am_id == 0) {
            am_id = group.id;
        }
        CHECK(group.id == am_id);
        ++whole;
    }
    CHECK(whole > run.decisions / 2);

    // Born together: the three lines of one station arrive within the birth
    // rule's own few decisions of each other.
    CHECK(am.front()->birth_spread < static_cast<dsp::SampleIndex>(kSceneRate));
}

// Does the detector put lines either side of one steady carrier at the HF
// grid?
//
// THE QUESTION THE HF CORPUS RAISED. Grouped across the six excerpts in
// docs/recordings.md, six different carriers on two bands came with a line
// nine to twelve hertz either side of them. Six carriers doing the same thing
// points at the analysis before it points at the transmitters, and the cheap
// way to find out is one carrier with nothing else and no propagation: if the
// pair appears here it is the detector.
//
// WHAT IT MEASURED, 2026-09-22: it does not. One track, 6 Hz wide at 16.1 dB,
// tracked at all 55 decisions with no second track within 50 Hz at any of
// them, and no group. So a steady line alone does not make the pair. A
// carrier that drifts, and propagation, are both still open.
//
// The HF geometry exactly: 96 kS/s over 64 channels and a 2048-point second
// stage, 1.465 Hz a bin at 1.465 frames a second. The carrier sits 7.19 kHz
// up, where 40 m had one, at about 16 dB in the 2500 Hz reference, which is
// where the corpus's carriers sit.
//
// A measurement, hidden, and it asserts only that the carrier was found.
TEST_CASE("groups survey: one steady carrier at the HF grid", "[.groups-survey]")
{
    constexpr dsp::SampleRate kHfRate = 96'000;
    constexpr dsp::Hertz kCarrier = 7'190;
    constexpr double kSeconds = 40.0;

    test::SceneGeometry geometry;
    geometry.rate = kHfRate;
    geometry.channels = 64;
    geometry.transform = 2048;

    siggen::SceneSpec spec;
    spec.rate = kHfRate;
    spec.center_hz = 0;
    spec.duration_samples = static_cast<dsp::SampleIndex>(
        std::llround(kSeconds * static_cast<double>(kHfRate)));
    spec.seed = 20260922;
    spec.noise_power_full_band_dbfs = -60.0;
    std::println("groups survey carrier: seed {}", spec.seed);

    siggen::ModulatorSpec carrier;
    carrier.kind = siggen::Modulation::Am;
    carrier.common.rate = kHfRate;
    carrier.common.carrier_offset = kCarrier;
    carrier.common.amplitude = 0.001;
    carrier.am.modulation_index = 0.0;

    siggen::EmitterPlacement placement;
    placement.modulator = carrier;
    placement.use_snr = false;
    placement.start_sample = 0;
    placement.end_sample = spec.duration_samples;
    spec.emitters.push_back(placement);

    auto frames = test::SceneFrames::create(geometry, spec, test::FrontEndModel{});
    if (!frames) {
        FAIL("scene frames: " << frames.error().message);
    }
    detect::DetectorConfig config;
    config.source_rate = kHfRate;
    config.grid_channels = geometry.channels;
    auto detector = detect::Detector::create(config, frames->spectrum());
    if (!detector) {
        FAIL("detector: " << detector.error().message);
    }
    detect::LineGrouper grouper = grouper_at(300);

    std::size_t near_carrier_decisions = 0;
    std::size_t neighbour_decisions = 0;
    dsp::SampleIndex previous = 0;
    for (std::uint64_t i = 0; i < frames->frames_available(); ++i) {
        auto frame = frames->next();
        REQUIRE(frame.has_value());
        REQUIRE(detector->consume(*frame).has_value());
        const dsp::SampleIndex decided = detector->last_decision();
        if (decided == 0 || decided == previous) {
            continue;
        }
        previous = decided;
        REQUIRE(grouper.observe(detector->tracks(), decided).has_value());

        std::size_t close = 0;
        for (const detect::Track& track : detector->tracks()) {
            if (std::abs(track.center - kCarrier) <= 50) {
                ++close;
            }
        }
        near_carrier_decisions += close > 0 ? 1 : 0;
        neighbour_decisions += close > 1 ? 1 : 0;
    }

    for (const detect::Track& track : detector->tracks()) {
        std::println("  #{} {} at {} Hz, {} Hz wide, {:.1f} dB, age {:.1f} s",
                     track.id, detect::track_state_name(track.state), track.center,
                     track.bandwidth, track.snr_2500_db,
                     static_cast<double>(track.age_samples()) / static_cast<double>(kHfRate));
    }
    for (const detect::LineGroup& group : grouper.groups()) {
        std::string line = std::format("  group #{}:", group.id);
        for (const detect::GroupMember& member : grouper.members(group)) {
            line += std::format(" {:+}", member.offset);
        }
        std::println("{}", line);
    }
    std::println("  {} decisions with the carrier tracked, {} of them with a second track "
                 "within 50 Hz; {} tracks born, {} groups formed",
                 near_carrier_decisions, neighbour_decisions, detector->stats().tracks_born,
                 grouper.stats().groups_formed);

    CHECK(near_carrier_decisions > 0);
}
