#include "tools/webgate/audio_opus.h"

#include <opus.h>

#include <cstddef>
#include <string>
#include <utility>

namespace revenant::webgate {

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr int kFrameSamples = 960;
constexpr opus_int32 kBitrate = 32000;
// Far above the size of a 32 kbit/s frame; the encoder returns what it needs.
constexpr int kMaxPacketBytes = 1275;

}  // namespace

struct OpusEncoder::Impl {
    ::OpusEncoder* enc = nullptr;
    std::vector<float> pending;
    std::vector<std::vector<std::uint8_t>> packets;

    ~Impl() {
        if (enc != nullptr) {
            opus_encoder_destroy(enc);
        }
    }
};

OpusEncoder::OpusEncoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

OpusEncoder::~OpusEncoder() = default;

Expected<std::unique_ptr<OpusEncoder>> OpusEncoder::create(std::uint32_t sample_rate) {
    if (sample_rate != kSampleRate) {
        return fail("Opus encoding needs 48000 Hz audio, got " + std::to_string(sample_rate) +
                    " Hz");
    }
    auto impl = std::make_unique<Impl>();
    int err = OPUS_OK;
    impl->enc = opus_encoder_create(static_cast<opus_int32>(sample_rate), 1,
                                    OPUS_APPLICATION_AUDIO, &err);
    if (err != OPUS_OK || impl->enc == nullptr) {
        return fail(std::string("Could not create the Opus encoder: ") + opus_strerror(err));
    }
    err = opus_encoder_ctl(impl->enc, OPUS_SET_BITRATE(kBitrate));
    if (err != OPUS_OK) {
        return fail(std::string("Could not set the Opus bitrate: ") + opus_strerror(err));
    }
    impl->pending.reserve(kFrameSamples * 2);
    return std::unique_ptr<OpusEncoder>(new OpusEncoder(std::move(impl)));
}

void OpusEncoder::push(std::span<const float> samples) {
    auto& p = *impl_;
    p.pending.insert(p.pending.end(), samples.begin(), samples.end());

    std::size_t used = 0;
    while (p.pending.size() - used >= static_cast<std::size_t>(kFrameSamples)) {
        std::vector<std::uint8_t> packet(kMaxPacketBytes);
        const int n = opus_encode_float(p.enc, p.pending.data() + used, kFrameSamples,
                                        packet.data(), static_cast<opus_int32>(packet.size()));
        used += kFrameSamples;
        // A failed frame is dropped rather than thrown: this runs on the audio
        // path, and a gap of 20 ms is better than a dead stream.
        if (n > 0) {
            packet.resize(static_cast<std::size_t>(n));
            p.packets.push_back(std::move(packet));
        }
    }
    p.pending.erase(p.pending.begin(), p.pending.begin() + static_cast<std::ptrdiff_t>(used));
}

std::vector<std::vector<std::uint8_t>> OpusEncoder::take_packets() {
    return std::exchange(impl_->packets, {});
}

}  // namespace revenant::webgate
