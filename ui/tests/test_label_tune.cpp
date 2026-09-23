// label_tune, bracket_text and label_detail: what a detection's label puts on
// its bracket and what a click on it sets.
//
// EVERY TEST HERE NAMES THE WRONG IMPLEMENTATION IT REJECTS, in its own
// comment, for the reason test_audio_ring.cpp gives.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "models/label_tune.h"

using revenant::rpc::DetectionLabel;
using revenant::rpc::LabelKind;
using revenant::ui::bracket_text;
using revenant::ui::label_detail;
using revenant::ui::label_tune;
using revenant::ui::LabelTune;

namespace {

[[nodiscard]] DetectionLabel labelled(LabelKind kind, std::string name, bool may_drive = true) {
    DetectionLabel label;
    label.kind = kind;
    label.name = std::move(name);
    label.confidence = 0.9;
    label.may_drive = may_drive;
    label.probes = 1;
    return label;
}

}  // namespace

// REJECTS: a click that sets a mode from a label it was told not to trust, or
// from one it cannot name.
TEST_CASE("a label that may not drive, or is unknown, sets nothing", "[label-tune]") {
    CHECK_FALSE(label_tune(DetectionLabel{}, 12'000.0).drives);
    CHECK_FALSE(label_tune(labelled(LabelKind::Protocol, "P25", false), 8'100.0).drives);
    CHECK_FALSE(label_tune(labelled(LabelKind::Protocol, "WHATEVER"), 8'100.0).drives);
    CHECK_FALSE(label_tune(labelled(LabelKind::AnalogModulation, "USB"), 2'700.0).drives);
}

// REJECTS: protocols mapped to the width rule's guess, which is what put a
// P25 channel on an NFM receiver before labels existed.
TEST_CASE("a protocol sets its decoder's mode and attaches the decoder", "[label-tune]") {
    const LabelTune p25 = label_tune(labelled(LabelKind::Protocol, "P25"), 8'100.0);
    CHECK(p25.drives);
    CHECK(p25.mode == "p25p1");
    CHECK(p25.decoder == "p25p1");

    const LabelTune pocsag = label_tune(labelled(LabelKind::Protocol, "POCSAG"), 12'000.0);
    CHECK(pocsag.mode == "nfm");
    CHECK(pocsag.decoder == "pocsag");

    const LabelTune rtty = label_tune(labelled(LabelKind::Protocol, "RTTY"), 250.0);
    CHECK(rtty.mode == "usb");
    CHECK(rtty.decoder == "rtty");

    const LabelTune psk31 = label_tune(labelled(LabelKind::Protocol, "PSK31"), 60.0);
    CHECK(psk31.mode == "usb");
    CHECK(psk31.decoder == "psk31");

    const LabelTune m17 = label_tune(labelled(LabelKind::Protocol, "M17"), 9'000.0);
    CHECK(m17.mode == "p25p1");
    CHECK(m17.decoder == "m17");

    // DMR has a mode and, until core/decode/dmr.h lands, no decoder.
    const LabelTune dmr = label_tune(labelled(LabelKind::Protocol, "DMR"), 8'100.0);
    CHECK(dmr.drives);
    CHECK(dmr.mode == "p25p1");
    CHECK(dmr.decoder.empty());
}

// REJECTS: an analogue CW label that attaches the Morse decoder to a carrier
// that was never keyed, and an analogue label that picks a mode other than
// its own name.
TEST_CASE("an analogue label sets its own mode and attaches nothing", "[label-tune]") {
    for (const std::string_view mode : {"AM", "NFM", "WFM", "CW"}) {
        const LabelTune tune =
            label_tune(labelled(LabelKind::AnalogModulation, std::string(mode)), 5'000.0);
        INFO(std::string(mode));
        CHECK(tune.drives);
        CHECK(tune.decoder.empty());
    }
    CHECK(label_tune(labelled(LabelKind::AnalogModulation, "AM"), 6'000.0).mode == "am");
    CHECK(label_tune(labelled(LabelKind::AnalogModulation, "WFM"), 180'000.0).mode == "wfm");
    CHECK(label_tune(labelled(LabelKind::AnalogModulation, "CW"), 60.0).mode == "cw");
}

// REJECTS: a digital family that forces a sideband receiver onto a signal too
// wide for one, and one that leaves a narrow data signal to the FM width rule.
TEST_CASE("a digital family sets usb when narrow and leaves the width rule when wide",
          "[label-tune]") {
    const DetectionLabel bpsk = labelled(LabelKind::DigitalFamily, "BPSK");
    CHECK(label_tune(bpsk, 1'600.0).drives);
    CHECK(label_tune(bpsk, 1'600.0).mode == "usb");
    CHECK_FALSE(label_tune(bpsk, 25'000.0).drives);
}

// REJECTS: a labelled plate carrying the frequency as well, which is three
// times the width and loses plates to collision, and one that prints an empty
// name for an unknown track rather than what the plate always said.
TEST_CASE("the bracket is the name alone, and unlabelled keeps what it had", "[label-tune]") {
    CHECK(bracket_text(labelled(LabelKind::Protocol, "P25"), 462'562'500, 18.2) == "P25");
    CHECK(bracket_text(labelled(LabelKind::AnalogModulation, "NFM"), 146'520'000, 22.0) ==
          "NFM");
    CHECK(bracket_text(DetectionLabel{}, 462'562'500, 18.2) == "462.5625  18.2 dB");
}

// REJECTS: a hover card that says "unknown" for a track nobody has looked at,
// which reads as a verdict, and one that hides whether the label drives.
TEST_CASE("the hover line tells not probed from probed and found nothing", "[label-tune]") {
    CHECK(label_detail(DetectionLabel{}) == "not identified yet");

    DetectionLabel looked;
    looked.probes = 2;
    CHECK(label_detail(looked) == "probed 2 times, nothing identified");

    DetectionLabel p25 = labelled(LabelKind::Protocol, "P25");
    p25.confidence = 0.99;
    CHECK(label_detail(p25) == "P25, protocol, sync verified, 0.99, sets the receiver");

    DetectionLabel bpsk = labelled(LabelKind::DigitalFamily, "BPSK");
    bpsk.symbol_rate_hz = 1199.7;
    CHECK(label_detail(bpsk) == "BPSK, digital family, 0.90, 1200 Bd, sets the receiver");

    // And the line the card prints puts back the frequency and SNR a labelled
    // plate no longer carries.
    CHECK(revenant::ui::hover_line(p25, 149'850'000, 25.04) ==
          "P25, protocol, sync verified, 0.99, sets the receiver  ·  149.8500 MHz, 25.0 dB");
}
