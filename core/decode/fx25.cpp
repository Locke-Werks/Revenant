#include "core/decode/fx25.h"

#include <bit>

namespace revenant::decode {

std::optional<Fx25TagMatch> fx25_match_tag(std::uint64_t window) {
    std::optional<Fx25TagMatch> best;
    for (const Fx25Mode& mode : kFx25Modes) {
        const auto errors = static_cast<unsigned>(std::popcount(window ^ mode.tag));
        if (errors <= kFx25TagTolerance && (!best || errors < best->errors)) {
            best = Fx25TagMatch{&mode, errors};
        }
    }
    return best;
}

const Fx25Mode* fx25_mode(std::uint8_t index) {
    for (const Fx25Mode& mode : kFx25Modes) {
        if (mode.index == index) {
            return &mode;
        }
    }
    return nullptr;
}

Rs8Params fx25_rs_params(std::size_t check_symbols) {
    return Rs8Params{kFx25FieldPolynomial, kFx25FirstRoot, check_symbols};
}

Expected<Fx25Receiver> Fx25Receiver::create() {
    Fx25Receiver receiver;
    for (const std::size_t check : kFx25CheckSymbolCounts) {
        auto codec = Rs8Codec::create(fx25_rs_params(check));
        if (!codec) {
            return std::unexpected(with_context(codec.error(), "FX.25"));
        }
        receiver.codecs_.push_back(std::move(*codec));
    }
    return receiver;
}

const Rs8Codec& Fx25Receiver::codec_for(std::size_t check_symbols) const {
    for (const Rs8Codec& codec : codecs_) {
        if (codec.params().check_symbols == check_symbols) {
            return codec;
        }
    }
    // Every Table 1 row has one of the three counts create() built.
    return codecs_.front();
}

void Fx25Receiver::reset() {
    window_ = 0;
    window_fill_ = 0;
    mode_ = nullptr;
    tag_errors_ = 0;
    bits_.clear();
    samples_.clear();
}

void Fx25Receiver::push(std::uint8_t bit, SampleIndex sample, std::vector<Fx25Block>& out) {
    if (mode_ != nullptr) {
        bits_.push_back(bit);
        samples_.push_back(sample);
        if (bits_.size() < 8 * mode_->n) {
            return;
        }

        // "Physical Layer Considerations": octets least significant bit first.
        std::vector<std::uint8_t> word(mode_->n, 0);
        for (std::size_t i = 0; i < bits_.size(); ++i) {
            word[i / 8] = static_cast<std::uint8_t>(word[i / 8] | (bits_[i] << (i % 8)));
        }
        Fx25Block block;
        block.mode = mode_;
        block.tag_errors = tag_errors_;
        block.corrected = codec_for(mode_->check()).decode(word);
        // "Protocol Summary": "The FEC Check Symbols are applied at the end
        // of the FEC Codeblock ... All other symbols inside the FEC
        // Codeblock area are Information Symbols."
        block.information.assign(word.begin(), word.begin() + static_cast<std::ptrdiff_t>(mode_->k));
        block.samples.assign(samples_.begin(),
                             samples_.begin() + static_cast<std::ptrdiff_t>(8 * mode_->k));
        out.push_back(std::move(block));

        // "Multi-Frame Blocks": another tag may follow at once, so the
        // search starts again from an empty window.
        mode_ = nullptr;
        bits_.clear();
        samples_.clear();
        window_ = 0;
        window_fill_ = 0;
        return;
    }

    window_ = (window_ >> 1U) | (static_cast<std::uint64_t>(bit & 1U) << 63U);
    if (window_fill_ < kFx25TagBits) {
        ++window_fill_;
    }
    if (window_fill_ < kFx25TagBits) {
        return;
    }
    if (const auto match = fx25_match_tag(window_)) {
        mode_ = match->mode;
        tag_errors_ = match->errors;
        bits_.clear();
        samples_.clear();
        bits_.reserve(8 * mode_->n);
        samples_.reserve(8 * mode_->n);
    }
}

}  // namespace revenant::decode
