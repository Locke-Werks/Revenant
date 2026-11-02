#include "core/decode/jpeg_baseline.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <optional>

namespace revenant::decode {

namespace {

// A.3.6, figure A.6: zig-zag index to natural (row * 8 + column) position,
// walked along the anti-diagonals rather than written out.
constexpr std::array<std::uint8_t, 64> make_zigzag() {
    std::array<std::uint8_t, 64> out{};
    int k = 0;
    for (int s = 0; s < 15; ++s) {
        if (s % 2 == 0) {
            for (int row = std::min(s, 7); row >= 0 && s - row <= 7; --row) {
                out[static_cast<std::size_t>(k++)] = static_cast<std::uint8_t>(row * 8 + (s - row));
            }
        } else {
            for (int row = std::max(0, s - 7); row <= std::min(s, 7); ++row) {
                out[static_cast<std::size_t>(k++)] = static_cast<std::uint8_t>(row * 8 + (s - row));
            }
        }
    }
    return out;
}
constexpr std::array<std::uint8_t, 64> kZigzag = make_zigzag();

// A.3.3: c[x][u] = C(u) / 2 * cos((2x + 1) u pi / 16), so the IDCT is two
// passes of an 8 x 8 product.
struct IdctBasis {
    std::array<std::array<double, 8>, 8> c{};
    IdctBasis() {
        for (int x = 0; x < 8; ++x) {
            for (int u = 0; u < 8; ++u) {
                const double cu = u == 0 ? 1.0 / std::numbers::sqrt2 : 1.0;
                c[static_cast<std::size_t>(x)][static_cast<std::size_t>(u)] =
                    0.5 * cu * std::cos((2.0 * x + 1.0) * u * std::numbers::pi / 16.0);
            }
        }
    }
};

const IdctBasis& basis() {
    static const IdctBasis b;
    return b;
}

// C.2 and figure F.15.
struct HuffmanDecoder {
    std::array<std::int32_t, 17> mincode{};
    std::array<std::int32_t, 17> maxcode{};
    std::array<std::int32_t, 17> valptr{};
    std::vector<std::uint8_t> values;
    bool present = false;
};

Expected<HuffmanDecoder> build_huffman(const JpegHuffmanSpec& spec) {
    HuffmanDecoder d;
    std::size_t total = 0;
    for (const std::uint8_t b : spec.bits) {
        total += b;
    }
    if (total == 0 || total > 256 || total != spec.values.size()) {
        return fail("jpeg: a Huffman table's BITS do not match its values");
    }
    d.values = spec.values;
    std::int32_t code = 0;
    std::int32_t k = 0;
    for (int length = 1; length <= 16; ++length) {
        const int count = spec.bits[static_cast<std::size_t>(length - 1)];
        if (count == 0) {
            d.maxcode[static_cast<std::size_t>(length)] = -1;
        } else {
            d.valptr[static_cast<std::size_t>(length)] = k;
            d.mincode[static_cast<std::size_t>(length)] = code;
            code += count;
            k += count;
            d.maxcode[static_cast<std::size_t>(length)] = code - 1;
            if (code > (1 << length)) {
                return fail("jpeg: a Huffman table holds more codes than its lengths allow");
            }
        }
        code <<= 1;
    }
    d.present = true;
    return d;
}

// F.2.2.5 NEXTBIT, over an entropy-coded segment. A marker, or the end of
// the data, feeds zeros and is remembered rather than read through.
class BitReader {
public:
    BitReader(std::span<const std::uint8_t> data, bool stuffed) : data_(data), stuffed_(stuffed) {}

    int bit() {
        if (count_ == 0) {
            if (blocked_ || at_ >= data_.size()) {
                short_ = true;
                return 0;
            }
            const std::uint8_t b = data_[at_];
            if (stuffed_ && b == 0xFF) {
                if (at_ + 1 < data_.size() && data_[at_ + 1] == 0x00) {
                    at_ += 2;
                } else {
                    blocked_ = true;   // a marker
                    short_ = true;
                    return 0;
                }
            } else {
                ++at_;
            }
            byte_ = b;
            count_ = 8;
        }
        --count_;
        return (byte_ >> count_) & 1;
    }

