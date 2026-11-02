#include "core/decode/g3ruh.h"

namespace revenant::decode {

namespace {

// Bit k of the history is the bit k + 1 places back.
std::uint8_t taps(std::uint32_t history) {
    return static_cast<std::uint8_t>(((history >> (kG3ruhTapShort - 1U)) ^
                                      (history >> (kG3ruhTapLong - 1U))) & 1U);
}

}  // namespace

std::uint8_t G3ruhUnscrambler::push(std::uint8_t received) {
    const std::uint8_t out = static_cast<std::uint8_t>((received ^ taps(history_)) & 1U);
    history_ = (history_ << 1U) | (received & 1U);
    return out;
}

std::uint8_t G3ruhScrambler::push(std::uint8_t data) {
    const std::uint8_t sent = static_cast<std::uint8_t>((data ^ taps(history_)) & 1U);
    history_ = (history_ << 1U) | sent;
    return sent;
}

Expected<G3ruhDecoder> G3ruhDecoder::create(const G3ruhConfig& config) {
    if (config.rate < 2 * static_cast<SampleRate>(config.baud)) {
        return fail("G3RUH needs at least two samples a bit");
    }
    LevelDiscriminatorConfig level;
    level.rate = config.rate;
    level.symbol_rate = config.baud;
    auto discriminator = LevelDiscriminator::create(level);
    if (!discriminator) {
        return std::unexpected(with_context(discriminator.error(), "G3RUH"));
    }
    BitClockConfig clock;
    clock.rate = config.rate;
    clock.symbol_rate = config.baud;
    auto bit_clock = BitClock::create(clock);
    if (!bit_clock) {
        return std::unexpected(with_context(bit_clock.error(), "G3RUH"));
    }
    G3ruhDecoder d;
    d.config_ = config;
    d.discriminator_ = std::move(*discriminator);
    d.clock_ = std::move(*bit_clock);
    d.reset();
    return d;
}

void G3ruhDecoder::reset() {
    discriminator_.reset();
    clock_.reset();
    unscrambler_.reset();
    deframer_.reset();
    previous_ = 0;
    stats_ = {};
}

void G3ruhDecoder::process(ConstRealSpan audio, std::vector<Ax25Frame>& out) {
    soft_.clear();
    bits_.clear();
    discriminator_.process(audio, soft_);
    clock_.process(soft_, bits_);
    const auto delay = static_cast<SampleIndex>(discriminator_.group_delay());
    for (const SoftBit& b : bits_) {
        const std::uint8_t level = b.value >= 0.0F ? 1U : 0U;
        // Miller, "Unscrambler", then NRZI: no change is a one.
        const std::uint8_t line = unscrambler_.push(level);
        const std::uint8_t bit = line == previous_ ? 1U : 0U;
        previous_ = line;
        const SampleIndex sample = b.position > delay ? b.position - delay : 0;
        frames_.clear();
        deframer_.push(bit, sample, frames_);
        for (const HdlcFrame& f : frames_) {
            ++stats_.candidates;
            const std::size_t n = f.octets.size();
            const auto content = std::span<const std::uint8_t>(f.octets).first(n - 2);
            const auto received = static_cast<std::uint16_t>(
                f.octets[n - 2] | (static_cast<unsigned>(f.octets[n - 1]) << 8U));
            if (ax25_fcs(content) != received) {
                ++stats_.fcs_failures;
                continue;
            }
            auto frame = ax25_parse(content);
            if (!frame) {
                ++stats_.malformed;
                continue;
            }
            frame->first_sample = f.first_sample;
            frame->last_sample = f.last_sample;
            out.push_back(std::move(*frame));
        }
    }
}

}  // namespace revenant::decode
