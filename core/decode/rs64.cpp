#include "core/decode/rs64.h"

#include <algorithm>

namespace revenant::decode {

namespace {

struct Field {
    std::array<std::uint8_t, 126> exp{};
    std::array<int, 64> log{};
    std::array<std::uint8_t, kRs64Roots + 1> gen{};  // gen[i]: coefficient of x^i
};

const Field& field() {
    static const Field f = [] {
        Field t;
        int x = 1;
        for (int i = 0; i < 63; ++i) {
            t.exp[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(x);
            t.log[static_cast<std::size_t>(x)] = i;
            x <<= 1;
            if (x & 64) x ^= 0x43;  // x^6 + x + 1
        }
        for (int i = 63; i < 126; ++i) t.exp[static_cast<std::size_t>(i)] = t.exp[static_cast<std::size_t>(i - 63)];
        t.log[0] = -1;
        // Product of (x - alpha^j) for j = 3 .. 53.
        std::array<std::uint8_t, kRs64Roots + 1> g{};
        g[0] = 1;
        int deg = 0;
        for (int j = kRs64FirstRoot; j < kRs64FirstRoot + kRs64Roots; ++j) {
            const std::uint8_t root = t.exp[static_cast<std::size_t>(j % 63)];
            for (int i = deg + 1; i >= 0; --i) {
                std::uint8_t v = i > 0 ? g[static_cast<std::size_t>(i - 1)] : 0;
                const std::uint8_t c = g[static_cast<std::size_t>(i)];
                if (c != 0) v ^= t.exp[static_cast<std::size_t>((t.log[c] + t.log[root]) % 63)];
                g[static_cast<std::size_t>(i)] = v;
            }
            ++deg;
        }
        t.gen = g;
        return t;
    }();
    return f;
}

std::uint8_t mul(const Field& f, std::uint8_t a, std::uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return f.exp[static_cast<std::size_t>(f.log[a] + f.log[b])];
}

std::uint8_t pow_alpha(const Field& f, int e) {
    e %= 63;
    if (e < 0) e += 63;
    return f.exp[static_cast<std::size_t>(e)];
}

std::uint8_t inv(const Field& f, std::uint8_t a) { return f.exp[static_cast<std::size_t>((63 - f.log[a]) % 63)]; }

}  // namespace

Rs64Word rs64_encode(const Rs64Message& message) {
    const Field& f = field();
    std::array<std::uint8_t, kRs64N> d{};
    for (int k = 0; k < kRs64K; ++k) d[static_cast<std::size_t>(kRs64Roots + k)] = message[static_cast<std::size_t>(k)] & 63U;
    for (int deg = kRs64N - 1; deg >= kRs64Roots; --deg) {
        const std::uint8_t c = d[static_cast<std::size_t>(deg)];
        if (c == 0) continue;
        for (int i = 0; i <= kRs64Roots; ++i) {
            d[static_cast<std::size_t>(deg - kRs64Roots + i)] ^= mul(f, c, f.gen[static_cast<std::size_t>(i)]);
        }
    }
    Rs64Word w{};
    for (int i = 0; i < kRs64Roots; ++i) w[static_cast<std::size_t>(i)] = d[static_cast<std::size_t>(i)];
    for (int k = 0; k < kRs64K; ++k) w[static_cast<std::size_t>(kRs64Roots + k)] = message[static_cast<std::size_t>(k)] & 63U;
    return w;
}

Rs64Syndromes rs64_syndromes(const Rs64Word& r) {
    const Field& f = field();
    Rs64Syndromes s{};
    for (int j = 0; j < kRs64Roots; ++j) {
        // Horner from the highest coefficient.
        const std::uint8_t a = pow_alpha(f, kRs64FirstRoot + j);
        std::uint8_t acc = 0;
        for (int i = kRs64N - 1; i >= 0; --i) acc = static_cast<std::uint8_t>(mul(f, acc, a) ^ r[static_cast<std::size_t>(i)]);
        s[static_cast<std::size_t>(j)] = acc;
    }
    return s;
}

std::optional<Rs64Word> rs64_decode(const Rs64Word& received, const Rs64Syndromes& syn, std::span<const int> erasures) {
    const Field& f = field();
    const int s = static_cast<int>(erasures.size());
    if (s > kRs64Roots) return std::nullopt;
    if (std::all_of(syn.begin(), syn.end(), [](std::uint8_t v) { return v == 0; })) return received;

    // Erasure locator, product of (1 + X x) for each erased X = alpha^i.
    std::array<std::uint8_t, kRs64Roots + 1> lambda{};
    lambda[0] = 1;
    for (int e = 0; e < s; ++e) {
        const std::uint8_t x = pow_alpha(f, erasures[static_cast<std::size_t>(e)]);
        for (int i = e + 1; i > 0; --i) lambda[static_cast<std::size_t>(i)] ^= mul(f, x, lambda[static_cast<std::size_t>(i - 1)]);
    }
    std::array<std::uint8_t, kRs64Roots + 1> b = lambda;
    std::array<std::uint8_t, kRs64Roots + 1> t{};
    int el = s;
    // Berlekamp-Massey from the erasure locator.
    for (int r = s + 1; r <= kRs64Roots; ++r) {
        std::uint8_t discr = 0;
        for (int i = 0; i < r; ++i) discr ^= mul(f, lambda[static_cast<std::size_t>(i)], syn[static_cast<std::size_t>(r - 1 - i)]);
        if (discr == 0) {
            for (int i = kRs64Roots; i > 0; --i) b[static_cast<std::size_t>(i)] = b[static_cast<std::size_t>(i - 1)];
            b[0] = 0;
            continue;
        }
        t[0] = lambda[0];
        for (int i = 1; i <= kRs64Roots; ++i) t[static_cast<std::size_t>(i)] = lambda[static_cast<std::size_t>(i)] ^ mul(f, discr, b[static_cast<std::size_t>(i - 1)]);
        if (2 * el <= r + s - 1) {
            el = r + s - el;
            const std::uint8_t di = inv(f, discr);
            for (int i = 0; i <= kRs64Roots; ++i) b[static_cast<std::size_t>(i)] = mul(f, lambda[static_cast<std::size_t>(i)], di);
        } else {
            for (int i = kRs64Roots; i > 0; --i) b[static_cast<std::size_t>(i)] = b[static_cast<std::size_t>(i - 1)];
            b[0] = 0;
        }
        lambda = t;
    }
    int deg = 0;
    for (int i = 0; i <= kRs64Roots; ++i) {
        if (lambda[static_cast<std::size_t>(i)] != 0) deg = i;
    }
    if (deg == 0) return std::nullopt;

    // Chien search: a root at alpha^-i places an error at index i.
    std::array<int, kRs64Roots> loc{};
    int roots = 0;
    for (int i = 0; i < kRs64N; ++i) {
        std::uint8_t acc = 0;
        for (int k = deg; k >= 0; --k) acc = static_cast<std::uint8_t>(mul(f, acc, pow_alpha(f, -i)) ^ lambda[static_cast<std::size_t>(k)]);
        if (acc == 0) {
            if (roots == kRs64Roots) return std::nullopt;
            loc[static_cast<std::size_t>(roots++)] = i;
        }
    }
    if (roots != deg) return std::nullopt;

    // Omega = S(x) Lambda(x) mod x^51.
    std::array<std::uint8_t, kRs64Roots> omega{};
    for (int i = 0; i < kRs64Roots; ++i) {
        std::uint8_t acc = 0;
        for (int k = 0; k <= std::min(i, deg); ++k) acc ^= mul(f, lambda[static_cast<std::size_t>(k)], syn[static_cast<std::size_t>(i - k)]);
        omega[static_cast<std::size_t>(i)] = acc;
    }
    Rs64Word out = received;
    for (int n = 0; n < roots; ++n) {
        const int i = loc[static_cast<std::size_t>(n)];
        const std::uint8_t xinv = pow_alpha(f, -i);
        std::uint8_t num = 0;
        for (int k = kRs64Roots - 1; k >= 0; --k) num = static_cast<std::uint8_t>(mul(f, num, xinv) ^ omega[static_cast<std::size_t>(k)]);
        // Formal derivative: odd terms only.
        std::uint8_t den = 0;
        for (int k = 1; k <= deg; k += 2) {
            den ^= mul(f, lambda[static_cast<std::size_t>(k)], pow_alpha(f, -i * (k - 1)));
        }
        if (den == 0) return std::nullopt;
        // e = X^(1 - fcr) Omega(X^-1) / Lambda'(X^-1).
        const std::uint8_t e = mul(f, mul(f, num, inv(f, den)), pow_alpha(f, i * (1 - kRs64FirstRoot)));
        out[static_cast<std::size_t>(i)] ^= e;
    }
    const auto check = rs64_syndromes(out);
    if (!std::all_of(check.begin(), check.end(), [](std::uint8_t v) { return v == 0; })) return std::nullopt;
    return out;
}

}  // namespace revenant::decode