    // F.2.2.4 RECEIVE.
    int receive(int ssss) {
        int v = 0;
        for (int i = 0; i < ssss; ++i) {
            v = (v << 1) | bit();
        }
        return v;
    }

    // E.2.4: at a restart, the bits left in the byte are padding and an RSTm
    // marker follows. Finds the next one if this is not it.
    void restart() {
        count_ = 0;
        blocked_ = false;
        while (at_ + 1 < data_.size()) {
            if (data_[at_] == 0xFF && data_[at_ + 1] >= 0xD0 && data_[at_ + 1] <= 0xD7) {
                at_ += 2;
                return;
            }
            ++at_;
        }
        at_ = data_.size();
    }

    [[nodiscard]] bool ran_short() const { return short_; }

private:
    std::span<const std::uint8_t> data_;
    bool stuffed_;
    std::size_t at_ = 0;
    std::uint8_t byte_ = 0;
    int count_ = 0;
    bool blocked_ = false;
    bool short_ = false;
};

// F.2.2.3 DECODE, figure F.16. Nothing for a code no table holds.
std::optional<std::uint8_t> decode_symbol(BitReader& in, const HuffmanDecoder& h) {
    std::int32_t code = in.bit();
    for (int i = 1; i <= 16; ++i) {
        const auto l = static_cast<std::size_t>(i);
        if (h.maxcode[l] >= 0 && code <= h.maxcode[l]) {
            const std::int32_t j = h.valptr[l] + code - h.mincode[l];
            if (j < 0 || static_cast<std::size_t>(j) >= h.values.size()) {
                return std::nullopt;
            }
            return h.values[static_cast<std::size_t>(j)];
        }
        code = (code << 1) | in.bit();
    }
    return std::nullopt;
}

// Figure F.12 EXTEND.
int extend(int v, int t) {
    return t == 0 ? 0 : (v < (1 << (t - 1)) ? v - (1 << t) + 1 : v);
}

struct Plane {
    std::uint32_t stride = 0;
    std::uint32_t rows = 0;
    std::vector<std::uint8_t> px;
};

struct Frame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<JpegComponent> comps;
    int hmax = 1;
    int vmax = 1;
    std::uint32_t mcux = 0;
    std::uint32_t mcuy = 0;
    std::vector<Plane> planes;
    std::array<JpegQuantSpec, 4> quant{};
    std::array<bool, 4> quant_set{};
    std::array<HuffmanDecoder, 4> dc{};
    std::array<HuffmanDecoder, 4> ac{};
    std::uint32_t restart = 0;
    bool damaged = false;
};

Status start_frame(Frame& f) {
    if (f.width == 0 || f.height == 0 || f.comps.empty() || f.comps.size() > 4) {
        return fail("jpeg: the frame has no size or no components");
    }
    f.hmax = 1;
    f.vmax = 1;
    for (const JpegComponent& c : f.comps) {
        if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4 || c.tq > 3) {
            return fail("jpeg: a component's sampling factor or table is out of range");
        }
        f.hmax = std::max<int>(f.hmax, c.h);
        f.vmax = std::max<int>(f.vmax, c.v);
    }
    // A.2.3: MCUs of 8 Hmax by 8 Vmax samples.
    f.mcux = (f.width + 8U * f.hmax - 1) / (8U * f.hmax);
    f.mcuy = (f.height + 8U * f.vmax - 1) / (8U * f.vmax);
    f.planes.clear();
    for (const JpegComponent& c : f.comps) {
        Plane p;
        p.stride = f.mcux * c.h * 8;
        p.rows = f.mcuy * c.v * 8;
        p.px.assign(static_cast<std::size_t>(p.stride) * p.rows, 128);
        f.planes.push_back(std::move(p));
    }
    return {};
}

