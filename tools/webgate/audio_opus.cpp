#include "tools/webgate/audio_opus.h"

#include <opus.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <deque>
#include <iterator>
#include <string>
#include <utility>

namespace revenant::webgate {

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr int kFrameSamples = 960;
constexpr opus_int32 kBitrate = 32000;
// The largest packet Opus can produce; the encoder returns what it needs.
constexpr int kMaxPacketBytes = 1275;

// Sets one encoder control and reads it back, so a build whose default differs
// or a refused setting is an error here rather than a surprise on the air.
Status set_and_check(::OpusEncoder* enc, const char* name, int set_request, int get_request,
                     opus_int32 value) {
    int err = opus_encoder_ctl(enc, set_request, value);
    if (err != OPUS_OK) {
        return fail(std::string("Could not set Opus ") + name + ": " + opus_strerror(err));
    }
    opus_int32 got = 0;
    err = opus_encoder_ctl(enc, get_request, &got);
    if (err != OPUS_OK || got != value) {
        return fail(std::string("Opus ") + name + " did not take the requested value");
    }
    return {};
}

}  // namespace

struct LiveOpusEncoder::Impl {
    ::OpusEncoder* enc = nullptr;
    std::vector<float> pending;
    std::array<std::uint8_t, kMaxPacketBytes> scratch{};
    std::deque<std::vector<std::uint8_t>> packets;
    std::string error;
    std::uint64_t dropped = 0;

    ~Impl() {
        if (enc != nullptr) {
            opus_encoder_destroy(enc);
        }
    }
};

LiveOpusEncoder::LiveOpusEncoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

LiveOpusEncoder::~LiveOpusEncoder() = default;

Expected<std::unique_ptr<LiveOpusEncoder>> LiveOpusEncoder::create(std::uint32_t sample_rate) {
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
    if (auto s = set_and_check(impl->enc, "bitrate", OPUS_SET_BITRATE_REQUEST,
                               OPUS_GET_BITRATE_REQUEST, kBitrate);
        !s) {
        return std::unexpected(s.error());
    }
    if (auto s = set_and_check(impl->enc, "VBR", OPUS_SET_VBR_REQUEST, OPUS_GET_VBR_REQUEST, 1);
        !s) {
        return std::unexpected(s.error());
    }
    if (auto s = set_and_check(impl->enc, "complexity", OPUS_SET_COMPLEXITY_REQUEST,
                               OPUS_GET_COMPLEXITY_REQUEST, 9);
        !s) {
        return std::unexpected(s.error());
    }
    if (auto s = set_and_check(impl->enc, "signal type", OPUS_SET_SIGNAL_REQUEST,
                               OPUS_GET_SIGNAL_REQUEST, OPUS_AUTO);
        !s) {
        return std::unexpected(s.error());
    }
    impl->pending.reserve(kFrameSamples * 2);
    return std::unique_ptr<LiveOpusEncoder>(new LiveOpusEncoder(std::move(impl)));
}

void LiveOpusEncoder::push(std::span<const float> samples) {
    auto& p = *impl_;
    for (const float s : samples) {
        p.pending.push_back(std::isfinite(s) ? std::clamp(s, -1.0f, 1.0f) : 0.0f);
    }

    std::size_t used = 0;
    while (p.pending.size() - used >= static_cast<std::size_t>(kFrameSamples)) {
        const int n = opus_encode_float(p.enc, p.pending.data() + used, kFrameSamples,
                                        p.scratch.data(), kMaxPacketBytes);
        used += kFrameSamples;
        if (n <= 0) {
            if (p.error.empty()) {
                p.error = std::string("Opus encode failed: ") + opus_strerror(n);
            }
            ++p.dropped;
            continue;
        }
        if (p.packets.size() >= kMaxQueuedPackets) {
            p.packets.pop_front();
            ++p.dropped;
        }
        p.packets.emplace_back(p.scratch.begin(), p.scratch.begin() + n);
    }
    p.pending.erase(p.pending.begin(), p.pending.begin() + static_cast<std::ptrdiff_t>(used));
}

std::vector<std::vector<std::uint8_t>> LiveOpusEncoder::take_packets() {
    std::vector<std::vector<std::uint8_t>> out(std::make_move_iterator(impl_->packets.begin()),
                                               std::make_move_iterator(impl_->packets.end()));
    impl_->packets.clear();
    return out;
}

bool LiveOpusEncoder::ok() const { return impl_->error.empty(); }

const std::string& LiveOpusEncoder::error() const { return impl_->error; }

std::uint64_t LiveOpusEncoder::dropped_packets() const { return impl_->dropped; }

}  // namespace revenant::webgate
