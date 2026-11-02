// Baseline JPEG: pictures through a small encoder written here from T.81 and
// T.871, then back through core/decode/jpeg_baseline.h.
//
// The encoder is the test's own: an FDCT from A.3.3's equation, the uniform
// quantizer of A.3.4, Huffman codes made by C.2's procedure from tables
// chosen here (not Annex K's), F.1.2's coding of DC differences and AC runs,
// byte stuffing and restart markers. Scored as PSNR against the picture that
// went in.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <numbers>
#include <vector>

#include "core/decode/jpeg_baseline.h"

using namespace revenant;

namespace {

std::array<std::uint8_t, 64> zigzag() {
    std::array<std::uint8_t, 64> out{};
    int k = 0;
    for (int s = 0; s < 15; ++s) {
        for (int i = 0; i <= s; ++i) {
            const int row = s % 2 == 0 ? s - i : i;
            const int col = s - row;
            if (row < 8 && col < 8) {
                out[static_cast<std::size_t>(k++)] = static_cast<std::uint8_t>(row * 8 + col);
            }
        }
    }
    return out;
}

// Code lengths chosen for this test: 12 DC categories and the 162 AC
// run/size symbols, Kraft sums under one so no code is all ones.
decode::JpegHuffmanSpec dc_spec() {
    decode::JpegHuffmanSpec s;
    s.bits[1] = 1;
    s.bits[2] = 3;
    s.bits[3] = 4;
    s.bits[5] = 4;
    for (std::uint8_t v = 0; v < 12; ++v) {
        s.values.push_back(v);
    }
    return s;
}

decode::JpegHuffmanSpec ac_spec() {
    decode::JpegHuffmanSpec s;
    s.values = {0x00, 0xF0};
    for (int r = 0; r < 16; ++r) {
        for (int size = 1; size <= 10; ++size) {
            s.values.push_back(static_cast<std::uint8_t>(r << 4 | size));
        }
    }
    s.bits[2] = 2;
    s.bits[4] = 6;
    s.bits[6] = 20;
    s.bits[8] = 134;
    return s;
}

struct Code {
    std::uint16_t code = 0;
    int length = 0;
};

// C.2, figures C.1 and C.2, indexed by symbol.
std::array<Code, 256> codes(const decode::JpegHuffmanSpec& spec) {
    std::array<Code, 256> out{};
    std::uint16_t code = 0;
    std::size_t k = 0;
    for (int length = 1; length <= 16; ++length) {
        for (int i = 0; i < spec.bits[static_cast<std::size_t>(length - 1)]; ++i) {
            out[spec.values[k++]] = Code{code++, length};
        }
        code = static_cast<std::uint16_t>(code << 1);
    }
    return out;
}

class Bits {
public:
    explicit Bits(bool stuffed) : stuffed_(stuffed) {}
    void put(std::uint32_t value, int length) {
        for (int i = length - 1; i >= 0; --i) {
            acc_ = static_cast<std::uint8_t>(acc_ << 1 | ((value >> i) & 1));
            if (++count_ == 8) {
                emit(acc_);
            }
        }
    }
    // Padding with 1 bits to the byte boundary.
    void align() {
        while (count_ != 0) {
            put(1, 1);
        }
    }
    void marker(std::uint8_t m) {
        align();
        out.push_back(0xFF);
        out.push_back(m);
    }
    std::vector<std::uint8_t> out;

private:
    void emit(std::uint8_t b) {
        out.push_back(b);
        if (stuffed_ && b == 0xFF) {
            out.push_back(0x00);
        }
        acc_ = 0;
        count_ = 0;
    }
    bool stuffed_;
    std::uint8_t acc_ = 0;
    int count_ = 0;
};

struct Sampling {
    int h = 1;
    int v = 1;
};

struct Picture {
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    int channels = 3;
    std::vector<std::uint8_t> px;
};

Picture picture(std::uint32_t w, std::uint32_t h, int channels) {
    Picture p{w, h, channels, {}};
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const double r = 128.0 + 100.0 * std::sin(x / 7.0);
            const double g = 255.0 * y / h;
            const double b = 128.0 + 90.0 * std::cos((x + 2.0 * y) / 11.0);
            if (channels == 1) {
                p.px.push_back(static_cast<std::uint8_t>(std::lround(0.5 * r + 0.5 * g)));
            } else {
                p.px.push_back(static_cast<std::uint8_t>(std::lround(r)));
                p.px.push_back(static_cast<std::uint8_t>(std::lround(g)));
                p.px.push_back(static_cast<std::uint8_t>(std::lround(b)));
            }
        }
    }
    return p;
}

