#include "core/decode/feld_hell.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace revenant::decode {

namespace {

// Engineering choices: the bits' threshold is half a peak that decays to
// half in about four seconds of slots, so a fade is followed and a burst of
// noise is forgotten.
constexpr double kPeakDecayPerSlot = 0.9993;
constexpr double kThreshold = 0.5;
// And the peak never falls below -80 dB of full scale, so the filter's
// ringing before the first dot is not scaled up into dots of its own.
constexpr double kPeakFloor = 1e-4;

}  // namespace

Expected<FeldHellDecoder> FeldHellDecoder::create(const FeldHellConfig& config) {
    if (config.rate <= 0 || config.carrier_hz <= 0.0 || config.bandwidth_hz <= 0.0 ||
        config.carrier_hz + config.bandwidth_hz >= config.rate / 2.0) {
        return fail("Feld-Hell: carrier and bandwidth must sit inside the audio band");
    }
    FeldHellDecoder d;
    d.config_ = config;
    d.samples_per_slot_ = static_cast<double>(config.rate) / kFeldHellSlotRate;

    // Windowed-sinc low pass, Hann window, about four cycles of the cutoff.
    const double fc = config.bandwidth_hz / config.rate;
    auto length = static_cast<std::size_t>(std::ceil(4.0 / fc)) | 1U;
    d.taps_.resize(length);
    const double mid = static_cast<double>(length - 1) / 2.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < length; ++i) {
        const double x = static_cast<double>(i) - mid;
        const double sinc = x == 0.0 ? 2.0 * fc
                                     : std::sin(2.0 * std::numbers::pi * fc * x) / (std::numbers::pi * x);
        const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                              static_cast<double>(length - 1));
        d.taps_[i] = static_cast<float>(sinc * w);
        sum += d.taps_[i];
    }
    for (float& t : d.taps_) {
        t = static_cast<float>(t / sum);
    }
    d.step_ = std::polar(1.0, -2.0 * std::numbers::pi * config.carrier_hz / config.rate);
    d.reset();
    return d;
}

void FeldHellDecoder::reset() {
    history_.assign(taps_.size(), {0.0F, 0.0F});
    history_at_ = 0;
    phasor_ = {1.0, 0.0};
    index_ = 0;
    slot_ = 0;
    slot_sum_ = 0.0;
    slot_count_ = 0;
    peak_ = 0.0;
    column_ = {};
    in_column_ = 0;
    pending_.clear();
}

void FeldHellDecoder::process(ConstRealSpan audio, std::vector<FeldHellColumn>& out) {
    const auto delay = static_cast<std::int64_t>(taps_.size() / 2);
    const std::size_t n = taps_.size();
    for (const float x : audio) {
        history_[history_at_] = std::complex<float>(static_cast<float>(x * phasor_.real()),
                                                    static_cast<float>(x * phasor_.imag()));
        history_at_ = (history_at_ + 1) % n;
        phasor_ *= step_;
        if ((index_ & 1023U) == 0) {
            phasor_ /= std::abs(phasor_);
        }
        // The filter's output at this sample describes the input delay
        // samples ago; slots are counted in input time.
        const std::int64_t t = static_cast<std::int64_t>(index_) - delay;
        ++index_;
        if (t < 0) {
            continue;
        }
        std::complex<float> acc{0.0F, 0.0F};
        std::size_t at = history_at_;
        for (std::size_t i = 0; i < n; ++i) {
            acc += history_[at] * taps_[i];
            at = (at + 1) % n;
        }
        const auto slot = static_cast<std::int64_t>(std::floor(static_cast<double>(t) / samples_per_slot_));
        if (slot != slot_) {
            const double fresh = slot_count_ > 0 ? slot_sum_ / slot_count_ : 0.0;
            peak_ = std::max({fresh, peak_ * kPeakDecayPerSlot, kPeakFloor});
            // A slot is decided a column after it was measured, against a
            // peak that has seen that column too, so the first dot of a
            // transmission is not judged against a peak it has not set yet.
            pending_.push_back(fresh);
            if (pending_.size() > static_cast<std::size_t>(kFeldHellHalfPixelsPerColumn)) {
                const double level = pending_.front();
                pending_.pop_front();
                const std::int64_t decided = slot_ - kFeldHellHalfPixelsPerColumn;
                const double scaled = level / peak_;
                if (in_column_ == 0) {
                    column_ = {};
                    column_.position = static_cast<SampleIndex>(std::llround(decided * samples_per_slot_));
                }
                column_.levels[static_cast<std::size_t>(in_column_)] = static_cast<float>(scaled);
                if (scaled >= kThreshold) {
                    column_.bits = static_cast<std::uint16_t>(column_.bits | (1U << in_column_));
                }
                if (++in_column_ == kFeldHellHalfPixelsPerColumn) {
                    out.push_back(column_);
                    in_column_ = 0;
                }
            }
            slot_ = slot;
            slot_sum_ = 0.0;
            slot_count_ = 0;
        }
        // Only the middle half of each slot is integrated, an engineering
        // choice: the dot's shaped edges and the detector's own response
        // spill about half a slot into the neighbours, and integrating the
        // whole slot read that spill as a dot. Twice the magnitude, so a
        // carrier of amplitude A reads A.
        const double within = static_cast<double>(t) / samples_per_slot_ - static_cast<double>(slot);
        if (within >= 0.25 && within < 0.75) {
            slot_sum_ += 2.0 * std::abs(acc);
            ++slot_count_;
        }
    }
}

}  // namespace revenant::decode
