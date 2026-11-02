#include "core/decode/lrpt.h"

#include <memory>
#include <utility>

namespace revenant::decode {

Expected<LrptDecoder> LrptDecoder::create(const LrptConfig& config, LrptEvents events) {
    LrptDecoder d;
    d.apids_ = std::make_unique<std::map<std::uint16_t, LrptApidStats>>();
    CcsdsDownlinkConfig lc;
    lc.sample_rate = config.sample_rate;
    lc.symbol_rate = config.symbol_rate;
    lc.modulation = config.oqpsk ? PskModulation::Oqpsk : PskModulation::Qpsk;
    lc.roll_off = config.roll_off;
    lc.g2_inverted = config.g2_inverted;
    lc.insert_zone_octets = config.insert_zone_octets;
    CcsdsDownlinkEvents le;
    le.on_frame = std::move(events.on_frame);
    auto* apids = d.apids_.get();
    le.on_packet = [apids, cb = std::move(events.on_packet)](std::uint8_t vc, const SpacePacket& p) {
        auto& s = (*apids)[p.header.apid];
        ++s.packets;
        s.octets += p.octets.size();
        s.virtual_channel = vc;
        if (cb) {
            cb(vc, p);
        }
    };
    auto link = CcsdsDownlink::create(lc, std::move(le));
    if (!link) {
        return std::unexpected(link.error());
    }
    d.link_ = std::make_unique<CcsdsDownlink>(std::move(*link));
    return d;
}

void LrptDecoder::push(std::span<const dsp::Complex32> samples) { link_->push(samples); }

void LrptDecoder::finish() { link_->finish(); }

}  // namespace revenant::decode
