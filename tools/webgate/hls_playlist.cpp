#include "tools/webgate/hls_playlist.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <utility>

#include "tools/webgate/audio_hls.h"

namespace revenant::webgate {
namespace {

constexpr std::uint32_t kFrameSamples = 1024;
constexpr std::uint32_t kBitrate = 64000;

using Bytes = std::vector<std::uint8_t>;

void put8(Bytes& out, std::uint32_t v) { out.push_back(static_cast<std::uint8_t>(v)); }
void put16(Bytes& out, std::uint32_t v) {
    put8(out, v >> 8);
    put8(out, v);
}
void put32(Bytes& out, std::uint32_t v) {
    put16(out, v >> 16);
    put16(out, v & 0xFFFF);
}
void put64(Bytes& out, std::uint64_t v) {
    put32(out, static_cast<std::uint32_t>(v >> 32));
    put32(out, static_cast<std::uint32_t>(v));
}
void put_tag(Bytes& out, const char (&tag)[5]) { out.insert(out.end(), tag, tag + 4); }
void put_zeros(Bytes& out, std::size_t count) { out.insert(out.end(), count, 0); }
void put_bytes(Bytes& out, const Bytes& more) { out.insert(out.end(), more.begin(), more.end()); }

// A box is its size, its type, then a payload. Size is patched in once the
// payload is known, so nested boxes can be written straight into the parent.
class Box {
public:
    Box(Bytes& out, const char (&tag)[5]) : out_(out), start_(out.size()) {
        put32(out_, 0);
        put_tag(out_, tag);
    }
    ~Box() {
        const auto size = static_cast<std::uint32_t>(out_.size() - start_);
        for (int i = 0; i < 4; ++i) {
            out_[start_ + static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>(size >> (24 - 8 * i));
        }
    }
    Box(const Box&) = delete;
    Box& operator=(const Box&) = delete;

private:
    Bytes& out_;
    std::size_t start_;
};

// Version and flags word of a "full box".
void put_full(Bytes& out, std::uint32_t version, std::uint32_t flags) {
    put32(out, (version << 24) | flags);
}

void put_unity_matrix(Bytes& out) {
    for (const std::uint32_t v : {0x00010000U, 0U, 0U, 0U, 0x00010000U, 0U, 0U, 0U, 0x40000000U}) {
        put32(out, v);
    }
}

// MPEG-4 descriptor: tag, then a length. Every payload here is under 128
// bytes, so the length is one byte rather than the 7-bit continuation form.
void put_descriptor(Bytes& out, std::uint8_t tag, const Bytes& payload) {
    put8(out, tag);
    put8(out, static_cast<std::uint32_t>(payload.size()));
    put_bytes(out, payload);
}

Bytes build_init(std::uint32_t rate, std::span<const std::uint8_t> asc) {
    Bytes out;
    {
        Box ftyp(out, "ftyp");
        put_tag(out, "iso6");
        put32(out, 1);
        put_tag(out, "iso6");
        put_tag(out, "isom");
        put_tag(out, "mp41");
    }
    // Scoped so moov's size is patched before `out` is returned: a Box that
    // outlives the return value would patch a moved-from vector.
    {
    Box moov(out, "moov");
    {
        Box mvhd(out, "mvhd");
        put_full(out, 0, 0);
        put32(out, 0);  // creation
        put32(out, 0);  // modification
        put32(out, rate);
        put32(out, 0);  // duration: unknown, it is a live stream
        put32(out, 0x00010000);  // rate 1.0
        put16(out, 0x0100);      // volume 1.0
        put_zeros(out, 10);
        put_unity_matrix(out);
        put_zeros(out, 24);  // pre_defined
        put32(out, 2);       // next_track_ID
    }
    {
        Box trak(out, "trak");
        {
            Box tkhd(out, "tkhd");
            put_full(out, 0, 3);  // enabled, in movie
            put32(out, 0);
            put32(out, 0);
            put32(out, 1);  // track_ID
            put32(out, 0);
            put32(out, 0);  // duration
            put_zeros(out, 8);
            put16(out, 0);  // layer
            put16(out, 0);  // alternate group
            put16(out, 0x0100);
            put16(out, 0);
            put_unity_matrix(out);
            put32(out, 0);  // width
            put32(out, 0);  // height
        }
        Box mdia(out, "mdia");
        {
            Box mdhd(out, "mdhd");
            put_full(out, 0, 0);
            put32(out, 0);
            put32(out, 0);
            put32(out, rate);
            put32(out, 0);
            put16(out, 0x55C4);  // "und"
            put16(out, 0);
        }
        {
            Box hdlr(out, "hdlr");
            put_full(out, 0, 0);
            put32(out, 0);
            put_tag(out, "soun");
            put_zeros(out, 12);
            const char name[] = "SoundHandler";
            out.insert(out.end(), name, name + sizeof(name));  // includes the NUL
        }
        Box minf(out, "minf");
        {
            Box smhd(out, "smhd");
            put_full(out, 0, 0);
            put16(out, 0);  // balance
            put16(out, 0);
        }
        {
            Box dinf(out, "dinf");
            Box dref(out, "dref");
            put_full(out, 0, 0);
            put32(out, 1);
            Box url(out, "url ");
            put_full(out, 0, 1);  // media is in this file
        }
        Box stbl(out, "stbl");
        {
            Box stsd(out, "stsd");
            put_full(out, 0, 0);
            put32(out, 1);
            Box mp4a(out, "mp4a");
            put_zeros(out, 6);
            put16(out, 1);  // data_reference_index
            put_zeros(out, 8);
            put16(out, 1);   // channels
            put16(out, 16);  // sample size
            put16(out, 0);
            put16(out, 0);
            put32(out, rate << 16);  // 16.16 fixed point; 48000 fits
            Box esds(out, "esds");
            put_full(out, 0, 0);

            Bytes decoder_specific;
            put_bytes(decoder_specific, Bytes(asc.begin(), asc.end()));
            Bytes decoder_config;
            put8(decoder_config, 0x40);  // MPEG-4 audio
            put8(decoder_config, 0x15);  // audio stream, upstream flag clear, reserved bit set
            put8(decoder_config, 0);     // buffer size, 24 bits
            put16(decoder_config, 0x0800);
            put32(decoder_config, kBitrate);  // max
            put32(decoder_config, kBitrate);  // average
            put_descriptor(decoder_config, 0x05, decoder_specific);
            Bytes sl_config{0x02};  // reserved for MP4 files
            Bytes es;
            put16(es, 0);  // ES_ID, unused in a file
            put8(es, 0);   // no stream dependence, URL or OCR
            put_descriptor(es, 0x04, decoder_config);
            put_descriptor(es, 0x06, sl_config);
            put_descriptor(out, 0x03, es);
        }
        // An fMP4 track has no samples in the moov; the tables must exist and
        // be empty.
        {
            Box stts(out, "stts");
            put_full(out, 0, 0);
            put32(out, 0);
        }
        {
            Box stsc(out, "stsc");
            put_full(out, 0, 0);
            put32(out, 0);
        }
        {
            Box stco(out, "stco");
            put_full(out, 0, 0);
            put32(out, 0);
        }
        {
            Box stsz(out, "stsz");
            put_full(out, 0, 0);
            put32(out, 0);
            put32(out, 0);
        }
    }
    {
        Box mvex(out, "mvex");
        Box trex(out, "trex");
        put_full(out, 0, 0);
        put32(out, 1);  // track_ID
        put32(out, 1);  // default sample description
        put32(out, kFrameSamples);
        put32(out, 0);
        put32(out, 0);
    }
    }
    return out;
}

}  // namespace

HlsSegmenter::HlsSegmenter(std::uint32_t sample_rate, double segment_seconds, std::size_t window)
    : sample_rate_(sample_rate),
      frames_per_segment_(std::max<std::size_t>(
          1, static_cast<std::size_t>(segment_seconds * sample_rate / kFrameSamples))),
      window_(std::max<std::size_t>(1, window)) {
    auto created = AacEncoder::create(sample_rate);
    if (!created) {
        error_ = created.error().message;
        return;
    }
    encoder_ = std::move(*created);
    init_ = build_init(sample_rate, encoder_->audio_specific_config());
}

HlsSegmenter::~HlsSegmenter() = default;

void HlsSegmenter::push(std::span<const float> samples) {
    if (!encoder_) {
        return;
    }
    if (!encoder_->push(samples)) {
        // The encoder failing mid-stream is a codec fault with no recovery; the
        // stream stalls and the client's player gives up on its own.
        return;
    }
    for (auto& unit : encoder_->take_access_units()) {
        pending_.push_back(std::move(unit));
        if (pending_.size() == frames_per_segment_) {
            cut_segment();
        }
    }
}

void HlsSegmenter::cut_segment() {
    Segment seg;
    seg.sequence = next_sequence_++;
    seg.seconds = static_cast<double>(pending_.size()) * kFrameSamples / sample_rate_;

    Bytes& out = seg.bytes;
    std::size_t data_offset_at = 0;
    {
        Box moof(out, "moof");
        {
            Box mfhd(out, "mfhd");
            put_full(out, 0, 0);
            put32(out, static_cast<std::uint32_t>(seg.sequence + 1));  // starts at 1
        }
        Box traf(out, "traf");
        {
            Box tfhd(out, "tfhd");
            put_full(out, 0, 0x020000);  // offsets are relative to the moof
            put32(out, 1);
        }
        {
            Box tfdt(out, "tfdt");
            put_full(out, 1, 0);
            put64(out, frames_emitted_ * kFrameSamples);
        }
        {
            Box trun(out, "trun");
            put_full(out, 0, 0x000301);  // data offset, sample duration, sample size
            put32(out, static_cast<std::uint32_t>(pending_.size()));
            data_offset_at = out.size();
            put32(out, 0);  // patched once the moof's size is known
            for (const Bytes& unit : pending_) {
                put32(out, kFrameSamples);
                put32(out, static_cast<std::uint32_t>(unit.size()));
            }
        }
    }
    // moof is closed: its size is the first word, and the first sample sits
    // after the 8-byte mdat header.
    const std::uint32_t moof_size = (std::uint32_t{out[0]} << 24) | (std::uint32_t{out[1]} << 16) |
                                    (std::uint32_t{out[2]} << 8) | std::uint32_t{out[3]};
    const std::uint32_t offset = moof_size + 8;
    for (int i = 0; i < 4; ++i) {
        out[data_offset_at + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(offset >> (24 - 8 * i));
    }
    {
        Box mdat(out, "mdat");
        for (const Bytes& unit : pending_) {
            put_bytes(out, unit);
        }
    }

    frames_emitted_ += pending_.size();
    pending_.clear();
    segments_.push_back(std::move(seg));
    while (segments_.size() > window_) {
        segments_.pop_front();
    }
}

std::optional<std::span<const std::uint8_t>> HlsSegmenter::segment(std::uint64_t sequence) const {
    for (const Segment& seg : segments_) {
        if (seg.sequence == sequence) {
            return std::span<const std::uint8_t>(seg.bytes);
        }
    }
    return std::nullopt;
}

std::string HlsSegmenter::playlist() const {
    if (segments_.empty()) {
        return {};
    }
    // The target duration is the longest segment rounded to a whole second,
    // which HLS requires to be at least every EXTINF.
    double longest = 0.0;
    for (const Segment& seg : segments_) {
        longest = std::max(longest, seg.seconds);
    }
    std::string text = "#EXTM3U\n#EXT-X-VERSION:7\n";
    text += std::format("#EXT-X-TARGETDURATION:{}\n", std::max(1L, std::lround(longest)));
    text += std::format("#EXT-X-MEDIA-SEQUENCE:{}\n", segments_.front().sequence);
    text += "#EXT-X-INDEPENDENT-SEGMENTS\n";
    text += "#EXT-X-MAP:URI=\"init.mp4\"\n";
    for (const Segment& seg : segments_) {
        text += std::format("#EXTINF:{:.5f},\n{}.m4s\n", seg.seconds, seg.sequence);
    }
    return text;
}

}  // namespace revenant::webgate