// A.3.4 dequantization and the A.3.3 IDCT, A.3.1's level shift undone, into
// the plane at block (bx, by).
void put_block(const std::array<int, 64>& zz, const JpegQuantSpec& q, Plane& plane,
               std::uint32_t bx, std::uint32_t by) {
    std::array<double, 64> s{};
    for (std::size_t k = 0; k < 64; ++k) {
        s[kZigzag[k]] = static_cast<double>(zz[k]) * q.q[k];
    }
    const auto& c = basis().c;
    std::array<double, 64> rows{};
    // rows[y][u] = sum over v of c[y][v] * S[v][u].
    for (std::size_t y = 0; y < 8; ++y) {
        for (std::size_t u = 0; u < 8; ++u) {
            double t = 0.0;
            for (std::size_t v = 0; v < 8; ++v) {
                t += c[y][v] * s[v * 8 + u];
            }
            rows[y * 8 + u] = t;
        }
    }
    for (std::size_t y = 0; y < 8; ++y) {
        for (std::size_t x = 0; x < 8; ++x) {
            double t = 0.0;
            for (std::size_t u = 0; u < 8; ++u) {
                t += c[x][u] * rows[y * 8 + u];
            }
            const long v = std::lround(t + 128.0);
            plane.px[static_cast<std::size_t>(by * 8 + y) * plane.stride + bx * 8 + x] =
                static_cast<std::uint8_t>(std::clamp(v, 0L, 255L));
        }
    }
}

struct ScanComponent {
    std::size_t index = 0;   // into the frame's components
    std::uint8_t td = 0;
    std::uint8_t ta = 0;
};

// F.2.2.1 and F.2.2.2 for one block, F.2.1.3.1 for its DC prediction.
bool read_block(BitReader& in, const HuffmanDecoder& dc, const HuffmanDecoder& ac, int& pred,
                std::array<int, 64>& zz) {
    zz.fill(0);
    const auto t = decode_symbol(in, dc);
    if (!t || *t > 11) {
        return false;
    }
    pred += extend(in.receive(*t), *t);
    zz[0] = pred;
    for (int k = 1; k < 64;) {
        const auto rs = decode_symbol(in, ac);
        if (!rs) {
            return false;
        }
        const int r = *rs >> 4;
        const int ssss = *rs & 0x0F;
        if (ssss == 0) {
            if (r != 15) {
                break;   // EOB
            }
            k += 16;     // ZRL
            continue;
        }
        k += r;
        if (k > 63) {
            return false;
        }
        zz[static_cast<std::size_t>(k)] = extend(in.receive(ssss), ssss);
        ++k;
    }
    return true;
}

