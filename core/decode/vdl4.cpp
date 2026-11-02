#include "core/decode/vdl4.h"

#include <format>
#include <utility>

namespace revenant::decode {

Expected<Vdl4Decoder> Vdl4Decoder::create(const Vdl4Config& config) {
    if (config.rate < kVdl4MinimumRate) {
        return fail(std::format("the VDL Mode 4 decoder needs at least {} samples a second, three "
                                "to each of EN 301 842-1's 19 200 bits, and was given {}",
                                kVdl4MinimumRate, config.rate));
    }
    AisConfig engine;
    engine.rate = config.rate;
    engine.sync_threshold = config.sync_threshold;
    engine.bit_rate = kVdl4BitRate;
    engine.maximum_frame_bits = kVdl4MaximumFrameBits;
    engine.integrate_bits = config.integrate_bits;
    auto decoder = AisDecoder::create(engine);
    if (!decoder) {
        return std::unexpected(with_context(decoder.error(), "VDL Mode 4"));
    }
    return Vdl4Decoder(std::move(*decoder));
}

void Vdl4Decoder::process(ConstRealSpan audio, std::vector<Vdl4Burst>& out) {
    scratch_.clear();
    engine_.process(audio, scratch_);
    for (AisMessage& m : scratch_) {
        Vdl4Burst b;
        b.octets = std::move(m.octets);
        b.first_sample = m.first_sample;
        b.last_sample = m.last_sample;
        b.phase = m.phase;
        out.push_back(std::move(b));
    }
}

}  // namespace revenant::decode