struct Options {
    std::vector<Sampling> sampling = {{1, 1}};
    int quant = 1;                  // every step size
    std::uint16_t restart = 0;
    bool interleaved = true;
};

// The component planes, T.871 clause 7 forward, chroma box-averaged down.
std::vector<std::vector<double>> planes(const Picture& p, const Options& o, int hmax, int vmax,
                                        std::vector<std::uint32_t>& cw, std::vector<std::uint32_t>& ch) {
    std::vector<std::vector<double>> out;
    for (std::size_t c = 0; c < o.sampling.size(); ++c) {
        const int sh = hmax / o.sampling[c].h;
        const int sv = vmax / o.sampling[c].v;
        const std::uint32_t w = (p.w * o.sampling[c].h + hmax - 1) / hmax;
        const std::uint32_t h = (p.h * o.sampling[c].v + vmax - 1) / vmax;
        cw.push_back(w);
        ch.push_back(h);
        std::vector<double> plane(static_cast<std::size_t>(w) * h);
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                double total = 0.0;
                int n = 0;
                for (int dy = 0; dy < sv; ++dy) {
                    for (int dx = 0; dx < sh; ++dx) {
                        const std::uint32_t px = std::min(p.w - 1, x * sh + dx);
                        const std::uint32_t py = std::min(p.h - 1, y * sv + dy);
                        const std::size_t i = static_cast<std::size_t>(py) * p.w + px;
                        double v = 0.0;
                        if (p.channels == 1) {
                            v = p.px[i];
                        } else {
                            const double r = p.px[3 * i];
                            const double g = p.px[3 * i + 1];
                            const double b = p.px[3 * i + 2];
                            if (c == 0) {
                                v = 0.299 * r + 0.587 * g + 0.114 * b;
                            } else if (c == 1) {
                                v = (-0.299 * r - 0.587 * g + 0.886 * b) / 1.772 + 128.0;
                            } else {
                                v = (0.701 * r - 0.587 * g - 0.114 * b) / 1.402 + 128.0;
                            }
                        }
                        total += v;
                        ++n;
                    }
                }
                plane[static_cast<std::size_t>(y) * w + x] = total / n;
            }
        }
        out.push_back(std::move(plane));
    }
    return out;
}

// A.3.1 and A.3.3 forward, A.3.4 quantization, in zig-zag order.
std::array<int, 64> block(const std::vector<double>& plane, std::uint32_t w, std::uint32_t h,
                          std::uint32_t bx, std::uint32_t by, int quant) {
    static const auto zz = zigzag();
    std::array<double, 64> s{};
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            // Edge blocks repeat the last row and column.
            const std::uint32_t px = std::min(w - 1, bx * 8 + static_cast<std::uint32_t>(x));
            const std::uint32_t py = std::min(h - 1, by * 8 + static_cast<std::uint32_t>(y));
            s[static_cast<std::size_t>(y * 8 + x)] = plane[static_cast<std::size_t>(py) * w + px] - 128.0;
        }
    }
    std::array<int, 64> out{};
    for (int v = 0; v < 8; ++v) {
        for (int u = 0; u < 8; ++u) {
            double t = 0.0;
            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 8; ++x) {
                    t += s[static_cast<std::size_t>(y * 8 + x)] *
                         std::cos((2 * x + 1) * u * std::numbers::pi / 16.0) *
                         std::cos((2 * y + 1) * v * std::numbers::pi / 16.0);
                }
            }
            const double cu = u == 0 ? 1.0 / std::numbers::sqrt2 : 1.0;
            const double cv = v == 0 ? 1.0 / std::numbers::sqrt2 : 1.0;
            const int natural = v * 8 + u;
            for (std::size_t k = 0; k < 64; ++k) {
                if (zz[k] == natural) {
                    out[k] = static_cast<int>(std::lround(0.25 * cu * cv * t / quant));
                }
            }
        }
    }
    return out;
}

