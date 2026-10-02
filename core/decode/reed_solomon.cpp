#include "core/decode/reed_solomon.h"

#include <algorithm>

namespace revenant::decode {

Expected<Rs8Codec> Rs8Codec::create(const Rs8Params& params) {
    if (params.field_polynomial < 0x100U || params.field_polynomial > 0x1FFU) {
        return fail("a GF(2^8) field polynomial has degree 8");
    }
    if (params.check_symbols < 2 || params.check_symbols >= kRs8FullLength ||
        params.check_symbols % 2 != 0) {
        return fail("a Reed-Solomon code over GF(2^8) has an even number of check symbols "
                    "from 2 to 254");
    }

    Rs8Codec codec;
    codec.params_ = params;
    codec.log_.fill(0);

    // Powers of x reduced by the field polynomial. x is primitive exactly
    // when the first 255 powers are all different, which is the same as none
    // of them before the 255th being 1.
    unsigned value = 1;
    for (std::size_t i = 0; i < kRs8FullLength; ++i) {
        if (i > 0 && value == 1U) {
            return fail("the field polynomial does not have x as a primitive element");
        }
        codec.exp_[i] = static_cast<std::uint8_t>(value);
        codec.log_[value] = static_cast<std::uint16_t>(i);
        value <<= 1U;
        if ((value & 0x100U) != 0U) {
            value ^= params.field_polynomial;
        }
    }
    if (value != 1U) {
        return fail("the field polynomial does not have x as a primitive element");
    }
    for (std::size_t i = kRs8FullLength; i < codec.exp_.size(); ++i) {
        codec.exp_[i] = codec.exp_[i - kRs8FullLength];
    }

    // g(x) = (x + alpha^b)(x + alpha^(b+1)) ... , highest power first.
    codec.generator_.assign(1, 1);
    for (std::size_t j = 0; j < params.check_symbols; ++j) {
        const std::uint8_t root =
            codec.alpha_power(params.first_root + static_cast<unsigned>(j));
        std::vector<std::uint8_t> next(codec.generator_.size() + 1, 0);
        for (std::size_t i = 0; i < codec.generator_.size(); ++i) {
            next[i] ^= codec.generator_[i];
            next[i + 1] ^= codec.multiply(root, codec.generator_[i]);
        }
        codec.generator_ = std::move(next);
    }
    return codec;
}

std::uint8_t Rs8Codec::alpha_power(unsigned exponent) const {
    return exp_[exponent % kRs8FullLength];
}

std::uint8_t Rs8Codec::multiply(std::uint8_t a, std::uint8_t b) const {
    if (a == 0U || b == 0U) {
        return 0;
    }
    return exp_[log_[a] + log_[b]];
}

std::uint8_t Rs8Codec::divide(std::uint8_t a, std::uint8_t b) const {
    if (a == 0U) {
        return 0;
    }
    return exp_[log_[a] + kRs8FullLength - log_[b]];
}

void Rs8Codec::encode(std::span<const std::uint8_t> information,
                      std::span<std::uint8_t> check) const {
    // The remainder of m(x) x^R divided by g(x), by the usual shift
    // register: each information symbol enters at the top and the feedback
    // is that symbol plus what leaves the register.
    const std::size_t r = params_.check_symbols;
    std::fill(check.begin(), check.end(), std::uint8_t{0});
    for (const std::uint8_t symbol : information) {
        const auto feedback = static_cast<std::uint8_t>(symbol ^ check[0]);
        for (std::size_t j = 0; j + 1 < r; ++j) {
            check[j] = static_cast<std::uint8_t>(check[j + 1] ^ multiply(feedback, generator_[j + 1]));
        }
        check[r - 1] = multiply(feedback, generator_[r]);
    }
}

std::optional<std::size_t> Rs8Codec::decode(std::span<std::uint8_t> word) const {
    const std::size_t n = word.size();
    const std::size_t r = params_.check_symbols;
    if (n <= r || n > kRs8FullLength) {
        return std::nullopt;
    }

    const auto syndromes = [&](std::vector<std::uint8_t>& s) {
        s.assign(r, 0);
        bool any = false;
        for (std::size_t j = 0; j < r; ++j) {
            const std::uint8_t root = alpha_power(params_.first_root + static_cast<unsigned>(j));
            std::uint8_t acc = 0;
            for (const std::uint8_t symbol : word) {
                acc = static_cast<std::uint8_t>(multiply(acc, root) ^ symbol);
            }
            s[j] = acc;
            any = any || acc != 0U;
        }
        return any;
    };

    std::vector<std::uint8_t> s;
    if (!syndromes(s)) {
        return std::size_t{0};
    }

    // Berlekamp-Massey. lambda and prior are lowest power first.
    std::vector<std::uint8_t> lambda(r + 1, 0);
    std::vector<std::uint8_t> prior(r + 1, 0);
    lambda[0] = 1;
    prior[0] = 1;
    std::size_t degree = 0;
    std::size_t shift = 1;
    std::uint8_t prior_discrepancy = 1;
    for (std::size_t k = 0; k < r; ++k) {
        std::uint8_t d = s[k];
        for (std::size_t i = 1; i <= degree; ++i) {
            d = static_cast<std::uint8_t>(d ^ multiply(lambda[i], s[k - i]));
        }
        if (d == 0U) {
            ++shift;
            continue;
        }
        const std::uint8_t scale = divide(d, prior_discrepancy);
        std::vector<std::uint8_t> before = lambda;
        for (std::size_t i = 0; i + shift <= r; ++i) {
            lambda[i + shift] = static_cast<std::uint8_t>(lambda[i + shift] ^ multiply(scale, prior[i]));
        }
        if (2 * degree <= k) {
            degree = k + 1 - degree;
            prior = std::move(before);
            prior_discrepancy = d;
            shift = 1;
        } else {
            ++shift;
        }
    }
    if (degree == 0 || degree > r / 2) {
        return std::nullopt;
    }

    // Chien search over the positions the word occupies. Position i holds
    // the coefficient of x^(n-1-i), so its locator is alpha^(n-1-i) and the
    // locator polynomial vanishes at that locator's inverse.
    std::vector<std::size_t> positions;
    for (std::size_t i = 0; i < n; ++i) {
        const auto power = static_cast<unsigned>(n - 1 - i);
        const std::uint8_t x_inverse =
            alpha_power(static_cast<unsigned>(kRs8FullLength) - power % kRs8FullLength);
        std::uint8_t sum = 0;
        std::uint8_t term = 1;
        for (std::size_t j = 0; j <= degree; ++j) {
            sum = static_cast<std::uint8_t>(sum ^ multiply(lambda[j], term));
            term = multiply(term, x_inverse);
        }
        if (sum == 0U) {
            positions.push_back(i);
        }
    }
    if (positions.size() != degree) {
        return std::nullopt;
    }

    // Error evaluator: S(x) lambda(x) mod x^R.
    std::vector<std::uint8_t> omega(r, 0);
    for (std::size_t i = 0; i < r; ++i) {
        for (std::size_t j = 0; j <= std::min(i, degree); ++j) {
            omega[i] = static_cast<std::uint8_t>(omega[i] ^ multiply(lambda[j], s[i - j]));
        }
    }

    // Forney: e = X^(1-b) omega(X^-1) / lambda'(X^-1). In characteristic 2
    // the derivative keeps only the odd powers of lambda, and the sign the
    // general formula carries is gone.
    std::vector<std::uint8_t> original(word.begin(), word.end());
    for (const std::size_t i : positions) {
        const auto power = static_cast<unsigned>(n - 1 - i);
        const std::uint8_t x_inverse =
            alpha_power(static_cast<unsigned>(kRs8FullLength) - power % kRs8FullLength);
        std::uint8_t numerator = 0;
        std::uint8_t term = 1;
        for (std::size_t j = 0; j < r; ++j) {
            numerator = static_cast<std::uint8_t>(numerator ^ multiply(omega[j], term));
            term = multiply(term, x_inverse);
        }
        std::uint8_t denominator = 0;
        const std::uint8_t x_inverse_squared = multiply(x_inverse, x_inverse);
        term = 1;
        for (std::size_t j = 1; j <= degree; j += 2) {
            denominator = static_cast<std::uint8_t>(denominator ^ multiply(lambda[j], term));
            term = multiply(term, x_inverse_squared);
        }
        if (denominator == 0U) {
            std::copy(original.begin(), original.end(), word.begin());
            return std::nullopt;
        }
        // X^(1-b), with the exponent taken modulo 255.
        const long long exponent =
            (static_cast<long long>(power) * (1LL - static_cast<long long>(params_.first_root))) %
            static_cast<long long>(kRs8FullLength);
        const auto scale = alpha_power(static_cast<unsigned>(
            (exponent + static_cast<long long>(kRs8FullLength)) %
            static_cast<long long>(kRs8FullLength)));
        const std::uint8_t value = multiply(scale, divide(numerator, denominator));
        word[i] = static_cast<std::uint8_t>(word[i] ^ value);
    }

    if (syndromes(s)) {
        std::copy(original.begin(), original.end(), word.begin());
        return std::nullopt;
    }
    return positions.size();
}

}  // namespace revenant::decode
