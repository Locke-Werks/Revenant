// What a detection's label means for the span and for the receiver a click
// tunes: the text on its bracket, the line in the hover card, and the mode and
// decoder the click sets.
//
// THE OWNER'S REQUEST OF 2026-09-23. "The pink block that brackets a signal
// should show what the signal is: modulation if analog, detected digi mode if
// digital, and it should set the receiver accordingly based on that
// information." The engine names the signal, in rpc::Detection::label; this
// is the client's half, the mapping from a name to a receiver.
//
// THE MAPPING, AND WHERE EACH ROW COMES FROM
//
// A protocol goes to the mode its decoder reads and to that decoder, both as
// core/rpc/decoders.h registers them: P25 to p25p1 and the p25p1 decoder,
// D-STAR and TETRA to their own modes, M17 to p25p1, whose 48000 S/s complex
// baseband is what the m17 decoder reads, POCSAG and AX.25 to nfm, RTTY,
// SITOR-B and PSK31 to usb, CW to cw, DMR to dmr and the dmr decoder. RDS
// goes to wfm, whose RDS pane is where RDS is decoded, not the decoder seam.
//
// An analogue label is the mode by name: AM, NFM, WFM, CW, USB and LSB. A CW
// label is a carrier, not a Morse decode, so it attaches nothing; a CW
// PROTOCOL is Morse the engine read, and attaches the cw decoder. USB and LSB
// are a talker on a suppressed carrier and attach nothing either.
//
// A digital family with no protocol sets usb when it is no wider than
// kFamilySidebandMaxHz, so the tones of a narrow data signal land in the audio
// where an operator and a decoder can use them, and otherwise leaves the mode
// to the width rule, ui::demod_for_detection, because an FM receiver on a wide
// digital signal is no worse than today and nothing better is known.
//
// FALLING BACK. A label that is unknown, or that may not drive, or whose name
// this table does not know, sets nothing, and the click does what it did
// before the label existed: the width rule for the mode, the mode's default
// passband, no decoder. So does a click on a receiver whose mode the operator
// already named, by click_chooses_demod in models/receiver_match.h.
//
// This header holds no Qt; ui/tests links it.

#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

#include "core/rpc/types.h"

namespace revenant::ui {

// A digital family no wider than this is data a sideband receiver can carry:
// 3 kHz is the voice channel an upper sideband receiver's default passband
// holds.
inline constexpr double kFamilySidebandMaxHz = 3'000.0;

struct LabelTune {
    // Whether the label chose anything. False is the fall back above.
    bool drives = false;

