#include "core/decode/link11.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace revenant::decode {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

// Table III's sequence, s[n] = s[n-1] ^ s[n-6], period 63, s[0..5] the
// window of address 01.
const std::array<std::uint8_t, 63>& sequence() {
    static const std::array<std::uint8_t, 63> s = [] {
        std::array<std::uint8_t, 63> v{};
        v[5] = 1;
        for (std::size_t n = 6; n < 63; ++n) {
            v[n] = static_cast<std::uint8_t>(v[n - 1] ^ v[n - 6]);
        }
        return v;
    }();
    return s;
}

// The code whose six-bit window (s[k..k+5], s[k] the MSB) is `value`.
Link11Code code_for(int value) {
    const auto& s = sequence();
    int k = -1;
    for (int i = 0; i < 63; ++i) {
        int w = 0;
        for (int j = 0; j < 6; ++j) {
            w = (w << 1) | s[static_cast<std::size_t>((i + j) % 63)];
        }
        if (w == value) {
            k = i;
            break;
        }
    }
    Link11Code c;
    if (k < 0) {
        return c;
    }
    // Frame 1 is the 30 bits ending with the window, bit 29 the earliest;
    // frame 2 the 30 before those (table III, checked row by row in the
    // tests).
    for (int i = 0; i < 30; ++i) {
        const auto b1 = s[static_cast<std::size_t>(((k + 6 - 30 + i) % 63 + 63) % 63)];
        const auto b2 = s[static_cast<std::size_t>(((k + 6 - 60 + i) % 63 + 63) % 63)];
        c.first |= static_cast<std::uint32_t>(b1) << static_cast<unsigned>(29 - i);
        c.second |= static_cast<std::uint32_t>(b2) << static_cast<unsigned>(29 - i);
    }
    return c;
}

// 5.2.4.1 and table V: the check sets, K1 to K16, each including its own
// parity bit; K0 is every bit.
std::uint32_t mask_of(std::initializer_list<int> bits) {
    std::uint32_t m = 0;
    for (const int b : bits) {
        m |= 1U << static_cast<unsigned>(b);
    }
    return m;
}
const std::array<std::uint32_t, 5>& checks() {
    static const std::array<std::uint32_t, 5> c = {
        mask_of({0, 1, 3, 4, 6, 8, 10, 11, 13, 15, 17, 19, 21, 23, 25}),         // K1
        mask_of({0, 2, 3, 5, 6, 9, 10, 12, 13, 16, 17, 20, 21, 26}),             // K2
        mask_of({1, 2, 3, 7, 8, 9, 10, 14, 15, 16, 17, 22, 23, 27}),             // K4
        mask_of({4, 5, 6, 7, 8, 9, 10, 18, 19, 20, 21, 22, 23, 28}),             // K8
        mask_of({11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 29}),       // K16
    };
    return c;
}

int parity(std::uint32_t v) { return std::popcount(v) & 1; }

double wrap(double a) { return std::remainder(a, kTwoPi); }

}  // namespace

int link11_phase_step(int even, int odd) {
    // Table I: 1 1 is -45, 0 1 is -135, 0 0 is -225, 1 0 is -315.
    if (odd != 0) {
        return even != 0 ? 1 : 3;
    }
    return even != 0 ? 7 : 5;
}

Link11Code link11_start_code() { return code_for(077); }

Link11Code link11_address_code(int address) {
    if (address < 1 || address > 076) {
        return {};
    }
    return code_for(address);
}

std::uint32_t link11_encode_word(std::uint32_t data) {
    std::uint32_t f = data & 0xFFFFFFU;
    const auto& c = checks();
    for (std::size_t i = 0; i < 5; ++i) {
        // Odd parity over the set, its own bit included.
        if (parity(f & c[i]) == 0) {
            f |= 1U << static_cast<unsigned>(25 + i);
        }
    }
    if (parity(f) == 0) {
        f |= 1U << 24U;
    }
    return f;
}

