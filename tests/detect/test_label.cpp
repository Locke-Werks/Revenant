// The label a track carries to the bracket on the span, from hand-built
// tracks, so each case states exactly which fields it set.

#include <catch2/catch_test_macros.hpp>

#include "core/detect/label.h"

using namespace revenant;
using detect::Classification;
using detect::LabelKind;

namespace {

[[nodiscard]] detect::Track track_with(Classification family, double confidence = 0.8) {
    detect::Track track;
    track.id = 1;
    track.state = detect::TrackState::Live;
    track.center = 146'520'000;
    track.bandwidth = 11'000;
    track.classification = family;
    track.classification_confidence = confidence;
    return track;
}

}  // namespace

// REJECTS: a label that fills unknown with the nearest name. docs/
// detection.md makes unknown a real answer and a track nothing named gets none.
TEST_CASE("an unclassified track has no label and may not drive", "[detect][label]") {
    detect::Track track = track_with(Classification::Unknown, 0.0);
    track.probes = 3;
    track.last_probe.family = Classification::Psk;  // refused, so not reported
    const detect::TrackLabel label = detect::label_track(track);
    CHECK(label.kind == LabelKind::Unknown);
    CHECK(label.name.empty());
    CHECK_FALSE(label.may_drive);
    CHECK(label.confidence == 0.0);
}

// REJECTS: a label that lets the family overrule a verified sync, or that
// needs a family before it will name a protocol at all.
TEST_CASE("a verified protocol names the track whatever the family said", "[detect][label]") {
    detect::Track track = track_with(Classification::AnalogueFm, 0.5);
    track.protocol = identify::Protocol::P25Phase1;
    track.protocol_confidence = 0.99;
    const detect::TrackLabel label = detect::label_track(track);
    CHECK(label.kind == LabelKind::Protocol);
    CHECK(label.name == "P25");
    CHECK(label.confidence == 0.99);
    CHECK(label.may_drive);

    detect::Track bare = track_with(Classification::Unknown, 0.0);
    bare.protocol = identify::Protocol::Pocsag;
    bare.protocol_confidence = 0.95;
    const detect::TrackLabel alone = detect::label_track(bare);
    CHECK(alone.kind == LabelKind::Protocol);
    CHECK(alone.name == "POCSAG");
    CHECK(alone.may_drive);
}

TEST_CASE("analogue families name a modulation", "[detect][label]") {
    SECTION("a bare carrier is CW, and one with mirrored sidebands is AM") {
        detect::Track carrier = track_with(Classification::Unmodulated);
        carrier.bandwidth = 60;
        CHECK(detect::label_track(carrier).name == "CW");
        CHECK(detect::label_track(carrier).kind == LabelKind::AnalogModulation);

        detect::Track am = track_with(Classification::Unmodulated);
        am.bandwidth = 6'000;
        am.classification_double_sideband = true;
        CHECK(detect::label_track(am).name == "AM");
    }
    SECTION("analogue FM is NFM or WFM by the track's width") {
        detect::Track nfm = track_with(Classification::AnalogueFm);
        nfm.bandwidth = 11'000;
        CHECK(detect::label_track(nfm).name == "NFM");

        detect::Track wfm = track_with(Classification::AnalogueFm);
        wfm.bandwidth = detect::kWfmMinimumBandwidthHz;
        CHECK(detect::label_track(wfm).name == "WFM");
        CHECK(detect::label_track(wfm).may_drive);
    }
}

TEST_CASE("digital families name the family with its order or tone count", "[detect][label]") {
    detect::Track psk = track_with(Classification::Psk);
    psk.classification_order = 2;
    psk.symbol_rate_hz = 1200.0;
    const detect::TrackLabel bpsk = detect::label_track(psk);
    CHECK(bpsk.kind == LabelKind::DigitalFamily);
    CHECK(bpsk.name == "BPSK");
    CHECK(bpsk.symbol_rate_hz == 1200.0);

    psk.classification_order = 4;
    CHECK(detect::label_track(psk).name == "QPSK");
    psk.classification_order = 0;
    CHECK(detect::label_track(psk).name == "PSK");

    detect::Track fsk = track_with(Classification::Fsk);
    fsk.classification_tones = 4;
    CHECK(detect::label_track(fsk).name == "4FSK");
    fsk.classification_tones = 0;
    CHECK(detect::label_track(fsk).name == "FSK");

    CHECK(detect::label_track(track_with(Classification::Ofdm)).name == "OFDM");
}

// REJECTS: a talker on single sideband left unlabelled once the probe read its
// side, a side read that overrules an accepted family or a verified protocol,
// and a sideband label that claims more than the half the characteriser's own
// analogue FM calls are held to.
TEST_CASE("a talker's side names USB or LSB when no family was accepted", "[detect][label]") {
    detect::Track upper = track_with(Classification::Unknown, 0.0);
    upper.probes = 2;
    upper.voice_sideband = characterise::VoiceSideband::Upper;
    const detect::TrackLabel usb = detect::label_track(upper);
    CHECK(usb.kind == LabelKind::AnalogModulation);
    CHECK(usb.name == "USB");
    CHECK(usb.may_drive);
    CHECK(usb.confidence == detect::kVoiceSidebandConfidence);
    CHECK(usb.symbol_rate_hz == 0.0);

    detect::Track lower = upper;
    lower.voice_sideband = characterise::VoiceSideband::Lower;
    CHECK(detect::label_track(lower).name == "LSB");

    detect::Track carrier = track_with(Classification::Unmodulated);
    carrier.classification_double_sideband = true;
    carrier.voice_sideband = characterise::VoiceSideband::Upper;
    CHECK(detect::label_track(carrier).name == "AM");

    detect::Track framed = upper;
    framed.protocol = identify::Protocol::Rtty;
    framed.protocol_confidence = 0.9;
    CHECK(detect::label_track(framed).kind == LabelKind::Protocol);
}
