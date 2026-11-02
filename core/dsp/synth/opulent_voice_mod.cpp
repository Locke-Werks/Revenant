#include "core/dsp/synth/opulent_voice_mod.h"

#include <format>

namespace revenant::siggen {

Expected<std::vector<std::uint8_t>> opv_frame_bits(
    std::span<const std::array<std::uint8_t, decode::kOpvFrameBytes>> frames) {
    std::vector<std::uint8_t> out;
    out.reserve(frames.size() * decode::kOpvFrameBits);
    for (const auto& frame : frames) {
        auto bits = decode::opv_encode_frame(frame);
        if (!bits) {
            return std::unexpected(bits.error());
        }
        out.insert(out.end(), bits->begin(), bits->end());
    }
    return out;
}

Expected<std::array<std::uint8_t, decode::kOpvFrameBytes>> opv_make_frame(
    std::string_view station, std::span<const std::uint8_t> payload) {
    if (payload.size() > decode::kOpvPayloadBytes) {
        return fail(std::format("an OPV payload is at most {} bytes (OPV 2.1); got {}",
                                decode::kOpvPayloadBytes, payload.size()));
    }
    auto value = decode::opv_encode_station(station);
    if (!value) {
        return std::unexpected(value.error());
    }
    std::array<std::uint8_t, decode::kOpvFrameBytes> frame{};
    for (std::size_t i = 0; i < 6; ++i) {
        frame[i] = static_cast<std::uint8_t>(*value >> (8U * (5U - i)));
    }
    frame[6] = static_cast<std::uint8_t>(decode::kOpvDefaultToken >> 16U);
    frame[7] = static_cast<std::uint8_t>(decode::kOpvDefaultToken >> 8U);
    frame[8] = static_cast<std::uint8_t>(decode::kOpvDefaultToken);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        frame[decode::kOpvHeaderBytes + i] = payload[i];
    }
    return frame;
}

std::vector<Complex32> opv_render_antipodal(std::span<const std::uint8_t> bits) {
    std::vector<Complex32> out;
    out.reserve(bits.size());
    for (const std::uint8_t bit : bits) {
        out.emplace_back(bit != 0U ? -1.0F : 1.0F, 0.0F);
    }
    return out;
}

}  // namespace revenant::siggen
