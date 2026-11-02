#include "core/decode/bch.h"

#include <algorithm>
#include <set>

namespace revenant::decode {

unsigned BchCodec::mul(unsigned a, unsigned b) const {
    if (a == 0 || b == 0) {
        return 0;
    }
    return exp_[(log_[a] + log_[b]) % n_];
}

Expected<BchCodec> BchCodec::create(const BchParams& params) {
    const unsigned m = params.m;
    if (m < 3 || m > 16) {
        return fail("a BCH code here is over GF(2^m) with m from 3 to 16");
    }
    if ((params.field_polynomial >> m) != 1U) {
        return fail("a GF(2^m) field polynomial has degree m");
    }

    BchCodec codec;
    codec.n_ = (std::size_t{1} << m) - 1U;
    const std::size_t n = codec.n_;
    codec.exp_.assign(n, 0);
    codec.log_.assign(n + 1, 0);
    std::vector<bool> seen(n + 1, false);
    unsigned x = 1;
    for (std::size_t i = 0; i < n; ++i) {
        if (seen[x]) {
            return fail("the field polynomial is not primitive");
        }
        seen[x] = true;
        codec.exp_[i] = x;
        codec.log_[x] = static_cast<unsigned>(i);
        x <<= 1U;
        if ((x >> m) != 0U) {
            x ^= params.field_polynomial;
        }
    }
    if (x != 1U) {
        return fail("the field polynomial is not primitive");
    }

    if (params.generator.empty()) {
        const unsigned d = params.design_distance;
        if (d < 3 || d > n) {
            return fail("a BCH design distance is from 3 to the code length");
        }
        // Lin and Costello section 6.1: the union of the cyclotomic cosets
        // of 1 to d-1 is the generator's root set.
        std::set<std::size_t> roots;
        for (std::size_t j = 1; j < d; ++j) {
            std::size_t e = j;
            do {
                roots.insert(e);
                e = (2 * e) % n;
            } while (e != j);
        }
        // Lowest power first while multiplying out.
        std::vector<unsigned> g{1};
        for (const std::size_t r : roots) {
            std::vector<unsigned> next(g.size() + 1, 0);
            const unsigned a = codec.exp_[r];
            for (std::size_t i = 0; i < g.size(); ++i) {
                next[i + 1] ^= g[i];
                next[i] ^= codec.mul(g[i], a);
            }
            g = std::move(next);
        }
        for (auto it = g.rbegin(); it != g.rend(); ++it) {
            // A product over whole cosets has binary coefficients.
            codec.generator_.push_back(static_cast<std::uint8_t>(*it & 1U));
        }
        if (codec.generator_.size() - 1 >= n) {
            return fail("the design distance leaves no information bits");
        }
        codec.t_ = (d - 1) / 2;
        return codec;
    }

    const auto& gen = params.generator;
    if (gen.front() == 0U || gen.size() < 2 || gen.size() - 1 >= n) {
        return fail("a BCH generator is monic, of degree 1 to n - 1");
    }
    for (const std::uint8_t c : gen) {
        codec.generator_.push_back(c != 0U ? 1U : 0U);
    }
    // Divides x^n + 1: long division over GF(2), highest power first.
    std::vector<std::uint8_t> rem(n + 1, 0);
    rem[0] = 1;
    rem[n] = 1;
    const std::size_t r = gen.size() - 1;
    for (std::size_t i = 0; i + r <= n; ++i) {
        if (rem[i] != 0U) {
            for (std::size_t j = 0; j <= r; ++j) {
                rem[i + j] ^= codec.generator_[j];
            }
        }
    }
    if (std::any_of(rem.begin(), rem.end(), [](std::uint8_t b) { return b != 0U; })) {
        return fail("a cyclic code's generator divides x^n + 1");
    }
    // The BCH bound: the longest run of consecutive roots from alpha^1.
    std::size_t run = 0;
    for (std::size_t j = 1; j < n; ++j) {
        const unsigned a = codec.exp_[j];
        unsigned value = 0;
        for (const std::uint8_t c : codec.generator_) {
            value = codec.mul(value, a) ^ c;
        }
        if (value != 0U) {
            break;
        }
        run = j;
    }
    codec.t_ = run / 2;
    if (codec.t_ == 0) {
        return fail("the generator's roots do not make it correct an error");
    }
    return codec;
}

Status BchCodec::encode(std::span<const std::uint8_t> information,
                        std::span<std::uint8_t> check) const {
    const std::size_t r = check_bits();
    if (information.empty() || information.size() > k()) {
        return fail("BCH information is 1 to k bits");
    }
    if (check.size() != r) {
        return fail("BCH check bits are exactly the generator's degree");
    }
    std::fill(check.begin(), check.end(), std::uint8_t{0});
    for (const std::uint8_t bit : information) {
        const std::uint8_t fb = static_cast<std::uint8_t>((bit != 0U ? 1U : 0U) ^ check[0]);
        for (std::size_t i = 0; i + 1 < r; ++i) {
            check[i] = static_cast<std::uint8_t>(check[i + 1] ^ (fb & generator_[i + 1]));
        }
        check[r - 1] = static_cast<std::uint8_t>(fb & generator_[r]);
    }
    return {};
}

std::optional<std::size_t> BchCodec::decode(std::span<std::uint8_t> word) const {
    const std::size_t len = word.size();
    if (len <= check_bits() || len > n_) {
        return std::nullopt;
    }
    const std::size_t two_t = 2 * t_;

    const auto syndromes = [&](std::vector<unsigned>& s) {
        s.assign(two_t, 0);
        bool any = false;
        for (std::size_t i = 0; i < len; ++i) {
            if (word[i] == 0U) {
                continue;
            }
            const std::size_t power = len - 1 - i;
            for (std::size_t j = 0; j < two_t; ++j) {
                s[j] ^= pow(static_cast<unsigned>(((j + 1) * power) % n_));
            }
        }
        for (const unsigned v : s) {
            any = any || v != 0U;
        }
        return any;
    };

    std::vector<unsigned> s;
    if (!syndromes(s)) {
        return std::size_t{0};
    }

    // Berlekamp-Massey, Lin and Costello section 6.2 in Massey's form.
    std::vector<unsigned> c{1};
    std::vector<unsigned> b{1};
    std::size_t degree = 0;
    std::size_t shift = 1;
    unsigned last = 1;
    for (std::size_t step = 0; step < two_t; ++step) {
        unsigned disc = s[step];
        for (std::size_t i = 1; i <= degree && i < c.size(); ++i) {
            disc ^= mul(c[i], s[step - i]);
        }
        if (disc == 0U) {
            ++shift;
            continue;
        }
        const unsigned scale = mul(disc, exp_[(n_ - log_[last]) % n_]);
        std::vector<unsigned> next = c;
        if (next.size() < b.size() + shift) {
            next.resize(b.size() + shift, 0);
        }
        for (std::size_t i = 0; i < b.size(); ++i) {
            next[i + shift] ^= mul(scale, b[i]);
        }
        if (2 * degree <= step) {
            b = c;
            degree = step + 1 - degree;
            last = disc;
            shift = 1;
        } else {
            ++shift;
        }
        c = std::move(next);
    }
    if (degree > t_) {
        return std::nullopt;
    }

    // Chien search over the sent positions only.
    std::vector<std::size_t> errors;
    for (std::size_t i = 0; i < len; ++i) {
        const std::size_t power = len - 1 - i;
        const unsigned x = pow(static_cast<unsigned>((n_ - power % n_) % n_));
        unsigned value = 0;
        for (std::size_t j = c.size(); j-- > 0;) {
            value = mul(value, x) ^ c[j];
        }
        if (value == 0U) {
            errors.push_back(i);
        }
    }
    if (errors.size() != degree) {
        return std::nullopt;
    }
    for (const std::size_t i : errors) {
        word[i] ^= 1U;
    }
    if (syndromes(s)) {
        for (const std::size_t i : errors) {
            word[i] ^= 1U;
        }
        return std::nullopt;
    }
    return errors.size();
}

}  // namespace revenant::decode
