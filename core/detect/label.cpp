// core/detect/label.h has the rule; this is the table under it.

#include "core/detect/label.h"

namespace revenant::detect {
namespace {

[[nodiscard]] std::string_view psk_name(std::uint32_t order) {
    switch (order) {
        case 2: return "BPSK";
        case 4: return "QPSK";
        case 8: return "8PSK";
        default: return "PSK";
    }
}

[[nodiscard]] std::string_view fsk_name(std::uint32_t tones) {
    switch (tones) {
        case 2: return "2FSK";
        case 4: return "4FSK";
        case 8: return "8FSK";
        default: return "FSK";
    }
}

}  // namespace

const char* label_kind_name(LabelKind kind) {
    switch (kind) {
        case LabelKind::Unknown: return "unknown";
        case LabelKind::AnalogModulation: return "analog modulation";
        case LabelKind::DigitalFamily: return "digital family";
        case LabelKind::Protocol: return "protocol";
    }
    return "unknown";
}

TrackLabel label_track(const Track& track) {
    TrackLabel out;

    if (track.protocol != identify::Protocol::None) {
        out.kind = LabelKind::Protocol;
        out.name = identify::protocol_name(track.protocol);
        out.confidence = track.protocol_confidence;
        out.may_drive = true;
        out.symbol_rate_hz = track.symbol_rate_hz;
        return out;
    }

    // Classification is only ever set from a probe may_drive_detection
    // accepted, so anything but Unknown here may drive.
    out.confidence = track.classification_confidence;
    out.symbol_rate_hz = track.symbol_rate_hz;
    switch (track.classification) {
        case Classification::Unknown:
            // Rule 3: a talker on a suppressed carrier, by its side.
            switch (track.voice_sideband) {
                case characterise::VoiceSideband::Unknown: return TrackLabel{};
                case characterise::VoiceSideband::Upper: out.name = "USB"; break;
                case characterise::VoiceSideband::Lower: out.name = "LSB"; break;
            }
            out.kind = LabelKind::AnalogModulation;
            out.confidence = kVoiceSidebandConfidence;
            out.symbol_rate_hz = 0.0;
            break;
        case Classification::Unmodulated:
            out.kind = LabelKind::AnalogModulation;
            out.name = track.classification_double_sideband ? "AM" : "CW";
            break;
        case Classification::AnalogueFm:
            out.kind = LabelKind::AnalogModulation;
            out.name = track.bandwidth >= kWfmMinimumBandwidthHz ? "WFM" : "NFM";
            break;
        case Classification::Fsk:
            out.kind = LabelKind::DigitalFamily;
            out.name = fsk_name(track.classification_tones);
            break;
        case Classification::Psk:
            out.kind = LabelKind::DigitalFamily;
            out.name = psk_name(track.classification_order);
            break;
        case Classification::Ofdm:
            out.kind = LabelKind::DigitalFamily;
            out.name = "OFDM";
            break;
    }
    out.may_drive = true;
    return out;
}

}  // namespace revenant::detect
