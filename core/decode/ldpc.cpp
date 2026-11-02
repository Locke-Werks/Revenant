#include "core/decode/ldpc.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace revenant::decode {

Expected<LdpcDecoder> LdpcDecoder::create(LdpcConfig config) {
    if (config.variables == 0 || config.checks.empty()) {
        return fail("an LDPC code needs at least one variable and one check");
    }
    if (config.variables > std::numeric_limits<std::uint32_t>::max()) {
        return fail("an LDPC code's variable count must fit in 32 bits");
    }
    if (config.max_iterations == 0) {
        return fail("an LDPC decoder needs at least one iteration");
    }
    if (config.rule == LdpcCheckRule::NormalisedMinSum &&
        !(config.scale > 0.0F && config.scale <= 1.0F)) {
        return fail("the normalised min-sum scale is in (0, 1]");
    }
    if (config.rule == LdpcCheckRule::OffsetMinSum &&
        !(config.offset >= 0.0F && std::isfinite(config.offset))) {
        return fail("the offset min-sum offset is finite and not negative");
    }

    LdpcDecoder decoder;
    decoder.check_start_.reserve(config.checks.size() + 1);
    decoder.check_start_.push_back(0);
    for (const auto& row : config.checks) {
        if (row.size() < 2) {
            return fail("an LDPC parity check constrains at least two variables");
        }
        std::vector<std::uint32_t> sorted = row;
        std::sort(sorted.begin(), sorted.end());
        if (sorted.back() >= config.variables) {
            return fail("an LDPC parity check names a variable beyond the code length");
        }
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
            return fail("an LDPC parity check names the same variable twice");
        }
        decoder.edge_variable_.insert(decoder.edge_variable_.end(), row.begin(), row.end());
        decoder.check_start_.push_back(decoder.edge_variable_.size());
    }
    decoder.to_check_.resize(decoder.edge_variable_.size());
    decoder.to_variable_.resize(decoder.edge_variable_.size());
    decoder.total_.resize(config.variables);
    decoder.config_ = std::move(config);
    return decoder;
}

bool LdpcDecoder::syndrome_zero(std::span<const std::uint8_t> bits) const {
    for (std::size_t c = 0; c + 1 < check_start_.size(); ++c) {
        unsigned parity = 0;
        for (std::size_t e = check_start_[c]; e < check_start_[c + 1]; ++e) {
            parity ^= bits[edge_variable_[e]] & 1U;
        }
        if (parity != 0U) {
            return false;
        }
    }
    return true;
}

LdpcResult LdpcDecoder::decode(std::span<const float> llr, std::span<std::uint8_t> bits) {
    const std::size_t n = config_.variables;
    for (std::size_t v = 0; v < n; ++v) {
        bits[v] = llr[v] < 0.0F ? 1 : 0;
    }
    if (syndrome_zero(bits)) {
        return {0, true};
    }

    // The first variable-to-check message is the channel's own opinion.
    for (std::size_t e = 0; e < edge_variable_.size(); ++e) {
        to_check_[e] = llr[edge_variable_[e]];
    }

    const bool normalised = config_.rule == LdpcCheckRule::NormalisedMinSum;
    for (std::size_t iteration = 1; iteration <= config_.max_iterations; ++iteration) {
        // Check nodes. Each outgoing message is the product of the other
        // incoming signs times the least of the other incoming magnitudes,
        // which is the smallest overall unless this edge supplied it.
        for (std::size_t c = 0; c + 1 < check_start_.size(); ++c) {
            float min1 = std::numeric_limits<float>::infinity();
            float min2 = min1;
            std::size_t min_edge = check_start_[c];
            bool negative = false;
            for (std::size_t e = check_start_[c]; e < check_start_[c + 1]; ++e) {
                const float m = to_check_[e];
                negative ^= m < 0.0F;
                const float mag = std::fabs(m);
                if (mag < min1) {
                    min2 = min1;
                    min1 = mag;
                    min_edge = e;
                } else if (mag < min2) {
                    min2 = mag;
                }
            }
            for (std::size_t e = check_start_[c]; e < check_start_[c + 1]; ++e) {
                float mag = e == min_edge ? min2 : min1;
                mag = normalised ? mag * config_.scale : std::max(mag - config_.offset, 0.0F);
                const bool sign = negative ^ (to_check_[e] < 0.0F);
                to_variable_[e] = sign ? -mag : mag;
            }
        }

        // Variable nodes: the total is the channel plus every check, and
        // each outgoing message leaves out what that check itself said.
        std::copy(llr.begin(), llr.begin() + static_cast<std::ptrdiff_t>(n), total_.begin());
        for (std::size_t e = 0; e < edge_variable_.size(); ++e) {
            total_[edge_variable_[e]] += to_variable_[e];
        }
        for (std::size_t e = 0; e < edge_variable_.size(); ++e) {
            to_check_[e] = total_[edge_variable_[e]] - to_variable_[e];
        }
        for (std::size_t v = 0; v < n; ++v) {
            bits[v] = total_[v] < 0.0F ? 1 : 0;
        }
        if (syndrome_zero(bits)) {
            return {iteration, true};
        }
    }
    return {config_.max_iterations, false};
}

Expected<LdpcEncoder> LdpcEncoder::create(
    const std::vector<std::vector<std::uint8_t>>& generator) {
    if (generator.empty()) {
        return fail("an LDPC generator needs at least one row");
    }
    const std::size_t k = generator.size();
    const std::size_t n = generator.front().size();
    if (n <= k) {
        return fail("an LDPC generator's rows are longer than its row count");
    }

    LdpcEncoder encoder;
    encoder.k_ = k;
    encoder.n_ = n;
    encoder.words_per_row_ = (n - k + 63) / 64;
    encoder.parity_.assign(k * encoder.words_per_row_, 0);
    for (std::size_t r = 0; r < k; ++r) {
        const auto& row = generator[r];
        if (row.size() != n) {
            return fail("an LDPC generator's rows are all the same length");
        }
        for (std::size_t c = 0; c < n; ++c) {
            if (row[c] > 1U) {
                return fail("an LDPC generator holds only zeros and ones");
            }
            if (c < k) {
                if (row[c] != (c == r ? 1U : 0U)) {
                    return fail("an LDPC generator for a systematic encoder starts with "
                                "the identity");
                }
            } else if (row[c] != 0U) {
                const std::size_t p = c - k;
                encoder.parity_[r * encoder.words_per_row_ + p / 64] |= std::uint64_t{1}
                                                                       << (p % 64);
            }
        }
    }
    return encoder;
}

void LdpcEncoder::encode(std::span<const std::uint8_t> information,
                         std::span<std::uint8_t> codeword) const {
    std::vector<std::uint64_t> parity(words_per_row_, 0);
    for (std::size_t r = 0; r < k_; ++r) {
        codeword[r] = information[r] & 1U;
        if (codeword[r] != 0U) {
            for (std::size_t w = 0; w < words_per_row_; ++w) {
                parity[w] ^= parity_[r * words_per_row_ + w];
            }
        }
    }
    for (std::size_t p = 0; p < n_ - k_; ++p) {
        codeword[k_ + p] = static_cast<std::uint8_t>((parity[p / 64] >> (p % 64)) & 1U);
    }
}

}  // namespace revenant::decode
