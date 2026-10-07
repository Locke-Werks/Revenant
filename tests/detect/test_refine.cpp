// The label tree of core/detect/refine.h: promotion one level at a time,
// demotion back to the parent, hysteresis, per-emitter memory, and the P25
// census telling a control channel from a voice channel on synthetic C4FM.
//
// The readings are written by hand so the arithmetic is what is under test
// and not the characteriser. The census case is the exception: it runs the
// real P25 row of core/identify on modulated IQ, because what it asserts is
// that the row reports the data units the refinement relies on.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "core/detect/label.h"
#include "core/detect/refine.h"
#include "core/dsp/synth/dv_mod.h"
#include "core/identify/identify.h"

using namespace revenant;
using detect::refine::Continuity;
using detect::refine::Node;
using detect::refine::Reading;
using detect::refine::Refinement;
using characterise::ModulationFamily;

namespace {

[[nodiscard]] Reading family(ModulationFamily which, double confidence, double bandwidth_hz,
                             std::uint32_t tones = 0) {
    Reading reading;
    reading.family = which;
    reading.family_confidence = confidence;
    reading.bandwidth_hz = bandwidth_hz;
    reading.tones = tones;
    return reading;
}

[[nodiscard]] Reading fsk4(double confidence = 0.7) {
    Reading reading = family(ModulationFamily::Fsk, confidence, 9'000.0, 4);
    reading.symbol_rate_hz = 4800.0;
    return reading;
}

[[nodiscard]] Reading p25(std::uint32_t tsbk, std::uint32_t voice, std::uint16_t nac = 0xD10) {
    Reading reading = fsk4();
    reading.protocol = identify::Protocol::P25Phase1;
    reading.protocol_confidence = 0.95;
    reading.p25.tsbk = tsbk;
    reading.p25.voice = voice;
    reading.p25.nac = nac;
    reading.p25.nac_steady = true;
    return reading;
}

[[nodiscard]] std::uint32_t bit(identify::Protocol protocol) {
    return 1U << static_cast<unsigned>(protocol);
}

[[nodiscard]] std::string path_of(const detect::TrackLabel& label) {
    std::string out;
    for (std::size_t i = 0; i < label.path_size; ++i) {
        out += (i > 0 ? " > " : "") + std::string(label.path[i].name) +
               (label.path[i].confirmed ? "" : "?");
    }
    return out;
}

[[nodiscard]] detect::TrackLabel label_of(const Refinement& refinement) {
    detect::Track track;
    track.refinement = refinement;
    return detect::label_track(track);
}

}  // namespace

TEST_CASE("the tree reads from the obvious to the specific", "[detect][refine]") {
    CHECK(detect::refine::parent_of(Node::P25Control) == Node::P25);
    CHECK(detect::refine::parent_of(Node::P25) == Node::NfmFsk4);
    CHECK(detect::refine::parent_of(Node::NfmFsk4) == Node::Nfm);
    CHECK(detect::refine::parent_of(Node::Nfm) == Node::Root);
    CHECK(detect::refine::depth_of(Node::P25Control) == 4);
    CHECK(detect::refine::node_of(identify::Protocol::P25Phase1) == Node::P25);
    CHECK(detect::refine::node_of(identify::Protocol::Cw) == Node::Morse);
    CHECK(detect::refine::node_info(Node::P25Control).name == "P25 control");
    CHECK(detect::refine::node_info(Node::P25Control).step == "control");
    CHECK_FALSE(detect::refine::has_children(Node::P25Voice));
    CHECK(detect::refine::has_children(Node::P25));

    // Every node but the root reaches the root, so no cycle hides in the table.
    for (std::size_t n = 1; n < detect::refine::kNodeCount; ++n) {
        CHECK(detect::refine::depth_of(static_cast<Node>(n)) <= detect::kMaxLabelPath - 1);
    }
}

// REJECTS: a level shown on a single weak reading, and a deeper level shown
// before the one above it.
TEST_CASE("a level is shown only once its own confidence passes the bar", "[detect][refine]") {
    Refinement r;
    detect::refine::observe(r, family(ModulationFamily::AnalogueFm, 0.5, 11'000.0));
    CHECK(r.deepest() == Node::Root);
    CHECK(r.confidence(Node::Nfm) < detect::refine::kPromote);

    // The stopwatch: the same reading again confirms it.
    detect::refine::observe(r, family(ModulationFamily::AnalogueFm, 0.5, 11'000.0));
    CHECK(r.deepest() == Node::Nfm);
    const double after_two = r.confidence(Node::Nfm);
    detect::refine::observe(r, family(ModulationFamily::AnalogueFm, 0.5, 11'000.0));
    CHECK(r.confidence(Node::Nfm) > after_two);
}

// The owner's P25 example, in his order: NFM, then P25, then P25 control.
TEST_CASE("a P25 control channel drills down NFM, P25, control", "[detect][refine]") {
    Refinement r;
    detect::refine::observe(r, family(ModulationFamily::AnalogueFm, 0.8, 11'000.0));
    CHECK(r.deepest() == Node::Nfm);
    CHECK(label_of(r).name == "NFM");

    detect::refine::observe(r, p25(0, 0));
    CHECK(r.deepest() == Node::P25);
    CHECK(label_of(r).name == "P25");
    CHECK(label_of(r).kind == detect::LabelKind::Protocol);

    detect::refine::observe(r, p25(12, 0));
    const detect::TrackLabel label = label_of(r);
    CHECK(r.deepest() == Node::P25Control);
    CHECK(label.name == "P25 control");
    CHECK(path_of(label) == "NFM > 4FSK > P25 > control");
    CHECK(label.confirmed_depth == 4);
    for (std::size_t i = 0; i < label.path_size; ++i) {
        CHECK(label.path[i].confidence >= detect::refine::kPromote);
    }
}

TEST_CASE("a P25 voice channel's census lands on voice", "[detect][refine]") {
    Refinement r;
    detect::refine::observe(r, p25(0, 9));
    CHECK(r.deepest() == Node::P25Voice);
    CHECK(label_of(r).name == "P25 voice");
}

// REJECTS: a census read across two NACs, and role evidence kept across a
// change of system on the same frequency.
TEST_CASE("the P25 role needs a steady NAC and resets on a new one", "[detect][refine]") {
    Refinement r;
    Reading mixed = p25(12, 0);
    mixed.p25.nac_steady = false;
    detect::refine::observe(r, mixed);
    CHECK(r.deepest() == Node::P25);

    detect::refine::observe(r, p25(12, 0, 0xD10));
    CHECK(r.deepest() == Node::P25Control);
    detect::refine::observe(r, p25(0, 9, 0x293));
    CHECK(r.deepest() == Node::P25Voice);
    CHECK(r.p25_nac == 0x293);
}

// The owner's other property: a signal misread early keeps gathering evidence
// and lands on the right leaf.
TEST_CASE("an emitter misread as 2FSK converges on P25 control", "[detect][refine]") {
    Refinement r;
    for (int i = 0; i < 3; ++i) {
        detect::refine::observe(r, family(ModulationFamily::Fsk, 0.7, 9'000.0, 2));
    }
    CHECK(r.deepest() == Node::NfmFsk2);
    CHECK(label_of(r).name == "2FSK");

    // Correct readings arrive. The P25 row does not verify on the first two,
    // as on a weak signal, and the family alone has to turn the 2FSK call.
    std::vector<std::string> names;
    for (int i = 0; i < 4; ++i) {
        Reading reading = fsk4(0.7);
        reading.protocols_unverified = bit(identify::Protocol::P25Phase1);
        detect::refine::observe(r, reading);
        names.emplace_back(label_of(r).name);
    }
    CHECK(r.deepest() == Node::NfmFsk4);
    CHECK_FALSE(r.nodes[static_cast<std::size_t>(Node::NfmFsk2)].confirmed);

    detect::refine::observe(r, p25(0, 0));
    detect::refine::observe(r, p25(10, 0));
    CHECK(r.deepest() == Node::P25Control);

    // On the way, the label went 2FSK, through NFM or straight to 4FSK, and
    // never anywhere outside that branch.
    for (const std::string& name : names) {
        INFO(name);
        CHECK((name == "2FSK" || name == "NFM" || name == "4FSK"));
    }
}

// REJECTS: a wrong deeper guess that sticks. A P25 call that stops verifying
// falls back to 4FSK; Track::protocol still holding it does not matter.
TEST_CASE("a protocol that stops verifying falls back to its parent", "[detect][refine]") {
    Refinement r;
    detect::refine::observe(r, p25(0, 0));
    REQUIRE(r.deepest() == Node::P25);

    Reading miss = fsk4(0.8);
    miss.protocols_unverified = bit(identify::Protocol::P25Phase1) | bit(identify::Protocol::Dmr);
    detect::refine::observe(r, miss);
    CHECK(r.deepest() == Node::P25);  // one miss is not enough

    for (int i = 0; i < 3; ++i) {
        detect::refine::observe(r, miss);
    }
    CHECK(r.deepest() == Node::NfmFsk4);

    detect::Track track;
    track.refinement = r;
    track.protocol = identify::Protocol::P25Phase1;
    track.protocol_confidence = 0.95;
    CHECK(detect::label_track(track).name == "4FSK");
}

// REJECTS: a label that flickers when the readings alternate.
TEST_CASE("alternating readings do not flicker the label", "[detect][refine]") {
    Refinement r;
    for (int i = 0; i < 4; ++i) {
        detect::refine::observe(r, family(ModulationFamily::Fsk, 0.8, 9'000.0, 4));
    }
    REQUIRE(r.deepest() == Node::NfmFsk4);

    std::string last(label_of(r).name);
    int changes = 0;
    for (int i = 0; i < 12; ++i) {
        const std::uint32_t tones = i % 2 == 0 ? 2 : 4;
        detect::refine::observe(r, family(ModulationFamily::Fsk, 0.8, 9'000.0, tones));
        const std::string now(label_of(r).name);
        if (now != last) {
            ++changes;
            last = now;
        }
    }
    INFO("changes " << changes);
    CHECK(changes <= 1);
}

TEST_CASE("continuity alone confirms a role only over several probes", "[detect][refine]") {
    Refinement r;
    detect::refine::observe(r, p25(0, 0));
    REQUIRE(r.deepest() == Node::P25);

    Reading steady = fsk4();
    steady.continuity = Continuity::Continuous;
    detect::refine::observe(r, steady);
    CHECK(r.deepest() == Node::P25);
    for (int i = 0; i < 4; ++i) {
        detect::refine::observe(r, steady);
    }
    CHECK(r.deepest() == Node::P25Control);

    // And a continuity reading means nothing until P25 is believed.
    Refinement analogue;
    Reading carrier = family(ModulationFamily::AnalogueFm, 0.8, 11'000.0);
    carrier.continuity = Continuity::Continuous;
    for (int i = 0; i < 6; ++i) {
        detect::refine::observe(analogue, carrier);
    }
    CHECK(analogue.deepest() == Node::Nfm);
    CHECK(analogue.nodes[static_cast<std::size_t>(Node::P25Control)].support == 0.0F);
}

TEST_CASE("NXDN is named only when every other consistent row was ruled out",
          "[detect][refine]") {
    // 4FSK at 2400 in 6.25 kHz is NXDN or dPMR, and dPMR has no decoder, so
    // nothing rules it out: no NXDN.
    Refinement narrow;
    Reading reading = family(ModulationFamily::Fsk, 0.8, 6'000.0, 4);
    reading.symbol_rate_hz = 2400.0;
    for (int i = 0; i < 4; ++i) {
        detect::refine::observe(narrow, reading);
    }
    CHECK(narrow.nodes[static_cast<std::size_t>(Node::Nxdn)].support == 0.0F);
    CHECK(narrow.deepest() == Node::NfmFsk4);
}

TEST_CASE("the hover path carries the next level that has evidence", "[detect][refine]") {
    Refinement r;
    detect::refine::observe(r, family(ModulationFamily::AnalogueFm, 0.9, 11'000.0));
    Reading continuous = fsk4(0.35);
    detect::refine::observe(r, continuous);
    const detect::TrackLabel label = label_of(r);
    REQUIRE(label.confirmed_depth == 1);
    CHECK(path_of(label) == "NFM > 4FSK?");
    CHECK_FALSE(label.path[1].confirmed);
}

TEST_CASE("an unrefined track keeps the one-probe label", "[detect][refine]") {
    detect::Track track;
    track.classification = detect::Classification::Psk;
    track.classification_confidence = 0.7;
    track.classification_order = 2;
    const detect::TrackLabel label = detect::label_track(track);
    CHECK(label.name == "BPSK");
    CHECK(label.path_size == 0);
    CHECK(label.confirmed_depth == 0);
}

// ---- per emitter ------------------------------------------------------------

TEST_CASE("the emitter book matches by frequency and keeps adjacent channels apart",
          "[detect][refine]") {
    detect::refine::EmitterBook book;
    detect::refine::EmitterMemory& a = book.remember(853'050'100, 9'000, 0);
    const std::uint64_t a_id = a.id;
    detect::refine::EmitterMemory& b = book.remember(853'062'600, 9'000, 0);
    const std::uint64_t b_id = b.id;

    REQUIRE(book.match(853'050'900, 8'500) != nullptr);
    CHECK(book.match(853'050'900, 8'500)->id == a_id);
    REQUIRE(book.match(853'062'000, 9'000) != nullptr);
    CHECK(book.match(853'062'000, 9'000)->id == b_id);
    CHECK(book.match(853'056'350, 9'000) == nullptr);

    book.forget_older_than(1'000'000, 10);
    CHECK(book.size() == 0);
}

// A repeater: each keyup is a new track that lives for one probe. The memory
// is what lets the evidence add up across them.
TEST_CASE("evidence accumulates across keyups through the emitter book", "[detect][refine]") {
    detect::refine::EmitterBook book;
    std::uint64_t emitter_id = 0;
    for (int keyup = 0; keyup < 4; ++keyup) {
        detect::Track track;
        track.center = 146'940'000 + (keyup % 2 == 0 ? 300 : -300);
        track.bandwidth = 11'000;
        if (detect::refine::EmitterMemory* known = book.match(track.center, track.bandwidth)) {
            track.refinement = known->refinement;
            emitter_id = known->id;
        } else {
            emitter_id = book.remember(track.center, track.bandwidth, 0).id;
        }
        // Weak readings: none of them would confirm NFM on its own.
        detect::refine::observe(track.refinement,
                                family(ModulationFamily::AnalogueFm, 0.45, 11'000.0));
        book.find(emitter_id)->refinement = track.refinement;
    }
    REQUIRE(book.size() == 1);
    CHECK(book.find(emitter_id)->refinement.observations == 4);
    CHECK(book.find(emitter_id)->refinement.deepest() == Node::Nfm);
}

// ---- the census on real C4FM ------------------------------------------------

namespace {

constexpr dsp::SampleRate kRate = 48'000;

[[nodiscard]] std::vector<dsp::Complex32> with_noise(std::vector<dsp::Complex32> samples,
                                                     double snr_db, std::uint64_t seed) {
    std::mt19937_64 generator(seed);
    // The signal is unit amplitude; the noise is set over the 48 kHz band,
    // which puts the stated SNR well under the one in a 12.5 kHz channel.
    const double sigma = std::sqrt(std::pow(10.0, -snr_db / 10.0) / 2.0);
    std::normal_distribution<double> noise(0.0, sigma);
    for (dsp::Complex32& sample : samples) {
        sample += dsp::Complex32(static_cast<float>(noise(generator)),
                                 static_cast<float>(noise(generator)));
    }
    return samples;
}

[[nodiscard]] identify::Identification identify_of(const std::vector<dsp::Complex32>& samples) {
    identify::IdentifyHints hints;
    hints.occupied_hz = 8'100.0;
    identify::IdentifyConfig config;
    config.rate = kRate;
    auto result = identify::identify(dsp::ConstComplexSpan(samples), hints, config);
    REQUIRE(result.has_value());
    return *result;
}

[[nodiscard]] Reading reading_of(const identify::Identification& found) {
    Reading reading = fsk4();
    reading.protocol = found.protocol;
    reading.protocol_confidence = found.confidence;
    for (const identify::Attempt& attempt : found.attempts) {
        if (attempt.result == identify::AttemptResult::NotVerified) {
            reading.protocols_unverified |= bit(attempt.protocol);
        }
    }
    reading.p25 = found.p25;
    return reading;
}

}  // namespace

TEST_CASE("the P25 row's census tells a control channel from a voice channel",
          "[detect][refine][p25]") {
    // A control channel: TSDUs back to back for two seconds, NAC 0xD10, the
    // local system's. The blocks' contents are arbitrary opcodes; only their
    // CRC matters to the census.
    siggen::P25TsduMessage tsdu;
    tsdu.network_access_code = 0xD10;
    tsdu.blocks.push_back({0x3A, 0x00, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0});
    tsdu.blocks.push_back({0x3B, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
    std::vector<std::uint8_t> control_dibits;
    while (control_dibits.size() < 2 * 4800) {
        auto one = siggen::p25_tsdu_dibits(tsdu);
        REQUIRE(one.has_value());
        control_dibits.insert(control_dibits.end(), one->begin(), one->end());
    }
    siggen::P25ModConfig mod;
    mod.rate = kRate;
    auto control = siggen::p25_render_dibits(mod, control_dibits);
    REQUIRE(control.has_value());

    // A voice channel: a header and LDUs, as a keyup sends them.
    siggen::P25VoiceMessage voice;
    voice.network_access_code = 0xD10;
    voice.header = decode::P25Header{};
    std::uint32_t state = 0x2468ACEU;
    for (std::size_t f = 0; f < 18 * 9; ++f) {
        std::array<std::uint8_t, decode::kP25VoiceFrameBits> frame{};
        for (std::uint8_t& b : frame) {
            state = state * 1103515245U + 12345U;
            b = static_cast<std::uint8_t>((state >> 16) & 1U);
        }
        voice.voice.push_back(frame);
    }
    auto voice_dibits = siggen::p25_voice_message_dibits(voice);
    REQUIRE(voice_dibits.has_value());
    auto voiced = siggen::p25_render_dibits(mod, *voice_dibits);
    REQUIRE(voiced.has_value());

    const identify::Identification on_control = identify_of(with_noise(*control, 15.0, 7));
    const identify::Identification on_voice = identify_of(with_noise(*voiced, 15.0, 8));
    INFO("control: tsbk " << on_control.p25.tsbk << " voice " << on_control.p25.voice
                          << " nac " << on_control.p25.nac);
    INFO("voice: tsbk " << on_voice.p25.tsbk << " voice " << on_voice.p25.voice << " nac "
                        << on_voice.p25.nac);

    CHECK(on_control.protocol == identify::Protocol::P25Phase1);
    CHECK(on_control.p25.tsbk >= detect::refine::kCensusMinimum);
    CHECK(on_control.p25.voice == 0);
    CHECK(on_control.p25.nac == 0xD10);
    CHECK(on_control.p25.nac_steady);

    CHECK(on_voice.protocol == identify::Protocol::P25Phase1);
    CHECK(on_voice.p25.voice >= detect::refine::kCensusMinimum);
    CHECK(on_voice.p25.tsbk == 0);

    Refinement control_refinement;
    detect::refine::observe(control_refinement, reading_of(on_control));
    CHECK(label_of(control_refinement).name == "P25 control");

    Refinement voice_refinement;
    detect::refine::observe(voice_refinement, reading_of(on_voice));
    CHECK(label_of(voice_refinement).name == "P25 voice");
}