int category(int v) {
    int a = std::abs(v);
    int n = 0;
    while (a != 0) {
        a >>= 1;
        ++n;
    }
    return n;
}

// F.1.2.1 and F.1.2.2.
void code_block(Bits& out, const std::array<int, 64>& zz, int& pred) {
    static const auto dc = codes(dc_spec());
    static const auto ac = codes(ac_spec());
    const auto put_value = [&](int v, int size) {
        out.put(static_cast<std::uint32_t>(v < 0 ? v + (1 << size) - 1 : v), size);
    };
    const int diff = zz[0] - pred;
    pred = zz[0];
    const int t = category(diff);
    out.put(dc[static_cast<std::size_t>(t)].code, dc[static_cast<std::size_t>(t)].length);
    put_value(diff, t);
    int run = 0;
    for (std::size_t k = 1; k < 64; ++k) {
        if (zz[k] == 0) {
            ++run;
            continue;
        }
        while (run > 15) {
            out.put(ac[0xF0].code, ac[0xF0].length);
            run -= 16;
        }
        const int size = category(zz[k]);
        const auto rs = static_cast<std::size_t>(run << 4 | size);
        out.put(ac[rs].code, ac[rs].length);
        put_value(zz[k], size);
        run = 0;
    }
    if (run > 0) {
        out.put(ac[0].code, ac[0].length);
    }
}

