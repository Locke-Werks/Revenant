#include "core/decode/viterbi_stream.h"

#include <algorithm>
#include <bit>
#include <format>
#include <utility>

namespace revenant::decode {

Expected<StreamingViterbi> StreamingViterbi::create(StreamingViterbiConfig config) {
    if (config.generators.empty()) {
        return fail("StreamingViterbi was given no generator polynomials");
    }
    if (config.memory == 0 || config.memory > 12) {
        return fail(std::format("StreamingViterbi needs a memory in 1..12; got {}",
                                config.memory));
    }
    const std::size_t outputs = config.generators.size();
    if (!config.puncture.empty()) {
        if (config.puncture.size() % outputs != 0) {
            return fail(std::format(
                "puncturing pattern of {} bits is not a whole number of {}-bit encoder steps",
                config.puncture.size(), outputs));
        }
        if (std::ranges::none_of(config.puncture, [](std::uint8_t b) { return b != 0; })) {
            return fail("puncturing pattern deletes every bit");
        }
    }

    StreamingViterbi v;
    v.memory_ = config.memory;
    v.outputs_ = outputs;
    v.states_ = std::size_t{1} << config.memory;
    v.traceback_ = config.traceback != 0 ? config.traceback : 5 * (config.memory + 1);
    v.puncture_ = std::move(config.puncture);
    v.invert_mask_ = config.invert_mask;

    const std::uint32_t reg_mask = (1U << (config.memory + 1U)) - 1U;
    v.branch_bits_.resize(v.states_ * 2);
    for (std::size_t state = 0; state < v.states_; ++state) {
        for (std::uint32_t bit = 0; bit < 2U; ++bit) {
            const std::uint32_t reg = ((static_cast<std::uint32_t>(state) << 1U) | bit) & reg_mask;
            std::uint32_t emitted = 0;
            for (std::size_t g = 0; g < outputs; ++g) {
                emitted |= static_cast<std::uint32_t>(std::popcount(reg & config.generators[g]) & 1)
                           << g;
            }
            v.branch_bits_[state * 2 + bit] = emitted;
        }
    }
    v.metric_.resize(v.states_);
    v.next_metric_.resize(v.states_);
    v.dropped_.resize((v.traceback_ + 1) * v.states_);
    v.reset();
    return v;
}

void StreamingViterbi::reset() {
    // The stream is assumed to start from the zero state, as an encoder does
    // when it is switched on. Other states start a long way behind rather than
    // unreachable, so a decoder joining mid-stream still converges.
    std::ranges::fill(metric_, 1e4F);
    metric_[0] = 0.0F;
    steps_ = 0;
    pending_.clear();
    puncture_pos_ = 0;
}

std::size_t StreamingViterbi::best_state() const {
    return static_cast<std::size_t>(std::ranges::min_element(metric_) - metric_.begin());
}

void StreamingViterbi::push(SoftSpan soft, std::vector<std::uint8_t>& out) {
    std::size_t i = 0;
    while (true) {
        // Fill the step, inserting erasures wherever the pattern deleted a bit.
        while (pending_.size() < outputs_) {
            const bool sent = puncture_.empty() || puncture_[puncture_pos_] != 0;
            if (sent) {
                if (i == soft.size()) {
                    return;
                }
                float value = soft[i++];
                if (((invert_mask_ >> pending_.size()) & 1U) != 0U) {
                    value = -value;
                }
                pending_.push_back(value);
            } else {
                pending_.push_back(0.0F);
            }
            if (!puncture_.empty()) {
                puncture_pos_ = (puncture_pos_ + 1) % puncture_.size();
            }
        }
        step(out);
        pending_.clear();
    }
}

void StreamingViterbi::step(std::vector<std::uint8_t>& out) {
    constexpr float kUnreached = 3.0e38F;
    std::ranges::fill(next_metric_, kUnreached);
    std::uint8_t* dropped = &dropped_[(steps_ % (traceback_ + 1)) * states_];
    const std::size_t state_mask = states_ - 1;
    const std::size_t top_shift = memory_ - 1;

    for (std::size_t state = 0; state < states_; ++state) {
        const float base = metric_[state];
        for (std::uint32_t bit = 0; bit < 2U; ++bit) {
            const std::uint32_t emitted = branch_bits_[state * 2 + bit];
            float cost = 0.0F;
            for (std::size_t g = 0; g < outputs_; ++g) {
                cost += ((emitted >> g) & 1U) != 0U ? pending_[g] : -pending_[g];
            }
            const std::size_t target = ((state << 1U) | bit) & state_mask;
            const float candidate = base + cost;
            if (candidate < next_metric_[target]) {
                next_metric_[target] = candidate;
                dropped[target] = static_cast<std::uint8_t>((state >> top_shift) & 1U);
            }
        }
    }

    // Renormalise so the metrics stay small however long the stream runs.
    const float floor = *std::ranges::min_element(next_metric_);
    for (std::size_t s = 0; s < states_; ++s) {
        metric_[s] = next_metric_[s] - floor;
    }
    ++steps_;

    if (steps_ > traceback_) {
        // Trace back `traceback` steps from the best state; the bit decided
        // at the oldest of them is that state's least significant bit.
        std::size_t state = best_state();
        std::size_t at = steps_ - 1;
        for (std::size_t k = 0; k < traceback_; ++k, --at) {
            const std::uint8_t top = dropped_[(at % (traceback_ + 1)) * states_ + state];
            state = (state >> 1U) | (static_cast<std::size_t>(top) << top_shift);
        }
        out.push_back(static_cast<std::uint8_t>(state & 1U));
    }
}

void StreamingViterbi::flush(bool terminated, std::vector<std::uint8_t>& out) {
    const std::size_t held = std::min(steps_, traceback_);
    std::vector<std::uint8_t> tail(held);
    std::size_t state = terminated ? 0 : best_state();
    std::size_t at = steps_ - 1;
    for (std::size_t k = 0; k < held; ++k, --at) {
        tail[held - 1 - k] = static_cast<std::uint8_t>(state & 1U);
        const std::uint8_t top = dropped_[(at % (traceback_ + 1)) * states_ + state];
        state = (state >> 1U) | (static_cast<std::size_t>(top) << (memory_ - 1));
    }
    out.insert(out.end(), tail.begin(), tail.end());
    reset();
}

std::vector<std::uint8_t> puncture_bits(ConstBitSpan encoded, std::span<const std::uint8_t> pattern) {
    if (pattern.empty()) {
        return {encoded.begin(), encoded.end()};
    }
    std::vector<std::uint8_t> out;
    out.reserve(encoded.size());
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (pattern[i % pattern.size()] != 0) {
            out.push_back(encoded[i]);
        }
    }
    return out;
}

}  // namespace revenant::decode