// A.2.2 and A.2.3: one scan's data units, with restarts every `restart` MCUs.
void decode_scan(Frame& f, const std::vector<ScanComponent>& scan,
                 std::span<const std::uint8_t> data, bool stuffed) {
    BitReader in(data, stuffed);
    std::vector<int> pred(scan.size(), 0);
    std::array<int, 64> zz{};
    bool lost = false;

    // A non-interleaved scan's MCU is one block, in the component's own
    // raster of ceil(ceil(X * H / Hmax) / 8) blocks across.
    std::uint32_t units_x = f.mcux;
    std::uint32_t units_y = f.mcuy;
    if (scan.size() == 1) {
        const JpegComponent& c = f.comps[scan[0].index];
        const std::uint32_t cx = (f.width * c.h + f.hmax - 1) / f.hmax;
        const std::uint32_t cy = (f.height * c.v + f.vmax - 1) / f.vmax;
        units_x = (cx + 7) / 8;
        units_y = (cy + 7) / 8;
    }
    const std::uint32_t total = units_x * units_y;
    for (std::uint32_t m = 0; m < total; ++m) {
        if (f.restart != 0 && stuffed && m != 0 && m % f.restart == 0) {
            in.restart();
            std::ranges::fill(pred, 0);
            lost = false;
        }
        const std::uint32_t mx = m % units_x;
        const std::uint32_t my = m / units_x;
        for (std::size_t s = 0; s < scan.size(); ++s) {
            const JpegComponent& c = f.comps[scan[s].index];
            Plane& plane = f.planes[scan[s].index];
            const HuffmanDecoder& dc = f.dc[scan[s].td];
            const HuffmanDecoder& ac = f.ac[scan[s].ta];
            const std::uint32_t bh = scan.size() == 1 ? 1 : c.h;
            const std::uint32_t bv = scan.size() == 1 ? 1 : c.v;
            for (std::uint32_t v = 0; v < bv; ++v) {
                for (std::uint32_t h = 0; h < bh; ++h) {
                    if (!lost && !read_block(in, dc, ac, pred[s], zz)) {
                        lost = true;
                    }
                    if (lost) {
                        // Until the next restart, every block keeps the
                        // prediction and nothing else.
                        zz.fill(0);
                        zz[0] = pred[s];
                    }
                    put_block(zz, f.quant[c.tq], plane, mx * bh + h, my * bv + v);
                }
            }
        }
    }
    f.damaged = f.damaged || lost || in.ran_short();
}

JpegImage render(const Frame& f) {
    JpegImage out;
    out.width = f.width;
    out.height = f.height;
    out.damaged = f.damaged;
    const auto sample = [&](std::size_t i, std::uint32_t x, std::uint32_t y) {
        const JpegComponent& c = f.comps[i];
        const Plane& p = f.planes[i];
        const std::uint32_t sx = x * c.h / static_cast<std::uint32_t>(f.hmax);
        const std::uint32_t sy = y * c.v / static_cast<std::uint32_t>(f.vmax);
        return static_cast<double>(p.px[static_cast<std::size_t>(sy) * p.stride + sx]);
    };
    if (f.comps.size() == 3) {
        out.channels = 3;
        out.pixels.resize(static_cast<std::size_t>(f.width) * f.height * 3);
        const auto clamp = [](double v) {
            return static_cast<std::uint8_t>(std::clamp(std::floor(v + 0.5), 0.0, 255.0));
        };
        for (std::uint32_t y = 0; y < f.height; ++y) {
            for (std::uint32_t x = 0; x < f.width; ++x) {
                // T.871 clause 7, the exact form.
                const double yy = sample(0, x, y);
                const double cb = sample(1, x, y) - 128.0;
                const double cr = sample(2, x, y) - 128.0;
                const std::size_t o = (static_cast<std::size_t>(y) * f.width + x) * 3;
                out.pixels[o] = clamp(yy + 1.402 * cr);
                out.pixels[o + 1] = clamp(yy - (0.114 * 1.772 * cb + 0.299 * 1.402 * cr) / 0.587);
                out.pixels[o + 2] = clamp(yy + 1.772 * cb);
            }
        }
    } else {
        out.channels = 1;
        out.pixels.resize(static_cast<std::size_t>(f.width) * f.height);
        for (std::uint32_t y = 0; y < f.height; ++y) {
            for (std::uint32_t x = 0; x < f.width; ++x) {
                out.pixels[static_cast<std::size_t>(y) * f.width + x] =
                    static_cast<std::uint8_t>(sample(0, x, y));
            }
        }
    }
    return out;
}

std::uint16_t be16(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint16_t>((b[at] << 8) | b[at + 1]);
}

}  // namespace

