#include "core/dsp/synth/sstv_mod.h"

#include <cmath>
#include <numbers>

namespace revenant::siggen {

namespace {

struct Piece {
    double ms = 0.0;
    double hz = 0.0;
    std::vector<double> pixels;   // when not empty, hz per pixel across the piece
};

double frequency(double value) { return decode::kSstvBlackHz + value * decode::kSstvHzPerLevel; }

}  // namespace

Expected<std::vector<float>> sstv_render(const SstvModConfig& config, const decode::SstvMode& mode,
                                         std::span<const std::uint8_t> rgb) {
    const std::size_t w = mode.width;
    const std::size_t h = mode.height;
    if (rgb.size() != w * h * 3) {
        return fail("sstv_render: the image must be width * height * 3 bytes");
    }
    if (config.rate < 4'000.0) {
        return fail("sstv_render: the rate is too low");
    }

    std::vector<Piece> pieces;
    if (config.vis) {
        // "VIS Code and Robot calibration header".
        pieces.push_back({decode::kVisLeaderMs, 1900.0, {}});
        pieces.push_back({decode::kVisBreakMs, 1200.0, {}});
        pieces.push_back({decode::kVisLeaderMs, 1900.0, {}});
        pieces.push_back({decode::kVisBitMs, 1200.0, {}});
        int ones = 0;
        for (int k = 0; k < 7; ++k) {
            const int bit = (mode.vis >> k) & 1;
            ones += bit;
            pieces.push_back({decode::kVisBitMs, bit != 0 ? decode::kVisOneHz : decode::kVisZeroHz, {}});
        }
        pieces.push_back({decode::kVisBitMs, (ones & 1) != 0 ? decode::kVisOneHz : decode::kVisZeroHz, {}});
        pieces.push_back({decode::kVisBitMs, 1200.0, {}});
    }
    for (const decode::SstvSegment& s : mode.lead) {
        pieces.push_back({s.ms, s.hz, {}});
    }

    const auto pixel = [&](std::size_t row, std::size_t x, int c) {
        return static_cast<double>(rgb[(row * w + x) * 3 + static_cast<std::size_t>(c)]);
    };
    const std::size_t lpp = static_cast<std::size_t>(mode.lines_per_period);
    for (std::uint32_t p = 0; p < mode.periods(); ++p) {
        const bool blank = p < static_cast<std::uint32_t>(mode.skip_periods);
        const std::size_t first =
            static_cast<std::size_t>(p - (blank ? p : static_cast<std::uint32_t>(mode.skip_periods))) * lpp;
        for (const decode::SstvSegment& s : mode.period) {
            if (!s.scan()) {
                pieces.push_back({s.ms, s.hz, {}});
                continue;
            }
            Piece piece{s.ms, 0.0, std::vector<double>(w)};
            for (std::size_t x = 0; x < w; ++x) {
                double v = 255.0;
                if (!blank) {
                    switch (mode.colour) {
                        case decode::SstvColour::Rgb:
                            v = pixel(first, x, s.channel);
                            break;
                        case decode::SstvColour::Gray:
                            v = pixel(first, x, 1);
                            break;
                        case decode::SstvColour::Ycc: {
                            if (s.channel == 0) {
                                const std::size_t row = first + static_cast<std::size_t>(s.line);
                                v = decode::sstv_rgb_to_ycc(pixel(row, x, 0), pixel(row, x, 1),
                                                            pixel(row, x, 2))
                                        .y;
                            } else {
                                // Chroma averaged over the period's lines.
                                double total = 0.0;
                                for (std::size_t l = 0; l < lpp; ++l) {
                                    const decode::SstvYcc c = decode::sstv_rgb_to_ycc(
                                        pixel(first + l, x, 0), pixel(first + l, x, 1),
                                        pixel(first + l, x, 2));
                                    total += s.channel == 1 ? c.ry : c.by;
                                }
                                v = total / static_cast<double>(lpp);
                            }
                            break;
                        }
                    }
                }
                piece.pixels[x] = frequency(v);
            }
            pieces.push_back(std::move(piece));
        }
    }

    const auto pad = static_cast<std::size_t>(config.pad_seconds * config.rate);
    double total_ms = 0.0;
    for (const Piece& p : pieces) {
        total_ms += p.ms;
    }
    const auto body = static_cast<std::size_t>(total_ms * config.rate / 1000.0);
    std::vector<float> out(pad, 0.0F);
    out.reserve(pad * 2 + body);
    double phase = 0.0;
    std::size_t at = 0;
    double piece_start = 0.0;
    for (std::size_t n = 0; n < body; ++n) {
        const double t = static_cast<double>(n) * 1000.0 / config.rate;
        while (at + 1 < pieces.size() && t >= piece_start + pieces[at].ms) {
            piece_start += pieces[at].ms;
            ++at;
        }
        const Piece& p = pieces[at];
        double hz = p.hz;
        if (!p.pixels.empty()) {
            const auto i = std::min(p.pixels.size() - 1,
                                    static_cast<std::size_t>((t - piece_start) / p.ms *
                                                             static_cast<double>(p.pixels.size())));
            hz = p.pixels[i];
        }
        phase += 2.0 * std::numbers::pi * (hz + config.offset_hz) / config.rate;
        if (phase > 2.0 * std::numbers::pi) {
            phase -= 2.0 * std::numbers::pi;
        }
        out.push_back(static_cast<float>(config.amplitude * std::sin(phase)));
    }
    out.insert(out.end(), pad, 0.0F);
    return out;
}

}  // namespace revenant::siggen