Link11WordCheck link11_check_word(std::uint32_t frame) {
    const auto& c = checks();
    int syndrome = 0;
    for (std::size_t i = 0; i < 5; ++i) {
        if (parity(frame & c[i]) == 0) {
            syndrome |= 1 << i;
        }
    }
    const bool overall_bad = parity(frame & kLink11AllOnes) == 0;
    Link11WordCheck r;
    r.data = frame & 0xFFFFFFU;
    if (syndrome == 0 && !overall_bad) {
        return r;
    }
    if (!overall_bad) {
        r.status = -1;  // an even number of errors
        return r;
    }
    // A single error: the bit whose check membership is the syndrome.
    for (int b = 0; b < 30; ++b) {
        int pattern = 0;
        for (std::size_t i = 0; i < 5; ++i) {
            if ((c[i] >> static_cast<unsigned>(b) & 1U) != 0) {
                pattern |= 1 << i;
            }
        }
        if (pattern == syndrome) {
            r.data = (frame ^ (1U << static_cast<unsigned>(b))) & 0xFFFFFFU;
            r.status = 1;
            return r;
        }
    }
    r.status = -1;
    return r;
}

// ---------------------------------------------------------------------------
// Parsing a transmission's frames
// ---------------------------------------------------------------------------

namespace {

struct Match {
    int value = -1;  // 077 start, 1..076 address
    int errors = 99;
};

Match match_code(std::uint32_t a, std::uint32_t b) {
    Match m;
    for (int v = 1; v <= 077; ++v) {
        const auto c = code_for(v);
        const int e1 = std::popcount(a ^ c.first);
        const int e2 = std::popcount(b ^ c.second);
        // 5.1.4.3.1: four or fewer errors a frame.
        if (e1 <= 4 && e2 <= 4 && e1 + e2 < m.errors) {
            m = {v, e1 + e2};
        }
    }
    return m;
}

bool is_fill(std::uint32_t a, std::uint32_t b, std::uint32_t fill, int& errors) {
    const int e1 = std::popcount((a ^ fill) & kLink11AllOnes);
    const int e2 = std::popcount((b ^ fill) & kLink11AllOnes);
    errors = e1 + e2;
    return e1 <= 4 && e2 <= 4;
}

// Returns true when the frames end the transmission (an address code, or a
// picket stop code).
bool parse(const std::vector<std::uint32_t>& bits, bool correct, Link11Transmission& t) {
    t.frames.clear();
    t.address = -1;
    t.started = t.picket_stop = t.control_stop = false;
    t.words = t.corrected = t.failed = 0;
    const std::size_t n = bits.size();
    std::size_t i = 0;
    const auto add_code = [&](Link11Frame::Kind kind, int address, int errors) {
        for (int k = 0; k < 2; ++k) {
            Link11Frame f;
            f.kind = kind;
            f.bits = bits[i + static_cast<std::size_t>(k)];
            f.address = address;
            f.code_errors = errors;
            t.frames.push_back(f);
        }
        i += 2;
    };
    if (n >= 2) {
        const auto m = match_code(bits[0], bits[1]);
        if (m.value == 077) {
            t.started = true;
            add_code(Link11Frame::Kind::StartCode, -1, m.errors);
        } else if (m.value > 0) {
            t.address = m.value;
            add_code(Link11Frame::Kind::AddressCode, m.value, m.errors);
            return true;
        }
    }
    if (!t.started) {
        for (; i < n; ++i) {
            Link11Frame f;
            f.bits = bits[i];
            t.frames.push_back(f);
        }
        return false;
    }
    while (i < n) {
        int errors = 0;
        if (i + 1 < n && is_fill(bits[i], bits[i + 1], 0, errors)) {
            t.control_stop = true;
            add_code(Link11Frame::Kind::ControlStop, -1, errors);
            if (i + 1 < n) {
                const auto m = match_code(bits[i], bits[i + 1]);
                if (m.value > 0 && m.value < 077) {
                    t.address = m.value;
                    add_code(Link11Frame::Kind::AddressCode, m.value, m.errors);
                    return true;
                }
            }
            continue;
        }
        if (i + 1 < n && is_fill(bits[i], bits[i + 1], kLink11AllOnes, errors)) {
            t.picket_stop = true;
            add_code(Link11Frame::Kind::PicketStop, -1, errors);
            return true;
        }
        Link11Frame f;
        f.kind = Link11Frame::Kind::Data;
        f.bits = bits[i];
        f.word = link11_check_word(bits[i]);
        if (!correct && f.word.status == 1) {
            f.word.data = bits[i] & 0xFFFFFFU;
            f.word.status = -1;
        }
        ++t.words;
        t.corrected += f.word.status == 1 ? 1 : 0;
        t.failed += f.word.status < 0 ? 1 : 0;
        t.frames.push_back(f);
        ++i;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

Expected<Link11Decoder> Link11Decoder::create(const Link11Config& config) {
    if (config.rate < 6600 || config.rate > 192000) {
        return fail("Link-11 decoder: sample rate must be 6.6 to 192 kHz");
    }
    return Link11Decoder(config);
}

Link11Decoder::Link11Decoder(const Link11Config& config) : config_(config) {
    fs_ = static_cast<double>(config.rate);
    window_ = static_cast<int>(std::lround(fs_ / kLink11SpacingHz));
    hop_ = static_cast<int>(std::lround(fs_ * 0.005));
}

void Link11Decoder::reset() {
    buf_.clear();
    base_ = 0;
    end_ = 0;
    state_ = State::Search;
    next_hop_ = 0;
    bits_.clear();
    pending_.clear();
}

Link11Decoder::Cplx Link11Decoder::correlate(double hz, std::int64_t start) const {
    const Cplx step = std::polar(1.0, -kTwoPi * hz / fs_);
    Cplx ph{1.0, 0.0};
    Cplx acc{};
    for (int i = 0; i < window_; ++i) {
        acc += static_cast<double>(at(start + i)) * ph;
        ph *= step;
    }
    return acc;
}

bool Link11Decoder::search() {
    // The preamble: 605 Hz holds four fifths of the power (5.1.3.3).
    const int n = static_cast<int>(std::lround(fs_ * 0.0267));
    if (next_hop_ + hop_ + n > end_) {
        return false;
    }
    const int span = static_cast<int>(std::ceil(config_.max_offset_hz / 2.0));
    const auto measure = [&](std::int64_t p, double& off) {
        double energy = 0.0;
        for (int i = 0; i < n; ++i) {
            energy += static_cast<double>(at(p + i)) * at(p + i);
        }
        if (energy <= 0.0) {
            return 0.0;
        }
        std::vector<double> pw(static_cast<std::size_t>(2 * span + 1));
        for (int o = -span; o <= span; ++o) {
            const Cplx step = std::polar(1.0, -kTwoPi * (kLink11DopplerHz + 2.0 * o) / fs_);
            Cplx ph{1.0, 0.0};
            Cplx acc{};
            for (int i = 0; i < n; ++i) {
                acc += static_cast<double>(at(p + i)) * ph;
                ph *= step;
            }
            pw[static_cast<std::size_t>(o + span)] = std::norm(acc);
        }
        const auto it = std::max_element(pw.begin(), pw.end());
        const auto k = static_cast<int>(it - pw.begin());
        double frac = 0.0;
        if (k > 0 && k < 2 * span) {
            const double a = pw[static_cast<std::size_t>(k - 1)];
            const double c = pw[static_cast<std::size_t>(k + 1)];
            const double den = a - 2.0 * *it + c;
            if (den < 0.0) {
                frac = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
            }
        }
        off = 2.0 * ((k - span) + frac);
        return *it / (0.5 * n * energy);
    };
    double off = 0.0;
    const double r = measure(next_hop_, off);
    if (r < config_.detect_threshold) {
        next_hop_ += hop_;
        return true;
    }
    double off2 = 0.0;
    const double r2 = measure(next_hop_ + hop_, off2);
    detect_ = next_hop_;
    offset_ = r2 > r ? off2 : off;
    state_ = State::Timing;
    return true;
}

bool Link11Decoder::timing() {
    const double slow = fs_ * link11_frame_seconds(Link11Rate::Slow);
    const std::int64_t need = detect_ + static_cast<std::int64_t>(std::ceil(24.0 * slow)) + window_;
    if (need > end_) {
        return false;
    }
    const auto exact = [&](double hz, double t) {
        const auto s = static_cast<std::int64_t>(std::llround(t));
        return correlate(hz, s) * std::polar(1.0, -kTwoPi * hz * (static_cast<double>(s) - t) / fs_);
    };
    const double sync_hz = kLink11SyncHz + offset_;
    // Frame rate and coarse timing from the 2915 Hz reversals: consecutive
    // frames differ by one fixed rotation, so the products add coherently
    // while the window lies inside frames.
    double best_score = -1.0;
    double centre = 0.0;
    for (const auto r : {Link11Rate::Fast, Link11Rate::Slow}) {
        if (!config_.auto_rate && r != config_.frame_rate) {
            continue;
        }
        const double p = fs_ * link11_frame_seconds(r);
        const int period = static_cast<int>(std::lround(p));
        std::vector<double> f(static_cast<std::size_t>(period));
        double energy = 0.0;
        for (int u = 0; u < period; ++u) {
            Cplx acc{};
            Cplx prev = exact(sync_hz, static_cast<double>(detect_ + u));
            double e = std::norm(prev);
            for (int m = 1; m <= 3; ++m) {
                const Cplx cur = exact(sync_hz, static_cast<double>(detect_ + u) + m * p);
                acc += prev * std::conj(cur);
                e += std::norm(cur);
                prev = cur;
            }
            f[static_cast<std::size_t>(u)] = std::abs(acc);
            energy += e;
        }
        const auto b = static_cast<int>(std::max_element(f.begin(), f.end()) - f.begin());
        const double score = f[static_cast<std::size_t>(b)] / (energy / period + 1e-30);
        if (score > best_score) {
            best_score = score;
            rate_ = r;
            period_ = p;
            const double thr = 0.9 * f[static_cast<std::size_t>(b)];
            int lo = b;
            int hi = b;
            const auto fat = [&](int i) { return f[static_cast<std::size_t>(((i % period) + period) % period)]; };
            while (hi - lo < period - 1 && fat(lo - 1) > thr) {
                --lo;
            }
            while (hi - lo < period - 1 && fat(hi + 1) > thr) {
                ++hi;
            }
            centre = static_cast<double>(detect_) + 0.5 * (lo + hi);
        }
    }
    const auto fail_back = [&] {
        state_ = State::Search;
        next_hop_ = detect_ + 4 * hop_;
        return true;
    };
    // The phase reference: the first frame where the data tones appear.
    int ref = -1;
    for (int m = 0; m < 14; ++m) {
        const double t = centre + m * period_;
        double data = 0.0;
        for (int d = 0; d < 14; ++d) {
            data += std::norm(exact(link11_tone_hz(d) + offset_, t));
        }
        if (data > 2.0 * std::norm(exact(sync_hz, t))) {
            ref = m;
            break;
        }
    }
    if (ref < 0) {
        return fail_back();
    }
    preamble_frames_ = 0;
    const double sync_ref = std::norm(exact(sync_hz, centre + (ref - 1) * period_));
    for (int m = ref - 1; m >= 0; --m) {
        if (std::norm(exact(sync_hz, centre + m * period_)) > 0.25 * sync_ref) {
            ++preamble_frames_;
        } else {
            break;
        }
    }
    // Refine on the frames after the phase reference, under each phase
    // reading: every change is an odd multiple of 45 degrees, so the fourth
    // power of the changes adds coherently across tones only when the
    // window and the reading are both right.
    const double coarse = centre + ref * period_;
    const int period = static_cast<int>(std::lround(period_));
    const int reach = period / 6;
    double best = -1.0;
    double best_v = 0.0;
    bool best_cont = false;
    for (const bool cont : {false, true}) {
        if (config_.phase_model == Link11PhaseModel::Onset && cont) {
            continue;
        }
        if (config_.phase_model == Link11PhaseModel::Continuous && !cont) {
            continue;
        }
        std::vector<double> g(static_cast<std::size_t>(2 * reach + 1), 0.0);
        for (int v = -reach; v <= reach; ++v) {
            std::array<Cplx, 15> prev{};
            double sum = 0.0;
            for (int n = 0; n <= 8; ++n) {
                const double t = coarse + v + n * period_;
                Cplx q{};
                for (int d = 0; d < 15; ++d) {
                    const double hz = link11_tone_hz(d) + offset_;
                    const Cplx cur = exact(hz, t);
                    Cplx dd = cur * std::conj(prev[static_cast<std::size_t>(d)]);
                    if (cont) {
                        dd *= std::polar(1.0, -kTwoPi * link11_tone_hz(d) * period_ / fs_);
                    }
                    prev[static_cast<std::size_t>(d)] = cur;
                    const double a = std::abs(dd);
                    if (n > 0 && a > 0.0) {
                        q += dd * dd * dd * dd / (a * a * a);
                    }
                }
                sum += std::abs(q);
            }
            g[static_cast<std::size_t>(v + reach)] = sum;
        }
        // Smooth, then the plateau's centre.
        std::vector<double> sm(g.size(), 0.0);
        for (std::size_t i = 0; i < g.size(); ++i) {
            for (int j = -2; j <= 2; ++j) {
                const auto k = static_cast<std::size_t>(std::clamp<int>(static_cast<int>(i) + j, 0, static_cast<int>(g.size()) - 1));
                sm[i] += g[k];
            }
        }
        const auto b = static_cast<int>(std::max_element(sm.begin(), sm.end()) - sm.begin());
        const double thr = 0.95 * sm[static_cast<std::size_t>(b)];
        int lo = b;
        int hi = b;
        while (lo > 0 && sm[static_cast<std::size_t>(lo - 1)] > thr) {
            --lo;
        }
        while (hi + 1 < static_cast<int>(sm.size()) && sm[static_cast<std::size_t>(hi + 1)] > thr) {
            ++hi;
        }
        if (sm[static_cast<std::size_t>(b)] > best) {
            best = sm[static_cast<std::size_t>(b)];
            best_v = 0.5 * (lo + hi) - reach;
            best_cont = cont;
        }
    }
    t0_ = coarse + best_v;
    continuous_ = best_cont;
    state_ = State::Data;
    index_ = 0;
    rho_ = 0.0;
    doppler_power_ = 0.0;
    weak_ = 0;
    bits_.clear();
    done_ = false;
    return true;
}

bool Link11Decoder::frame() {
    const double exact_t = t0_ + index_ * period_;
    const auto s = static_cast<std::int64_t>(std::llround(exact_t));
    if (s + window_ > end_) {
        return false;
    }
    std::array<Cplx, 16> x{};
    for (int k = 0; k < 16; ++k) {
        const double hz = (k == 0 ? kLink11DopplerHz : link11_tone_hz(k - 1)) + offset_;
        x[static_cast<std::size_t>(k)] =
            correlate(hz, s) * std::polar(1.0, -kTwoPi * hz * (static_cast<double>(s) - exact_t) / fs_);
    }
    const int index = index_++;
    const double pd = std::norm(x[0]);
    if (index == 0) {
        prev_ = x;
        doppler_power_ = pd;
        rho_ = 0.0;
        return true;
    }
    std::array<Cplx, 16> d{};
    for (std::size_t k = 0; k < 16; ++k) {
        d[k] = x[k] * std::conj(prev_[k]);
        if (continuous_ && k > 0) {
            d[k] *= std::polar(1.0, -kTwoPi * link11_tone_hz(static_cast<int>(k) - 1) * period_ / fs_);
        }
    }
    prev_ = x;
    if (index == 1) {
        rho_ = wrap(std::arg(d[0]) - kTwoPi * kLink11DopplerHz * period_ / fs_);
    }
    Cplx q{};
    double amp = 0.0;
    for (std::size_t k = 1; k < 16; ++k) {
        const double a = std::abs(d[k]);
        if (a <= 0.0) {
            continue;
        }
        const Cplx u = d[k] * std::polar(1.0, -rho_) / a;
        q += a * u * u * u * u;
        amp += a;
    }
    if (amp > 0.0) {
        rho_ = wrap(rho_ + 0.3 * std::arg(-q) / 4.0);
    }
    // End of the transmission: the Doppler tone gone for two frames.
    if (pd < 0.1 * doppler_power_) {
        if (++weak_ >= 2) {
            done_ = true;
            if (!bits_.empty()) {
                bits_.pop_back();
            }
            finish();
            return true;
        }
    } else {
        weak_ = 0;
        doppler_power_ = 0.9 * doppler_power_ + 0.1 * pd;
    }
    std::uint32_t word = 0;
    for (std::size_t k = 1; k < 16; ++k) {
        const Cplx u = d[k] * std::polar(1.0, -rho_);
        // Table I in units of -45 degrees: even bit MARK at -45 and -315
        // (cos > 0), odd bit MARK at -45 and -135 (sin of the negated
        // change > 0).
        const auto loc = static_cast<unsigned>(2 * (k - 1));
        if (std::real(u) > 0.0) {
            word |= 1U << loc;
        }
        if (-std::imag(u) > 0.0) {
            word |= 1U << (loc + 1U);
        }
    }
    bits_.push_back(word);
    Link11Transmission probe;
    if (parse(bits_, config_.correct, probe) || bits_.size() > 100000) {
        finish();
    }
    return true;
}

void Link11Decoder::finish() {
    Link11Transmission t;
    parse(bits_, config_.correct, t);
    t.position = static_cast<std::int64_t>(std::llround(t0_ - 0.5 * (period_ - window_)));
    t.frame_rate = rate_;
    t.continuous_phase = continuous_;
    t.offset_hz = offset_;
    t.preamble_frames = preamble_frames_;
    if (!bits_.empty() || t.address >= 0) {
        pending_.push_back(std::move(t));
    }
    bits_.clear();
    state_ = State::Search;
    next_hop_ = static_cast<std::int64_t>(std::llround(t0_ + index_ * period_));
}

void Link11Decoder::trim() {
    std::int64_t keep = end_;
    switch (state_) {
    case State::Search: keep = next_hop_; break;
    case State::Timing: keep = detect_; break;
    case State::Data: keep = static_cast<std::int64_t>(std::floor(t0_ + index_ * period_)) - 2; break;
    }
    keep = std::min(keep, end_);
    if (keep - base_ > 65536) {
        const auto drop = static_cast<std::size_t>(keep - base_);
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(drop));
        base_ = keep;
    }
}

bool Link11Decoder::step() {
    switch (state_) {
    case State::Search: return search();
    case State::Timing: return timing();
    case State::Data: return frame();
    }
    return false;
}

void Link11Decoder::process(dsp::ConstRealSpan audio, std::vector<Link11Transmission>& out) {
    buf_.insert(buf_.end(), audio.begin(), audio.end());
    end_ += static_cast<std::int64_t>(audio.size());
    while (step()) {
        trim();
    }
    for (auto& t : pending_) {
        out.push_back(std::move(t));
    }
    pending_.clear();
}

void Link11Decoder::flush(std::vector<Link11Transmission>& out) {
    const auto pad = static_cast<std::size_t>(std::ceil(fs_ * 0.6));
    const std::vector<float> zeros(pad, 0.0F);
    process(zeros, out);
    if (state_ == State::Data) {
        finish();
    }
    for (auto& t : pending_) {
        out.push_back(std::move(t));
    }
    pending_.clear();
    reset();
}

}  // namespace revenant::decode