void be16(std::vector<std::uint8_t>& out, std::size_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void huffman_segment(std::vector<std::uint8_t>& out, int tc, int th,
                     const decode::JpegHuffmanSpec& spec) {
    out.insert(out.end(), {0xFF, 0xC4});
    be16(out, 2 + 17 + spec.values.size());
    out.push_back(static_cast<std::uint8_t>(tc << 4 | th));
    out.insert(out.end(), spec.bits.begin(), spec.bits.end());
    out.insert(out.end(), spec.values.begin(), spec.values.end());
}

struct Encoded {
    std::vector<std::uint8_t> file;
    std::vector<std::uint8_t> raw_scan;   // the interleaved scan unstuffed, no restarts
    decode::JpegTables tables;
};

Encoded encode(const Picture& p, const Options& o) {
    int hmax = 1;
    int vmax = 1;
    for (const Sampling& s : o.sampling) {
        hmax = std::max(hmax, s.h);
        vmax = std::max(vmax, s.v);
    }
    std::vector<std::uint32_t> cw;
    std::vector<std::uint32_t> ch;
    const auto comp = planes(p, o, hmax, vmax, cw, ch);
    const std::uint32_t mcux = (p.w + 8 * hmax - 1) / (8 * hmax);
    const std::uint32_t mcuy = (p.h + 8 * vmax - 1) / (8 * vmax);

    Encoded e;
    auto& f = e.file;
    f = {0xFF, 0xD8};
    // B.2.4.1: table 0 for luminance, table 1 for chrominance, both flat.
    f.insert(f.end(), {0xFF, 0xDB});
    be16(f, 2 + 2 * 65);
    for (int t = 0; t < 2; ++t) {
        f.push_back(static_cast<std::uint8_t>(t));
        for (int k = 0; k < 64; ++k) {
            f.push_back(static_cast<std::uint8_t>(o.quant));
        }
    }
    // B.2.2.
    f.insert(f.end(), {0xFF, 0xC0});
    be16(f, 8 + 3 * o.sampling.size());
    f.push_back(8);
    be16(f, p.h);
    be16(f, p.w);
    f.push_back(static_cast<std::uint8_t>(o.sampling.size()));
    for (std::size_t c = 0; c < o.sampling.size(); ++c) {
        f.push_back(static_cast<std::uint8_t>(c + 1));
        f.push_back(static_cast<std::uint8_t>(o.sampling[c].h << 4 | o.sampling[c].v));
        f.push_back(c == 0 ? 0 : 1);
    }
    huffman_segment(f, 0, 0, dc_spec());
    huffman_segment(f, 1, 0, ac_spec());
    huffman_segment(f, 0, 1, dc_spec());
    huffman_segment(f, 1, 1, ac_spec());
    if (o.restart != 0) {
        f.insert(f.end(), {0xFF, 0xDD, 0x00, 0x04});
        be16(f, o.restart);
    }
    const auto sos = [&](const std::vector<std::size_t>& comps) {
        f.insert(f.end(), {0xFF, 0xDA});
        be16(f, 6 + 2 * comps.size());
        f.push_back(static_cast<std::uint8_t>(comps.size()));
        for (const std::size_t c : comps) {
            f.push_back(static_cast<std::uint8_t>(c + 1));
            f.push_back(c == 0 ? 0x00 : 0x11);
        }
        f.insert(f.end(), {0x00, 0x3F, 0x00});
    };
    const auto scan = [&](const std::vector<std::size_t>& comps, bool stuffed, bool restarts) {
        Bits bits(stuffed);
        std::vector<int> pred(comps.size(), 0);
        std::uint32_t ux = mcux;
        std::uint32_t uy = mcuy;
        if (comps.size() == 1) {
            ux = (cw[comps[0]] + 7) / 8;
            uy = (ch[comps[0]] + 7) / 8;
        }
        std::uint8_t rst = 0;
        for (std::uint32_t m = 0; m < ux * uy; ++m) {
            if (restarts && o.restart != 0 && m != 0 && m % o.restart == 0) {
                bits.marker(static_cast<std::uint8_t>(0xD0 + rst));
                rst = static_cast<std::uint8_t>((rst + 1) % 8);
                std::ranges::fill(pred, 0);
            }
            for (std::size_t s = 0; s < comps.size(); ++s) {
                const std::size_t c = comps[s];
                const int bh = comps.size() == 1 ? 1 : o.sampling[c].h;
                const int bv = comps.size() == 1 ? 1 : o.sampling[c].v;
                for (int v = 0; v < bv; ++v) {
                    for (int h = 0; h < bh; ++h) {
                        code_block(bits,
                                   block(comp[c], cw[c], ch[c], (m % ux) * bh + h, (m / ux) * bv + v, o.quant),
                                   pred[s]);
                    }
                }
            }
        }
        bits.align();
        return bits.out;
    };
    std::vector<std::size_t> all;
    for (std::size_t c = 0; c < o.sampling.size(); ++c) {
        all.push_back(c);
    }
    if (o.interleaved) {
        sos(all);
        const auto data = scan(all, true, true);
        f.insert(f.end(), data.begin(), data.end());
    } else {
        for (const std::size_t c : all) {
            sos({c});
            const auto data = scan({c}, true, true);
            f.insert(f.end(), data.begin(), data.end());
        }
    }
    f.insert(f.end(), {0xFF, 0xD9});

    e.raw_scan = scan(all, false, false);
    e.tables.width = p.w;
    e.tables.height = p.h;
    for (int t = 0; t < 2; ++t) {
        e.tables.quant[static_cast<std::size_t>(t)].q.fill(static_cast<std::uint16_t>(o.quant));
        e.tables.dc[static_cast<std::size_t>(t)] = dc_spec();
        e.tables.ac[static_cast<std::size_t>(t)] = ac_spec();
    }
    for (std::size_t c = 0; c < o.sampling.size(); ++c) {
        const std::uint8_t t = c == 0 ? 0 : 1;
        e.tables.components.push_back(decode::JpegComponent{
            static_cast<std::uint8_t>(c + 1), static_cast<std::uint8_t>(o.sampling[c].h),
            static_cast<std::uint8_t>(o.sampling[c].v), t, t, t});
    }
    return e;
}

double psnr(const Picture& want, const decode::JpegImage& got) {
    REQUIRE(got.width == want.w);
    REQUIRE(got.height == want.h);
    REQUIRE(got.channels == want.channels);
    double err = 0.0;
    for (std::size_t i = 0; i < want.px.size(); ++i) {
        const double d = static_cast<double>(got.pixels[i]) - want.px[i];
        err += d * d;
    }
    const double mse = err / static_cast<double>(want.px.size());
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

}  // namespace

TEST_CASE("the zig-zag walk and Huffman tables of the test encoder are sound",
          "[decode][jpeg]") {
    const auto zz = zigzag();
    CHECK(zz[0] == 0);
    CHECK(zz[1] == 1);
    CHECK(zz[2] == 8);
    CHECK(zz[3] == 16);
    CHECK(zz[63] == 63);
    CHECK(ac_spec().values.size() == 162);
}

TEST_CASE("baseline pictures decode to what went in", "[decode][jpeg]") {
    struct Case {
        const char* what;
        std::uint32_t w;
        std::uint32_t h;
        int channels;
        Options o;
        double floor;
    };
    const Case cases[] = {
        {"grey 37x21, quantizer 1", 37, 21, 1, Options{{{1, 1}}, 1, 0, true}, 45.0},
        {"YCbCr 4:4:4 32x16, quantizer 1", 32, 16, 3, Options{{{1, 1}, {1, 1}, {1, 1}}, 1, 0, true}, 40.0},
        {"YCbCr 4:2:0 64x48, quantizer 4", 64, 48, 3, Options{{{2, 2}, {1, 1}, {1, 1}}, 4, 0, true}, 30.0},
        {"YCbCr 4:2:2 50x34, restart every 3", 50, 34, 3, Options{{{2, 1}, {1, 1}, {1, 1}}, 2, 3, true}, 30.0},
        {"YCbCr 4:2:0 41x29, one scan per component", 41, 29, 3, Options{{{2, 2}, {1, 1}, {1, 1}}, 2, 0, false}, 30.0},
        {"YCbCr 4:2:0 41x29, separate scans, restart every 2", 41, 29, 3, Options{{{2, 2}, {1, 1}, {1, 1}}, 2, 2, false}, 30.0},
    };
    for (const Case& c : cases) {
        const Picture p = picture(c.w, c.h, c.channels);
        const Encoded e = encode(p, c.o);
        const auto got = decode::jpeg_decode(e.file);
        INFO(c.what << ": " << (got ? "" : got.error().message));
        REQUIRE(got.has_value());
        const double score = psnr(p, *got);
        WARN(std::format("{}: {} bytes, PSNR {:.1f} dB", c.what, e.file.size(), score));
        CHECK_FALSE(got->damaged);
        CHECK(score > c.floor);
    }
}

TEST_CASE("a scan without markers decodes from tables given out of band", "[decode][jpeg]") {
    const Picture p = picture(48, 32, 3);
    const Encoded e = encode(p, Options{{{2, 2}, {1, 1}, {1, 1}}, 2, 0, true});
    const auto whole = decode::jpeg_decode(e.file);
    REQUIRE(whole.has_value());
    const auto bare = decode::jpeg_decode_entropy(e.tables, e.raw_scan, false);
    REQUIRE(bare.has_value());
    CHECK(bare->pixels == whole->pixels);
    CHECK_FALSE(bare->damaged);
}

TEST_CASE("damage is reported and does not stop the picture", "[decode][jpeg]") {
    const Picture p = picture(64, 48, 3);
    const Encoded e = encode(p, Options{{{2, 2}, {1, 1}, {1, 1}}, 2, 2, true});
    // Truncated in the middle of the scan.
    std::vector<std::uint8_t> cut(e.file.begin(), e.file.begin() + static_cast<std::ptrdiff_t>(e.file.size() * 2 / 3));
    const auto short_one = decode::jpeg_decode(cut);
    REQUIRE(short_one.has_value());
    CHECK(short_one->damaged);
    CHECK(short_one->pixels.size() == p.px.size());
    // Bytes overwritten mid-scan: the restart markers bound the loss.
    std::vector<std::uint8_t> hit = e.file;
    for (std::size_t i = hit.size() / 2; i < hit.size() / 2 + 6; ++i) {
        hit[i] = 0x5A;
    }
    const auto damaged = decode::jpeg_decode(hit);
    REQUIRE(damaged.has_value());
    WARN(std::format("six bytes overwritten: PSNR {:.1f} dB", psnr(p, *damaged)));
    CHECK(psnr(p, *damaged) > 15.0);
}

TEST_CASE("what is not baseline is refused by name", "[decode][jpeg]") {
    const Picture p = picture(16, 16, 1);
    Encoded e = encode(p, Options{});
    CHECK_FALSE(decode::jpeg_decode(std::span<const std::uint8_t>(e.file).subspan(2)).has_value());
    for (std::size_t i = 0; i + 1 < e.file.size(); ++i) {
        if (e.file[i] == 0xFF && e.file[i + 1] == 0xC0) {
            e.file[i + 1] = 0xC2;   // progressive
            break;
        }
    }
    const auto refused = decode::jpeg_decode(e.file);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message.find("SOF2") != std::string::npos);
}