    // The receiver mode as core/engine/vrx.h spells it, and the decoder by its
    // registry name. The decoder is empty when the label calls for none.
    std::string_view mode;
    std::string_view decoder;
};

namespace label_tune_detail {

struct Row {
    std::string_view name;
    std::string_view mode;
    std::string_view decoder;
};

// Protocols, by the names core/identify/identify.cpp's protocol_name gives.
inline constexpr std::array<Row, 12> kProtocols = {{
    {"P25", "p25p1", "p25p1"},
    {"D-STAR", "dstar", "dstar"},
    {"TETRA", "tetra", "tetra"},
    {"M17", "p25p1", "m17"},
    {"DMR", "dmr", "dmr"},
    {"POCSAG", "nfm", "pocsag"},
    {"AX.25", "nfm", "ax25"},
    {"RTTY", "usb", "rtty"},
    {"SITOR-B", "usb", "sitor_b"},
    {"PSK31", "usb", "psk31"},
    {"CW", "cw", "cw"},
    {"RDS", "wfm", ""},
}};

// Analogue modulations, by the names core/detect/label.cpp gives.
inline constexpr std::array<Row, 6> kAnalogue = {{
    {"AM", "am", ""},
    {"NFM", "nfm", ""},
    {"WFM", "wfm", ""},
    {"CW", "cw", ""},
    {"USB", "usb", ""},
    {"LSB", "lsb", ""},
}};

template <std::size_t N>
[[nodiscard]] constexpr const Row* find(const std::array<Row, N>& rows, std::string_view name)
{
    for (const Row& row : rows) {
        if (row.name == name) {
            return &row;
        }
    }
    return nullptr;
}

}  // namespace label_tune_detail

// What a click on a detection with this label sets. `occupied_hz` is the
// detection's measured width, which only a digital family reads.
[[nodiscard]] constexpr LabelTune label_tune(const rpc::DetectionLabel& label, double occupied_hz)
{
    using namespace label_tune_detail;
    LabelTune out;
    if (!label.may_drive) {
        return out;
    }
    switch (label.kind) {
        case rpc::LabelKind::Unknown: return out;
        case rpc::LabelKind::Protocol:
            if (const Row* row = find(kProtocols, label.name); row != nullptr) {
                out = LabelTune{true, row->mode, row->decoder};
            }
            return out;
        case rpc::LabelKind::AnalogModulation:
            if (const Row* row = find(kAnalogue, label.name); row != nullptr) {
                out = LabelTune{true, row->mode, row->decoder};
            }
            return out;
        case rpc::LabelKind::DigitalFamily:
            if (occupied_hz > 0.0 && occupied_hz <= kFamilySidebandMaxHz) {
                out = LabelTune{true, "usb", ""};
            }
            return out;
    }
    return out;
}

// The text on a detection's bracket. A labelled detection's plate is its name
// and nothing else, because plates that collide are dropped, the weaker
// first, and a name is a third the width of a name and a frequency: on the
// labelled scene at 1280 pixels, "P25  149.8500" plates showed seven of eleven
// labels and the names alone one for every emitter. The frequency is on the
// ruler under the bracket and in the hover card. An unlabelled detection keeps the frequency
// and its SNR, which is what the plate said before labels existed.
[[nodiscard]] inline std::string bracket_text(const rpc::DetectionLabel& label,
                                              std::int64_t center_hz, double snr_2500_db)
{
    if (label.kind != rpc::LabelKind::Unknown && !label.name.empty()) {
        return label.name;
    }
    return std::format("{:.4f}  {:.1f} dB", static_cast<double>(center_hz) / 1.0e6,
                       snr_2500_db);
}

// The hover card's line for the label, which carries what the bracket has no
// room for: the kind, the confidence, the symbol rate, whether it may set the
// receiver, and how many probes have looked. The three unknown states are
// three different sentences, because "nobody has looked" and "looked and
// found nothing" are different things to tell an operator.
[[nodiscard]] inline std::string label_detail(const rpc::DetectionLabel& label)
{
    const auto kind = [&]() -> std::string_view {
        switch (label.kind) {
            case rpc::LabelKind::Unknown: return "unknown";
            case rpc::LabelKind::AnalogModulation: return "analog modulation";
            case rpc::LabelKind::DigitalFamily: return "digital family";
            case rpc::LabelKind::Protocol: return "protocol, sync verified";
        }
        return "unknown";
    }();

    if (label.kind == rpc::LabelKind::Unknown) {
        if (label.probes == 0) {
            return "not identified yet";
        }
        return std::format("probed {} time{}, nothing identified", label.probes,
                           label.probes == 1 ? "" : "s");
    }
    std::string out = std::format("{}, {}, {:.2f}", label.name, kind, label.confidence);
    if (label.symbol_rate_hz > 0.0) {
        out += std::format(", {:.0f} Bd", label.symbol_rate_hz);
    }
    out += label.may_drive ? ", sets the receiver" : ", does not set the receiver";
    return out;
}

// The hover card's first line: the label's detail, then the frequency and the
// SNR a labelled plate no longer carries.
[[nodiscard]] inline std::string hover_line(const rpc::DetectionLabel& label,
                                            std::int64_t center_hz, double snr_2500_db)
{
    return label_detail(label) +
           std::format("  ·  {:.4f} MHz, {:.1f} dB", static_cast<double>(center_hz) / 1.0e6,
                       snr_2500_db);
}

}  // namespace revenant::ui
