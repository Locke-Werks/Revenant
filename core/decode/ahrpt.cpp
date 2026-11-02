#include "core/decode/ahrpt.h"

#include <utility>

namespace revenant::decode {

std::string_view ahrpt_instrument_name(std::uint8_t vc, std::uint16_t apid) {
    // TD 18 table 1. The text extraction of that table interleaves its
    // columns; only pairings that read unambiguously are listed, and the
    // rest of a VC is named by the VC alone.
    switch (vc) {
        case 9:
            return (apid == 103 || apid == 104) ? "AVHRR/3" : "";
        case 3:
            switch (apid) {
                case 39: return "AMSU-A1";
                case 40: return "AMSU-A2";
                case 38: return "HIRS/4";
                case 37: return "SEM";
                default: return "";
            }
        case 27: return "A-DCS";
        case 10: return "IASI";
        case 12: return "MHS";
        case 15: return "ASCAT";
        case 24: return "GOME-2";
        case 29: return "GRAS";
        case 34:
            switch (apid) {
                case 1: return "Satellite housekeeping";
                case 6: return "Admin message";
                case 2:
                case 3: return "GRAS positioning and timing";
                default: return "";
            }
        default: return "";
    }
}

Expected<AhrptDecoder> AhrptDecoder::create(const AhrptConfig& config, AhrptEvents events) {
    CcsdsDownlinkConfig lc;
    lc.sample_rate = config.sample_rate;
    lc.symbol_rate = config.symbol_rate;
    lc.modulation = PskModulation::Qpsk;
    lc.roll_off = config.roll_off;
    lc.puncture = ccsds_puncture_r34();
    lc.g2_inverted = false;  // 131.0 3.4.1 (5)
    CcsdsDownlinkEvents le;
    le.on_frame = std::move(events.on_frame);
    le.on_packet = [cb = std::move(events.on_packet)](std::uint8_t vc, const SpacePacket& p) {
        if (cb) {
            cb(AhrptPacket{vc, ahrpt_instrument_name(vc, p.header.apid), &p});
        }
    };
    auto link = CcsdsDownlink::create(lc, std::move(le));
    if (!link) {
        return std::unexpected(link.error());
    }
    AhrptDecoder d;
    d.link_ = std::make_unique<CcsdsDownlink>(std::move(*link));
    return d;
}

void AhrptDecoder::push(std::span<const dsp::Complex32> samples) { link_->push(samples); }

void AhrptDecoder::finish() { link_->finish(); }

}  // namespace revenant::decode
