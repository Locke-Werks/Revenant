#include "core/decode/gk2a_lrit.h"

#include <algorithm>
#include <utility>

namespace revenant::decode {

std::optional<Gk2aSegment> gk2a_segment(const XritFile& file) {
    const auto* r = file.find(128);
    if (r == nullptr || r->data.size() < 4) {
        return std::nullopt;
    }
    Gk2aSegment s;
    s.sequence = r->data[0];
    s.total = r->data[1];
    s.first_line = static_cast<std::uint16_t>((r->data[2] << 8U) | r->data[3]);
    return s;
}

bool xrit_emit_image(const XritFile& file, const ImageSink& sink, std::uint32_t image_id, const std::string& label) {
    const auto is = file.image_structure();
    if (!is || is->compression != 0 || is->bits_per_pixel != 8 || file.encrypted() || is->columns == 0) {
        return false;
    }
    // CGMS 4.2.3.1: pixels MSB first, line by line, left to right.
    const std::size_t lines = std::min<std::size_t>(is->lines, file.data.size() / is->columns);
    if (sink.on_open) {
        sink.on_open(ImageOpen{image_id, is->columns, ImageRow::Gray8, label, 0.0});
    }
    for (std::size_t y = 0; y < lines; ++y) {
        if (sink.on_row) {
            ImageRow row;
            row.image_id = image_id;
            row.row = static_cast<std::uint32_t>(y);
            row.width = is->columns;
            row.bytes.assign(file.data.begin() + static_cast<std::ptrdiff_t>(y * is->columns),
                             file.data.begin() + static_cast<std::ptrdiff_t>((y + 1) * is->columns));
            sink.on_row(row);
        }
    }
    if (sink.on_close) {
        sink.on_close(ImageClose{image_id, static_cast<std::uint32_t>(lines), lines == is->lines});
    }
    return true;
}

Expected<Gk2aLritDecoder> Gk2aLritDecoder::create(const Gk2aLritConfig& config, Gk2aLritEvents events) {
    Gk2aLritDecoder d;
    d.state_ = std::make_unique<State>();
    d.state_->events = std::move(events);
    CcsdsDownlinkConfig lc;
    lc.sample_rate = config.sample_rate;
    lc.symbol_rate = config.symbol_rate;
    lc.modulation = PskModulation::Bpsk;
    lc.roll_off = kGk2aLritRollOff;
    lc.g2_inverted = config.g2_inverted;
    CcsdsDownlinkEvents le;
    auto* st = d.state_.get();
    le.on_packet = [st](std::uint8_t vc, const SpacePacket& p) {
        st->files.clear();
        st->transport.push(vc, p, st->files);
        for (const auto& f : st->files) {
            if (st->events.on_file) {
                st->events.on_file(f);
            }
            if (f.file_type == 0) {
                const std::string label = "GK-2A LRIT " + f.annotation();
                if (xrit_emit_image(f, st->events.image, st->next_image, label)) {
                    ++st->next_image;
                }
            }
        }
    };
    auto link = CcsdsDownlink::create(lc, std::move(le));
    if (!link) {
        return std::unexpected(link.error());
    }
    d.link_ = std::make_unique<CcsdsDownlink>(std::move(*link));
    return d;
}

void Gk2aLritDecoder::push(std::span<const dsp::Complex32> samples) { link_->push(samples); }

void Gk2aLritDecoder::finish() { link_->finish(); }

}  // namespace revenant::decode
