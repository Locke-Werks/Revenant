#include "core/decode/goes_hrit.h"

#include <utility>

#include "core/decode/gk2a_lrit.h"

namespace revenant::decode {

std::optional<NoaaHeader> noaa_header(const XritFile& file) {
    const auto* r = file.find(129);
    if (r == nullptr || r->data.size() < 11) {
        return std::nullopt;
    }
    const auto& d = r->data;
    NoaaHeader h;
    h.agency.assign(d.begin(), d.begin() + 4);
    h.product_id = static_cast<std::uint16_t>((d[4] << 8U) | d[5]);
    h.subproduct_id = static_cast<std::uint16_t>((d[6] << 8U) | d[7]);
    h.parameter = static_cast<std::uint16_t>((d[8] << 8U) | d[9]);
    h.compression = d[10];
    return h;
}

std::optional<EmwinText> emwin_text(const XritFile& file) {
    const auto noaa = noaa_header(file);
    if (file.file_type != 2 || !noaa || noaa->product_id != kNoaaProductEmwin || noaa->compression != 0 ||
        file.encrypted()) {
        return std::nullopt;
    }
    EmwinText t;
    t.filename = file.annotation();
    t.body.assign(file.data.begin(), file.data.end());
    // Lines end CR CR LF in the observed bulletin; split on LF and trim CRs.
    std::size_t pos = 0;
    int got = 0;
    while (pos < t.body.size() && got < 2) {
        std::size_t end = t.body.find('\n', pos);
        if (end == std::string::npos) {
            end = t.body.size();
        }
        std::string line = t.body.substr(pos, end - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty()) {
            (got == 0 ? t.heading : t.product) = line;
            ++got;
        }
        pos = end + 1;
    }
    return t;
}

Expected<GoesHritDecoder> GoesHritDecoder::create(const GoesHritConfig& config, GoesHritEvents events) {
    GoesHritDecoder d;
    d.state_ = std::make_unique<State>();
    d.state_->events = std::move(events);
    CcsdsDownlinkConfig lc;
    lc.sample_rate = config.sample_rate;
    lc.symbol_rate = config.symbol_rate;
    lc.modulation = PskModulation::Bpsk;
    lc.roll_off = config.roll_off;
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
            if (auto t = emwin_text(f); t && st->events.on_emwin_text) {
                st->events.on_emwin_text(*t);
            }
            if (f.file_type == 0 && xrit_emit_image(f, st->events.image, st->next_image, "GOES HRIT " + f.annotation())) {
                ++st->next_image;
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

void GoesHritDecoder::push(std::span<const dsp::Complex32> samples) { link_->push(samples); }

void GoesHritDecoder::finish() { link_->finish(); }

}  // namespace revenant::decode