Expected<JpegImage> jpeg_decode(std::span<const std::uint8_t> bytes) {
    // B.1.1.2: SOI first.
    if (bytes.size() < 4 || bytes[0] != 0xFF || bytes[1] != 0xD8) {
        return fail("jpeg: no SOI marker");
    }
    Frame f;
    bool have_frame = false;
    bool any_scan = false;
    std::size_t at = 2;
    while (at + 1 < bytes.size()) {
        if (bytes[at] != 0xFF) {
            ++at;   // stray bytes between segments
            continue;
        }
        const std::uint8_t marker = bytes[at + 1];
        at += 2;
        if (marker == 0xFF || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            if (marker == 0xFF) {
                --at;   // fill bytes
            }
            continue;
        }
        if (marker == 0xD9) {
            break;      // EOI
        }
        if (at + 2 > bytes.size()) {
            return fail("jpeg: a marker segment runs past the end");
        }
        const std::size_t length = be16(bytes, at);
        if (length < 2 || at + length > bytes.size()) {
            return fail("jpeg: a marker segment runs past the end");
        }
        const std::span<const std::uint8_t> seg = bytes.subspan(at + 2, length - 2);
        at += length;
        switch (marker) {
            case 0xDB: {   // B.2.4.1 DQT
                for (std::size_t i = 0; i < seg.size();) {
                    const int pq = seg[i] >> 4;
                    const int tq = seg[i] & 0x0F;
                    const std::size_t need = 1 + 64 * (pq == 0 ? 1 : 2);
                    if (tq > 3 || pq > 1 || i + need > seg.size()) {
                        return fail("jpeg: a bad quantization table");
                    }
                    for (std::size_t k = 0; k < 64; ++k) {
                        f.quant[static_cast<std::size_t>(tq)].q[k] =
                            pq == 0 ? seg[i + 1 + k] : be16(seg, i + 1 + 2 * k);
                    }
                    f.quant_set[static_cast<std::size_t>(tq)] = true;
                    i += need;
                }
                break;
            }
            case 0xC4: {   // B.2.4.2 DHT
                for (std::size_t i = 0; i < seg.size();) {
                    if (i + 17 > seg.size()) {
                        return fail("jpeg: a bad Huffman table");
                    }
                    const int tc = seg[i] >> 4;
                    const int th = seg[i] & 0x0F;
                    JpegHuffmanSpec spec;
                    std::size_t n = 0;
                    for (std::size_t l = 0; l < 16; ++l) {
                        spec.bits[l] = seg[i + 1 + l];
                        n += spec.bits[l];
                    }
                    if (tc > 1 || th > 3 || i + 17 + n > seg.size()) {
                        return fail("jpeg: a bad Huffman table");
                    }
                    spec.values.assign(seg.begin() + static_cast<std::ptrdiff_t>(i + 17),
                                       seg.begin() + static_cast<std::ptrdiff_t>(i + 17 + n));
                    auto built = build_huffman(spec);
                    if (!built) {
                        return std::unexpected(built.error());
                    }
                    (tc == 0 ? f.dc : f.ac)[static_cast<std::size_t>(th)] = std::move(*built);
                    i += 17 + n;
                }
                break;
            }
            case 0xDD:   // B.2.4.4 DRI
                if (seg.size() < 2) {
                    return fail("jpeg: a bad restart interval");
                }
                f.restart = be16(seg, 0);
                break;
            case 0xC0:
            case 0xC1: {   // B.2.2, baseline and extended sequential Huffman
                if (seg.size() < 6 || seg[0] != 8) {
                    return fail("jpeg: only 8-bit sample precision is decoded");
                }
                f.height = be16(seg, 1);
                f.width = be16(seg, 3);
                const std::size_t nf = seg[5];
                if (f.height == 0) {
                    return fail("jpeg: a frame sized by a DNL marker is not decoded");
                }
                if (seg.size() < 6 + 3 * nf) {
                    return fail("jpeg: a bad frame header");
                }
                f.comps.clear();
                for (std::size_t i = 0; i < nf; ++i) {
                    JpegComponent c;
                    c.id = seg[6 + 3 * i];
                    c.h = seg[7 + 3 * i] >> 4;
                    c.v = seg[7 + 3 * i] & 0x0F;
                    c.tq = seg[8 + 3 * i];
                    f.comps.push_back(c);
                }
                if (auto ok = start_frame(f); !ok) {
                    return std::unexpected(ok.error());
                }
                have_frame = true;
                break;
            }
            case 0xDA: {   // B.2.3 SOS
                if (!have_frame || seg.empty()) {
                    return fail("jpeg: a scan before its frame header");
                }
                const std::size_t ns = seg[0];
                if (ns < 1 || ns > 4 || seg.size() < 1 + 2 * ns + 3) {
                    return fail("jpeg: a bad scan header");
                }
                std::vector<ScanComponent> scan;
                for (std::size_t i = 0; i < ns; ++i) {
                    const std::uint8_t id = seg[1 + 2 * i];
                    const auto found = std::ranges::find(f.comps, id, &JpegComponent::id);
                    if (found == f.comps.end()) {
                        return fail("jpeg: a scan names a component the frame lacks");
                    }
                    ScanComponent sc;
                    sc.index = static_cast<std::size_t>(found - f.comps.begin());
                    sc.td = seg[2 + 2 * i] >> 4;
                    sc.ta = seg[2 + 2 * i] & 0x0F;
                    if (sc.td > 3 || sc.ta > 3 || !f.dc[sc.td].present || !f.ac[sc.ta].present ||
                        !f.quant_set[f.comps[sc.index].tq]) {
                        return fail("jpeg: a scan uses a table that was never defined");
                    }
                    scan.push_back(sc);
                }
                // The entropy-coded data runs to the first marker that is
                // not stuffing or a restart.
                std::size_t stop = at;
                while (stop + 1 < bytes.size() &&
                       !(bytes[stop] == 0xFF && bytes[stop + 1] != 0x00 &&
                         !(bytes[stop + 1] >= 0xD0 && bytes[stop + 1] <= 0xD7))) {
                    ++stop;
                }
                if (stop + 1 >= bytes.size()) {
                    stop = bytes.size();
                }
                decode_scan(f, scan, bytes.subspan(at, stop - at), true);
                any_scan = true;
                at = stop;
                break;
            }
            case 0xDC:
                return fail("jpeg: a DNL marker is not decoded");
            default:
                if ((marker >= 0xC2 && marker <= 0xCF) && marker != 0xC4 && marker != 0xC8 &&
                    marker != 0xCC) {
                    return fail(std::format("jpeg: SOF{} frames are not baseline", marker - 0xC0));
                }
                break;   // APPn, COM and the rest carry nothing a decoder needs
        }
    }
    if (!any_scan) {
        return fail("jpeg: no scan");
    }
    return render(f);
}

Expected<JpegImage> jpeg_decode_entropy(const JpegTables& tables,
                                        std::span<const std::uint8_t> scan_bytes,
                                        bool byte_stuffed) {
    Frame f;
    f.width = tables.width;
    f.height = tables.height;
    f.comps = tables.components;
    f.restart = tables.restart_interval;
    f.quant = tables.quant;
    f.quant_set.fill(true);
    std::vector<ScanComponent> scan;
    for (std::size_t i = 0; i < f.comps.size(); ++i) {
        const JpegComponent& c = f.comps[i];
        if (c.td > 3 || c.ta > 3) {
            return fail("jpeg: a component names a table out of range");
        }
        for (const auto& [spec, slot] : {std::pair{&tables.dc[c.td], &f.dc[c.td]},
                                         std::pair{&tables.ac[c.ta], &f.ac[c.ta]}}) {
            if (!slot->present) {
                auto built = build_huffman(*spec);
                if (!built) {
                    return std::unexpected(built.error());
                }
                *slot = std::move(*built);
            }
        }
        scan.push_back(ScanComponent{i, c.td, c.ta});
    }
    if (auto ok = start_frame(f); !ok) {
        return std::unexpected(ok.error());
    }
    decode_scan(f, scan, scan_bytes, byte_stuffed);
    return render(f);
}

}  // namespace revenant::decode
